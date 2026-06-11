# MSIX Microsoft Store Build — Design

Date: 2026-06-11. Status: approved (design review with maintainer).
Planning/requirements background: `MSIX_STORE_HANDOFF.md` (repo root,
validated against Microsoft Learn docs and Store Policies v7.19).

## Goal

Produce an unsigned `.msix` of OrcaSlicer in CI, built from the same
Windows install tree as the NSIS installer, suitable for manual upload to
Partner Center. The Store re-signs with Microsoft's certificate, which
makes the Store build install cleanly on Smart App Control (SAC) machines
that block the unsigned NSIS installer.

Everything is additive: existing build outputs (NSIS exe, portable zip)
are untouched, and every runtime behavior change is gated behind a
packaged-context check so classic builds behave exactly as today.

## Decisions (already made)

- Packaging path: hand-written `AppxManifest.xml` + `makeappx pack` over
  the existing install tree. No CPack MSIX generator exists.
- Minimum Windows: 10 1903 (`MinVersion 10.0.18362.0`), declaring BOTH
  virtualization elements (Win11 fine-grained exclusion + Win10 coarse
  disable). Each OS honors the one it supports.
- Network plugin: keep the runtime downloader, do NOT bundle Bambu's
  closed-source DLLs in the package.
- Config stays at the real `%APPDATA%\OrcaSlicer` via manifest
  virtualization exclusions + `unvirtualizedResources` restricted
  capability. No `data_dir` code changes.
- Submission is manual via Partner Center (first submission must be);
  CI only produces the artifact.

## Components

### 1. `scripts/msix/` — packaging files (new)

Precedent: platform packaging already lives under `scripts/` (flatpak).

**`AppxManifest.xml`** — template with four substitution tokens:
`@MSIX_VERSION@`, `@MSIX_IDENTITY_NAME@`, `@MSIX_PUBLISHER@`,
`@MSIX_PUBLISHER_DISPLAY_NAME@` (all three identity strings must match
the Partner Center-assigned values exactly).
Key contents (exact schema/namespace details verified against the MSIX
schema reference during implementation; `makeappx`/WACK validate):
- `TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.18362.0"`.
- Capabilities: `runFullTrust` + `rescap:unvirtualizedResources`.
- Under `Properties`, both virtualization elements:
  - `virtualization:FileSystemWriteVirtualization` with
    `ExcludedDirectory = $(KnownFolder:RoamingAppData)\OrcaSlicer`
    (Windows 11 fine-grained), and
  - `desktop6:FileSystemWriteVirtualization = disabled`
    (Windows 10 1903+ coarse fallback).
- `Application Executable="orca-slicer.exe"
  EntryPoint="Windows.FullTrustApplication"` (launcher output name set
  at `src/CMakeLists.txt:189`).
- Extensions:
  - `windows.fileTypeAssociation` for `.3mf`, `.stl`, `.step`/`.stp`,
    `.gcode`, `.drc`. Note: `.gcode` and `.drc` are opt-in in the
    classic build; manifest declarations are static, so they are
    always declared. Declaring an FTA only adds OrcaSlicer to "Open
    with" — the user controls defaults via Windows Settings, so this is
    not a behavior regression.
  - `windows.protocol` for `orcaslicer://`. The classic build also
    offers opt-in handlers for `prusaslicer://`, `bambustudio://` and
    `cura://` (Preferences "Associate" tab); those are NOT declared in
    the manifest for now (possible follow-up) and their toggles are
    hidden in packaged context.
  - `rescap3:MigrationProgIds` declaring `Orca.Slicer.1`.
    KNOWN LIMITATION: the classic build writes its ProgID with a
    leading space (`L" Orca.Slicer.1"`, `GUI_App.cpp:9119`), which
    MigrationProgIds cannot express. Migration hand-off from existing
    NSIS installs may therefore be partial; Windows shows both apps in
    "Open with" and the user picks once. The legacy ProgID is NOT fixed
    in this change (separate change with its own migration risk).

**`assets/`** — committed PNGs generated once from
`resources/images/OrcaSlicer.svg`: Square44x44Logo, Square150x150Logo,
StoreLogo (50x50), plus standard scale variants. Committed rather than
generated in CI so the pack step has no extra tool dependencies.

**`build_msix.ps1`** — single script, runnable in CI and locally (needs
Windows SDK for `makeappx`):

1. Parse `version.inc` `SoftFever_VERSION` (`2.4.0-dev` → `2.4.0.0`;
   MAJOR.MINOR.PATCH from the leading semver triplet, revision fixed
   at 0 as the Store requires).
2. Stage: copy the install tree (`build/OrcaSlicer`) + token-substituted
   manifest + `assets/` into a temp layout.
3. `makeappx pack` → `OrcaSlicer_Windows_MSIX_<version>.msix`.

Parameters: `-InstallDir`, `-OutputPath`, `-IdentityName`, `-Publisher`,
`-PublisherDisplayName` (identity defaults are the reserved Partner
Center identity — `OrcaSlicer.OrcaSlicer` /
`CN=38F7EA55-C73B-4072-B3B2-C8E0EA15BB82` / `OrcaSlicer`). NO signing — the Store
strips and re-signs; local testing uses Developer Mode loose-layout
registration instead.

### 2. CI — `.github/workflows/build_orca.yml`

One new step in the Windows job, after the install tree exists (adjacent
to the portable-zip step): run `build_msix.ps1`, upload the `.msix` as a
workflow artifact. Repo variables
(`vars.ORCA_MSIX_IDENTITY_NAME`, `vars.ORCA_MSIX_PUBLISHER`,
`vars.ORCA_MSIX_PUBLISHER_DISPLAY_NAME`) can override the identity but
are optional; when unset, the in-repo defaults carry the reserved
Partner Center identity, so the artifact is Store-uploadable as-is.
The step must not fail the job when repo variables are absent (forks).

### 3. Runtime changes (Windows-only, packaged-context-gated)

**New helper** — `bool is_running_in_msix()` in
`src/slic3r/GUI/GUI_Utils.{hpp,cpp}`: cached null-buffer
`GetCurrentPackageFullName` probe (`ERROR_INSUFFICIENT_BUFFER` ⇒
packaged, `APPMODEL_ERROR_NO_PACKAGE` ⇒ not); constant `false` on
non-Windows.

**Updater redirection (R4)** — Store apps must not self-update, but
OrcaSlicer's version check is notification-only (it never
auto-downloads), so the check itself stays enabled when packaged:

- The startup auto-check (`check_new_version_sf()`) and the manual
  "Check for updates" menu action run unchanged.
- The new-version dialog (`UpdateVersionDialog`) changes when packaged:
  the Download button is hidden, the info text tells the user to update
  OrcaSlicer from the Microsoft Store, and the "Check on Github"
  hyperlink becomes "Open Microsoft Store", which opens the Store
  listing via `ms-windows-store://pdp/?PFN=<family>`. The package
  family name comes from `GetCurrentPackageFamilyName` at runtime — no
  build-time ProductId define or extra repo variable needed, and it
  works identically in pre- and post-reservation builds. The packaged
  build never opens the GitHub release page.

**Association suppression (R3)** — the manifest owns shell integration;
runtime registry writes are virtualized and invisible:

- Early-return in `associate_files`, `disassociate_files`,
  `associate_url`, `disassociate_url` when packaged.
- Hide the whole "Associate" tab in Preferences
  (`Preferences.cpp:1848-1883`) in packaged context — the
  file-association checkboxes and the `prusaslicer://` /
  `bambustudio://` / `cura://` URL-handler rows all rely on runtime
  registry writes that are virtualized. The `check_url_association`
  consumers live exclusively in that tab, so no other prompt suppression
  is needed.

## What does NOT change

- `data_dir` / config location code — `%APPDATA%\OrcaSlicer` everywhere.
- NSIS/CPack packaging, portable zip, all non-Windows builds.
- Bambu network plugin download flow (works against the real AppData
  path; SAC behavior verified per the test plan).
- Classic-build association/updater behavior (gates are packaged-only).

## Verification

No meaningful unit-test surface (Windows shell + packaging behavior);
the PR documents manual verification per repo review guidelines:

1. Local loose-layout install (Developer Mode,
   `Add-AppxPackage -Register AppxManifest.xml` over the staged layout):
   app launches; `.3mf` open-with and `orcaslicer://` activation work;
   single-instance hand-off works when launched via the package alias.
2. Config interop: profiles created by a classic install are visible in
   the packaged app and vice versa (Win11 machine); profiles survive
   packaged-app uninstall.
3. Updater: version check runs as in the classic build (startup +
   menu); the new-version dialog hides the Download button, asks the
   user to update from the Microsoft Store, and its "Open Microsoft
   Store" link opens the Store listing.
4. Network plugin: download + load succeeds in packaged context.
5. WACK run against the CI artifact passes (or failures triaged).
6. CI: Windows job produces the `.msix` artifact with correct version;
   NSIS and portable artifacts byte-identical in content to before
   (spot-check).
7. Post-certification only: SAC end-to-end test via private-audience
   listing (cannot be done locally; Store signature is the variable
   under test).

## Sequencing / rollout

The Partner Center app name reservation is complete and the reserved
identity (identity name, publisher `CN=<GUID>`, publisher display name)
is now the in-repo default, so CI produces the uploadable package
without further configuration; the three repo variables remain as an
optional override. First Store submission is manual (listing, IARC,
`runFullTrust` + `unvirtualizedResources` justifications) per the
handoff doc's "Submission process" section.

Main external risk (tracked in handoff doc R2.5): Store approval of
`unvirtualizedResources` for a non-game app is not guaranteed; fallback
is documented there and does not change this design's code shape.
