from pathlib import Path
import json, math, sys
from plot_contours import read_moves, WALLS, plt, LineCollection, segment_distance
root=Path('sandboxes/continuous_print/verify')
out=root/'handoff_after'
old=read_moves('圆柱体_PLA_12m40s.gcode')
new=read_moves(out/'plate_1.gcode')
report={}
report['layer3_last_role']=new[3][-1]['role']
report['layer3_end']=new[3][-1]['b']
report['layer4_start']=new[4][0]['a']
report['fixed_z_layers']=sum(all(abs(m['z']-i*.2)<1e-6 for m in rows) for i,rows in new.items())
key=lambda rows:[(m['a'],m['b'],m['e'],m['role']) for m in rows]
report['hex_sparse_5_69_changed']=[i for i in range(5,70) if key(old[i])!=key(new[i])]
# The incoming contour arc precedes the native sparse trace. Drop its two short radial links.
first_fill=next(i for i,m in enumerate(new[4]) if m['role']=='Sparse infill')
arc=new[4][1:first_fill-1]
fills=[m for m in new[4] if m['role']=='Sparse infill']
report['layer4_arc_segments']=len(arc)
report['layer4_fill_arc_min_distance']=min(segment_distance(m['a'],m['b'],a['a'],a['b']) for m in fills for a in arc)
cold=read_moves(root/'contour_final/plate_1.gcode')
cnew=read_moves(out/'cylinder/plate_1.gcode')
report['cylinder_sparse_5_124_changed']=[i for i in range(5,125) if key(cold[i])!=key(cnew[i])]
fig,axs=plt.subplots(1,2,figsize=(12,5.5))
for ax,title,data in zip(axs,['Before: extra ending and hidden travel','After: true ending and contour entrance'],[old,new]):
 rows=data[4]
 ax.add_collection(LineCollection([[m['a'],m['b']] for m in rows],colors='#aab8c3',linewidths=.7))
 end=data[3][-1]['b'];ax.scatter(*end,c='#008a93',s=30,zorder=5,label='Layer 3 endpoint')
 if data is old:
  tail=[]
  for m in reversed(data[3]):
   if m['role'] not in WALLS:break
   tail.append(m)
  highlight=tail+rows[:1]
 else: highlight=rows[:first_fill]
 ax.add_collection(LineCollection([[m['a'],m['b']] for m in highlight],colors='#e74736',linewidths=2))
 ax.set(aspect='equal',xlim=(117,143),ylim=(118,142),title=title)
 ax.legend(loc='lower left');ax.axis('off')
fig.tight_layout();fig.savefig(out/'layer3_4_handoff.png',dpi=170);plt.close(fig)
fig,ax=plt.subplots(figsize=(10,3.2))
for title,data,color in [('Before: spiral Z',old,'#d45946'),('After: fixed layer Z',new,'#008a93')]:
 xs=[];zs=[]
 for i in (3,4,5):
  rows=data[i]
  for j,m in enumerate(rows):xs.append(i+j/len(rows));zs.append(m['z'])
 ax.plot(xs,zs,label=title,color=color,lw=2)
ax.set(xlabel='Layer (fraction shows progress within layer)',ylabel='Z (mm)',xticks=[3,4,5,6])
ax.grid(alpha=.2);ax.legend();fig.tight_layout();fig.savefig(out/'fixed_z_comparison.png',dpi=160);plt.close(fig)
(out/'geometry_report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(json.dumps(report,indent=2))
assert report['layer3_last_role']=='Internal solid infill'
assert report['layer3_end']==report['layer4_start']
assert report['fixed_z_layers']==75
assert report['layer4_fill_arc_min_distance']>=.448
