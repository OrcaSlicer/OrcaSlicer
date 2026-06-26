#!/usr/bin/env python3
"""
Assign globally-unique, per-vendor-namespaced setting_id / filament_id to
OrcaSlicer system profiles.

Policy (see resources/profiles + AGENTS.md "Critical Constraints"):
  * Bambu (BBL) owns the "G*" id space; OrcaFilamentLibrary owns "O*". Both are
    reserved and never modified by this script.
  * Every other vendor must keep its ids inside its own prefix namespace so that
    no id is ever shared between two vendors and every instantiated profile file
    has its own unique id.
  * Vendors that already live in a clean, isolated, non-"G*" namespace are
    "grandfathered" (left untouched).
  * Vendors that copy Bambu's ids (any id in the "G*" space) are "remapped":
    every profile carrying a setting_id gets a fresh sequential id "<PREFIX><NNNN>".

Only setting_id is rewritten. filament_id is deliberately left untouched: it is a
per-material id, shared across a filament's nozzle variants and inherited from base
templates, so it must not be made per-file unique.

Vendor prefix = upper(first letter) + upper(last letter) of the vendor name.
If that collides with a reserved/used prefix, walk the second letter backwards
(2nd-to-last, 3rd-to-last, ...), finally falling back to first letter + an
unused letter. The mapping is persisted to resources/profiles/vendor_prefixes.json
so reruns are stable and future vendors append deterministically.

Stability guarantee (this is a one-time migration): once a preset has a valid,
unique id in its vendor's namespace it is NEVER renumbered. A later run - e.g.
after adding a new vendor or new profiles - only assigns ids to presets that are
still missing one or carry a foreign/duplicate id; it takes the lowest free
number in that vendor and leaves every existing id untouched.

Run from anywhere:  python3 scripts/assign_vendor_setting_ids.py
The script is idempotent: a second run over an unchanged tree produces no diff.
"""

import json
import os
import re
import sys

PROFILES_DIR = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "resources", "profiles"))
PREFIX_REGISTRY = os.path.join(PROFILES_DIR, "vendor_prefixes.json")

# Vendors whose ids are authoritative / user-owned and must never be rewritten.
RESERVED_VENDORS = {"BBL", "OrcaFilamentLibrary", "user", "Custom"}

PROFILE_SUBDIRS = ("filament", "process", "machine")
COUNTER_WIDTH = 4          # PREFIX(2) + 4 digits = 6 chars


def leading_alpha(s):
    """Leading run of ascii letters, upper-cased (the 'prefix' of an id)."""
    m = re.match(r"[A-Za-z]+", s or "")
    return m.group(0).upper() if m else ""


def iter_profile_files(vendor_dir):
    """Yield profile json paths under a vendor, in a deterministic order."""
    for sub in PROFILE_SUBDIRS:
        base = os.path.join(vendor_dir, sub)
        if not os.path.isdir(base):
            continue
        for root, dirs, files in os.walk(base):
            dirs.sort()  # deterministic traversal across filesystems
            for name in sorted(files):
                if name.endswith(".json"):
                    yield os.path.join(root, name)


def read_profile(path):
    """Return (setting_id, filament_id, instantiation) as present (or None)."""
    try:
        with open(path, "rb") as f:
            data = json.loads(f.read())
    except (ValueError, OSError):
        return None, None, None
    if not isinstance(data, dict):
        return None, None, None
    return data.get("setting_id"), data.get("filament_id"), data.get("instantiation")


def list_vendors():
    return sorted(
        d for d in os.listdir(PROFILES_DIR)
        if os.path.isdir(os.path.join(PROFILES_DIR, d))
    )


def collect_state():
    """Scan every vendor for ALL instantiated presets (instantiation == "true").

    Each entry is (path, setting_id_or_None, filament_id). Presets without a
    setting_id are included so they can be assigned one - every instantiated
    preset must end up with a unique, vendor-namespaced setting_id. Base profiles
    (instantiation != "true") are excluded; by convention they carry no
    setting_id (see strip_base_setting_ids).

    id_owners maps each *existing* setting_id to the vendors using it (used for
    collision / grandfather detection only).
    """
    vendor_ids = {}          # vendor -> list of (path, setting_id|None, filament_id)
    id_owners = {}           # existing setting_id value -> set(vendors)
    for vendor in list_vendors():
        entries = []
        vdir = os.path.join(PROFILES_DIR, vendor)
        for path in iter_profile_files(vdir):
            sid, fid, inst = read_profile(path)
            if inst == "true":
                entries.append((path, sid, fid))
                if sid:
                    id_owners.setdefault(sid, set()).add(vendor)
        if entries:
            vendor_ids[vendor] = entries
    return vendor_ids, id_owners


_JSON_STR = r'"(?:[^"\\]|\\.)*"'


def remove_key_line(text, key):
    """Remove a top-level `"key": "..."` member, preserving formatting.

    Handles both the common case (member has a trailing comma) and the member
    being the LAST in its object (consume the preceding comma instead, so no
    dangling comma is left). Returns (new_text, count).
    """
    # Member followed by a comma (not the last in the object).
    trailing = re.compile(
        r'[ \t]*"' + re.escape(key) + r'"[ \t]*:[ \t]*' + _JSON_STR + r'[ \t]*,[ \t]*\r?\n'
    )
    new, n = trailing.subn("", text, count=1)
    if n:
        return new, n
    # Member is the last one: drop the preceding comma and the member itself.
    leading = re.compile(
        r',[ \t]*\r?\n[ \t]*"' + re.escape(key) + r'"[ \t]*:[ \t]*' + _JSON_STR
    )
    return leading.subn("", text, count=1)


def _remove_key_in_tree(key, should_remove):
    """Remove `key` from files where should_remove(sid, fid, inst, text) is True."""
    removed = 0
    for vendor in list_vendors():
        for path in iter_profile_files(os.path.join(PROFILES_DIR, vendor)):
            with open(path, "rb") as f:
                text = f.read().decode("utf-8")
            sid, fid, inst = read_profile(path)
            if not should_remove(sid, fid, inst, text):
                continue
            new_text, n = remove_key_line(text, key)
            if n == 0:
                raise RuntimeError(f"Could not locate {key} line to remove: {path}")
            json.loads(new_text)  # fail loudly if removal broke the JSON
            with open(path, "wb") as f:
                f.write(new_text.encode("utf-8"))
            removed += 1
    return removed


def remove_misspelled_settings_id():
    """Delete the misspelled "settings_id" key (extra "s") wherever it appears.

    The app never reads that key, so those presets effectively had no setting_id
    and get a correct one assigned by the normal pass; here we drop the junk key.
    """
    return _remove_key_in_tree(
        "settings_id", lambda sid, fid, inst, text: '"settings_id"' in text
    )


def strip_base_setting_ids():
    """Remove setting_id from every base profile (instantiation != "true").

    Bambu's convention: only instantiated, user-selectable presets carry a
    setting_id; base/template profiles do not. Applied across all vendors.
    """
    return _remove_key_in_tree(
        "setting_id", lambda sid, fid, inst, text: bool(sid) and inst != "true"
    )


def reserved_tokens(vendor_ids):
    """Leading-alpha prefixes owned by the reserved vendors (BBL / OrcaFilamentLibrary)."""
    tokens = set()
    bbl_ofl_id_set = set()
    for vendor in ("BBL", "OrcaFilamentLibrary"):
        for _path, sid, _fid in vendor_ids.get(vendor, []):
            if not sid:
                continue
            bbl_ofl_id_set.add(sid)
            t = leading_alpha(sid)
            if t:
                tokens.add(t)
    return tokens, bbl_ofl_id_set


def is_grandfathered(vendor, entries, id_owners, bbl_ofl_id_set):
    """A vendor is left untouched iff every instantiated preset already has a
    clean, isolated, unique, non-G* setting_id.

    A vendor with any instantiated preset missing a setting_id is NOT
    grandfathered - it must be remapped so the gap can be filled.
    """
    existing = [sid for _path, sid, _fid in entries if sid]
    if len(existing) != len(entries) or not existing:
        return False
    seen = set()
    for sid in existing:
        # (a) no id squatting Bambu's "G*" space and none copied from BBL/OFL
        if leading_alpha(sid).startswith("G") or sid in bbl_ofl_id_set:
            return False
        # (b) not shared with any other vendor
        if id_owners.get(sid, set()) - {vendor}:
            return False
        # (c) internally unique (every file already has its own id)
        if sid in seen:
            return False
        seen.add(sid)
    return True


def prefix_ok(prefix, reserved, used):
    if prefix in used:
        return False
    for t in reserved:
        # block exact Bambu/OFL prefixes (GP, GM, ...) and 2-char initial
        # substrings of a longer family (GF -> GFSA) so generated ids can't overlap.
        if prefix == t or t.startswith(prefix):
            return False
    return True


def derive_prefix(vendor, reserved, used):
    letters = [c for c in vendor if c.isalpha()]
    if not letters:
        letters = list("XX")
    first = letters[0].upper()
    # first + (last, 2nd-to-last, 3rd-to-last, ...)
    for k in range(1, len(letters) + 1):
        cand = first + letters[-k].upper()
        if prefix_ok(cand, reserved, used):
            return cand
    # fallback: first + an unused letter
    for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ":
        cand = first + c
        if prefix_ok(cand, reserved, used):
            return cand
    raise RuntimeError(f"Could not assign a prefix for vendor {vendor!r}")


def replace_id_value(text, key, new_value):
    """Replace the first top-level `"key": "..."` value, preserving all formatting."""
    pattern = re.compile(r'("' + re.escape(key) + r'"\s*:\s*)"(?:[^"\\]|\\.)*"')
    repl = lambda m: m.group(1) + json.dumps(new_value, ensure_ascii=False)
    new_text, n = pattern.subn(repl, text, count=1)
    return new_text, n


def insert_setting_id(text, new_id):
    """Insert a `"setting_id"` line into a preset that lacks one.

    Placed just before `filament_id` (or, failing that, `instantiation`) so it
    matches the canonical key order, reusing that anchor line's indentation and
    line ending. Only setting_id is added; filament_id is left untouched.
    """
    for key in ("filament_id", "instantiation"):
        m = re.search(r'^([ \t]*)"' + key + r'"[ \t]*:.*?(\r?\n)', text, re.MULTILINE)
        if m:
            line = f'{m.group(1)}"setting_id": {json.dumps(new_id, ensure_ascii=False)},{m.group(2)}'
            return text[:m.start()] + line + text[m.start():], 1
    return text, 0


def rewrite_file(path, new_id, has_setting_id):
    """Set the preset's setting_id to new_id (replacing or inserting as needed).

    filament_id is intentionally left untouched. Uses binary IO so the file's
    original line endings (LF or CRLF) and exact formatting are preserved
    byte-for-byte apart from the changed/added line. The result is re-parsed to
    guarantee it is still valid JSON.
    """
    with open(path, "rb") as f:
        text = f.read().decode("utf-8")
    if has_setting_id:
        text, n = replace_id_value(text, "setting_id", new_id)
    else:
        text, n = insert_setting_id(text, new_id)
    if n == 0:
        raise RuntimeError(f"Could not set setting_id on {path}")
    json.loads(text)  # fail loudly if the edit broke the JSON
    with open(path, "wb") as f:
        f.write(text.encode("utf-8"))
    return True


def main():
    # 0. Drop the misspelled "settings_id" key wherever it appears.
    typos = remove_misspelled_settings_id()

    # 1. Strip setting_id from base profiles everywhere (Bambu convention).
    stripped = strip_base_setting_ids()

    # 2. Renumber the remaining (instantiated) presets into vendor namespaces.
    vendor_ids, id_owners = collect_state()
    reserved, bbl_ofl_id_set = reserved_tokens(vendor_ids)

    # Load any previously-assigned prefixes so the mapping stays stable.
    registry = {}
    if os.path.exists(PREFIX_REGISTRY):
        with open(PREFIX_REGISTRY, encoding="utf-8") as f:
            registry = json.load(f)

    # Classify vendors. A vendor that already has an assigned prefix stays
    # remapped forever (after renumbering its ids look "grandfathered", but the
    # registry must remain stable across reruns).
    grandfathered, remapped = [], []
    for vendor in sorted(vendor_ids):
        if vendor in RESERVED_VENDORS:
            continue
        if vendor not in registry and is_grandfathered(
            vendor, vendor_ids[vendor], id_owners, bbl_ofl_id_set
        ):
            grandfathered.append(vendor)
        else:
            remapped.append(vendor)

    # Grandfathered prefixes are reserved so new prefixes never collide with them.
    for vendor in grandfathered:
        for _path, sid, _fid in vendor_ids[vendor]:
            t = leading_alpha(sid) if sid else ""
            if t:
                reserved.add(t)

    # Assign / reuse a prefix per remapped vendor (deterministic, alphabetical).
    used = set(registry.values())
    for vendor in remapped:
        if vendor not in registry:
            registry[vendor] = derive_prefix(vendor, reserved, used)
            used.add(registry[vendor])

    # Number every remapped vendor's instantiated presets. Stability rule: this is
    # a one-time migration - any preset that ALREADY has a valid, unique id in its
    # vendor's namespace keeps it forever. Only presets with a foreign/missing/
    # duplicate id are (re)assigned, taking the lowest free number. So adding a new
    # vendor or new profiles later never renumbers existing presets.
    changed = added = 0
    for vendor in remapped:
        prefix = registry[vendor]
        id_re = re.compile(r"^" + re.escape(prefix) + r"([0-9]+)$")
        entries = vendor_ids[vendor]

        # Pass 1: keep already-valid, in-namespace, first-seen (unique) ids.
        preserved = set()      # paths whose id is kept as-is
        used_nums = set()      # numeric suffixes already taken
        seen_ids = set()
        for path, sid, _fid in entries:
            m = id_re.match(sid) if sid else None
            if m and sid not in seen_ids:
                seen_ids.add(sid)
                preserved.add(path)
                used_nums.add(int(m.group(1)))

        # Pass 2: assign the lowest free number to every remaining preset.
        next_num = 1
        for path, sid, _fid in entries:
            if path in preserved:
                continue
            while next_num in used_nums:
                next_num += 1
            used_nums.add(next_num)
            new_id = f"{prefix}{next_num:0{COUNTER_WIDTH}d}"
            next_num += 1
            rewrite_file(path, new_id, has_setting_id=sid is not None)
            changed += 1
            if sid is None:
                added += 1

    # Persist the registry (all remapped vendors, stable & sorted).
    out = {v: registry[v] for v in sorted(remapped)}
    with open(PREFIX_REGISTRY, "w", encoding="utf-8") as f:
        json.dump(out, f, indent="\t", ensure_ascii=False, sort_keys=True)
        f.write("\n")

    print(f"Misspelled settings_id removed : {typos}")
    print(f"Base setting_ids stripped : {stripped}")
    print(f"Reserved vendors : {sorted(RESERVED_VENDORS)}")
    print(f"Grandfathered    : {grandfathered}")
    print(f"Remapped vendors : {len(remapped)}")
    for v in remapped:
        print(f"    {registry[v]:<3} {v}  ({len(vendor_ids[v])} profiles)")
    print(f"Files rewritten  : {changed}  (of which newly assigned: {added})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
