# Travel optimization — High Level Design

## Purpose and scope

Inside an island, the infill paths of each region are chained into one print
order by a greedy pass that joins the closest free path ends first. That pass
leaves its longest joins for last, and they often cross the whole island. Two
things shorten the travel between infill paths:

- the fill generators join their lines into as few paths as they can, since
  every separate path needs a travel to it;
- the G-code export refines the greedy chain with a local search that shortens
  the straight-line travel between the paths;
- a fill that keeps its order prints each piece of its gap fill where it passes
  by, since the G-code export cannot reorder it.

The refinement covers every chain that `chain_and_reorder_extrusion_entities()`
builds from a start point: the infill and ironing of `GCode::extrude_infill()`
and the support of `GCode::extrude_support()`, as well as the order of objects
and islands under `print_order` Default. It changes only the order and the
direction of the paths, not the paths or the walls. Neither part has a setting.

## The chain

`ObjectByExtruder::Island::Region::append()` flattens every sortable fill
collection into its paths, so the paths of all the fills of a region in one
island are chained together. A collection that must keep its order (`no_sort`:
monotonic, concentric, a forced fill order, the flow calibration) stays a single
entry that cannot be reversed. Paths that must keep their direction cannot be
reversed either: Grid's, since it alternates their direction between layers for
strength, and those of a fill with a forced order. A loop starts and ends at the
same point, so reversing it changes nothing.

`extrude_infill()` chains each region from the current nozzle position with
`chain_and_reorder_extrusion_entities()`, and chains the paths inside a nested
sortable collection the same way; `extrude_support()` chains the support paths
of a layer so too, unless they keep their order. The overload without a start
point, used to chain collections while slicing, keeps the greedy chain. The
order the fill generators give their paths does not reach the G-code for a
sortable fill, which is chained again.

## Connecting the lines

Most patterns cut their lines at the infill boundary, and `Fill::connect_infill()`
joins pairs of line ends that are neighbors on a boundary contour by the arc of
wall between them, shortest arcs first, as long as neither end is joined yet and
the join closes no loop. An arc is joined when it is shorter than
`infill_anchor_max`, or than two anchors and a line width if that is longer:
two ends further apart each get an anchor toward the other, and where those
would meet, the join prints about as much wall as one path instead of two.

Regular patterns leave many arcs of the same length, and the order among them
decides the pairing. In an arbitrary order the pairs along one wall can fall out
of step, such as lines 1–2 and 3–4, then 6–7 and 8–9, which leaves the end of
line 5 loose, and every loose end starts or ends a separate path. So the arcs
are also sorted in steps of half the line spacing and, within a step, along each
contour forwards and backwards, which pairs the ends of a wall in one phase.
Each order is replayed on the flags and a union-find of the lines, without
building any path, and the one that joins the most lines is used; a tie keeps
the shortest-first order.

The replay includes the last pass, which joins any two open ends that are
neighbors on a contour. With multiline infill the main pass leaves the copies of
one line to that pass, so that each copy reaches a wall of its own. Two
neighboring groups cut at both walls can then be joined at either wall by arcs
of the same length: at one, their open ends stay next to their own copies, which
the last pass joins, so each line prints out and back; at the other, the copies
end up in separate paths printed apart.

A line end left unpaired gets an anchor, a piece of the wall at most
`infill_anchor` long. It runs toward the nearest open end of another path,
looking up to 64 ends along the contour, since the next path usually starts
there; without one it takes the side with more free wall. With multiline infill
an anchor toward a copy of the same line stops short of the copy rather than
printing over it. A copy is a neighboring end less than the group width
(`multiline` spacings) across the line, at any wall angle, while the next group
lies a whole line spacing away. Meeting the wall at an angle θ, the copy covers
half a width over sin θ of the wall on each side of its end, so the anchor stops
that far and half its own width before it, or is left out; the end still counts
as anchored, so the last pass pairs the other ends as before.

Last, `Fill::fill_surface_extrusion()` joins the sparse infill lines whose ends
are at most two line spacings apart with a straight piece. With the anchors off
(`infill_anchor_max` 0) nothing above runs and this is the only joining; with
them on, it takes what the steps above leave next to each other, such as two
anchors that stop just short of each other or a short piece cut off by a corner.
A gap that short leaves no room to print anything in between, and as a travel it
is the one hop a reordering can trade for a longer travel that retracts.
Multiline copies leave the most such ends: one spacing apart across the line,
their ends where it meets a wall at an angle θ are one spacing over sin θ apart
along it, so two spacings reach walls met at 30° and steeper.

Ends that meet are joined first, then the gaps between the leading ends of their
lines, along each line's overall direction, then the rest, shortest first within
each, without closing a loop or crossing a line or another join. Taking the
leading ends first joins every pair of copies at the same side, which leaves
their open ends next to each other; by length alone, neighboring pairs were
joined at different walls and the travel between them crossed the island. Lines
that cannot reverse, such as Grid's, only join the end of one to the start of the
next, and fills that keep their order are left alone.

## Gap fill in a fill that keeps its order

`Fill::_create_gap_fill()` adds the gap fill of a solid surface, where
`gap_fill_target` asks for it, after the lines of the fill. The chain mixes it
in with the lines of a sortable fill like any other path. A fill that keeps its
order, such as monotonic, would print all of its lines and then come back for
every piece of gap fill, most of which lies at the walls where the lines end.

So each piece, in turn, goes between the two extrusions of the fill where it
adds the least travel, flipped when that is shorter. A closed piece counts from
its point nearest the nozzle, where `GCode::extrude_loop()` starts it. The two
ends of the fill cost nothing, since the travel to and from them depends on what
is printed before and after. The fill's own extrusions keep their order and
direction, so a monotonic fill stays monotonic. When the result travels no less
than the gap fill printed after the lines in its own order, that order is kept.

## The search

A chain of up to eight paths is found exactly, by dynamic programming over the
subsets of paths: a state is the paths visited so far, the last one and its
direction, about four thousand states for eight paths. The shared greedy
chaining, `chain_segments_greedy_constrained_reversals_()`, does the same for two
to four segments, which also orders up to four islands of a layer and the loops
of a perimeter collection exactly. Either way the first path is no longer
always the one nearest the start; with a free end, a path near the start can be
worth leaving for last.

Objects (`print_order` Default) and the tour of islands across objects are
visited in the same order on every layer, so `chain_points_with_postprocessing()`
orders them as a closed tour rather than an open chain. Up to ten points the
shortest tour is found exactly by the same dynamic programming with the first
point fixed; with a start point the tour then begins at the point nearest it.
Larger sets take the greedy chain and alternate 2-opt, or-opt and crossing
removal; or-opt moves runs of up to three points next to one of the eight points
nearest an end of the run, found on the same grid as the chain's candidates.

For longer chains, the search refines the greedy chain. On an island cut into many short pieces,
such as infill without anchors, that chain can sweep away from the nozzle and
come back for the rest with one long jump, which no local move below removes.
So a nearest-next chain, going from the nozzle to the nearest free path end each
time, is built too; when it is already shorter than the refined greedy chain, it
is refined and used instead. Elsewhere it costs one grid query per path.

`improve_chain_by_local_search()` sees the chain as a sequence of segments with
an entry and an exit point each. Its cost is the straight distance from the
nozzle to the first entry plus the distance from every exit to the next entry;
the end of the chain is free. It applies three kinds of moves:

- 2-opt reverses a run of segments, which flips each of them and replaces the
  two links at the ends of the run.
- Or-opt moves a run of up to three segments between two others, flipped or
  not.
- A run of any length that starts or ends next to the nozzle moves to the front.
  Local search can settle on a chain that starts with a block of paths far from
  the nozzle while the paths next to it sit in the middle, and moving that block
  takes more than three segments at once.

A move that would flip a segment that cannot be reversed is skipped, so such
segments are only reordered.

A move is tried only if every link it creates joins an end point to one of the
32 end points nearest to it, which covers every move on a chain of up to 17
segments, as most islands are; with 8 candidates the search missed most of the
gain on grid-like patterns such as Triangles. The candidates come from a uniform
grid of about two end points per cell, searched in rings of cells around the
query. `KDTreeIndirect` visits its subtrees in a fixed order rather than the
nearer one first, so a query for 32 points walks most of the tree, and on long
chains finding the candidates would cost more than the search itself.

The candidates of an end point are tried nearest first, and only while they are
closer to it than the link the move would replace there. A 2-opt move that pays
off creates a link shorter than the one it replaces at one of its two ends, so
this misses no 2-opt move. An or-opt move is tried only while its new link is
shorter than what taking the run out saves, which skips a few or-opt moves that
would still pay off but cuts the search on long chains to a fraction.

The search works through a queue of segments and applies the first move that
shortens the chain around the segment it examines. After a move, only the
segments next to a changed link are queued again. A new link can also make room
for a run elsewhere that no segment next to it would try, so when the queue
empties after any move, every segment is queued once more, and the search ends
when such a sweep finds no move.

Every move shortens the chain by more than `SCALED_EPSILON`, so the result is
never longer than the greedy or the nearest-next chain and the search ends; a
cap of 50 moves per segment only bounds its time. The search has no randomness
and visits the segments in a fixed order, so the same input always gives the
same G-code.

## Cost

Finding the candidates of an end point visits a few cells, and the nearest-next
chain makes one such query per path, which only grows where the paths around the
nozzle are used up and the chain jumps. Examining a segment tries at most a few
hundred candidate links, usually only the nearest few, plus a scan of the chain
for the move to the front when it is the first segment, and a move rewrites only
the range of the chain it changes, so the search stays close to linear in the
number of segments. Most islands hold a few to a few dozen paths; the cost shows
on infill cut into thousands of pieces per island, such as dense infill with the
anchors off. The straight joins only look at the line segments near a candidate
join for crossings.

## Limitations

The search minimizes straight-line distance. It does not know which travels
leave the infill area and retract under `reduce_infill_retraction`, nor the
detours of `reduce_crossing_wall`, and a travel's time is not proportional to
its length: a short travel is bound by acceleration, so its time grows about
with the square root of its length. A chain that is shorter in total but trades
very short travels for longer ones, or for ones past `retraction_minimum_travel`,
can occasionally take a little longer to print.

The end of the chain is free: it is not steered toward the next island or the
start of the next layer. The regions of an island are chained one after the
other, in region order, each from where the previous one ended.
