#!/usr/bin/env python3
import os
import sys
import re
import json
import argparse
import zipfile
import io
import tempfile
from collections import Counter, defaultdict

class GCodeParser:
    def __init__(self, filepath):
        self.filepath = filepath
        self.filename = os.path.basename(filepath)
        self.layers = defaultdict(list)
        self.header_lines = []
        self.footer_lines = []
        self.total_lines = 0
        
        # Parsed structured data
        self.layer_z_heights = {} # layer_idx -> actual print Z height
        self.layer_toolchanges = defaultdict(list) # layer_idx -> list of T commands
        self.toolchange_blocks = [] # list of dicts with tc metadata and lines
        self.commands_counter = Counter()
        
        # Extrusion and feature stats
        self.extrusion_by_tool = Counter() # tool_idx -> float E
        self.extrusion_by_feature = defaultdict(lambda: Counter()) # feature -> tool -> float E
        self.feedrates_by_feature = defaultdict(list) # feature -> list of feedrates
        self.wipe_tower_coords = [] # list of (X, Y) from M620.14
        self.total_extrusion = 0.0
        
        # Extrusion multipliers / parameters from footer configuration
        self.config_params = {}
        
        # 3MF project metadata configurations
        self.project_settings = {}
        self.slice_info = {}
        self.model_settings = {}

    def _parse_key_value_string(self, content):
        params = {}
        # Parse XML metadata key/value
        for match in re.finditer(r'key="([^"]+)"\s+value="([^"]*)"', content):
            params[match.group(1)] = match.group(2)
        # Parse text lines key=value or key:value
        for line in content.splitlines():
            line = line.strip()
            if not line or line.startswith('<') or line.startswith(';'):
                continue
            if ':' in line:
                k, v = line.split(':', 1)
                k = k.strip().strip('"')
                v = v.strip().strip('"').rstrip(',').strip('"')
                if k not in params:
                    params[k] = v
            elif '=' in line:
                k, v = line.split('=', 1)
                k = k.strip().strip('"')
                v = v.strip().strip('"').rstrip(',').strip('"')
                if k not in params:
                    params[k] = v
        return params

    def parse(self):
        import zipfile
        import io
        import tempfile
        
        # Check if the file is a zip/3mf archive
        if zipfile.is_zipfile(self.filepath):
            with zipfile.ZipFile(self.filepath, 'r') as z:
                # 1. Parse Metadata files if present
                for name in z.namelist():
                    if name.endswith("project_settings.config"):
                        try:
                            content = z.read(name).decode('utf-8', errors='ignore')
                            self.project_settings = json.loads(content)
                        except Exception as e:
                            print(f"Warning: Failed to parse project_settings.config: {e}", file=sys.stderr)
                    elif name.endswith("slice_info.config"):
                        try:
                            content = z.read(name).decode('utf-8', errors='ignore')
                            self.slice_info = self._parse_key_value_string(content)
                        except Exception as e:
                            print(f"Warning: Failed to parse slice_info.config: {e}", file=sys.stderr)
                    elif name.endswith("model_settings.config"):
                        try:
                            content = z.read(name).decode('utf-8', errors='ignore')
                            if content.strip().startswith('{'):
                                self.model_settings = json.loads(content)
                            else:
                                self.model_settings = self._parse_key_value_string(content)
                        except Exception as e:
                            print(f"Warning: Failed to parse model_settings.config: {e}", file=sys.stderr)

                # 2. Parse G-code file inside ZIP
                gcode_files = [n for n in z.namelist() if n.endswith(".gcode")]
                if gcode_files:
                    with z.open(gcode_files[0], 'r') as f_zip:
                        f = io.TextIOWrapper(f_zip, encoding='utf-8', errors='ignore')
                        self._parse_stream(f)
                else:
                    # Fallback recursive search for recently modified temp gcode files in system
                    search_dirs = ["/var/folders", tempfile.gettempdir()]
                    model_dir = "orcaslicer_model" if "orca" in self.filename.lower() else "bamboo_model"
                    
                    candidates = []
                    for s_dir in search_dirs:
                        if os.path.exists(s_dir):
                            for root, dirs, files in os.walk(s_dir):
                                if model_dir in root:
                                    for file in files:
                                        if file.endswith(".gcode"):
                                            candidates.append(os.path.join(root, file))
                    
                    if candidates:
                        # Sort by modification time, newest first
                        candidates.sort(key=os.path.getmtime, reverse=True)
                        newest_temp = candidates[0]
                        print(f"Warning: No G-code found in '{self.filename}'. Falling back to active temp sliced file: {newest_temp}", file=sys.stderr)
                        with open(newest_temp, 'r', encoding='utf-8', errors='ignore') as f:
                            self._parse_stream(f)
                    else:
                        print(f"Warning: No G-code file found inside ZIP archive '{self.filename}' and no temp sliced file found in system. Slicing stats will be empty.", file=sys.stderr)
        else:
            with open(self.filepath, 'r', encoding='utf-8', errors='ignore') as f:
                self._parse_stream(f)
                
        # Merge project_settings into config_params for consistency in correctness rules
        for k, v in self.project_settings.items():
            if isinstance(v, list):
                if k == "filament_type":
                    self.config_params[k] = ";".join(str(x) for x in v)
                else:
                    self.config_params[k] = ",".join(str(x) for x in v)
            else:
                self.config_params[k] = str(v)

    def _parse_stream(self, f):
        current_layer = 0
        in_toolchange = False
        current_tc_block = []
        current_tool = -1
        current_z = 0.0
        current_feature = "Unknown"
        
        # Compile regexes for speed
        z_re = re.compile(r'\b[Gg][01]\b.*?[Zz]([-+]?\d*\.\d+|\d+)')
        e_re = re.compile(r'E(-?\d*\.\d+|-?\d+)')
        f_re = re.compile(r'F(\d+)')
        t_re = re.compile(r'^T(\d+)(?:\s|$|H)')
        
        for line_idx, line in enumerate(f, 1):
            self.total_lines += 1
            line_str = line.strip()
            
            # Parse configuration comments anywhere: "; key = value"
            if line_str.startswith(";"):
                self.footer_lines.append(line_str) # keep track of all config comments
                config_match = re.match(r'^;\s*([a-zA-Z0-9_]+)\s*=\s*(.*)', line_str)
                if config_match:
                    key, val = config_match.groups()
                    self.config_params[key.strip()] = val.strip()

            if line_idx < 1000 and not current_layer and not line_str.startswith("; CHANGE_LAYER"):
                self.header_lines.append(line_str)

            # Command counter (ignoring comments and empty lines)
            if line_str and not line_str.startswith(";"):
                parts = line_str.split()
                if parts:
                    self.commands_counter[parts[0]] += 1

            # Track Z height changes
            z_match = z_re.search(line_str)
            if z_match:
                current_z = float(z_match.group(1))

            # Track feature types
            if "; FEATURE:" in line_str:
                current_feature = line_str.split(":", 1)[1].strip()
            
            # Track Layer Changes
            if "; CHANGE_LAYER" in line_str:
                current_layer += 1
                current_feature = "Unknown"

            if current_layer > 0:
                self.layers[current_layer].append(line_str)
            
            # Track toolchanges (T0-T4)
            t_match = t_re.match(line_str)
            if t_match and not line_str.startswith("T100") and not line_str.startswith("T65"):
                t_val = int(t_match.group(1))
                current_tool = t_val
                if current_layer > 0:
                    self.layer_toolchanges[current_layer].append(t_val)

            # Track extrusion and feature stats
            if current_tool != -1 and (line_str.startswith("G1") or line_str.startswith("G0") or line_str.startswith("G2") or line_str.startswith("G3")):
                e_match = e_re.search(line_str)
                if e_match:
                    e_val = float(e_match.group(1))
                    if e_val > 0:
                        self.total_extrusion += e_val
                        self.extrusion_by_tool[current_tool] += e_val
                        self.extrusion_by_feature[current_feature][current_tool] += e_val
                        
                        # Keep track of actual printing Z heights
                        if current_layer > 0:
                            self.layer_z_heights[current_layer] = current_z
                
                # Track feedrates
                f_match = f_re.search(line_str)
                if f_match:
                    self.feedrates_by_feature[current_feature].append(int(f_match.group(1)))

            # Track Wipe Tower Coordinates (M620.14)
            if "M620.14" in line_str:
                x_match = re.search(r'X([\d.]+)', line_str)
                y_match = re.search(r'Y([\d.]+)', line_str)
                if x_match and y_match:
                    self.wipe_tower_coords.append((float(x_match.group(1)), float(y_match.group(1))))

            # Parse Toolchange blocks
            if ";======== H2C filament_change ========" in line_str:
                if in_toolchange and current_tc_block:
                    self.toolchange_blocks.append({
                        'layer': current_layer,
                        'start_line': line_idx - len(current_tc_block),
                        'lines': current_tc_block
                    })
                current_tc_block = [line_str]
                in_toolchange = True
            elif in_toolchange:
                current_tc_block.append(line_str)
                if "M623" in line_str or "; FEATURE" in line_str or "; object" in line_str or "; CHANGE_LAYER" in line_str:
                    self.toolchange_blocks.append({
                        'layer': current_layer,
                        'start_line': line_idx - len(current_tc_block) + 1,
                        'lines': current_tc_block
                    })
                    in_toolchange = False
                    current_tc_block = []
                    
        if in_toolchange and current_tc_block:
            self.toolchange_blocks.append({
                'layer': current_layer,
                'start_line': self.total_lines - len(current_tc_block) + 1,
                'lines': current_tc_block
            })
            
        self.calculate_flush_stats()

    def calculate_flush_stats(self):
        self.flush_by_tool = Counter()
        current_tool = -1
        for tc in self.toolchange_blocks:
            block = tc['lines']
            target_tool = None
            for line in block:
                t_match = re.match(r'^T(\d+)', line)
                if t_match and not line.startswith("T100") and not line.startswith("T65"):
                    target_tool = int(t_match.group(1))
            
            for line in block:
                if "M620.10" in line:
                    l_match = re.search(r'L([\d.]+)', line)
                    a_match = re.search(r'A([01])', line)
                    if l_match and a_match:
                        l_val = float(l_match.group(1))
                        a_val = int(a_match.group(1))
                        if a_val == 0 and current_tool != -1:
                            self.flush_by_tool[current_tool] += l_val
                        elif a_val == 1 and target_tool is not None:
                            self.flush_by_tool[target_tool] += l_val
            if target_tool is not None:
                current_tool = target_tool

class GCodeAnalyzer:
    def __init__(self, parser):
        self.parser = parser
        self.issues = []
        
    def analyze_correctness(self):
        """Runs checks to verify H2C G-code parameters are valid and safe."""
        # 1. Parse nozzle mappings
        # In H2C config, filament_map maps tool index to hotend index (1-indexed in config, 0-indexed in firmware).
        # We try to extract these mappings from config parameters.
        filament_map_str = self.parser.config_params.get("filament_map", "1,2,2,2,1").replace(";", ",")
        filament_nozzle_map_str = self.parser.config_params.get("filament_nozzle_map", "0,3,2,0,1").replace(";", ",")
        
        try:
            filament_map = [int(x) - 1 for x in filament_map_str.split(",")] # Convert to 0-indexed hotend ID
        except Exception:
            filament_map = [0, 1, 1, 1, 0] # fallback default
            
        try:
            filament_nozzle_map = [int(x) for x in filament_nozzle_map_str.split(",")]
        except Exception:
            filament_nozzle_map = [0, 3, 2, 0, 1] # fallback default

        filament_type_str = self.parser.config_params.get("filament_type", "")
        filament_types = [t.strip().upper() for t in filament_type_str.split(";")] if filament_type_str else []

        current_tool = -1
        
        for tc_idx, tc in enumerate(self.parser.toolchange_blocks, 1):
            block = tc['lines']
            start_line = tc['start_line']
            layer = tc['layer']
            
            # Find target tool in block
            target_tool = None
            m620_11_o1_val = None
            m104s = []
            m620_15_temps = []
            
            for offset, line in enumerate(block):
                line_str = line.strip()
                if line_str.startswith(";"):
                    continue
                
                # Check target tool
                t_match = re.match(r'^T(\d+)', line_str)
                if t_match and not line_str.startswith("T100") and not line_str.startswith("T65"):
                    target_tool = int(t_match.group(1))
                    
                # Check M620.11 retractions
                if "M620.11" in line_str and "O1" in line_str:
                    match = re.search(r'T(\d+)', line_str)
                    if match:
                        m620_11_o1_val = int(match.group(1))
                        
                # Check M104 temperatures
                if "M104" in line_str:
                    s_match = re.search(r'S(\d+)', line_str)
                    t_match = re.search(r'T(\d+)', line_str)
                    s_val = int(s_match.group(1)) if s_match else None
                    t_val = int(t_match.group(1)) if t_match else None
                    m104s.append((offset, t_val, s_val, line_str))
                    
                # Check M620.15 cooling
                if "M620.15" in line_str and "P" in line_str:
                    p_match = re.search(r'P(\d+)', line_str)
                    if p_match:
                        m620_15_temps.append(int(p_match.group(1)))

            # Rule Checks
            
            # Rule 1: Retraction Length Check (PLA should be 18mm, PETG-CF should be 14mm)
            # Extruder 0 on carousel maps to nozzle 1-3. Let's inspect the target tools or current tool.
            if m620_11_o1_val is not None:
                # Retract for PLA is 18, PETG is 14
                # In standard setup: Tool 4 / Tool 0 depending on swap.
                # Let's verify based on the retraction lengths
                if m620_11_o1_val not in [14, 18]:
                    self.issues.append({
                        'layer': layer,
                        'line': start_line,
                        'type': 'Invalid Retraction Length',
                        'desc': f"Toolchange #{tc_idx}: M620.11 O1 specifies T{m620_11_o1_val} retraction, expected 14 or 18."
                    })
            
            # Rule 2: Pre-cooling Hotend Address Check (Must cool parked hotend, NOT active hotend)
            if target_tool is not None and current_tool != -1 and current_tool != target_tool:
                # The hotend index transitioning FROM (parked) is current_tool's hotend
                if current_tool < len(filament_map) and target_tool < len(filament_map):
                    parked_hotend = filament_map[current_tool]
                    active_hotend = filament_map[target_tool]
                    
                    if parked_hotend != active_hotend:
                        # Check M104 commands inside the block that cool
                        for offset, t_val, s_val, raw in m104s:
                            if s_val is not None and s_val < 230: # typically cooling is to lower temps
                                # If it targets active_hotend, that's an issue!
                                if t_val == active_hotend:
                                    self.issues.append({
                                        'layer': layer,
                                        'line': start_line + offset,
                                        'type': 'Wrong Cooling Target',
                                        'desc': f"Toolchange #{tc_idx}: Command cools active hotend T{t_val} (temp {s_val}°C) instead of parked hotend T{parked_hotend}."
                                    })
            
            # Rule 3: Redundant Heating/Cooling Commands
            # (e.g. multiple M104 commands targeting the same hotend in same toolchange)
            hotend_targets = defaultdict(list)
            for offset, t_val, s_val, raw in m104s:
                if t_val is not None:
                    hotend_targets[t_val].append((offset, s_val))
            for hotend, temps in hotend_targets.items():
                if len(temps) > 1:
                    # check if they have different temperatures set close to each other
                    diff_temps = [t[1] for t in temps]
                    if len(set(diff_temps)) > 1:
                        self.issues.append({
                            'layer': layer,
                            'line': start_line + temps[-1][0],
                            'type': 'Redundant M104 Sequence',
                            'desc': f"Toolchange #{tc_idx}: Hotend T{hotend} receives conflicting temperature commands: {diff_temps} in same block."
                        })

            # Rule 4: Clamping limits check (no pre-cooling below 150°C)
            # Check M620.15 and M104 pre-cooling S values
            for offset, t_val, s_val, raw in m104s:
                if "cooling" in raw.lower() and s_val is not None:
                    # If temperature is less than 150°C and it's not the final layer park
                    if s_val < 150 and layer < len(self.parser.layers) - 1:
                        self.issues.append({
                            'layer': layer,
                            'line': start_line + offset,
                            'type': 'Pre-cooling Temperature Floor Violated',
                            'desc': f"Toolchange #{tc_idx}: Pre-cooling temperature S{s_val}°C is below safe floor (150°C)."
                        })
            for temp in m620_15_temps:
                if temp < 150 and layer < len(self.parser.layers) - 1:
                    self.issues.append({
                        'layer': layer,
                        'line': start_line,
                        'type': 'Pre-cooling Temperature Floor Violated',
                        'desc': f"Toolchange #{tc_idx}: M620.15 P{temp} temperature is below safe floor (150°C)."
                    })

            # Rule 5: Extruder Change Retraction distance checking
            k1_found = False
            k1_r_val = None
            for offset, line in enumerate(block):
                if "M620.11" in line and "K1" in line:
                    k1_found = True
                    r_match = re.search(r'R(\d+)', line)
                    if r_match:
                        k1_r_val = int(r_match.group(1))
            if k1_found:
                if k1_r_val is None or k1_r_val == 0:
                    self.issues.append({
                        'layer': layer,
                        'line': start_line,
                        'type': 'CRITICAL: Missing Extruder Change Retraction',
                        'desc': f"Toolchange #{tc_idx}: M620.11 K1 specifies R{k1_r_val if k1_r_val is not None else 0} retraction. Expected R10 or similar to prevent FTS jamming."
                    })

            # Rule 6: Scheduler command balance (M632 / M633)
            scheduler_active = False
            scheduler_line = None
            for offset, line in enumerate(block):
                if "M632" in line and not line.startswith(";"):
                    scheduler_active = True
                    scheduler_line = start_line + offset
                if "M633" in line and not line.startswith(";"):
                    if not scheduler_active:
                        self.issues.append({
                            'layer': layer,
                            'line': start_line + offset,
                            'type': 'Unbalanced M633 Scheduler Sync',
                            'desc': f"Toolchange #{tc_idx}: M633 sync command found without preceding M632 schedule command."
                        })
                    scheduler_active = False
            if scheduler_active:
                self.issues.append({
                    'layer': layer,
                    'line': scheduler_line,
                    'type': 'Unbalanced M632 Scheduler Request',
                    'desc': f"Toolchange #{tc_idx}: M632 schedule command is not followed by M633 sync command before the end of the toolchange block."
                })

            # Rule 7: PETG Pre-Extrusion check (Oozing compensation config)
            if target_tool is not None and target_tool < len(filament_types):
                f_type = filament_types[target_tool]
                if "PETG" in f_type:
                    # check if pre-extrusion length is configured
                    pre_ext_len_str = self.parser.config_params.get("filament_tower_interface_pre_extrusion_length", "")
                    if pre_ext_len_str:
                        pre_ext_len_str = pre_ext_len_str.replace(";", ",")
                        pre_ext_lens = [float(x.strip()) for x in pre_ext_len_str.split(",") if x.strip()]
                    else:
                        pre_ext_lens = []
                    if pre_ext_lens and target_tool < len(pre_ext_lens):
                        p_len = pre_ext_lens[target_tool]
                        if p_len == 0.0:
                            self.issues.append({
                                'layer': layer,
                                'line': start_line,
                                'type': 'Missing PETG Pre-Extrusion Oozing Compensation',
                                'desc': f"Toolchange #{tc_idx}: Slicing for PETG tool T{target_tool} has pre-extrusion length set to 0.0. Expected 2.0mm or similar to compensate for oozing."
                            })

            if target_tool is not None:
                current_tool = target_tool
                
        return self.issues

class GCodeComparator:
    def __init__(self, parser1, parser2):
        self.p1 = parser1
        self.p2 = parser2
        self.mapping = {}

    def detect_tool_mapping(self):
        """Automatically detects tool mapping / swaps between the two slices."""
        # Find which layers have tool changes, and get their sequences.
        # We align by layers and see what tools are mapped.
        # For instance, if Layer 2 in BBL uses [4, 2, 0] and in Orca uses [0, 2, 4],
        # we can build candidates: {4: 0, 2: 2, 0: 4}
        candidates = defaultdict(Counter)
        
        for lyr in sorted(list(set(self.p1.layer_toolchanges.keys()) & set(self.p2.layer_toolchanges.keys()))):
            tc1 = self.p1.layer_toolchanges[lyr]
            tc2 = self.p2.layer_toolchanges[lyr]
            if len(tc1) == len(tc2):
                for t1, t2 in zip(tc1, tc2):
                    candidates[t1][t2] += 1
                    
        # Solve the mapping
        detected_map = {}
        for t1, counts in candidates.items():
            if counts:
                most_common_t2 = counts.most_common(1)[0][0]
                detected_map[t1] = most_common_t2
                
        # Fill missing mappings with identity mapping
        for i in range(5):
            if i not in detected_map:
                detected_map[i] = i
                
        self.mapping = detected_map
        return detected_map

    def compare(self):
        report = []
        stats = {}
        
        # 1. Line count & Global comparison
        stats['file1'] = {
            'name': self.p1.filename,
            'lines': self.p1.total_lines,
            'layers': len(self.p1.layers),
            'toolchanges': len(self.p1.toolchange_blocks),
            'extrusion': self.p1.total_extrusion
        }
        stats['file2'] = {
            'name': self.p2.filename,
            'lines': self.p2.total_lines,
            'layers': len(self.p2.layers),
            'toolchanges': len(self.p2.toolchange_blocks),
            'extrusion': self.p2.total_extrusion
        }
        
        # Detect tool mapping
        self.detect_tool_mapping()
        stats['tool_mapping'] = self.mapping
        
        # 2. Compare command distribution
        all_cmds = sorted(list(set(self.p1.commands_counter.keys()) | set(self.p2.commands_counter.keys())))
        cmd_diffs = {}
        for cmd in all_cmds:
            cnt1 = self.p1.commands_counter[cmd]
            cnt2 = self.p2.commands_counter[cmd]
            cmd_diffs[cmd] = {
                'file1': cnt1,
                'file2': cnt2,
                'diff': cnt1 - cnt2
            }
        stats['command_distribution'] = cmd_diffs
        
        # 3. Layer details comparison
        layer_diffs = []
        max_layer = max(max(self.p1.layers.keys(), default=0), max(self.p2.layers.keys(), default=0))
        for l in range(1, max_layer + 1):
            lyr1 = self.p1.layers.get(l, [])
            lyr2 = self.p2.layers.get(l, [])
            z1 = self.p1.layer_z_heights.get(l, 0.0)
            z2 = self.p2.layer_z_heights.get(l, 0.0)
            
            layer_diffs.append({
                'layer': l,
                'file1_z': z1,
                'file2_z': z2,
                'file1_lines': len(lyr1),
                'file2_lines': len(lyr2),
                'line_diff': len(lyr1) - len(lyr2)
            })
        stats['layers'] = layer_diffs
        
        # 4. Compare feature extrusion profiles
        features1 = set(self.p1.extrusion_by_feature.keys())
        features2 = set(self.p2.extrusion_by_feature.keys())
        all_features = sorted(list(features1 | features2))
        feature_extrusion = {}
        
        for feat in all_features:
            tools_data = {}
            for t in range(5):
                mapped_t = self.mapping.get(t, t)
                e1 = self.p1.extrusion_by_feature[feat][t]
                e2 = self.p2.extrusion_by_feature[feat][mapped_t]
                if e1 > 0 or e2 > 0:
                    tools_data[f"T{t} (F1) / T{mapped_t} (F2)"] = {
                        'file1_e': e1,
                        'file2_e': e2,
                        'diff': e1 - e2
                    }
            if tools_data:
                feature_extrusion[feat] = tools_data
        stats['feature_extrusion'] = feature_extrusion
        
        # Compare flush lengths
        flush_comparison = {}
        for t in range(5):
            mapped_t = self.mapping.get(t, t)
            l1 = self.p1.flush_by_tool[t]
            l2 = self.p2.flush_by_tool[mapped_t]
            if l1 > 0 or l2 > 0:
                flush_comparison[f"T{t} (F1) / T{mapped_t} (F2)"] = {
                    'file1_flush_mm': l1,
                    'file2_flush_mm': l2,
                    'diff': l1 - l2
                }
        stats['flush_comparison'] = flush_comparison
        
        # 5. Block-by-block toolchange diffs
        # Both files should have same toolchanges. If count matches, align them logically.
        tc_mismatches = []
        if len(self.p1.toolchange_blocks) == len(self.p2.toolchange_blocks):
            for idx, (tc1, tc2) in enumerate(zip(self.p1.toolchange_blocks, self.p2.toolchange_blocks), 1):
                block1 = tc1['lines']
                block2 = tc2['lines']
                
                # Check for mismatching commands after strip and comment removal
                clean_b1 = [l.strip() for l in block1 if l.strip() and not l.strip().startswith(";")]
                clean_b2 = [l.strip() for l in block2 if l.strip() and not l.strip().startswith(";")]
                
                # Try to map T command values in clean_b2 to align with clean_b1
                mapped_b2 = []
                for line in clean_b2:
                    mapped_line = line
                    # Replace T0-T4
                    t_match = re.match(r'^T(\d+)', line)
                    if t_match:
                        t_val = int(t_match.group(1))
                        # Reverse map for display alignment
                        reverse_mapping = {v: k for k, v in self.mapping.items()}
                        orig_t = reverse_mapping.get(t_val, t_val)
                        mapped_line = re.sub(r'^T\d+', f"T{orig_t}", line)
                    mapped_b2.append(mapped_line)
                
                # Simple alignment comparison
                mismatches = []
                for offset, (l1, l2) in enumerate(zip(clean_b1, mapped_b2)):
                    if l1 != l2:
                        mismatches.append({
                            'offset': offset,
                            'file1_cmd': l1,
                            'file2_cmd': l2
                        })
                        
                if mismatches or len(clean_b1) != len(clean_b2):
                    tc_mismatches.append({
                        'toolchange_idx': idx,
                    'layer1': tc1['layer'],
                        'layer2': tc2['layer'],
                        'diffs': mismatches,
                        'file1_len': len(clean_b1),
                        'file2_len': len(clean_b2)
                    })
        stats['toolchange_mismatches'] = tc_mismatches
        
        # Compare project settings if present
        stats['project_metadata_diff'] = self.compare_project_settings()
        
        return stats

    def compare_project_settings(self):
        bbl = self.p1.project_settings
        orca = self.p2.project_settings
        
        # Categorized keys definitions
        categories = {
            "PRINT PROCESS & QUALITY (ПРОЦЕСС ПЕЧАТИ И КАЧЕСТВО)": [
                "layer_height", "initial_layer_print_height", "layer_height_max", "layer_height_min",
                "wall_loops", "wall_generator",
                "top_shell_layers", "bottom_shell_layers", "top_shell_thickness", "bottom_shell_thickness",
                "sparse_infill_density", "sparse_infill_pattern", "infill_direction",
                "gap_fill_target", "detect_thin_wall", "detect_overhang_wall",
                "seam_position", "enable_arc_fitting", "brim_type", "brim_width",
                "sparse_infill_line_width", "outer_wall_line_width", "inner_wall_line_width"
            ],
            "SPEEDS & ACCELERATIONS (СКОРОСТИ И УСКОРЕНИЯ)": [
                "initial_layer_speed", "initial_layer_infill_speed", "outer_wall_speed", "inner_wall_speed",
                "sparse_infill_speed", "internal_solid_infill_speed", "top_surface_speed", "gap_infill_speed",
                "travel_speed", "bridge_speed",
                "default_acceleration", "initial_layer_acceleration", "outer_wall_acceleration", "inner_wall_acceleration", "travel_acceleration"
            ],
            "TEMPERATURE & COOLING (ТЕМПЕРАТУРЫ И ОХЛАЖДЕНИЕ)": [
                "nozzle_temperature", "nozzle_temperature_initial_layer", "nozzle_temperature_range_high", "nozzle_temperature_range_low",
                "bed_temperature", "bed_temperature_initial_layer", "cool_plate_temp", "eng_plate_temp",
                "fan_min_speed", "fan_max_speed", "auxiliary_fan", "chamber_temperature"
            ],
            "FILAMENT & EXTRUSION (ПЛАСТИК И ЭКСТРУЗИЯ)": [
                "filament_flow_ratio", "filament_density", "filament_diameter",
                "filament_max_volumetric_speed", "filament_flush_temp", "filament_pre_cooling_temperature_nc",
                "pressure_advance", "enable_pressure_advance"
            ],
            "PRINTER & RETRACTION (ПРИНТЕР И РЕТРАКЦИЯ)": [
                "retraction_length", "retraction_speed", "z_hop_height", "z_hop_types"
            ]
        }
        
        all_categorized_keys = []
        for keys in categories.values():
            all_categorized_keys.extend(keys)
            
        # We also want to find any other keys that exist in both and differ
        common_keys = set(bbl.keys()) & set(orca.keys())
        other_differing_keys = []
        for k in sorted(list(common_keys)):
            if k not in all_categorized_keys:
                if bbl[k] != orca[k]:
                    other_differing_keys.append(k)
                    
        if other_differing_keys:
            categories["OTHER COMMON SETTINGS (ДРУГИЕ ОБЩИЕ ПАРАМЕТРЫ)"] = other_differing_keys
            
        metadata_report = []
        
        # Check if we have project settings at all
        if not bbl and not orca:
            return "  No 3MF project settings found in either file.\n"
            
        for cat_name, keys in categories.items():
            cat_diffs = []
            for k in keys:
                v1 = bbl.get(k)
                v2 = orca.get(k)
                if v1 is not None or v2 is not None:
                    if v1 != v2:
                        cat_diffs.append((k, v1, v2))
            if cat_diffs:
                metadata_report.append(f"\n  === {cat_name} ===")
                metadata_report.append(f"    {'Parameter':35s} | {'File 1 (BBL)':25s} | {'File 2 (Orca)':25s}")
                metadata_report.append(f"    {'-'*35} + {'-'*25} + {'-'*25}")
                for k, v1, v2 in cat_diffs:
                    s1 = str(v1) if v1 is not None else "<missing>"
                    s2 = str(v2) if v2 is not None else "<missing>"
                    # Truncate strings if too long (e.g. gcode)
                    if len(s1) > 22: s1 = s1[:19] + "..."
                    if len(s2) > 22: s2 = s2[:19] + "..."
                    metadata_report.append(f"    {k:35s} | {s1:25s} | {s2:25s}")
                    
        # Check start/end/change gcode differences specifically (often long, so print them nicely or note diff)
        gcode_keys = ["change_filament_gcode", "machine_start_gcode", "machine_end_gcode"]
        gcode_diffs = []
        for k in gcode_keys:
            v1 = bbl.get(k)
            v2 = orca.get(k)
            if v1 != v2:
                gcode_diffs.append(k)
                
        if gcode_diffs:
            metadata_report.append(f"\n  === G-CODE SCRIPTS DIFFERENCES (РАЗЛИЧИЯ В G-CODE СКРИПТАХ) ===")
            for k in gcode_diffs:
                metadata_report.append(f"    * {k} differs between BBL and Orca.")
                
        # Also print slice_info.config differences
        bbl_si = self.p1.slice_info
        orca_si = self.p2.slice_info
        si_keys = sorted(list(set(bbl_si.keys()) | set(orca_si.keys())))
        si_diffs = []
        for k in si_keys:
            v1 = bbl_si.get(k)
            v2 = orca_si.get(k)
            if v1 != v2:
                si_diffs.append((k, v1, v2))
                
        if si_diffs:
            metadata_report.append(f"\n  === SLICE INFO METADATA (ИНФОРМАЦИЯ О НАРЕЗКЕ) ===")
            metadata_report.append(f"    {'Metadata Key':35s} | {'File 1 (BBL)':25s} | {'File 2 (Orca)':25s}")
            metadata_report.append(f"    {'-'*35} + {'-'*25} + {'-'*25}")
            for k, v1, v2 in si_diffs:
                s1 = str(v1) if v1 is not None else "<missing>"
                s2 = str(v2) if v2 is not None else "<missing>"
                metadata_report.append(f"    {k:35s} | {s1:25s} | {s2:25s}")
                
        return "\n".join(metadata_report)

def main():
    parser = argparse.ArgumentParser(description="H2C G-code Correctness and Match Analyzer")
    parser.add_argument("-f1", "--file1", required=True, help="Path to first G-code file (BBL or OrcaSlicer)")
    parser.add_argument("-f2", "--file2", help="Path to second G-code file for comparison")
    parser.add_argument("-o", "--output", help="Output reports prefix path (defaults to desktop/cwd when --to-file is set)")
    parser.add_argument("--to-file", action="store_true", help="Save reports to files instead of printing to console")
    
    args = parser.parse_args()
    
    # 1. Parse File 1
    if not os.path.exists(args.file1):
        print(f"Error: File '{args.file1}' not found.", file=sys.stderr)
        sys.exit(1)
        
    print(f"Parsing '{args.file1}'...", file=sys.stderr)
    p1 = GCodeParser(args.file1)
    p1.parse()
    
    # 2. Analyze File 1 Correctness
    analyzer = GCodeAnalyzer(p1)
    issues = analyzer.analyze_correctness()
    
    # Define report path
    desktop = os.path.expanduser("~/Desktop")
    out_dir = desktop if os.path.exists(desktop) else "."
    
    if args.output:
        report_base = args.output
    else:
        report_base = os.path.join(out_dir, f"{os.path.splitext(p1.filename)[0]}_analysis")

    # If single file mode
    if not args.file2:
        # Write JSON output for agent analysis
        result_json = {
            'file': p1.filepath,
            'summary': {
                'total_lines': p1.total_lines,
                'layers_count': len(p1.layers),
                'toolchanges_count': len(p1.toolchange_blocks),
                'total_extrusion_mm': p1.total_extrusion,
            },
            'command_counts': dict(p1.commands_counter),
            'extrusion_by_tool': dict(p1.extrusion_by_tool),
            'flush_by_tool': dict(p1.flush_by_tool),
            'issues': issues
        }
        
        # Build text report string
        report_text_lines = []
        report_text_lines.append("========================================================================\n")
        report_text_lines.append(f"  H2C G-CODE CORRECTNESS ANALYSIS REPORT: {p1.filename}\n")
        report_text_lines.append("========================================================================\n\n")
        
        report_text_lines.append("--- 1. OVERVIEW SUMMARY ---\n")
        report_text_lines.append(f"  File Path:            {p1.filepath}\n")
        report_text_lines.append(f"  Total Lines:          {p1.total_lines}\n")
        report_text_lines.append(f"  Total Sliced Layers:  {len(p1.layers)}\n")
        report_text_lines.append(f"  Total Toolchanges:    {len(p1.toolchange_blocks)}\n")
        report_text_lines.append(f"  Total Extrusion:      {p1.total_extrusion:.2f} mm\n\n")
        
        report_text_lines.append("--- 2. EXTRUSION & FLUSHING BY TOOL ---\n")
        for t, val in sorted(p1.extrusion_by_tool.items()):
            flush_val = p1.flush_by_tool.get(t, 0.0)
            report_text_lines.append(f"  Tool T{t}:             Extrusion: {val:.2f} mm | Flushing: {flush_val:.2f} mm\n")
        report_text_lines.append("\n")
        
        report_text_lines.append("--- 3. EXTRUSION BY FEATURE TYPE ---\n")
        for feat, tools in sorted(p1.extrusion_by_feature.items()):
            report_text_lines.append(f"  Feature '{feat}':\n")
            for t, val in sorted(tools.items()):
                report_text_lines.append(f"    Tool T{t}:           {val:.2f} mm\n")
        report_text_lines.append("\n")
        
        report_text_lines.append("--- 4. CORRECTNESS SANITY CHECK ISSUES ---\n")
        if not issues:
            report_text_lines.append("  [SUCCESS] No correctness or safety issues identified in this file.\n")
        else:
            report_text_lines.append(f"  [WARNING] Identified {len(issues)} potential issues:\n\n")
            for idx, issue in enumerate(issues, 1):
                report_text_lines.append(f"  Issue {idx} [Layer {issue['layer']}, Line {issue['line']}]:\n")
                report_text_lines.append(f"    Type: {issue['type']}\n")
                report_text_lines.append(f"    Desc: {issue['desc']}\n\n")
                
        report_text = "".join(report_text_lines)
        
        if args.to_file:
            report_txt_path = f"{report_base}_report.txt"
            report_json_path = f"{report_base}_report.json"
            with open(report_json_path, 'w', encoding='utf-8') as f:
                json.dump(result_json, f, indent=2)
            with open(report_txt_path, 'w', encoding='utf-8') as out:
                out.write(report_text)
            print(f"Correctness analysis completed.", file=sys.stderr)
            print(f"Human-readable report saved to: {report_txt_path}", file=sys.stderr)
            print(f"Machine-readable JSON saved to: {report_json_path}", file=sys.stderr)
        else:
            sys.stdout.write(report_text)
        
    else:
        # Comparison Mode
        if not os.path.exists(args.file2):
            print(f"Error: Comparison file '{args.file2}' not found.", file=sys.stderr)
            sys.exit(1)
            
        print(f"Parsing comparison file '{args.file2}'...", file=sys.stderr)
        p2 = GCodeParser(args.file2)
        p2.parse()
        
        print("Comparing files...", file=sys.stderr)
        comparator = GCodeComparator(p1, p2)
        comp_stats = comparator.compare()
        
        # Run correctness analysis on both files for a unified single-pass report
        analyzer1 = GCodeAnalyzer(p1)
        issues1 = analyzer1.analyze_correctness()
        analyzer2 = GCodeAnalyzer(p2)
        issues2 = analyzer2.analyze_correctness()
        
        comp_stats['file1_issues'] = issues1
        comp_stats['file2_issues'] = issues2
        
        # Build comparison text report string
        report_text_lines = []
        report_text_lines.append("========================================================================\n")
        report_text_lines.append("  H2C G-CODE COMPARATIVE ANALYSIS REPORT\n")
        report_text_lines.append(f"  File 1 (BBL):  {p1.filename}\n")
        report_text_lines.append(f"  File 2 (Orca): {p2.filename}\n")
        report_text_lines.append("========================================================================\n\n")
        
        report_text_lines.append("--- 1. METRICS COMPARISON ---\n")
        report_text_lines.append(f"  {'Metric':<25} | {'File 1 (BBL)':<20} | {'File 2 (Orca)':<20} | {'Diff':<15}\n")
        report_text_lines.append("-" * 88 + "\n")
        report_text_lines.append(f"  {'Total Lines':<25} | {p1.total_lines:<20} | {p2.total_lines:<20} | {p1.total_lines - p2.total_lines:<15}\n")
        report_text_lines.append(f"  {'Total Layers':<25} | {len(p1.layers):<20} | {len(p2.layers):<20} | {len(p1.layers) - len(p2.layers):<15}\n")
        report_text_lines.append(f"  {'Total Toolchanges':<25} | {len(p1.toolchange_blocks):<20} | {len(p2.toolchange_blocks):<20} | {len(p1.toolchange_blocks) - len(p2.toolchange_blocks):<15}\n")
        report_text_lines.append(f"  {'Total Extrusion (mm)':<25} | {p1.total_extrusion:<20.2f} | {p2.total_extrusion:<20.2f} | {p1.total_extrusion - p2.total_extrusion:<15.2f}\n\n")
        
        report_text_lines.append("--- 2. DETECTED LOGICAL TOOL MAPPING ---\n")
        report_text_lines.append("  This mapping translates File 1 (BBL) tool indices to File 2 (Orca) tool indices:\n")
        for t1, t2 in sorted(comp_stats['tool_mapping'].items()):
            report_text_lines.append(f"    File 1 Tool T{t1} maps to File 2 Tool T{t2}\n")
        report_text_lines.append("\n")
        
        report_text_lines.append("--- 3. COMMAND DISTRIBUTION ---\n")
        report_text_lines.append(f"  {'Command':<12} | {'File 1':<12} | {'File 2':<12} | {'Diff (F1 - F2)':<15}\n")
        report_text_lines.append("-" * 60 + "\n")
        for cmd, cdata in sorted(comp_stats['command_distribution'].items()):
            if abs(cdata['diff']) > 5 or cdata['file1'] > 20 or cdata['file2'] > 20:
                report_text_lines.append(f"  {cmd:<12} | {cdata['file1']:<12} | {cdata['file2']:<12} | {cdata['diff']:<15}\n")
        report_text_lines.append("\n")
        
        report_text_lines.append("--- 4. LAYER-BY-LAYER Z HEIGHTS & LINE COUNTS (First 20 Layers) ---\n")
        report_text_lines.append(f"  {'Layer':<6} | {'F1 Z':<8} | {'F2 Z':<8} | {'F1 Lines':<9} | {'F2 Lines':<9} | {'Line Diff':<10}\n")
        report_text_lines.append("-" * 60 + "\n")
        for lyr_data in comp_stats['layers'][:20]:
            report_text_lines.append(f"  {lyr_data['layer']:<6} | {lyr_data['file1_z']:<8.2f} | {lyr_data['file2_z']:<8.2f} | {lyr_data['file1_lines']:<9} | {lyr_data['file2_lines']:<9} | {lyr_data['line_diff']:<10}\n")
        if len(comp_stats['layers']) > 20:
            report_text_lines.append(f"  ... and {len(comp_stats['layers']) - 20} more layers.\n")
        report_text_lines.append("\n")
 
        report_text_lines.append("--- 5. FEATURE EXTRUSION PROFILES (mm) ---\n")
        report_text_lines.append(f"  {'Feature':<25} | {'Logical Tool Combination':<25} | {'File 1 E':<10} | {'File 2 E':<10} | {'Diff':<10}\n")
        report_text_lines.append("-" * 88 + "\n")
        for feat, t_data in sorted(comp_stats['feature_extrusion'].items()):
            for comb, edata in sorted(t_data.items()):
                report_text_lines.append(f"  {feat:<25} | {comb:<25} | {edata['file1_e']:<10.2f} | {edata['file2_e']:<10.2f} | {edata['diff']:<10.2f}\n")
        report_text_lines.append("\n")
        
        report_text_lines.append("--- 6. FLUSHING BY TOOL COMPARISON (mm) ---\n")
        report_text_lines.append(f"  {'Logical Tool Combination':<25} | {'File 1 Flush':<15} | {'File 2 Flush':<15} | {'Diff':<10}\n")
        report_text_lines.append("-" * 72 + "\n")
        for comb, fdata in sorted(comp_stats['flush_comparison'].items()):
            report_text_lines.append(f"  {comb:<25} | {fdata['file1_flush_mm']:<15.2f} | {fdata['file2_flush_mm']:<15.2f} | {fdata['diff']:<10.2f}\n")
        report_text_lines.append("\n")
        
        report_text_lines.append("--- 7. LOGICALLY ALIGNED TOOLCHANGE MISMATCHES ---\n")
        mismatches = comp_stats['toolchange_mismatches']
        if not mismatches:
            report_text_lines.append("  [SUCCESS] All aligned toolchange commands match perfectly!\n")
        else:
            report_text_lines.append(f"  [WARNING] Found command mismatches in {len(mismatches)} toolchange blocks:\n\n")
            for item in mismatches:
                report_text_lines.append(f"  Toolchange #{item['toolchange_idx']} (F1 Layer {item['layer1']} | F2 Layer {item['layer2']}):\n")
                report_text_lines.append(f"    Line count: F1={item['file1_len']} commands, F2={item['file2_len']} commands\n")
                if item['diffs']:
                    report_text_lines.append("    Differing commands (aligned by target tool index):\n")
                    for d in item['diffs'][:10]:
                        report_text_lines.append(f"      Offset {d['offset']}:\n")
                        report_text_lines.append(f"        F1 (BBL) : {d['file1_cmd']}\n")
                        report_text_lines.append(f"        F2 (Orca): {d['file2_cmd']}\n")
                    if len(item['diffs']) > 10:
                        report_text_lines.append(f"      ... and {len(item['diffs']) - 10} more command mismatches in this block.\n")
                report_text_lines.append("\n")

        report_text_lines.append("--- 8. PROJECT METADATA COMPARISON (СРАВНЕНИЕ МЕТАДАННЫХ ПРОЕКТОВ) ---\n")
        report_text_lines.append(comp_stats.get('project_metadata_diff', '  No project metadata differences found.\n'))
        report_text_lines.append("\n\n")

        report_text_lines.append("--- 9. FILE 1 (BBL) CORRECTNESS SANITY CHECK ISSUES ---\n")
        if not issues1:
            report_text_lines.append("  [SUCCESS] No correctness or safety issues identified in File 1 (BBL).\n")
        else:
            report_text_lines.append(f"  [WARNING] Identified {len(issues1)} potential issues in File 1 (BBL):\n\n")
            for idx, issue in enumerate(issues1, 1):
                report_text_lines.append(f"  Issue {idx} [Layer {issue['layer']}, Line {issue['line']}]:\n")
                report_text_lines.append(f"    Type: {issue['type']}\n")
                report_text_lines.append(f"    Desc: {issue['desc']}\n\n")
        report_text_lines.append("\n")

        report_text_lines.append("--- 10. FILE 2 (Orca) CORRECTNESS SANITY CHECK ISSUES ---\n")
        if not issues2:
            report_text_lines.append("  [SUCCESS] No correctness or safety issues identified in File 2 (Orca).\n")
        else:
            report_text_lines.append(f"  [WARNING] Identified {len(issues2)} potential issues in File 2 (Orca):\n\n")
            for idx, issue in enumerate(issues2, 1):
                report_text_lines.append(f"  Issue {idx} [Layer {issue['layer']}, Line {issue['line']}]:\n")
                report_text_lines.append(f"    Type: {issue['type']}\n")
                report_text_lines.append(f"    Desc: {issue['desc']}\n\n")
                
        report_text = "".join(report_text_lines)
        
        if args.to_file:
            report_txt_path = f"{report_base}_comparison.txt"
            report_json_path = f"{report_base}_comparison.json"
            with open(report_json_path, 'w', encoding='utf-8') as f:
                json.dump(comp_stats, f, indent=2)
            with open(report_txt_path, 'w', encoding='utf-8') as out:
                out.write(report_text)
            print(f"Comparison completed.", file=sys.stderr)
            print(f"Human-readable comparison report saved to: {report_txt_path}", file=sys.stderr)
            print(f"Machine-readable JSON comparison saved to: {report_json_path}", file=sys.stderr)
        else:
            sys.stdout.write(report_text)

if __name__ == '__main__':
    main()
