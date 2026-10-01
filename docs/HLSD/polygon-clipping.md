# Polygon Clipping — High Level Design

## Purpose and scope

Almost every stage of slicing works on 2D regions: slices, perimeters, infill
areas, bridges, supports and brims are all produced by boolean operations and
offsets on polygons. libslic3r does this through two interfaces, both built on
the Clipper2 library vendored in `deps_src/clipper2`:

- `ClipperUtils` (`src/libslic3r/ClipperUtils.hpp`) takes and returns Slic3r
  geometry: `Polygon(s)`, `ExPolygon(s)`, `Polyline(s)`, `Lines` and
  `Surfaces`. It provides unions, intersections, differences and xor, closed
  and open offsets, morphological opening and closing, variable width offsets
  and polyline clipping.
- `ClipperZUtils` (`src/libslic3r/ClipperZUtils.hpp`) clips paths whose
  vertices carry a Z value, which callers use to tag vertices with a source
  index or an extrusion width.

No other code calls Clipper2.

`ClipperUtils` declares its own `JoinType`, `EndType`, `PolyFillType` and
`ClipType` enums and maps them to Clipper2's. Every call builds its own
Clipper2 objects and shares no state, so slicing threads can clip
concurrently.

Clipping is one of the largest costs of slicing, and nearly all of it goes
through `ClipperUtils`. The layer is therefore designed for throughput as much
as for predictable geometry.

## Vendored Clipper2

`deps_src/clipper2` builds the static target `Clipper2`. It carries four
changes to the upstream sources that must be carried over when Clipper2 is
updated. The namespace switch sits at the top of every header and source, the
other three are marked with `Orca:` comments.

| Change | Files | Why |
| --- | --- | --- |
| Z build in its own namespace | all headers and sources, `clipper2_z.cpp`, `clipper2_z.hpp` | The library is compiled a second time with `USINGZ` in namespace `Clipper2Lib_Z`, so the 2D and the Z variants link into one binary. |
| Engine nodes from tbbmalloc | `clipper.engine.h`, `clipper.engine.cpp` | Vertices, active edges, output points and records, local minima and `PolyTree` nodes are allocated one by one. `CLIPPER2_NODE_ALLOCATOR` routes them through `scalable_malloc`, because the default heap does not scale when all slicing threads clip at once. |
| Concave joins at the edge crossing | `clipper.offset.cpp` | For closed paths, a concave corner is joined at the crossing of the two offset edges when that point lies within half of both adjacent edges. The upstream 3-point loop makes inward offsets of dense curves very slow to union. |
| Rounded arc steps | `clipper.offset.cpp` | Round joins use the rounded number of steps, not the ceiling, which keeps the vertex count of round offsets that the rest of the code is tuned for. |

## ClipperUtils semantics

The callers of `ClipperUtils` rely on a fixed set of behaviours. Where
Clipper2 behaves differently by default, the wrapper adjusts it.

### Booleans

- The fill rule is non-zero unless the function takes a `PolyFillType`. One
  rule applies to both subject and clip; Clipper2 has no per-operand rule.
- Collinear vertices are removed from the result. Clipper2 keeps them by
  default, so every boolean sets `PreserveCollinear(false)`.
- Outer contours are CCW and holes are CW. No output contour touches
  itself: where one would pass twice through a vertex, it is split there into
  two contours.
- `ExPolygons` results are built from one `PolyTree64` pass. An island inside
  a hole becomes an `ExPolygon` of its own.
- `ApplySafetyOffset::Yes` grows the clip polygons by `ClipperSafetyOffset`
  before an intersection or a difference, so that edges shared by subject and
  clip do not leave slivers.
- Open polylines are clipped with the non-zero rule and keep their direction.

### Offsets

- Before offsetting, input vertices closer than
  `ClipperOffsetShortestEdgeFactor` × |delta| to the previously kept vertex
  are dropped. This bounds the work on dense contours, and the error it
  introduces is far below the offset distance.
- The miter limit is at least 2. For `jtRound`, a positive `miterLimit`
  argument is the arc tolerance, capped at |delta| / 4, and 0.25 is used
  otherwise. Other joins use the smaller of 0.25 and |delta| / 4 for round end
  caps.
- A single `Polygon` keeps its orientation: a CCW polygon grows with a
  positive delta, a CW polygon is a hole and shrinks.
- `Polygons` follow the same rule per path. When every CW path lies strictly
  inside the bounding box of a CCW path, which is the usual case of contours
  with their holes, all paths are offset in one Clipper2 group. Otherwise
  each path is offset on its own and the results are united, with the
  non-zero rule when growing and the positive rule when shrinking.
- `ExPolygons` and `Surfaces` are offset as one group after the contours are
  oriented CCW and the holes CW, whatever their input orientation.
- Zero-area paths vanish under a negative offset instead of growing.
- Polyline offsets use the requested end type. Clipper2 already unites the
  result, so no further union is done.

### Coordinate range

Clipper2 computes intersections and slopes in doubles, which hold integers
exactly only up to 2^53 (about 9e15 units, 9,000 km). Geometry passed to
`ClipperUtils` must stay well inside that range; near the int64 limit the
results shift by hundreds of units. This is why the arrange `InfiniteBed` is a
box of ±2^50 units around its centre rather than libnest2d's infinite box,
which reaches ±2.3e18.

## ClipperZUtils

`ZPoint` is a `Vec3crd`, and a `ZPath` is a vector of them.
`clip_zpaths()` runs one boolean with the non-zero rule on the Clipper2 Z
build. The subject may be open, the clip is closed, and the result lists the
closed paths before the open ones.

The Z of each output vertex follows these rules:

- An input vertex keeps its Z.
- An intersection that lies on an end point of one of the two crossing edges
  takes that end point's Z, preferring the subject edge.
- Any other intersection gets its Z from the callback, which receives both
  crossing edges, the subject edge first.

Clipper2 calls the callback only when it creates an output vertex at an
intersection, not for every crossing it processes. A callback that records
intersections, like `ClipperZIntersectionVisitor`, therefore sees only those.

The users are:

| User | Z carries |
| --- | --- |
| `Algorithm::wave_seeds()` (region expansion) | source and boundary index; intersections get a negative index into the visitor's list of crossing pairs |
| `Algorithm::split_line()` | index of the source vertex; an intersection gets the negated index of its source edge, so the pieces can be put back in path order |
| `PerimeterGenerator` overhang and top-surface clipping of Arachne walls | extrusion width, interpolated along the edge at intersections |
| Tree support anchors in `SupportCommon` | index of the source contour, -1 at intersections |
| `extrusion_paths_append()` | extrusion width, turned into extrusion paths |

## Testing

`tests/libslic3r/test_clipper_utils.cpp` and `test_clipper_offset.cpp` cover
the wrapper's booleans, orientation and offset rules. The perimeter, support
and region expansion users are exercised by the slicing tests in
`tests/fff_print`.
