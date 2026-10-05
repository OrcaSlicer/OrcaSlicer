#!/usr/bin/env python3
"""Tests for the diff handling in scripts/clang_tidy_diff.py (stdlib unittest, no
external deps).

Run from the repo root:  python -m unittest discover -s scripts/tests -v
"""

import json
import os
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

import clang_tidy_diff  # noqa: E402

DIFF = """\
diff --git a/src/libslic3r/Color.cpp b/src/libslic3r/Color.cpp
index 1111111..2222222 100644
--- a/src/libslic3r/Color.cpp
+++ b/src/libslic3r/Color.cpp
@@ -3,0 +4 @@
+#include <cassert>
@@ -8 +8,0 @@
-#include "libslic3r/Point.hpp"
@@ -20,2 +21,3 @@ void f()
-    old();
-    old();
+    a();
+    b();
+    c();
@@ -40 +42,0 @@ void g()
-    gone();
diff --git a/src/libslic3r/New.hpp b/src/libslic3r/New.hpp
new file mode 100644
--- /dev/null
+++ b/src/libslic3r/New.hpp
@@ -0,0 +1,2 @@
+#pragma once
+int f();
diff --git a/src/libslic3r/Old.cpp b/src/libslic3r/Renamed.cpp
similarity index 100%
rename from src/libslic3r/Old.cpp
rename to src/libslic3r/Renamed.cpp
"""


class TestParseChangedLines(unittest.TestCase):
    def setUp(self):
        self.changed = clang_tidy_diff.parse_diff(DIFF)

    def test_added_and_replaced_hunks_become_line_ranges(self):
        self.assertEqual(self.changed["src/libslic3r/Color.cpp"].lines, [[4, 4], [21, 23]])

    def test_deleted_includes_are_collected(self):
        self.assertEqual(self.changed["src/libslic3r/Color.cpp"].removed_includes, {"libslic3r/Point.hpp"})
        self.assertEqual(self.changed["src/libslic3r/New.hpp"].removed_includes, set())

    def test_new_file_covers_every_line(self):
        self.assertEqual(self.changed["src/libslic3r/New.hpp"].lines, [[1, 2]])

    def test_pure_rename_has_no_changed_lines(self):
        self.assertNotIn("src/libslic3r/Renamed.cpp", self.changed)

    def test_path_with_a_space_drops_the_tab_git_appends(self):
        changed = clang_tidy_diff.parse_diff("+++ b/src/libslic3r/Foo Bar.cpp\t\n@@ -1,0 +2 @@\n+int x;\n")
        self.assertEqual(changed["src/libslic3r/Foo Bar.cpp"].lines, [[2, 2]])


class TestNamesRemovedInclude(unittest.TestCase):
    def test_matches_however_the_include_was_spelled(self):
        removed = {"Point.hpp", "cassert"}
        self.assertTrue(clang_tidy_diff.names_removed_include("<cassert>", removed))
        self.assertTrue(clang_tidy_diff.names_removed_include("<libslic3r/Point.hpp>", removed))
        self.assertTrue(clang_tidy_diff.names_removed_include('"Point.hpp"', {"libslic3r/Point.hpp"}))

    def test_other_headers_and_no_suggestion_do_not_match(self):
        removed = {"libslic3r/Point.hpp"}
        self.assertFalse(clang_tidy_diff.names_removed_include("<libslic3r/MultiPoint.hpp>", removed))
        self.assertFalse(clang_tidy_diff.names_removed_include("<vector>", removed))
        self.assertFalse(clang_tidy_diff.names_removed_include("", removed))


class TestIsChecked(unittest.TestCase):
    def test_project_sources_and_headers(self):
        for path in ("src/libslic3r/Color.cpp", "src/slic3r/GUI/Tab.hpp", "tests/fff_print/test_flow.cpp",
                     "src/libslic3r/Format/bbs_3mf.h"):
            self.assertTrue(clang_tidy_diff.is_checked(path), path)

    def test_vendored_code_other_languages_and_other_dirs(self):
        for path in ("src/glad/src/gl.c", "src/glad/include/glad/gl.h", "tests/catch2/src/catch.hpp",
                     "deps_src/imgui/imgui.cpp", "src/slic3r/Utils/MacUtils.mm", "src/CMakeLists.txt"):
            self.assertFalse(clang_tidy_diff.is_checked(path), path)


FIXES = """\
---
MainSourceFile:  '/repo/src/libslic3r/Color.cpp'
Diagnostics:
  - DiagnosticName:  misc-include-cleaner
    DiagnosticMessage:
      Message:         'no header providing "assert" is directly included'
      FilePath:        '/repo/src/libslic3r/Color.cpp'
      FileOffset:      1974
      Replacements:
        - FilePath:        '/repo/src/libslic3r/Color.cpp'
          Offset:          39
          Length:          0
          ReplacementText: "#include <cassert>\\n"
    Level:           Error
  - DiagnosticName:  misc-include-cleaner
    DiagnosticMessage:
      Message:         'no header providing "Slic3r::comExpert" is directly included'
      FilePath:        '/repo/src/libslic3r/Color.cpp'
      FileOffset:      2148
      Replacements:    []
    Level:           Error
...
"""


class TestParseSuggestedIncludes(unittest.TestCase):
    def test_maps_each_message_to_the_include_its_fix_inserts(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "fixes.yaml")
            with open(path, "w", encoding="utf-8") as f:
                f.write(FIXES)
            self.assertEqual(clang_tidy_diff.parse_suggested_includes(path),
                             {'no header providing "assert" is directly included': "<cassert>"})

    def test_missing_file_means_no_suggestions(self):
        self.assertEqual(clang_tidy_diff.parse_suggested_includes("/nonexistent/fixes.yaml"), {})


class TestSubprocessCalls(unittest.TestCase):
    def run_patched(self, function, *args, returncode=0, stdout=""):
        done = subprocess.CompletedProcess([], returncode, stdout, "")
        with mock.patch.object(clang_tidy_diff.subprocess, "run", return_value=done) as run, \
             mock.patch.object(clang_tidy_diff.os.path, "normpath", wraps=os.path.normpath) as normpath:
            result = function(*args)
        return result, run.call_args, normpath

    def test_line_filter_names_the_file_with_native_separators(self):
        _, call, normpath = self.run_patched(clang_tidy_diff.run_clang_tidy, "clang-tidy", "build",
                                             "src/libslic3r/Color.cpp", [[4, 4]], [])
        line_filter = next(arg for arg in call.args[0] if arg.startswith("--line-filter="))
        self.assertEqual(json.loads(line_filter.split("=", 1)[1]),
                         [{"name": os.path.join("src", "libslic3r", "Color.cpp"), "lines": [[4, 4]]}])
        normpath.assert_any_call("src/libslic3r/Color.cpp")

    def test_changed_files_reads_non_ascii_paths_and_text_as_utf8(self):
        diff = "+++ b/src/libslic3r/Über.cpp\n@@ -1,0 +2 @@\n+// 打印\n"
        files, call, _ = self.run_patched(clang_tidy_diff.changed_files, "base", stdout=diff)
        self.assertIn("core.quotePath=false", call.args[0])
        self.assertEqual(call.kwargs["encoding"], "utf-8")
        self.assertEqual(files["src/libslic3r/Über.cpp"].lines, [[2, 2]])

    def test_clang_tidy_and_git_show_output_is_decoded_as_utf8(self):
        _, call, _ = self.run_patched(clang_tidy_diff.run_clang_tidy, "clang-tidy", "build",
                                      "src/libslic3r/Color.cpp", None, [])
        self.assertEqual(call.kwargs["encoding"], "utf-8")
        _, call, _ = self.run_patched(clang_tidy_diff.errors_alone_at, "base", "clang-tidy", "build",
                                      "src/libslic3r/Color.hpp", returncode=128)
        self.assertEqual(call.kwargs["encoding"], "utf-8")


if __name__ == "__main__":
    unittest.main()
