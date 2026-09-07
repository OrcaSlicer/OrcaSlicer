[CmdletBinding()]
param(
    [string] $SourceArchive = "output\packages\OrcaSlicer-AI-Windows-x64-20260813-demo3-simple-config.zip",
    [string] $DistributionDirectory = "output\packages\OrcaAI-demo3-send-to-colleague"
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$source = [IO.Path]::GetFullPath((Join-Path $projectRoot $SourceArchive))
$destination = [IO.Path]::GetFullPath((Join-Path $projectRoot $DistributionDirectory))

if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Source archive was not found: $source" }
if (Test-Path -LiteralPath $destination) { throw "Distribution directory already exists: $destination" }

New-Item -ItemType Directory -Path $destination | Out-Null
Copy-Item -LiteralPath $source -Destination (Join-Path $destination "OrcaAI-demo3.zip")
Copy-Item -LiteralPath (Join-Path $projectRoot "packaging\windows-ai-test\extract-package.bat") -Destination (Join-Path $destination "01-extract-package.bat")

$batch = Join-Path $destination "01-extract-package.bat"
$content = [IO.File]::ReadAllText($batch) -replace "`r?`n", "`r`n"
[IO.File]::WriteAllText($batch, $content, [Text.Encoding]::ASCII)

$archive = Join-Path $destination "OrcaAI-demo3.zip"
$hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
"$hash  OrcaAI-demo3.zip" | Set-Content -LiteralPath (Join-Path $destination "OrcaAI-demo3.zip.sha256.txt") -Encoding ASCII

@"
Send all four files in this folder to the tester, or send the outer delivery ZIP.

The tester should double-click 01-extract-package.bat.
It extracts the package to %%USERPROFILE%%\OrcaAI-demo3 using Windows tar.exe.
Do not use Windows Explorer 'Extract All' because the package contains 15,000+ small resource files and can appear stuck.

After extraction:
1. Edit setup\ai-config.bat and fill the two API keys.
2. Run 02-check-environment.bat.
3. Run 03-start-orcaslicer-ai.bat.
"@ | Set-Content -LiteralPath (Join-Path $destination "README-extract.txt") -Encoding ASCII

Get-ChildItem -LiteralPath $destination | Select-Object Name, Length
