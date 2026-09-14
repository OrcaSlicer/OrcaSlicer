"""Offline OBJ cache publication checks with synthetic geometry and I/O faults."""
from contextlib import contextmanager
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import glb_artifact
from glb_artifact import Mesh, write_analysis_obj


class AnalysisObjAtomicTests(unittest.TestCase):
    def setUp(self):
        temporary_directory = tempfile.TemporaryDirectory()
        self.addCleanup(temporary_directory.cleanup)
        self.root = Path(temporary_directory.name)
        self.destination = self.root / "analysis.obj"
        self.old_cache = b"# existing complete analysis\nv 0 0 0\n"
        mesh = Mesh(
            vertices=[(.001, .002, -.003), (.004, .005, -.006), (.007, .008, -.009)],
            colors=[(1, 0, 0), (0, 1, 0), (0, 0, 1)],
            faces=[(0, 1, 2)],
        )
        parser = mock.patch.object(glb_artifact, "Glb")
        parser.start().return_value.mesh.return_value = mesh
        self.addCleanup(parser.stop)

    def assert_cache_unchanged(self, existing):
        if existing:
            self.assertEqual(self.destination.read_bytes(), self.old_cache)
        else:
            self.assertFalse(self.destination.exists())

    def assert_no_temporary_files(self, existing):
        self.assertEqual(set(self.root.iterdir()), {self.destination} if existing else set())

    def test_complete_projection_is_published_for_new_and_existing_cache(self):
        expected = (
            "# GLB analysis projection: Z-up, millimetres, sRGB vertex colors\n"
            "v 1 3 2 1.000000 0.000000 0.000000\n"
            "v 4 6 5 0.000000 1.000000 0.000000\n"
            "v 7 9 8 0.000000 0.000000 1.000000\n"
            "f 1 2 3\n"
        ).encode("ascii")
        for existing in (False, True):
            with self.subTest(existing=existing):
                self.destination.unlink(missing_ok=True)
                if existing:
                    self.destination.write_bytes(self.old_cache)
                original_replace = Path.replace

                def publish(path, target):
                    self.assertEqual(path.parent, self.destination.parent)
                    self.assertEqual(path.read_bytes(), expected)
                    self.assert_cache_unchanged(existing)
                    return original_replace(path, target)

                with mock.patch.object(Path, "replace", autospec=True, side_effect=publish):
                    self.assertEqual(write_analysis_obj("synthetic.glb", self.destination), self.destination)
                self.assertEqual(self.destination.read_bytes(), expected)
                self.assert_no_temporary_files(True)

    def test_interrupted_write_preserves_existing_cache_or_leaves_no_cache(self):
        original_temporary_file = tempfile.NamedTemporaryFile
        for existing in (False, True):
            for failure in (OSError("simulated disk full"), KeyboardInterrupt()):
                with self.subTest(existing=existing, failure=type(failure).__name__):
                    self.destination.unlink(missing_ok=True)
                    if existing:
                        self.destination.write_bytes(self.old_cache)

                    @contextmanager
                    def interrupted_file(*args, **kwargs):
                        with original_temporary_file(*args, **kwargs) as stream:
                            original_write = stream.write
                            writes = 0

                            def write(text):
                                nonlocal writes
                                writes += 1
                                if writes == 3:
                                    self.assert_cache_unchanged(existing)
                                    raise failure
                                result = original_write(text)
                                stream.flush()
                                return result

                            stream.write = write
                            yield stream

                    with mock.patch.object(glb_artifact.tempfile, "NamedTemporaryFile", interrupted_file):
                        with self.assertRaises(type(failure)):
                            write_analysis_obj("synthetic.glb", self.destination)
                    self.assert_cache_unchanged(existing)
                    self.assert_no_temporary_files(existing)

    def test_failed_replace_preserves_existing_cache_and_removes_temporary(self):
        self.destination.write_bytes(self.old_cache)
        with mock.patch.object(Path, "replace", side_effect=PermissionError("cache is locked")):
            with self.assertRaises(PermissionError):
                write_analysis_obj("synthetic.glb", self.destination)
        self.assert_cache_unchanged(True)
        self.assert_no_temporary_files(True)


if __name__ == "__main__":
    unittest.main()
