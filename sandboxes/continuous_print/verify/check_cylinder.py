"""Inspect every model layer, including flat bottom and fallback surface layers."""
import json
import math
from pathlib import Path
import re
import sys

def inspect(path):
    x = y = z = e = 0.0
    relative_e = relative_xyz = False
    role = ''
    layers = []
    layer = None
    pending = []
    seen = False
    pending_between = []
    last_model_layer = None
    for lineno, raw in enumerate(Path(path).read_text(encoding='utf-8').splitlines(), 1):
        if raw.strip() in (';LAYER_CHANGE', '; CHANGE_LAYER'):
            layer = dict(number=len(layers)+1, travel=[], boundary_travel=[], roles=[], arcs=0, zmin=None, zmax=None, z_backwards=[], extrusion={}, first_line=None)
            layers.append(layer)
            pending, seen = [], False
        if raw.startswith(';TYPE:'):
            role = raw[len(';TYPE:'):].strip()
        elif raw.startswith('; FEATURE: '):
            role = raw[len('; FEATURE: '):].strip()
        code = raw.split(';')[0].split()
        if not code:
            continue
        cmd = code[0]
        if cmd == 'M83': relative_e = True
        if cmd == 'M82': relative_e = False
        if cmd == 'G91': relative_xyz = True
        if cmd == 'G90': relative_xyz = False
        vals = {k:float(v) for k,v in re.findall(r'([XYZE])(-?\d*\.?\d+)', raw.split(';')[0])}
        if cmd == 'G92':
            x, y, z, e = (vals.get(k, old) for k, old in zip('XYZE', (x,y,z,e)))
            continue
        if cmd not in ('G0', 'G1', 'G2', 'G3'):
            continue
        nx, ny, nz = (old+vals.get(k,0) if relative_xyz else vals.get(k,old) for k,old in zip('XYZ',(x,y,z)))
        ne = vals.get('E', 0 if relative_e else e)
        de = ne if relative_e else ne-e
        moved = math.hypot(nx-x, ny-y) > 1e-7 or cmd in ('G2','G3')
        if layer and role not in ('','Custom','Skirt','Brim') and moved:
            if de > 1e-8:
                if last_model_layer is not None and last_model_layer != layer['number']:
                    layer['boundary_travel'].extend(pending_between)
                last_model_layer = layer['number']
                pending_between = []
                if layer['first_line'] is None: layer['first_line'] = lineno
                layer['travel'].extend(pending)
                pending = []
                seen = True
                if not layer['roles'] or layer['roles'][-1] != role: layer['roles'].append(role)
                layer['arcs'] += cmd in ('G2','G3')
                if layer['zmax'] is not None and nz < layer['zmax'] - 1e-5: layer['z_backwards'].append(lineno)
                layer['zmin'] = nz if layer['zmin'] is None else min(layer['zmin'],nz)
                layer['zmax'] = nz if layer['zmax'] is None else max(layer['zmax'],nz)
                layer['extrusion'][role] = layer['extrusion'].get(role,0) + de
            elif seen:
                pending.append(dict(line=lineno, distance=math.hypot(nx-x,ny-y)))
        if last_model_layer is not None and moved and de <= 1e-8:
            pending_between.append(dict(line=lineno, distance=math.hypot(nx-x,ny-y)))
        x, y, z = nx, ny, nz
        if not relative_e: e = ne
    return layers

if __name__ == '__main__':
    for path in sys.argv[1:]:
        layers = inspect(path)
        Path(str(path)+'.audit.json').write_text(json.dumps(layers,ensure_ascii=False,indent=2),encoding='utf-8')
        print(path)
        print('zero travel:',sum(not x['travel'] for x in layers),'/',len(layers))
        print('travel layers:',[(x['number'],len(x['travel'])) for x in layers if x['travel']])
        print('z backwards:',[(x['number'],len(x['z_backwards'])) for x in layers if x['z_backwards']])
        print('boundary travel:',[(x['number'],len(x['boundary_travel'])) for x in layers if x['boundary_travel']])
        print('arcs:',sum(x['arcs'] for x in layers))
        for x in layers[:3]+layers[-5:]:
            print('layer',x['number'],'travel',len(x['travel']),'Z',x['zmin'],x['zmax'],'roles',list(dict.fromkeys(x['roles'])),'gap E',round(x['extrusion'].get('Gap infill',0),5))
