# Snap packaging

OrcaSlicer ships a [snap](https://snapcraft.io/orcaslicer) built by **repackaging the AppImage
build output** (`build/package`) — the compiled binary, bundled private libraries and resources
are reused as-is.

The snap uses **classic confinement**: like the AppImage, it runs in the host namespace and
resolves the desktop stack (GTK / WebKitGTK / GStreamer / GLU) from the host. Classic is required
for full hardware/filesystem access — notably the **3D mouse** (3Dconnexion SpaceMouse via the host
`spacenavd` socket at `/run/spnav.sock`), which no strict-confinement interface can reach.

- `snapcraft.yaml` — the manifest (`plugin: dump` of `build/package`, classic confinement).
- `local/launcher` — the runtime wrapper (sets `LD_LIBRARY_PATH`, `LC_NUMERIC=C`, `SPNAV_SOCKET`).

## CI flow

| Trigger | Where | Snap action |
|---|---|---|
| push to `main` | `build_orca.yml` (amd64 + aarch64) | build both arches + publish to **edge** |
| PR | (none) | snap is not built on PRs (the AppImage build still runs) |
| release (manual) | `publish_release.yml` | push the build run's `.snap` artifacts to the channel below |

The snap is **Store-only** — unlike the AppImage/Flatpak it is *not* attached to GitHub releases
(a downloaded `.snap` is useless without `snap install`). Both arches go through the same
`build_orca.yml` Linux build, reusing the AppDir (`build/package`) it just produced for the snap.
`build_all.yml`'s `build_linux` job matrixes over amd64 and aarch64 on every event; aarch64 always
uses a GitHub-hosted arm runner (amd64 honors the self-hosted runner when configured).

## Channel mapping (tag suffix → Snap Store channel)

| Tag | Channel |
|---|---|
| `vX.Y.Z` (release) | `stable` |
| `-rc` / `-rcN` | `candidate` |
| `-beta` / `-alpha` | `beta` |
| nightly (push to `main`) | `edge` |

## One-time maintainer setup

1. `snapcraft login` then `snapcraft register orcaslicer` (the name must be free; if not, change
   `name:` in `snapcraft.yaml` and the asset names in the workflows).
2. **Request classic confinement** for `orcaslicer` on the [snapcraft forum](https://forum.snapcraft.io/)
   (Store Requests category). Justification: a desktop slicer needs the host `spacenavd` socket
   (`/run/spnav.sock`) for 3D mice plus arbitrary user/network filesystem paths that strict
   interfaces cannot provide. **Until this is granted, uploads to every channel are held for manual
   review**, so the automated publish below will not go live yet.
3. Export CI credentials:
   `snapcraft export-login --snaps orcaslicer --channels stable,candidate,beta,edge --acls package_push,package_release exported.txt`
4. Add the file contents as the GitHub Actions secret **`SNAPCRAFT_STORE_CREDENTIALS`**.

## Notes

- Cross-distro library behavior matches the AppImage (relies on host libs): the host must provide
  the GTK/WebKitGTK/GStreamer/GLU/OpenGL stack (the same packages the AppImage documents).
- The `classic`/`library` snapcraft linters are silenced in `snapcraft.yaml` because they assume a
  self-contained snap and would flag every host-resolved library. Runtime smoke tests are the real
  check.

## Local build / test

```shell
sudo snap install snapcraft --classic
sudo snap install lxd && sudo lxd init --auto
./build_linux.sh -dsir -l -L        # produces build/package
snapcraft                            # -> orcaslicer_<ver>_amd64.snap
sudo snap install --dangerous --classic ./orcaslicer_*.snap
snap run orcaslicer
# Smoke test: load an STL, slice, USB/serial printer, network share, and a SpaceMouse if available.
```
