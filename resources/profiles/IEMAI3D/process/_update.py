raise BaseException("Please check file names and configurations before run this script!")
import os
import json

parent_hold_list = [
    "type", "setting_id", "name", "from", "instantiation", "inherits", "printer_model",
    "print_settings_id"
]

custom_dict = {
    "type": "process",
    "setting_id": "YMP032",
    "name": "Pellet-2.5 @IEMAI3D",
    "from": "system",
    "inherits": "fdm_process_iemai_common",
    "instantiation": "true",
    "compatible_printers": [
        "FAST-JET-1500 4.0 Nozzle",
    ]
}

parent_file_name = "_empty.json"
# parent_file_name = "0.24mm Dual Support @IEMAI3D.json"
child_file_name = ""
# child_file_path = ""
child_file_path = 'f:\\EMAI_PROJECT\\OrcaSlicer\\profiles from 子聪 2024-10-25\\Process\\Pellet-2.5 @IEMAI3D.json'
output_file_name = "Pellet-2.5 @IEMAI3D.json"

def read_json(file_path):
    with open(file_path, "r", encoding='utf-8') as f:
        return json.load(f)

def write_json(file_path, data):
    with open(file_path, "w", encoding='utf-8') as f:
        json.dump(data, f, indent=4, ensure_ascii=True)

def update_to_parent(parent, child):
    for key in child:
        if key not in parent_hold_list:
            parent[key] = child[key]
    for key in custom_dict:
        parent[key] = custom_dict[key]
    return parent

if __name__ == "__main__":
    this_file_path = os.path.abspath(__file__)
    this_dir_path = os.path.dirname(this_file_path)
    
    file_parent = os.path.join(this_dir_path, parent_file_name)
    if child_file_path:
        file_child = child_file_path
    else:
        file_child = os.path.join(this_dir_path, child_file_name)
    file_output = os.path.join(this_dir_path, output_file_name)
    
    parent = read_json(file_parent)
    child = read_json(file_child)
    # print(type(parent))
    # print(parent)

    parent = update_to_parent(parent, child)
    # print(parent)

    write_json(file_output, parent)

    with open(this_file_path, "r", encoding='utf-8') as f:
        content = f.read()
    with open(this_file_path, "w", encoding='utf-8') as f:
        f.write('raise BaseException("Please check file names and configurations before run this script!")\n')
        f.write(content)

