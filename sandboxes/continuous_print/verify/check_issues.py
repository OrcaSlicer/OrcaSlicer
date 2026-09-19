import json, pathlib, subprocess, sys, os, re
from collections import Counter
root=pathlib.Path('sandboxes/continuous_print')
tag=sys.argv[1]
exe=sys.argv[2]
def win(p):return 'G:\\SinglePath\\OrcaSlicer\\'+str(p).replace('/','\\')
for walls in [1,2,3,4,6]:
 out=root/'verify'/tag/('walls'+str(walls));out.mkdir(parents=True,exist_ok=True)
 p=json.loads((root/'profiles/cp_real_on.json').read_text());p['wall_loops']=str(walls);p['name']='issues_w'+str(walls)
 if tag.endswith('off'):p['continuous_print_mode']='0'
 f=out/'process.json';f.write_text(json.dumps(p))
 args=[str(pathlib.Path(exe).resolve()),'--slice','0','--outputdir',win(out),'--load-settings',win(pathlib.Path('resources/profiles/BBL/machine/Bambu Lab X1 Carbon 0.4 nozzle.json'))+';'+win(f),'--load-filaments',win(pathlib.Path('resources/profiles/BBL/filament/Bambu PLA Basic @BBL X1C.json')),win(root/'solid_cube.stl')]
 with (out/'slice.log').open('w') as log:
  r=subprocess.run(args,stdout=log,stderr=subprocess.STDOUT,env=dict(os.environ,WSLENV='CP_DEBUG',CP_DEBUG='1'))
 if r.returncode: print(walls,'exit',r.returncode,flush=True);continue
 # Measure object moves, excluding skirt and machine start/end scripts, using actual modal state.
 x=y=e=z=0.;relative=False;layer=0;role='';seen=False;travel=0;stats={}
 for raw in (out/'plate_1.gcode').read_text().splitlines():
  if raw.strip()=='; CHANGE_LAYER': layer+=1;seen=False;travel=0;role='';stats[layer]=dict(travel=0,roles=set(),zs=[])
  if raw.startswith('; FEATURE: '):role=raw[11:]
  code=raw.split(';')[0].split()
  if not code:continue
  if code[0]=='M83':relative=True
  if code[0]=='M82':relative=False
  vals={k:float(v) for k,v in re.findall(r'([XYZE])(-?\d*\.?\d+)',raw.split(';')[0])}
  if code[0]=='G92':e=vals.get('E',e);continue
  if code[0] not in ['G0','G1']:continue
  nx=vals.get('X',x);ny=vals.get('Y',y);nz=vals.get('Z',z);ne=vals.get('E',0 if relative else e)
  de=ne if relative else ne-e
  if layer and role not in ['', 'Custom','Skirt','Brim']:
   if abs(nx-x)+abs(ny-y)>1e-7:
    if de>1e-8:
     seen=True;stats[layer]['travel']+=travel;travel=0;stats[layer]['roles'].add(role);stats[layer]['zs'].append(nz)
    elif seen:travel+=1
  x,y,z=nx,ny,nz
  if not relative:e=ne
 print(tag,walls,'clean',sum(v['travel']==0 for v in stats.values()),'/',len(stats),
       'surfaces',[(i,v['travel']) for i,v in stats.items() if v['roles'] & {'Top surface','Bottom surface'}],
       'fallback',[(i,v['travel']) for i,v in stats.items() if v['travel']],flush=True)
