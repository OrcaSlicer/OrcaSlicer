#!/usr/bin/env python3
"""Where a setting applies, as the test_settings entry of tools/mcp/test-entries/filaments_settings.md designs it.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_settings.py

Exits 0 when every check passes.
"""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from checks import (GENERIC_PLA, KLIPPER, KLIPPER_STD, SLOT_COLOURS, Checks, add_slot, g_text, presets,  # noqa: E402
                    run, start, wait)

B = GENERIC_PLA
G = "Generic PETG @System"
PART_NOTE = 'A part\'s value (source "part", or in parts) applies only inside that part\'s volume.'
RANGE_NOTE = ('A height range\'s value (source "height_range", or in height_ranges) applies only between its min_z and '
              'max_z.')
OVERLAP_NOTE = ('Inside a modifier, mesh or painted, that sets none of a key (source "overlapped_part"), the value of '
                'the part it overlaps applies: that part\'s own in parts, else object_effective; which parts it overlaps '
                'is not worked out.')


def values_text(values):
    """{key: value} written as the entry writes it: `{extruder: 1, wall_loops: 4}`, keys in order, no quotes."""
    return "{" + ", ".join(f"{k}: {values[k]}" for k in sorted(values)) + "}"


class SettingsTest:
    def __init__(self, client, folder):
        self.c = client
        self.t = Checks()
        self.folder = folder

    # ---- Reads ----

    def listed(self, kind, **args):
        """settings_list's reply: total, next_offset where given, and the items as {key, value, meta, targets}, meta
        being whether the item has one."""
        reply = self.c.call("settings_list", type=kind, **args)
        items = [{"key": i["key"], "value": i.get("value"), "meta": "meta" in i, "targets": i.get("targets")}
                 for i in reply["items"]]
        return {"total": reply["total"], "next_offset": reply.get("next_offset"), "items": items}

    @staticmethod
    def values(listing):
        return {i["key"]: i["value"] for i in listing["items"]}

    def item(self, target, key, **where):
        """One item of settings_get whole, the reply's note and the target the reply names."""
        reply = self.c.call("settings_get", target=target, keys=[key], include_meta=False, **where)
        item = reply["items"][0]
        fields = {k: item[k] for k in ("value", "effective", "source", "parts", "height_ranges", "object_effective",
                                       "object_source") if k in item}
        return fields, reply.get("note"), self.named(reply)

    @staticmethod
    def named(reply):
        out = {}
        if "objects" in reply:
            out["objects"] = [o.get("index") for o in reply["objects"]]
        if "parts" in reply:
            out["parts"] = [{"object": p.get("object"), "part": p.get("part")} for p in reply["parts"]]
        if "height_range" in reply:
            out["height_range"] = {k: reply["height_range"].get(k) for k in ("min_z", "max_z")}
        return out

    def overrides(self, target, **where):
        """What the target sets, as {key: value}; a filament of 0 (Default) does not count."""
        reply = self.c.call("settings_get", target=target, include_meta=False, **where)
        return {i["key"]: i["value"] for i in reply["items"] if not (i["key"] == "extruder" and i["value"] == "0")}

    def cube(self):
        return self.c.call("object_list", limit=500, detail=True)["plates"][0]["objects"][0]

    def ranges(self):
        return [{"min_z": r["min_z"], "max_z": r["max_z"], "values": r["values"]}
                for r in self.cube().get("layer_config_ranges", [])]

    def targets(self):
        """`O · P0 · P1 · ranges`, a range as `min–max {values}`."""
        ranges = ", ".join(f"{g_text(r['min_z'])}–{g_text(r['max_z'])} {values_text(r['values'])}"
                           for r in self.ranges())
        return " · ".join([values_text(self.overrides("object", object=0)),
                           values_text(self.overrides("part", object=0, part=0)),
                           values_text(self.overrides("part", object=0, part=1)), ranges])

    def targets_are(self, label, want):
        self.t.equal(f"{label}: the targets' settings", self.targets(), want)

    def layers(self):
        stats = self.c.call("plate_slice_stats", include=["layer_features"], limit=1000)
        return [{"z": layer["z"], "height": layer["height"], "inner": layer["features"].get("Inner wall", 0),
                 "sparse": layer["features"].get("Sparse infill", 0)} for layer in stats["layers"]["items"]]

    @staticmethod
    def nearest(layers, z):
        return min(layers, key=lambda layer: abs(layer["z"] - z))

    # ---- Calls ----

    def run(self, tool, **args):
        return run(self.c, tool, **args)

    def start(self, name):
        self.t.equal(f"{name}: the start's presets", start(self.c),
                     {"printer": KLIPPER, "process": KLIPPER_STD, "filaments": [B]})
        self.t.equal(f"{name}: the start has no object", self.c.call("object_list", limit=500)["total"], 0)

    def whole(self, label, listing):
        """The list comes whole: as many items as total, no next page, no item with meta."""
        self.t.equal(f"{label}: whole, without meta",
                     (len(listing["items"]), listing["next_offset"], [i["key"] for i in listing["items"] if i["meta"]]),
                     (listing["total"], None, []))

    def has(self, label, listing, want):
        values = self.values(listing)
        self.t.equal(f"{label}: the values", {k: values.get(k) for k in want}, want)

    # ---- Scenarios ----

    def whole_types(self):
        s = "a type's settings"
        self.start(s)
        process = self.listed("process")
        self.t.check(f"{s} 1: at least 300 process settings", process["total"] >= 300, process["total"])
        self.whole(f"{s} 1", process)
        self.has(f"{s} 1", process, {"layer_height": "0.2", "wall_loops": "3", "sparse_infill_density": "15%",
                                     "top_shell_layers": "4"})
        keys = [i["key"] for i in process["items"]]

        printer = self.listed("printer")
        self.t.check(f"{s} 2: more than 100 printer settings", printer["total"] > 100, printer["total"])
        self.whole(f"{s} 2", printer)
        self.has(f"{s} 2", printer, {"nozzle_diameter": "0.4", "printable_height": "250"})

        filament = self.listed("filament")
        self.t.check(f"{s} 3: more than 100 filament settings", filament["total"] > 100, filament["total"])
        self.whole(f"{s} 3", filament)
        self.has(f"{s} 3", filament, {"filament_type": "PLA", "slow_down_layer_time": "4"})

        pages, offset = [], None
        for _ in range(20):
            page = self.listed("process", include_meta=True, **({} if offset is None else {"offset": offset}))
            pages.append(page)
            offset = page["next_offset"]
            if offset is None:
                break
        shapes = [(len(p["items"]), p["next_offset"]) for p in pages]
        want = [(100, 100 * (n + 1)) for n in range(len(pages) - 1)]
        self.t.equal(f"{s} 4: each page but the last holds 100 items and gives the next hundred", shapes[:-1], want)
        self.t.check(f"{s} 4: the last page holds at most 100 items and no next_offset",
                     shapes[-1][0] <= 100 and shapes[-1][1] is None, shapes[-1])
        self.t.equal(f"{s} 4: every item has meta", [i["key"] for p in pages for i in p["items"] if not i["meta"]], [])
        self.t.check(f"{s} 4: the pages' keys are step 1's", [i["key"] for p in pages for i in p["items"]] == keys,
                     len([i for p in pages for i in p["items"]]))

        page = self.listed("process", limit=50)
        self.t.equal(f"{s} 5: 50 items, next_offset 50, none with meta",
                     (len(page["items"]), page["next_offset"], [i["key"] for i in page["items"] if i["meta"]]),
                     (50, 50, []))
        self.t.check(f"{s} 5: step 1's first 50", [i["key"] for i in page["items"]] == keys[:50],
                     [i["key"] for i in page["items"]][:5])

        fine = self.listed("process", preset="0.16mm Optimal @MyKlipper")
        self.has(f"{s} 6", fine, {"layer_height": "0.16"})
        self.t.equal(f"{s} 6: reading a preset does not select it", presets(self.c)["process"], KLIPPER_STD)

        add_slot(self.c, SLOT_COLOURS[1])
        self.run("preset_select", type="filament", name="Generic PETG", filament_slot=2)
        wait(self.c, "idle")
        self.t.equal(f"{s} 7: the slots' presets", presets(self.c)["filaments"], [B, G])
        self.has(f"{s} 7: slot 2", self.listed("filament", filament_slot=2),
                 {"filament_type": "PETG", "slow_down_layer_time": "8"})
        self.has(f"{s} 7: slot 1", self.listed("filament", filament_slot=1),
                 {"filament_type": "PLA", "slow_down_layer_time": "4"})

    def object_and_region(self):
        s = "what an object and a region take"
        self.start(s)
        process = self.values(self.listed("process"))
        objects = self.listed("object")
        regions = self.listed("region")
        o, r = self.values(objects), self.values(regions)
        self.t.check(f"{s} 1: more than 200 object settings and 100 region settings", len(o) > 200 and len(r) > 100,
                     (len(o), len(r)))
        self.t.equal(f"{s} 1: every object setting is a process setting with its value",
                     [k for k in o if process.get(k, None) != o[k]], [])
        self.t.equal(f"{s} 1: every region setting but layer_height is an object setting",
                     [k for k in r if k != "layer_height" and k not in o], [])
        self.t.equal(f"{s} 1: both lists' walls and infill",
                     [(v.get("wall_loops"), v.get("sparse_infill_density")) for v in (o, r)],
                     [("3", "15%"), ("3", "15%")])
        layer = [(i["value"], i["targets"]) for i in regions["items"] if i["key"] == "layer_height"]
        self.t.equal(f"{s} 1: the region list's layer_height is a height range's", layer, [("0.2", ["height_range"])])
        others = [("object", i["key"]) for i in objects["items"] if i["targets"] is not None] + [
            ("region", i["key"]) for i in regions["items"] if i["targets"] is not None and i["key"] != "layer_height"]
        self.t.equal(f"{s} 1: no other item has targets", others, [])

    def precedence(self):
        s = "targets and precedence"
        self.start(s)
        plate = self.c.call("plate_list", limit=500)["items"][0]
        x, y = (plate["origin"][i] + plate["size"][i] / 2 for i in (0, 1))
        self.run("object_add_primitive", shape="cube", size=[20, 20, 20], name="cube", position=[x, y, 0])
        self.run("part_add", object=0, type="modifier", shape="cube", size=[5, 5, 5], position=[x, y, 10])
        self.run("object_set_height_ranges", object=0, ranges=[{"min_z": 0, "max_z": 5}, {"min_z": 5, "max_z": 10}])
        wait(self.c, "idle")
        cube = self.cube()
        self.t.equal(f"{s}: the start's cube", ([p["type"] for p in cube["parts"]], cube["overrides"]),
                     (["part", "modifier"], ["extruder"]))
        r0, r5 = "0–5 {extruder: 0, layer_height: 0.2}", "5–10 {extruder: 0, layer_height: 0.2}"
        self.targets_are(f"{s}: the start", f"{{extruder: 1}} · {{}} · {{}} · {r0}, {r5}")
        self.run("settings_set", type="filament", values={"slow_down_for_layer_cooling": False})
        self.t.equal(f"{s}: the cooling slowdown is off",
                     self.c.call("settings_get", type="filament", keys=["slow_down_for_layer_cooling"],
                                 include_meta=False)["items"][0]["value"], "0")
        reply = self.run("plate_slice")
        self.t.equal(f"{s}: the control slice", [p.get("outcome") for p in reply.get("plates", [])], ["ok"])
        control = self.layers()
        self.t.equal(f"{s}: the control's layers are 0.2 mm high",
                     [layer["z"] for layer in control if abs(layer["height"] - 0.2) > 0.001], [])
        at3, at15 = self.nearest(control, 3.0), self.nearest(control, 15.0)
        self.t.check(f"{s}: the control's layers at 3 and 15 mm print their inner walls alike",
                     abs(at3["z"] - 3.0) < 0.001 and abs(at15["z"] - 15.0) < 0.001 and
                     abs(at3["inner"] - at15["inner"]) <= 0.02 * at15["inner"], (at3, at15))

        o4 = "{extruder: 1, wall_loops: 4}"
        reply = self.run("settings_set", target="object", object=0, values={"wall_loops": 4})
        self.t.equal(f"{s} 1: the reply", (reply.get("applied"), [o.get("index") for o in reply.get("objects", [])]),
                     ({"wall_loops": "4"}, [0]))
        self.targets_are(f"{s} 1", f"{o4} · {{}} · {{}} · {r0}, {r5}")
        self.t.equal(f"{s} 1: object_list's overrides", self.cube()["overrides"], ["extruder", "wall_loops"])

        reply = self.run("settings_set", target="part", object=0, part=1, values={"wall_loops": 5})
        self.t.equal(f"{s} 2: the reply", (reply.get("applied"), self.named(reply).get("parts")),
                     ({"wall_loops": "5"}, [{"object": 0, "part": 1}]))
        self.targets_are(f"{s} 2", f"{o4} · {{}} · {{wall_loops: 5}} · {r0}, {r5}")

        reply = self.run("settings_set", target="height_range", object=0, min_z=0, max_z=5, values={"wall_loops": 6})
        named = self.named(reply)
        self.t.equal(f"{s} 3: the reply", (reply.get("applied"), named.get("objects"), named.get("height_range")),
                     ({"wall_loops": "6"}, [0], {"min_z": 0, "max_z": 5}))
        r0 = "0–5 {extruder: 0, layer_height: 0.2, wall_loops: 6}"
        self.targets_are(f"{s} 3", f"{o4} · {{}} · {{wall_loops: 5}} · {r0}, {r5}")
        self.t.equal(f"{s} 3: the range's overrides", self.overrides("height_range", object=0, min_z=0, max_z=5),
                     {"layer_height": "0.2", "wall_loops": "6"})

        reply = self.run("settings_set", target="height_range", object=0, min_z=5, max_z=10,
                         values={"layer_height": 0.12})
        self.t.equal(f"{s} 4: the reply", reply.get("applied"), {"layer_height": "0.12"})
        r5 = "5–10 {extruder: 0, layer_height: 0.12}"
        after4 = f"{o4} · {{}} · {{wall_loops: 5}} · {r0}, {r5}"
        self.targets_are(f"{s} 4", after4)

        self.run("object_select", parts=[[0, 1]])
        reply = self.c.call("settings_get", target="part")
        self.t.equal(f"{s} 5: the selected part's settings",
                     (self.named(reply).get("parts"), {i["key"]: i["value"] for i in reply["items"]}),
                     ([{"object": 0, "part": 1}], {"wall_loops": "5"}))
        self.targets_are(f"{s} 5", after4)
        fields, note, _ = self.item("part", "wall_loops")
        self.t.equal(f"{s} 6: the selected part's walls", (fields, note),
                     ({"value": "5", "effective": "5", "source": "part"}, PART_NOTE))
        self.targets_are(f"{s} 6", after4)

        reply = self.run("settings_set", target="part", object=0, part=0, values={"sparse_infill_density": "30%"})
        self.t.equal(f"{s} 7: the reply", reply.get("applied"), {"sparse_infill_density": "30%"})
        after7 = f"{o4} · {{sparse_infill_density: 30%}} · {{wall_loops: 5}} · {r0}, {r5}"
        self.targets_are(f"{s} 7", after7)

        fields, note, _ = self.item("part", "sparse_infill_density", object=0, part=1)
        self.t.equal(f"{s} 8: the modifier's infill", (fields, note), (
            {"value": None, "effective": None, "source": "overlapped_part", "object_effective": "15%",
             "object_source": "global", "parts": [{"part": 0, "value": "30%"}]}, f"{PART_NOTE} {OVERLAP_NOTE}"))
        self.targets_are(f"{s} 8", after7)
        fields, note, _ = self.item("object", "wall_loops", object=0)
        self.t.equal(f"{s} 9: the object's walls", (fields, note), (
            {"value": "4", "effective": "4", "source": "object", "parts": [{"part": 1, "value": "5"}],
             "height_ranges": [{"min_z": 0, "max_z": 5, "value": "6"}]}, f"{PART_NOTE} {RANGE_NOTE}"))
        self.targets_are(f"{s} 9", after7)
        reply = self.c.call("settings_get", target="height_range", object=0, min_z=0, max_z=5,
                            keys=["wall_loops", "sparse_infill_density"], include_meta=False)
        items = {i["key"]: {k: v for k, v in i.items() if k != "key"} for i in reply["items"]}
        self.t.equal(f"{s} 10: the range's walls and infill", (items, reply.get("note")), ({
            "wall_loops": {"value": "6", "effective": "6", "source": "height_range", "parts": [{"part": 1, "value": "5"}]},
            "sparse_infill_density": {"value": None, "effective": "15%", "source": "global",
                                      "parts": [{"part": 0, "value": "30%"}]}}, f"{PART_NOTE} {RANGE_NOTE}"))
        self.targets_are(f"{s} 10", after7)

        self.run("object_select", objects=[0])
        fields, note, named = self.item("height_range", "layer_height", min_z=5, max_z=10)
        self.t.equal(f"{s} 11: the selected object stands for the range's object",
                     (named.get("objects"), named.get("height_range"), fields, note),
                     ([0], {"min_z": 5, "max_z": 10}, {"value": "0.12", "effective": "0.12", "source": "height_range"},
                      RANGE_NOTE))
        self.targets_are(f"{s} 11", after7)

        reply = self.run("plate_slice")
        self.t.equal(f"{s} 12: the slice", [p.get("outcome") for p in reply.get("plates", [])], ["ok"])
        layers = self.layers()
        for name, (bottom, top), height in [("the range's layers", (5.3, 9.7), 0.12),
                                            ("the layers above it", (10.5, 19.5), 0.2)]:
            window = [(layer["z"], layer["height"]) for layer in layers if bottom <= layer["z"] <= top]
            self.t.check(f"{s} 12: {name} are {g_text(height)} mm high",
                         bool(window) and all(abs(h - height) <= 0.001 for _, h in window), window)
        low, high = self.nearest(layers, 3.0), self.nearest(layers, 15.0)
        self.t.check(f"{s} 12: six walls in the range, four on the object, three in the control",
                     abs(low["z"] - 3.0) < 0.001 and low["inner"] > high["inner"] > at15["inner"], (low, high, at15))
        self.t.check(f"{s} 12: part 0's infill at 30% prints longer than the control's 15%",
                     high["sparse"] > at15["sparse"], (high, at15))
        self.targets_are(f"{s} 12", after7)
        self.run("settings_set", type="filament", reset_all=True)
        self.t.equal(f"{s} 12: the cooling slowdown is on again",
                     self.c.call("settings_get", type="filament", keys=["slow_down_for_layer_cooling"],
                                 include_meta=False)["items"][0]["value"], "1")

        self.run("object_set_height_ranges", object=0, ranges=[{"min_z": 0, "max_z": 5}, {"min_z": 5, "max_z": 12}])
        after13 = f"{o4} · {{sparse_infill_density: 30%}} · {{wall_loops: 5}} · {r0}, 5–12 {{extruder: 0, layer_height: 0.2}}"
        self.targets_are(f"{s} 13: the range that stays keeps its settings", after13)

        path = os.path.join(self.folder, "settings.3mf")
        self.run("project_save", path=path, overwrite=True)
        self.run("project_new", discard_changes=True)
        self.run("project_open", path=path)
        wait(self.c, "idle")
        self.targets_are(f"{s} 14: the project file keeps every target's settings", after13)
        cube = self.cube()
        self.t.equal(f"{s} 14: the cube", ([p["type"] for p in cube["parts"]], cube["overrides"]),
                     (["part", "modifier"], ["extruder", "wall_loops"]))

    def run_all(self):
        for name, scenario in [("a type's settings", self.whole_types),
                               ("what an object and a region take", self.object_and_region),
                               ("targets and precedence", self.precedence)]:
            self.t.scenario(name, scenario)
        return self.t.exit_code()


def main():
    client, _ = orca_mcp.session_client()
    with tempfile.TemporaryDirectory(prefix="orca-settings-") as folder:
        return SettingsTest(client, folder).run_all()


if __name__ == "__main__":
    sys.exit(main())
