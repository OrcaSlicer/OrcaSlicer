import os
import json
import argparse
from pathlib import Path

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
    "retraction_distance_when_cut", "extruder_type", "internal_bridge_support_thickness",
    "extruder_clearance_max_radius", "top_area_threshold", "reduce_wall_solid_infill",
    "filament_load_time", "filament_unload_time", "smooth_coefficient",
    "overhang_totally_speed", "silent_mode", "overhang_speed_classic"
}

# Utility functions for printing messages in different colors.
def print_error(msg):
    print(f"\033[91m[ERROR]\033[0m {msg}")  # Red

def print_warning(msg):
    print(f"\033[93m[WARNING]\033[0m {msg}")  # Yellow

def print_info(msg):
    print(f"\033[94m[INFO]\033[0m {msg}")  # Blue

def print_success(msg):
    print(f"\033[92m[SUCCESS]\033[0m {msg}")  # Green


# Add helper function for duplicate key detection.
def no_duplicates_object_pairs_hook(pairs):
    seen = {}
    for key, value in pairs:
        if key in seen:
            raise ValueError(f"Duplicate key detected: {key}")
        seen[key] = value
    return seen

# NOTE: currently Orca expects compatible_printers to be a defined in every instantiation profile, inheritation is not supported in Profile page
def check_filament_compatible_printers(vendor_folder):
    """
    Checks JSON files in the vendor folder for missing or empty 'compatible_printers'
    when 'instantiation' is flagged as true.

    Parameters:
        vendor_folder (str or Path): The directory to search for JSON profile files.

    Returns:
        int: The number of profiles with missing or empty 'compatible_printers'.
    """
    error = 0
    vendor_path = Path(vendor_folder)
    if not vendor_path.exists():
        return 0
    
    profiles = {}

    # Use rglob to recursively find .json files.
    for file_path in vendor_path.rglob("*.json"):
        if file_path.name == 'filaments_color_codes.json': # Ignore non-profile file
            continue

        try:
            with open(file_path, 'r', encoding='UTF-8') as fp:
                # Use custom hook to detect duplicates.
                data = json.load(fp, object_pairs_hook=no_duplicates_object_pairs_hook)
        except ValueError as ve:
            print_error(f"Duplicate key error in {file_path}: {ve}")
            error += 1
            continue
        except Exception as e:
            print_error(f"Error processing {file_path}: {e}")
            error += 1
            continue

        profile_name = data['name']
        if profile_name in profiles:
            print_error(f"Duplicated profile {profile_name}: {file_path}")
            error += 1
            continue

        profiles[profile_name] = {
            'file_path': file_path,
            'content': data,
        }
    
    def get_property(profile, key):
        content = profile['content']
        if key in content:
            return content[key]
        return None

    def get_inherit_property(profile, key):
        content = profile['content']
        if key in content:
            return content[key]

        if 'inherits' in content:
            inherits = content['inherits']
            if inherits not in profiles:
                raise ValueError(f"Parent profile not found: {inherits}, referenced in {profile['file_path']}")
            
            return get_inherit_property(profiles[inherits], key)
        
        return None

    for profile in profiles.values():
        instantiation = str(profile['content'].get("instantiation", "")).lower() == "true"
        if instantiation:
            try:
                compatible_printers = get_property(profile, "compatible_printers")
                if not compatible_printers or (isinstance(compatible_printers, list) and not compatible_printers):
                    print_error(f"'compatible_printers' missing in {profile['file_path']}")
                    error += 1
            except ValueError as ve:
                print_error(f"Unable to parse {profile['file_path']}: {ve}")
                error += 1
                continue

    return error

def load_available_filament_profiles(profiles_dir, vendor_name):
    """
    Load all available filament profiles from a vendor's directory.
    
    Parameters:
        profiles_dir (Path): The directory containing vendor profile directories
        vendor_name (str): The name of the vendor directory
        
    Returns:
        set: A set of filament profile names
    """
    profiles = set()
    vendor_path = profiles_dir / vendor_name / "filament"
    
    if not vendor_path.exists():
        return profiles
    
    for file_path in vendor_path.rglob("*.json"):
        try:
            with open(file_path, 'r', encoding='UTF-8') as fp:
                data = json.load(fp)
                if "name" in data:
                    profiles.add(data["name"])
        except Exception as e:
            print_error(f"Error loading filament profile {file_path}: {e}")
    
    return profiles

def check_machine_default_materials(profiles_dir, vendor_name):
    """
    Checks if default materials referenced in machine profiles exist in 
    the vendor's filament library or in the global OrcaFilamentLibrary.
    
    Parameters:
        profiles_dir (Path): The base profiles directory
        vendor_name (str): The vendor name to check
        
    Returns:
        int: Number of missing filament references found
        int: the number of warnings found (0 or 1)
    """
    error_count = 0
    machine_dir = profiles_dir / vendor_name / "machine"
    
    if not machine_dir.exists():
        print_warning(f"No machine profiles found for vendor: {vendor_name}")
        return 0, 1
        
    # Load available filament profiles
    vendor_filaments = load_available_filament_profiles(profiles_dir, vendor_name)
    global_filaments = load_available_filament_profiles(profiles_dir, "OrcaFilamentLibrary")
    all_available_filaments = vendor_filaments.union(global_filaments)
    
    # Check each machine profile
    for file_path in machine_dir.rglob("*.json"):
        try:
            with open(file_path, 'r', encoding='UTF-8') as fp:
                data = json.load(fp)
                
            default_materials = None
            if "default_materials" in data:
                default_materials = data["default_materials"]
            elif "default_filament_profile" in data:
                default_materials = data["default_filament_profile"]
                
            if default_materials:
                if isinstance(default_materials, list):
                    for material in default_materials:
                        if material not in all_available_filaments:
                            print_error(f"Missing filament profile: '{material}' referenced in {file_path.relative_to(profiles_dir)}")
                            error_count += 1
                else:
                    # Handle semicolon-separated list of materials in a string
                    if ";" in default_materials:
                        for material in default_materials.split(";"):
                            material = material.strip()
                            if material and material not in all_available_filaments:
                                print_error(f"Missing filament profile: '{material}' referenced in {file_path.relative_to(profiles_dir)}")
                                error_count += 1
                    else:
                        # Single material in a string
                        if default_materials not in all_available_filaments:
                            print_error(f"Missing filament profile: '{default_materials}' referenced in {file_path.relative_to(profiles_dir)}")
                            error_count += 1
                        
        except Exception as e:
            print_error(f"Error processing machine profile {file_path}: {e}")
            error_count += 1
            
    return error_count, 0

def check_name_consistency(profiles_dir, vendor_name):
    """
    Make sure filament profile names match in both vendor json and subpath files.
    Filament profiles work only if the name in <vendor>.json matches the name in sub_path file,
    or if it's one of the sub_path file's `renamed_from`.

    Parameters:
        profiles_dir (Path): Base profiles directory
        vendor_name (str): Vendor name

    Returns:
        int: Number of errors found
        int: Number of warnings found (0 or 1)
    """
    error_count = 0
    vendor_dir = profiles_dir / vendor_name
    vendor_file = profiles_dir / (vendor_name + ".json")
    
    if not vendor_file.exists():
        print_warning(f"No profiles found for vendor: {vendor_name} at {vendor_file}")
        return 0, 1
    
    try:
        with open(vendor_file, 'r', encoding='UTF-8') as fp:
            data = json.load(fp)
    except Exception as e:
        print_error(f"Error loading vendor profile {vendor_file}: {e}")
        return 1, 0

    for section in ['filament_list', 'machine_model_list', 'machine_list', 'process_list']:
        if section not in data:
            continue
        
        for child in data[section]:
            name_in_vendor = child['name']
            sub_path = child['sub_path']
            sub_file = vendor_dir / sub_path

            if not sub_file.exists():
                print_error(f"Missing sub profile: '{sub_path}' declared in {vendor_file.relative_to(profiles_dir)}")
                error_count += 1
                continue

            try:
                with open(sub_file, 'r', encoding='UTF-8') as fp:
                    sub_data = json.load(fp)
            except Exception as e:
                print_error(f"Error loading profile {sub_file}: {e}")
                error_count += 1
                continue

            name_in_sub = sub_data['name']

            if name_in_sub == name_in_vendor:
                continue

            # if 'renamed_from' in sub_data:
            #     renamed_from = [n.strip() for n in sub_data['renamed_from'].split(';')]
            #     if name_in_vendor in renamed_from:
            #         continue

            print_error(f"{section} name mismatch: required '{name_in_vendor}' in {vendor_file.relative_to(profiles_dir)} but found '{name_in_sub}' in {sub_file.relative_to(profiles_dir)}")
            error_count += 1
    
    return error_count, 0

def check_filament_id(vendor, vendor_folder):
    """
    Make sure filament_id is not longer than 8 characters, otherwise AMS won't work properly
    """
    if vendor not in ('BBL', 'OrcaFilamentLibrary'):
        return 0
    
    error = 0
    vendor_path = Path(vendor_folder)
    if not vendor_path.exists():
        return 0

    # Use rglob to recursively find .json files.
    for file_path in vendor_path.rglob("*.json"):
        try:
            with open(file_path, 'r', encoding='UTF-8') as fp:
                # Use custom hook to detect duplicates.
                data = json.load(fp, object_pairs_hook=no_duplicates_object_pairs_hook)
        except ValueError as ve:
            print_error(f"Duplicate key error in {file_path}: {ve}")
            error += 1
            continue
        except Exception as e:
            print_error(f"Error processing {file_path}: {e}")
            error += 1
            continue

        if 'filament_id' not in data:
            continue
            
        filament_id = data['filament_id']

        if len(filament_id) > 8:
            error += 1
            print_error(f"Filament id too long \"{filament_id}\": {file_path}")
    
    return error

def check_obsolete_keys(profiles_dir, vendor_name):
    """
    Check for obsolete keys in all filament profiles for a vendor.

    Parameters:
        profiles_dir (Path): Base profiles directory
        vendor_name (str): Vendor name
        obsolete_keys (set): Set of obsolete key names to check

    Returns:
        int: Number of obsolete keys found
    """
    error_count = 0
    vendor_path = profiles_dir / vendor_name / "filament"

    if not vendor_path.exists():
        return 0

    for file_path in vendor_path.rglob("*.json"):
        try:
            with open(file_path, "r", encoding="UTF-8") as fp:
                data = json.load(fp)
        except Exception as e:
            print_warning(f"Error reading profile {file_path.relative_to(profiles_dir)}: {e}")
            error_count += 1
            continue

        for key in data.keys():
            if key in OBSOLETE_KEYS:
                print_warning(f"Obsolete key: '{key}' found in {file_path.relative_to(profiles_dir)}")
                error_count += 1

    return error_count


CONFLICT_KEYS = [
    ['extruder_clearance_radius', 'extruder_clearance_max_radius'],
]

def check_conflict_keys(profiles_dir, vendor_name):
    """
    Check for keys that could not be specified at the same time,
    due to option renaming & backward compatibility reasons.

    For example, `extruder_clearance_max_radius` and `extruder_clearance_radius` cannot co-exist
    otherwise slicer won't know which one to use.

    Parameters:
        profiles_dir (Path): Base profiles directory
        vendor_name (str): Vendor name

    Returns:
        int: Number of errors found
        int: Number of warnings found
    """
    error_count = 0
    warn_count = 0
    vendor_path = profiles_dir / vendor_name

    if not vendor_path.exists():
        print_warning(f"No machine profiles found for vendor: {vendor_name}")
        return 0, 1

    for file_path in vendor_path.rglob("*.json"):
        try:
            with open(file_path, 'r', encoding='UTF-8') as fp:
                # Use custom hook to detect duplicates.
                data = json.load(fp, object_pairs_hook=no_duplicates_object_pairs_hook)
        except ValueError as ve:
            print_error(f"Duplicate key error in {file_path.relative_to(profiles_dir)}: {ve}")
            error_count += 1
            continue
        except Exception as e:
            print_error(f"Error processing {file_path.relative_to(profiles_dir)}: {e}")
            error_count += 1
            continue

        for key_sets in CONFLICT_KEYS:
            if sum([1 if k in data else 0 for k in key_sets]) > 1:
                print_error(f"Conflict keys {key_sets} co-exist in {file_path.relative_to(profiles_dir)}")
                error_count += 1

    return error_count, warn_count


def main():
    parser = argparse.ArgumentParser(
        description="Check 3D printer profiles for common issues",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    parser.add_argument("--vendor", type=str, help="Specify a single vendor to check")
    parser.add_argument("--check-filaments", action="store_true", help="Check 'compatible_printers' in filament profiles")
    parser.add_argument("--check-materials", action="store_true", help="Check default materials in machine profiles")
    parser.add_argument("--check-obsolete-keys", action="store_true", help="Warn if obsolete keys are found in filament profiles")
    args = parser.parse_args()

    print_info("Checking profiles ...")

    script_dir = Path(__file__).resolve().parent
    profiles_dir = script_dir.parent / "resources" / "profiles"
    checked_vendor_count = 0
    errors_found = 0
    warnings_found = 0

    def run_checks(vendor_name):
        nonlocal errors_found, warnings_found, checked_vendor_count
        vendor_path = profiles_dir / vendor_name

        if args.check_filaments or not (args.check_materials and not args.check_filaments):
            errors_found += check_filament_compatible_printers(vendor_path / "filament")

        if args.check_materials:
            new_errors, new_warnings = check_machine_default_materials(profiles_dir, vendor_name)
            errors_found += new_errors
            warnings_found += new_warnings

        if args.check_obsolete_keys:
            warnings_found += check_obsolete_keys(profiles_dir, vendor_name)

        new_errors, new_warnings = check_name_consistency(profiles_dir, vendor_name)
        errors_found += new_errors
        warnings_found += new_warnings

        new_errors, new_warnings = check_conflict_keys(profiles_dir, vendor_name)
        errors_found += new_errors
        warnings_found += new_warnings

        errors_found += check_filament_id(vendor_name, vendor_path / "filament")
        checked_vendor_count += 1

    if args.vendor:
        run_checks(args.vendor)
    else:
        for vendor_dir in profiles_dir.iterdir():
            if not vendor_dir.is_dir() or vendor_dir.name == "OrcaFilamentLibrary":
                continue
            run_checks(vendor_dir.name)

    # ✨ Output finale in stile "compilatore"
    print("\n==================== SUMMARY ====================")
    print_info(f"Checked vendors     : {checked_vendor_count}")
    if errors_found > 0:
        print_error(f"Files with errors   : {errors_found}")
    else:
        print_success("Files with errors   : 0")
    if warnings_found > 0:
        print_warning(f"Files with warnings : {warnings_found}")
    else:
        print_success("Files with warnings : 0")
    print("=================================================")
    if errors_found > 0 or warnings_found > 0 :
        print_warning('Issue(s) found, try `orca_filament_lib.py --fix` to fix common issues automatically')

    exit(-1 if errors_found > 0 else 0)


if __name__ == "__main__":
    main()
