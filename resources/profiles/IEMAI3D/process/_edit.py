
import os
import json

ADD_PARAMS = {
    "support_type": "tree(auto)",
    "support_style": "default",
    "support_threshold_angle": "40",
    "raft_first_layer_expansion": "2",

    "tree_support_branch_diameter_organic": "5",
    "tree_support_branch_distance_organic": "2",
    "tree_support_tip_diameter": "1.6",
}

TARGET_FILES = [
    '0.24mm Dual For HT@IEMAI3D.json',
    '0.24mm Dual For NT@IEMAI3D.json',
    '0.24mm Dual Support @IEMAI3D.json',
    '0.24mm Single For HT@IEMAI3D.json',
    '0.24mm Single For NT@IEMAI3D.json',
]

if __name__ == "__main__":
    this_file_dir = os.path.dirname(os.path.abspath(__file__))
    
    # search all dirs in this_file_dir
    for fname in os.listdir(this_file_dir):
        json_file = os.path.join(this_file_dir, fname)
        if fname in TARGET_FILES:
            print(f"edit file: {fname}")
            # read json file
            json_data = None
            with open(json_file, "r", encoding='utf-8') as fp:
                json_data = fp.read()
            json_data = json.loads(json_data)
            # add parameters
            json_data.update(ADD_PARAMS)
            # write json file
            with open(json_file, "w", encoding='utf-8') as fp:
                fp.write(json.dumps(json_data, indent=4))
            print(f"done.")
