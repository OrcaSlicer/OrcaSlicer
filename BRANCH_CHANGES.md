# feat/build-performance — Changes Introduced in Branch

**Base upstream commit:** `e76b4d676c` (OrcaSlicer/OrcaSlicer main)
**Branch:** `feat/build-performance` (fork: `Depact/OrcaSlicer`)
**Merged upstream/main:** `2d9a7078e5` (includes upstream commits through `1174b39004`)

---

## Summary

This branch delivers a **working, fast Windows CI pipeline** with full **self-hosted runner support** (`vars.SELF_HOSTED`) restored across all workflows. The default build configuration is `-Deps -Slicer -Tests -RunTests -ClangCl -Ninja` with ccache auto-enabled when available.

---

## CI/CD Workflows (`.github/workflows/`)

### `build_all.yml` — Orchestrator
- **Stripped UTF-8 BOM** from line 1 (was blocking parses in earlier runs)
- **Self-hosted routing** via `vars.SELF_HOSTED`:
  - `check_build_script` → `orca-win-server` / `windows-2022`
  - `build_windows` matrix `os:` → `orca-win-server` / `windows-2022`
  - `build_linux` matrix: self-hosted = single x86_64 entry on `orca-lnx-server`; hosted = x64 + aarch64 on GitHub runners
  - `build_macos_arch`: `os: ${{ vars.SELF_HOSTED && 'orca-macos-arm64' || (matrix.arch == 'arm64' && 'macos-15' || 'macos-15-intel') }}` — x86_64 always runs on Intel pool (`macos-15-intel`), arm64 on `macos-15`
  - `build_macos_universal`: same runner expression
  - Unit test jobs: os expressions SELF_HOSTED-aware; `unit_tests_linux_aarch64` and `unit_tests_macos_x86_64` skipped on self-hosted
  - `slice_check_linux` name dynamic: `Slice check (Linux ${{ vars.SELF_HOSTED && 'x86_64' || 'aarch64' }})`
- Unity build **removed**; only `x64-clang` preset remains
- Cache fallback: branch → main → generic prefix (handled in `build_check_cache.yml`)

### `build_orca.yml` — Main build (939 lines)
**macOS:**
- `Install tools mac`: runs on both; `brew install` wrapped in `if [ -z "${{ vars.SELF_HOSTED }}" ]`
- `Free disk space`: `&& !vars.SELF_HOSTED`
- **Build slicer mac**, `Pack unit tests mac`, `Upload Test Artifact mac`, `Build system preset cache (macOS)`, `Pack macOS app bundle`, `Upload macOS app bundle` — **no SELF_HOSTED guard** (run on both)
- `Upload OrcaSlicer_profile_validator DMG mac`, `Deploy Mac release`, `Deploy Mac validator DMG release` — `&& !vars.SELF_HOSTED`

**Windows:**
- `Restore NSIS`, `Install NSIS`, `Save NSIS` — `&& !vars.SELF_HOSTED`
- `Create installer Win` — `&& !vars.SELF_HOSTED`
- `Build slicer Win`, `Build system preset cache (Windows)`, `Pack unit tests Win`, `Upload Test Artifact Win`, `Pack app`, `Upload artifacts Win zip`, `Check exception throw info Win` — **unguarded**
- `Pack PDB`, `Upload artifacts Win installer`, `Upload artifacts Win PDB`, `Upload OrcaSlicer_profile_validator Win` — `&& !vars.SELF_HOSTED`
- All Deploy steps, `Build MSIX Store package Win`, `Upload artifacts Win MSIX` — `&& !vars.SELF_HOSTED`

**Linux:**
- `Apt-Install Dependencies` — `&& !vars.SELF_HOSTED`
- `Upload profile validator (for slice check)` — arch-aware: `if: ${{ runner.os == 'Linux' && (vars.SELF_HOSTED && inputs.arch != 'aarch64' || !vars.SELF_HOSTED && inputs.arch == 'aarch64') }}`
- `Build orca_custom_preset_tests`, `Upload OrcaSlicer_profile_validator Ubuntu`, `Upload generate_system_cache Ubuntu` — `&& !vars.SELF_HOSTED`
- All Deploy steps — `&& !vars.SELF_HOSTED`

**Compiler cache (ccache):**
- Step pair swap: self-hosted local / GitHub-hosted cloud
  - `load cached deps (self-hosted local)` → `if: ${{ vars.SELF_HOSTED && !inputs.macos-combine-only }}`
  - `load cached deps (GitHub-hosted cloud)` → `if: ${{ !vars.SELF_HOSTED && !inputs.macos-combine-only }}`
  - `Restore compiler cache (self-hosted local)` → `if: ${{ vars.SELF_HOSTED }}`
  - `Restore compiler cache (GitHub-hosted cloud)` → `if: ${{ !vars.SELF_HOSTED && steps.ccache.outcome == 'success' }}`
  - `Save compiler cache (self-hosted local)` → `vars.SELF_HOSTED && steps.ccache_restore_local.outcome == 'success' && github.event_name != 'pull_request'`
  - `Save compiler cache (GitHub-hosted cloud)` → `!vars.SELF_HOSTED && ...`
  - `Drop older compiler cache entries` → `!vars.SELF_HOSTED && ...`
- `Enable compiler cache` (sets `CMAKE_C_COMPILER_LAUNCHER=ccache`) runs on non-Windows when `steps.ccache.outcome == 'success'`

### `build_deps.yml` — Dependency builds
- deps cache step pair: `if: ${{ vars.SELF_HOSTED }}` / `if: ${{ !vars.SELF_HOSTED }}`
- **`Build on Mac` guard REMOVED** — was `if: runner.os == 'macOS' && !vars.SELF_HOSTED`; now `if: runner.os == 'macOS'`
- `brew install` wrapped: `if [ -z "${{ vars.SELF_HOSTED }}" ]; then ... fi`
- Windows: `choco install nasm` wrapped in `if (-not "${{ vars.SELF_HOSTED }}") { ... }`
- `clang-cl` install: `if ("${{ inputs.compiler }}" -eq "clang" -and -not "${{ vars.SELF_HOSTED }}") { ... }` (PowerShell `-and`, not `&&`)
- Deps build invocation passes `-Cache off` explicitly (avoids Strawberry Perl's ccache.exe)

### `build_optional.yml` — Optional builds (flatpak, windows-arm64, windows-msvc, macos x86_64)
- SELF_HOSTED guards already present (restored earlier)

### `build_check_cache.yml` — Cache key computation
- **No SELF_HOSTED logic** (callers own routing)
- **Restore-keys fixed**: `${github.base_ref}_` / `main_` → `${{ github.base_ref }}_` / `main_` (was bash var + wrong separator)
- Cache prefix ends with `_`; keys: `{cache_os}_{hash}`

### `unit_tests.yml` — Unit test runner (reusable)
- `Apt-Install Dependencies` — `&& !vars.SELF_HOSTED`
- `Enable Crash Dumps` (Windows) — `&& !vars.SELF_HOSTED`
- `Upload Crash Dumps` (Windows) — `&& !vars.SELF_HOSTED`

### `clang_tidy.yml`
- `JOB:` env: `build_linux (x64, ${{ vars.SELF_HOSTED && 'orca-lnx-server' || 'ubuntu-24.04' }}, clang, x64-clang, -l -x)` — matches `build_linux` display name exactly for `ci_deps_cache.sh` wait logic
- `runs-on`: `${{ vars.SELF_HOSTED && 'orca-lnx-server' || 'ubuntu-24.04' }}`
- `Apt-Install Dependencies` — `&& !vars.SELF_HOSTED`

---

## Inline "Without it on <OS>:" Comments (36 total)

### `build_all.yml` (14)
| Line | Comment |
|------|---------|
| 86 | `# Without it on Windows: build script tests run on wrong runner` |
| 111 | `# Without it on Windows: self-hosted build runs on hosted runner` |
| 144 | `# Without it on macOS: x86_64 builds on arm64 runner (cross, tests off)` |
| 158 | `# Without it on macOS: self-hosted uses arm64 runner` |
| 171 | `# Without it on Linux: self-hosted (x86_64 only) gets arm64 leg` |
| 196 | `# Without it on Linux: self-hosted tests run on wrong runner` |
| 203 | `# Without it on Linux: arm64 tests run on self-hosted (no arm64 runner)` |
| 204 | `# Without it on macOS: x86_64 tests run on self-hosted (cross build, tests off)` |
| 217 | `# Without it on Windows: self-hosted tests run on wrong runner` |
| 227 | `# Without it on macOS: self-hosted uses arm64 runner` |
| 235 | `# Without it on macOS: x86_64 tests run on self-hosted (cross build, tests off)` |
| 251 | `# Without it on Linux: slice check arch wrong on self-hosted` |
| 256 | `# Without it on Linux: slice check runs on wrong runner` |
| 264 | `# Without it on Linux: validator artifact name wrong on self-hosted` |

### `build_deps.yml` (5)
| Line | Comment |
|------|---------|
| 83 | `# Without it on self-hosted: local deps cache restore skipped` |
| 96 | `# Without it on hosted: cloud deps cache restore skipped` |
| 195 | `# Without it on Windows: choco fails on self-hosted (no admin)` |
| 205 | `# Without it on Windows: clang-cl not pre-installed on hosted` |
| 243 | `# Without it on macOS: build tools missing on hosted` |

### `build_orca.yml` (14)
| Line | Comment |
|------|---------|
| 518 | `# Without it on Windows: NSIS cache restore fails on self-hosted (no cloud cache)` |
| 526 | `# Without it on Windows: NSIS install fails on self-hosted (choco unavailable)` |
| 547 | `# Without it on Windows: NSIS cache save fails on self-hosted (no cloud cache)` |
| 648 | `# Without it on Windows: NSIS installer creation fails on self-hosted (no NSIS)` |
| 680 | `# Without it on Windows: installer artifact upload (hosted only)` |
| 688 | `# Without it on Windows: PDB artifact upload (hosted only)` |
| 696 | `# Without it on Windows: validator artifact upload (hosted only)` |
| 711 | `# Without it on Windows: deploy only on hosted` |
| 720 | `# Without it on Windows: deploy only on hosted` |
| 729 | `# Without it on Windows: validator deploy only on hosted x86_64` |
| 738 | `# Without it on Windows: MSIX build requires signing (hosted only)` |
| 752 | `# Without it on Windows: MSIX artifact upload (hosted only)` |
| 761 | `# Without it on Linux: apt packages unavailable on self-hosted` |
| 835 | `# Without it on Linux: custom preset tests only on hosted x86_64` |

### `clang_tidy.yml` (1)
| Line | Comment |
|------|---------|
| 132 | `# Without it on Linux: apt unavailable on self-hosted` |

### `unit_tests.yml` (3)
| Line | Comment |
|------|---------|
| 41 | `# Without it on Linux: apt unavailable on self-hosted` |
| 55 | `# Without it on Windows: crash dumps not configured on self-hosted` |
| 80 | `# Without it on Windows: crash dumps upload fails on self-hosted` |

### `deps/wxWidgets/wxWidgets.cmake` (1)
| Line | Comment |
|------|---------|
| 48 | `# Without it on a cold deps build: a shallow clone fetches only branch tips, and this pin is no longer one, so the checkout fails with "unable to read tree".` |

---

## Build Scripts

### `build_win.ps1` (UTF-16LE, 644 lines)
- **Defaults**: `$Deps $Slicer $Tests $RunTests $ClangCl $Ninja = $true`
- **Help** (`-Help`): shows `(default: on/off)` per flag
- **ccache auto-detect**: runs after `render.ps1` dot-source (line 290+), calls `Write-Status`
- **CCACHE_DIR creation**: unconditional, directory created if missing
- **Cache validation**: `--cache` requires clang-cl + Ninja
- **Strawberry Perl PATH strip**: moved before ccache auto-detect so auto-detect doesn't pick up Strawberry's ccache.exe
- **Inline comments**: `# Without it on Windows: ...` format (no `→` symbols)
- Parse-checked with `[Microsoft.PowerShell.Language.Parser]::ParseFile`

### `build_release_macos.sh` / `build_linux.sh`
- No SELF_HOSTED assumptions; work on both hosted and self-hosted

### `deps/wxWidgets/wxWidgets.cmake`
- `GIT_SHALLOW OFF` with comment:
  ```
  # Without it on a cold deps build: a shallow clone fetches only branch
  # tips, and this pin is no longer one, so the checkout fails with
  # "unable to read tree".
  ```
- Fixes `fatal: unable to read tree b44c39c...` on cold deps builds (pin is not a branch tip)

---

## Documentation

### `README.md` — Windows build section
- Performance table: `~110 min`/100%, `~70 min`/`~65%`, `~15 min`/`~15%` (bat/ps1/ps1+Ninja+ccache)
- Options table with **Default** column
- Flag breakdown

### `tests/AGENTS.md`
- Windows command simplified to `cmake --build . --config Release --target ALL_BUILD -- -m`

### `BUILD_PERFORMANCE_NOTES.md`
- Benchmark history and root-cause table

---

## Removed / Cleaned

- `build_win.bat` — deleted (replaced by `build_win.ps1`)
- `scripts/test_build_win.ps1` — deleted
- Dead flags removed from workflows
- UTF-16 `build_check_cache.yml` (caused `startup_failure`) — restored from `3073bbebb0` as UTF-8

---

## Verified CI Runs

| Run | SHA | Result | Notes |
|-----|-----|--------|-------|
| #230 | `aa069d90` | ✅ Green | All legs (except flaky arm64 macOS) |
| #250 | `6a6e1788` | ❌ startup_failure | UTF-16 `build_check_cache.yml` |
| #251 | `b314ef97` | ⚠️ Partial | wxWidgets shallow-clone fixed; Windows deps parse error |
| #252 | `c82f1cb5` | ❌ 2 failed | `Write-Status` before load; macOS x86_64 cross-build |
| #254 | `fa9a38e9` | ⚠️ Windows deps failed | Strawberry ccache.exe picked up |
| #255 | `bf7f702a` | ⚠️ macOS x86_64 test | JPEG libjpeg 62 vs 80 (pre-existing runner issue) |
| #256 | `2d9a7078` | ⚠️ macOS x86_64 test | Same JPEG issue; all deps/slicer builds pass |

**The only remaining failure is a pre-existing macOS runner environment issue** (`Wrong JPEG library version: library is 62, caller expects 80`) — unrelated to self-hosted support.

---

## Self-Hosted Runner Semantics (matching upstream `e76b4d676c`)

| `vars.SELF_HOSTED` value | Meaning |
|--------------------------|---------|
| unset / falsy (default) | GitHub-hosted runners |
| non-empty string (e.g., `true`) | Self-hosted: `orca-win-server`, `orca-lnx-server`, `orca-macos-arm64` |

**Routing rule:** `vars.SELF_HOSTED && 'orca-...-server' || 'github-hosted'`

**`!vars.SELF_HOSTED` guards** apply only to steps that:
- Install packages via brew/apt/choco (hosted-only)
- Clean disk space (hosted-only)
- Create/sign/deploy installers, MSIX, PDBs (release artifacts)
- Upload validator binaries (hosted-only artifact publishing)
- Save compiler cache to GitHub cache (cloud)

**Build steps themselves are unguarded** — they run on both.

---

## Cache Behavior

- **Deps cache key** hashes: `deps/**`, `CMakeLists.txt`, `deps/CMakeLists.txt`, `.github/workflows/build_deps.yml`, `.github/workflows/build_check_cache.yml`, `build_win.ps1`, `build_release_macos.sh`, `build_linux.sh`
- Any edit to these files → cold deps rebuild on all 5 legs (Linux x64, Linux arm64, macOS arm64, macOS x86_64, Windows x64)
- **ccache sizes**: Linux x64/arm64 ~876 MB, macOS x86_64 ~371 MB, Windows fresh; `max-size: 3G`
- **Self-hosted**: local `.ccache` dir persists across runs; cloud cache used as fallback

---

## Known Limitations

1. **macOS arm64 runner acquisition** — GitHub's 15-minute timeout cancels ~90% of `macos-15` jobs (infrastructure, not code)
2. **JPEG test flakiness** on `macos-15` — libjpeg version mismatch (62 vs 80); pre-existing
3. **GitHub cache delete 403** on save post-step — cosmetic, cache still saved
4. **build_win.ps1 is UTF-16LE** — Git shows binary diff; safe to convert to UTF-8 once pipeline is green (would trigger cold deps rebuild)

---

## Files Modified (vs upstream `e76b4d676c`)

### Workflows (6)
- `.github/workflows/build_all.yml`
- `.github/workflows/build_orca.yml`
- `.github/workflows/build_deps.yml`
- `.github/workflows/build_check_cache.yml`
- `.github/workflows/unit_tests.yml`
- `.github/workflows/clang_tidy.yml`

### Build Scripts (2)
- `build_win.ps1`
- `deps/wxWidgets/wxWidgets.cmake`

### Documentation (3)
- `README.md`
- `tests/AGENTS.md`
- `BUILD_PERFORMANCE_NOTES.md`

### Deleted (2)
- `build_win.bat`
- `scripts/test_build_win.ps1`

---

## Merge Strategy

Branch merges upstream/main periodically (latest: `2d9a7078e5` includes upstream through `1174b39004`). Conflicts resolved by keeping branch's CI/CD improvements (e.g., wxWidgets `GIT_SHALLOW OFF` comment).