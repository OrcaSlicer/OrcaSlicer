# Windows Build Script Performance Notes

**Branch:** `feat/build-performance` (Depact/OrcaSlicer)
**Test period:** November 2026
**Runners:** GitHub-hosted `windows-latest`, plus a self-hosted run on `HOME-LAPTOP` (i5-10500H) early on

## Where this ended up

`build_win.ps1` is the only Windows build script in the branch. `build_win.bat` was deleted along with its matrix leg, and the pipeline builds one Windows configuration: `x64-clang` (clang-cl + Ninja Multi-Config), incremental by default and full rebuild when dispatched with `clean-build: true`.

The bat numbers below are kept because they are the evidence for the switch, not because the bat still exists.

## Benchmarks: bat vs ps1

GitHub-hosted `windows-latest`, `clean-build: true` for both legs of a comparison.

| Run | Preset | bat | ps1 | Delta |
|-----|--------|-----|-----|-------|
| #179 | x64-clang | 1:37:52 | 0:57:31 | ps1 41% faster |
| #191 | x64-clang | 56:44 | 1:13:38 | ps1 30% slower |
| #182 | x64-msvc (MSVC + Ninja) | 1:57:28 | 1:26:08 | ps1 27% faster |
| #182 | x64-msbuild (MSVC + MSBuild) | 1:29:00 | n/a | ps1 leg skipped by workflow |
| #179 | x64-clang-cache | 0:17:35 | 1:36:07 | bat leg almost certainly a stale cache hit |
| #179 | x64-clang-unity | failed (0:01:47) | 1:36:58 | unity removed afterwards |

Self-hosted, warm deps and ccache:

| Runs | bat | ps1 |
|------|-----|-----|
| #150, #151 | 67:19, 70:18 (mean 68:49) | 66:49, 64:44 (mean 65:47) |

**Reading the numbers.** Clean-build legs on GitHub-hosted runners are not a controlled experiment. `clean-build: true` wipes the slicer tree but leaves `deps/` and ccache from the branch cache, so the two legs can start from different cache states - which is the most likely explanation for #179 and #191 disagreeing by 70 points. The self-hosted pair is the only run where both legs shared a machine and a warm cache, and there the ps1 is ~4.5% faster, inside noise. The defensible claim is that the ps1 is not slower, and it is the script the repository can actually maintain (single implementation, real error reporting, no duplicated flag parsing).

## Root causes fixed along the way

| Symptom | Cause | Fix |
|---|---|---|
| ps1 rebuilt every dependency | Manifest written outside the cached tree | Write it inside `OrcaSlicer_dep` |
| Preset cache step failed after ~2s | `$MyInvocation` resolved the wrong directory | Use `$PSScriptRoot` |
| Preset cache crashed on an empty directory | `Get-ChildItem` returned null | Guard before `Remove-Item` |
| No install tree, portable zip silently skipped | `-InstallSlicer` was parsed and ignored | Run `cmake --build --target install` |
| Deps cache v1 needed Iconv | Boost built against Iconv | Cache key v2, Iconv-free |
| clang C++ build failed on paths | Forward slashes in `ORCA_BUNDLED_PYTHON_ROOT` | Fix the path construction |
| VS version always detected as 17 | Over-escaped regex never matched | `'\\(1[6-9]\|[2-9][0-9])\\'` |
| Script died on PowerShell 5.1 | `??` operator does not exist there | Explicit fallback |
| Windows ARM64 rebuilt deps every run | Tree hardcoded to `deps/build`, cache restores `deps/build-arm64` | Resolve `$DepsDir` from the architecture |
| Failure log tail never printed | `Get-Content` output was captured by the caller's variable and discarded on throw | Write the tail straight to the host |
| unity build enabled by ccache | `-DSLIC3R_UNITY_BUILD=ON` added whenever `-Cache ccache` | Unity support removed |
| Dead flags and dead paths | `-NoConfigure`, `-InstallSlicer`, `-CleanDeps`, `-DepsDir`, `-ClangPath` did nothing | Wired up or deleted |

## Pipeline

- Windows: one `x64-clang` job running `build_win.ps1`, incremental by default. `clean-build: true` on a manual dispatch forces a from-scratch slicer tree.
- Dependencies and ccache come from `actions/cache`, keyed on OS/arch/compiler/BuildConfig, with `restore-keys` falling back from the branch to main to a generic prefix.
- Self-hosted runs (`vars.SELF_HOSTED`) use the local cache directory instead of the cloud cache.
- Linux (x64, aarch64), macOS (arm64, x86_64, universal) and Flatpak use their own scripts and are unaffected by any of this.

## Known limitations

1. All timings come from GitHub-hosted 4 vCPU runners, so they say more about queueing than about the script.
2. Clean-build comparisons inherit deps and ccache state, so leg-for-leg deltas are not trustworthy.
3. The ccache manifest hash costs ~19s per ps1 invocation (1,846 dependency files).
4. Incremental-build performance, which is what pull requests actually see, has not been benchmarked.

## Reproducing

```bash
gh workflow run build_all.yml -f clean-build=true -R Depact/OrcaSlicer
```

Locally:

```powershell
.\build_win.ps1 -Help
.\build_win.ps1 -Deps -Slicer
```
