from __future__ import annotations

import ast
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS_AI = REPO_ROOT / "tools" / "ai"


def packaged_runtime_files() -> list[Path]:
    cmake = (REPO_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    block = re.search(r"set\(ORCA_AI_SIDECAR_RUNTIME_FILES\s+(.*?)\)", cmake, re.DOTALL)
    if block is None:
        raise AssertionError("CMake AI runtime file list is missing")
    names = re.findall(r'"\$\{CMAKE_SOURCE_DIR\}/tools/ai/([^"/]+\.py)"', block[1])
    if not names:
        raise AssertionError("CMake AI runtime file list is empty")
    return [TOOLS_AI / name for name in names]


class PackagedSidecarTests(unittest.TestCase):
    def test_runtime_list_contains_all_local_imports(self) -> None:
        files = packaged_runtime_files()
        included = {path.stem for path in files}
        self.assertIn("orca_ai_installed_bootstrap", included)
        self.assertIn("orca_ai_sidecar", included)
        for path in files:
            self.assertFalse(path.name.startswith("test_"))
            for node in ast.walk(ast.parse(path.read_text(encoding="utf-8"))):
                names = []
                if isinstance(node, ast.Import):
                    names = [alias.name for alias in node.names]
                elif isinstance(node, ast.ImportFrom) and node.module:
                    names = [node.module]
                for name in names:
                    module = name.split(".")[0]
                    if (TOOLS_AI / f"{module}.py").is_file():
                        with self.subTest(source=path.name, dependency=module):
                            self.assertIn(module, included, f"Missing packaged dependency: {module}.py")

    def test_only_packaged_files_import_in_isolated_python(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            runtime = Path(directory)
            for path in packaged_runtime_files():
                shutil.copyfile(path, runtime / path.name)
            # Never inherit a developer's live providers into a packaging check.
            environment = {name: value for name, value in os.environ.items()
                           if not name.upper().startswith(("OPENAI_", "TRIPO_", "TRIPO3D_", "ORCASLICER_AI_"))}
            code = """
import pathlib, socket, sys
def deny_network(*args, **kwargs):
    raise AssertionError('Packaging import check must not connect to a service')
socket.socket.connect = deny_network
socket.create_connection = deny_network
sys.path.insert(0, sys.argv[1])
import orca_ai_sidecar
assert pathlib.Path(orca_ai_sidecar.__file__).parent == pathlib.Path(sys.argv[1])
print('Packaged sidecar import passed')
"""
            result = subprocess.run(
                [sys.executable, "-I", "-B", "-c", code, str(runtime)], cwd=runtime,
                env=environment, capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("Packaged sidecar import passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
