import concurrent.futures, json, pathlib, subprocess, zipfile, sys
import xml.etree.ElementTree as ET
ROOT=pathlib.Path(__file__).resolve().parent
REPO=ROOT.parents[3]
EXE=REPO/'build/src/Release/orca-slicer.exe'
def win(p):return subprocess.check_output(['wslpath','-w',str(p)],text=True).strip()
def variant(name, changes, second=False, duplicate=False):
 p=ROOT/(name+'.3mf')
 with zipfile.ZipFile(REPO/'圆柱体.3mf') as src,zipfile.ZipFile(p,'w',zipfile.ZIP_DEFLATED) as dst:
  for entry in src.infolist():
   data=src.read(entry.filename)
   if entry.filename=='Metadata/project_settings.config':
    config=json.loads(data);config.update(changes);data=json.dumps(config,ensure_ascii=False).encode()
   if second and entry.filename=='Metadata/model_settings.config':
    root=ET.fromstring(data)
    for meta in root.findall('object/metadata'):
     if meta.get('key')=='extruder':meta.set('value','2')
    data=ET.tostring(root,encoding='utf-8',xml_declaration=True)
   if duplicate and entry.filename=='3D/3dmodel.model':
    import re,uuid
    text=data.decode();item=re.search(r'<item\s[^>]+/>',text).group()
    extra=re.sub(r'transform="([^"]+)"',lambda m:'transform="'+' '.join(m.group(1).split()[:-3]+['200','155','15.5'])+'"',item)
    extra=re.sub(r'p:UUID="[^"]+"','p:UUID="'+str(uuid.uuid4())+'"',extra)
    data=text.replace(item,item+'\n'+extra).encode()
   if duplicate and entry.filename=='Metadata/model_settings.config':
    text=data.decode();data=text.replace('</plate>','<model_instance><metadata key="object_id" value="2"/><metadata key="instance_id" value="1"/><metadata key="identify_id" value="55"/></model_instance></plate>').encode()
   dst.writestr(entry,data)
 return p
cases=[('cylinder',REPO/'圆柱体.3mf',None),('cube',REPO/'立方体.3mf',None),
 ('prime',variant('prime',{'enable_prime_tower':'1','top_surface_filament_id':'2'}),'the prime tower must be disabled'),
 ('sequence',variant('sequence',{'print_sequence':'by object'}),'print sequence must be By layer'),
 ('multi',variant('multi',{'top_surface_filament_id':'2'}),'exactly one used material is required (found 2)'),
 ('second',variant('second',{},second=True),None),
 ('instances',variant('instances',{},duplicate=True),'exactly one object instance is required'),
 ('off',variant('off',{'continuous_print_mode':'0'}),'')]
def run(case):
 name,src,expected=case;out=ROOT/name;out.mkdir(exist_ok=True)
 with (out/'slice.log').open('w') as log:
  r=subprocess.run([str(EXE),'--slice','0','--outputdir',win(out)]+(['--filament-colour','#FF0000;#00FF00','--enable-prime-tower'] if name=='prime' else [])+[win(src)],stdout=log,stderr=subprocess.STDOUT,timeout=600)
 report=(out/'slice.log').read_text(errors='replace')
 assert r.returncode==0,(name,r.returncode,report)
 assert (out/'plate_1.gcode').exists(),name
 if expected is None:assert 'was requested but not applied' not in report,(name,report)
 elif expected:assert expected in report,(name,report)
 else:assert 'Continuous print:' not in report,(name,report)
 print(name,'PASS',report.strip(),flush=True)
 return name
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
 list(pool.map(run,[c for c in cases if len(sys.argv)==1 or c[0] in sys.argv[1:]]))
