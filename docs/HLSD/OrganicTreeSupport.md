# Organic tree support

Organic supports combine a routed tree with printable support material. The tree
graph routes branch positions, radii and junctions around model collision masks. Slicing its
common silhouette determines the support walls; interface generation determines
which parts of that silhouette carry model contacts. These are separate geometric
constraints: overlapping influence areas or nominal branch discs do not establish
that an exported extrusion has material underneath it.

## Contact and routing domains

The overhang detector supplies the required contact region. A routing roof can be
larger, for example where sharp-tail handling reserves room for support heads.
`TreeSupport3D` retains both regions. The top contact follows the overhang; lower
interface layers may use the routing roof to reach real anchors outside the
contact's projection. Treating the entire routing roof as required top contact
creates unnecessary unsupported turns.

The thin axes that a dense contact must cover come from the overhang itself, not
from the beads grown around it, which would move the boundary of a wide core and
lose part of the place where a narrow arm joins it. A roof recovered from a
branch has no overhang to be checked against and is not checked.

Roof printability uses the selected contact pattern and its world origin. The
eventual interface index is assigned after collecting the support layers, so an
interlaced pattern is checked in both possible phases. Base interfaces on another
filament use their own pattern, with the interface spacing and angle used by the
final generator. They are distinct from ordinary base infill.

A dense contact is checked against the beads its pattern lays, with their width,
in every phase it may print in. The core of each part of the overhang, the part
more than a line inside its outline, must be covered: by Rectilinear and Grid
entirely, by SpiralInset and Concentric, whose fixed-width loops can leave pores
inside, along its whole boundary, holes included. A part too narrow for a core
must have its medial axis covered instead, and so must the narrow arms of a part
with a core, outside the core and within the overhang. For a part with a core the
axes are taken before short branches are pruned, so a wide junction does not hide a
narrow arm attached to it; a part with no core goes through ExPolygon::medial_axis,
which drops endpoint branches shorter than twice the widest section. The medial axis keeps its facing-edge rule, so the axis of a curved narrow
arm may come out short.

Where the pattern of a dense top contact or interface leaves a narrow part without
material, a bead along the medial axis of that part fills it, in the roof preview
as in the final extrusion. Its axis lies inside the area inset by half a bead, and
the beads already there, each with its own width, trim it; the pattern's paths
keep their flow. Rafts, sparse interfaces, bottom interfaces and base interfaces
on another filament are filled by their pattern alone. Whether the contact holds
is decided on the overhang, not on these added beads.

Where the contact of the overhang leaves part of it uncovered, or pores in a loop
pattern, the overhang grown by up to half a bead within the roof is tried too, and
the one that holds with the smaller uncovered core area in its worst phase is
used; a smaller uncovered area never makes up for an edge left without contact.
The whole contact is filled and checked at once, as joining islands changes the
loops of a loop pattern. A sparse contact keeps its gaps but must lay material on
every part of the overhang in every phase, so a broad part cannot hide a
neighbour the lines miss. Where the whole contact fails, dense or sparse, the
parts it holds keep it, checked again together, and the others, such as a speck
too small for a line beside a broad overhang, rest on the interface layer below,
as the overhangs that a sparse interface misses do. Otherwise one speck that the
routing allowance joins to a broad overhang would take the whole roof, and the
area would get a tip every half tip radius instead.

An overhang gets a roof of its own only where one layer of it holds interface
lines over the minimum roof area of 1 mm². Elsewhere the tips carry the
interface: each tip gets a disc of roof at its top, whatever its size. A gentle
slope overhangs every layer by slivers too thin for a roof of their own, at fine
layer heights thinner than an interface line, and an organic surface or the tip
of a horn splits them into pieces with a single tip each. CuraEngine gives a tip
smaller than the minimum roof area no roof, so such slopes would get an
interface only where the tip diameter exceeds 1.13 mm, and with the default
0.8 mm tips none at all.

At small top Z distances, a fine-layer slope can leave no admissible tip area
after the minimum XY clearance is applied: each layer overhangs the one below
it by a sliver within the clearance of the model below. Placement retries such
a fully blocked overhang on successively lower layers while retaining the
original overhang as the contact target. It stops at the first admissible
area, within twice the configured overhang slope's XY travel, at most 50 layers
or 3 mm, so the recovered tips lie that many layers lower. Collision and
build-plate reachability checks still apply on every attempted layer. A blocked
overhang is retried part by part, each part only where the ordinary placement
put no tip within twice the XY distance and a branch radius around it, down to
the layers a retry goes to, or above it, whose branch comes down past it, clear
of the model in between: a retried tip would merge into it at once, while a tip
over the model there comes down elsewhere, if at all. Retrying each blocked
layer beside existing contacts added tips under slivers a thousandth of a square
millimetre in area, which changed how the branches merged, and new trees under
slivers beside a branch coming down. A horn rising from the model, whose every
layer is blocked, keeps its contacts beside the other overhangs of the object,
whatever another horn far from it carries on the same layers.

Under a roof of its own the tips hold the interface, which bridges between
them, so how far its points lie from the nearest tip matters, at an edge and a
corner most, where nothing beyond holds it. CuraEngine samples the roof lines,
one zigzag through the whole area, keeping each tip the connection length (set
by Branch Density) from those before it. That guarantees no more than every
point of the lines within the connection length of a tip: the rows end
irregularly, every other row stops short of an edge, and a corner, such as one
of a notch cut out by the avoidance of a model part, can be left at the full
distance. Newer CuraEngine samples a sparser grid along its lines instead,
which covers the interface no better and puts tips almost on top of each other
where the lines cross. Orca places these tips by the shape of the area, inset
by half a line: one at every corner of 135° or sharper and evenly between, at
most the connection length apart; inside, a hexagonal lattice with that spacing,
fixed in the object's coordinates so that the tips of flat overhangs stacked
above each other line up, at least half the spacing from the outline; and one
wherever a point is still farther than 0.7 of the spacing from them all, which
inside the lattice never happens, as there no point lies farther than
spacing / √3. Only the band along the outline is searched for such points. No
tip stands closer to another than the tip diameter and a line, so their discs and
the beads around them stay apart; otherwise the tips along the outline of a small
area would merge into a ring around a cavity. The spacing is at least that
distance, and the points left farther than 0.7 of the spacing get a tip only where
they are also farther than it. Two points of the outline closer than it, across a
strip too narrow for two rows of tips or at the ends of an edge too short for
two, share one tip in their middle, which reaches both; a tip kept on one of them
would leave the interface hanging over the other edge by nearly the width of the
strip, and the branches gathering under such a lopsided row lean steeply. An
area whose outline, inset by half a line, lies within two lines of the disc of
one tip in its middle gets only that tip; a larger one gets several, between
which the interface bridges, rather than one it would overhang far. CuraEngine replaces up to three tips of an area by points on the
very edge of its outline, moved closer together until three fit; under a roof
of its own Orca keeps its tips and samples the outline only for an area
narrower than a line, which gets none.

Without a roof of its own an overhang gets its tips along the lines of the base
pattern through it, both ends of a line among them. CuraEngine walks each line
from its start by the connection length and keeps a point only that far from
those placed, the far end among them, so the last gap before the far end can
reach nearly twice the length: along an edge of a box standing on its edge every
row of tips misses one at the same end. Orca shortens the step to divide the way
to the far end evenly, so no gap is longer than the connection length.

Within one overhang part the tips stand apart, along its lines nearly the
connection length and under a roof of its own at least their diameter and a
line, but nothing keeps the tips of different parts and layers apart. A curved
surface splits the overhang of every layer into many small parts, and a slope
gives every layer tips a few hundredths of a millimetre from those of the layer
above, so tips crowd together far closer than their diameter and their branches
run side by side until they merge.

Overlapping influence areas are merged on every layer, so branches join on the
first layer they touch rather than running side by side, which under a low flat
overhang would leave more trees and more paths. The check costs little.

A failed roof does not consume a contact depth. Routing snapshots restore the
domain at the depth where tips must be inserted, and pending recovery preserves
the requested contact height and interface count. An overhang a few layers over
the bed may take roofs on every layer below its contact, leaving no room for a
branch; with no body on any layer, those roofs are kept as they are and stand on
the bed, as in BambuStudio. A recovered roof is the part
of the branch slice that the element sweeps since the layer below. The slices of
a path that leaves a junction sideways also hold the round end of the junction,
which for a wide trunk reaches up to the tip; it stays branch body.

Tips are placed in the overhang widened by the tip radius, so a tip can stand
beside the model inside a branch that passes there on its way down from higher
tips. A roof part inside the body of its layer, over which the body of the next
layer up continues as base rather than as roof, lies inside the support and
becomes branch body. Only whole roof parts go, as cutting a partly covered roof
leaves unprintable shreds. A removable part must be fully covered by the continuing
branch, allowing only the polygon clipping tolerance: even a thin outside tail can
carry a long printable interface line.
A roof in a hole of the body, at the bottom of a
cavity between branches, stays, and so does a roof that carries a wall of the
layer above with no outline of its own layer's body below, such as the wall of a
branch that separates inside a hollow trunk. The layers are walked from the top
down, so a whole stack of such roofs goes. The bottom interfaces lie inside the
body by design and stay: the roof parts that a stack of roofs rising from a bottom
contact reaches are left out of the walk.

## Branch routing

Each upper node has one lower neighbour, so the routed graph is a forest.
Influence areas grow by the per-layer movement of a branch, using round offsets
in obstacle-sized steps. At fine layer heights this movement is only a few
microns: at 0.02 mm and 40 degrees it is 17 microns, comparable to a fixed
10 micron arc tolerance and 15 micron simplification, which together could
cancel most of the growth. Both tolerances therefore scale with the offset
distance, to 1/16 and 1/8 of it, capped at 10 and 15 microns, which offsets of
0.16 and 0.12 mm reach. Rounding then removes at most 3/16 of a step, for steps
of 8 microns and more, below which the arc tolerance stays at half a micron, and
the vertex count of a round area stays bounded by its arc tolerance instead of
growing with every layer.

An influence area grows in steps of the XY distance, each clipped by the model,
so that it cannot pass through a thin wall of the model in one layer. Where the
Z distances leave at least a layer above and below the support, the step also
takes the Minimum feature size (`min_feature_size`), a percentage of the nozzle
diameter, as for the walls of the model.

Near the bed a branch widens towards the Initial Layer Diameter
(`tree_support_initial_layer_diameter`, CuraEngine's `support_tree_bp_diameter`),
7.5 mm by default, at about 40° and at most half a line width per layer. A
diameter not larger than the branch diameter leaves the branches as they are. As
in PrusaSlicer, a tree widens only until its base is as wide as the tree is tall,
so the trees under low overhangs do not get wide feet that would join them at the
bed; a branch already wider than that keeps its width.

Near the bed, where its foot is not to widen, a tree may move faster by the
growth of a foot a layer, as in CuraEngine, which leans low trees toward each
other so that they merge near the bed. This lean is no more than the step the
bodies grow by a layer, half a line or the branch angle where it sets more, less
the move of the branch angle, so nothing where the branch angle sets the step: a
tree leaning farther would leave its body behind, its leading side cut off. Only
the lean is limited so, not the whole move of a layer. The slow move gets no more
lean, though the branch angle bounds it already: with the whole growth of a foot
in it, more floating support islands were counted in some of the configurations
measured and fewer in none. At fine layers the whole growth of a foot fits; at
0.2 mm layers and 0.42 mm lines the lean is what half a line leaves of the branch
angle.

The Trunk Diameter (`tree_support_trunk_diameter`, CuraEngine's
`support_tree_max_diameter`) limits the radius of every branch, its foot included,
but not below the branch diameter. CuraEngine defaults it to 25 mm; here the
default 0 sets no limit, so existing projects keep their trees.

The branches are drawn as tubes with cross-sections perpendicular to their axis,
so a branch leaning by an angle θ slices into an oval at least 1 / cos θ as wide as
its disc in the direction it leans, and wider on its trailing side, where the rims
of the larger rings below rise to the slice. Branches whose influence areas have
not merged can therefore touch where they lean towards each other, even where their
discs stay apart, and the body of their layer joins them.

The cross-section at a node lies on the plane bisecting the two segments that
meet there. Where a thick branch bends sharply, two consecutive cross-sections
intersect, and the strip of triangles joining them folds over itself, so its
slices lose area and leave a gap in the branch. The path is split there, as in
BambuStudio: such a segment becomes a closed capsule of its own, overlapping the
parts of the path on either side at the nodes they share, and the slices of the
layer merge them.

A branch is sliced from the layer of its lowest node to the layer of its highest,
widened by the layers its round ends reach at their poles, and never past the
layers of the support, whose collisions are precalculated: computing a missing
one would grow the collision cache that branches drawn in parallel hold
references into. The layers a round end reaches only off its pole are not sliced.
Slicing them as well, as BambuStudio does, carries the top of a branch into the
layers of the roof over it, and the roof over a thin part of an overhang gets
gaps.

## Body outline and holes

The branch slices of a layer are unioned into one body, smoothed outward and
clipped again against the model collision, the bed and the belt floor. The wall
bead lies inside the body contour with its outer edge on it, and next to an
overhang the XY gap is only half an external perimeter wide, so without the clip
the smoothing would push the wall bead into the model.

A wall cannot go round a part of a body narrower than two lines without running
back over its own bead: a small branch merging into a body joins it through a
neck that narrow for many layers at fine layer heights, and the wall would run
into the neck and out again closer than a line. The walls go round the parts of
a body at least two lines wide, and the rest of it, the neck and the small branch
beyond, prints as one bead along its middle, as wide as it is, as the thin walls
of a model do. A body narrower than two lines all over, a speck or the tip of a
branch, keeps its loop. The wide parts are what is left of the body shrunk by a
line and grown back, kept within the body: grown back to sharp corners, the
sides of a bump too narrow to keep would meet past its top, and the wall round
them would print out of the body, into the model where the body lies at its
least distance from it.

A wall starts at its seam without the anchor other tree supports turn in along a
second loop a line inside it, on the infill of the body: inside the walls of an
Organic body there is no infill, so the anchor would be a hook hanging in the air.

Walls-only base patterns print a wall around every hole of the body. Converging
branches can enclose a cavity that closes towards the bed; the wall around the
lowest layers of such a hole rests on the empty interior of the trunk below.
Filling these holes, as CuraEngine's filterFloatingLines does, would instead
remove the material under the walls of branches that separate above the cavity,
which would then bridge, so such holes are kept; only the holes and slits below
are filled.

A hole is filled where the body of the layer above covers it, together with
everything inside, as the hole around a branch that merges into a trunk from
inside. This holds whatever the base pattern: in a hollow trunk the walls of such
a hole and of the branch carry nothing, and in a filled one they break up the
fill. A hole that goes on up keeps the part under the hole above where it is
wider than a line, so its walls stand under the walls above. A hole that the
model passes through stays, and so does one over or under the model within the
bottom or top Z distance, which cuts the bodies out there as well: filled, it
would put support against the model without the gap. A gap narrower than two
lines, a slit that opens to the outside or between two bodies, is filled only
where the body above covers it, as elsewhere it may be the notch where two
branches meet. Going down, that notch
closes where the branches merge into a hole, which may go on down for a few
layers before it closes over a solid body, and the wall of its lowest layer stands
on nothing in a hollow trunk. A pocket of holes narrower than two lines, whose top
does not go on above as a hole, is therefore filled over its whole height where it
closes over a solid body. That trades the floating loop for the walls of the notch
above, which then rest on the walls below only at the outline, as wherever a notch
closes going down: in a hollow trunk they hang from there over the interior for a
layer. A pocket that opens below into a wider hole or to the outside stays. One
that reaches out below under another hole going on up is filled too: the walls of
that hole stood on a chain of walls that floats at the bottom of the pocket, and
they now float a layer higher instead. The layers are walked from the top down, so
a hole under a filled one fills too.

## Common walls

When the base and a dense interface of a layer share the filament and line
width, the walls of the Organic body are generated once for the whole body,
before the interfaces take their areas: offsetting the two regions separately
would leave a notch where their boundary meets the outer contour. Organic bodies
always have walls, so this holds for every base pattern, but not on the first
layer, on raft layers, on bridging layers, or with Interface loop pattern
(`support_interface_loop_pattern`). Inside a dense
interface the wall takes the interface role; a stretch of the wall shorter than
a bead between two parts on the other side of the interface boundary takes their
role, so the wall does not change its role for a dot. The interface pattern fills
the whole interface, edge included, and the wall is printed only where its axis
lies outside the fill beads. Inside them the wall would pass over the fill again;
outside them the fill does not reach the axis. Where the fill runs along the
wall, the wall axis enters and leaves the fill beads every few microns. Stretches
inside them shorter than a bead therefore stay in the wall and pieces outside
them shorter than a bead between such stretches are left to the fill, so the
wall is not printed as a dashed line with a travel before every dash. The fill
may then leave a pore narrower than half a bead on the wall line.

## Dots

Clipping paths along the edges of beads, as for the common walls and the thin
parts of interfaces, leaves pieces of a few microns. Once all extrusions of a
support layer are collected, a path shorter than a quarter of its bead width is
removed unless both of its ends meet other paths. Such a piece adds next to
nothing to the layer, and the G-code does not reliably continue another path with
it, so it would be printed as a dot after a travel, and often a retraction, of
its own.

## Bodies under interfaces

Under an interface of its own the tips stand apart by Branch Density, and the
lowest interface layer bridges the gaps between them and the bodies they merge
into: at 20 % the gap between 0.8 mm tips is 0.88 mm, which at the fine layer
heights Organic supports are often printed with does not print. A line of that
layer bridges between what it passes over, not between what lies within reach of
it: a line running along the gap between two bodies passes over neither. So every
line of it is to touch the material below, by half a line, at least every span of
a line between two lines of the base pattern, and its ends are to rest on it, a
stretch ending in the air no longer than a line. The lines run in the directions
the interface pattern gives them: one with Rectilinear and the default, two with
Rectilinear Interlaced, which turns them from layer to layer, and with Grid. Their
positions are not known before the paths, so the lines are taken a quarter of a
line apart, each touching what lies within half a line less half that pitch of it.
A stretch also stops short of that span by as much on both sides, so that a point
midway still lies within half the base pattern spacing and a line of what holds
it. A concentric or spiral interface prints no lines; under it every point is to
lie within that distance instead. Where a line hangs, the top layers of the bodies
near it widen going up
by the branch angle, by half a line a layer at most, the overhang a wall prints
over the wall below it, over as many layers as they need, up to two and a half
lines in all, so the bodies end in flares. A wall leaning more than the branch
angle curls up at fine layers. A layer lower down widens no farther from its own
bodies than its steps, as a leaning body moves away from under the flare, and a
piece of the flare that this leaves apart from them is dropped. A body widens all
around rather than toward the hanging part alone, which would cut its band into
slivers no bead prints, and stays under the interface, away from the model and a
line and a half away from the other bodies, so that bodies do not merge, not even
where their beads squish: a hollow body merged over a gap would leave the gap
hanging over its inside. Near where the roof still hangs they come closer: a roof
line running along a gap between two bodies narrower than a bridge passes over
neither, even touching one of them with its edge, while a line across the gap
bridges it, so the bodies close the gap under it with their bands, printed
solid. The flares of tips merge with each other, being printed
solid: kept apart, tips a lattice pitch apart would leave the middle of each
triangle of them hanging. Only a body wide enough to
print counts as holding the interface. A tip, no wider than a tip
and half a line, is printed solid with its flare, as a spiral from its outline in
instead of a wall; a wider body prints the part of the band it widens by that lies
within two lines of its outline solid, where its walls lie. A sliver of the band narrower than a
line prints nothing and holds nothing up: the trees inside the body grow under
what lies over it. This holds whatever the base pattern, as the gaps lie between the
bodies, outside any fill. The touch band is removed from every gap narrower than the
bridge span, so a roof line running between tips is not held by them, and at the
default base pattern spacing the hanging check can still fire and widen bodies.

The loops of the spiral are as wide as the part leaves room for, as Arachne lays
them. Loops a line wide leave a slit up to a line wide where they run out, along
the middle of a part mostly, and as the part widens going up, the loop the layer
above starts there would lie over the slit. A line along the slit would fill it
too, but as a path of its own on every layer, whether the layer above needs it or
not, which the layer does not know. Arachne also fits short pieces in the middle
of a part apart from the spiral; one shorter than a quarter of a line is left
out, as a dot after a travel, and the paths of each piece, one for each width
along it, print in a row. Arachne at times lays nothing but such dots in a part
a couple of lines across, which the trees inside the body take for material.
Such a part is filled with loops going inward, as a tree is, or where it is too
narrow for one, with a bead along its middle as wide as it is.

## Interfaces inside hollow trunks

With walls-only base patterns the interior of a body is empty, so the lowest
layer of an interface over it has nothing underneath except the walls. This is
where a tip under the start of a model island merges into a trunk passing by.
Lines bridging the interior break at the fine layer heights Organic supports are
often printed with, and so does anything else the layer above prints over the
interior: a part printed solid, the flare of a tip merged into the trunk, the
walls of branches merging into it going down, or a tree cut short below. So
trees grow inside the body, a tree inside a tree, under the lines of a roof that
hang over the walls, the roofs and the trees below them, as for the bodies under
interfaces, and under the beads of the rest not touching them. The first layer of
the second wall a body wide enough for one gets lies beside the first wall,
fused with it, as the last bead of a tree going into a wall does. The paths
decide on the second wall by the outline they print, whose area may fall on the
other side of the threshold from the body's, so the trees grow up to the first
wall all the same and are cut against the second one where it is printed, as
against every bead of their layer: kept a line farther in, a tree going into the
wall would end short of the only wall printed and hang. With the number of walls
chosen by area, the second walls are chosen going up: by area alone the second
wall comes and goes as the area of a body swings around the threshold from layer
to layer, and over a body of one wall it prints in the air. A body large enough
can start one on the bed or immediately above the first-layer flange, and it
goes on wherever it lies on a second wall below, however small the body gets; it
does not start higher up. A hole in a body
is as open as its inside, and trees grow in it too, but not in the part of it
that opens to the outside going down, as where two branches merge going up
around the gap between them: a tree in it would go on in the body only, and
leave what it printed over the hole on nothing. A tree leaning into the wall of
such a hole ends in the wall. Under a roof a tip stands
where a line hangs: in the middle of a stretch between two bearings, or a tip
radius in from the end of one ending in the air, one for the lines it crosses,
round by round until no line hangs. So the tips stand in rows across the lines,
the base pattern spacing apart, and in a row, close enough that a line between
two of them touches one. Under the beads of a layer, and under a roof printing
no lines, the tips stand on a hexagonal lattice fixed in the object's
coordinates, so close that every point between them lies within the tip radius
and the distance a load is held from of one, with one more wherever a point of
the part is still farther from them. A tip has the tip
radius, a line and the reach at most, so that what lies over the middle of its
ring lies within reach of it too, and a line at least, whatever the tip
diameter: a tip not much wider than a line prints no loop and grows no tree.

Going down, a tree that reaches the wall before the bottom of the body leans
toward the nearest wall by the branch angle, half a line a layer at most, and
goes into it until what is left of it inside is too thin to print: a small
hanging part grows out of the wall, and the last bead of the tree, up to a
line and a half across, lies beside the wall, fused with it. The inside of a
body begins three quarters of a line in from its outline, so that bead lies
on the wall below by a quarter of a line, the rest of it over the inside of
the body. A tree goes on from what it printed on the layer above only: a part of
its disc beyond the model or a roof is no part of it, but the start of a tube
holding nothing up. Where the model inside a body reaches into the disc of a
tree farther than on the layer above, as the tree runs into it on its way to the
wall or as the model widens under it going down, the tree moves away from it by
that much: cut short by the model, it would end beside it, its last bead over
nothing, and get a tree under it, which the model would cut in turn, and so on
down, a stack of short tubes along the model. A tree keeps out of its disc what
the model took where it started, which lies under what it holds up, and what a
step of the model takes, more than half a line farther in than on the layer
above, which no move of a layer gets it out of. Where a roof bars its way, a
tree ends short of the wall, its last bead within a line of it but touching
nothing: a part of a tree touching nothing below gets a tree under it, however
near the wall it lies. Within a quarter of a line of the wall below it touches
it, as the wall is printed that far beyond the material of the body and the tree
that far into the wall beside it: the end of a tree going into a wall that lies
a little farther out below does not get a tree under it. The loops of two trees
beside each other do not fuse, and a tree touches another only where they meet.
The others go
straight down to the bed, or onto a roof or a solid part below, and merge by the
preferred angle with the nearest tree they meet before the bottom, so that a
wide part in a large body stands on a tree like the ones outside. A tree ends
once it stands on the material below, each bead of it half on it at least. A
tree that would go straight down onto the model leans to the wall instead. Where
the body splits under a tree going straight down, the part of the tree out of
it becomes a part of the body. On the first layer every tree has a foot, inside
the body or in a hole of it, two lines of the first layer across at least:
the first layer may print lines wider than those of the base, and with no
expansion of it a narrower tree prints neither a loop nor a fill, which leaves
the tree above standing on nothing. In a sparse first layer, each foot is printed
with its own perimeter and dense interior, and its area is taken out of the
remaining fill. At 100% density, the flange is filled as one region without
internal foot perimeters. A sparse flange may leave a gap under a tree, so its
outline alone does not count as material the tree can end on. A tree widens by the diameter angle. It is
printed with loops of its own beside the walls of the body, the roofs and the
solid parts, fusing with them by a quarter of a line but not over them, the
walls a body shares with an interface of its layer as well. A loop
leaves the middle of the tree open, and the middle moves from layer to layer as
the tree leans and as trees grow into one, as a lattice of tips under a load
grows; so where the gap inside the beads of the loops is wider than a line,
another loop goes round it half a line inside, until a bead over any gap touches
the beads on both sides of it. In a
corner the part of a tree narrower than a line, which prints nothing, leaves the
rest of it a few hundredths off the wall. As the outline the trees grow in may
miss what the layer prints, a body cut by the interfaces of its layer for one,
the loop is cut against the beads of its layer before it is printed: a nozzle
passing over a bead of its own layer ploughs it, at coarse layers into the part.
The intermediate layers of a raft print these inner loops too, with the same
clipping and splicing as the body layers above, after accounting for the raft
material already printed on their layer.
Bottom interfaces may be sparse. Their contours reserve material ownership,
but the spaces between their lines cannot carry a tree. Once the paths are
known, a base path with no contact below, or a run of one hanging past the lines
of the pattern farther than their spacing, receives a local interface path inside
the permitted bottom-interface area. It uses the interface flow and stays clear
  of material already printed. Where the projected axis cannot fit, a straight
  bead or an axis through the remaining legal space can carry the edge of the
  load. With a wider interface flow, parallel beads are planned together and
  accepted only if they hold every remaining stem; choosing one stem's footing
  first can occupy the only space for its neighbour. Anchoring and floating-island cleanup use the same bead footprint with
  round ends. The load is carried down through consecutive bottom-interface layers by
the actual interface paths touching it. The rest of the sparse pattern stays as
selected. This also supplies a root when the pattern misses a whole small
bottom-interface island.
A loop shorter than a line is a dot and is left out, so a part of a tree that
would print one is not grown at all, and what stands on it gets trees under it
instead: left out only when printed, it would leave the loops above it standing
on nothing, a tree that ends in a sliver for one. A loop lying within a line of a
wall, the bottom of a tree going into it, is spliced into the wall, which leaves
itself half a line before the loop, goes round it the long way and comes back
half a line after it, so that the path does not cross itself or pass over what it
has just printed: printed on its own, the loop would come before the wall and
stand on next to nothing, as a short path after a travel. Where two trees merge their
loops come closer than a line for a layer or two, as a loop cannot go round a
neck narrower than two lines without passing over itself; the trees are not
merged any sooner, which would leave the lines over the inside of the merged
loop without the rings that hold them. The filled base patterns, such as
Rectilinear, carry these interfaces on the body fill and grow no trees. With the
default base pattern spacing of 2.5 mm only an interface over an inside wider
than about 3 mm gets them. The passes that fill covered holes, limit the growth of
the bodies and widen them under roofs run for the filled base patterns as well.

Trees whose tops merge while their trunks stand apart give a body that splits
again going down. With a walls-only base the gaps between the trunks open below
the body, and the walls of the trunks are left inside it, where the trees inside
it would stand. So such a body is kept whole below: the trunks below it go
inside it, and it narrows going down by the branch angle into them, a gap
between them opening from its mouth as it narrows. Kept whole straight down to
the bed it would stand the trees up as pillars. It stays away from the model on
its layer and below it, as the body stands beside the model rather than on it.
On a belt, whose floor slopes, a body is not kept whole.

The bodies keep their distance from the model on each layer alone. Where the
model narrows going up, a body would move into the room it leaves at once, by
any distance, its walls standing on nothing over the model or the gap around
it, and the branch of a tree that the model cut below would start in the air
beside it. So going up, a body that stands on the layer below keeps within half
a line, or the move of the branch angle where that is more, of what it stands
on there: the body below, without its parts narrower than a line, which print
no wall, the roofs, and the model where the support may rest on it. It reaches
the room a step a layer. Half a line a layer is 80° from the vertical at
0.02 mm layers, where walls so stacked lean out over nothing, so where the
support may not rest on the model, a body widens over the model of the layers
below by the branch angle only, as the trees lean. Elsewhere it keeps half a
line: the branches of a tree splitting going up move apart by more than the
branch angle, and cut, would leave their tops in the air. A body standing on
none of them is left to the checks below. On a belt a body is not limited
either.

A hollow body is walls only, so a wall that goes in by more than half a line
prints over the inside of the body below. Where branches split going up, the
walls between them would start over the inside of the trunk, and where a body
narrows, over the inside of the body below. So going up, a hollow body that goes
on over a body below keeps all of it but a band of the same step along its
outline: a gap between branches opens from its mouth a step a layer, and a body
narrows a step a layer. The model and the roofs of the layer cut it as they
come in, which leaves the wall over the inside, for the checks below. A body
that ends is not carried on.

After the paths are generated, an island of the support smaller than a bead a
millimetre long that stands on nothing, on neither the support below nor the
model within the bottom Z distance as the trees round it to whole layers, or the
0.1 mm a bead sags across, is dropped: a crumb left where a wall is cut short.
So is a dot of the base, a path no longer than it is wide, whose axis lies over
neither:
touching what is below only by its edge, it hangs, though a bead beside it keeps
it in a rooted island and the bead-scale test below leaves runs that short out.
Arachne lays such dots where a solid part runs out, over the slit between the
loops of the layer below. With support on the build plate only, nothing of the support is to stand on the
model, which then holds nothing up: a branch cut short by the model in a notch
of it ends in the air over its floor. A larger floating base appendage can be
removed when its entire upward chain joins already bed-rooted support or ends
before carrying an interface or model contact. Reachability is computed from the
printed beads, and a required contact is retained even when it has no dense
interface. Should a larger island still stand on nothing with a
walls-only base, the object gets a warning suggesting a filled base pattern,
with the five lowest heights at which such islands lie, for the user to find
them in the preview.

A hollow Organic wall can also leave the material below, curve out and return
to a rooted body on the same layer, and any bead can run on into the air past
it. Sideways contact makes it part of a rooted XY island, but does not put
anything beneath it. So the warning also clips the axes of all support beads,
walls, trees and interfaces, against the actual finite-width material below:
the support, including the empty cavities of hollow bodies, and the model where
the support may stand on it. A run farther than half a bead from it hangs when
it is longer than a bead and ends in the air or leaves a corridor within one
bead width of the chord between its bearings, or when it bridges straight
between two bearings farther than the support spans anywhere else: a line
between two lines of the base pattern, or of a bottom interface. Along a bead
that Arachne prints in paths of several widths, each segment of a run counts in
its length, its span and its corridor against the width of its own path, so a
short stretch of a wide path lends a narrow one hanging on from it nothing.
This bead-scale test also applies below the whole-island area cutoff. A bead
whose ends lie within reach of each other, within a width for a bead of one
width or with the round caps of its ends touching for one of several, is
closed, and its runs are rejoined across the seam. Between the ends of a bead
of several widths only those caps lie over the gap, so its seam is held only
where their cross-section meets the material below, and a seam nothing holds
joins the free stretches on both sides of it however short it is.

A wall excursion is shortened only when nothing needs it. With support on the
build plate only, the upward load check must find no interface or model contact
before a rooted join; where the support may stand on the model, the excursion must
hang over the model too, and carry neither support of the layer above nor model
within the top Z distance. Its retained bearings are connected by
a straight line, or by the shorter legal route along the footing boundary.
Both bearings must touch printed material below; each free span is limited to
one base-pattern pitch. A connection lying on the edge of what the material
below holds touches it, and the warning and the search for excursions take it
as standing there: by rounding, it would otherwise hang off that edge. The new bead stays inside the support region and away
from interface beads. A closed wall stays closed, including when the cut
crosses its seam. If no shorter legal connection exists, the wall is retained
and the warning reports its excursion. Cleanup repeats on that layer to remove
small fill paths disconnected by the repair. Required contacts remain.

Support paths with zero effective role flow do not count as material for either
the islands being checked or their footing. This includes the first-layer flow
factor when enabled. Changing these factors invalidates Organic support even
when its roots are restricted to the bed, so cleanup and warnings are refreshed.

Model footing is the finite-width material of its generated perimeters and
infill, including round path ends. An empty interior inside a sliced outline
provides no footing. Paths suppressed by a zero role flow ratio provide no
footing either. Positive calibration factors retain the nominal line width.
Top-surface flow follows the fixed physical nozzle and volume variant resolved
by the print's process-column map. While an automatic assignment is not yet
known, a path is retained if any possible variant delivers material; the
assignment matters only where the top-surface flow of some variants is zero
and of others not, and only then is it checked again below. A virtual
mixed filament similarly retains possible material from its physical components,
whose final choice or sublayer ordering is resolved after support generation.
Once tool ordering has selected the physical components and nozzles, the warning
is checked again using the actual top-surface flow and sublayer heights, including
object and volume gradients. Sequential copies are checked separately and their
unsupported heights are combined for the object. This check also counts small
islands retained conservatively during cleanup. It updates the object's warning
without changing completed geometry or the tool plan; removing paths at this
stage could change the very layer indices that selected the components. Warning
updates preserve unrelated diagnostics and distinguish objects in the interface.
Saved warnings are restored from completed support steps when the interface clears
its notifications, including slices reused from cache. Slice All restores the
warnings of all valid plates; a single-plate slice restores only its own warnings.
Loading an already sliced plate's preview restores its warnings without reslicing.
Queued warnings are resolved against their source object across the plates,
and repeated updates replace the Organic message rather than appending it.
Rebuilding a nozzle assignment refreshes this check even when the support paths
remain cached.
Layer tool changes in MultiAsSingle mode also defer the physical choice. Early
cleanup retains possible material until that override is known, and changing
the tool-change schedule or its mode regenerates Organic support with model roots.
For a sequential job requiring this check, the writer's filament inventory also
expands virtual mixed slots to their physical components. Otherwise a component
selected by the plan may be absent from the writer and inherit a different
slot's flow calibration. Merged layers use the same first object layer as the
exporter when indexing the published nozzle grouping.
Contoured paths use their actual printed Z, including paths
displaced out of their nominal layer's height band. Sloped segments are clipped
to the root's Z gap before projecting their finite-width material onto XY.
Actual height bounds and planar footprints are cached for each check; model
material is inspected only for islands without footing on support below.
When Scarf flow is zero, seam placement can suppress only part of a perimeter or
conditionally leave it unchanged. Export records the finite-width model paths
that actually extrude, clipped to each support root's permitted Z band. The
warning is then refined separately for every instance using those footprints,
after seam selection and flow multipliers are resolved. This preserves the
first layer and unmodified portions of partial or disabled seams without using
zero-delivery ramps as footing. The recorder is active only for hollow Organic
objects with model roots and a zero-flow Scarf region.
Changing model perimeters, infill, ironing, Z contouring, role flow ratios or
fixed nozzle/volume assignments
invalidates Organic support with model roots so that cleanup and warnings are
refreshed on a repeated slice.
