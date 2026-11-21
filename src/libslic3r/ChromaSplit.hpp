#pragma once

#include <vector>

#include "ChromaStack.hpp"
#include "TriangleMesh.hpp"

namespace Slic3r {

// A solved ChromaStack cut into one solid per band, so that a picture meant for a printer that swaps
// filament can be printed on one that does not.
//
// What a stack is, geometrically, is a plate whose height at every point decides which filaments the
// light passes through on its way back out. Splitting it by colour is the other way of making that
// object: instead of one plate printed in several filaments with a swap at each band boundary, one
// piece per band, each printed on its own in a single filament and the stack glued or pressed back
// together afterwards. It is also what feeds a mesh boolean, which is the other thing people do with
// these - a multi-material printer can be handed the pieces as separate parts rather than a height
// range per filament.
//
// Each piece is the region of the plate between the Z where its band starts and the Z where the next
// one does, clipped to the part of the picture that actually reaches that high. So a piece is not a
// slab: its top is the terraced surface the picture makes inside that band, and where the picture
// does not reach the band at all there is no piece there, which is what puts holes in the middle
// pieces and leaves the topmost ones as a handful of islands.
//
// Two properties are what make the result usable, and both are structural rather than hoped for:
//
//  - every piece is closed. Each edge of each piece belongs to exactly two triangles - the bottom,
//    the terraced top, the outer wall and the wall around every hole are all built from the same
//    cell quads, so there is nothing to leave open. A mesh with a hole in it is useless for a
//    boolean, which is the use this exists for;
//  - the pieces fit back together exactly. Piece n's top in the region where the picture carries on
//    upward is flat at the band boundary, and piece n+1's bottom is that same region at that same
//    height, generated from the same grid by the same arithmetic, so the two faces share their
//    vertex positions exactly rather than nearly. Stacked, the pieces fill the plate with no gap and
//    no overlap.
//
// The picture's own area is what is split. A frame (ChromaFrame) is a swept moulding whose colour at
// any height is decided by the swaps like any other surface, and splitting a sweep by Z planes is a
// different piece of geometry from splitting a height map; it is left out, and
// ChromaSplitPieces::frame_omitted says so, so a caller can tell the user that a plate to be split
// wants its frame width at zero.

// How much geometry a split may make of a picture.
//
// A piece is built on a grid of cells, one per texel of the solved height map, and a cell is about
// four triangles. A photograph solved at its own resolution is millions of texels, which is tens of
// millions of triangles per band and nothing downstream would thank us for it, so the grid is
// coarsened until it fits. The default is a 500 x 500 grid, which on a 100 mm plate is a fifth of a
// millimetre a cell - finer than any nozzle can print, and therefore finer than the split can
// usefully be. Raise it for a plate that is large or a picture that is being split for a boolean
// rather than for a printer.
// Which side of the picture the fill goes on.
//
// A stack carries its picture in the height of its columns, so the plate it makes tapers, and a flat
// tile has to put something where a short column is not. There are two places to put it, and they
// are not variations on one idea - they are different prints.
//
// `InFrontOfThePicture` keeps the stack where a HueForge puts it, measured from the backing, and
// fills the space between the terraces and the plate's near face. That space is on the eye's side of
// the picture, so whatever fills it is in the light's way before the light reaches any colour: it
// wants a filament that passes light, and it adds that filament's own haze, thickest where the
// picture is thinnest. This is the arrangement HueForge's FlatFill plugin prints.
//
// `BehindThePicture` hangs the stack from the plate's own face instead, so the face is flat and the
// terraces point inward, and fills the space behind them with the backing's own filament. Nothing
// stands between the eye and the colours, and no clear filament is needed at all, because the fill
// is the backing: it comes forward to meet the picture where the picture is thin, and it was never
// going to be seen.
//
// What changes optically is the order in which the light crosses the bands, and only that. A column
// of k layers holds the same k bands either way - the set is the same, the sequence is reversed -
// and absorption multiplies, so the hue a column shows is very nearly the same. What does differ is
// the part that depends on order: each layer's own scattered light, and which filament's surface the
// eye is looking at.
// `FlatStack` is the third, and it is a different print rather than a third place to put the fill.
// The other two keep the stack where a swapped print puts it, with every band at the same height
// across the whole plate, and differ only in which side the leftover space is on. FlatStack moves the
// bands instead: each column's own sequence is slid up against the flat face, so the band a column
// ends on is the one the eye meets first, exactly as it would on the terraced plate, and what varies
// from column to column is where each band sits rather than which of them are there.
//
// That is what makes it faithful and what makes it expensive. The light crosses the same filaments in
// the same order as on the plate the picture was solved for, so the colours are the solve's own and
// not a reversal of them. In return, one printed layer no longer holds one band: at a given height a
// tall column is deep in its stack while a short one has finished and is backing, so a layer holds
// several filaments and the printer swaps inside it.
enum class ChromaFlatMode
{
    ClearInFront,
    BackingBehind,
    FlatStack,
};

struct ChromaSplitOptions
{
    int max_cells = 250000;

    // Which flat tile to build. ClearInFront by default, because that is the arrangement this function
    // has always produced and the one the bands' own documented order describes; the other two give a
    // different set of pieces, and a caller asks for them by name.
    ChromaFlatMode flat_mode = ChromaFlatMode::ClearInFront;
};

// What chroma_split_bands() produced.
struct ChromaSplitPieces
{
    // One mesh per band of the stack, in stack order: `bands[i]` is printed in
    // `stack.bands[i].filament`, and it is empty for a band the picture never reaches and for a band
    // of no layers. Empty rather than degenerate on purpose - what to say about a band that prints
    // nothing is the caller's question, not this one's.
    //
    // The order is the stack's, not the printer's. Face up the two agree; face down the plate is
    // printed the other way up, so `bands.front()` is the piece at the *top* of the plate (the one
    // the backing is part of) and `bands.back()` is the one against the build plate. That is the same
    // reversal chroma_ranges() makes, and for the same reason: the light enters through the face that
    // printed first.
    //
    // A band that the picture reaches in several places that do not touch comes back as one mesh with
    // several shells rather than as several meshes, because the piece is one band and the caller's
    // own indexing is by band. its_split() separates the shells for a caller that wants one file per
    // island.
    std::vector<indexed_triangle_set> bands;

    // The solid that fills what the picture leaves empty, so the pieces together are the plate as a
    // flat slab rather than as a terraced one.
    //
    // A stack carries its picture in the height of its columns, so the plate it makes tapers: every
    // layer is a subset of the one below it, and a column that stops early leaves the space above it -
    // below it, face down - empty. That is what the plate is for, and it is also what stops it being
    // printed as a flat tile, where the whole footprint has to be there at every layer. This piece is
    // exactly that empty space: cell by cell, from the column's own terrace to the plate's own face.
    //
    // Built from the same grid and the same arithmetic as the bands, so the faces where it meets them
    // share their vertices exactly rather than nearly, and `bands` plus `fill` occupy the plate's full
    // box with no gap and no overlap.
    //
    // Which side it sits on, and therefore what it has to be printed in, is ChromaSplitOptions::
    // flat_mode. In front of the picture it stands between the eye and the stack and wants a filament
    // that passes light; behind it - which is both of the other two modes - it is continuous with the
    // backing and prints in the backing's own filament, `stack.bands.front().filament`.
    //
    // In front of the picture it is empty when the picture reaches the top of the stack everywhere,
    // which is the one case with nothing to fill. Behind it, it also carries the plate's solid part,
    // so it is empty only for a plate with no backing at all.
    indexed_triangle_set fill;

    // The grid the pieces were built on, after any coarsening (see ChromaSplitOptions::max_cells),
    // and the size of one of its cells in millimetres. Reported because it is the resolution of
    // everything above: the terraces step at this size in XY, and a silhouette that is not a
    // rectangle is cut to it.
    int   grid_width  = 0;
    int   grid_height = 0;
    float cell_x_mm   = 0.f;
    float cell_y_mm   = 0.f;

    // The plate had a frame, and the pieces do not. See the note at the top of this file.
    bool frame_omitted = false;

    bool empty() const { return bands.empty(); }
};

// Split a solved stack into one mesh per band.
//
// `solve` is the height map chroma_solve() produced for `stack`, and `stack.plate` is the plate the
// picture made for itself: the pieces are built over the picture's own rectangle, centred on the
// origin and sitting on z = 0, which is where chroma_plate_mesh() puts the plate they came from.
// Nothing comes back for a picture painted onto somebody else's model (`plate.generated` false),
// because then the plate is the model and this has no business deciding what shape it is.
//
// `pieces.fill` comes back with them, and is what turns the terraced plate into a flat one. Which
// flat tile is built, and so which filament the fill wants, is ChromaSplitOptions::flat_mode.
//
// Face down (ChromaOrientation::FaceDown) is split as faithfully as face up: there the relief is a
// recess cut up into the bottom of the plate and the bands print in reverse order, so the pieces come
// out with the terraces pointing down and the backing on top, which is the plate that mode actually
// prints. A face-down plate too thin to hold the whole stack is split as if it were exactly thick
// enough; below that the recess opens through the back, and a hole is not a colour anybody can print.
ChromaSplitPieces chroma_split_bands(const ChromaStack &stack, const ChromaSolve &solve,
                                     const ChromaSplitOptions &options = ChromaSplitOptions());

} // namespace Slic3r
