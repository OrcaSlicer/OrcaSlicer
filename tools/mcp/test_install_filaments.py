"""Checks filament_install and filament_uninstall: a system filament installed by the short name the filament selector
shows is offered for a slot, also after a restart, and goes again when uninstalled; the filament a slot uses is not
uninstalled; and the app keeps a printer's default filaments installed: when an uninstall leaves an installed printer (a
second one, the Goofoo Velo+ 0.4, installed from the shipped profiles) with no filament that fits it, its default filaments
are installed again at once, the reply names them, and a restart shows the same. A printer that is only shown, not installed
(the Bambu Lab A1 mini 0.4, selected once), is not one of those: a filament call installs none of its default filaments,
then or after a restart.
Its design is the test_install_filaments entry of tools/mcp/test-entries/commands.md.

It launches OrcaSlicer itself, twice, on a fresh data folder seeded with one printer, the Bambu Lab X1 Carbon 0.4 nozzle,
and one filament, Bambu PLA Basic @BBL X1C, in slot 1; the restart shows that a filament installed stays installed. Each
launch is stopped on every path out, and nothing but a launch of this test is stopped. It needs no network, signs in to
nothing and connects to no printer.
Run it as python tools/mcp/test_install_filaments.py [--bin PATH] [--datadir PATH].
"""
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from checks import Checks, launched, refusal as error_of  # noqa: E402
from orca_mcp import McpError  # noqa: E402

TOKEN = "install-filaments-test-token"
SLOT_ONE = "Bambu PLA Basic @BBL X1C"
# The printer the fresh folder is seeded with, in place of the harness's default Generic Klipper Printer: the scenarios
# rest on Bambu Lab filaments that fit Bambu Lab printers alone, where the default printer's Generic @System filaments
# fit every printer.
SEED = ["--vendor", "BBL", "--printer", "Bambu Lab X1 Carbon@0.4", "--filament", SLOT_ONE]
# A printer of the seeded printer's bundle that the folder does not install and that SLOT_ONE does not fit, and a
# filament that fits only the seeded printer's family, so that installing it gives the A1 mini nothing that fits.
SHOWN_PRINTER = "Bambu Lab A1 mini 0.4 nozzle"
X1C_ONLY = "Bambu PLA Matte @BBL X1C"
# The test's named checks; check() records one and prints it.
t = Checks()
check = t.check


def refusal(orca, tool, **args):
    """The text and details of the error `tool` replies, or None when it runs."""
    try:
        orca.call(tool, **args)
    except McpError as e:
        return error_of(e)
    return None


def filaments(orca, **args):
    """preset_list's filament items by name, every page of them: those the selectors offer, or with include_uninstalled
    every one."""
    items, offset = {}, 0
    while offset is not None:
        page = orca.call("preset_list", type="filament", limit=500, offset=offset, **args)
        items.update((item["name"], item) for item in page["items"])
        offset = page.get("next_offset")
    return items


def offered_for_slot(orca, name, preset):
    """Whether preset_select assigns `name` to slot 1 as the selector offers it: the slot then holds `preset`, and the
    reply shows no hidden preset. Slot 1 gets its filament back afterwards."""
    reply = orca.call("preset_select", type="filament", name=name, filament_slot=1)
    orca.call("preset_select", type="filament", name=SLOT_ONE, filament_slot=1)
    return reply.get("selected") == preset and "shown" not in reply, reply


def bbl_printer(orca, name):
    """preset_list's item for the printer preset `name` of the seeded printer's bundle, installed or not."""
    items = orca.call("preset_list", type="printer", vendor="BBL", include_uninstalled=True, limit=500)["items"]
    return next((item for item in items if item["name"] == name), {})


def shown_printer_left_out(orca):
    """Selects a printer that is not installed and then the seeded printer again, which shows the printer and its
    default filaments for this session only; a filament call then installs none of those filaments, as a restart would
    not, and leaves the printer shown. The selection is as it was afterwards. Returns the default filaments it showed."""
    selected = orca.call("app_state")["presets"]
    listed = bbl_printer(orca, SHOWN_PRINTER)
    check("a printer of the seeded bundle is not installed and no installed filament fits it",
          listed.get("installed") is False and not fitting(orca, SHOWN_PRINTER), (listed, sorted(fitting(orca, SHOWN_PRINTER))))
    before = set(filaments(orca, include_incompatible=True))
    orca.call("preset_select", type="printer", name=SHOWN_PRINTER)
    orca.call("preset_select", type="printer", name=selected["printer"])
    orca.call("preset_select", type="process", name=selected["process"])
    orca.call("preset_select", type="filament", name=SLOT_ONE, filament_slot=1)
    shown = set(filaments(orca, include_incompatible=True)) - before
    check("selecting it shows it and its default filaments for this session",
          shown and "installed" not in bbl_printer(orca, SHOWN_PRINTER), (sorted(shown), bbl_printer(orca, SHOWN_PRINTER)))

    reply = orca.call("filament_install", names=[X1C_ONLY])
    print(f"filament_install {X1C_ONLY!r} with {SHOWN_PRINTER!r} shown: {reply}", flush=True)
    after = set(filaments(orca, include_incompatible=True))
    check("a filament call then names none of that printer's default filaments as installed",
          reply.get("installed") == [X1C_ONLY] and not reply.get("filaments_installed"), reply)
    check("and what is installed is what was installed before and the filament it named", after == before | {X1C_ONLY},
          (sorted(after - before - {X1C_ONLY}), sorted(before - after)))
    check("and the printer stays shown for this session", "installed" not in bbl_printer(orca, SHOWN_PRINTER),
          bbl_printer(orca, SHOWN_PRINTER))
    return sorted(shown)


def first_run(orca):
    shown = shown_printer_left_out(orca)
    # A short name also stands for the presets whose compatible_printers name no printer, as the dialog's row does, which
    # a compatibility condition can keep from the selected printer: preset_list lists those with include_incompatible.
    every = filaments(orca, include_uninstalled=True, include_incompatible=True)
    # A filament of the seeded printer the folder has not installed, and the short name its selector shows.
    candidate = next((item for item in every.values() if item.get("installed") is False and item.get("compatible", True)
                      and item.get("alias") and item.get("vendor") == "Bambu Lab"), None)
    check("preset_list lists a compatible filament with installed: false", candidate is not None)
    if candidate is None:
        return None
    short = candidate["alias"]

    found = refusal(orca, "filament_install", names=[short + "X"])
    check("a name no filament has is refused with the closest names", found is not None
          and found[1].get("code") == "no_such_filament" and short in found[1].get("similar", []), found)

    installed = orca.call("filament_install", names=[short])
    print(f"filament_install {short!r}: {installed}", flush=True)
    offered = filaments(orca)
    check("filament_install by the short name installs its preset of the seeded printer",
          candidate["name"] in installed.get("installed", []) and candidate["name"] in offered
          and "installed" not in offered[candidate["name"]], (installed, short))
    check("each preset it installed is of that short name", all(every[name].get("alias") == short
                                                                for name in installed.get("installed", [])), installed)
    ok, reply = offered_for_slot(orca, short, candidate["name"])
    check("and the slot's selector offers it (preset_select shows no hidden preset)", ok, reply)
    again = orca.call("filament_install", names=[short])
    check("installing it again changes nothing and says so", again.get("installed") == []
          and candidate["name"] in again.get("installed_already", []), again)
    return short, candidate["name"], defaults_given_back(orca), shown


def fitting(orca, printer):
    """The installed filaments preset_list offers for `printer`, a printer preset, by name."""
    return set(filaments(orca, printer=printer))


def defaults_given_back(orca):
    """With a second printer installed and not selected, uninstalls every installed filament that fits it and no slot
    uses: the app keeps a printer's default filaments installed, so they are installed again at once, and the reply
    names them. Returns the second printer's preset and the filaments offered for it afterwards, which a restart keeps."""
    seeded = orca.call("app_state")["presets"]["printer"]
    added = orca.await_job(orca.call("printer_install", models=[{"vendor": "Goofoo", "model": "Goofoo Velo+", "nozzles": ["0.4"]}]))
    velo = added["selected"]
    orca.call("preset_select", type="printer", name=seeded)
    in_slots = {name for name, item in filaments(orca).items() if item.get("filament_slots")}
    fits = fitting(orca, velo) - in_slots
    check("a second printer, not selected, has installed filaments that fit it and no slot uses", fits and velo != seeded,
          (added, sorted(fits)))
    removed = orca.call("filament_uninstall", names=sorted(fits))
    print(f"filament_uninstall {sorted(fits)}: {removed}", flush=True)
    given = set(removed.get("filaments_installed", []))
    now = fitting(orca, velo)
    installed = set(filaments(orca, include_incompatible=True))
    check("the reply names the default filaments the app gives the second printer again, and only those",
          given and given == now - in_slots and given <= installed, (removed, sorted(now)))
    # filament_uninstall lists under uninstalled only the presets left uninstalled: those asked that the app did not
    # give back.
    uninstalled = removed.get("uninstalled")
    check("what the reply calls uninstalled is exactly what stays uninstalled",
          uninstalled is not None and set(uninstalled) == fits - given and not set(uninstalled) & installed, removed)
    return velo, sorted(now)


def second_run(orca, short, preset, given_back, shown):
    installed = set(filaments(orca, include_incompatible=True))
    check("after a restart none of the default filaments of the printer that was only shown is installed",
          not installed & set(shown), sorted(installed & set(shown)))
    velo, offered_for_velo = given_back
    check("after a restart the filaments offered for the second printer are what the uninstall replied",
          sorted(fitting(orca, velo)) == offered_for_velo, (sorted(fitting(orca, velo)), offered_for_velo))
    offered = filaments(orca)
    check("after a restart it is still installed", preset in offered and "installed" not in offered[preset],
          offered.get(preset))
    ok, reply = offered_for_slot(orca, short, preset)
    check("and offered for a slot", ok, reply)

    removed = orca.call("filament_uninstall", names=[short])
    check("filament_uninstall by the short name uninstalls it", preset in removed.get("uninstalled", [])
          and preset not in filaments(orca) and filaments(orca, include_uninstalled=True)[preset].get("installed") is False,
          removed)

    found = refusal(orca, "filament_uninstall", names=["Bambu PLA Basic"])
    check("uninstalling the filament slot 1 uses is refused, naming the slot", found is not None
          and found[1].get("code") == "filament_in_use" and "Filament slot 1 uses " + SLOT_ONE + "." in found[0], found)
    check("and it stays installed", SLOT_ONE in filaments(orca))


def main():
    extra = sys.argv[1:]
    binary = ["--bin", extra[extra.index("--bin") + 1]] if "--bin" in extra else []
    given = "--datadir" in extra
    root = os.path.abspath(extra[extra.index("--datadir") + 1]) if given else tempfile.mkdtemp(prefix="oif-")
    datadir = os.path.join(root, "filaments")
    try:
        named = None
        with launched(datadir, binary, TOKEN, SEED) as orca:
            check("the app starts", orca is not None)
            if orca is not None:
                named = first_run(orca)
        if named is not None:
            with launched(datadir, binary, TOKEN, SEED) as orca:
                check("the app starts again on the same folder", orca is not None)
                if orca is not None:
                    second_run(orca, *named)
    finally:
        if not given:
            shutil.rmtree(root, ignore_errors=True)

    return t.exit_code()


if __name__ == "__main__":
    sys.exit(main())
