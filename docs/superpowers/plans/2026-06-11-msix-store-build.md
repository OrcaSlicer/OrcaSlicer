# MSIX Microsoft Store Build Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build an unsigned MSIX Store package of OrcaSlicer in CI from the existing Windows install tree, and make the app behave correctly in packaged context (no self-update, no runtime registry-based file associations).

**Architecture:** A hand-written `AppxManifest.xml` template + PowerShell pack script under `scripts/msix/` (precedent: `scripts/flatpak/`), one new CI step in the Windows job of `.github/workflows/build_orca.yml`, and a small packaged-context detection helper in `src/slic3r/GUI/GUI_Utils.{hpp,cpp}` used to gate the updater and association code. Everything is additive: NSIS/portable outputs are untouched, all runtime gates are no-ops in classic builds.

**Tech Stack:** PowerShell 5.1+ (System.Drawing, makeappx from Windows SDK), GitHub Actions, C++17/wxWidgets (Win32 `GetCurrentPackageFullName`/`GetCurrentPackageFamilyName` from `appmodel.h`, kernel32).

**Spec:** `docs/superpowers/specs/2026-06-11-msix-store-build-design.md`
**Branch:** `msix-store-build` (already created; spec is committed on it)

**Why no TDD here:** none of this is unit-testable in this repo — the C++ changes live in the wxWidgets GUI module which no test target links (tests cover `libslic3r`), and the behavior under test is Windows shell/packaging behavior. Each task instead has concrete verification steps (script output checks, XML well-formedness, full app build), and Task 8 collects the manual verification documented in the PR per the repo's review guidelines.

**Windows build command** (required for any task touching C++; plain shells fail without the VS environment — run via PowerShell):

```
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build build --config RelWithDebInfo --target OrcaSlicer 2>&1'
```

The `'vswhere.exe' is not recognized` line vcvars prints is harmless. Expected: build completes with exit code 0 (warnings OK). Incremental builds are fast when warm.

---

### Task 1: MSIX logo assets + generator script

**Files:**
- Create: `scripts/msix/generate_assets.ps1`
- Create (generated): `scripts/msix/assets/Square150x150Logo.png`, `scripts/msix/assets/Square44x44Logo.png`, `scripts/msix/assets/Square44x44Logo.targetsize-44_altform-unplated.png`, `scripts/msix/assets/StoreLogo.png`

- [ ] **Step 1: Write the generator script**

Create `scripts/msix/generate_assets.ps1` with exactly this content:

```powershell
# Generates the MSIX package logo assets from the 192px master logo.
# Run once on Windows (re-run if the logo changes), then commit the PNGs in assets/.
# Scale-200 variants would need a >192px master; regenerate from OrcaSlicer.svg if ever needed.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$source   = Join-Path $repoRoot 'resources\images\OrcaSlicer_192px.png'
$outDir   = Join-Path $PSScriptRoot 'assets'
New-Item -ItemType Directory -Force $outDir | Out-Null

$sizes = @{
    'Square150x150Logo.png'                              = 150
    'Square44x44Logo.png'                                = 44
    'Square44x44Logo.targetsize-44_altform-unplated.png' = 44
    'StoreLogo.png'                                      = 50
}

$src = [System.Drawing.Image]::FromFile($source)
foreach ($name in $sizes.Keys) {
    $px  = $sizes[$name]
    $bmp = New-Object System.Drawing.Bitmap($px, $px)
    $gfx = [System.Drawing.Graphics]::FromImage($bmp)
    $gfx.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $gfx.DrawImage($src, 0, 0, $px, $px)
    $gfx.Dispose()
    $bmp.Save((Join-Path $outDir $name), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Output "Wrote $name (${px}x${px})"
}
$src.Dispose()
```

- [ ] **Step 2: Run it**

Run (PowerShell, from repo root): `./scripts/msix/generate_assets.ps1`
Expected output: four `Wrote <name> (<px>x<px>)` lines, no errors.

- [ ] **Step 3: Verify the generated dimensions**

```powershell
Add-Type -AssemblyName System.Drawing
Get-ChildItem scripts/msix/assets/*.png | ForEach-Object { $i = [System.Drawing.Image]::FromFile($_.FullName); "$($_.Name): $($i.Width)x$($i.Height)"; $i.Dispose() }
```

Expected: `Square150x150Logo.png: 150x150`, `Square44x44Logo.png: 44x44`, `Square44x44Logo.targetsize-44_altform-unplated.png: 44x44`, `StoreLogo.png: 50x50`.

- [ ] **Step 4: Commit**

```bash
git add scripts/msix/generate_assets.ps1 scripts/msix/assets
git commit -m "ci: add MSIX logo asset generator and generated assets"
```

---

### Task 2: AppxManifest.xml template

**Files:**
- Create: `scripts/msix/AppxManifest.xml`

- [ ] **Step 1: Write the manifest template**

Create `scripts/msix/AppxManifest.xml` with exactly this content. The four `@...@` tokens are substituted by `build_msix.ps1` (Task 3). Namespace URIs and the virtualization/MigrationProgIds element shapes were verified against learn.microsoft.com (flexible-virtualization and element-rescap3-migrationprogids pages) on 2026-06-11. `MigrationProgId` is `Orca.Slicer.1` — note the classic build writes the registry ProgID with a leading space (`GUI_App.cpp:9119`), which MigrationProgIds cannot express, so migration from existing NSIS installs is best-effort (Windows shows both apps in "Open with"); deliberately NOT fixed here.

```xml
<?xml version="1.0" encoding="utf-8"?>
<Package
  xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
  xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"
  xmlns:rescap="http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities"
  xmlns:rescap3="http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities/3"
  xmlns:desktop6="http://schemas.microsoft.com/appx/manifest/desktop/windows10/6"
  xmlns:virtualization="http://schemas.microsoft.com/appx/manifest/virtualization/windows10"
  IgnorableNamespaces="uap rescap rescap3 desktop6 virtualization">

  <Identity Name="@MSIX_IDENTITY_NAME@"
            Publisher="@MSIX_PUBLISHER@"
            Version="@MSIX_VERSION@"
            ProcessorArchitecture="x64" />

  <Properties>
    <DisplayName>OrcaSlicer</DisplayName>
    <PublisherDisplayName>@MSIX_PUBLISHER_DISPLAY_NAME@</PublisherDisplayName>
    <Logo>Assets\StoreLogo.png</Logo>
    <!-- Keep config in the real %APPDATA%\OrcaSlicer so it survives uninstall and
         is shared with the classic (NSIS/portable) install.
         Win10 1903+: coarse switch disables AppData write virtualization entirely.
         Win11+: fine-grained exclusion below takes precedence over the coarse switch. -->
    <desktop6:FileSystemWriteVirtualization>disabled</desktop6:FileSystemWriteVirtualization>
    <virtualization:FileSystemWriteVirtualization>
      <virtualization:ExcludedDirectories>
        <virtualization:ExcludedDirectory>$(KnownFolder:RoamingAppData)\OrcaSlicer</virtualization:ExcludedDirectory>
      </virtualization:ExcludedDirectories>
    </virtualization:FileSystemWriteVirtualization>
  </Properties>

  <Dependencies>
    <TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.18362.0" MaxVersionTested="10.0.26100.0" />
  </Dependencies>

  <Resources>
    <Resource Language="en-us" />
  </Resources>

  <Applications>
    <Application Id="OrcaSlicer" Executable="orca-slicer.exe" EntryPoint="Windows.FullTrustApplication">
      <uap:VisualElements
        DisplayName="OrcaSlicer"
        Description="Open-source slicer for FDM 3D printers"
        BackgroundColor="transparent"
        Square150x150Logo="Assets\Square150x150Logo.png"
        Square44x44Logo="Assets\Square44x44Logo.png" />
      <Extensions>
        <uap:Extension Category="windows.fileTypeAssociation">
          <uap:FileTypeAssociation Name="orcaslicer-models">
            <uap:SupportedFileTypes>
              <uap:FileType>.3mf</uap:FileType>
              <uap:FileType>.stl</uap:FileType>
              <uap:FileType>.step</uap:FileType>
              <uap:FileType>.stp</uap:FileType>
              <uap:FileType>.gcode</uap:FileType>
              <uap:FileType>.drc</uap:FileType>
            </uap:SupportedFileTypes>
            <rescap3:MigrationProgIds>
              <rescap3:MigrationProgId>Orca.Slicer.1</rescap3:MigrationProgId>
            </rescap3:MigrationProgIds>
          </uap:FileTypeAssociation>
        </uap:Extension>
        <uap:Extension Category="windows.protocol">
          <uap:Protocol Name="orcaslicer" />
        </uap:Extension>
      </Extensions>
    </Application>
  </Applications>

  <Capabilities>
    <rescap:Capability Name="runFullTrust" />
    <rescap:Capability Name="unvirtualizedResources" />
  </Capabilities>
</Package>
```

- [ ] **Step 2: Verify XML is well-formed**

Run (PowerShell): `[xml](Get-Content scripts/msix/AppxManifest.xml -Raw) | Out-Null; 'XML OK'`
Expected: `XML OK` (no exception).

- [ ] **Step 3: Commit**

```bash
git add scripts/msix/AppxManifest.xml
git commit -m "ci: add MSIX AppxManifest template"
```

---

### Task 3: build_msix.ps1 pack script

**Files:**
- Create: `scripts/msix/build_msix.ps1`

- [ ] **Step 1: Write the pack script**

Create `scripts/msix/build_msix.ps1` with exactly this content:

```powershell
<#
Builds the unsigned MSIX Store package from an existing install tree.
The package is intentionally NOT signed: the Microsoft Store strips and
re-signs uploads with Microsoft's certificate. For local installs use
Developer Mode loose-layout registration instead:
  ./scripts/msix/build_msix.ps1 -StageOnly
  Add-AppxPackage -Register <staging>\AppxManifest.xml
Requires the Windows SDK (makeappx.exe) unless -StageOnly is used.
#>
param(
    [string]$InstallDir = "build/OrcaSlicer",
    [string]$OutputPath = "build/OrcaSlicer_Windows_MSIX.msix",
    [string]$StagingDir = "",
    [switch]$StageOnly,
    [string]$IdentityName = "OrcaSlicer.Dev",
    [string]$Publisher = "CN=00000000-0000-0000-0000-000000000000",
    [string]$PublisherDisplayName = "OrcaSlicer (dev)"
)
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent

# MSIX version = MAJOR.MINOR.PATCH.0 from the SoftFever_VERSION semver triplet
# (Store requires the revision field to be 0).
$versionContent = Get-Content (Join-Path $repoRoot 'version.inc') -Raw
if ($versionContent -notmatch 'set\(SoftFever_VERSION "(\d+)\.(\d+)\.(\d+)') {
    throw "Could not parse SoftFever_VERSION from version.inc"
}
$msixVersion = "$($Matches[1]).$($Matches[2]).$($Matches[3]).0"
Write-Output "MSIX version: $msixVersion"

if (-not (Test-Path (Join-Path $InstallDir 'orca-slicer.exe'))) {
    throw "orca-slicer.exe not found in '$InstallDir' - build the install tree first"
}

if ([string]::IsNullOrEmpty($StagingDir)) {
    $StagingDir = Join-Path ([System.IO.Path]::GetTempPath()) 'orca-msix-staging'
}
if (Test-Path $StagingDir) { Remove-Item $StagingDir -Recurse -Force }
New-Item -ItemType Directory -Force $StagingDir | Out-Null

Copy-Item -Path (Join-Path $InstallDir '*') -Destination $StagingDir -Recurse
Copy-Item -Path (Join-Path $PSScriptRoot 'assets') -Destination (Join-Path $StagingDir 'Assets') -Recurse

$manifest = Get-Content (Join-Path $PSScriptRoot 'AppxManifest.xml') -Raw
$manifest = $manifest.Replace('@MSIX_VERSION@', $msixVersion)
$manifest = $manifest.Replace('@MSIX_IDENTITY_NAME@', $IdentityName)
$manifest = $manifest.Replace('@MSIX_PUBLISHER@', $Publisher)
$manifest = $manifest.Replace('@MSIX_PUBLISHER_DISPLAY_NAME@', $PublisherDisplayName)
Set-Content -Path (Join-Path $StagingDir 'AppxManifest.xml') -Value $manifest -Encoding utf8

if ($StageOnly) {
    Write-Output "Staged loose layout at: $StagingDir"
    exit 0
}

$makeappx = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.*\x64\makeappx.exe" -ErrorAction SilentlyContinue |
    Sort-Object { [version]$_.Directory.Parent.Name } -Descending |
    Select-Object -First 1 -ExpandProperty FullName
if (-not $makeappx) {
    throw "makeappx.exe not found under '${env:ProgramFiles(x86)}\Windows Kits\10\bin' - install the Windows SDK"
}
Write-Output "Using makeappx: $makeappx"

& $makeappx pack /d $StagingDir /p $OutputPath /o
if ($LASTEXITCODE -ne 0) { throw "makeappx pack failed with exit code $LASTEXITCODE" }
Write-Output "Packed: $OutputPath"
```

- [ ] **Step 2: Verify locally (conditional)**

If `build/OrcaSlicer/orca-slicer.exe` exists on this machine, run from repo root:
`./scripts/msix/build_msix.ps1 -OutputPath build/OrcaSlicer_test.msix`
Expected: `MSIX version: 2.4.0.0` (current `version.inc`), `Using makeappx: ...`, makeappx output ending in package creation succeeded, `Packed: build/OrcaSlicer_test.msix`. Then delete the test package: `Remove-Item build/OrcaSlicer_test.msix`.

If the install tree does NOT exist, run only the parse/staging guard paths instead:
`./scripts/msix/build_msix.ps1 -InstallDir nonexistent` — expected: throws `orca-slicer.exe not found in 'nonexistent' - build the install tree first`. Full pack then gets verified by the CI run (Task 8).

- [ ] **Step 3: Commit**

```bash
git add scripts/msix/build_msix.ps1
git commit -m "ci: add MSIX packaging script"
```

---

### Task 4: CI step in build_orca.yml

**Files:**
- Modify: `.github/workflows/build_orca.yml` (insert after the "Create installer Win" step, currently lines 295-299, before "Pack app")

- [ ] **Step 1: Add the MSIX build + upload steps**

In `.github/workflows/build_orca.yml`, directly after this existing block:

```yaml
      - name: Create installer Win
        if: runner.os == 'Windows' && !vars.SELF_HOSTED
        working-directory: ${{ github.workspace }}/build
        run: |
          cpack -G NSIS
```

insert:

```yaml
      - name: Build MSIX Store package Win
        if: runner.os == 'Windows' && !vars.SELF_HOSTED
        working-directory: ${{ github.workspace }}
        shell: pwsh
        run: |
          ./scripts/msix/build_msix.ps1 `
            -InstallDir "${{ github.workspace }}/build/OrcaSlicer" `
            -OutputPath "${{ github.workspace }}/build/OrcaSlicer_Windows_MSIX_${{ env.ver }}.msix" `
            -IdentityName "${{ vars.ORCA_MSIX_IDENTITY_NAME || 'OrcaSlicer.Dev' }}" `
            -Publisher "${{ vars.ORCA_MSIX_PUBLISHER || 'CN=00000000-0000-0000-0000-000000000000' }}" `
            -PublisherDisplayName "${{ vars.ORCA_MSIX_PUBLISHER_DISPLAY_NAME || 'OrcaSlicer (dev)' }}"
```

and after the existing "Upload artifacts Win installer" step (uploads `build/OrcaSlicer*.exe`), insert:

```yaml
      - name: Upload artifacts Win MSIX
        if: runner.os == 'Windows' && !vars.SELF_HOSTED
        uses: actions/upload-artifact@v7
        with:
          name: OrcaSlicer_Windows_MSIX_${{ env.ver }}
          path: ${{ github.workspace }}/build/OrcaSlicer_Windows_MSIX_${{ env.ver }}.msix
```

Notes: gating mirrors the NSIS installer step (`!vars.SELF_HOSTED` — github-hosted runners are guaranteed to have the Windows SDK). The unset-variable fallbacks (`|| '...'`) keep fork/pre-reservation builds green with placeholder identity. Do NOT touch any existing step.

- [ ] **Step 2: Verify YAML parses**

Run (PowerShell): `python -c "import yaml; yaml.safe_load(open('.github/workflows/build_orca.yml', encoding='utf-8')); print('YAML OK')"`
Expected: `YAML OK`. (If python/pyyaml is unavailable, fall back to careful diff review — `git diff .github/workflows/build_orca.yml` — checking indentation matches sibling steps at 6 spaces; final validation is the CI run in Task 8.)

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/build_orca.yml
git commit -m "ci: build MSIX Store package in Windows job"
```

---

### Task 5: packaged-context helpers in GUI_Utils

**Files:**
- Modify: `src/slic3r/GUI/GUI_Utils.hpp` (declarations after `format_nozzle_diameter`, ~line 69)
- Modify: `src/slic3r/GUI/GUI_Utils.cpp` (include + implementations after `format_nozzle_diameter`, ~line 47)

- [ ] **Step 1: Declare the helpers**

In `src/slic3r/GUI/GUI_Utils.hpp`, after the line
`wxString format_nozzle_diameter(float diameter);`
add:

```cpp
// True when running inside an MSIX package (Microsoft Store build); always false on non-Windows.
bool is_running_in_msix();
// Opens the Microsoft Store product page for the current package. No-op when not packaged.
void open_ms_store_product_page();
```

- [ ] **Step 2: Implement them**

In `src/slic3r/GUI/GUI_Utils.cpp`, extend the existing Windows include block (lines 10-14)

```cpp
#ifdef _WIN32
    #include <Windows.h>
    #include "libslic3r/AppConfig.hpp"
    #include <wx/msw/registry.h>
#endif // _WIN32
```

to

```cpp
#ifdef _WIN32
    #include <Windows.h>
    #include <appmodel.h>
    #include "libslic3r/AppConfig.hpp"
    #include <wx/msw/registry.h>
#endif // _WIN32
```

and add `#include <wx/utils.h>` to the wx include group (after `#include <wx/display.h>`, line 26).

Then, after the `format_nozzle_diameter` function body (ends ~line 47), add:

```cpp
bool is_running_in_msix()
{
#ifdef _WIN32
    // Null-buffer probe: returns ERROR_INSUFFICIENT_BUFFER when packaged,
    // APPMODEL_ERROR_NO_PACKAGE when running unpackaged.
    static const bool packaged = []() {
        UINT32 length = 0;
        return ::GetCurrentPackageFullName(&length, nullptr) != APPMODEL_ERROR_NO_PACKAGE;
    }();
    return packaged;
#else
    return false;
#endif
}

void open_ms_store_product_page()
{
#ifdef _WIN32
    UINT32 length = 0;
    if (::GetCurrentPackageFamilyName(&length, nullptr) != ERROR_INSUFFICIENT_BUFFER)
        return;
    std::wstring family_name(length, L'\0');
    if (::GetCurrentPackageFamilyName(&length, family_name.data()) != ERROR_SUCCESS)
        return;
    family_name.resize(length > 0 ? length - 1 : 0); // drop the terminating null
    wxLaunchDefaultBrowser(wxString(L"ms-windows-store://pdp/?PFN=") + family_name.c_str());
#endif
}
```

- [ ] **Step 3: Build**

Run the Windows build command from the plan header (target `OrcaSlicer`). Expected: exit code 0.

- [ ] **Step 4: Commit**

```bash
git add src/slic3r/GUI/GUI_Utils.hpp src/slic3r/GUI/GUI_Utils.cpp
git commit -m "feat: add MSIX packaged-context detection helpers"
```

---

### Task 6: gate the updater in packaged context

**Files:**
- Modify: `src/slic3r/GUI/GUI_App.cpp` (~line 934, startup auto-check)
- Modify: `src/slic3r/GUI/MainFrame.cpp` (~line 2578, Help menu "Check for Updates")

Both files already include `GUI_Utils.hpp` (GUI_App.cpp directly; MainFrame.cpp via MainFrame.hpp) and are inside `namespace Slic3r::GUI`, so the helpers are callable unqualified.

- [ ] **Step 1: Gate the startup auto-check**

In `src/slic3r/GUI/GUI_App.cpp`, change (inside the `CallAfter` at ~line 934):

```cpp
            this->check_new_version_sf();
```

to:

```cpp
            // Store builds update through the Microsoft Store, never self-update.
            if (!is_running_in_msix())
                this->check_new_version_sf();
```

- [ ] **Step 2: Redirect the manual menu check**

In `src/slic3r/GUI/MainFrame.cpp`, change (~lines 2578-2583):

```cpp
    append_menu_item(helpMenu, wxID_ANY, _L("Check for Updates"), _L("Check for Updates"),
        [](wxCommandEvent&) {
            wxGetApp().check_new_version_sf(true, 1);
        }, "", nullptr, []() {
            return true;
        });
```

to:

```cpp
    append_menu_item(helpMenu, wxID_ANY, _L("Check for Updates"), _L("Check for Updates"),
        [](wxCommandEvent&) {
            if (is_running_in_msix())
                open_ms_store_product_page();
            else
                wxGetApp().check_new_version_sf(true, 1);
        }, "", nullptr, []() {
            return true;
        });
```

- [ ] **Step 3: Build**

Run the Windows build command from the plan header (target `OrcaSlicer`). Expected: exit code 0.

- [ ] **Step 4: Commit**

```bash
git add src/slic3r/GUI/GUI_App.cpp src/slic3r/GUI/MainFrame.cpp
git commit -m "feat: suppress self-update in MSIX Store build"
```

---

### Task 7: gate runtime file associations in packaged context

**Files:**
- Modify: `src/slic3r/GUI/GUI_App.cpp` (`associate_files` ~9112, `disassociate_files` ~9137, `associate_url` ~9188, `disassociate_url` ~9213 — line numbers shifted ~+2 by Task 6)
- Modify: `src/slic3r/GUI/Preferences.cpp` (Associate tab, ~lines 1848-1883)

- [ ] **Step 1: Early-return in the four association functions**

In `src/slic3r/GUI/GUI_App.cpp`, add an early return as the first statement inside the `#ifdef WIN32` block of each of the four functions. In `associate_files`:

```cpp
void GUI_App::associate_files(std::wstring extend)
{
#ifdef WIN32
    // MSIX: shell integration is declared in the package manifest; registry
    // writes from a packaged process are virtualized and invisible to the shell.
    if (is_running_in_msix())
        return;
    wchar_t app_path[MAX_PATH];
```

In `disassociate_files`, `associate_url`, and `disassociate_url`, add the same guard without repeating the comment — first statement inside `#ifdef WIN32`:

```cpp
    if (is_running_in_msix())
        return;
```

(`associate_url` keeps its Linux `#elif` branch untouched — the guard goes only in the WIN32 branch.)

- [ ] **Step 2: Hide the Associate tab in Preferences**

In `src/slic3r/GUI/Preferences.cpp`, wrap the body of the Associate-tab block (lines 1848-1883). Change:

```cpp
#ifdef _WIN32
    m_pref_tabs->AppendItem(_L("Associate"));
```

to:

```cpp
#ifdef _WIN32
    // MSIX: associations are declared in the package manifest and defaults are
    // managed by Windows Settings; the runtime registry toggles below cannot work.
    if (!is_running_in_msix()) {
    m_pref_tabs->AppendItem(_L("Associate"));
```

and change the end of that block:

```cpp
    g_sizer->AddSpacer(FromDIP(10));
    sizer_page->Add(g_sizer, 0, wxEXPAND);
#endif // _WIN32

    //////////////////////////
    //// DEVELOPER TAB
```

to:

```cpp
    g_sizer->AddSpacer(FromDIP(10));
    sizer_page->Add(g_sizer, 0, wxEXPAND);
    }
#endif // _WIN32

    //////////////////////////
    //// DEVELOPER TAB
```

(Keep the inner lines at their current indentation — a re-indent would bloat the diff; the `{`/`}` pair at statement level matches the file's pragmatic style. Skipping the whole block keeps `m_pref_tabs` items and `f_sizers` entries paired, which is what the tab-switching logic relies on. This also removes the only `check_url_association` consumers, so no other prompt suppression is needed.)

- [ ] **Step 3: Build**

Run the Windows build command from the plan header (target `OrcaSlicer`). Expected: exit code 0.

- [ ] **Step 4: Sanity-check classic behavior is unchanged**

Launch the freshly built classic exe: `build/src/RelWithDebInfo/orca-slicer.exe` (path per local layout). Open Preferences — the Associate tab must still be present with all checkboxes (we are not packaged, so `is_running_in_msix()` is false). Close the app.

- [ ] **Step 5: Commit**

```bash
git add src/slic3r/GUI/GUI_App.cpp src/slic3r/GUI/Preferences.cpp
git commit -m "feat: suppress runtime file associations in MSIX Store build"
```

---

### Task 8: end-to-end verification + PR

No new files. This is the verification gate the PR description documents (repo guideline: behavior changes need documented verification).

- [ ] **Step 1: Push branch and open a draft PR**

```bash
git push -u origin msix-store-build
gh pr create --draft --title "Add Microsoft Store MSIX package build" --body "<composed per below>"
```

(Compose the body first — pass it via `--body` with a here-string, not `--body-file -`, which blocks on stdin in non-interactive shells.) PR body must include: motivation (SAC blocks unsigned NSIS installer; Store re-signs), summary of the four-token identity scheme + the three repo variables to set after Partner Center name reservation (`ORCA_MSIX_IDENTITY_NAME`, `ORCA_MSIX_PUBLISHER`, `ORCA_MSIX_PUBLISHER_DISPLAY_NAME`), the spec path, and the manual-verification checklist from Steps 3-5 below with results.

- [ ] **Step 2: Verify CI**

Wait for the PR's Windows job. Expected: new "Build MSIX Store package Win" step succeeds with placeholder identity; artifact `OrcaSlicer_Windows_MSIX_PR<N>` contains one `.msix`; NSIS installer and portable zip artifacts still produced unchanged.

- [ ] **Step 3: Local loose-layout install test (Developer Mode required)**

```powershell
./scripts/msix/build_msix.ps1 -StageOnly
Add-AppxPackage -Register "$env:TEMP\orca-msix-staging\AppxManifest.xml"
```

Then verify, with results recorded in the PR:
- App launches from Start menu ("OrcaSlicer (dev)" entry).
- Preferences has NO Associate tab; Help > Check for Updates opens the Store app (placeholder PFN won't resolve to a listing — that's expected pre-reservation; the point is it does NOT run the classic downloader).
- Right-click a `.3mf` file > Open with — packaged OrcaSlicer is listed and opens the file. Visit `orcaslicer://` from a browser — packaged app activates.
- Single-instance hand-off: with the packaged app running, open a second `.3mf` the same way — it loads in the existing instance instead of spawning a new one.
- Config interop: profiles created by the classic install are visible in the packaged app and vice versa (`%APPDATA%\OrcaSlicer` is shared; requires Win11 for the fine-grained exclusion on a loose layout).
- Bambu network plugin: install/download plugin from inside the packaged app succeeds and loads.
- Uninstall the packaged app (right-click in Start) — `%APPDATA%\OrcaSlicer` still exists.

Remove the test registration when done: `Get-AppxPackage *OrcaSlicer.Dev* | Remove-AppxPackage`

- [ ] **Step 4: WACK (optional but recommended)**

On a machine with the Windows SDK: pack a real `.msix` (Task 3 script), then run the Windows App Certification Kit GUI against it. Record pass/fail + any triaged warnings in the PR. (Restricted-capability warnings about `unvirtualizedResources` are expected and justified at submission time.)

- [ ] **Step 5: Mark PR ready for review**

```bash
gh pr ready
```

---

## Post-merge (NOT part of this plan — tracked in MSIX_STORE_HANDOFF.md)

Once the Partner Center company account is approved: reserve the app name, set the three repo variables from Product identity, download the next CI `.msix` artifact, and do the first manual Store submission (listing, IARC, `runFullTrust` + `unvirtualizedResources` justifications). True SAC verification happens post-certification via a private-audience listing.
