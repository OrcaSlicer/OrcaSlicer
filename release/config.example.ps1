# Optional local internal-build settings; copy to config.local.ps1.
# Keep local paths/private settings out of Git. No website or SSH configuration.
$BuildRelease = @{
    BuildDir = 'build-validation'
    OutputDir = 'build\windows-installer'
    SourceManifest = '' # Complete handoff manifest; required for dirty source.
    CMakeExecutable = '' # Optional; otherwise resolved from CMakeCache.
    NsisDir = '' # Optional; otherwise resolved by the packager.
}
