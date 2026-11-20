# CLAUDE.md

OrcaSlicer — open-source C++17 3D slicer. wxWidgets GUI, CMake build system.

## Build Commands

Build the Release configuration unless asked otherwise.

```bash
# macOS
cmake --build build/arm64 --config Release --target all --

# Linux
cmake --build build --config Release --target all --

# Windows
cmake --build . --config Release --target ALL_BUILD -- -m
```

## Testing

Catch2 framework. Tests in `tests/`; see [tests/AGENTS.md](tests/AGENTS.md) for where a new test belongs and the conventions to follow.

```bash
cd build && ctest -C Release --output-on-failure    # all tests
ctest --test-dir ./tests/libslic3r -C Release       # individual suite
ctest --test-dir ./tests/fff_print -C Release
```

## Documentation

- Docs live in `docs/`; the high-level design of a subsystem goes in `docs/HLSD/<subsystem>.md`.
- Describe the design as it stands — what the subsystem does, why it exists, and the constraints that shape it. Not the route that got there: no phases, task lists, status markers, or "before/after this PR" framing.
- Planning and investigation output (brainstorms, superpowers design and plan docs) stays in `docs/superpowers/`, which is gitignored. Never commit it.
- Write a doc only when the design is not evident from the code, and when a change invalidates an existing one, update it in the same PR.

## Code Style

- C++17, selective C++20. PascalCase classes, snake_case functions/variables
- `#pragma once` for headers. Smart pointers and RAII preferred
- Include what you use: include the header for every symbol a file uses, and keep headers compilable on their own. Never rely on the precompiled header or a transitive include. The `clang-tidy` CI job enforces this on changed lines; run the same check locally with `scripts/run_clang_tidy.sh` (`scripts\run_clang_tidy.ps1` on Windows), which sets up everything it needs
- Parallelization via TBB — be mindful of shared state
- Always use `SetSizerAndFit(sizer)` instead of `SetSizer(sizer)` on top level window. Unless `SetSizer` must be called before the full layout is built, call `sizer->SetSizeHints(window)` afterwards in this case.

## Key Entry Points

- App startup: `src/OrcaSlicer.cpp`
- Slicing pipeline: `src/libslic3r/Print.cpp`
- All print/printer/material settings: `src/libslic3r/PrintConfig.cpp`
- GUI: `src/slic3r/GUI/`
- Core algorithms: `src/libslic3r/` (GCode/, Fill/, Support/, Geometry/, Format/, Arachne/)
- Printer profiles: `resources/profiles/[manufacturer].json`

## Critical Constraints

- **Backward compatibility required** for .3mf project files and printer profiles
- **Cross-platform** — all changes must work on Windows, macOS, and Linux
- Profile/format changes need version migration handling
- Dependencies built separately in `deps/build/`, then linked to main app

## Pull requests

Before opening a pull request or posting on GitHub, read [CONTRIBUTING.md](CONTRIBUTING.md), including its "If you are an AI agent" section.

## Code review focus areas

- Changes must not cause regressions in existing functionality, defaults, profiles, or project compatibility.
- Features gated by options must not affect existing behavior when those options are disabled.
- Changes should follow the existing code style and architecture. Architectural changes should be justified in code comments and the PR description.
- Add helper functions or utilities only when existing code cannot reasonably be reused. Avoid duplication.
- Keep code concise and clear. Manually simplify AI generated bloated codes before review.
- Include targeted tests or documented verification for behavior changes, especially in slicing logic, profiles, formats, and GUI defaults.
- For profile changes (`resources/profiles/<Vendor>/**`), check that `version` in the sibling `resources/profiles/<Vendor>.json` was bumped.
- For new or changed UI strings, check they follow [Writing translatable strings in the source](#writing-translatable-strings-in-the-source): no concatenated fragments, plural forms for counts, contexts for ambiguous words.
- For translation changes (`localization/i18n/**/*.po`), check that recurring terms match the [Localization glossary](https://github.com/OrcaSlicer/OrcaSlicer_WIKI/blob/main/developer_reference/localization_glossary.md) for that language.

## Localization & translations

Catalogs live in `localization/i18n/<lang>/OrcaSlicer_<lang>.po`; the template is `OrcaSlicer.pot`.
See the [Localization guide](https://github.com/OrcaSlicer/OrcaSlicer_WIKI/blob/main/developer_reference/localization_guide.md) for the human-facing version of these principles.

### Writing translatable strings in the source

Translators see one msgid at a time and cannot change the code around it, so each message must be complete in a single msgid. Use the wrapper that fits instead of assembling text from pieces:

- **One msgid per sentence, with placeholders.** Never build a message by concatenating translated fragments (`_L("A") + name + _L(" is B")`) or by splicing a translated word such as `_L("left")` into another sentence: the translator can't reorder or inflect the parts. Write `format(_L("%1% is too close to others"), name)`.
- **A count needs a plural form.** Don't write `file(s)` or leave a verb unchanged after a number. Use `_L_PLURAL("%1% object", "%1% objects", n)`. libslic3r has no plural lookup, so word the message so it doesn't depend on a count, or build it in the GUI.
- **An ambiguous word needs a context.** When a short string means different things in different places (verb or noun, a mode name, a CAD term), give each sense its own context with `_L_CONTEXT`/`_u8L_CONTEXT`/`L_CONTEXT`. Keep the most common sense without a context so its existing translations stay valid.
- **Mark every user-visible string** (`_L`/`_u8L`, or `L` where it is translated later) and list new source files in `localization/i18n/list.txt`: files that aren't listed are never extracted.
- Add a `// TRN` comment above a new string whose meaning or placeholders aren't obvious.
- Never choose behavior by comparing translated text; compare ids or flags.

See `.claude/skills/orca-translations/references/source-audit.md` for the full checklist.

### Terminology

- Use the [Localization glossary](https://github.com/OrcaSlicer/OrcaSlicer_WIKI/blob/main/developer_reference/localization_glossary.md) as the source of truth for recurring terms, so the same English term is always rendered the same way within a language, and terms that must stay in English (brand/product names, acronyms, materials, file formats, G-code tokens, macros/variables/identifiers) are not translated.
- If a term's established translation changes, update both the affected `.po` files and the glossary (`developer_reference/glossary/<lang>.tsv` in the wiki, then regenerate) so they stay in sync.
- Translate the *meaning*, not the words. Check what the string actually controls before translating it — English reuses one word for different things. `Flow ratio` (multiplier), `Flow Rate` (throughput) and `Flow Dynamics` (pressure compensation) are three different terms; `extruder` may mean the toolhead, the feeder motor, or the nozzle depending on the string.
- Reuse one template per recurring message shape (`Failed to connect to …`, `Are you sure you want to …?`), even where the English wording varies.

### Editing rules

- Only edit `msgstr` — **never** change `msgid`, and never "fix" wrong English in the translation alone. Report the source string instead.
- Preserve exactly: placeholders (`%s`, `%d`, `%1%`, `%zu`, `%%`), every `\n` (count *and* position, including leading/trailing), leading/trailing spaces, HTML tags, `℃`, and the file's encoding and line endings.
- **Never reorder positional arguments** in a `c-format` string. If the msgid is `%d` then `%s`, that order must hold — swapping them breaks at runtime.
- `msgctxt` separates homonyms — always read it. `Back`/`Camera View` is the rear view of the 3D navigator, while `Back`/`Navigation` is the go-back button; `Top` exists in the *Alignment*, *Layers* and *Camera View* senses.
- When a string needs disambiguating, add context in the source (`_L_CONTEXT`/`_u8L_CONTEXT`), don't work around it in the translation.
- A literal `%` inside a string xgettext flagged `possible-c-format` will fail `msgfmt`. Fix it with a `// xgettext:no-c-format, no-boost-format` comment above the string in the source — do not mangle the translation or use `%%` in text that is never passed through printf.
- Plural entries: read `nplurals` from the catalog's `Plural-Forms` header (it is **not** always 2 — ja/ko/zh/th/vi use 1, ru/cs/pl/lt use 3, uk uses 4). Each form must be genuinely inflected for its quantity; repeating one sentence across all forms is a bug in Slavic/Baltic languages, though it is correct for Turkish and Hungarian.
- An entry whose `msgstr` equals its `msgid` is untranslated even though it is not empty; a plural entry with any empty form is likewise incomplete.
- Mark machine-produced translations with an `# AI Translated` translator comment. Don't add it to a human translation you didn't actually rewrite.
- When you can't be sure of a machine translation's meaning or UI wording, also add `# Needs human review: <what to check>`. Never use `fuzzy` for this — fuzzy entries are hidden from users.
- Don't reflow or re-wrap unrelated entries — keep the diff limited to the strings you changed.

### Verifying

- `scripts/run_gettext.bat --full` (Windows) regenerates the template, merges every catalog and compiles the `.mo` files. It must exit 0.
- Or check a single catalog with `msgfmt --check-format --check-header -o <out>.mo localization/i18n/<lang>/OrcaSlicer_<lang>.po`. Without `--check-header`, msgfmt skips the placeholder check in plural forms.
- Fuzzy entries are not shown to users. If you correct one, clear its `fuzzy` flag, otherwise the fix never ships.

## GitHub Actions CI/CD Toolkit

Reusable pipeline management scripts live in `scripts/gha/`:

- **`OrcaCI.psm1`** — PowerShell module with 6 functions for GitHub Actions API:
  - `Get-Runs` — list workflow runs with filtering by branch/status
  - `Get-RunDetails` — get full run metadata
  - `Get-Jobs` — list jobs for a run with status/conclusion
  - `Cancel-Run` — cancel a workflow run
  - `Dispatch-Workflow` — trigger a workflow_dispatch event
  - `Format-Duration` — human-readable time formatting

- **`ci_build.ps1`** — Orchestrator script that:
  1. Checks for pending/in-progress runs and cancels them (clears concurrency group)
  2. Dispatches a new workflow_dispatch
  3. Waits for completion with live progress (shows running jobs, runners)
  4. Reports final status with job summary (success/failed/skipped counts)

- **`README.md`** — Usage documentation with examples

Usage:
```powershell
# Just dispatch and exit
.\scripts\gha\ci_build.ps1 -Token $env:GITHUB_TOKEN -Wait false

# Full pipeline with wait and queue clearing
.\scripts\gha\ci_build.ps1 -Token $env:GITHUB_TOKEN -TimeoutMinutes 180
```

## CI Pipeline Status (feat/build-performance)

**Branch:** `feat/build-performance` (fork: `Depact/OrcaSlicer`)

**Self-hosted runner:** `HOME-LAPTOP` (Windows, X64, labels: `self-hosted`, `Windows`, `X64`)

**Current status (Run #133, SHA `cb61d5a4`):** 
- ✅ **Main build passes** — `Build slicer Win` (step 38) succeeds on self-hosted Windows runner
- ❌ **Post-build preset cache fails** — Step 39 "Build system preset cache (Windows)" fails in ~2 seconds
- All previous blockers resolved:
  - Self-hosted runner `HOME-LAPTOP` (id: 25, labels: self-hosted/Windows/X64) picks up jobs
  - `check_build_script` passes on GitHub-hosted
  - `check_cache` passes on self-hosted runner
  - Main OrcaSlicer build (`Build slicer Win`, step 38) succeeds
  - Linux/macOS/Flatpak jobs disabled via `if: false`

**Fixes in `build_win.ps1` (commit `cb61d5a4`):**
- Strips `Strawberry\c\bin` (broken cmake.exe with bad FindIconv.cmake)
- Keeps `c\perl\bin` (OpenSSL Configure needs Perl)
- Adds `-DCMAKE_PREFIX_PATH=$depsInstallDir` for JPEG support on self-hosted
- Adds `-DORCA_TOOLS=ON` to enable `generate_system_cache.exe` for preset cache
- Downloads official CMake 3.29.3 as fallback when PATH cmake is broken

**Known blocker:** Post-build preset cache step 39 "Build system preset cache (Windows)" fails in ~2 seconds after main build succeeds. The `-DORCA_TOOLS=ON` flag enables `generate_system_cache.exe` build, but the tool execution fails. Needs investigation of `generate_system_cache.exe` runtime dependencies or CMake configuration.
