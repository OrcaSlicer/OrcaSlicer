#!/usr/bin/env python3
"""
3MF Project Analyzer for H2C Variant Overrides

Analyzes a .3mf project file and extracts:
1. All objects with their names and extruder assignments
2. Variant Overrides (VO) per object — per-variant speed/accel/jerk values
3. Global project settings (preset-level VO)
4. Summary table with per-object, per-extruder variant parameters

BBL H2C extruder mapping:
  - Left extruder  = extruder_id 0 (BBL convention: T0)
  - Right extruder = extruder_id 1 (BBL convention: T1)
  
  Variant indices within each extruder:
    Left  variants: index 0, 1  (e.g., 0.4mm standard, 0.4mm high-flow)
    Right variants: index 2, 3  (if applicable)
    
  In VO arrays: [val_variant0, val_variant1, val_variant2, val_variant3]
"""

import os
import sys
import re
import json
import argparse
import zipfile
import xml.etree.ElementTree as ET
from collections import defaultdict, OrderedDict

# Keys that support variant overrides (from VariantOverrides.cpp)
VARIANT_KEYS = {
    # Speeds
    "initial_layer_speed", "initial_layer_infill_speed", "initial_layer_travel_speed",
    "slow_down_layers",
    "outer_wall_speed", "inner_wall_speed", "small_perimeter_speed", "small_perimeter_threshold",
    "sparse_infill_speed", "internal_solid_infill_speed", "top_surface_speed",
    "enable_overhang_speed", "slowdown_for_curled_perimeters",
    "overhang_1_4_speed", "overhang_2_4_speed", "overhang_3_4_speed", "overhang_4_4_speed",
    "bridge_speed", "internal_bridge_speed", "gap_infill_speed",
    "support_speed", "support_interface_speed",
    "travel_speed", "travel_speed_z",
    # Acceleration
    "default_acceleration", "initial_layer_acceleration", "initial_layer_travel_acceleration",
    "outer_wall_acceleration", "inner_wall_acceleration",
    "sparse_infill_acceleration", "internal_solid_infill_acceleration",
    "bridge_acceleration", "top_surface_acceleration", "travel_acceleration",
    # Jerk
    "default_jerk", "outer_wall_jerk", "inner_wall_jerk", "top_surface_jerk",
    "infill_jerk", "initial_layer_jerk", "travel_jerk", "initial_layer_travel_jerk",
    # Advanced
    "max_volumetric_extrusion_rate_slope", "max_volumetric_extrusion_rate_slope_segment_length",
    "extrusion_rate_smoothing_external_perimeter_only",
    # Extruder identity
    "print_extruder_id", "print_extruder_variant",
}

# Categories for display grouping
CATEGORY_MAP = OrderedDict([
    ("Speeds", [
        "outer_wall_speed", "inner_wall_speed", "small_perimeter_speed",
        "sparse_infill_speed", "internal_solid_infill_speed", "top_surface_speed",
        "bridge_speed", "internal_bridge_speed", "gap_infill_speed",
        "support_speed", "support_interface_speed",
        "travel_speed", "travel_speed_z",
        "initial_layer_speed", "initial_layer_infill_speed", "initial_layer_travel_speed",
        "overhang_1_4_speed", "overhang_2_4_speed", "overhang_3_4_speed", "overhang_4_4_speed",
    ]),
    ("Acceleration", [
        "default_acceleration", "outer_wall_acceleration", "inner_wall_acceleration",
        "sparse_infill_acceleration", "internal_solid_infill_acceleration",
        "bridge_acceleration", "top_surface_acceleration", "travel_acceleration",
        "initial_layer_acceleration", "initial_layer_travel_acceleration",
    ]),
    ("Jerk", [
        "default_jerk", "outer_wall_jerk", "inner_wall_jerk", "top_surface_jerk",
        "infill_jerk", "initial_layer_jerk", "travel_jerk", "initial_layer_travel_jerk",
    ]),
    ("Advanced", [
        "max_volumetric_extrusion_rate_slope",
        "max_volumetric_extrusion_rate_slope_segment_length",
        "extrusion_rate_smoothing_external_perimeter_only",
    ]),
    ("Extruder", [
        "print_extruder_id", "print_extruder_variant",
        "slow_down_layers", "small_perimeter_threshold",
        "enable_overhang_speed", "slowdown_for_curled_perimeters",
    ]),
])


def parse_value(value_str):
    """Parse a config value string. Returns list of floats if multi-value, else single value."""
    value_str = value_str.strip()
    # Check for array-like values (comma or semicolon separated)
    if ',' in value_str:
        parts = [p.strip() for p in value_str.split(',')]
    elif ';' in value_str:
        parts = [p.strip() for p in value_str.split(';')]
    else:
        parts = [value_str]
    
    result = []
    for p in parts:
        try:
            # Handle percent values
            if p.endswith('%'):
                result.append(p)  # keep as string with %
            else:
                result.append(float(p))
        except ValueError:
            result.append(p)
    
    return result if len(result) > 1 else result[0] if result else value_str


def parse_model_settings_xml(content):
    """Parse model_settings.config XML to extract per-object configs.
    
    Format:
      <config>
        <object id="N">
          <metadata key="name" value="ObjectName"/>
          <metadata key="outer_wall_speed" value="65"/>  (scalar — no VO)
          <metadata key="outer_wall_speed" value="65,500,45,500"/>  (multi — VO array)
          <metadata key="extruder" value="1"/>
          ...
          <part id="N" subtype="normal_part">
            <metadata key="..." value="..."/>
          </part>
        </object>
      </config>
    """
    objects = {}
    
    try:
        root = ET.fromstring(content)
    except ET.ParseError:
        # Try fixing common issues
        content = content.strip()
        if not content.startswith('<?xml'):
            content = '<?xml version="1.0" encoding="UTF-8"?>\n' + content
        try:
            root = ET.fromstring(content)
        except ET.ParseError as e:
            print(f"  ⚠ Failed to parse model_settings.config XML: {e}", file=sys.stderr)
            return objects

    for obj_elem in root.iter('object'):
        obj_id = obj_elem.get('id', 'unknown')
        obj_data = {
            'id': obj_id,
            'name': '',
            'extruder': None,
            'config': {},
            'variant_keys': {},  # keys that have multi-value (VO arrays)
            'volumes': [],
        }
        
        for meta in obj_elem.findall('metadata'):
            key = meta.get('key', '')
            value = meta.get('value', '')
            
            if key == 'name':
                obj_data['name'] = value
            elif key == 'extruder':
                obj_data['extruder'] = value
            else:
                parsed = parse_value(value)
                obj_data['config'][key] = parsed
                # Detect multi-value variant overrides
                if key in VARIANT_KEYS and isinstance(parsed, list) and len(parsed) > 1:
                    obj_data['variant_keys'][key] = parsed
        
        # Parse volumes (parts)
        for part in obj_elem.findall('part'):
            vol_data = {
                'id': part.get('id', ''),
                'subtype': part.get('subtype', ''),
                'config': {},
            }
            for meta in part.findall('metadata'):
                key = meta.get('key', '')
                value = meta.get('value', '')
                vol_data['config'][key] = parse_value(value)
            obj_data['volumes'].append(vol_data)
        
        objects[obj_id] = obj_data
    
    return objects


def parse_project_settings_json(content):
    """Parse project_settings.config (JSON) for global preset VO."""
    try:
        settings = json.loads(content)
    except json.JSONDecodeError as e:
        print(f"  ⚠ Failed to parse project_settings.config JSON: {e}", file=sys.stderr)
        return {}, {}
    
    global_config = {}
    global_vo = {}
    
    for key, value in settings.items():
        if key in VARIANT_KEYS:
            if isinstance(value, list) and len(value) > 1:
                global_vo[key] = value
                global_config[key] = value[0]  # scalar = first variant
            elif isinstance(value, list) and len(value) == 1:
                global_config[key] = value[0]
            else:
                global_config[key] = value
        elif key in ('extruder', 'printer_extruder_id'):
            global_config[key] = value
    
    return global_config, global_vo


def parse_slice_info(content):
    """Parse slice_info.config for object metadata."""
    objects_info = {}
    for match in re.finditer(r'<object\s+identify_id="(\d+)"\s+name="([^"]+)"', content):
        objects_info[match.group(1)] = match.group(2)
    return objects_info


def get_variant_label(idx, total_variants):
    """Map variant index to human-readable label.
    
    BBL H2C convention:
      idx 0 = Left Standard (0.4mm)
      idx 1 = Left High-Flow (0.6mm)  
      idx 2 = Right Standard (0.4mm)
      idx 3 = Right High-Flow (0.6mm)
    """
    labels_4 = ["L-Std(0.4)", "L-HF(0.6)", "R-Std(0.4)", "R-HF(0.6)"]
    labels_2 = ["Left", "Right"]
    
    if total_variants == 4:
        return labels_4[idx] if idx < 4 else f"V{idx}"
    elif total_variants == 2:
        return labels_2[idx] if idx < 2 else f"V{idx}"
    else:
        return f"V{idx}"


def get_extruder_for_variant(idx, total_variants):
    """Return which extruder a variant index belongs to."""
    if total_variants <= 2:
        return idx  # 0=Left, 1=Right
    else:
        return idx // 2  # 0,1=Left  2,3=Right


def format_table(headers, rows, title=None):
    """Format a table with aligned columns."""
    # Calculate column widths
    col_widths = [len(h) for h in headers]
    for row in rows:
        for i, cell in enumerate(row):
            if i < len(col_widths):
                col_widths[i] = max(col_widths[i], len(str(cell)))
    
    lines = []
    if title:
        lines.append(f"\n{'='*60}")
        lines.append(f"  {title}")
        lines.append(f"{'='*60}")
    
    # Header
    header_line = " │ ".join(str(h).ljust(col_widths[i]) for i, h in enumerate(headers))
    lines.append(header_line)
    lines.append("─┼─".join("─" * w for w in col_widths))
    
    # Rows
    for row in rows:
        cells = []
        for i, cell in enumerate(row):
            w = col_widths[i] if i < len(col_widths) else 20
            cells.append(str(cell).ljust(w))
        lines.append(" │ ".join(cells))
    
    return "\n".join(lines)


def analyze_3mf(filepath, show_all=False):
    """Main analysis function."""
    if not os.path.exists(filepath):
        print(f"Error: File not found: {filepath}", file=sys.stderr)
        sys.exit(1)
    
    if not zipfile.is_zipfile(filepath):
        print(f"Error: Not a valid ZIP/3MF file: {filepath}", file=sys.stderr)
        sys.exit(1)
    
    print(f"\n📦 Analyzing: {os.path.basename(filepath)}")
    print(f"   Path: {filepath}")
    
    objects = {}
    global_config = {}
    global_vo = {}
    slice_objects = {}
    nozzle_info = {}
    
    with zipfile.ZipFile(filepath, 'r') as z:
        filelist = z.namelist()
        print(f"   Files in archive: {len(filelist)}")
        
        # 1. Parse model_settings.config (per-object configs)
        model_settings_files = [f for f in filelist if f.endswith('model_settings.config')]
        for msf in model_settings_files:
            content = z.read(msf).decode('utf-8', errors='ignore')
            objects.update(parse_model_settings_xml(content))
            print(f"   ✓ Parsed {msf}: {len(objects)} objects")
        
        # 2. Parse project_settings.config (global preset)
        proj_files = [f for f in filelist if f.endswith('project_settings.config')]
        for pf in proj_files:
            content = z.read(pf).decode('utf-8', errors='ignore')
            global_config, global_vo = parse_project_settings_json(content)
            print(f"   ✓ Parsed {pf}: {len(global_config)} keys, {len(global_vo)} VO keys")
        
        # 3. Parse slice_info.config (object names)
        slice_files = [f for f in filelist if f.endswith('slice_info.config')]
        for sf in slice_files:
            content = z.read(sf).decode('utf-8', errors='ignore')
            slice_objects = parse_slice_info(content)
            if slice_objects:
                print(f"   ✓ Parsed {sf}: {len(slice_objects)} objects")
        
        # 4. Try to get nozzle/extruder info from project settings
        if proj_files:
            content = z.read(proj_files[0]).decode('utf-8', errors='ignore')
            try:
                settings = json.loads(content)
                nozzle_info = {
                    'nozzle_diameter': settings.get('nozzle_diameter', []),
                    'extruder_count': len(settings.get('nozzle_diameter', [1])),
                    'printer_model': settings.get('printer_model', 'unknown'),
                    'nozzle_volume_type': settings.get('nozzle_volume_type', []),
                    'extruder_nozzle_stats': settings.get('extruder_nozzle_stats', []),
                }
            except json.JSONDecodeError:
                pass
    
    # Determine total variant count from global VO
    total_variants = 0
    if global_vo:
        for vals in global_vo.values():
            if isinstance(vals, list):
                total_variants = max(total_variants, len(vals))
    
    # Also check per-object VO
    for obj in objects.values():
        for vals in obj.get('variant_keys', {}).values():
            if isinstance(vals, list):
                total_variants = max(total_variants, len(vals))
    
    if total_variants == 0:
        total_variants = 2  # default assumption for H2C
    
    # ── Print Report ──
    
    print(f"\n{'='*60}")
    print(f"  PRINTER & NOZZLE INFO")
    print(f"{'='*60}")
    if nozzle_info:
        print(f"  Printer model:     {nozzle_info.get('printer_model', '?')}")
        print(f"  Nozzle diameters:  {nozzle_info.get('nozzle_diameter', '?')}")
        print(f"  Extruder count:    {nozzle_info.get('extruder_count', '?')}")
        print(f"  Nozzle stats:      {nozzle_info.get('extruder_nozzle_stats', '?')}")
        print(f"  Total variants:    {total_variants}")
    
    # ── Global VO Table ──
    if global_vo:
        variant_headers = ["Parameter"] + [get_variant_label(i, total_variants) for i in range(total_variants)]
        rows = []
        
        for category, keys in CATEGORY_MAP.items():
            category_has_data = False
            for key in keys:
                if key in global_vo:
                    if not category_has_data:
                        rows.append([f"── {category} ──"] + [""] * total_variants)
                        category_has_data = True
                    vals = global_vo[key]
                    row = [key]
                    for i in range(total_variants):
                        if i < len(vals):
                            v = vals[i]
                            row.append(str(v) if not isinstance(v, float) or v != int(v) else str(int(v)))
                        else:
                            row.append("—")
                    rows.append(row)
        
        if rows:
            print(format_table(variant_headers, rows, "GLOBAL PRESET — Variant Overrides"))
    else:
        print(f"\n  ℹ No global Variant Overrides found (single-variant preset)")
    
    # ── Per-Object Tables ──
    if objects:
        print(format_table(
            ["ID", "Name", "Extruder", "VO Keys", "Config Keys"],
            [
                [
                    obj['id'],
                    obj['name'] or slice_objects.get(obj['id'], '(unnamed)'),
                    obj.get('extruder', '—'),
                    len(obj.get('variant_keys', {})),
                    len(obj.get('config', {})),
                ]
                for obj in objects.values()
            ],
            "OBJECTS SUMMARY"
        ))
        
        for obj_id, obj in objects.items():
            obj_name = obj['name'] or slice_objects.get(obj_id, f'Object #{obj_id}')
            extruder = obj.get('extruder', '?')
            
            # Determine effective extruder (BBL: 1-based in config, 0-based internally)
            ext_display = f"Extruder {extruder}"
            try:
                ext_idx = int(extruder) - 1 if extruder else 0
                ext_display = f"{'Left' if ext_idx == 0 else 'Right'} (T{ext_idx})"
            except (ValueError, TypeError):
                ext_idx = 0
            
            vo_keys = obj.get('variant_keys', {})
            config = obj.get('config', {})
            
            if vo_keys or show_all:
                print(f"\n{'─'*60}")
                print(f"  OBJECT: {obj_name} (id={obj_id}, {ext_display})")
                print(f"{'─'*60}")
                
                if vo_keys:
                    variant_headers = ["Parameter"] + [get_variant_label(i, total_variants) for i in range(total_variants)] + ["Diff?"]
                    rows = []
                    
                    for category, keys in CATEGORY_MAP.items():
                        category_has_data = False
                        for key in keys:
                            if key in vo_keys:
                                if not category_has_data:
                                    rows.append([f"── {category} ──"] + [""] * (total_variants + 1))
                                    category_has_data = True
                                vals = vo_keys[key]
                                row = [key]
                                unique_vals = set()
                                for i in range(total_variants):
                                    if i < len(vals):
                                        v = vals[i]
                                        v_str = str(v) if not isinstance(v, float) or v != int(v) else str(int(v))
                                        row.append(v_str)
                                        unique_vals.add(v_str)
                                    else:
                                        row.append("—")
                                # Mark if values differ across variants
                                row.append("✦" if len(unique_vals) > 1 else "=")
                                rows.append(row)
                    
                    if rows:
                        print(format_table(variant_headers, rows))
                    
                    # Also show comparison with global
                    if global_vo:
                        diff_rows = []
                        for key, obj_vals in vo_keys.items():
                            if key in global_vo:
                                glob_vals = global_vo[key]
                                differs = False
                                for i in range(min(len(obj_vals), len(glob_vals))):
                                    try:
                                        if abs(float(obj_vals[i]) - float(glob_vals[i])) > 0.01:
                                            differs = True
                                            break
                                    except (ValueError, TypeError):
                                        if str(obj_vals[i]) != str(glob_vals[i]):
                                            differs = True
                                            break
                                if differs:
                                    diff_rows.append([
                                        key,
                                        str(glob_vals),
                                        str(obj_vals),
                                    ])
                        if diff_rows:
                            print(format_table(
                                ["Parameter", "Global VO", "Object VO"],
                                diff_rows,
                                f"  DIFF: {obj_name} vs Global"
                            ))
                
                # Show non-VO per-object overrides
                non_vo_overrides = {k: v for k, v in config.items() 
                                     if k not in vo_keys and k not in ('name', 'module')
                                     and k in VARIANT_KEYS}
                if non_vo_overrides and show_all:
                    rows = [[k, str(v)] for k, v in sorted(non_vo_overrides.items())]
                    print(format_table(
                        ["Parameter", "Scalar Value"],
                        rows,
                        f"  {obj_name}: Scalar Overrides (no VO)"
                    ))
    else:
        print(f"\n  ℹ No per-object configs found in model_settings.config")
    
    # ── Effective Values per Object ──
    # Build a combined view: for each object, show what the slicer would use
    if objects and (global_vo or any(o.get('variant_keys') for o in objects.values())):
        print(f"\n{'='*60}")
        print(f"  EFFECTIVE VALUES (what slicer uses per object)")
        print(f"{'='*60}")
        
        # Key subset for compact view
        key_subset = [
            "outer_wall_speed", "inner_wall_speed", "sparse_infill_speed",
            "top_surface_speed", "bridge_speed", "travel_speed",
            "outer_wall_acceleration", "inner_wall_acceleration",
            "default_jerk", "outer_wall_jerk",
        ]
        
        for obj_id, obj in objects.items():
            obj_name = obj['name'] or f'Object #{obj_id}'
            extruder = obj.get('extruder', '1')
            try:
                ext_idx = int(extruder) - 1
            except (ValueError, TypeError):
                ext_idx = 0
            
            # For this object's extruder, which variant indices apply?
            # BBL: Left=extruder 0 → variants 0,1; Right=extruder 1 → variants 2,3
            if total_variants == 4:
                active_variants = [ext_idx * 2, ext_idx * 2 + 1]
            elif total_variants == 2:
                active_variants = [ext_idx]
            else:
                active_variants = list(range(total_variants))
            
            active_labels = [get_variant_label(i, total_variants) for i in active_variants]
            headers = ["Parameter"] + active_labels
            rows = []
            
            for key in key_subset:
                # Object VO takes priority, then global VO, then global scalar
                obj_vo = obj.get('variant_keys', {})
                if key in obj_vo:
                    source_vals = obj_vo[key]
                    source = "obj"
                elif key in global_vo:
                    source_vals = global_vo[key]
                    source = "glob"
                elif key in obj.get('config', {}):
                    v = obj['config'][key]
                    source_vals = [v] * total_variants
                    source = "obj-scalar"
                elif key in global_config:
                    source_vals = [global_config[key]] * total_variants
                    source = "glob-scalar"
                else:
                    continue
                
                row = [f"{key} ({source})"]
                for vi in active_variants:
                    if vi < len(source_vals):
                        v = source_vals[vi]
                        v_str = str(int(v)) if isinstance(v, float) and v == int(v) else str(v)
                    else:
                        v_str = "—"
                    row.append(v_str)
                rows.append(row)
            
            if rows:
                ext_label = "Left" if ext_idx == 0 else "Right"
                print(f"\n  {obj_name} — {ext_label} extruder (T{ext_idx})")
                print(format_table(headers, rows))
    
    print(f"\n{'='*60}")
    print(f"  Analysis complete.")
    print(f"{'='*60}\n")


def main():
    parser = argparse.ArgumentParser(
        description="Analyze H2C Variant Overrides in OrcaSlicer 3MF project files",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s project.3mf
  %(prog)s --all project.3mf          # Show all keys including non-VO
  %(prog)s /path/to/saved_project.3mf
        """
    )
    parser.add_argument("file", help="Path to .3mf project file")
    parser.add_argument("--all", "-a", action="store_true", default=False,
                        help="Show all variant keys, including scalar-only (no VO)")
    
    args = parser.parse_args()
    analyze_3mf(args.file, show_all=args.all)


if __name__ == "__main__":
    main()
