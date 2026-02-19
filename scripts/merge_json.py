import json
import sys
import os

def merge_logic(path1, path2, output_path=None):
    # Set default output name if not provided
    if not output_path:
        name1 = os.path.splitext(os.path.basename(path1))[0]
        name2 = os.path.splitext(os.path.basename(path2))[0]
        output_path = f"{name1}To{name2}.json"

    try:
        with open(path1, 'r') as f:
            data1 = json.load(f)
        with open(path2, 'r') as f:
            data2 = json.load(f)
    except FileNotFoundError as e:
        print(f"Error: {e}")
        return
    except json.JSONDecodeError as e:
        print(f"Error parsing JSON: {e}")
        return

    lists_to_merge = ['machine_model_list', 'machine_list', 'filament_list', 'process_list']

    for list_key in lists_to_merge:
        if list_key not in data1 or list_key not in data2:
            continue
        existing_names = {item['name'] for item in data2[list_key] if 'name' in item}
        
        # Identify items in first that are NOT in the second
        unique_to_first = [item for item in data1[list_key] if item.get('name') not in existing_names]

        # Combine: Second file data first, unique items from first file appended
        # ex. 1) abcdef 2) def out) defabc
        # order is set to align with the 2nd
        # i.e. 1) abcdef 2) ace out) acebdf
        # the second file should be the file which you want a similar structure to (i.e. BBL's source json, for easier imports and diffs)
        data2[list_key] = data2[list_key] + unique_to_first

    with open(output_path, 'w') as f:
        json.dump(data2, f, indent=4)
    
    print(f"Successfully merged into {output_path}")

if __name__ == "__main__":
    # Check if at least the two input files are provided
    if len(sys.argv) < 3:
        print("Usage: python script.py <orca.json> <bbl.json> [output.json]")
    else:
        file1 = sys.argv[1]
        file2 = sys.argv[2]
        out_file = sys.argv[3] if len(sys.argv) > 3 else None
        merge_logic(file1, file2, out_file)
