#!/usr/bin/env python3
"""
3MF Project Analyzer for H2C Variant Overrides

Analyzes a .3mf sliced project (contains G-code) and extracts:
1. All objects from G-code comments (OBJECT: / OBJECT_ID:)
2. Per-object tool (extruder) usage + feedrates per feature PER TOOL
3. Variant Override arrays from project_settings.config JSON (BBS file order)
4. Maps each object's active tool → variant index → effective VO values
5. Per-object tables: derives ACTUAL VO from G-code feedrates per tool,
   compares with global VO, marks differences.

BBL H2C extruder/variant mapping:
  print_extruder_id = 1,2  → Left=1 (T0), Right=2 (T3 in firmware)
  
  Variant indices in VO arrays (4-variant, BBS file order):
    idx 0 = Left-Standard   (extruder 1)
    idx 1 = Left-HighFlow    (extruder 1, different nozzle volume)
    idx 2 = Right-Standard   (extruder 2)
    idx 3 = Right-HighFlow   (extruder 2, different nozzle volume)
  
  Tool → variant index mapping uses extruder_nozzle_stats.
  
  G-code footer config is in INTERNAL Orca order (Right, Left).
  project_settings.config JSON is in BBS file order (Left, Right).
  We prefer JSON as the source of truth for VO arrays.
"""

import os
import sys
import re
import json
import argparse
import zipfile
import io
from collections import defaultdict, OrderedDict, Counter

# Keys that support variant overrides (from VariantOverrides.cpp)
VARIANT_KEYS = {
    "initial_layer_speed", "initial_layer_infill_speed", "initial_layer_travel_speed",
    "slow_down_layers",
    "outer_wall_speed", "inner_wall_speed", "small_perimeter_speed", "small_perimeter_threshold",
    "sparse_infill_speed", "internal_solid_infill_speed", "top_surface_speed",
    "enable_overhang_speed", "slowdown_for_curled_perimeters",
    "overhang_1_4_speed", "overhang_2_4_speed", "overhang_3_4_speed", "overhang_4_4_speed",
    "bridge_speed", "internal_bridge_speed", "gap_infill_speed",
    "support_speed", "support_interface_speed",
    "travel_speed", "travel_speed_z",
    "default_acceleration", "initial_layer_acceleration", "initial_layer_travel_acceleration",
    "outer_wall_acceleration", "inner_wall_acceleration",
    "sparse_infill_acceleration", "internal_solid_infill_acceleration",
    "bridge_acceleration", "top_surface_acceleration", "travel_acceleration",
    "default_jerk", "outer_wall_jerk", "inner_wall_jerk", "top_surface_jerk",
    "infill_jerk", "initial_layer_jerk", "travel_jerk", "initial_layer_travel_jerk",
    "max_volumetric_extrusion_rate_slope", "max_volumetric_extrusion_rate_slope_segment_length",
    "extrusion_rate_smoothing_external_perimeter_only",
    "print_extruder_id", "print_extruder_variant",
}

# G-code feature name → VO speed key
FEATURE_TO_VO_KEY = {
    "Outer wall":            "outer_wall_speed",
    "Inner wall":            "inner_wall_speed",
    "Sparse infill":         "sparse_infill_speed",
    "Internal solid infill": "internal_solid_infill_speed",
    "Top surface":           "top_surface_speed",
    "Bottom surface":        "internal_solid_infill_speed",
    "Bridge":                "bridge_speed",
    "Internal Bridge":       "internal_bridge_speed",
    "Gap infill":            "gap_infill_speed",
    "Support":               "support_speed",
    "Support interface":     "support_interface_speed",
}

# Display categories
SPEED_KEYS = [
    "outer_wall_speed", "inner_wall_speed", "small_perimeter_speed",
    "sparse_infill_speed", "internal_solid_infill_speed", "top_surface_speed",
    "bridge_speed", "internal_bridge_speed", "gap_infill_speed",
    "support_speed", "support_interface_speed",
    "travel_speed",
    "initial_layer_speed", "initial_layer_infill_speed",
]
ACCEL_KEYS = [
    "default_acceleration", "outer_wall_acceleration", "inner_wall_acceleration",
    "sparse_infill_acceleration", "internal_solid_infill_acceleration",
    "bridge_acceleration", "top_surface_acceleration", "travel_acceleration",
    "initial_layer_acceleration",
]
JERK_KEYS = [
    "default_jerk", "outer_wall_jerk", "inner_wall_jerk", "top_surface_jerk",
    "infill_jerk", "initial_layer_jerk", "travel_jerk",
]


class ThreeMFAnalyzer:
    def __init__(self, filepath):
        self.filepath = filepath
        self.filename = os.path.basename(filepath)
        
        # From G-code parsing
        self.objects = {}          # obj_id -> {name, tools, features, extrusion, layers, tool_per_layer}
        self.object_names = {}     # obj_id -> name string
        self.total_layers = 0
        self.total_lines = 0
        self.toolchanges = 0
        
        # From config footer / project_settings
        self.config_params = {}    # key -> raw string value
        self.vo_arrays = {}        # key -> [v0, v1, v2, v3]  (BBS file order)
        self.variant_count = 0
        
        # Nozzle/extruder info
        self.nozzle_diameter = []
        self.extruder_count = 0
        self.printer_model = ""
        self.nozzle_stats = []
        self.tool_to_variant = {}  # firmware tool idx -> list of variant indices
        
        # Feedrate tracking: per object, per feature, per tool (filament slot)
        # obj_feedrates[obj_id][(feature, tool)] = [F_values_in_mm_per_min]
        self.obj_feedrates = defaultdict(lambda: defaultdict(list))
        
    def parse(self):
        if not zipfile.is_zipfile(self.filepath):
            print(f"Error: Not a valid ZIP/3MF: {self.filepath}", file=sys.stderr)
            sys.exit(1)
        
        with zipfile.ZipFile(self.filepath, 'r') as z:
            # 1. Parse project_settings.config (JSON) — global preset with VO
            for name in z.namelist():
                if name.endswith("project_settings.config"):
                    content = z.read(name).decode('utf-8', errors='ignore')
                    self._parse_project_settings(content)
            
            # 2. Parse slice_info.config for object names
            for name in z.namelist():
                if name.endswith("slice_info.config") and not name.endswith(".rels"):
                    content = z.read(name).decode('utf-8', errors='ignore')
                    self._parse_slice_info(content)
            
            # 3. Parse G-code for objects, tools, feedrates, config footer
            gcode_files = [n for n in z.namelist() if n.endswith(".gcode")]
            if gcode_files:
                with z.open(gcode_files[0], 'r') as f_zip:
                    f = io.TextIOWrapper(f_zip, encoding='utf-8', errors='ignore')
                    self._parse_gcode(f)
        
        # Build tool → variant mapping
        self._build_tool_variant_map()
    
    def _parse_project_settings(self, content):
        """Parse project_settings.config JSON for global VO arrays."""
        try:
            settings = json.loads(content)
        except json.JSONDecodeError:
            return
        
        self.printer_model = settings.get("printer_model", "")
        self.nozzle_diameter = settings.get("nozzle_diameter", [])
        self.extruder_count = len(self.nozzle_diameter) if self.nozzle_diameter else 1
        self.nozzle_stats = settings.get("extruder_nozzle_stats", [])
        
        for key, value in settings.items():
            if key in VARIANT_KEYS:
                if isinstance(value, list) and len(value) > 1:
                    self.vo_arrays[key] = value
                    self.variant_count = max(self.variant_count, len(value))
                self.config_params[key] = value
    
    def _parse_slice_info(self, content):
        """Parse slice_info.config XML for object names."""
        for match in re.finditer(r'<object\s+identify_id="(\d+)"\s+name="([^"]+)"', content):
            obj_id = int(match.group(1))
            obj_name = match.group(2)
            self.object_names[obj_id] = obj_name
    
    def _parse_gcode(self, f):
        """Parse G-code stream for objects, tools, feedrates, footer config."""
        current_layer = 0
        current_tool = -1
        current_feature = "Unknown"
        current_object_id = -1
        last_object_name = None
        
        t_re = re.compile(r'^T(\d+)(?:\s|$)')
        f_re = re.compile(r'F(\d+)')
        e_re = re.compile(r'E(-?\d*\.?\d+)')
        
        for line in f:
            self.total_lines += 1
            s = line.strip()
            
            # Config comments: "; key = value"
            if s.startswith(";"):
                config_m = re.match(r'^;\s*([a-zA-Z0-9_]+)\s*=\s*(.*)', s)
                if config_m:
                    key, val = config_m.group(1).strip(), config_m.group(2).strip()
                    # Parse comma-separated variant arrays from footer
                    if key in VARIANT_KEYS and ',' in val:
                        parts = val.split(',')
                        parsed = []
                        for p in parts:
                            p = p.strip().strip('"')
                            try:
                                parsed.append(float(p) if '.' in p or p.lstrip('-').isdigit() else p)
                            except ValueError:
                                parsed.append(p)
                        if len(parsed) > 1:
                            # Footer is in INTERNAL Orca order (Right,Left).
                            # project_settings.config JSON is in BBS file order (Left,Right).
                            # Since labels come from BBS metadata, prefer JSON.
                            # Only use footer if JSON had NO data for this key.
                            if key not in self.vo_arrays:
                                self.vo_arrays[key] = parsed
                            self.variant_count = max(self.variant_count, len(parsed))
                    self.config_params[key] = val
                
                # Object name comment
                if s.startswith("; OBJECT:"):
                    m = re.match(r'^;\s*OBJECT:\s*(.*)', s)
                    if m:
                        last_object_name = m.group(1).strip()
                
                # Object ID comment
                if "; OBJECT_ID:" in s:
                    m = re.search(r';\s*OBJECT_ID:\s*(\d+)', s)
                    if m:
                        current_object_id = int(m.group(1))
                        if last_object_name and current_object_id not in self.object_names:
                            self.object_names[current_object_id] = last_object_name
                        if current_object_id not in self.objects:
                            self.objects[current_object_id] = {
                                'name': self.object_names.get(current_object_id, 
                                        last_object_name or f'Object #{current_object_id}'),
                                'tools': set(),
                                'features': defaultdict(float),
                                'extrusion': 0.0,
                                'layers': set(),
                                'tool_per_layer': defaultdict(set),
                            }
                
                if "; FEATURE:" in s:
                    current_feature = s.split(":", 1)[1].strip()
                
                if "; CHANGE_LAYER" in s:
                    current_layer += 1
                    current_feature = "Unknown"
                continue
            
            # Tool change
            t_m = t_re.match(s)
            if t_m:
                t_val = int(t_m.group(1))
                if t_val < 20:  # filter T100, T65 etc.
                    current_tool = t_val
                    self.toolchanges += 1
            
            # G1/G0 moves with extrusion
            if current_tool >= 0 and (s.startswith("G1") or s.startswith("G0")):
                if current_object_id >= 0 and current_object_id in self.objects:
                    obj = self.objects[current_object_id]
                    obj['tools'].add(current_tool)
                    obj['layers'].add(current_layer)
                    obj['tool_per_layer'][current_layer].add(current_tool)
                    
                    e_m = e_re.search(s)
                    if e_m:
                        e_val = float(e_m.group(1))
                        if e_val > 0:
                            obj['extrusion'] += e_val
                            obj['features'][current_feature] += e_val
                    
                    f_m = f_re.search(s)
                    if f_m and current_feature not in ("Unknown", "Prime tower"):
                        feedrate = int(f_m.group(1))
                        # Track per (feature, tool) — key for per-object VO reconstruction
                        self.obj_feedrates[current_object_id][(current_feature, current_tool)].append(feedrate)
        
        self.total_layers = current_layer
    
    def _build_tool_variant_map(self):
        """Map G-code T commands (filament slot indices) to variant array indices.
        
        CRITICAL: T in G-code is the FILAMENT SLOT index (0-based), NOT the
        firmware extruder/tool index.
        
        Mapping chain:
          T0 (filament slot 0) → filament_map[0] = 2 → extruder 2 (Right)
          T3 (filament slot 3) → filament_map[3] = 1 → extruder 1 (Left)
        
        Then extruder → variant indices via print_extruder_id VO array:
          print_extruder_id = [1, 1, 2, 2]
          → variants 0,1 belong to extruder 1 (Left)
          → variants 2,3 belong to extruder 2 (Right)
        
        BBL convention: extruder IDs are 1-based (1=Left, 2=Right).
        Left ≠ 0!
        """
        # 1. Parse filament_map: filament_slot → extruder_id (1-based)
        fm = self.config_params.get("filament_map", "")
        if isinstance(fm, list):
            self.filament_map = [int(x) for x in fm]
        elif isinstance(fm, str) and ',' in fm:
            self.filament_map = [int(x.strip()) for x in fm.split(',')]
        else:
            self.filament_map = []
        
        # 2. Parse print_extruder_id VO array: variant_idx → extruder_id (1-based)
        ext_ids = self.vo_arrays.get("print_extruder_id", [])
        if not ext_ids:
            raw = self.config_params.get("print_extruder_id", "")
            if isinstance(raw, str) and ',' in raw:
                ext_ids = [int(x.strip()) for x in raw.split(',')]
            elif isinstance(raw, list):
                ext_ids = [int(x) for x in raw]
        self._ext_ids = [int(x) for x in ext_ids] if ext_ids else []
        
        # 3. Build extruder_id → [variant_indices]
        self.extruder_to_variants = defaultdict(list)
        for vi, eid in enumerate(self._ext_ids):
            self.extruder_to_variants[eid].append(vi)
        
        # 4. Build filament_slot (T command) → extruder_id
        self.filament_to_extruder = {}
        for slot, eid in enumerate(self.filament_map):
            self.filament_to_extruder[slot] = eid
        
        # 5. Parse print_extruder_variant for labels
        self.variant_labels = self.vo_arrays.get("print_extruder_variant", [])
        if not self.variant_labels:
            raw = self.config_params.get("print_extruder_variant", "")
            if isinstance(raw, str) and ';' in raw:
                self.variant_labels = [v.strip().strip('"') for v in raw.split(';')]
        
        # Debug info
        if self.filament_map:
            print(f"  Filament map:  {self.filament_map}  (slot → extruder_id, 1-based)")
        if self._ext_ids:
            print(f"  VO extruder:   {self._ext_ids}  (variant_idx → extruder_id)")
        for eid, vis in sorted(self.extruder_to_variants.items()):
            ext_name = "Left" if eid == 1 else "Right" if eid == 2 else f"Ext{eid}"
            labels = [self.get_variant_label(vi) for vi in vis]
            print(f"  Extruder {eid} ({ext_name}): variants {vis} = {labels}")
    
    def get_variant_label(self, idx):
        """Human label for variant index using print_extruder_variant + extruder."""
        eid = self._ext_ids[idx] if idx < len(self._ext_ids) else 0
        ext_name = "L" if eid == 1 else "R" if eid == 2 else f"E{eid}"
        
        if self.variant_labels and idx < len(self.variant_labels):
            vname = str(self.variant_labels[idx])
            short = vname.replace("Direct Drive ", "").replace("Standard", "Std").replace("High Flow", "HF")
            return f"{ext_name}-{short}"
        return f"{ext_name}-V{idx}"
    
    def get_tool_label(self, filament_slot):
        """Human label for a G-code T command (filament slot index)."""
        eid = self.filament_to_extruder.get(filament_slot, 0)
        ext_name = "Left" if eid == 1 else "Right" if eid == 2 else f"Ext{eid}"
        return f"T{filament_slot}→Ext{eid}({ext_name})"
    
    def get_active_variants_for_tool(self, filament_slot):
        """Given a G-code T command (filament slot), return variant indices."""
        eid = self.filament_to_extruder.get(filament_slot, 0)
        return self.extruder_to_variants.get(eid, [0])

    def _get_std_variant_for_tool(self, filament_slot):
        """Get the Standard variant index for a tool's extruder (first variant)."""
        eid = self.filament_to_extruder.get(filament_slot, 0)
        variants = self.extruder_to_variants.get(eid, [])
        return variants[0] if variants else 0
    
    def _derive_actual_speed_per_object(self, obj_id, vo_key):
        """Derive actual speed (mm/s) for a VO speed key from G-code feedrates.
        
        Returns dict: {filament_slot: median_speed_mm_s} for this object.
        """
        # Reverse lookup: VO key → G-code feature names
        key_to_features = defaultdict(list)
        for feat, vk in FEATURE_TO_VO_KEY.items():
            key_to_features[vk].append(feat)
        
        features = key_to_features.get(vo_key, [])
        if not features:
            return {}
        
        result = {}
        obj_data = self.obj_feedrates.get(obj_id, {})
        
        # Collect feedrates per tool for matching features
        tool_feedrates = defaultdict(list)
        for (feat, tool), f_vals in obj_data.items():
            if feat in features:
                tool_feedrates[tool].extend(f_vals)
        
        for tool, frates in tool_feedrates.items():
            if frates:
                sorted_f = sorted(frates)
                median_f = sorted_f[len(sorted_f) // 2]
                result[tool] = median_f / 60.0  # F mm/min → mm/s
        
        return result
    
    def _fmt_val(self, v):
        """Format a VO value for display."""
        if isinstance(v, float) and v == int(v):
            return str(int(v))
        return str(v)

    def format_table(self, headers, rows, title=None):
        """Format aligned table."""
        col_widths = [len(str(h)) for h in headers]
        for row in rows:
            for i, cell in enumerate(row):
                if i < len(col_widths):
                    col_widths[i] = max(col_widths[i], len(str(cell)))
        
        lines = []
        if title:
            lines.append(f"\n{'='*70}")
            lines.append(f"  {title}")
            lines.append(f"{'='*70}")
        
        header_line = " │ ".join(str(h).ljust(col_widths[i]) for i, h in enumerate(headers))
        lines.append(header_line)
        lines.append("─┼─".join("─" * w for w in col_widths))
        
        for row in rows:
            cells = []
            for i, cell in enumerate(row):
                w = col_widths[i] if i < len(col_widths) else 20
                cells.append(str(cell).ljust(w))
            lines.append(" │ ".join(cells))
        
        return "\n".join(lines)
    
    def report(self):
        """Print full analysis report."""
        vc = self.variant_count or 1
        
        # ── Header ──
        print(f"\n{'═'*70}")
        print(f"  H2C VARIANT OVERRIDE ANALYSIS: {self.filename}")
        print(f"{'═'*70}")
        print(f"  Printer:       {self.printer_model}")
        print(f"  Nozzles:       {self.nozzle_diameter}")
        print(f"  Nozzle stats:  {self.nozzle_stats}")
        print(f"  Variants:      {vc}")
        print(f"  Layers:        {self.total_layers}")
        print(f"  G-code lines:  {self.total_lines}")
        print(f"  Toolchanges:   {self.toolchanges}")
        print(f"  Objects:       {len(self.objects)}")
        
        # ── Objects Summary ──
        rows = []
        for obj_id, obj in sorted(self.objects.items()):
            tools = sorted(obj['tools'])
            tool_str = ", ".join(self.get_tool_label(t) for t in tools)
            # Determine active variant indices for this object's tools
            var_indices = set()
            for t in tools:
                var_indices.update(self.get_active_variants_for_tool(t))
            var_str = ", ".join(self.get_variant_label(i) for i in sorted(var_indices))
            
            rows.append([
                obj_id,
                obj['name'],
                tool_str,
                var_str,
                f"{obj['extrusion']:.1f}mm",
                len(obj['layers']),
            ])
        
        print(self.format_table(
            ["ID", "Name", "Tools", "Active Variants", "Extrusion", "Layers"],
            rows,
            "OBJECTS"
        ))
        
        # ── Global VO Table ──
        if self.vo_arrays:
            headers = ["Parameter"] + [self.get_variant_label(i) for i in range(vc)]
            rows = []
            
            def add_section(title, keys):
                has_data = False
                for key in keys:
                    if key in self.vo_arrays:
                        if not has_data:
                            rows.append([f"── {title} ──"] + [""] * vc)
                            has_data = True
                        vals = self.vo_arrays[key]
                        row = [key]
                        for i in range(vc):
                            if i < len(vals):
                                row.append(self._fmt_val(vals[i]))
                            else:
                                row.append("—")
                        rows.append(row)
            
            add_section("Speeds", SPEED_KEYS)
            add_section("Acceleration", ACCEL_KEYS)
            add_section("Jerk", JERK_KEYS)
            
            print(self.format_table(headers, rows, "GLOBAL VARIANT OVERRIDES"))
        
        # ── Per-Object: VO + Actual Feedrates ──
        for obj_id, obj in sorted(self.objects.items()):
            tools = sorted(obj['tools'])
            var_indices = set()
            for t in tools:
                var_indices.update(self.get_active_variants_for_tool(t))
            var_indices = sorted(var_indices)
            
            if not var_indices:
                continue
            
            ext_label = ", ".join(self.get_tool_label(t) for t in tools)
            
            # Build per-object table: Global VO + Actual (detected from G-code)
            # For speed keys: show global value and actual median from G-code
            # Mark ≠ where actual differs from expected
            headers = ["Parameter"] + [self.get_variant_label(i) for i in var_indices]
            rows = []
            
            def add_obj_section(title, keys, is_speed=False):
                has_data = False
                for key in keys:
                    if key not in self.vo_arrays:
                        continue
                    if not has_data:
                        rows.append([f"── {title} ──"] + [""] * len(var_indices))
                        has_data = True
                    
                    vals = self.vo_arrays[key]
                    
                    # Try to get actual speed from G-code feedrates
                    actual_speeds = {}
                    if is_speed and key in FEATURE_TO_VO_KEY.values():
                        actual_speeds = self._derive_actual_speed_per_object(obj_id, key)
                    
                    row = [key]
                    for vi in var_indices:
                        global_val = self._fmt_val(vals[vi]) if vi < len(vals) else "—"
                        
                        # Check if we have actual data for this variant's extruder
                        actual_str = ""
                        if is_speed and actual_speeds:
                            # Find which tool corresponds to this variant
                            eid = self._ext_ids[vi] if vi < len(self._ext_ids) else 0
                            for tool, speed in actual_speeds.items():
                                tool_eid = self.filament_to_extruder.get(tool, 0)
                                if tool_eid == eid:
                                    expected_num = vals[vi] if vi < len(vals) else None
                                    if isinstance(expected_num, (int, float)):
                                        actual_int = int(round(speed))
                                        expected_int = int(expected_num)
                                        if actual_int != expected_int:
                                            actual_str = f"{global_val} (actual≈{actual_int}≠)"
                                        else:
                                            actual_str = global_val
                                    break
                        
                        row.append(actual_str if actual_str else global_val)
                    rows.append(row)
            
            add_obj_section("Speeds", SPEED_KEYS, is_speed=True)
            add_obj_section("Acceleration", ACCEL_KEYS)
            add_obj_section("Jerk", JERK_KEYS)
            
            print(self.format_table(
                headers, rows,
                f"OBJECT: {obj['name']} (id={obj_id}) — {ext_label}"
            ))
            
            # ── Actual feedrates from G-code per tool ──
            obj_data = self.obj_feedrates.get(obj_id, {})
            if obj_data:
                # Group by feature, show per-tool breakdown
                feature_tool_data = defaultdict(dict)
                for (feat, tool), f_vals in obj_data.items():
                    if f_vals:
                        sorted_f = sorted(f_vals)
                        feature_tool_data[feat][tool] = {
                            'min': sorted_f[0],
                            'med': sorted_f[len(sorted_f)//2],
                            'max': sorted_f[-1],
                            'count': len(sorted_f),
                        }
                
                feedrate_rows = []
                for feature in sorted(feature_tool_data.keys()):
                    tool_data = feature_tool_data[feature]
                    vo_key = FEATURE_TO_VO_KEY.get(feature, "")
                    
                    for tool in sorted(tool_data.keys()):
                        d = tool_data[tool]
                        tool_label = self.get_tool_label(tool)
                        
                        # Expected from VO for this tool's standard variant
                        expected_str = ""
                        if vo_key and vo_key in self.vo_arrays:
                            std_vi = self._get_std_variant_for_tool(tool)
                            if std_vi < len(self.vo_arrays[vo_key]):
                                v = self.vo_arrays[vo_key][std_vi]
                                if isinstance(v, (int, float)):
                                    expected_str = f"{int(v)}"
                        
                        feedrate_rows.append([
                            feature,
                            tool_label,
                            f"{d['min']/60:.0f}",
                            f"{d['med']/60:.0f}",
                            f"{d['max']/60:.0f}",
                            expected_str or "—",
                            str(d['count']),
                        ])
                
                if feedrate_rows:
                    print(self.format_table(
                        ["Feature", "Tool", "Min mm/s", "Med mm/s", "Max mm/s", "VO Std", "Moves"],
                        feedrate_rows,
                        f"  ACTUAL FEEDRATES: {obj['name']}"
                    ))
        
        # ── Multi-tool objects: layer-by-layer tool usage ──
        multi_tool_objs = {oid: obj for oid, obj in self.objects.items() if len(obj['tools']) > 1}
        if multi_tool_objs:
            print(f"\n{'='*70}")
            print(f"  MULTI-TOOL OBJECTS — Tool per Layer")
            print(f"{'='*70}")
            for obj_id, obj in sorted(multi_tool_objs.items()):
                tool_layers = defaultdict(list)
                for layer, tools in sorted(obj['tool_per_layer'].items()):
                    for t in tools:
                        tool_layers[t].append(layer)
                
                print(f"\n  {obj['name']} (id={obj_id}):")
                for tool in sorted(tool_layers):
                    layers = tool_layers[tool]
                    # Compact range display
                    ranges = []
                    start = layers[0]
                    prev = layers[0]
                    for l in layers[1:]:
                        if l == prev + 1:
                            prev = l
                        else:
                            ranges.append(f"{start}-{prev}" if start != prev else str(start))
                            start = prev = l
                    ranges.append(f"{start}-{prev}" if start != prev else str(start))
                    print(f"    {self.get_tool_label(tool)}: layers {', '.join(ranges)} ({len(layers)} layers)")
        
        print(f"\n{'═'*70}")
        print(f"  Analysis complete.")
        print(f"{'═'*70}\n")


def main():
    parser = argparse.ArgumentParser(
        description="Analyze H2C Variant Overrides in sliced 3MF project files",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s project.3mf
  %(prog)s ~/Desktop/3cubes.gcode.3mf
        """
    )
    parser.add_argument("file", help="Path to sliced .3mf project file")
    args = parser.parse_args()
    
    analyzer = ThreeMFAnalyzer(args.file)
    analyzer.parse()
    analyzer.report()


if __name__ == "__main__":
    main()
