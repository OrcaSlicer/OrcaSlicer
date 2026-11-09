"""Regression tests use fake processes/SDK exports and never connect to printers."""
import argparse
import contextlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools/lan_monitor_probe"))
import run_probe as probe


class ProbeRunnerTests(unittest.TestCase):
    def test_sdk_series_resolves_installed_build_and_prefers_exact_file(self):
        with tempfile.TemporaryDirectory() as directory:
            data = Path(directory)
            (data / "plugins").mkdir()
            older = probe.resolve_sdk(data, "02.08.01.20")
            newest = probe.resolve_sdk(data, "02.08.01.53")
            for path in (older, newest, probe.resolve_sdk(data, "02.09.00.99")):
                path.touch()
            self.assertEqual(probe.resolve_sdk(data, "02.08.01"), newest)
            self.assertEqual(probe.resolve_sdk(data, "02.08.01.20"), older)
            missing = probe.resolve_sdk(data, "02.08.01.30")
            self.assertFalse(missing.exists())
            exact = newest.with_name(newest.name.replace("02.08.01.53", "02.08.01"))
            exact.touch()
            self.assertEqual(probe.resolve_sdk(data, "02.08.01"), exact)

    def test_restart_preserves_the_worker_lifetime_and_shutdown_protocol(self):
        class Input(io.StringIO):
            def close(self):
                self.recorded = self.getvalue()
                super().close()

        class Process:
            def __init__(self):
                self.stdin, self.stdout = Input(), io.StringIO()
                self.returncode = None

            def terminate(self):
                self.returncode = -1

            def wait(self, timeout):
                self.returncode = 0

        for application_mode in (False, True):
            with self.subTest(application_mode=application_mode), tempfile.TemporaryDirectory() as directory:
                data = Path(directory)
                (data / "plugins").mkdir()
                probe.resolve_sdk(data, "02.08.01.53").touch()
                config = {"app": {"network_plugin_version": "02.08.01"}, "local_machines": {
                    name: {"printer_type": "C12", "dev_ip": f"{name}.invalid", "access_code": "fixture"}
                    for name in ("first", "second")}}
                (data / "OrcaSlicer.conf").write_text(json.dumps(config), encoding="utf-8")
                processes = []

                def start(*args, **kwargs):
                    process = Process()
                    processes.append(process)
                    return process

                def sample(target=0):
                    return {"kind": "sample", "target": target, "reports": 2, "connection": 0,
                            "age_seconds": 0, "status": {"gcode_state": "RUNNING"}}

                events = mock.Mock()
                events.get.side_effect = [(0, 0, sample()), (1, 0, sample()), (0, 1, sample()),
                                          (0, 1, {"kind": "exited"}), (1, 0, {"kind": "exited"})]
                args = argparse.Namespace(data_dir=data, sdk_version=None, worker=sys.executable,
                    mode="workers", application_mode=application_mode, seconds=40, second_delay=0, restart_after=15)
                with mock.patch.object(probe, "ROOT", data), \
                     mock.patch.object(probe.subprocess, "Popen", side_effect=start), \
                     mock.patch.object(probe.threading.Thread, "start"), \
                     mock.patch.object(probe.queue, "Queue", return_value=events), \
                     mock.patch.object(probe.time, "monotonic", side_effect=[0, 1, 15, 16, 40, 41]), \
                     contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(probe.run(args), 0)
                replacement = processes[2].stdin
                contents = replacement.recorded if replacement.closed else replacement.getvalue()
                settings = json.loads(contents.splitlines()[0])
                self.assertEqual(settings["seconds"], 0 if application_mode else 25)
                if application_mode:
                    self.assertIn('{"command":"stop"}', contents)


class CameraProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.build = Path(cls.directory.name) / "build"
        for command in (
            ["cmake", "-S", str(Path(__file__).parent), "-B", str(cls.build),
             f"-DORCA_SOURCE_ROOT={ROOT}", "-DCMAKE_BUILD_TYPE=Release"],
            ["cmake", "--build", str(cls.build), "--config", "Release", "--target",
             "lan_camera_probe", "fake_camera_sdk", "--parallel", "2"],
        ):
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
        cls.worker = next(path for path in cls.build.rglob("lan_camera_probe*")
                          if path.is_file() and path.name in ("lan_camera_probe", "lan_camera_probe.exe"))
        cls.sdk = next(path for path in cls.build.rglob("*fake_camera_sdk*")
                       if path.suffix in (".dll", ".so", ".dylib"))

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def test_terminal_read_failure_cannot_pass_after_two_frames(self):
        self.check_camera("failure.invalid", expected_exit=1, expected_error=1)

    def test_recoverable_log_diagnostic_does_not_fail_a_healthy_camera(self):
        self.check_camera("healthy.invalid", expected_exit=0, expected_error=0)

    def check_camera(self, host, expected_exit, expected_error):
        with tempfile.TemporaryDirectory() as output:
            config = {"module": str(self.sdk), "seconds": 1, "output": output,
                      "printers": [{"host": host, "code": "fixture", "id": "fixture"}]}
            result = subprocess.run([str(self.worker)], input=json.dumps(config) + "\n",
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, expected_exit, result.stdout + result.stderr)
            line = next(line for line in result.stdout.splitlines() if line.startswith("ORCA_CAMERA_PROBE "))
            record = json.loads(line.removeprefix("ORCA_CAMERA_PROBE "))[0]
            self.assertEqual(record["frames"], 2)
            self.assertEqual(record["error"], expected_error)
            self.assertEqual(record["diagnostic_codes"], [3])


class FlatpakSourceTests(unittest.TestCase):
    def test_native_tools_configure_with_only_the_flatpak_staged_source_tree(self):
        with tempfile.TemporaryDirectory() as directory:
            staged = Path(directory)
            tools = staged / "src/dev-utils"
            tools.mkdir(parents=True)
            # Flatpak stages src/, but deliberately does not stage tools/.
            for source in (ROOT / "src/dev-utils").iterdir():
                if source.suffix == ".cpp" or source.name == "CMakeLists.txt":
                    shutil.copy2(source, tools / source.name)
            (staged / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.16)
project(FlatpakSourceFixture LANGUAGES CXX)
set(ORCA_TOOLS ON)
set(SLIC3R_GUI ON)
set(SLIC3R_ENC_CHECK OFF CACHE BOOL "" FORCE)
add_library(libslic3r_gui INTERFACE)
add_library(libslic3r INTERFACE)
add_library(boost_headeronly INTERFACE)
add_subdirectory(src/dev-utils)
''', encoding="utf-8")
            result = subprocess.run(["cmake", "-S", str(staged), "-B", str(staged / "build")],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
