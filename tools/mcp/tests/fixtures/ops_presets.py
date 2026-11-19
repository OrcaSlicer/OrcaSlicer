"""The plugin API's preset editors called from a script, for test_operation_routes.py, run with script_run:

    client.call("script_run", filename=".../fixtures/ops_presets.py",
                code="import ops_presets\nresult = ops_presets.preset_api(args)", args={...})

A call into the plugin API that raises is reported by its exception's type and message, as data the test compares,
rather than failing the run.
"""
import orca


def _transaction(t):
    return {"target_name": t.target_name, "selection_changed": t.selection_changed, "slot_number": t.slot_number,
            "category": t.category}


def _update_result(r):
    return {"target_name": r.target_name, "selection_changed": r.selection_changed,
            "slot_assignment_changed": r.slot_assignment_changed, "shared_slots": list(r.shared_slots),
            "keys": list(r.keys), "dirty": r.dirty}


def preset_api(args):
    """One call into orca.host.preset.<editor> (process, printer or filament). args: form "begin_update" with
    slot and name as given (either may be left out), then set [key, value] and commit when asked; or form
    "update" with slot when given, key and value."""
    editor = getattr(orca.host.preset, args["editor"])
    out = {"call": args}
    try:
        if args["form"] == "begin_update":
            positional = [args[k] for k in ("slot", "name") if k in args]
            t = editor.begin_update(*positional)
            out["begin_update"] = _transaction(t)
            if "set" in args:
                t.set(args["set"][0], args["set"][1])
            if args.get("commit"):
                out["commit"] = _update_result(t.commit())
        else:
            positional = [args[k] for k in ("slot", "key", "value") if k in args]
            editor.update(*positional)
            out["update"] = "returned"
    except Exception as e:
        out["raised"] = {"type": type(e).__name__, "message": str(e)}
    return out
