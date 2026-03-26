# Repository Guidelines

## Build Constraints
- **Never build with more than 3 cores** - use `-j3` or `./build_linux.sh -j3`
- Use wrapper script: `./build_linux.sh -rs` for quick rebuilds
- Direct cmake/ninja: `cmake --build build --parallel 3` (no more than 3)

## Project Structure & Module Organization
OrcaSlicer's C++17 sources live in `src/`, split by feature modules and platform adapters. User assets, icons, and printer presets are in `resources/`; translations stay in `localization/`. Tests sit in `tests/`, grouped by domain (`libslic3r/`, `sla_print/`, etc.) with fixtures under `tests/data/`. CMake helpers reside in `cmake/`, and longer references in `doc/` and `SoftFever_doc/`. Automation scripts belong in `scripts/` and `tools/`. Treat everything in `deps/` and `deps_src/` as vendored snapshots—do not modify without mirroring upstream tags.

## Build, Test, and Development Commands
Use out-of-source builds:
- Platform helpers: `build_linux.sh`, `build_release_macos.sh`, `build_release_vs2022.bat`. Use `./build_linux.sh -dsi` for full build, `-rs` for quick rebuild. Always add `-j3` to limit building to 3 cores max.
- DO NOT EVER ATTEMPT TO BUILD WITH `cmake` OR `ninja` DIRECTLY!

## CLI Slicing
The CLI requires a GUI environment (gtk initialization). Without display, it hangs. Options:
- Run from GUI session
- Use `--slice 0` to slice all plates
- Thumbnail options: `--thumbnail-mode sliced`, `--thumbnail-shading-mode 0|1`, `--thumbnail-rolling-avg-segments N`
- Example: `orca-slicer --slice 0 --outputdir /tmp/out --thumbnail-mode sliced tests/file.3mf`

## Coding Style & Naming Conventions
`.clang-format` enforces 4-space indents, a 140-column limit, aligned initializers, and brace wrapping for classes and functions. Run `clang-format -i <file>` before committing; the CMake `clang-format` target is available when LLVM tools are on your PATH. Prefer `CamelCase` for classes, `snake_case` for functions and locals, and `SCREAMING_CASE` for constants, matching conventions in `src/`. Keep headers self-contained and align include order with the IWYU pragmas.

## Testing Guidelines
Unit tests rely on Catch2 (`tests/catch2/`). Name specs after the component under test—for example `tests/libslic3r/TestPlanarHole.cpp`—and tag long-running cases so `ctest -L fast` remains useful. Cover new algorithms with deterministic fixtures or sample G-code stored in `tests/data/`. Document manual printer validation or regression slicer checks in your PR when automated coverage is insufficient.

## Commit & Pull Request Guidelines
The history favors concise, sentence-style subject lines with optional issue references, e.g., `Fix grid lines origin for multiple plates (#10724)`. Squash fixups locally before opening a PR. Complete `.github/pull_request_template.md`, include reproduction steps or screenshots for UI changes, and mention impacted presets or translations. Link issues via `Closes #NNNN` when applicable, and call out dependency bumps or profile migrations for maintainer review.

## Security & Configuration Tips
Follow `SECURITY.md` for vulnerability reporting. Keep API tokens and printer credentials out of tracked configs; use `sandboxes/` for experimental settings. When touching third-party code in `deps_src/`, record the upstream commit or release in your PR description and run the relevant platform build script to confirm integration.
