#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdint>
#include <sstream>
#include <vector>

#include <cereal/archives/binary.hpp>
#include <nlohmann/json.hpp>

#include "libslic3r/ChromaStack.hpp"
#include "libslic3r/ChromaTiles.hpp"
#include "libslic3r/Color.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

ChromaFilament filament(int index, const ColorRGB &color, float td_mm)
{
    ChromaFilament f;
    f.filament = index;
    f.color    = color;
    f.td_mm    = td_mm;
    return f;
}

ChromaBand band(const ChromaFilament &f, int layers)
{
    ChromaBand b;
    b.filament = f;
    b.layers   = layers;
    return b;
}

// A plate of `plate_w` x `plate_h` millimetres with a frame `frame_mm` wide, carrying a two band
// stack. Every geometric test below sets the plate so that one millimetre is a whole number of
// pixels, which keeps the expected tile sizes exact rather than approximate.
ChromaStack make_stack(float plate_w, float plate_h, float frame_mm)
{
    ChromaStack s;
    s.layer_height_mm = 0.1f;
    s.base_layers     = 4;
    s.bands.push_back(band(filament(0, ColorRGB(1.f, 1.f, 1.f), 4.f), 4));
    s.bands.push_back(band(filament(1, ColorRGB(0.1f, 0.1f, 0.1f), 2.f), 4));

    s.plate.generated      = true;
    s.plate.width_mm       = plate_w;
    s.plate.height_mm      = plate_h;
    s.plate.base_mm        = 1.2f;
    s.plate.cell_mm        = 1.f;
    s.plate.frame.shape    = ChromaFrame::Shape::Rectangle;
    s.plate.frame.profile  = ChromaFrame::Profile::Bevel;
    s.plate.frame.width_mm = frame_mm;
    s.plate.frame.rise_mm  = 2.f;
    return s;
}

ChromaTileOptions on_bed(float w, float h)
{
    ChromaTileOptions o;
    o.bed_width_mm  = w;
    o.bed_height_mm = h;
    return o;
}

// A picture with a bit of structure in it, so a solve of it is not one flat colour.
std::vector<uint8_t> make_picture(int w, int h)
{
    std::vector<uint8_t> rgb(size_t(w) * size_t(h) * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i = (size_t(y) * size_t(w) + size_t(x)) * 3;
            rgb[i + 0]     = uint8_t((x * 255) / (w > 1 ? w - 1 : 1));
            rgb[i + 1]     = uint8_t((y * 255) / (h > 1 ? h - 1 : 1));
            rgb[i + 2]     = uint8_t(((x + y) * 255) / ((w + h) > 2 ? (w + h - 2) : 1));
        }
    return rgb;
}

// A solve of a known picture, built by hand rather than by chroma_solve(), so a test of the cut can
// say exactly which source pixel every tile pixel should have come from.
ChromaSolve make_solve(int w, int h)
{
    ChromaSolve s;
    s.width  = w;
    s.height = h;
    s.levels.resize(size_t(w) * size_t(h));
    s.grey.resize(size_t(w) * size_t(h));
    s.preview.resize(size_t(w) * size_t(h) * 3);
    s.delta.resize(size_t(w) * size_t(h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const size_t i  = size_t(y) * size_t(w) + size_t(x);
            s.levels[i]     = uint16_t(i);
            s.grey[i]       = uint8_t(i % 251);
            s.delta[i]      = uint8_t(i % 7);
            s.preview[i * 3 + 0] = uint8_t(x);
            s.preview[i * 3 + 1] = uint8_t(y);
            s.preview[i * 3 + 2] = uint8_t(i % 97);
        }
    return s;
}

} // namespace

TEST_CASE("A picture that fits the printable area is planned as a single tile", "[ChromaTiles]")
{
    // 80 x 40 mm of picture plus a 6 mm frame on all four sides is 92 x 52, which fits a 100 x 100 bed.
    const ChromaStack    stack = make_stack(80.f, 40.f, 6.f);
    const ChromaTilePlan plan  = chroma_tile_plan(stack, 40, 20, on_bed(100.f, 100.f));

    REQUIRE(plan.cols == 1);
    REQUIRE(plan.rows == 1);
    REQUIRE(plan.tiles.size() == 1);
    REQUIRE(plan.fits_bed);
    REQUIRE(plan.problems == 0);

    const ChromaTile &t = plan.tiles.front();
    REQUIRE(t.edges.all());
    REQUIRE(t.core.x0 == 0);
    REQUIRE(t.core.y0 == 0);
    REQUIRE(t.core.x1 == 40);
    REQUIRE(t.core.y1 == 20);
    REQUIRE_THAT(t.plate.width_mm, WithinAbs(80.f, 1e-3f));
    REQUIRE_THAT(t.plate.height_mm, WithinAbs(40.f, 1e-3f));
    // The whole picture in one piece is the plate it would have made untiled, frame and all.
    REQUIRE_THAT(t.outer_width_mm, WithinAbs(92.f, 1e-3f));
    REQUIRE_THAT(t.outer_height_mm, WithinAbs(52.f, 1e-3f));
    REQUIRE_THAT(plan.assembled_width_mm, WithinAbs(92.f, 1e-3f));
    REQUIRE_THAT(plan.assembled_height_mm, WithinAbs(52.f, 1e-3f));
}

TEST_CASE("A picture larger than the printable area is split until every tile fits", "[ChromaTiles]")
{
    // 240 x 240 mm of picture, a 6 mm frame, and a 100 x 100 bed. A tile on the outside carries the
    // frame on the side that faces out, so what has to fit is 240/n + 6.
    const ChromaStack    stack = make_stack(240.f, 240.f, 6.f);
    const ChromaTilePlan plan  = chroma_tile_plan(stack, 120, 120, on_bed(100.f, 100.f));

    REQUIRE(plan.cols == 3);
    REQUIRE(plan.rows == 3);
    REQUIRE(plan.fits_bed);
    REQUIRE_FALSE(chroma_tile_has(plan.problems, ChromaTileProblem::TileTooLarge));

    for (const ChromaTile &t : plan.tiles) {
        REQUIRE(t.fits);
        REQUIRE(t.outer_width_mm <= 100.f + 1e-3f);
        REQUIRE(t.outer_height_mm <= 100.f + 1e-3f);
    }
}

TEST_CASE("A forced tile count that does not fit is honoured and reported", "[ChromaTiles]")
{
    const ChromaStack stack   = make_stack(240.f, 240.f, 6.f);
    ChromaTileOptions options = on_bed(100.f, 100.f);
    options.cols              = 1;
    options.rows              = 1;

    const ChromaTilePlan plan = chroma_tile_plan(stack, 120, 120, options);

    REQUIRE(plan.cols == 1);
    REQUIRE(plan.rows == 1);
    REQUIRE_FALSE(plan.fits_bed);
    REQUIRE(chroma_tile_has(plan.problems, ChromaTileProblem::TileTooLarge));
}

TEST_CASE("The tiles cover the picture once, with no gap and no overlap", "[ChromaTiles]")
{
    // A picture whose width does not divide by the tile count, which is where a per-tile rounding
    // would show up as a column of pixels printed twice or not at all.
    const ChromaStack    stack = make_stack(240.f, 240.f, 6.f);
    const ChromaTilePlan plan  = chroma_tile_plan(stack, 121, 119, on_bed(100.f, 100.f));

    REQUIRE(plan.cols > 1);
    REQUIRE(plan.rows > 1);

    for (int r = 0; r < plan.rows; ++r) {
        const ChromaTile *first = chroma_tile_at(plan, 0, r);
        const ChromaTile *last  = chroma_tile_at(plan, plan.cols - 1, r);
        REQUIRE(first != nullptr);
        REQUIRE(last != nullptr);
        REQUIRE(first->core.x0 == 0);
        REQUIRE(last->core.x1 == plan.picture_width);
        for (int c = 1; c < plan.cols; ++c)
            REQUIRE(chroma_tile_at(plan, c - 1, r)->core.x1 == chroma_tile_at(plan, c, r)->core.x0);
    }
    for (int c = 0; c < plan.cols; ++c) {
        REQUIRE(chroma_tile_at(plan, c, 0)->core.y0 == 0);
        REQUIRE(chroma_tile_at(plan, c, plan.rows - 1)->core.y1 == plan.picture_height);
        for (int r = 1; r < plan.rows; ++r)
            REQUIRE(chroma_tile_at(plan, c, r - 1)->core.y1 == chroma_tile_at(plan, c, r)->core.y0);
    }

    int covered = 0;
    for (const ChromaTile &t : plan.tiles)
        covered += t.core.width() * t.core.height();
    REQUIRE(covered == plan.picture_width * plan.picture_height);
}

TEST_CASE("Only the sides on the outside of the assembled picture carry the frame", "[ChromaTiles]")
{
    const ChromaStack    stack = make_stack(240.f, 240.f, 6.f);
    const ChromaTilePlan plan  = chroma_tile_plan(stack, 120, 120, on_bed(100.f, 100.f));

    REQUIRE(plan.cols == 3);
    REQUIRE(plan.rows == 3);

    const ChromaTile *middle = chroma_tile_at(plan, 1, 1);
    REQUIRE(middle != nullptr);
    REQUIRE_FALSE(middle->edges.any());
    REQUIRE_THAT(middle->insets.left, WithinAbs(0.f, 1e-6f));
    REQUIRE_THAT(middle->insets.right, WithinAbs(0.f, 1e-6f));
    REQUIRE_THAT(middle->insets.top, WithinAbs(0.f, 1e-6f));
    REQUIRE_THAT(middle->insets.bottom, WithinAbs(0.f, 1e-6f));
    // An interior tile's plate has no frame at all, so a plate builder handed it cannot grow one.
    REQUIRE_THAT(middle->plate.frame.width_mm, WithinAbs(0.f, 1e-6f));
    REQUIRE_THAT(middle->outer_width_mm, WithinAbs(middle->plate.width_mm, 1e-3f));

    const ChromaTile *corner = chroma_tile_at(plan, 0, 0);
    REQUIRE(corner != nullptr);
    REQUIRE(corner->edges.left);
    REQUIRE(corner->edges.top);
    REQUIRE_FALSE(corner->edges.right);
    REQUIRE_FALSE(corner->edges.bottom);
    REQUIRE_THAT(corner->insets.left, WithinAbs(6.f, 1e-6f));
    REQUIRE_THAT(corner->insets.top, WithinAbs(6.f, 1e-6f));
    REQUIRE_THAT(corner->insets.right, WithinAbs(0.f, 1e-6f));
    REQUIRE_THAT(corner->insets.bottom, WithinAbs(0.f, 1e-6f));
    REQUIRE_THAT(corner->outer_width_mm, WithinAbs(corner->plate.width_mm + 6.f, 1e-3f));

    const ChromaTile *edge = chroma_tile_at(plan, 1, 0);
    REQUIRE(edge != nullptr);
    REQUIRE(edge->edges.top);
    REQUIRE_FALSE(edge->edges.left);
    REQUIRE_FALSE(edge->edges.right);
    REQUIRE_THAT(edge->insets.top, WithinAbs(6.f, 1e-6f));
    REQUIRE_THAT(edge->insets.left, WithinAbs(0.f, 1e-6f));
}

TEST_CASE("Spacing between the tiles counts toward the assembled size", "[ChromaTiles]")
{
    const ChromaStack stack   = make_stack(240.f, 240.f, 6.f);
    ChromaTileOptions options = on_bed(100.f, 100.f);
    options.spacing_mm        = 2.f;

    const ChromaTilePlan plan = chroma_tile_plan(stack, 120, 120, options);

    REQUIRE(plan.cols == 3);
    REQUIRE(plan.rows == 3);
    // 240 of picture, 6 of frame on each side, and a 2 mm gap at each of the two joins.
    REQUIRE_THAT(plan.assembled_width_mm, WithinAbs(240.f + 12.f + 4.f, 1e-3f));
    REQUIRE_THAT(plan.assembled_height_mm, WithinAbs(240.f + 12.f + 4.f, 1e-3f));

    // The gap moves the pieces apart; it does not stretch the picture, so no tile grew.
    const ChromaTilePlan tight = chroma_tile_plan(stack, 120, 120, on_bed(100.f, 100.f));
    for (size_t i = 0; i < plan.tiles.size(); ++i)
        REQUIRE_THAT(plan.tiles[i].plate.width_mm, WithinAbs(tight.tiles[i].plate.width_mm, 1e-4f));

    // And every gap to the left of a tile is in its offset.
    const ChromaTile *last = chroma_tile_at(plan, 2, 0);
    REQUIRE(last != nullptr);
    REQUIRE_THAT(last->offset_x_mm, WithinAbs(6.f + 160.f + 2.f * 2.f, 1e-3f));
}

TEST_CASE("Spacing does not change where a tile is cut out of the picture", "[ChromaTiles]")
{
    const ChromaStack stack   = make_stack(240.f, 240.f, 0.f);
    ChromaTileOptions spaced  = on_bed(100.f, 100.f);
    spaced.spacing_mm         = 5.f;

    const ChromaTilePlan a = chroma_tile_plan(stack, 120, 120, on_bed(100.f, 100.f));
    const ChromaTilePlan b = chroma_tile_plan(stack, 120, 120, spaced);

    REQUIRE(a.tiles.size() == b.tiles.size());
    for (size_t i = 0; i < a.tiles.size(); ++i)
        REQUIRE(a.tiles[i].core == b.tiles[i].core);
}

TEST_CASE("Every tile of a plan is derived from one stack and one layer assignment", "[ChromaTiles]")
{
    const ChromaStack    stack = make_stack(240.f, 240.f, 6.f);
    const ChromaTilePlan plan  = chroma_tile_plan(stack, 120, 120, on_bed(100.f, 100.f));

    REQUIRE(chroma_tile_seam_ok(plan));
    REQUIRE(chroma_tile_stack_matches(plan, stack));

    // The picture's surface sits at the same print Z on every tile, prints the same way up, and is
    // divided off the same grid. That is the geometric half of the seam.
    for (const ChromaTile &t : plan.tiles) {
        REQUIRE_THAT(t.plate.base_mm, WithinAbs(stack.plate.base_mm, 1e-6f));
        REQUIRE(t.plate.orientation == stack.plate.orientation);
        REQUIRE_THAT(t.plate.cell_mm, WithinAbs(stack.plate.cell_mm, 1e-6f));
    }

    // And the swaps are the stack's, derived once for the whole plan.
    const std::vector<ChromaRange> from_plan  = chroma_tile_ranges(plan);
    const std::vector<ChromaRange> from_stack = chroma_ranges(stack);
    REQUIRE(from_plan.size() == from_stack.size());
    for (size_t i = 0; i < from_plan.size(); ++i) {
        REQUIRE(from_plan[i].filament == from_stack[i].filament);
        REQUIRE_THAT(from_plan[i].z_min, WithinAbs(from_stack[i].z_min, 1e-9));
        REQUIRE_THAT(from_plan[i].z_max, WithinAbs(from_stack[i].z_max, 1e-9));
    }
}

TEST_CASE("A stack edited after the plan was made no longer matches it", "[ChromaTiles]")
{
    const ChromaStack    stack = make_stack(240.f, 240.f, 6.f);
    const ChromaTilePlan plan  = chroma_tile_plan(stack, 120, 120, on_bed(100.f, 100.f));

    SECTION("a band one layer longer")
    {
        ChromaStack other = stack;
        other.bands[0].layers += 1;
        REQUIRE_FALSE(chroma_tile_stack_matches(plan, other));
    }
    SECTION("a different layer height")
    {
        ChromaStack other   = stack;
        other.layer_height_mm = 0.2f;
        REQUIRE_FALSE(chroma_tile_stack_matches(plan, other));
    }
    SECTION("a different base")
    {
        ChromaStack other = stack;
        other.base_layers += 1;
        REQUIRE_FALSE(chroma_tile_stack_matches(plan, other));
    }
    SECTION("a different filament colour")
    {
        ChromaStack other             = stack;
        other.bands[1].filament.color = ColorRGB(0.9f, 0.2f, 0.2f);
        REQUIRE_FALSE(chroma_tile_stack_matches(plan, other));
    }
    SECTION("a different lighting mode")
    {
        ChromaStack other = stack;
        other.lighting    = ChromaLighting::BackLit;
        REQUIRE_FALSE(chroma_tile_stack_matches(plan, other));
    }
    SECTION("a backing of a different thickness")
    {
        ChromaStack other    = stack;
        other.plate.base_mm += 0.4f;
        REQUIRE_FALSE(chroma_tile_stack_matches(plan, other));
    }
    SECTION("a filament preset that was only renamed")
    {
        // The name is a label, not a print. A plan that had to be rebuilt over a rename would be
        // rebuilt constantly.
        ChromaStack other            = stack;
        other.bands[0].filament.name = "Some Other Spool";
        REQUIRE(chroma_tile_stack_matches(plan, other));
    }
    SECTION("an importance region, which weights the error and moves no pixel")
    {
        ChromaStack           other = stack;
        std::vector<uint8_t>  mask(16 * 16, 0);
        mask[5 * 16 + 5]            = 1;
        other.importance.region     = chroma_mask_encode(mask.data(), 16, 16);
        other.importance.weight     = 4.f;
        REQUIRE(chroma_tile_stack_matches(plan, other));
    }
}

TEST_CASE("A tile carries a halo of the neighbouring picture beyond its own edge", "[ChromaTiles]")
{
    const ChromaStack stack   = make_stack(240.f, 240.f, 0.f);
    ChromaTileOptions options = on_bed(100.f, 100.f);
    options.halo_px           = 4;

    const ChromaTilePlan plan = chroma_tile_plan(stack, 120, 120, options);
    REQUIRE(plan.cols == 3);
    REQUIRE(plan.rows == 3);

    const ChromaTile *middle = chroma_tile_at(plan, 1, 1);
    REQUIRE(middle != nullptr);
    // An interior tile has real neighbours on all four sides, so it gets the full halo on all four.
    REQUIRE(middle->halo.x0 == middle->core.x0 - 4);
    REQUIRE(middle->halo.y0 == middle->core.y0 - 4);
    REQUIRE(middle->halo.x1 == middle->core.x1 + 4);
    REQUIRE(middle->halo.y1 == middle->core.y1 + 4);

    const ChromaTile *corner = chroma_tile_at(plan, 0, 0);
    REQUIRE(corner != nullptr);
    // There is no picture outside the picture, so the halo is clipped there and nowhere else.
    REQUIRE(corner->halo.x0 == 0);
    REQUIRE(corner->halo.y0 == 0);
    REQUIRE(corner->halo.x1 == corner->core.x1 + 4);
    REQUIRE(corner->halo.y1 == corner->core.y1 + 4);

    // The halo is what a filter or a solve is handed, and it is bigger than what prints.
    const std::vector<uint8_t> picture = make_picture(120, 120);
    const std::vector<uint8_t> crop    = chroma_tile_crop_rgb(picture.data(), 120, 120, plan, 4);
    REQUIRE(crop.size() == size_t(middle->halo.width()) * size_t(middle->halo.height()) * 3);
    // And the first pixel of the crop is the picture's, four rows and four columns before the tile.
    const size_t first = (size_t(middle->halo.y0) * 120 + size_t(middle->halo.x0)) * 3;
    REQUIRE(crop[0] == picture[first]);
    REQUIRE(crop[1] == picture[first + 1]);
    REQUIRE(crop[2] == picture[first + 2]);
}

TEST_CASE("Cutting a solved picture into tiles copies it pixel for pixel", "[ChromaTiles]")
{
    const ChromaStack    stack = make_stack(240.f, 240.f, 0.f);
    const ChromaTilePlan plan  = chroma_tile_plan(stack, 120, 120, on_bed(100.f, 100.f));
    const ChromaSolve    full  = make_solve(120, 120);

    const ChromaTile *left  = chroma_tile_at(plan, 0, 1);
    const ChromaTile *right = chroma_tile_at(plan, 1, 1);
    REQUIRE(left != nullptr);
    REQUIRE(right != nullptr);

    const ChromaSolve a = chroma_tile_cut(full, plan, size_t(plan.cols) + 0);
    const ChromaSolve b = chroma_tile_cut(full, plan, size_t(plan.cols) + 1);
    REQUIRE(a.width == left->core.width());
    REQUIRE(a.height == left->core.height());
    REQUIRE(b.width == right->core.width());

    // Every pixel of the cut is the pixel the whole-picture solve put there, grey level included. The
    // grey map's encoding is the stack's - 0 at base_layers, 255 at the top - so copying it rather
    // than rescaling it is what makes one grey mean one print height on every tile.
    for (int y = 0; y < a.height; ++y)
        for (int x = 0; x < a.width; ++x) {
            const size_t from = size_t(left->core.y0 + y) * 120 + size_t(left->core.x0 + x);
            const size_t to   = size_t(y) * size_t(a.width) + size_t(x);
            REQUIRE(a.grey[to] == full.grey[from]);
            REQUIRE(a.levels[to] == full.levels[from]);
        }

    // The seam itself: the last column of the left tile and the first column of the right tile are
    // neighbouring columns of the one solve, so the picture does not step across the join.
    for (int y = 0; y < a.height; ++y) {
        const size_t left_last  = size_t(y) * size_t(a.width) + size_t(a.width - 1);
        const size_t right_first = size_t(y) * size_t(b.width);
        const size_t src_left   = size_t(left->core.y0 + y) * 120 + size_t(left->core.x1 - 1);
        const size_t src_right  = size_t(right->core.y0 + y) * 120 + size_t(right->core.x0);
        REQUIRE(a.grey[left_last] == full.grey[src_left]);
        REQUIRE(b.grey[right_first] == full.grey[src_right]);
        REQUIRE(src_right == src_left + 1);
    }
}

TEST_CASE("Cropping a tile drops its halo and keeps what prints", "[ChromaTiles]")
{
    const ChromaStack stack   = make_stack(240.f, 240.f, 0.f);
    ChromaTileOptions options = on_bed(100.f, 100.f);
    options.halo_px           = 4;

    const ChromaTilePlan plan = chroma_tile_plan(stack, 120, 120, options);
    const ChromaTile    *mid  = chroma_tile_at(plan, 1, 1);
    REQUIRE(mid != nullptr);

    // A solve of the tile's halo rectangle, lifted out of a solve of the whole picture so that every
    // pixel's expected value is known.
    const ChromaSolve full = make_solve(120, 120);
    ChromaSolve       over;
    over.width  = mid->halo.width();
    over.height = mid->halo.height();
    over.levels.resize(size_t(over.width) * size_t(over.height));
    over.grey.resize(over.levels.size());
    over.preview.resize(over.levels.size() * 3);
    over.delta.resize(over.levels.size());
    for (int y = 0; y < over.height; ++y)
        for (int x = 0; x < over.width; ++x) {
            const size_t from = size_t(mid->halo.y0 + y) * 120 + size_t(mid->halo.x0 + x);
            const size_t to   = size_t(y) * size_t(over.width) + size_t(x);
            over.levels[to]   = full.levels[from];
            over.grey[to]     = full.grey[from];
            over.delta[to]    = full.delta[from];
            over.preview[to * 3 + 0] = full.preview[from * 3 + 0];
            over.preview[to * 3 + 1] = full.preview[from * 3 + 1];
            over.preview[to * 3 + 2] = full.preview[from * 3 + 2];
        }

    const ChromaSolve cropped = chroma_tile_crop(over, plan, 4);
    REQUIRE(cropped.width == mid->core.width());
    REQUIRE(cropped.height == mid->core.height());
    for (int y = 0; y < cropped.height; ++y)
        for (int x = 0; x < cropped.width; ++x) {
            const size_t from = size_t(mid->core.y0 + y) * 120 + size_t(mid->core.x0 + x);
            const size_t to   = size_t(y) * size_t(cropped.width) + size_t(x);
            REQUIRE(cropped.grey[to] == full.grey[from]);
            REQUIRE(cropped.levels[to] == full.levels[from]);
        }
}

TEST_CASE("A tile solved on its own agrees with the same tile cut out of the whole picture",
          "[ChromaTiles]")
{
    // Without error diffusion every pixel is matched on its own, so the two paths have to produce the
    // same height map. If they ever stop doing so, a tile has picked up a dependence on where it was
    // cut, which is the seam failing.
    const ChromaStack    stack   = make_stack(80.f, 40.f, 0.f);
    ChromaTileOptions    options = on_bed(45.f, 45.f);
    options.halo_px              = 3;
    const ChromaTilePlan plan    = chroma_tile_plan(stack, 40, 20, options);

    REQUIRE(plan.cols > 1);
    REQUIRE_FALSE(plan.cut_required);

    const std::vector<uint8_t> picture = make_picture(40, 20);
    const ChromaSolve          full    = chroma_solve(picture.data(), 40, 20, stack);
    REQUIRE_FALSE(full.empty());

    for (size_t i = 0; i < plan.tiles.size(); ++i) {
        const ChromaSolve alone = chroma_tile_solve(picture.data(), 40, 20, plan, i);
        const ChromaSolve cut   = chroma_tile_cut(full, plan, i);
        REQUIRE(alone.width == cut.width);
        REQUIRE(alone.height == cut.height);
        REQUIRE(alone.grey.size() == cut.grey.size());
        REQUIRE(alone.levels.size() == cut.levels.size());
        for (size_t p = 0; p < cut.grey.size(); ++p) {
            REQUIRE(alone.grey[p] == cut.grey[p]);
            REQUIRE(alone.levels[p] == cut.levels[p]);
        }
    }
}

TEST_CASE("A dithered stack refuses to be solved one tile at a time", "[ChromaTiles]")
{
    ChromaStack stack = make_stack(240.f, 240.f, 0.f);
    stack.dither      = true;

    const ChromaTilePlan plan = chroma_tile_plan(stack, 120, 120, on_bed(100.f, 100.f));
    REQUIRE(plan.cut_required);

    const std::vector<uint8_t> picture = make_picture(120, 120);
    // Error diffusion passes its residual along the whole row, so a tile solved on its own disagrees
    // with its neighbour however wide the halo is. Refusing is the point.
    REQUIRE(chroma_tile_solve(picture.data(), 120, 120, plan, 0).empty());
}

TEST_CASE("A retouched stack also refuses to be solved one tile at a time", "[ChromaTiles]")
{
    ChromaStack          stack = make_stack(240.f, 240.f, 0.f);
    std::vector<uint8_t> mask(120 * 120, 0);
    for (int i = 0; i < 120; ++i)
        mask[size_t(i) * 120 + size_t(i)] = 1;

    ChromaRetouchEdit edit;
    edit.region = chroma_mask_encode(mask.data(), 120, 120);
    edit.target = ChromaRetouchEdit::Target::Delta;
    edit.value  = 2;
    stack.retouch.edits.push_back(edit);

    const ChromaTilePlan plan = chroma_tile_plan(stack, 120, 120, on_bed(100.f, 100.f));
    REQUIRE(plan.cut_required);
}

TEST_CASE("The same tiles come out of a grid of a different size", "[ChromaTiles]")
{
    // The GUI solves a coarse preview on every keystroke and the picture itself only once, so the two
    // have to be cut the same way or the preview shows seams the print will not have.
    const ChromaStack    stack = make_stack(240.f, 240.f, 0.f);
    const ChromaTilePlan plan  = chroma_tile_plan(stack, 1201, 1199, on_bed(100.f, 100.f));

    REQUIRE(plan.cols > 1);
    REQUIRE(plan.rows > 1);

    const int coarse_w = 257, coarse_h = 193;
    for (int r = 0; r < plan.rows; ++r)
        for (int c = 0; c < plan.cols; ++c) {
            const size_t         i    = size_t(r) * size_t(plan.cols) + size_t(c);
            const ChromaTileRect rect = chroma_tile_rect_on(plan, i, coarse_w, coarse_h);
            if (c == 0)
                REQUIRE(rect.x0 == 0);
            if (c == plan.cols - 1)
                REQUIRE(rect.x1 == coarse_w);
            if (r == 0)
                REQUIRE(rect.y0 == 0);
            if (r == plan.rows - 1)
                REQUIRE(rect.y1 == coarse_h);
            if (c > 0) {
                const ChromaTileRect before = chroma_tile_rect_on(plan, i - 1, coarse_w, coarse_h);
                REQUIRE(before.x1 == rect.x0);
            }
            if (r > 0) {
                const ChromaTileRect above =
                    chroma_tile_rect_on(plan, i - size_t(plan.cols), coarse_w, coarse_h);
                REQUIRE(above.y1 == rect.y0);
            }
        }
}

TEST_CASE("A picture painted onto a model is not tiled at all", "[ChromaTiles]")
{
    ChromaStack stack      = make_stack(240.f, 240.f, 6.f);
    stack.plate.generated  = false;

    const ChromaTilePlan plan = chroma_tile_plan(stack, 120, 120, on_bed(100.f, 100.f));
    REQUIRE(chroma_tile_has(plan.problems, ChromaTileProblem::NoPlate));
    REQUIRE(plan.tiles.empty());
}

TEST_CASE("A frame as wide as the printable area cannot be tiled out of", "[ChromaTiles]")
{
    // Every tile on the outside of the picture carries the moulding, so no count of tiles helps.
    const ChromaStack    stack = make_stack(240.f, 240.f, 40.f);
    const ChromaTilePlan plan  = chroma_tile_plan(stack, 120, 120, on_bed(40.f, 40.f));

    REQUIRE(chroma_tile_has(plan.problems, ChromaTileProblem::FrameTooWide));
    REQUIRE_FALSE(plan.fits_bed);
}

TEST_CASE("A shaped plate outline is dropped when the plate is cut into tiles", "[ChromaTiles]")
{
    ChromaStack stack         = make_stack(240.f, 240.f, 6.f);
    stack.plate.frame.shape   = ChromaFrame::Shape::Ellipse;

    const ChromaTilePlan plan = chroma_tile_plan(stack, 120, 120, on_bed(100.f, 100.f));
    REQUIRE(plan.cols > 1);
    REQUIRE(chroma_tile_has(plan.problems, ChromaTileProblem::OutlineDropped));
    for (const ChromaTile &t : plan.tiles)
        REQUIRE(t.plate.frame.shape == ChromaFrame::Shape::Rectangle);

    // One tile is the whole picture, so its shape survives.
    const ChromaTilePlan single = chroma_tile_plan(stack, 120, 120, on_bed(400.f, 400.f));
    REQUIRE(single.cols == 1);
    REQUIRE(single.rows == 1);
    REQUIRE_FALSE(chroma_tile_has(single.problems, ChromaTileProblem::OutlineDropped));
    REQUIRE(single.tiles.front().plate.frame.shape == ChromaFrame::Shape::Ellipse);
}

TEST_CASE("A bed margin is kept clear on every side", "[ChromaTiles]")
{
    const ChromaStack stack   = make_stack(240.f, 240.f, 0.f);
    ChromaTileOptions options = on_bed(100.f, 100.f);
    options.bed_margin_mm     = 10.f;

    const ChromaTilePlan plan = chroma_tile_plan(stack, 120, 120, options);
    for (const ChromaTile &t : plan.tiles) {
        REQUIRE(t.outer_width_mm <= 80.f + 1e-3f);
        REQUIRE(t.outer_height_mm <= 80.f + 1e-3f);
    }
}

TEST_CASE("Square tiles come out square", "[ChromaTiles]")
{
    const ChromaStack stack   = make_stack(240.f, 120.f, 0.f);
    ChromaTileOptions options = on_bed(100.f, 100.f);
    options.square_tiles      = true;

    const ChromaTilePlan plan = chroma_tile_plan(stack, 120, 60, options);

    REQUIRE(plan.cols > 1);
    REQUIRE(plan.tile_px_x > 0);
    REQUIRE(plan.tile_px_y > 0);

    // Every tile but the trailing strip is the locked side, which is the same number of millimetres
    // on both axes.
    const ChromaTile *first = chroma_tile_at(plan, 0, 0);
    REQUIRE(first != nullptr);
    REQUIRE_THAT(first->plate.width_mm, WithinAbs(first->plate.height_mm, 1e-3f));
    for (int r = 0; r + 1 < plan.rows; ++r)
        for (int c = 0; c + 1 < plan.cols; ++c) {
            const ChromaTile *t = chroma_tile_at(plan, c, r);
            REQUIRE(t->core.width() == plan.tile_px_x);
            REQUIRE(t->core.height() == plan.tile_px_y);
        }
    // And the picture is still covered exactly once.
    REQUIRE(chroma_tile_seam_ok(plan));
}

TEST_CASE("A checkerboard prints every other tile and the parity toggle prints the rest",
          "[ChromaTiles]")
{
    const ChromaStack stack   = make_stack(240.f, 240.f, 0.f);
    ChromaTileOptions options = on_bed(100.f, 100.f);
    options.layout            = ChromaTileLayout::Checkerboard;

    const ChromaTilePlan first = chroma_tile_plan(stack, 120, 120, options);
    options.skip_first         = true;
    const ChromaTilePlan second = chroma_tile_plan(stack, 120, 120, options);

    REQUIRE(first.cols == 3);
    REQUIRE(first.rows == 3);
    REQUIRE(first.tiles.size() == second.tiles.size());
    REQUIRE(first.included_count() == 5);
    REQUIRE(second.included_count() == 4);

    for (size_t i = 0; i < first.tiles.size(); ++i) {
        // Exactly one of the two plans prints each tile, so the two together are the picture.
        REQUIRE(first.tiles[i].included != second.tiles[i].included);
        // And they agree about where every seam falls, which is what lets the halves be interleaved.
        REQUIRE(first.tiles[i].core == second.tiles[i].core);
    }
    // Both halves are planned against the same stack, so the two colour sets still meet at the joins.
    REQUIRE(first.stack_key == second.stack_key);
}

TEST_CASE("A tile budget leaves the surplus planned and unprinted", "[ChromaTiles]")
{
    const ChromaStack stack   = make_stack(240.f, 240.f, 0.f);
    ChromaTileOptions options = on_bed(100.f, 100.f);
    options.max_tiles         = 4;

    const ChromaTilePlan plan = chroma_tile_plan(stack, 120, 120, options);
    REQUIRE(plan.tiles.size() == 9);
    REQUIRE(plan.included_count() == 4);
    REQUIRE(chroma_tile_has(plan.problems, ChromaTileProblem::TooManyTiles));
    // The geometry is complete, so the tiles left out line up with the ones a later plan prints.
    REQUIRE(chroma_tile_seam_ok(plan));
}

TEST_CASE("A cap on the tiles per row is honoured and reported", "[ChromaTiles]")
{
    const ChromaStack stack   = make_stack(240.f, 240.f, 0.f);
    ChromaTileOptions options = on_bed(100.f, 100.f);
    options.max_per_row       = 2;

    const ChromaTilePlan plan = chroma_tile_plan(stack, 120, 120, options);
    REQUIRE(plan.cols == 2);
    REQUIRE(chroma_tile_has(plan.problems, ChromaTileProblem::RowCapBinds));
    // Two columns of a 240 mm picture is 120 mm a tile, which does not fit a 100 mm bed, and the plan
    // says so rather than quietly overriding the cap.
    REQUIRE_FALSE(plan.fits_bed);
    REQUIRE(chroma_tile_has(plan.problems, ChromaTileProblem::TileTooLarge));
}

TEST_CASE("A plan survives a round trip through the project file", "[ChromaTiles]")
{
    const ChromaStack stack   = make_stack(240.f, 240.f, 6.f);
    ChromaTileOptions options = on_bed(100.f, 100.f);
    options.spacing_mm        = 1.5f;
    options.halo_px           = 6;
    options.layout            = ChromaTileLayout::Checkerboard;
    options.skip_first        = true;
    options.max_per_row       = 0;

    const ChromaTilePlan plan = chroma_tile_plan(stack, 120, 120, options);

    nlohmann::json j = nlohmann::json::object();
    chroma_tiles_to_json(j, plan);

    ChromaTilePlan back;
    chroma_tiles_from_json(j, back);

    REQUIRE(back.cols == plan.cols);
    REQUIRE(back.rows == plan.rows);
    REQUIRE(back.picture_width == plan.picture_width);
    REQUIRE(back.picture_height == plan.picture_height);
    REQUIRE(back.options == plan.options);
    REQUIRE(back.tiles.size() == plan.tiles.size());
    REQUIRE_THAT(back.assembled_width_mm, WithinAbs(plan.assembled_width_mm, 1e-4f));
    for (size_t i = 0; i < plan.tiles.size(); ++i) {
        REQUIRE(back.tiles[i].core == plan.tiles[i].core);
        REQUIRE(back.tiles[i].included == plan.tiles[i].included);
    }
    // The stack came back with it, so the reopened tiling still knows what it was solved against.
    REQUIRE(back.stack_key == plan.stack_key);
    REQUIRE(chroma_tile_stack_matches(back, stack));
    REQUIRE(chroma_tile_seam_ok(back));
}

TEST_CASE("An empty project object leaves a plan at what it was handed", "[ChromaTiles]")
{
    // Every key is optional on the way back in, which is what makes a project written before a field
    // existed read back as the tiling it was.
    const ChromaStack stack   = make_stack(240.f, 240.f, 6.f);
    ChromaTilePlan    seeded  = chroma_tile_plan(stack, 120, 120, on_bed(100.f, 100.f));
    const int         cols    = seeded.cols;

    const nlohmann::json empty = nlohmann::json::object();
    chroma_tiles_from_json(empty, seeded);

    REQUIRE(seeded.cols == cols);
    REQUIRE(seeded.picture_width == 120);
    REQUIRE(chroma_tile_stack_matches(seeded, stack));
}

TEST_CASE("A plan survives a round trip through a binary archive", "[ChromaTiles]")
{
    const ChromaStack    stack = make_stack(240.f, 240.f, 6.f);
    const ChromaTilePlan plan  = chroma_tile_plan(stack, 120, 120, on_bed(100.f, 100.f));

    std::stringstream buffer;
    {
        cereal::BinaryOutputArchive out(buffer);
        ChromaTilePlan              copy = plan;
        out(copy);
    }
    ChromaTilePlan back;
    {
        cereal::BinaryInputArchive in(buffer);
        in(back);
    }

    REQUIRE(back.cols == plan.cols);
    REQUIRE(back.rows == plan.rows);
    REQUIRE(back.stack_key == plan.stack_key);
    REQUIRE(back.tiles.size() == plan.tiles.size());
    for (size_t i = 0; i < plan.tiles.size(); ++i) {
        REQUIRE(back.tiles[i].core == plan.tiles[i].core);
        REQUIRE_THAT(back.tiles[i].outer_width_mm, WithinAbs(plan.tiles[i].outer_width_mm, 1e-4f));
    }
    REQUIRE(chroma_tile_seam_ok(back));
}

TEST_CASE("A plan whose tiles were tampered with is not seam safe", "[ChromaTiles]")
{
    const ChromaStack stack = make_stack(240.f, 240.f, 6.f);
    ChromaTilePlan    plan  = chroma_tile_plan(stack, 120, 120, on_bed(100.f, 100.f));
    REQUIRE(chroma_tile_seam_ok(plan));

    SECTION("a gap opened between two tiles")
    {
        plan.tiles[1].core.x0 += 1;
        REQUIRE_FALSE(chroma_tile_seam_ok(plan));
    }
    SECTION("one tile given a backing of its own")
    {
        plan.tiles[4].plate.base_mm += 0.5f;
        REQUIRE_FALSE(chroma_tile_seam_ok(plan));
    }
    SECTION("one tile turned over")
    {
        plan.tiles[4].plate.orientation = ChromaOrientation::FaceDown;
        REQUIRE_FALSE(chroma_tile_seam_ok(plan));
    }
    SECTION("a moulding put on an interior edge")
    {
        plan.tiles[4].insets.left = 6.f;
        REQUIRE_FALSE(chroma_tile_seam_ok(plan));
    }
    SECTION("the stack swapped under the plan")
    {
        plan.stack.bands[0].layers += 1;
        REQUIRE_FALSE(chroma_tile_seam_ok(plan));
    }
}
