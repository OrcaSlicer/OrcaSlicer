#!/usr/bin/env python3
"""
Assign and validate the deterministic ids of OrcaSlicer system profiles: the
per-product filament_id and the per-preset setting_id.

Both ids are pure functions of the thing they name, so nothing here is ever
invented: the tool only writes the id the rules below already imply, and a tree
that already satisfies them is left untouched.

filament_id policy (see docs/HLSD/filament_id.md):
  * filament_id is a PRODUCT id: one named spool product = one id, shared by all
    of that product's per-printer/per-nozzle variants in every bundle. The
    granularity is the name on the spool, not the brand: "AAA PLA Lite" and
    "AAA PLA Pro" are two products with two ids, not variants of one. The
    id is a pure function of the product triple (below), so WHERE a preset gets
    it from is irrelevant: it may declare the key itself or inherit it from any
    ancestor — a root preset, a real (instantiated) filament, an
    OrcaFilamentLibrary (OFL) preset — as long as the id it ends up with is the
    mint of its OWN triple. Inheritance carries settings, never identity; the
    key is bundle-independent, so moving a filament into OFL never changes it.
  * Ids are content-addressed by the product triple, resolved from the preset's
    flattened config (filament_vendor and filament_type are inheritable list
    options — first element; filament name = preset base name):
        filament_id = "OF" + base62_6( uuid5(FILAMENT_ID_NAMESPACE,
            "filament_product/<filament_vendor>/<filament_type>/<filament_name>") )
    8 chars total, which satisfies the AMS length limit. Nobody invents ids by
    hand, and nothing but the triple feeds the mint — not the rest of the tree,
    not the snapshot. Two products whose triples mint one id (a base62
    collision; odds ~1e-5 over the whole tree) is an error --check reports and
    --generate refuses to write; the remedy is a rename so the triples differ,
    never a salted or hand-picked second id.
    Identity changes (a filament rename, a filament_vendor/filament_type fix)
    change the id BY DESIGN.
  * Reserved id spaces that are never minted into or altered:
      - GF*                    Bambu AMS/RFID catalog: frozen, no preset of any
                               vendor (including BBL) may declare one; the
                               generated resources/printers/bambu_filament_ids.json
                               carries the correspondence instead
      - QD_*                   Qidi device protocol: the box composes these ids
                               at runtime, they are not preset ids, and no
                               preset may declare one
      - P + 7 hex chars (case-insensitive) and the literal "null"
                               user-custom presets (CreatePresetsDialog.cpp)
  * scripts/filament_id_snapshot.json is the sanctioned-state snapshot: one
    entry per id, carrying the product triple it is minted from and the
    "Vendor/Filament" presets claiming it. It must exactly equal the tree-derived
    state at all times, so any id/claim/triple change shows up as a reviewable
    diff to that file (the maintainer gate). It sanctions state, never
    exceptions: no check consults it to excuse a preset from the rules above.

setting_id policy (see AGENTS.md "Critical Constraints"):
  * setting_id is a PRESET id, a pure function of the preset's identity:
        setting_id = base62_16( uuid5(NAMESPACE, "<vendor>/<type>/<name>") )
    The same value is recomputed on the fly by the C++ app
    (Slic3r::generate_preset_setting_id); the two MUST stay byte-identical, and
    the validator (orca_extra_profile_check.py) imports the rule from here.
    Uniqueness is therefore automatic: two presets collide only if they share
    vendor + type + name, which the validator flags.
  * Only instantiated presets (instantiation == "true") carry a setting_id;
    base / template profiles do not.
  * Bambu (BBL) owns the authoritative "G*" setting_id space and is the only
    reserved vendor: its setting_ids are never rewritten, which keeps
    Bambu-synced presets backward compatible. filament_id has no such exemption
    — the GF* catalog space is frozen and ownerless, so BBL's filament_ids are
    minted like every other vendor's.

The effective-id resolution below is loader-faithful (PresetBundle.cpp
load_vendor_configs_from_json): own filament_id key, else walk `inherits` within
the vendor map, with OrcaFilamentLibrary base-bundle fallback; once a chain enters
OFL it stays in OFL; a vendor chain that dead-ends id-less retries its direct
parent in the OFL map. filament_vendor / filament_type resolve the same way.

Run from anywhere:
  python scripts/orca_id_tool.py --generate         write the ids every profile should carry
  python scripts/orca_id_tool.py --dry-run          preview that; writes nothing
  python scripts/orca_id_tool.py --check            validate filament_id state (what CI runs)
  python scripts/orca_id_tool.py --update-snapshot  re-record the sanctioned filament_id state
Narrow --generate with --filament-id / --setting-id and --vendor VENDOR (repeatable).
"""

import argparse
import json
import os
import re
import sys
import uuid

# The id namespace baked into both Python and C++ (Slic3r::generate_preset_setting_id).
# Dedicated, distinct from the cloud namespace (f47ac10b-...) so the two id spaces never
# coincide; it is the root of BOTH id rules below — never change it.
NAMESPACE = uuid.UUID("c1f4d9e2-7a3b-5c8d-9e0f-1a2b3c4d5e6f")
ALPHABET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
SETTING_ID_LENGTH = 16

# Dedicated namespace for filament_id, derived from the setting_id namespace above.
# Never change it.
# FILAMENT_ID_NAMESPACE == UUID("c4d3ff49-4c32-5534-a3e3-00894157ab97")
FILAMENT_ID_NAMESPACE = uuid.uuid5(NAMESPACE, "filament_id")
FILAMENT_ID_LENGTH = 6  # base62 digits after the "OF" prefix -> 8 chars total

SCRIPTS_DIR = os.path.dirname(os.path.abspath(__file__))
PROFILES_DIR = os.path.normpath(os.path.join(SCRIPTS_DIR, "..", "resources", "profiles"))
SNAPSHOT_PATH = os.path.join(SCRIPTS_DIR, "filament_id_snapshot.json")
# The single source of truth for the map path; update_bambu_filament_ids.py
# imports this rather than recomputing it.
BAMBU_MAP_PATH = os.path.normpath(
    os.path.join(SCRIPTS_DIR, "..", "resources", "printers", "bambu_filament_ids.json"))

OFL = "OrcaFilamentLibrary"

# Bambu (BBL) is the only vendor exempt from the setting_id rule: it keeps its
# authoritative "G*" cloud ids. No vendor is exempt from the filament_id rule.
RESERVED_VENDORS = {"BBL"}

# The profile types that carry a setting_id; the subdir name is also the type
# name, matching Preset::get_type_string() on the C++ side.
PROFILE_SUBDIRS = ("filament", "process", "machine")

OF_ID_RE = re.compile(r"^OF[0-9A-Za-z]{6}$")
# User-custom id space minted by CreatePresetsDialog.cpp ("P" + md5(name)[0:7]);
# reserved case-insensitively, together with its "null" sentinel.
USER_CUSTOM_ID_RE = re.compile(r"^P[0-9A-Fa-f]{7}$", re.IGNORECASE)
# Filament name = preset base name: strip the first "@..." suffix. The space before
# "@" is optional because names like "Afinia PLA@HS" exist.
BASE_NAME_RE = re.compile(r"\s?@.*$")
# A JSON string literal, for the byte-preserving key edits.
_JSON_STR = r'"(?:[^"\\]|\\.)*"'

GENERATE_CMD = "python scripts/orca_id_tool.py --generate"
UPDATE_HINT = 'run "python scripts/orca_id_tool.py --update-snapshot" and commit the diff for maintainer review'
BAMBU_MAP_HINT = 'regenerate the map with "python scripts/update_bambu_filament_ids.py" and commit the diff for maintainer review'


# Same output helpers/format as orca_extra_profile_check.py (not imported from
# there to avoid a circular import: that script imports check_filament_ids).
def print_error(msg):
    print(f"\033[91m[ERROR]\033[0m {msg}")  # Red

def print_warning(msg):
    print(f"\033[93m[WARNING]\033[0m {msg}")  # Yellow

def print_info(msg):
    print(f"\033[94m[INFO]\033[0m {msg}")  # Blue

def print_success(msg):
    print(f"\033[92m[SUCCESS]\033[0m {msg}")  # Green


def _utf8_console():
    """Make stdout/stderr survive non-ASCII profile names on cp1252 consoles."""
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            try:
                stream.reconfigure(encoding="utf-8", errors="replace")
            except (ValueError, OSError):
                pass


# ---------------------------------------------------------------------------
# Minting
# ---------------------------------------------------------------------------

def _base62_tail(n, length):
    """The low `length` base62 digits of n, most-significant first.

    The shared tail of both id rules. Its output bytes are pinned by the C++
    golden vectors (tests/libslic3r/test_preset_setting_id.cpp) and by the
    filament_id snapshot — never change it.
    """
    digits = []
    for _ in range(length):
        digits.append(ALPHABET[n % 62])
        n //= 62
    return "".join(reversed(digits))


def generate_preset_setting_id(vendor, type_name, name):
    """Deterministic 16-char base62 setting_id for a preset.

    input = f"{vendor}/{type_name}/{name}"; u = uuid5(NAMESPACE, input);
    id = the low SETTING_ID_LENGTH base62 digits of int(u.bytes, "big"),
    most-significant first. Kept byte-identical to the C++
    Slic3r::generate_preset_setting_id.
    """
    u = uuid.uuid5(NAMESPACE, f"{vendor}/{type_name}/{name}")
    return _base62_tail(int.from_bytes(u.bytes, "big"), SETTING_ID_LENGTH)


def base_name(name):
    """Filament name of a preset: name with the first "@..." suffix stripped."""
    return BASE_NAME_RE.sub("", name, count=1)


def generate_filament_id(filament_vendor, filament_type, filament_name):
    """Deterministic "OF" + 6-char base62 filament_id for a filament product.

    The triple is the only input: no salt, no state, no second value.
    input = "filament_product/<filament_vendor>/<filament_type>/<filament_name>";
    u = uuid5(FILAMENT_ID_NAMESPACE, input); the id tail is the low
    FILAMENT_ID_LENGTH base62 digits of int(u.bytes, "big"), most-significant
    first — the same derivation as generate_preset_setting_id.
    """
    key = f"filament_product/{filament_vendor}/{filament_type}/{filament_name}"
    u = uuid.uuid5(FILAMENT_ID_NAMESPACE, key)
    return "OF" + _base62_tail(int.from_bytes(u.bytes, "big"), FILAMENT_ID_LENGTH)


# ---------------------------------------------------------------------------
# Tree loading + loader-faithful effective-id resolution
# ---------------------------------------------------------------------------

def load_json(path):
    with open(path, "r", encoding="utf-8-sig") as f:
        return json.load(f)


def list_vendor_names(profiles_dir):
    """Vendor bundles = subdirectories with a matching <name>.json index file.

    (Ignores stray non-bundle entries such as the tracked "user" directory,
    which has no user.json index.)
    """
    profiles_dir = str(profiles_dir)
    return sorted(
        os.path.splitext(f)[0] for f in os.listdir(profiles_dir)
        if f.endswith(".json")
        and os.path.isdir(os.path.join(profiles_dir, os.path.splitext(f)[0]))
    )


def list_profile_dirs(profiles_dir):
    """Every vendor directory under the tree, index or not.

    What the setting_id pass walks, and what orca_extra_profile_check.py walks:
    setting_id is a per-file property, so a bundle whose index has not landed yet
    must still be assignable — otherwise the validator flags files the tool
    refuses to touch. (filament_id is driven by each bundle's filament_list
    instead, hence list_vendor_names above.)
    """
    profiles_dir = str(profiles_dir)
    return sorted(d for d in os.listdir(profiles_dir)
                  if os.path.isdir(os.path.join(profiles_dir, d)))


def iter_profile_files(vendor_dir):
    """Yield (json path, type) under a vendor bundle, in a deterministic order."""
    for sub in PROFILE_SUBDIRS:
        base = os.path.join(vendor_dir, sub)
        if not os.path.isdir(base):
            continue
        for root, dirs, files in os.walk(base):
            dirs.sort()  # deterministic traversal across filesystems
            for name in sorted(files):
                if name.endswith(".json"):
                    yield os.path.join(root, name), sub


def load_vendor_filaments(profiles_dir, vendor):
    """Load a vendor's filament presets from its index's filament_list.

    Returns (presets dict name -> record, list of unreadable-file messages).
    """
    profiles_dir = str(profiles_dir)
    presets = {}
    errors = []
    try:
        idx = load_json(os.path.join(profiles_dir, vendor + ".json"))
    except (OSError, ValueError) as e:
        return presets, [f"unreadable vendor index {vendor}.json: {e}"]
    for entry in idx.get("filament_list", []):
        rel = f"{vendor}/{entry.get('sub_path', '')}"
        path = os.path.join(profiles_dir, vendor, entry.get("sub_path", ""))
        try:
            data = load_json(path)
        except (OSError, ValueError) as e:
            errors.append(f"unreadable filament profile {rel}: {e}")
            continue
        name = data.get("name", entry.get("name"))
        presets[name] = {
            "name": name,
            "file": rel,
            "path": path,
            "filament_id": data.get("filament_id"),
            "inherits": data.get("inherits"),
            "instantiation": str(data.get("instantiation", "")).lower() == "true",
            "compatible_printers": data.get("compatible_printers") or [],
            "filament_vendor": data.get("filament_vendor"),
            "filament_type": data.get("filament_type"),
            "renamed_from": data.get("renamed_from"),
        }
    return presets, errors


def resolve_filament_id(name, filaments, ofl_filaments, seen=None, in_ofl=False):
    """Walk the inherits chain for the effective filament_id, loader-faithfully.

    Mirrors PresetBundle.cpp load_vendor_configs_from_json: a hop resolves in the
    vendor's own map first, then falls back to the OFL base-bundle map. OFL's map
    was memoized entirely within OFL, so once a chain enters OFL it stays in OFL
    (a vendor file sharing an OFL preset's name must not shadow OFL-internal
    hops). Additionally, a vendor preset that never resolves an id inside the
    vendor is re-tried against the OFL map keyed by its direct parent name.

    Returns (filament_id or None, source, ofl_entry) where source is one of
    "own"/"inherited"/"missing"/"dangling"/"cycle" and ofl_entry is the name of
    the OFL preset through which a vendor chain entered OFL (None when the id was
    declared vendor-side or resolution started inside OFL).
    """
    if seen is None:
        seen = set()
    if name in seen:
        return None, "cycle", None
    seen.add(name)
    entry = None
    if in_ofl:
        rec = ofl_filaments.get(name)
    else:
        rec = filaments.get(name)
        if rec is None and name in ofl_filaments:
            rec, in_ofl, entry = ofl_filaments[name], True, name
    if rec is None:
        return None, "dangling", None
    if rec.get("filament_id"):
        return rec["filament_id"], "own" if len(seen) == 1 else "inherited", entry
    parent = rec.get("inherits")
    if parent:
        fid, src, sub_entry = resolve_filament_id(parent, filaments, ofl_filaments, seen, in_ofl)
        if fid or in_ofl:
            return fid, src, entry if entry is not None else sub_entry
        # Vendor chain dead-ended id-less: the loader would have consulted the
        # OFL map at each vendor hop's inherits; retry this hop's parent in OFL.
        if parent in ofl_filaments:
            fid, src, _ = resolve_filament_id(parent, filaments, ofl_filaments, set(), True)
            return fid, src, parent
        return fid, src, sub_entry
    return None, "missing", entry


def resolve_filament_field(name, field, filaments, ofl_filaments, seen=None, in_ofl=False):
    """Resolve an inheritable list option (filament_vendor / filament_type) with
    the same hop semantics as resolve_filament_id: own value, else walk
    `inherits` in the vendor map with OFL base-bundle fallback. Values are list
    options — the first element counts; "" when the chain never defines one.
    """
    if seen is None:
        seen = set()
    if name in seen:
        return ""
    seen.add(name)
    if in_ofl:
        rec = ofl_filaments.get(name)
    else:
        rec = filaments.get(name)
        if rec is None and name in ofl_filaments:
            rec, in_ofl = ofl_filaments[name], True
    if rec is None:
        return ""
    value = rec.get(field)
    if isinstance(value, str):
        value = [value]
    if value and value[0]:
        return value[0]
    parent = rec.get("inherits")
    if parent:
        found = resolve_filament_field(parent, field, filaments, ofl_filaments, seen, in_ofl)
        if found or in_ofl:
            return found
        if parent in ofl_filaments:
            return resolve_filament_field(parent, field, filaments, ofl_filaments, set(), True)
        return found
    return ""


def resolve_triple(name, filaments, ofl_filaments):
    """The preset's mint-key triple (filament_vendor, filament_type, filament name)."""
    return (resolve_filament_field(name, "filament_vendor", filaments, ofl_filaments),
            resolve_filament_field(name, "filament_type", filaments, ofl_filaments),
            base_name(name))


def analyze_tree(profiles_dir):
    """Load every vendor bundle and derive the full filament_id state.

    Returns a dict with the tree-derived snapshot sections plus the working data
    the checks and the assign pass need. All claims are "Vendor/Filament" strings
    over INSTANTIATED system filaments, tree-wide including OFL and BBL.
    """
    profiles_dir = str(profiles_dir)
    vendor_names = list_vendor_names(profiles_dir)
    ofl_filaments, ofl_errors = (
        load_vendor_filaments(profiles_dir, OFL) if OFL in vendor_names else ({}, [])
    )

    vendors = {}
    read_errors = list(ofl_errors)
    for vendor in vendor_names:
        if vendor == OFL:
            filaments = ofl_filaments
        else:
            filaments, errs = load_vendor_filaments(profiles_dir, vendor)
            read_errors.extend(errs)
        for rec in filaments.values():
            eff, src, _entry = resolve_filament_id(rec["name"], filaments, ofl_filaments)
            rec["eff_filament_id"] = eff
            rec["id_source"] = src
        vendors[vendor] = filaments

    # id -> set of "Vendor/Filament" claims over instantiated presets. Every id
    # occurring in the tree is a key; ids only ever DECLARED (e.g. on a root
    # none of whose descendants instantiate) keep an empty claim list, so that
    # the snapshot exactly equals the tree-derived state.
    ids = {}
    vendor_ids = {}             # vendor -> set of ids occurring there (declared or effective)
    declared_ids = {}           # vendor -> set of ids DECLARED in that vendor's own files
    missing_effective = []      # (vendor, name, file) instantiated presets resolving no id
    inherited = []              # (vendor, rec, eff, triple) instantiated presets inheriting an OF id
    triples = {}                # fid -> set of triples of its declarers
    declarer_triples = []       # (vendor, rec, fid, triple) per declarer
    filament_triples = {}       # (vendor, filament_name) -> {triple: [declarers]}
    mints = {}                  # minted id -> triples minting it (declarers + instantiated)

    for vendor, filaments in vendors.items():
        occurring = vendor_ids.setdefault(vendor, set())
        for rec in filaments.values():
            triple = resolve_triple(rec["name"], filaments, ofl_filaments)
            rec["triple"] = triple
            if rec.get("filament_id") or rec["instantiation"]:
                mints.setdefault(generate_filament_id(*triple), set()).add(triple)
            if rec.get("filament_id"):
                fid = rec["filament_id"]
                occurring.add(fid)
                declared_ids.setdefault(vendor, set()).add(fid)
                ids.setdefault(fid, set())
                declarer_triples.append((vendor, rec, fid, triple))
                triples.setdefault(fid, set()).add(triple)
                filament_triples.setdefault(
                    (vendor, base_name(rec["name"])), {}).setdefault(
                    triple, []).append(rec["name"])
            if not rec["instantiation"]:
                continue
            eff = rec.get("eff_filament_id")
            if not eff:
                missing_effective.append((vendor, rec["name"], rec["file"]))
                continue
            occurring.add(eff)
            ids.setdefault(eff, set()).add(f"{vendor}/{base_name(rec['name'])}")
            if not rec.get("filament_id") and OF_ID_RE.match(eff):
                inherited.append((vendor, rec, eff, triple))

    # Cross-bundle triple divergence (check 5, warning only): the same filament
    # name declared in several bundles with different triples cannot converge
    # on one id until the divergence is fixed.
    name_bundles = {}
    for (vendor, filament_name), tmap in filament_triples.items():
        name_bundles.setdefault(filament_name, {})[vendor] = frozenset(tmap)
    cross_bundle_triples = [
        (filament_name, {v: sorted(ts) for v, ts in per_vendor.items()})
        for filament_name, per_vendor in sorted(name_bundles.items())
        if len(per_vendor) > 1 and len(set(per_vendor.values())) > 1
    ]

    return {
        "vendors": vendors,
        "read_errors": read_errors,
        "ids": {fid: sorted(claims) for fid, claims in ids.items()},
        "vendor_ids": vendor_ids,
        "declared_ids": declared_ids,
        "missing_effective": sorted(missing_effective),
        "inherited": inherited,
        "triples": {fid: sorted(list(t) for t in ts) for fid, ts in triples.items()},
        "declarer_triples": declarer_triples,
        "filament_triples": filament_triples,
        "cross_bundle_triples": cross_bundle_triples,
        # id -> the products (triples) minting it, where there is more than one
        "collisions": {fid: sorted(ts) for fid, ts in mints.items() if len(ts) > 1},
    }


# ---------------------------------------------------------------------------
# Snapshot IO
# ---------------------------------------------------------------------------

def snapshot_from_analysis(analysis):
    """One entry per id, in id order: the product triple it is minted from and
    the "Vendor/Filament" claims on it. Requires exactly one declared triple per
    id (update_snapshot refuses any other state; check 3 rejects it anyway)."""
    ids = {}
    for fid, claims in sorted(analysis["ids"].items()):
        [(vendor, ftype, filament_name)] = analysis["triples"][fid]
        ids[fid] = {"filaments": sorted(claims), "name": filament_name,
                    "filament_type": ftype, "filament_vendor": vendor}
    return {"ids": ids}


def snapshot_triple(entry):
    return [entry["filament_vendor"], entry["filament_type"], entry["name"]]


def load_snapshot(path):
    """Return the snapshot dict, or None when the file does not exist."""
    if not os.path.exists(path):
        return None
    data = load_json(path)
    data.setdefault("ids", {})
    return data


def write_snapshot(path, obj):
    """Deterministic serialization: snapshot_from_analysis order, indent 1, LF,
    trailing newline."""
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(obj, f, indent=1, ensure_ascii=False)
        f.write("\n")


# ---------------------------------------------------------------------------
# Reserved namespaces
# ---------------------------------------------------------------------------

def reserved_space_owner(fid):
    """(is_reserved, owner_vendor or None) for the frozen id spaces."""
    if fid.startswith("GF"):
        return True, None  # Bambu AMS/RFID catalog: frozen, no vendor (not even BBL) may declare it
    if fid.startswith("QD_"):
        return True, None  # dissolved Qidi device-protocol space: NO vendor may declare it
    if USER_CUSTOM_ID_RE.match(fid) or fid == "null":
        return True, None  # user-custom space: no system vendor may own it
    return False, None


def reserved_space_desc(fid, owner):
    """Human description of a reserved space for error messages."""
    if owner:
        return f"owned by {owner}"
    if fid.startswith("GF"):
        return "Bambu AMS/RFID catalog; frozen, no preset may declare it"
    if fid.startswith("QD_"):
        return "Qidi device protocol; composed by the device, never a preset id"
    return "reserved for user-custom presets"


# ---------------------------------------------------------------------------
# Checks (imported and called tree-wide by orca_extra_profile_check.py)
# ---------------------------------------------------------------------------

def check_filament_ids(profiles_dir=PROFILES_DIR, snapshot_path=SNAPSHOT_PATH,
                       map_path=BAMBU_MAP_PATH):
    """Validate filament_id state across every vendor. Returns the error count.

    1. Format: every id occurring in the tree (declared or effective) must
       match ^OF[0-9A-Za-z]{6}$. No exceptions: not the snapshot, not BBL.
    2. Snapshot equality, both directions: every id in the tree, the filaments
       claiming it and the triple its declarers resolve must equal the snapshot
       entry exactly (the snapshot diff is the maintainer gate).
    3. Identity: the id is a function of the triple alone, and there is no
       second acceptable value. (a) A declared id must equal the one id the
       declarer's own triple mints; (b) the id an instantiated preset inherits
       must equal the one ITS own triple mints — how it inherits it (a root, a
       real filament, an OFL preset) is irrelevant; (c) every instantiated
       filament resolves an effective id at all (an id-less one is a hard load
       error in C++); (d) no two products mint one id (a base62 collision,
       resolved by renaming one of them).
    4. Reserved namespaces (GF*/QD_*/P-hex/"null", all ownerless) must not be
       claimed by any vendor.
    5. Triple integrity: (a) every declarer resolves non-empty filament_vendor
       and filament_type; (b) declarers of one (bundle, filament) resolve
       identical triples; cross-bundle divergence on the same filament name is a
       warning only.
    6. Bambu catalog map: resources/printers/bambu_filament_ids.json must parse,
       carry source/bambustudio_commit/generated, key only OF-format ids, map
       each Bambu id at most once, and for every row whose key the tree claims,
       the tree's triple for that id must equal the row's (vendor, type, name).

    Nothing is grandfathered: the snapshot sanctions state, never exceptions.
    """
    _utf8_console()
    errors = 0
    analysis = analyze_tree(profiles_dir)
    snapshot = load_snapshot(snapshot_path)
    if snapshot is None:
        print_error(f"filament_id snapshot not found at {snapshot_path}; {UPDATE_HINT}")
        return 1
    for msg in analysis["read_errors"]:
        print_error(msg)
        errors += 1

    snap_ids = snapshot["ids"]
    tree_ids = analysis["ids"]

    # -- 1. format ----------------------------------------------------------
    for vendor in sorted(analysis["vendor_ids"]):
        for fid in sorted(analysis["vendor_ids"][vendor]):
            if OF_ID_RE.match(fid):
                continue
            print_error(
                f'filament_id "{fid}" ({vendor}) is not a minted "OF" id; new '
                f'filament ids must come from "{GENERATE_CMD}"')
            errors += 1

    # -- 2. snapshot equality (both directions) -----------------------------
    tree_triples = analysis["triples"]
    for fid in sorted(tree_ids):
        entry = snap_ids.get(fid)
        if entry is None:
            print_error(
                f'filament_id "{fid}" is not sanctioned by '
                f"scripts/filament_id_snapshot.json; {UPDATE_HINT}")
            errors += 1
            continue
        for claim in tree_ids[fid]:
            if claim not in entry["filaments"]:
                print_error(
                    f'filament_id "{fid}" claim "{claim}" is not sanctioned by '
                    f"scripts/filament_id_snapshot.json; {UPDATE_HINT}")
                errors += 1
        # Every tree id has at least one declarer; the snapshot records one
        # triple per id, so a divergent declarer is a mismatch in both directions.
        sanctioned = snapshot_triple(entry)
        for t in tree_triples[fid]:
            if t != sanctioned:
                print_error(
                    f'filament_id "{fid}" triple "{"/".join(t)}" is not sanctioned by '
                    f'scripts/filament_id_snapshot.json, which records '
                    f'"{"/".join(sanctioned)}"; {UPDATE_HINT}')
                errors += 1
    for fid in sorted(snap_ids):
        if fid not in tree_ids:
            print_error(
                f'filament_id stability: snapshot id "{fid}" vanished from the tree; '
                f"{UPDATE_HINT}")
            errors += 1
            continue
        for claim in snap_ids[fid]["filaments"]:
            if claim not in tree_ids[fid]:
                print_error(
                    f'filament_id stability: snapshot claim "{claim}" of id "{fid}" '
                    f"vanished from the tree; {UPDATE_HINT}")
                errors += 1

    # -- 3. identity: the id is a function of the triple alone ---------------
    # One triple, one id: a declaration must carry exactly the mint of its
    # triple, and there is no second acceptable value — not a salt, not a
    # hand-picked one, not whatever another preset of the product carries. Two
    # presets of one product that would be AMS-ambiguous on a printer are fixed
    # in the profiles, by making their compatible_printers disjoint or by
    # retiring the redundant one.
    for vendor, rec, fid, triple in sorted(
            analysis["declarer_triples"], key=lambda x: (x[0], x[1]["file"])):
        want = generate_filament_id(*triple)
        if not OF_ID_RE.match(fid) or fid == want:
            continue  # a non-OF id is check 1's error
        print_error(
            f'filament_id "{fid}" declared by "{rec["name"]}" ({rec["file"]}) does '
            f'not match the mint of its triple "{"/".join(triple)}": expected '
            f'"{want}"; paste the expected id, or fix the triple and run '
            f'"{GENERATE_CMD} --vendor {vendor}" (preview with --dry-run), then '
            f"--update-snapshot")
        errors += 1
    # (3b) An inherited id is held to the same single value, and every preset
    # missing it is listed — a variant under a wrong root as much as a preset
    # riding another product's root. Nothing is folded into the declarer's
    # error: the report names each preset whose id is wrong.
    for vendor, rec, eff, triple in sorted(
            analysis["inherited"], key=lambda x: (x[0], x[1]["file"])):
        want = generate_filament_id(*triple)
        if eff == want:
            continue
        print_error(
            f'preset "{rec["name"]}" ({rec["file"]}) inherits filament_id "{eff}" but '
            f'its own triple "{"/".join(triple)}" mints "{want}"; '
            f"a preset carries the id of its own product: inherit a preset of the "
            f"same filament, or declare its own key")
        errors += 1
    ofl_map = analysis["vendors"].get(OFL, {})
    for vendor, name, file in analysis["missing_effective"]:
        triple = resolve_triple(name, analysis["vendors"][vendor], ofl_map)
        expected = generate_filament_id(*triple)
        print_error(
            f'instantiated filament "{name}" ({file}) resolves no filament_id anywhere '
            f"in its inherits chain — this is a hard load error in the C++ loader; "
            f'run "{GENERATE_CMD}" (expected id for filament '
            f'"{vendor}/{base_name(name)}": "{expected}")')
        errors += 1
    # (3d) The mint is injective over the tree's products, or two of them are
    # indistinguishable to every device that matches on the id.
    for fid, ts in sorted(analysis["collisions"].items()):
        print_error(
            f'filament_id "{fid}" is the mint of {len(ts)} different products '
            f'({"; ".join("/".join(t) for t in ts)}): a base62 collision; rename one '
            f"of them so their triples differ")
        errors += 1

    # -- 4. reserved namespaces ----------------------------------------------
    for fid in sorted(tree_ids):
        is_reserved, owner = reserved_space_owner(fid)
        if not is_reserved:
            continue
        for claim in tree_ids[fid]:
            vendor = claim.split("/", 1)[0]
            if vendor == owner:
                continue
            space = reserved_space_desc(fid, owner)
            print_error(
                f'filament_id "{fid}" of "{claim}" is in a reserved id space '
                f"({space}) and must not be claimed by system presets of other vendors")
            errors += 1

    # -- 5. triple integrity ---------------------------------------------------
    for vendor, rec, fid, triple in sorted(
            analysis["declarer_triples"], key=lambda x: (x[0], x[1]["file"])):
        if triple[0] and triple[1]:
            continue
        missing = " and ".join(
            k for k, v in (("filament_vendor", triple[0]),
                           ("filament_type", triple[1])) if not v)
        print_error(
            f'preset "{rec["name"]}" ({rec["file"]}) declares filament_id "{fid}" but '
            f"resolves empty {missing}; the mint key needs both (generic materials "
            f'use filament_vendor "Generic")')
        errors += 1
    for (vendor, filament_name), tmap in sorted(analysis["filament_triples"].items()):
        if len(tmap) < 2:
            continue
        detail = "; ".join(
            f'"{"/".join(t)}" ({", ".join(sorted(names))})'
            for t, names in sorted(tmap.items()))
        print_error(
            f'filament "{vendor}/{filament_name}" declarers resolve divergent triples: '
            f"{detail}; declarers of one filament must agree on "
            f"(filament_vendor, filament_type)")
        errors += 1
    for filament_name, per_vendor in analysis["cross_bundle_triples"]:
        detail = "; ".join(
            f'{v}: {", ".join("/".join(t) for t in ts)}'
            for v, ts in sorted(per_vendor.items()))
        print_warning(
            f'filament name "{filament_name}" resolves different triples across bundles '
            f"({detail}); bundles of one product converge on one id only once "
            f"their triples agree")

    # -- 6. Bambu catalog map --------------------------------------------------
    try:
        bambu_map = load_json(map_path)
        if not isinstance(bambu_map, dict):
            raise ValueError("top level is not a JSON object")
    except (OSError, ValueError) as e:
        print_error(f"Bambu catalog map {map_path} does not parse ({e}); {BAMBU_MAP_HINT}")
        errors += 1
    else:
        for key in ("source", "bambustudio_commit", "generated"):
            if not bambu_map.get(key):
                print_error(f'Bambu catalog map {map_path} is missing "{key}"; {BAMBU_MAP_HINT}')
                errors += 1
        rows = bambu_map.get("filaments")
        # An empty or absent section is not a well-formed map: it makes every runtime
        # translation silently degrade to identity (BBLPrinterAgent logs nothing for it),
        # and it is what a regeneration against the wrong --bambustudio-dir writes.
        if not isinstance(rows, dict) or not rows:
            print_error(f'Bambu catalog map {map_path} declares no "filaments" rows; '
                        f"{BAMBU_MAP_HINT}")
            errors += 1
            rows = {}
        bambu_id_owners = {}
        for fid, row in sorted(rows.items()):
            if not OF_ID_RE.match(fid):
                print_error(f'Bambu catalog map key "{fid}" is not a minted "OF" id; '
                           f"{BAMBU_MAP_HINT}")
                errors += 1
            bambu_id = row.get("bambu_id")
            if not bambu_id:
                # An empty id would map the empty string to a real filament at runtime.
                print_error(f'Bambu catalog map row "{fid}" declares no "bambu_id"; '
                            f"{BAMBU_MAP_HINT}")
                errors += 1
            elif bambu_id in bambu_id_owners:
                print_error(
                    f'Bambu catalog map: Bambu id "{bambu_id}" is mapped by both '
                    f'"{bambu_id_owners[bambu_id]}" and "{fid}"; {BAMBU_MAP_HINT}')
                errors += 1
            else:
                bambu_id_owners[bambu_id] = fid
            claimed = tree_triples.get(fid)
            if not claimed:
                continue  # a product BambuStudio ships that the tree does not (yet)
            row_triple = [row.get("vendor", ""), row.get("type", ""), row.get("name", "")]
            if row_triple not in claimed:
                print_error(
                    f'Bambu catalog map row "{fid}" claims triple "{"/".join(row_triple)}" '
                    f'but the tree declares "{"; ".join("/".join(t) for t in claimed)}" for '
                    f"that id; {BAMBU_MAP_HINT}")
                errors += 1

    return errors


# ---------------------------------------------------------------------------
# --update-snapshot
# ---------------------------------------------------------------------------

def update_snapshot(profiles_dir=PROFILES_DIR, snapshot_path=SNAPSHOT_PATH, dry_run=False):
    """Regenerate the snapshot from the tree.

    Refuses to sanction a tree it could not read whole, a reserved-namespace id
    (or a claim on one) and an id declared under more than one triple: none of
    them can ever pass --check, so writing them into the snapshot would only
    hide the mistake until CI.
    Idempotent: a second run over an unchanged tree changes nothing. Returns 0
    on success.
    """
    analysis = analyze_tree(profiles_dir)
    # A tree that could not be read whole cannot be sanctioned: the snapshot
    # would silently drop the unreadable bundle's ids and claims, and the diff
    # would read as a deliberate removal.
    refusals = len(analysis["read_errors"])
    for msg in analysis["read_errors"]:
        print_error(msg)

    for vendor in sorted(analysis["vendor_ids"]):
        for fid in sorted(analysis["vendor_ids"][vendor]):
            is_reserved, owner = reserved_space_owner(fid)
            if is_reserved and vendor != owner:
                print_error(
                    f'refusing to sanction filament_id "{fid}" ({vendor}): reserved id '
                    f"space, {reserved_space_desc(fid, owner)}")
                refusals += 1
    for fid, ts in sorted(analysis["triples"].items()):
        if len(ts) > 1:
            print_error(
                f'refusing to sanction filament_id "{fid}": declared under {len(ts)} '
                f'triples ({"; ".join("/".join(t) for t in ts)}); one id names one '
                f"product (check 3)")
            refusals += 1
    if refusals:
        return 1

    new_snap = snapshot_from_analysis(analysis)
    old_snap = load_snapshot(snapshot_path)
    old_ids = old_snap["ids"] if old_snap else {}

    # Diff summary.
    added_ids = sorted(set(new_snap["ids"]) - set(old_ids))
    removed_ids = sorted(set(old_ids) - set(new_snap["ids"]))
    added_claims = sum(
        len(set(entry["filaments"]) - set(old_ids.get(fid, {}).get("filaments", [])))
        for fid, entry in new_snap["ids"].items())
    removed_claims = sum(
        len(set(entry["filaments"]) - set(new_snap["ids"].get(fid, {}).get("filaments", [])))
        for fid, entry in old_ids.items())
    changed = new_snap != (old_snap or {"ids": {}})

    if changed and not dry_run:
        write_snapshot(snapshot_path, new_snap)

    print_info(f"snapshot ids      : {len(new_snap['ids'])} (+{len(added_ids)} / -{len(removed_ids)})")
    print_info(f"claims added      : {added_claims}")
    print_info(f"claims removed    : {removed_claims}")
    if changed and dry_run:
        print_success(f"dry run: {snapshot_path} would be rewritten; nothing written")
    elif changed:
        print_success(f"snapshot written to {snapshot_path}")
    else:
        print_success("snapshot already up to date; nothing changed")
    return 0


# ---------------------------------------------------------------------------
# Byte-preserving profile edits
# ---------------------------------------------------------------------------
# Binary IO throughout: a profile keeps its original line endings (LF or CRLF),
# its BOM and its exact formatting apart from the one line being touched.

def insert_key_line(text, key, value, before=(), after=()):
    """Insert a `"key": value` line into a preset that lacks one.

    Placed just before the first `before` anchor the file has (matching the
    canonical key order), else just after the first `after` anchor, reusing that
    anchor line's indentation and line ending. Returns (text, insertions made).
    """
    def line(m):
        return (f'{m.group(1)}"{key}": {json.dumps(value, ensure_ascii=False)},'
                f'{m.group(2)}')

    for anchors, at_start in ((before, True), (after, False)):
        for anchor in anchors:
            m = re.search(r'^([ \t]*)"' + re.escape(anchor) + r'"[ \t]*:.*?(\r?\n)',
                          text, re.MULTILINE)
            if m:
                cut = m.start() if at_start else m.end()
                return text[:cut] + line(m) + text[cut:], 1
    return text, 0


def replace_key_value(text, key, new_value, old_value=None):
    """Swap the JSON string VALUE on the `"key"` line, byte-preserving the rest.

    When old_value is given the line must carry exactly that value, so a stale
    rewrite fails loudly instead of clobbering an unexpected id.
    Returns (text, replacements made).
    """
    value = (re.escape(json.dumps(old_value, ensure_ascii=False))
             if old_value is not None else _JSON_STR)
    pattern = re.compile(
        r'(^[ \t]*"' + re.escape(key) + r'"[ \t]*:[ \t]*)' + value, re.MULTILINE)
    return pattern.subn(
        lambda m: m.group(1) + json.dumps(new_value, ensure_ascii=False), text, count=1)


def delete_key_line(text, key, old_value=None):
    """Delete the `"key"` line, byte-preserving the rest.

    Handles both the canonical layout (trailing comma) and a last-property
    layout (comma on the preceding line, consumed so no dangling comma is left).
    Returns (text, deletions made).
    """
    value = (re.escape(json.dumps(old_value, ensure_ascii=False))
             if old_value is not None else _JSON_STR)
    member = r'"' + re.escape(key) + r'"[ \t]*:[ \t]*' + value
    m = re.search(r'^[ \t]*' + member + r'[ \t]*,[ \t]*\r?\n', text, re.MULTILINE)
    if m:
        return text[:m.start()] + text[m.end():], 1
    m = re.search(r',[ \t]*\r?\n[ \t]*' + member + r'[ \t]*(?=\r?\n)', text)
    if m:
        return text[:m.start()] + text[m.end():], 1
    return text, 0


def insert_filament_id(text, new_id):
    """Insert a `"filament_id"` line before `instantiation`, else after `name`."""
    return insert_key_line(text, "filament_id", new_id,
                           before=("instantiation",), after=("name",))


def replace_filament_id_value(text, old_id, new_id):
    """Swap the value on the `"filament_id"` line; it must carry old_id."""
    return replace_key_value(text, "filament_id", new_id, old_value=old_id)


def insert_setting_id(text, new_id):
    """Insert a `"setting_id"` line before `filament_id`, else `instantiation`.

    Falls back to `name` — the one key every preset has — so the anchor does not
    depend on whether filament_id has been written yet: a dry run, which does not
    write it, must reach the same verdict as the real run that does.
    """
    return insert_key_line(text, "setting_id", new_id,
                           before=("filament_id", "instantiation"), after=("name",))


def _edit_profile(path, edit, dry_run=False, what="edit"):
    """Apply `edit(text) -> (text, n)` to the profile at path, byte-preserving.

    The result is re-parsed and returned so the caller can verify the outcome —
    in a dry run too, where only the write itself is skipped. Raises when the
    edit found no anchor or produced invalid JSON.
    """
    with open(path, "rb") as f:
        raw = f.read()
    bom = raw.startswith(b"\xef\xbb\xbf")
    text = raw.decode("utf-8-sig")
    text, n = edit(text)
    if n == 0:
        raise RuntimeError(f"could not apply {what} to {path}")
    try:
        data = json.loads(text)  # fail loudly if the edit broke the JSON
    except ValueError as e:
        raise RuntimeError(f"{what} broke the JSON in {path}: {e}") from None
    if not dry_run:
        with open(path, "wb") as f:
            f.write((b"\xef\xbb\xbf" if bom else b"") + text.encode("utf-8"))
    return data


def write_filament_id(path, new_id, dry_run=False):
    """Insert new_id into the profile at path, byte-preserving everything else."""
    _edit_profile(path, lambda text: insert_filament_id(text, new_id),
                  dry_run, "filament_id insert")


def rewrite_filament_id(path, old_id, new_id, dry_run=False):
    """Replace the filament_id value old_id -> new_id; re-parses to verify."""
    data = _edit_profile(path, lambda text: replace_filament_id_value(text, old_id, new_id),
                         dry_run, "filament_id rewrite")
    if data.get("filament_id") != new_id:
        raise RuntimeError(f'rewrite of filament_id "{old_id}" -> "{new_id}" in {path} '
                           f"did not take effect")


# ---------------------------------------------------------------------------
# --generate
# ---------------------------------------------------------------------------

def _incomplete_triple(triple):
    """The name(s) of the empty mint-key fields, or "" when both are present."""
    return " and ".join(k for k, v in (("filament_vendor", triple[0]),
                                       ("filament_type", triple[1])) if not v)


def generate_filament_ids(profiles_dir=PROFILES_DIR, vendors=None, dry_run=False,
                          changed_paths=None):
    """Make every filament carry the id its own triple mints.

    One rule, applied to declarations and to id-less filaments alike:
      * a declared id that is not the one its own triple mints — a wrong OF id,
        or a foreign one such as a Bambu "GF*" arriving with an upstream sync —
        is replaced in place;
      * an instantiated filament that resolves no id at all gets one inserted
        into its root(s): the id-less presets of the SAME filament its members
        inherit, or the member itself (a parent of another filament cannot carry
        this filament's id — check 3).
    A declaration is left alone exactly when it already equals the one id its
    triple mints (check 3). Two products minting one id (check 3d) are reported
    and left unwritten: nothing salts past a collision, a rename resolves it.

    `vendors` restricts what is WRITTEN; the id is a function of the triple
    alone, so a narrowed run writes exactly what a full one would, and --check
    reports whatever it was not allowed to touch. `changed_paths`, when a set is
    passed, collects the files that changed. A file whose layout offers no
    anchor for the edit is reported and counted as an error, so one odd profile
    cannot abort the pass over all the others. Never reads or touches the
    snapshot — run --update-snapshot afterwards and review the diff. Returns
    (files_changed, errors).
    """
    _utf8_console()
    analysis = analyze_tree(profiles_dir)
    errors = 0
    for msg in analysis["read_errors"]:
        print_error(msg)
        errors += 1
    wanted = None
    if vendors is not None:
        wanted = set(vendors)
        # A vendor directory without a bundle index simply has no filaments to
        # process; only a name that is no directory at all is an error.
        unknown = sorted(wanted - set(list_profile_dirs(profiles_dir)))
        if unknown:
            for v in unknown:
                print_error(f'unknown vendor "{v}" in {profiles_dir}')
            return 0, errors + len(unknown)

    colliding = set()  # triples no run may write an id for
    for fid, ts in sorted(analysis["collisions"].items()):
        print_error(
            f'cannot write filament_id "{fid}": it is the mint of {len(ts)} different '
            f'products ({"; ".join("/".join(t) for t in ts)}), a base62 collision; '
            f"rename one of them so their triples differ")
        errors += 1
        colliding.update(ts)

    verb = "would " if dry_run else ""
    files_changed = 0
    reminted = 0
    inserted = 0

    # 1. Declarations that are not the mint of their own triple.
    for vendor, rec, fid, triple in sorted(
            analysis["declarer_triples"], key=lambda x: (x[0], x[1]["file"])):
        want = generate_filament_id(*triple)
        if (fid == want or (wanted is not None and vendor not in wanted)
                or triple in colliding):
            continue
        missing = _incomplete_triple(triple)
        if missing:
            print_error(
                f'cannot re-mint "{rec["file"]}" (filament_id "{fid}"): resolves empty '
                f'{missing}; the mint key needs both (generic materials use '
                f'filament_vendor "Generic")')
            errors += 1
            continue
        try:
            rewrite_filament_id(rec["path"], fid, want, dry_run)
        except (OSError, RuntimeError, ValueError) as e:
            print_error(str(e))
            errors += 1
            continue
        files_changed += 1
        reminted += 1
        if changed_paths is not None:
            # Index sub_paths are "/"-joined even on Windows, where the
            # setting_id pass reaches the same file through os.walk: normalize
            # or one file touched by both passes counts as two.
            changed_paths.add(os.path.normpath(rec["path"]))
        print_info(f'{verb}rewrite {rec["file"]}: "{fid}" -> "{want}" '
                   f'(triple "{"/".join(triple)}")')

    # 2. Instantiated filaments that resolve no id at all, grouped by filament.
    filaments = {}  # (vendor, filament name) -> [rec]
    for vendor, name, _file in analysis["missing_effective"]:
        if wanted is not None and vendor not in wanted:
            continue
        rec = analysis["vendors"][vendor][name]
        if rec["id_source"] in ("cycle", "dangling"):
            print_error(f'cannot mint for "{vendor}/{name}": broken inherits chain '
                        f'({rec["id_source"]})')
            errors += 1
            continue
        filaments.setdefault((vendor, base_name(name)), []).append(rec)

    ofl_map = analysis["vendors"].get(OFL, {})
    for (vendor, filament_name), members in sorted(filaments.items()):
        vendor_map = analysis["vendors"][vendor]
        # Root preset(s): the direct vendor-side parents of the members (id-less
        # by construction) that belong to the same filament, or the member itself.
        roots = {}
        for rec in members:
            parent = rec.get("inherits")
            root = vendor_map.get(parent) if parent else None
            if (root is None or root.get("filament_id")
                    or base_name(root["name"]) != filament_name):
                root = rec  # root-less member carries the id itself
            roots[root["name"]] = root
        fields = {(resolve_filament_field(n, "filament_vendor", vendor_map, ofl_map),
                   resolve_filament_field(n, "filament_type", vendor_map, ofl_map))
                  for n in roots}
        if len(fields) > 1:
            print_error(
                f'cannot mint for filament "{vendor}/{filament_name}": its roots resolve '
                f"divergent (filament_vendor, filament_type) pairs {sorted(fields)}; "
                f"align the fields first")
            errors += 1
            continue
        triple = (*next(iter(fields)), filament_name)
        if triple in colliding:
            continue  # reported above
        missing = _incomplete_triple(triple)
        if missing:
            print_error(
                f'cannot mint for filament "{vendor}/{filament_name}": it resolves empty '
                f'{missing}; the mint key needs both (generic materials use '
                f'filament_vendor "Generic")')
            errors += 1
            continue
        new_id = generate_filament_id(*triple)
        for name in sorted(roots):
            root = roots[name]
            try:
                write_filament_id(root["path"], new_id, dry_run)
            except (OSError, RuntimeError, ValueError) as e:
                print_error(str(e))
                errors += 1
                continue
            files_changed += 1
            inserted += 1
            if changed_paths is not None:
                changed_paths.add(os.path.normpath(root["path"]))
            print_info(f'{verb}insert filament "{vendor}/{filament_name}": filament_id '
                       f'"{new_id}" -> {root["file"]}')

    print_info(f"filament_ids inserted  : {inserted}")
    print_info(f"filament_ids re-minted : {reminted}")
    return files_changed, errors


def generate_setting_ids(profiles_dir=PROFILES_DIR, vendors=None, dry_run=False,
                         changed_paths=None):
    """Make every preset carry the setting_id its identity mints.

    One walk over filament/, process/ and machine/ of every vendor bundle, and
    one composite edit per file: drop the misspelled "settings_id" key (the app
    never reads it), strip setting_id from base profiles (only instantiated
    presets carry one), and set generate_preset_setting_id(vendor, type, name) on
    instantiated presets — except BBL's, which keep their authoritative "G*"
    cloud ids. `changed_paths`, when a set is passed, collects the files that
    changed. Returns (files_changed, errors).
    """
    _utf8_console()
    profiles_dir = str(profiles_dir)
    errors = 0
    names = list_profile_dirs(profiles_dir)
    if vendors is not None:
        wanted = set(vendors)
        unknown = sorted(wanted - set(names))
        if unknown:
            for v in unknown:
                print_error(f'unknown vendor "{v}" in {profiles_dir}')
            return 0, len(unknown)
        names = [v for v in names if v in wanted]
        for v in sorted(wanted & RESERVED_VENDORS):
            print_info(f'{v} keeps its authoritative "G*" setting_ids; only its base '
                       f"declarations are stripped")

    verb = "would " if dry_run else ""
    files_changed = 0
    counts = {"typos": 0, "stripped": 0, "assigned": 0}
    for vendor in names:
        for path, type_name in iter_profile_files(os.path.join(profiles_dir, vendor)):
            try:
                with open(path, "rb") as f:
                    data = json.loads(f.read().decode("utf-8-sig"))
                if not isinstance(data, dict):
                    raise ValueError("top level is not a JSON object")
            except (OSError, ValueError) as e:
                print_error(f"unreadable profile {path}: {e}")
                errors += 1
                continue

            sid = data.get("setting_id")
            # Strictly "true", exactly as orca_extra_profile_check.py tests it:
            # a preset the validator calls a base profile must not be given an id
            # here, or the two would fight over it forever.
            instantiated = data.get("instantiation") == "true"
            edits = []  # (counter key, description, edit function)
            if "settings_id" in data:
                edits.append(("typos", 'drop the misspelled "settings_id"',
                              lambda text: delete_key_line(text, "settings_id")))
                typo = data["settings_id"]
                if (vendor in RESERVED_VENDORS and instantiated and sid is None
                        and isinstance(typo, str) and typo):
                    # A reserved vendor's ids are authoritative, so there is no
                    # formula to fall back on: correct the key and keep the
                    # value, or the drop would leave an instantiated preset with
                    # no setting_id and nothing able to give it one.
                    edits.append(("assigned", 'restore its value as "setting_id"',
                                  lambda text, new=typo: insert_setting_id(text, new)))
            if not instantiated:
                if sid is not None:
                    edits.append(("stripped", "strip the base profile's setting_id",
                                  lambda text, old=sid: delete_key_line(
                                      text, "setting_id", old)))
            elif vendor not in RESERVED_VENDORS:
                name = data.get("name")
                if not name:
                    # Report and carry on: an edit already queued for this file
                    # (a misspelled key) is still worth applying.
                    print_error(f'instantiated preset has no "name": {path}')
                    errors += 1
                else:
                    new_id = generate_preset_setting_id(vendor, type_name, name)
                    if sid is None:
                        edits.append(("assigned", "insert the setting_id",
                                      lambda text, new=new_id: insert_setting_id(text, new)))
                    elif sid != new_id:
                        edits.append(("assigned", "replace the setting_id",
                                      lambda text, new=new_id, old=sid: replace_key_value(
                                          text, "setting_id", new, old)))
            if not edits:
                continue

            def apply(text, _edits=edits, _path=path):
                for _key, what, edit in _edits:
                    text, n = edit(text)
                    if n == 0:
                        raise RuntimeError(f"could not {what} in {_path}")
                return text, len(_edits)

            try:
                _edit_profile(path, apply, dry_run)
            except (OSError, RuntimeError, ValueError) as e:
                print_error(str(e))
                errors += 1
                continue
            files_changed += 1
            if changed_paths is not None:
                changed_paths.add(os.path.normpath(path))
            for key, _what, _edit in edits:
                counts[key] += 1
            rel = os.path.relpath(path, profiles_dir).replace(os.sep, "/")
            print_info(f"{verb}update {rel}: "
                       f"{', '.join(what for _key, what, _edit in edits)}")

    print_info(f'misspelled "settings_id" dropped : {counts["typos"]}')
    print_info(f'base setting_ids stripped        : {counts["stripped"]}')
    print_info(f'setting_ids assigned             : {counts["assigned"]}')
    return files_changed, errors


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

EXAMPLES = """\
examples:
  orca_id_tool.py --generate
      give every profile the id its identity mints, in every vendor bundle
  orca_id_tool.py --dry-run
      preview exactly that; writes nothing
  orca_id_tool.py --generate --setting-id
      setting_id only (filament, process and machine presets)
  orca_id_tool.py --generate --filament-id --vendor Creality --vendor Elegoo
      filament_id only, and only in those two bundles
  orca_id_tool.py --check
      validate filament_id state against the snapshot (the filament_id half
      of scripts/orca_extra_profile_check.py, which is what CI runs)
  orca_id_tool.py --update-snapshot
      re-record the sanctioned filament_id state after a --generate run

a maintenance round:
  --dry-run  ->  --generate  ->  --update-snapshot  ->  --check  ->  commit the diff
"""


def build_parser():
    parser = argparse.ArgumentParser(
        prog="orca_id_tool.py", allow_abbrev=False,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description="Assign and validate the deterministic ids of OrcaSlicer system\n"
                    "profiles: the per-product filament_id and the per-preset setting_id.\n"
                    "\n"
                    "Both are pure functions of the profile's own identity, so this tool\n"
                    "never invents an id: it writes the one the rules already imply, and\n"
                    "leaves a conforming tree alone.",
        epilog=EXAMPLES)
    modes = parser.add_argument_group("modes (pick one; no mode prints this help)")
    modes.add_argument("--generate", action="store_true",
                       help="write the id every profile should carry: filament_id from "
                            "each filament's (filament_vendor, filament_type, name) "
                            "triple, setting_id from each preset's (vendor, type, name). "
                            "Idempotent and byte-preserving")
    modes.add_argument("--check", action="store_true",
                       help="validate filament_id state against "
                            "scripts/filament_id_snapshot.json; exit nonzero on errors")
    modes.add_argument("--update-snapshot", action="store_true",
                       help="re-record the sanctioned filament_id state in "
                            "scripts/filament_id_snapshot.json; commit the diff for "
                            "maintainer review")
    narrow = parser.add_argument_group("narrowing --generate")
    narrow.add_argument("--filament-id", action="store_true",
                        help="write filament_id only, skipping setting_id")
    narrow.add_argument("--setting-id", action="store_true",
                        help="write setting_id only, skipping filament_id")
    narrow.add_argument("--vendor", metavar="VENDOR", action="append", default=[],
                        help="write only in this vendor bundle; repeatable. The id is a "
                             "function of the triple alone, so a narrowed run writes "
                             "exactly what a full one would; --check reports whatever "
                             "it left outside")
    parser.add_argument("--dry-run", "--dryrun", dest="dry_run", action="store_true",
                        help="report what would change and write nothing; with no mode of "
                             "its own it previews --generate")
    parser.add_argument("--profiles", default=PROFILES_DIR,
                        help="profiles directory (default: resources/profiles)")
    parser.add_argument("--snapshot", default=None, metavar="PATH",
                        help="the sanctioned filament_id state of that tree (default: "
                             "scripts/filament_id_snapshot.json, which describes "
                             "resources/profiles and no other tree)")
    return parser


def main(argv=None):
    _utf8_console()
    argv = sys.argv[1:] if argv is None else list(argv)
    parser = build_parser()
    if not argv:
        parser.print_help()
        return 0
    args = parser.parse_args(argv)

    modes = [flag for flag, on in (("--generate", args.generate),
                                   ("--check", args.check),
                                   ("--update-snapshot", args.update_snapshot)) if on]
    if len(modes) > 1:
        parser.error(f"{' and '.join(modes)} cannot be combined; pick one mode")
    if args.filament_id and args.setting_id:
        parser.error("--filament-id and --setting-id each exclude the other; "
                     "pass neither to write both")
    narrowing = [flag for flag, on in (("--filament-id", args.filament_id),
                                       ("--setting-id", args.setting_id),
                                       ("--vendor", bool(args.vendor))) if on]
    if not modes:
        if args.dry_run:
            mode = "--generate"  # --dry-run previews the writing mode
        elif narrowing:
            parser.error(f"{', '.join(narrowing)} narrows --generate; "
                         f"add --generate (or --dry-run to preview it)")
        else:
            parser.print_help()
            return 0
    else:
        mode = modes[0]
        if narrowing and mode != "--generate":
            parser.error(f"{', '.join(narrowing)} applies to --generate, not {mode}")

    snapshot_path = args.snapshot or SNAPSHOT_PATH
    if (args.snapshot is None and mode in ("--check", "--update-snapshot")
            and os.path.abspath(args.profiles) != os.path.abspath(PROFILES_DIR)):
        # The repo snapshot is the sanctioned state of resources/profiles alone:
        # checking another tree against it is meaningless, and re-recording one
        # into it would overwrite the tracked file with a foreign tree's state.
        parser.error(f"{mode} reads and writes the sanctioned state of the tree it is "
                     f"given, so --profiles needs --snapshot PATH for that tree too")

    if mode == "--check":
        errors = check_filament_ids(args.profiles, snapshot_path)
        if errors:
            print_error(f"filament_id check: {errors} error(s)")
            return 1
        print_success("filament_id check: no errors")
        return 0

    if mode == "--update-snapshot":
        return update_snapshot(args.profiles, snapshot_path, dry_run=args.dry_run)

    vendors = sorted(set(args.vendor)) or None
    if vendors:
        unknown = sorted(set(vendors) - set(list_profile_dirs(args.profiles)))
        if unknown:
            for v in unknown:
                print_error(f'unknown vendor "{v}" in {args.profiles}')
            return 1

    # Both by default. filament_id runs first so its keys are in place before the
    # setting_id pass reads the files back.
    do_filament = args.filament_id or not args.setting_id
    do_setting = args.setting_id or not args.filament_id
    changed = set()  # one file the two passes both touch is still one file
    filament_files = errors = 0
    if do_filament:
        filament_files, e = generate_filament_ids(
            args.profiles, vendors, args.dry_run, changed)
        errors += e
    if do_setting:
        _n, e = generate_setting_ids(args.profiles, vendors, args.dry_run, changed)
        errors += e

    summary = (f"dry run: {len(changed)} file(s) would change; nothing written"
               if args.dry_run else f"{len(changed)} file(s) changed")
    if errors:
        print_error(f"{summary}; {errors} error(s)")
    else:
        print_success(summary)
    if filament_files and not args.dry_run:
        # A filament_id write may or may not move the sanctioned state (an id
        # repaired back to the value the snapshot already records does not), so
        # regenerate and let the diff — empty or not — say.
        print_warning('now run "python scripts/orca_id_tool.py --update-snapshot" '
                      "and commit any resulting diff for maintainer review")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
