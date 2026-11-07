#include <exception>
#include "../../src/libslic3r/Gpu/TreeContourCandidates.hpp"
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <stdexcept>
#include <tuple>
#include <vector>

struct Point { int64_t px, py; int64_t x() const { return px; } int64_t y() const { return py; } };
struct Contour { std::vector<Point> points; };
struct Line { Point a, b; };
struct Batch { bool dispatched; std::vector<uint8_t> may_intersect; };
using Box = std::tuple<int64_t,int64_t,int64_t,int64_t>;
static void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
int main()
{
    try {
        const std::vector<Contour> contours{
            {{{0,0},{100,0},{100,100},{0,100}}},
            {{{30,30},{30,70},{70,70},{70,30}}},
            {{{40,40},{60,40},{60,60},{40,60}}},
            {}, {{{-10,-20}}}};
        std::vector<Box> boxes;
        const auto emit = [&](auto x0,auto y0,auto x1,auto y1) { boxes.emplace_back(x0,y0,x1,y1); };
        require(Slic3r::Gpu::visit_filled_contour_boxes(true, contours, emit), "whole contours rejected");
        require(boxes == std::vector<Box>{{0,0,100,100},{30,30,70,70},{40,40,60,60},{-10,-20,-10,-20}},
                "whole contour/hole/nested/degenerate boxes differ");
        boxes.clear();
        require(!Slic3r::Gpu::visit_filled_contour_boxes(false, contours, emit), "disabled boxes accepted");
        require(boxes.empty(), "disabled boxes emitted");
        require(Slic3r::Gpu::visit_filled_contour_boxes(true, std::vector<Contour>{}, emit), "empty contours rejected");
        require(boxes.empty(), "empty contours manufactured a box");
        size_t emitted = 0;
        const std::vector<Contour> oversized(128 * 1024 + 1, Contour{{{1,1}}});
        require(!Slic3r::Gpu::visit_filled_contour_boxes(true, oversized, [&](auto,auto,auto,auto) { ++emitted; }), "oversized contour set accepted");
        require(emitted == 128 * 1024, "contour packing exceeded staging cap");
        const std::vector<Line> lines{{{10,10},{20,10}},{{-20,-20},{-10,-10}},{{50,50},{50,50}},{{-10,0},{0,0}}};
        std::map<Box,bool> cache;
        const auto cache_negative = [&](const Point& a,const Point& b) { cache.emplace(Box{a.x(),a.y(),b.x(),b.y()},false); };
        require(Slic3r::Gpu::consume_tree_negatives(Batch{true,{1,0,1,1}},lines,cache_negative), "validated tree batch rejected");
        require(cache.size() == 2 && cache.count(Box{-20,-20,-10,-10}) && cache.count(Box{-10,-10,-20,-20}),
                "negative result was not mapped to both directed cache keys");
        require(!cache.count(Box{10,10,20,10}) && !cache.count(Box{50,50,50,50}) && !cache.count(Box{-10,0,0,0}),
                "contained/degenerate/touching candidates bypassed CPU clipping");
        for (const auto& bad : std::vector<Batch>{{false,{0,0,0,0}},{true,{0}},{true,{0,0,0,2}}}) {
            cache.clear();
            require(!Slic3r::Gpu::consume_tree_negatives(bad,lines,cache_negative), "malformed tree batch accepted");
            require(cache.empty(), "malformed tree batch partially seeded cache");
        }
        std::cout << "tree contour box and cache fixtures passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
