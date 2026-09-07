# Internal Release Toolkit

This directory is the portable runbook and script entry point for building and
publishing the Windows internal release from `codex/orca-integration-v2`.
All paths that vary by computer are supplied through a local configuration file.

## Public packages and private test configuration

**Current direction (2026-09-07, INS-20260907-006):** public installers and portable
ZIPs contain no provider credentials, including anonymous 3dprint.beer downloads.
Test configuration is delivered separately through an access-controlled channel to
identified testers. The earlier public credential-bearing exception is superseded
for subsequent operations; do not implement an override for it. Its exact historical
record remains in the [authorization archive](../Docs/coordination/model-generation/internal-test-authorization-20260905.md).

The existing CMake/build/package path rejects embedded defaults. The uploader
requires fresh actual EXE/ZIP inspection before any network operation. Findings,
missing extraction tools and incomplete inspection block publication. Keep each
report bound to the artifact SHA-256 and preserve actual findings; a scoped
`NOT_DETECTED_WITHIN_SCOPE` result is not proof against every possible encoding.

This change does not itself replace or withdraw old public files, change server
access controls or rotate keys. Those actions need concrete object/operation
scope. It does not clear historical platform findings or unresolved review blocks.
See the [implementation record](../Docs/coordination/model-generation/credential-free-distribution-20260907.md).

Offline inspection (Python 3.11+; EXE extraction additionally needs 7-Zip):

```powershell
python -I release/verify_package_contents.py <artifact.exe-or.zip> --report <report.json>
python -m unittest discover -s release -p test_verify_package_contents.py
```

The scanner recognizes credential fields, literal assignments and selected
token/private-key patterns. It does not establish validity or revocation; encoded
or unrecognized secrets can evade detection. No full secret values are emitted.
Inspect both reports, not just a source scan or an archive integrity test.
Any alternate ZIP upload/site-promotion path must check its own exact artifact
through this gate; this script does not authorize that operation. Copies in other
worktrees retain their old behavior until reviewed integration. See the dated
[investigation](../Docs/coordination/model-generation/release-boundary-20260907/report.md).

The tooltip JavaScript bundle has a public emoji dictionary entry named `secret`
(U+3299 U+FE0F). The inspector records it as `PUBLIC_EMOJI_NAME_MAPPING` only for
the exact audited bundle SHA-256 pinned in the scanner. It still scans the entire
file; changed bytes, other values and credentials elsewhere remain blocked.
Keep the original finding reports when rechecking this corrected classification.

This GitHub repository is public. Commit only scripts, examples, and the
non-secret server directory contract. Never commit:

- a real SSH target, account, port, private key, or password;
- provider/API keys or credential-bearing defaults payloads;
- `release/config.local.ps1` or any `release/*.local.json` file;
- generated installers, manifests, build directories, or deployment logs.

Provider credentials are never included in the public package path. Configuration
bundles are private test assets, not installer components or website release assets.
Do not include them in public upload commands, shared release directories or logs.

For the default package path, installers read machine/user environment
variables at runtime: Image2 prefers `OPENAI_PRO_API` plus `OPENAI_PRO_URL`, while
legacy `OPENAI_API_KEY` plus `OPENAI_BASE_URL` remains a compatibility fallback
only when both PRO settings are absent.

For controlled internal testing, `create_ai_provisioner.ps1` can generate a
separate credential-bearing configuration ZIP from the operator's effective
Windows environment. That ZIP is ignored by Git, must not be uploaded to the
public download site, and is intentionally extractable by its recipients. It
writes only current-user environment variables and does not modify the OrcaSlicer
installer.

## Files

| File | Purpose |
| --- | --- |
| `config.example.ps1` | Machine-specific configuration template |
| `build_internal.ps1` | Locks the source identity, builds, tests, and validates the installer |
| `upload_installer.ps1` | Verifies the manifest and uploads the exact installer atomically |
| `verify_public_release.ps1` | Checks the public page, range download, checksum header, and health endpoint |
| `create_ai_provisioner.ps1` | Creates the separately distributed one-click internal PRO configuration ZIP |
| `server/` | Root-owned forced-command protocol and publisher enrollment template |

## Create the internal AI configuration ZIP

Run this only on an authorized release computer where `OPENAI_PRO_API` and
`OPENAI_PRO_URL` resolve to the verified internal provider configuration:

```powershell
& .\release\create_ai_provisioner.ps1
```

The command writes a ZIP and SHA-256 file under
`build\internal-ai-provisioner`. Send that ZIP through the approved internal
channel. The recipient extracts it and double-clicks
`Install-OrcaAIConfig.cmd`, then fully closes and restarts OrcaSlicer. The
matching removal command deletes only the two current-user PRO variables.

The provisioner never calls a generation endpoint. Its network check is limited
to DNS and TCP connectivity, and its log at
`%LOCALAPPDATA%\OrcaSlicer\logs\ai-config-install.log` contains no credential.

## Other test-provider configuration

The existing one-click configuration ZIP covers **Image2 PRO only**. It does not
configure the text/vision or Tripo services and is not an encrypted container.
Recipients can extract its contents; access control belongs to the delivery channel.

| Service | Runtime configuration names | Current separate setup |
| --- | --- | --- |
| Image2 | `OPENAI_PRO_API`, `OPENAI_PRO_URL`; optional `OPENAI_IMAGE_MODEL` | Existing private PRO provisioner covers the key and URL; optional model remains a runtime setting. |
| Text/vision | `OPENAI_API_KEY`, `OPENAI_BASE_URL`, optional `OPENAI_TEXT_MODEL` | Supply the approved values privately; the tester sets the named current-user environment variables. |
| Tripo | `TRIPO_API_KEY`, `TRIPO_API_BASE`, optional `TRIPO_MODEL` | Supply the approved values privately; the tester sets the named current-user environment variables. |

For manual setup, use Windows “Edit environment variables for your account” and
enter only the supplied names/values. Fully restart OrcaSlicer afterwards. Do not
put actual values in a command line, screenshot, support log, issue or public file.
The application already reads these variables; no new generation API or server
service is needed for this distribution change. The PRO removal command affects
only its two PRO variables, not independently configured text/vision/Tripo values.

Before sending any real configuration, identify the intended tester and an approved
channel with recipient authentication/access control (for example an existing
restricted secret share or internal file service). An unlisted anonymous URL is
not an access-controlled channel. No concrete private delivery destination has
been configured by this change; no real configuration bundle was exported or sent.

## One-time setup on each Windows computer

Prerequisites are the normal OrcaSlicer Windows build dependencies, NSIS, Git,
OpenSSH (`ssh`/`scp`), and access to the separately managed website repository
and server. Configure the dependency tree and a CMake build directory by following
the Windows build documentation linked from the repository root `README.md`.
The wrapper intentionally does not download toolchains or private configuration.

From the repository root:

```powershell
Copy-Item .\release\config.example.ps1 .\release\config.local.ps1
notepad .\release\config.local.ps1
```

Set the local SSH alias or `user@host` in `SshTarget`; SSH host, port, and key details
belong in the operator's local OpenSSH config. Set `WebsiteWorktree` to the
website checkout on that computer. The local config is ignored by Git.

For an enrolled restricted publisher, set `EmployeeId` to the assigned employee
number and keep `RestrictedPublisher = $true`. Accepted aliases are the exact
numeric ID, a zero-padded ID, or one `s`/`S` prefix; the client sends only the
canonical numeric ID. The ID selects authorization but is not a credential—the
matching private key remains required.

## Build and upload

Always start from the official integration branch, pull the newest remote commit,
and make sure the worktree is clean. Do not build while another process is
changing the same checkout.

```powershell
git switch codex/orca-integration-v2
git pull --ff-only origin codex/orca-integration-v2
git status --short

. .\release\config.local.ps1
& .\release\build_internal.ps1 @BuildRelease -ValidateOnly
$buildResult = & .\release\build_internal.ps1 @BuildRelease
$manifestPath = $buildResult.Manifest

& .\release\upload_installer.ps1 -ManifestPath $manifestPath @UploadRelease -ValidateOnly
& .\release\upload_installer.ps1 -ManifestPath $manifestPath @UploadRelease
```

The build script rejects the wrong branch, a dirty worktree, a credential-bearing build cache, and a build cache from
another checkout, and source changes made during packaging. It runs the existing
authoritative `scripts/package_internal_fast.ps1`, Python integration guardrails,
the AI integration verifier, and focused model-generation/smart-slicing tests.
It then checks the installer and portable ZIP identities and SHA-256 values,
optional 7-Zip integrity, and Authenticode status.

The credential-bearing-cache rejection is required by the current public-package
policy. Use a clean build configured with an empty `ORCA_AI_INTERNAL_DEFAULTS_FILE`;
never remove only the manifest flag or weaken the final archive inspection.

## Restricted publisher enrollment

The server administrator follows `release/server/README.md`. The employee sends
only one ED25519 `.pub` line. The key is stored only in that employee's server-side
`authorized_keys`, with OpenSSH `restrict` and a forced command; neither the key
nor the real server address belongs in this public repository.

The resulting role is intentionally `installer-upload` only. It cannot open a
shell, forward ports or agents, edit the website checkout, run the deployment
script, or change homepage metadata. Those website operations remain an
administrator step while the separate website worktree contains uncommitted work.

## Update the website metadata

The website is a separate repository. Do not copy it into this repository and do
not reset unrelated changes in its worktree. Update only these website files:

- `app/public.py`
- `app/templates/home.html`
- `tests/test_public.py`

Use the new manifest as the source of truth for filename, version/revision,
display label, byte size, SHA-256, CST build time, and full source commit. Derive
the “本次更新” list from the actual Git range between the previous published
source commit and the new manifest source commit; list four concise, user-visible
changes. Keep anonymous download enabled and retain the unsigned-build warning.

Before changing the website, make a targeted backup of those three files. Run its
tests in the local website checkout, then prepare an isolated server candidate.
On the server, create the candidate from the resolved release directory:

```bash
current=$(readlink -f /srv/3dprint-beer/current)
candidate=/tmp/3dprint-site-candidate-REVISION
test "${current#/srv/3dprint-beer/releases/}" != "$current"
install -d -m 0755 "$candidate"
cp -a "$current/." "$candidate/"
test -d "$candidate/app"
test ! -L "$candidate"
```

Never run `cp -a /srv/3dprint-beer/current "$candidate"`: `current` is a symlink,
and copying it as the candidate can make a supposedly isolated edit touch the live
release. Replace `REVISION` with the manifest revision and update only the targeted
files inside the candidate. Then run:

```bash
/home/web/3dprint-web/.venv/bin/python -m pytest -q
```

## Server layout and deployment

The non-secret production layout is:

- website source: `/home/web/3dprint-web`
- downloads: `/srv/3dprint-beer/data/downloads`
- current symlink: `/srv/3dprint-beer/current`
- immutable releases: `/srv/3dprint-beer/releases`
- deploy script: `/home/web/bin/deploy-3dprint-beer`
- service user: `web`

After candidate tests pass and the targeted website source files are updated,
deploy through the maintained script under the service user's systemd session:

```bash
web_uid=$(id -u web)
runuser -u web -- env \
  XDG_RUNTIME_DIR=/run/user/$web_uid \
  DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$web_uid/bus \
  bash /home/web/bin/deploy-3dprint-beer
```

Do not manually change the `current` symlink. If deployment verification fails,
restore only the three targeted website files from the backup, run the tests, and
redeploy through the same script.

## Public verification

From the OrcaSlicer repository root:

```powershell
. .\release\config.local.ps1
& .\release\verify_public_release.ps1 -ManifestPath $manifestPath @PublicRelease
```

The verifier requires the page to show the exact filename, source identity, CST
build time, update section, and anonymous download button. It also verifies a
16-byte HTTP range response with an `MZ` prefix, total file size, checksum header,
and `/healthz`. Finally inspect the page once at desktop width and once around
390 px to catch layout regressions.

Record the final source commit, revision, installer filename, byte size, SHA-256,
upload destination, public URL, test results, and website deployment result in the
release handoff. Report any skipped gate explicitly.
