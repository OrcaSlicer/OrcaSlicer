#!/usr/bin/env python3
"""
Assign deterministic, globally-unique setting_id to OrcaSlicer system profiles.

Policy (see AGENTS.md "Critical Constraints"):
  * A preset's setting_id is a pure function of its identity:
        setting_id = base62_16( uuid5(NAMESPACE, "<vendor>/<type>/<name>") )
    The same value is recomputed on the fly by the C++ app
    (Slic3r::generate_preset_setting_id); the two MUST stay byte-identical. The rule
    (generate_preset_setting_id, below) is also imported by the validator
    (orca_extra_profile_check.py). Uniqueness is therefore automatic: two presets
    collide only if they share vendor + type + name, which the validator flags.
  * Bambu (BBL) owns the authoritative "G*" id space and is the only reserved vendor:
    its ids are never rewritten (preserves backward-compat with Bambu-synced presets).
    Every other vendor - including OrcaFilamentLibrary and Custom - follows the
    deterministic rule.
  * Only instantiated presets (instantiation == "true") carry a setting_id; base /
    template profiles do not.

Only setting_id is rewritten. filament_id is deliberately left untouched: it is a
per-material id, shared across a filament's nozzle variants and inherited from base
templates, so it must not be made per-file unique.

Run from anywhere:  python3 scripts/assign_vendor_setting_ids.py
The script is idempotent: a second run over an unchanged tree produces no diff.
"""

import json
import os
import re
import sys
import uuid


# Deterministic preset setting_id rule. Imported by the validator
# (orca_extra_profile_check.py) and kept byte-identical to the C++
# Slic3r::generate_preset_setting_id. Dedicated namespace, distinct from the cloud
# namespace (f47ac10b-...) so the two id spaces never coincide; this constant is baked
# into both languages - never change it.
NAMESPACE = uuid.UUID("c1f4d9e2-7a3b-5c8d-9e0f-1a2b3c4d5e6f")
ALPHABET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
ID_LENGTH = 16


def generate_preset_setting_id(vendor, type_name, name):
    """Deterministic 16-char base62 setting_id for a preset.

    input = f"{vendor}/{type_name}/{name}"; u = uuid5(NAMESPACE, input);
    id = the low ID_LENGTH base62 digits of int(u.bytes, "big"), most-significant first.
    """
    u = uuid.uuid5(NAMESPACE, f"{vendor}/{type_name}/{name}")
    n = int.from_bytes(u.bytes, "big")
    digits = []
    for _ in range(ID_LENGTH):
        digits.append(ALPHABET[n % 62])
        n //= 62
    return "".join(reversed(digits))


PROFILES_DIR = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "resources", "profiles"))

# Bambu (BBL) is the only reserved vendor: it keeps its authoritative "G*" cloud ids.
RESERVED_VENDORS = {"BBL"}

PROFILE_SUBDIRS = ("filament", "process", "machine")


def iter_profile_files(vendor_dir):
    """Yield (json path, type) under a vendor, in a deterministic order.

    type is the subdir name ("filament"/"process"/"machine"), which matches
    Preset::get_type_string() on the C++ side.
    """
    for sub in PROFILE_SUBDIRS:
        base = os.path.join(vendor_dir, sub)
        if not os.path.isdir(base):
            continue
        for root, dirs, files in os.walk(base):
            dirs.sort()  # deterministic traversal across filesystems
            for name in sorted(files):
                if name.endswith(".json"):
                    yield os.path.join(root, name), sub


def read_profile(path):
    """Return (setting_id, instantiation, name) as present (or None)."""
    try:
        with open(path, "rb") as f:
            data = json.loads(f.read())
    except (ValueError, OSError):
        return None, None, None
    if not isinstance(data, dict):
        return None, None, None
    return data.get("setting_id"), data.get("instantiation"), data.get("name")


def list_vendors():
    return sorted(
        d for d in os.listdir(PROFILES_DIR)
        if os.path.isdir(os.path.join(PROFILES_DIR, d))
    )


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
    """Remove `key` from files where should_remove(sid, inst, text) is True."""
    removed = 0
    for vendor in list_vendors():
        for path, _type in iter_profile_files(os.path.join(PROFILES_DIR, vendor)):
            with open(path, "rb") as f:
                text = f.read().decode("utf-8")
            sid, inst, _name = read_profile(path)
            if not should_remove(sid, inst, text):
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
        "settings_id", lambda sid, inst, text: '"settings_id"' in text
    )


def strip_base_setting_ids():
    """Remove setting_id from every base profile (instantiation != "true").

    Convention: only instantiated, user-selectable presets carry a setting_id;
    base/template profiles do not. Applied across all vendors.
    """
    return _remove_key_in_tree(
        "setting_id", lambda sid, inst, text: bool(sid) and inst != "true"
    )


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

    # 1. Strip setting_id from base profiles everywhere (only instantiated presets keep one).
    stripped = strip_base_setting_ids()

    # 2. Assign the deterministic setting_id to every instantiated preset of every
    #    non-reserved vendor.
    changed = added = 0
    vendors_touched = []
    for vendor in list_vendors():
        if vendor in RESERVED_VENDORS:
            continue
        vendor_changed = 0
        for path, type_name in iter_profile_files(os.path.join(PROFILES_DIR, vendor)):
            sid, inst, name = read_profile(path)
            if inst != "true":
                continue
            if not name:
                raise RuntimeError(f"instantiated preset has no \"name\": {path}")
            new_id = generate_preset_setting_id(vendor, type_name, name)
            if sid == new_id:
                continue  # already correct - idempotent
            rewrite_file(path, new_id, has_setting_id=sid is not None)
            changed += 1
            vendor_changed += 1
            if sid is None:
                added += 1
        if vendor_changed:
            vendors_touched.append((vendor, vendor_changed))

    print(f"Misspelled settings_id removed : {typos}")
    print(f"Base setting_ids stripped : {stripped}")
    print(f"Reserved vendors : {sorted(RESERVED_VENDORS)}")
    print(f"Vendors updated  : {len(vendors_touched)}")
    for v, n in vendors_touched:
        print(f"    {v}  ({n} files)")
    print(f"Files rewritten  : {changed}  (of which newly assigned: {added})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
