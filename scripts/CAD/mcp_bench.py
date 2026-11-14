#!/usr/bin/env python3
"""Score card for the Orca-CAD MCP: how cheaply, quickly, correctly, completely and safely an agent can build with it.

Design record: docs/CAD/IDEAS-mcp-benchmark.md (claim, rivals, why each metric). Drives a RUNNING scratch app:

    ORCA_CAD_MCP=/tmp/orca-nd.sock ORCA_CAD_MCP_UI=1 <app>      # a rig; it creates and discards documents
    python3 scripts/CAD/mcp_bench.py [socket] [--json out.json] [--baseline base.json] [--only A,S,T,R,I]

Families: S steps, T speed, A accuracy (vs closed form), C coverage, R safety (fuzz), I integrity (determinism, undo), M memory and resource guard.
Exit 0 only if the critical gates (R, I and A) pass; S/T/C are scored against targets and reported as REGRESS when they
get worse than --baseline by more than 15%. Numbers that depend on machine load are medians of repeats.
"""
import json, math, socket, statistics, subprocess, sys, time, itertools, os

args = [a for a in sys.argv[1:] if not a.startswith('--')]
SOCK = args[0] if args else '/tmp/orca-nd.sock'
def opt(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default
ONLY = set((opt('--only', 'S,T,A,C,R,I,M')).split(','))
_ids = itertools.count(1)
LOG = []                      # (method, ms, ok): one entry per request
INNER = []                    # one entry per command actually run: a batch of N adds N (batching must not game the step count)

def raw(method, params=None, timeout=300):
    s = socket.socket(socket.AF_UNIX); s.settimeout(timeout); s.connect(SOCK)
    t0 = time.perf_counter()
    s.sendall((json.dumps({'jsonrpc': '2.0', 'id': next(_ids), 'method': method, 'params': params if params is not None else {}}) + '\n').encode())
    b = b''
    while b'\n' not in b:
        d = s.recv(1 << 20)
        if not d: break
        b += d
    s.close()
    ms = (time.perf_counter() - t0) * 1000
    r = json.loads(b.split(b'\n')[0])
    LOG.append((method, ms, 'error' not in r))
    if method == 'batch' and isinstance(params, dict): INNER.extend((c.get('command') or c.get('method') or '?') for c in params.get('commands', []) if isinstance(c, dict))
    else: INNER.append(method)
    return r

def call(m, p=None):
    r = raw(m, p)
    if 'error' in r: raise RuntimeError(f"{m}: {r['error'].get('message', r['error'])}")
    return r['result']

def pct(xs, q):
    xs = sorted(xs); return xs[min(len(xs) - 1, int(round(q * (len(xs) - 1))))] if xs else float('nan')

RESULTS = {}      # metric id -> dict(value, target, op, ok, note)
def metric(mid, value, target, op, note='', critical=False):
    ok = {'<=': value <= target, '>=': value >= target, '==': value == target}[op]
    RESULTS[mid] = dict(value=value, target=target, op=op, ok=bool(ok), note=note, critical=critical)
    print(f"  {'PASS' if ok else 'FAIL'}  {mid:4s} {note:58s} {value if not isinstance(value, float) else round(value, 6)} {op} {target}")

def vol(b=0): return call('mass_properties', {'body': b})
def reset():
    try: call('sketch_cancel')
    except Exception: pass
    call('new_document')

# ------------------------------------------------------------------------------------------- the reference task (PCB + case + lid)
def var(n, e): call('set_variable', {'name': n, 'expr': str(e)})
def bind(f, field, e): return call('set_feature_expr', {'feature': f, 'field': field, 'expr': e})
def nm(i, n): call('rename_feature', {'feature': i, 'name': n})

def centered_rect(plane, L, W, Le, We, label):
    call('sketch_begin', {'plane': plane}); call('sketch_add', {'rect': [-L / 2, -W / 2, L / 2, W / 2]})
    call('sketch_constrain', {'atomic': False, 'constraints': [
        {'type': 'distance', 'a': '0.start', 'b': '0.end', 'value': L, 'orientation': 'horizontal'},
        {'type': 'distance', 'a': '1.start', 'b': '1.end', 'value': W, 'orientation': 'vertical'},
        {'type': 'distance', 'a': 'origin', 'b': '0.start', 'value': -L / 2, 'orientation': 'horizontal'},
        {'type': 'distance', 'a': 'origin', 'b': '0.start', 'value': -W / 2, 'orientation': 'vertical'}]})
    si = call('sketch_commit', {'as': label})['sketch_index']
    binds = {}
    for d in call('sketch_dimensions', {'sketch': si})['dimensions']:
        v = d['value']
        e = Le if abs(v - L) < 1e-9 else We if abs(v - W) < 1e-9 else f'-({Le})/2' if abs(v + L / 2) < 1e-9 else f'-({We})/2' if abs(v + W / 2) < 1e-9 else None
        if e: binds[f"dim:{d['index']}"] = e
    for f_, e_ in binds.items(): bind(si, f_, e_)
    return si

def rect_extrude(plane, L, W, Le, We, d, de, mode, name, sketch_name, body_name=None):
    """Centred, constrained, bound rectangle sketch + extrude: one command."""
    p = {'plane': plane, 'width': L, 'height': W, 'width_expr': Le, 'height_expr': We, 'distance': d, 'boolean': mode,
         'bind': {'distance': de}, 'as': name, 'sketch_as': sketch_name}
    if body_name: p['body_as'] = body_name
    return call('extrude_rect', p)['extrude_index']

def faces(b): return call('query_topology', {'body': b})['faces']
def face_where(b, nz, pick):
    fs = [f for f in faces(b) if f['kind'] == 'planar' and abs(f['normal'][2] - nz) < 1e-6]
    return pick(fs)['id']
_DATUMS = [0]
def datum(off, expr, label):
    n = _DATUMS[0]; _DATUMS[0] += 1
    call('plane', {'base': 0, 'offset': off, 'bind': {'plane_offset': expr}, 'as': label}); return n
def cs(body, face, label):
    return call('coordsys', {'type': 'face_and_direction', 'body': body, 'face': face, 'x_hint': [1, 0, 0], 'as': label})['coordsys_index']

def task_pcb_case_lid():
    _DATUMS[0] = 0
    """The PCB, its case and the lid with a 0.1 mm clearance, assembled by mates, everything bound to variables."""
    call('set_variable', {'variables': dict(pcb_L=60, pcb_W=40, pcb_T=1.6, hole_d=3.2, hole_inset=4, ic_L=14, ic_W=10, ic_H=3, wall=2, gap=0.5, case_h=14,
                     standoff_h=4, standoff_d=6, lid_t=2, lip_h=3, tol=0.1, case_L='pcb_L+2*(gap+wall)', case_W='pcb_W+2*(gap+wall)',
                     lip_L='pcb_L+2*gap-2*tol', lip_W='pcb_W+2*gap-2*tol')})
    rect_extrude('XY', 60, 40, 'pcb_L', 'pcb_W', 1.6, 'pcb_T', 'new', 'PCB board', 'PCB outline', 'PCB')
    h = call('hole', {'diameter': 3.2, 'through': True, 'x': 26, 'y': 16, 'plane': 'XY', 'body': 0, 'as': 'Mounting hole',
                      'bind': {'hole_diameter': 'hole_d', 'hole_x': 'pcb_L/2-hole_inset', 'hole_y': 'pcb_W/2-hole_inset'}})['hole_index']
    call('pattern', {'features': [h], 'axis': [1, 0, 0], 'count': 2, 'spacing': -52, 'axis2': [0, 1, 0], 'count2': 2, 'spacing2': -32, 'as': '4 mounting holes',
                     'bind': {'pattern_spacing': '-(pcb_L-2*hole_inset)', 'pattern_spacing2': '-(pcb_W-2*hole_inset)'}})
    d = datum(1.6, 'pcb_T', 'Board top plane'); rect_extrude(f'datum:{d}', 14, 10, 'ic_L', 'ic_W', 3, 'ic_H', 'union', 'IC package', 'IC outline')
    rect_extrude('XY', 65, 45, 'case_L', 'case_W', 14, 'case_h', 'new', 'Case block', 'Case outline', 'Case')
    call('shell', {'thickness': 2, 'face': {'normal': [0, 0, 1], 'pick': 'max_z'}, 'body': 1, 'bind': {'shell_thickness': 'wall'}, 'as': 'Hollow case'})
    d = datum(2, 'wall', 'Cavity floor plane')
    se = call('extrude_circle', {'plane': f'datum:{d}', 'diameter': 6, 'x': 26, 'y': 16, 'distance': 4, 'boolean': 'union', 'diameter_expr': 'standoff_d',
                                 'x_expr': 'pcb_L/2-hole_inset', 'y_expr': 'pcb_W/2-hole_inset', 'bind': {'distance': 'standoff_h'}, 'as': 'Standoff',
                                 'sketch_as': 'Standoff sketch'})['extrude_index']
    call('pattern', {'features': [se], 'axis': [1, 0, 0], 'count': 2, 'spacing': -52, 'axis2': [0, 1, 0], 'count2': 2, 'spacing2': -32, 'as': '4 standoffs',
                     'bind': {'pattern_spacing': '-(pcb_L-2*hole_inset)', 'pattern_spacing2': '-(pcb_W-2*hole_inset)'}})
    rect_extrude('XY', 60.8, 40.8, 'lip_L', 'lip_W', 3, 'lip_h', 'new', 'Lid lip', 'Lid lip outline', 'Lid')
    d = datum(3, 'lip_h', 'Lid plate plane'); rect_extrude(f'datum:{d}', 65, 45, 'case_L', 'case_W', 2, 'lid_t', 'union', 'Lid plate', 'Lid plate outline')
    c_floor = cs(1, {'normal': [0, 0, 1], 'pick': 'min_z'}, 'Case floor connector')
    c_pcb = cs(0, {'normal': [0, 0, 1], 'pick': 'min_z'}, 'PCB top connector')
    c_rim = cs(1, {'normal': [0, 0, 1], 'pick': 'max_z'}, 'Case rim connector')
    c_lid = cs(2, {'normal': [0, 0, -1], 'pick': 'max_z'}, 'Lid underside connector')
    call('mate', {'kind': 0, 'cs_a': c_floor, 'cs_b': c_pcb, 'offset': 5.6, 'flip': False, 'bind': {'mate_offset': 'standoff_h+pcb_T'}, 'as': 'PCB on standoffs'})
    call('mate', {'kind': 0, 'cs_a': c_rim, 'cs_b': c_lid, 'offset': 0, 'flip': True, 'as': 'Lid on case rim'})
    for b, c, o in ((1, '#9fb4c7', .3), (2, '#e0a458', .4), (0, '#1f8a4c', .85)): call('set_body_color', {'body': b, 'color': c, 'opacity': o})

def task_signature():
    r = call('inspect_design')
    return dict(features=[(f['type'], f['name']) for f in r['features']], vols=[round(b['volume'], 6) for b in r['bodies']],
                bboxes=[[round(v, 6) for v in b['bbox']['min'] + b['bbox']['max']] for b in r['bodies']], err=r['error_feature'])

def family_S_T_I():
    print('S/T/I: the reference task (PCB + case + lid, parametric, assembled)')
    reset(); n0 = len(LOG); i0 = len(INNER); t0 = time.perf_counter()
    task_pcb_case_lid(); wall = time.perf_counter() - t0
    task_log = LOG[n0:]; inner = INNER[i0:]
    r = call('inspect_design'); feats = len(r['features'])
    sig1 = task_signature()
    if 'S' in ONLY:
        metric('S1', len(inner), 35, '<=', 'commands run to build PCB+case+lid (a batch counts each command)')
        metric('S2', round(len(inner) / max(1, feats), 3), 1.4, '<=', f'commands per feature ({feats} features)')
        metric('S4', len(task_log), 35, '<=', 'round trips (requests; a batch is one)')
        by = {}
        for m in inner: by[m] = by.get(m, 0) + 1
        book = sum(by.get(k, 0) for k in ('set_feature_expr', 'sketch_dimensions', 'rename_feature', 'set_variable', 'rename_body'))
        metric('S3', round(book / max(1, len(inner)), 3), 0.15, '<=', 'share of commands that are bookkeeping (bind/name/variable)')
        RESULTS['S1']['by_verb'] = dict(sorted(by.items(), key=lambda kv: -kv[1])[:12])
    if 'T' in ONLY:
        writes = [ms for m, ms, ok in task_log if m not in ('query_topology', 'describe_scene', 'inspect_design', 'sketch_dimensions', 'mass_properties')]
        metric('T2', round(pct(writes, .95), 1), 500, '<=', f'write-call p95 ms (p50 {pct(writes, .5):.1f}, max {max(writes):.0f})')
        metric('T0', round(wall, 2), 30, '<=', 'wall seconds for the whole task')
        reads = {}
        for m, p in (('describe_scene', {}), ('query_topology', {'body': 0}), ('inspect_design', {}), ('mass_properties', {'body': 0})):
            xs = []
            for _ in range(60):
                t = time.perf_counter(); call(m, p); xs.append((time.perf_counter() - t) * 1000)
            reads[m] = xs
        allr = [x for xs in reads.values() for x in xs]
        metric('T1', round(pct(allr, .95), 1), 50, '<=', 'read-call p95 ms at 25 features (p50 %.1f)' % pct(allr, .5))
        # T3/T4: rebuild scaling and replay cache
        times = {}
        for n in (10, 20, 40):
            reset(); var('w', 10)
            for i in range(n):
                e = call('extrude', {'width': 10, 'height': 10, 'distance': 5, 'boolean': 'new'})['extrude_index']
                if i == 0: bind(e, 'width', 'w')
            xs = []
            for k in range(5):
                t = time.perf_counter(); var('w', 10 + 0.01 * (k + 1)); xs.append((time.perf_counter() - t) * 1000)
            times[n] = statistics.median(xs)
        expo = math.log(times[40] / times[10]) / math.log(4) if times[10] > 0 else float('nan')
        metric('T3', round(expo, 2), 1.3, '<=', f'rebuild scaling exponent 10->40 features ({times[10]:.0f}/{times[20]:.0f}/{times[40]:.0f} ms)')
        t = time.perf_counter(); var('w', 10.5); first = (time.perf_counter() - t) * 1000
        t = time.perf_counter(); var('w', 10.5); second = (time.perf_counter() - t) * 1000
        metric('T4', round(second / first, 3), 0.5, '<=', f'no-change rebuild vs first ({second:.0f}/{first:.0f} ms)')
    if 'I' in ONLY:
        reset(); task_pcb_case_lid(); sig2 = task_signature()
        metric('I1', int(sig1 == sig2), 1, '==', 'same task twice: identical features, volumes, boxes', critical=True)
        gp = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'mcp_bench_golden.json')
        mine = {'vols': [round(v, 3) for v in sig2['vols']], 'bboxes': [[round(v, 3) for v in b] for b in sig2['bboxes']]}
        if '--write-golden' in sys.argv: json.dump(mine, open(gp, 'w'))
        if os.path.exists(gp): metric('I4', int(json.load(open(gp)) == mine), 1, '==', 'optimised task builds the same bodies as the golden (volumes, boxes)', critical=True)
        # A2: every bound variable still drives its measure (a macro that drops a binding fails here)
        def lip_gap(): return call('min_distance', {'a': 2, 'b': 1, 'move_b': [0, 0, -2]})['distance']
        def bb(i): d = call('describe_scene')['bodies'][i]['bbox']; return d['min'] + d['max']
        checks = [('tol', '0.3', lambda: lip_gap(), 0.3), ('pcb_L', '70', lambda: bb(1)[3] - bb(1)[0], 75.0), ('wall', '2.5', lambda: bb(1)[3] - bb(1)[0], 65.0 + 1.0),
                  ('standoff_h', '5', lambda: bb(0)[2], 7.0), ('case_h', '16', lambda: bb(1)[5], 16.0)]
        good = 0
        for name_, val, probe, want in checks:
            old = call('inspect_design')['variables'][name_]
            var(name_, val); got = probe(); var(name_, old)
            if abs(got - want) < 1e-3: good += 1
            else: print(f'    A2: {name_}={val}: measure {got} expected {want}')
        metric('A2', good, len(checks), '==', 'bound variables that move their measure (tol, pcb_L, wall, standoff_h, case_h)', critical=True)
        # undo returns to the previous state
        reset(); var('a', 5)
        e = call('extrude', {'width': 10, 'height': 10, 'distance': 5, 'boolean': 'new'})['extrude_index']
        before = task_signature()
        for i in range(3): call('extrude', {'width': 4, 'height': 4, 'distance': 2 + i, 'boolean': 'new'})
        for i in range(3): call('undo')
        metric('I2', int(task_signature() == before), 1, '==', 'three features added then three undos: state identical', critical=True)
        for i in range(3): call('redo')
        metric('I3', int(len(call('inspect_design')['bodies']) == 4), 1, '==', 'redo brings the three bodies back', critical=True)
    return task_log

# ------------------------------------------------------------------------------------------------------------- accuracy
def rel(a, b): return abs(a - b) / max(abs(b), 1e-12)
ACC = []
def acc(name, got, want, tol=1e-6): ACC.append((name, got, want, rel(got, want), tol))

def family_A():
    print('A: accuracy against closed forms')
    pi = math.pi
    def prim(shape, **kw):
        reset(); call('primitive', dict(shape=shape, **kw)); return call('mass_properties', {'body': 0})
    m = prim('box', width=10, depth=20, height=30); acc('box volume', m['volume'], 6000); acc('box area', m['surface_area'], 2 * (200 + 300 + 600))
    m = prim('cylinder', radius=5, height=10); acc('cylinder volume', m['volume'], pi * 25 * 10); acc('cylinder area', m['surface_area'], 2 * pi * 5 * 10 + 2 * pi * 25)
    m = prim('sphere', radius=5); acc('sphere volume', m['volume'], 4 / 3 * pi * 125, 1e-5); acc('sphere area', m['surface_area'], 4 * pi * 25, 1e-5)
    m = prim('cone', radius=6, radius2=2, height=9); acc('frustum volume', m['volume'], pi * 9 / 3 * (36 + 12 + 4), 1e-5)
    m = prim('torus', radius=10, tube_radius=2); acc('torus volume', m['volume'], 2 * pi * pi * 10 * 4, 1e-5)
    reset(); call('primitive', {'shape': 'box', 'width': 10, 'depth': 10, 'height': 10}); call('transform', {'body': 0, 'scale': 2.5})
    acc('scale 2.5 volume', vol()['volume'], 1000 * 2.5 ** 3)
    reset(); call('primitive', {'shape': 'box', 'width': 20, 'depth': 20, 'height': 10})
    top = [f for f in faces(0) if f['normal'][2] > .99][0]['id']; call('push_pull', {'face': top, 'distance': 5}); acc('push +5', vol()['volume'], 4000 + 2000)
    top = [f for f in faces(0) if f['normal'][2] > .99][0]['id']; call('push_pull', {'face': top, 'distance': -3}); acc('pull -3', vol()['volume'], 6000 - 1200)
    reset(); call('primitive', {'shape': 'box', 'width': 20, 'depth': 20, 'height': 10}); call('hole', {'diameter': 4, 'through': True, 'x': 0, 'y': 0, 'plane': 'XY', 'body': 0})
    acc('through hole', vol()['volume'], 4000 - pi * 4 * 10)
    reset(); call('primitive', {'shape': 'box', 'width': 20, 'depth': 20, 'height': 20})
    t = [f for f in faces(0) if f['normal'][2] > .99][0]['id']; call('shell', {'thickness': 2, 'face': t, 'body': 0})
    acc('shell', vol()['volume'], 8000 - 16 * 16 * 18)
    reset(); call('primitive', {'shape': 'box', 'width': 20, 'depth': 20, 'height': 10})
    e = [x for x in call('query_topology', {'body': 0})['edges'] if x['kind'] == 'line' and abs(x['p0'][2] - x['p1'][2]) > 5][0]['id']
    call('fillet', {'edge': e, 'radius': 3}); acc('fillet', vol()['volume'], 4000 - (1 - pi / 4) * 9 * 10)
    reset(); call('primitive', {'shape': 'box', 'width': 20, 'depth': 20, 'height': 10})
    e = [x for x in call('query_topology', {'body': 0})['edges'] if x['kind'] == 'line' and abs(x['p0'][2] - x['p1'][2]) > 5][0]['id']
    call('chamfer', {'edge': e, 'distance': 3}); acc('chamfer', vol()['volume'], 4000 - 0.5 * 9 * 10)
    reset(); call('primitive', {'shape': 'box', 'width': 10, 'depth': 10, 'height': 10}); call('primitive', {'shape': 'box', 'width': 10, 'depth': 10, 'height': 10, 'at': [5, 0, 0]})
    call('boolean', {'target': 0, 'tool': 1, 'op': 'union'}); acc('union of overlapping boxes', vol()['volume'], 1500)
    reset(); call('primitive', {'shape': 'box', 'width': 10, 'depth': 10, 'height': 10}); call('primitive', {'shape': 'box', 'width': 10, 'depth': 10, 'height': 10, 'at': [5, 0, 0]})
    call('boolean', {'target': 0, 'tool': 1, 'op': 'intersect'}); acc('intersection of overlapping boxes', vol()['volume'], 500)
    reset(); call('pipe', {'points': [[0, 0, 0], [0, 0, 50]], 'radius': 3}); acc('straight pipe', vol()['volume'], pi * 9 * 50)
    reset(); call('pipe', {'points': [[0, 0, 0], [0, 0, 30], [30, 0, 30]], 'radius': 2}); acc('bent pipe (both legs)', vol()['volume'], pi * 4 * 60, 0.03)
    reset(); R_, P_, H_, wr = 10.0, 5.0, 20.0, 1.0
    call('coil', {'radius': R_, 'pitch': P_, 'height': H_, 'wire_radius': wr}); acc('coil volume', vol()['volume'], pi * wr * wr * (H_ / P_) * math.hypot(2 * pi * R_, P_), 5e-3)
    reset(); call('sketch_begin', {'plane': 'XY'}); call('sketch_add', {'entities': [{'type': 'circle', 'center': [0, 0], 'radius': 10}]})
    s = call('sketch_commit')['sketch_index']; call('surface_extrude', {'sketch': s, 'distance': 10})
    rim = [x['id'] for x in call('query_topology', {'body': 0})['edges'] if x['kind'] != 'line' and abs(x['p0'][2] - 10) < 1e-6][0]
    a0 = vol()['surface_area']; call('surface_extend', {'edge': rim, 'distance': 5}); acc('curved surface_extend area', vol()['surface_area'] - a0, 2 * pi * 10 * 5, 2e-3)
    reset(); call('primitive', {'shape': 'box', 'width': 10, 'depth': 10, 'height': 10}); call('primitive', {'shape': 'box', 'width': 10, 'depth': 10, 'height': 10, 'at': [20, 0, 0]})
    acc('min_distance of two boxes 10 apart', call('min_distance', {'a': 0, 'b': 1})['distance'], 10)
    reset(); call('primitive', {'shape': 'box', 'width': 10, 'depth': 20, 'height': 30})
    com = call('mass_properties', {'body': 0})['center_of_mass']; bb = call('describe_scene')['bodies'][0]['bbox']
    acc('box centre of mass z', com[2], bb['min'][2] + 15)
    worst = max(a[3] / a[4] for a in ACC)
    for name, got, want, r, tol in ACC:
        if r > tol: print(f'    off: {name}: got {got} want {want} rel {r:.2e} > {tol}')
    metric('A1', round(worst, 4), 1.0, '<=', f'worst error / its tolerance over {len(ACC)} closed-form checks', critical=True)
    metric('A1n', sum(1 for a in ACC if a[3] <= a[4]), len(ACC), '==', 'closed-form checks inside tolerance', critical=True)

# --------------------------------------------------------------------------------------- coverage, safety (fuzz)
def tools():
    d = call('describe_tools'); return d.get('tools', d)

def family_C():
    print('C: coverage')
    names = [t['name'] for t in tools()]
    ok = {m for m, ms, good in LOG if good}
    hit = [n for n in names if n in ok]
    metric('C1', round(len(hit) / len(names), 3), 0.9, '>=', f'verbs with a passing call in this run ({len(hit)}/{len(names)})')
    RESULTS['C1']['untouched'] = sorted(set(names) - ok)
    thin = [t['name'] for t in tools() if len(t.get('summary', '')) < 25]
    metric('C2', len(thin), 0, '<=', 'verbs whose description is shorter than 25 characters')

def family_R():
    print('R: safety (fuzz); document must be unchanged by a refused call')
    here = os.path.dirname(os.path.abspath(__file__))
    if os.environ.get('SKIP_R1'):   # the 20,000-call checker takes ~15 min: skippable when only R2/R3 are wanted
        p = subprocess.CompletedProcess([], 0, '', '')
    else:
      p = subprocess.run([sys.executable, os.path.join(here, 'check-mcp-hostile.py'), SOCK], capture_output=True, text=True, timeout=3600)
    if os.environ.get('SKIP_R1'): print('  SKIP  R1   check-mcp-hostile.py not run (SKIP_R1 set): the score does NOT include it')
    else:
        metric('R1', p.returncode, 0, '==', 'check-mcp-hostile.py (junk args never kill the app, owners named, paths fenced)', critical=True)
        if p.returncode: print('\n'.join('    ' + l for l in p.stdout.splitlines()[-8:]))
    skip = ('save', 'open', 'export', 'screenshot', 'new_document', 'import_step', 'import_mesh', 'undo', 'redo', 'ui.', 'batch', 'run_verb', 'execute', 'variables_import')
    bad = []; vague = 0; refused = 0
    junk = [{}, {'face': 'x', 'body': 'y', 'distance': 'z', 'edge': [None]}, {'distance': -1e9, 'body': 99999, 'feature': -5, 'face': 2 ** 40}]
    for t in tools():
        n = t['name']
        if n.startswith(skip) or n in skip: continue
        reset(); call('primitive', {'shape': 'box', 'width': 10, 'depth': 10, 'height': 10})
        for j in junk:
            before = (call('inspect_design')['features'], call('describe_scene')['generation'])
            r = raw(n, j, timeout=60)
            if 'error' in r:
                refused += 1
                msg = str(r['error'].get('message', ''))
                if len(msg) < 15: vague += 1
                after = (call('inspect_design')['features'], call('describe_scene')['generation'])
                if after[0] != before[0]:
                    bad.append(f'{n}({json.dumps(j)[:40]}) changed the feature list on refusal')
            try: call('describe_scene')
            except Exception as e: bad.append(f'{n}: app stopped answering ({e})'); break
        try: call('sketch_cancel')
        except Exception: pass
    metric('R2', len(bad), 0, '==', f'refused calls that changed the document ({refused} refusals)', critical=True)
    for b in bad[:10]: print('    ' + b)
    metric('R3', round(vague / max(1, refused), 3), 0.05, '<=', 'refusals whose message is shorter than 15 characters')

# --------------------------------------------------------------------------------------------- memory and resource guard
def family_M():
    print('M: memory and resource guard (the app is capped by its cgroup; every loop here stops itself at 4 GB)')
    def mem(): return call('inspect_design')['memory_mb']
    HARD = 4000
    reset(); base = mem(); task_pcb_case_lid(); after = mem()
    metric('M1', after, 2500, '<=', f'resident MB after the PCB+case+lid task (idle {base} MB)')
    seen = []
    for i in range(8):
        reset(); task_pcb_case_lid(); seen.append(mem())
        if seen[-1] > HARD: print('    stopped: over', HARD, 'MB'); break
    growth = seen[-1] - seen[1] if len(seen) > 2 else 0
    metric('M2', growth, 400, '<=', f'resident MB growth over {len(seen)} build + new_document cycles ({seen[0]}..{seen[-1]})')
    reset()
    t0 = time.time(); n = 0; refused = None
    while time.time() - t0 < 120 and mem() < HARD:
        r = raw('primitive', {'shape': 'box', 'width': 2, 'depth': 2, 'height': 2, 'at': [n * 3 % 300, n * 3 // 300 * 3, 0]})
        if 'error' in r: refused = r['error'].get('message', ''); break
        n += 1
        if n > 260: break
    ok_guard = refused is not None and 'size guard' in refused and 190 <= n <= 205
    metric('M3', int(ok_guard), 1, '==', f'the body cap refuses at 200 bodies with a clear message (refused after {n}: {str(refused)[:60]})', critical=True)
    still = raw('undo'); ok_undo = 'error' not in still
    metric('M4', int(ok_undo), 1, '==', 'undo still passes once the cap is hit (the caller can always get back under it)', critical=True)
    # M6: the refused-call guarantee must survive the undo cap (once the stack is full its depth stops changing, so a rollback keyed on it stops working)
    reset(); call('set_variable', {'name': 'a', 'expr': '1'})
    for i in range(260): call('set_variable', {'name': 'a', 'expr': str(2 + i)})
    call('primitive', {'shape': 'box', 'width': 4, 'depth': 4, 'height': 4})
    before = [(f['type'], f['name']) for f in call('inspect_design')['features']]
    refusals = [raw('plane', {'base': 'x'}), raw('plane', {'offset': 'x', 'base': 0}), raw('axis', {'type': 'two_points', 'body': 'x'}), raw('coordsys', {'type': 'face_and_direction', 'body': 'x', 'face': 'y'})]
    after = [(f['type'], f['name']) for f in call('inspect_design')['features']]
    metric('M6', int(before == after), 1, '==', f'refused calls leave the document unchanged even past the undo cap ({len(before)} features, {sum("error" in r for r in refusals)}/4 refused)', critical=True)
    reset()
    junk = raw('coil', {})
    metric('M5', int('error' in junk), 1, '==', 'coil with no parameters is refused instead of building a default coil', critical=True)

# ----------------------------------------------------------------------------------------------------------- report
if __name__ == '__main__':
    t_start = time.time()
    try: call('describe_scene')
    except Exception as e: print('no app on', SOCK, e); sys.exit(2)
    if ONLY & set('STI'): family_S_T_I()
    if 'A' in ONLY: family_A()
    if 'M' in ONLY: family_M()
    if 'R' in ONLY: family_R()
    if 'C' in ONLY: family_C()
    base = json.load(open(opt('--baseline'))) if opt('--baseline') and os.path.exists(opt('--baseline')) else None
    regress = []
    if base:
        for k, v in RESULTS.items():
            b = base.get(k)
            if b and isinstance(v['value'], (int, float)) and isinstance(b['value'], (int, float)) and b['value']:
                worse = (v['value'] > b['value'] * 1.15) if v['op'] == '<=' else (v['value'] < b['value'] * 0.85) if v['op'] == '>=' else False
                # Calibrated on 5 repeats of one build (2026-11-01): T0 spread 1.19x, T1 1.91x, T2 1.49x, T3 1.33x, T4 1.69x. The speed family is
                # flagged only beyond 2.2x. T2 (p95 per CALL) is not comparable across API shapes: a macro that does ten steps in one call is
                # a bigger call, so T0 (whole-task wall time) is the speed gate; T4 sits near 1.0 and is a backlog item, not a regression gate.
                if k in ('T2', 'T4'): continue
                if worse and not k.startswith(('T',)): regress.append(f"{k}: {b['value']} -> {v['value']}")
                elif worse and k.startswith('T') and v['value'] > b['value'] * 2.2: regress.append(f"{k}: {b['value']} -> {v['value']}")
    crit_fail = [k for k, v in RESULTS.items() if v['critical'] and not v['ok']]
    soft_fail = [k for k, v in RESULTS.items() if not v['critical'] and not v['ok']]
    print(f"\nSCORE  {sum(v['ok'] for v in RESULTS.values())}/{len(RESULTS)} pass | critical failing: {crit_fail or 'none'} | targets missed: {soft_fail or 'none'} | regressions: {regress or 'none'} | {time.time() - t_start:.0f}s")
    if opt('--json'): json.dump(RESULTS, open(opt('--json'), 'w'), indent=1, default=str)
    sys.exit(1 if crit_fail or regress else 0)
