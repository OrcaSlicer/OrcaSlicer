"""Desired support decisions from an exact GCode.cpp source excerpt.

This is a synthetic-geometry source-component test, not an application
reproduction. It needs an already installed C++17 compiler, uses only Python's
standard library, and removes its temporary C++ build automatically.
The candidate support behavior is tentative pending maintainer agreement.
"""

import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/libslic3r/GCode.cpp"
TEMPLATE = Path(__file__).with_name("support_branch.cpp")
EXCERPT_MARKER = "    // SOURCE_EXCERPT\n"


def source_excerpt():
    """Keep the support/infill decisions verbatim between neighboring paths."""
    source = SOURCE.read_bytes()
    function_start = source.index(b"bool GCode::needs_retraction(")
    function_end = source.index(b"\nstd::string GCode::retract(", function_start)
    function = source[function_start:function_end]
    preceding_path = function.index(b"    if (leaving_external_perimeter) {")
    preceding_end = b"        return true;\n    }\n\n"
    start = function.index(preceding_end, preceding_path) + len(preceding_end)
    end = function.index(b"    // retract if reduce_infill_retraction is disabled", start)
    excerpt = function[start:end]
    first_line = source[:function_start + start].count(b"\n") + 1
    last_line = first_line + excerpt.count(b"\n") - 1
    return source, excerpt, first_line, last_line


class SupportBranchChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source, excerpt, first_line, last_line = source_excerpt()
        template = TEMPLATE.read_text(encoding="utf-8")
        if template.count(EXCERPT_MARKER) != 1:
            raise ValueError("the C++ harness needs exactly one excerpt marker")
        before, after = template.split(EXCERPT_MARKER)
        harness = (
            before
            + f'#line {first_line} "src/libslic3r/GCode.cpp"\n'
            + excerpt.decode("utf-8")
            + '#line 1 "support_branch_harness_tail"\n'
            + after
        )

        temporary = tempfile.TemporaryDirectory(prefix="orca-support-component-")
        cls.addClassCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        harness_path = directory / "support_branch.cpp"
        cls.binary = directory / "support_branch"
        harness_path.write_text(harness, encoding="utf-8")
        if sys.platform == "darwin":
            compiler = subprocess.run(
                ["/usr/bin/xcrun", "--find", "clang++"],
                check=True, capture_output=True, text=True,
            ).stdout.strip()
        else:
            compiler = shutil.which("clang++") or shutil.which("c++")
            if compiler is None:
                raise RuntimeError("an already installed C++17 compiler is required")
        version = subprocess.run(
            [compiler, "--version"], check=True, capture_output=True, text=True,
        ).stdout.splitlines()[0]
        print("\nSynthetic-geometry source-component test; no application slicing.", flush=True)
        print(f"Source SHA-256: {hashlib.sha256(source).hexdigest()}", flush=True)
        print(f"Exact excerpt lines: {first_line}-{last_line}", flush=True)
        print(f"Excerpt SHA-256: {hashlib.sha256(excerpt).hexdigest()}", flush=True)
        print(f"Compiler: {version}", flush=True)
        subprocess.run(
            [compiler, "-std=c++17", "-Wall", "-Wextra", "-pedantic",
             str(harness_path), "-o", str(cls.binary)],
            check=True,
        )
        print("C++17 source-component compilation succeeded.", flush=True)

    def check_decision(self, geometry, context, reduce, expected):
        result = subprocess.run(
            [str(self.binary), geometry, str(context), str(reduce)],
            check=True, capture_output=True, text=True,
        )
        self.assertEqual(
            result.stdout.strip(), expected,
            f"{geometry}: context={context}, effective reduce_infill_retraction={reduce}",
        )

    def test_disabled_context_preserves_contained_support_shortcuts(self):
        # Given contained normal islands and tree bases, process reduction alone
        # must not change their legacy decisions when the context is disabled.
        for geometry, reduce in (("normal", 0), ("normal", 1), ("tree", 0), ("tree", 1)):
            with self.subTest(geometry=geometry, reduce=reduce):
                # When the real support/infill excerpt evaluates the travel,
                # Then both original support shortcuts still skip selection.
                self.check_decision(geometry, 0, reduce, "skip")

    def test_enabled_context_reaches_selection_for_contained_support(self):
        # Given the tentative global behavior and its effective reduction=false,
        # a support containment shortcut must no longer suppress selection.
        for geometry in ("normal", "tree"):
            with self.subTest(geometry=geometry):
                # When each distinct support shortcut would otherwise apply,
                # Then evaluation continues to the normal retraction selection.
                self.check_decision(geometry, 1, 0, "selection")

    def test_outside_support_reaches_selection(self):
        # Given travel outside both synthetic support and internal regions,
        # neither context state nor process reduction creates containment.
        for geometry in ("outside-normal", "outside-tree"):
            for context, reduce in ((0, 0), (0, 1), (1, 0), (1, 1)):
                with self.subTest(geometry=geometry, context=context, reduce=reduce):
                    # When the excerpt evaluates this uncontained travel,
                    # Then it reaches the normal retraction selection.
                    self.check_decision(geometry, context, reduce, "selection")

    def test_ordinary_infill_keeps_process_option_semantics(self):
        # Given an ordinary infill move inside an internal region at 15% density,
        # the support-specific context must preserve both legacy option states.
        for context, reduce, expected in (
            (0, 0, "selection"), (0, 1, "skip"),
            (1, 0, "selection"), (1, 1, "skip"),
        ):
            with self.subTest(context=context, reduce=reduce):
                # When the unchanged ordinary-infill branch evaluates the move,
                # Then only the process option controls its containment shortcut.
                self.check_decision("infill", context, reduce, expected)


if __name__ == "__main__":
    unittest.main(verbosity=2)
