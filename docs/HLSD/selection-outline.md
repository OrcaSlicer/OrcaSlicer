# Selection outline — High Level Design

## Purpose and scope

When the Outline view option is on, the selected volumes of the 3D canvas are outlined: a ring of
constant screen width around their silhouette, and thinner lines along their inner edges, the creases
between faces, the contours where one part of the selection hides another, and the seams between
neighboring parts. It is a view setting. It changes nothing in the model or the project, and it reaches
neither the picking pass nor thumbnails.

`SelectionOutline` draws it once per frame for the whole selection, after the object passes of
`GLCanvas3D::_render_scene()`. Its cost depends on the screen area of the selection, not on its triangle
count, apart from one extra draw of the selected volumes.

## Passes

`GLVolumeCollection::render()` collects the selected volumes it draws, when the canvas passes it a list,
and sets bit `SelectionOutline::StencilBit` of the canvas stencil wherever one of them passes the depth
test. The stencil is cleared with the depth buffer at the start of the frame. `SelectionOutline::render()`
then runs three passes, restricted by the scissor to the screen box of the selection, padded by how far
each pass reads the one before it.

1. **Mask** (`outline_mask`). The selected volumes are drawn again, with the clipping of the object
   passes, into a 16 bit RGBA target with a depth texture of its own. Each texel packs, in two bytes per
   channel, the octahedral encoded view normal turned towards the camera, the part index of its volume,
   and the distances from the texel center to the edge of its triangle along +x, -x,
   +y and -y. GLModel gives every triangle three vertices of its own, so `gl_VertexID % 3` yields the
   barycentrics, and their screen derivatives the distances.
2. **Edges** (`outline_edges`). For every texel, and each pair it forms with its right and upper neighbor,
   an inner edge is classified and located, and packed as a 4 bit strength and a 4 bit crossing position
   between the two texel centers. Along its row, the pass also finds the nearest covered texel and sums
   the coverage under a gaussian.
3. **Composite** (`outline`). A full screen quad, drawn twice into the canvas framebuffer. The ring pass
   draws where the stencil bit is clear, so its inner edge is the multisampled silhouette of the
   selection itself. The lines pass draws where it is set, so inner lines stop exactly at the silhouette.

The targets are allocated once at canvas size and kept, and they are freed with the canvas.

## Inner edges

A pair of covered texels makes an edge when its normals differ by more than 25 degrees, fully at 40,
when one surface hides the other, or when the texels belong to different parts. One surface hides the
other when the view space step between the texels lies more than a few pixels off their planes, and
each texel lies more than a pixel off the surface of the other side carried on over the pair. A curved
surface seen edge on carries on smoothly, so it does not break into lines, nor does a fillet whose facets
turn a little at every texel on both sides. A texel on a triangle smaller than a pixel both ways
takes the normal of a neighbor beside it: the normals of such slivers can be far off their surface and
would otherwise show as dots.

The crossing is placed between the texel centers, in this order of preference:

- where the triangles under both texels end, when they agree: the edge they share;
- where the depth planes of the two sides meet, extrapolated from two texels on each side, when each side
  is flat over them and the planes part clearly along the axis;
- for a hidden surface, where the triangle of the nearer one ends, when the texel behind it or the two
  beside it end on the same straight line, as a curved contour made of strips narrower than a pixel does
  not;
- otherwise half way.

The lines pass takes the nearest crossing along the row of the pixel and the nearest along its column.
For a straight line at distance d, they lie at d / |cos| and d / |sin| of its normal, so
1 / d² = 1 / dx² + 1 / dy², which gives the distance at any angle without knowing it. The line is drawn
with a smooth edge around its half width.

## Ring

The binary mask places the silhouette only within half a texel, which would show as a ripple along the
outer edge of the ring. The coverage of the mask under a gaussian, summed over the rows by the edges pass
and over the columns by the ring pass, is the normal tail beyond d / sigma for a straight silhouette at
distance d, which the ring pass inverts for a sub-texel distance. Corners and features thinner than the
gaussian lose coverage faster, so the distance is kept within half a texel of the one to the nearest
covered texel, which the same two passes find exactly.

## Occlusion

The composite is depth tested against the canvas, so what stands in front of the selection hides its
outline. It writes the depth of the selection at each pixel, from the mask, moved forward by the spread
of that depth over the pixel, so that the selection passes the test on every sample. The ring takes the
depth of the nearest covered texel, brought forward with its distance from the silhouette, so that it
also shows over the plate the object stands on, which lies in front of the silhouette right below it.

## Colors and sizes

The outline is white. Its lines have a width on the model, 0.35 mm for the ring and 0.18 mm for the inner
lines, so they grow as the camera closes in, kept between 2.5 and 5 pixels for the ring and 1.2 and 2.4 for
the inner lines, scaled by the canvas scale. An inner line that would be thinner than 1.2 pixels fades
instead, down to 40%, so that a far selection does not turn into lines. Inner lines are drawn at 75%
opacity. Both have a smooth edge 1.2 pixels wide.

## Requirements

The outline needs ARB framebuffer objects, a 16 bit RGBA color renderable texture and the outline
shaders, and it is not drawn without them. Under GLSL 1.10 there is no `gl_VertexID`: crossings fall
back on the depth planes and the half way point, and slivers are not detected. Without a stencil buffer
the ring stays off the pixels the mask covers, and loses the exact inner edge.
