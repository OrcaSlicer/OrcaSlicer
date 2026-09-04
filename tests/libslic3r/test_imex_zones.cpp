#include <catch2/catch_all.hpp>

#include <string>
#include <vector>

#include "libslic3r/IMEXHelpers.hpp"
#include "libslic3r/IMEXZones.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

// A square 200x200 bed keeps every expected coordinate an exact round number, so a
// failure reports a wrong decision rather than a rounding difference.
const BoundingBoxf kBed{ Vec2d(0.0, 0.0), Vec2d(200.0, 200.0) };

constexpr const char* kMode = "parallel";

struct PrinterCfg
{
    int            gantry_count     = 1;
    int            tools_per_gantry = 2;
    ImexToolLayout tool_layout      = ImexToolLayout::FrontLeft;
    std::string    active_tools;          // the imex_mode_active_tools entry for kMode
    double         clearance_x      = 0.0;
    double         clearance_y      = 0.0;
    double         margin           = 0.0;
    bool           is_imex          = true;
};

// Builds the subset of the printer preset compute_imex_zone_layout actually reads. Every
// key is set explicitly so no assertion below depends on a PrintConfig default.
DynamicPrintConfig make_cfg(const PrinterCfg& p)
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("is_imex", new ConfigOptionBool(p.is_imex));
    cfg.set_key_value("imex_gantry_count", new ConfigOptionInt(p.gantry_count));
    cfg.set_key_value("imex_tools_per_gantry", new ConfigOptionInt(p.tools_per_gantry));
    cfg.set_key_value("imex_tool_layout", new ConfigOptionEnum<ImexToolLayout>(p.tool_layout));
    // Slot 0 is the reserved Primary mode, which never carries tools; kMode is slot 1.
    cfg.set_key_value("imex_mode_names",
                      new ConfigOptionStrings(std::vector<std::string>{ kImexPrimaryMode, kMode }));
    cfg.set_key_value("imex_mode_active_tools",
                      new ConfigOptionStrings(std::vector<std::string>{ std::string(), p.active_tools }));
    cfg.set_key_value("imex_nozzle_clearance_x", new ConfigOptionFloat(p.clearance_x));
    cfg.set_key_value("imex_nozzle_clearance_y", new ConfigOptionFloat(p.clearance_y));
    cfg.set_key_value("imex_carriage_margin", new ConfigOptionFloat(p.margin));
    return cfg;
}

ImexZoneLayout layout_for(const PrinterCfg& p,
                          const std::string& plate_mode   = kMode,
                          const std::string& process_mode = std::string())
{
    return compute_imex_zone_layout(make_cfg(p), plate_mode, process_mode, kBed);
}

void check_rect(const BoundingBoxf& b, double x0, double y0, double x1, double y1)
{
    CHECK_THAT(b.min.x(), WithinAbs(x0, 1e-9));
    CHECK_THAT(b.min.y(), WithinAbs(y0, 1e-9));
    CHECK_THAT(b.max.x(), WithinAbs(x1, 1e-9));
    CHECK_THAT(b.max.y(), WithinAbs(y1, 1e-9));
}

void check_box_xy(const BoundingBoxf3& b, double x0, double y0, double x1, double y1)
{
    CHECK_THAT(b.min.x(), WithinAbs(x0, 1e-9));
    CHECK_THAT(b.min.y(), WithinAbs(y0, 1e-9));
    CHECK_THAT(b.max.x(), WithinAbs(x1, 1e-9));
    CHECK_THAT(b.max.y(), WithinAbs(y1, 1e-9));
}

void check_point(const Vec2d& p, double x, double y)
{
    CHECK_THAT(p.x(), WithinAbs(x, 1e-9));
    CHECK_THAT(p.y(), WithinAbs(y, 1e-9));
}

bool is_empty(const ImexZoneLayout& l)
{
    return l.primary_head == -1
        && !l.primary_zone_box.has_value()
        && l.head_zone_centers.empty()
        && l.copy_zones.empty()
        && l.mirror_zones.empty()
        && l.secondary_zone_boxes.empty()
        && l.collision_zones.empty()
        && l.margin_bands.empty();
}

} // namespace

// D1 regression: both strip loops are gated on `carriage_w > 0.0`, so a clearance fallback of
// 0.0 would emit no collision strips at all -- the geometry that keeps two carriages from
// meeting -- while the preview still drew 30 mm toolhead boxes. The fallback must match the
// value registered in PrintConfig.cpp (30.0), not zero.
//
// So the strip's WIDTH is the claim, not its existence: a fallback of 0.1 would leave the zone
// list non-empty and the carriages a nozzle's width apart. 1 gantry x 2 tools on the 200mm bed
// puts the column boundary at x = 100, and the strip lies one clearance inside the primary's
// column, so the registered 30.0 is the rectangle x [70, 100] over the primary's full depth.
TEST_CASE("a config missing the nozzle clearance keys falls back to the registered clearance",
          "[IMEXZones]")
{
    // Deliberately NOT via make_cfg(): the point is a config that never carries the keys.
    // imex_tool_layout is left out too, so the layout falls back to FrontLeft and T0 owns the
    // left column -- the same corner every other case in this file starts from.
    DynamicPrintConfig cfg;
    cfg.set_key_value("is_imex", new ConfigOptionBool(true));
    cfg.set_key_value("imex_gantry_count", new ConfigOptionInt(1));
    cfg.set_key_value("imex_tools_per_gantry", new ConfigOptionInt(2));
    cfg.set_key_value("imex_mode_names", new ConfigOptionStrings({ kMode }));
    cfg.set_key_value("imex_mode_active_tools", new ConfigOptionStrings({ "0:P,1:M" }));

    const ImexZoneLayout l = compute_imex_zone_layout(cfg, kMode, std::string(), kBed);

    REQUIRE(l.primary_head == 0);
    REQUIRE(l.primary_zone_box.has_value());
    check_rect(*l.primary_zone_box, 0.0, 0.0, 100.0, 200.0);

    // One X boundary, one strip, 30mm wide against it.
    REQUIRE(l.collision_zones.size() == 1);
    check_box_xy(l.collision_zones[0], 70.0, 0.0, 100.0, 200.0);

    // The margin's own fallback is 0.0, and a zero margin raises no band.
    CHECK(l.margin_bands.empty());
}

TEST_CASE("an IDEX mirror pair splits the bed into two columns", "[IMEXZones]")
{
    // 1 gantry x 2 tools: the classic IDEX case. T1 mirrors T0, so the split is on X and
    // the boundary between the two zones carries a carriage danger strip.
    PrinterCfg p;
    p.gantry_count     = 1;
    p.tools_per_gantry = 2;
    p.active_tools     = "0:P,1:M";
    p.clearance_x      = 5.0;
    p.margin           = 2.0;

    const ImexZoneLayout l = layout_for(p);

    REQUIRE(l.primary_head == 0);
    REQUIRE(l.primary_zone_box.has_value());
    check_rect(*l.primary_zone_box, 0.0, 0.0, 100.0, 200.0);

    CHECK(l.copy_zones.empty());
    REQUIRE(l.mirror_zones.size() == 1);
    check_rect(l.mirror_zones[0], 100.0, 0.0, 200.0, 200.0);

    // Both heads get a zone centre; the ghost offset is the difference between them.
    REQUIRE(l.head_zone_centers.size() == 2);
    check_point(l.head_zone_centers.at(0), 50.0, 100.0);
    check_point(l.head_zone_centers.at(1), 150.0, 100.0);

    // The mirror zone is off limits to objects, over the full Z range.
    REQUIRE(l.secondary_zone_boxes.size() == 1);
    check_box_xy(l.secondary_zone_boxes[0], 100.0, 0.0, 200.0, 200.0);
    CHECK_THAT(l.secondary_zone_boxes[0].min.z(), WithinAbs(-1.0, 1e-9));
    CHECK(l.secondary_zone_boxes[0].max.z() > 1000.0);

    // Danger strip sits inside the primary zone, against the boundary, one clearance wide.
    REQUIRE(l.collision_zones.size() == 1);
    check_box_xy(l.collision_zones[0], 95.0, 0.0, 100.0, 200.0);

    // The advisory margin band abuts the strip on the primary side, one margin wide.
    REQUIRE(l.margin_bands.size() == 1);
    check_rect(l.margin_bands[0], 93.0, 0.0, 95.0, 200.0);
}

TEST_CASE("a copy secondary claims a zone but raises no collision strip", "[IMEXZones]")
{
    // Copy tools travel in the same direction as the primary and can never close on it,
    // so the shared boundary needs no danger strip -- only mirrors do.
    PrinterCfg p;
    p.gantry_count     = 1;
    p.tools_per_gantry = 2;
    p.active_tools     = "0:P,1:C";
    p.clearance_x      = 5.0;
    p.margin           = 2.0;

    const ImexZoneLayout l = layout_for(p);

    REQUIRE(l.copy_zones.size() == 1);
    check_rect(l.copy_zones[0], 100.0, 0.0, 200.0, 200.0);
    CHECK(l.mirror_zones.empty());
    CHECK(l.secondary_zone_boxes.size() == 1);
    CHECK(l.collision_zones.empty());
    CHECK(l.margin_bands.empty());
}

TEST_CASE("the tool layout decides which corner the primary zone occupies", "[IMEXZones]")
{
    // imex_tool_layout names the bed corner T0 sits in. With all four tools of a 2x2 IQEX
    // active the bed splits into quadrants, so the primary quadrant is a direct readout of
    // how tool indices were mapped onto columns and rows.
    struct Case { ImexToolLayout layout; const char* name; double x0, y0, x1, y1; };
    const Case cases[] = {
        { ImexToolLayout::FrontLeft,  "FrontLeft",    0.0,   0.0, 100.0, 100.0 },
        { ImexToolLayout::FrontRight, "FrontRight", 100.0,   0.0, 200.0, 100.0 },
        { ImexToolLayout::RearLeft,   "RearLeft",     0.0, 100.0, 100.0, 200.0 },
        { ImexToolLayout::RearRight,  "RearRight",  100.0, 100.0, 200.0, 200.0 },
    };

    for (const Case& c : cases) {
        DYNAMIC_SECTION(c.name) {
            PrinterCfg p;
            p.gantry_count     = 2;
            p.tools_per_gantry = 2;
            p.tool_layout      = c.layout;
            p.active_tools     = "0:P,1:C,2:M,3:M";

            const ImexZoneLayout l = layout_for(p);

            REQUIRE(l.primary_zone_box.has_value());
            check_rect(*l.primary_zone_box, c.x0, c.y0, c.x1, c.y1);
            // The primary's zone centre must agree with its zone.
            REQUIRE(l.head_zone_centers.count(0) == 1);
            check_point(l.head_zone_centers.at(0), 0.5 * (c.x0 + c.x1), 0.5 * (c.y0 + c.y1));
            // Three secondaries, each with its own quadrant.
            CHECK(l.secondary_zone_boxes.size() == 3);
        }
    }
}

TEST_CASE("a 2x2 IQEX with independent secondaries gives one quadrant per tool", "[IMEXZones]")
{
    // No Span is declared, so nothing aggregates: T1, T2 and T3 each keep their own cell.
    // Only T2 shares the primary's column, so it alone can collide with the primary
    // carriage -- the diagonal T3 cannot, and must not raise a strip of its own.
    PrinterCfg p;
    p.gantry_count     = 2;
    p.tools_per_gantry = 2;
    p.active_tools     = "0:P,1:C,2:M,3:M";
    p.clearance_x      = 6.0;
    p.clearance_y      = 4.0;

    const ImexZoneLayout l = layout_for(p);

    REQUIRE(l.primary_zone_box.has_value());
    check_rect(*l.primary_zone_box, 0.0, 0.0, 100.0, 100.0);

    REQUIRE(l.copy_zones.size() == 1);
    check_rect(l.copy_zones[0], 100.0, 0.0, 200.0, 100.0);

    REQUIRE(l.mirror_zones.size() == 2);
    check_rect(l.mirror_zones[0], 0.0, 100.0, 100.0, 200.0);
    check_rect(l.mirror_zones[1], 100.0, 100.0, 200.0, 200.0);

    // Blocking boxes are the copy rects followed by the mirror rects, so the placement
    // check and the rendered overlay always describe the same three areas.
    REQUIRE(l.secondary_zone_boxes.size() == 3);
    check_box_xy(l.secondary_zone_boxes[0], 100.0, 0.0, 200.0, 100.0);
    check_box_xy(l.secondary_zone_boxes[1], 0.0, 100.0, 100.0, 200.0);
    check_box_xy(l.secondary_zone_boxes[2], 100.0, 100.0, 200.0, 200.0);

    // T2 is directly behind the primary: one strip on the primary's rear boundary,
    // clearance_y deep, spanning the primary zone's width. T3 contributes nothing.
    REQUIRE(l.collision_zones.size() == 1);
    check_box_xy(l.collision_zones[0], 0.0, 96.0, 100.0, 100.0);
    CHECK(l.margin_bands.empty()); // margin defaults to 0
}

TEST_CASE("a Span partner collapses the far gantry into one full-width strip", "[IMEXZones]")
{
    // Paired-gantry multicolor: T1 is the primary's within-gantry Span partner, so the
    // second gantry acts as a single mirrored unit rather than two independent tools. Its
    // two mirrors merge into one row strip spanning the whole bed width, and the primary
    // zone widens to match.
    PrinterCfg p;
    p.gantry_count     = 2;
    p.tools_per_gantry = 2;
    p.active_tools     = "0:P,1:S,2:M,3:M";
    p.clearance_x      = 6.0;
    p.clearance_y      = 4.0;

    const ImexZoneLayout l = layout_for(p);

    REQUIRE(l.primary_head == 0);
    REQUIRE(l.primary_zone_box.has_value());
    check_rect(*l.primary_zone_box, 0.0, 0.0, 200.0, 100.0);

    REQUIRE(l.mirror_zones.size() == 1);
    check_rect(l.mirror_zones[0], 0.0, 100.0, 200.0, 200.0);
    CHECK(l.copy_zones.empty());
    CHECK(l.secondary_zone_boxes.size() == 1);

    // A Span tool prints in the primary's own zone, so it owns no zone centre of its own.
    CHECK(l.head_zone_centers.count(1) == 0);
    check_point(l.head_zone_centers.at(0), 50.0, 50.0);
    check_point(l.head_zone_centers.at(2), 50.0, 150.0);
    check_point(l.head_zone_centers.at(3), 150.0, 150.0);

    // Regression guard: T3 sits on the other gantry's row, so the gantries never overlap
    // in X and no right-edge strip may be drawn. Only the rear boundary is dangerous, and
    // it runs the full width of the widened primary zone.
    REQUIRE(l.collision_zones.size() == 1);
    check_box_xy(l.collision_zones[0], 0.0, 96.0, 200.0, 100.0);
}

TEST_CASE("inactive tools donate their bed share to the active ones", "[IMEXZones]")
{
    // Three tools on one gantry, but the mode activates only T0 and T2. Zones are sized by
    // active tool count, so each gets half the bed rather than a third with a dead middle
    // band -- and T2's zone starts where the primary's ends.
    PrinterCfg p;
    p.gantry_count     = 1;
    p.tools_per_gantry = 3;
    p.active_tools     = "0:P,2:M";
    p.clearance_x      = 5.0;

    const ImexZoneLayout l = layout_for(p);

    REQUIRE(l.primary_zone_box.has_value());
    check_rect(*l.primary_zone_box, 0.0, 0.0, 100.0, 200.0);
    CHECK_THAT(l.primary_zone_box->size().x(), WithinAbs(100.0, 1e-9)); // half, not 200/3

    REQUIRE(l.mirror_zones.size() == 1);
    check_rect(l.mirror_zones[0], 100.0, 0.0, 200.0, 200.0);

    REQUIRE(l.head_zone_centers.size() == 2);
    check_point(l.head_zone_centers.at(0), 50.0, 100.0);
    check_point(l.head_zone_centers.at(2), 150.0, 100.0);

    // The two zones are adjacent, so the boundary between them is a real collision risk.
    REQUIRE(l.collision_zones.size() == 1);
    check_box_xy(l.collision_zones[0], 95.0, 0.0, 100.0, 200.0);
}

TEST_CASE("the plate mode falls back to the process preset only when it is Primary", "[IMEXZones]")
{
    PrinterCfg p;
    p.gantry_count     = 1;
    p.tools_per_gantry = 2;
    p.active_tools     = "0:P,1:M";

    SECTION("a plate still on Primary picks up the process preset's mode") {
        const ImexZoneLayout l = layout_for(p, kImexPrimaryMode, kMode);
        REQUIRE(l.primary_zone_box.has_value());
        check_rect(*l.primary_zone_box, 0.0, 0.0, 100.0, 200.0);
    }

    SECTION("a plate with its own mode ignores the process preset") {
        // The process preset names a mode that does not exist; the plate's own mode wins,
        // so the layout is still built from kMode's tools.
        const ImexZoneLayout l = layout_for(p, kMode, "no-such-mode");
        REQUIRE(l.primary_zone_box.has_value());
        check_rect(*l.primary_zone_box, 0.0, 0.0, 100.0, 200.0);
    }

    SECTION("Primary on both sides yields no zones at all") {
        CHECK(is_empty(layout_for(p, kImexPrimaryMode, std::string())));
        CHECK(is_empty(layout_for(p, kImexPrimaryMode, kImexPrimaryMode)));
    }
}

TEST_CASE("configurations with nothing to divide yield an empty layout", "[IMEXZones]")
{
    PrinterCfg base;
    base.gantry_count     = 1;
    base.tools_per_gantry = 2;
    base.active_tools     = "0:P,1:M";

    SECTION("a non-IMEX printer") {
        PrinterCfg p = base;
        p.is_imex = false;
        CHECK(is_empty(layout_for(p)));
    }

    SECTION("an empty plate mode") {
        CHECK(is_empty(layout_for(base, std::string(), std::string())));
    }

    SECTION("a single-tool grid has no second zone to dim") {
        PrinterCfg p = base;
        p.gantry_count     = 1;
        p.tools_per_gantry = 1;
        p.active_tools     = "0:P";
        CHECK(is_empty(layout_for(p)));
    }

    SECTION("a mode name that the printer does not define") {
        // A stale mode carried in from another printer's process preset: the name resolves
        // to no tool roster, so there is nothing to lay out.
        CHECK(is_empty(layout_for(base, "carried-over-mode")));
    }

    SECTION("a mode whose tool roster is empty") {
        PrinterCfg p = base;
        p.active_tools = "";
        CHECK(is_empty(layout_for(p)));
    }
}

TEST_CASE("a Primary that is not on the grid yields no zones at all", "[IMEXZones]")
{
    // The modes editor preserves tool assignments across imex_gantry_count changes, so a mode
    // authored on a 4-tool IQEX with Primary on T3 keeps that assignment after the printer is
    // reconfigured as a 2-tool IDEX. T3 is then off the grid and cannot anchor a layout: the
    // primary's cell is what every zone, strip and offset is measured from.
    PrinterCfg p;
    p.gantry_count     = 1;
    p.tools_per_gantry = 2;
    p.active_tools     = "3:P,0:M";
    p.clearance_x      = 5.0;
    p.margin           = 2.0;

    const ImexZoneLayout l = layout_for(p);

    CHECK(is_empty(l));

    // Spelled out, because the failure this guards is a self-contradiction rather than a
    // wrong rectangle: the primary zone used to report the whole bed printable while the
    // secondary box reported the very same rectangle blocked.
    CHECK(l.primary_head == -1);
    CHECK_FALSE(l.primary_zone_box.has_value());
    CHECK(l.secondary_zone_boxes.empty());
    CHECK(l.mirror_zones.empty());
    // No zone centre either, so no ghost is placed from a cell no tool occupies.
    CHECK(l.head_zone_centers.empty());
    // And with no primary zone, a firmware-managed printer shifts the slice by nothing
    // rather than by the bed centre.
    check_point(compute_imex_slice_offset(true, kMode, l.primary_zone_box), 0.0, 0.0);
}

TEST_CASE("a mode with no Primary marker at all yields no zones", "[IMEXZones]")
{
    // Same defect by a different route: every tool is a secondary, so there is no `:P` to
    // anchor the layout even though the roster is non-empty and entirely on the grid.
    PrinterCfg p;
    p.gantry_count     = 1;
    p.tools_per_gantry = 2;
    p.active_tools     = "0:M,1:M";
    p.clearance_x      = 5.0;

    CHECK(is_empty(layout_for(p)));
}

TEST_CASE("a Primary on the last tool of the grid lays out normally", "[IMEXZones]")
{
    // Boundary of the bound that drops an off-grid Primary: the highest valid index must
    // still be accepted, so the guard cannot be an off-by-one that swallows real modes.
    SECTION("last column of a 1x2 IDEX") {
        PrinterCfg p;
        p.gantry_count     = 1;
        p.tools_per_gantry = 2;
        p.active_tools     = "1:P,0:M";
        p.clearance_x      = 5.0;
        p.margin           = 2.0;

        const ImexZoneLayout l = layout_for(p);

        REQUIRE(l.primary_head == 1);
        REQUIRE(l.primary_zone_box.has_value());
        check_rect(*l.primary_zone_box, 100.0, 0.0, 200.0, 200.0);

        REQUIRE(l.mirror_zones.size() == 1);
        check_rect(l.mirror_zones[0], 0.0, 0.0, 100.0, 200.0);

        REQUIRE(l.head_zone_centers.size() == 2);
        check_point(l.head_zone_centers.at(0), 50.0, 100.0);
        check_point(l.head_zone_centers.at(1), 150.0, 100.0);

        // The mirror sits to the primary's left, so the strip hugs the primary zone's
        // left boundary and the advisory band lies further inside it.
        REQUIRE(l.collision_zones.size() == 1);
        check_box_xy(l.collision_zones[0], 100.0, 0.0, 105.0, 200.0);
        REQUIRE(l.margin_bands.size() == 1);
        check_rect(l.margin_bands[0], 105.0, 0.0, 107.0, 200.0);
    }

    SECTION("last tool of a 2x2 IQEX") {
        PrinterCfg p;
        p.gantry_count     = 2;
        p.tools_per_gantry = 2;
        p.active_tools     = "3:P,0:M";
        p.clearance_x      = 5.0;
        p.clearance_y      = 5.0;

        const ImexZoneLayout l = layout_for(p);

        REQUIRE(l.primary_head == 3);
        REQUIRE(l.primary_zone_box.has_value());
        check_rect(*l.primary_zone_box, 100.0, 100.0, 200.0, 200.0);

        REQUIRE(l.mirror_zones.size() == 1);
        check_rect(l.mirror_zones[0], 0.0, 0.0, 100.0, 100.0);

        // T0 is diagonally opposite the primary, so the gantries never overlap and no
        // boundary of the primary zone is a collision risk.
        CHECK(l.collision_zones.empty());
    }
}

TEST_CASE("zero clearance and zero margin suppress the strips they size", "[IMEXZones]")
{
    // The strip width is the literal clearance value, so a printer that has not configured
    // one gets no strip rather than a zero-width box the placement check would still test.
    PrinterCfg p;
    p.gantry_count     = 1;
    p.tools_per_gantry = 2;
    p.active_tools     = "0:P,1:M";

    SECTION("no clearance, no strip") {
        const ImexZoneLayout l = layout_for(p);
        CHECK(l.collision_zones.empty());
        CHECK(l.margin_bands.empty());
    }

    SECTION("clearance without margin gives the strip but no advisory band") {
        p.clearance_x = 5.0;
        const ImexZoneLayout l = layout_for(p);
        CHECK(l.collision_zones.size() == 1);
        CHECK(l.margin_bands.empty());
    }
}

TEST_CASE("an aggregated gantry raises its collision strip from the zone it paints", "[IMEXZones]")
{
    // A Span partner on the primary's gantry collapses the far gantry to a single cell,
    // pinned to the primary's column and expanded into a full-bed-width row strip. The
    // danger strip on the primary's rear boundary has to follow that painted strip, not any
    // one head's own grid cell -- with three tools per gantry the surviving mirrors sit in
    // columns 1 and 2, neither of which is the primary's, yet the zone they jointly paint
    // still runs the full width of the bed and still closes on the primary's carriage.
    //
    // Both rosters describe the same machine geometry (one primary gantry, one mirrored
    // gantry behind it), so both must produce the same rectangles.
    struct Case { const char* name; int tools_per_gantry; const char* active_tools; };
    const Case cases[] = {
        { "two tools per gantry, mirrors on T2 and T3",   2, "0:P,1:S,2:M,3:M" },
        { "three tools per gantry, mirrors on T4 and T5", 3, "0:P,1:S,4:M,5:M" },
    };

    for (const Case& c : cases) {
        DYNAMIC_SECTION(c.name) {
            PrinterCfg p;
            p.gantry_count     = 2;
            p.tools_per_gantry = c.tools_per_gantry;
            p.active_tools     = c.active_tools;
            p.clearance_x      = 6.0;
            p.clearance_y      = 4.0;
            p.margin           = 2.0;

            const ImexZoneLayout l = layout_for(p);

            REQUIRE(l.primary_head == 0);
            REQUIRE(l.primary_zone_box.has_value());
            check_rect(*l.primary_zone_box, 0.0, 0.0, 200.0, 100.0);

            // One merged rear strip for the whole far gantry, however many heads it carries.
            CHECK(l.copy_zones.empty());
            REQUIRE(l.mirror_zones.size() == 1);
            check_rect(l.mirror_zones[0], 0.0, 100.0, 200.0, 200.0);
            REQUIRE(l.secondary_zone_boxes.size() == 1);
            check_box_xy(l.secondary_zone_boxes[0], 0.0, 100.0, 200.0, 200.0);

            // The rear boundary is the collision risk, one clearance_y deep and as wide as
            // the widened primary zone. Exactly one strip: the two gantries share no X
            // extent, so neither side boundary may raise one of its own.
            REQUIRE(l.collision_zones.size() == 1);
            check_box_xy(l.collision_zones[0], 0.0, 96.0, 200.0, 100.0);

            // And the advisory band immediately inside it, one margin deep.
            REQUIRE(l.margin_bands.size() == 1);
            check_rect(l.margin_bands[0], 0.0, 94.0, 200.0, 96.0);
        }
    }
}
