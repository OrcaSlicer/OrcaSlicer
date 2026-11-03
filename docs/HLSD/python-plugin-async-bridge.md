# Python plugin async bridge — High Level Design

## Purpose and scope

The embedded Python plugin runtime runs most capability calls synchronously: a C++ call site acquires
the GIL and the capability's Python method runs to completion on the calling thread. The staged preset
update (`begin_update()` in [preset-plugin-api.md](preset-plugin-api.md)) is also synchronous, but
its target selection can open unsaved-edit dialogs. It therefore blocks the Script worker, not the UI
thread, and returns only when the selection settles.

This document describes the Script capability's background execution path, including the optional
per-invocation asyncio loop for an `async def execute`, and the blocking UI marshalling that host calls
use from that worker. The bridge is deliberately narrow: only the Script capability gains a
background/async execution path. Every other capability — including the SlicingPipeline hooks — stays
synchronous on its caller's thread.

## Execution baseline

### How a capability call runs

A capability instance is a Python object owned through `Slic3r::PluginInstanceHandle`
(`src/slic3r/plugin/PythonPluginBridge.cpp`) and handed to the rest of the program as a
`std::shared_ptr<Slic3r::PluginCapabilityInterface>`. Its identity (`name()`, `type()`,
`audit_plugin_key()`) is resolved once at materialization and cached in C++
(`PluginCapabilityInterface` in `src/slic3r/plugin/PythonPluginInterface.hpp`).

Every C++→Python call crosses a trampoline method. `PyPluginCommonTrampoline` and
`PyPluginInterfaceTrampoline` (`src/slic3r/plugin/PyPluginTrampoline.hpp`), and the per-type subclass
`PyScriptPluginCapabilityTrampoline`
(`src/slic3r/plugin/pluginTypes/script/ScriptPluginCapabilityTrampoline.hpp`), wrap the call in the
`ORCA_PY_OVERRIDE_AUDITED` macro. For one synchronous call that macro, in order:

1. constructs a `PluginCapabilityInterface::RefCounter`, which `increment()`s the capability's
   reference count for the call's duration;
2. constructs a `PythonGILState`, which first acquires a `PythonRuntimeLease` and then
   `PyGILState_Ensure()`;
3. opens a `ScopedPluginAuditContext(audit_plugin_key(), name())`;
4. dispatches into Python via `PYBIND11_OVERRIDE`/`PYBIND11_OVERRIDE_PURE`;

and unwinds those guards when the call returns.

`PythonGILState` and `PythonRuntimeLease` live in `src/slic3r/plugin/PythonInterpreter.hpp`. A lease is
a `shared_lock` on `PythonInterpreter::m_runtime_mutex`; `PythonInterpreter::shutdown()` takes the
exclusive side, so a thread that holds a lease can safely touch Python objects, and interpreter
finalization cannot run underneath it. `ScopedPluginAuditContext`
(`src/slic3r/plugin/PluginAuditManager.hpp`) stores the previous thread-local plugin/capability pair
and scoped filesystem roots, sets this call's, and restores them on destruction; the audit hook then
attributes every filesystem/socket/process event to the plugin currently executing on that thread.

### The Script capability

`Slic3r::ScriptPluginCapability` (`src/slic3r/plugin/pluginTypes/script/ScriptPluginCapability.hpp`)
declares one pure virtual method, `ExecutionResult execute()`. The async runtime invokes it through
`invoke_script_execute` (`src/slic3r/plugin/PyPluginTrampoline.hpp`), which replicates the audited
trampoline call (ref counter, audit scope, traceback-preserving rethrow) but returns a
`ScriptExecuteInvocation` instead of forcing a cast:

- a plain `def execute` runs to completion in `invoke_script_execute` and yields
  `ScriptExecuteInvocation::result`;
- an `async def execute` (or any override that returns a coroutine, detected with
  `inspect.iscoroutine`) yields the coroutine object in `ScriptExecuteInvocation::awaitable` without
  running it, so the runtime can drive it.

`ScriptExecuteInvocation` is defined in `PyPluginTrampoline.hpp`; `py_is_coroutine` is the detection
helper.

### UI marshalling

`PluginHostUi::run_on_ui_thread` (`src/slic3r/plugin/host/PluginHostUi.cpp`) forwards to the
file-local `run_on_ui_blocking`:

- if the caller is already on the wx main thread, it runs the callable inline after a
  `py::gil_scoped_release`;
- otherwise it sets up a `std::promise`/`std::future`, releases the GIL, posts the callable with
  `GUI::wxGetApp().CallAfter`, and blocks on `future.get()`.

That is a blocking handoff: the caller's stack is parked on a thread while the UI thread runs the
callable. It is the correct primitive for the synchronous `update()`/`commit()` path and for
`reload_local_bundle`/`list_bundle_ids`, but it cannot be used by an operation that must suspend.

### Slicing hooks

`Print::set_slicing_pipeline_hook_fn` is installed by `src/slic3r/plugin/PluginHooks.cpp`. The hook
runs on the slicing worker thread and dispatches to `SlicingPipelinePluginCapability::execute(ctx)`
per capability, acquiring a `PythonGILState` per capability. `SlicingPipelineContext`
(`SlicingPipelinePluginCapability.hpp`) holds raw pointers into the live slicing graph
(`ctx.print`, `ctx.object`) that are valid only for the duration of `execute(ctx)`. The UI thread can
be blocked waiting on the slicing worker (`BackgroundSlicingProcess::stop()` parks it), so a
marshalled UI call from a hook can deadlock; `PluginHostSlicing.cpp`, `SlicingPipelinePluginCapability.hpp`
and `PluginHostUi.hpp` all document this.

## Script-only async execution

### Per-invocation worker and loop

`ScriptAsyncRuntime` (declared in `src/slic3r/plugin/PluginManager.hpp`, implemented in
`PluginManager.cpp`) drives every Script invocation off the caller's thread. Its `start()` creates a
`Run` and launches a detached worker thread that runs `Impl::run_invocation`. There is one worker
thread per invocation, so no invocation ever blocks the UI thread and no invocation is queued behind
another plugin's.

`run_invocation` runs detection and execution on the worker:

- it re-checks `cancel_requested` before anything (a cancellation that arrived before the run
  started is honored without running the plugin);
- it calls `invoke_script_execute`; a plain `def execute` result is used directly, and an `async def`
  coroutine is driven on a loop created for that invocation:
  `asyncio.new_event_loop()` → `asyncio.set_event_loop(loop)` → `loop.create_task(coroutine)` →
  `loop.run_until_complete(task)` → `loop.close()`, then `asyncio.set_event_loop(None)`.

The loop is per invocation, not global, and no loop outlives its invocation. The loop for the
calling thread is published in the thread-local `tl_script_loop` and exposed through
`ScriptAsyncRuntime::current_loop()` (GIL held). A host call such as `begin_update()` does not suspend
the loop: it blocks the worker thread while the UI settles, then returns a value the coroutine uses
directly.

`PluginManager::launch_script_capability` is the entry point: it resolves the capability, hands it to
`ScriptAsyncRuntime::start`, and sets `ScriptLaunchResult::launched_async`. The completion callback
runs on the GUI thread (or inline when there is no wx app), and `launch_script_capability` records a
run error through `set_plugin_error` before invoking the caller's `ScriptCompletionFn`.
`PluginManager::run_script_capability` is kept as a compatibility wrapper: it waits for the
completion callback, pumping `wxYield()` when it is itself on the GUI thread, and returns the
`ExecutionResult`.

### Capability, runtime, GIL and audit lifetime across a suspension

For the whole invocation — including an `async def` suspended at an `await` — `run_invocation` holds:

- a `PluginCapabilityInterface::RefCounter` per invocation and a `shared_ptr` to the capability in the
  `Run`, so the capability and its module stay alive across awaits and unload cannot drop them;
- a `PythonRuntimeLease` (acquired before the GIL). `PythonInterpreter::shutdown()` cannot finalize
  the interpreter while any invocation worker holds a lease;
- the GIL, acquired with `pybind11::gil_scoped_acquire` on the worker;
- a `ScopedPluginAuditContext(audit_plugin_key(), name())`, so filesystem/socket/process events
  raised by the coroutine are attributed to the plugin.

The capability reference is released inside the GIL scope (`run->capability.reset()`), because
dropping the pybind holder from a non-Python thread would `DECREF` without the GIL. The loop is
closed and `tl_script_loop` cleared before the worker exits.

A consequence of running Script on a non-main thread: the audit permission prompt is fail-closed off
the main thread. `request_permission` in `src/slic3r/plugin/PluginAuditManager.cpp` posts the dialog
with `CallAfter` and denies the current event, persisting the grant when the user accepts, so a
Script sees a denial on the first attempt and can succeed on a later one. This matches the existing
behavior for the plugin-load worker and the slicing worker.

### Blocking UI continuation

`PluginHostUi::run_on_ui_thread` (in `src/slic3r/plugin/host/PluginHostUi.cpp`) is the primitive host
calls use from the Script worker. It forwards to `run_on_ui_blocking`:

- if the caller is already on the wx main thread, it runs the callable inline after a
  `py::gil_scoped_release`;
- otherwise it sets up a `std::promise`/`std::future`, releases the GIL, posts the callable with
  `GUI::wxGetApp().CallAfter`, and blocks on `future.get()`.

Because the Script capability owns a worker thread, blocking there does not freeze the UI. The same
primitive is used by `begin_update()`, `set()`/`commit()`, `reload_local_bundle()` and
`list_bundle_ids()`.

### Synchronous `begin_update()`

`begin_preset_update` and `begin_filament_update` (`src/slic3r/plugin/host/PluginHostPresets.cpp`) run
on the Script worker with the GIL held and block it:

1. the binding enters through `PluginHostUi::run_on_ui_thread`, which fails with a `RuntimeError`
   before posting when there is no wx application;
2. the UI thread runs the target binding with the GIL released — `bind_preset_target` for
   process/printer, `bind_filament_target` for filament — including `Tab::select_preset()` and any
   unsaved-edit prompt;
3. the worker resumes with the name-based `PresetTransaction`, and nothing is staged until the
   caller's later `set()`/`commit()`.

An unavailable target is rejected before selection and changes no preset. A selection the user
declines raises `RuntimeError` and creates no transaction; decisions already accepted in an earlier
dialog are not rolled back. No GUI or preset pointer outlives the call: the transaction holds only
copied category/name/slot values.

`set()` stages a serialized value without mutating the preset, and `commit()` applies the batch
through `Tab::apply_preset_config_values` on the UI thread. Both are synchronous and, per their
bindings, valid only on the background Script worker.

### Cancellation

`ScriptAsyncRuntime::cancel_plugin(plugin_key)` sets `cancel_requested` on every active run for the
plugin and, when a loop is live (`has_loop`), cancels the task with
`loop.call_soon_threadsafe(task.cancel)` while holding the GIL. A synchronous `def execute`
invocation has no loop, so only the flag is set (checked before the run starts).

A task cancellation cannot interrupt a blocking `begin_update()` native call: the worker is inside
that call, so the loop does not run the scheduled `task.cancel()` until the selection settles and the
call returns. The transaction is therefore delivered, and any accepted UI selection stands; the
cancellation is observed at the coroutine's next suspension point. A user-declined selection is an
ordinary `RuntimeError`, not `asyncio.CancelledError`.

A plugin's own `asyncio.CancelledError` is treated as a cancellation only when `cancel_requested` was
set; otherwise it stays a genuine error (see `run_invocation`), so a plugin that cancels its own task
does not masquerade as an unload cancellation.

### Unload and shutdown drain

Unloading or shutting down must guarantee that no plugin Python code runs after its module is
unloaded, and that a blocking UI selection cannot mutate presets after teardown.

- `PluginManager::cancel_and_wait_for_capabilities` calls `on_cancelled()` first (so the plugin can
  observe state while the run is intact), then `ScriptAsyncRuntime::cancel_plugin`, then drains.
  The drain is **unbounded while a Script invocation worker is outstanding** — its loop-exit
  condition is `all_released && !invocation_active`. `has_active_run()` distinguishes a run that was
  cancelled before it started from one whose worker is still live, and a worker blocked in
  `begin_update()` keeps the run active until its UI callback returns. On the GUI thread the drain
  pumps `wxYield()` so a modal selection can settle instead of being starved by the blocking wait.
  Only the non-Script path keeps the historical bounded 2-second grace: when no invocation is active,
  the drain stops waiting for capability references at the grace deadline and logs any remaining
  ones.
- `PluginManager::shutdown` detaches the libslic3r hooks (`plugin_hooks::uninstall`), stops new
  loads, waits for in-flight loads, unloads every plugin (draining the async runs above), then calls
  `ScriptAsyncRuntime::instance().shutdown()`, which refuses new runs and waits without a deadline
  for every invocation worker to finish — nothing is torn down by force. `ScriptAsyncRuntime::reset()`
  re-arms the runtime so a later `initialize()` in the same process can start runs again.

Cancellation triggered by unload or shutdown is a separate, expected completion, not a plugin
failure. `run_invocation` marks a run whose `cancel_requested` was set as `completion.cancelled` and
clears any error, whatever the coroutine body did. `launch_script_capability` records a plugin error
only when `!completion.cancelled && !completion.error.empty()`; `run_script_capability` reports an
empty error for a cancelled completion; and the two GUI launchers treat `cancelled` as neither error
nor success (the speed dial stays silent, the Plugins dialog updates no status). A plugin's own
`CancelledError` is not counted as this expected termination unless `cancel_requested` is set, so a
self-cancelled plugin still records its failure.

## Supported and unsupported capabilities

| Capability | Async support | Reason |
| --- | --- | --- |
| Script | `def execute` and `async def execute` both run on the per-invocation worker | The runtime owns the worker and, for a coroutine, a per-invocation loop; nothing about a Script invocation borrows C++ state. |
| SlicingPipeline | Synchronous only | `execute(ctx)` borrows `Print`/`PrintObject` for the callback's duration, and the UI thread can be blocked on the slicing worker. `PluginHooks.cpp` rejects an async override: `cap->execute()` casts its Python result to `ExecutionResult`, and a coroutine object cannot be cast, so it surfaces as a `SlicingError` rather than being scheduled. |
| PrinterConnection, Pages, Analysis, Importer, Exporter, Visualization, ResourceTexture | Unchanged | Not part of this bridge; they keep their existing synchronous contract. |

## Deadlock constraints

- **Never block the UI thread on plugin work.** Every Script launch path goes through
  `launch_script_capability`; the speed-dial `ActionRegistry` action and the Plugins dialog launcher
  return immediately and report the outcome later (see Examples).
- **Never call `orca.host.ui.*` from a slicing hook.** The slicing worker runs while the UI thread
  can be parked on it.
- **Marshal host UI work from the Script worker only.** `PluginHostUi::run_on_ui_thread` blocks the
  calling worker while the UI thread runs the callback; calling it from the UI thread itself would
  deadlock the marshalling path.
- **No pointer crosses a call boundary.** `begin_update()` returns copied category/name/slot values;
  the `SlicingPipelineContext` graph is valid only within one synchronous `execute(ctx)`.

## File and symbol map

Script execution runtime:

- `src/slic3r/plugin/PluginManager.hpp` / `.cpp` — `ScriptAsyncRuntime` (`instance`, `start`,
  `cancel_plugin`, `has_active_run`, `shutdown`, `reset`, `current_loop`), `ScriptCompletion`,
  `ScriptCompletionFn`, `ScriptLaunchResult`, `launch_script_capability`, `run_script_capability`.
- `src/slic3r/plugin/PyPluginTrampoline.hpp` — `invoke_script_execute`, `ScriptExecuteInvocation`,
  `py_is_coroutine`.
- `src/slic3r/plugin/pluginTypes/script/ScriptPluginCapability.hpp` / `ScriptPluginCapabilityTrampoline.hpp`
  — `ScriptPluginCapability::execute`, `PyScriptPluginCapabilityTrampoline`.

Lifetime and GIL primitives:

- `src/slic3r/plugin/PythonInterpreter.hpp` — `PythonInterpreter`, `PythonRuntimeLease`,
  `PythonGILState`.
- `src/slic3r/plugin/PythonPluginInterface.hpp` — `PluginCapabilityInterface`, `RefCounter`,
  `increment`/`decrement`/`ref_count`.
- `src/slic3r/plugin/PluginAuditManager.hpp` / `.cpp` — `ScopedPluginAuditContext`,
  `request_filesystem_read_permissions`, `request_permission` (fail-closed off the main thread).

UI marshalling:

- `src/slic3r/plugin/host/PluginHostUi.hpp` / `.cpp` — `PluginHostUi::run_on_ui_thread`, the
  file-local `run_on_ui_blocking`, and `close_windows_for_plugin`.
- `GUI::wxGetApp().CallAfter` — the post-to-UI-thread primitive.

Preset targets:

- `src/slic3r/plugin/host/PluginHostPresets.cpp` — `PresetEditor`, `PresetTransaction`,
  `bind_preset_target`, `bind_filament_target`, `begin_preset_update`, `begin_filament_update`,
  `SelectionCancelled`, `resolve_combo_preset`/`resolve_preset_in_combo`.
- `src/slic3r/GUI/Tab.hpp` / `.cpp` — `Tab::select_preset`, `Tab::apply_preset_config_values`,
  `Tab::commit_preset_config_value`.

Launch paths:

- `src/slic3r/GUI/ActionRegistry.cpp` — `PluginScriptAction::run`, `notify_script_outcome`.
- `src/slic3r/GUI/PluginsDialog.cpp` — `PluginsDialog::run_script_plugin_capability`.

Slicing (synchronous only):

- `src/slic3r/plugin/PluginHooks.cpp` — `install_slicing_pipeline_hook`.
- `src/slic3r/plugin/pluginTypes/slicingPipeline/SlicingPipelinePluginCapability.hpp` —
  `SlicingPipelineContext`, `execute`.

Unload/shutdown:

- `src/slic3r/plugin/PluginManager.cpp` — `unload_plugin`, `unload_all_plugins`,
  `cancel_and_wait_for_capabilities`, `shutdown`.

## Examples

An `async def execute` that updates a preset. `execute` runs on the background Script worker with its
own asyncio loop; `begin_update()` blocks that worker until any selection / unsaved-edit dialog is
answered, without blocking the UI thread; `set()`/`commit()` then run synchronously on the same
worker. This is the shape used by `sandboxes/orca_preset_transaction_demo.py`:

```python
import asyncio

import orca


class AutoTune(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "Auto tune"

    async def execute(self):
        # begin_update() blocks this worker until the user answers any
        # selection / unsaved-edit dialog. The UI thread stays responsive.
        tx = orca.host.preset.process.begin_update(preset_name="0.20mm Standard")
        tx.set("layer_height", "0.2")
        tx.commit()
        return orca.ExecutionResult.success("process preset updated")
```

The window-driven demo shows the same pattern where the plugin's own UI must hand work to the worker:
`on_message` fires on the UI thread and only enqueues onto the worker loop with
`loop.call_soon_threadsafe`; `execute` consumes that queue and calls `begin_update()` per command.

Sequence for one `begin_update(...)`:

```text
Script worker (owned, per invocation)        UI thread
  loop.run_until_complete(task)
    begin_update()  ───────────────────────►  CallAfter(run_on_ui_blocking)
    (blocked; not suspended)                   bind_*_target →
                                               Tab::select_preset() … dialog
                                               settles
    promise set; worker resumes with PresetTransaction
  set(); commit() (UI thread for the batch)
  coroutine resumes at its next await
  execute() returns ExecutionResult
```

A plain `def execute` needs no loop and runs to completion on the same worker; both kinds are
launched with `launch_script_capability`, so the caller (UI thread) never blocks. A launch failure is
reported inline; a started run reports its outcome through the completion callback — the speed dial
shows "started" and pushes a completion notification, and the Plugins dialog updates its status when
`finish` runs.
