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
| `fs_readwrite` | target list | Write-shaped and read/write filesystem events mapped to `FsReadWrite`: a write-mode `open`, rename, chmod, utime, copy, move, archive extraction, and similar. Same target matching as `fs_read`. A write-mode `open` can create a file, but this grant does not cover events mapped to `FsCreate` or `FsDelete`, which have no persistent grant list. |
| `network_http` | target list | Network targets, consulted together with `network_socket` for both HTTP and socket events. A canonical `scheme://host` entry is host-wide and covers both layers; a URL with a path, port, query or trailing slash is exact; a bare host or legacy address tuple covers raw sockets. |
| `network_socket` | target list | The same network matching, kept as a separate list so a plugin can declare socket access without an HTTP URL. |
| `process` | target list | Process creation and process replacement: `os.system`, `subprocess.Popen`, `os.spawn*`, `os.exec*`, `pty.spawn`, `_winapi.CreateProcess`, and similar. Targets match the audited value verbatim. |
| `threading` | boolean | Permission to start a Python thread with `_thread.start_new_thread`. |
| `events` | target list | Audited events that expose no target, keyed as `<Category>:<event>`, for example `Socket:socket.__new__`. Matched verbatim. |
| `full_access` | boolean | Turns off the permission decision for the plugin. See "Full access". |

Filesystem create and delete events (`os.mkdir`, `os.symlink`, `tempfile.mkstemp`,
`os.remove`, `shutil.rmtree`, and similar) are audited, but they have no persistent grant
list. A `fs_read` or `fs_readwrite` grant does not cover these events, although a write grant
can create a file through `open()`. Without full access, each such
event outside an allowed root is prompted only when it can be answered inline. On the wx
main thread a Yes applies only to the operation that raised it and is not saved. Off the
main thread the event is denied immediately and an informational denial is shown instead;
there is no Yes/No prompt, no saved grant, and no promise that a later attempt will be
approved.

### Target matching

- Exact filesystem entries match the audited path verbatim. An absolute exact entry also
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
- Network entries can be an HTTP URL, a canonical `scheme://host`, a bare host, a
  `host:port` pair, or a legacy persisted address tuple such as `('host', 443)`.
  `normalize_network_url` reduces a URL to lowercase `scheme://host`; path, query,
  fragment, userinfo and port are dropped. A bare host, a `host:port` pair, or a legacy
  tuple is a raw-socket grant; it does not match an HTTP URL. A raw-socket entry has its
  port removed when it is matched, so a host-based network grant does not restrict ports.
  A canonical entry is host-wide and applies to HTTP and raw socket events for that host.
  A URL with a path, port, query, fragment or trailing slash that was stored without
  normalization remains an exact match.
- Process entries are compared verbatim with the string the audit event reports,
  including Python representations of a command.
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
   nested events raised through its standard-library frames are allowed without another
   prompt. The coverage is bounded to the operation frame and does not extend to plugin
   or third-party frames it calls back into. A nested `urllib.Request` is judged on its
   own URL, including when it follows a redirect.
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

## Permission prompts

### Declared permissions during load

A plugin declares its needs from Python with `orca.request_permissions(...)` while
`register_capabilities` runs. The bridge collects the declarations per plugin. After
registration returns and before any capability instance is created, OrcaSlicer presents
one combined prompt for the targets not already stored. Denying the request aborts the
load. A readable sidecar with `full_access` accepts the declaration without a prompt and
without adding a grant.

Network URLs passed to `request_permissions(network=...)` are normalized to
`scheme://host` before prompting or storing, so one approval covers every path and port
on that host and also authorizes raw sockets to it. A relative filesystem tree grant is
resolved against the working directory when the request is made, so the prompt and the
sidecar show the absolute scope it covers.

### Runtime prompts

An event that is not covered by a saved grant or an implicit rule is either put to the
user or denied. If the caller is on the wx main thread, the modal runs inline. If it is
not, as on the Script workflow, the plugin-load worker, or the slicing worker, the current
event is denied and the outcome is reported asynchronously. For a category with a persistent
grant list, a Yes/No prompt is posted to the main thread with `CallAfter`; if the user
accepts, the grant is saved and a later matching attempt can succeed without another prompt.
Create and delete do not support persistent grants: on the main thread they use the inline
prompt described above, while off the main thread they are denied and only an informational
denial is posted. That path offers no Yes/No choice and does not claim that the operation can
be approved on a retry. Blocking a worker on a marshalled modal is avoided because the UI
thread can itself be parked on the slicing worker.

Runtime approvals store the target observed by the audit hook. A URL with a path, port or
query is therefore an exact grant; a bare host remains usable for raw sockets. The
declared form and the runtime form are not normalized by the same code path.

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
`permissions` object stores `fs_read`, `fs_readwrite`, `network_http`,
`network_socket`, `process`, `threading`, `events`, and `full_access`. The file is
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
refused. The matcher also refuses to widen a relative tree root, so a relative tree entry
saved by hand or carried over from an older release never authorizes. New network targets
must be a full `http://` or `https://` URL; the writer stores `normalize_network_url`'s canonical
lowercase `scheme://host`, dropping port, path, query and fragment. Bare hosts, other
schemes, wildcards and URLs carrying userinfo are refused rather than guessed. Network
entries cover every port, path and query on the host.

The dialog warns before enabling full access. Grants stored
while full access is on are inert and apply again when full access is off. Turning full access
on does not delete the stored lists. A permission change affects subsequent audit decisions,
not operations already authorized.

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
- **Host APIs are not audit events.** Calls into OrcaSlicer's Python host APIs, such as
  preset updates or UI actions, execute C++ host code and follow that API's contract. The
  audit hook judges the Python-level filesystem, network, process and thread events it is
  given, not the host API's internal work.
- **Denied paths are a direct-access block.** Path laundering through a symlink,
  hardlink, subprocess, or Windows 8.3 short name is outside the model. A tree grant is
  canonicalized for matching, but the deny check itself is base-name and component based.
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
  `PermissionError`. Create and delete have no persistable grant and no retryable approval:
  off the main thread they are denied and only an informational denial is shown, while the
  inline main-thread prompt covers only the operation that raised it.
- **A running root operation is not re-evaluated.** Revoking full access or changing a
  grant affects later top-level operations, not an operation whose root frame already
  recorded coverage.

## File and symbol map

- `src/slic3r/plugin/PluginAuditManager.hpp` / `.cpp` - `PluginAuditManager`,
  `install_hook`, `audit_plugin_event`, `decide_audited_event`, `request_permissions`,
  `request_permission`, `check_path_access`, `network_permission_matches`,
  `default_denied_filenames`, `default_denied_path_keywords`, `ScopedPluginAuditContext`.
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
