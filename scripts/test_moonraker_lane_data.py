#!/usr/bin/env python3
"""
Test script for MoonrakerPrinterAgent filament sync feature.
Inserts/deletes/modifies random lane data in Moonraker database,
then reads back and displays with colored output.
"""

import requests
import random
import argparse
import json
import time
import sys

# Configuration
DEFAULT_HOST = "192.168.88.9"
DEFAULT_PORT = 7125
NAMESPACE = "lane_data"
LANE_KEYS = [f"lane{i}" for i in range(1, 9)]  # lane1-lane8
MATERIALS = ["PLA", "ABS", "PETG", "ASA", "ASA Sparkle", "TPU", ""]

# Material default temperatures (None = use null)
MATERIAL_TEMPS = {
    "PLA":        {"nozzle": 210, "bed": 60},
    "ABS":        {"nozzle": 240, "bed": 100},
    "PETG":       {"nozzle": 235, "bed": 80},
    "ASA":        {"nozzle": 245, "bed": 105},
    "ASA Sparkle":{"nozzle": 245, "bed": 105},
    "TPU":        {"nozzle": 220, "bed": 50},
    "":           {"nozzle": None, "bed": None},
}

def test_connection(host, port, api_key=None, verbose=False):
    """Test basic connectivity to Moonraker."""
    url = f"http://{host}:{port}/server/info"
    headers = {"X-Api-Key": api_key} if api_key else {}

    if verbose:
        print(f"  Testing: GET {url}")

    try:
        resp = requests.get(url, headers=headers, timeout=10)
        if verbose:
            print(f"  Response: HTTP {resp.status_code}")
        if resp.status_code == 200:
            data = resp.json()
            if verbose:
                print(f"  Moonraker version: {data.get('result', {}).get('moonraker_version', 'unknown')}")
            return True
        else:
            print(f"  Server returned HTTP {resp.status_code}")
            if verbose:
                print(f"  Response: {resp.text[:500]}")
            return False
    except requests.exceptions.ConnectionError as e:
        print(f"  Connection error: {e}")
        return False
    except requests.exceptions.Timeout:
        print(f"  Connection timed out")
        return False
    except Exception as e:
        print(f"  Error: {type(e).__name__}: {e}")
        return False

def hex_to_rgb(hex_color):
    """Convert hex color to RGB tuple."""
    hex_color = hex_color.lstrip('#')
    if hex_color.startswith('0x') or hex_color.startswith('0X'):
        hex_color = hex_color[2:]
    if len(hex_color) == 6:
        return tuple(int(hex_color[i:i+2], 16) for i in (0, 2, 4))
    return (128, 128, 128)  # Default gray

def color_block(hex_color):
    """Return ANSI color block for terminal display."""
    r, g, b = hex_to_rgb(hex_color)
    return f"\033[48;2;{r};{g};{b}m    \033[0m"

def random_color():
    """Generate random hex color, occasionally returning empty or '#None' like real data."""
    r = random.random()
    if r < 0.1:
        return ""       # Empty color (empty lane)
    if r < 0.15:
        return "#None"  # Observed in real data for unknown colors
    return "#{:06x}".format(random.randint(0, 0xFFFFFF))

def get_lane_data(host, port, api_key=None):
    """Fetch all lane data from Moonraker database."""
    url = f"http://{host}:{port}/server/database/item"
    params = {"namespace": NAMESPACE}
    headers = {"X-Api-Key": api_key} if api_key else {}

    try:
        resp = requests.get(url, params=params, headers=headers, timeout=5)
        if resp.status_code == 200:
            data = resp.json()
            return data.get("result", {}).get("value", {})
        elif resp.status_code == 404:
            return {}  # Namespace doesn't exist yet
        else:
            print(f"Error fetching lane data: HTTP {resp.status_code}")
            return None
    except Exception as e:
        print(f"Error fetching lane data: {e}")
        return None

def set_lane_data(host, port, lane_key, lane_data, api_key=None):
    """Set lane data in Moonraker database."""
    url = f"http://{host}:{port}/server/database/item"
    headers = {"Content-Type": "application/json"}
    if api_key:
        headers["X-Api-Key"] = api_key

    payload = {
        "namespace": NAMESPACE,
        "key": lane_key,
        "value": lane_data
    }

    try:
        resp = requests.post(url, json=payload, headers=headers, timeout=5)
        return resp.status_code == 200
    except Exception as e:
        print(f"Error setting lane data: {e}")
        return False

def delete_lane_data(host, port, lane_key, api_key=None):
    """Delete lane data from Moonraker database."""
    url = f"http://{host}:{port}/server/database/item"
    params = {"namespace": NAMESPACE, "key": lane_key}
    headers = {"X-Api-Key": api_key} if api_key else {}

    try:
        resp = requests.delete(url, params=params, headers=headers, timeout=5)
        return resp.status_code == 200
    except Exception as e:
        print(f"Error deleting lane data: {e}")
        return False

def display_lanes(lanes):
    """Display lane data with color blocks."""
    print("\n" + "="*70)
    print("CURRENT LANE DATA")
    print("="*70)

    if not lanes:
        print("  (no lanes configured)")
        return

    # Sort by lane number
    sorted_lanes = sorted(lanes.items(),
                          key=lambda x: int(x[1].get("lane", "0")) if x[1].get("lane", "").isdigit() else 0)

    for lane_key, data in sorted_lanes:
        lane_num = data.get("lane", "?")
        material = data.get("material", "") or "(empty)"
        color = data.get("color", "")
        bed_temp = data.get("bed_temp")
        nozzle_temp = data.get("nozzle_temp")
        spool_id = data.get("spool_id")

        # Show color block only for valid hex colors
        if color and color.startswith("#") and color != "#None" and len(color) == 7:
            block = color_block(color)
        else:
            block = "    "  # No color block

        bed_str = f"{bed_temp}°C" if bed_temp is not None else "-"
        noz_str = f"{nozzle_temp}°C" if nozzle_temp is not None else "-"
        spool_str = f"  Spool: {spool_id}" if spool_id is not None else ""
        color_str = color if color else "(none)"

        print(f"  {lane_key} (T{lane_num}): {block} {color_str:10s}  {material:12s}  "
              f"Nozzle: {noz_str:6s}  Bed: {bed_str:5s}{spool_str}")

    print("="*70 + "\n")

def make_lane_entry(tool_number, material=None):
    """Generate a lane data entry matching real Moonraker AFC structure."""
    if material is None:
        material = random.choice(MATERIALS)
    temps = MATERIAL_TEMPS[material]
    color = random_color()

    bed = None
    nozzle = None
    if temps["bed"] is not None:
        bed = temps["bed"] + random.randint(-5, 5)
    if temps["nozzle"] is not None:
        nozzle = temps["nozzle"] + random.randint(-10, 10)

    spool_id = random.choice([None, random.randint(1, 50)])

    return {
        "color": color,
        "material": material,
        "bed_temp": bed,
        "nozzle_temp": nozzle,
        "scan_time": "",
        "td": "",
        "lane": str(tool_number),
        "spool_id": spool_id,
    }

def get_used_tool_numbers(host, port, api_key=None, exclude_key=None):
    """Get set of tool numbers currently in use."""
    lanes = get_lane_data(host, port, api_key) or {}
    used = set()
    for key, data in lanes.items():
        if key == exclude_key:
            continue
        lane_val = data.get("lane", "")
        if lane_val.isdigit():
            used.add(int(lane_val))
    return used

def pick_available_tool_number(used_tool_numbers):
    """Pick a random tool number (0-7) not already in use. Returns None if all taken."""
    available = [n for n in range(8) if n not in used_tool_numbers]
    if not available:
        return None
    return random.choice(available)

def fix_duplicate_lanes(host, port, lanes, api_key=None):
    """Detect and fix duplicate tool numbers in existing lane data.

    Returns the updated lane data after fixes.
    """
    if not lanes:
        return lanes

    # Map tool number -> list of lane keys using it
    tool_to_keys = {}
    for key, data in lanes.items():
        tool = data.get("lane", "")
        if tool == "":
            continue
        tool_to_keys.setdefault(tool, []).append(key)

    # Find duplicates
    duplicates = {tool: keys for tool, keys in tool_to_keys.items() if len(keys) > 1}
    if not duplicates:
        return lanes

    print("DUPLICATE TOOL NUMBERS DETECTED:")
    for tool, keys in duplicates.items():
        print(f"  Tool T{tool} used by: {', '.join(keys)}")

    # Collect all used tool numbers
    used = set()
    for tool, keys in tool_to_keys.items():
        if tool.isdigit():
            used.add(int(tool))

    # Fix: keep the first key for each tool, reassign the rest
    print("\nFixing duplicates...")
    for tool, keys in duplicates.items():
        # Keep the first one, reassign the rest
        for key in keys[1:]:
            available = [n for n in range(8) if n not in used]
            if not available:
                print(f"  {key}: cannot fix, no available tool numbers!")
                continue

            new_tool = available[0]
            used.add(new_tool)

            lanes[key]["lane"] = str(new_tool)
            if set_lane_data(host, port, key, lanes[key], api_key):
                print(f"  {key}: T{tool} -> T{new_tool}")
            else:
                print(f"  {key}: FAILED to update")

    print()
    return lanes

def perform_random_operations(host, port, api_key=None, num_ops=5):
    """Perform random insert/modify/delete operations."""
    operations = ["insert", "modify", "delete"]

    print(f"\nPerforming {num_ops} random operations...")
    print("-"*50)

    for i in range(num_ops):
        op = random.choice(operations)
        lane_key = random.choice(LANE_KEYS)

        if op in ("insert", "modify"):
            # Get currently used tool numbers, excluding this key (ok to reuse its own)
            used = get_used_tool_numbers(host, port, api_key, exclude_key=lane_key)
            tool_num = pick_available_tool_number(used)
            if tool_num is None:
                print(f"  [{op.upper()}] {lane_key}: SKIPPED (all tool numbers in use)")
                continue

            lane_data = make_lane_entry(tool_num)
            action = "INSERT" if op == "insert" else "MODIFY"
            color = lane_data["color"]
            material = lane_data["material"] or "(empty)"
            tool = lane_data["lane"]

            if color and color.startswith("#") and color != "#None" and len(color) == 7:
                block = color_block(color)
            else:
                block = "    "

            if set_lane_data(host, port, lane_key, lane_data, api_key):
                print(f"  [{action}] {lane_key} (T{tool}): {block} {color or '(none)'}  "
                      f"{material}  spool={lane_data['spool_id']}")
            else:
                print(f"  [{action}] {lane_key}: FAILED")

        elif op == "delete":
            if delete_lane_data(host, port, lane_key, api_key):
                print(f"  [DELETE] {lane_key}")
            else:
                print(f"  [DELETE] {lane_key}: FAILED (may not exist)")

        time.sleep(0.1)  # Small delay between operations

    print("-"*50)

def load_lanes_from_file(filepath, host, port, api_key=None):
    """Load lane data from a JSON file and overwrite all lanes on the printer.

    Accepts either the raw Moonraker response format:
        {"result": {"namespace": "lane_data", "value": {"lane1": {...}, ...}}}
    or the plain value object:
        {"lane1": {...}, "lane2": {...}, ...}
    """
    try:
        with open(filepath, "r") as f:
            data = json.load(f)
    except FileNotFoundError:
        print(f"Error: file not found: {filepath}")
        return False
    except json.JSONDecodeError as e:
        print(f"Error: invalid JSON in {filepath}: {e}")
        return False

    # Accept both wrapped and unwrapped formats
    if "result" in data and "value" in data.get("result", {}):
        lanes = data["result"]["value"]
    else:
        lanes = data

    if not isinstance(lanes, dict):
        print(f"Error: expected object with lane keys, got {type(lanes).__name__}")
        return False

    # Validate no duplicate tool numbers
    tool_to_keys = {}
    for key, entry in lanes.items():
        tool = entry.get("lane", "")
        if tool:
            tool_to_keys.setdefault(tool, []).append(key)
    dupes = {t: keys for t, keys in tool_to_keys.items() if len(keys) > 1}
    if dupes:
        print("Error: input JSON has duplicate tool numbers:")
        for tool, keys in dupes.items():
            print(f"  Tool T{tool} used by: {', '.join(keys)}")
        return False

    print(f"Loading {len(lanes)} lane(s) from {filepath}...")

    # Clear all existing lanes first
    print("  Clearing existing lanes...")
    for lane_key in LANE_KEYS:
        delete_lane_data(host, port, lane_key, api_key)

    # Write each lane from the file
    ok = True
    for lane_key, lane_data in lanes.items():
        if set_lane_data(host, port, lane_key, lane_data, api_key):
            tool = lane_data.get("lane", "?")
            material = lane_data.get("material", "") or "(empty)"
            color = lane_data.get("color", "")
            if color and color.startswith("#") and color != "#None" and len(color) == 7:
                block = color_block(color)
            else:
                block = "    "
            print(f"  [LOAD] {lane_key} (T{tool}): {block} {color or '(none)'}  {material}")
        else:
            print(f"  [LOAD] {lane_key}: FAILED")
            ok = False

    return ok

def main():
    parser = argparse.ArgumentParser(
        description="Test Moonraker lane data for MoonrakerPrinterAgent filament sync"
    )
    parser.add_argument("--host", default=DEFAULT_HOST,
                        help=f"Moonraker host (default: {DEFAULT_HOST})")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT,
                        help=f"Moonraker port (default: {DEFAULT_PORT})")
    parser.add_argument("--api-key", help="Moonraker API key (if required)")
    parser.add_argument("--ops", type=int, default=5,
                        help="Number of random operations (default: 5)")
    parser.add_argument("--clear", action="store_true",
                        help="Clear all lane data before starting")
    parser.add_argument("--read-only", action="store_true",
                        help="Only read and display current lane data")
    parser.add_argument("--load", metavar="FILE",
                        help="Load lane data from JSON file and overwrite printer lanes")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Verbose output for debugging")

    args = parser.parse_args()

    print(f"\nConnecting to Moonraker at {args.host}:{args.port}...")

    # First test basic connectivity
    if not test_connection(args.host, args.port, args.api_key, args.verbose):
        print("\nFailed to connect to Moonraker!")
        print("\nTroubleshooting:")
        print(f"  1. Check if Moonraker is running on {args.host}")
        print(f"  2. Verify port {args.port} is correct (default Moonraker port is 7125)")
        print(f"  3. Try: curl http://{args.host}:{args.port}/server/info")
        print(f"  4. Check if API key is required (--api-key)")
        return 1

    print("Connected!")

    # Now fetch lane data
    current = get_lane_data(args.host, args.port, args.api_key)
    if current is None:
        print("Connected to Moonraker but failed to fetch lane data!")
        return 1

    # Check for and fix duplicate tool numbers
    current = fix_duplicate_lanes(args.host, args.port, current, args.api_key)

    # Show current state
    display_lanes(current)

    if args.read_only:
        return 0

    # Load from JSON file if requested
    if args.load:
        if not load_lanes_from_file(args.load, args.host, args.port, args.api_key):
            return 1
        final = get_lane_data(args.host, args.port, args.api_key)
        display_lanes(final)
        if final is not None:
            print("RAW JSON:")
            print(json.dumps({"result": {"namespace": NAMESPACE, "key": None, "value": final}}, indent=2))
            print()
        return 0

    # Clear if requested
    if args.clear:
        print("Clearing all lane data...")
        for lane_key in LANE_KEYS:
            delete_lane_data(args.host, args.port, lane_key, args.api_key)
        print("Cleared!")
        display_lanes({})

    # Perform random operations
    perform_random_operations(args.host, args.port, args.api_key, args.ops)

    # Read back and display final state
    final = get_lane_data(args.host, args.port, args.api_key)
    display_lanes(final)

    # Print raw JSON
    if final is not None:
        print("RAW JSON:")
        print(json.dumps({"result": {"namespace": NAMESPACE, "key": None, "value": final}}, indent=2))
        print()

    return 0

if __name__ == "__main__":
    exit(main())
