#include <catch2/catch_all.hpp>

#include <algorithm>

#include "libslic3r/MinimumSpanningTree.hpp"
#include "libslic3r/Point.hpp"

using namespace Slic3r;

// A 5x5 lattice: at every step of Prim's algorithm several candidates sit at the same
// distance from the tree, so the tie-break decides the tree's shape.
static std::vector<Point> lattice()
{
    std::vector<Point> vertices;
    for (int y = 0; y < 5; ++y)
        for (int x = 0; x < 5; ++x)
            vertices.emplace_back(Point::new_scale(x, y));
    return vertices;
}

static std::vector<Point> sorted_neighbours(const MinimumSpanningTree &mst, const Point &vertex)
{
    std::vector<Point> neighbours = mst.adjacent_nodes(vertex);
    std::sort(neighbours.begin(), neighbours.end());
    return neighbours;
}

TEST_CASE("Minimum spanning tree connects every vertex", "[MinimumSpanningTree]")
{
    const std::vector<Point> vertices = lattice();
    const MinimumSpanningTree mst(vertices);

    REQUIRE(mst.vertices().size() == vertices.size());
    size_t adjacency_entries = 0;
    for (const Point &vertex : vertices) {
        const std::vector<Point> neighbours = mst.adjacent_nodes(vertex);
        REQUIRE(! neighbours.empty());
        adjacency_entries += neighbours.size();
    }
    // A tree on n vertices has n - 1 edges, each listed from both ends.
    REQUIRE(adjacency_entries == 2 * (vertices.size() - 1));
}

TEST_CASE("Minimum spanning tree does not depend on the order of the non-root vertices", "[MinimumSpanningTree][Regression]")
{
    const std::vector<Point> vertices = lattice();
    const MinimumSpanningTree reference(vertices);

    // The root stays first: Prim's tree legitimately depends on where it starts.
    // Every other order of the remaining vertices must give the same tree.
    std::vector<std::vector<Point>> orders;
    orders.emplace_back(vertices);
    std::reverse(orders.back().begin() + 1, orders.back().end());
    for (size_t shift = 1; shift + 1 < vertices.size(); ++shift) {
        orders.emplace_back(vertices);
        std::rotate(orders.back().begin() + 1, orders.back().begin() + 1 + shift, orders.back().end());
    }

    for (const std::vector<Point> &order : orders) {
        const MinimumSpanningTree mst(order);
        for (const Point &vertex : vertices) {
            INFO("vertex " << vertex.x() << "," << vertex.y());
            REQUIRE(sorted_neighbours(mst, vertex) == sorted_neighbours(reference, vertex));
        }
    }
}
