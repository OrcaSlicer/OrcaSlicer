#ifndef slic3r_ChromaTiles_hpp_
#define slic3r_ChromaTiles_hpp_

#include <cstddef>
#include <cstdint>
#include <vector>

#include <cereal/cereal.hpp>
// ChromaTilePlan holds a ChromaStack, whose filaments carry their preset's name, so serializing a
// plan serializes a std::string. ChromaStack.hpp does not pull this in itself, and cereal answers a
// missing type serializer with a page of static_assert rather than a missing include.
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>
#include <nlohmann/json_fwd.hpp>

#include "ChromaStack.hpp"

namespace Slic3r {

// Splitting a picture that is bigger than the bed into tiles that are not.
//
// A ChromaStack picture is a plate, and a plate has a size the user chose for the wall it is going on
// rather than for the printer it has to come off. Past that size there is exactly one answer: print it
// in pieces and hang them next to each other. That is what this file plans.
//
// It plans and nothing else. The output is a ChromaTilePlan: how many tiles across and down, which
// rectangle of the picture each one covers, the plate each one is, and how big the finished thing
// comes out. Nothing here arranges anything on a bed, creates an object, or touches the plater - the
// caller does that with the plan in hand, because where a tile goes on the bed is a question about the
// user's print queue and not about the picture.
//
// The whole difficulty is the seam. Two tiles hanging next to each other have to agree about colour
// along the line where they meet, and a ChromaStack print has no per-object colour: a filament swap is
// one print Z across the whole plate, so what a point shows is decided by how high it is and by the
// stack it was solved against. Two tiles solved against stacks that differ in any way - one band a
// layer longer, a different layer height, a different base - put different colours at the same height,
// and the picture steps at every join. So the plan owns one stack, every tile is derived from it, and
// there is no API in this file by which a caller can hand a tile a stack of its own. See
// ChromaTilePlan::stack.
//
// The second difficulty is filtering. Anything that reads a pixel's neighbours behaves differently at
// the edge of its input than in the middle, and run per tile that difference lands precisely on the
// seam. The fix is a halo: see CHROMA_TILE_HALO_PX.
//
// The third is the frame. A frame around every tile puts a moulding through the middle of the
// assembled picture, which is not a picture in a frame, it is nine pictures in nine frames. The frame
// belongs on the outside of the assembled piece only, so each tile records which of its four sides are
// on that outside and the plate builder is told. See ChromaTile::edges.

// How much of the neighbouring picture a tile carries beyond its own edge, in pixels of the picture.
//
// Any filter that reads a pixel's neighbours - a blur, a sharpen, an unsharp mask, an edge-preserving
// denoise, a local tone curve - gives a different answer at the edge of its input than in the middle,
// because at the edge it has to invent the neighbours it cannot see. Run per tile, that difference
// lands exactly on the seam, which is the one line in the assembled picture the eye is already looking
// for. The halo is the answer: the tile is prepared and solved over a rectangle wider than it prints,
// and the extra ring is thrown away afterwards, so every pixel that survives was filtered with its
// real neighbours.
//
// Eight pixels is a kernel radius of eight, which covers every neighbourhood filter in this pipeline
// with room over, and it costs almost nothing: on a tile a few hundred pixels a side it is a few
// percent more pixels to solve and they are discarded.
//
// It does not fix error diffusion, and nothing of this shape can. Floyd-Steinberg's support is the
// rest of the picture rather than a kernel of any radius, so a dithered tile solved on its own
// disagrees with its neighbour however wide the halo is. That is what chroma_tile_cut() is for, and
// why ChromaTilePlan::cut_required exists.
static constexpr int CHROMA_TILE_HALO_PX = 8;

// The most tiles one plan may hold. A picture a metre across on a small bed is a legitimate request;
// four hundred plates is not a print, it is a misunderstanding, and the planner should say so rather
// than hand back a plan nobody can execute.
static constexpr int CHROMA_TILE_MAX_TILES = 400;

// How the tiles are laid out.
enum class ChromaTileLayout : int
{
    // Every tile of the grid prints. The default, and the only layout that reproduces the picture in
    // one pass.
    Grid         = 0,
    // Every other tile prints; see ChromaTileOptions::skip_first.
    Checkerboard = 1,
};

// What is wrong with a plan, as a bitmask over ChromaTilePlan::problems.
//
// A bitmask of causes rather than a list of sentences because this is libslic3r: the GUI has the
// message catalogue and the room to phrase it, and a translated string built down here could only be
// assembled out of fragments, which is the one thing a translator cannot work with.
enum class ChromaTileProblem : uint32_t
{
    None = 0,
    // No picture to split: a zero-sized image, or a plate with no area.
    NoPicture = 1u << 0,
    // The stack prints nothing - no bands, or no relief layers - so there is no picture to tile.
    NoStack = 1u << 1,
    // The stack's plate is not a generated one. A picture painted onto somebody's model cannot be
    // tiled: the model is the model, and cutting it up is not this file's business.
    NoPlate = 1u << 2,
    // At least one tile does not fit the printable area even after splitting. Either a count was
    // forced that is too coarse, or the bed is smaller than the frame needs.
    TileTooLarge = 1u << 3,
    // The frame alone is as wide as the printable area, so no number of tiles helps: every tile on the
    // outside of the picture carries it.
    FrameTooWide = 1u << 4,
    // The grid needs more tiles than ChromaTileOptions::max_tiles allows, or more than
    // CHROMA_TILE_MAX_TILES. The surplus tiles are planned and left out of the print, so the plan is a
    // part of the picture rather than the whole of it.
    TooManyTiles = 1u << 5,
    // ChromaTileOptions::max_per_row is what decided the number of columns rather than the bed, which
    // is worth saying because it is the reason a tile may be smaller than it had to be.
    RowCapBinds = 1u << 6,
    // The plate's outline was a shape rather than a rectangle - an ellipse, or an SVG silhouette - and
    // the tiles are rectangles instead.
    //
    // ChromaFrame's outline is centred on the picture and grown symmetrically, so a tile cut out of an
    // oval plate would be given an oval of its own rather than its share of the big one, and nine
    // small ovals is not an oval. Clipping the silhouette per tile is a real feature and is not this
    // one; until then a tiled plate is rectangular and the plan says so.
    OutlineDropped = 1u << 7,
};

inline bool chroma_tile_has(uint32_t problems, ChromaTileProblem p)
{
    return (problems & uint32_t(p)) != 0u;
}

// A rectangle of the picture, in the picture's own pixels: [x0, x1) by [y0, y1), row 0 at the top, the
// same way round as every other image in ChromaStack.
struct ChromaTileRect
{
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;

    int  width() const { return x1 > x0 ? x1 - x0 : 0; }
    int  height() const { return y1 > y0 ? y1 - y0 : 0; }
    bool empty() const { return width() == 0 || height() == 0; }

    bool operator==(const ChromaTileRect &o) const { return x0 == o.x0 && y0 == o.y0 && x1 == o.x1 && y1 == o.y1; }
    bool operator!=(const ChromaTileRect &o) const { return !(*this == o); }

    template<class Archive> void serialize(Archive &ar) { ar(x0, y0, x1, y1); }
};

// Which of a tile's four sides are on the outside of the assembled picture.
//
// This is the whole of what HugeForge learned between 1.1 and 1.2.0: it put a border around every
// tile first, and a border around every tile is a moulding running through the middle of the finished
// piece. A side that faces a neighbouring tile gets nothing at all, so the two pictures meet.
//
// Named for the picture rather than for the bed: `top` is the side at the picture's first row, and it
// is the caller's job to map that onto the plate the same way its height map projection does. Nothing
// here knows which way round the picture lands on the mesh.
struct ChromaTileEdges
{
    bool left   = true;
    bool right  = true;
    bool top    = true;
    bool bottom = true;

    bool any() const { return left || right || top || bottom; }
    bool all() const { return left && right && top && bottom; }
    int  count() const { return int(left) + int(right) + int(top) + int(bottom); }

    template<class Archive> void serialize(Archive &ar) { ar(left, right, top, bottom); }
};

// How far a tile's plate runs past its picture area on each of the four sides, in millimetres. It is
// the frame's width on a side that is on the outside of the assembled piece and zero on a side that
// meets a neighbour, so it is ChromaTileEdges expressed as a dimension.
struct ChromaTileInsets
{
    float left   = 0.f;
    float right  = 0.f;
    float top    = 0.f;
    float bottom = 0.f;

    template<class Archive> void serialize(Archive &ar) { ar(left, right, top, bottom); }
};

// One tile: a rectangle of the picture and the plate that prints it.
//
// There is deliberately no stack here. A tile carries no colour decision of its own, because every
// colour decision in a ChromaStack print is a print height and a print height is the same on every
// plate that comes off the same plan. ChromaTilePlan::stack is the only stack there is.
struct ChromaTile
{
    int col = 0;
    int row = 0;

    // What this tile prints, in the picture's pixels. Two neighbouring tiles share a boundary exactly:
    // the left one's x1 is the right one's x0, with no gap and no overlap, which is what makes the
    // same picture row land at the same height on both sides of the seam.
    ChromaTileRect core;
    // `core` grown by ChromaTileOptions::halo_px and clamped to the picture, which is the rectangle to
    // prepare and solve. The ring between the two is thrown away; see CHROMA_TILE_HALO_PX.
    ChromaTileRect halo;

    // Which sides are on the outside of the assembled picture, and the frame width that follows.
    ChromaTileEdges  edges;
    ChromaTileInsets insets;

    // The tile's own plate: its picture area in millimetres, and the backing, cell size, orientation
    // and frame it inherits from the plan's plate. The backing and the orientation are the plan's
    // unchanged, and they have to be - the picture's surface has to sit at the same print Z on every
    // tile or the swaps land in different places in the picture.
    //
    // The frame on this plate is symmetric, because that is the only frame ChromaPlate can describe:
    // `frame.width_mm` is the plan's on a tile that has any outer side at all and zero on an interior
    // tile. A plate builder that is handed this must cut the frame back to `insets` on the sides that
    // meet a neighbour. The one tiling where that is unnecessary is a single tile, which is on the
    // outside on all four sides and is exactly the plate the picture would have made untiled.
    ChromaPlate plate;

    // Whether this plan prints this tile. False for the half of a checkerboard that the other colour
    // set covers, and for a tile beyond ChromaTileOptions::max_tiles. Planned either way, so the
    // geometry of the assembled piece is complete and both halves of a checkerboard agree about where
    // every seam falls.
    bool included = true;

    // Whether the tile's footprint fits the printable area the options described.
    bool fits = true;

    // The tile's footprint on the bed: its picture area plus `insets`. This, not the picture area, is
    // what has to fit the printable area, which is the whole reason the insets are in the plan.
    float outer_width_mm  = 0.f;
    float outer_height_mm = 0.f;

    // Where this tile's picture area starts in the assembled piece, measured in millimetres from the
    // assembled piece's own top left corner - so the outer frame and every gap to the left of and
    // above this tile are included. What it is for is hanging the pieces: the user needs to know the
    // offsets, and so does anything that wants to draw the layout.
    float offset_x_mm = 0.f;
    float offset_y_mm = 0.f;

    template<class Archive> void serialize(Archive &ar)
    {
        // Appended, never reordered; see ChromaStack::serialize().
        ar(col, row, core, halo, edges, insets, plate, included, fits);
        ar(outer_width_mm, outer_height_mm, offset_x_mm, offset_y_mm);
    }
};

// What the user chose about the tiling. Persisted with the project, because a tiling nobody can reopen
// is a tiling that has to be done again from memory, and the settings are what the plan is rebuilt
// from: the plan itself is derived and chroma_tile_plan() is deterministic.
struct ChromaTileOptions
{
    // The printable area, in millimetres: the largest axis-aligned rectangle a tile may occupy. A
    // rectangle rather than the bed's polygon because that is what a rectangular tile needs, and
    // because the caller is the only thing that knows the bed shape - for a delta or any other
    // non-rectangular bed it passes the largest rectangle that fits, which is its own business.
    float bed_width_mm  = 0.f;
    float bed_height_mm = 0.f;
    // Kept clear on every side of the printable area, for the skirt, the brim, and the user's own
    // opinion of how close to the edge they are willing to print.
    float bed_margin_mm = 0.f;

    // The gap left between two tiles when the pieces are hung, in millimetres. It is part of the
    // finished thing rather than a slicing detail - a deliberate reveal between the panels is a
    // decision about how the piece looks - so it counts toward the assembled size and nothing about
    // the picture is scaled to absorb it. Zero means the tiles butt up against each other.
    float spacing_mm = 0.f;

    // How many tiles across and down, or 0 to take whatever fits the printable area. A forced count is
    // honoured even when the tiles do not fit, because the user may be about to change the bed or the
    // plate size and a planner that quietly overrode them would be impossible to drive; the plan says
    // so through ChromaTileProblem::TileTooLarge instead.
    int cols = 0;
    int rows = 0;

    // How much of the neighbouring picture each tile carries past its own edge; see
    // CHROMA_TILE_HALO_PX. Zero is honest when nothing filters the picture, and costs a seam the
    // moment something does.
    int halo_px = CHROMA_TILE_HALO_PX;

    ChromaTileLayout layout = ChromaTileLayout::Grid;

    // Lock the tiles to squares. A tile's millimetres per pixel is the same on both axes - the plate
    // keeps the picture's proportions - so a square in millimetres is a square in pixels, and the
    // tiles come out at one fixed side with the trailing column and row carrying whatever is left
    // over. What it is for is a set of panels that are interchangeable to look at; the remainder strip
    // is the price, and it is on the outside where a frame covers it.
    bool square_tiles = false;

    // Caps on the grid, 0 for none. `max_per_row` is a limit on the columns, and `max_tiles` on the
    // tiles this plan actually prints: the grid is planned in full either way, and the surplus is
    // marked not included so that what is left still lines up with what a later plan prints.
    int max_per_row = 0;
    int max_tiles   = 0;

    // Invert the checkerboard's parity, so that the tiles this plan leaves out are the ones it printed
    // before. Printing one parity in one set of filaments and the other parity in another gets a
    // picture out of twice as many colours as the printer has extruders, and this toggle is how the
    // second half is generated: the same plan, the same seams, the other half of the squares. Ignored
    // by ChromaTileLayout::Grid.
    bool skip_first = false;

    bool operator==(const ChromaTileOptions &o) const
    {
        return bed_width_mm == o.bed_width_mm && bed_height_mm == o.bed_height_mm &&
               bed_margin_mm == o.bed_margin_mm && spacing_mm == o.spacing_mm && cols == o.cols &&
               rows == o.rows && halo_px == o.halo_px && layout == o.layout &&
               square_tiles == o.square_tiles && max_per_row == o.max_per_row && max_tiles == o.max_tiles &&
               skip_first == o.skip_first;
    }
    bool operator!=(const ChromaTileOptions &o) const { return !(*this == o); }

    template<class Archive> void serialize(Archive &ar)
    {
        // Appended, never reordered; see ChromaStack::serialize().
        ar(bed_width_mm, bed_height_mm, bed_margin_mm, spacing_mm, cols, rows, halo_px);
        ar(layout, square_tiles, max_per_row, max_tiles, skip_first);
    }
};

// The tiling: one stack, one grid, and a plate per tile.
struct ChromaTilePlan
{
    // The one stack every tile of this plan is solved against, held by value.
    //
    // This is the seam, and it is the reason the plan is shaped the way it is. A ChromaStack print has
    // no per-object colour: a swap is a print Z across the whole plate, so the colour at a point is a
    // function of the point's height and of the stack it was solved against, and two tiles whose
    // stacks differ in any respect - a band one layer longer, another layer height, another base, a
    // different lamp behind a back-lit plate - put different colours at the same height and the
    // picture steps at every join.
    //
    // So it is here rather than in ChromaTile, every tile's plate is derived from `plate` below, and
    // the only entry points that solve a tile (chroma_tile_solve(), chroma_tile_cut()) take the plan
    // and a tile index. There is no overload that takes a stack: a caller cannot pass one tile a stack
    // the others did not get, because there is nowhere to put it.
    //
    // What remains possible is for the stack to be edited after the plan was made, which is a stale
    // plan rather than an inconsistent one. `stack_key` is how that is caught.
    ChromaStack stack;

    // A fingerprint of everything about `stack` that decides what colour a printed layer shows, taken
    // when the plan was built. chroma_tile_stack_matches() compares it against the live stack, which is
    // what tells the GUI that the plan has to be rebuilt before anything is printed from it.
    uint64_t stack_key = 0;

    ChromaTileOptions options;

    // The grid. `tiles` holds cols * rows of them, row-major, included or not.
    int cols = 0;
    int rows = 0;

    // The picture this plan was made for, in pixels, and the scale that ties it to the plate.
    //
    // One scale per axis, and they are equal for any plate a picture made for itself, since the plate
    // follows the picture's proportions. They are kept separately anyway so the tiles always add back
    // up to the plate the user set, even for a plate whose proportions were overridden by hand.
    //
    // The scale is the same for every tile, which is the point of it being here: a tile's millimetres
    // are its pixel rectangle times this, never the plate divided by the tile count, so the tile
    // widths sum to the picture's width exactly rather than to the picture's width plus a rounding
    // error per tile - and a rounding error per tile is a visible step at every seam.
    int   picture_width  = 0;
    int   picture_height = 0;
    float mm_per_px_x    = 0.f;
    float mm_per_px_y    = 0.f;

    // The side of a square tile, in pixels of the picture, or 0 on each axis when the picture is
    // divided evenly instead. Kept because it is what chroma_tile_rect_on() needs in order to cut the
    // same tiles out of a grid of a different size.
    int tile_px_x = 0;
    int tile_px_y = 0;

    // The finished thing, millimetres, spacing and outer frame included. The spacing is in here on
    // purpose: a user who leaves two millimetres between nine panels has built a picture six
    // millimetres wider than the one they asked for, and the only place they will find that out before
    // it is on the wall is this number.
    float assembled_width_mm  = 0.f;
    float assembled_height_mm = 0.f;

    // Whether every included tile fits the printable area.
    bool fits_bed = true;

    // Whether this plan has to go through chroma_tile_cut() rather than chroma_tile_solve().
    //
    // Two things in a stack reach across a tile boundary and no halo can contain them. Error diffusion
    // (ChromaStack::dither) passes its residual along the whole row, so two tiles dithered separately
    // disagree at the join. A retouch edit's mask is in the picture's own coordinates and chroma_solve()
    // samples a mask over whatever grid it is handed, so a tile-sized grid stretches the edit over the
    // tile. Either way the answer is the same: solve the picture once and cut the result up.
    //
    // chroma_tile_solve() refuses when this is set, rather than returning a solve with a seam in it.
    bool cut_required = false;

    // A bitmask of ChromaTileProblem.
    uint32_t problems = 0;

    bool empty() const { return tiles.empty(); }

    // How many tiles this plan prints, which is all of them in a grid and about half in a
    // checkerboard.
    size_t included_count() const;

    std::vector<ChromaTile> tiles;

    template<class Archive> void serialize(Archive &ar)
    {
        // Appended, never reordered; see ChromaStack::serialize().
        ar(stack, stack_key, options, cols, rows, picture_width, picture_height);
        ar(mm_per_px_x, mm_per_px_y, tile_px_x, tile_px_y);
        ar(assembled_width_mm, assembled_height_mm, fits_bed, cut_required, problems, tiles);
    }
};

// A fingerprint of everything about a stack that decides what colour a printed layer shows: the layer
// height, the base, every band's filament and length, the lighting, and whether the solve diffuses its
// error or carries edits. Two stacks with the same key print the same colour at the same height, which
// is the only property a seam needs.
//
// It is not a hash of the struct. ChromaStack::applied_ranges is a record of what was written to the
// object and ChromaFilament::name and td_source are labels, so none of them belongs in a key that
// answers "will these two tiles match": a stack whose filament preset was renamed is the same print.
uint64_t chroma_tile_stack_key(const ChromaStack &stack);

// Whether `stack` is still the stack the plan was built against, i.e. whether the plan may still be
// printed from. A plan whose stack has moved under it has to be rebuilt with chroma_tile_plan(); the
// tiles it already holds would each be right and would not agree with each other.
bool chroma_tile_stack_matches(const ChromaTilePlan &plan, const ChromaStack &stack);

// Whether the plan is internally consistent: its key is its stack's, its tiles tile the picture with
// no gap and no overlap, and every tile agrees about the scale, the backing and which way up it
// prints. chroma_tile_plan() always produces one that is; this is for a plan that came off a project
// file or an undo stack, where the bytes are whatever was written.
bool chroma_tile_seam_ok(const ChromaTilePlan &plan);

// Plans the tiling of `picture_width` x `picture_height` pixels, for the plate and the stack that
// `stack` carries.
//
// The stack is the whole input apart from the options, which is deliberate: ChromaStack::plate is the
// plate the picture would make at full size, so there is no way to hand this a plate from one stack
// and bands from another. The plate has to be a generated one (ChromaPlate::generated); a picture
// painted onto a model gives a plan with ChromaTileProblem::NoPlate and no tiles.
//
// It always returns a plan, never throws and never refuses: a plan that cannot be printed says so
// through `problems` and still describes the grid it would have been, because that is what a panel has
// to draw in order to tell the user what to change.
ChromaTilePlan chroma_tile_plan(const ChromaStack &stack, int picture_width, int picture_height,
                                const ChromaTileOptions &options);

// The tile at (col, row), or nullptr when there is none.
const ChromaTile *chroma_tile_at(const ChromaTilePlan &plan, int col, int row);

// The swaps every tile of this plan prints.
//
// It is chroma_ranges() of the plan's stack and nothing else, which is the point of it existing: one
// set of height ranges, derived once, applied to every tile. A caller that reaches for the stack per
// tile instead has written the bug this whole file is arranged to prevent.
std::vector<ChromaRange> chroma_tile_ranges(const ChromaTilePlan &plan, double z_bottom = 0.);

// A tile's rectangle on a grid of a different size, which is what the coarse preview the GUI solves on
// every keystroke is: a few hundred texels a side rather than the picture's own millions.
//
// The cuts are mapped rather than recomputed, so neighbouring rectangles still meet exactly on the
// coarse grid: whatever the scale, tile n's far edge and tile n+1's near edge come out of the same
// expression. `halo_px` is in pixels of the grid being asked about, not of the picture.
ChromaTileRect chroma_tile_rect_on(const ChromaTilePlan &plan, size_t tile, int width, int height,
                                   int halo_px = 0);

// The tile's halo rectangle out of `rgb` (width * height * 3 bytes, row-major, top-to-bottom), as a
// tight buffer of its own, ready to hand to chroma_solve() or to a filter. Empty when the tile or the
// picture is not there.
//
// It is the halo rather than the core on purpose: everything that runs over this buffer should see the
// neighbouring pixels, and the ring is dropped afterwards by chroma_tile_crop().
std::vector<uint8_t> chroma_tile_crop_rgb(const uint8_t *rgb, int width, int height,
                                          const ChromaTilePlan &plan, size_t tile);

// Drops the halo from a solve that was run over a tile's halo rectangle, leaving the tile's core.
//
// `solved` must be the halo rectangle's own size, which is what chroma_tile_crop_rgb() hands out. The
// grey map is not rescaled, and must not be: its encoding - 0 at base_layers, 255 at total_layers() -
// is a property of the stack, so the same grey means the same print height on every tile, and that is
// precisely what makes the pieces line up. The statistics are recomputed over what is left, from the
// error image, so they are quantized to ChromaSolve::delta's byte and saturate at
// CHROMA_ERROR_IMAGE_FULL_SCALE_DE.
ChromaSolve chroma_tile_crop(const ChromaSolve &solved, const ChromaTilePlan &plan, size_t tile);

// Solves one tile on its own: crops its halo rectangle out of `rgb`, solves that against the plan's
// stack, and drops the halo.
//
// Returns an empty solve when ChromaTilePlan::cut_required is set, because for a dithered or retouched
// stack a per-tile solve has a seam in it by construction and handing one back would be the one
// failure this file exists to prevent. Use chroma_tile_cut() there.
ChromaSolve chroma_tile_solve(const uint8_t *rgb, int width, int height, const ChromaTilePlan &plan,
                              size_t tile, const ChromaSolveOptions &options = ChromaSolveOptions{});

// Cuts a tile out of a solve of the whole picture.
//
// This is the seam-exact path and the one to prefer whenever the whole picture can be solved in one
// go. Every pixel was matched, dithered and retouched in the company of its real neighbours, and the
// cut is a copy: two tiles cut from one solve cannot disagree about anything, including the things a
// halo cannot reach. `full` must be the picture's own size.
ChromaSolve chroma_tile_cut(const ChromaSolve &full, const ChromaTilePlan &plan, size_t tile);

// The plan as the keys of the .3mf's `chroma_tiles` object, and back.
//
// What is written is the options, the picture's size and the stack, which is everything the plan is
// derived from; the tiles are rebuilt by chroma_tile_plan() on the way back in, because it is
// deterministic and a derived field in a project file is a derived field that can be inconsistent with
// what it was derived from. The saved stack is what makes the tiling reopenable at all: without it a
// project whose stack has since been edited would reopen on a plan whose seams no longer hold and
// nothing would say so.
//
// Keys are only ever added, and every one of them is optional on the way back in. A plan read out of a
// project written before a field existed comes back with that field at its default, which by
// construction is the tiling the project had.
void chroma_tiles_to_json(nlohmann::json &tiles, const ChromaTilePlan &plan);
void chroma_tiles_from_json(const nlohmann::json &tiles, ChromaTilePlan &plan);

} // namespace Slic3r

#endif // slic3r_ChromaTiles_hpp_
