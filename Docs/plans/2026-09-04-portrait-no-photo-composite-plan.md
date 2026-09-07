# Portrait Reference Integrity Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Prevent original-photo pixels and gray background from being pasted onto generated portrait references.

**Architecture:** Remove local photographic/relief compositing while retaining identity detection evidence and one provider request. Preserve raw provider bytes and use those bytes for the portrait geometry copy. Existing history remains untouched.

**Tech Stack:** Python, Pillow, unittest; no C++ or format changes.

---

## Tasks
1. Add byte-preservation regression tests to tools/ai/test_openai_preprocessor.py using real local masks and a mocked image provider. Run them against current code and record the expected failure.
2. Remove source-face compositing helpers/calls in tools/ai/openai_preprocessor.py; replace obsolete compositing assertions and clarify geometry-first prompt wording if necessary. Keep detector/filename contracts.
3. Run targeted preprocessor/portrait/OBJ tests and complete offline tools/ai suite. Review the scoped diff and test actual source-image detection without altering history.
4. Check the running app's Python runtime and safe update path. Do not interrupt a generation or overwrite unrelated runtime changes. State any restart required.
5. Record verification and hand off without unrelated commits, release or push.

## Execution context
Execute locally in the current task. The superpowers-prefixed skill is not installed; the available Code workflow and file plan provide the task-by-task checks. No subagents required or authorized. Preserve all pre-existing worktree changes.

## Verification result
- Regression reproduced 48 source-photo overwrites before the fix and passed all 48 scenarios afterward.
- Full offline AI suite: 632 tests passed in 166.511 seconds. Final targeted suite: 24 tests passed.
- Reported source portrait: 4 offline integrity checks passed for both the repository and packaged module. Historical source/raw SHA256 values stayed unchanged; provider output was mocked, so this does not claim a fresh Image2 visual-quality run.
- Only openai_preprocessor.py was patched in the local model-generation-v2-app runtime after backing up the old module. Its normalized contents match the source. Running App processes were not interrupted; a normal exit/relaunch is required.
- Existing contaminated history is preserved. A newly generated AI shape reference is required to use the fix; simply regenerating 3D from an old contaminated reference does not repair that reference.
- No paid requests, C++ rebuild, release, remote push, or unrelated edits.
