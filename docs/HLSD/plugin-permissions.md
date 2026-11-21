# Plugin permissions and audit - High Level Design

## Purpose and scope

Python plugins run inside the OrcaSlicer process. OrcaSlicer restricts what a plugin
can do by installing a CPython audit hook and judging the events it recognizes against
per-plugin grants. This document describes that policy: how an event is attributed to a
plugin, when it is allowed without a prompt, how the user is asked, where the answer is
stored, and what full access changes.

The policy is a Python audit-hook policy, not an operating-system sandbox. A plugin still
runs in the host process with the user's filesystem and network rights. The limits that
follow from that are listed under "Security limitations".

The Plugins dialog exposes the stored grants and the full-access switch, but the
enforcement lives in `src/slic3r/plugin` (built into `libslic3r_gui`) and applies whenever
a capability call runs, whether or not the dialog is open. The Script execution path that opens the audit scope is described
in [python-plugin-async-bridge.md](python-plugin-async-bridge.md).

## Permission model

Permissions are stored per plugin key. A plugin can hold:

| Field | Kind | Governs |
| --- | --- | --- |
| `fs_read` | target list | Read-shaped filesystem events. Targets are exact paths or `<dir>/*` trees. |
| `fs_readwrite` | target list | Write-shaped and read/write filesystem events mapped to `FsReadWrite`: a write-mode `open`, rename, chmod, utime, copy, move, archive extraction, and similar. Same target matching as `fs_read`. A write-mode `open` can create a file, but this grant does not cover events mapped to `FsCreate` or `FsDelete`. |
| `fs_create` | folder list | Filesystem create events such as `os.mkdir` and `os.symlink`. |
| `fs_delete` | folder list | Filesystem delete events such as `os.remove` and `shutil.rmtree`. |
| `network_http` | target list | Network targets, consulted together with `network_socket` for both HTTP and socket events. A canonical `scheme://host` entry is host-wide and covers both layers; a URL with a path, port, query or trailing slash is exact; a bare host or legacy address tuple covers raw sockets. |
| `network_socket` | target list | The same network matching, kept as a separate list so a plugin can declare socket access without an HTTP URL. |
| `process` | target list | Process creation and process replacement: `subprocess.Popen`, `os.spawn*`, `os.exec*`, `pty.spawn`, `_winapi.CreateProcess`, and similar. Targets name the tool, not its arguments. The exception is a Windows command line with an unquoted executable followed by arguments: it is stored as an exact full command line, because `CreateProcess` may resolve a longer executable prefix than the first token shows. `os.system` names the shell it runs (see "Target matching"). |
| `threading` | boolean | Permission to start a Python thread with `_thread.start_new_thread`. |
| `events` | target list | Audited events that expose no target, keyed as `<Category>:<event>`, for example `Socket:socket.__new__`. Matched verbatim. |
| `full_access` | boolean | Turns off the permission decision for the plugin. See "Full access". |

Create and delete grants are independent of read and read/write grants. An empty list grants
no folders; an absolute folder path grants operations directly inside that folder, while a path
ending in `/*` also covers its subfolders; a list containing only `*` grants all folders. Runtime approval stores
the parent folder of the audited target, except for a recursive delete, whose consent is
transient and is never stored. A delete grant covers existing entries in that folder,
not just entries created by the plugin. The unconditional denied paths still apply.

A recursive delete (`shutil.rmtree`) is a subtree operation: a plain folder grant is
insufficient even when it names the target folder or its parent. It requires either a saved
delete tree grant whose root covers the target, or explicit one-time consent for that canonical
subtree. A tree grant rooted exactly at the target covers the entries inside it but not the
final removal of the target folder itself, which is an entry in the parent; the operation asks
for consent unless the parent is granted separately. The consent applies only to the operation
that requested it and the nested deletes it performs inside that subtree.

### Target matching

- Read and read/write exact filesystem entries match the audited path verbatim. An absolute exact entry also
  matches an equivalent canonical path, including a symlink alias or a relative target resolved
  against the current working directory. Relative exact entries match only verbatim, so their
  equivalence scope cannot move with the working directory. Canonical equivalence fails closed
  for dangling final symlinks; verbatim matches retain their existing behavior.
  A tree grant `<dir>/*` covers the named folder and every path below it. `*` is the only
  wildcard and is recognized only as the final path component. A read grant never
  authorizes a write, and the reverse. Tree matching canonicalizes both sides, so a
  symlink below the granted folder cannot extend the grant outside it. A tree grant's
  root must be absolute on the current platform: a relative root never matches, even when a
  target is spelled exactly like the entry, so the scope cannot float with the process
  working directory.
- Create and delete folder grants compare canonical parent directories; a final symlink is
  governed by the folder containing the link. Link creation checks the destination folder;
  a hardlink also requires write access to its source because writes through either name
  affect the same file. Both source and destination remain
  subject to the denied-path check. An unresolved parent
  does not match a folder grant. Recursive deletion is checked as a subtree operation: an
  exact folder grant does not cover it, but a delete tree grant does, and a one-time consent
  covers the target subtree for that operation only. A target-rooted tree grant covers the
  entries inside the target but not removing the target folder itself; that final removal needs
  a grant for the containing folder or one-time consent. Nested delete entries inside an
  approved subtree are allowed while the operation runs, provided they stay inside the canonical
  subtree and are reached through standard-library frames. Scope is decided by the entry's
  containing folder: deleting a symlink acts on the link entry, never on its destination.
  Intermediate `..` components are resolved after symlinks, never lexically folded before
  them, and a final `..` fails closed. A
  symlink or `..` that would leave the subtree is judged on its own, and denied paths stay
  blocked.
- Network entries can be an HTTP URL, a canonical `scheme://host`, a bare host, a
  `host:port` pair, or a legacy persisted address tuple such as `('host', 443)`.
  `normalize_network_url` reduces a URL to lowercase `scheme://host`; path, query,
  fragment, userinfo and port are dropped. A bare host, a `host:port` pair, or a legacy
  tuple is a raw-socket grant; it does not match an HTTP URL. A raw-socket entry has its
  port removed when it is matched, so a host-based network grant does not restrict ports.
  A canonical entry is host-wide and applies to HTTP and raw socket events for that host.
  A URL with a path, port, query, fragment or trailing slash that was stored without
  normalization remains an exact match.
- Process entries name the tool an event runs, compared verbatim. When the event exposes
  the executable or path, that argument is the target. For a command string or argv, the
  first token or element is the target. Arguments after the tool are ignored, so one grant
  covers repeated runs of the tool with different arguments. The exception is a Windows
  command line whose executable is unquoted and followed by non-whitespace text.
  `CreateProcess` resolves such a name by trying successively longer prefixes, so the first
  whitespace-delimited token need not be the executable it starts: a command line
  `C:\Program Files\app.exe --x` can start `C:\Program Files\app.exe`, not `C:\Program`.
  The audit therefore stores the exact full command line instead of the first token, in a
  reserved `CommandLine:` namespace in the `process` list. An exact entry matches only the
  same full command line, compared verbatim, and a later run with different arguments asks
  again. A quoted first argument, an explicit executable or application name, and a command
  line that has no arguments keep the executable-only, argument-insensitive grant. The
  command line is not parsed further, and the resolved executable is not checked: the same
  approved command line can start a different binary if the working directory, `PATH`, or
  the files it resolves through change. `os.system` works differently:
  Python hands its whole string to the platform shell, so the target is every shell that may
  run it. On POSIX that is `/bin/sh`. On Windows the C runtime runs the interpreter named by
  `COMSPEC`, or `cmd.exe` when `COMSPEC` is unset or empty, and also falls back to `cmd.exe`
  when it cannot open the configured file (ENOENT or EACCES). The configured interpreter and
  the fallback are both required targets: a grant for a narrow `COMSPEC` does not authorize
  the fallback shell. A `COMSPEC` that is already the bare name `cmd.exe`, in any casing,
  names the fallback itself and is listed once. The value is read from the CRT environment
  that `_wsystem` consults, not the process environment block, which can differ. The audit
  does not test whether the `COMSPEC` file exists; the runtime decides that when it runs the
  command, so a file present at prompt time can be gone by the call. The command string is
  not parsed, and approving the shells covers every command and argument the plugin passes
  to `os.system`. The targets are read when the event is raised, so a grant saved under one
  `COMSPEC` value does not cover a run under another. Direct process creation, such as
  `subprocess.Popen`, still names its executable.
- An event with no target is granted by the `events` entry for its category and name. A
  targetless network event, such as `http.client.send`, is covered by any recorded
  network grant instead.

## Runtime enforcement

### Audit hook and attribution

`PluginAuditManager::install_hook` installs a global CPython audit hook with
`PySys_AddAuditHook` while the embedded interpreter initializes, before any plugin code
runs. The hook recognizes a fixed set of audit events and groups them into filesystem
read, filesystem read/write, filesystem create, filesystem delete, HTTP, socket, process
create, process replace, and threading categories. Events outside that map are ignored.
`open` is categorized from its mode: write, append, update, or exclusive modes are
write-shaped, and a read-only mode is read-shaped.

Attribution is thread-local. `ScopedPluginAuditContext` stores the previous plugin key,
capability name, scoped filesystem roots and operation-root frames, then installs the
current call's values. The trampoline machinery opens this scope for every C++ to Python
capability call; the Script worker opens it for the whole invocation, including an async
suspension, and the `_thread.start_new_thread` shim re-establishes it inside a thread the
plugin creates. Events raised while no plugin scope is active are allowed and not
attributed. Module import and `register_capabilities` run without such a scope.

### Decision order

For a recognized event raised inside a plugin scope, `audit_plugin_event` decides in this
order:

1. **Unconditional denied path.** For a filesystem event, a target whose base name is a
   denied filename, or whose path contains a denied keyword, is denied. This check runs
   before allowed roots, before a granted operation and before full access, so no grant
   can make a denied path reachable.
2. **Covered by a granted operation.** When a granted root operation is still running,
   nested events raised through its standard-library frames are generally allowed without
   another prompt. Delete operations still check nested delete events against their
   own grants, or against the canonical subtree recorded by a one-time recursive-delete
   consent started by this operation. The coverage is bounded to the operation frame and does
   not extend to plugin or third-party frames it calls back into. A nested `urllib.Request`
   is judged on its own URL, including when it follows a redirect.
3. **Implicit allowed root or standard-library read.** A filesystem target inside an
   allowed root is allowed; a read of the interpreter's standard library is also allowed.
   See "Implicit access".
4. **Full access.** If the plugin's readable sidecar has `full_access` set, the event is
   allowed.
5. **Saved grant.** A matching entry in the appropriate list allows the event.
6. **Prompt.** Otherwise the user is asked. A granted prompt allows the event and, for a
   category with a persistent list, saves the target. A denied or unanswered prompt raises
   `PermissionError` in the plugin and records the violation.

After a granted root event with a target, the hook records the operation frame so the
nested events of the same operation are not asked again.

### Python scripts

A Python script run from the Run Script panel, with `--run` or by an agent's `script_run`
is not a plugin, but its events are judged here under an identity of its own, `(script)` or `(mcp)`
(see "Python scripts" in [mcp-server.md](mcp-server.md)). A script has no sidecar, so it holds
no grant and is never asked: steps 1 to 3 apply to it, and any other event is denied with a
`PermissionError` that names the setting below. A file run also reads the folder of its file,
through a per-call scoped root.

The "Run Python scripts unrestricted" setting (`python_scripts_unrestricted`: Preferences, AI features,
Python scripts; off by default) lifts this for every script, and so does the command line's
`--unrestricted-scripts` for one run, without writing the setting: while either is on, a script's event
is allowed right after step 1, so only the denied paths stay blocked. No plugin's key is a script identity: a
local plugin's key keeps only the letters, digits, `_`, `-` and `.` of its file name, and a cloud plugin's is
its UUID.

## Permission prompts

### Declared permissions during load

A plugin declares its needs from Python with `orca.request_permissions(...)` while
`register_capabilities` runs. The bridge collects the declarations per plugin. After
registration returns and before any capability instance is created, OrcaSlicer presents
one combined prompt for the targets not already stored. The request joins the same
`AuditPromptQueue` as runtime prompts: a load worker waits for the answer, while a
GUI-thread caller is rejected when another prompt is active instead of blocking the GUI.
Denying the request aborts the load. A readable sidecar with `full_access` accepts the
declaration without a prompt and without adding a grant.

Network URLs passed to `request_permissions(network=...)` are normalized to
`scheme://host` before prompting or storing, so one approval covers every path and port
on that host and also authorizes raw sockets to it. A relative filesystem tree grant is
resolved against the working directory when the request is made, so the prompt and the
sidecar show the absolute scope it covers.

### Runtime prompts

An event that is not covered by a saved grant or an implicit rule is either put to the
user or denied. If the caller is on the wx main thread and no audit prompt is active, the
modal runs inline. Otherwise, as on the Script workflow, the plugin-load worker, or the
slicing worker, the current event is denied and the prompt is queued for the GUI thread.
`AuditPromptQueue` serializes runtime prompts, one-time recursive-delete consent, and
load-time declarations, so at most one is shown at a time,
including during a modal dialog's nested event loop. An event raised inside that loop is
queued and denied instead of opening a second dialog; the queued prompt opens after the
active dialog has closed.

For a category with a persistent grant list, a Yes saves the grant and a later matching
attempt can succeed without another prompt. Create and delete approvals save the target's
parent folder, so a worker can retry after the queued Yes. Recursive delete instead uses
one-time consent, as described below. Blocking a worker on a marshalled modal is avoided
because the UI thread can itself be parked on the slicing worker. A load worker does block on its declared-permission answer:
shutdown or plugin unload cancels queued requests and active requests whose accepted answer
has not begun committing, releasing waiting workers with a denial. An admission that races
that cancellation is discarded as soon as it becomes visible. A cancelled load stops being
treated as a live plugin even while its worker is still unwinding. A cancellation observed before
display suppresses the prompt; a dialog racing with cancellation may still appear or remain on
screen, but its answer is ignored unless committing won the race. An ordinary load failure is not
a cancellation: its queued runtime prompt stays actionable, so the user can approve the event
whose denial aborted the load and then retry with the saved grant.

Runtime approvals store the target observed by the audit hook, except that create and delete
approvals store its parent folder. A URL with a path, port or
query is therefore an exact grant; a bare host remains usable for raw sockets. A Windows
command line that needs the exact fallback is saved in the reserved `CommandLine:` namespace
as its full text, so the approval is argument-sensitive; the prompt shows the command line
without the storage marker and states that the full line, including any sensitive values in
its arguments, is shown and saved. The declared form and the runtime form are not normalized
by the same code path.

A recursive delete is different: a Yes grants consent for the canonical target subtree for
that operation only. The prompt shows the subtree root and states that the consent is not
saved; a filesystem, drive, or network share root gets an explicit warning that its whole
filesystem, drive, or network share is covered. Nested deletes inside that subtree proceed
during the same operation; after it ends, the same target asks again. Denied paths stay blocked
throughout. When the prompt cannot run inline, the current attempt is denied and a Yes from the
posted dialog is kept as a short-lived, single-use approval bound to the plugin key and
canonical target. The posted prompt says it applies to the next matching attempt within one
minute after approval; the next matching attempt consumes it. A different plugin, a different
target, a reused or an expired approval does not. A prompt already open for the same plugin and
target is not posted again, so a retry loop cannot queue duplicate dialogs. The pending-prompt
set is bounded; at capacity, new prompts are denied rather than evicting a dialog that is
already queued. A permission edit, revocation, or reinstall invalidates the plugin's transient
approvals and pending prompt keys; a dialog callback already queued becomes a no-op instead of
recreating a stale approval.

## Implicit access

Some access needs no grant because it is part of the plugin system's normal operation:

- `data_dir()` is a read-write root. It contains the plugin directories, each plugin's
  storage, the installed/system profile cache, and the plugin package files themselves.
- `resources_dir()` is a read-only root. Plugins can inspect bundled assets but cannot
  write to the shared installation.
- Standard-library files are readable. Site-packages are excluded from this exemption.
- A per-call scoped root can be added. The G-code post-process hook grants the folder
  containing the exported G-code file so the plugin can edit it; the root is scoped to
  that call.
- A delete inside a writable allowed root is scoped by the entry's containing folder. Deleting
  a symlink that lives in the root is therefore implicit even when the link points outside,
  because the operation removes the link entry; an outside link that points into the root is
  not implicit. A recursive delete inside a writable root records its target subtree as the
  running operation, so its fd-based reads do not trigger a separate prompt and its nested
  deletes cannot leave that subtree.

### Denied paths

The following are denied before any allowed root or full-access check:

- Base names starting with the application configuration names, the cloud user-secret
  file, or `.install_state.json`. The comparison is case-insensitive and covers `.bak`
  and `.tmp` companions through the prefix match.
- Any path component matching `secret`, `secrets`, `cert`, `certs`, `certificate`,
  `certificates`, `conf`, or `config`. The `conf` keyword also blocks `.conf` and `.ini`
  file names. Matching is on whole components, not substrings, so imports such as
  `numpy/__config__.py` remain usable. Inside `data_dir()`, only components below
  `data_dir()` are checked, because the application data directory can itself sit under
  a path component such as `config`.

## Persistence and editing

Grants and install state live in `.install_state.json` in the plugin root. The
`permissions` object stores `fs_read`, `fs_readwrite`, `fs_create`, `fs_delete`, `network_http`,
`network_socket`, `process`, `threading`, `events`, and `full_access`. It never stores a
recursive-delete consent: that approval and any retry approval are in memory only, so accepting
one does not widen the plugin's saved delete scope. The file is
written by `write_install_state`, the single writer for the sidecar schema. In-process
reads and read-modify-write operations are synchronized. Writes prepare a complete
replacement in the plugin directory and replace the sidecar only after the write succeeds.
The lock does not coordinate separate OrcaSlicer processes sharing a data directory.

Reading is deliberately fail-closed:

- A missing, unreadable, or malformed sidecar means no grants and no full access.
- `threading` is a boolean. Older sidecars stored a string or array; a nonempty value
  reads as granted, an empty one as denied, and the next normal write stores the boolean
  form. Reading never rewrites the file.
- `full_access` is enabled only by an explicit JSON boolean. A missing key, a string, a
  number, or any other type reads as false.

A sidecar that exists but cannot be read is never overwritten, because it may still hold
grants and capability choices. The one exception is a plugin load: if the sidecar is a
regular file that reads in full but does not parse, as a crash or a hand edit can leave
it, the load moves it to `.install_state.json.corrupt` before any plugin code runs. The
plugin then counts as having no sidecar, so its declared permissions are asked for again
and a fresh sidecar is written, while the old bytes stay beside it for manual recovery.
Anything else at the sidecar path is left in place: a link or a directory, or a file
that cannot be opened or read in full, since a read error can look like truncated
contents.

Declared permissions are added in one locked read-modify-write, including the `threading`
boolean. A successful write notifies an open Plugins dialog so its Permissions tab
refreshes; a failed write notifies nothing. If the sidecar was readable when the declaration
was read but is absent when the answer is saved, the write is refused instead of recreating
it: that is lost state, not a new install. Only a sidecar absent at both reads is seeded.
Runtime grants use the same per-target helper, which takes the lock and re-reads the sidecar
for every target.

Each audit decision reads the current sidecar through
`PluginManager::get_install_state`, so a grant persisted by a prompt applies to
subsequent top-level operations. A root frame that was already recorded is not
re-evaluated.

Installing or reinstalling a package resets its permissions, then
`revoke_plugin_permissions` clears the permission object so the new code must request
access again. Loading an already installed package preserves the existing sidecar
grants while updating install metadata, the enabled flag, and capability flags.

The Plugins dialog edits the same sidecar, including while the plugin is loaded or loading.
Each committed edit is applied immediately to the latest stored state, preserving unrelated
permissions, install metadata, the enabled flag, and capability flags. Concurrent changes to
the same permission take effect in write order. An open Permissions tab merges a grant made
outside the dialog into its displayed rows. Unsaved edits identify their original value, including
when the edited text matches another grant. If the original grant disappears, the edit stays visible
as no longer granted and can be added as a new grant or discarded. The dialog validates added or changed
targets; unchanged entries, including grants written by an older release, are carried across
byte-for-byte. New filesystem targets must be absolute on the current
platform: a POSIX path starts at `/`, and a Windows path names a drive plus root or a UNC
share. A `*` is accepted only as the final `/*` of a folder tree, and `..` components are
refused for filesystem grants. Create and delete use absolute folder paths, optional final `/*`
tree grants, or the all-folders mode. The matcher also refuses to widen a relative tree root, so a relative tree entry
saved by hand or carried over from an older release never authorizes. New network targets
must be a full `http://` or `https://` URL; the writer stores `normalize_network_url`'s canonical
lowercase `scheme://host`, dropping port, path, query and fragment. Bare hosts, other
schemes, wildcards and URLs carrying userinfo are refused rather than guessed. Network
entries cover every port, path and query on the host.

The dialog warns before enabling full access. Grants stored
while full access is on are inert and apply again when full access is off. Turning full access
on does not delete the stored lists. A permission change affects subsequent audit decisions,
not operations already authorized.

The dialog also asks for confirmation before granting create or delete access to all folders.
On Linux and macOS, create and delete operations using a directory file descriptor are checked
against its resolved path. If that path cannot be resolved, the operation is denied, even with
full access.

## Full access

Full access is a per-plugin boolean that turns off the permission decision. It does not
remove the audit hook. For recognized events it is reached after the unconditional
denied-path check, so the application configuration, the cloud user-secret file,
certificates, configuration paths, and `.install_state.json` stay blocked from direct
Python access. Everything else that the audit hook recognizes is allowed without a grant
or a prompt, and declared permissions are accepted without prompting. The read-only
`resources_dir()` root is a normal-mode restriction only: full access permits writes there
when the operating system allows them. The denied `cert` keyword still blocks the bundled
certificate.

Enabling full access requires a known installed package with valid metadata. Disabling it
accepts any known installed package, including one whose metadata has become invalid, so
the switch can always be turned off. The Plugins dialog asks for a native confirmation
before enabling it and gives the No button the initial focus. The confirmation is a
warning about the actual limit: full access is not a sandbox. The plugin can read and
write most files the user can access, and modifying startup or application files can run
code outside these checks. A subprocess started by the plugin is not restricted by the
audit hook at all.

A change applies to subsequent top-level audited operations. An operation that already
recorded its root frame keeps that coverage for its lifetime; changing the flag does not
retract it. Because the flag is read from the sidecar, a sidecar that cannot be read is
never treated as an opt-out.

## Security limitations

- **Python audit policy, not an OS sandbox.** Plugins run in the OrcaSlicer process with
  the user's operating-system rights. The hook constrains only the mapped, audited
  operations it observes.
- **Only recognized events are judged.** An audit event outside the map is ignored.
  Native code that performs filesystem, network, or process work without raising a
  mapped CPython audit event is not checked.
- **Module import and registration are not attributed.** The audit scope opens for
  capability calls, not for importing a plugin module or running
  `register_capabilities`. Code executed during those phases has no plugin audit
  context, so its events are not attributed or restricted.
- **Subprocesses leave the policy.** A process created by a granted or full-access
  plugin runs with the user's rights and is not covered by the parent's audit scope.
- **An `os.system` grant is a shell grant.** It authorizes every command string the plugin
  passes to `os.system`, not only the one that triggered the prompt, because the shell
  parses and runs the whole string. Arguments are not inspected or restricted.
- **An exact command line grant is command-sensitive, not binary-sensitive.** It is stored
  and shown in full, so it can expose credentials or tokens passed as arguments to anyone
  who sees the prompt or the sidecar. Approval covers only the same full command line.
  `CreateProcess` still resolves the executable from the working directory, `PATH`, and the
  filesystem, so the same approved command line can start a different binary later; the
  grant does not pin the resolved image or verify it.
- **Host APIs are not audit events.** Calls into OrcaSlicer's Python host APIs, such as
  preset updates or UI actions, execute C++ host code and follow that API's contract. The
  audit hook judges the Python-level filesystem, network, process and thread events it is
  given, not the host API's internal work. That includes `orca.host.tools`, through which any
  plugin calls the MCP tools: it can write the files those tools write and answer dialogs,
  permission prompts included.
- **Denied paths are a direct-access block.** Path laundering through a symlink,
  hardlink, subprocess, or Windows 8.3 short name is outside the model. A tree grant is
  canonicalized for matching, but the deny check itself is base-name and component based.
- **A recursive delete stops at a denied path.** Consent for the subtree does not override the
  deny registry. A protected entry inside the tree makes `shutil.rmtree` fail with
  `PermissionError` (or be skipped by `ignore_errors`), leaving that entry and any unvisited
  entries in place. This is deliberate: protected paths stay protected.
- **fd-based recursive deletes expose only partial paths.** `shutil.rmtree` reads with
  `os.scandir(fd)` and opens entries with `dir_fd`; those audit events carry an integer
  descriptor or a relative name rather than the full path. The running operation frame covers
  them normally, and each relative entry name is still checked, so denied entries stay blocked.
  A relative `os.open` target is canonicalized against the process working directory, so a
  working directory containing a denied keyword can over-block nested opens. That fails closed
  and does not widen access.
- **`data_dir()` is shared and writable.** Every plugin can write anywhere in the
  application data directory, including other plugin package files. Only the
  install-state sidecar and the denied names and keywords are blocked there.
- **Legacy entries keep their old meaning, except relative tree roots.** Plugin
  declarations normalize network URLs and resolve relative tree grants at request time,
  and the Plugins dialog normalizes newly entered or edited values the same way. A grant
  already stored by an older release is preserved verbatim when it is left unchanged, so
  a persisted URL with a path, port, query, fragment or trailing slash remains an exact
  grant, and an exact relative entry still matches verbatim. A persisted relative tree
  entry no longer authorizes: the matcher requires an absolute tree root, so the entry must
  be re-entered as an absolute path to take effect.
- **An off-main-thread prompt denies the current event.** A persistable grant is saved
  only if the user later accepts, so the first attempt from a worker fails with
  `PermissionError`; a later attempt can succeed after approval. A recursive-delete consent is
  instead kept as a short-lived, single-use retry approval bound to the plugin and canonical
  target, valid for the next matching attempt within one minute after approval, and is never
  persisted. Duplicate prompts for the same plugin and target are not posted.
- **A running root operation is generally not re-evaluated.** Revoking full access or changing a
  grant affects later top-level operations, not an operation whose root frame already recorded
  coverage. Nested delete events within a delete operation are checked separately, or covered
  by the one-time subtree consent recorded on that operation.

## File and symbol map

- `src/slic3r/plugin/PluginAuditManager.hpp` / `.cpp` - `PluginAuditManager`,
  `install_hook`, `audit_plugin_event`, `decide_audited_event`, `request_permissions`,
  `request_permission`, `request_recursive_delete_consent`, `delete_target_inside_approved_subtree`,
  `delete_target_inside_writable_allowed_root`, `AuditPromptQueue`, `check_path_access`,
  `network_permission_matches`, `default_denied_filenames`, `default_denied_path_keywords`,
  `ScopedPluginAuditContext`.
- `src/slic3r/plugin/PluginFsUtils.hpp` / `.cpp` - `PluginPermissions`,
  `PluginInstallState`, `read_install_state`, `write_install_state`.
- `src/slic3r/plugin/PythonPluginBridge.cpp` - `request_permissions` binding,
  `finalize_plugin_capture`, `install_python_thread_audit_shim`.
- `src/slic3r/plugin/PyPluginTrampoline.hpp` - `ORCA_PY_OVERRIDE_AUDITED`,
  `ORCA_PY_AUDIT_SCOPE`, `invoke_script_execute`.
- `src/slic3r/plugin/pluginTypes/slicingPipeline/SlicingPipelinePluginCapabilityTrampoline.hpp`
  provides the scoped G-code folder root.
- `src/slic3r/plugin/PluginManager.hpp` / `.cpp` - `mutate_plugin_permissions`,
  `revoke_plugin_permissions`, `get_install_state`.
- `src/slic3r/GUI/PluginsDialog.cpp` - permission editor payloads, native confirmations,
  and the full-access request path.
- `tests/slic3rutils/test_plugin_audit.cpp`, `tests/slic3rutils/test_plugin_install.cpp`,
  `tests/slic3rutils/test_plugin_lifecycle.cpp` - audit decision, sidecar, and permission
  mutation coverage.
