# In-app updater — High Level Design

## Why it exists

OrcaSlicer is installed by many routes: an NSIS installer, a portable zip, a DMG, an
AppImage, Flatpak, the Microsoft Store, or a distribution package. Before this
subsystem the application could only tell the user that a newer release existed and
open a browser; what to download, and whether the application could replace itself,
was left to them.

The updater answers three separate questions:

- **What is newer?** Resolved from two inputs: the running build's identity and the
  channel the user picked.
- **What kind of install is this?** A flavour derived from the filesystem, which
  decides whether an artifact can be downloaded or installed at all.
- **How does the swap happen?** Either the application replaces itself with a small
  helper process, or it hands the user the right file and gets out of the way.

The work is split so that the first question is pure and testable, and the third is
the only part that needs a platform-specific implementation.

## Channels and identity

A channel says which releases may be offered:

| Channel | Release it resolves to | Skip is keyed by |
|---|---|---|
| Stable | the newest tag that is not a prerelease | version |
| Pre-Release | the newest tag of any kind, alpha, beta, rc or final | version |
| Nightly | the rolling release tagged `nightly-builds` | artifact digest |

`update_channel_from_key()` maps the stored string; anything unrecognised resolves to
Stable, so a damaged configuration can only ever be more conservative.

The identity of a build is the **digest of the artifact it came from**, not its
version string, because a nightly keeps its version while its contents change. The
digest is recorded by the install itself and read on the next start; until then a
nightly is compared with the release's `updated_at` plus a six-hour margin, which
covers assets that are uploaded up to an hour after a build starts.

A release is offered even when it has no digest or no asset for this platform: the
caller then opens the Store, Flathub or the release page instead of installing. A
release that cannot be installed is still news, and hiding it would leave the user
on a stale build forever.

`resolve_update()` performs the whole decision and returns a candidate plus a reason
string for the log. It is pure — no network, no wx, no globals — so every rule above
is covered by `tests/libslic3r/update_channel_tests.cpp` without a fixture.

## Install flavours

The flavour is decided once per run from filesystem and environment facts:

| Flavour | Recognised by | Update path |
|---|---|---|
| `windows_installer` | `uninstall.exe` beside the executable | in-app |
| `windows_portable` | neither that file nor a build tree | direct download |
| `macos_bundle` | running from `.app/Contents/MacOS`, writable volume | in-app |
| `linux_appimage` | `$APPIMAGE` and `$APPDIR`, executable under `$APPDIR` | direct download |
| `flatpak` | `/.flatpak-info` | Flathub page |
| `msix` | Windows package identity | Microsoft Store |
| `other` | distro package, source build, build tree, read-only bundle | release page |

`in_app_install_supported()` and `is_download_only()` name the two special cases, so
the GUI decides a button label once and the helper is only ever started for a flavour
that can use it. An executable inside a CMake build tree resolves to `other` on both
Windows and macOS: a development build must never replace the tree it runs from.

## The two flows

A check the user starts from Help → Check for Updates keeps its dialog, shows the
channel, and installs on request. The automatic check — startup, or the re-check after
Preferences closes — never opens a dialog:

```
startup check → resolve_update → prefetch helper (download + sha256)
                                    ↓
                            application keeps running
                                    ↓
                              user quits
                                    ↓
                    install helper (verify, lock, swap, record)
```

The prefetch helper exits once the file is verified. The install helper runs when the
application is closing, takes the install lock only after the parent process is gone,
verifies the file again, swaps it in, records the digest and — for a quit-time install
— does not start the application again.

`update_action()` owns the choice between these flows, and `update_ready_to_install()`
owns the question of whether a quit may install: a dirty project or a running job
defers it to a later quit. Both are pure functions, so the policy is tested rather
than observed.

An automatic update that fails leaves a message in `update_failed.txt`, which the next
start shows as a notification. Without a dialog this is the only way a failure reaches
the user; the detailed trail is in `updater.log`.

## What the two processes exchange

Everything travels through the OS cache directory, never the data directory: it holds
a download the user can lose, and the data directory is small and synced. The
application resolves the cache and puts its path in the handoff, because the helper's
environment may resolve a different one.

| File | Written by | Read by | Purpose |
|---|---|---|---|
| `handoff.json` | app | helper, once | the job: asset, digest, size, install root, parent pid, lock path |
| `installed.json` | helper | app, next start | what was installed, and the tree it belongs to |
| `pending.json` | helper, before the swap | app, next start | the tree moved aside, for rollback and cleanup |
| `install.lock` | app, then helper | both | an OS file lock held for the life of the holder |
| `update_failed.txt` | helper | app, next start | why an automatic update failed |
| `downloads/`, `helper/`, `updater.log` | both | both | artifact, extracted helper, log |

The digest of the build a `.part` belongs to is recorded beside it, so a resumed
download can never be appended to a file for a different build. A `.part` that is
already the size of the release is verified instead of fetched again, and a verified
artifact is kept until it is installed rather than downloaded once per start.

The lock is an OS-level lock on an open handle, not a pid file: the OS releases it
when the holder dies, and a held lock cannot be read through another handle, so it
carries no state. It prevents two installs, and the cleanup respects it.

## Installing

**Windows.** The installer is `packaging/windows/orca-slicer.nsi`, built by `makensis`
over a tree staged with `cmake --install`; it is not a CPack generator, because CPack's
template forces administrator rights and Program Files. One installer covers both
scopes and chooses at start:

- a machine-wide install, recognised by the uninstall key in HKLM (both registry
  views, since older installers were 32-bit), is replaced under `%PROGRAMFILES64%`
  by the elevated helper — the installer itself never asks for a token;
- anything else installs under `%LOCALAPPDATA%\Programs\OrcaSlicer` at the user's own
  level, which is why an update there raises no UAC prompt.

The helper decides whether to elevate by probing the install root for writability. It
refuses to start the installer while another process runs an image from that tree,
because NSIS uninstalls before it installs and would otherwise fail halfway.

**macOS.** The downloaded DMG is mounted, the application inside it is verified, copied
beside the target as `<name>.staged-<pid>`, and swapped with the old bundle, which is
renamed to `<name>.backup-<pid>`. `pending.json` is written before the first rename, so
a start after a crash restores the backup; the next successful start deletes it. A
folder the user cannot write is handled by a single administrator step that copies the
staged bundle in and moves both.

## Trust

Four checks stand between a downloaded file and the install, and none of them depends
on the user reading a dialog:

1. **Origin.** Only `https://github.com/OrcaSlicer/OrcaSlicer/releases/download/`
   URLs are fetched, and only in the exact form `<tag>/<asset name>` — no `..`, no
   query, no other segment. The downloader keeps curl from normalising the path and
   accepts redirects only to https.
2. **Digest.** The artifact's `sha256` from the release listing is checked against the
   downloaded bytes. This proves the transfer, not the listing.
3. **Signature.** The installer or bundle must verify with the platform's trust
   service.
4. **Publisher.** The signer must be *ours*: the Authenticode signer's display name on
   Windows, the Apple Developer Team ID on macOS. The expected values are compiled in
   from the CI signing material, so the check names the certificate that actually
   signs the release. An empty value disables the check with a warning, which is what
   a local or unsigned PR build wants.

Checks 3 and 4 together are what replaces the user's judgement once nothing prompts.

## Limits

- Windows portable and AppImage builds download through the browser, so the digest is
  not checked for them; they also cannot replace themselves.
- Existing machine-wide Windows installs keep their current elevation: replacing them
  needs an administrator token on every update until a maintenance service is
  installed at install time.
- On macOS the file is installed by path after it is hashed; only Windows keeps the
  verified file open through the install.
- A build that starts and then crashes is not rolled back: the backup is removed by
  the next start, which that build may never reach.
- The Windows check for another running instance cannot see processes it has no
  permission to open.

## Where the code lives

| Concern | Files |
|---|---|
| channel, candidate, policy | `src/libslic3r/UpdateChannel.{hpp,cpp}` |
| cache, records, lock, download preparation, URL allowlist | `src/libslic3r/UpdateHandoff.{hpp,cpp}` |
| flavour detection, handoff, launching the helper | `src/slic3r/GUI/GUI_Utils.cpp`, `GUI_App.cpp` |
| helper window, modes, verification | `src/slic3r/UpdateHelper/` |
| Windows installer | `packaging/windows/orca-slicer.nsi` |
