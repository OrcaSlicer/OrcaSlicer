#include <catch2/catch_all.hpp>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCode/ContinuousPrint.hpp"
#include "libslic3r/GCode/SpiralVase.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/ShortestPath.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;

// Build an open extrusion path through the given XY points (in mm, unscaled).
static ExtrusionPath make_path(std::initializer_list<Vec2d> points_mm)
{
    ExtrusionPath path{erPerimeter, 1.0, 1.0, 1.0};
    for (const Vec2d &p : points_mm)
        path.polyline.append(Point3::new_scale(p.x(), p.y(), 0.0));
    return path;
}

// Build a closed loop through the given XY points (in mm, unscaled; do not repeat the first point).
static ExtrusionLoop make_loop(std::initializer_list<Vec2d> points_mm)
{
    ExtrusionPath path = make_path(points_mm);
    path.polyline.append(path.polyline.points.front());
    return ExtrusionLoop(path);
}

static Point to_point(const Vec2d &p_mm) { return Point::new_scale(p_mm.x(), p_mm.y()); }

// Check that consecutive entities of the chain connect end-to-start (with reversals applied).
static void check_chain_continuity(const std::vector<ExtrusionEntity*> &entities, const ExactChainResult &chain)
{
    REQUIRE(chain.order.size() == entities.size());
    for (size_t k = 1; k < chain.order.size(); ++ k) {
        const auto &[prev_idx, prev_reversed] = chain.order[k - 1];
        const auto &[curr_idx, curr_reversed] = chain.order[k];
        Point prev_end   = prev_reversed ? entities[prev_idx]->first_point() : entities[prev_idx]->last_point();
        Point curr_start = curr_reversed ? entities[curr_idx]->last_point() : entities[curr_idx]->first_point();
        CHECK(is_approx(prev_end, curr_start));
    }
}

SCENARIO("chain_extrusion_entities_exact: exact single-chain ordering", "[ContinuousPrint]") {
    GIVEN("An empty entity set") {
        std::vector<ExtrusionEntity*> entities;
        THEN("The layer is rejected") {
            CHECK(! chain_extrusion_entities_exact(entities));
        }
    }

    GIVEN("A single closed loop (vase-like layer)") {
        ExtrusionLoop loop = make_loop({{0, 0}, {10, 0}, {10, 10}, {0, 10}});
        std::vector<ExtrusionEntity*> entities{&loop};
        auto chain = chain_extrusion_entities_exact(entities);
        THEN("A closed chain is produced") {
            REQUIRE(chain);
            CHECK(chain->closed);
            CHECK(chain->order.size() == 1);
            CHECK(chain->order.front().second == false);
            CHECK(chain->start == chain->end);
        }
    }

    GIVEN("Two parallel disconnected lines (rectilinear-like infill)") {
        ExtrusionPath a = make_path({{0, 0}, {10, 0}});
        ExtrusionPath b = make_path({{0, 1}, {10, 1}});
        std::vector<ExtrusionEntity*> entities{&a, &b};
        THEN("The layer is rejected (more than 2 odd-degree endpoints)") {
            CHECK(! chain_extrusion_entities_exact(entities));
        }
    }

    GIVEN("Two disconnected loops") {
        ExtrusionLoop loop1 = make_loop({{0, 0}, {10, 0}, {10, 10}, {0, 10}});
        ExtrusionLoop loop2 = make_loop({{20, 0}, {30, 0}, {30, 10}, {20, 10}});
        std::vector<ExtrusionEntity*> entities{&loop1, &loop2};
        THEN("The layer is rejected (disconnected graph)") {
            CHECK(! chain_extrusion_entities_exact(entities));
        }
    }

    GIVEN("An open chain of three end-to-end segments") {
        ExtrusionPath a = make_path({{0, 0}, {10, 0}});
        ExtrusionPath b = make_path({{10, 0}, {10, 10}});
        ExtrusionPath c = make_path({{10, 10}, {20, 10}});
        std::vector<ExtrusionEntity*> entities{&a, &b, &c};
        auto chain = chain_extrusion_entities_exact(entities);
        THEN("An open chain covering all segments is produced") {
            REQUIRE(chain);
            CHECK(! chain->closed);
            check_chain_continuity(entities, *chain);
            // The two odd-degree endpoints are the chain start/end (order unspecified without preferred_start).
            CHECK(((chain->start == to_point({0, 0}) && chain->end == to_point({20, 10})) ||
                   (chain->start == to_point({20, 10}) && chain->end == to_point({0, 0}))));
        }
        AND_THEN("preferred_start selects the nearest odd endpoint as chain start") {
            Point near_end = to_point({19, 9});
            auto chain2 = chain_extrusion_entities_exact(entities, &near_end);
            REQUIRE(chain2);
            CHECK(chain2->start == to_point({20, 10}));
            CHECK(chain2->end == to_point({0, 0}));
            check_chain_continuity(entities, *chain2);
        }
    }

    GIVEN("A chain whose middle segment must be reversed") {
        ExtrusionPath a = make_path({{0, 0}, {10, 0}});
        ExtrusionPath b = make_path({{10, 10}, {10, 0}}); // stored "backwards"
        ExtrusionPath c = make_path({{10, 10}, {20, 10}});
        std::vector<ExtrusionEntity*> entities{&a, &b, &c};
        auto chain = chain_extrusion_entities_exact(entities);
        THEN("The chain succeeds and flags the middle segment as reversed") {
            REQUIRE(chain);
            check_chain_continuity(entities, *chain);
            auto it = std::find_if(chain->order.begin(), chain->order.end(), [](const auto &entry) { return entry.first == 1; });
            REQUIRE(it != chain->order.end());
            CHECK(it->second == true);
        }
    }

    GIVEN("Two segments with a gap larger than the endpoint tolerance") {
        ExtrusionPath a = make_path({{0, 0}, {10, 0}});
        ExtrusionPath b = make_path({{11, 0}, {20, 0}});
        std::vector<ExtrusionEntity*> entities{&a, &b};
        THEN("The layer is rejected (no connection lines are created)") {
            CHECK(! chain_extrusion_entities_exact(entities));
        }
    }

    GIVEN("A set containing a zero-length entity") {
        ExtrusionPath a = make_path({{0, 0}, {10, 0}});
        ExtrusionPath degenerate = make_path({{10, 0}, {10, 0}});
        std::vector<ExtrusionEntity*> entities{&a, &degenerate};
        THEN("The layer is rejected") {
            CHECK(! chain_extrusion_entities_exact(entities));
        }
    }
}

static std::vector<ExtrusionEntity*> view_of(const std::vector<std::unique_ptr<ExtrusionEntity>> &owned)
{
    std::vector<ExtrusionEntity*> view;
    view.reserve(owned.size());
    for (const auto &e : owned)
        view.push_back(e.get());
    return view;
}

SCENARIO("split_entities_at_junctions: lollipop junction splitting", "[ContinuousPrint]") {
    GIVEN("A wall loop with an infill trace ending on its edge (lollipop graph)") {
        ExtrusionLoop loop = make_loop({{0, 0}, {10, 0}, {10, 10}, {0, 10}});
        ExtrusionPath tail = make_path({{15, 0}, {5, 0}}); // endpoint (5,0) lies on the loop edge interior
        std::vector<ExtrusionEntity*> entities{&loop, &tail};
        auto working = split_entities_at_junctions(entities);
        THEN("The loop is linearized at the junction and the graph becomes chainable") {
            REQUIRE(working.size() == 2);
            std::vector<ExtrusionEntity*> view = view_of(working);
            auto chain = chain_extrusion_entities_exact(view);
            REQUIRE(chain);
            CHECK(! chain->closed);
            check_chain_continuity(view, *chain);
            // Odd-degree vertices: the junction (5,0) and the tail far end (15,0).
            CHECK(((chain->start == to_point({5, 0}) && chain->end == to_point({15, 0})) ||
                   (chain->start == to_point({15, 0}) && chain->end == to_point({5, 0}))));
        }
        AND_THEN("preflight_layer accepts the layer") {
            PrintConfig cfg;
            ContinuousLayerPlan plan;
            CHECK(preflight_layer(entities, nullptr, cfg, &plan) == ContinuousPrintVerdict::Applicable);
            CHECK(plan.entities.size() == 2);
            CHECK(! plan.is_closed);
        }
    }

    GIVEN("A ring bridged by two traces sharing an outer endpoint") {
        ExtrusionLoop loop = make_loop({{0, 0}, {10, 0}, {10, 10}, {0, 10}});
        ExtrusionPath a = make_path({{5, 0}, {20, 5}});  // (5,0) on the bottom edge
        ExtrusionPath b = make_path({{5, 10}, {20, 5}}); // (5,10) on the top edge
        std::vector<ExtrusionEntity*> entities{&loop, &a, &b};
        auto working = split_entities_at_junctions(entities);
        THEN("The ring splits into two pieces and the graph is chainable (2 odd vertices)") {
            REQUIRE(working.size() == 4);
            std::vector<ExtrusionEntity*> view = view_of(working);
            auto chain = chain_extrusion_entities_exact(view);
            REQUIRE(chain);
            CHECK(! chain->closed);
            check_chain_continuity(view, *chain);
            CHECK(((chain->start == to_point({5, 0}) && chain->end == to_point({5, 10})) ||
                   (chain->start == to_point({5, 10}) && chain->end == to_point({5, 0}))));
        }
    }

    GIVEN("An open path with a T-junction trace (4 odd-degree vertices)") {
        ExtrusionPath p = make_path({{0, 0}, {10, 0}});
        ExtrusionPath q = make_path({{5, 0}, {5, 8}}); // endpoint (5,0) on p's interior
        std::vector<ExtrusionEntity*> entities{&p, &q};
        auto working = split_entities_at_junctions(entities);
        THEN("The path is split but the layer is still rejected") {
            REQUIRE(working.size() == 3);
            std::vector<ExtrusionEntity*> view = view_of(working);
            CHECK(! chain_extrusion_entities_exact(view));
        }
    }

    GIVEN("A single loop without junctions (pass-through)") {
        ExtrusionLoop loop = make_loop({{0, 0}, {10, 0}, {10, 10}, {0, 10}});
        std::vector<ExtrusionEntity*> entities{&loop};
        auto working = split_entities_at_junctions(entities);
        THEN("The entity set is unchanged and still chains as closed") {
            REQUIRE(working.size() == 1);
            std::vector<ExtrusionEntity*> view = view_of(working);
            auto chain = chain_extrusion_entities_exact(view);
            REQUIRE(chain);
            CHECK(chain->closed);
        }
    }
}

SCENARIO("preflight_layer: in-layer single-chain check", "[ContinuousPrint]") {
    PrintConfig cfg;

    GIVEN("A layer organizable into one open chain") {
        ExtrusionPath a = make_path({{0, 0}, {10, 0}});
        ExtrusionPath b = make_path({{10, 0}, {10, 10}});
        ExtrusionPath c = make_path({{10, 10}, {20, 10}});
        std::vector<ExtrusionEntity*> entities{&a, &b, &c};
        ContinuousLayerPlan plan;
        THEN("The layer is applicable and the plan is consistent") {
            CHECK(preflight_layer(entities, nullptr, cfg, &plan) == ContinuousPrintVerdict::Applicable);
            CHECK(! plan.is_closed);
            CHECK(plan.order.size() == 3);
            CHECK(plan.total_length == Catch::Approx(30.0));
            REQUIRE(! plan.sampling.empty());
            CHECK(plan.sampling.front() == plan.start_point);
            CHECK(is_approx(plan.sampling.back(), plan.end_point));
            // ~1 mm sampling step over a 30 mm chain.
            CHECK(plan.sampling.size() >= 30);
        }
    }

    GIVEN("A vase-like layer with a single loop") {
        ExtrusionLoop loop = make_loop({{0, 0}, {10, 0}, {10, 10}, {0, 10}});
        std::vector<ExtrusionEntity*> entities{&loop};
        ContinuousLayerPlan plan;
        THEN("The layer is applicable and closed") {
            CHECK(preflight_layer(entities, nullptr, cfg, &plan) == ContinuousPrintVerdict::Applicable);
            CHECK(plan.is_closed);
            CHECK(plan.total_length == Catch::Approx(40.0));
        }
    }

    GIVEN("A layer of disconnected parallel lines") {
        ExtrusionPath a = make_path({{0, 0}, {10, 0}});
        ExtrusionPath b = make_path({{0, 1}, {10, 1}});
        std::vector<ExtrusionEntity*> entities{&a, &b};
        ContinuousLayerPlan plan;
        THEN("The layer is rejected") {
            CHECK(preflight_layer(entities, nullptr, cfg, &plan) == ContinuousPrintVerdict::Reject);
        }
    }
}

// A single-chain layer G-code snippet: one pure-Z move at the beginning, a travel move,
// a retract, then a closed square of extrusion moves (relative E).
static std::string make_chain_layer_gcode(double z)
{
    std::string s;
    s += "G1 Z" + std::to_string(z) + " F600\n";
    s += "G1 X0 Y0 F9000\n";   // travel to chain start: must be filtered out
    s += "G1 E-0.8 F1800\n";   // retract: must be filtered out
    s += "G1 X10 Y0 E0.5 F1500\n";
    s += "G1 X10 Y10 E0.5\n";
    s += "G1 X0 Y10 E0.5\n";
    s += "G1 X0 Y0 E0.5\n";
    return s;
}

SCENARIO("preflight_layer: preferred start and loop linearization (M3)", "[ContinuousPrint]") {
    PrintConfig cfg;

    GIVEN("A single closed loop and a preferred start lying on it") {
        ExtrusionLoop loop = make_loop({{0, 0}, {10, 0}, {10, 10}, {0, 10}});
        std::vector<ExtrusionEntity*> entities{&loop};
        ContinuousLayerPlan plan;
        const Point preferred = to_point({10, 5}); // middle of the right edge
        THEN("The plan is closed, starts at the preferred point and contains only paths") {
            CHECK(preflight_layer(entities, nullptr, cfg, &plan, &preferred) == ContinuousPrintVerdict::Applicable);
            CHECK(plan.is_closed);
            CHECK(plan.start_point == preferred);
            CHECK(plan.end_point == preferred);
            REQUIRE(plan.entities.size() == 1);
            // Loops are linearized so the chain emitter only ever deals with ExtrusionPath.
            CHECK(dynamic_cast<const ExtrusionLoop*>(plan.entities.front().get()) == nullptr);
            CHECK(dynamic_cast<const ExtrusionPath*>(plan.entities.front().get()) != nullptr);
        }
    }

    GIVEN("A single closed loop and a preferred start off the loop") {
        ExtrusionLoop loop = make_loop({{0, 0}, {10, 0}, {10, 10}, {0, 10}});
        std::vector<ExtrusionEntity*> entities{&loop};
        ContinuousLayerPlan plan;
        const Point preferred = to_point({100, 100});
        THEN("The seam moves to the vertex nearest the preferred point") {
            CHECK(preflight_layer(entities, nullptr, cfg, &plan, &preferred) == ContinuousPrintVerdict::Applicable);
            CHECK(plan.start_point == to_point({10, 10}));
        }
    }
}

SCENARIO("flatten_extrusion_entities resolves nested collections", "[ContinuousPrint]") {
    GIVEN("A collection with a nested collection of two paths") {
        ExtrusionPath a = make_path({{0, 0}, {10, 0}});
        ExtrusionPath b = make_path({{10, 0}, {10, 10}});
        auto inner = std::make_unique<ExtrusionEntityCollection>();
        inner->append(a);
        inner->append(b);
        auto outer = std::make_unique<ExtrusionEntityCollection>();
        outer->append(*inner);
        std::vector<ExtrusionEntity*> in;
        std::vector<ExtrusionEntity*> out;
        in.push_back(outer.get());
        flatten_extrusion_entities(in, out);
        THEN("Both leaf paths are exposed in order") {
            REQUIRE(out.size() == 2);
            CHECK(out[0]->first_point() == to_point({0, 0}));
            CHECK(out[1]->last_point() == to_point({10, 10}));
        }
        AND_THEN("preflight_layer accepts a raw collection input directly") {
            ContinuousLayerPlan plan;
            PrintConfig cfg;
            CHECK(preflight_layer(in, nullptr, cfg, &plan) == ContinuousPrintVerdict::Applicable);
            CHECK(! plan.is_closed);
            CHECK(plan.order.size() == 2);
        }
    }
}

SCENARIO("ContinuousPrint filter: parity with SpiralVase and zero-travel output", "[ContinuousPrint]") {
    PrintConfig cfg;
    cfg.option<ConfigOptionBool>("use_relative_e_distances", true)->value = true;
    cfg.option<ConfigOptionBool>("spiral_mode_smooth", true)->value      = false;

    SpiralVase      spiral(cfg);
    ContinuousPrint continuous(cfg);
    spiral.enable(true);
    continuous.enable(true);

    const std::string layer1 = make_chain_layer_gcode(0.2);
    const std::string layer2 = make_chain_layer_gcode(0.4);

    const std::string spiral_out1     = spiral.process_layer(layer1, false);
    const std::string continuous_out1 = continuous.process_layer(layer1, false);
    const std::string spiral_out2     = spiral.process_layer(layer2, true);
    const std::string continuous_out2 = continuous.process_layer(layer2, true);

    THEN("The ContinuousPrint filter matches SpiralVase exactly on a closed loop") {
        CHECK(continuous_out1 == spiral_out1);
        CHECK(continuous_out2 == spiral_out2);
    }

    THEN("The filtered output contains no non-extruding XY move (zero travel)") {
        for (const std::string &out : {continuous_out1, continuous_out2}) {
            GCodeReader reader;
            reader.apply_config(cfg);
            reader.parse_buffer(out, [](GCodeReader &r, const GCodeReader::GCodeLine &line) {
                if (line.cmd_is("G1") && (line.has_x() || line.has_y()) && line.dist_XY(r) > 0)
                    CHECK(line.extruding(r));
            });
        }
    }

    THEN("Z ramps up smoothly and monotonically across the chain") {
        GCodeReader reader;
        reader.apply_config(cfg);
        float last_z = 0.f;
        size_t extrusion_moves = 0;
        reader.parse_buffer(continuous_out2, [&last_z, &extrusion_moves](GCodeReader &r, const GCodeReader::GCodeLine &line) {
            if (line.cmd_is("G1") && line.extruding(r) && line.dist_XY(r) > 0) {
                const float z = line.new_Z(r);
                CHECK(z >= last_z);
                last_z = z;
                ++ extrusion_moves;
            }
        });
        CHECK(extrusion_moves == 4);
        CHECK(last_z == Catch::Approx(0.4).margin(1e-3));
    }
}

// Guards the GUI/preset registration: an option that is appended to a settings tab but missing from
// the process preset option list makes the tab read a key absent from its config and crash.
SCENARIO("continuous_print_mode is registered as a process preset option", "[ContinuousPrint]") {
    THEN("The key is exposed by Preset::print_options() and present in the full print config") {
        const std::vector<std::string> &keys = Preset::print_options();
        CHECK(std::find(keys.begin(), keys.end(), "continuous_print_mode") != keys.end());
        FullPrintConfig config;
        CHECK(config.has("continuous_print_mode"));
    }
}

// Real slicer output leaves a sub-line-width gap between wall and fill (measured ~0.12 mm on
// top/bottom surfaces). The junction tolerance must be physical and the contacting endpoint must be
// snapped, otherwise every real layer is rejected (this was the case with SCALED_EPSILON).
SCENARIO("preflight_layer: physical junction tolerance snaps a nearby fill endpoint", "[ContinuousPrint]") {
    PrintConfig cfg;
    GIVEN("A wall loop and an infill trace ending 0.15 mm away from its edge") {
        ExtrusionLoop loop = make_loop({{0, 0}, {10, 0}, {10, 10}, {0, 10}});
        ExtrusionPath tail = make_path({{5, -3}, {5, -0.15}});
        std::vector<ExtrusionEntity*> entities{&loop, &tail};
        ContinuousLayerPlan plan;
        THEN("The strict geometric tolerance rejects it") {
            CHECK(preflight_layer(entities, nullptr, cfg, &plan) == ContinuousPrintVerdict::Reject);
        }
        AND_THEN("A physical tolerance accepts it and the resulting chain is exactly connected") {
            CHECK(preflight_layer(entities, nullptr, cfg, &plan, nullptr, scale_(0.3)) == ContinuousPrintVerdict::Applicable);
            REQUIRE(plan.entities.size() == 2);
            std::vector<ExtrusionEntity*> view = view_of(plan.entities);
            auto chain = chain_extrusion_entities_exact(view);
            REQUIRE(chain);
            check_chain_continuity(view, *chain);
            CHECK(! chain->closed);
        }
    }
}
