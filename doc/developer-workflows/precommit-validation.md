# Local Pre-commit Validation Workflow

This document describes the repository-managed local pre-commit validation flow.

## Purpose

Run a fast, deterministic local build validation before each commit to catch breakages early.

## Files

- `.githooks/pre-commit`: repository hook entrypoint.
- `scripts/install-git-hooks.sh`: installs hook path configuration and executable bits.
- `scripts/precommit-validate.sh`: performs CMake configure/build and optional tests.

## Install and enable

From repository root:

```bash
./scripts/install-git-hooks.sh
```

This sets:

- `git config core.hooksPath .githooks`

## Manual usage

Build validation (default):

```bash
./scripts/precommit-validate.sh
```

Build + tests:

```bash
RUN_TESTS=1 ./scripts/precommit-validate.sh
```

Environment options:

- `BUILD_DIR` (default: `build`)
- `BUILD_TYPE` (default: `Release`)
- `RUN_TESTS` (default: `0`; set to `1` to build/run tests)

## Documentation synchronization policy

When introducing new local tools, scripts, workflows, or moved paths, update the relevant first-party Markdown docs in the same change whenever practical, including:

- `AGENTS.md`
- `README.md`
- affected `doc/**` pages and indexes

This keeps contributor and automation guidance aligned with the current repository layout.

## Troubleshooting

- If the hook blocks commit due to missing system dependencies, resolve dependencies and rerun validation manually.
- To confirm hook path:

```bash
git config --get core.hooksPath
```
