import os
import json

setting_id_used=set()
setting_id_all=set()
root_dir=os.path.dirname(os.path.abspath(__file__))


def loadBlackList():
    with open(root_dir+'/blacklist.json') as file:
        data=json.load(file)

    for key,val in data.items():
        for item in val:
            setting_id_used.add(item)
            setting_id_all.add(item)

def traverse_files(path):
    for file in os.listdir(path):
        file_path = os.path.join(path, file)
        if os.path.isdir(file_path):
            traverse_files(file_path)  # 递归遍历子文件夹
        elif file_path.endswith('.json'):
            # 解析 JSON 文件并提取 setting_id 的值
            with open(file_path) as f:
                data = json.load(f)
                if 'setting_id' in data:
                    setting_id_all.add(data['setting_id'])

def getUsedId(brand):
    with open(root_dir+'/'+brand+'.json')as file:
        data=json.load(file)

    key_list=["machine_model_list","machine_list","filament_list","process_list"]

    for key in key_list:
          for elem in data[key]:
            path=elem['sub_path']
            with open(root_dir+'/'+brand+'/'+path) as file:
                file_data=json.load(file)
            if 'setting_id' in file_data:
                setting_id_used.add(file_data['setting_id'])


def getTotalId(brand):
    traverse_files(root_dir+'/'+brand)


loadBlackList()
getUsedId('BBL')
getTotalId('BBL')

print("unused setting_id :")
print(setting_id_all.difference(setting_id_used))
