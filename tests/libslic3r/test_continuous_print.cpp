#include <catch2/catch_all.hpp>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCode/ContinuousPrint.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/ClipperUtils.hpp"
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

// Both surface endpoints touch the inner wall away from its stored seam. Splitting the
// contour before area ordering used to turn it into open fragments and reject multi-wall layers.
SCENARIO("Continuous print joins surfaces through nested walls", "[ContinuousPrint]") {
    const int wall_count = GENERATE(1, 2, 3, 4, 6);
    const bool start_at_fill = GENERATE(false, true);
    const ExtrusionRole fill_role = GENERATE(erSolidInfill, erTopSolidInfill, erInternalInfill);
    std::vector<std::unique_ptr<ExtrusionEntity>> owned;
    for (int i = 0; i < wall_count; ++ i) {
        const double d = 0.45 * i;
        auto loop = make_loop({{d, d}, {20-d, d}, {20-d, 20-d}, {d, 20-d}});
        loop.paths.front().set_extrusion_role(i == 0 ? erExternalPerimeter : erPerimeter);
        owned.emplace_back(loop.clone());
    }
    const double d = 0.45 * (wall_count - 1);
    auto fill = make_path({{5, d+0.15}, {5, 10}, {15, 10}, {15, d+0.15}});
    fill.set_extrusion_role(fill_role);
    owned.emplace_back(fill.clone());
    PrintConfig cfg;
    Point preferred = start_at_fill ? fill.first_point() : to_point({0, 0});
    for (int layer = 0; layer < 2; ++ layer) {
        ContinuousLayerPlan plan;
        REQUIRE(preflight_layer(view_of(owned), nullptr, cfg, &plan, &preferred,
                                scale_(0.2), scale_(2.0)) == ContinuousPrintVerdict::Applicable);
        for (size_t i = 1; i < plan.entities.size(); ++ i)
            CHECK(is_approx(plan.entities[i-1]->last_point(), plan.entities[i]->first_point()));
        CHECK(plan.entities.front()->role() == ((layer == 0) != start_at_fill ? erExternalPerimeter : fill_role));
        preferred = plan.end_point;
    }
}

TEST_CASE("Continuous print reseaming preserves every wall corner", "[ContinuousPrint]") {
    auto loop = make_loop({{0, 0}, {10, 0}, {10, 10}, {0, 10}});
    PrintConfig cfg;
    ContinuousLayerPlan plan;
    Point preferred = to_point({10, 5});
    REQUIRE(preflight_layer({&loop}, nullptr, cfg, &plan, &preferred) == ContinuousPrintVerdict::Applicable);
    CHECK(plan.total_length == Catch::Approx(40.0));
}

TEST_CASE("Surface fragments are visited locally without losing geometry or flow", "[ContinuousPrint]") {
    auto surface = make_path({{0, 0}, {20, 0}});
    surface.set_extrusion_role(GENERATE(erBottomSurface, erTopSolidInfill, erSolidInfill));
    auto a = make_path({{5, 0.2}, {6, 0.2}});
    auto b = make_path({{15, 0.2}, {16, 0.2}});
    a.set_extrusion_role(erGapFill);
    b.set_extrusion_role(erGapFill);
    a.mm3_per_mm = b.mm3_per_mm = 0.025;
    a.width = b.width = 0.2f;
    const std::vector<ExtrusionEntity*> input{&surface, &a, &b};
    PrintConfig cfg;
    ContinuousLayerPlan plan;
    const Point preferred = GENERATE(to_point({0, 0}), to_point({20, 0}));
    REQUIRE(preflight_layer(input, nullptr, cfg, &plan, &preferred, SCALED_EPSILON, scale_(2.)) == ContinuousPrintVerdict::Applicable);
    REQUIRE(plan.entities.size() == 1);
    const auto *joined = dynamic_cast<const ExtrusionMultiPath*>(plan.entities.front().get());
    REQUIRE(joined != nullptr);
    double gap_volume = 0.;
    size_t fragments = 0;
    for (size_t i = 0; i < joined->paths.size(); ++ i) {
        const auto &path = joined->paths[i];
        if (i > 0)
            CHECK(path.first_point() == joined->paths[i - 1].last_point());
        if (path.role() == erGapFill) {
            ++ fragments;
            gap_volume += path.total_volume();
            CHECK(path.width == Catch::Approx(0.2));
            CHECK(path.mm3_per_mm == Catch::Approx(0.025));
        }
    }
    CHECK(fragments == 2);
    CHECK(gap_volume == Catch::Approx(a.total_volume() + b.total_volume()));
    CHECK(plan.total_length >= 22.);
    CHECK(plan.total_length <= 30.); // original 22 mm plus at most four 2 mm connectors
    // A zero or insufficient connector budget must still reject these disconnected traces.
    CHECK(preflight_layer(input, nullptr, cfg, &plan, &preferred, SCALED_EPSILON, 0.) == ContinuousPrintVerdict::Reject);
    CHECK(preflight_layer(input, nullptr, cfg, &plan, &preferred, SCALED_EPSILON, scale_(0.1)) == ContinuousPrintVerdict::Reject);
}

TEST_CASE("Curved solid remnants and clipped rings join the main infill", "[ContinuousPrint]") {
    PrintConfig cfg;
    auto surface = make_path({{0, 0}, {20, 0}});
    surface.set_extrusion_role(erInternalInfill);
    ContinuousLayerPlan plan;
    SECTION("A four millimeter remnant is visited in bounded pieces") {
        auto remnant = make_path({{5, 0.2}, {6, 0.4}, {7, 0.2}, {8, 0.4}});
        remnant.set_extrusion_role(erSolidInfill);
        REQUIRE(preflight_layer({&surface, &remnant}, nullptr, cfg, &plan, nullptr,
                               scale_(0.2), scale_(2.)) == ContinuousPrintVerdict::Applicable);
        REQUIRE(plan.entities.size() == 1);
        const auto *joined = dynamic_cast<const ExtrusionMultiPath*>(plan.entities.front().get());
        REQUIRE(joined != nullptr);
        double volume = 0.;
        for (const auto &path : joined->paths)
            if (path.role() == erSolidInfill)
                volume += path.total_volume();
        CHECK(volume == Catch::Approx(remnant.total_volume()));
    }
    SECTION("A clipped solid contour can be entered away from its stored seam") {
        auto ring = make_path({{0, -1}, {21, -1}, {21, 10}, {0, 10}, {0, -0.9}});
        ring.set_extrusion_role(erSolidInfill);
        const Point preferred = surface.first_point();
        REQUIRE(preflight_layer({&surface, &ring}, nullptr, cfg, &plan, &preferred,
                               scale_(0.2), scale_(2.)) == ContinuousPrintVerdict::Applicable);
        CHECK(plan.total_length == Catch::Approx(85.)); // 20 mm fill + 64 mm ring + 1 mm link
        for (size_t i = 1; i < plan.entities.size(); ++ i)
            CHECK(plan.entities[i - 1]->last_point() == plan.entities[i]->first_point());
    }
    SECTION("A short solid hairpin stays a fragment rather than becoming a closed contour") {
        surface.set_extrusion_role(erSolidInfill);
        auto remnant = make_path({{5, 0.2}, {5, 1.5}, {5.1, 1.5}, {5.1, 0.2}});
        remnant.set_extrusion_role(erSolidInfill);
        REQUIRE(preflight_layer({&surface, &remnant}, nullptr, cfg, &plan, nullptr,
                               scale_(0.2), scale_(2.)) == ContinuousPrintVerdict::Applicable);
        REQUIRE(plan.entities.size() == 1);
        CHECK(dynamic_cast<const ExtrusionMultiPath*>(plan.entities.front().get()) != nullptr);
    }
}

TEST_CASE("A reachable surface fragment keeps its direct connection", "[ContinuousPrint]") {
    auto surface = make_path({{0, 0}, {20, 0}});
    surface.set_extrusion_role(erTopSolidInfill);
    auto fragment = make_path({{19, 0.2}, {20, 0.2}});
    fragment.set_extrusion_role(erGapFill);
    PrintConfig cfg;
    ContinuousLayerPlan plan;
    REQUIRE(preflight_layer({&surface, &fragment}, nullptr, cfg, &plan, nullptr,
                           scale_(0.2), scale_(2.)) == ContinuousPrintVerdict::Applicable);
    CHECK(plan.total_length == Catch::Approx(21.2)); // original traces and a single 0.2 mm connector
    CHECK(plan.end_point == fragment.first_point());
}

TEST_CASE("Solid surfaces enter on the current wall and reserve a contour extension", "[ContinuousPrint]") {
    PrintConfig cfg;
    cfg.continuous_print_mode.value = true;
    struct TestLayer : Layer { TestLayer() : Layer(2, nullptr, 0.2, 0.6, 0.5) {} } layer;
    std::vector<std::unique_ptr<ExtrusionEntity>> input;
    for (int i = 0; i < 3; ++ i) {
        const double d = 0.45 * i;
        auto wall = make_loop({{d,d},{20-d,d},{20-d,20-d},{d,20-d}});
        wall.paths.front().set_extrusion_role(i == 0 ? erExternalPerimeter : erPerimeter);
        wall.paths.front().width = 0.45;
        wall.paths.front().height = 0.2;
        input.emplace_back(wall.clone());
    }
    auto fill = make_path({{4,1.05},{4,18},{16,18},{16,1.05}});
    fill.set_extrusion_role(GENERATE(erBottomSurface, erSolidInfill, erTopSolidInfill, erInternalBridgeInfill));
    fill.width = 0.42;
    fill.height = 0.2;
    input.emplace_back(fill.clone());
    const Point preferred = to_point({20,10});
    ContinuousLayerPlan plan;
    REQUIRE(preflight_layer(view_of(input), &layer, cfg, &plan, &preferred, scale_(0.2), scale_(2.)) == ContinuousPrintVerdict::Applicable);
    CHECK(plan.start_point == preferred);
    CHECK(plan.entities.front()->role() == erExternalPerimeter);
    CHECK(is_solid_infill(plan.entities.back()->role()));
    // The presence of sparse fill above must not add a trip back to the outer wall.
    TestLayer upper;
    auto sparse = make_path({{4,2},{16,2}});
    sparse.set_extrusion_role(erInternalInfill);
    upper.add_region(nullptr)->fills.append(sparse);
    layer.upper_layer = &upper;
    ContinuousLayerPlan below_sparse;
    REQUIRE(preflight_layer(view_of(input), &layer, cfg, &below_sparse, &preferred,
                            scale_(0.2), scale_(2.)) == ContinuousPrintVerdict::Applicable);
    CHECK(below_sparse.end_point == plan.end_point);
    CHECK(below_sparse.total_length == plan.total_length);
    CHECK(is_solid_infill(below_sparse.entities.back()->role()));
    const ExtrusionEntity *extension = nullptr;
    bool reached_fill = false;
    for (size_t i = 0; i < plan.entities.size(); ++ i) {
        const auto &entity = plan.entities[i];
        if (i > 0) CHECK(plan.entities[i-1]->last_point() == entity->first_point());
        if (is_solid_infill(entity->role())) reached_fill = true;
        if (entity->role() == erPerimeter && entity->first_point() != entity->last_point() && entity->length() > scale_(2.)) {
            CHECK_FALSE(reached_fill);
            extension = entity.get();
        }
    }
    REQUIRE(extension != nullptr);
    CHECK(extension->as_polyline().points.size() >= 3);
    CHECK(extension->length() > (extension->first_point()-extension->last_point()).cast<double>().norm());
    for (const auto &entity : plan.entities)
        if (is_solid_infill(entity->role())) {
            CHECK(intersection_pl(entity->as_polyline(), offset(extension->as_polyline(), float(scale_(0.434)),
                  ClipperLib::jtRound, scale_(0.0001), ClipperLib::etOpenRound)).empty());
            for (const Point &p : entity->as_polyline().points)
                CHECK(unscale_((p - p.projection_onto(extension->as_polyline())).cast<double>().norm()) >= 0.434);
        }
}

TEST_CASE("Solid contour routing can enter beyond the nearest fill trace", "[ContinuousPrint]") {
    PrintConfig cfg;
    cfg.continuous_print_mode.value = true;
    auto wall = make_loop({{0,0},{20,0},{20,20},{0,20}});
    wall.paths.front().set_extrusion_role(erExternalPerimeter);
    wall.paths.front().width = 0.45;
    wall.paths.front().height = 0.2;
    auto middle = make_path({{4,1.3},{4,18},{16,18},{16,1.3}});
    auto left = make_path({{3,1.3},{3,6},{3.5,6},{3.5,1.3}});
    auto right = make_path({{16.5,1.3},{16.5,6},{17,6},{17,1.3}});
    for (auto *fill : {&middle, &left, &right}) {
        fill->set_extrusion_role(erSolidInfill);
        fill->width = 0.42;
        fill->height = 0.2;
    }
    const Point preferred = to_point({4,0});
    ContinuousLayerPlan plan;
    REQUIRE(preflight_layer({&wall, &middle, &left, &right}, nullptr, cfg, &plan, &preferred,
                            scale_(0.2), scale_(2.)) == ContinuousPrintVerdict::Applicable);
    CHECK(plan.start_point == preferred);
    for (size_t i = 1; i < plan.entities.size(); ++ i)
        CHECK(plan.entities[i-1]->last_point() == plan.entities[i]->first_point());
    for (const auto &point : {to_point({3,6}), to_point({4,18}), to_point({17,6})}) {
        bool retained = false;
        for (const auto &entity : plan.entities)
            if (is_solid_infill(entity->role()))
                retained |= point == point.projection_onto(entity->as_polyline());
        CHECK(retained);
    }
}

TEST_CASE("Sparse layer enters from the actual solid endpoint along its contour", "[ContinuousPrint]") {
    PrintConfig cfg;
    cfg.continuous_print_mode.value = true;
    struct TestLayer : Layer { TestLayer() : Layer(3, nullptr, 0.2, 0.8, 0.7) {} } layer;
    auto outer = make_loop({{118.465,130},{124.232,120.01},{135.768,120.01},{141.535,130},{135.768,139.99},{124.232,139.99}});
    auto middle = make_loop({{118.967,130},{124.483,120.445},{135.517,120.445},{141.033,130},{135.517,139.555},{124.483,139.555}});
    auto inner = make_loop({{119.437,130},{124.718,120.852},{135.282,120.852},{140.563,130},{135.282,139.148},{124.718,139.148}});
    outer.paths.front().set_extrusion_role(erExternalPerimeter);
    for (auto *wall : {&outer, &middle, &inner}) {
        wall->paths.front().width = 0.45;
        wall->paths.front().height = 0.2;
    }
    auto fill = make_path({{121.026,132.54},{127.345,138.858},{131.183,138.858},
                          {120.706,128.382},{122.11,125.948},{135.02,138.858},{135.114,138.858},
                          {136.485,136.485},{123.515,123.515},{124.886,121.142},{124.98,121.142},
                          {137.89,134.052},{139.294,131.618},{128.818,121.142},{132.655,121.142},{138.974,127.46}});
    fill.set_extrusion_role(erInternalInfill);
    fill.width = 0.45;
    fill.height = 0.2;
    const Point previous = to_point({125.549,139.043});
    ContinuousLayerPlan plan;
    REQUIRE(preflight_layer({&outer, &middle, &inner, &fill}, &layer, cfg, &plan, &previous,
                            scale_(0.2), scale_(2.)) == ContinuousPrintVerdict::Applicable);
    CHECK(plan.start_point == previous);
    const ExtrusionEntity *entrance_arc = nullptr;
    for (const auto &entity : plan.entities) {
        if (entity->role() == erInternalInfill) break;
        CHECK(entity->role() == erPerimeter);
        if (entity->length() > scale_(2.)) entrance_arc = entity.get();
    }
    REQUIRE(entrance_arc != nullptr);
    CHECK(entrance_arc->as_polyline().points.size() > 2);
    for (size_t i = 1; i < plan.entities.size(); ++ i)
        CHECK(plan.entities[i-1]->last_point() == plan.entities[i]->first_point());
    for (const auto &entity : plan.entities)
        if (entity->role() == erInternalInfill)
            CHECK(intersection_pl(entity->as_polyline(), offset(entrance_arc->as_polyline(), float(scale_(0.449)),
                  ClipperLib::jtRound, scale_(0.0001), ClipperLib::etOpenRound)).empty());
}

TEST_CASE("Short layer entrance is explicit and does not distort the first extrusion", "[ContinuousPrint]") {
    PrintConfig cfg;
    cfg.continuous_print_mode.value = true;
    struct TestLayer : Layer { TestLayer() : Layer(3, nullptr, 0.2, 0.8, 0.7) {} } layer;
    auto fill = make_path({{0,0},{10,0},{10,10}});
    fill.set_extrusion_role(erInternalInfill);
    const Point previous = to_point({0,0.2});
    ContinuousLayerPlan plan;
    REQUIRE(preflight_layer({&fill}, &layer, cfg, &plan, &previous,
                            scale_(0.2), scale_(2.)) == ContinuousPrintVerdict::Applicable);
    REQUIRE(plan.entities.size() == 2);
    CHECK(plan.start_point == previous);
    CHECK(plan.entities.front()->last_point() == fill.first_point());
    CHECK(plan.entities.back()->as_polyline().points == fill.as_polyline().points);
    CHECK(plan.total_length == Catch::Approx(20.2));
}
