# ChromaStack - High Level Design

## Purpose and scope

ChromaStack prints a colour picture on a single face of a model: a few filaments are printed one on
top of another, the printer swaps filament at fixed layer boundaries, and the *height* of every
pixel decides how many of those layers reach it. Filament is translucent, so a pixel that stops
part-way through a run shows a blend of that run's filament and whatever is under it, which is
where the colours come from - far more of them than there are filaments loaded.

This is that model inside OrcaSlicer, as a mode of a texture displacement layer. The picture and
the filament stack are the input; the output is a grayscale height map and a set of filament swaps.
Nothing about it is a new pipeline: the height map is handed to the layer the user painted, so the
existing projection, preview and bake turn it into real geometry, and the swaps are handed to the
object's height ranges, which is the slicer's own way of printing a Z band in another filament.

It does not import other filament-painting tools' project files, and it does not choose the filaments or
their order - that is the user's artistic decision, and the panel exists to make its result
visible before the print.

## The colour model

`libslic3r/ChromaStack.hpp` holds the whole of it, free of any GUI dependency, which is what lets the
solve run against plain image bytes and be unit tested.

A **stack** (`ChromaStack`) is a list of **bands** (`ChromaBand`), bottom-up: a filament and
how many layers it gets. Alongside them it carries the print's layer height, which the colours
depend on directly, and the **base**: how many layers of solid backing the picture sits on.

The base is not a band. It is a measurement of the model - everything below the painted face, often
a hundred layers or more on a tall part - while a band is a handful of layers the user chooses. It
prints in the bottom band's filament, because that is what is loaded when the print starts, so it
extends that band's height range down to the first layer instead of being a swap of its own. Only
the bands are quantized into the height map, which is why the 255-layer limit
(`CHROMA_MAX_RELIEF_LAYERS`) bounds the picture and not the stack.

Each filament carries its **transmission distance** (TD), the usual measure for how deep
light gets into it: about 1 for an opaque filament, 4 to 7 for a usefully translucent one, 8 and
above for a nearly clear one. TD lives in the filament preset, as
`filament_transmission_distance` - it is a property of the material, so it belongs with the
material and ships with a profile rather than being retyped per project. It has no effect on
slicing.

`chroma_palette()` turns a stack into the colour it shows at every possible column height, which
is all the solver ever needs. The model is Beer-Lambert absorption, layer by layer: a layer of
thickness `h` in a filament of transmission distance `TD` passes

    T = exp(-h / (CHROMA_TD_OPAQUE_FRACTION * TD / 3))

of the light from under it and shows its own colour with the rest, so walking up from an opaque
base,

    C_k = (1 - T_k) * c_k + T_k * C_(k-1),  C_0 = c_1

The constants say what TD means in physical terms: a filament stops showing what is behind it at
about a tenth of its TD number (`CHROMA_TD_OPAQUE_FRACTION`), and three e-folding lengths of
absorption pass 5% of the light, which is where the eye stops seeing through. Those are calibrated
against how the community's TD step prints behave, not derived from first principles - TD is an
empirical scale.

How fast that happens is the single most useful fact about a stack, and `chroma_layers_to_opaque()`
is it: a filament hides what is under it at about a tenth of its TD, so how many *layers* that takes
depends entirely on how thick they are. TD 4 covers in two layers at 0.2 mm and in five at 0.08 mm.
Below about two layers a band has no intermediate colours at all - the picture can only use the
filament or not use it - which is why an otherwise sound stack prints as flat steps at a coarse
layer height. The panel reports the number under the palette ramp, and warns when it falls below
two, because nothing else makes it visible: the filaments look fine, the bands look long enough, and
the result looks like the colour model is wrong when it is the layer height.

How many of a picture's layers actually earn their place is a second question, and
`chroma_distinct_levels()` answers it: a band eventually hides what is under it, and another layer of
it then shows the colour the one below already showed. It is measured against the palette rather than
against TD, because how long a band keeps paying depends on what is under it as much as on the
filament - a light band over a dark one is finished in four or five layers, while a dark one over a
light one goes on moving the colour perceptibly well past the depth at which it is nominally opaque,
since the last few percent of light coming through still shifts a near-black colour. `Trim` shortens
each band to its last layer that still changes anything.

Beer-Lambert is also the only model that stays self-consistent applied per layer: transmittance
multiplies, so a run of n layers of one filament passes `exp(-n h / d)` whether it is composited in
one step or n, and the palette therefore does not depend on how the stack happens to be cut into
bands. A unit test asserts exactly that. Compositing is done on linear light, because the quantity
being attenuated is light rather than an sRGB code.

The base is assumed opaque. That is what makes a column's colour depend only on the layers above it,
and it is why the panel warns when the base is thin.

### Lit from behind

All of the above is a print with the light in front of it. A lithophane is the same stack with the
light behind it, and it is a different colour model rather than a different view of the same one: what
the eye sees is the light that got through the whole column, so the transmittances multiply, each band
tints what passes through it, and thicker means darker rather than more opaque. `chroma_palette()`
returns the same shape in either mode, which is what lets the solve, the swaps and the preview shader
stay exactly as they are.

Three things invert with it, and they are the user's problem rather than the model's, so the panel says
so instead of silently compensating. The dark filaments belong at the bottom, because a dark band
starves everything above it. The base is part of the column rather than an opaque backing, so it wants
to be a layer or two, and a picture on a hundred-layer base is black. And a stack solved for one lamp
is not solved for another: the lamp's colour and brightness are part of the palette, which is why they
live on the stack and are hashed into the key that decides when a re-solve is due.

A third mode sits between the two, because most back-lit prints hang in a lit room rather than in the
dark: with the light behind *and* in front, some of what reaches the eye came through the column and
the rest bounced off its face, and the two add. `chroma_palette()` runs both walks and mixes them in
linear light, which is the only space where adding two lights means anything, by a balance the user
sets. It is a mode rather than a view because the sum is a different set of colours from either walk,
and a stack chosen against the lamp alone washes out the moment room light lands on it. The balance is
part of the palette and so is part of the solve key.

## The solve

`chroma_solve()` maps each pixel of the picture to the column height whose palette colour is
closest to it, perceptually: both sides go to CIELAB and the match is a CIE76 distance. It runs in
stages - palette, match, dither, retouch, quantize - because everything that corrects a solve has to
hook in at a defined point rather than be folded into one pass, and `ChromaSolveOptions` is what the
caller sets per run rather than per project.

The metric is one of those options. CIE76 is the interactive default: a 1-D palette of a few dozen
entries makes the extra accuracy of CIEDE2000 hard to see and the cost of it easy to feel, several
hundred million comparisons for a large picture. The pass whose height map is actually printed asks
for CIEDE2000 instead, which is affordable only because the solve runs on a worker rather than on
the UI thread.

Resolution is bounded by what can be printed, not by what the picture happens to carry. The live
path works at 1024 texels on the longer side; the bake works at two texels per nozzle width, which
for a photograph on a postcard-sized plate is roughly a twentieth of its pixels. A region narrower
than an extrusion cannot be printed in a colour of its own, so the texels past that point buy
nothing and are paid for twice, once in the match and once in the map the bake samples.

Optional Floyd-Steinberg error diffusion, serpentine, trades the bands a photograph shows across a
sky or a cheek for fine stipple. Off by default, since stipple is the artifact rather than the cure
for flat-coloured artwork - and forced off for artwork whatever the setting says, since the decode
can tell a palette-exact picture from a photograph (`DecodedHeightTexture::palette_exact`) and such a
picture is also resampled nearest-neighbour so its edges stay hard. The diffusion works at a cell
sized from the extrusion width rather than per texel: a one-texel column is dropped by the slicer, so
a per-texel dither promises a colour the print cannot make.

Four things come back: the per-pixel layer count, the same thing as an 8-bit grayscale height map
(0 at the base, 255 at the top of the stack), the palette colours chosen as an RGB image, and the
colour difference at every pixel as a second grey image. The quantization belongs to the solve rather
than to the displacement because the relief's steps have to land on layer boundaries, and a grey
level is the only channel the displacement reads.

The solve also reports the mean and worst colour difference it had to accept. That is the only
honest answer to "will this stack reproduce my picture": under about 5 is a good match, over about
15 means the filaments cannot reach the picture's colours and no number of layers will help. The
per-pixel image is the same answer without the averaging, which is what makes it actionable: a mean
of 5 says nothing about the one red roof the filaments cannot reach, and the overlay that draws it
(see "Seeing the colours") is where a correction is decided.

### Correcting one part of a picture

A solve is one rule applied everywhere, and there is always somewhere it decides wrong. Everything
else on the card changes the whole picture to fix one part of it; `ChromaRetouch` is the part that
does not. An edit is a region of the picture plus what to do with it - move it by a number of layers,
or take it to the top of a named band - and the edits apply in order after the match and the dither.

The region is a run-length encoded 1-bit mask over the picture's own grid, sampled nearest-neighbour,
so one stroke painted once holds for the coarse interactive solve and for the full-resolution bake
alike. Runs rather than rectangles because a brush stroke does not decompose into rectangles cheaply,
and a mask rather than a vector shape because what is painted is a region of a raster picture.

Each edit carries an enabled flag, because the useful question while judging a correction is what the
picture looks like without it.

## How it attaches to a texture displacement layer

A ChromaStack layer is an ordinary `TextureDisplacementLayer` whose height map is computed rather than
imported:

- `chroma_source` keeps the picture the user chose, so the solve can be redone whenever the stack,
  the layer height or the picture changes;
- `chroma` is the stack;
- `image_data` - the same field every other layer uses - holds whatever the last solve produced.

So everything downstream needs no changes at all and knows nothing about colour stacks. It also
means the solve writes `depth_mm` (the relief is exactly as tall as the layers above the base) and
pins `invert`, `midlevel` and `smoothing`: blurring a solved height map moves its steps off the
layer boundaries they were solved for.

Colour by filament swap and the layer's own `color_enabled` (one filament per triangle, via
`mmu_segmentation_facets`) are mutually exclusive, and the panel enforces it. They are two
different ways of colouring the same surface, and painting the relief per triangle would hide the
very stack the heights were solved against.

## The filament swaps

`chroma_ranges()` turns the bands into Z bands in the object's own coordinates, merging any two
adjacent bands that share a filament, since nothing is swapped between them. The panel writes them
onto the object as `layer_config_ranges`, each range carrying `extruder` - which
`apply_to_print_region_config()` spreads over every feature filament the range does not otherwise
override, i.e. the whole band prints in that filament - and `layer_height`, both because the solve
is only valid at the height it was run at and because `layer_height_profile_from_ranges()` reads
that option without checking it is there.

Height ranges rather than colour-change custom G-code: they are object-relative, so they survive
the object being moved, they are saved in the project file, and they appear in the object list where
the user can see and edit them. The cost is that an object can only have one set of them, so
applying a stack replaces whatever ranges the object had, with a confirmation when it had any.

`ObjectList::rebuild_layer_ranges()` mirrors them in the object tree without touching the
selection - changing the selection while a gizmo is open tears the gizmo down under the user.

## Geometry the model assumes

A filament swap happens at one Z across the whole plate, and the relief grows along the painted
surface's normal. The two agree only where that surface is horizontal, so:

- the painted patch's height above the bottom of the object is what the base layer count has to
  match, and the panel measures it from the patch and offers the number rather than leaving the user
  to work it out;
- a patch that is not flat in Z gets a warning, because its colours will only be right where it is
  level. The relief itself is still computed, since a gently domed plaque is a legitimate thing to
  print and only the user can judge how much error is acceptable.

The model itself is the user's: the stack needs a slab of the base thickness with a face to paint,
and nothing here creates or thickens geometry.

## Seeing the colours

The colours are blends of translucent filaments. No single filament makes them, so they cannot be
expressed as the per-triangle filament assignment the 3D scene paints with, and there is nothing for
the ordinary view to draw. They exist in two places: the sliced preview, which shows the filament
bands as they print, and an overlay the gizmo draws over whatever surface is on screen.

That overlay is a shader, `resources/shaders/*/chromastack.*`, and it composites the stack itself,
per fragment, with the same Beer-Lambert absorption the solve used. `chroma_layer_density()` is the
one place the relation between a transmission distance, a layer height and an absorption lives, so
the two cannot drift apart: the palette and the shader both ask it.

It walks bands rather than layers, which is exact rather than a shortcut - a run of n layers of one
filament passes `exp(-n d)` whether it is composited in one step or n - so a stack of sixteen bands
costs sixteen iterations however many hundreds of layers it has. The base goes into the bottom band,
being the filament it prints in.

Compositing there rather than reading a table is not the same picture computed a harder way. A table
indexed by height holds the answer for one geometry, a column seen straight down, and the two places
that is not true are exactly where a print is looked at:

- **At a grazing angle** the light leaving the surface crosses each layer along the view ray rather
  than straight up. It passes through more filament, and the colour deepens. The shader measures that
  path against the stacking axis in eye space and lengthens the absorption by it, clamped, since a ray
  running along the layers passes through no stack at all.
- **A wall is not a column seen through.** It is the cut side of the layers at that height, backed by
  more of the same filament, so it shows that band's own colour rather than the composite - which is
  why the side of a ChromaStack print is a set of stripes while its top is a picture. The shader
  blends between the two by how far the surface faces the stacking axis.

A band edge still lands exactly on the layer boundary however coarse the triangles are, and it all
still works after a bake, when the relief is real geometry and the height map is attached to nothing
on screen: the colour comes from the fragment's own height, and the mesh carries none of it.

On top of the colour the shader tries to look like printed plastic, because a flat shaded colour
reads as a rendering and the whole point of the overlay is judging a print:

- the terminator is **wrapped** and the grazing angles **glow**, both scaled by the transmission
  distance of the filament on top - filament translucent enough to be worth stacking scatters light
  rather than reflecting it, and an opaque stack correctly gets neither;
- the walls carry **layer lines**, a groove at every layer boundary that also modulates the
  specular, so a wall glints in stripes as a printed one does. Terraces get none: a terrace is one
  layer's top surface and has no lines to show.

Both effects are driven by the fragment's height, so they are free of any texture or UV, and both
fade out through `fwidth` once a pixel covers more than about half a layer - lines finer than a
pixel are noise, and a colour read from one layer out of the dozen under a pixel flickers as the
camera moves. The palette lookup slides off the texel centres over the same range so the linear
filter averages the bands a pixel covers instead of picking one of them.

The overlay is drawn pulled forward and without depth writes, the same pass the painted-colour
overlay uses, since it is the same surface as whatever is underneath it.

The same shader draws the other things there are to see. Back lit it composites the transmitted light
instead of the reflected light, by the same per-band walk and the same grazing-angle path, with the
lamp's colour and brightness as uniforms; lit from both sides it weighs that against the reflected
light by the same balance the palette used, so the preview and the colours the solve was chosen
against stay the same picture. Face down it measures each fragment's depth from the top of the stack
instead of the bottom, since the painted face is then the plate's underside and what a fragment is
seen through is the filament above it rather than below it: one uniform, and the walls and the layer
lines follow from it. And it can draw the solve's error image in place of the
colours: a ramp from the unlit surface grey through amber at a difference of about 5 to a saturated
red at 25 and above, keeping the shading and the layer lines so the shape is still legible. The ramp
falls monotonically in luminance, so it survives a greyscale screenshot and a colour-blind reader
rather than relying on hue alone. The error has no height to be indexed by - two pixels at the same
height can be wrong by different amounts - so this is the one thing the shader reconstructs the
picture's own coordinate for, in closed form from the fragment's world position, through the same
projection the bake samples by.

## What the panel shows

The kind of a layer is the layer's own, chosen when it is added. The chip in the card's header is
also the control that changes it: there are two kinds and one of them is the current one, so the chip
is the switch rather than something that opens a switch, and clicking it converts. Converting keeps
everything -
a picture's stack survives a trip through being a relief layer, and a relief layer's texture becomes
the picture it is solved from, a height map being a picture with no colour in it. Converting away
from a picture clears the swaps it wrote, and only those.
There is no panel-wide mode, so one object can hold a picture on its face and a knurl on its rim, and
a card never shows a control that cannot do anything on it. A picture cannot be reordered (anything
printed over it buries the colours) and an object may hold only one, refused when the layer is added
rather than when its swaps would overwrite the first one's.

The card carries the four things a user decides and nothing else: which picture, which filaments in
what order and how deep, whether the result is good enough, and - as a single row of `2 3 4 6` -
how many colours to spend. Everything else is either derived or rare, and either shows as a state
chip or sits behind More settings.

### The solve and the swaps are not buttons

A height map that has to be asked for can be out of date, and a set of swaps that has to be applied
can be missing - and both of those states then need a warning to explain themselves. `chroma_settle()`
removes the states rather than the warnings: every edit marks the stack, and once it has held still
for a moment the layer re-solves and writes its swaps, the way the preview already rebuilds itself.

The swaps are the object's height ranges, not the layer's, so writing them automatically needs a
rule about whose they are. The stack remembers a fingerprint of what it last wrote
(`ChromaStack::applied_ranges`); the automatic path writes only when the ranges are empty or still
match that fingerprint, and otherwise leaves them exactly as they are and says so with a chip, with
a button that asks before replacing them. The panel never takes over something it was not given.

### Saving one

The .3mf stores a texture displacement layer as JSON with its image beside it, and a ChromaStack
layer has two images rather than one: the picture it was solved from and the height map that came
out. Both go in, and the stack itself - the bands, their filaments and transmission distances, the
base, the dither, and the fingerprint of the swaps it wrote - goes in the JSON.

The height map alone would not do. A project that kept only that reopens as a relief layer whose
texture happens to be a staircase: it still prints, but it can never be solved again, and nothing on
screen says why. A stack that comes back with no bands in it is treated as a relief layer on purpose,
since that is what it is.

### Measuring a filament

Transmission distance cannot be read off a spool and no filament preset ships one, so without a
measurement every stack starts from a default of 4.0 and every colour the panel predicts rests on a
guess. `chroma_wedge_mesh()` is the measurement: a staircase of the filament under test, one layer
taller at each step, printed over a base of a filament it has to hide. The step at which the base
stops showing through is the depth at which it covers, and `chroma_td_from_step()` turns that depth
into a TD - the same relationship `chroma_layers_to_opaque()` runs the other way.

`Plater::calib_transmission_distance()` builds the plate: the wedge, the print forced to the layer
height its steps are counted in (first layer included, or every step would be a different thickness
from the one it is read as), and two height ranges putting the base in one filament and the staircase
in the other. It is the only calibration that generates its model rather than loading a shipped one,
because the steps are layers and the layer height is the user's.

It is reached from the Calibration menu, which picks the darkest other loaded filament as the base
and confirms before replacing the plate, and from a ruler beside each band's TD in the card, which
measures that band's filament and offers to put the result straight into the stack.

The other way to get the number is that somebody else measured it. A shipped table
(`resources/filament_td`) is matched on vendor **and** colour, because a transmission distance is a
property of a colour rather than of a product line - one maker's matte PLA runs from 0.1 to 4.1 across
its colours - and a match on the name alone would hand a white filament the number measured for a
black one, which is worse than the default because it looks measured. `filament_td_import()` reads a
library a user points at, in JSON or CSV, normalizes it into the same shape and writes it under
`data_dir()`, where it is read beside what we ship. Imported entries carry where they came from, the
card says so, and anything the user set on a preset themselves still outranks both. We import such a
file rather than shipping one: the data is somebody else's.

### Choosing the filaments

`chroma_auto_stack()` answers the question the panel could previously only report on: which of the
loaded filaments reach this picture, and in what order. The order matters as much as the choice,
since a stack is filaments seen through each other, so it grows the stack one filament at a time and
tries every remaining filament at every position, keeping whichever placement loses least to the
picture. That is a few dozen candidates rather than the tens of thousands an exhaustive search over
subsets and orderings would be.

`chroma_suggest_colors()` answers the other half of that question: not which of the loaded filaments
to use, but which colours the picture would most like to be printed in, owned or not. Picking from a
drawer has a ceiling - a picture whose colours are not in it cannot be printed well out of it however
it is arranged - so this lets the colours themselves move: a weighted k-means over the picture for a
starting set, then a hill climb that walks each band's colour wherever the stack's own error falls
fastest. Each suggestion carries the transmission distance its band wants and the nearest thing the
user already has, since a colour is only half of what to go looking for.

Candidates are scored against `chroma_histogram()` - the picture's colours bucketed four bits per
channel - rather than against the picture, which is what makes trying dozens of stacks affordable. A
photograph is millions of pixels and a few hundred distinguishable colours.

How those buckets are weighted is the whole of what HueForge solves with seven luminance modes. By
coverage alone, the background of a photograph is most of its area, so the one saturated thing the
picture is *of* gets outvoted by the wall behind it, and the filaments chosen serve the wall. Two
things fix that here instead of seven modes. A bucket's weight rises with how far its colour sits from
the picture's own average, so a vivid sliver can earn a band its size would not. And the user can say
which part of the picture matters by painting it, which weights those pixels and nothing else. One
mapping with a say in what it is for beats seven mappings to choose between: the mode names never told
anyone which one their picture wanted.

The search is parallel and deterministic, which for a search is not automatic. Candidates are built in
a fixed order, scored in parallel, and the winner picked by walking the scores in that order and
keeping the first that is strictly better - so the same picture and the same drawer give the same
stack every time, which a parallel reduction that accepted whichever thread finished first would not.

A suggestion is only half useful as a list, so the card can act on it: one colour puts the nearest
filament the user owns into that band, and the whole list builds the stack from those nearest
filaments, in the suggested order and with the suggested layer counts, merging two that land on the
same filament because that is not a swap. It is the suggested colours that move the picture, but it
is the owned filament's own colour and TD that go into the band - a prediction computed from a colour
nobody owns would be a prediction of a print that cannot happen.

A band also remembers the name, colour and TD it was solved against, which is what lets a preset be
swapped under a saved project without the stack silently claiming to be something it is not. When the
project's filament in that slot no longer matches, the band's swatch carries a mark, and the mark is
the control: clicking it adopts what the project has and re-solves. Reported rather than corrected,
because adopting it changes every colour in the picture and that is the user's call.

### The picture, and what can be done to it before solving

A picture is chosen from a gallery rather than a file dialog: built-in pictures and the user's own,
in hideable categories, so a ChromaStack card never offers height-map textures and a relief layer
never offers photographs. The window is shared with the frame gallery, since choosing a frame is the
same act - look at pictures of the options, pick one.

The adjustment panel every relief layer has applies here too, on the picture rather than on the solved
height map: brightness, contrast, sharpness, blur, hue and saturation. Saturation matters more here
than it would in a photo editor, because a stack reaches a limited set of colours and pulling a
picture towards what it can reach is often the difference between a print that resembles it and one
that does not.

Above the sliders sit filters, which are the same adjustments as one named thing. A phone camera
offers them for a reason: the decision is made by looking rather than by understanding what contrast
does to a palette of thirty colours, so each filter is shown as a thumbnail of *this* picture under
it. They are chosen for what a filament print needs - more bite, fewer colours, opened shadows,
removed speckle, no colour at all - rather than for a look.

Committing an adjustment writes a new picture into the user's gallery and points the layer at it,
which is the same shape as the relief path: the edit becomes an asset of its own, so the picture it
came from is still there and the edited one can be used again on another plate.

### The plate a picture makes for itself

Most of what this is good at - a photograph, a poster, a piece of line art - has no model behind it
and does not want one, so the plate is generated: the picture's own proportions, a solid backing, and
a frame.

The frame is a swept cross-section, and that is how it is built rather than how it is chosen. A frame
is something looked at, so it comes from a gallery of frame definitions - a few shipped, any the user
adds - each one an outline, a cross-section polyline, and the proportions that make that moulding look
like itself. What the plate stores is which frame it is and the cross-section resolved from it at this
frame's size, so a project reopens on the frame it used and a frame whose file has gone missing still
prints, having kept its resolved section. The width and the rise stay as fields, because a dimension
is not a design: the same ogee at 6 mm and at 20 mm is the same ogee. Those two fields live in the
gallery rather than on the card, beside a drawing of the cross-section they change, since six
millimetres means nothing on a row of its own and everything next to the profile it shapes. Taking
the frame off is the gallery's first cell for the same reason - it is a choice made while looking at
frames - and so is cutting the plate to an SVG silhouette, which is a separate choice from the
moulding swept around it and can be used with any frame in the gallery.

A fresh pick takes the moulding's own width and rise, because proportions are part of what makes a
frame look like itself; a width the user has set survives browsing the gallery, because that is a
dimension they chose for this plate.

A plate can print either way up. Face up is the ordinary one and the picture carries the layer lines
of its own terraces. Face down prints the picture's face against the build plate, which comes out as
smooth as that plate is, and inverts everything about the geometry: the relief grows into the plate
rather than out of it, so the height map is complemented; the swaps come out top down, the base
printing last on top; and the backing has to be at least as thick as the whole stack, because
everything the picture is made of prints above it. The base cannot be measured from a painted face in
that mode, so it stops following one.

Face down also leaves the 3D view showing the wrong side of the print. The slicer looks down at the
bed, the picture is on the underside, and what faces up is the opaque backing, so the honest view of
what sits on the bed is a blank slab. The panel does not pretend otherwise: it offers a way round to
the other side instead, and the preview shader reads the stack from the top so that side shows the
picture when the user gets there.

### Fixing an area

The brush that paints a retouch edit works on the model, not on a flat copy of the picture. That is
where the user can see what is wrong - it is where the error overlay is drawn, and the two are meant
to be used together - and it is the only view in which the correction and the print are the same
object. A stroke is raycast onto the mesh and projected into the picture's grid through the mapping
the bake samples by, so what was painted on the print lands on the texels it covered.

The brush is measured in millimetres on the print rather than in pixels on screen, so it covers the
same part of the picture however far the camera has zoomed. A stroke is one edit: that is the unit a
user would undo, switch off to compare, or delete, and it is why the mask is accumulated until the
button is released rather than committed per motion event.

Correcting by a number of layers and correcting to a band are both offered because they fail
differently. Moving by layers keeps the shading of whatever was painted and is the gentler fix for a
face that went a band too dark; taking an area to the top of a band flattens it, which is what a
region that should be one flat colour actually wants.

The same brush paints the importance region, which is not a correction at all: it changes which of the
picture's colours the filament search and the suggestions are trying to serve, and nothing about the
height map. It is the same brush because it is the same gesture - point at the part of the picture
that matters - and keeping it one tool is what stops the panel growing a second one.

### Surfaces that are not level

A filament swap is a print-Z event: the whole plate changes colour at one height. What a point of the
picture shows is therefore decided by how high that point is, not by how far the relief stands off the
surface under it. Two consequences, and they are different problems:

- **The relief has to be deepened.** A displacement of d mm along a tilted surface's normal only
  raises the print by d*cos(tilt), so every step of the picture would land below the height its colour
  was solved for. `chroma_tilt_relief()` scales the solved height map texel by texel by 1/cos(tilt) and
  hands back the depth the layer must carry. The surface is measured on the picture's own grid by
  `chroma_surface()`, which rasterises the painted patch through the same projection the bake samples
  it with. Past `CHROMA_MAX_TILT_DEG` the factor is clamped: it runs away towards vertical, and a
  sliver of a steep wall must not deepen the whole layer by a factor of fifty.
- **The surface's own rise cannot be fixed at all.** Where the patch is higher at one end than the
  other, that end is further into the stack and shows a later colour. Nothing about the relief changes
  this - only filling the patch up to a level reference would, and that would throw away the shape the
  model has. So it is reported rather than corrected: `chroma_problems()` says how many layers the
  patch rises across itself, which is how many bands the picture drifts by.

A ChromaStack picture is also pinned to the XY plane when it is sampled, instead of being blended
across the three planes the way an ordinary texture layer is. Its colours are heights, so it has to
keep its place on the plate as the surface tilts away from level; on a level face that is the plane
the blend settles on anyway, so nothing about a flat picture changes.

### The shape of the card

Four blocks in the order of the work, so the card carries the task's sequence rather than leaving the
reader to find it:

1. **The picture** - the source row, the before and after at the top where they get the most space,
   one line saying whether this is ready to print, and the brush that corrects what those two
   previews show. A problem bar sits above it when there is one.
2. **Colors** - the ramp first, because it reads the whole stack at once and settles whether any of
   it is worth printing; then one row per filament, top of the stack first; then the two ways of
   choosing the whole stack again.
3. **The plate** - every dimension of the physical thing, which is why the base and which way up it
   prints are among them. Closable, and open by default, with its size in the header while closed.
4. **Light and detail** - chosen once for a picture and then left, so it starts closed.

Where the picture sits on the model (its size there, its angle, the mapping, the handle that moves
it) is a fifth block, but only for a picture painted onto somebody's model. A generated plate pins
all of it - the picture *is* the plate - so for one of those the block is absent rather than merely
disabled. A control that cannot act is worse than one that is not there, because the reader has to
work out which it is.

A filament's row is two lines. The first holds the three things that get changed: which filament, how
many layers, and the chevron that opens the rest. The second is derived and never interactive: how
deep light gets into that filament and how many layers of it hide what is under it, which is the fact
that decides whether the stack works at this layer height and used to live only in a tooltip. The
chevron opens the five controls that are a filament's own rather than the stack's - its transmission
distance, measuring it, where it comes in the print order, and taking it out - one row at a time.

### The states

Two things are said in colour rather than in words, and one line is said in words because it is the
question the whole card is there to answer.

- **How good the match is** is the colour of the prediction's own frame, plus the figure on a chip
  over the image: amber when the filaments cannot reach the picture, green when they do, the
  separator grey when it is merely close, and the accent colour while the stack is settling. It is a
  property of that picture, so it is drawn on it.
- **Whether this is ready** is one derived line under the previews: no picture, no filaments, still
  working it out, the worst thing `chroma_problems()` found, or that it is ready to bake. Bake's
  reason for being disabled is the same sentence from the same function, because two readings of one
  state are two things that can come to disagree, and the place a user looks for "can I print this
  yet" must not contradict the button they are looking at.
- **Everything else wrong** is a bar at the top of the card: the first problem in a sentence, the one
  action that answers it when there is one, and a count of the rest with the full list in its
  tooltip. It was a single amber triangle, which could carry neither the sentence nor the action, and
  it is the one thing on the card that is about the object rather than about a control on it.
- **Whether the base follows the model** is a padlock beside the field, with the state in words
  beside it: closed-means-automatic is not a thing a reader can guess.

A tooltip that is about what a stack reaches carries a picture of it. These cannot be renders of a
model the way the relief layer's are: what they show is a property of the colour model, so they are
computed from it by `scripts/make_chromastack_tips.py`, which mirrors the constants in
`ChromaStack.hpp` and is run again when they change. Most of them are animations, because what each
one has to show is a change - another filament added, the same filaments reordered, a band growing
deeper - and a pair of stills leaves the reader to find the difference.

The reasoning is that a warning printed on a panel is read once and then occupies that space for
ever, and a panel that explains itself in paragraphs cannot be scanned at all. A mark that appears
only when it has something to say costs one glyph when there is nothing wrong, and carries as much
prose as the problem needs when there is.

The base is a measurement rather than a setting: it follows the painted face unless the padlock is
opened, which is why it sits among the plate's dimensions instead of among the stack's. The lighting
and the dither are the card's closed section because neither is touched on a normal pass: back lit is
a different print of the same picture, chosen once, and the lamp's colour and brightness only exist
when it is on.

The printed-colour overlay is a view, so its switch is in the View row with the other views rather
than inside a layer's card, and the error overlay is the second switch beside it. Both draw the same
surface through the same shader: one shows what the stack will print, the other how far that is from
the picture. Which is why the error view opens the overlay on its own - it is the one case where the
overlay is wanted with the colours off.
