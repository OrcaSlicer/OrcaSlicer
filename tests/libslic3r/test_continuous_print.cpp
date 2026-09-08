#include <catch2/catch_all.hpp>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode/ContinuousPrint.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PrintConfig.hpp"
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
