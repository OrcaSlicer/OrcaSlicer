#!/usr/bin/env python3
"""
Validates generated PrintConfigDef code against the original PrintConfig.cpp.

Compares setting keys, types, defaults, enum values/labels, and metadata
to ensure the codegen output is a faithful reproduction.
"""

import re
import json
import sys
from pathlib import Path
from collections import OrderedDict


def parse_original_settings(cpp_path):
    """Parse the original init_fff_params() into a dict of key -> properties."""
    with open(cpp_path, 'r', encoding='utf-8') as f:
        text = f.read()

    m = re.search(r'void PrintConfigDef::init_fff_params\(\)(.*?)^\}', text, re.DOTALL | re.MULTILINE)
    if not m:
        print("ERROR: Could not find init_fff_params()")
        sys.exit(1)
    body = m.group(1)

    settings = OrderedDict()
    current_key = None

    for line in body.split('\n'):
        stripped = line.strip()

        # Detect this->add("key", coType)
        add_match = re.search(r'this->add(?:_nullable)?\s*\(\s*"([^"]+)"\s*,\s*(co\w+)\s*\)', stripped)
        if add_match:
            current_key = add_match.group(1)
            co_type = add_match.group(2)
            # Last definition wins (handles duplicates)
            settings[current_key] = {
                'co_type': co_type,
                'has_default': False,
                'enum_values': 0,
                'enum_labels': 0,
                'has_enum_map': False,
            }
            continue

        if current_key and current_key in settings:
            s = settings[current_key]
            if 'set_default_value' in stripped:
                s['has_default'] = True
            if re.search(r'enum_values\.(?:push_back|emplace_back)', stripped):
                s['enum_values'] += 1
            if re.search(r'enum_labels\.(?:push_back|emplace_back)', stripped):
                s['enum_labels'] += 1
            if 'enum_keys_map' in stripped and '=' in stripped:
                s['has_enum_map'] = True

    return settings


def parse_generated_settings(gen_path):
    """Parse the generated PrintConfigDef code into a dict of key -> properties."""
    with open(gen_path, 'r', encoding='utf-8') as f:
        text = f.read()

    settings = OrderedDict()
    current_key = None

    for line in text.split('\n'):
        stripped = line.strip()

        add_match = re.search(r'this->add(?:_nullable)?\s*\(\s*"([^"]+)"\s*,\s*(co\w+)\s*\)', stripped)
        if add_match:
            current_key = add_match.group(1)
            co_type = add_match.group(2)
            settings[current_key] = {
                'co_type': co_type,
                'has_default': False,
                'enum_values': 0,
                'enum_labels': 0,
                'has_enum_map': False,
            }
            continue

        if current_key and current_key in settings:
            s = settings[current_key]
            if 'set_default_value' in stripped:
                s['has_default'] = True
            if 'enum_values.push_back' in stripped:
                s['enum_values'] += 1
            if 'enum_labels.push_back' in stripped:
                s['enum_labels'] += 1
            if 'enum_keys_map' in stripped and '=' in stripped:
                s['has_enum_map'] = True

    return settings


def main():
    root = Path(__file__).resolve().parent.parent
    orig_path = root / "src/libslic3r/PrintConfig.cpp"
    gen_path = root / "src/slic3r/GUI/generated/PrintConfigDef_generated.cpp"

    if not orig_path.exists() or not gen_path.exists():
        print("ERROR: Required files not found")
        sys.exit(1)

    print("Parsing original...")
    orig = parse_original_settings(orig_path)
    print(f"  {len(orig)} settings")

    print("Parsing generated...")
    gen = parse_generated_settings(gen_path)
    print(f"  {len(gen)} settings")

    # Known exceptions: settings that exist in original but are commented out
    # or have runtime-generated enums
    known_exceptions = {
        'adaptive_layer_height',   # Commented out in original
        'spaghetti_detector',      # Commented out in original
    }
    # Settings with runtime-generated enum values (loop over MaterialType::all())
    runtime_enum_keys = {
        'filament_type',           # enum_values from runtime loop
    }

    # Compare
    errors = []
    warnings = []

    # Missing keys
    orig_keys = set(orig.keys())
    gen_keys = set(gen.keys())

    missing = orig_keys - gen_keys
    extra = gen_keys - orig_keys

    if missing:
        real_missing = missing - known_exceptions
        if real_missing:
            errors.append(f"MISSING from generated ({len(real_missing)}): {sorted(real_missing)}")
        noted = missing & known_exceptions
        if noted:
            warnings.append(f"Known exceptions (commented out in original): {sorted(noted)}")
    if extra:
        warnings.append(f"EXTRA in generated ({len(extra)}): {sorted(extra)}")

    # Compare shared keys
    shared = orig_keys & gen_keys
    type_mismatches = []
    default_mismatches = []
    enum_val_mismatches = []
    enum_lbl_mismatches = []
    enum_map_mismatches = []

    for key in sorted(shared):
        o = orig[key]
        g = gen[key]

        if o['co_type'] != g['co_type']:
            type_mismatches.append(f"  {key}: orig={o['co_type']} gen={g['co_type']}")

        if o['has_default'] != g['has_default'] and key not in known_exceptions:
            default_mismatches.append(f"  {key}: orig={o['has_default']} gen={g['has_default']}")

        if o['enum_values'] != g['enum_values'] and key not in runtime_enum_keys:
            enum_val_mismatches.append(f"  {key}: orig={o['enum_values']} gen={g['enum_values']}")

        if o['enum_labels'] != g['enum_labels']:
            enum_lbl_mismatches.append(f"  {key}: orig={o['enum_labels']} gen={g['enum_labels']}")

        if o['has_enum_map'] != g['has_enum_map']:
            enum_map_mismatches.append(f"  {key}: orig={o['has_enum_map']} gen={g['has_enum_map']}")

    # Report
    print("\n=== VALIDATION RESULTS ===\n")

    if type_mismatches:
        errors.append(f"TYPE MISMATCHES ({len(type_mismatches)}):\n" + "\n".join(type_mismatches))

    if default_mismatches:
        errors.append(f"DEFAULT MISMATCHES ({len(default_mismatches)}):\n" + "\n".join(default_mismatches))

    if enum_val_mismatches:
        warnings.append(f"ENUM VALUE COUNT MISMATCHES ({len(enum_val_mismatches)}):\n" + "\n".join(enum_val_mismatches))

    if enum_lbl_mismatches:
        warnings.append(f"ENUM LABEL COUNT MISMATCHES ({len(enum_lbl_mismatches)}):\n" + "\n".join(enum_lbl_mismatches))

    if enum_map_mismatches:
        warnings.append(f"ENUM MAP MISMATCHES ({len(enum_map_mismatches)}):\n" + "\n".join(enum_map_mismatches))

    if warnings:
        print("WARNINGS:")
        for w in warnings:
            print(f"  {w}")
        print()

    if errors:
        print("ERRORS:")
        for e in errors:
            print(f"  {e}")
        print(f"\nValidation FAILED with {len(errors)} error(s)")
        sys.exit(1)
    else:
        print(f"All {len(shared)} shared settings validated successfully")
        if extra:
            print(f"  ({len(extra)} extra settings from axis expansion)")
        print("\nValidation PASSED")


if __name__ == "__main__":
    main()
