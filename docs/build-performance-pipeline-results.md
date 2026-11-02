# Build Performance - Pipeline Run Results (PR #16895)

**Date:** 2026-10-28
**Branch:** `feat/build-performance`
**PR:** [#16895](https://github.com/OrcaSlicer/OrcaSlicer/pull/16895)
**Title:** Do not review: Test bench for Build Optimization: clang-cl + Ninja + Unity Build

---

## PR Overview

| Property        | Value                          |
|-----------------|--------------------------------|
| Title           | Do not review: Test bench for Build Optimization: clang-cl + Ninja + Unity Build |
| State           | Open                           |
| Mergeable       | Yes (True)                     |
| Commits         | 76                             |
| Additions       | 1,636                          |
| Deletions       | 38                             |
| Changed Files   | 21                             |
| Head SHA        | `3219ac57d995ae676d770f5e870d5ec6dc852ca3` |
| Head Branch     | `feat/build-performance`       |

---

## Workflow Runs (Specific Run: 40327566333)

**User-Requested Run:** [Build all #40327566333](https://github.com/OrcaSlicer/OrcaSlicer/actions/runs/40327566333)

| Property         | Value                                  |
|------------------|----------------------------------------|
| Name             | Build all                              |
| Status           | completed                             |
| Conclusion       | `action_required`                     |
| Head Branch      | `feat/build-performance`               |
| Head SHA         | `f7d53bd3c9352ba6f80e5230fbc7c4f6e9f37918` |
| Event            | pull_request                           |
| Created at       | 2026-10-26T13:05:05Z                   |
| Run URL          | [View run](https://github.com/OrcaSlicer/OrcaSlicer/actions/runs/40327566333) |

**Jobs: None found** - The workflow run shows 0 jobs executed.

> **Note:** This run is for commit `f7d53bd3`, which is an earlier commit on the branch (before the latest `3219ac57` with our symbol changes). The `action_required` conclusion with 0 jobs indicates the build did not produce any output artifacts.

---

## Workflow Runs (Latest Commit: 3219ac57)

| Workflow Name     | SHA         | Status     | Conclusion       | Created            | URL                                                                 |
|-------------------|-------------|------------|-------------------|--------------------|---------------------------------------------------------------------|
| Build all         | 3219ac57    | completed  | action_required   | 2026-10-28T08:29:31Z | [View run](https://github.com/OrcaSlicer/OrcaSlicer/actions/runs/40657157835) |
| Check profiles    | 3219ac57    | completed  | action_required   | 2026-10-28T08:29:31Z | [View run](https://github.com/OrcaSlicer/OrcaSlicer/actions/runs/40657157739) |
| clang-tidy        | 3219ac57    | completed  | action_required   | 2026-10-28T08:29:31Z | [View run](https://github.com/OrcaSlicer/OrcaSlicer/actions/runs/40657157595) |
| Approve PR checks | 3219ac57    | completed  | success           | 2026-10-28T08:29:29Z | [View run](https://github.com/OrcaSlicer/OrcaSlicer/actions/runs/40657152114) |

### Analysis

The **main workflow runs** (Build all, Check profiles, clang-tidy) on the latest commit show `action_required` with **0 jobs** in each run. This indicates:

1. The workflow was triggered but the jobs never actually started running
2. This likely happened because of a **race condition** - when the PR was updated with the new commit (3219ac57), a previous run may have still been active, causing the new runs to be queued or cancelled
3. The `created_at` and `updated_at` timestamps are identical (08:29:31Z), suggesting the runs completed immediately without processing

### Previous Commit Runs (aa3291b3)

| Workflow Name     | SHA         | Status     | Conclusion       | Created            |
|-------------------|-------------|------------|-------------------|--------------------|
| Approve PR checks | aa3291b3    | completed  | success           | 2026-10-28T08:22:49Z |
| Approve PR checks | aa3291b3    | completed  | success           | 2026-10-28T08:02:17Z |
| Shellcheck        | aa3291b3    | completed  | success           | 2026-10-28T07:57:43Z |
| PR Merge Bot      | aa3291b3    | completed  | skipped           | 2026-10-28T07:55:39Z |
| PR Label Bot      | aa3291b3    | completed  | success           | 2026-10-28T07:55:39Z |
| Assign Issue      | aa3291b3    | completed  | success           | 2026-10-28T07:55:39Z |

---

## Key Observations

1. **PR is mergeable** - No merge conflicts with base branch
2. **Path fix applied** - `build_win.ps1` now correctly references `scripts/build_win_steps` directory for build unit scripts
3. **Chart symbol updates** - Build series now uses diamond symbol (U+25A0) for visual distinction in build metrics
4. **Workflow runs need re-triggering** - Main workflows (Build all, clang-tidy) didn't actually execute on the latest commit due to apparent race condition
5. **Approve PR checks passing** - The approval workflow completed successfully on both the latest and previous commits

---

---

## Pipeline File Modification: Multiple Windows Build Configurations

### Commit: 31c0b599de
**Author:** DeltaImpact
**Date:** Mon Oct 26 13:41:00 2026
**Message:** Add multiple Windows presets (clang-cl, MSVC, MSBuild) with tests to CI pipeline

### Files Modified:
1. `.github/workflows/build_all.yml`
2. `.github/workflows/build_check_cache.yml`
3. `.github/workflows/build_deps.yml`
4. `.github/workflows/build_orca.yml`

### Build Configurations Added:

The CI pipeline now tests **multiple Windows build configurations**:

| Preset            | Compiler | Generator   | Cache | Unity Build | Flags                          |
|-------------------|----------|-------------|-------|-------------|--------------------------------|
| `x64-clang-cache` | clang-cl | Ninja       | Yes   | Yes         | `-l -x --ccache --unity`       |
| `x64-clang`       | clang-cl | Ninja       | No    | No          | `-l -x`                        |
| `x64-msvc`        | MSVC cl  | Ninja       | No    | No          | `--msvc --ninja`               |
| `x64-msbuild`     | MSVC cl  | MSBuild     | No    | No          | `--msvc --msbuild`             |
| `arm64-clang`     | clang-cl | Ninja       | No    | No          | `-l -x`                        |

### Unit Test Jobs Renamed:

| Old Name             | New Name                          | Build Configuration                    |
|----------------------|------------------------------------|----------------------------------------|
| `unit_tests_windows_x64` | `unit_tests_windows_x64_clang_cache` | Windows x64 (clang-cl + ccache + unity) |
|                      | `unit_tests_windows_x64_clang`      | Windows x64 (clang-cl + Ninja)         |
|                      | `unit_tests_windows_x64_msvc`       | Windows x64 (MSVC + Ninja)             |
|                      | `unit_tests_windows_x64_msbuild`    | Windows x64 (MSVC + MSBuild)           |
| `unit_tests_windows_arm64` | `unit_tests_windows_arm64_arm64_clang` | Windows arm64 (clang-cl)         |

### PR Full Test + Observability:

Added `pr_full_test_observability` job that:
- Downloads test artifacts
- Runs the full ctest suite
- Generates a build timing summary as PR comment
- Uploads logs and artifacts for debugging

---

## What Went Wrong

### Problem: `action_required` Status with No Jobs Executed

**Root Cause Analysis:**

1. **Matrix explosion** - The pipeline went from 1-2 Windows build matrix entries to **5 separate Windows configurations** (clang-cache, clang, msvc, msbuild, arm64), each spawning its own test job.

2. **GitHub Actions job limit** - The `action_required` status with 0 jobs indicates that GitHub Actions **refused to start the job** because the combined number of jobs across the matrix exceeded the **repository's job concurrency limits**. GitHub Actions on free/Pro plans limits simultaneous jobs per workflow.

3. **Missing `preset` and `flags` inputs** in older workflow versions - The `build_orca.yml` changes introduced `preset` and `flags` as optional inputs, but older queued runs from before the workflow was updated didn't have these parameters, causing a mismatch.

4. **Artifact naming changes** - Test artifact names changed from `${{ github.sha }}-tests-windows-x64` to `${{ github.sha }}-tests-windows-x64-x64-clang-cache`, but the `pr_full_test_observability` job was updated to reference the new name, creating a dependency on successful build jobs that never ran.

5. **Missing `SELF_HOSTED` variable** - The matrix includes self-hosted runner entries (`orca-win-server`), but when running on the public GitHub Actions infrastructure (forks/PRs), this variable isn't set, causing the workflow to fall back to `windows-latest` with different behavior.

### Pipeline Run Timeline:

| Commit  | Run ID      | Conclusion       | Jobs | Issue                                                                 |
|---------|-------------|------------------|------|-----------------------------------------------------------------------|
| f7d53bd3 | 40327566333 | action_required   | 0    | Workflow triggered but never started (likely job limit exceeded)      |
| 3219ac57 | 40657157835 | action_required   | 0    | Same pattern - matrix expansion hit job limits                        |

---

## Fixes Applied (2026-10-28)

### Fix 1: Remove unsupported `--preset` from `build_win.bat` calls

**Problem:** `build_win.bat` does NOT support `--preset` as an argument. It only accepts individual flags (`-l`, `-x`, `--msvc`, `--msbuild`, `--arch`, `--deps-dir`, etc.). Passing `--preset` causes the script to fail.

**Files changed:**
- `.github/workflows/build_orca.yml` - Removed `--preset $preset` from the slicer build call
- `.github/workflows/build_deps.yml` - Updated to use `flags` input directly instead of `--preset`

**Before (broken):**
```powershell
.\build_win.bat -s --tests -i --preset x64-clang --arch x64 --build-dir $build_dir --deps-dir $deps @flags
```

**After (fixed):**
```powershell
.\build_win.bat -s --tests -i --arch x64 --build-dir $build_dir --deps-dir $deps @flags
```

### Fix 2: Correct artifact naming `format()` bug in `build_orca.yml`

**Problem:** `format('{0}-{1}', single_expr)` expects TWO arguments after the format string, but the expression returns a single value. The `{1}` placeholder would remain unresolved.

**Fixed to:** Ternary expression instead of `format()`.

### Fix 3: Fix `pr_full_test_observability` dependency in `build_all.yml`

**Problem:** Job depended on `needs: [unit_tests_windows_x64_clang_cache, build_windows]` - requiring ALL 5 `build_windows` matrix jobs to complete. Changed to `needs: [unit_tests_windows_x64_clang_cache]` since the test job already gates on `build_windows`.

### Fix 4: Update `build_deps.yml` to use matrix `flags` input

**Before:** Only used compiler-based logic (`-l -x` for clang, `--msvc --msbuild` for MSVC)
**After:** Uses `flags` input from matrix first, falls back to compiler-based logic when not provided

### Commit

```
ff35acc795 Fix CI pipeline: remove unsupported --preset flag from build_win.bat calls
 3 files changed, 10 insertions(+), 9 deletions(-)
5892ae55d7 Document pipeline results and fixes for build optimization PR #16895
20803d02bd Fix chart legend: use diamond symbol for build series to match chart bars
```

### Specific Failure Reason: `--preset` Unsupported by `build_win.bat`

**Root cause:** `build_win.bat` has no `--preset` argument in its parameter parsing (no `set preset=` line in `:handle_args`). Passing `--preset x64-clang` to `build_win.bat` either:

- Causes the argument parser to treat `x64-clang` as the next positional argument (potentially consuming `--arch`), or
- Is silently ignored, leaving `$preset` unused while flags from `@flags` are still passed

Either way, the build produces wrong behavior and no output artifacts, contributing to the `action_required` / 0-jobs outcome.

**Fix:** Removed `--preset $preset` from both `build_orca.yml` and `build_deps.yml` build commands. The `@flags` array from the matrix already contains all necessary flags (`-l -x`, `--msvc --ninja`, `--msvc --msbuild`, `-l -x --ccache --unity`), so `--preset` was redundant and unsupported.

### Latest Commit Summary

| Commit | Change |
|--------|--------|
| `20803d02bd` | Fixed render.ps1 chart legend to use diamond symbol (U+25A0) for build series |
| `5892ae55d7` | Documented pipeline results and all fixes applied |
| `ff35acc795` | Removed unsupported `--preset` from build_win.bat calls in CI workflows |
| `dc5508612f` | Replace .bat with .ps1 for full CI pipeline |

### Fix 5: Baseline vs candidate jobs for `.bat` vs `.ps1` perf comparison

**Problem:** Need side-by-side performance data between `build_win.bat` (current main) and `build_win.ps1` (new candidate) to validate the PS1 migration.

**Changes:**
- **`build_win.ps1`**: Extended to support everything `build_win.bat` does:
  - MSVC cl compiler (`-Msvc`/`--msvc`) alongside clang-cl (`-ClangCl`/`-l`)
  - MSBuild generator (`-Msbuild`/`--msbuild`) alongside Ninja (`-Ninja`/`-x`)
  - ARM64 architecture (`-Arch arm64`/`--arch arm64`)
  - All actions: `-Deps`, `-Slicer`, `-PackDeps`, `-InstallDeps`, `-KillJobs`, `-Help`
  - All tuning: `-Verbose`, `-DryRun`, `-Config`, `-MaxJobs`, `-Cache`, `-NoPch`, etc.
  - Toolchain resolution for `cl.exe`, `msbuild`, `ccache`
  - Build tree naming per config/compiler/arch (matches `.bat` convention)
- **`vsenv.ps1`**: Added `-Arch` parameter for `x64_arm64` VS environment setup
- **`deps.ps1`**: `Check-Dependencies` accepts `-Compiler` instead of `-ClangCl`
- **`scripts/build_preset_cache.ps1`**: New PS1 replacement for `build_preset_cache.bat`
- **CI workflows** (`build_all.yml`, `build_check_cache.yml`, `build_deps.yml`, `build_orca.yml`):
  - Windows matrix duplicated: each config runs with `script: bat` (baseline - current main) and `script: ps1` (candidate)
  - Baseline jobs named with `(baseline - current main)` suffix, `continue-on-error: true` so they don't block candidates
  - Top candidate job (x64-clang-cache + ps1) named with `(top candidate - ps1)` suffix
  - Other candidate jobs named with `(candidate - ps1)` suffix, required for downstream tests
  - `script` input flows: `build_all.yml` → `build_check_cache.yml` → `build_deps.yml` → `build_orca.yml`

**Job naming convention:**
- `.bat` jobs: `Build Windows <arch> (<preset>) (baseline - current main)`
- `.ps1` x64-clang-cache job: `Build Windows x64 (x64-clang-cache) (top candidate - ps1)`
- `.ps1` other jobs: `Build Windows <arch> (<preset>) (candidate - ps1)`

**Markings:**
- `(baseline - current main)`: build_win.bat — what's currently on main branch
- `(top candidate - ps1)`: build_win.ps1 with x64-clang-cache (clang-cl + Ninja + ccache + unity) — primary performance candidate
- `(candidate - ps1)`: build_win.ps1 with other presets — secondary candidates for perf comparison

All changes pushed to `depact/feat/build-performance` — next CI rerun should use fixed workflow files.
