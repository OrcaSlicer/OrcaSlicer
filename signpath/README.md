# SignPath configurations

This directory contains SignPath artifact configurations used by GitHub Actions.

## `windows-portable-v1`

`windows-portable-v1.xml` is the initial conservative Windows portable-bundle signing configuration. It signs only the two first-party binaries:

- `orca-slicer.exe`
- `OrcaSlicer.dll`

Do not broaden this to all DLLs without first confirming ownership, provenance, and whether upstream vendor signatures should be verified instead.

The Windows workflow uploads `${{ github.workspace }}/build/OrcaSlicer` with `actions/upload-artifact`. GitHub stores that artifact as a ZIP, and the uploaded directory contents are rooted at the ZIP root. Because of that, the SignPath configuration uses `<zip-file>` with `orca-slicer.exe` and `OrcaSlicer.dll` directly beneath it.

The release portable ZIP is a separate archive created with 7-Zip from `${{ github.workspace }}/build/OrcaSlicer`; that archive keeps the top-level `OrcaSlicer/` folder. After SignPath returns the signed artifact, the workflow copies the signed files back into `build/OrcaSlicer` and recreates the portable release ZIP so the public ZIP layout stays unchanged.
