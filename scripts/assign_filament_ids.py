#!/usr/bin/env python3
"""
Mint deterministic filament_id values for OrcaSlicer system filament products and
validate the tree against the sanctioned-state snapshot.

Policy (companion to assign_vendor_setting_ids.py; see docs/HLSD/filament_id.md):
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
    hand; on the astronomically rare collision with an existing id the input
    is salted ("/1", "/2", ...) until free and the result is frozen in file.
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

The effective-id resolution below is loader-faithful (PresetBundle.cpp
load_vendor_configs_from_json): own filament_id key, else walk `inherits` within
the vendor map, with OrcaFilamentLibrary base-bundle fallback; once a chain enters
OFL it stays in OFL; a vendor chain that dead-ends id-less retries its direct
parent in the OFL map. filament_vendor / filament_type resolve the same way.

Run from anywhere:  python3 scripts/assign_filament_ids.py
  (default)          mint + insert ids for id-less filaments, mint + replace
                     non-OF-format declarations; idempotent, never rewrites a
                     valid OF-format id; a no-op once every filament has one
  --mint "Vendor/Type/Filament"
                     print the id that triple would mint; touches nothing
  --update-snapshot  regenerate the snapshot from the tree
  --remint VENDOR    re-derive VENDOR's declared ids from their triples and
                     rewrite mismatches in place (repeatable)
  --drop-redundant-ids VENDOR
                     delete declarations that re-declare an inherited OFL id
  --check            run the validation checks (also run by CI through
                     orca_extra_profile_check.py); exit nonzero on errors
"""

import argparse
import json
import os
import re
import sys
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from assign_vendor_setting_ids import ALPHABET, NAMESPACE  # noqa: E402

# Dedicated namespace for filament_id, derived from the setting_id namespace baked
# into both Python and C++ (assign_vendor_setting_ids.NAMESPACE). Never change it.
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

OF_ID_RE = re.compile(r"^OF[0-9A-Za-z]{6}$")
# User-custom id space minted by CreatePresetsDialog.cpp ("P" + md5(name)[0:7]);
# reserved case-insensitively, together with its "null" sentinel.
USER_CUSTOM_ID_RE = re.compile(r"^P[0-9A-Fa-f]{7}$", re.IGNORECASE)
# Filament name = preset base name: strip the first "@..." suffix. The space before
# "@" is optional because names like "Afinia PLA@HS" exist.
BASE_NAME_RE = re.compile(r"\s?@.*$")
# Salt iterations accepted by the identity check (check 3).
MAX_CHECK_SALT = 8

UPDATE_HINT = 'run "python scripts/assign_filament_ids.py --update-snapshot" and commit the diff for maintainer review'
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

def base_name(name):
    """Filament name of a preset: name with the first "@..." suffix stripped."""
    return BASE_NAME_RE.sub("", name, count=1)


def generate_filament_id(filament_vendor, filament_type, filament_name, salt=0):
    """Deterministic "OF" + 6-char base62 filament_id for a filament product.

    input = "filament_product/<filament_vendor>/<filament_type>/<filament_name>"
    (+ "/<salt>" when salted); u = uuid5(FILAMENT_ID_NAMESPACE, input); the id
    tail is the low FILAMENT_ID_LENGTH base62 digits of int(u.bytes, "big"),
    most-significant first — the same derivation as generate_preset_setting_id.
    """
    key = f"filament_product/{filament_vendor}/{filament_type}/{filament_name}"
    if salt:
        key = f"{key}/{salt}"
    u = uuid.uuid5(FILAMENT_ID_NAMESPACE, key)
    n = int.from_bytes(u.bytes, "big")
    digits = []
    for _ in range(FILAMENT_ID_LENGTH):
        digits.append(ALPHABET[n % 62])
        n //= 62
    return "OF" + "".join(reversed(digits))


def mint_filament_id(filament_vendor, filament_type, filament_name, taken):
    """Mint the product's id, salting past any id in `taken`."""
    for salt in range(10000):
        candidate = generate_filament_id(filament_vendor, filament_type, filament_name, salt)
        if candidate not in taken:
            return candidate
    raise RuntimeError(
        f"could not mint a free filament_id for {filament_vendor}/{filament_type}/{filament_name}")


def mint_iterations(triple):
    """The ids that count as the mint of `triple`: salt 0..MAX_CHECK_SALT."""
    return {generate_filament_id(*triple, salt=s) for s in range(MAX_CHECK_SALT + 1)}


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


def resolve_filament_id(name, filaments, ofl_filaments, seen=None, in_ofl=False, skip_own=False):
    """Walk the inherits chain for the effective filament_id, loader-faithfully.

    Mirrors PresetBundle.cpp load_vendor_configs_from_json: a hop resolves in the
    vendor's own map first, then falls back to the OFL base-bundle map. OFL's map
    was memoized entirely within OFL, so once a chain enters OFL it stays in OFL
    (a vendor file sharing an OFL preset's name must not shadow OFL-internal
    hops). Additionally, a vendor preset that never resolves an id inside the
    vendor is re-tried against the OFL map keyed by its direct parent name.

    skip_own ignores the first preset's own filament_id key (used to compute the
    id its inherits chain would resolve WITHOUT the declaration — the
    --drop-redundant-ids redundancy test).

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
    if rec.get("filament_id") and not skip_own:
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

    for vendor, filaments in vendors.items():
        occurring = vendor_ids.setdefault(vendor, set())
        for rec in filaments.values():
            triple = resolve_triple(rec["name"], filaments, ofl_filaments)
            rec["triple"] = triple
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

    # Identity (check 3b): an inherited id must be the mint of the preset's
    # OWN triple. A declared id is held to the same rule as a declarer (3a),
    # and an id whose declarer already fails 3a is reported there once, not
    # again under every preset inheriting it.
    unminted = {fid for _v, _r, fid, triple in declarer_triples
                if OF_ID_RE.match(fid) and fid not in mint_iterations(triple)}
    id_mismatches = [
        (vendor, rec, eff, triple) for vendor, rec, eff, triple in inherited
        if eff not in unminted and eff not in mint_iterations(triple)]

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
        "id_mismatches": id_mismatches,
        "triples": {fid: sorted(list(t) for t in ts) for fid, ts in triples.items()},
        "triple_sets": triples,
        "declarer_triples": declarer_triples,
        "filament_triples": filament_triples,
        "cross_bundle_triples": cross_bundle_triples,
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
    3. Identity: the id is a function of the triple alone. (a) A declared id
       must equal the mint of the declarer's own triple or a salted iteration;
       (b) the id an instantiated preset inherits must equal the mint of ITS
       own triple — how it inherits it (a root, a real filament, an OFL
       preset) is irrelevant; (c) every instantiated filament resolves an
       effective id at all (an id-less one is a hard load error in C++).
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
                f'filament ids must come from "python scripts/assign_filament_ids.py" '
                f'(see --mint)')
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
    for vendor, rec, fid, triple in sorted(
            analysis["declarer_triples"], key=lambda x: (x[0], x[1]["file"])):
        if not OF_ID_RE.match(fid) or fid in mint_iterations(triple):
            continue
        print_error(
            f'filament_id "{fid}" declared by "{rec["name"]}" ({rec["file"]}) does '
            f'not match the mint of its triple "{"/".join(triple)}": expected '
            f'"{generate_filament_id(*triple)}" (or a salted iteration); paste the '
            f"expected id, or fix the triple and --remint the vendor")
        errors += 1
    for vendor, rec, eff, triple in sorted(
            analysis["id_mismatches"], key=lambda x: (x[0], x[1]["file"])):
        print_error(
            f'preset "{rec["name"]}" ({rec["file"]}) inherits filament_id "{eff}" but '
            f'its own triple "{"/".join(triple)}" mints "{generate_filament_id(*triple)}"; '
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
            f'run "python scripts/assign_filament_ids.py" (expected id for filament '
            f'"{vendor}/{base_name(name)}": "{expected}", salted if taken)')
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

def update_snapshot(profiles_dir=PROFILES_DIR, snapshot_path=SNAPSHOT_PATH):
    """Regenerate the snapshot from the tree.

    Refuses to sanction a reserved-namespace id (or a claim on one) and an id
    declared under more than one triple: neither can ever pass --check, so
    writing it into the snapshot would only hide the mistake until CI.
    Idempotent: a second run over an unchanged tree changes nothing. Returns 0
    on success.
    """
    analysis = analyze_tree(profiles_dir)
    for msg in analysis["read_errors"]:
        print_error(msg)

    refusals = 0
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

    if changed:
        write_snapshot(snapshot_path, new_snap)

    print_info(f"snapshot ids      : {len(new_snap['ids'])} (+{len(added_ids)} / -{len(removed_ids)})")
    print_info(f"claims added      : {added_claims}")
    print_info(f"claims removed    : {removed_claims}")
    if changed:
        print_success(f"snapshot written to {snapshot_path}")
    else:
        print_success("snapshot already up to date; nothing changed")
    return 0


# ---------------------------------------------------------------------------
# Byte-preserving profile edits
# ---------------------------------------------------------------------------

def insert_filament_id(text, new_id):
    """Insert a `"filament_id"` line into a preset that lacks one.

    Placed just before `instantiation` (or, failing that, after the `name` line)
    so it matches the canonical key order, reusing that anchor line's indentation
    and line ending. Same byte-preserving approach as assign_vendor_setting_ids.
    """
    m = re.search(r'^([ \t]*)"instantiation"[ \t]*:.*?(\r?\n)', text, re.MULTILINE)
    if m:
        line = f'{m.group(1)}"filament_id": {json.dumps(new_id, ensure_ascii=False)},{m.group(2)}'
        return text[:m.start()] + line + text[m.start():], 1
    m = re.search(r'^([ \t]*)"name"[ \t]*:.*?(\r?\n)', text, re.MULTILINE)
    if m:
        line = f'{m.group(1)}"filament_id": {json.dumps(new_id, ensure_ascii=False)},{m.group(2)}'
        return text[:m.end()] + line + text[m.end():], 1
    return text, 0


def replace_filament_id_value(text, old_id, new_id):
    """Swap the JSON string VALUE on the `"filament_id"` line, byte-preserving
    everything else. Returns (text, replacements made).
    """
    pattern = re.compile(
        r'(^[ \t]*"filament_id"[ \t]*:[ \t]*)'
        + re.escape(json.dumps(old_id, ensure_ascii=False)),
        re.MULTILINE)
    return pattern.subn(
        lambda m: m.group(1) + json.dumps(new_id, ensure_ascii=False), text, count=1)


def delete_filament_id_line(text, old_id):
    """Delete the `"filament_id"` line, byte-preserving the rest.

    Handles both the canonical layout (trailing comma) and a last-property
    layout (comma on the preceding line). Returns (text, deletions made).
    """
    val = re.escape(json.dumps(old_id, ensure_ascii=False))
    m = re.search(r'^[ \t]*"filament_id"[ \t]*:[ \t]*' + val + r'[ \t]*,[ \t]*\r?\n',
                  text, re.MULTILINE)
    if m:
        return text[:m.start()] + text[m.end():], 1
    m = re.search(r',[ \t]*\r?\n[ \t]*"filament_id"[ \t]*:[ \t]*' + val + r'[ \t]*(?=\r?\n)',
                  text)
    if m:
        return text[:m.start()] + text[m.end():], 1
    return text, 0


def _edit_profile(path, edit):
    """Apply `edit(text) -> (text, n)` to the profile at path, byte-preserving.

    Binary IO keeps the file's original line endings (LF or CRLF), BOM and exact
    formatting apart from the edit; the result is re-parsed and returned so the
    caller can verify the outcome. Raises when the edit found no anchor.
    """
    with open(path, "rb") as f:
        raw = f.read()
    bom = raw.startswith(b"\xef\xbb\xbf")
    text = raw.decode("utf-8-sig")
    text, n = edit(text)
    if n == 0:
        raise RuntimeError(f"could not apply filament_id edit to {path}")
    data = json.loads(text)  # fail loudly if the edit broke the JSON
    with open(path, "wb") as f:
        f.write((b"\xef\xbb\xbf" if bom else b"") + text.encode("utf-8"))
    return data


def write_filament_id(path, new_id):
    """Insert new_id into the profile at path, byte-preserving everything else."""
    _edit_profile(path, lambda text: insert_filament_id(text, new_id))


def rewrite_filament_id(path, old_id, new_id):
    """Replace the filament_id value old_id -> new_id; re-parses to verify."""
    data = _edit_profile(path, lambda text: replace_filament_id_value(text, old_id, new_id))
    if data.get("filament_id") != new_id:
        raise RuntimeError(f'rewrite of filament_id "{old_id}" -> "{new_id}" in {path} '
                           f"did not take effect")


def remove_filament_id(path, old_id):
    """Delete the filament_id declaration line; re-parses to verify."""
    data = _edit_profile(path, lambda text: delete_filament_id_line(text, old_id))
    if "filament_id" in data:
        raise RuntimeError(f'deletion of filament_id "{old_id}" from {path} did not take effect')


# ---------------------------------------------------------------------------
# Default run: mint + insert ids for id-less filaments
# ---------------------------------------------------------------------------

def assign_missing_ids(profiles_dir=PROFILES_DIR, snapshot_path=SNAPSHOT_PATH):
    """Mint + insert ids for id-less filaments; mint + replace non-OF-format
    declarations. Never rewrites a valid (OF-format) existing id.

    A filament = (vendor, base name) group over instantiated presets with no
    effective id. The minted id is a pure function of the filament's triple —
    (filament_vendor, filament_type) resolved on the root(s), filament name —
    and is inserted into the filament's root(s): the id-less presets of the
    SAME filament its members inherit, or the member itself otherwise (a parent
    of another filament cannot carry this filament's id — check 3).

    A declaration whose value is not OF-format (e.g. a vendor bundle synced
    from an upstream source that ships its own catalog ids, such as BBL's GF*)
    is treated the same as a missing filament: a fresh id is minted for the
    declarer's triple and the value is replaced in place (declarers that share
    one triple across several per-printer roots converge on the same id, same
    as the multi-root filaments above). This is what makes a future BBL sync
    self-healing: upstream files arrive with GF ids, this pass replaces them,
    and the generated Bambu catalog map (keyed by the ids this mints) already
    knows the resulting rows.

    Returns (files_changed, errors).
    """
    analysis = analyze_tree(profiles_dir)
    errors = 0
    for msg in analysis["read_errors"]:
        print_error(msg)
        errors += 1

    # Group id-less instantiated presets by filament.
    filaments = {}  # (vendor, filament_name) -> [rec]
    for vendor, name, _file in analysis["missing_effective"]:
        rec = analysis["vendors"][vendor][name]
        if rec["id_source"] in ("cycle", "dangling"):
            print_error(f'cannot mint for "{vendor}/{name}": broken inherits chain '
                        f'({rec["id_source"]})')
            errors += 1
            continue
        filaments.setdefault((vendor, base_name(name)), []).append(rec)

    # Declarations whose value is not OF-format: treated as missing too (see
    # docstring). Grouped by triple, not by (vendor, filament), so declarers
    # that legitimately share one triple across several files converge on one
    # freshly minted id instead of each getting their own.
    non_of_declarers = [
        (vendor, rec, fid, triple) for vendor, rec, fid, triple in analysis["declarer_triples"]
        if not OF_ID_RE.match(fid)]

    if not filaments and not non_of_declarers:
        print_success("every instantiated filament already resolves an OF-format "
                      "filament_id; nothing to do (0 files changed)")
        return 0, errors

    # Ids already spoken for: the whole tree (declared or effective) + snapshot.
    snapshot = load_snapshot(snapshot_path) or {"ids": {}}
    taken = set(snapshot["ids"])
    for occurring in analysis["vendor_ids"].values():
        taken |= occurring

    # Root preset(s): the direct vendor-side parents of the members (id-less by
    # construction) that belong to the same filament, or the member itself.
    roots = {}  # (vendor, filament_name) -> {preset name: rec}
    for key, members in sorted(filaments.items()):
        vendor, filament_name = key
        vendor_map = analysis["vendors"][vendor]
        filament_roots = {}
        for rec in members:
            parent = rec.get("inherits")
            root = vendor_map.get(parent) if parent else None
            if (root is None or root.get("filament_id")
                    or base_name(root["name"]) != filament_name):
                root = rec  # root-less member carries the id itself
            filament_roots[root["name"]] = root
        roots[key] = filament_roots

    ofl_map = analysis["vendors"].get(OFL, {})
    files_changed = 0
    filaments_minted = 0
    for key, filament_roots in sorted(roots.items()):
        vendor, filament_name = key
        vendor_map = analysis["vendors"][vendor]
        fields = {(resolve_filament_field(n, "filament_vendor", vendor_map, ofl_map),
                   resolve_filament_field(n, "filament_type", vendor_map, ofl_map))
                  for n in filament_roots}
        if len(fields) > 1:
            print_error(
                f'cannot mint for filament "{vendor}/{filament_name}": its roots resolve '
                f"divergent (filament_vendor, filament_type) pairs {sorted(fields)}; "
                f"align the fields first")
            errors += 1
            continue
        fvendor, ftype = next(iter(fields))
        if not fvendor or not ftype:
            missing = " and ".join(
                k for k, v in (("filament_vendor", fvendor),
                               ("filament_type", ftype)) if not v)
            print_error(
                f'cannot mint for filament "{vendor}/{filament_name}": it resolves empty '
                f'{missing}; the mint key needs both (generic materials use '
                f'filament_vendor "Generic")')
            errors += 1
            continue
        new_id = mint_filament_id(fvendor, ftype, filament_name, taken)
        taken.add(new_id)
        filaments_minted += 1
        for name in sorted(filament_roots):
            root = filament_roots[name]
            write_filament_id(root["path"], new_id)
            files_changed += 1
            print_info(f'filament "{vendor}/{filament_name}": filament_id "{new_id}" '
                       f'-> {root["file"]}')

    # Non-OF-format declarations: mint once per triple, rewrite every declarer
    # that shares it (see docstring).
    declarations_reminted = 0
    assigned = {}  # triple -> id chosen this run
    for vendor, rec, fid, triple in sorted(non_of_declarers, key=lambda x: (x[0], x[1]["file"])):
        fvendor, ftype, filament_name = triple
        if not fvendor or not ftype:
            missing = " and ".join(
                k for k, v in (("filament_vendor", fvendor),
                               ("filament_type", ftype)) if not v)
            print_error(
                f'cannot re-mint "{rec["file"]}" (non-OF filament_id "{fid}"): resolves '
                f'empty {missing}; the mint key needs both (generic materials use '
                f'filament_vendor "Generic")')
            errors += 1
            continue
        if triple not in assigned:
            assigned[triple] = mint_filament_id(fvendor, ftype, filament_name, taken)
            taken.add(assigned[triple])
        new_id = assigned[triple]
        rewrite_filament_id(rec["path"], fid, new_id)
        files_changed += 1
        declarations_reminted += 1
        print_info(f'filament "{vendor}/{filament_name}": non-OF filament_id "{fid}" '
                   f'-> "{new_id}" ({rec["file"]})')

    print_info(f"filaments minted : {filaments_minted}")
    print_info(f"non-OF reminted  : {declarations_reminted}")
    print_info(f"files changed    : {files_changed}")
    if files_changed:
        print_warning(f"now {UPDATE_HINT}")
    return files_changed, errors


# ---------------------------------------------------------------------------
# --remint / --drop-redundant-ids (v3.1/v3.2 migration modes)
# ---------------------------------------------------------------------------

def remint_vendors(vendor_list, profiles_dir=PROFILES_DIR):
    """Re-derive every declared id in the given vendors from its triple;
    rewrite mismatching declarations in place (byte-preserving). Never touches
    the snapshot — run --update-snapshot afterwards and review the diff.
    Accepts BBL like any other vendor: the GF* catalog is reserved and
    ownerless, so BBL's own declarations mint OF ids the same as everyone
    else's.

    A declaration already equal to ANY salt iteration of its own triple is
    mint-conformant (check 3) and left alone — deliberate salt splits (distinct
    presets of one product kept apart for per-printer AMS matching) survive.
    A candidate id is blocked when it occurs in the tree with any triple set
    other than exactly {T}. Equal-triple reuse
    is convergence: the same product must end up under the same id everywhere.
    Returns (rewritten, errors).
    """
    _utf8_console()
    analysis = analyze_tree(profiles_dir)
    errors = 0
    for msg in analysis["read_errors"]:
        print_error(msg)
        errors += 1
    unknown = sorted(set(vendor_list) - set(analysis["vendors"]))
    if unknown:
        for v in unknown:
            print_error(f"--remint: unknown vendor {v!r}")
        return 0, errors + len(unknown)

    occurring = set()
    for vids in analysis["vendor_ids"].values():
        occurring |= vids
    triple_sets = analysis["triple_sets"]

    assigned = {}       # triple -> id chosen this run (all declarers converge)
    run_taken = {}      # id -> triple (a run-local mint may not collide either)

    def want_id(triple):
        if triple in assigned:
            return assigned[triple]
        for salt in range(10000):
            cand = generate_filament_id(*triple, salt=salt)
            if cand in occurring and triple_sets.get(cand, set()) != {triple}:
                continue
            if run_taken.get(cand, triple) != triple:
                continue
            assigned[triple] = cand
            run_taken[cand] = triple
            return cand
        raise RuntimeError(f"could not mint a free filament_id for triple {triple}")

    scanned = 0
    rewritten = 0
    for vendor in sorted(set(vendor_list)):
        recs = sorted(
            (rec for rec in analysis["vendors"][vendor].values() if rec.get("filament_id")),
            key=lambda r: r["file"])
        for rec in recs:
            fid = rec["filament_id"]
            scanned += 1
            # Any salt iteration of the declarer's own triple is already
            # mint-conformant (check 3) — leave it. This keeps deliberate salt
            # splits (two presets of one product that must stay distinct for
            # per-printer AMS matching, validator -f) stable across re-mints.
            if fid in mint_iterations(rec["triple"]):
                continue
            want = want_id(rec["triple"])
            if fid == want:
                continue
            rewrite_filament_id(rec["path"], fid, want)
            rewritten += 1
            print_info(f'{rec["file"]}: "{fid}" -> "{want}" '
                       f'(triple "{"/".join(rec["triple"])}")')
    print_info(f"declarations scanned : {scanned}")
    print_info(f"declarations reminted: {rewritten}")
    if rewritten:
        print_warning(f"now {UPDATE_HINT}")
    return rewritten, errors


def drop_redundant_ids(vendor, profiles_dir=PROFILES_DIR):
    """Delete filament_id declarations in `vendor` that merely re-declare an OFL
    filament's id path: ignoring its own key, the preset's inherits chain enters
    OFL and resolves an OFL-declared id, and the preset keeps the OFL filament's
    base name. Such a preset is a specialization and rides the OFL id (v3.2(b)).
    Never touches the snapshot. Returns (dropped, errors).
    """
    _utf8_console()
    analysis = analyze_tree(profiles_dir)
    errors = 0
    for msg in analysis["read_errors"]:
        print_error(msg)
        errors += 1
    if vendor == "BBL":
        print_error("--drop-redundant-ids BBL is forbidden (the GF* catalog is frozen)")
        return 0, errors + 1
    if vendor not in analysis["vendors"]:
        print_error(f"--drop-redundant-ids: unknown vendor {vendor!r}")
        return 0, errors + 1

    vendor_map = analysis["vendors"][vendor]
    ofl_map = analysis["vendors"].get(OFL, {})
    ofl_declared = {r["filament_id"] for r in ofl_map.values() if r.get("filament_id")}
    dropped = 0
    for rec in sorted(vendor_map.values(), key=lambda r: r["file"]):
        fid = rec.get("filament_id")
        if not fid or not rec.get("inherits"):
            continue
        resolved, _src, entry = resolve_filament_id(
            rec["name"], vendor_map, ofl_map, skip_own=True)
        if not (entry and resolved and resolved in ofl_declared):
            continue
        if base_name(rec["name"]) != base_name(entry):
            continue
        remove_filament_id(rec["path"], fid)
        dropped += 1
        print_info(f'{rec["file"]}: dropped filament_id "{fid}" '
                   f'(rides OFL "{entry}", id "{resolved}")')
    print_info(f"declarations dropped : {dropped}")
    if dropped:
        print_warning(f"now {UPDATE_HINT}")
    return dropped, errors


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv=None):
    _utf8_console()
    parser = argparse.ArgumentParser(
        description="Mint deterministic filament_id values for id-less filaments "
                    "and validate the tree against the sanctioned snapshot.")
    parser.add_argument("--mint", metavar='"Vendor/Type/Filament"',
                        help="print the id the (filament_vendor, filament_type, "
                             "filament name) triple would mint; touches nothing")
    parser.add_argument("--update-snapshot", action="store_true",
                        help="regenerate scripts/filament_id_snapshot.json from "
                             "the tree")
    parser.add_argument("--check", action="store_true",
                        help="run the filament_id checks; exit nonzero on errors")
    parser.add_argument("--remint", metavar="VENDOR", action="append",
                        help="re-derive VENDOR's declared filament_ids from their "
                             "triples and rewrite mismatches in place; repeatable")
    parser.add_argument("--drop-redundant-ids", metavar="VENDOR",
                        help="delete filament_id declarations in VENDOR that "
                             "re-declare the OFL filament id they already resolve "
                             "through inherits")
    parser.add_argument("--profiles", default=PROFILES_DIR,
                        help="profiles directory (default: resources/profiles)")
    args = parser.parse_args(argv)


    if args.mint:
        parts = args.mint.split("/", 2)
        if len(parts) != 3 or not all(parts):
            parser.error('--mint expects "filament_vendor/filament_type/filament_name" '
                         "with all three components non-empty (check 5 rejects empty "
                         "vendor/type in the tree)")
        snapshot = load_snapshot(SNAPSHOT_PATH) or {"ids": {}}
        taken = set(snapshot["ids"])
        print(mint_filament_id(parts[0], parts[1], parts[2], taken))
        return 0

    if args.remint:
        if args.update_snapshot or args.check or args.drop_redundant_ids:
            parser.error("--remint cannot be combined with other modes")
        _changed, errors = remint_vendors(args.remint, args.profiles)
        return 1 if errors else 0

    if args.drop_redundant_ids:
        if args.update_snapshot or args.check:
            parser.error("--drop-redundant-ids cannot be combined with other modes")
        _changed, errors = drop_redundant_ids(args.drop_redundant_ids, args.profiles)
        return 1 if errors else 0

    if args.update_snapshot:
        return update_snapshot(args.profiles, SNAPSHOT_PATH)

    if args.check:
        errors = check_filament_ids(args.profiles, SNAPSHOT_PATH)
        if errors:
            print_error(f"filament_id check: {errors} error(s)")
            return 1
        print_success("filament_id check: no errors")
        return 0

    _changed, errors = assign_missing_ids(args.profiles, SNAPSHOT_PATH)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
