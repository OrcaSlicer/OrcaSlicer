"""
Slicing G-code Comparison Tool: OrcaSlicer vs BambuStudio (BBL)
====================================================================

The script performs a deep comparison of two `.3mf` slicing project files 
(the first one from OrcaSlicer, the second from Bambu Studio) for debugging the Vortek H2C nozzle changer.

Default behavior (file paths):
------------------------------------
If only filenames are passed (e.g. `54orca.3mf` and `bbl.3mf`), the script searches by priority:
1. Looks for the file on the user's Desktop (~/Desktop/<filename>).
2. Looks for the file relative to the current working directory.
3. Supports absolute paths.

What the script analyzes:
----------------------
1. Summary metadata (slice_info.config):
   - Print time, part weight, first layer time, slicer versions, printer model ID.
   - filament_maps parameters, dynamic mapping, and filament switcher settings.
   - Summary nozzle settings: extruder_nozzle_stats, filament_nozzle_map, filament_volume_map.
2. Preheat and Standby Cooldown Analysis:
   - Matches M104/M109 heating commands of the target nozzle before toolchange (T).
   - Calculates lead lines (how many G-code lines before physical T the heating started).
   - Extracts standby cooldown temperatures of the inactive nozzle.
3. Vortek Nozzles comparison:
   - Analyzes assigned nozzle IDs, extruder IDs, and diameters.
4. Differences in slicing settings (project_settings.config):
   - Generates a comparison table of configuration discrepancies between slicers.
5. Nozzle changes, prime tower, and retracts:
   - Total number of toolchanges and sequence.
   - Prime Tower G-code volume (lines and blocks).
   - Retract parameters during nozzle switch (M620.11 combinations of E, R, F).
6. Change filament G-code blocks (change_filament_gcode):
   - Runs a unified diff for each toolchange G-code block.
7. Timeline of G-code control commands:
   - Builds a differential track of toolchanges, nozzle switches, and heating events.
8. Critical discrepancies analysis:
   - Signals invalid nozzle indices (out of H2C carousel limits).
   - Detects nozzle collisions (duplicate slots causing AMS flush instead of physical swap).
   - Pinpoints the Standard#7 inventory bug, weight discrepancies due to flushes, etc.

Results are saved to:
/Users/denn/Develop/3dprint/dehancer lab/H2C_v2/mp_reports/compare_report_<timestamp>.md

AGENTS NOTE:
When a slicing report is requested, do not print raw setting diffs or full timelines in chat.
Provide only the final analytical report of KEY and CRITICAL discrepancies (mapping errors, preheat differences, Vortek anomalies).
"""

import os
import sys
import zipfile
import json
import xml.etree.ElementTree as ET
import difflib
from datetime import datetime

DESKTOP_DIR = os.path.expanduser("~/Desktop")
REPORTS_DIR = "/Users/denn/Develop/3dprint/dehancer lab/H2C_v2/mp_reports"

def escape_markdown_table(val):
    if val is None:
        return "None"
    # Replace pipe with unicode vertical bar to avoid breaking markdown table columns
    return str(val).replace("|", " ⎮ ")

def find_file(filename):
    if os.path.isabs(filename):
        return filename
    desktop_path = os.path.join(DESKTOP_DIR, filename)
    if os.path.exists(desktop_path):
        return desktop_path
    if os.path.exists(filename):
        return os.path.abspath(filename)
    return None

def parse_metadata(zip_file):
    metadata = {
        "version_info": {},
        "plate_meta": {},
        "filaments": [],
        "nozzles": []
    }
    try:
        xml_data = zip_file.read("Metadata/slice_info.config")
        root = ET.fromstring(xml_data)
        
        header = root.find("header")
        if header is not None:
            for item in header.findall("header_item"):
                metadata["version_info"][item.attrib.get("key")] = item.attrib.get("value")
                
        plate = root.find("plate")
        if plate is not None:
            for item in plate.findall("metadata"):
                metadata["plate_meta"][item.attrib.get("key")] = item.attrib.get("value")
                
            for fil in plate.findall("filament"):
                metadata["filaments"].append({
                    "id": fil.attrib.get("id"),
                    "type": fil.attrib.get("type"),
                    "color": fil.attrib.get("color"),
                    "used_g": fil.attrib.get("used_g"),
                    "used_m": fil.attrib.get("used_m"),
                    "nozzle_diameter": fil.attrib.get("nozzle_diameter"),
                    "volume_type": fil.attrib.get("volume_type")
                })
                
            for noz in plate.findall("nozzle"):
                metadata["nozzles"].append({
                    "id": noz.attrib.get("id"),
                    "extruder_id": noz.attrib.get("extruder_id"),
                    "nozzle_diameter": noz.attrib.get("nozzle_diameter")
                })
    except KeyError:
        pass
    except Exception as e:
        print(f"Error parsing metadata: {e}")
    return metadata

def parse_project_settings(zip_file):
    try:
        content = zip_file.read("Metadata/project_settings.config").decode('utf-8')
        return json.loads(content)
    except KeyError:
        return {}
    except Exception as e:
        print(f"Error parsing project_settings: {e}")
        return {}

def parse_critical_gcode(zip_file, filament_maps_str=None):
    critical_events = []
    total_lines = 0
    file_size = 0
    
    # Try to parse filament_maps from Metadata/slice_info.config if not provided
    if not filament_maps_str:
        try:
            xml_data = zip_file.read("Metadata/slice_info.config")
            import xml.etree.ElementTree as ET
            root = ET.fromstring(xml_data)
            plate = root.find("plate")
            if plate is not None:
                for meta in plate.findall("metadata"):
                    if meta.attrib.get("key") == "filament_maps":
                        filament_maps_str = meta.attrib.get("value", "").strip()
                        break
        except Exception:
            pass
            
    if not filament_maps_str:
        filament_maps_str = "1 1 1 1 1 2"
        
    extruder_map = {}
    for i, v in enumerate(filament_maps_str.split()):
        try:
            extruder_map[i] = int(v)
        except ValueError:
            pass
            
    # For H2C, Extruder 1 (Left) maps to Heater 1, Extruder 2 (Right) maps to Heater 0
    heater_to_ext = {1: 1, 2: 0}
    nozzle_map = {}
    for fid, ext_id in extruder_map.items():
        nozzle_map[fid] = heater_to_ext.get(ext_id, 1)

    toolchange_count = 0
    toolchange_sequence = []
    prime_tower_blocks = 0
    prime_tower_lines = 0
    in_prime_tower = False
    
    m620_11_retracts = []
    temp_events = []
    
    active_extruder = 0
    next_filament = 0
    temp_T0 = 0
    temp_T1 = 0
    temp_track = []
    
    toolchange_blocks = []
    current_tc_block = []
    in_tc_block = False
    in_m620_block = False
    printing_started = False
    m73_points = []
    try:
        gcode_names = [name for name in zip_file.namelist() if name.endswith('.gcode')]
        if not gcode_names:
            raise KeyError("No .gcode file found in 3mf")
        gcode_name = gcode_names[0]
        info = zip_file.getinfo(gcode_name)
        file_size = info.file_size
        
        with zip_file.open(gcode_name) as f:
            for line_bytes in f:
                total_lines += 1
                line = line_bytes.decode("utf-8", errors="ignore").strip()
                
                if "; CHANGE_LAYER" in line:
                    printing_started = True
                
                # Special check for H2C virtual temperature commands (e.g. ;VM104, ;VM109)
                is_virtual_temp = False
                if line.startswith(";VM104") or line.startswith(";VM109"):
                    is_virtual_temp = True
                    cmd_part = line[1:].split(";")[0].strip()
                else:
                    cmd_part = line.split(";")[0].strip()
                
                if ";======== H2C filament_change ========" in line:
                    in_tc_block = True
                    current_tc_block = [f"Line {total_lines}: {line}"]
                elif in_tc_block:
                    current_tc_block.append(f"Line {total_lines}: {line}")
                    if "M1002 gcode_claim_action : 0" in cmd_part:
                        in_tc_block = False
                        toolchange_blocks.append("\n".join(current_tc_block))
                
                if not cmd_part:
                    continue
                    
                words = cmd_part.split()
                first_word = words[0]
                
                if first_word == "M628" and "S1" in cmd_part:
                    in_prime_tower = True
                    prime_tower_blocks += 1
                elif first_word == "M628" and "S0" in cmd_part:
                    in_prime_tower = False
                
                if in_prime_tower:
                    prime_tower_lines += 1
                
                if first_word == "M73" and not first_word.startswith("M73.2"):
                    parts = cmd_part.split()
                    r_val = next((p for p in parts if p.startswith("R")), None)
                    p_val = next((p for p in parts if p.startswith("P")), None)
                    if r_val and p_val:
                        try:
                            r_min = int(r_val[1:])
                            m73_points.append((total_lines, r_min))
                        except ValueError:
                            pass
                
                is_critical = False
                
                if first_word.startswith("T") and first_word[1:].isdigit():
                    is_critical = True
                    toolchange_count += 1
                    toolchange_sequence.append(first_word)
                    t_val = int(first_word[1:])
                    if t_val < 60000:
                        active_extruder = t_val
                        next_filament = t_val
                    temp_track.append((total_lines, active_extruder, temp_T0, temp_T1, f"T{t_val} (Active Filament: T{t_val})", in_m620_block, printing_started))
                
                elif any(first_word.startswith(prefix) for prefix in ["M620", "M621", "M622", "M623", "M628", "M629", "M1002", "G29"]):
                    is_critical = True
                    if first_word == "M620":
                        in_m620_block = True
                        parts = cmd_part.split()
                        s_val = next((p for p in parts if p.startswith("S")), None)
                        if s_val and s_val.endswith("A") and len(s_val) > 2:
                            try:
                                next_filament = int(s_val[1:-1])
                            except ValueError:
                                pass
                    elif first_word == "M621":
                        in_m620_block = False
                    elif first_word == "M620.11":
                        parts = cmd_part.split()
                        e_val = next((p for p in parts if p.startswith("E")), "E?")
                        r_val = next((p for p in parts if p.startswith("R")), "R?")
                        f_val = next((p for p in parts if p.startswith("F")), "F?")
                        m620_11_retracts.append(f"{e_val} {r_val} {f_val}")
                    elif first_word == "M620.15":
                        parts = cmd_part.split()
                        p_val = next((p for p in parts if p.startswith("P")), None)
                        c_val = next((p for p in parts if p.startswith("C")), None)
                        desc_parts = []
                        # M620.15 is a firmware command that sets target temp of the INCOMING nozzle
                        # during the switch to prevent oozing / prepare for print.
                        target_heater = 1 if next_filament == 1 else 0
                        if p_val:
                            desc_parts.append(f"Pre-cool P{p_val[1:]}°C")
                            temp_events.append((total_lines, f"M620.15 Pre-cool P{p_val[1:]}°C"))
                            try:
                                val = int(p_val[1:])
                                if target_heater == 0:
                                    temp_T0 = val
                                else:
                                    temp_T1 = val
                            except ValueError:
                                pass
                        if c_val:
                            desc_parts.append(f"Target-cool C{c_val[1:]}°C")
                            temp_events.append((total_lines, f"M620.15 Target-cool C{c_val[1:]}°C"))
                            try:
                                val = int(c_val[1:])
                                if target_heater == 0:
                                    temp_T0 = val
                                else:
                                    temp_T1 = val
                            except ValueError:
                                pass
                        desc = " & ".join(desc_parts)
                        temp_track.append((total_lines, active_extruder, temp_T0, temp_T1, f"M620.15 {desc}", in_m620_block, printing_started))
                
                elif any(first_word.startswith(prefix) for prefix in ["M104", "M109", "VM104", "VM109"]):
                    is_critical = True
                    parts = cmd_part.split()
                    s_val = next((p for p in parts if p.startswith("S")), None)
                    t_val = next((p for p in parts if p.startswith("T")), "")
                    if s_val:
                        s_temp = int(s_val[1:])
                        t_desc = f" T{t_val[1:]}" if t_val else ""
                        temp_events.append((total_lines, f"{first_word}{t_desc} S{s_val[1:]}°C"))
                        
                        # Determine target heater
                        if t_val:
                            # Explicit T parameter: T0 = heater 0, T1 = heater 1
                            if t_val == "T0":
                                targeted_heater = 0
                            elif t_val == "T1":
                                targeted_heater = 1
                            else:
                                targeted_heater = 0
                        else:
                            # No T param: applies to active heater
                            # H2C mapping: use nozzle_map
                            targeted_heater = nozzle_map.get(active_extruder, 1)
                                
                        if targeted_heater == 0:
                            temp_T0 = s_temp
                        else:
                            temp_T1 = s_temp
                            
                        temp_track.append((total_lines, active_extruder, temp_T0, temp_T1, f"{first_word}{t_desc} S{s_temp}°C", in_m620_block, printing_started))
                    
                if is_critical:
                    critical_events.append(f"Line {total_lines}: {cmd_part}")
                
                # Insert regular temperature samples every 100 lines to prevent linear interpolation artifacts
                if total_lines % 100 == 0:
                    temp_track.append((total_lines, active_extruder, temp_T0, temp_T1, "Regular Sample", in_m620_block, printing_started))
    except KeyError:
        pass
    except Exception as e:
        print(f"Error parsing G-code: {e}")
        
    stats = {
        "toolchange_count": toolchange_count,
        "toolchange_sequence": toolchange_sequence,
        "prime_tower_blocks": prime_tower_blocks,
        "prime_tower_lines": prime_tower_lines,
        "m620_11_retracts": sorted(list(set(m620_11_retracts))),
        "temp_events": temp_events,
        "temp_track": temp_track,
        "toolchange_blocks": toolchange_blocks,
        "m73_points": m73_points
    }
    return critical_events, total_lines, file_size, stats

def get_slicer_name(meta):
    version_info = meta.get("version_info", {})
    if "OrcaSlicer-Version" in version_info:
        return "OrcaSlicer"
    elif "X-BBL-Client-Version" in version_info:
        return "BambuStudio"
    for k in version_info.keys():
        if "orcaslicer" in k.lower():
            return "OrcaSlicer"
        if "bambu" in k.lower() or "bbl" in k.lower():
            return "BambuStudio"
    return "UnknownSlicer"

def analyze_critical_discrepancies(f1_meta, f2_meta, f1_settings, f2_settings, f1_stats, f2_stats):
    discrepancies = []
    slicer1 = get_slicer_name(f1_meta)
    slicer2 = get_slicer_name(f2_meta)
    
    # 1. Nozzle map index errors
    def clean_map(m):
        if not m:
            return []
        if isinstance(m, str):
            m = m.replace("[", "").replace("]", "").replace("'", "").replace(",", " ")
            return [int(x) for x in m.split()]
        elif isinstance(m, list):
            return [int(x) for x in m]
        return []
        
    fnm1 = clean_map(f1_settings.get("filament_nozzle_map"))
    fnm2 = clean_map(f2_settings.get("filament_nozzle_map"))
    
    if fnm1:
        # Compute max valid nozzle slot from extruder_nozzle_stats
        # Format: ['Standard#1', 'Standard#4|High Flow#2'] → total = 1+4+2 = 7, max_slot = 6
        max_nozzle_slot = 5  # default fallback
        ens = f1_settings.get("extruder_nozzle_stats") or f2_settings.get("extruder_nozzle_stats")
        if ens:
            if isinstance(ens, str):
                ens = ens.split("','")
            total_nozzles = 0
            for entry in ens:
                entry = entry.strip().strip("'\"[] ")
                for part in entry.split("|"):
                    part = part.strip()
                    if "#" in part:
                        try:
                            total_nozzles += int(part.split("#")[1])
                        except (ValueError, IndexError):
                            pass
            if total_nozzles > 0:
                max_nozzle_slot = total_nozzles - 1
        invalid = [x for x in fnm1 if x < 0 or x > max_nozzle_slot]
        if invalid:
            discrepancies.append({
                "level": "CRITICAL ERROR",
                "message": f"{slicer1} `filament_nozzle_map` contains invalid nozzle slots {invalid} (out of H2C nozzle limits 0..{max_nozzle_slot}). This breaks physical switching."
            })
            
    # 2. Nozzle group collisions (multiple active filaments on one slot)
    if fnm1:
        active_filaments = [i for i, fil in enumerate(f1_meta["filaments"]) if fil.get("used_g") and float(fil["used_g"]) > 0]
        used_slots = {}
        for f_idx in active_filaments:
            if f_idx < len(fnm1):
                slot = fnm1[f_idx]
                if slot != 0:
                    if slot in used_slots:
                        used_slots[slot].append(f_idx + 1)
                    else:
                        used_slots[slot] = [f_idx + 1]
        for slot, fils in used_slots.items():
            if len(fils) > 1:
                discrepancies.append({
                    "level": "CRITICAL DISCREPANCY",
                    "message": f"{slicer1}: different active filament colors {fils} share the same carousel slot {slot} (duplicate nozzle). Because of this, filament changes cause AMS flushing instead of physical nozzle change!"
                })
                
    # 3. Toolchange counts comparison
    tc1 = f1_stats["toolchange_count"]
    tc2 = f2_stats["toolchange_count"]
    if tc1 != tc2:
        discrepancies.append({
            "level": "CRITICAL ERROR",
            "message": f"Filament change count mismatch: {slicer1} has {tc1} changes, {slicer2} has {tc2} changes."
        })
        
    # 4. Filament maps differences
    fmap1 = f1_meta["plate_meta"].get("filament_maps")
    fmap2 = f2_meta["plate_meta"].get("filament_maps")
    if fmap1 != fmap2:
        discrepancies.append({
            "level": "WARNING",
            "message": f"Filament maps differ: {slicer1} `{fmap1}` vs {slicer2} `{fmap2}`."
        })
        
    # 5. Weight discrepancy
    w1 = float(f1_meta["plate_meta"].get("weight", 0))
    w2 = float(f2_meta["plate_meta"].get("weight", 0))
    if w1 > 0 and w2 > 0:
        ratio = abs(w1 - w2) / max(w1, w2)
        if ratio > 0.2:
            discrepancies.append({
                "level": "CRITICAL DISCREPANCY",
                "message": f"Huge difference in part weight: {slicer1} {w1:.2f} g vs {slicer2} {w2:.2f} g (difference {abs(w1-w2):.2f} g or {ratio*100:.1f}%). The reason is incorrect nozzle mapping in {slicer1}, causing huge AMS flushing."
            })
            
    # 6. Prediction time discrepancy
    pred1 = float(f1_meta["plate_meta"].get("prediction", 0))
    pred2 = float(f2_meta["plate_meta"].get("prediction", 0))
    if pred1 > 0 and pred2 > 0:
        ratio = abs(pred1 - pred2) / max(pred1, pred2)
        if ratio > 0.2:
            discrepancies.append({
                "level": "CRITICAL DISCREPANCY",
                "message": f"Print time difference exceeds 20%: {slicer1} {int(pred1/60)} min vs {slicer2} {int(pred2/60)} min (difference {int((pred1-pred2)/60)} min or {ratio*100:.1f}%)."
            })
            
    # 7. Physical extruder map
    pem1 = f1_settings.get("physical_extruder_map")
    pem2 = f2_settings.get("physical_extruder_map")
    if pem1 != pem2:
        discrepancies.append({
            "level": "WARNING",
            "message": f"Physical extruder map `physical_extruder_map` differs: {slicer1} {pem1} vs {slicer2} {pem2}."
        })
        
    # 8. Nozzle statistics (extruder_nozzle_stats)
    ens1 = f1_settings.get("extruder_nozzle_stats")
    ens2 = f2_settings.get("extruder_nozzle_stats")
    if ens1 != ens2:
        discrepancies.append({
            "level": "WARNING",
            "message": f"Nozzle inventories `extruder_nozzle_stats` differ: {slicer1} `{ens1}` vs {slicer2} `{ens2}`."
        })
        if ens1:
            ens1_str = str(ens1)
            if "Standard#7" in ens1_str:
                discrepancies.append({
                    "level": "CRITICAL ERROR",
                    "message": f"H2C nozzle changer bug detected in {slicer1}: `extruder_nozzle_stats` is set to `Standard#7` for both extruders. This causes incorrect linear nozzle list construction and mapping crash!"
                })
                
    return discrepancies

def interpret_temp_action(desc, active_extruder):
    try:
        desc_clean = desc.replace("°C", "")
        words = desc_clean.split()
        if not words:
            return "Control Command"
        cmd = words[0]
        
        if cmd.startswith("T") and "Active Extruder" in desc:
            return "Active Extruder Switch"
            
        if "Pre-cool" in desc:
            p_temp = desc.split("P")[-1].replace("°C", "").strip()
            return f"Virtual Preheat (Pre-cool) before change to {p_temp}°C"
            
        if "Target-cool" in desc:
            c_temp = desc.split("C")[-1].replace("°C", "").strip()
            return f"Set Target Active Nozzle Temp to {c_temp}°C"
            
        if cmd in ["M104", "M109", "VM104", "VM109"]:
            s_val = None
            t_val = None
            for w in words:
                if w.startswith("S"):
                    try:
                        s_val = int(w[1:])
                    except ValueError:
                        pass
                elif w.startswith("T") and not "Active" in desc:
                    t_val = w
                    
            heater = active_extruder
            if t_val == "T0":
                heater = 0
            elif t_val == "T1":
                heater = 1
                
            heater_name = "Left (T0)" if heater == 0 else "Right (T1)"
            
            if s_val is not None:
                if s_val <= 50:
                    action_type = "Cooling" if s_val > 0 else "Power Off"
                    target_desc = "standby (cooldown)" if s_val > 0 else "heater"
                    return f"{action_type} {heater_name} to {target_desc} ({s_val}°C)"
                elif 150 <= s_val <= 215:
                    if heater != active_extruder:
                        return f"Standby Preheat {heater_name} to {s_val}°C"
                    else:
                        return f"Heating {heater_name} to intermediate temp {s_val}°C"
                else:
                    action = "Stabilizing Temperature" if cmd == "M109" else "Heating"
                    return f"{action} {heater_name} to active print temp ({s_val}°C)"
    except Exception as e:
        return f"Temperature Command ({e})"
    return "Control Command"

def format_side_by_side_temp_track(f1_track, f2_track, f1_name, f2_name):
    # 1. Find toolchanges for File 1 (Orca)
    tc1 = []
    for idx, item in enumerate(f1_track):
        desc = item[4]
        line = item[0]
        if desc.startswith("T") and "Active Extruder" in desc:
            first_part = desc.split()[0]
            t_num = first_part[1:]
            if t_num.isdigit() and int(t_num) < 1000:
                tc1.append((idx, line, int(t_num)))
                
    # 2. Find toolchanges for File 2 (BBL)
    tc2 = []
    for idx, item in enumerate(f2_track):
        desc = item[4]
        line = item[0]
        if desc.startswith("T") and "Active Extruder" in desc:
            first_part = desc.split()[0]
            t_num = first_part[1:]
            if t_num.isdigit() and int(t_num) < 1000:
                tc2.append((idx, line, int(t_num)))
                
    if not tc1 and not tc2:
        res = ["##### Start Warmup Comparison (first 30 events):\n"]
        res.append("| Orca: Line | Orca: Command | T0/T1 | BBL: Line | BBL: Command | T0/T1 |\n")
        res.append("| :---: | :--- | :---: | :---: | :--- | :---: |\n")
        limit = min(30, max(len(f1_track), len(f2_track)))
        for i in range(limit):
            item1 = f1_track[i] if i < len(f1_track) else None
            item2 = f2_track[i] if i < len(f2_track) else None
            
            o_line = str(item1[0]) if item1 else ""
            o_desc = item1[4] if item1 else ""
            o_temps = f"{item1[2]}/{item1[3]}°C" if item1 else ""
            
            b_line = str(item2[0]) if item2 else ""
            b_desc = item2[4] if item2 else ""
            b_temps = f"{item2[2]}/{item2[3]}°C" if item2 else ""
            
            res.append(f"| {o_line} | {o_desc} | {o_temps} | {b_line} | {b_desc} | {b_temps} |\n")
        return "".join(res)

    # 3. Create ranges around toolchanges for Orca
    ranges1 = []
    for idx, line, t_num in tc1:
        start_idx = max(0, idx - 10)
        end_idx = min(len(f1_track) - 1, idx + 8)
        ranges1.append((start_idx, end_idx, f"Change to T{t_num} (line {line})", line))
        
    m_ranges1 = []
    if ranges1:
        curr_start, curr_end, curr_label, curr_last_line = ranges1[0]
        for start, end, label, line in ranges1[1:]:
            if line - curr_last_line <= 1000:
                curr_end = max(curr_end, end)
                curr_label += f" -> T{label.split(' to T')[-1].split()[0]}"
                curr_last_line = line
            else:
                m_ranges1.append((curr_start, curr_end, curr_label))
                curr_start, curr_end, curr_label, curr_last_line = start, end, label, line
        m_ranges1.append((curr_start, curr_end, curr_label))
        
    # Ranges for BBL
    ranges2 = []
    for idx, line, t_num in tc2:
        start_idx = max(0, idx - 10)
        end_idx = min(len(f2_track) - 1, idx + 8)
        ranges2.append((start_idx, end_idx, f"Change to T{t_num} (line {line})", line))
        
    m_ranges2 = []
    if ranges2:
        curr_start, curr_end, curr_label, curr_last_line = ranges2[0]
        for start, end, label, line in ranges2[1:]:
            if line - curr_last_line <= 1000:
                curr_end = max(curr_end, end)
                curr_label += f" -> T{label.split(' to T')[-1].split()[0]}"
                curr_last_line = line
            else:
                m_ranges2.append((curr_start, curr_end, curr_label))
                curr_start, curr_end, curr_label, curr_last_line = start, end, label, line
        m_ranges2.append((curr_start, curr_end, curr_label))
        
    res = []
    
    # Render Start Warmup
    res.append("#### Start Warmup Before Printing:\n")
    res.append("| Orca: Line | Orca: Event (Phase) | Orca T0/T1 | BBL: Line | BBL: Event (Phase) | BBL T0/T1 |\n")
    res.append("| :---: | :--- | :---: | :---: | :--- | :---: |\n")
    
    first_start1 = m_ranges1[0][0] if m_ranges1 else len(f1_track)
    first_start2 = m_ranges2[0][0] if m_ranges2 else len(f2_track)
    
    limit = max(first_start1, first_start2)
    for i in range(limit):
        item1 = f1_track[i] if i < first_start1 else None
        item2 = f2_track[i] if i < first_start2 else None
        
        o_line, o_desc, o_temps = "", "", ""
        if item1:
            o_line = str(item1[0])
            act1 = interpret_temp_action(item1[4], item1[1])
            o_desc = f"{item1[4]} ({act1})"
            o_temps = f"{item1[2]}/{item1[3]}°C"
            
        b_line, b_desc, b_temps = "", "", ""
        if item2:
            b_line = str(item2[0])
            act2 = interpret_temp_action(item2[4], item2[1])
            b_desc = f"{item2[4]} ({act2})"
            b_temps = f"{item2[2]}/{item2[3]}°C"
            
        res.append(f"| {o_line} | {o_desc} | {o_temps} | {b_line} | {b_desc} | {b_temps} |\n")
    res.append("\n")
    
    # Render transitions
    num_transitions = max(len(m_ranges1), len(m_ranges2))
    for t_idx in range(num_transitions):
        r1 = m_ranges1[t_idx] if t_idx < len(m_ranges1) else None
        r2 = m_ranges2[t_idx] if t_idx < len(m_ranges2) else None
        
        label1 = r1[2] if r1 else "No transition"
        label2 = r2[2] if r2 else "No transition"
        
        res.append(f"#### Transition #{t_idx + 1}:\n")
        res.append(f"**Orca:** {label1} | **BBL:** {label2}\n\n")
        res.append("| Orca: Line | Orca: Event (Phase) | Orca T0/T1 | BBL: Line | BBL: Event (Phase) | BBL T0/T1 |\n")
        res.append("| :---: | :--- | :---: | :---: | :--- | :---: |\n")
        
        events1 = f1_track[r1[0]:r1[1]+1] if r1 else []
        events2 = f2_track[r2[0]:r2[1]+1] if r2 else []
        
        max_ev = max(len(events1), len(events2))
        for i in range(max_ev):
            item1 = events1[i] if i < len(events1) else None
            item2 = events2[i] if i < len(events2) else None
            
            o_line, o_desc, o_temps = "", "", ""
            if item1:
                o_line = str(item1[0])
                act1 = interpret_temp_action(item1[4], item1[1])
                o_desc = f"{item1[4]} ({act1})"
                o_temps = f"{item1[2]}/{item1[3]}°C"
                if item1[4].startswith("T") and "Active Extruder" in item1[4] and not "T100" in item1[4]:
                    o_line = f"**{o_line}**"
                    o_desc = f"**{o_desc}**"
                    o_temps = f"**{o_temps}**"
                    
            b_line, b_desc, b_temps = "", "", ""
            if item2:
                b_line = str(item2[0])
                act2 = interpret_temp_action(item2[4], item2[1])
                b_desc = f"{item2[4]} ({act2})"
                b_temps = f"{item2[2]}/{item2[3]}°C"
                if item2[4].startswith("T") and "Active Extruder" in item2[4] and not "T100" in item2[4]:
                    b_line = f"**{b_line}**"
                    b_desc = f"**{b_desc}**"
                    b_temps = f"**{b_temps}**"
                    
            res.append(f"| {o_line} | {o_desc} | {o_temps} | {b_line} | {b_desc} | {b_temps} |\n")
        res.append("\n")
        
    return "".join(res)


def get_nozzle_map(filament_maps_str, track):
    if not filament_maps_str:
        filament_maps_str = "1 1 1 1 1 2"
        
    extruder_map = {}
    for i, v in enumerate(filament_maps_str.split()):
        try:
            extruder_map[i] = int(v)
        except ValueError:
            pass
            
    # H2C physical mapping: Left (Extruder 1) -> Heater 1, Right (Extruder 2) -> Heater 0
    heater_to_ext = {1: 1, 2: 0}
    nozzle_map = {}
    for fid, ext_id in extruder_map.items():
        nozzle_map[fid] = heater_to_ext.get(ext_id, 1)
        
    return nozzle_map


def analyze_preheat_cooldown_events(track, nozzle_map=None):
    if nozzle_map is None:
        nozzle_map = {1: 1} # Fallback H2C: T1 -> heater 1, others -> heater 0

    events = []
    tc_indices = []
    
    prev_nozzle = None
    for idx, item in enumerate(track):
        desc = item[4]
        if desc.startswith("T") and "Active Filament" in desc:
            first_part = desc.split()[0]
            t_num_str = first_part[1:]
            if t_num_str.isdigit():
                t_num = int(t_num_str)
                if t_num >= 60000:
                    continue
                    
                norm_nozzle = t_num
                if t_num == 1000:
                    norm_nozzle = 0
                elif t_num == 1001:
                    norm_nozzle = 1
                    
                if prev_nozzle is not None and norm_nozzle != prev_nozzle:
                    target_ext = nozzle_map.get(norm_nozzle, 0)
                    source_ext = nozzle_map.get(prev_nozzle, 0)
                    if target_ext != source_ext:
                        tc_indices.append((idx, item[0], target_ext, source_ext, norm_nozzle))
                prev_nozzle = norm_nozzle
                
    for i, (tc_idx, tc_line, target_ext, source_ext, nozzle_num) in enumerate(tc_indices):
        # 1. Look for preheat command for target_ext before the toolchange
        preheat_line = None
        preheat_temp = None
        
        prev_tc_idx = tc_indices[i - 1][0] if i > 0 else 0
        for k in range(tc_idx - 1, prev_tc_idx - 1, -1):
                
            desc_k = track[k][4]
            active_ext_k = track[k][1]
            
            # Check M104 / M109
            if "M104" in desc_k or "M109" in desc_k:
                words = desc_k.split()
                s_temp_str = next((w[1:].replace("°C", "") for w in words if w.startswith("S")), None)
                t_val = next((w for w in words if w.startswith("T")), None)
                
                targeted = None
                if t_val == "T0":
                    targeted = 0
                elif t_val == "T1":
                    targeted = 1
                elif t_val is None:
                    targeted = nozzle_map.get(active_ext_k, 0)
                    
                if targeted is not None and targeted == target_ext and s_temp_str and s_temp_str.isdigit():
                    temp_val = int(s_temp_str)
                    if temp_val >= 150: # Real preheat threshold
                        preheat_line = track[k][0]
                        preheat_temp = temp_val
                        break  # Stop at first (closest to toolchange)
                        
            # Check M620.15 (Vortek pre-cooling/pre-heating commands)
            elif "M620.15" in desc_k and target_ext == 1:
                words = desc_k.split()
                p_temp_str = next((w[1:].replace("°C", "") for w in words if w.startswith("P") and len(w) > 1 and w[1].isdigit()), None)
                c_temp_str = next((w[1:].replace("°C", "") for w in words if w.startswith("C") and len(w) > 1 and w[1].isdigit()), None)
                
                temp_val = None
                if p_temp_str and p_temp_str.isdigit():
                    temp_val = int(p_temp_str)
                elif c_temp_str and c_temp_str.isdigit():
                    temp_val = int(c_temp_str)
                    
                if temp_val and temp_val >= 150:
                    preheat_line = track[k][0]
                    preheat_temp = temp_val
                    
        cooldown_line = None
        cooldown_temp = None
        start_look = max(0, tc_idx - 15)
        end_look = min(len(track), tc_idx + 40)
        
        for k in range(start_look, end_look):
            desc_k = track[k][4]
            active_ext_k = track[k][1]
            
            if "M104" in desc_k or "M109" in desc_k:
                words = desc_k.split()
                s_temp_str = next((w[1:].replace("°C", "") for w in words if w.startswith("S")), None)
                t_val = next((w for w in words if w.startswith("T")), None)
                
                targeted = None
                if t_val == "T0":
                    targeted = 0
                elif t_val == "T1":
                    targeted = 1
                elif t_val is None:
                    targeted = nozzle_map.get(active_ext_k, 0)
                    
                if targeted == source_ext and s_temp_str and s_temp_str.isdigit():
                    temp_val = int(s_temp_str)
                    if temp_val < 200:
                        cooldown_line = track[k][0]
                        cooldown_temp = temp_val
                        break
                        
            elif "M620.15" in desc_k and source_ext == 1:
                words = desc_k.split()
                p_temp_str = next((w[1:].replace("°C", "") for w in words if w.startswith("P") and len(w) > 1 and w[1].isdigit()), None)
                c_temp_str = next((w[1:].replace("°C", "") for w in words if w.startswith("C") and len(w) > 1 and w[1].isdigit()), None)
                
                temp_val = None
                if p_temp_str and p_temp_str.isdigit():
                    temp_val = int(p_temp_str)
                elif c_temp_str and c_temp_str.isdigit():
                    temp_val = int(c_temp_str)
                    
                if temp_val and temp_val < 200:
                    cooldown_line = track[k][0]
                    cooldown_temp = temp_val
                    break
                    
        events.append({
            "tc_line": tc_line,
            "target_ext": target_ext,
            "nozzle_num": nozzle_num,
            "preheat_line": preheat_line,
            "preheat_temp": preheat_temp,
            "cooldown_line": cooldown_line,
            "cooldown_temp": cooldown_temp
        })
    return events


def build_comparison_report(f1_name, f1_meta, f1_settings, f1_events, f1_lines, f1_size, f1_stats,
                             f2_name, f2_meta, f2_settings, f2_events, f2_lines, f2_size, f2_stats):
    
    now_str = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    slicer1 = get_slicer_name(f1_meta)
    slicer2 = get_slicer_name(f2_meta)
    
    hdr1 = f"File 1 ({slicer1})"
    hdr2 = f"File 2 ({slicer2})"
    
    v1_val = f1_meta["version_info"].get("OrcaSlicer-Version") or f1_meta["version_info"].get("X-BBL-Client-Version") or "Unknown"
    v2_val = f2_meta["version_info"].get("OrcaSlicer-Version") or f2_meta["version_info"].get("X-BBL-Client-Version") or "Unknown"
    
    report = []
    report.append(f"# Slicing G-code Comparison (3MF): {f1_name} vs {f2_name}\n")
    report.append(f"**Report Generation Date:** {now_str}\n")
    
    # 1. Summary table
    report.append("## 1. Summary File Statistics\n")
    report.append(f"| Metric | {hdr1} | {hdr2} | Difference |\n")
    report.append("| :--- | :---: | :---: | :---: |\n")
    report.append(f"| **File Name** | `{f1_name}` | `{f2_name}` | — |\n")
    report.append(f"| **G-code Size** | {f1_size / (1024*1024):.2f} MB | {f2_size / (1024*1024):.2f} MB | { (f1_size - f2_size) / (1024*1024):+.2f} MB |\n")
    report.append(f"| **Total G-code Lines** | {f1_lines:,} | {f2_lines:,} | {f1_lines - f2_lines:+,} |\n")
    report.append(f"| **Critical Events** | {len(f1_events):,} | {len(f2_events):,} | {len(f1_events) - len(f2_events):+,} |\n")
    
    pred1 = float(f1_meta["plate_meta"].get("prediction", 0))
    pred2 = float(f2_meta["plate_meta"].get("prediction", 0))
    report.append(f"| **Print Time** | {int(pred1/60)} min | {int(pred2/60)} min | {int((pred1 - pred2)/60):+} min |\n")
    
    w1 = float(f1_meta["plate_meta"].get("weight", 0))
    w2 = float(f2_meta["plate_meta"].get("weight", 0))
    report.append(f"| **Total Part Weight** | {w1:.2f} g | {w2:.2f} g | {w1 - w2:+.2f} g |\n")
    
    fl1 = float(f1_meta["plate_meta"].get("first_layer_time", 0))
    fl2 = float(f2_meta["plate_meta"].get("first_layer_time", 0))
    report.append(f"| **First Layer Time** | {fl1/60:.2f} min | {fl2/60:.2f} min | {(fl1 - fl2)/60:+.2f} min |\n")
    
    report.append(f"| **Slicer Version** | {slicer1} `{v1_val}` | {slicer2} `{v2_val}` | — |\n")
    
    m1 = f1_meta["plate_meta"].get("printer_model_id", "Unknown")
    m2 = f2_meta["plate_meta"].get("printer_model_id", "Unknown")
    report.append(f"| **Printer Model ID** | `{m1}` | `{m2}` | — |\n")
    
    fmap1 = f1_meta["plate_meta"].get("filament_maps", "None")
    fmap2 = f2_meta["plate_meta"].get("filament_maps", "None")
    report.append(f"| **filament_maps Map** | `{fmap1}` | `{fmap2}` | — |\n")
    
    dynamic1 = f1_meta["plate_meta"].get("enable_filament_dynamic_map", "Unknown")
    dynamic2 = f2_meta["plate_meta"].get("enable_filament_dynamic_map", "Unknown")
    report.append(f"| **Dynamic Mapping** | `{dynamic1}` | `{dynamic2}` | — |\n")
    
    switcher1 = f1_meta["plate_meta"].get("has_filament_switcher", "Unknown")
    switcher2 = f2_meta["plate_meta"].get("has_filament_switcher", "Unknown")
    report.append(f"| **Nozzle Switcher** | `{switcher1}` | `{switcher2}` | — |\n")
    
    ens1 = f1_settings.get("extruder_nozzle_stats", "None")
    ens2 = f2_settings.get("extruder_nozzle_stats", "None")
    report.append(f"| **Nozzle Stats (extruder_nozzle_stats)** | `{escape_markdown_table(ens1)}` | `{escape_markdown_table(ens2)}` | — |\n")

    fnm1 = f1_settings.get("filament_nozzle_map", "None")
    fnm2 = f2_settings.get("filament_nozzle_map", "None")
    report.append(f"| **Nozzle Map (filament_nozzle_map)** | `{escape_markdown_table(fnm1)}` | `{escape_markdown_table(fnm2)}` | — |\n")

    fvm1 = f1_settings.get("filament_volume_map", "None")
    fvm2 = f2_settings.get("filament_volume_map", "None")
    report.append(f"| **Volume Maps (filament_volume_map)** | `{escape_markdown_table(fvm1)}` | `{escape_markdown_table(fvm2)}` | — |\n")
    report.append("\n")
    
    # 2. Preheat and Standby analysis table
    report.append("## 2. Preheat and Standby Cooldown Analysis\n")
    report.append("The table matches preheating events of the target nozzle before change with the cooldown of the inactive nozzle.\n")
    report.append("Lead (lines) shows how many G-code lines before physical toolchange `T` the printer sends `M104/M109` heating command.\n\n")
    report.append(f"| Change # | Target Extruder | {slicer1}: Preheat | {slicer1}: Cooldown | {slicer2}: Preheat | {slicer2}: Cooldown |\n")
    report.append("| :---: | :---: | :--- | :--- | :--- | :--- |\n")
    
    f1_nozzle_map = get_nozzle_map(f1_meta["plate_meta"].get("filament_maps"), f1_stats["temp_track"])
    f2_nozzle_map = get_nozzle_map(f2_meta["plate_meta"].get("filament_maps"), f2_stats["temp_track"])
    f1_preheat = analyze_preheat_cooldown_events(f1_stats["temp_track"], f1_nozzle_map)
    f2_preheat = analyze_preheat_cooldown_events(f2_stats["temp_track"], f2_nozzle_map)
    
    max_preheat = max(len(f1_preheat), len(f2_preheat))
    for idx in range(max_preheat):
        p1 = f1_preheat[idx] if idx < len(f1_preheat) else None
        p2 = f2_preheat[idx] if idx < len(f2_preheat) else None
        
        target_str = f"T{p1['nozzle_num']}" if p1 else (f"T{p2['nozzle_num']}" if p2 else "")
        
        o_preheat_str = "—"
        if p1 and p1["preheat_temp"] is not None:
            lead = p1["tc_line"] - p1["preheat_line"]
            o_preheat_str = f"{p1['preheat_temp']}°C ({lead} lines lead)"
            
        o_cooldown_str = "—"
        if p1 and p1["cooldown_temp"] is not None:
            o_cooldown_str = f"{p1['cooldown_temp']}°C"
            
        b_preheat_str = "—"
        if p2 and p2["preheat_temp"] is not None:
            lead = p2["tc_line"] - p2["preheat_line"]
            b_preheat_str = f"{p2['preheat_temp']}°C ({lead} lines lead)"
            
        b_cooldown_str = "—"
        if p2 and p2["cooldown_temp"] is not None:
            b_cooldown_str = f"{p2['cooldown_temp']}°C"
            
        report.append(f"| {idx+1} | `{target_str}` | {o_preheat_str} | {o_cooldown_str} | {b_preheat_str} | {b_cooldown_str} |\n")
    report.append("\n")
    
    # 3. Nozzle to Extruder mappings
    report.append("## 3. Nozzle and Extruder Mapping (Vortek Nozzles)\n")
    report.append(f"**{slicer1} `extruder_nozzle_stats`:** `{ens1}`\n\n")
    report.append(f"**{slicer2} `extruder_nozzle_stats`:** `{ens2}`\n\n")
    
    report.append(f"### {hdr1}:\n")
    if f1_meta["nozzles"]:
        report.append("| Nozzle ID | Extruder ID | Diameter |\n| :---: | :---: | :---: |\n")
        for noz in f1_meta["nozzles"]:
            report.append(f"| {noz['id']} | {noz['extruder_id']} | {noz['nozzle_diameter']} |\n")
    else:
        report.append("*Nozzle mapping is not present in metadata*\n")
    report.append("\n")
    
    report.append(f"### {hdr2}:\n")
    if f2_meta["nozzles"]:
        report.append("| Nozzle ID | Extruder ID | Diameter |\n| :---: | :---: | :---: |\n")
        for noz in f2_meta["nozzles"]:
            report.append(f"| {noz['id']} | {noz['extruder_id']} | {noz['nozzle_diameter']} |\n")
    else:
        report.append("*Nozzle mapping is not present in metadata*\n")
    report.append("\n")
    
    # 4. Filaments used
    report.append("## 4. Filaments Used\n")
    report.append(f"### {hdr1}:\n")
    report.append("| ID | Type | Color | Weight (g) | Length (m) | Nozzle Dia |\n| :---: | :--- | :---: | :---: | :---: | :---: |\n")
    for fil in f1_meta["filaments"]:
        c = fil["color"]
        used_m = f"{float(fil.get('used_m', 0)):.2f} m" if fil.get("used_m") else "—"
        report.append(f"| {fil['id']} | {fil['type']} | <font color=\"{c}\">■</font> `{c}` | {fil['used_g']} g | {used_m} | {fil['nozzle_diameter']} |\n")
    report.append("\n")
    
    report.append(f"### {hdr2}:\n")
    report.append("| ID | Type | Color | Weight (g) | Length (m) | Nozzle Dia |\n| :---: | :--- | :---: | :---: | :---: | :---: |\n")
    for fil in f2_meta["filaments"]:
        c = fil["color"]
        used_m = f"{float(fil.get('used_m', 0)):.2f} m" if fil.get("used_m") else "—"
        report.append(f"| {fil['id']} | {fil['type']} | <font color=\"{c}\">■</font> `{c}` | {fil['used_g']} g | {used_m} | {fil['nozzle_diameter']} |\n")
    report.append("\n")
    
    # 5. Settings differences
    report.append("## 5. Differences in Key Slicing Settings (project_settings.config)\n")
    
    interesting_keys = {
        "extruder_nozzle_stats",
        "filament_nozzle_map",
        "filament_volume_map",
        "physical_extruder_map",
        "master_extruder_id",
        "extruder_max_nozzle_count",
        "has_filament_switcher",
        "enable_filament_dynamic_map",
        "filament_settings_id",
        "filament_type",
        "machine_load_filament_time",
        "machine_unload_filament_time",
        "enable_prime_tower",
        "prime_tower_width",
        "prime_tower_brim_width",
        "wipe_tower_size",
        "wipe_tower_width",
        "filament_pre_cooling_temperature",
        "filament_pre_cooling_temperature_nc",
        "filament_retract_length_nc",
        "filament_retract_speed_nc",
        "filament_deretract_speed_nc",
        "layer_height",
        "initial_layer_print_height",
        "wall_loops",
        "sparse_infill_density",
        "sparse_infill_pattern",
        "bridge_flow_ratio",
        "flush_volumes_matrix",
        "flush_multiplier",
    }
    
    diff_settings = {}
    for key in sorted(interesting_keys):
        v1 = f1_settings.get(key)
        v2 = f2_settings.get(key)
        if v1 != v2:
            diff_settings[key] = (v1, v2)
            
    if diff_settings:
        report.append(f"| Setting Key | Value in {slicer1} | Value in {slicer2} |\n")
        report.append("| :--- | :--- | :--- |\n")
        for key, (v_orca, v_bbl) in diff_settings.items():
            report.append(f"| `{key}` | `{escape_markdown_table(v_orca)}` | `{escape_markdown_table(v_bbl)}` |\n")
    else:
        report.append("*No significant differences in key slicing settings found.*\n")
    report.append("\n")
    
    # 6. Deep Vortek & Print Logic Analysis
    report.append("## 6. Detailed Vortek Logic and Print Parameters Analysis\n")
    report.append("### A. Nozzle Changes and Tool Change Operations\n")
    report.append(f"| Parameter | {hdr1} | {hdr2} |\n")
    report.append("| :--- | :---: | :---: |\n")
    report.append(f"| **Total nozzle/extruder changes (T)** | {f1_stats['toolchange_count']} | {f2_stats['toolchange_count']} |\n")
    seq1 = " -> ".join(f1_stats['toolchange_sequence'][:12]) + ("..." if len(f1_stats['toolchange_sequence']) > 12 else "")
    seq2 = " -> ".join(f2_stats['toolchange_sequence'][:12]) + ("..." if len(f2_stats['toolchange_sequence']) > 12 else "")
    report.append(f"| **Change Sequence (first 12)** | `{seq1}` | `{seq2}` |\n")
    report.append("\n")
    report.append("### B. Prime Tower Comparison\n")
    report.append(f"| Parameter | {hdr1} | {hdr2} |\n")
    report.append("| :--- | :--- | :---: |\n")
    report.append(f"| **Total Prime Tower Entries (M628 S1)** | {f1_stats['prime_tower_blocks']} | {f2_stats['prime_tower_blocks']} |\n")
    report.append(f"| **Total G-code Lines Inside Tower** | {f1_stats['prime_tower_lines']:,} | {f2_stats['prime_tower_lines']:,} |\n")
    report.append("\n")
    report.append("### C. Retract Parameters During Nozzle Switch (M620.11)\n")
    report.append(f"| {hdr1} | {hdr2} |\n")
    report.append("| :--- | :--- |\n")
    max_len = max(len(f1_stats['m620_11_retracts']), len(f2_stats['m620_11_retracts']))
    for i in range(max_len):
        r1 = f1_stats['m620_11_retracts'][i] if i < len(f1_stats['m620_11_retracts']) else ""
        r2 = f2_stats['m620_11_retracts'][i] if i < len(f2_stats['m620_11_retracts']) else ""
        report.append(f"| `{r1}` | `{r2}` |\n")
    report.append("\n")
    
    # Toolchange G-code Blocks Diff
    report.append("### D. Change Filament G-code Blocks Analysis (change_filament_gcode)\n")
    max_tc_blocks = max(len(f1_stats['toolchange_blocks']), len(f2_stats['toolchange_blocks']))
    diffs_count = 0
    diff_summary = []
    
    def clean_gcode_lines(lines_list):
        res = []
        for line in lines_list:
            if line.startswith("Line "):
                parts = line.split(": ", 1)
                if len(parts) == 2:
                    res.append(parts[1])
                else:
                    res.append(line)
            else:
                res.append(line)
        return res

    for b_idx in range(max_tc_blocks):
        b1 = f1_stats['toolchange_blocks'][b_idx].splitlines() if b_idx < len(f1_stats['toolchange_blocks']) else []
        b2 = f2_stats['toolchange_blocks'][b_idx].splitlines() if b_idx < len(f2_stats['toolchange_blocks']) else []
        
        b1_clean = clean_gcode_lines(b1)
        b2_clean = clean_gcode_lines(b2)
        
        tc_diff = list(difflib.unified_diff(b1_clean, b2_clean, lineterm=""))
        if tc_diff:
            diffs_count += 1
            changed_lines = [l for l in tc_diff if l.startswith('+') or l.startswith('-')]
            changed_lines = [l for l in changed_lines if not l.startswith('+++') and not l.startswith('---')]
            commands_changed = set()
            for cl in changed_lines:
                cmd = cl[1:].strip().split()[0] if cl[1:].strip() else ""
                if cmd:
                    commands_changed.add(cmd)
            cmds_str = ", ".join(sorted(list(commands_changed)))
            diff_summary.append(f"- **Change #{b_idx + 1}**: differences found ({len(changed_lines)} modified lines, commands: `{cmds_str}`)\n")
            
    if diffs_count == 0:
        report.append("> [!NOTE]\n> All change filament G-code blocks (`change_filament_gcode`) are identical!\n\n")
    else:
        report.append(f"Filament change G-code blocks difference summary (total changes: {max_tc_blocks}, differing: {diffs_count}, identical: {max_tc_blocks - diffs_count}):\n")
        limit = 10
        for item in diff_summary[:limit]:
            report.append(item)
        if len(diff_summary) > limit:
            report.append(f"- ... and {len(diff_summary) - limit} more differing changes ...\n")
        report.append("\n")
            
    # G-code critical events diff
    report.append("## 6. Differences in G-code Control Commands (Toolchange / Nozzle Changer / Temp)\n")
    
    def clean_events(events_list):
        res = []
        for ev in events_list:
            if ev.startswith("Line "):
                parts = ev.split(": ", 1)
                if len(parts) == 2:
                    res.append(parts[1])
                else:
                    res.append(ev)
            else:
                res.append(ev)
        return res
        
    events1_clean = clean_events(f1_events)
    events2_clean = clean_events(f2_events)
    
    diff = difflib.unified_diff(
        events1_clean, 
        events2_clean, 
        fromfile=slicer1, 
        tofile=slicer2, 
        n=2, 
        lineterm=""
    )
    
    diff_lines = list(diff)
    if diff_lines:
        diff_lines_clean = [dl for dl in diff_lines if not dl.startswith('---') and not dl.startswith('+++') and not dl.startswith('@@')]
        report.append(f"Detected {len(diff_lines_clean)} lines of differences in control commands timeline.\n")
        report.append("Showing key differences (max 12 lines):\n")
        report.append("```diff\n")
        for dline in diff_lines_clean[:12]:
            report.append(f"{dline}\n")
        if len(diff_lines_clean) > 12:
            report.append(f"... and {len(diff_lines_clean) - 12} more difference lines ...\n")
        report.append("```\n")
    else:
        report.append("> [!NOTE]\n")
        report.append("> G-code control commands timeline is identical!\n")
        
    # Critical Discrepancies Analyzer
    report.append("\n## 7. Critical Discrepancies and Errors Analysis (Analytics)\n")
    discrepancies = analyze_critical_discrepancies(f1_meta, f2_meta, f1_settings, f2_settings, f1_stats, f2_stats)
    if discrepancies:
        report.append("| Status | Problem Description |\n")
        report.append("| :--- | :--- |\n")
        for item in discrepancies:
            level_str = f"**{item['level']}**"
            if "ERROR" in item["level"]:
                level_str = f"<font color=\"#ff0000\">❌ {item['level']}</font>"
            elif "DISCREPANCY" in item["level"]:
                level_str = f"<font color=\"#ff9900\">⚠️ {item['level']}</font>"
            else:
                level_str = f"ℹ️ {item['level']}"
            report.append(f"| {level_str} | {item['message']} |\n")
    else:
        report.append("> [!NOTE]\n> No critical discrepancies or slicing logic errors found.\n")
    return "".join(report)

def main():
    if len(sys.argv) < 3:
        print("Usage: python3 compare_slices.py <file1.3mf> <file2.3mf>")
        sys.exit(1)
        
    f1_input = sys.argv[1]
    f2_input = sys.argv[2]
    
    f1_path = find_file(f1_input)
    f2_path = find_file(f2_input)
    
    if not f1_path:
        print(f"Error: File '{f1_input}' not found on Desktop or local path.")
        sys.exit(1)
    if not f2_path:
        print(f"Error: File '{f2_input}' not found on Desktop or local path.")
        sys.exit(1)
        
    print(f"Analyzing File 1: {f1_path}")
    with zipfile.ZipFile(f1_path, "r") as z1:
        f1_meta = parse_metadata(z1)
        f1_settings = parse_project_settings(z1)
        f1_maps_str = f1_meta["plate_meta"].get("filament_maps")
        f1_events, f1_lines, f1_size, f1_stats = parse_critical_gcode(z1, f1_maps_str)
        
    print(f"Analyzing File 2: {f2_path}")
    with zipfile.ZipFile(f2_path, "r") as z2:
        f2_meta = parse_metadata(z2)
        f2_settings = parse_project_settings(z2)
        f2_maps_str = f2_meta["plate_meta"].get("filament_maps")
        f2_events, f2_lines, f2_size, f2_stats = parse_critical_gcode(z2, f2_maps_str)
        
    f1_basename = os.path.basename(f1_path)
    f2_basename = os.path.basename(f2_path)
    
    report_content = build_comparison_report(
        f1_basename, f1_meta, f1_settings, f1_events, f1_lines, f1_size, f1_stats,
        f2_basename, f2_meta, f2_settings, f2_events, f2_lines, f2_size, f2_stats
    )
    
    os.makedirs(REPORTS_DIR, exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    report_name = f"compare_report_{timestamp}.md"
    report_path = os.path.join(REPORTS_DIR, report_name)
    
    with open(report_path, "w", encoding="utf-8") as f:
        f.write(report_content)
        
    print("\n" + "="*50)
    print(f"Comparison completed successfully!")
    print(f"Report saved to: {report_path}")
    print("="*50)

if __name__ == "__main__":
    main()
