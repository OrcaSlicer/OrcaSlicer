from pathlib import Path
import sys, re, math, json
sys.path.insert(0, str(Path(__file__).parent/'plot_deps'))
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.collections import LineCollection

def segment_distance(a,b,c,d):
    def cross(p,q,r):return (q[0]-p[0])*(r[1]-p[1])-(q[1]-p[1])*(r[0]-p[0])
    def point_dist(p,u,v):
        dx,dy=v[0]-u[0],v[1]-u[1]
        n=dx*dx+dy*dy
        t=max(0.,min(1.,((p[0]-u[0])*dx+(p[1]-u[1])*dy)/n)) if n else 0.
        return math.hypot(p[0]-u[0]-t*dx,p[1]-u[1]-t*dy)
    if cross(a,b,c)*cross(a,b,d)<0 and cross(c,d,a)*cross(c,d,b)<0:return 0.
    return min(point_dist(a,c,d),point_dist(b,c,d),point_dist(c,a,b),point_dist(d,a,b))

WALLS={'Outer wall','Inner wall','Overhang wall'}
def read_moves(path):
    x=y=z=e=0.;rel=False;layer=0;role='';width=.42;rows={}
    for no,raw in enumerate(Path(path).read_text(encoding='utf-8').splitlines(),1):
        if raw.strip() in (';LAYER_CHANGE','; CHANGE_LAYER'):layer+=1;rows[layer]=[]
        if raw.startswith(';TYPE:'):role=raw[6:].strip()
        if raw.startswith('; FEATURE: '):role=raw[11:].strip()
        if raw.startswith(';WIDTH:'):width=float(raw[7:])
        c=raw.split(';')[0].split()
        if not c:continue
        if c[0]=='M83':rel=True
        if c[0]=='M82':rel=False
        v={k:float(s) for k,s in re.findall(r'([XYZE])(-?\d*\.?\d+)',raw.split(';')[0])}
        if c[0]=='G92':e=v.get('E',e);continue
        if c[0] not in ('G0','G1','G2','G3'):continue
        nx,ny,nz,ne=v.get('X',x),v.get('Y',y),v.get('Z',z),v.get('E',0 if rel else e)
        de=ne if rel else ne-e
        if layer and role not in ('','Custom','Skirt','Brim') and math.hypot(nx-x,ny-y)>1e-6 and de>0:
            rows[layer].append(dict(line=no,a=(x,y),b=(nx,ny),e=de,z=nz,role=role,width=width))
        x,y,z=nx,ny,nz
        if not rel:e=ne
    return rows

def centered(rows):
    pts=[p for m in rows if m['role']=='Outer wall' for p in (m['a'],m['b'])]
    cx=(min(p[0] for p in pts)+max(p[0] for p in pts))/2
    cy=(min(p[1] for p in pts)+max(p[1] for p in pts))/2
    return [dict(m,a=(m['a'][0]-cx,m['a'][1]-cy),b=(m['b'][0]-cx,m['b'][1]-cy)) for m in rows]

if __name__=='__main__':
    old,new=read_moves(sys.argv[1]),read_moves(sys.argv[2])
    out=Path(sys.argv[3]);out.mkdir(exist_ok=True,parents=True)
    selected=[1,2,3,126,127,128,129,130]
    fig,axs=plt.subplots(2,4,figsize=(16,8.5))
    report=[]
    for layer,ax in zip(selected,axs.flat):
        before=centered(old[layer]);after=centered(new[layer])
        base=min(math.hypot(*m['b']) for m in before if m['role'] in WALLS)
        arcs=[m for m in after if m['role'] in WALLS and
              max(math.hypot(*m['a']),math.hypot(*m['b'])) < base-.15 and
              abs(math.hypot(*m['a'])-math.hypot(*m['b'])) < .012]
        for kind,color,lw in [('fill','#acb9c3',.48),('wall','#303c48',.9),('arc','#e74736',1.7)]:
            moves=arcs if kind=='arc' else [m for m in after if (m['role'] in WALLS)==(kind=='wall')]
            ax.add_collection(LineCollection([[m['a'],m['b']] for m in moves],colors=color,linewidths=lw))
        p=after[0]['a'];ax.scatter(*p,color='#007c87',s=25,zorder=5)
        ax.set(aspect='equal',xlim=(-13.5,13.5),ylim=(-13.5,13.5),title=f'Layer {layer}: {after[0]["role"]}')
        ax.axis('off')
        radius=[math.hypot(*m['b']) for m in arcs]
        gaps=[min(segment_distance(m['a'],m['b'],arc['a'],arc['b']) for arc in arcs)
              for m in after if m['role'] not in WALLS] if arcs else []
        report.append(dict(layer=layer,first_role=after[0]['role'],first_length=math.dist(after[0]['a'],after[0]['b']),
                           arc_segments=len(arcs),arc_radius=(min(radius),max(radius)) if radius else None,
                           min_infill_distance_to_arc=min(gaps) if gaps else None))
    fig.suptitle('Solid layers: contour extensions (red), start (teal), infill (grey)',fontsize=15)
    fig.tight_layout();fig.savefig(out/'solid_layers.png',dpi=160);plt.close(fig)
    # Show the user's reported third-layer start and the corrected toolpath side by side.
    fig,axs=plt.subplots(1,2,figsize=(12,6))
    for label,data,ax in [('Before',old[3],axs[0]),('After',new[3],axs[1])]:
        rows=centered(data)
        ax.add_collection(LineCollection([[m['a'],m['b']] for m in rows],colors='#b8c0c7',linewidths=.55))
        early=rows[:25]
        ax.add_collection(LineCollection([[m['a'],m['b']] for m in early],colors='#e74736',linewidths=1.8))
        ax.scatter(*rows[0]['a'],color='#007c87',s=30,zorder=5)
        ax.set(aspect='equal',xlim=(-13.5,13.5),ylim=(-13.5,13.5),title=f'{label}: first 25 extrusion moves')
        ax.axis('off')
    fig.tight_layout();fig.savefig(out/'layer3_comparison.png',dpi=160);plt.close(fig)
    (out/'geometry_audit.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps(report,indent=2))
