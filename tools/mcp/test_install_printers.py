"""Checks printer_list_models, printer_install and printer_uninstall, and that the Printer Selection dialog's Confirm,
which applies the printers through the same step, installs a model ticked in its page and keeps the others.
Its design is the test_install_printers entry of tools/mcp/test-entries/commands.md.

It launches OrcaSlicer itself, twice, on a fresh data folder seeded with one printer, the Bambu Lab X1 Carbon 0.4 nozzle,
and no cache/wizard_profile_data.json, so that the first printer_list_models builds that file; the restart shows that a
printer installed stays installed. Each launch is stopped on every path out, and nothing but a launch of this test is
stopped. What it installs comes from the profiles OrcaSlicer ships (the Goofoo Velo+ with its 0.4 nozzle, of a vendor
the folder has not installed); it needs no network, signs in to nothing and connects to no printer.

The dialog is opened as a person opens it, with the button beside the sidebar's Printer title. Its Confirm installs every
nozzle of each model it shows ticked, so the test installs the X1 Carbon's other nozzles first, with the presets' unsaved
changes carried over (preset_changes transfer, a process change and the printer's own), which makes the set the dialog
collects the set installed; it then ticks the Velo+, which printer_uninstall took out, clicking its block in the page as a
person does, confirms, and reads every Velo+ nozzle installed and the X1 Carbon's kept. Last, with Generic ABS @System installed, it
installs a model of a third vendor (the Voxelab Aquila X2) and reads the filaments its reload installed in the reply and
in preset_list, and that it swapped none.
Run it as python tools/mcp/test_install_printers.py [--bin PATH] [--datadir PATH].
"""
import os
import re
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from checks import Checks, launched, refusal as error_of, until  # noqa: E402
from orca_mcp import McpError  # noqa: E402

TOKEN = "install-printers-test-token"
# The printer the fresh folder is seeded with, in place of the harness's default Generic Klipper Printer: every
# filament its folder holds names Bambu Lab printers alone, so none fits the Velo+, whose install then installs its
# default filament, where the Generic @System filaments the default printer comes with fit every printer.
SEED = ["--vendor", "BBL", "--printer", "Bambu Lab X1 Carbon@0.4", "--filament", "Bambu PLA Basic @BBL X1C"]
X1C = "Bambu Lab X1 Carbon"
VELO = {"vendor": "Goofoo", "model": "Goofoo Velo+"}
# The test's named checks; check() records one and prints it.
t = Checks()
check = t.check


def call(orca, tool, **args):
    """`tool`'s result, followed to its end when it runs as a job."""
    return orca.await_job(orca.call(tool, **args))


def refusal(orca, tool, **args):
    """The text and details of the error `tool` replies, or None when it runs."""
    try:
        call(orca, tool, **args)
    except McpError as e:
        return error_of(e)
    return None


def printers(orca):
    """The printer presets the printer selector offers, and the one selected."""
    items = orca.call("preset_list", type="printer", limit=500)["items"]
    return sorted(item["name"] for item in items), next((item["name"] for item in items if item.get("selected")), None)


def model(orca, vendor, name):
    """printer_list_models' item for `vendor`'s model `name`."""
    items = call(orca, "printer_list_models", vendor=vendor, query=name, limit=500)["items"]
    return next((item for item in items if item["model"] == name), None)


def preset_of(orca, model_name, nozzle):
    """The installed printer preset of `model_name` with `nozzle`, or None."""
    for item in orca.call("preset_list", type="printer", limit=500)["items"]:
        if item.get("printer_model") == model_name and item.get("nozzle") == nozzle:
            return item["name"]
    return None


def first_run(orca, datadir):
    cache = os.path.join(datadir, "cache", "wizard_profile_data.json")
    check("the fresh folder holds no printer data cache", not os.path.exists(cache))
    listed = call(orca, "printer_list_models", limit=500)
    vendors = {item["vendor"] for item in listed["items"]}
    check("printer_list_models lists more than one vendor", len(vendors) > 1 and listed["total"] > 100,
          (len(vendors), listed["total"]))
    x1c = next((item for item in listed["items"] if item["vendor"] == "BBL" and item["model"] == X1C), None)
    check("the seeded model is marked installed with its nozzle", x1c is not None and x1c["installed_nozzles"] == ["0.4"]
          and "0.2" in x1c["nozzles"], x1c)
    check("and the printer data cache is written", os.path.exists(cache))
    velo = model(orca, "goofoo", VELO["model"])
    check("a model of a vendor not installed has several nozzles, none installed",
          velo is not None and {"0.2", "0.4", "0.6"} <= set(velo["nozzles"]) and velo["installed_nozzles"] == [], velo)

    found = refusal(orca, "printer_install", models=[{"model": "Goofoo Velo"}])
    check("a model name no vendor has is refused with the closest names", found is not None and
          found[1].get("code") == "no_such_model" and "Goofoo Velo+" in found[1].get("similar", []), found)
    found = refusal(orca, "printer_install", models=[dict(VELO, nozzles=["0.3"])])
    check("a nozzle the model lacks is refused with its nozzles", found is not None and found[1].get("code") == "no_such_nozzle"
          and "0.4" in found[1].get("nozzles", []), found)
    check("and nothing was installed", preset_of(orca, VELO["model"], "0.4") is None)

    before, _ = printers(orca)
    filaments_before = installed_filaments(orca)
    installed = call(orca, "printer_install", models=[dict(VELO, nozzles=["0.4"])])
    print(f"printer_install: {installed}", flush=True)
    velo_preset = preset_of(orca, VELO["model"], "0.4")
    names, selected = printers(orca)
    check("printer_install of the Velo+ with its 0.4 nozzle offers its preset, selected", velo_preset is not None
          and installed.get("added") == [velo_preset] and selected == velo_preset == installed.get("selected"),
          (installed, names, selected))
    check("and not its other nozzles, and keeps the seeded printer",
          preset_of(orca, VELO["model"], "0.2") is None and set(before) <= set(names), names)
    velo = model(orca, "Goofoo", VELO["model"])
    check("printer_list_models marks its 0.4 nozzle installed", velo is not None and velo["installed_nozzles"] == ["0.4"], velo)
    # The reload installs a printer's default filaments where no installed one fits it (load_installed_filaments()), and
    # none of the X1 Carbon's filaments the folder holds fits the Velo+: the reply names the Velo+ model's
    # default_materials, Goofoo ABS @System, which preset_list then lists installed, and the selectors offer them.
    reported = installed.get("filaments_installed", [])
    gained = installed_filaments(orca) - filaments_before
    check("the reload installs the Velo+'s default filament",
          reported != [] and set(reported) == gained and "Goofoo ABS @System" in reported
          and "filaments_uninstalled" not in installed, (installed, sorted(gained)))
    offered = {item["name"] for item in orca.call("preset_list", type="filament", printer=velo_preset, limit=500)["items"]}
    check("and offers them for it", reported != [] and set(reported) <= offered, (reported, sorted(offered)))
    return velo_preset


def second_run(orca, velo_preset):
    names, selected = printers(orca)
    check("after a restart the Velo+ is still installed", velo_preset in names, names)
    seeded = preset_of(orca, X1C, "0.4")
    orca.call("preset_select", type="printer", name=seeded)
    fits_velo = {item["name"] for item in orca.call("preset_list", type="filament", printer=velo_preset, limit=500)["items"]}
    fits_x1c = {item["name"] for item in orca.call("preset_list", type="filament", limit=500)["items"]}
    check("with the seeded printer selected, preset_list {printer} lists what fits the Velo+",
          fits_velo and "Bambu PLA Basic @BBL X1C" in fits_x1c and "Bambu PLA Basic @BBL X1C" not in fits_velo,
          (sorted(fits_velo), sorted(fits_x1c)))

    orca.call("preset_select", type="printer", name=velo_preset)
    removed = call(orca, "printer_uninstall", models=[VELO])
    names, selected = printers(orca)
    check("printer_uninstall of the selected Velo+ takes its preset out and selects the seeded printer",
          velo_preset not in names and removed.get("removed") == [velo_preset] and selected == seeded == removed.get("selected"),
          (removed, names, selected))

    found = refusal(orca, "printer_uninstall", models=[{"model": X1C, "nozzles": ["0.4"]}])
    check("uninstalling the last printer is refused", found is not None and found[1].get("code") == "last_printer"
          and "At least one printer model must stay installed." in found[0], found)
    check("and leaves it installed", printers(orca) == (names, selected), printers(orca))

    # Unsaved changes: refused without preset_changes, carried over with transfer: the process's, and the printer's own
    # onto the printer the install selects, whose printable height (250 mm) is not the one changed.
    orca.call("settings_set", type="process", values={"layer_height": 0.12})
    orca.call("settings_set", type="printer", values={"printable_height": 240})
    found = refusal(orca, "printer_install", models=[{"vendor": "BBL", "model": X1C}])
    check("an install while a preset has unsaved changes is refused, saying what to pass",
          found is not None and found[1].get("code") == "unsaved_preset_changes" and "preset_changes" in found[0], found)
    check("and installs nothing", printers(orca) == (names, selected), printers(orca))
    every = call(orca, "printer_install", models=[{"vendor": "BBL", "model": X1C}], preset_changes="transfer")
    kept = orca.call("settings_get", type="process", keys=["layer_height"])["items"][0]
    check("printer_install of every X1 Carbon nozzle with transfer keeps the process's unsaved change",
          sorted(every.get("added", [])) == sorted(p for p in (preset_of(orca, X1C, n) for n in ("0.2", "0.6", "0.8")) if p)
          and len(every.get("added", [])) == 3 and float(kept["value"]) == 0.12
          and "layer_height" in every.get("modified", {}).get("process", {}).get("keys", []), (every, kept))
    height = orca.call("settings_get", type="printer", keys=["printable_height"])["items"][0]
    saved = orca.call("settings_get", type="printer", keys=["printable_height"], preset=every.get("selected"))["items"][0]
    check("and puts the printer's own unsaved change on the printer it selects", every.get("selected") != seeded
          and float(height["value"]) == 240 and float(saved["value"]) == 250
          and "printable_height" in every.get("modified", {}).get("printer", {}).get("keys", []), (every, height, saved))
    orca.call("settings_set", type="process", reset_all=True)
    orca.call("settings_set", type="printer", reset_all=True)

    # The dialog's Confirm with a model ticked in its page, the Velo+ that printer_uninstall took out: it installs every
    # nozzle of it and keeps the X1 Carbon's.
    names_before, _ = printers(orca)
    x1c_before, velo_before = model(orca, "BBL", X1C), model(orca, "Goofoo", VELO["model"])
    check("before the dialog, the Velo+ is not installed", velo_before is not None and velo_before["installed_nozzles"] == [],
          velo_before)
    confirmed = confirm_printer_selection(orca, tick=VELO)
    check("the Printer Selection dialog opens from the sidebar, takes a tick of the Velo+, and its Confirm closes it",
          confirmed is True, confirmed)
    names_after, selected_after = printers(orca)
    velo_after, x1c_after = model(orca, "Goofoo", VELO["model"]), model(orca, "BBL", X1C)
    velo_presets = sorted(p for p in (preset_of(orca, VELO["model"], n) for n in velo_before["nozzles"]) if p)
    check("confirmed, it installs every nozzle of the Velo+, which the printer selector offers",
          velo_after is not None and sorted(velo_after["installed_nozzles"]) == sorted(velo_before["nozzles"])
          and len(velo_presets) == len(velo_before["nozzles"]) and set(velo_presets) == set(names_after) - set(names_before),
          (velo_after, velo_presets, sorted(set(names_after) - set(names_before))))
    check("and keeps the X1 Carbon's nozzles and printers installed",
          x1c_after == x1c_before and set(names_before) <= set(names_after), (x1c_before, x1c_after))
    check("and selects a printer the Confirm added", selected_after in velo_presets, selected_after)

    # The filaments the reload of a third vendor's printer install changes, with a @System filament installed. A reload
    # swaps a @System filament only for a vendor whose bundle lists default filaments of its own (its default_filaments,
    # which PresetBundle::apply_vendor_config() reads); only an INI bundle fills that list and the shipped JSON profiles
    # never do, so the install swaps none.
    orca.call("filament_install", names=["Generic ABS @System"])
    before = installed_filaments(orca)
    voxelab = call(orca, "printer_install", models=[{"vendor": "Voxelab", "model": "Voxelab Aquila X2"}])
    after = installed_filaments(orca)
    print(f"printer_install of another vendor's model: {voxelab}", flush=True)
    check("printer_install's filaments_installed is what preset_list shows installed",
          set(voxelab.get("filaments_installed", [])) == after - before, (voxelab, sorted(after - before)))
    check("and swaps no filament", "filaments_uninstalled" not in voxelab and before <= after and "Generic ABS @System" in after,
          (voxelab, sorted(before - after)))


def installed_filaments(orca):
    """The names of every installed filament, whatever its compatibility with the printer selected."""
    names, offset = set(), 0
    while offset is not None:
        page = orca.call("preset_list", type="filament", include_incompatible=True, limit=500, offset=offset)
        names.update(item["name"] for item in page["items"])
        offset = page.get("next_offset")
    return names


def printer_settings_button(orca):
    """The ref of the button beside the sidebar's Printer title that opens the Printer Selection dialog: the title row's
    last icon button, which says nothing of itself (no label, no tooltip), after the icons that do."""
    orca.call("view_set", tab="prepare")
    _, texts = orca.call_with_text("widget_list", target="main", include_menus=False, include_imgui=False, include_web=False,
                                   max_nodes=3000)
    lines = "\n".join(texts).splitlines()
    title = next((i for i, line in enumerate(lines) if re.search(r'label "Printer" \(Label\)', line)), None)
    if title is None:
        return None
    ref = None
    for line in lines[title + 1:]:
        if "combo" in line:
            break
        found = re.match(r"\s*\[(\w+)\] button \(ScalableButton\)", line)
        if found:
            ref = found.group(1)
    return ref


def confirm_printer_selection(orca, tick):
    """Opens the Printer Selection dialog with the button beside the sidebar's Printer title, as a person does, clicks
    the model `tick` ({vendor, model}) in its page, as a person ticks or unticks it, presses its Confirm, and waits until
    the dialog has closed and the click's job has ended. True, or what went wrong."""
    ref = printer_settings_button(orca)
    if ref is None:
        return "no button beside the sidebar's Printer title"
    clicked = orca.call("widget_click", target=ref)
    if not orca.call("app_wait", **{"for": "dialog", "timeout_s": 30}).get("satisfied"):
        return ("no dialog opened", clicked)
    # The page lists the models once it has the dialog's data, a moment after it opens, on a page it navigates to. A
    # model is ticked by a click on its block, which marks its box.
    box = f".ModelCheckBox[vendor='{tick['vendor']}'][model='{tick['model']}']"
    block = f".PrinterBlock[onclick=\"ChooseModel('{tick['vendor']}','{tick['model']}')\"]"
    selected = (f'(function () {{ var e = document.querySelector("{box}"); '
                'return e ? e.classList.contains("ModelCheckBoxSelected") : null; })()')

    def box_state():
        try:
            return orca.call("webview_run_javascript", code=selected).get("result")
        except McpError:  # the page is navigating
            return None

    ticked, button = None, "#PreBtn"
    try:
        until(lambda: box_state() is not None, "the Printer Selection page lists the models", timeout_s=40)
        orca.call("widget_click", target={"css": block}, timeout_s=40)
        ticked = box_state()
        # A page that did not take the tick is left with its Cancel, so that nothing is confirmed.
        if ticked is True:
            button = "#AcceptBtn"
    except (McpError, TimeoutError) as e:
        ticked = "the page could not be driven: " + str(e)
    orca.call("widget_click", target={"css": button}, timeout_s=40)
    if not orca.call("app_wait", **{"for": "no_dialog", "timeout_s": 50}).get("satisfied"):
        return "the dialog stayed open"
    orca.await_job(clicked)
    return True if button == "#AcceptBtn" else ("the model's box did not take the tick", ticked)


def main():
    extra = sys.argv[1:]
    binary = ["--bin", extra[extra.index("--bin") + 1]] if "--bin" in extra else []
    given = "--datadir" in extra
    root = os.path.abspath(extra[extra.index("--datadir") + 1]) if given else tempfile.mkdtemp(prefix="oip-")
    datadir = os.path.join(root, "printers")
    try:
        velo_preset = None
        with launched(datadir, binary, TOKEN, SEED) as orca:
            check("the app starts", orca is not None)
            if orca is not None:
                velo_preset = first_run(orca, datadir)
        if velo_preset is not None:
            with launched(datadir, binary, TOKEN, SEED) as orca:
                check("the app starts again on the same folder", orca is not None)
                if orca is not None:
                    second_run(orca, velo_preset)
    finally:
        if not given:
            shutil.rmtree(root, ignore_errors=True)

    return t.exit_code()


if __name__ == "__main__":
    sys.exit(main())
