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

Switching channel offers that channel's current artifact even when it sorts lower, so
a nightly user who chooses Stable graduates to the final release. A build installed
outside the application carries no channel, so the channel is read from its version
instead: `-dev` or an unparseable version is nightly, `-rc`, `-beta` and `-alpha` are
pre-release, the rest stable. That inferred channel may only switch to a candidate that
does not sort lower, and a version that cannot be compared takes no switch at all:
otherwise a hand-installed `2.6.0-rc1` would be offered an older stable on a switch,
and a click would replace a newer build with an older one.

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

## The flows

Every check resolves the same way. When something newer exists, the prefetch helper
downloads it, verifies it, records a verified marker beside the artifact and exits. The
application keeps running and shows a notification:

```
check → resolve_update → prefetch (download, verify, mark) → notification
                                                              ↓
                                                     user opens the dialog
                                                              ↓
                                    install helper (lock, verify, swap, record, relaunch)
```

Nothing installs without that click. There is no install at quit: quitting in any state
leaves the verified artifact in the cache, and the next start notifies again.

`update_action()` owns the choice between doing nothing, prefetching and notifying, and
takes readiness as an input on top of the skip and manual-check rules. It is pure, so the
policy is tested rather than observed. Readiness is a filesystem fact:
`download_is_verified()` requires the artifact at its expected size *and* a marker naming
the candidate's digest, because the helper renames the file before verifying it, and a
nightly reuses one asset name across builds.

The notification carries one action, which opens the same dialog as Help → Check for
Updates: release notes, channel, "Install and restart", "Skip this Version", "Cancel". A
manual check with a verified artifact opens it immediately; one that finds nothing answers
"This is the newest version."; one that runs while a download is in flight says so.

Neither the notification nor the candidate is persisted. The next check recomputes both
from the release list and the cache, which is why an artifact that finished while the
application was closed still produces a notification at the next start.

`Skip this Version` stops offering that build and deletes the cached artifact. The deletion
takes a per-download lock, because a prefetch helper started before a restart may still be
writing the `.part`; when the lock is held, the deletion is deferred to the next skipped
check.

In stealth mode the startup check does not run, because it starts a background download;
the re-check after a channel change and Help → Check for Updates still do.

A failed download leaves a message in `update_failed.txt`, which the next start shows as a
notification; the detailed trail is in `updater.log`. A failed install is visible in the
helper window instead, because every install is started by the user.

## What the two processes exchange

Everything travels through the OS cache directory, never the data directory: it holds
a download the user can lose, and the data directory is small and synced. The
application resolves the cache and puts its path in the handoff, because the helper's
environment may resolve a different one.

Downloads are kept per install, under `downloads/<install identity>/`, keyed by the tree
the application runs from; the extracted helper and the handoff that names the job are
keyed the same way. Two installs share one cache, and each one downloads the artifact for
its own platform and channel: a single directory would let one install consume a file the
other had verified, run a helper the other extracted, or overwrite the job the other had
handed to its helper.

| File | Written by | Read by | Purpose |
|---|---|---|---|
| `handoff-<install>.json` | app | helper, once | the job: asset, digest, size, install root, parent pid, lock path |
| `installed.json` | helper | app, next start | what was installed, and the tree it belongs to |
| `pending.json` | helper, before the swap | app, next start | the swap in flight; on macOS the tree moved aside, for rollback and cleanup |
| `install.lock` | app, then helper | both | an OS file lock held for the life of the holder |
| `download.lock` | prefetch helper | app, skip path | held while a prefetch writes; a skip defers its cleanup while it is held |
| `<artifact>.verified` | prefetch helper, after verification | app | the digest the artifact verified for |
| `update_failed.txt` | helper | app, next start | why a background download failed |
| `downloads/<install>/`, `helper/<install>/`, `updater.log` | both | both | artifact, extracted helper, log |

The digest of the build a `.part` belongs to is recorded beside it, so a resumed
download can never be appended to a file for a different build. A `.part` that is
already the size of the release is verified instead of fetched again, and a verified
artifact is kept until it is installed rather than downloaded once per start. The
artifact is reported ready only when a `.verified` marker beside it names the candidate's
digest: the helper renames the file before verifying it, and a nightly keeps one asset
name across builds, so the name and its size are not identity.

A download whose size or digest does not match the listing is dropped with its digest
marker, so the next check fetches it again rather than trusting its size. A file that
cannot be read is set aside instead: a read error says nothing about the bytes, so the
check is repeated on the next start rather than the file being thrown away.

The lock is an OS-level lock on an open handle, not a pid file: the OS releases it
when the holder dies, and a held lock cannot be read through another handle, so it
carries no state. It prevents two installs, and the cleanup respects it. A second lock in
the download directory is held by the prefetch while it writes, so a skip's cleanup and an
install that follows a click both wait or refuse rather than race it.

## Installing

**Windows.** The installer is `packaging/windows/orca-slicer.nsi`, built by `makensis`
over a tree staged with `cmake --install`; it is not a CPack generator, because CPack's
template forces administrator rights and Program Files. One installer covers both
scopes and chooses at start:

- a tree the machine hive records is machine-wide and is replaced in place. The record is
  resolved with the same order on both sides: the 64-bit view first, then WOW6432Node
  because older installers were 32-bit, and within a view `InstallLocation`, else the
  CPack-era `Software\SoftFever\OrcaSlicer` default value, else `UninstallString`;
- a tree the caller names with `/D=`, which the updater always passes, is used as given —
  except the per-user default path, which the installer takes as "none named" while a
  machine record exists, because a first double-click and a `/D=` naming that same path
  look identical. A double-click that names none gets the recorded machine tree when there
  is one, and otherwise `%LOCALAPPDATA%\Programs\OrcaSlicer`. A tree the record does not
  name is installed at the user's own level, which is why an update there raises no UAC
  prompt; a machine-wide install is replaced by the elevated helper, and the installer
  itself never asks for a token.

The scope is decided again whenever the directory page changes the tree, because both the
hive that receives the record and the shell context that selects the shortcuts follow it.
A folder no machine record names is a per-user install, recorded in HKCU: writing the
recorded tree's entry from there would put one tree's record on another.

Both scopes install by stage and swap. The new tree is written to `<tree>.new`, a stage
left behind by an earlier failed run is removed first so it cannot merge with the payload,
and the tree in place is left alone until a complete new one exists. On an upgrade the old
uninstaller runs in place with `_?=`, which keeps it from deleting its own file, so that
file is deleted and the remaining directory removed before the stage is renamed onto it;
if anything survives in the tree the installer stops rather than renaming into it. A first
install skips the removal and renames the stage onto an empty or absent folder.

The installer reports three codes of its own. 1 means the tree needs an administrator and
this run has no token. 2 means nothing was written: the tree is in use, the stage could not
be written, or the old uninstaller could not be run. 3 means the old tree was removed and
the new one could not be put in place. An old uninstaller that runs and fails is the one
case that reports its own code instead, because only it knows what it removed.

**Authorization.** The helper decides the token, not the installer. It starts the installer
unelevated when this user can write to the tree and no machine record names it, and through
`runas` otherwise: a machine record elevates even a tree this user can write to, because
rewriting the Apps & features entry in HKLM needs the token, and the uninstaller that the
administrator then runs is one this user could have replaced. Before that prompt it checks
two things about the tree it is handing over: the folder resolves to the path the record
names, so a link or a substituted drive cannot redirect the removal that `RMDir /r` and
`File /r` perform, and the uninstaller in it carries the pinned publisher's signature.

The old uninstaller cannot delete a mapped image, so the helper waits for any other process
running an image from that tree, and fails after a while if one stays open. The installer
probes once more before it removes anything: Windows refuses to rename a mapped image, so a
rename of the executable aside and back fails while OrcaSlicer still runs from that folder.
That closes the window between the helper's process check and the UAC consent.

Two combinations are refused instead of retargeted: a per-user copy sitting at exactly
`%LOCALAPPDATA%\Programs\OrcaSlicer` cannot be told from a first install while a machine
record exists, so the helper stops with a message naming the recorded tree; and a tree
recorded only in the other registry view — a CPack-era leftover beside a newer machine
install — is refused rather than written by the wrong scope rule. Telling the first apart
needs to know whether the caller named the tree, which `$INSTDIR` alone does not say.

The helper reports a failure in its window, and records it for the next start only when it
has no window. It adds that the previous version was removed only when the executable is no
longer in the tree after the installer stopped, which with stage and swap means the installer
reached the swap and could not finish; every other failure leaves a version that still runs.

**Uninstalling.** The uninstaller removes only the record of the tree it sits in: the
machine entry when a machine record names that tree, the per-user entry when the user hive
does, and neither for a copied or unrecorded tree. The Start-menu folder and the desktop
shortcut are deleted in the same branch, and only from the profile of that scope, so
removing a copy cannot strip a live install's shortcuts. The CPack-era value in
`Software\SoftFever\OrcaSlicer` that names the tree is removed with it, in both registry
views, and an install that replaces a tree removes the value naming that tree too. A
leftover would let the next uninstall or the next double-click read a folder that no
longer exists as a machine-wide record, and retarget to it.

**macOS.** The downloaded DMG is mounted, the application inside it is verified, and the
bundle is copied beside the target as `<name>.staged-<pid>`, or into the cache directory
when the target's parent is not writable. Where the parent is writable the two paths are
exchanged in one step (`renamex_np` with `RENAME_SWAP`), so no crash can leave the bundle's
path without an application, and `pending.json` names `staged` as the place the previous
bundle now sits. A filesystem without an atomic exchange falls back to two renames, and a
target the user cannot write is handled by one administrator step that does both; on either
of those the record is rewritten before the first move to name `<name>.backup-<pid>`, which
is where the previous bundle then lands. The record is written before anything moves, so a
start after a crash puts the previous bundle back; the next successful start removes both,
except a previous bundle the administrator step placed where this user cannot delete it,
which is left in place and named in the log. On Windows the same record names the install
tree and carries no backup, because the installer replaces the tree itself.

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
   Windows, the Apple Developer Team ID on macOS. The values are compiled in from the CI
   signing material, so the check names the certificate that actually signs the release.
   An empty value disables it with a warning, which is what a local or unsigned PR build
   wants and what a release build must not have.

Checks 3 and 4 together are what let a single click install the file without the user
judging what it is.

A release build carries the pin of its platform and cannot be configured without it:
`ORCA_RELEASE_BUILD=ON` makes the configure fail when Windows has no publisher or macOS
has no team id, and every release and nightly build sets it. Each pin is read from the
environment, which overrides the cached value whenever it is set — including set empty, which
clears it — so a tree reconfigured after a publisher, team id or timestamp was rotated or
cleared cannot keep the old one. A developer tree that sets none of them keeps whatever its
cache holds, which is what makes a local rebuild cheap. The macOS packaging step compares
what the helper was built with — a stamp the configure wrote — against the values it
packages under, and refuses to ship a helper that would accept anything else.

## Limits

- Windows portable and AppImage builds download through the browser, so the digest is
  not checked for them; they also cannot replace themselves.
- Existing machine-wide Windows installs keep their current elevation: replacing them
  needs an administrator token on every update until a maintenance service is
  installed at install time.
- A per-user copy that sits at exactly `%LOCALAPPDATA%\Programs\OrcaSlicer` beside a
  machine-wide install cannot be told apart from a first install without a `/D=` switch,
  so the helper refuses rather than writing the wrong scope.
- On macOS the file is installed by path after it is hashed; only Windows keeps the
  verified file open through the install.
- A build that starts and then crashes is not rolled back: the backup is removed by
  the next start, which that build may never reach.
- The Windows check for another running instance cannot see processes it has no
  permission to open.
- Nothing installs while the application closes. Every update ends in a restart the user
  started, so a nightly notifies at every launch until it is installed or skipped.
- A launch without a network cannot install a cached artifact: the asset URL and digest
  arrive with the check.
- Skip deletes the cached artifact only when no prefetch holds the download lock, so a
  skipped build's file can survive one more check.

## Where the code lives

| Concern | Files |
|---|---|
| channel, candidate, policy | `src/libslic3r/UpdateChannel.{hpp,cpp}` |
| cache, records, lock, download preparation, URL allowlist | `src/libslic3r/UpdateHandoff.{hpp,cpp}` |
| flavour detection, handoff, launching the helper | `src/slic3r/GUI/GUI_Utils.cpp`, `GUI_App.cpp` |
| helper window, modes, verification | `src/slic3r/UpdateHelper/` |
| Windows installer | `packaging/windows/orca-slicer.nsi` |
