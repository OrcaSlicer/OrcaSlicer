#!/usr/bin/env python3
"""
H2C Temperature Timeline Comparison Tool.
Generates interactive HTML with temperature plots for analyzing and comparing OrcaSlicer vs BambuStudio G-code.

================================================================================
ARCHITECTURE & MAPPING LOGIC:
================================================================================
- H2C configuration utilizes a dual-extruder layout with 4 Vortek nozzles.
- Physical heaters are mapped as:
  - Heater 0: Extruder 2 (physical RIGHT nozzle slot, T0/T2/T3/T4)
  - Heater 1: Extruder 1 (physical LEFT nozzle slot, T1)
- The active heater mapping is derived dynamically based on active G-code temperature signals:
  - When filament X is active and heater N is heated to printing temperature (>200°C), 
    we associate filament X with heater N.

================================================================================
EXECUTION & RUN RULES:
================================================================================
1. Single File Analysis:
   Generates a temperature profile layout for a single 3MF file.
   Usage:
     python3 show_temp_plot.py <path_to_file.3mf>

2. Comparison Mode (Two Files):
   Renders a side-by-side alignment of temperature panels for both files (e.g. OrcaSlicer vs BambuStudio).
   Usage:
     python3 show_temp_plot.py <path_to_file1.3mf> <path_to_file2.3mf>

Output:
- The script automatically outputs the compiled interactive HTML report to:
  ~/Desktop/temp_plot.html
- Opens the HTML report in the system's default web browser immediately.
"""
import sys
import os
import zipfile
import json
import xml.etree.ElementTree as ET
import webbrowser

sys.path.append(os.path.dirname(os.path.abspath(__file__)))
try:
    import compare_slices
except ImportError:
    sys.path.append("/Users/denn/Develop/3dprint/dehancer lab/H2C_v2/scripts")
    import compare_slices


def parse_filaments_and_colors(zip_file):
    """Parse filament colors and types from Metadata/slice_info.config inside 3MF."""
    filaments = {}
    try:
        xml_data = zip_file.read("Metadata/slice_info.config")
        root = ET.fromstring(xml_data)
        plate = root.find("plate")
        if plate is not None:
            for fil in plate.findall("filament"):
                fid = int(fil.attrib.get("id"))  # 1-based in XML
                color = fil.attrib.get("color")
                ftype = fil.attrib.get("type", "PLA")
                filaments[fid - 1] = {"color": color, "type": ftype}
    except Exception as e:
        print(f"Error parsing filaments: {e}")
    return filaments


def parse_nozzle_groups(zip_file):
    """
    Parse filament-to-extruder mapping from filament_maps in slice_info.config.
    
    filament_maps = "2 1 2 2 2" → {0: 2, 1: 1, 2: 2, 3: 2, 4: 2}
    
    Returns:
        extruder_map: {filament_id_0based: extruder_id}
        extruder_groups: {extruder_id: [list of filament IDs 0-based]}
    """
    extruder_map = {}
    
    try:
        xml_data = zip_file.read("Metadata/slice_info.config").decode("utf-8", errors="replace")
        root = ET.fromstring(xml_data)
        plate = root.find("plate")
        if plate is not None:
            for meta in plate.findall("metadata"):
                if meta.attrib.get("key") == "filament_maps":
                    raw = meta.attrib.get("value", "").strip()
                    if raw:
                        for i, v in enumerate(raw.split()):
                            extruder_map[i] = int(v)
                    break
    except Exception as e:
        print(f"  Warning: could not parse filament_maps: {e}")
    
    extruder_groups = {}
    for fid, eid in extruder_map.items():
        extruder_groups.setdefault(eid, []).append(fid)
    
    return extruder_map, extruder_groups


def determine_heater_to_extruder(track, extruder_map):
    # H2C physical mapping: Left (Extruder 1) -> Heater 1, Right (Extruder 2) -> Heater 0
    return {1: 1, 2: 0}


def build_timeline_and_interpolate(track, tool_changes, m73_points, total_lines, m400_weights=None,
                                    gcode_lines=None):
    """Build a line→time mapping from physical G1 motion time estimation.

    When *gcode_lines* is provided (list of raw gcode strings, 0-indexed),
    the timeline is computed from actual feedrates + M400 delays, giving a
    physically accurate time axis independent of M73 granularity.
    Falls back to M73 interpolation when gcode_lines is not available.
    """
    import re as _re
    import math as _math

    if m400_weights is None:
        m400_weights = {}

    # ── Physical timeline from G1 moves ──────────────────────────────────
    if gcode_lines is not None:
        cumulative = [0.0] * (len(gcode_lines) + 2)  # 1-indexed
        cur_x, cur_y, cur_z = 0.0, 0.0, 0.0
        cur_f = 1800.0  # mm/min default
        t = 0.0

        for i, raw in enumerate(gcode_lines):
            line_num = i + 1
            gl = raw.strip()

            # M400 S/P delays
            ms = _re.match(r'^M400\s+S([\d.]+)', gl)
            mp = _re.match(r'^M400\s+P([\d.]+)', gl)
            if ms:
                t += float(ms.group(1))
            elif mp:
                t += float(mp.group(1)) / 1000.0

            # G1 moves
            if gl.startswith('G1 '):
                fm = _re.search(r'F([\d.]+)', gl)
                if fm:
                    cur_f = float(fm.group(1))

                xm = _re.search(r'X([-\d.]+)', gl)
                ym = _re.search(r'Y([-\d.]+)', gl)
                zm = _re.search(r'Z([-\d.]+)', gl)

                nx = float(xm.group(1)) if xm else cur_x
                ny = float(ym.group(1)) if ym else cur_y
                nz = float(zm.group(1)) if zm else cur_z

                dist = _math.sqrt((nx - cur_x)**2 + (ny - cur_y)**2 + (nz - cur_z)**2)
                if dist > 0.001 and cur_f > 0:
                    t += dist / (cur_f / 60.0)  # F is mm/min → mm/s

                cur_x, cur_y, cur_z = nx, ny, nz

            if line_num < len(cumulative):
                cumulative[line_num] = t

        # Fill any remaining slots
        for j in range(line_num + 1, len(cumulative)):
            cumulative[j] = t

        total_duration = t

        # ── Scale physical timeline to match M73 trapezoid estimate ──────
        # Physical dist/speed ignores acceleration/deceleration, giving an
        # underestimated total (e.g. 29 min vs real 48 min).  M73 from the
        # trapezoid planner accounts for accel/decel and is closer to reality.
        # We scale only the G1 motion component; M400 delays are physical
        # waits and must not be inflated.
        if m73_points and total_duration > 0:
            m73_total_sec = max(p[1] for p in m73_points) * 60

            # Compute total M400 delay time
            m400_total = 0.0
            for raw in gcode_lines:
                gl = raw.strip()
                ms = _re.match(r'^M400\s+S([\d.]+)', gl)
                mp = _re.match(r'^M400\s+P([\d.]+)', gl)
                if ms:
                    m400_total += float(ms.group(1))
                elif mp:
                    m400_total += float(mp.group(1)) / 1000.0

            g1_time = total_duration - m400_total
            m73_g1_time = m73_total_sec - m400_total

            if g1_time > 0 and m73_g1_time > g1_time:
                scale = m73_g1_time / g1_time
                # Re-scale: for each line, separate M400-contributed time
                # from G1-contributed time, scale only G1 part.
                # Since M400 delays are sparse and cumulative is monotonic,
                # we rebuild by scaling the G1 increments.
                m400_lines = set()
                for i, raw in enumerate(gcode_lines):
                    gl = raw.strip()
                    if _re.match(r'^M400\s+[SP]', gl):
                        m400_lines.add(i + 1)  # 1-indexed

                prev = 0.0
                new_t = 0.0
                for j in range(1, len(cumulative)):
                    delta = cumulative[j] - prev
                    prev = cumulative[j]
                    if j in m400_lines:
                        new_t += delta  # M400: no scale
                    else:
                        new_t += delta * scale  # G1: scale
                    cumulative[j] = new_t

                total_duration = cumulative[-1]

        def get_time(line):
            idx = max(1, min(int(round(line)), len(cumulative) - 1))
            return cumulative[idx]

        for tr in track:
            tr["time"] = get_time(tr["line"])
        for tc in tool_changes:
            tc["time"] = get_time(tc["line"])

        return total_duration, get_time

    # ── Fallback: M73-based interpolation (original logic) ───────────────
    m73_points = sorted(list(set([(p[0], p[1]) for p in m73_points])))
    filtered_m73 = []
    seen_times = set()
    for line, r_val in m73_points:
        if r_val not in seen_times:
            filtered_m73.append((line, r_val))
            seen_times.add(r_val)
    m73_points = filtered_m73
    timeline = []
    if m73_points:
        max_R = m73_points[0][1]
        for _, r in m73_points[:5]:
            if r > max_R:
                max_R = r
        for line, r in m73_points:
            elapsed_sec = (max_R - r) * 60
            timeline.append((line, elapsed_sec))
        if timeline[0][0] > 1:
            timeline.insert(0, (1, 0))
        if timeline[-1][0] < total_lines:
            timeline.append((total_lines, max_R * 60))
    else:
        timeline = [(1, 0), (total_lines, total_lines * 0.02)]

    line_to_time = {}
    # Build continuous M620 ranges from sparse track samples.
    m620_ranges = []
    m620_start = None
    for t in track:
        if t.get("in_m620", False):
            if m620_start is None:
                m620_start = t["line"]
        else:
            if m620_start is not None:
                m620_ranges.append((m620_start, t["line"]))
                m620_start = None
    if m620_start is not None:
        m620_ranges.append((m620_start, track[-1]["line"] if track else m620_start))

    def is_in_m620(l):
        for rs, re_ in m620_ranges:
            if rs <= l <= re_:
                return True
        return False
    
    for k in range(len(timeline) - 1):
        line1, time1 = timeline[k]
        line2, time2 = timeline[k + 1]
        
        dT = time2 - time1
        dL = line2 - line1
        if dL <= 0:
            continue
            
        weights = []
        total_w = 0.0
        for l in range(line1, line2 + 1):
            is_tc = is_in_m620(l)
            w = 500.0 if is_tc else 1.0
            if l in m400_weights:
                w += m400_weights[l] * 50.0
            weights.append((l, w))
            total_w += w
            
        current_t = time1
        if total_w == 0:
            total_w = 1.0
            
        for l, w in weights:
            line_to_time[l] = current_t
            current_t += (w / total_w) * dT

    def get_time(line):
        l_round = int(round(line))
        if l_round in line_to_time:
            return line_to_time[l_round]
            
        if line <= timeline[0][0]:
            return timeline[0][1]
        if line >= timeline[-1][0]:
            return timeline[-1][1]
        for k in range(len(timeline) - 1):
            pt1 = timeline[k]
            pt2 = timeline[k + 1]
            if pt1[0] <= line <= pt2[0]:
                if pt2[0] == pt1[0]:
                    return pt1[1]
                return pt1[1] + (line - pt1[0]) / (pt2[0] - pt1[0]) * (pt2[1] - pt1[1])
        return 0

    for t in track:
        t["time"] = get_time(t["line"])
    for tc in tool_changes:
        tc["time"] = get_time(tc["line"])

    total_duration = timeline[-1][1]
    return total_duration, get_time


def parse_file_data(filepath):
    if not filepath or not os.path.exists(filepath):
        return None
    try:
        with zipfile.ZipFile(filepath) as z:
            filaments = parse_filaments_and_colors(z)
            extruder_map, extruder_groups = parse_nozzle_groups(z)
            _, total_lines, _, stats = compare_slices.parse_critical_gcode(z)
            track_raw = stats["temp_track"]
            m73_points = stats.get("m73_points", [])

            # Find the start of machine end gcode to cut off non-printing trailing commands (e.g. air filtration wait)
            end_gcode_line = total_lines
            raw_gcode_lines = None
            for name in z.namelist():
                if name.endswith('.gcode'):
                    gcode_text = z.read(name).decode('utf-8', errors='replace')
                    gcode_lines = gcode_text.split('\n')
                    for i in range(len(gcode_lines) - 1, -1, -1):
                        gl = gcode_lines[i]
                        line_num = i + 1
                        if ';' in gl and '=' not in gl:
                            if 'MACHINE_END_GCODE_START' in gl or 'filament end gcode' in gl or 'machine: H2C end' in gl:
                                end_gcode_line = line_num
                                break
                    raw_gcode_lines = gcode_lines
                    break

            # Keep end_gcode_line but do not trim data arrays to preserve full time duration
            pass

            # Parse TC, Wipe Tower, Toolchange zones and M400 weights from raw gcode
            tc_zones_raw = []  # list of (start_line, end_line)
            wipe_zones_raw = []  # list of (start_line, end_line)
            toolchange_zones_raw = []  # list of (start_line, end_line)
            m400_weights = {}
            for name in z.namelist():
                if name.endswith('.gcode'):
                    gcode_text = z.read(name).decode('utf-8', errors='replace')
                    gcode_lines = gcode_text.split('\n')
                    nc_start = None
                    wipe_start = None
                    tc_block_start = None
                    for i, gl in enumerate(gcode_lines):
                        line_num = i + 1
                        # Parse M400 delay commands
                        if 'M400' in gl:
                            import re
                            m_s = re.search(r'M400\s+S(\d+)', gl)
                            if m_s:
                                m400_weights[line_num] = float(m_s.group(1))
                            else:
                                m_p = re.search(r'M400\s+P(\d+)', gl)
                                if m_p:
                                    m400_weights[line_num] = float(m_p.group(1)) * 0.001
                        # Match both Orca and BBS toolchange start comments
                        if 'CP TOOLCHANGE START' in gl:
                            tc_block_start = line_num
                        elif 'CP TOOLCHANGE END' in gl:
                            if tc_block_start is not None:
                                toolchange_zones_raw.append((tc_block_start, line_num))
                                tc_block_start = None

                        # Match BBS (; NOZZLE_CHANGE_START) and Orca (; Nozzle change start/end)
                        if 'NOZZLE_CHANGE_START' in gl or 'Nozzle change start' in gl:
                            nc_start = line_num
                        elif 'NOZZLE_CHANGE_END' in gl or 'Nozzle change end' in gl:
                            if nc_start is not None:
                                tc_zones_raw.append((nc_start, line_num))
                                nc_start = None

                        # Fallback: detect M632 M N / M633 as carousel nozzle change zones
                        # (Orca doesn't always emit ; Nozzle change start/end around M632/M633)
                        if gl.startswith('M632') and ' M ' in gl and nc_start is None:
                            nc_start = line_num
                        elif gl.startswith('M633') and nc_start is not None:
                            tc_zones_raw.append((nc_start, line_num))
                            nc_start = None
                        # Match both Orca (; CP TOOLCHANGE WIPE) and BBS (; CP_TOOLCHANGE_WIPE)
                        elif 'CP TOOLCHANGE WIPE' in gl or 'CP_TOOLCHANGE_WIPE' in gl:
                            wipe_start = line_num
                        elif '; CP TOOLCHANGE END' in gl:
                            if wipe_start is not None:
                                wipe_zones_raw.append((wipe_start, line_num))
                                wipe_start = None
                    break

        # Extract preheat events in raw G-code lines format using dynamic nozzle_map
        filament_maps_str = " ".join([str(extruder_map[i]) for i in sorted(extruder_map.keys())])
        nozzle_map_for_preheat = compare_slices.get_nozzle_map(filament_maps_str, track_raw)
        raw_preheats = compare_slices.analyze_preheat_cooldown_events(track_raw, nozzle_map_for_preheat)

        track = []
        tool_changes = []
        current_tool = 0
        for line, active_ext, t0, t1, desc, in_m620_block, printing_started in track_raw:
            if desc.startswith("T") and "Active Filament" in desc:
                parts = desc.split()
                t_name = parts[0]
                if t_name[1:].isdigit():
                    t_val = int(t_name[1:])
                    if t_val < 60000:
                        current_tool = t_val

            track.append({
                "line": line,
                "active": current_tool,
                "t0": t0,
                "t1": t1,
                "desc": desc,
                "in_m620": in_m620_block,
                "printing_started": printing_started
            })
            if desc.startswith("T") and "Active Filament" in desc:
                parts = desc.split()
                t_name = parts[0]
                t_idx = -1
                if t_name[1:].isdigit():
                    t_idx = int(t_name[1:])
                tool_changes.append({
                    "line": line,
                    "name": t_name,
                    "idx": t_idx,
                    "desc": desc,
                })

        total_duration, get_time = build_timeline_and_interpolate(track, tool_changes, m73_points, total_lines, m400_weights,
                                                                    gcode_lines=raw_gcode_lines)
        end_gcode_time = get_time(end_gcode_line)
        
        # Convert preheat lines to time (seconds)
        preheats = []
        for ev in raw_preheats:
            if ev["preheat_line"] is not None and ev["tc_line"] is not None:
                start_t = get_time(ev["preheat_line"])
                end_t = get_time(ev["tc_line"])
                preheats.append({
                    "start_time": start_t,
                    "end_time": end_t,
                    "heater": ev["target_ext"],  # 0 or 1
                    "target_temp": ev["preheat_temp"],
                    "nozzle_num": ev["nozzle_num"]
                })

        # Convert precool (cooldown) lines to time (seconds)
        # Precool = M104 S<low_temp> on the DEPARTING nozzle before tool change
        # Filter: skip S0 (heater off), negative durations, and very short zones (<2s)
        precools = []
        for ev in raw_preheats:
            if ev.get("cooldown_line") is not None and ev["tc_line"] is not None and ev.get("cooldown_temp") is not None:
                if ev["cooldown_temp"] <= 0:  # S0 = heater off, not a real precool
                    continue
                start_t = get_time(ev["cooldown_line"])
                end_t = get_time(ev["tc_line"])
                dur = end_t - start_t
                if dur < 2.0:  # Too short or negative — not a visible precool zone
                    continue
                # source_ext = the extruder that is LEAVING (opposite of target_ext)
                source_heater = 1 - ev["target_ext"] if ev["target_ext"] in (0, 1) else 0
                precools.append({
                    "start_time": start_t,
                    "end_time": end_t,
                    "heater": source_heater,
                    "target_temp": ev["cooldown_temp"],
                    "nozzle_num": ev.get("nozzle_num", -1)
                })
        
        heater_to_ext = determine_heater_to_extruder(track, extruder_map)
        
        nozzle_map = {}
        for fid, ext_id in extruder_map.items():
            nozzle_map[fid] = heater_to_ext.get(ext_id, 0)

        # Convert TC/Wipe zones to time coordinates.
        # M73 has 1-minute resolution, so TC zones (which last ~20-30s) often map
        # to identical times. When that happens, estimate width from line count ratio.
        def zone_to_time(start_line, end_line, min_dur=8.0):
            t_start = get_time(start_line)
            t_end = get_time(end_line)
            actual_dur = t_end - t_start
            if actual_dur < min_dur:
                # M73 has 1-minute resolution and BBS TC is only ~7 lines,
                # so interpolated duration can be <1s. Use line-based estimate.
                estimated_dur = max(min_dur, (end_line - start_line) * 0.15)
                t_end = t_start + estimated_dur
            return {"start_time": t_start, "end_time": t_end}

        # Sequence nozzle changes (tc_zones) and wipe tower blocks (wipe_zones) sequentially
        # to prevent visual overlaps caused by artificial duration extension.
        all_sub_zones = []
        for s, e in tc_zones_raw:
            all_sub_zones.append({"type": "nc", "start_line": s, "end_line": e})
        for s, e in wipe_zones_raw:
            all_sub_zones.append({"type": "wipe", "start_line": s, "end_line": e})
            
        all_sub_zones.sort(key=lambda z: z["start_line"])
        
        tc_zones = []
        wipe_zones = []
        prev_end_time = -1.0
        min_dur = 8.0
        
        for zone in all_sub_zones:
            s_line = zone["start_line"]
            e_line = zone["end_line"]
            t_start = get_time(s_line)
            t_end = get_time(e_line)
            
            actual_dur = t_end - t_start
            dur = actual_dur
            if actual_dur < min_dur:
                dur = max(min_dur, (e_line - s_line) * 0.15)
                
            if t_start < prev_end_time:
                t_start = prev_end_time
                
            t_end = t_start + dur
            prev_end_time = t_end
            
            formatted_zone = {"start_time": t_start, "end_time": t_end}
            if zone["type"] == "nc":
                tc_zones.append(formatted_zone)
            else:
                wipe_zones.append(formatted_zone)
                
        toolchange_zones = [zone_to_time(s, e) for s, e in toolchange_zones_raw]


        slicer_name = "OrcaSlicer" if "orca" in os.path.basename(filepath).lower() else (
            "BambuStudio" if "bbl" in os.path.basename(filepath).lower() else "Slicer"
        )

        print(f"  Extruder groups: {extruder_groups}")
        print(f"  Heater→Extruder: {heater_to_ext}")
        for fid, heater in sorted(nozzle_map.items()):
            ext_id = extruder_map.get(fid, "?")
            fil_info = filaments.get(fid, {})
            print(f"    T{fid} ({fil_info.get('type','?')} {fil_info.get('color','?')}) → Extruder {ext_id} → Heater {heater}")

        cooldown_count = sum(1 for ev in raw_preheats if ev.get("cooldown_temp") is not None)

        return {
            "filename": os.path.basename(filepath),
            "slicer": slicer_name,
            "total_lines": total_lines,
            "total_duration": total_duration,
            "end_gcode_time": end_gcode_time,
            "filaments": filaments,
            "nozzle_map": nozzle_map,
            "extruder_groups": extruder_groups,
            "heater_to_ext": heater_to_ext,
            "tool_changes": tool_changes,
            "preheats": preheats,
            "precools": precools,
            "preheat_count": len(preheats),
            "cooldown_count": cooldown_count,
            "tc_zones": tc_zones,
            "wipe_zones": wipe_zones,
            "toolchange_zones": toolchange_zones,
            "track": track,
        }
    except Exception as e:
        import traceback
        print(f"Error parsing {filepath}: {e}")
        traceback.print_exc()
        return None


def find_desktop_3mf_files():
    desktop = os.path.expanduser("~/Desktop")
    files = [os.path.join(desktop, f) for f in os.listdir(desktop) if f.endswith(".3mf")]
    files = sorted(files, key=os.path.getmtime, reverse=True)
    return files


def main():
    file1_path = None
    file2_path = None

    if len(sys.argv) > 1:
        file1_path = sys.argv[1]
    if len(sys.argv) > 2:
        file2_path = sys.argv[2]

    if not file1_path:
        desktop_files = find_desktop_3mf_files()
        if len(desktop_files) >= 2:
            orcas = [f for f in desktop_files if "orca" in os.path.basename(f).lower()]
            bbls = [f for f in desktop_files if "bbl" in os.path.basename(f).lower() or "bambu" in os.path.basename(f).lower()]
            if orcas and bbls:
                file1_path = orcas[0]
                file2_path = bbls[0]
            else:
                file1_path = desktop_files[0]
                file2_path = desktop_files[1]
        elif len(desktop_files) == 1:
            file1_path = desktop_files[0]

    if not file1_path:
        print("Usage: python3 show_temp_plot.py <file1.3mf> [file2.3mf]")
        sys.exit(1)

    print(f"Loading File 1: {file1_path}")
    f1_data = parse_file_data(file1_path)

    f2_data = None
    if file2_path:
        print(f"Loading File 2: {file2_path}")
        f2_data = parse_file_data(file2_path)

    if not f1_data:
        print("Error: Failed to parse first file.")
        sys.exit(1)

    js_data = {
        "is_comparison": f2_data is not None,
        "file1": f1_data,
        "file2": f2_data,
        "total_duration": max(f1_data["total_duration"], f2_data["total_duration"]) if f2_data else f1_data["total_duration"],
    }

    html_content = HTML_TEMPLATE.replace("%DATA_JSON%", json.dumps(js_data))

    output_html = os.path.join(os.path.dirname(file1_path), "temp_plot_v3.html")
    with open(output_html, "w", encoding="utf-8") as f:
        f.write(html_content)

    print(f"Interactive HTML report: {output_html}")
    webbrowser.open("file://" + os.path.abspath(output_html))


# H2C hardware fact: heater 0 = RIGHT nozzle, heater 1 = LEFT nozzle
# Panel layout: top panels show heater 0 (RIGHT), bottom panels show heater 1 (LEFT)
HTML_TEMPLATE = r"""<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <title>H2C Temperature Timeline</title>
    <style>
        body {
            margin: 0; padding: 0;
            background-color: #18181b; color: #f4f4f5;
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
            overflow: hidden;
        }
        #header {
            background-color: #27272a; padding: 10px 20px;
            display: flex; align-items: center; justify-content: space-between;
            border-bottom: 1px solid #3f3f46; height: 50px;
        }
        #title { font-size: 15px; font-weight: bold; max-width: 250px; line-height: 1.2; }
        #info-panel {
            font-size: 12px; color: #a1a1aa; flex-grow: 1; margin-left: 20px;
            font-family: monospace; white-space: pre-line; line-height: 1.3;
        }
        #instructions { font-size: 11px; color: #71717a; text-align: right; line-height: 1.3; }
        #canvas-container { width: 100vw; height: calc(100vh - 75px); position: relative; }
        canvas { display: block; width: 100%; height: 100%; }
    </style>
</head>
<body>
    <div id="header">
        <div id="title">H2C Temperature Timeline</div>
        <div id="info-panel">Hover over graph for synchronized temperature comparison</div>
        <div id="instructions">Scroll: Zoom | Drag: Pan | Double-Click: Reset</div>
    </div>
    <div id="canvas-container"><canvas id="plotCanvas"></canvas></div>
<script>
const data = %DATA_JSON%;
// Get extruder label: "Ext. N (M fil.)"
function getExtLabel(fileData, extId) {
    const fils = (fileData.extruder_groups || {})[String(extId)] || [];
    return "Ext. " + extId + " (" + fils.length + " fil.)";
}

// Get heater index for a given extruder ID
function getHeaterForExt(fileData, extId) {
    if (!fileData.heater_to_ext) return 0;
    return fileData.heater_to_ext[String(extId)] ?? 0;
}

// Get sorted extruder IDs for a file
function getExtruderIds(fileData) {
    return Object.keys(fileData.extruder_groups || {}).map(Number).sort();
}

// Invert heater_to_ext: heater index → extruder ID
function getExtIdForHeater(fileData, heater) {
    if (!fileData.heater_to_ext) return heater;
    for (const [extId, h] of Object.entries(fileData.heater_to_ext)) {
        if (h === heater) return parseInt(extId);
    }
    return heater;
}

// nozzle_map: {filament_id → 0 or 1} (heater index)
function getHeater(fileData, filamentId) {
    if (!fileData.nozzle_map) return 0;
    let fid = filamentId;
    if (fid >= 1000) fid -= 1000;
    const key = String(fid);
    if (key in fileData.nozzle_map) return fileData.nozzle_map[key];
    return 0;
}

function isActiveOnPanel(fileData, tool, panelHeater) {
    return getHeater(fileData, tool) === panelHeater;
}

function inNozzleChange(fileData, time) {
    if (!fileData.tc_zones) return false;
    return fileData.tc_zones.some(z => time >= z.start_time && time <= z.end_time);
}

function inToolchangeZone(fileData, time) {
    if (!fileData.toolchange_zones) return false;
    return fileData.toolchange_zones.some(z => time >= z.start_time && time <= z.end_time);
}

function inWipeZone(fileData, time) {
    if (!fileData.wipe_zones) return false;
    return fileData.wipe_zones.some(z => time >= z.start_time && time <= z.end_time);
}

const titleEl = document.getElementById("title");
if (data.is_comparison) {
    titleEl.innerText = "H2C Comparison:\n1. " + data.file1.filename + "\n2. " + data.file2.filename;
} else {
    titleEl.innerText = "H2C Temperatures:\n" + data.file1.filename;
}

const canvas = document.getElementById("plotCanvas");
const ctx = canvas.getContext("2d");

let width, height, scaleXVal = 1.0, offsetX = 0;
let isDragging = false, startDragX = 0, startOffsetX = 0;
let mouseX = -1, mouseY = -1;

const marginLeft = 170, marginRight = 220, topYStart = 60;
let graphH = 200;
let panels = [];

function getVortekFilaments(fileData) {
    // Vortek nozzles = all filaments on Heater 0 (shared swappable nozzle block)
    return Object.keys(fileData.filaments).map(Number).sort()
        .filter(fid => getHeater(fileData, fid) === 0);
}

function calculateLayout() {
    width = window.innerWidth;
    height = window.innerHeight - 75;
    const dpr = window.devicePixelRatio || 1;
    canvas.width = width * dpr;
    canvas.height = height * dpr;
    ctx.scale(dpr, dpr);

    const panelColors = ["#3b82f6", "#ec4899", "#60a5fa", "#f472b6"];
    const NOZZLE_H = 52;   // height of mini nozzle-track panels
    const MAIN_GAP = 18;   // gap between main extruder panels
    const NOZZLE_GAP = 8;  // gap between mini nozzle panels
    const SECTION_GAP = 28; // gap between main section and nozzle section
    panels = [];

    const files = [data.file1, data.file2].filter(Boolean);
    const allExts = files.flatMap(fd => getExtruderIds(fd).map(extId => ({ fd, extId })));
    const allVortek = files.flatMap(fd => getVortekFilaments(fd).map(fid => ({ fd, fid })));

    const nMain = allExts.length;
    const nNozzle = allVortek.length;

    // Height budget: main panels take remaining space after nozzle panels + gaps
    const nozzleBudget = nNozzle * NOZZLE_H + Math.max(0, nNozzle - 1) * NOZZLE_GAP + SECTION_GAP;
    const mainBudget = height - topYStart - nozzleBudget - Math.max(0, nMain - 1) * MAIN_GAP - 30;
    graphH = Math.max(70, Math.floor(mainBudget / Math.max(1, nMain)));

    let y = topYStart;

    // Main extruder panels
    allExts.forEach(({ fd, extId }, i) => {
        panels.push({
            type: 'main',
            yStart: y,
            height: graphH,
            heater: getHeaterForExt(fd, extId),
            extId,
            file: fd,
            color: panelColors[i % 4]
        });
        y += graphH + MAIN_GAP;
    });

    // Section divider then mini nozzle panels
    y += SECTION_GAP - MAIN_GAP;
    allVortek.forEach(({ fd, fid }, i) => {
        const filColor = getFilColor(fd, fid);
        const filInfo = fd.filaments[fid] || {};
        panels.push({
            type: 'nozzle',
            yStart: y,
            height: NOZZLE_H,
            heater: getHeater(fd, fid),
            filId: fid,
            file: fd,
            color: filColor,
            label: fd.slicer + " T" + fid + (filInfo.type ? " " + filInfo.type : "")
        });
        y += NOZZLE_H + NOZZLE_GAP;
    });

    // Pre-classify toolchange markers
    [data.file1, data.file2].forEach(fileData => {
        if (!fileData) return;
        const filtered = [];
        fileData.tool_changes.forEach(tc => {
            const fid = tc.idx;
            if (fid >= 1000) return;
            tc.label = fmtTool(fid) + " " + formatTime(tc.time);
            filtered.push(tc);
        });
        fileData.tool_changes = filtered;
    });
}

window.addEventListener("resize", () => { calculateLayout(); draw(); });

function formatTime(s) {
    if (s < 0) s = 0;
    const h = Math.floor(s/3600), m = Math.floor((s%3600)/60), sec = Math.floor(s%60);
    if (h > 0) return h + ":" + (m<10?"0":"") + m + ":" + (sec<10?"0":"") + sec;
    return m + ":" + (sec<10?"0":"") + sec;
}

function draw() {
    ctx.clearRect(0, 0, width, height);
    const visibleWidth = width - marginLeft - marginRight;
    const plotW = visibleWidth * scaleXVal;
    offsetX = Math.max(0, Math.min(offsetX, Math.max(0, plotW - visibleWidth)));

    const scaleX = t => marginLeft + (t / data.total_duration) * plotW - offsetX;
    
    // Calculate dynamic visibility of toolchange markers to prevent overlapping
    [data.file1, data.file2].forEach(fileData => {
        if (!fileData) return;
        let last_drawn_x = -9999;
        const min_text_gap = 72; // pixels gap to prevent overlap
        fileData.tool_changes.forEach(tc => {
            const x = scaleX(tc.time);
            if (x >= marginLeft && x <= marginLeft + visibleWidth) {
                if (x - last_drawn_x < min_text_gap) {
                    tc.hidden_dynamically = true;
                } else {
                    tc.hidden_dynamically = false;
                    last_drawn_x = x;
                }
            } else {
                tc.hidden_dynamically = true;
            }
        });
    });

    // scaleY now takes per-panel height
    const scaleY = (temp, yS, pH) => yS + pH - (temp / 250) * pH;
    const tempLevels = [0, 50, 100, 140, 180, 220, 250];
    const nozzleTempLevels = [0, 100, 180, 220];

    const lastPanel = panels[panels.length - 1];
    const bottomY = lastPanel ? lastPanel.yStart + lastPanel.height : height;

    // Draw section divider label before nozzle panels
    const firstNozzlePanel = panels.find(p => p.type === 'nozzle');
    if (firstNozzlePanel) {
        const divY = firstNozzlePanel.yStart - 16;
        ctx.fillStyle = "#52525b"; ctx.font = "10px system-ui"; ctx.textAlign = "left";
        ctx.fillText("▾ Vortek nozzle tracks", marginLeft, divY);
        ctx.strokeStyle = "#3f3f46"; ctx.lineWidth = 1; ctx.setLineDash([4, 4]);
        ctx.beginPath(); ctx.moveTo(marginLeft + 130, divY - 3); ctx.lineTo(marginLeft + visibleWidth, divY - 3); ctx.stroke();
        ctx.setLineDash([]);
    }

    // Draw all panels
    panels.forEach((p, pIdx) => {
        const pH = p.height;
        const isNozzle = p.type === 'nozzle';
        const levels = isNozzle ? nozzleTempLevels : tempLevels;

        ctx.fillStyle = isNozzle ? "#0e0e10" : "#111113";
        ctx.fillRect(marginLeft, p.yStart, visibleWidth, pH);
        ctx.strokeStyle = isNozzle ? "#2d2d30" : "#27272a";
        ctx.lineWidth = isNozzle ? 1 : 1.5;
        ctx.strokeRect(marginLeft, p.yStart, visibleWidth, pH);

        // End G-code zone (end_gcode_time → total_duration) — dark grey / hatched
        if (!isNozzle && p.file.end_gcode_time) {
            const xS = scaleX(p.file.end_gcode_time), xE = scaleX(p.file.total_duration);
            const l = Math.max(marginLeft, xS), r = Math.min(marginLeft + visibleWidth, xE);
            if (l < r) {
                ctx.fillStyle = "rgba(75, 85, 99, 0.15)";
                ctx.fillRect(l, p.yStart, r - l, pH);
                
                ctx.strokeStyle = "rgba(156, 163, 175, 0.5)";
                ctx.lineWidth = 1;
                ctx.setLineDash([4, 4]);
                ctx.beginPath();
                ctx.moveTo(l, p.yStart);
                ctx.lineTo(l, p.yStart + pH);
                ctx.stroke();
                ctx.setLineDash([]);
                
                ctx.fillStyle = "#9ca3af";
                ctx.font = "9px sans-serif";
                ctx.fillText("End G-code", l + 6, p.yStart + 12);
            }
        }

        // TC zones (NOZZLE_CHANGE_START → END) — purple, deepest background layer
        if (!isNozzle && p.file.tc_zones) {
            p.file.tc_zones.forEach(z => {
                const xS = scaleX(z.start_time), xE = scaleX(z.end_time);
                const l = Math.max(marginLeft, xS), r = Math.min(marginLeft + visibleWidth, xE);
                if (l < r) {
                    ctx.fillStyle = "rgba(147, 51, 234, 0.15)";
                    ctx.fillRect(l, p.yStart, r - l, pH);
                    ctx.strokeStyle = "rgba(147, 51, 234, 0.4)";
                    ctx.lineWidth = 1; ctx.setLineDash([2, 2]);
                    ctx.beginPath();
                    ctx.moveTo(l, p.yStart); ctx.lineTo(l, p.yStart + pH);
                    ctx.moveTo(r, p.yStart); ctx.lineTo(r, p.yStart + pH);
                    ctx.stroke(); ctx.setLineDash([]);
                }
            });
        }

        // Wipe Tower zones (CP TOOLCHANGE WIPE → END) — cyan
        if (!isNozzle && p.file.wipe_zones) {
            p.file.wipe_zones.forEach(z => {
                const xS = scaleX(z.start_time), xE = scaleX(z.end_time);
                const l = Math.max(marginLeft, xS), r = Math.min(marginLeft + visibleWidth, xE);
                if (l < r) {
                    ctx.fillStyle = "rgba(6, 182, 212, 0.12)";
                    ctx.fillRect(l, p.yStart, r - l, pH);
                    ctx.strokeStyle = "rgba(6, 182, 212, 0.35)";
                    ctx.lineWidth = 1; ctx.setLineDash([2, 2]);
                    ctx.beginPath();
                    ctx.moveTo(l, p.yStart); ctx.lineTo(l, p.yStart + pH);
                    ctx.moveTo(r, p.yStart); ctx.lineTo(r, p.yStart + pH);
                    ctx.stroke(); ctx.setLineDash([]);
                }
            });
        }

        // Preheat regions (only on main panels)
        if (!isNozzle && p.file.preheats) {
            p.file.preheats.forEach(ph => {
                if (ph.heater === p.heater) {
                    const xStart = scaleX(ph.start_time);
                    const xEnd = scaleX(ph.end_time);
                    const left = Math.max(marginLeft, xStart);
                    const right = Math.min(marginLeft + visibleWidth, xEnd);
                    if (left < right) {
                        ctx.fillStyle = "rgba(220, 38, 38, 0.15)";
                        ctx.fillRect(left, p.yStart, right - left, pH);
                        ctx.strokeStyle = "rgba(220, 38, 38, 0.45)";
                        ctx.lineWidth = 1; ctx.setLineDash([3, 3]);
                        ctx.beginPath();
                        if (xStart >= marginLeft && xStart <= marginLeft + visibleWidth) {
                            ctx.moveTo(xStart, p.yStart); ctx.lineTo(xStart, p.yStart + pH);
                        }
                        ctx.stroke(); ctx.setLineDash([]);
                    }
                }
            });
        }

        // Precool (cooldown) regions (only on main panels)
        if (!isNozzle && p.file.precools) {
            p.file.precools.forEach(pc => {
                if (pc.heater === p.heater) {
                    const xStart = scaleX(pc.start_time);
                    const xEnd = scaleX(pc.end_time);
                    const left = Math.max(marginLeft, xStart);
                    const right = Math.min(marginLeft + visibleWidth, xEnd);
                    if (left < right) {
                        ctx.fillStyle = "rgba(132, 204, 22, 0.15)";
                        ctx.fillRect(left, p.yStart, right - left, pH);
                        ctx.strokeStyle = "rgba(132, 204, 22, 0.45)";
                        ctx.lineWidth = 1; ctx.setLineDash([3, 3]);
                        ctx.beginPath();
                        if (xStart >= marginLeft && xStart <= marginLeft + visibleWidth) {
                            ctx.moveTo(xStart, p.yStart); ctx.lineTo(xStart, p.yStart + pH);
                        }
                        ctx.stroke(); ctx.setLineDash([]);
                    }
                }
            });
        }

        // Label
        ctx.fillStyle = isNozzle ? "#71717a" : "#e4e4e7";
        ctx.font = isNozzle ? "10px system-ui" : "bold 11px system-ui, sans-serif";
        ctx.textAlign = "left";
        const label = isNozzle ? p.label : (p.file.slicer + " Ext " + p.extId);
        ctx.fillText(label, 15, p.yStart + pH / 2 + 4);

        // Temp grid lines
        levels.forEach(t => {
            const y = scaleY(t, p.yStart, pH);
            if (y < p.yStart || y > p.yStart + pH) return;
            ctx.strokeStyle = "#27272a"; ctx.lineWidth = 1;
            ctx.beginPath(); ctx.setLineDash([4, 4]); ctx.moveTo(marginLeft, y); ctx.lineTo(marginLeft + visibleWidth, y); ctx.stroke(); ctx.setLineDash([]);
            ctx.fillStyle = "#52525b"; ctx.font = "9px system-ui"; ctx.textAlign = "right";
            ctx.fillText(t + "°", marginLeft - 5, y + 3);
        });

        // Time grid (only bottom-most panel gets labels)
        for (let i = 0; i <= 6; i++) {
            const tv = (i / 6) * data.total_duration, x = scaleX(tv);
            if (x >= marginLeft && x <= marginLeft + visibleWidth) {
                ctx.strokeStyle = "#27272a"; ctx.lineWidth = 1;
                ctx.beginPath(); ctx.moveTo(x, p.yStart); ctx.lineTo(x, p.yStart + pH); ctx.stroke();
                if (pIdx === panels.length - 1) {
                    ctx.fillStyle = "#71717a"; ctx.font = "11px system-ui"; ctx.textAlign = "center";
                    ctx.fillText(formatTime(tv), x, p.yStart + pH + 16);
                }
            }
        }
    });

    // Toolchange markers (main panels only)
    panels.filter(p => p.type === 'main').forEach(p => {
        const pH = p.height;
        p.file.tool_changes.forEach(tc => {
            if (tc.hidden_dynamically) return;
            const x = scaleX(tc.time);
            if (x >= marginLeft && x <= marginLeft + visibleWidth) {
                ctx.strokeStyle = "#ffffff"; ctx.lineWidth = 1.8;
                ctx.beginPath(); ctx.setLineDash([12, 3, 3, 3]);
                ctx.moveTo(x, p.yStart); ctx.lineTo(x, p.yStart + pH); ctx.stroke(); ctx.setLineDash([]);
                ctx.fillStyle = "#ffffff"; ctx.font = "bold 9.5px system-ui"; ctx.textAlign = "center";
                ctx.fillText(tc.label, x, p.yStart - 5);
            }
        });
    });

    // Temperature lines
    panels.forEach(p => {
        if (p.type === 'nozzle') {
            drawNozzlePanel(p, scaleX, scaleY, visibleWidth);
        } else {
            drawTempLine(p.yStart, p.height, p.heater, p.file, scaleX, scaleY, visibleWidth);
        }
    });

    // Crosshair markers in nozzle panels too
    panels.filter(p => p.type === 'nozzle').forEach(p => {
        const pH = p.height;
        p.file.tool_changes.forEach(tc => {
            if (tc.hidden_dynamically) return;
            const x = scaleX(tc.time);
            if (x >= marginLeft && x <= marginLeft + visibleWidth) {
                ctx.strokeStyle = "rgba(255,255,255,0.15)"; ctx.lineWidth = 1;
                ctx.beginPath(); ctx.setLineDash([4, 4]);
                ctx.moveTo(x, p.yStart); ctx.lineTo(x, p.yStart + pH); ctx.stroke(); ctx.setLineDash([]);
            }
        });
    });

    drawLegend(visibleWidth);
    drawTooltips(scaleX, scaleY, visibleWidth, plotW);
}

// --- Main extruder panel temperature line ---
// Shows single heater temp, colored by active filament.
function drawTempLine(yStart, pH, heater, fileData, scaleX, scaleY, visibleWidth) {
    ctx.lineWidth = 2.5; ctx.lineCap = "square"; ctx.lineJoin = "miter";
    const lB = marginLeft, rB = marginLeft + visibleWidth;

    for (let i = 0; i < fileData.track.length - 1; i++) {
        const pt1 = fileData.track[i], pt2 = fileData.track[i+1];
        const temp1 = heater === 0 ? pt1.t0 : pt1.t1;
        const temp2 = heater === 0 ? pt2.t0 : pt2.t1;
        const x1 = scaleX(pt1.time), y1 = scaleY(temp1, yStart, pH);
        const x2 = scaleX(pt2.time), y2 = scaleY(temp2, yStart, pH);
        if (x2 < lB || x1 > rB) continue;

        const in_tc1 = inToolchangeZone(fileData, pt1.time);
        const in_wipe1 = inWipeZone(fileData, pt1.time);
        const is_printing1 = !in_tc1 || in_wipe1;
        const is_end_gcode1 = fileData.end_gcode_time && pt1.time >= fileData.end_gcode_time;
        const active = isActiveOnPanel(fileData, pt1.active, heater) && is_printing1 && pt1.printing_started && !is_end_gcode1;
        let color = "#4b5563";
        if (active && temp1 >= 190) color = getFilColor(fileData, pt1.active);

        ctx.strokeStyle = color; ctx.beginPath();
        if (!active) {
            ctx.setLineDash([6, 4]);
            ctx.lineWidth = 1.0;
        } else {
            ctx.setLineDash([]);
            ctx.lineWidth = 2.5;
        }
        let hx1 = Math.max(x1, lB), hx2 = Math.min(x2, rB);
        if (hx1 <= hx2) { ctx.moveTo(hx1, y1); ctx.lineTo(hx2, y1); ctx.stroke(); }
        ctx.setLineDash([]);

        if (x2 >= lB && x2 <= rB) {
            const in_tc2 = inToolchangeZone(fileData, pt2.time);
            const in_wipe2 = inWipeZone(fileData, pt2.time);
            const is_printing2 = !in_tc2 || in_wipe2;
            const is_end_gcode2 = fileData.end_gcode_time && pt2.time >= fileData.end_gcode_time;
            const nextActive = isActiveOnPanel(fileData, pt2.active, heater) && is_printing2 && pt2.printing_started && !is_end_gcode2;
            let nc = "#4b5563";
            if (nextActive && temp2 >= 190) nc = getFilColor(fileData, pt2.active);
            ctx.strokeStyle = (temp2 > temp1) ? nc : color;
            ctx.lineWidth = nextActive ? 2.5 : 1.0;
            ctx.setLineDash(nextActive ? [] : [6, 4]);
            ctx.beginPath(); ctx.moveTo(x2, y1); ctx.lineTo(x2, y2); ctx.stroke();
            ctx.setLineDash([]);
        }
    }
}

// --- Mini nozzle panel: single filament track ---
// Shows:
//   - Active period: thick colored line (this nozzle is in heater, printing)
//   - Preheat/cooldown transitions: thin gray (temp CHANGING = real thermal event)
//   - Flat high-temp while OTHER nozzle prints: SKIPPED (not this nozzle's data)
//   - Background: thin 25°C baseline
//
// Key insight: T0/T2/T3/T4 share Heater 0. When another nozzle is printing at 220°C,
// temp_T0 is STABLE (no change). When preheat or cooldown happens, temp_T0 is CHANGING.
// We use this to distinguish "our" thermal events from "other nozzle printing".
//
// key threshold changed from 150 to 190.
function drawNozzlePanel(panel, scaleX, scaleY, visibleWidth) {
    const { filId, file: fileData, yStart, height: pH } = panel;
    const filHeater = getHeater(fileData, filId);
    const filColor = getFilColor(fileData, filId);
    const lB = marginLeft, rB = marginLeft + visibleWidth;
    const STEPS = 4; // track index steps before/after active period to capture preheat/cooldown
    ctx.lineCap = "square"; ctx.lineJoin = "miter";

    const track = fileData.track;

    // Thin standby baseline at 25°C
    const yStandby = scaleY(25, yStart, pH);
    ctx.strokeStyle = "#2d2d30"; ctx.lineWidth = 1; ctx.setLineDash([]);
    ctx.beginPath(); ctx.moveTo(lB, yStandby); ctx.lineTo(rB, yStandby); ctx.stroke();

    // 1. Find active index ranges for this filament
    const activeIdxRanges = [];
    let aStart = -1;
    for (let i = 0; i < track.length; i++) {
        const t = filHeater === 0 ? track[i].t0 : track[i].t1;
        const in_tc = inToolchangeZone(fileData, track[i].time);
        const in_wipe = inWipeZone(fileData, track[i].time);
        const is_printing = !in_tc || in_wipe;
        const is_end_gcode = fileData.end_gcode_time && track[i].time >= fileData.end_gcode_time;
        const on = (track[i].active === filId) && (t >= 190) && is_printing && track[i].printing_started && !is_end_gcode;
        if (on && aStart === -1) aStart = i;
        else if (!on && aStart !== -1) { activeIdxRanges.push({ s: aStart, e: i - 1 }); aStart = -1; }
    }
    if (aStart !== -1) activeIdxRanges.push({ s: aStart, e: track.length - 1 });
    if (activeIdxRanges.length === 0) return;

    const isActiveIdx = (i) => activeIdxRanges.some(r => i >= r.s && i <= r.e);
    const inWindow   = (i) => activeIdxRanges.some(r => i >= r.s - STEPS && i <= r.e + STEPS);

    // 2. Draw segments
    for (let i = 0; i < track.length - 1; i++) {
        if (!inWindow(i) && !inWindow(i + 1)) continue;

        const pt1 = track[i], pt2 = track[i + 1];
        const temp1 = filHeater === 0 ? pt1.t0 : pt1.t1;
        const temp2 = filHeater === 0 ? pt2.t0 : pt2.t1;
        const active = isActiveIdx(i) || isActiveIdx(i + 1);

        if (!active) {
            // In preheat/cooldown window: only draw when temperature is CHANGING.
            // A flat high temp means another nozzle is printing — skip it.
            const tempChanging = Math.abs(temp2 - temp1) > 3 || temp1 < 100;
            if (!tempChanging) continue;
        }

        const x1 = scaleX(pt1.time), y1 = scaleY(temp1, yStart, pH);
        const x2 = scaleX(pt2.time), y2 = scaleY(temp2, yStart, pH);
        if (x2 < lB || x1 > rB) continue;

        if (active) {
            ctx.lineWidth = 2.0; ctx.strokeStyle = filColor; ctx.setLineDash([]);
        } else {
            ctx.lineWidth = 1; ctx.strokeStyle = "#52525b"; ctx.setLineDash([3, 2]);
        }

        ctx.beginPath();
        let hx1 = Math.max(x1, lB), hx2 = Math.min(x2, rB);
        if (hx1 <= hx2) { ctx.moveTo(hx1, y1); ctx.lineTo(hx2, y1); ctx.stroke(); }
        ctx.setLineDash([]);

        if (x2 >= lB && x2 <= rB) {
            ctx.lineWidth = active ? 2.0 : 1;
            ctx.strokeStyle = active ? filColor : "#52525b";
            ctx.setLineDash(active ? [] : [3, 2]);
            ctx.beginPath(); ctx.moveTo(x2, y1); ctx.lineTo(x2, y2); ctx.stroke();
            ctx.setLineDash([]);
        }
    }
}

function getFilColor(fileData, tool) {
    let fid = tool; if (fid >= 1000) fid -= 1000;
    if (fid >= 0 && fileData.filaments[fid]) {
        let c = fileData.filaments[fid].color;
        if (c.toUpperCase() === "#FFFFFF") c = "#e4e4e7";
        return c;
    }
    return "#3b82f6";
}

function getClosestState(track, timeVal) {
    let best = track[0];
    for (let i = 0; i < track.length; i++) {
        if (track[i].time <= timeVal) best = track[i]; else break;
    }
    return best;
}

function fmtTool(t) { return "T" + (t >= 1000 ? t - 1000 : t); }

function drawLegend(visibleWidth) {
    const lx = marginLeft + visibleWidth + 20;
    ctx.fillStyle = "#f4f4f5"; ctx.font = "bold 14px system-ui"; ctx.textAlign = "left";
    ctx.fillText("Legend", lx, topYStart + 15);

    let y = topYStart + 35;
    [data.file1, data.file2].forEach((fd, idx) => {
        if (!fd) return;
        ctx.fillStyle = "#e4e4e7"; ctx.font = "bold 11px system-ui";
        ctx.fillText((idx+1) + ". " + fd.slicer + ":", lx, y); y += 15;
        
        ctx.fillStyle = "#71717a"; ctx.font = "10px system-ui";
        const toolchangesCount = (fd.tool_changes || []).length;
        const nozzleChangesCount = (fd.tc_zones || []).length;
        const preheatsCount = fd.preheat_count || 0;
        const cooldownsCount = fd.cooldown_count || 0;
        ctx.fillText("T-changes: " + toolchangesCount + " | H2C: " + nozzleChangesCount, lx, y); y += 13;
        ctx.fillText("Preheats: " + preheatsCount + " | Cools: " + cooldownsCount, lx, y); y += 16;
        
        ctx.font = "11px system-ui";
        Object.keys(fd.filaments).forEach(fid => {
            const fil = fd.filaments[fid];
            let dc = fil.color; if (dc.toUpperCase() === "#FFFFFF") dc = "#e4e4e7";
            const heater = getHeater(fd, parseInt(fid));
            const extId = getExtIdForHeater(fd, heater);
            const extLbl = getExtLabel(fd, extId);
            ctx.fillStyle = dc; ctx.fillRect(lx, y-8, 10, 8);
            ctx.fillStyle = "#a1a1aa"; ctx.font = "11px system-ui";
            ctx.fillText(fmtTool(fid) + ": " + fil.type + " (" + fil.color.toUpperCase() + ")", lx+16, y);
            y += 15;
        });
        y += 8;
    });

    y += 5;
    ctx.fillStyle = "#e4e4e7"; ctx.font = "bold 11px system-ui";
    ctx.fillText("Graph:", lx, y); y += 15;
    // Active printing
    ctx.strokeStyle = "#e4e4e7"; ctx.lineWidth = 2.5; ctx.setLineDash([]);
    ctx.beginPath(); ctx.moveTo(lx, y-5); ctx.lineTo(lx+18, y-5); ctx.stroke();
    ctx.fillStyle = "#a1a1aa"; ctx.font = "11px system-ui";
    ctx.fillText("Active (printing)", lx+24, y-1); y += 14;
    // Standby
    ctx.strokeStyle = "#52525b"; ctx.lineWidth = 1; ctx.setLineDash([6, 4]);
    ctx.beginPath(); ctx.moveTo(lx, y-5); ctx.lineTo(lx+18, y-5); ctx.stroke(); ctx.setLineDash([]);
    ctx.fillStyle = "#a1a1aa"; ctx.fillText("Standby / Cooldown", lx+24, y-1); y += 14;
    // Preheat zone
    ctx.fillStyle = "rgba(220, 38, 38, 0.35)"; ctx.fillRect(lx, y-8, 12, 6);
    ctx.fillStyle = "#a1a1aa"; ctx.fillText("Preheat zone", lx+20, y-1); y += 14;
    // Precool zone
    ctx.fillStyle = "rgba(132, 204, 22, 0.35)"; ctx.fillRect(lx, y-8, 12, 6);
    ctx.fillStyle = "#a1a1aa"; ctx.fillText("Precool zone", lx+20, y-1); y += 14;
    // TC zone
    ctx.fillStyle = "rgba(147, 51, 234, 0.35)"; ctx.fillRect(lx, y-8, 12, 6);
    ctx.fillStyle = "#a1a1aa"; ctx.fillText("Toolchange", lx+20, y-1); y += 14;
    // Wipe tower zone
    ctx.fillStyle = "rgba(6, 182, 212, 0.3)"; ctx.fillRect(lx, y-8, 12, 6);
    ctx.fillStyle = "#a1a1aa"; ctx.fillText("Wipe Tower", lx+20, y-1); y += 14;
    // Nozzle mini panels note
    ctx.fillStyle = "#52525b"; ctx.font = "10px system-ui";
    ctx.fillText("↓ Mini panels = Vortek", lx, y); y += 12;
    ctx.fillText("  nozzle tracks", lx, y);
}

function drawTooltips(scaleX, scaleY, visibleWidth, plotW) {
    const last = panels[panels.length - 1];
    const lastBottom = last ? last.yStart + last.height : height;
    if (mouseX < marginLeft || mouseX > marginLeft+visibleWidth || mouseY < topYStart || mouseY > lastBottom) return;

    ctx.strokeStyle = "#e4e4e7"; ctx.lineWidth = 1;
    ctx.beginPath(); ctx.setLineDash([2,2]); ctx.moveTo(mouseX, topYStart); ctx.lineTo(mouseX, lastBottom); ctx.stroke(); ctx.setLineDash([]);

    const timeNum = ((mouseX - marginLeft + offsetX) / plotW) * data.total_duration;
    updateInfoPanel(timeNum);

    const tips = [];
    
    // 1. Always show the active printing tooltip for both files (main panels only)
    [data.file1, data.file2].forEach(fd => {
        if (!fd) return;
        const st = getClosestState(fd.track, timeNum);
        const h = getHeater(fd, st.active);
        const p = panels.find(pan => pan.type === 'main' && pan.file === fd && pan.heater === h);
        if (p) tips.push({ panel: p, type: "active", state: st });
    });

    // 2. If hovering a preheat region, add a tiny preheat tooltip
    const hoveredPanel = panels.find(p => mouseY >= p.yStart && mouseY <= p.yStart + p.height);
    if (hoveredPanel) {
        const fd = hoveredPanel.file;
        let ph = null;
        if (fd.preheats) {
            ph = fd.preheats.find(h => 
                h.heater === hoveredPanel.heater && 
                timeNum >= h.start_time && 
                timeNum <= h.end_time
            );
        }
        if (ph) {
            tips.push({ panel: hoveredPanel, type: "preheat", info: ph });
        }

        // 2b. Precool (cooldown) region tooltip
        let pc = null;
        if (fd.precools) {
            pc = fd.precools.find(h => 
                h.heater === hoveredPanel.heater && 
                timeNum >= h.start_time && 
                timeNum <= h.end_time
            );
        }
        if (pc) {
            tips.push({ panel: hoveredPanel, type: "precool", info: pc });
        }

        // 3. TC zone tooltip
        if (fd.tc_zones) {
            const tcz = fd.tc_zones.find(z => timeNum >= z.start_time && timeNum <= z.end_time);
            if (tcz) tips.push({ panel: hoveredPanel, type: "tc_zone", info: tcz });
        }
        // 4. Wipe tower zone tooltip
        if (fd.wipe_zones) {
            const wz = fd.wipe_zones.find(z => timeNum >= z.start_time && timeNum <= z.end_time);
            if (wz) tips.push({ panel: hoveredPanel, type: "wipe_zone", info: wz });
        }
    }

    if (tips.length === 0) return;

    // Calculate total height of the stack to center it vertically around mouseY
    let totalHeight = 0;
    const gap = 8;
    tips.forEach(item => {
        if (item.type === "tc_zone" || item.type === "wipe_zone" || item.type === "preheat" || item.type === "precool") {
            totalHeight += 20 + gap;
        } else {
            totalHeight += 108 + gap;
        }
    });
    totalHeight -= gap; // remove last gap

    let startY = mouseY - totalHeight / 2;
    startY = Math.max(topYStart + 10, Math.min(startY, height - totalHeight - 15));

    // Determine X position (left or right of cursor)
    const maxTooltipW = 220;
    let ttX = mouseX + 15;
    if (ttX + maxTooltipW > width) {
        ttX = mouseX - maxTooltipW - 15;
    }

    let currentY = startY;

    tips.forEach(item => {
        const p = item.panel;

        if (item.type === "tc_zone" || item.type === "wipe_zone" || item.type === "preheat" || item.type === "precool") {
            let label, color, ttW;
            if (item.type === "tc_zone") {
                label = "🔧 Toolchange"; color = "rgba(147, 51, 234, 0.9)"; ttW = 100;
            } else if (item.type === "wipe_zone") {
                label = "🏗️ Wipe Tower"; color = "rgba(6, 182, 212, 0.9)"; ttW = 95;
            } else if (item.type === "preheat") {
                label = "🔥 Pheat " + item.info.target_temp + "°C"; color = "rgba(220, 38, 38, 0.9)"; ttW = 115;
            } else {
                label = "❄️ Pcool " + item.info.target_temp + "°C"; color = "rgba(132, 204, 22, 0.9)"; ttW = 115;
            }
            const ttH = 20;
            let itemX = ttX;
            if (ttX < mouseX) {
                itemX = mouseX - ttW - 15;
            }
            ctx.fillStyle = "rgba(17,17,19,0.9)";
            ctx.strokeStyle = color; ctx.lineWidth = 1;
            ctx.beginPath(); ctx.roundRect(itemX, currentY, ttW, ttH, 3); ctx.fill(); ctx.stroke();
            ctx.textAlign = "center";
            ctx.fillStyle = "#ffffff"; ctx.font = "bold 9px system-ui";
            ctx.fillText(label, itemX + ttW/2, currentY + 14);
            currentY += ttH + gap;
        } else {
            const state = item.state;
            const pH = p.height;
            const temp = p.heater === 0 ? state.t0 : state.t1;
            let fIdx = state.active; if (fIdx >= 1000) fIdx -= 1000;
            let filColor = "#FFFFFF", filType = "PLA";
            if (fIdx >= 0 && p.file.filaments[fIdx]) {
                filColor = p.file.filaments[fIdx].color;
                filType = p.file.filaments[fIdx].type;
            }
            const activeOnThisPanel = isActiveOnPanel(p.file, state.active, p.heater);
            const yPos = scaleY(temp, p.yStart, pH);
            const ttW = 220, ttH = 108;

            let sc = activeOnThisPanel ? filColor : "#4b5563";
            if (sc.toUpperCase() === "#FFFFFF") sc = "#e4e4e7";
            ctx.fillStyle = "rgba(17,17,19,0.95)";
            ctx.strokeStyle = sc; ctx.lineWidth = 2;
            ctx.beginPath(); ctx.roundRect(ttX, currentY, ttW, ttH, 6); ctx.fill(); ctx.stroke();

            ctx.textAlign = "left";
            ctx.fillStyle = "#ffffff"; ctx.font = "bold 11px system-ui";
            ctx.fillText(p.file.slicer + " — Ext. " + p.extId, ttX+12, currentY+18);
            ctx.strokeStyle="#27272a"; ctx.lineWidth=1; ctx.beginPath(); ctx.moveTo(ttX+10,currentY+25); ctx.lineTo(ttX+ttW-10,currentY+25); ctx.stroke();

            ctx.font = "11px system-ui"; ctx.fillStyle = "#a1a1aa";
            ctx.fillText("Time: " + formatTime(timeNum) + " (Line " + state.line.toLocaleString() + ")", ttX+12, currentY+41);
            ctx.fillText("Temp:", ttX+12, currentY+57);
            ctx.fillStyle="#ffffff"; ctx.font="bold 11px system-ui";
            ctx.fillText(temp + "°C", ttX+100, currentY+57);

            let activeText = fmtTool(state.active);
            let isEndGCode = p.file.end_gcode_time && timeNum >= p.file.end_gcode_time;
            if (isEndGCode) {
                activeText = "End G-code";
            }
            ctx.font="11px system-ui"; ctx.fillStyle="#a1a1aa";
            ctx.fillText("Active:", ttX+12, currentY+73);
            ctx.fillStyle="#ffffff"; ctx.font="bold 11px system-ui";
            ctx.fillText(activeText, ttX+100, currentY+73);

            ctx.font="11px system-ui"; ctx.fillStyle="#a1a1aa";
            ctx.fillText("Filament:", ttX+12, currentY+89);
            let dc = filColor;
            if (!activeOnThisPanel || isEndGCode) dc = "#4b5563";
            else if (dc.toUpperCase()==="#FFFFFF") dc = "#e4e4e7";
            ctx.fillStyle = dc; ctx.fillRect(ttX+100, currentY+81, 8, 8);
            ctx.fillStyle="#ffffff"; ctx.font="bold 11px system-ui";
            if (isEndGCode) ctx.fillText("N/A", ttX+114, currentY+89);
            else if (!activeOnThisPanel) ctx.fillText("Idle", ttX+114, currentY+89);
            else ctx.fillText(filType + " (" + filColor.toUpperCase() + ")", ttX+114, currentY+89);

            ctx.fillStyle = sc; ctx.beginPath(); ctx.arc(mouseX, yPos, 5, 0, Math.PI*2); ctx.fill();
            currentY += ttH + gap;
        }
    });
}

function updateInfoPanel(timeNum) {
    const el = document.getElementById("info-panel");
    let txt = "Time: " + formatTime(timeNum);

    [data.file1, data.file2].forEach((fd, idx) => {
        if (!fd) return;
        const st = getClosestState(fd.track, timeNum);
        let fIdx = st.active; if (fIdx >= 1000) fIdx -= 1000;
        let fType = "PLA", fHex = "#FFFFFF";
        if (fIdx >= 0 && fd.filaments[fIdx]) { fHex = fd.filaments[fIdx].color.toUpperCase(); fType = fd.filaments[fIdx].type; }
        const htr = getHeater(fd, st.active);
        const eId = getExtIdForHeater(fd, htr);
        const eLbl = getExtLabel(fd, eId);
        const totalTimeStr = formatTime(fd.total_duration);
        const printTimeStr = formatTime(fd.end_gcode_time || fd.total_duration);
        txt += "\n" + (idx+1) + ". " + fd.slicer + " (Total: " + totalTimeStr + ", Print: " + printTimeStr + "): T0=" + st.t0 + "°C T1=" + st.t1 + "°C [" + eLbl + ", " + fmtTool(st.active) + ": " + fType + " (" + fHex + ")]";
    });
    el.innerText = txt; el.style.color = "#e4e4e7";
}

// Events
canvas.addEventListener("mousedown", e => {
    const r = canvas.getBoundingClientRect(), mx = e.clientX - r.left;
    if (mx >= marginLeft && mx <= width - marginRight) { isDragging = true; startDragX = mx; startOffsetX = offsetX; }
});
window.addEventListener("mouseup", () => { isDragging = false; });
canvas.addEventListener("mousemove", e => {
    const r = canvas.getBoundingClientRect(); mouseX = e.clientX - r.left; mouseY = e.clientY - r.top;
    if (isDragging) offsetX = startOffsetX - (mouseX - startDragX);
    draw();
});
canvas.addEventListener("wheel", e => {
    e.preventDefault();
    const r = canvas.getBoundingClientRect(), mx = e.clientX - r.left;
    if (mx < marginLeft || mx > width - marginRight) return;
    const vw = width - marginLeft - marginRight;
    const tuc = ((mx - marginLeft + offsetX) / (vw * scaleXVal)) * data.total_duration;
    scaleXVal = Math.max(1.0, Math.min(scaleXVal * (e.deltaY < 0 ? 1.25 : 0.8), 100.0));
    offsetX = (tuc / data.total_duration) * (vw * scaleXVal) - (mx - marginLeft);
    draw();
}, { passive: false });
canvas.addEventListener("dblclick", () => { scaleXVal = 1.0; offsetX = 0; draw(); });

calculateLayout();
draw();
</script>
</body>
</html>
"""

if __name__ == "__main__":
    main()
