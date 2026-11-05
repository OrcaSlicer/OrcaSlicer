#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/ShortestPath.hpp"
#include "libslic3r/libslic3r.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <numeric>
#include <math.h>
#include <random>
#include <utility>
#include <vector>

using namespace Slic3r;

namespace {

// Infill-like paths: lines of 1 to 30 mm scattered over a 100 mm square. The collection owns them, and entities hands
// them out as the raw pointers chain_and_reorder_extrusion_entities() reorders.
struct Paths
{
    ExtrusionEntityCollection     collection;
    std::vector<ExtrusionEntity*> entities;

    Paths(unsigned seed, size_t count, bool can_reverse)
    {
        std::mt19937                     rng(seed);
        std::uniform_real_distribution<> coordinate(0., 100.), length(1., 30.), angle(0., 2. * M_PI);
        Polylines lines;
        for (size_t i = 0; i < count; ++ i) {
            // One draw per statement, as the evaluation order of function arguments is unspecified.
            const double x = coordinate(rng);
            const double y = coordinate(rng);
            const double l = length(rng);
            const double t = angle(rng);
            const Vec2d  a(x, y);
            const Vec2d  b = a + l * Vec2d(std::cos(t), std::sin(t));
            lines.emplace_back(Point::new_scale(a), Point::new_scale(b));
        }
        extrusion_entities_append_paths(collection.entities, std::move(lines), erInternalInfill, 0.1, 0.45f, 0.2f, can_reverse);
        entities = collection.entities;
    }
};

double travel(const std::vector<ExtrusionEntity*> &entities, const Point &start)
{
    double length = 0.;
    Point  last   = start;
    for (const ExtrusionEntity *ee : entities) {
        length += (ee->first_point() - last).cast<double>().norm();
        last = ee->last_point();
    }
    return length;
}

// Travel of the chain that always goes on to the nearest free path end.
double nearest_next_travel(const std::vector<ExtrusionEntity*> &entities, bool can_reverse, const Point &from)
{
    std::vector<char> taken(entities.size(), 0);
    double length = 0.;
    Point  last   = from;
    for (size_t step = 0; step < entities.size(); ++ step) {
        double best          = std::numeric_limits<double>::max();
        size_t best_idx      = 0;
        bool   best_reversed = false;
        for (size_t i = 0; i < entities.size(); ++ i)
            for (const bool reversed : { false, true })
                if (! taken[i] && (can_reverse || ! reversed)) {
                    const double d = ((reversed ? entities[i]->last_point() : entities[i]->first_point()) - last).cast<double>().norm();
                    if (d < best) {
                        best          = d;
                        best_idx      = i;
                        best_reversed = reversed;
                    }
                }
        taken[best_idx] = 1;
        length += best;
        last = best_reversed ? entities[best_idx]->first_point() : entities[best_idx]->last_point();
    }
    return length;
}

// The shortest travel over every order and allowed reversal of the paths, from start.
double shortest_travel(const std::vector<ExtrusionEntity*> &entities, bool can_reverse, const Point &from)
{
    std::vector<size_t> order(entities.size());
    std::iota(order.begin(), order.end(), 0);
    double best = std::numeric_limits<double>::max();
    do {
        for (size_t mask = 0; mask < (size_t(1) << order.size()) && (can_reverse || mask == 0); ++ mask) {
            double length = 0.;
            Point  last   = from;
            for (size_t k = 0; k < order.size(); ++ k) {
                const ExtrusionEntity *ee       = entities[order[k]];
                const bool             reversed = (mask >> k & 1) != 0;
                length += ((reversed ? ee->last_point() : ee->first_point()) - last).cast<double>().norm();
                last = reversed ? ee->first_point() : ee->last_point();
            }
            best = std::min(best, length);
        }
    } while (std::next_permutation(order.begin(), order.end()));
    return best;
}

const Point start = Point::new_scale(-10., -10.);

} // namespace

TEST_CASE("An improved infill chain is never longer than the greedy or nearest-next one and keeps every path", "[ShortestPath]")
{
    const size_t count       = GENERATE(2, 3, 10, 60, 300);
    const bool   can_reverse = GENERATE(true, false);
    double greedy_total = 0., improved_total = 0.;
    for (unsigned seed = 1; seed <= 20; ++ seed) {
        Paths greedy(seed, count, can_reverse), improved(seed, count, can_reverse);
        const double nearest_travel = nearest_next_travel(improved.entities, can_reverse, start);
        chain_and_reorder_extrusion_entities(greedy.entities, &start);
        chain_and_reorder_extrusion_entities(improved.entities, start);

        std::vector<ExtrusionEntity*> sorted = improved.entities;
        std::sort(sorted.begin(), sorted.end());
        REQUIRE(std::unique(sorted.begin(), sorted.end()) == sorted.end());
        REQUIRE(sorted.size() == count);

        const double greedy_travel = travel(greedy.entities, start), improved_travel = travel(improved.entities, start);
        CHECK(improved_travel <= greedy_travel + SCALED_EPSILON);
        CHECK(improved_travel <= nearest_travel + SCALED_EPSILON);
        greedy_total += greedy_travel;
        improved_total += improved_travel;
    }
    // Twenty random sets of ten or more paths always leave the greedy chain something to shorten.
    if (count >= 10)
        CHECK(improved_total < 0.95 * greedy_total);
}

TEST_CASE("Improving an infill chain keeps the direction of paths that cannot reverse", "[ShortestPath]")
{
    Paths paths(7, 100, false);
    std::map<const ExtrusionEntity*, Point> first_points;
    for (const ExtrusionEntity *ee : paths.entities)
        first_points.emplace(ee, ee->first_point());

    chain_and_reorder_extrusion_entities(paths.entities, start);

    for (const ExtrusionEntity *ee : paths.entities)
        CHECK(ee->first_point() == first_points.at(ee));
}

TEST_CASE("Chains of a few paths are the shortest possible", "[ShortestPath]")
{
    const size_t count       = GENERATE(2, 3, 4, 5, 6);
    const bool   can_reverse = GENERATE(true, false);
    for (unsigned seed = 1; seed <= 10; ++ seed) {
        Paths        paths(seed, count, can_reverse);
        const double shortest = shortest_travel(paths.entities, can_reverse, start);
        chain_and_reorder_extrusion_entities(paths.entities, start);
        CHECK(travel(paths.entities, start) <= shortest + SCALED_EPSILON);
    }
}

TEST_CASE("The greedy chain of two to four paths is the shortest possible", "[ShortestPath]")
{
    const size_t count       = GENERATE(2, 3, 4);
    const bool   can_reverse = GENERATE(true, false);
    for (unsigned seed = 1; seed <= 10; ++ seed) {
        Paths        paths(seed, count, can_reverse);
        const double shortest = shortest_travel(paths.entities, can_reverse, start);
        chain_and_reorder_extrusion_entities(paths.entities, &start);
        CHECK(travel(paths.entities, start) <= shortest + SCALED_EPSILON);
    }
}

TEST_CASE("A short chain need not start with the path nearest to its start", "[ShortestPath]")
{
    // The nearest end leads 10 mm away from the two other paths, so the shortest chain leaves that path for last.
    const Point from = Point::new_scale(0., 0.);
    for (const bool greedy : { false, true }) {
        ExtrusionEntityCollection collection;
        extrusion_entities_append_paths(collection.entities,
                                        Polylines{ Polyline(Point::new_scale(-0.9, 0.), Point::new_scale(-10., 0.)), Polyline(Point::new_scale(1., 0.), Point::new_scale(2., 0.)),
                                                   Polyline(Point::new_scale(3., 1.), Point::new_scale(4., 1.)) },
                                        erInternalInfill, 0.1, 0.45f, 0.2f);
        std::vector<ExtrusionEntity*> entities = collection.entities;
        const double                  shortest = shortest_travel(entities, true, from);
        if (greedy)
            chain_and_reorder_extrusion_entities(entities, &from);
        else
            chain_and_reorder_extrusion_entities(entities, from);
        CHECK(entities.back() == collection.entities.front());
        CHECK_THAT(travel(entities, from), Catch::Matchers::WithinAbs(shortest, SCALED_EPSILON));
    }
}
