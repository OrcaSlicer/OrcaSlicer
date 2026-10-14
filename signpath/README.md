# SignPath configurations

This directory contains SignPath artifact configurations used by GitHub Actions.

## `windows-portable-v1`

`windows-portable-v1.xml` is the initial conservative Windows portable-bundle signing configuration. It signs only the two first-party binaries:

- `orca-slicer.exe`
- `OrcaSlicer.dll`

Do not broaden this to all DLLs without first confirming ownership, provenance, and whether upstream vendor signatures should be verified instead.

The Windows workflow uploads `${{ env.BUILD_DIR }}/OrcaSlicer` with `actions/upload-artifact`. GitHub stores that artifact as a ZIP, and the uploaded directory contents are rooted at the ZIP root. Because of that, the SignPath configuration uses `<zip-file>` with `orca-slicer.exe` and `OrcaSlicer.dll` directly beneath it.

The release portable ZIP is a separate archive created with 7-Zip from `${{ env.BUILD_DIR }}/OrcaSlicer`; that archive keeps the top-level `OrcaSlicer/` folder. After SignPath returns the signed artifact, the workflow copies the signed files back into `${{ env.BUILD_DIR }}/OrcaSlicer` and recreates the portable release ZIP so the public ZIP layout stays unchanged. It also copies them over the build output in `${{ env.BUILD_DIR }}/src/Release`, because `cpack` packages the installer from there rather than from the portable bundle.

## `windows-installer-v1`

`windows-installer-v1.xml` signs the NSIS installer that `cpack` produces, `OrcaSlicer_Windows_Installer_<version>_<arch>.exe`. The workflow uploads that single file, so it sits at the root of the artifact ZIP. The installer is built after the binaries are signed, so the files it installs carry the `windows-portable-v1` signatures.

Both configurations must also be created in the SignPath project under the same slugs; the workflow refers to them only by slug.
