import os
import json

f1 = '_750_.json'
f2 = '_base_MyMarlin 0.4 nozzle.json'

def read_json(file_path):
    with open(file_path, "r", encoding='utf-8') as f:
        return json.load(f)

def write_json(file_path, data):
    with open(file_path, "w", encoding='utf-8') as f:
        json.dump(data, f, indent=4, ensure_ascii=True)

if __name__ == "__main__":
    this_file_path = os.path.abspath(__file__)
    this_dir_path = os.path.dirname(this_file_path)
    
    file_1 = os.path.join(this_dir_path, f1)
    file_2 = os.path.join(this_dir_path, f2)
    
    conf_1 = read_json(file_1)
    conf_2 = read_json(file_2)

    # compare two files and print differences
    for key in conf_1.keys():
        if 'gcode' not in key:
            if key not in conf_2:
                print(f"{key:<40}:  {str(conf_1[key]):<20}  <-->  None")
            elif conf_1[key] != conf_2[key]:
                print(f"{key:<40}:  {str(conf_1[key]):<20}  <-->  {conf_2[key]}")
    for key in conf_2.keys():
        if 'gcode' not in key:
            if key not in conf_1:
                print(f"{key:<40}:  None  <-->  {conf_2[key]}")

