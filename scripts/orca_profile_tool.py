#!/usr/bin/env python3
"""
Every maintenance job for the OrcaSlicer system profile tree, in one tool.

usage: python scripts/orca_profile_tool.py <command> [options]

commands:
  check            validate the whole tree -- what CI runs
  fix-variant      resize the variant arrays check rejects
  generate-id      write the filament_id / setting_id each profile's identity implies
  normalize        rewrite profile files into their canonical shape
  trim             delete profile files no <vendor>.json list references
  update-index     regenerate the *_list sections of <vendor>.json

options shared by several commands:
  --vendor VENDOR      act on one vendor bundle only; repeatable, empty means all
  --profile-type TYPE  one of machine_model/process/filament/machine; repeatable
                       (normalize, trim, update-index)
  --dry-run            report what would change and write nothing (every command
                       that writes)
  --profiles DIR       act on another profile tree (default: resources/profiles)

After adding, renaming or deleting profile files, run:
  normalize -> update-index -> generate-id -> check
normalize supplies missing types; update-index registers presets before id
generation.
Use trim only for deliberate cleanup, previewed with --dry-run: it judges against
the current index and can delete newly added, unindexed presets.

Run from anywhere; "python scripts/orca_profile_tool.py --help" repeats this list
and "... <command> --help" documents one command in full.

The two id rules are the heart of it. Both ids are pure functions of the thing
they name, so nothing here is ever invented: the tool only writes the id the
rules below already imply, and a tree that already satisfies them is left
untouched.

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
    hand, and nothing but the triple feeds the mint — not the rest of the tree.
    Two products whose triples mint one id (a base62
    collision; odds ~1e-5 over the whole tree) is an error --check reports and
    --generate refuses to write; the remedy is a rename so the triples differ,
    never a salted or hand-picked second id.
    Identity changes (a filament rename, a filament_vendor/filament_type fix)
    change the id BY DESIGN.
  * EVERY filament profile carries a minted id, with no exceptions and no
    spellings held back for anyone. Ids that other systems compose for their own
    purposes are simply not mints, so no system profile can carry one and there
    is nothing to reserve: Bambu's GF* catalog ids (the generated
    resources/printers/bambu_filament_ids.json records the correspondence, which
    the app applies at the printer boundary), the QD_* ids a Qidi box composes at
    runtime, and the P+7-hex ids CreatePresetsDialog.cpp gives user-created
    filaments all fail the format rule like any other stray value.

setting_id policy (see AGENTS.md "Critical Constraints"):
  * setting_id is a PRESET id, a pure function of the preset's identity:
        setting_id = base62_16( uuid5(NAMESPACE, "<vendor>/<type>/<name>") )
    The same value is recomputed on the fly by the C++ app
    (Slic3r::generate_preset_setting_id); the two MUST stay byte-identical.
    Uniqueness is therefore automatic: two presets collide only if they share
    vendor + type + name, which "check" flags.
  * Only instantiated presets (instantiation == "true") carry a setting_id;
    base / template profiles do not.
  * Bambu (BBL) owns the authoritative "G*" setting_id space and is the only
    reserved vendor: its setting_ids are never rewritten, which keeps
    Bambu-synced presets backward compatible. That exemption is setting_id's
    alone — BBL's filament_ids are minted like every other vendor's.

The effective-id resolution below is loader-faithful (PresetBundle.cpp
load_vendor_configs_from_json): own filament_id key, else walk `inherits` within
the vendor map, with OrcaFilamentLibrary base-bundle fallback; once a chain enters
OFL it stays in OFL; a vendor chain that dead-ends id-less retries its direct
parent in the OFL map. filament_vendor / filament_type resolve the same way.
"""

import argparse
import functools
import json
import os
import posixpath
import re
import sys
import uuid
from collections import Counter, defaultdict
from pathlib import Path

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
# The single source of truth for the map path; update_bambu_filament_ids.py
# imports this rather than recomputing it.
BAMBU_MAP_PATH = os.path.normpath(
    os.path.join(SCRIPTS_DIR, "..", "resources", "printers", "bambu_filament_ids.json"))

OFL = "OrcaFilamentLibrary"
# The validator's data dir, created under resources/profiles by a local run;
# not a vendor bundle, so an unscoped pass leaves it alone.
USER_DIR = "user"

# Bambu (BBL) is the only vendor exempt from the setting_id rule: it keeps its
# authoritative "G*" cloud ids. No vendor is exempt from the filament_id rule.
RESERVED_VENDORS = {"BBL"}

# The profile types that carry a setting_id; the subdir name is also the type
# name, matching Preset::get_type_string() on the C++ side.
PROFILE_SUBDIRS = ("filament", "process", "machine")

# The profile kinds a <vendor>.json indexes, each under its "<type>_list" section.
# NOT the same thing as PROFILE_SUBDIRS above, and deliberately not merged with it:
# machine_model is an index section whose files live in the machine/ directory, and
# this order is the order the sections are rebuilt (and therefore written) in.
PROFILE_TYPES = ("machine_model", "process", "filament", "machine")

# Data files that sit under a vendor bundle but are not presets: no name, no type.
NON_PROFILE_FILES = {
    "filaments_color_codes.json",
    "cli_config.json",
    "filament_id_map.json",
    "filament_name_map.json",
    "support_recommended_params.json",
}

# Mirror PrintConfigDef::handle_legacy's ignore set in PrintConfig.cpp; a test
# checks parity. Used by normalize and check. Active options and
# legacy aliases that the loader migrates do not belong here.
OBSOLETE_KEYS = {
    "acceleration", "scale", "rotate", "duplicate", "duplicate_grid",
    "bed_size", "print_center", "g0", "wipe_tower_per_color_wipe",
    "support_sharp_tails", "support_remove_small_overhangs", "support_with_sheath",
    "tree_support_collision_resolution", "tree_support_with_infill",
    "max_volumetric_speed", "max_print_speed", "support_closing_radius",
    "remove_freq_sweep", "remove_bed_leveling", "remove_extrusion_calibration",
    "support_transition_line_width", "support_transition_speed", "bed_temperature",
    "bed_temperature_initial_layer", "can_switch_nozzle_type", "can_add_auxiliary_fan",
    "extra_flush_volume", "spaghetti_detector", "adaptive_layer_height",
    "z_hop_type", "z_lift_type", "bed_temperature_difference", "long_retraction_when_cut",
    "retraction_distance_when_cut", "internal_bridge_support_thickness",
    "top_area_threshold", "reduce_wall_solid_infill",
    "filament_load_time", "filament_unload_time", "smooth_coefficient",
    "overhang_totally_speed", "silent_mode", "overhang_speed_classic",
    "anisotropic_surfaces",
}

# Keys renamed at some point, whose old and new spellings must never co-exist:
# the loader would pick one arbitrarily. extruder_clearance_radius vs
# extruder_clearance_max_radius decides toolhead collision avoidance.
CONFLICT_KEYS = [
    ["extruder_clearance_radius", "extruder_clearance_max_radius"],
]

# Options the config system stores as vectors; a scalar there is a silent misload.
VECTOR_KEYS = {
    "filament_type",
}

PRINT_CONFIG_CPP = os.path.normpath(
    os.path.join(SCRIPTS_DIR, "..", "src", "libslic3r", "PrintConfig.cpp"))


@functools.lru_cache(maxsize=None)
def _variant_scheme():
    """Per config preset type: the key listing the preset's own variants, and the
    stride of every key sized by it.

    Read from the four variant key sets in PrintConfig.cpp, the std::set
    initializers DynamicPrintConfig::get_parameter_size sizes by, so membership is
    the engine's own and not guessable from names. Each holds one value per variant;
    printer_options_with_variant_2, the machine_max_* limits, holds a (normal,
    silent) pair per variant, stride 2. Flow override keys added by
    with_filament_flow_overrides come from PrintConfig.hpp.
    """
    with open(PRINT_CONFIG_CPP, encoding="utf-8") as f:
        source = f.read()

    def members(name):
        match = re.search(
            r"std::set<std::string>\s+" + name + r"\s*=\s*"
            r"(?:(with_filament_flow_overrides)\(\s*std::set<std::string>\s*)?"
            r"\{(.*?)\}\s*\)?\s*;", source, re.DOTALL)
        if match is None:
            raise RuntimeError(f"{PRINT_CONFIG_CPP} no longer defines {name}")
        # An initializer can carry a commented-out entry (filament_extruder_id).
        body = re.sub(r"//[^\n]*|/\*.*?\*/", "", match.group(2), flags=re.DOTALL)
        keys = set(re.findall(r'"([^"\n]+)"', body))
        if match.group(1):
            header_path = os.path.splitext(PRINT_CONFIG_CPP)[0] + ".hpp"
            with open(header_path, encoding="utf-8") as f:
                header = f.read()
            overrides = re.search(r"\bflow_ratio_overrides\s*=\s*\{\{(.*?)\}\};",
                                  header, re.DOTALL)
            if overrides is None:
                raise RuntimeError(f"{header_path} no longer defines flow_ratio_overrides")
            body = re.sub(r"//[^\n]*|/\*.*?\*/", "", overrides.group(1), flags=re.DOTALL)
            keys.update("filament_" + key for key in re.findall(r'\{\s*"([^"\n]+)"\s*,', body))
        return sorted(keys)

    return {
        "machine": ("printer_extruder_variant", {
            **dict.fromkeys(members("printer_options_with_variant_1"), 1),
            **dict.fromkeys(members("printer_options_with_variant_2"), 2)}),
        "process": ("print_extruder_variant",
                    dict.fromkeys(members("print_options_with_variant"), 1)),
        "filament": ("filament_extruder_variant",
                     dict.fromkeys(members("filament_options_with_variant"), 1)),
    }


@functools.lru_cache(maxsize=None)
def _variant_names():
    """The parts a variant string is built from, read from PrintConfig.cpp.

    A variant is selected by exact string compare of "<extruder type> <nozzle volume
    type>", so what is legal is the two enum maps the engine builds that string from,
    not the list of strings some profile happens to ship. Returns (extruder types,
    writable volume types, variant-name rewrites, extruder_type rewrites): the volume
    types minus RUNTIME_VOLUME_TYPES, and the spellings PrintConfigDef::handle_legacy
    rewrites in the six variant keys. The loader still accepts such a spelling, but a
    profile must write the enum name, so the rewrite only names that name in the error;
    it is read rather than restated because it belongs to the engine too.
    """
    with open(PRINT_CONFIG_CPP, encoding="utf-8") as f:
        source = f.read()

    def enum_map(name):
        match = re.search(r"t_config_enum_values\s+" + name + r"\s*=\s*\{(.*?)\};",
                          source, re.DOTALL)
        if match is None:
            raise RuntimeError(f"{PRINT_CONFIG_CPP} no longer defines {name}")
        body = re.sub(r"//[^\n]*|/\*.*?\*/", "", match.group(1), flags=re.DOTALL)
        return set(re.findall(r'"([^"\n]+)"', body))

    def rewrites(anchor):
        """The (old, new) ReplaceString pairs of handle_legacy's branch for one key.

        An absent branch means the loader rewrites nothing, which is a defined answer
        rather than a broken parse: every legacy name is then reported as unknown.
        """
        match = re.search(r'opt_key == "' + anchor + r'"\s*\)\s*\{(.*?)\}',
                          source, re.DOTALL)
        return ({} if match is None else
                dict(re.findall(r'ReplaceString\(value,\s*"([^"]+)",\s*"([^"]+)"\)',
                                match.group(1))))

    return (enum_map("s_keys_map_ExtruderType"),
            enum_map("s_keys_map_NozzleVolumeType") - RUNTIME_VOLUME_TYPES,
            rewrites("extruder_variant_list"),
            rewrites("extruder_type"))


# What a preset that writes no variant layout takes: the default extruder_type and
# nozzle volume type (PrintConfig.cpp defaults for extruder_type and
# default_nozzle_volume_type), one such variant per extruder on a machine
# (extend_extruder_variant) and a single one on a process or filament.
DEFAULT_EXTRUDER_TYPE = "Direct Drive"
DEFAULT_NOZZLE_VOLUME_TYPE = "Standard"
DEFAULT_VARIANT = f"{DEFAULT_EXTRUDER_TYPE} {DEFAULT_NOZZLE_VOLUME_TYPE}"

# A nozzle volume type the engine computes for a hybrid extruder at runtime, and no
# profile ever writes: s_keys_map_NozzleVolumeType carries it for the several
# sub-nozzles of one extruder, which no single variant string can name.
RUNTIME_VOLUME_TYPES = {"Hybrid"}

OF_ID_RE = re.compile(r"^OF[0-9A-Za-z]{6}$")
# Filament name = preset base name: strip the first "@..." suffix. The space before
# "@" is optional because names like "Afinia PLA@HS" exist.
BASE_NAME_RE = re.compile(r"\s?@.*$")
# A JSON string literal, for the byte-preserving key edits.
_JSON_STR = r'"(?:[^"\\]|\\.)*"'

GENERATE_CMD = "python scripts/orca_profile_tool.py generate-id"
SETTING_ID_CMD = '"python scripts/orca_profile_tool.py generate-id --setting-id"'
BAMBU_MAP_HINT = 'regenerate the map with "python scripts/update_bambu_filament_ids.py" and commit the diff for maintainer review'
NORMALIZE_HINT = 'try "python scripts/orca_profile_tool.py normalize" to fix common issues automatically'

# What to do about a defect check found, keyed by defect. check prints each of these
# ONCE for the whole run, after the files themselves: a bundle that forgot to index
# twenty presets needs the remedy spelled out once, not twenty times over the one list
# a maintainer has to read.
REMEDY_HINTS = {
    "unindexed": 'unreferenced file(s) above: delete them, or run "python scripts/'
                 'orca_profile_tool.py update-index" to add them to their <vendor>.json',
    "unindexable": 'unreferenced file(s) above declare no profile type: run "python '
                   'scripts/orca_profile_tool.py normalize" to write one so '
                   "update-index can place them, or delete them",
    "unnormalized": 'profile file(s) above are not what "python scripts/'
                    'orca_profile_tool.py normalize" writes: run it and commit the result',
    "stale_index": 'vendor index(es) above are not what "python scripts/'
                   'orca_profile_tool.py update-index" writes: run it and commit the '
                   "result",
}


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
    golden vectors (tests/libslic3r/test_preset_setting_id.cpp) and by every
    filament_id in the tree — never change it.
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

class DuplicateKeyError(ValueError):
    """A profile declares one key twice; json would silently keep the last."""


def _no_duplicates_hook(pairs):
    seen = {}
    for key, value in pairs:
        if key in seen:
            raise DuplicateKeyError(f"Duplicate key detected: {key}")
        seen[key] = value
    return seen


def load_json(path, detect_duplicates=False):
    """Parse a profile file. BOM-tolerant, because some vendors ship one.

    detect_duplicates is opt-in rather than always on: only the checks that have
    always had it use it, since switching it on everywhere would newly reject files
    the tree has always accepted, and switching it off would lose real coverage.
    """
    with open(path, "r", encoding="utf-8-sig") as f:
        return json.load(f, object_pairs_hook=_no_duplicates_hook if detect_duplicates
                         else None)


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

    What the setting_id pass and the per-vendor checks walk: setting_id is a
    per-file property, so a bundle whose index has not landed yet must still be
    assignable — otherwise check flags files generate-id refuses to touch.
    (filament_id is driven by each bundle's filament_list instead, hence
    list_vendor_names above.)
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
    Templates pulled in by `include` are not consulted: none states either field.
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

    Returns a dict of the tree-derived state the checks and the assign pass need,
    tree-wide including OFL and BBL.
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
            if not rec.get("filament_id") and OF_ID_RE.match(eff):
                inherited.append((vendor, rec, eff, triple))

    # Cross-bundle triple divergence (check 3, warning only): the same filament
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
# filament_id validation
# ---------------------------------------------------------------------------

def check_filament_ids(profiles_dir=PROFILES_DIR, map_path=BAMBU_MAP_PATH):
    """Validate filament_id state across every vendor. Returns the error count.

    1. Format: every id occurring in the tree (declared or effective) must
       match ^OF[0-9A-Za-z]{6}$. No exceptions, not even BBL.
    2. Identity: the id is a function of the triple alone, and there is no
       second acceptable value. (a) A declared id must equal the one id the
       declarer's own triple mints; (b) the id an instantiated preset inherits
       must equal the one ITS own triple mints — how it inherits it (a root, a
       real filament, an OFL preset) is irrelevant; (c) every instantiated
       filament resolves an effective id at all (an id-less one is a hard load
       error in C++); (d) no two products mint one id (a base62 collision,
       resolved by renaming one of them).
    3. Triple integrity: (a) every declarer resolves non-empty filament_vendor
       and filament_type; (b) declarers of one (bundle, filament) resolve
       identical triples; cross-bundle divergence on the same filament name is a
       warning only.
    4. Bambu catalog map: resources/printers/bambu_filament_ids.json must parse,
       carry source/bambustudio_commit/generated, key only OF-format ids, map
       each Bambu id at most once, and for every row whose key the tree claims,
       the tree's triple for that id must equal the row's (vendor, type, name).
    """
    _utf8_console()
    errors = 0
    analysis = analyze_tree(profiles_dir)
    for msg in analysis["read_errors"]:
        print_error(msg)
        errors += 1

    # -- 1. format ----------------------------------------------------------
    for vendor in sorted(analysis["vendor_ids"]):
        for fid in sorted(analysis["vendor_ids"][vendor]):
            if OF_ID_RE.match(fid):
                continue
            print_error(
                f'filament_id "{fid}" ({vendor}) is not a minted "OF" id; new '
                f'filament ids must come from "{GENERATE_CMD}"')
            errors += 1

    # -- 2. identity: the id is a function of the triple alone ---------------
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
            f'"{GENERATE_CMD} --vendor {vendor}" (preview with --dry-run)')
        errors += 1
    # (2b) An inherited id is held to the same single value, and every preset
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
    # (2d) The mint is injective over the tree's products, or two of them are
    # indistinguishable to every device that matches on the id.
    for fid, ts in sorted(analysis["collisions"].items()):
        print_error(
            f'filament_id "{fid}" is the mint of {len(ts)} different products '
            f'({"; ".join("/".join(t) for t in ts)}): a base62 collision; rename one '
            f"of them so their triples differ")
        errors += 1

    # -- 3. triple integrity ---------------------------------------------------
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

    # -- 4. Bambu catalog map --------------------------------------------------
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
            claimed = analysis["triples"].get(fid)
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
# setting_id validation
# ---------------------------------------------------------------------------

def check_setting_id_uniqueness(profiles_dir):
    """Validate setting_id across every vendor. Returns the error count.

      1. Every instantiated preset must HAVE a setting_id.            (all vendors)
      2. A stored setting_id must equal generate_preset_setting_id(vendor, type,
         name); a stale value means the JSON was edited without rerunning
         generate-id.               (all vendors EXCEPT the reserved ones, i.e. BBL)
      3. Base profiles (instantiation != "true") must not carry a setting_id.
      4. setting_id must be globally unique - no two files may share one.
      5. No profile may use the misspelled key "settings_id".

    Cross-vendor by nature (rule 4), so it always runs over the whole tree, never
    narrowed by --vendor. BBL keeps its authoritative "G*" cloud ids, which the
    formula does not produce, so only rule 2 is skipped for it; it is still held to
    presence, uniqueness, base-no-id and the typo check.
    """
    errors = 0
    owners = {}  # setting_id -> [relative path], every vendor
    for vendor in list_profile_dirs(profiles_dir):
        formula_exempt = vendor in RESERVED_VENDORS
        for path, sub in iter_profile_files(os.path.join(profiles_dir, vendor)):
            try:
                data = load_json(path)
            except (ValueError, OSError):
                # Parse failures are reported by the per-vendor checks, which walk
                # the same files; reporting them here too would double-count.
                continue
            if not isinstance(data, dict):
                continue
            rel = os.path.relpath(path, profiles_dir).replace(os.sep, "/")
            # Rule 5: catch the misspelled "settings_id" key.
            if "settings_id" in data:
                errors += 1
                print_error(
                    f'profile {rel} uses the misspelled key "settings_id" '
                    f'(should be "setting_id"); run {SETTING_ID_CMD}')
            sid = data.get("setting_id")
            if data.get("instantiation") != "true":
                # Rule 3: base/template profiles must not carry a setting_id.
                if sid:
                    errors += 1
                    print_error(
                        f'base profile {rel} (instantiation != "true") must not have a '
                        f'setting_id ("{sid}"); run {SETTING_ID_CMD}')
                continue
            # Rule 1: every instantiated preset must have a setting_id.
            if not sid:
                errors += 1
                print_error(f"instantiated preset {rel} is missing a setting_id; "
                            f"run {SETTING_ID_CMD}")
                continue
            # Rule 2: the stored id must match the deterministic rule.
            if not formula_exempt:
                expected = generate_preset_setting_id(vendor, sub, data.get("name", ""))
                if sid != expected:
                    errors += 1
                    print_error(
                        f'setting_id "{sid}" in {rel} does not match the expected '
                        f'"{expected}" for {vendor}/{sub}/{data.get("name", "")}; '
                        f"run {SETTING_ID_CMD}")
                    continue
            owners.setdefault(sid, []).append(rel)

    # Rule 4: a setting_id shared by two files is an error. For managed vendors that
    # means a duplicate vendor/type/name; for BBL a copy-pasted id.
    for sid, locs in sorted(owners.items()):
        if len(locs) < 2:
            continue
        errors += 1
        print_error(f'setting_id "{sid}" is shared by {len(locs)} files ({sorted(locs)}); '
                    f"setting_id must be globally unique")
    return errors


def check_machine_model_name_uniqueness(profiles_dir):
    """No two bundles may declare a machine_model with the same name.

    A machine_model name is the key the whole tree resolves a printer type by:
    Preset::get_printer_type (and get_current_printer_type) walk every vendor's
    models and return the model_id of the first whose name equals the preset's
    printer_model, so two models sharing a name make that lookup depend on vendor
    order. The name is also what the Add Printer list shows, so a duplicate
    renders the same printer twice.

    Unlike preset names, which are per bundle - base profiles share one name
    across dozens of bundles by design - a machine_model name is global. A vendor
    copying another vendor's model (the Custom "Generic Klipper Printer" being the
    usual source) is the common way this happens.

    Cross-vendor by nature, so it always runs over the whole tree, never narrowed
    by --vendor. Returns the error count.
    """
    errors = 0
    owners = defaultdict(list)  # model name -> [relative path]
    for vendor in list_profile_dirs(profiles_dir):
        for path, _sub in iter_profile_files(os.path.join(profiles_dir, vendor)):
            if os.path.basename(path) in NON_PROFILE_FILES:
                continue
            try:
                data = load_json(path)
            except (ValueError, OSError):
                # Parse failures are reported by the checks that walk the same
                # files; reporting them here too would double-count.
                continue
            if not isinstance(data, dict) or data.get("type") != "machine_model":
                continue
            name = data.get("name")
            if name:
                owners[name].append(
                    os.path.relpath(path, profiles_dir).replace(os.sep, "/"))

    for name, paths in sorted(owners.items()):
        if len(paths) < 2:
            continue
        errors += 1
        print_error(
            f'machine_model name "{name}" is declared by {len(paths)} bundles '
            f'({", ".join(sorted(paths))}); a machine model name is global, so the '
            f"printer type resolves to whichever bundle is seen first and the Add "
            f"Printer list shows it twice - rename or delete the duplicate")
    return errors


# ---------------------------------------------------------------------------
# Per-vendor validation
# ---------------------------------------------------------------------------
# These walk one vendor bundle each and are what --vendor narrows. They use
# pathlib where the originals did; the check driver converts at the boundary.

def _vendor_json_files(vendor_path):
    """Every .json under a vendor bundle, deepest-last, in a stable order."""
    return sorted(vendor_path.rglob("*.json"))


def check_preset_name_uniqueness(profiles_dir, vendor):
    """No two profiles in a bundle may share a type and a name, indexed or not.

    The loader resolves "inherits" and "include" through per-type maps of the bundle's
    profiles (PresetBundle.cpp load_subfiles), and std::map::emplace keeps the first
    insertion: a second file claiming the name is silently dropped, and which one
    wins is nothing but index order. An unindexed twin counts too - it is one
    sub_path edit away from deciding that silently.

    Names are per bundle, never global: base profiles reuse them across vendors by
    design (fdm_process_common exists in 61 of them). Returns the error count.
    """
    errors = 0
    vendor_dir = os.path.join(profiles_dir, vendor)
    claimed = defaultdict(list)  # (type, name) -> [sub_path]
    for path, _sub in iter_profile_files(vendor_dir):
        if os.path.basename(path) in NON_PROFILE_FILES:
            continue
        try:
            data = load_json(path)
        except (ValueError, OSError):
            # Parse failures are reported by the checks that walk the same files;
            # reporting them here too would double-count.
            continue
        if not isinstance(data, dict) or data.get("type") not in PROFILE_TYPES:
            continue
        if not data.get("name"):
            continue  # a nameless profile is check_filament_compatible_printers'
        claimed[(data["type"], data["name"])].append(
            posixpath.normpath(os.path.relpath(path, vendor_dir).replace(os.sep, "/")))

    for (profile_type, name), sub_paths in sorted(claimed.items()):
        if len(sub_paths) < 2:
            continue
        errors += 1
        print_error(f"{vendor} has {len(sub_paths)} {profile_type} profiles named "
                    f'"{name}" ({", ".join(sorted(sub_paths))}); a bundle holds one '
                    f"profile per name, so the loader keeps whichever it reaches first "
                    f"and silently drops the rest")
    return errors


def check_filament_compatible_printers(profiles_dir, vendor):
    """Every instantiated filament preset must declare a non-empty compatible_printers.

    Orca resolves compatible_printers from the preset itself; inheriting it is not
    supported on the Profile page. In the OrcaFilamentLibrary it is optional instead:
    a profile without it is generic and offered on every printer, while one that
    lists printers supersedes the generic profile there.

    Returns the error count.
    """
    error = 0
    vendor_path = Path(profiles_dir) / vendor / "filament"
    if not vendor_path.exists():
        return 0

    profiles = []
    for file_path in _vendor_json_files(vendor_path):
        if file_path.name in NON_PROFILE_FILES:
            continue
        rel = file_path.relative_to(profiles_dir)
        try:
            data = load_json(file_path, detect_duplicates=True)
        except DuplicateKeyError as e:
            print_error(f"Duplicate key error in {rel}: {e}")
            error += 1
            continue
        except (ValueError, OSError) as e:
            print_error(f"Error processing {rel}: {e}")
            error += 1
            continue

        profile_name = data.get("name")
        if not profile_name:
            print_error(f"'name' missing in {rel}")
            error += 1
            continue
        # Two files claiming this name is check_preset_name_uniqueness' to report,
        # over the whole bundle rather than the filament/ directory alone.
        profiles.append((rel, data))

    if vendor == OFL:
        return error

    for rel, data in profiles:
        if str(data.get("instantiation", "")).lower() != "true":
            continue
        compatible_printers = data.get("compatible_printers")
        if not compatible_printers:
            print_error(f"'compatible_printers' missing in {rel}")
            error += 1
    return error


def load_available_filament_profiles(profiles_dir, vendor):
    """The set of filament preset names a vendor bundle offers."""
    profiles = set()
    vendor_path = Path(profiles_dir) / vendor / "filament"
    if not vendor_path.exists():
        return profiles

    for file_path in _vendor_json_files(vendor_path):
        try:
            data = load_json(file_path)
        except (ValueError, OSError) as e:
            print_error(f"Error loading filament profile "
                        f"{file_path.relative_to(profiles_dir)}: {e}")
            continue
        if isinstance(data, dict) and "name" in data:
            profiles.add(data["name"])
    return profiles


def check_machine_default_materials(profiles_dir, vendor):
    """Every default material a machine names must exist, in the bundle or in OFL.

    Returns (errors, warnings); a bundle with no machine/ has nothing to check.
    """
    error_count = 0
    machine_dir = Path(profiles_dir) / vendor / "machine"
    if not machine_dir.exists():
        return 0, 0

    available = (load_available_filament_profiles(profiles_dir, vendor)
                 | load_available_filament_profiles(profiles_dir, OFL))

    for file_path in _vendor_json_files(machine_dir):
        rel = file_path.relative_to(profiles_dir)
        try:
            data = load_json(file_path)
        except (ValueError, OSError) as e:
            print_error(f"Error processing machine profile {rel}: {e}")
            error_count += 1
            continue

        default_materials = data.get("default_materials") or data.get(
            "default_filament_profile")
        if not default_materials:
            continue
        if isinstance(default_materials, list):
            materials = default_materials
        elif ";" in default_materials:
            # A ";"-separated list; a trailing separator leaves an empty segment,
            # which is formatting noise rather than a missing profile.
            materials = [m.strip() for m in default_materials.split(";") if m.strip()]
        else:
            materials = [default_materials]
        for material in materials:
            if material not in available:
                print_error(f"Missing filament profile: '{material}' referenced in {rel}")
                error_count += 1
    return error_count, 0


def check_name_consistency(profiles_dir, vendor):
    """Each <vendor>.json entry must name the preset its sub_path file declares.

    A preset loads only if the two agree, so a mismatch silently drops it.
    Returns (errors, warnings); the warning is the bundle having no index at all.
    """
    error_count = 0
    profiles_path = Path(profiles_dir)
    vendor_dir = profiles_path / vendor
    vendor_file = profiles_path / (vendor + ".json")
    if not vendor_file.exists():
        print_warning(f"No profiles found for vendor: {vendor} at {vendor_file}")
        return 0, 1

    try:
        data = load_json(vendor_file)
    except (ValueError, OSError) as e:
        print_error(f"Error loading vendor profile {vendor_file.name}: {e}")
        return 1, 0

    for section in ("filament_list", "machine_model_list", "machine_list", "process_list"):
        for child in data.get(section, []):
            name_in_vendor = child.get("name")
            sub_path = child.get("sub_path")
            if not name_in_vendor or not sub_path:
                print_error(f"{section} entry without a name/sub_path in {vendor}.json: "
                            f"{child}")
                error_count += 1
                continue
            sub_file = vendor_dir / sub_path
            if not sub_file.exists():
                print_error(f"Missing sub profile: '{sub_path}' declared in {vendor}.json")
                error_count += 1
                continue
            try:
                sub_data = load_json(sub_file)
            except (ValueError, OSError) as e:
                print_error(f"Error loading profile {sub_file.relative_to(profiles_path)}: {e}")
                error_count += 1
                continue

            name_in_sub = sub_data.get("name")
            if name_in_sub == name_in_vendor:
                continue
            print_error(f"{section} name mismatch: required '{name_in_vendor}' in "
                        f"{vendor}.json but found '{name_in_sub}' in "
                        f"{sub_file.relative_to(profiles_path)}")
            error_count += 1
    return error_count, 0


def check_index_coverage(profiles_dir, vendor):
    """Every profile file in a bundle must be listed in its <vendor>.json.

    The mirror of check_name_consistency, which walks the index and looks for the
    files: this walks the files and looks for them in the index. The loader reads the
    sub_paths listed there and nothing else, so a file no list names is dead weight
    that looks live - it sits in the bundle, gets edited and reviewed, and never
    reaches a single user.

    Returns (errors, gaps), gaps counting the files per REMEDY_HINTS category so
    the caller can print each remedy once for the whole run instead of once per file.
    """
    errors = 0
    gaps = Counter()
    vendor_dir = os.path.join(profiles_dir, vendor)
    try:
        library = load_json(os.path.join(profiles_dir, vendor + ".json"))
    except (ValueError, OSError):
        # A bundle with no readable index at all is check_name_consistency's to
        # report; calling every file in it unindexed would only bury that.
        return 0, gaps

    listed = set()
    for section in PROFILE_TYPES:
        for entry in library.get(section + "_list", []):
            if entry.get("sub_path"):
                # Index entries are hand-written; "filament/./X.json" names the same
                # file as "filament/X.json" and must not read as unlisted.
                listed.add(posixpath.normpath(entry["sub_path"].replace("\\", "/")))

    for path, _sub in iter_profile_files(vendor_dir):
        if os.path.basename(path) in NON_PROFILE_FILES:
            continue  # data files carry no name or type and are never indexed
        sub_path = posixpath.normpath(
            os.path.relpath(path, vendor_dir).replace(os.sep, "/"))
        if sub_path in listed:
            continue
        try:
            data = load_json(path)
        except (ValueError, OSError):
            data = None
        errors += 1
        if isinstance(data, dict) and data.get("type") in PROFILE_TYPES:
            gaps["unindexed"] += 1
            print_error(f"{vendor}/{sub_path}: no {vendor}.json list references it, so "
                        f"it never loads")
        else:
            gaps["unindexable"] += 1
            print_error(f"{vendor}/{sub_path}: no {vendor}.json list references it and "
                        f"it declares no profile type")
    return errors, gaps


def check_filament_id_length(profiles_dir, vendor):
    """No indexed filament preset may declare a filament_id longer than 8 chars.

    Longer ids break the AMS. Runs for every vendor alike (BBL included: the id
    format is what matters, not the vendor). Every .json under the bundle's
    filament directory is still parsed through the duplicate-key hook, so that
    coverage is unchanged; only the length rule itself is scoped to presets the
    index (<vendor>.json filament_list) actually references. A file the index does
    not reference never loads, so its filament_id cannot break anything -- and some
    vendors (e.g. SeeMeCNC) ship such orphaned files pre-dating this check.

    Returns the error count.
    """
    error = 0
    profiles_path = Path(profiles_dir)
    vendor_path = profiles_path / vendor / "filament"
    if not vendor_path.exists():
        return 0

    referenced = set()
    vendor_file = profiles_path / (vendor + ".json")
    if vendor_file.exists():
        try:
            index = load_json(vendor_file)
            for entry in index.get("filament_list", []):
                sub_path = entry.get("sub_path")
                if sub_path:
                    referenced.add((profiles_path / vendor / sub_path).resolve())
        except (ValueError, OSError) as e:
            print_error(f"Error loading vendor profile {vendor_file.name}: {e}")
            error += 1

    for file_path in _vendor_json_files(vendor_path):
        rel = file_path.relative_to(profiles_path)
        try:
            data = load_json(file_path, detect_duplicates=True)
        except DuplicateKeyError as e:
            print_error(f"Duplicate key error in {rel}: {e}")
            error += 1
            continue
        except (ValueError, OSError) as e:
            print_error(f"Error processing {rel}: {e}")
            error += 1
            continue

        filament_id = data.get("filament_id")
        if filament_id and len(filament_id) > 8 and file_path.resolve() in referenced:
            print_error(f'Filament id too long "{filament_id}": {rel}')
            error += 1
    return error


def check_obsolete_keys(profiles_dir, vendor):
    """Warn about settings PrintConfig.cpp explicitly discards. Returns the count."""
    warn_count = 0
    profiles_path = Path(profiles_dir)
    vendor_path = profiles_path / vendor / "filament"
    if not vendor_path.exists():
        return 0

    for file_path in _vendor_json_files(vendor_path):
        rel = file_path.relative_to(profiles_path)
        try:
            data = load_json(file_path)
        except (ValueError, OSError) as e:
            print_warning(f"Error reading profile {rel}: {e}")
            warn_count += 1
            continue
        for key in data:
            if key in OBSOLETE_KEYS:
                print_warning(f"Obsolete key: '{key}' found in {rel}")
                warn_count += 1
    return warn_count


def check_vector_type_keys(profiles_dir, vendor):
    """Options the config system stores as vectors must be JSON arrays.

    `filament_type` must be ["PA-CF"], not "PA-CF". Returns the error count.
    """
    error_count = 0
    profiles_path = Path(profiles_dir)
    vendor_path = profiles_path / vendor
    if not vendor_path.exists():
        return 0

    for file_path in _vendor_json_files(vendor_path):
        rel = file_path.relative_to(profiles_path)
        try:
            data = load_json(file_path)
        except (ValueError, OSError) as e:
            print_error(f"Error processing {rel}: {e}")
            error_count += 1
            continue
        if not isinstance(data, dict):
            continue
        for key in VECTOR_KEYS:
            if key in data and not isinstance(data[key], list):
                print_error(f"'{key}' must be an array in {rel}, "
                            f"got {type(data[key]).__name__}: {data[key]!r}")
                error_count += 1
    return error_count


@functools.lru_cache(maxsize=None)
def load_vendor_configs(profiles_dir, vendor):
    """name -> (tree-relative file, data) per config preset type, from the bundle's
    index lists - the per-kind maps the loader resolves inherits and include in.

    Cached because every vendor's filaments may reach OrcaFilamentLibrary. Unreadable
    files are check_name_consistency's to report; when two indexed files claim one
    name the first wins, as std::map::emplace keeps the first insertion.
    """
    configs = {sub: {} for sub in PROFILE_SUBDIRS}
    try:
        index = load_json(os.path.join(str(profiles_dir), vendor + ".json"))
    except (OSError, ValueError):
        return configs
    for section in PROFILE_TYPES:
        for entry in index.get(section + "_list", []):
            sub_path = entry.get("sub_path", "")
            try:
                data = load_json(os.path.join(str(profiles_dir), vendor, sub_path))
            except (OSError, ValueError):
                continue
            if isinstance(data, dict) and data.get("type") in configs and data.get("name"):
                configs[data["type"]].setdefault(data["name"], (f"{vendor}/{sub_path}", data))
    return configs


def compose_config(name, presets, ofl_presets, cache, in_ofl=False, seen=frozenset()):
    """A preset's config as the loader composes it: the parent's composed config,
    then each `include` in the order listed, then its own keys - later layers win.

    Each key maps to (value, path): the value as the winning file wrote it, width
    included, and the tree-relative files it passes through on the way to this
    preset, from that file down to this one. `inherits` resolves in the bundle
    first, then in OrcaFilamentLibrary (filaments only) and stays there once it
    enters; `include` never leaves the includer's bundle. A name that resolves to
    nothing, or a cycle, contributes nothing - the loader's own error paths leave
    the preset without it too.
    """
    if (in_ofl, name) in cache:
        return cache[(in_ofl, name)]
    bundle = ofl_presets if in_ofl else presets
    if name not in bundle and not in_ofl and name in ofl_presets:
        bundle, in_ofl = ofl_presets, True
    if name not in bundle or name in seen:
        return {}
    seen = seen | {name}
    rel, data = bundle[name]
    config = {}
    if data.get("inherits"):
        config.update(
            compose_config(data["inherits"], presets, ofl_presets, cache, in_ofl, seen))
    include = data.get("include") or []
    for included in ([include] if isinstance(include, str) else include):
        if included in bundle:
            config.update(compose_config(included, presets, ofl_presets, cache, in_ofl, seen))
    config = {key: (value, path + (rel,)) for key, (value, path) in config.items()}
    config.update((key, (value, (rel,))) for key, value in data.items())
    cache[(in_ofl, name)] = config
    return config


def _as_list(value):
    return value if isinstance(value, list) else [value]


def _per_extruder(values, i):
    """Entry i of a per-extruder vector, padded the way the loader pads: by its first value."""
    return values[i] if i < len(values) else values[0]


def _check_printer_layout(rel, config):
    """A machine's extruder_variant_list, printer_extruder_variant, printer_extruder_id,
    extruder_type and default_nozzle_volume_type agree with each other, the loader's
    defaults standing in for whatever the composed config leaves out.

    A machine that writes none of the three list keys takes one default variant
    "Direct Drive Standard" per extruder, so there is nothing to check. Otherwise
    printer_extruder_variant is extruder_variant_list flattened extruder-major and
    printer_extruder_id gives each of its entries its 1-based extruder (extruder 1
    everywhere when absent); every variant starts with its extruder's extruder_type
    ("Direct Drive" when absent); the default_nozzle_volume_type ("Standard" when
    absent) is one the extruder lists. A list without the pair leaves the loader on
    the default single variant; a pair without the list gives the sidebar no variant
    switch, and with single_extruder_multi_material off the loader replaces it with
    one default variant per extruder. Returns (errors, warnings).
    """
    menu = config.get("extruder_variant_list")
    variant = config.get("printer_extruder_variant")
    if menu is None and variant is None:
        return 0, 0
    errors = warnings = 0
    extruders = len(_as_list(config.get("nozzle_diameter", [""])))
    types = _as_list(config.get("extruder_type", [DEFAULT_EXTRUDER_TYPE]))
    volume_types = _as_list(config.get("default_nozzle_volume_type", [DEFAULT_NOZZLE_VOLUME_TYPE]))
    semm_off = str(_as_list(config.get("single_extruder_multi_material", ["1"]))[0]) \
        .lower() in ("0", "false")
    flat, ids = [], []
    if menu is not None:
        menu = _as_list(menu)
        if len(menu) != extruders:
            print_error(f"{rel}: extruder_variant_list has {len(menu)} entries for {extruders} "
                        f"extruder(s) (nozzle_diameter); it holds one entry per extruder")
            errors += 1
        for i, entry in enumerate(menu):
            variants = entry.split(",")
            flat += variants
            ids += [str(i + 1)] * len(variants)
            extruder_type = _per_extruder(types, i)
            if not all(v.startswith(extruder_type + " ") for v in variants):
                print_error(f'{rel}: extruder_variant_list entry {i + 1} "{entry}" holds a '
                            f"variant that does not start with extruder {i + 1}'s "
                            f'extruder_type "{extruder_type}"')
                errors += 1
            if f"{extruder_type} {_per_extruder(volume_types, i)}" not in variants:
                print_error(f'{rel}: default_nozzle_volume_type "{_per_extruder(volume_types, i)}" is '
                            f'not a nozzle volume type extruder {i + 1} lists ("{entry}")')
                errors += 1
    if variant is not None:
        variant = _as_list(variant)
        written_ids = config.get("printer_extruder_id")
        pair_ids = ["1"] * len(variant) if written_ids is None else _as_list(written_ids)
        if len(pair_ids) != len(variant):
            print_error(f"{rel}: printer_extruder_id has {len(pair_ids)} entries for the "
                        f"{len(variant)} entries of printer_extruder_variant")
            errors += 1
    if menu is not None and variant is not None:
        if variant != flat:
            print_error(f"{rel}: printer_extruder_variant {json.dumps(variant)} is not "
                        f"extruder_variant_list flattened extruder-major {json.dumps(flat)}")
            errors += 1
        elif pair_ids != ids:
            print_error(f"{rel}: printer_extruder_id {json.dumps(pair_ids)}"
                        f"{' (absent, so extruder 1 everywhere)' if written_ids is None else ''} "
                        f"does not give each entry of printer_extruder_variant its 1-based "
                        f"extruder {json.dumps(ids)}")
            errors += 1
    elif menu is not None:
        if len(flat) > 1:
            print_error(f"{rel}: extruder_variant_list offers {len(flat)} variants but the "
                        f"preset writes no printer_extruder_variant/printer_extruder_id, so the "
                        f'loader sizes every variant array to the single default variant '
                        f'"{DEFAULT_VARIANT}" and no other variant has a value; write the pair')
            errors += 1
    else:
        crowded = [i for i in dict.fromkeys(pair_ids) if pair_ids.count(i) > 1]
        if crowded:
            print_error(f"{rel}: printer_extruder_variant lists several variants for extruder "
                        f"{crowded[0]} but the preset writes no extruder_variant_list, so the "
                        f"sidebar offers no variant switch and the extra variants are unreachable")
            errors += 1
        elif semm_off and variant != [DEFAULT_VARIANT] * extruders:
            print_warning(f"{rel}: with single_extruder_multi_material off the loader replaces "
                          f"printer_extruder_variant {json.dumps(variant)} by one "
                          f'"{DEFAULT_VARIANT}" per extruder, because extruder_variant_list is '
                          f"absent; write the list or drop the pair")
            warnings += 1
    return errors, warnings


def _variant_length(ptype, config):
    """(variant length, reason) of a config as compose_config gives it, after
    inherits and include.

    The variant length is the length of the preset's *_extruder_variant list. A
    machine without one has the variants its extruder_variant_list offers, and
    without that list one default "Direct Drive Standard" variant per extruder
    (len(nozzle_diameter)) - the list's default in extend_extruder_variant. A process
    or filament without its list has the single default variant. The reason says
    which of these gave the length, and is empty when the list is written.
    """
    list_key = _variant_scheme()[ptype][0]
    if list_key in config:
        return len(_as_list(config[list_key][0])), ""
    if ptype != "machine":
        return 1, f"no {list_key}, so the single default variant"
    if "extruder_variant_list" in config:
        menu = _as_list(config["extruder_variant_list"][0])
        return (sum(len(entry.split(",")) for entry in menu),
                f"no {list_key}, so the variants extruder_variant_list offers")
    extruders = len(_as_list(config["nozzle_diameter"][0])) if "nozzle_diameter" in config else 1
    return extruders, f"no {list_key}, so one default variant per extruder"


def _variant_presets(profiles_dir, vendor):
    """Yield (ptype, rel, data, config, variant length, reason) for every machine,
    process and filament preset of the bundle, bases included: data as its file
    holds it, config as compose_config gives it, and the variant length and reason
    as _variant_length does.
    """
    presets = load_vendor_configs(profiles_dir, vendor)
    library = {} if vendor == OFL else load_vendor_configs(profiles_dir, OFL)["filament"]
    for ptype in _variant_scheme():
        ofl_presets = library if ptype == "filament" else {}
        cache = {}
        for name, (rel, data) in sorted(presets[ptype].items(), key=lambda item: item[1][0]):
            config = compose_config(name, presets[ptype], ofl_presets, cache)
            yield (ptype, rel, data, config, *_variant_length(ptype, config))


def indexed_sub_paths(profiles_dir, vendor):
    """The sub_paths a bundle's index lists, normalized, or None with no index.

    The loader reads what <vendor>.json references and nothing else, so a file no list
    names never loads and its contents are not worth judging; "filament/./X.json"
    names the same file as "filament/X.json". A bundle with no readable index is
    check_name_consistency's to report. Returns a set of sub_paths, or None.
    """
    try:
        library = load_json(os.path.join(profiles_dir, vendor + ".json"))
    except (ValueError, OSError):
        return None
    listed = set()
    for section in PROFILE_TYPES:
        for entry in library.get(section + "_list", []):
            if entry.get("sub_path"):
                listed.add(posixpath.normpath(entry["sub_path"].replace("\\", "/")))
    return listed


def _variant_entries(key, value):
    """(where, variant string) of every entry of a variant list key, as the loader
    reads it: extruder_variant_list holds one ","-joined menu per extruder, the other
    three one variant per array entry."""
    if key == "extruder_variant_list":
        return [(f"extruder {i}", variant)
                for i, entry in enumerate(_as_list(value), 1)
                for variant in str(entry).split(",")]
    return [(f"entry {i}", str(variant))
            for i, variant in enumerate(_as_list(value), 1)]


def _check_variant_string(rel, key, where, variant):
    """Report one variant string that is not "<extruder type> <nozzle volume type>"
    in the engine's enum names. Returns the error count, 0 or 1.

    Legality is the two enum maps _variant_names reads, in every bundle: a variant
    outside them is dead - nothing selects it, nothing rejects it, and it still counts
    toward the variant length every array is sized by, so the values tuned for it
    silently never reach the G-code. A legacy name the loader rewrites in these keys
    is an error too, naming the enum name to write instead.
    """
    extruder_types, volume_types, legacy, _extruder_legacy = _variant_names()

    if not variant:
        print_error(f"{rel}: {key} {where} is empty; every entry names one variant, "
                    f'"<extruder type> <nozzle volume type>"')
        return 1
    # Longest first, so an extruder type that is a prefix of another cannot win.
    extruder = next((t for t in sorted(extruder_types, key=len, reverse=True)
                     if variant.startswith(t + " ")), None)
    if extruder is None:
        # "DirectDrive" is rewritten in extruder_type alone, so a variant string
        # carrying it is dead.
        print_error(f'{rel}: {key} {where} holds "{variant}", which no extruder can '
                    f"select: it does not start with an extruder type the enum has "
                    f'({", ".join(sorted(extruder_types))}); a variant is '
                    f'"<extruder type> <nozzle volume type>"')
        return 1
    volume = variant[len(extruder) + 1:]
    if volume in volume_types:
        return 0
    if volume in legacy:
        print_error(f'{rel}: {key} {where} holds the legacy variant "{variant}"; the '
                    f'loader still rewrites "{volume}" to "{legacy[volume]}", but a '
                    f'profile writes the enum name: "{extruder} {legacy[volume]}"')
    elif volume in RUNTIME_VOLUME_TYPES:
        print_error(f'{rel}: {key} {where} holds "{variant}", which no extruder can '
                    f"select: Hybrid names the sub-nozzles of one hybrid extruder at "
                    f"runtime, never a nozzle volume type a profile writes")
    else:
        print_error(f'{rel}: {key} {where} holds "{variant}", which no extruder can '
                    f'select: "{volume}" is not a nozzle volume type the enum has '
                    f'({", ".join(sorted(volume_types))}), so nothing selects the variant '
                    f"and the values tuned for it never reach the G-code")
    return 1


def _variant_repeats(data):
    """One message per variant a preset's own lists name twice.

    The lookup returns the first equal string, so a repeat is a variant nothing
    selects, and the value written beside it sits at an index no extruder reads. A
    process's variant is the (extruder id, variant) pair, so one string on two
    extruders is two pairs rather than a repeat; with print_extruder_id absent or
    short every pair reads as extruder 1, which check_variant_arrays warns about once
    per preset instead.
    """
    messages = []
    if "filament_extruder_variant" in data:
        variants = [str(v) for v in _as_list(data["filament_extruder_variant"])]
        messages += [
            f'filament_extruder_variant lists "{v}" {variants.count(v)} times; a '
            f"filament's variants are matched by exact string, so the repeat is a "
            f"variant nothing selects and its value sits at an index no extruder reads"
            for v in sorted({v for v in variants if variants.count(v) > 1})]
    if "extruder_variant_list" in data:
        for i, entry in enumerate(_as_list(data["extruder_variant_list"]), 1):
            variants = str(entry).split(",")
            messages += [
                f'extruder_variant_list extruder {i} lists "{v}" '
                f"{variants.count(v)} times; the sidebar offers it once, so the repeat "
                f"is a variant nothing selects and the arrays hold a value no extruder "
                f"reads"
                for v in sorted({v for v in variants if variants.count(v) > 1})]
    if "print_extruder_variant" in data and "print_extruder_id" in data:
        ids = _as_list(data["print_extruder_id"])
        variants = _as_list(data["print_extruder_variant"])
        if len(ids) == len(variants):
            pairs = [(str(i), str(v)) for i, v in zip(ids, variants)]
            messages += [
                f'print_extruder_variant lists the pair (extruder {i}, "{v}") '
                f"{pairs.count((i, v))} times; a variant is found by that pair, so the "
                f"repeat is unreachable"
                for i, v in sorted({p for p in pairs if pairs.count(p) > 1})]
    return messages


def _check_variant_seed(rel, option, value, allowed, legacy, runtime=frozenset()):
    """Report the values of an enum option a variant string is composed from.

    extruder_type and the nozzle volume type (nozzle_volume_type, seeded by
    default_nozzle_volume_type) are the two halves of every variant string, and all
    three are enum options: an unknown value fails the validator's load of the whole
    bundle, while the app silently loads the option's default instead, which is worth
    naming precisely here. A legacy name the loader rewrites, and a name in `runtime`
    the engine computes for itself, are errors too. Returns the error count.
    """
    errors = 0
    for name in ([] if value is None else _as_list(value)):
        name = str(name)
        if name in allowed:
            continue
        if name in legacy:
            print_error(f'{rel}: {option} spells the legacy name "{name}"; the loader '
                        f'still rewrites it to "{legacy[name]}", but a profile writes the '
                        f"enum name")
        elif name in runtime:
            print_error(f'{rel}: {option} names "{name}", which the engine computes '
                        f"for a hybrid extruder at runtime; no profile writes it")
        else:
            print_error(f'{rel}: {option} "{name}" is not one of '
                        f'({", ".join(sorted(allowed))}); the option is an enum, so an '
                        f"unknown value fails loading the whole bundle in the validator "
                        f"and silently becomes the default in the app")
        errors += 1
    return errors


def check_variant_names(profiles_dir, vendor):
    """Every variant string and extruder or nozzle volume type a preset writes is an
    enum name the engine has.

    check_variant_arrays judges how many values a variant key holds; this judges what
    the variants are called, the other half of the same scheme. It reads the four list
    keys, extruder_type, nozzle_volume_type and default_nozzle_volume_type of every
    preset the bundle's index references - bases included, since a base's list is what
    sizes its children's arrays - and reports as errors, in every bundle alike:

      * a variant string the two enums cannot build (dead variant), a legacy spelling
        the loader still rewrites in these keys included (_check_variant_string);
      * a variant list naming one variant twice: the lookup returns the first equal
        string, so the repeat is unreachable. A filament list takes variant strings, a
        menu takes them per extruder, and a process takes (extruder id, variant) pairs,
        where one string on two extruders is two pairs, not a repeat;
      * an extruder_type, nozzle_volume_type or default_nozzle_volume_type that is not
        an enum name a profile writes (_check_variant_seed).

    Key/value types elsewhere are check_vector_type_keys' and the validator's; a file
    no list references is skipped, check_index_coverage having reported it. Returns
    the error count.
    """
    extruder_types, volume_types, legacy, extruder_legacy = _variant_names()
    errors = 0
    listed = indexed_sub_paths(profiles_dir, vendor)
    if listed is None:
        return 0
    vendor_dir = os.path.join(profiles_dir, vendor)

    for path, _sub in iter_profile_files(vendor_dir):
        if os.path.basename(path) in NON_PROFILE_FILES:
            continue
        sub_path = posixpath.normpath(
            os.path.relpath(path, vendor_dir).replace(os.sep, "/"))
        if sub_path not in listed:
            continue
        try:
            data = load_json(path)
        except (ValueError, OSError):
            # Parse failures are reported by the checks that walk the same files.
            continue
        if not isinstance(data, dict):
            continue
        rel = f"{vendor}/{sub_path}"

        for key in ("extruder_variant_list", "printer_extruder_variant",
                    "print_extruder_variant", "filament_extruder_variant"):
            if key in data:
                for where, variant in _variant_entries(key, data[key]):
                    errors += _check_variant_string(rel, key, where, variant)

        # A list naming one variant twice, and the two enum values a variant string is
        # composed from.
        for message in _variant_repeats(data):
            print_error(f"{rel}: {message}")
            errors += 1
        errors += _check_variant_seed(rel, "extruder_type", data.get("extruder_type"),
                                      extruder_types, extruder_legacy)
        for option in ("nozzle_volume_type", "default_nozzle_volume_type"):
            errors += _check_variant_seed(rel, option, data.get(option), volume_types,
                                          legacy, RUNTIME_VOLUME_TYPES)
    return errors


def check_variant_arrays(profiles_dir, vendor, strict=False):
    """Every variant array an instantiated preset writes is exactly its
    variant length x stride wide, and a printer's variant layout keys agree.

    Only instantiated presets are judged, each at the variant length of its composed
    config (_variant_length's); a base is not, since what it writes counts only where
    it reaches a preset that does not override it. A full-width array holds one
    value per variant; any other width is an error, one value included and whatever
    the values. A key the preset does not write takes what reaches it - the default,
    or an array it inherits or includes - which the loader sizes to the preset, and
    is not checked. With strict, every instantiated preset also holds each key that
    reaches it at its own width, so a preset with other variants than the file its
    array comes from restates the array. The machine_max_* limits hold
    a (normal, silent) pair per variant. A process that lists variants pairs each
    with its extruder id (extruder 1 everywhere when absent); a machine's layout keys
    are held to _check_printer_layout, which reports a printer_extruder_id that does
    not fit a written printer_extruder_variant, as the process check below does for
    print_extruder_id. Returns (errors, warnings).
    """
    errors = warnings = 0
    for ptype, rel, data, config, variant_length, reason in _variant_presets(profiles_dir,
                                                                             vendor):
        if data.get("instantiation") != "true":
            continue
        list_key, strides = _variant_scheme()[ptype]
        listed = list_key in config
        for key, stride in sorted(strides.items()):
            if key not in config or (listed and key in ("printer_extruder_id",
                                                         "print_extruder_id")):
                continue
            if key not in data and not strict:
                continue
            value, path = config[key]
            width, need = len(_as_list(value)), variant_length * stride
            if width == need:
                continue
            print_error(f'{rel}: "{key}" has {width} values for variant length {variant_length}'
                        f"{' (' + reason + ')' if reason else ''} at stride {stride}, "
                        f"which takes {need}"
                        f"{'' if key in data else ' (it comes from ' + path[0] + ')'}")
            errors += 1
        config = {key: value for key, (value, _path) in config.items()}
        if ptype == "machine":
            new_errors, new_warnings = _check_printer_layout(rel, config)
            errors += new_errors
            warnings += new_warnings
        elif ptype == "process" and listed:
            variants = _as_list(config[list_key])
            if "print_extruder_id" in config:
                ids = _as_list(config["print_extruder_id"])
                if len(ids) != variant_length:
                    print_error(f"{rel}: print_extruder_id has {len(ids)} entries for the "
                                f"{variant_length} entries of print_extruder_variant; the two "
                                f"list the (extruder id, variant) pairs, one per entry")
                    errors += 1
            elif len(set(variants)) < len(variants):
                print_warning(f"{rel}: print_extruder_variant repeats a variant but "
                              f"print_extruder_id is absent, so every entry defaults to "
                              f"extruder 1 and the repeated variant is unreachable")
                warnings += 1
    return errors, warnings


def check_conflict_keys(profiles_dir, vendor):
    """A renamed option and its old spelling must not co-exist in one profile.

    The loader would pick one arbitrarily, and for extruder_clearance_radius vs
    extruder_clearance_max_radius the wrong pick means a toolhead collision.
    Returns (errors, warnings).
    """
    error_count = 0
    profiles_path = Path(profiles_dir)
    vendor_path = profiles_path / vendor
    if not vendor_path.exists():
        print_warning(f"No profile directory for vendor: {vendor}")
        return 0, 1

    for file_path in _vendor_json_files(vendor_path):
        rel = file_path.relative_to(profiles_path)
        try:
            data = load_json(file_path, detect_duplicates=True)
        except DuplicateKeyError as e:
            print_error(f"Duplicate key error in {rel}: {e}")
            error_count += 1
            continue
        except (ValueError, OSError) as e:
            print_error(f"Error processing {rel}: {e}")
            error_count += 1
            continue
        if not isinstance(data, dict):
            continue
        for key_set in CONFLICT_KEYS:
            if sum(1 for k in key_set if k in data) > 1:
                print_error(f"Conflict keys {key_set} co-exist in {rel}")
                error_count += 1
    return error_count, 0


def check_normalized(profiles_dir, vendor):
    """A bundle must already be what normalize and update-index write.

    Those two commands define a profile file's canonical shape - identifying keys
    first, keys the slicer no longer reads gone, filament options that are vectors
    written as vectors - and a <vendor>.json's canonical lists, ordered
    dependencies-first so the loader resolves every "inherits" and "include" in one
    pass. Running them over a contributed bundle has to be a no-op; where it would
    not be, the file that was reviewed is not the file that ships, and the next
    maintainer to run normalize carries an unrelated diff into their own change.

    It asks the normalize and update-index sections below rather than restating what
    they do, because a second definition of normal is free to drift from the one that
    writes.

    The index half is skipped when the bundle has a file update-index cannot place, or
    a preset name two files claim: the index is then unbuildable for a reason
    check_index_coverage and check_preset_name_uniqueness already report on their own,
    and "would be rebuilt" stacked on top of that is noise.

    Returns (errors, gaps), gaps counting per REMEDY_HINTS category.
    """
    errors = 0
    gaps = Counter()
    vendor_dir = os.path.join(profiles_dir, vendor)

    for path, sub in iter_profile_files(vendor_dir):
        if os.path.basename(path) in NON_PROFILE_FILES:
            continue  # data files carry no name or type; normalize skips them too
        try:
            data = load_json(path)
        except (ValueError, OSError):
            continue  # an unreadable file is check_name_consistency's to report
        if not isinstance(data, dict):
            continue
        changes = _normalize_profile(data, sub)
        if not changes:
            continue
        sub_path = os.path.relpath(path, vendor_dir).replace(os.sep, "/")
        print_error(f"{vendor}/{sub_path}: normalize would {'; '.join(changes)}")
        errors += 1
        gaps["unnormalized"] += 1

    try:
        library = load_json(os.path.join(profiles_dir, vendor + ".json"))
    except (ValueError, OSError):
        # A bundle with no readable index has nothing to compare against, and saying so
        # again here would only bury check_name_consistency's report of it.
        return errors, gaps

    sections, problems = build_index_sections(profiles_dir, vendor)
    if sections is None or problems:
        return errors, gaps
    stale = sorted(s for s, entries in sections.items() if library.get(s) != entries)
    if stale:
        print_error(f"{vendor}.json: update-index would rebuild {', '.join(stale)}")
        errors += 1
        gaps["stale_index"] += 1
    return errors, gaps


# ---------------------------------------------------------------------------
# check
# ---------------------------------------------------------------------------

def check_profiles(profiles_dir=PROFILES_DIR, vendors=None, strict=False):
    """Validate the whole profile tree. Returns the error count.

    strict holds every preset's inherited and included variant arrays to its
    own width too (check_variant_arrays).

    The per-vendor checks honour `vendors`; the setting_id and filament_id checks are
    cross-vendor properties a narrowed run cannot answer, so they always cover the
    whole tree. With no `vendors`, every bundle is checked except the `user` directory
    a local validator run leaves behind, being its data dir rather than a bundle;
    naming it explicitly checks it. OrcaFilamentLibrary is checked like any other
    bundle, its only exemption being that a library filament may leave
    compatible_printers empty - what check_filament_compatible_printers applies. The
    normalization pass takes its own vendor list - see the comment on that loop.
    """
    print_info("Checking profiles ...")
    errors_found = 0
    warnings_found = 0
    remedies = Counter()

    if vendors:
        checked = list(vendors)
    else:
        checked = [v for v in list_profile_dirs(profiles_dir) if v != USER_DIR]

    for vendor in checked:
        errors_found += check_preset_name_uniqueness(profiles_dir, vendor)
        errors_found += check_filament_compatible_printers(profiles_dir, vendor)

        new_errors, new_warnings = check_machine_default_materials(profiles_dir, vendor)
        errors_found += new_errors
        warnings_found += new_warnings

        warnings_found += check_obsolete_keys(profiles_dir, vendor)

        new_errors, new_warnings = check_name_consistency(profiles_dir, vendor)
        errors_found += new_errors
        warnings_found += new_warnings

        new_errors, new_warnings = check_conflict_keys(profiles_dir, vendor)
        errors_found += new_errors
        warnings_found += new_warnings

        errors_found += check_vector_type_keys(profiles_dir, vendor)
        errors_found += check_variant_names(profiles_dir, vendor)
        new_errors, new_warnings = check_variant_arrays(profiles_dir, vendor, strict)
        errors_found += new_errors
        warnings_found += new_warnings
        errors_found += check_filament_id_length(profiles_dir, vendor)

        new_errors, gaps = check_index_coverage(profiles_dir, vendor)
        errors_found += new_errors
        remedies.update(gaps)

    # normalize and update-index judge file and index shape, not the preset-content
    # rules above, so this pass takes its own vendor list. Unscoped that is the
    # bundles with an index, exactly what those two commands take; a --vendor is
    # passed through as given, so a bundle whose index has not landed yet still has
    # its files held to what normalize writes.
    for vendor in (vendors or list_vendor_names(profiles_dir)):
        new_errors, gaps = check_normalized(profiles_dir, vendor)
        errors_found += new_errors
        remedies.update(gaps)

    for category, hint in REMEDY_HINTS.items():
        if remedies[category]:
            print_warning(f"{remedies[category]} {hint}")

    # Cross-vendor checks: setting_id and machine_model name uniqueness and the
    # whole filament_id state, all validated over the entire tree regardless of
    # --vendor.
    errors_found += check_setting_id_uniqueness(profiles_dir)
    errors_found += check_machine_model_name_uniqueness(profiles_dir)
    errors_found += check_filament_ids(profiles_dir)

    print("\n==================== SUMMARY ====================")
    print_info(f"Checked vendors     : {len(checked)}")
    if errors_found > 0:
        print_error(f"Files with errors   : {errors_found}")
    else:
        print_success("Files with errors   : 0")
    if warnings_found > 0:
        print_warning(f"Files with warnings : {warnings_found}")
    else:
        print_success("Files with warnings : 0")
    print("=================================================")
    if errors_found > 0 or warnings_found > 0:
        print_warning(f"Issue(s) found, {NORMALIZE_HINT}")
    return errors_found


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


_JSON_WS = re.compile(r"[ \t\r\n]*")


def _json_members(text):
    """(key, key start, value start, value end) of each member of the top-level
    JSON object in text, in file order."""
    decoder = json.JSONDecoder()
    members = []
    i = _JSON_WS.match(text, text.index("{") + 1).end()
    while text[i] != "}":
        key_start = i
        key, i = decoder.raw_decode(text, i)
        i = _JSON_WS.match(text, _JSON_WS.match(text, i).end() + 1).end()  # past the ':'
        _value, end = decoder.raw_decode(text, i)
        members.append((key, key_start, i, end))
        i = _JSON_WS.match(text, end).end()
        i = _JSON_WS.match(text, i + (text[i] == ",")).end()
    return members


def set_array_value(text, key, values):
    """Write values as the "key" array, byte-preserving the rest.

    An existing array keeps its one-line or one-value-per-line layout; a preset
    without the key gets it as its last member, one value per line, in the file's
    own indentation and line ending. Returns (text, 1).
    """
    items = [json.dumps(v, ensure_ascii=False) for v in values]
    members = _json_members(text)
    for name, _key_start, start, end in reversed(members):  # json.load keeps the last
        if name == key:
            m = re.fullmatch(r"\[(\r?\n)([ \t]*).*(\r?\n)([ \t]*)\]", text[start:end], re.DOTALL)
            array = ("[" + ", ".join(items) + "]" if m is None else
                     "[" + m[1] + ("," + m[1]).join(m[2] + item for item in items)
                     + m[3] + m[4] + "]")
            return text[:start] + array + text[end:], 1
    _name, key_start, _start, end = members[-1]
    indent = text[text.rfind("\n", 0, key_start) + 1:key_start]
    nl = "\r\n" if "\r\n" in text else "\n"
    array = "[" + nl + ("," + nl).join(indent * 2 + item for item in items) + nl + indent + "]"
    return text[:end] + "," + nl + indent + json.dumps(key) + ": " + array + text[end:], 1


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
# generate-id
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
        or a foreign one such as a Bambu catalog id arriving with an upstream
        sync — is replaced in place;
      * an instantiated filament that resolves no id at all gets one inserted
        into its root(s): the id-less presets of the SAME filament its members
        inherit, or the member itself (a parent of another filament cannot carry
        this filament's id — check 2).
    A declaration is left alone exactly when it already equals the one id its
    triple mints (check 2). Two products minting one id (check 2d) are reported
    and left unwritten: nothing salts past a collision, a rename resolves it.

    `vendors` restricts what is WRITTEN; the id is a function of the triple
    alone, so a narrowed run writes exactly what a full one would, and --check
    reports whatever it was not allowed to touch. `changed_paths`, when a set is
    passed, collects the files that changed. A file whose layout offers no
    anchor for the edit is reported and counted as an error, so one odd profile
    cannot abort the pass over all the others. Returns (files_changed, errors).
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
            # Strictly "true", exactly as check_setting_id_uniqueness tests it:
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


def run_generate_id(profiles_dir, vendors, filament_id, setting_id, dry_run):
    """The generate-id command: write the ids the tree's identities imply.

    Returns the process exit code.
    """
    # Both by default. filament_id runs first so its keys are in place before the
    # setting_id pass reads the files back.
    do_filament = filament_id or not setting_id
    do_setting = setting_id or not filament_id
    changed = set()  # one file the two passes both touch is still one file
    errors = 0
    if do_filament:
        _n, e = generate_filament_ids(profiles_dir, vendors, dry_run, changed)
        errors += e
    if do_setting:
        _n, e = generate_setting_ids(profiles_dir, vendors, dry_run, changed)
        errors += e

    summary = (f"dry run: {len(changed)} file(s) would change; nothing written"
               if dry_run else f"{len(changed)} file(s) changed")
    if errors:
        print_error(f"{summary}; {errors} error(s)")
    else:
        print_success(summary)
    return 1 if errors else 0



# ---------------------------------------------------------------------------
# Whole-file profile writes (normalize / update-index)
# ---------------------------------------------------------------------------
# Unlike the byte-preserving id edits above, these reserialise a whole file: they
# reorder keys and normalize formatting by design.

def _rel(path, profiles_dir):
    """Tree-relative, forward-slashed path - what every message below prints."""
    return os.path.relpath(path, profiles_dir).replace(os.sep, "/")


def _walk_json(directory):
    """Every .json file under a directory, in a deterministic order."""
    for root, dirs, files in os.walk(directory):
        dirs.sort()
        for name in sorted(files):
            if name.lower().endswith(".json"):
                yield os.path.join(root, name)


def _profile_subdir(profile_type):
    """The directory a profile type lives in: Orca keeps machine models in machine/."""
    return "machine" if profile_type == "machine_model" else profile_type


def write_profile_json(path, data):
    """Reserialise a profile file: tab-indented, LF endings, trailing newline.

    newline="\\n" is not cosmetic: without it Python translates to os.linesep, so a
    normalize run on Windows would rewrite every file it touches with CRLF endings.
    """
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent="\t", ensure_ascii=False)
        f.write("\n")


def create_ordered_profile(profile, priority_fields):
    """`profile` with priority_fields hoisted to the front, in that order."""
    ordered = {k: profile[k] for k in priority_fields if k in profile}
    ordered.update((k, v) for k, v in profile.items() if k not in priority_fields)
    return ordered


# ---------------------------------------------------------------------------
# normalize
# ---------------------------------------------------------------------------

# The keys that identify a preset, hoisted to the top of every rewritten file.
NORMALIZE_FIELD_ORDER = ("type", "name", "renamed_from", "inherits", "from",
                   "setting_id", "filament_id", "instantiation")


def identity_block_misplaced(keys):
    """The identifying keys are not a correctly ordered prefix of `keys`.

    create_ordered_profile hoists them, so this is the one part of the canonical
    shape a rewrite is guaranteed to change. Without asking, normalize would only
    ever apply the hoist as a side effect of some other rule firing, which leaves
    a file that breaks this rule reported as already normalized.
    """
    present = [k for k in keys if k in NORMALIZE_FIELD_ORDER]
    return (keys[:len(present)] != present
            or present != [k for k in NORMALIZE_FIELD_ORDER if k in keys])


# Settings a filament profile must not pin: they belong to the process.
FILAMENT_DROP_FIELDS = ("initial_layer_print_speed", "outer_wall_speed",
                        "inner_wall_speed", "infill_speed", "top_surface_speed",
                        "travel_speed")
# Filament options the config system stores as vectors, written as scalars by hand.
FILAMENT_ARRAY_FIELDS = ("filament_cost", "filament_density", "filament_type",
                         "temperature_vitrification", "filament_max_volumetric_speed",
                         "filament_vendor")


def _normalize_profile(data, sub):
    """Normalize one loaded profile in place. Returns what it changed, one line each.

    It reports rather than prints because check asks this function the same question
    normalize does - would this file be rewritten? - and has to render the answer as
    errors rather than as the running commentary of a write.
    """
    changes = []

    if not data.get("type"):
        if sub == "machine":
            # The machine folder holds both machine models and the nozzle variants
            # that are machines; the name is what tells them apart.
            name = data.get("name") or ""
            data["type"] = "machine" if "nozzle" in name.lower() else "machine_model"
        else:
            data["type"] = sub
        changes.append(f'add type "{data["type"]}"')

    for field in ("version", "is_custom_defined"):
        if data.get(field):
            del data[field]
            changes.append(f"remove {field}")

    for field in sorted(OBSOLETE_KEYS.intersection(data)):
        del data[field]
        changes.append(f"remove {field}")

    # BBS renamed extruder_clearance_radius to extruder_clearance_max_radius, but some
    # profiles carry both with different values, and the slicer cannot tell which one
    # to obey - a toolhead collision waiting to happen. Keep the larger one only.
    if "extruder_clearance_radius" in data and "extruder_clearance_max_radius" in data:
        drop = ("extruder_clearance_radius"
                if float(data["extruder_clearance_max_radius"])
                > float(data["extruder_clearance_radius"])
                else "extruder_clearance_max_radius")
        del data[drop]
        changes.append(f"drop conflicting {drop}")

    if sub == "filament":
        for field in FILAMENT_ARRAY_FIELDS:
            if field in data and not isinstance(data[field], list):
                data[field] = [data[field]]
                changes.append(f"convert {field} to an array")
        for field in FILAMENT_DROP_FIELDS:
            if field in data:
                del data[field]
                changes.append(f"remove {field}")

    # Last, because it describes the file as written rather than its contents, and
    # create_ordered_profile does the hoisting when the file is written.
    if identity_block_misplaced(list(data)):
        changes.append("put the identifying keys first")

    return changes


def normalize_profiles(profiles_dir=PROFILES_DIR, vendors=None, profile_types=None,
                 force=False, dry_run=False):
    """Normalize profile files in place. Returns (files changed, errors).

    Adds a missing "type", drops keys the slicer no longer reads, resolves the
    extruder_clearance_* conflict, arrayifies the filament options that are vectors,
    and rewrites the file with its identifying keys first.
    """
    vendors = vendors or list_vendor_names(profiles_dir)
    # Both machine_model and machine live in machine/, so the same directory is never
    # walked twice; what a file becomes is decided per file, from its name.
    subs = list(dict.fromkeys(_profile_subdir(t) for t in (profile_types or PROFILE_TYPES)))
    changed_files = errors = 0
    for vendor in vendors:
        for sub in subs:
            for path in _walk_json(os.path.join(profiles_dir, vendor, sub)):
                if os.path.basename(path) in NON_PROFILE_FILES:
                    continue
                rel = _rel(path, profiles_dir)
                try:
                    data = load_json(path)
                except (ValueError, OSError) as e:
                    print_error(f"{rel}: {e}")
                    errors += 1
                    continue
                if not isinstance(data, dict):
                    continue
                changes = _normalize_profile(data, sub)
                for change in changes:
                    print_info(f"{rel}: {change}")
                if not changes and not force:
                    continue
                changed_files += 1
                if dry_run:
                    print_info(f"{rel}: would be rewritten")
                    continue
                try:
                    write_profile_json(
                        path, create_ordered_profile(data, NORMALIZE_FIELD_ORDER))
                except OSError as e:
                    print_error(f"{rel}: {e}")
                    errors += 1
                    continue
                print_info(f"{rel}: rewritten")

    verb = "would be normalized" if dry_run else "normalized"
    print_success(f"{changed_files} profile(s) {verb}")
    return changed_files, errors


# ---------------------------------------------------------------------------
# trim
# ---------------------------------------------------------------------------

def trim_profiles(profiles_dir=PROFILES_DIR, vendors=None, profile_types=None,
                  dry_run=False):
    """Delete profile files that a vendor's <vendor>.json does not index.

    The loader only ever reads the sub_paths listed in <vendor>.json, so a profile
    file missing from every *_list never loads. Only recognisable presets are
    considered: assets (cover images, bed models, bed textures) are referenced from
    inside machine profiles rather than from the index, and data files such as
    cli_config.json carry no "type" - all of them stay. A file that cannot be parsed
    is reported and kept: never delete what could not be read.

    An unindexed file that some surviving profile names in "inherits" or "include" is
    kept too, and reported, UNLESS an indexed profile already carries that name: both
    are resolved by preset name, so the indexed one is the parent every child actually
    gets, and the unindexed file is a stale copy the loader never reaches. Where no
    indexed profile provides the name the inheriting preset really is broken, and
    deleting the file would destroy the only record of the settings it was written
    against - a repair job for a maintainer, not for this.

    Returns (files removed, errors).
    """
    vendors = vendors or list_vendor_names(profiles_dir)
    subs = list(dict.fromkeys(_profile_subdir(t) for t in (profile_types or PROFILE_TYPES)))

    total = errors = 0
    for vendor in vendors:
        vendor_dir = os.path.join(profiles_dir, vendor)
        try:
            library = load_json(os.path.join(profiles_dir, vendor + ".json"))
        except (ValueError, OSError) as e:
            print_error(f"{vendor}.json: {e}")
            errors += 1
            continue

        # A profile may be indexed under any of the lists whichever folder it sits
        # in, so the whole index is collected before anything is judged unreferenced.
        listed = set()
        for section in PROFILE_TYPES:
            for entry in library.get(section + "_list", []):
                sub_path = entry.get("sub_path")
                if sub_path:
                    # Index entries are hand-written; "filament/./X.json" names the
                    # same file as "filament/X.json" and must not read as an orphan.
                    listed.add(posixpath.normpath(sub_path.replace("\\", "/")))

        candidates = {}    # path -> profile, for every unindexed preset
        inherited = set()  # every name the files that stay claim as a parent or include
        provided = {}      # name -> sub_path, for the profiles the loader can see
        for sub in subs:
            for path in _walk_json(os.path.join(vendor_dir, sub)):
                rel = _rel(path, profiles_dir)
                sub_path = posixpath.normpath(
                    os.path.relpath(path, vendor_dir).replace(os.sep, "/"))
                try:
                    profile = load_json(path)
                except (ValueError, OSError) as e:
                    print_warning(f"{rel}: {e}; keeping it")
                    continue
                if not isinstance(profile, dict):
                    continue
                if sub_path in listed or profile.get("type") not in PROFILE_TYPES:
                    inherited.update(profile_dependencies(profile))
                    if sub_path in listed and profile.get("name"):
                        provided[profile["name"]] = sub_path
                    continue
                candidates[path] = profile

        # An orphan kept for being inherited can itself keep its own parent alive, so
        # the survivors are grown to a fixpoint before anything is deleted. A kept
        # orphan never enters "provided": it does not load, so it cannot be the parent
        # a child resolves to, and its own parent needs rescuing on the same terms.
        kept = {}
        while True:
            rescued = {p: d for p, d in candidates.items()
                       if d.get("name") in inherited and d.get("name") not in provided}
            if not rescued:
                break
            for path, profile in rescued.items():
                del candidates[path]
                kept[path] = profile
                inherited.update(profile_dependencies(profile))

        for path in sorted(kept):
            print_warning(f"{_rel(path, profiles_dir)}: not indexed by {vendor}.json but "
                          f'inherited from; keeping it (neither loads - fix the index)')

        removed = 0
        for path in sorted(candidates):
            rel = _rel(path, profiles_dir)
            twin = provided.get(candidates[path].get("name"))
            why = f", not indexed by {vendor}.json"
            if twin:
                why += f' ({twin} is the profile named "{candidates[path]["name"]}")'
            if dry_run:
                print_info(f"{rel}: would be removed{why}")
            else:
                try:
                    os.remove(path)
                except OSError as e:
                    print_error(f"{rel}: {e}")
                    errors += 1
                    continue
                print_info(f"{rel}: removed{why}")
            removed += 1
        if removed:
            print_info(f"{vendor}: {removed} unreferenced profile(s)")
        total += removed

    verb = "would be removed" if dry_run else "removed"
    print_success(f"{total} unreferenced profile(s) {verb}")
    return total, errors


# ---------------------------------------------------------------------------
# update-index
# ---------------------------------------------------------------------------

def profile_dependencies(profile):
    """The names a profile needs loaded before it: its parent and every include."""
    include = profile.get("include") or []
    if isinstance(include, str):
        include = [include]
    return [name for name in [profile.get("inherits"), *include] if name]


def topological_sort(profiles):
    """Order index entries dependencies-first, so the loader resolves every
    "inherits" and "include" in one pass.

    Entries that neither depend on nor are depended on by another in the same section
    go at the end in name order; the loader finds their parents, if any, through the
    base bundle instead. Every entry on a dependency cycle, which no order can satisfy,
    goes there too.
    """
    graph = defaultdict(list)
    in_degree = defaultdict(int)
    by_name = {p["name"]: p for p in profiles}
    all_names = set(by_name)

    for profile in profiles:
        child = profile["name"]
        for parent in profile_dependencies(profile):
            if parent in all_names:
                graph[parent].append(child)
                in_degree[child] += 1
                in_degree.setdefault(parent, 0)

    queue = sorted(name for name, degree in in_degree.items() if degree == 0)
    result = []
    while queue:
        current = queue.pop(0)
        result.append(by_name[current])
        for child in sorted(graph[current]):
            in_degree[child] -= 1
            if in_degree[child] == 0:
                queue.append(child)

    ordered = {p["name"] for p in result}
    result.extend(by_name[name] for name in sorted(all_names - ordered))
    return result


def build_index_sections(profiles_dir, vendor, profile_types=None):
    """The *_list sections update-index would write for one bundle, from its own files.

    A profile is indexed under the section its own "type" names, so a file whose type
    is missing or unrecognised cannot be placed - run normalize first, which is what
    writes the type.

    Returns (sections, problems). `problems` names every file that could not be placed,
    one message each, for the caller to report. `sections` is None when two files claim
    one preset name: the bundle can only hold one profile under a name, so rebuilding
    would pick a winner by directory order and quietly drop the other, and the index has
    to be left alone instead. Identify the intended preset and delete or rename the
    duplicate before retrying. Use trim only for deliberate unindexed-file cleanup,
    previewed with --dry-run.
    """
    vendor_dir = os.path.join(profiles_dir, vendor)
    sections = {}
    problems = []
    unplaceable = {}
    by_name = defaultdict(list)
    for profile_type in (profile_types or PROFILE_TYPES):
        entries = []
        for path in _walk_json(os.path.join(vendor_dir, _profile_subdir(profile_type))):
            if os.path.basename(path) in NON_PROFILE_FILES:
                continue
            rel = _rel(path, profiles_dir)
            try:
                profile = load_json(path)
            except (ValueError, OSError) as e:
                problems.append(f"{rel}: {e}")
                continue
            if not isinstance(profile, dict):
                continue
            if profile.get("type") not in PROFILE_TYPES:
                unplaceable[rel] = profile.get("type")
                continue
            if profile.get("type") != profile_type:
                continue
            name = profile.get("name")
            if not name:
                problems.append(f"{rel}: no name, cannot be indexed")
                continue
            entry = {
                "name": name,
                "sub_path": os.path.relpath(path, vendor_dir).replace(os.sep, "/"),
            }
            for key in ("inherits", "include"):
                if profile.get(key):
                    entry[key] = profile[key]
            by_name[name].append(entry["sub_path"])
            entries.append(entry)

        # inherits/include were ordering input only; the index holds name and sub_path
        sections[profile_type + "_list"] = [{"name": e["name"], "sub_path": e["sub_path"]}
                                            for e in topological_sort(entries)]

    for rel, found in sorted(unplaceable.items()):
        problems.append(f'{rel}: type {found!r} is not one of {list(PROFILE_TYPES)}, so it '
                        f"cannot be indexed; run "
                        f'"python scripts/orca_profile_tool.py normalize" first')

    clashes = {name: subs for name, subs in by_name.items() if len(subs) > 1}
    for name, subs in sorted(clashes.items()):
        problems.append(f'{vendor}.json: {len(subs)} profiles are named "{name}" '
                        f'({", ".join(sorted(subs))}); only one can be indexed under '
                        f"that name, so delete or rename the others - "
                        f"preview unindexed-file cleanup with "
                        f'"python scripts/orca_profile_tool.py trim --dry-run"')

    return (None if clashes else sections), problems


def update_profile_indexes(profiles_dir=PROFILES_DIR, vendors=None, profile_types=None,
                           dry_run=False):
    """Rebuild the *_list sections of each <vendor>.json from the files on disk.

    Returns (indexes changed, errors). See build_index_sections for what a file has to
    carry to be placed, and for when a bundle is left alone instead.
    """
    vendors = vendors or list_vendor_names(profiles_dir)
    changed = errors = 0
    for vendor in vendors:
        lib_path = os.path.join(profiles_dir, vendor + ".json")
        try:
            library = load_json(lib_path)
        except (ValueError, OSError) as e:
            print_error(f"{vendor}.json: {e}")
            errors += 1
            continue

        sections, problems = build_index_sections(profiles_dir, vendor, profile_types)
        for problem in problems:
            print_error(problem)
        errors += len(problems)
        if sections is None:
            print_error(f"{vendor}.json: left unchanged, it would have dropped a profile")
            continue

        if all(library.get(section) == entries for section, entries in sections.items()):
            continue
        changed += 1
        if dry_run:
            print_info(f"{vendor}.json: {', '.join(sorted(sections))} would be rebuilt")
            continue
        library.update(sections)
        try:
            write_profile_json(lib_path, library)
        except OSError as e:
            print_error(f"{vendor}.json: {e}")
            errors += 1
            continue
        print_info(f"{vendor}.json: {', '.join(sorted(sections))} rebuilt")

    verb = "would be rebuilt" if dry_run else "rebuilt"
    print_success(f"{changed} vendor index(es) {verb}")
    return changed, errors


# ---------------------------------------------------------------------------
# fix-variant
# ---------------------------------------------------------------------------

# The members of the variant sets that name the variants rather than hold values.
VARIANT_LAYOUT_KEYS = {"printer_extruder_variant", "printer_extruder_id", "print_extruder_variant",
                       "print_extruder_id", "filament_extruder_variant"}


def _fit_width(values, need, stride):
    """values cut to need, or padded by repeating the last value - the last
    (normal, silent) pair at stride 2, after dropping a trailing half pair; a lone
    value fills every entry, normal and silent alike."""
    if len(values) > need:
        return values[:need]
    if len(values) == 1:
        return values * need
    values = values[:len(values) - len(values) % stride]
    return values + values[-stride:] * ((need - len(values)) // stride)


def fix_variant_arrays(profiles_dir=PROFILES_DIR, vendors=None, dry_run=False, strict=False):
    """Resize every variant array check_variant_arrays rejects to its
    width: extra values are dropped, missing ones repeat the last. The variant lists
    and extruder ids are layout, not values, and are left to check.

    An array an instantiated preset writes is resized in place, to the preset's
    width; bases are left alone and no key is added. With strict, a key that then
    reaches an instantiated preset at another width is written, resized, into the
    most general file on the way down to the preset whose own width it fits and whose
    instantiated presets taking it all need that width - a base only the presets with
    a variant list inherit, say - else into the preset's own file. Presets of every bundle count towards that agreement; only
    files of `vendors` are touched. Returns (files changed, errors).
    """
    profiles_dir = str(profiles_dir)
    names = list_vendor_names(profiles_dir)
    scope = set(vendors or names)
    files = {rel: data for vendor in names
             for configs in load_vendor_configs(profiles_dir, vendor).values()
             for rel, data in configs.values()}
    verb = "would " if dry_run else ""
    changed, errors = set(), 0

    def fit(rel, key, values, need, stride):
        fitted = _fit_width(values, need, stride)
        if not fitted:
            print_error(f'{rel}: "{key}" cannot be resized from {len(values)} to {need} values')
        return fitted

    # Each preset's own arrays first, then (strict) what reaches the presets; a write
    # into a preset hands its children a new source, which the next pass judges. The
    # in-memory configs take each write so a dry run sees it too.
    for _ in range(8):
        own = {}  # file -> its own variant length
        users = defaultdict(list)  # (file, key) -> [(value, path, need, stride)]
        writes = defaultdict(dict)  # file -> {key: values}
        for vendor in names:
            for ptype, rel, data, config, variant_length, _reason in _variant_presets(
                    profiles_dir, vendor):
                own[rel] = variant_length
                instantiated = data.get("instantiation") == "true"
                for key, stride in _variant_scheme()[ptype][1].items():
                    if key not in config or key in VARIANT_LAYOUT_KEYS:
                        continue
                    value, path = config[key]
                    values, need = _as_list(value), variant_length * stride
                    if key in data:
                        if len(values) != need and instantiated and vendor in scope:
                            fitted = fit(rel, key, values, need, stride)
                            if fitted:
                                writes[rel][key] = fitted
                            else:
                                errors += 1
                    elif strict and instantiated:
                        users[(path[0], key)].append((values, path, need, stride))
        if not writes and strict:
            for (_source, key), group in users.items():
                for values, path, need, stride in group:
                    if len(values) == need or path[-1].split("/")[0] not in scope:
                        continue
                    target = next((f for f in path if f.split("/")[0] in scope
                                   and own[f] * stride == need
                                   and all(n == need for _v, p, n, _s in group if f in p)),
                                  path[-1])
                    fitted = fit(path[-1], key, values, need, stride)
                    if fitted:
                        writes[target][key] = fitted
                    else:
                        errors += 1
        if not writes or errors:
            break
        for rel, arrays in sorted(writes.items()):
            def apply(text, _arrays=arrays):
                for key, values in _arrays.items():
                    text, _n = set_array_value(text, key, values)
                return text, len(_arrays)

            try:
                data = _edit_profile(os.path.join(profiles_dir, *rel.split("/")), apply, dry_run,
                                     "variant array resize")
            except (OSError, RuntimeError, ValueError) as e:
                print_error(str(e))
                errors += 1
                continue
            print_info(f"{verb}resize {rel}: " + ", ".join(
                f"{key} {len(_as_list(files[rel][key])) if key in files[rel] else 'added'}"
                f" -> {len(values)}" for key, values in arrays.items()))
            files[rel].update(arrays)
            changed.add(rel)
            if any(data.get(key) != values for key, values in arrays.items()):
                print_error(f"{rel}: the variant array resize did not take effect")
                errors += 1
        if errors:
            break
    else:
        print_error("variant array resize did not settle; run check")
        errors += 1
    load_vendor_configs.cache_clear()  # its configs took the writes above
    print_success(f"{len(changed)} profile(s) {'would be ' if dry_run else ''}resized")
    return len(changed), errors


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

EXAMPLES = """\
examples:
  orca_profile_tool.py check
      validate the whole tree, exactly as CI does
  orca_profile_tool.py check --vendor Creality
      the per-vendor checks for one bundle; setting_id and filament_id are
      cross-vendor properties, so those two always cover the whole tree
  orca_profile_tool.py generate-id
      give every profile the id its identity mints, in every vendor bundle
  orca_profile_tool.py generate-id --dry-run
      preview exactly that; writes nothing
  orca_profile_tool.py generate-id --setting-id --vendor Elegoo
      setting_id only, and only in that bundle
  orca_profile_tool.py fix-variant --vendor Snapmaker --dry-run
      preview resizing the variant arrays check rejects in that bundle
  orca_profile_tool.py check --strict --vendor BBL
      also hold every preset's inherited variant arrays to its own width

after adding, renaming or deleting profile files, run in this order:
  normalize -> update-index -> generate-id -> check
normalize supplies missing types; update-index registers presets before id
generation.
Use trim only for deliberate cleanup, previewed with --dry-run: it judges against
the current index and can delete newly added, unindexed presets.
"""


def build_parser():
    # Shared options, attached with parents=[...] so every command spells them the
    # same way and documents them once.
    profiles_opt = argparse.ArgumentParser(add_help=False)
    profiles_opt.add_argument("--profiles", default=PROFILES_DIR, metavar="DIR",
                              help="profiles directory (default: resources/profiles)")

    vendor_opt = argparse.ArgumentParser(add_help=False)
    vendor_opt.add_argument("--vendor", metavar="VENDOR", action="append", default=[],
                            help="act on this vendor bundle only; repeatable. "
                                 "An empty value means every vendor")

    type_opt = argparse.ArgumentParser(add_help=False)
    type_opt.add_argument("--profile-type", metavar="TYPE", action="append", default=[],
                          choices=PROFILE_TYPES, dest="profile_type",
                          help="act on this profile type only; repeatable. One of: "
                               + ", ".join(PROFILE_TYPES))

    dry_run_opt = argparse.ArgumentParser(add_help=False)
    dry_run_opt.add_argument("--dry-run", "--dryrun", dest="dry_run", action="store_true",
                             help="report what would change and write nothing")

    parser = argparse.ArgumentParser(
        prog="orca_profile_tool.py", allow_abbrev=False,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description="Every maintenance job for the OrcaSlicer system profile tree.\n"
                    "\n"
                    "The ids it writes are pure functions of the profile's own\n"
                    "identity, so it never invents one: it writes the id the rules\n"
                    "already imply, and leaves a conforming tree alone.",
        epilog=EXAMPLES)
    commands = parser.add_subparsers(dest="command", metavar="<command>")

    def add(name, parents, help_text, description):
        return commands.add_parser(
            name, parents=parents, help=help_text, description=description,
            allow_abbrev=False, formatter_class=argparse.RawDescriptionHelpFormatter)

    strict_opt = argparse.ArgumentParser(add_help=False)
    strict_opt.add_argument("--strict", action="store_true",
                            help="also hold every preset's inherited and included variant "
                                 "arrays to its own width")

    add(
        "check", [vendor_opt, strict_opt, profiles_opt],
        "validate the whole profile tree -- what CI runs",
        "Validate the whole profile tree: preset name uniqueness, index coverage\n"
        "both ways, compatible_printers, default-material references, obsolete,\n"
        "conflicting and vector-typed keys, variant names and variant array widths,\n"
        "filament_id length, that normalize and update-index would leave every\n"
        "bundle alone, and the tree-wide setting_id and filament_id state. Exits\n"
        "nonzero on errors.\n"
        "\n"
        "--vendor narrows the per-vendor checks only: setting_id uniqueness and the\n"
        "filament_id state are cross-vendor properties a narrowed run cannot answer,\n"
        "so they always cover the whole tree.")

    add("fix-variant", [vendor_opt, strict_opt, dry_run_opt, profiles_opt],
        "resize the variant arrays check rejects",
        "Resize every variant array that check reports as the wrong width\n"
        "in the selectable preset that writes it: extra values are dropped, missing\n"
        "ones repeat the last value (the last normal/silent pair for machine_max_*).\n"
        "Bases are left alone and no key is added. The variant lists and extruder\n"
        "ids are left to check. Byte-preserving apart from the arrays it writes.\n"
        "\n"
        "--strict then also writes each key that reaches a preset at another width:\n"
        "into the most general file on the way down to the preset that has the\n"
        "preset's width and whose presets all agree, else into the preset itself.\n"
        "Presets of every bundle count towards that agreement; --vendor limits the\n"
        "files written.")

    generate_cmd = add(
        "generate-id", [vendor_opt, dry_run_opt, profiles_opt],
        "write the id each profile's identity implies",
        "Write the id every profile should carry: filament_id from each filament's\n"
        "(filament_vendor, filament_type, name) triple, setting_id from each preset's\n"
        "(vendor, type, name). Idempotent and byte-preserving.\n"
        "\n"
        "The id is a function of the triple alone, so a narrowed run writes exactly\n"
        "what a full one would; check reports whatever it left outside.")
    which = generate_cmd.add_mutually_exclusive_group()
    which.add_argument("--filament-id", action="store_true", dest="filament_id",
                       help="write filament_id only, skipping setting_id")
    which.add_argument("--setting-id", action="store_true", dest="setting_id",
                       help="write setting_id only, skipping filament_id")

    normalize_cmd = add(
        "normalize", [vendor_opt, type_opt, dry_run_opt, profiles_opt],
        "rewrite profile files into their canonical shape",
        "Add a missing \"type\", drop keys the slicer no longer reads, resolve the\n"
        "extruder_clearance_* conflict, arrayify the filament options that are\n"
        "vectors, and rewrite each file with its identifying keys first.\n"
        "\n"
        "Rewrites whole files, so it normalizes their formatting and key order too.")
    normalize_cmd.add_argument("--force", action="store_true",
                         help="rewrite every profile, not only the ones that changed")

    add("trim", [vendor_opt, type_opt, dry_run_opt, profiles_opt],
        "delete profile files no <vendor>.json list references",
        "Delete profile files that a vendor's <vendor>.json does not index. The\n"
        "loader only ever reads the sub_paths listed there, so an unindexed preset\n"
        "never loads.\n"
        "\n"
        "Use only for deliberate cleanup, previewed with --dry-run. Newly added,\n"
        "unindexed presets can be deleted too; omit trim from the authoring workflow.\n"
        "\n"
        "Assets and data files are kept, a file that cannot be parsed is kept and\n"
        "reported, and so is one a surviving profile inherits from that no indexed\n"
        "profile provides -- but a stale copy of an indexed profile goes, since\n"
        "inherits resolves by name and the indexed one is what children get.")

    add("update-index", [vendor_opt, type_opt, dry_run_opt, profiles_opt],
        "regenerate the *_list sections of <vendor>.json",
        "Rebuild the *_list sections of each <vendor>.json from the files on disk,\n"
        "ordered dependencies-first so the loader resolves inherits and include\n"
        "in one pass.\n"
        "\n"
        "A profile is indexed under the section its own \"type\" names, so run\n"
        "normalize first: it is what writes a missing type. Two files claiming one\n"
        "preset name leave that index alone, because a rebuild could only keep one\n"
        "of them. Identify the intended preset and delete or rename the duplicate.\n"
        "Use trim only for deliberate unindexed-file cleanup, previewed with\n"
        "--dry-run; it can also delete newly authored presets.")

    return parser


def main(argv=None):
    _utf8_console()
    argv = sys.argv[1:] if argv is None else list(argv)
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.command is None:
        parser.print_help()
        return 0

    profiles_dir = args.profiles
    # check_profile.sh passes --vendor "${VENDOR}" unconditionally (bash 3.2 cannot
    # expand an empty array under set -u), and an empty value has always meant
    # "every vendor" -- so drop empties rather than looking up a vendor named "".
    vendors = sorted({v for v in getattr(args, "vendor", []) if v}) or None
    if vendors:
        unknown = sorted(set(vendors) - set(list_profile_dirs(profiles_dir)))
        if unknown:
            for vendor in unknown:
                print_error(f'unknown vendor "{vendor}" in {profiles_dir}')
            return 1
    profile_types = tuple(getattr(args, "profile_type", []) or ()) or None

    if args.command == "check":
        errors = check_profiles(profiles_dir, vendors, strict=args.strict)
        return 1 if errors else 0

    if args.command == "generate-id":
        return run_generate_id(profiles_dir, vendors, args.filament_id, args.setting_id,
                               args.dry_run)

    if args.command == "fix-variant":
        _changed, errors = fix_variant_arrays(profiles_dir, vendors, dry_run=args.dry_run,
                                               strict=args.strict)
    elif args.command == "normalize":
        _changed, errors = normalize_profiles(profiles_dir, vendors, profile_types,
                                        force=args.force, dry_run=args.dry_run)
    elif args.command == "trim":
        _removed, errors = trim_profiles(profiles_dir, vendors, profile_types,
                                         dry_run=args.dry_run)
    else:  # update-index
        _changed, errors = update_profile_indexes(profiles_dir, vendors, profile_types,
                                                  dry_run=args.dry_run)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
