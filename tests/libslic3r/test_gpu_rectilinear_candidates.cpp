#include <cstddef>
#include <utility>
#include <algorithm>
#include <exception>
#include "../../src/libslic3r/Gpu/VerticalIntersectionCandidates.hpp"
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <vector>

struct Point { int64_t px, py; int64_t x() const { return px; } int64_t y() const { return py; } };
struct Contour { std::vector<Point> points; };
struct Region {
    std::vector<Contour> contours;
    size_t n_contours;
    mutable size_t reads = 0;
    const Contour& contour(size_t i) const { ++reads; return contours.at(i); }
};
using Record = std::tuple<int64_t, int64_t, int64_t, int64_t, int64_t>;
static void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
int main()
{
    try {
        // Literal expected order catches edge reversal, reordering, and endpoint submission.
        Region region{{{{{-10, -10}, {10, 10}, {10, -10}}}}, 1};
        std::vector<Record> records;
        auto emit = [&](const auto& a, const auto& b, int64_t x) { records.emplace_back(a.x(), a.y(), b.x(), b.y(), x); };
        require(Slic3r::Gpu::visit_rectilinear_candidates(true, region, 5, int64_t(-10), int64_t(5), emit), "ordinary region rejected");
        const std::vector<Record> expected{
            {10,-10,-10,-10,-5}, {10,-10,-10,-10,0}, {10,-10,-10,-10,5},
            {-10,-10,10,10,-5}, {-10,-10,10,10,0}, {-10,-10,10,10,5}};
        require(records == expected, "strict-interior candidates differ from CPU edge/scanline order");
        Region disabled{{}, 99};
        require(!Slic3r::Gpu::visit_rectilinear_candidates(false, disabled, 5, int64_t(0), int64_t(1), emit), "disabled traversal accepted");
        require(disabled.reads == 0, "disabled traversal accessed contours");
        records.clear();
        Region hole{{{{{-10,-10}, {10,10}}}, {{{-5,5}, {5,-5}}}, {{{0,0}}}}, 3};
        require(Slic3r::Gpu::visit_rectilinear_candidates(true, hole, 5, int64_t(-10), int64_t(5), emit), "hole region rejected");
        require(records == std::vector<Record>{{10,10,-10,-10,-5},{10,10,-10,-10,0},{10,10,-10,-10,5},
                    {-10,-10,10,10,-5},{-10,-10,10,10,0},{-10,-10,10,10,5},
                    {5,-5,-5,5,0},{-5,5,5,-5,0}}, "hole/degenerate contour order differs");
        require(!Slic3r::Gpu::visit_rectilinear_candidates(true, region, 5, int64_t(0), int64_t(0), emit), "zero spacing accepted");
        require(!Slic3r::Gpu::visit_rectilinear_candidates(true, region, 3, std::numeric_limits<int64_t>::max()-1, int64_t(2), emit), "scanline overflow accepted");
        Region huge{{{{{0,0}, {300000,0}}}}, 1};
        require(!Slic3r::Gpu::visit_rectilinear_candidates(true, huge, 300000, int64_t(0), int64_t(1), emit), "oversized request set accepted");
        // Independent exhaustive scanline membership verifies CPU equivalence
        // without duplicating the visitor's quotient/range algorithm.
        for (int ax = -17; ax <= 17; ax += 2) {
            for (int bx = -16; bx <= 16; bx += 3) {
                for (int origin = -19; origin <= 7; origin += 5) {
                    Region sample{{{{{ax,-13}, {bx,11}}}}, 1};
                    records.clear();
                    require(Slic3r::Gpu::visit_rectilinear_candidates(true, sample, 13, int64_t(origin), int64_t(3), emit), "small sample rejected");
                    std::vector<Record> brute_force;
                    for (const auto& edge : std::vector<std::pair<Point,Point>>{{{bx,11},{ax,-13}},{{ax,-13},{bx,11}}})
                        for (int64_t x = origin; x <= origin + 36; x += 3)
                            if (std::min(edge.first.x(),edge.second.x()) < x && x < std::max(edge.first.x(),edge.second.x()))
                                brute_force.emplace_back(edge.first.x(),edge.first.y(),edge.second.x(),edge.second.y(),x);
                    require(records == brute_force, "scanline membership/order differs from exhaustive CPU reference");
                }
            }
        }
        std::cout << "rectilinear candidate fixtures passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
