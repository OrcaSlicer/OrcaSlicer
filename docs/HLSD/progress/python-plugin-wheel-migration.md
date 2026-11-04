# Wheel installation migration: working progress

**Temporary document: remove the `docs/HLSD/progress/` folder before merging into main.**

## Status

The implementation in the working tree is **unfinished and not ready to merge**. It has received static review only. No builds or tests have been run. The permanent [wheel installation design](../python-plugin-wheel-installation.md) describes the intended design and constraints; it is not evidence that the unfinished implementation meets them.

The working tree currently contains an opt-in installation path, a per-plugin action, cache handling, update and backup changes, and focused tests. The change grew to include plugin update lifecycle work. Review found remaining safety problems, so the current code should not be treated as a completed migration.

The earlier proposal is in `docs/superpowers/uv-plugin-wheel-migration.md` in this workspace; that folder is ignored by Git, so it is not a required permanent reference.

## Decisions made

- Existing plugins keep their existing method until the user explicitly selects the new one for that plugin. A stopped plugin can be switched back; no decision has been made to remove the legacy method or force migration in a future release.
- Switching methods requires the plugin to be stopped. A restart may be needed for plugins containing native code.
- Keep the stored wheel, plugin identity, settings and plugin data. Cloud UUIDs can identify plugin paths, but uv still needs a valid wheel filename for the input; a temporary copy can provide one without renaming the stored entry.
- Entry-wheel installation uses uv without installing dependencies or contacting a package index. Declared dependencies retain the existing shared installation policy.
- An update should prepare a replacement and back up the old package. Restore the backup if installation or replacement fails. If installation succeeds but the plugin later fails to start, keep the backup for recovery rather than automatically rolling back on that later failure.
- Resource layout differences are noted for review after implementation. Plugin developers are responsible for checking that their plugins work with the resulting layout.
- An automatic fallback from a failed new installation to legacy loading, and the eventual fate of the legacy method, remain undecided. The explicit switch back is not an automatic fallback.

## Remaining review findings

The following were identified against the unfinished working tree and need to be rechecked as code changes:

1. An unload callback can write old plugin state after an update has replaced the package. The current suppression mechanism does not cover the callback that performs that write. This can change the replacement's enabled state or restore stale permissions.
2. An update may begin after an unload has removed the plugin from the registry but before teardown is finished, allowing package files to be replaced while they are still in use.
3. If final replacement fails after a running plugin has been stopped, the old files may be restored but the plugin remains stopped. Restoring files is not the same as restoring the previous running state.
4. The update path can reject older cloud installations located outside the currently derived cloud directory, even when discovery accepts them. The actual validated installed root needs careful handling.
5. Simultaneous requests to switch installation methods can report an opt-out as successful while an opt-in is still being committed.

The first four concern update and lifecycle handling; the fifth directly concerns switching installation methods. Some existing update paths already lacked transactional recovery, but the unfinished changes must not introduce new permission, identity or lifecycle regressions.

Other constraints to check include safe wheel filename reconstruction and compatibility tags, import name collisions, incomplete installations, resource paths, stale caches, Windows file locks, and concurrent load, delete and update operations. A retained backup currently needs a practical recovery and cleanup story; repeated updates may accumulate backups.

## Suggested next steps

1. Separate the installation-method change from the broader update-safety work, or establish a small shared lifecycle design before combining them. Preserve unrelated working-tree changes while reworking the current diff.
2. Resolve the findings above and make the scope of backup, restore and recovery explicit in the interface and error messages. Confirm that cloud and local identities and permission choices are preserved as intended.
3. Review existing plugins for filename and metadata compatibility, package resources and the effects of moving files from extracted to installed layout. Report any incompatibilities for a separate decision.
4. Add or refine focused coverage for migration and reversal, local and cloud wheels, cache reuse, update failures, retained backups, settings and permissions, and lifecycle races. Check native extensions and packaged layouts on supported platforms.
5. Run the relevant authorized builds and tests and exercise the UI before claiming the change is ready. Remove this progress folder before merging.
