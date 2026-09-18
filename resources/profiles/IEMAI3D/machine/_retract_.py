raise BaseException("Please check file names before run this script!")
import os
import json

# 使用 child 文件 覆盖 parent 文件

parent_file_name_list = [
    # "MAGIC-HT-MAX 0.4 nozzle - Dual.json",
    # "YM-NT-750 0.6 Nozzle - Dual.json",
    # "YM-NT-1000 0.6 Nozzle - Dual.json",
    # "YM-NT-1200 0.6 Nozzle - Dual.json",
    # "",
    "FAST-JET-1500 4.0 Nozzle.json",
    "Medical-450 2.0 Nozzle.json",
    "",
]

# child_file_name = "_retract_dual_.json"
child_file_name = "_retract_single_.json"


def read_json(file_path):
    with open(file_path, "r", encoding='utf-8') as f:
        return json.load(f)


def write_json(file_path, data):
    with open(file_path, "w", encoding='utf-8') as f:
        json.dump(data, f, indent=4, ensure_ascii=True)


if __name__ == "__main__":
    this_file_path = os.path.abspath(__file__)
    this_dir_path = os.path.dirname(this_file_path)
    
    file_child = os.path.join(this_dir_path, child_file_name)
    child = read_json(file_child)

    for parent_file_name in parent_file_name_list:
        if not parent_file_name:
            continue
        file_parent = os.path.join(this_dir_path, parent_file_name)
        file_output = file_parent
        parent = read_json(file_parent)

        # parent.update(child)
        for key in child:
            parent[key] = child[key]

        write_json(file_output, parent)

    with open(this_file_path, "r", encoding='utf-8') as f:
        content = f.read()
    with open(this_file_path, "w", encoding='utf-8') as f:
        f.write('raise BaseException("Please check file names before run this script!")\n')
        f.write(content)
