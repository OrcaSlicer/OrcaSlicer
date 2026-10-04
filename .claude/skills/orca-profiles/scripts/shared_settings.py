#!/usr/bin/env python3
"""Find settings to move onto shared bases, and prove a move changed nothing.

  snapshot OUT.json       write every selectable preset's config as the loader stores it
  compare BEFORE.json     report every value that differs from the snapshot; exit 1 if any
  candidates --vendor V   restated values; per base, the settings its presets all share and the
                          defaults with exceptions that would pay

Every subcommand takes --profiles DIR (default resources/profiles).

Configs are composed the loader's way: the parent's stored config, then each include at
the width its file wrote, then the preset's own keys, and every variant key resized to the
preset's own variant list (one variant without one), padded with its first value or cut.
A base is stored after that resize, so a variant array wider than a base's list reaches its
children cut. Not modelled: the built-in defaults. A key no file in a preset's chain writes
loads its default, so compare reports a key written on one side only separately: it is no
change when the written value is the option's default in PrintConfig.cpp. Nor is it modelled
that an include template does not pass on a key equal to the default. Reads
scripts/orca_profile_tool.py.
"""
import argparse
import json
import os
import sys
from collections import Counter, defaultdict

# The profile tool lives in <repo>/scripts; this file in <repo>/.claude/skills/orca-profiles/scripts.
sys.path[:0] = [os.path.join(os.getcwd(), "scripts"),
                os.path.join(os.path.dirname(os.path.abspath(__file__)), *[os.pardir] * 4, "scripts")]
import orca_profile_tool as tool  # noqa: E402

TYPES = ("machine", "process", "filament")
# Keys the loader reads from each file as metadata; neither inherits nor include passes them on.
PER_FILE = {"type", "name", "from", "instantiation", "setting_id", "renamed_from", "description",
            "inherits", "include", "version", "url", "is_custom_defined"}
# Keys the app replaces with the selected presets' names before slicing: a file's value never counts.
REPLACED = {"print_settings_id", "printer_settings_id", "filament_settings_id"}
# What a restructure changes by design, or what never reaches a slice.
MOVED_BY_DESIGN = {"inherits", "include"} | REPLACED
# Keys that stay in their own file: metadata, identity, and each preset's compatibility.
NEVER_SHARED = PER_FILE | REPLACED | {"filament_id", "compatible_printers", "compatible_prints",
                                     "printer_variant", "printer_model"}


class Tree:
    def __init__(self, profiles_dir):
        self.dir = profiles_dir
        self.scheme = tool._variant_scheme()
        self.vendors = tool.list_vendor_names(profiles_dir)
        self.bundles = {v: tool.load_vendor_configs(profiles_dir, v) for v in self.vendors}
        self.cache = {}

    def lookup(self, vendor, ptype, name, in_ofl=False):
        """(vendor the name resolves in, (rel, data)); filaments fall back to the library."""
        if not in_ofl and name in self.bundles[vendor][ptype]:
            return vendor, self.bundles[vendor][ptype][name]
        if ptype == "filament" and tool.OFL in self.bundles and name in self.bundles[tool.OFL][ptype]:
            return tool.OFL, self.bundles[tool.OFL][ptype][name]
        return None, None

    def composed(self, vendor, ptype, name, drop=None, seen=frozenset()):
        """Config before the preset's own resize: parent stored, includes, own keys."""
        owner, found = self.lookup(vendor, ptype, name, vendor == tool.OFL)
        if found is None or (owner, name) in seen:
            return {}
        seen = seen | {(owner, name)}
        data = found[1]
        config = {}
        if data.get("inherits"):
            config.update(self.stored(owner, ptype, data["inherits"], seen))
        include = data.get("include") or []
        for included in [include] if isinstance(include, str) else include:
            if included in self.bundles[owner][ptype]:
                config.update(self.composed(owner, ptype, included, seen=seen))
        config = {k: v for k, v in config.items() if k not in PER_FILE}
        config.update((k, v) for k, v in data.items() if k != drop)
        return config

    def stored(self, vendor, ptype, name, seen=frozenset()):
        key = (vendor, ptype, name)
        if key not in self.cache:
            self.cache[key] = self.resize(ptype, self.composed(vendor, ptype, name, seen=seen))
        return self.cache[key]

    def resize(self, ptype, config):
        list_key, strides = self.scheme[ptype]
        length = len(tool._as_list(config[list_key])) if list_key in config else 1
        out = dict(config)
        for key, stride in strides.items():
            if key in out:
                values = tool._as_list(out[key])
                need = length * stride
                out[key] = values[:need] + values[:1] * (need - len(values))
        return out

    def presets(self, vendors=None, ptypes=TYPES):
        for vendor in vendors or self.vendors:
            for ptype in ptypes:
                for name, (rel, data) in sorted(self.bundles[vendor][ptype].items()):
                    yield vendor, ptype, name, rel, data


def snapshot(tree):
    return {f"{vendor}/{ptype}/{name}": {k: v for k, v in tree.stored(vendor, ptype, name).items()
                                        if k not in MOVED_BY_DESIGN}
            for vendor, ptype, name, _rel, data in tree.presets()
            if data.get("instantiation") == "true"}


def compare(before, after):
    changed, one_sided = 0, []
    for preset in sorted(before.keys() | after.keys()):
        old, new = before.get(preset), after.get(preset)
        if old is None or new is None:
            print(f"{preset}: {'added' if old is None else 'removed'}")
            changed += 1
            continue
        for key in sorted(old.keys() | new.keys()):
            if key not in old or key not in new:
                one_sided.append(f"{preset}: {key} "
                                 f"{json.dumps(old[key]) if key in old else '(built-in default)'} -> "
                                 f"{json.dumps(new[key]) if key in new else '(built-in default)'}")
            elif old[key] != new[key]:
                print(f"{preset}: {key} {json.dumps(old[key])} -> {json.dumps(new[key])}")
                changed += 1
    for line in one_sided:
        print(line)
    print(f"{changed} difference(s)")
    if one_sided:
        print(f"{len(one_sided)} key(s) written on one side only: each is a difference unless the "
              f"written value is the option's default in src/libslic3r/PrintConfig.cpp")
    return changed + len(one_sided)


def candidates(tree, vendor, ptypes, group_by):
    for ptype in ptypes:
        entries = {name: data for _v, _t, name, _rel, data in tree.presets([vendor], (ptype,))}
        selectable = [n for n, d in entries.items() if d.get("instantiation") == "true"]
        stored = {n: tree.stored(vendor, ptype, n) for n in entries}
        list_key = tree.scheme[ptype][0]

        # Restated: a key a file writes that it would inherit unchanged without writing it.
        restated = {}
        for name, data in entries.items():
            restated[name] = sorted(
                k for k in data if k not in NEVER_SHARED and k != list_key
                and (data.get("inherits") or data.get("include"))
                and tree.resize(ptype, tree.composed(vendor, ptype, name, drop=k)).get(k) == stored[name][k])
            if restated[name]:
                print(f"{vendor}/{ptype} {name}: restates what it inherits: {', '.join(restated[name])}")

        # Groups: every preset with selectable presets below it, or the --group-by values.
        chain = {n: [] for n in selectable}
        for name in selectable:
            node = entries[name].get("inherits")
            while node in entries and node not in chain[name]:
                chain[name].append(node)
                node = entries[node].get("inherits")
        groups = defaultdict(list)
        for name in selectable:
            if group_by:
                groups[json.dumps(stored[name].get(group_by))].append(name)
            else:
                for base in chain[name]:
                    groups[base].append(name)
        printed = {}
        for label, members in sorted(groups.items(), key=lambda g: -len(g[1])):
            if len(members) < 2 or label == "null":
                continue
            if frozenset(members) in printed:
                print(f"\n{vendor}/{ptype} {label}: the same presets as {printed[frozenset(members)]}")
                continue
            common = [b for b in chain[members[0]] if all(b in chain[m] for m in members[1:])]
            home = common[0] if group_by and common else None if group_by else label

            def below(m):
                """m and the files between it and the group's home."""
                return [m] + chain[m][:chain[m].index(home)] if home in chain[m] else [m]
            shared, defaults = [], []
            for key in sorted(set().union(*(entries[m].keys() for m in members)) - NEVER_SHARED):
                loaded = [json.dumps(stored[m].get(key)) for m in members]
                counts = Counter(loaded).most_common(2)
                if len(counts) == 1:
                    writers = sum(key in entries[m] and key not in restated[m] for m in members)
                    if writers >= 2:
                        shared.append(f"{key} ({writers} write it)")
                    continue
                (top, held), (_, runner_up) = counts
                if held == runner_up or top == "null" or home is None:
                    continue
                # Balance 5: w presets drop their copy; a presets that take the key from the home
                # (no file on their way to it writes it) and load another value must write theirs.
                w = sum(key in entries[m] and v == top for m, v in zip(members, loaded))
                a = sum(v != top and not any(key in entries[f] for f in below(m))
                        for m, v in zip(members, loaded))
                if w - a > 1:
                    shown = top if len(top) <= 40 else top[:37] + "..."
                    defaults.append(f"{key} = {shown}: {held} load it, {w} write it, "
                                    f"{a} would have to write their own")
            if shared or defaults:
                where = f"; nearest common base {common[0]}" if group_by and common else ""
                title = f"{group_by} = {label}" if group_by else label
                print(f"\n{vendor}/{ptype} {title}: {len(members)} presets{where}")
                printed[frozenset(members)] = title
                for line in shared:
                    print(f"  {line}")
                if defaults:
                    print("  default with exceptions:")
                    for line in defaults:
                        print(f"    {line}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    profiles = argparse.ArgumentParser(add_help=False)
    profiles.add_argument("--profiles", default=os.path.join("resources", "profiles"),
                          help="profiles directory (default: resources/profiles)")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("snapshot", parents=[profiles]).add_argument("out")
    sub.add_parser("compare", parents=[profiles]).add_argument("before")
    cand = sub.add_parser("candidates", parents=[profiles])
    cand.add_argument("--vendor", required=True)
    cand.add_argument("--type", choices=TYPES, action="append")
    cand.add_argument("--group-by", help="group selectable presets by this key's value instead of by "
                      "base: printer_model, gcode_flavor, extruder_type, filament_id, layer_height, ...")
    args = parser.parse_args()
    tree = Tree(args.profiles)
    if args.command == "snapshot":
        presets = snapshot(tree)
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(presets, f, sort_keys=True)
        print(f"{len(presets)} selectable presets written to {args.out}")
    elif args.command == "compare":
        with open(args.before, encoding="utf-8") as f:
            sys.exit(1 if compare(json.load(f), snapshot(tree)) else 0)
    else:
        candidates(tree, args.vendor, args.type or TYPES, args.group_by)


if __name__ == "__main__":
    main()
