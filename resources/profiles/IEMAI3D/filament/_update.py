raise BaseException("Please check file names and configurations before run this script!")
import os
import json

parent_hold_list = [
    "type", "filament_id", "setting_id", "name", "from", "instantiation", "inherits", "printer_model",
    "filament_settings_id"
]

custom_dict = {
    "type": "filament",
    "filament_id": "YMF020",      # 递增.
    "setting_id": "YMFS01",       # 可以不改.
    "name": "IEMAI3D-PLA-M",      # 使用输出文件的文件名.
    "from": "system",
    "instantiation": "true",
    # "inherits": "fdm_filament_abs",   # 继承, 必须确认.
    "compatible_printers": [
        "YM-NT-750 0.6 Nozzle - Dual",   # 对应机型, 必须确认.
    ],
    "filament_type": [
        "General",       # 必须确认好.
    ],
}

# 这4项仔细修改.
parent_file_name = "My Generic PLA.json"
child_file_name = ""
child_file_path = 'F:\\EMAI_PROJECT\\OrcaSlicer\\OrcaSlicer_profiles from 子聪 2024-10-18\\Filament presets\\PLA-M.json'
output_file_name = "IEMAI3D-PLA-M.json"


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
    
    if parent_file_name:
        pp_dir = os.path.dirname(os.path.dirname(this_dir_path))
        file_parent = os.path.join(pp_dir, "Custom", "filament", parent_file_name)
    else:
        file_parent = ""
    if child_file_path:
        file_child = child_file_path
    else:
        file_child = os.path.join(this_dir_path, child_file_name)
    file_output = os.path.join(this_dir_path, output_file_name)
    
    if file_parent:
        parent = read_json(file_parent)
    else:
        parent = {}
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

