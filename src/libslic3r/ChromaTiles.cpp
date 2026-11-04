#include "ChromaTiles.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <nlohmann/json.hpp>

#include "Color.hpp"
#include "Point.hpp"

namespace Slic3r {

namespace {

// How close two millimetre dimensions have to be before a tile counts as fitting. A micron: well
// inside anything a printer cares about, and large enough that a tile whose width came out of a
// division does not fail to fit the bed it exactly fills.
constexpr float FIT_EPS_MM = 1e-3f;

// FNV-1a, which is here because the key has to be stable across runs and across machines: a plan goes
// into a project file and is compared against a stack read back out of one, so std::hash - which is
// allowed to salt itself per process - cannot be used for this.
struct KeyHash
{
    uint64_t h = 1469598103934665603ull;

    void byte(uint8_t b)
    {
        h ^= uint64_t(b);
        h *= 1099511628211ull;
    }
    void bytes(const void *p, size_t n)
    {
        const uint8_t *q = static_cast<const uint8_t *>(p);
        for (size_t i = 0; i < n; ++i)
            byte(q[i]);
    }
    void add(int v) { bytes(&v, sizeof(v)); }
    void add(uint32_t v) { bytes(&v, sizeof(v)); }
    void add(uint64_t v) { bytes(&v, sizeof(v)); }
    void add(bool v) { byte(v ? 1u : 0u); }
    void add(float v)
    {
        // The bits rather than a rounded value, so that two layer heights that differ in the last
        // place are two different prints, which they are: the swaps land at different Z. The one thing
        // worth normalising is the sign of zero, because -0 and 0 are the same height and would
        // otherwise hash apart.
        if (v == 0.f)
            v = 0.f;
        bytes(&v, sizeof(v));
    }
};

void key_filament(KeyHash &k, const ChromaFilament &f)
{
    // The index, the colour and the transmission distance, and not the name or where the TD came
    // from: a filament preset that was renamed between two sessions is the same filament to print
    // through, and a plan that had to be rebuilt over a rename would be rebuilt constantly.
    k.add(f.filament);
    k.add(f.color.r());
    k.add(f.color.g());
    k.add(f.color.b());
    k.add(f.td_mm);
}

void key_frame(KeyHash &k, const ChromaFrame &frame)
{
    k.add(int(frame.shape));
    k.add(int(frame.profile));
    k.add(frame.width_mm);
    k.add(frame.rise_mm);
    k.add(frame.corner_mm);
    k.add(uint64_t(frame.outline.size()));
    for (const Vec2f &p : frame.outline) {
        k.add(p.x());
        k.add(p.y());
    }
    k.add(uint64_t(frame.profile_points.size()));
    for (const Vec2f &p : frame.profile_points) {
        k.add(p.x());
        k.add(p.y());
    }
}

// Where the cut before index `k` of `n` falls, on a grid `n_px` wide.
//
// Two properties and the whole seam rests on them. The cut before k and the cut after k-1 are the
// same expression, so neighbouring tiles share a boundary exactly rather than nearly; and cut(0) is 0
// while cut(n) is n_px, so the tiles cover the picture with nothing left over at either end.
//
// `tile_px` is a fixed tile side in pixels of `ref_px` - the grid the plan was made on - or 0 for an
// even division. The fixed side is rescaled rather than reused, so the same tiles come out of a
// coarse preview grid as out of the picture itself.
int tile_cut(int k, int n, int n_px, int tile_px, int ref_px)
{
    if (n <= 0 || n_px <= 0 || k <= 0)
        return 0;
    if (k >= n)
        return n_px;
    if (tile_px > 0 && ref_px > 0) {
        const int64_t side = std::max<int64_t>(1, (int64_t(tile_px) * int64_t(n_px) + int64_t(ref_px) / 2) /
                                                      int64_t(ref_px));
        return int(std::min<int64_t>(int64_t(k) * side, int64_t(n_px)));
    }
    return int((int64_t(k) * int64_t(n_px) + int64_t(n) / 2) / int64_t(n));
}

// The fewest tiles along one axis whose footprint fits the printable area.
//
// The frame is why this is not a division. A tile on the outside of the assembled picture carries the
// moulding on the side that faces out, so the tiles that have to fit are not all the same size: with
// one tile it is carried on both sides, and with two or more every tile carries it at most once. Hence
// the two cases, and hence a printable area narrower than the frame itself defeating the whole thing -
// there is no count at which some tile does not carry it.
int axis_count(float picture_mm, float frame_mm, float avail_mm, bool &frame_too_wide)
{
    if (picture_mm <= 0.f || avail_mm <= 0.f)
        // No printable area was given, so the caller is asking for the geometry rather than for a fit.
        return 1;
    if (picture_mm + 2.f * frame_mm <= avail_mm + FIT_EPS_MM)
        return 1;
    if (avail_mm <= frame_mm + FIT_EPS_MM) {
        frame_too_wide = true;
        return 1;
    }
    const float per = avail_mm - frame_mm;
    return std::max(2, int(std::ceil(picture_mm / per - 1e-4f)));
}

// Lays the grid out and measures it. Everything about a tile is decided here, so there is one place
// where a tile's rectangle, its plate and its place in the assembled picture are made to agree.
void build_tiles(ChromaTilePlan &plan, int cols, int rows, int tile_px_x, int tile_px_y, float avail_w,
                 float avail_h, float frame_mm, bool &fits_x, bool &fits_y)
{
    const ChromaPlate &plate   = plan.stack.plate;
    const float        spacing = std::max(0.f, plan.options.spacing_mm);
    const int          halo_px = std::max(0, plan.options.halo_px);

    cols = std::max(1, cols);
    rows = std::max(1, rows);

    plan.cols      = cols;
    plan.rows      = rows;
    plan.tile_px_x = tile_px_x;
    plan.tile_px_y = tile_px_y;
    plan.tiles.assign(size_t(cols) * size_t(rows), ChromaTile{});

    fits_x = true;
    fits_y = true;

    // The cuts, computed once per axis rather than per tile, which is also what makes it obvious that
    // tile c's far edge and tile c+1's near edge are the same number and not two roundings of it.
    std::vector<int> xs(size_t(cols) + 1), ys(size_t(rows) + 1);
    for (int c = 0; c <= cols; ++c)
        xs[size_t(c)] = tile_cut(c, cols, plan.picture_width, tile_px_x, plan.picture_width);
    for (int r = 0; r <= rows; ++r)
        ys[size_t(r)] = tile_cut(r, rows, plan.picture_height, tile_px_y, plan.picture_height);

    // The assembled piece is the picture, plus the frame once on each outside edge, plus a gap between
    // every pair of neighbours. The picture's own contribution is the plate's full width because the
    // tile widths were cut out of it and add back up to it exactly.
    plan.assembled_width_mm  = plate.width_mm + 2.f * frame_mm + float(cols - 1) * spacing;
    plan.assembled_height_mm = plate.height_mm + 2.f * frame_mm + float(rows - 1) * spacing;

    const bool tiled   = cols * rows > 1;
    const bool shaped  = plate.frame.shape != ChromaFrame::Shape::Rectangle;
    const bool checker = plan.options.layout == ChromaTileLayout::Checkerboard;
    // Which parity prints. Inverting it is how the second colour set of a checkerboard is generated.
    const int  parity  = plan.options.skip_first ? 1 : 0;

    // Both of these are facts about the grid that was just chosen, and the grid is chosen more than
    // once: the fit loop builds, measures and builds again. They are cleared rather than accumulated so
    // that a flag raised by an attempt that was thrown away does not survive into the plan.
    plan.problems &= ~(uint32_t(ChromaTileProblem::OutlineDropped) | uint32_t(ChromaTileProblem::TooManyTiles));

    // How many tiles the layout wants, as against how many the budget lets through.
    size_t wanted  = 0;
    size_t printed = 0;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            ChromaTile &t = plan.tiles[size_t(r) * size_t(cols) + size_t(c)];
            t.col         = c;
            t.row         = r;

            t.core.x0 = xs[size_t(c)];
            t.core.x1 = xs[size_t(c) + 1];
            t.core.y0 = ys[size_t(r)];
            t.core.y1 = ys[size_t(r) + 1];

            t.halo.x0 = std::max(0, t.core.x0 - halo_px);
            t.halo.y0 = std::max(0, t.core.y0 - halo_px);
            t.halo.x1 = std::min(plan.picture_width, t.core.x1 + halo_px);
            t.halo.y1 = std::min(plan.picture_height, t.core.y1 + halo_px);

            t.edges.left   = c == 0;
            t.edges.right  = c == cols - 1;
            t.edges.top    = r == 0;
            t.edges.bottom = r == rows - 1;

            t.insets.left   = t.edges.left ? frame_mm : 0.f;
            t.insets.right  = t.edges.right ? frame_mm : 0.f;
            t.insets.top    = t.edges.top ? frame_mm : 0.f;
            t.insets.bottom = t.edges.bottom ? frame_mm : 0.f;

            // The plate is the plan's with a new picture area. The backing, the cell size and which way
            // up it prints are carried over untouched, and they have to be: the picture's surface has
            // to sit at the same print Z on every tile, or the swaps land at different places in the
            // picture and the colours step at the join.
            t.plate                 = plate;
            t.plate.width_mm        = float(t.core.width()) * plan.mm_per_px_x;
            t.plate.height_mm       = float(t.core.height()) * plan.mm_per_px_y;
            t.plate.frame.width_mm  = t.edges.any() ? frame_mm : 0.f;
            if (tiled && shaped) {
                // A shaped outline is centred on its own picture and grown symmetrically, so a tile cut
                // out of an oval plate would be handed an oval of its own. See
                // ChromaTileProblem::OutlineDropped.
                t.plate.frame.shape = ChromaFrame::Shape::Rectangle;
                t.plate.frame.outline.clear();
            }

            t.outer_width_mm  = t.plate.width_mm + t.insets.left + t.insets.right;
            t.outer_height_mm = t.plate.height_mm + t.insets.top + t.insets.bottom;

            t.offset_x_mm = frame_mm + float(t.core.x0) * plan.mm_per_px_x + float(c) * spacing;
            t.offset_y_mm = frame_mm + float(t.core.y0) * plan.mm_per_px_y + float(r) * spacing;

            // Every tile is measured against the bed, not only the ones this plan prints: the other
            // parity of a checkerboard is the same geometry printed by a second plan, and finding out
            // then that it does not fit is finding out too late.
            const bool fx = avail_w <= 0.f || t.outer_width_mm <= avail_w + FIT_EPS_MM;
            const bool fy = avail_h <= 0.f || t.outer_height_mm <= avail_h + FIT_EPS_MM;
            t.fits        = fx && fy;
            fits_x        = fits_x && fx;
            fits_y        = fits_y && fy;

            t.included = !checker || (((r + c) & 1) == parity);
            if (t.included) {
                ++wanted;
                if (plan.options.max_tiles > 0 && printed >= size_t(plan.options.max_tiles))
                    // Over budget. Still planned, so the tiles that do print line up with the ones a
                    // later plan prints, and left out, so the user gets the part of the picture they
                    // asked for rather than a set of plates they did not.
                    t.included = false;
                else
                    ++printed;
            }
        }
    }

    plan.fits_bed = fits_x && fits_y;
    if (tiled && shaped)
        plan.problems |= uint32_t(ChromaTileProblem::OutlineDropped);
    if (plan.options.max_tiles > 0 && wanted > size_t(plan.options.max_tiles))
        plan.problems |= uint32_t(ChromaTileProblem::TooManyTiles);
}

// Copies a rectangle out of a solve. The rectangle is in the solve's own pixels.
//
// Every per-pixel channel is copied rather than recomputed, which is the point: the grey map's
// encoding - 0 at base_layers, 255 at total_layers() - is a property of the stack and not of the
// picture, so the same grey means the same print height in every tile cut out of the same solve. A
// rescale here would be the one thing that could make two tiles disagree.
ChromaSolve copy_region(const ChromaSolve &src, int x0, int y0, int w, int h)
{
    ChromaSolve out;
    if (src.width <= 0 || src.height <= 0 || w <= 0 || h <= 0)
        return out;
    if (x0 < 0 || y0 < 0 || x0 + w > src.width || y0 + h > src.height)
        return out;

    out.width  = w;
    out.height = h;

    const size_t px = size_t(w) * size_t(h);
    const bool   has_levels  = src.levels.size() == size_t(src.width) * size_t(src.height);
    const bool   has_grey    = src.grey.size() == size_t(src.width) * size_t(src.height);
    const bool   has_preview = src.preview.size() == size_t(src.width) * size_t(src.height) * 3;
    const bool   has_delta   = src.delta.size() == size_t(src.width) * size_t(src.height);

    if (has_levels)
        out.levels.resize(px);
    if (has_grey)
        out.grey.resize(px);
    if (has_preview)
        out.preview.resize(px * 3);
    if (has_delta)
        out.delta.resize(px);

    for (int y = 0; y < h; ++y) {
        const size_t from = size_t(y0 + y) * size_t(src.width) + size_t(x0);
        const size_t to   = size_t(y) * size_t(w);
        if (has_levels)
            std::memcpy(out.levels.data() + to, src.levels.data() + from, size_t(w) * sizeof(uint16_t));
        if (has_grey)
            std::memcpy(out.grey.data() + to, src.grey.data() + from, size_t(w));
        if (has_preview)
            std::memcpy(out.preview.data() + to * 3, src.preview.data() + from * 3, size_t(w) * 3);
        if (has_delta)
            std::memcpy(out.delta.data() + to, src.delta.data() + from, size_t(w));
    }

    // The statistics are about this piece of the picture rather than the whole of it, because that is
    // the question a panel showing one tile has. They come off the error image, so they carry its
    // quantization: a byte over CHROMA_ERROR_IMAGE_FULL_SCALE_DE, which means the worst case saturates
    // there rather than reporting how much worse than hopeless it is.
    if (has_delta) {
        double sum  = 0.;
        uint8_t worst = 0;
        for (uint8_t d : out.delta) {
            sum += double(d);
            worst = std::max(worst, d);
        }
        const double scale = double(CHROMA_ERROR_IMAGE_FULL_SCALE_DE) / 255.;
        out.mean_delta_e   = px > 0 ? sum / double(px) * scale : 0.;
        out.max_delta_e    = double(worst) * scale;
    } else {
        out.mean_delta_e = src.mean_delta_e;
        out.max_delta_e  = src.max_delta_e;
    }
    return out;
}

} // namespace

size_t ChromaTilePlan::included_count() const
{
    size_t n = 0;
    for (const ChromaTile &t : tiles)
        if (t.included)
            ++n;
    return n;
}

uint64_t chroma_tile_stack_key(const ChromaStack &stack)
{
    KeyHash k;

    // What decides the colour at a height: how tall a layer is, how much of the column is backing, and
    // the bands themselves in order.
    k.add(stack.layer_height_mm);
    k.add(stack.base_layers);
    k.add(uint64_t(stack.bands.size()));
    for (const ChromaBand &band : stack.bands) {
        key_filament(k, band.filament);
        k.add(band.layers);
    }

    // The lighting model, which changes the palette and therefore every colour the stack prints.
    k.add(int(stack.lighting));
    k.add(stack.led_color.r());
    k.add(stack.led_color.g());
    k.add(stack.led_color.b());
    k.add(stack.led_intensity);
    k.add(stack.lighting_balance);

    // How the solve decides a pixel, which is not a property of the palette but is a property of the
    // height map, and two tiles solved by different rules do not meet.
    k.add(stack.dither);
    // How close a match has to be. Two tiles solved either side of a change to it take different
    // palette entries for the same colour, and the seam between them is where that shows.
    k.add(stack.match_strictness);
    k.add(uint64_t(stack.retouch.edits.size()));
    for (const ChromaRetouchEdit &edit : stack.retouch.edits) {
        k.add(int(edit.target));
        k.add(edit.value);
        k.add(edit.enabled);
        k.add(edit.region.width);
        k.add(edit.region.height);
        k.add(uint64_t(edit.region.runs.size()));
        for (uint32_t run : edit.region.runs)
            k.add(run);
    }

    // The plate, because the tiling is a division of it: the backing sets the Z the picture's surface
    // sits at, the frame sets what has to fit the bed, and the dimensions set the scale.
    k.add(stack.plate.generated);
    k.add(stack.plate.width_mm);
    k.add(stack.plate.height_mm);
    k.add(stack.plate.base_mm);
    k.add(stack.plate.cell_mm);
    k.add(int(stack.plate.orientation));
    key_frame(k, stack.plate.frame);

    // ChromaStack::importance is deliberately absent: it weights the error the solve reports and
    // changes no pixel's height, so two tiles solved with different importance regions still meet.
    // ChromaStack::base_auto is absent for the same reason - it says where base_layers came from, and
    // base_layers is already here. So is applied_ranges, which is a record of what was written to an
    // object rather than anything about the print.
    return k.h;
}

bool chroma_tile_stack_matches(const ChromaTilePlan &plan, const ChromaStack &stack)
{
    return plan.stack_key != 0 && plan.stack_key == chroma_tile_stack_key(stack);
}

bool chroma_tile_seam_ok(const ChromaTilePlan &plan)
{
    if (plan.cols <= 0 || plan.rows <= 0)
        return false;
    if (plan.tiles.size() != size_t(plan.cols) * size_t(plan.rows))
        return false;
    if (plan.picture_width <= 0 || plan.picture_height <= 0)
        return false;
    if (!(plan.mm_per_px_x > 0.f) || !(plan.mm_per_px_y > 0.f))
        return false;
    // The plan's own record of its stack has to be that stack, or the staleness check is comparing the
    // live stack against a number that means nothing.
    if (plan.stack_key != chroma_tile_stack_key(plan.stack))
        return false;

    const ChromaPlate &plate = plan.stack.plate;
    for (int r = 0; r < plan.rows; ++r) {
        for (int c = 0; c < plan.cols; ++c) {
            const ChromaTile &t = plan.tiles[size_t(r) * size_t(plan.cols) + size_t(c)];
            if (t.col != c || t.row != r)
                return false;
            // The picture is covered once: the first cut is the picture's edge, the last is its other
            // edge, and every tile starts where the one before it stopped.
            if (c == 0 && t.core.x0 != 0)
                return false;
            if (r == 0 && t.core.y0 != 0)
                return false;
            if (c == plan.cols - 1 && t.core.x1 != plan.picture_width)
                return false;
            if (r == plan.rows - 1 && t.core.y1 != plan.picture_height)
                return false;
            if (c > 0 && plan.tiles[size_t(r) * size_t(plan.cols) + size_t(c) - 1].core.x1 != t.core.x0)
                return false;
            if (r > 0 && plan.tiles[size_t(r - 1) * size_t(plan.cols) + size_t(c)].core.y1 != t.core.y0)
                return false;
            // The halo may be clipped by the picture's edge but can never cut into the tile.
            if (t.halo.x0 > t.core.x0 || t.halo.y0 > t.core.y0)
                return false;
            if (t.halo.x1 < t.core.x1 || t.halo.y1 < t.core.y1)
                return false;
            // Every tile prints the picture's surface at the same height, in the same direction, off
            // the same grid. This is the geometric half of the seam.
            if (t.plate.base_mm != plate.base_mm)
                return false;
            if (t.plate.orientation != plate.orientation)
                return false;
            if (t.plate.cell_mm != plate.cell_mm)
                return false;
            // A moulding may only sit where the assembled picture ends.
            if (t.insets.left != (t.edges.left ? t.plate.frame.width_mm : 0.f))
                return false;
            if (t.insets.right != (t.edges.right ? t.plate.frame.width_mm : 0.f))
                return false;
            if (t.insets.top != (t.edges.top ? t.plate.frame.width_mm : 0.f))
                return false;
            if (t.insets.bottom != (t.edges.bottom ? t.plate.frame.width_mm : 0.f))
                return false;
        }
    }
    return true;
}

ChromaTilePlan chroma_tile_plan(const ChromaStack &stack, int picture_width, int picture_height,
                                const ChromaTileOptions &options)
{
    ChromaTilePlan plan;
    plan.stack          = stack;
    plan.stack_key      = chroma_tile_stack_key(plan.stack);
    plan.options        = options;
    plan.picture_width  = std::max(0, picture_width);
    plan.picture_height = std::max(0, picture_height);
    // Error diffusion and retouch masks both reach past a tile's edge in ways no halo can contain;
    // see ChromaTilePlan::cut_required.
    plan.cut_required = stack.dither || !stack.retouch.empty();

    const ChromaPlate &plate = plan.stack.plate;

    if (plan.picture_width <= 0 || plan.picture_height <= 0 || plate.width_mm <= 0.f ||
        plate.height_mm <= 0.f)
        plan.problems |= uint32_t(ChromaTileProblem::NoPicture);
    if (!plate.generated)
        plan.problems |= uint32_t(ChromaTileProblem::NoPlate);
    if (plan.problems != 0) {
        // There is no division of a picture that is not there, and nothing to draw either, so this is
        // the one case that comes back without a grid at all.
        plan.fits_bed = false;
        return plan;
    }
    if (plan.stack.empty() || plan.stack.relief_layers() <= 0)
        // A plate with no picture on it is still a plate and still has to be cut up to fit the bed, so
        // this is a flag on a plan rather than a refusal to make one.
        plan.problems |= uint32_t(ChromaTileProblem::NoStack);

    plan.mm_per_px_x = plate.width_mm / float(plan.picture_width);
    plan.mm_per_px_y = plate.height_mm / float(plan.picture_height);

    const float frame_mm = std::max(0.f, plate.frame.width_mm);
    const float avail_w  = std::max(0.f, options.bed_width_mm - 2.f * options.bed_margin_mm);
    const float avail_h  = std::max(0.f, options.bed_height_mm - 2.f * options.bed_margin_mm);

    bool frame_too_wide = false;
    int  cols = options.cols > 0 ? options.cols : axis_count(plate.width_mm, frame_mm, avail_w, frame_too_wide);
    int  rows = options.rows > 0 ? options.rows : axis_count(plate.height_mm, frame_mm, avail_h, frame_too_wide);
    if (frame_too_wide)
        plan.problems |= uint32_t(ChromaTileProblem::FrameTooWide);

    if (options.max_per_row > 0 && cols > options.max_per_row) {
        cols = options.max_per_row;
        plan.problems |= uint32_t(ChromaTileProblem::RowCapBinds);
    }

    int tile_px_x = 0, tile_px_y = 0;
    if (options.square_tiles) {
        // A fixed side rather than an even division, because that is what makes the panels
        // interchangeable to look at. The trailing column and row carry whatever is left over, which is
        // the price of the lock and lands on the outside of the picture where a frame covers it.
        int side_px;
        if (options.cols > 0) {
            side_px = (plan.picture_width + options.cols - 1) / options.cols;
        } else if (options.rows > 0) {
            const int side_y = (plan.picture_height + options.rows - 1) / options.rows;
            side_px = int(std::lround(double(side_y) * double(plan.mm_per_px_y) / double(plan.mm_per_px_x)));
        } else {
            // The square has to fit both axes, and an edge tile carries the frame on the side facing
            // out, so the frame comes off once. That is conservative where an axis turns out to be a
            // single tile and carries it twice; the loop below finds that out and shrinks the side.
            float side_mm = (cols == 1 && rows == 1) ? std::max(plate.width_mm, plate.height_mm)
                                                     : std::min(avail_w, avail_h) - frame_mm;
            if (side_mm <= 0.f)
                side_mm = std::min(avail_w, avail_h);
            if (side_mm <= 0.f)
                side_mm = std::max(plate.width_mm, plate.height_mm);
            side_px = int(std::floor(side_mm / plan.mm_per_px_x));
        }
        tile_px_x = std::clamp(side_px, 1, std::max(1, plan.picture_width));
        tile_px_y = std::clamp(int(std::lround(double(tile_px_x) * double(plan.mm_per_px_x) /
                                               double(plan.mm_per_px_y))),
                               1, std::max(1, plan.picture_height));
        cols      = (plan.picture_width + tile_px_x - 1) / tile_px_x;
        rows      = (plan.picture_height + tile_px_y - 1) / tile_px_y;
    }

    // A hard ceiling on the grid, before anything is allocated. A count typed into a field is a number
    // the user can get wrong by three orders of magnitude, and a plan of a million tiles is not a plan
    // that is merely too big - it is one nothing can hold.
    cols = std::clamp(cols, 1, CHROMA_TILE_MAX_TILES);
    rows = std::clamp(rows, 1, std::max(1, CHROMA_TILE_MAX_TILES / cols));

    const bool cols_fixed = options.cols > 0 || (options.max_per_row > 0 && cols >= options.max_per_row);
    const bool rows_fixed = options.rows > 0;

    // Count, build, measure, cut finer, try again.
    //
    // A closed form would have to case-split on whether each axis ends up as a single tile - which
    // carries the frame on both of its sides rather than on one - and then again on how a square side
    // rounded. Measuring the tiles that were actually built is shorter, and it checks the thing that
    // has to be true anyway.
    bool fits_x = true, fits_y = true;
    for (int attempt = 0; attempt < 32; ++attempt) {
        build_tiles(plan, cols, rows, tile_px_x, tile_px_y, avail_w, avail_h, frame_mm, fits_x, fits_y);
        if (plan.fits_bed)
            break;

        bool finer = false;
        if (options.square_tiles) {
            if (!cols_fixed && !rows_fixed && tile_px_x > 1) {
                tile_px_x = std::max(1, tile_px_x - std::max(1, tile_px_x / 16));
                tile_px_y = std::clamp(int(std::lround(double(tile_px_x) * double(plan.mm_per_px_x) /
                                                       double(plan.mm_per_px_y))),
                                       1, std::max(1, plan.picture_height));
                cols  = (plan.picture_width + tile_px_x - 1) / tile_px_x;
                rows  = (plan.picture_height + tile_px_y - 1) / tile_px_y;
                finer = true;
            }
        } else {
            if (!fits_x && !cols_fixed && cols < plan.picture_width) {
                ++cols;
                finer = true;
            }
            if (!fits_y && !rows_fixed && rows < plan.picture_height) {
                ++rows;
                finer = true;
            }
        }
        if (!finer)
            break;
        if (int64_t(cols) * int64_t(rows) > int64_t(CHROMA_TILE_MAX_TILES)) {
            plan.problems |= uint32_t(ChromaTileProblem::TooManyTiles);
            break;
        }
    }

    if (!plan.fits_bed)
        plan.problems |= uint32_t(ChromaTileProblem::TileTooLarge);

    return plan;
}

const ChromaTile *chroma_tile_at(const ChromaTilePlan &plan, int col, int row)
{
    if (col < 0 || row < 0 || col >= plan.cols || row >= plan.rows)
        return nullptr;
    const size_t i = size_t(row) * size_t(plan.cols) + size_t(col);
    return i < plan.tiles.size() ? &plan.tiles[i] : nullptr;
}

std::vector<ChromaRange> chroma_tile_ranges(const ChromaTilePlan &plan, double z_bottom)
{
    return chroma_ranges(plan.stack, z_bottom);
}

ChromaTileRect chroma_tile_rect_on(const ChromaTilePlan &plan, size_t tile, int width, int height,
                                   int halo_px)
{
    ChromaTileRect rect;
    if (tile >= plan.tiles.size() || width <= 0 || height <= 0 || plan.cols <= 0 || plan.rows <= 0)
        return rect;

    const ChromaTile &t = plan.tiles[tile];
    rect.x0 = tile_cut(t.col, plan.cols, width, plan.tile_px_x, plan.picture_width);
    rect.x1 = tile_cut(t.col + 1, plan.cols, width, plan.tile_px_x, plan.picture_width);
    rect.y0 = tile_cut(t.row, plan.rows, height, plan.tile_px_y, plan.picture_height);
    rect.y1 = tile_cut(t.row + 1, plan.rows, height, plan.tile_px_y, plan.picture_height);

    if (halo_px > 0) {
        rect.x0 = std::max(0, rect.x0 - halo_px);
        rect.y0 = std::max(0, rect.y0 - halo_px);
        rect.x1 = std::min(width, rect.x1 + halo_px);
        rect.y1 = std::min(height, rect.y1 + halo_px);
    }
    return rect;
}

std::vector<uint8_t> chroma_tile_crop_rgb(const uint8_t *rgb, int width, int height,
                                          const ChromaTilePlan &plan, size_t tile)
{
    std::vector<uint8_t> out;
    if (rgb == nullptr || width <= 0 || height <= 0 || tile >= plan.tiles.size())
        return out;

    // Asked for on this grid rather than taken from the tile, so the same call works for the picture
    // itself and for the coarse grid the interactive solve runs on.
    const ChromaTileRect rect = chroma_tile_rect_on(plan, tile, width, height, std::max(0, plan.options.halo_px));
    const int            w    = rect.width(), h = rect.height();
    if (w <= 0 || h <= 0)
        return out;

    out.resize(size_t(w) * size_t(h) * 3);
    for (int y = 0; y < h; ++y) {
        const size_t from = (size_t(rect.y0 + y) * size_t(width) + size_t(rect.x0)) * 3;
        std::memcpy(out.data() + size_t(y) * size_t(w) * 3, rgb + from, size_t(w) * 3);
    }
    return out;
}

ChromaSolve chroma_tile_crop(const ChromaSolve &solved, const ChromaTilePlan &plan, size_t tile)
{
    if (tile >= plan.tiles.size() || solved.empty())
        return ChromaSolve{};

    // The halo rectangle on the grid the solve was actually run at, which is how a solve of the coarse
    // preview is cropped by the same call as a solve of the picture.
    const int halo_px = std::max(0, plan.options.halo_px);
    int       grid_w = plan.picture_width, grid_h = plan.picture_height;
    for (int trial = 0; trial < 2; ++trial) {
        const ChromaTileRect h = chroma_tile_rect_on(plan, tile, grid_w, grid_h, halo_px);
        if (h.width() == solved.width && h.height() == solved.height) {
            const ChromaTileRect core = chroma_tile_rect_on(plan, tile, grid_w, grid_h, 0);
            return copy_region(solved, core.x0 - h.x0, core.y0 - h.y0, core.width(), core.height());
        }
        if (trial == 0) {
            // Not the picture's own grid. Recover the grid the caller solved on from the halo
            // rectangle it handed back, which is the only thing it can have been.
            if (plan.cols <= 0 || plan.rows <= 0 || plan.picture_width <= 0 || plan.picture_height <= 0)
                break;
            const double sx = double(solved.width) * double(plan.picture_width) /
                              double(std::max(1, plan.tiles[tile].halo.width()));
            const double sy = double(solved.height) * double(plan.picture_height) /
                              double(std::max(1, plan.tiles[tile].halo.height()));
            grid_w = std::max(1, int(std::lround(sx)));
            grid_h = std::max(1, int(std::lround(sy)));
        }
    }
    return ChromaSolve{};
}

ChromaSolve chroma_tile_solve(const uint8_t *rgb, int width, int height, const ChromaTilePlan &plan,
                              size_t tile, const ChromaSolveOptions &options)
{
    // The refusal is the whole point: for a dithered or retouched stack there is no honest per-tile
    // solve, and handing one back would be the failure this file exists to prevent.
    if (plan.cut_required)
        return ChromaSolve{};
    if (tile >= plan.tiles.size())
        return ChromaSolve{};

    const std::vector<uint8_t> crop = chroma_tile_crop_rgb(rgb, width, height, plan, tile);
    if (crop.empty())
        return ChromaSolve{};

    const ChromaTileRect halo = chroma_tile_rect_on(plan, tile, width, height,
                                                    std::max(0, plan.options.halo_px));
    // Against the plan's stack, which is the only stack there is.
    const ChromaSolve solved = chroma_solve(crop.data(), halo.width(), halo.height(), plan.stack, options);
    if (solved.empty())
        return ChromaSolve{};

    const ChromaTileRect core = chroma_tile_rect_on(plan, tile, width, height, 0);
    return copy_region(solved, core.x0 - halo.x0, core.y0 - halo.y0, core.width(), core.height());
}

ChromaSolve chroma_tile_cut(const ChromaSolve &full, const ChromaTilePlan &plan, size_t tile)
{
    if (tile >= plan.tiles.size() || full.empty())
        return ChromaSolve{};
    const ChromaTileRect core = chroma_tile_rect_on(plan, tile, full.width, full.height, 0);
    return copy_region(full, core.x0, core.y0, core.width(), core.height());
}

namespace {

// A key that is absent, or there with the wrong type, leaves the field alone, which is what makes
// every key optional both ways round. The same rule chroma_stack_from_json() follows.
template<class T> void tiles_read(const nlohmann::json &j, const char *key, T &out)
{
    const auto it = j.find(key);
    if (it == j.end())
        return;
    try {
        out = it->get<T>();
    } catch (...) {
    }
}

} // namespace

void chroma_tiles_to_json(nlohmann::json &tiles, const ChromaTilePlan &plan)
{
    const ChromaTileOptions &o = plan.options;
    tiles["bed_width_mm"]      = o.bed_width_mm;
    tiles["bed_height_mm"]     = o.bed_height_mm;
    tiles["bed_margin_mm"]     = o.bed_margin_mm;
    tiles["spacing_mm"]        = o.spacing_mm;
    tiles["cols"]              = o.cols;
    tiles["rows"]              = o.rows;
    tiles["halo_px"]           = o.halo_px;
    tiles["layout"]            = int(o.layout);
    tiles["square_tiles"]      = o.square_tiles;
    tiles["max_per_row"]       = o.max_per_row;
    tiles["max_tiles"]         = o.max_tiles;
    tiles["skip_first"]        = o.skip_first;

    tiles["picture_width"]  = plan.picture_width;
    tiles["picture_height"] = plan.picture_height;

    // Written for a reader to compare against what it rebuilds, and ignored on the way back in: the
    // stack is written out beside it, so the key can always be recomputed and a stale one would only
    // be a second opinion about the same bytes.
    tiles["stack_key"] = plan.stack_key;

    // The stack the tiling was planned against, in full. Without it a project whose stack has since
    // been edited would reopen on a plan whose seams no longer hold and nothing would say so.
    nlohmann::json &stack = tiles["stack"];
    stack                 = nlohmann::json::object();
    chroma_stack_to_json(stack, plan.stack);
}

void chroma_tiles_from_json(const nlohmann::json &tiles, ChromaTilePlan &plan)
{
    if (!tiles.is_object())
        return;

    // Started from what the caller handed in, so a key this build has never heard of and a key an
    // older project never wrote both leave the field at whatever the caller already had. A caller that
    // seeds `plan` with the live stack and the picture's size therefore gets the saved options applied
    // to the picture it actually has.
    ChromaTileOptions o  = plan.options;
    ChromaStack       stack = plan.stack;
    int               pw = plan.picture_width;
    int               ph = plan.picture_height;

    tiles_read(tiles, "bed_width_mm", o.bed_width_mm);
    tiles_read(tiles, "bed_height_mm", o.bed_height_mm);
    tiles_read(tiles, "bed_margin_mm", o.bed_margin_mm);
    tiles_read(tiles, "spacing_mm", o.spacing_mm);
    tiles_read(tiles, "cols", o.cols);
    tiles_read(tiles, "rows", o.rows);
    tiles_read(tiles, "halo_px", o.halo_px);
    int layout = int(o.layout);
    tiles_read(tiles, "layout", layout);
    // Anything this build does not know is a plain grid, which is the layout that reproduces the whole
    // picture and the only one a tiling can be edited back out of.
    o.layout = (layout == int(ChromaTileLayout::Checkerboard)) ? ChromaTileLayout::Checkerboard :
                                                                 ChromaTileLayout::Grid;
    tiles_read(tiles, "square_tiles", o.square_tiles);
    tiles_read(tiles, "max_per_row", o.max_per_row);
    tiles_read(tiles, "max_tiles", o.max_tiles);
    tiles_read(tiles, "skip_first", o.skip_first);

    o.halo_px     = std::max(0, o.halo_px);
    o.cols        = std::max(0, o.cols);
    o.rows        = std::max(0, o.rows);
    o.max_per_row = std::max(0, o.max_per_row);
    o.max_tiles   = std::max(0, o.max_tiles);
    o.spacing_mm  = std::max(0.f, o.spacing_mm);

    tiles_read(tiles, "picture_width", pw);
    tiles_read(tiles, "picture_height", ph);

    if (const auto s = tiles.find("stack"); s != tiles.end() && s->is_object())
        chroma_stack_from_json(*s, stack);

    // The tiles themselves are not read, they are rebuilt: chroma_tile_plan() is deterministic, so a
    // tiling stored as its inputs cannot come back inconsistent with them, which a stored grid could.
    plan = chroma_tile_plan(stack, pw, ph, o);
}

} // namespace Slic3r
