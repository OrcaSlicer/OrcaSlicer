#pragma once

#include "libslic3r/Color.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "../ModelGeneration/ModelPreviewPalette.hpp"

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/boykov_kolmogorov_max_flow.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace Slic3r::AI::SurfaceSelectionRefinement {

inline constexpr size_t maximum_roi_faces = 150000;

struct Result {
    std::vector<size_t> faces;
    bool canceled {false};
    bool limit_exceeded {false};
    std::string error;
};

namespace detail {
using Color = GUI::PreviewPalette::Color;
using Capacity = int64_t;
struct Canceled {};
struct Control {
    const std::function<bool()>& canceled;
    size_t operations {0};
    void check() const { if (canceled && canceled()) throw Canceled {}; }
    void tick() { if ((++operations & 1023) == 0) check(); }
};

// Boost's iterative BK solver has no cancellation visitor. A checked residual
// map supplies cooperative cancellation during the solve itself, not just
// before/after a potentially expensive max-flow operation.
template<class Map> struct CheckedMap {
    using key_type = typename boost::property_traits<Map>::key_type;
    using value_type = typename boost::property_traits<Map>::value_type;
    using reference = typename boost::property_traits<Map>::reference;
    using category = boost::read_write_property_map_tag;
    Map map;
    Control* control;
};
template<class Map>
typename CheckedMap<Map>::value_type get(const CheckedMap<Map>& map, const typename CheckedMap<Map>::key_type& key)
{
    map.control->tick();
    return boost::get(map.map, key);
}
template<class Map>
void put(const CheckedMap<Map>& map, const typename CheckedMap<Map>::key_type& key,
         const typename CheckedMap<Map>::value_type& value)
{
    map.control->tick();
    boost::put(map.map, key, value);
}

inline double distance(const Color& a, const Color& b)
{
    // Oklab coordinates are unlit color measurements, not rendered pixels.
    // Retain lightness information, but reduce baked-shadow sensitivity.
    const double l = (a[0] - b[0]) * 0.65, u = a[1] - b[1], v = a[2] - b[2];
    return l*l + u*u + v*v;
}

// Bounded seed appearance model. Multiple colors in a user's positive stroke
// remain multiple exemplars instead of collapsing into one average color.
struct SeedColors {
    std::array<Color, 64> sums {};
    std::array<size_t, 64> counts {};
    std::vector<Color> centers;
    void add(const Color& rgb, const Color& lab) {
        const auto bin = [](float value) { return std::min(3, int(value * 4)); };
        const size_t index = size_t(bin(rgb[0]) * 16 + bin(rgb[1]) * 4 + bin(rgb[2]));
        for (size_t c = 0; c < 3; ++c) sums[index][c] += lab[c];
        ++counts[index];
    }
    void finish() {
        for (size_t i = 0; i < counts.size(); ++i) if (counts[i]) {
            for (auto& value : sums[i]) value /= float(counts[i]);
            centers.push_back(sums[i]);
        }
    }
    Capacity cost(const Color& lab) const {
        double nearest = std::numeric_limits<double>::max();
        for (const auto& center : centers) nearest = std::min(nearest, distance(center, lab));
        return Capacity(std::llround(1000.0 * (1.0 - std::exp(-nearest / 0.0064))));
    }
};
struct EdgeOwners {
    size_t first, second {0}, count {1};
};
} // namespace detail

// This is an explicit, local seeded selection, not semantic segmentation.
// The immutable original colors and mesh are never modified. Both positive
// and protected seeds must occur inside the coarse ROI; a caller must retain
// that ROI even when displaying the protected faces as unselected.
// Shared-index manifold edges define connectivity; unwelded seams remain
// boundaries. Any error/cancellation returns no faces; preserve the old UI mask.
inline Result refine(const indexed_triangle_set& mesh, const std::vector<RGBA>& vertex_colors,
                     const std::vector<size_t>& roi_faces, const std::vector<size_t>& foreground_faces,
                     const std::vector<size_t>& protected_faces, const std::function<bool()>& canceled = {})
{
    Result result;
    detail::Control control {canceled};
    try {
        control.check();
        if (vertex_colors.size() != mesh.vertices.size()) {
            result.error = "Refinement requires original vertex colors.";
            return result;
        }
        std::unordered_map<size_t, size_t> local;
        std::vector<size_t> roi;
        for (size_t face : roi_faces) {
            control.tick();
            if (face >= mesh.indices.size()) { result.error = "The selection refers to a different mesh."; return result; }
            if (local.emplace(face, roi.size()).second) roi.push_back(face);
            if (roi.size() > maximum_roi_faces) {
                result.limit_exceeded = true;
                result.error = "Select a smaller region (at most 150000 faces).";
                return result;
            }
        }
        if (roi.empty()) { result.error = "Select a region before refining its boundary."; return result; }
        const size_t count = roi.size();
        std::vector<uint8_t> positive(count, 0), negative(count, 0);
        for (size_t face : foreground_faces) {
            control.tick();
            const auto found = local.find(face);
            if (found != local.end()) positive[found->second] = 1;
        }
        for (size_t face : protected_faces) {
            control.tick();
            const auto found = local.find(face);
            if (found != local.end()) negative[found->second] = 1;
        }
        if (std::find(positive.begin(), positive.end(), uint8_t(1)) == positive.end() ||
            std::find(negative.begin(), negative.end(), uint8_t(1)) == negative.end()) {
            result.error = "Mark both what to select and what to protect inside the region.";
            return result;
        }
        for (size_t i = 0; i < count; ++i) if (positive[i] && negative[i]) {
            result.error = "A face cannot be both selected and protected.";
            return result;
        }

        std::vector<detail::Color> labs(count);
        std::vector<Vec3d> normals(count);
        std::unordered_map<uint64_t, detail::EdgeOwners> edges;
        edges.reserve(count * 2);
        detail::SeedColors foreground_colors, background_colors;
        for (size_t i = 0; i < count; ++i) {
            control.tick();
            const auto& triangle = mesh.indices[roi[i]];
            detail::Color rgb {};
            for (int c = 0; c < 3; ++c) {
                const int vertex = triangle[c];
                if (vertex < 0 || size_t(vertex) >= mesh.vertices.size() || !mesh.vertices[vertex].allFinite()) {
                    result.error = "The selected region has invalid geometry.";
                    return result;
                }
                for (size_t channel = 0; channel < 3; ++channel) {
                    const float value = vertex_colors[vertex][channel];
                    if (!std::isfinite(value) || value < 0.0f || value > 1.0f) {
                        result.error = "The selected region has invalid vertex colors.";
                        return result;
                    }
                    rgb[channel] += value / 3.0f;
                }
            }
            labs[i] = GUI::PreviewPalette::to_lab(rgb);
            const Vec3d a = mesh.vertices[triangle[0]].cast<double>();
            const Vec3d b = mesh.vertices[triangle[1]].cast<double>();
            const Vec3d c = mesh.vertices[triangle[2]].cast<double>();
            normals[i] = (b - a).cross(c - a);
            const double length = normals[i].norm();
            if (length > 0.0) normals[i] /= length;
            if (positive[i]) foreground_colors.add(rgb, labs[i]);
            if (negative[i]) background_colors.add(rgb, labs[i]);
            for (int edge = 0; edge < 3; ++edge) {
                const uint32_t first = uint32_t(triangle[edge]), second = uint32_t(triangle[(edge + 1) % 3]);
                if (first == second) continue;
                const uint64_t key = (uint64_t(std::min(first, second)) << 32) | std::max(first, second);
                auto inserted = edges.emplace(key, detail::EdgeOwners {i});
                if (!inserted.second) {
                    inserted.first->second.second = i;
                    ++inserted.first->second.count;
                }
            }
        }
        foreground_colors.finish(); background_colors.finish();
        std::vector<std::vector<size_t>> neighbors(count);
        for (const auto& edge : edges) {
            control.tick();
            // Ambiguous non-manifold edges must not bridge unrelated sheets.
            const auto& owners = edge.second;
            if (owners.count == 2 && owners.first != owners.second) {
                neighbors[owners.first].push_back(owners.second);
                neighbors[owners.second].push_back(owners.first);
            }
        }
        std::vector<uint8_t> reachable(count, 0);
        std::vector<size_t> queue;
        for (size_t i = 0; i < count; ++i) if (positive[i]) { reachable[i] = 1; queue.push_back(i); }
        for (size_t at = 0; at < queue.size(); ++at) {
            control.tick();
            for (size_t next : neighbors[queue[at]]) if (!reachable[next] && !negative[next]) {
                reachable[next] = 1;
                queue.push_back(next);
            }
        }

        using Traits = boost::adjacency_list_traits<boost::vecS, boost::vecS, boost::directedS>;
        using EdgeProperty = boost::property<boost::edge_capacity_t, detail::Capacity,
            boost::property<boost::edge_residual_capacity_t, detail::Capacity,
            boost::property<boost::edge_reverse_t, Traits::edge_descriptor>>>;
        using Graph = boost::adjacency_list<boost::vecS, boost::vecS, boost::directedS, boost::no_property, EdgeProperty>;
        Graph graph(count + 2);
        const size_t source = count, sink = count + 1;
        auto capacity = boost::get(boost::edge_capacity, graph);
        auto residual = boost::get(boost::edge_residual_capacity, graph);
        auto reverse = boost::get(boost::edge_reverse, graph);
        auto connect = [&](size_t from, size_t to, detail::Capacity forward_capacity, detail::Capacity backward_capacity) {
            const auto forward = boost::add_edge(from, to, graph).first;
            const auto backward = boost::add_edge(to, from, graph).first;
            boost::put(capacity, forward, forward_capacity); boost::put(capacity, backward, backward_capacity);
            boost::put(reverse, forward, backward); boost::put(reverse, backward, forward);
        };
        // Greater than the sum of every possible soft edge/unary in this ROI.
        constexpr detail::Capacity hard = 1000000000000LL;
        for (size_t i = 0; i < count; ++i) {
            control.tick();
            const bool forced_background = negative[i] || !reachable[i];
            const auto fg_cost = positive[i] ? 0 : forced_background ? hard : foreground_colors.cost(labs[i]);
            const auto bg_cost = positive[i] ? hard : forced_background ? 0 : background_colors.cost(labs[i]);
            connect(source, i, bg_cost, 0);
            connect(i, sink, fg_cost, 0);
            for (size_t j : neighbors[i]) if (j > i) {
                const double normal = std::max(0.0, normals[i].dot(normals[j]));
                const double similarity = std::exp(-detail::distance(labs[i], labs[j]) / 0.0064);
                const auto weight = detail::Capacity(1 + std::llround(800.0 * similarity * (0.15 + 0.85 * normal * normal)));
                connect(i, j, weight, weight);
            }
        }
        control.check();
        detail::CheckedMap<decltype(residual)> checked_residual {residual, &control};
        boost::boykov_kolmogorov_max_flow(graph, capacity, checked_residual, reverse,
                                        boost::get(boost::vertex_index, graph), source, sink);
        control.check();
        // Derive the source side from residual reachability rather than relying
        // on BK's internal tree color conventions or a recursive traversal.
        std::vector<uint8_t> source_side(count + 2, 0);
        source_side[source] = 1;
        queue.assign(1, source);
        for (size_t at = 0; at < queue.size(); ++at) {
            control.tick();
            const auto outgoing = boost::out_edges(queue[at], graph);
            for (auto edge = outgoing.first; edge != outgoing.second; ++edge) {
                const size_t next = boost::target(*edge, graph);
                if (!source_side[next] && boost::get(residual, *edge) > 0) {
                    source_side[next] = 1;
                    queue.push_back(next);
                }
            }
        }
        for (size_t i = 0; i < count; ++i) {
            control.tick();
            if (source_side[i] && reachable[i] && !negative[i]) result.faces.push_back(roi[i]);
        }
        std::sort(result.faces.begin(), result.faces.end());
        control.check();
    } catch (const detail::Canceled&) {
        result.faces.clear();
        result.canceled = true;
    }
    return result;
}
} // namespace Slic3r::AI::SurfaceSelectionRefinement
