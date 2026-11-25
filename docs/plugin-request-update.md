# Requesting an update from a cloud plugin

```python
accepted = orca.host.plugin.request_update()
```

This optional API takes no arguments and returns a `bool`. It always addresses
only the cloud plugin associated with the current plugin callback. Plugins cannot
supply another plugin key or a target version.

Call it from a UI-thread plugin callback (for example, a page message handler),
then return promptly. Orca copies the calling plugin identity and defers native
work to the main event loop. No Python object or plugin callback is retained by
the queued task. Before entering native update dialogs, Orca verifies that no
plugin callback remains on the UI stack, including nested event-loop cases.

`True` means Orca accepted the request for later processing. It does **not** mean
that an update exists, that the user confirmed it, or that installation succeeded.
`False` means another plugin-update request is already pending. Runtime errors explain
missing plugin context, calls off the UI thread, a local or unavailable package,
missing OrcaCloud sign-in, and a closing application. A queued request is abandoned
if Orca begins closing; an account change is rejected before installation.

Orca refreshes its cloud catalog and shows a native confirmation naming the plugin
and target version. Cancelling leaves the plugin installed. Download, package
replacement, rollback, reactivation and error presentation use the existing plugin
manager update path. The confirmed version is passed to that path explicitly.

The caller should disable its update action until the synchronous response arrives.
After acceptance it should show only that Orca received the request. The old plugin
may be unloaded: never wait for an installation callback or announce installation
success from the requesting version. Read the running version after reload instead.

Older hosts have no such attribute. Feature-detect the API and explain the manual
**Preferences ? Plugins** update path when it is unavailable. Local wheel installs
must continue using the plugin manager's local installation workflow.
