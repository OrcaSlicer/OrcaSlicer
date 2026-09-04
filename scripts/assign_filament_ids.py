#!/usr/bin/env python3
"""
Mint deterministic filament_id values for OrcaSlicer system filament products and
validate the tree against the sanctioned-state snapshot.

Policy (companion to assign_vendor_setting_ids.py; see docs/HLSD/filament_id.md):
  * filament_id is a PRODUCT id: one commercial product line = one id, shared by
    all of that material's per-printer/per-nozzle variants in every bundle.
    OrcaFilamentLibrary (OFL) is the product catalog: a family's id is declared
    once, on its family root (instantiation != "true"); vendor bundles carry
    only specializations of OFL families — same base name, non-empty
    compatible_printers, no filament_id key — which resolve the OFL id through
    the loader's inherits/base-bundle walk. Products OFL does not carry mint
    their id in the vendor bundle with the same rule; the key is
    bundle-independent, so hoisting a family into OFL never changes its id.
  * New ids are content-addressed by the product triple, resolved from the
    declaring preset's flattened config (filament_vendor and filament_type are
    inheritable list options — first element; family name = preset base name):
        filament_id = "OF" + base62_6( uuid5(FILAMENT_ID_NAMESPACE,
            "filament_product/<filament_vendor>/<filament_type>/<family_name>") )
    8 chars total, which satisfies the AMS length limit. Nobody invents ids by
    hand; on the astronomically rare collision with an existing id the input
    is salted ("/1", "/2", ...) until free and the result is frozen in file.
    Identity changes (a family rename, a filament_vendor/filament_type fix)
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
      - every already-shipped id, grandfathered via scripts/filament_id_snapshot.json
  * scripts/filament_id_snapshot.json is the sanctioned-state snapshot (ids,
    claims and declared triples): it must exactly equal the tree-derived state
    at all times, so any id/claim/triple change shows up as a reviewable diff to
    that file (the maintainer gate).

The effective-id resolution below is loader-faithful (PresetBundle.cpp
load_vendor_configs_from_json): own filament_id key, else walk `inherits` within
the vendor map, with OrcaFilamentLibrary base-bundle fallback; once a chain enters
OFL it stays in OFL; a vendor chain that dead-ends id-less retries its direct
parent in the OFL map. filament_vendor / filament_type resolve the same way.

Run from anywhere:  python3 scripts/assign_filament_ids.py
  (default)          mint + insert ids for id-less families, mint + replace
                     non-OF-format declarations; idempotent, never rewrites a
                     valid OF-format id; a no-op once every family has one
  --mint "Vendor/Type/Family"
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
# Family name = preset base name: strip the first "@..." suffix. The space before
# "@" is optional because names like "Afinia PLA@HS" exist.
BASE_NAME_RE = re.compile(r"\s?@.*$")
# Salt iterations accepted by the mint-conformance check (check 3).
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
    """Family name of a preset: name with the first "@..." suffix stripped."""
    return BASE_NAME_RE.sub("", name, count=1)


def generate_filament_id(filament_vendor, filament_type, family, salt=0):
    """Deterministic "OF" + 6-char base62 filament_id for a filament product.

    input = "filament_product/<filament_vendor>/<filament_type>/<family_name>"
    (+ "/<salt>" when salted); u = uuid5(FILAMENT_ID_NAMESPACE, input); the id
    tail is the low FILAMENT_ID_LENGTH base62 digits of int(u.bytes, "big"),
    most-significant first — the same derivation as generate_preset_setting_id.
    """
    key = f"filament_product/{filament_vendor}/{filament_type}/{family}"
    if salt:
        key = f"{key}/{salt}"
    u = uuid.uuid5(FILAMENT_ID_NAMESPACE, key)
    n = int.from_bytes(u.bytes, "big")
    digits = []
    for _ in range(FILAMENT_ID_LENGTH):
        digits.append(ALPHABET[n % 62])
        n //= 62
    return "OF" + "".join(reversed(digits))


def mint_filament_id(filament_vendor, filament_type, family, taken):
    """Mint the product's id, salting past any id in `taken`."""
    for salt in range(10000):
        candidate = generate_filament_id(filament_vendor, filament_type, family, salt)
        if candidate not in taken:
            return candidate
    raise RuntimeError(
        f"could not mint a free filament_id for {filament_vendor}/{filament_type}/{family}")


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
    id its inherits chain would resolve WITHOUT the declaration — check 6b drift).

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
    """The preset's mint-key triple (filament_vendor, filament_type, family)."""
    return (resolve_filament_field(name, "filament_vendor", filaments, ofl_filaments),
            resolve_filament_field(name, "filament_type", filaments, ofl_filaments),
            base_name(name))


def analyze_tree(profiles_dir):
    """Load every vendor bundle and derive the full filament_id state.

    Returns a dict with the tree-derived snapshot sections plus the working data
    the checks and the assign pass need. All claims are "Vendor/Family" strings
    over INSTANTIATED system filaments, tree-wide including OFL and BBL.
    """
    profiles_dir = str(profiles_dir)
    vendor_names = list_vendor_names(profiles_dir)
    ofl_filaments, ofl_errors = (
        load_vendor_filaments(profiles_dir, OFL) if OFL in vendor_names else ({}, [])
    )
    ofl_declared = {r["filament_id"] for r in ofl_filaments.values() if r.get("filament_id")}

    vendors = {}
    read_errors = list(ofl_errors)
    for vendor in vendor_names:
        if vendor == OFL:
            filaments = ofl_filaments
        else:
            filaments, errs = load_vendor_filaments(profiles_dir, vendor)
            read_errors.extend(errs)
        for rec in filaments.values():
            eff, src, ofl_entry = resolve_filament_id(rec["name"], filaments, ofl_filaments)
            rec["eff_filament_id"] = eff
            rec["id_source"] = src
            rec["ofl_entry"] = ofl_entry
        vendors[vendor] = filaments

    # id -> set of "Vendor/Family" claims over instantiated presets. Every id
    # occurring in the tree is a key; ids only ever DECLARED (e.g. on a root
    # whose children all override them) keep an empty claim list, so that the
    # snapshot exactly equals the tree-derived state and the format/retirement
    # checks can grandfather them.
    ids = {}
    vendor_ids = {}             # vendor -> set of ids occurring there (declared or effective)
    declared_ids = {}           # vendor -> set of ids DECLARED in that vendor's own files
    instantiated_with_id = []   # "Vendor/PresetName" (own filament_id key on an instantiated preset)
    overrides = []              # (vendor, name, declared, inherited, file)
    missing_effective = []      # (vendor, name, file) instantiated presets resolving no id
    alias_candidates = []       # (vendor, rec, ofl_entry, own_key) presets riding an OFL family
    triples = {}                # fid -> set of triples of its declarers
    declarer_triples = []       # (vendor, rec, fid, triple) per declarer
    family_triples = {}         # (vendor, family) -> {triple: [declarer names]}

    for vendor, filaments in vendors.items():
        occurring = vendor_ids.setdefault(vendor, set())
        for rec in filaments.values():
            if rec.get("filament_id"):
                fid = rec["filament_id"]
                occurring.add(fid)
                declared_ids.setdefault(vendor, set()).add(fid)
                ids.setdefault(fid, set())
                triple = resolve_triple(rec["name"], filaments, ofl_filaments)
                rec["triple"] = triple
                declarer_triples.append((vendor, rec, fid, triple))
                triples.setdefault(fid, set()).add(triple)
                family_triples.setdefault(
                    (vendor, base_name(rec["name"])), {}).setdefault(
                    triple, []).append(rec["name"])
                if rec.get("inherits"):
                    inherited, _src, skip_entry = resolve_filament_id(
                        rec["name"], filaments, ofl_filaments, skip_own=True)
                    if inherited and inherited != fid:
                        overrides.append((vendor, rec["name"], fid, inherited, rec["file"]))
                    if (vendor != OFL and skip_entry and inherited
                            and inherited in ofl_declared):
                        alias_candidates.append((vendor, rec, skip_entry, True))
            if not rec["instantiation"]:
                continue
            eff = rec.get("eff_filament_id")
            if not eff:
                missing_effective.append((vendor, rec["name"], rec["file"]))
                continue
            occurring.add(eff)
            ids.setdefault(eff, set()).add(f"{vendor}/{base_name(rec['name'])}")
            if rec.get("filament_id"):
                instantiated_with_id.append(f"{vendor}/{rec['name']}")
            if vendor != OFL and rec["ofl_entry"] and eff in ofl_declared:
                alias_candidates.append((vendor, rec, rec["ofl_entry"], False))

    # Alias hygiene (check 4): a vendor preset that rides an OFL family (its id
    # resolves through OFL) is matched to the OFL preset by ALIAS (base name);
    # renaming it re-exposes the OFL preset and creates a live duplicate, empty
    # compatible_printers cannot claim any printer, and declaring its own
    # filament_id key forks the family off the catalog id.
    alias_violations = []       # (vendor, name, ofl_entry, reason, file)
    for vendor, rec, entry, own_key in alias_candidates:
        expected = base_name(entry)
        own_base = base_name(rec["name"])
        if own_key:
            alias_violations.append((
                vendor, rec["name"], entry,
                f'declares its own filament_id "{rec["filament_id"]}" — dropping the key '
                f"lets it resolve the OFL family's id through inherits",
                rec["file"]))
        elif own_base != expected:
            alias_violations.append((
                vendor, rec["name"], entry,
                f'base name "{own_base}" != "{expected}" — the rename re-exposes the '
                f"OFL preset on its printers (alias shadowing is name-based)",
                rec["file"]))
        elif not rec["compatible_printers"]:
            alias_violations.append((
                vendor, rec["name"], entry,
                "empty compatible_printers cannot shadow the OFL preset anywhere",
                rec["file"]))

    # Cross-bundle triple divergence (check 7, warning only): the same family
    # name declared in several bundles with different triples cannot converge
    # on one id until the divergence is fixed.
    name_bundles = {}
    for (vendor, family), tmap in family_triples.items():
        name_bundles.setdefault(family, {})[vendor] = frozenset(tmap)
    cross_bundle_triples = [
        (family, {v: sorted(ts) for v, ts in per_vendor.items()})
        for family, per_vendor in sorted(name_bundles.items())
        if len(per_vendor) > 1 and len(set(per_vendor.values())) > 1
    ]

    return {
        "vendors": vendors,
        "read_errors": read_errors,
        "ids": {fid: sorted(claims) for fid, claims in ids.items()},
        "vendor_ids": vendor_ids,
        "declared_ids": declared_ids,
        "instantiated_with_id": sorted(instantiated_with_id),
        "overrides": overrides,
        "id_overrides": sorted(f"{v}/{n}" for v, n, _d, _i, _f in overrides),
        "missing_effective": sorted(missing_effective),
        "alias_violations": alias_violations,
        "alias_exceptions": sorted(f"{v}/{n}" for v, n, _e, _r, _f in alias_violations),
        "triples": {fid: sorted(list(t) for t in ts) for fid, ts in triples.items()},
        "triple_sets": triples,
        "declarer_triples": declarer_triples,
        "family_triples": family_triples,
        "triple_exceptions": sorted(
            f"{v}/{f}" for (v, f), tmap in family_triples.items() if len(tmap) > 1),
        "cross_bundle_triples": cross_bundle_triples,
    }


# ---------------------------------------------------------------------------
# Snapshot IO
# ---------------------------------------------------------------------------

def snapshot_from_analysis(analysis):
    return {
        "ids": {fid: sorted(claims) for fid, claims in analysis["ids"].items()},
        "instantiated_with_id": analysis["instantiated_with_id"],
        "id_overrides": analysis["id_overrides"],
        "alias_exceptions": analysis["alias_exceptions"],
        "triples": analysis["triples"],
        "triple_exceptions": analysis["triple_exceptions"],
    }


def load_snapshot(path):
    """Return the snapshot dict, or None when the file does not exist."""
    if not os.path.exists(path):
        return None
    data = load_json(path)
    for key in ("ids", "triples"):
        data.setdefault(key, {})
    for key in ("instantiated_with_id", "id_overrides", "alias_exceptions",
                "triple_exceptions"):
        data.setdefault(key, [])
    return data


def write_snapshot(path, obj):
    """Deterministic serialization: sorted keys, indent 1, LF, trailing newline."""
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(obj, f, indent=1, ensure_ascii=False, sort_keys=True)
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
    2. Snapshot equality, both directions: the tree-derived id->families multimap
       AND the id->triples map of the declarers must equal the snapshot exactly
       (the snapshot diff is the maintainer gate).
    3. Mint conformance: an OF-format declaration must equal the mint of the
       declarer's triple or a salted iteration, unless that exact (id, triple)
       pair is grandfathered in the snapshot.
    4. Alias hygiene: a vendor preset riding an OFL family must keep the OFL
       base name, claim printers via non-empty compatible_printers, and declare
       no filament_id key of its own.
    5. Reserved namespaces (GF*/QD_*/P-hex/"null", all ownerless) must not be
       claimed by any vendor, except claims grandfathered in the snapshot.
    6. Structure ratchet: (a) no NEW instantiated preset carries its own
       filament_id key; (b) no NEW declared-vs-inherited id drift; (c) every
       instantiated filament resolves an effective id (a hard load error in C++).
    7. Triple integrity: (a) every declarer resolves non-empty filament_vendor
       and filament_type (hard error, no grandfathering); (b) declarers of one
       (bundle, family) resolve identical triples, unless grandfathered in
       snapshot triple_exceptions; cross-bundle divergence on the same family
       name is a warning only.
    8. Bambu catalog map: resources/printers/bambu_filament_ids.json must parse,
       carry source/bambustudio_commit/generated, key only OF-format ids, map
       each Bambu id at most once, and for every row whose key the tree claims,
       the tree's triple for that id must equal the row's (vendor, type, name).
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
                f'family ids must come from "python scripts/assign_filament_ids.py" '
                f'(see --mint)')
            errors += 1

    # -- 2. snapshot equality (both directions) -----------------------------
    for fid in sorted(tree_ids):
        if fid not in snap_ids:
            print_error(
                f'filament_id "{fid}" is not sanctioned by '
                f"scripts/filament_id_snapshot.json; {UPDATE_HINT}")
            errors += 1
            continue
        for claim in tree_ids[fid]:
            if claim not in snap_ids[fid]:
                print_error(
                    f'filament_id "{fid}" claim "{claim}" is not sanctioned by '
                    f"scripts/filament_id_snapshot.json; {UPDATE_HINT}")
                errors += 1
    for fid in sorted(snap_ids):
        if fid not in tree_ids:
            print_error(
                f'filament_id stability: snapshot id "{fid}" vanished from the tree; '
                f"{UPDATE_HINT}")
            errors += 1
            continue
        for claim in snap_ids[fid]:
            if claim not in tree_ids[fid]:
                print_error(
                    f'filament_id stability: snapshot claim "{claim}" of id "{fid}" '
                    f"vanished from the tree; {UPDATE_HINT}")
                errors += 1
    # ... and the declared triples, both directions.
    snap_triples = snapshot["triples"]
    tree_triples = analysis["triples"]
    for fid in sorted(tree_triples):
        for t in tree_triples[fid]:
            if t not in snap_triples.get(fid, []):
                print_error(
                    f'filament_id "{fid}" triple "{"/".join(t)}" is not sanctioned by '
                    f"scripts/filament_id_snapshot.json; {UPDATE_HINT}")
                errors += 1
    for fid in sorted(snap_triples):
        for t in snap_triples[fid]:
            if t not in tree_triples.get(fid, []):
                print_error(
                    f'filament_id triple stability: snapshot triple "{"/".join(t)}" of '
                    f'id "{fid}" vanished from the tree; {UPDATE_HINT}')
                errors += 1

    # -- 3. mint conformance for OF-format declarations ----------------------
    for vendor, rec, fid, triple in sorted(
            analysis["declarer_triples"], key=lambda x: (x[0], x[1]["file"])):
        if not OF_ID_RE.match(fid):
            continue
        if list(triple) in snap_triples.get(fid, []):
            continue  # grandfathered (id, triple) pair
        minted = [generate_filament_id(*triple, salt=s) for s in range(MAX_CHECK_SALT + 1)]
        if fid not in minted:
            print_error(
                f'filament_id "{fid}" declared by "{rec["name"]}" ({rec["file"]}) does '
                f'not match the mint of its triple "{"/".join(triple)}": expected '
                f'"{minted[0]}" (or a salted iteration); paste the expected id into the '
                f"family root (or, for an intentionally kept id, {UPDATE_HINT})")
            errors += 1

    # -- 4. alias hygiene for presets riding OFL families --------------------
    exceptions = set(snapshot["alias_exceptions"])
    for vendor, name, entry, reason, file in analysis["alias_violations"]:
        if f"{vendor}/{name}" in exceptions:
            continue
        print_error(
            f'preset "{name}" ({file}) rides the OFL family "{entry}" but {reason}; '
            f"a vendor specialization keeps the OFL base name, sets non-empty "
            f"compatible_printers and declares no filament_id key — or the family "
            f"gets its own minted id")
        errors += 1

    # -- 5. reserved namespaces ----------------------------------------------
    for fid in sorted(tree_ids):
        is_reserved, owner = reserved_space_owner(fid)
        if not is_reserved:
            continue
        for claim in tree_ids[fid]:
            vendor = claim.split("/", 1)[0]
            if vendor == owner:
                continue
            if claim in snap_ids.get(fid, []):
                continue  # grandfathered
            space = reserved_space_desc(fid, owner)
            print_error(
                f'filament_id "{fid}" of "{claim}" is in a reserved id space '
                f"({space}) and must not be claimed by system presets of other vendors")
            errors += 1

    # -- 6. structure ratchet -------------------------------------------------
    grandfathered = set(snapshot["instantiated_with_id"])
    for key in analysis["instantiated_with_id"]:
        if key not in grandfathered:
            print_error(
                f'instantiated preset "{key}" declares its own filament_id key; the key '
                f"belongs on the family root preset only (variants inherit it)")
            errors += 1
    grandfathered = set(snapshot["id_overrides"])
    for vendor, name, declared, inherited, file in analysis["overrides"]:
        if f"{vendor}/{name}" in grandfathered:
            continue
        print_error(
            f'preset "{name}" ({file}) declares filament_id "{declared}" but its '
            f'inherits chain resolves "{inherited}"; a preset must not override its '
            f"family's id")
        errors += 1
    ofl_map = analysis["vendors"].get(OFL, {})
    for vendor, name, file in analysis["missing_effective"]:
        triple = resolve_triple(name, analysis["vendors"][vendor], ofl_map)
        expected = generate_filament_id(*triple)
        print_error(
            f'instantiated filament "{name}" ({file}) resolves no filament_id anywhere '
            f"in its inherits chain — this is a hard load error in the C++ loader; "
            f'run "python scripts/assign_filament_ids.py" (expected id for family '
            f'"{vendor}/{base_name(name)}": "{expected}", salted if taken)')
        errors += 1

    # -- 7. triple integrity ---------------------------------------------------
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
    triple_exceptions = set(snapshot["triple_exceptions"])
    for (vendor, family), tmap in sorted(analysis["family_triples"].items()):
        if len(tmap) < 2 or f"{vendor}/{family}" in triple_exceptions:
            continue
        detail = "; ".join(
            f'"{"/".join(t)}" ({", ".join(sorted(names))})'
            for t, names in sorted(tmap.items()))
        print_error(
            f'family "{vendor}/{family}" declarers resolve divergent triples: {detail}; '
            f"declarers of one family must agree on (filament_vendor, filament_type)")
        errors += 1
    for family, per_vendor in analysis["cross_bundle_triples"]:
        detail = "; ".join(
            f'{v}: {", ".join("/".join(t) for t in ts)}'
            for v, ts in sorted(per_vendor.items()))
        print_warning(
            f'family name "{family}" resolves different triples across bundles '
            f"({detail}); bundles of one product converge on one id only once "
            f"their triples agree")

    # -- 8. Bambu catalog map --------------------------------------------------
    try:
        bambu_map = load_json(map_path)
    except (OSError, ValueError) as e:
        print_error(f"Bambu catalog map {map_path} does not parse ({e}); {BAMBU_MAP_HINT}")
        errors += 1
    else:
        for key in ("source", "bambustudio_commit", "generated"):
            if not bambu_map.get(key):
                print_error(f'Bambu catalog map {map_path} is missing "{key}"; {BAMBU_MAP_HINT}')
                errors += 1
        rows = bambu_map.get("filaments", {})
        bambu_id_owners = {}
        for fid, row in sorted(rows.items()):
            if not OF_ID_RE.match(fid):
                print_error(f'Bambu catalog map key "{fid}" is not a minted "OF" id; '
                           f"{BAMBU_MAP_HINT}")
                errors += 1
            bambu_id = row.get("bambu_id")
            if bambu_id in bambu_id_owners:
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

def update_snapshot(profiles_dir=PROFILES_DIR, snapshot_path=SNAPSHOT_PATH,
                    allow_shared_catalog=False):
    """Regenerate the snapshot from the tree.

    Refuses to sanction NEW reserved-namespace ids (or new claims on them) for
    non-owner vendors unless --allow-shared-catalog is passed. Idempotent: a
    second run over an unchanged tree changes nothing. Returns 0 on success.
    """
    analysis = analyze_tree(profiles_dir)
    for msg in analysis["read_errors"]:
        print_error(msg)
    new_snap = snapshot_from_analysis(analysis)
    old_snap = load_snapshot(snapshot_path)
    old_ids = old_snap["ids"] if old_snap else {}

    # Gate: new reserved-namespace ids / claims for non-owner vendors.
    refusals = []
    for fid, claims in sorted(new_snap["ids"].items()):
        is_reserved, owner = reserved_space_owner(fid)
        if not is_reserved:
            continue
        for claim in claims:
            if claim in old_ids.get(fid, []):
                continue
            vendor = claim.split("/", 1)[0]
            if vendor == owner:
                continue
            refusals.append((fid, claim, owner))
        if not claims and fid not in old_ids:
            # Declared-only new id: attribute it to its declaring vendor(s).
            for vendor in sorted(analysis["vendor_ids"]):
                if fid in analysis["vendor_ids"][vendor] and vendor != owner:
                    refusals.append((fid, f"{vendor}/(declared only)", owner))
    if refusals and not allow_shared_catalog:
        for fid, claim, owner in refusals:
            space = reserved_space_desc(fid, owner)
            print_error(
                f'refusing to sanction new claim "{claim}" on reserved-namespace id '
                f'"{fid}" ({space}); pass --allow-shared-catalog only for '
                f"maintainer-approved shared-catalog families")
        return 1

    # Diff summary.
    added_ids = sorted(set(new_snap["ids"]) - set(old_ids))
    removed_ids = sorted(set(old_ids) - set(new_snap["ids"]))
    added_claims = sum(
        len(set(claims) - set(old_ids.get(fid, [])))
        for fid, claims in new_snap["ids"].items())
    removed_claims = sum(
        len(set(claims) - set(new_snap["ids"].get(fid, [])))
        for fid, claims in old_ids.items())
    old_snap = old_snap or {"ids": {}, "instantiated_with_id": [], "id_overrides": [],
                            "alias_exceptions": [], "triples": {},
                            "triple_exceptions": []}
    changed = new_snap != old_snap

    if changed:
        write_snapshot(snapshot_path, new_snap)

    print_info(f"snapshot ids      : {len(new_snap['ids'])} (+{len(added_ids)} / -{len(removed_ids)})")
    print_info(f"claims added      : {added_claims}")
    print_info(f"claims removed    : {removed_claims}")
    print_info(f"declared triples  : {len(new_snap['triples'])}")
    for section in ("instantiated_with_id", "id_overrides", "alias_exceptions",
                    "triple_exceptions"):
        before, after = len(old_snap.get(section, [])), len(new_snap[section])
        print_info(f"{section:<18}: {after} ({after - before:+d})")
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
# Default run: mint + insert ids for id-less families
# ---------------------------------------------------------------------------

def assign_missing_ids(profiles_dir=PROFILES_DIR, snapshot_path=SNAPSHOT_PATH):
    """Mint + insert ids for id-less families; mint + replace non-OF-format
    declarations. Never rewrites a valid (OF-format) existing id.

    A family = (vendor, base name) group over instantiated filaments with no
    effective id. The minted id is a pure function of the family's triple —
    (filament_vendor, filament_type) resolved on the root(s), family name — and
    is inserted into the family's root(s): the presets its members inherit that
    carry no id, or the member itself when it has no vendor-side parent.

    A declaration whose value is not OF-format (e.g. a vendor bundle synced
    from an upstream source that ships its own catalog ids, such as BBL's GF*)
    is treated the same as a missing family: a fresh id is minted for the
    declarer's triple and the value is replaced in place (declarers that share
    one triple across several per-printer roots converge on the same id, same
    as the multi-root families above). This is what makes a future BBL sync
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

    # Group id-less instantiated presets into families.
    families = {}  # (vendor, family) -> [rec]
    for vendor, name, _file in analysis["missing_effective"]:
        rec = analysis["vendors"][vendor][name]
        if rec["id_source"] in ("cycle", "dangling"):
            print_error(f'cannot mint for "{vendor}/{name}": broken inherits chain '
                        f'({rec["id_source"]})')
            errors += 1
            continue
        families.setdefault((vendor, base_name(name)), []).append(rec)

    # Declarations whose value is not OF-format: treated as missing too (see
    # docstring). Grouped by triple, not by (vendor, family), so declarers
    # that legitimately share one triple across several files converge on one
    # freshly minted id instead of each getting their own.
    non_of_declarers = [
        (vendor, rec, fid, triple) for vendor, rec, fid, triple in analysis["declarer_triples"]
        if not OF_ID_RE.match(fid)]

    if not families and not non_of_declarers:
        print_success("every instantiated filament already resolves an OF-format "
                      "filament_id; nothing to do (0 files changed)")
        return 0, errors

    # Ids already spoken for: the whole tree (declared or effective) + snapshot.
    snapshot = load_snapshot(snapshot_path) or {"ids": {}}
    taken = set(snapshot["ids"])
    for occurring in analysis["vendor_ids"].values():
        taken |= occurring

    # Family root(s): the direct vendor-side parents of the members (id-less by
    # construction), or the member itself when it has none.
    roots = {}  # (vendor, family) -> {preset name: rec}
    root_claims = {}  # (vendor, root name) -> set of families wanting to write it
    for key, members in sorted(families.items()):
        vendor = key[0]
        vendor_map = analysis["vendors"][vendor]
        family_roots = {}
        for rec in members:
            parent = rec.get("inherits")
            root = vendor_map.get(parent) if parent else None
            if root is None or root.get("filament_id"):
                root = rec  # root-less member carries the id itself
            family_roots[root["name"]] = root
            root_claims.setdefault((vendor, root["name"]), set()).add(key)
        roots[key] = family_roots

    ofl_map = analysis["vendors"].get(OFL, {})
    files_changed = 0
    families_minted = 0
    for key, family_roots in sorted(roots.items()):
        vendor, family = key
        shared = [n for n in family_roots
                  if len(root_claims[(vendor, n)]) > 1]
        if shared:
            others = sorted({f"{v}/{f}" for n in shared
                             for (v, f) in root_claims[(vendor, n)] if (v, f) != key})
            print_error(
                f'cannot mint for family "{vendor}/{family}": root(s) '
                f"{sorted(shared)} are shared with famil(ies) {others}; split the "
                f"roots so each family has its own")
            errors += 1
            continue
        vendor_map = analysis["vendors"][vendor]
        fields = {(resolve_filament_field(n, "filament_vendor", vendor_map, ofl_map),
                   resolve_filament_field(n, "filament_type", vendor_map, ofl_map))
                  for n in family_roots}
        if len(fields) > 1:
            print_error(
                f'cannot mint for family "{vendor}/{family}": its roots resolve '
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
                f'cannot mint for family "{vendor}/{family}": it resolves empty '
                f'{missing}; the mint key needs both (generic materials use '
                f'filament_vendor "Generic")')
            errors += 1
            continue
        new_id = mint_filament_id(fvendor, ftype, family, taken)
        taken.add(new_id)
        families_minted += 1
        for name in sorted(family_roots):
            root = family_roots[name]
            write_filament_id(root["path"], new_id)
            files_changed += 1
            print_info(f'family "{vendor}/{family}": filament_id "{new_id}" -> {root["file"]}')

    # Non-OF-format declarations: mint once per triple, rewrite every declarer
    # that shares it (see docstring).
    declarations_reminted = 0
    assigned = {}  # triple -> id chosen this run
    for vendor, rec, fid, triple in sorted(non_of_declarers, key=lambda x: (x[0], x[1]["file"])):
        fvendor, ftype, family = triple
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
            assigned[triple] = mint_filament_id(fvendor, ftype, family, taken)
            taken.add(assigned[triple])
        new_id = assigned[triple]
        rewrite_filament_id(rec["path"], fid, new_id)
        files_changed += 1
        declarations_reminted += 1
        print_info(f'family "{vendor}/{family}": non-OF filament_id "{fid}" -> "{new_id}" '
                   f'({rec["file"]})')

    print_info(f"families minted  : {families_minted}")
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
            if fid in {generate_filament_id(*rec["triple"], salt=s)
                       for s in range(MAX_CHECK_SALT + 1)}:
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
    family's id path: ignoring its own key, the preset's inherits chain enters
    OFL and resolves an OFL-declared id, and the preset keeps the OFL family's
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
        description="Mint deterministic filament_id values for id-less filament "
                    "families and validate the tree against the sanctioned snapshot.")
    parser.add_argument("--mint", metavar='"Vendor/Type/Family"',
                        help="print the id the (filament_vendor, filament_type, "
                             "family name) triple would mint; touches nothing")
    parser.add_argument("--update-snapshot", action="store_true",
                        help="regenerate scripts/filament_id_snapshot.json from "
                             "the tree")
    parser.add_argument("--check", action="store_true",
                        help="run the filament_id checks; exit nonzero on errors")
    parser.add_argument("--allow-shared-catalog", action="store_true",
                        help="with --update-snapshot: allow sanctioning new claims "
                             "on reserved-namespace ids for non-owner vendors")
    parser.add_argument("--remint", metavar="VENDOR", action="append",
                        help="re-derive VENDOR's declared filament_ids from their "
                             "triples and rewrite mismatches in place; repeatable")
    parser.add_argument("--drop-redundant-ids", metavar="VENDOR",
                        help="delete filament_id declarations in VENDOR that "
                             "re-declare the OFL family id they already resolve "
                             "through inherits")
    parser.add_argument("--profiles", default=PROFILES_DIR,
                        help="profiles directory (default: resources/profiles)")
    args = parser.parse_args(argv)


    if args.mint:
        parts = args.mint.split("/", 2)
        if len(parts) != 3 or not all(parts):
            parser.error('--mint expects "filament_vendor/filament_type/family_name" '
                         "with all three components non-empty (check 7 rejects empty "
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
        return update_snapshot(args.profiles, SNAPSHOT_PATH,
                               allow_shared_catalog=args.allow_shared_catalog)

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
