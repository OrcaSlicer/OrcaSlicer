#include <catch2/catch_all.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include "libslic3r/Fill/FillSvg.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

static double total_length_mm(const Polylines &pls)
{
    double len = 0.;
    for (const Polyline &pl : pls)
        len += unscaled<double>(pl.length());
    return len;
}

// A 10 x 8 mm document with a filled 4 x 4 square and a stroked diagonal.
static const char *kSquareAndLineSvg = R"svg(<?xml version="1.0"?>
<svg xmlns="http://www.w3.org/2000/svg" width="10mm" height="8mm" viewBox="0 0 10 8">
  <rect x="1" y="1" width="4" height="4" fill="#000" stroke="none"/>
  <path d="M 6 1 L 9 7" fill="none" stroke="#000" stroke-width="0.3"/>
</svg>)svg";

TEST_CASE("SVG infill tile from text", "[FillSvg]") {
    const SvgInfillTile tile = FillSvg::tile_from_svg_text(kSquareAndLineSvg);
    REQUIRE_FALSE(tile.empty());
    // The document box is the tile pitch.
    REQUIRE_THAT(tile.size.x(), WithinAbs(10., 1e-6));
    REQUIRE_THAT(tile.size.y(), WithinAbs(8., 1e-6));
    // Square outline (16 mm) + diagonal (sqrt(9 + 36) mm).
    REQUIRE_THAT(tile.length, WithinAbs(16. + std::sqrt(45.), 0.05));
    REQUIRE(tile.fills.size() == 1);
    REQUIRE(tile.loops.empty());
    REQUIRE(tile.lines.size() == 1);

    SECTION("lines lie inside the document box with y up") {
        BoundingBox bb;
        for (const ExPolygon &ep : tile.fills)
            bb.merge(get_extents(ep));
        for (const Polyline &pl : tile.lines)
            bb.merge(pl.bounding_box());
        REQUIRE(bb.min.x() >= scaled<coord_t>(1.) - 10);
        REQUIRE(bb.max.x() <= scaled<coord_t>(9.) + 10);
        REQUIRE(bb.min.y() >= scaled<coord_t>(1.) - 10);
        REQUIRE(bb.max.y() <= scaled<coord_t>(7.) + 10);
        // The square (SVG y = 1..5 from the top) is the UPPER part of the tile once y points up.
        BoundingBox sq = get_extents(tile.fills.front());
        REQUIRE_THAT(unscaled<double>(sq.min.y()), WithinAbs(3., 0.01));
        REQUIRE_THAT(unscaled<double>(sq.max.y()), WithinAbs(7., 0.01));
    }

    SECTION("empty and invalid input") {
        REQUIRE(FillSvg::tile_from_svg_text("").empty());
        REQUIRE(FillSvg::tile_from_svg_text("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10mm\" height=\"10mm\"/>").empty());
        REQUIRE(FillSvg::tile_from_svg_text("not an svg").empty());
    }
}

TEST_CASE("SVG infill tile scale", "[FillSvg]") {
    const SvgInfillTile tile = FillSvg::tile_from_svg_text(kSquareAndLineSvg);
    SECTION("explicit scale wins") {
        REQUIRE_THAT(FillSvg::tile_scale(tile, 0.2, 0.45, 250.), WithinAbs(2.5, 1e-9));
    }
    SECTION("automatic scale realizes the requested density") {
        const double density = 0.15, spacing = 0.45;
        const double s = FillSvg::tile_scale(tile, density, spacing, 0.);
        const double line_area = tile.length * s * spacing;
        const double tile_area = tile.size.x() * tile.size.y() * s * s;
        REQUIRE_THAT(line_area / tile_area, WithinRel(density, 1e-6));
    }
}

TEST_CASE("SVG fragments meeting at their ends are joined", "[FillSvg]") {
    Polylines pls;
    pls.emplace_back(Points{ Point(0, 0), Point(100, 0) });
    pls.emplace_back(Points{ Point(200, 0), Point(101, 1) });   // reversed, one unit off
    pls.emplace_back(Points{ Point(200, 1), Point(300, 0) });
    pls.emplace_back(Points{ Point(1000, 1000), Point(1100, 1000) });
    FillSvg::join_at_endpoints(pls, 2);
    REQUIRE(pls.size() == 2);
    const Polyline &joined = pls.front().points.size() > 2 ? pls.front() : pls.back();
    // Coincident joints are not duplicated: (0,0) (100,0) (200,0) (300,0).
    REQUIRE(joined.points.size() == 4);
    REQUIRE(joined.points.front() == Point(0, 0));
    REQUIRE(joined.points.back() == Point(300, 0));
}

TEST_CASE("SVG infill tiling covers the region on a fixed grid", "[FillSvg]") {
    const SvgInfillTile tile = FillSvg::tile_from_svg_text(kSquareAndLineSvg);
    const BoundingBox region(Point::new_scale(-15., -12.), Point::new_scale(15., 12.));
    const Polylines lines = FillSvg::tile_lines(tile, 1., region);
    // 30 mm / 10 mm pitch -> tiles at x = -20, -10, 0, 10 (4); 24 mm / 8 mm -> y = -16, -8, 0, 8 (4).
    REQUIRE(lines.size() == 2 * 4 * 4);
    // Absolute grid: the same box shifted by exactly one pitch yields the same lines translated.
    const BoundingBox shifted(region.min + Point::new_scale(10., 8.), region.max + Point::new_scale(10., 8.));
    const Polylines lines2 = FillSvg::tile_lines(tile, 1., shifted);
    REQUIRE(lines2.size() == lines.size());
    const Point delta = lines2.front().points.front() - lines.front().points.front();
    REQUIRE(delta.x() == scaled<coord_t>(10.));
    REQUIRE(delta.y() == scaled<coord_t>(8.));
}

TEST_CASE("Catfill resource and a stroke-only SVG file produce infill", "[FillSvg]") {
    // resources/ relative to tests/data.
    const boost::filesystem::path repo_resources = boost::filesystem::path(TEST_DATA_DIR) / ".." / ".." / "resources";
    const std::string previous = resources_dir();
    set_resources_dir(repo_resources.string());
    struct Restore { const std::string &p; ~Restore() { set_resources_dir(p); } } restore{previous};

    double region_size = 60.;
    FillParams params;
    params.density = 0.15f;
    PrintRegionConfig region_config;

    auto fill_with = [&](InfillPattern pattern) {
        const double s = region_size;
        Surface surface(stInternal, ExPolygon(Polygon(Points{ Point::new_scale(0., 0.), Point::new_scale(s, 0.),
                                                              Point::new_scale(s, s), Point::new_scale(0., s) })));
        params.pattern = pattern;
        params.config  = &region_config;
        FillSvg fill;
        fill.z = 1.0; fill.spacing = 0.45; fill.angle = 0.f; fill.layer_id = 0;
        fill.set_bounding_box(surface.expolygon.contour.bounding_box());
        return fill.fill_surface(&surface, params);
    };

    SECTION("bundled catfill and sharkfill") {
        std::shared_ptr<const SvgInfillTile> tile = FillSvg::tile_for(ipCatfill, "");
        REQUIRE(tile != nullptr);
        REQUIRE_FALSE(tile->empty());
        // A 729 x 500 px pattern cell of stroked paths.
        REQUIRE_THAT(tile->size.x(), WithinRel(729. * 25.4 / 96., 1e-3));
        REQUIRE(tile->fills.empty());
        REQUIRE(tile->lines.size() >= 5);
        std::shared_ptr<const SvgInfillTile> shark = FillSvg::tile_for(ipSharkfill, "");
        REQUIRE(shark != nullptr);
        REQUIRE_THAT(shark->size.x(), WithinRel(500. * 25.4 / 96., 1e-3));
        REQUIRE_FALSE(fill_with(ipSharkfill).empty());
        // Puppies: a 240 x 432 px document (viewBox 60 x 108) of <use> clones of a <g> with
        // paths, circles and ellipses; the background rect is skipped.
        std::shared_ptr<const SvgInfillTile> puppy = FillSvg::tile_for(ipPuppyfill, "");
        REQUIRE(puppy != nullptr);
        REQUIRE_THAT(puppy->size.x(), WithinRel(240. * 25.4 / 96., 1e-3));
        REQUIRE_THAT(puppy->size.y(), WithinRel(432. * 25.4 / 96., 1e-3));
        REQUIRE(puppy->fills.empty());
        REQUIRE(puppy->loops.size() + puppy->lines.size() >= 12);
        REQUIRE_FALSE(fill_with(ipPuppyfill).empty());

        const Polylines out = fill_with(ipCatfill);
        REQUIRE_FALSE(out.empty());
        // Roughly the requested density over the 60 x 60 mm square.
        const double fraction = total_length_mm(out) * 0.45 / (60. * 60.);
        REQUIRE(fraction > 0.10);
        REQUIRE(fraction < 0.22);
        for (const Polyline &pl : out)
            for (const Point &p : pl.points) {
                REQUIRE(p.x() >= -10); REQUIRE(p.x() <= scaled<coord_t>(60.) + 10);
                REQUIRE(p.y() >= -10); REQUIRE(p.y() <= scaled<coord_t>(60.) + 10);
            }
    }

    SECTION("custom SVG file, stroke-only line art in pixel units") {
        region_config.sparse_infill_svg_path.value = (boost::filesystem::path(TEST_DATA_DIR) / "catsfill2.svg").string();
        std::shared_ptr<const SvgInfillTile> tile = FillSvg::tile_for(ipSvg, region_config.sparse_infill_svg_path.value);
        REQUIRE(tile != nullptr);
        REQUIRE_FALSE(tile->empty());
        // The art is a <pattern> cell of 729 x 500 px at 96 dpi; the background rects are skipped.
        REQUIRE_THAT(tile->size.x(), WithinRel(729. * 25.4 / 96., 1e-3));
        REQUIRE_THAT(tile->size.y(), WithinRel(500. * 25.4 / 96., 1e-3));
        // Five stroked paths; the one crossing the cell edge is clipped into two fragments.
        REQUIRE(tile->fills.empty());
        REQUIRE(tile->lines.size() >= 5);
        // Print at 20% (39 mm tiles) and check the density against that explicit scale.
        region_config.sparse_infill_svg_scale.value = 20.;
        const Polylines out = fill_with(ipSvg);
        REQUIRE_FALSE(out.empty());
        const double expected = tile->length * 0.2 * 0.45 / (tile->size.x() * tile->size.y() * 0.2 * 0.2);
        const double fraction = total_length_mm(out) * 0.45 / (60. * 60.);
        REQUIRE(fraction > 0.5 * expected);
        REQUIRE(fraction < 1.5 * expected);
    }

    SECTION("explicit scale prints the drawing at that size") {
        region_size = 210.; // enough tiles for the length ratio below to be meaningful
        region_config.sparse_infill_svg_path.value = (boost::filesystem::path(TEST_DATA_DIR) / "catfill-band.svg").string();
        region_config.sparse_infill_svg_scale.value = 100.; // native 21 x 15 mm cats
        const Polylines out = fill_with(ipSvg);
        REQUIRE_FALSE(out.empty());
        region_config.sparse_infill_svg_scale.value = 200.;
        const Polylines out2 = fill_with(ipSvg);
        // Twice the tile size -> a quarter of the tiles, each twice as long: about half the length.
        REQUIRE_THAT(total_length_mm(out2), WithinRel(0.5 * total_length_mm(out), 0.25));
    }

    SECTION("shark tessellation: <use> clones, cell clipping and continuous lines") {
        region_config.sparse_infill_svg_path.value = (boost::filesystem::path(TEST_DATA_DIR) / "sharkfill-pattern.svg").string();
        std::shared_ptr<const SvgInfillTile> tile = FillSvg::tile_for(ipSvg, region_config.sparse_infill_svg_path.value);
        REQUIRE(tile != nullptr);
        REQUIRE_FALSE(tile->empty());
        // The 500 x 420 px pattern cell; the six <use> copies of the unit stroke, clipped to it.
        REQUIRE_THAT(tile->size.x(), WithinRel(500. * 25.4 / 96., 1e-3));
        REQUIRE(tile->lines.size() >= 6);
        BoundingBox bb;
        for (const Polyline &pl : tile->lines)
            bb.merge(pl.bounding_box());
        REQUIRE(bb.min.x() >= -10);
        REQUIRE(bb.max.x() <= scaled<coord_t>(tile->size.x()) + 10);
        REQUIRE(bb.min.y() >= -10);
        REQUIRE(bb.max.y() <= scaled<coord_t>(tile->size.y()) + 10);

        // Print at 20% (26 mm cells): fragments cut at cell edges rejoin into long lines, so the
        // output has far fewer polylines than there are stamped fragments.
        region_size = 120.;
        region_config.sparse_infill_svg_scale.value = 20.;
        const Polylines out = fill_with(ipSvg);
        REQUIRE_FALSE(out.empty());
        double longest = 0.;
        for (const Polyline &pl : out)
            longest = std::max(longest, unscaled<double>(pl.length()));
        REQUIRE(longest > 60.); // several cells' worth of one continuous stroke
    }

    SECTION("single wall prints a band drawing's 1 mm bands as centerlines") {
        // The original cat: a stroked drawing exported as a 1 mm filled band.
        region_config.sparse_infill_svg_path.value = (boost::filesystem::path(TEST_DATA_DIR) / "catfill-band.svg").string();
        std::shared_ptr<const SvgInfillTile> tile = FillSvg::tile_for(ipSvg, region_config.sparse_infill_svg_path.value);
        REQUIRE(tile != nullptr);
        REQUIRE_FALSE(tile->fills.empty()); // body and tail are two filled regions
        const FillSvg::Art outline = FillSvg::make_art(*tile, 1., 0.);
        const FillSvg::Art single  = FillSvg::make_art(*tile, 1., FillSvg::kSingleWallMaxWidths * 0.45);
        auto total = [](const FillSvg::Art &a) {
            double len = 0.;
            for (const Polygon &p : a.loops) len += unscaled<double>(p.length());
            for (const Polyline &l : a.lines) len += unscaled<double>(l.length());
            return len;
        };
        REQUIRE(single.lines.size() > 0);
        // Both edges of every band become one centerline: about half the length.
        REQUIRE(total(single) < 0.65 * total(outline));
        REQUIRE(total(single) > 0.35 * total(outline));

        // Through the Filler at an explicit scale the printed length halves as well.
        region_config.sparse_infill_svg_scale.value = 100.;
        region_size = 210.;
        region_config.sparse_infill_single_wall.value = false;
        const double both_edges = total_length_mm(fill_with(ipSvg));
        region_config.sparse_infill_single_wall.value = true;
        const double centerline = total_length_mm(fill_with(ipSvg));
        REQUIRE(centerline > 0.);
        REQUIRE(centerline < 0.65 * both_edges);
        // At the automatic scale the density is realized with the single-wall art, so the
        // printed length is about the same as in outline mode (smaller tiles, one line each).
        region_config.sparse_infill_svg_scale.value = 0.;
        region_config.sparse_infill_single_wall.value = false;
        const double auto_outline = total_length_mm(fill_with(ipSvg));
        region_config.sparse_infill_single_wall.value = true;
        const double auto_single = total_length_mm(fill_with(ipSvg));
        REQUIRE_THAT(auto_single, WithinRel(auto_outline, 0.25));
    }

    SECTION("missing file yields nothing rather than throwing") {
        region_config.sparse_infill_svg_path.value = "/nonexistent/does-not-exist.svg";
        REQUIRE(fill_with(ipSvg).empty());
    }
}

TEST_CASE("SVG infill <use> resolves ids that are not regex-safe", "[FillSvg]") {
    // The referenced id is looked up by plain string search: an id with regex syntax in it used to
    // be pasted into a std::regex, which threw and failed the slice.
    for (const std::string id : { "cell", "cell(1)", "cell[2]", "a.b*c+" }) {
        INFO("id " << id);
        const std::string svg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" width="10mm" height="10mm" viewBox="0 0 10 10">
  <defs><g id=")svg" + id + R"svg("><path d="M 1 1 L 4 1" fill="none" stroke="#000" stroke-width="0.3"/></g></defs>
  <use href="#)svg" + id + R"svg(" x="0" y="0"/>
  <use xlink:href="#)svg" + id + R"svg(" x="5" y="5"/>
</svg>)svg";
        SvgInfillTile tile;
        REQUIRE_NOTHROW(tile = FillSvg::tile_from_svg_text(svg));
        REQUIRE_FALSE(tile.empty());
        // Two clones of the 3 mm stroke.
        CHECK(tile.lines.size() == 2);
        CHECK_THAT(tile.length, WithinAbs(6., 0.05));
    }
}

TEST_CASE("SVG infill <use> clones a group with nested groups whole", "[FillSvg]") {
    const char *svg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="10mm" height="10mm" viewBox="0 0 10 10">
  <defs><g id="outer"><g><path d="M 1 1 L 4 1" fill="none" stroke="#000" stroke-width="0.3"/></g>
    <path d="M 1 2 L 4 2" fill="none" stroke="#000" stroke-width="0.3"/></g></defs>
  <use href="#outer"/>
</svg>)svg";
    const SvgInfillTile tile = FillSvg::tile_from_svg_text(svg);
    // Both strokes: the first </g> closes the nested group, not the referenced one.
    CHECK(tile.lines.size() == 2);
}
