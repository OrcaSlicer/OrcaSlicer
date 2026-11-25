///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#ifndef VGCODE_UTILS_HPP
#define VGCODE_UTILS_HPP

#include "../include/PathVertex.hpp"
#include "../include/Types.hpp"
#include <vector>
#include <cstdint>
#include <cstddef>
#include <optional>
#include <utility>
#include <algorithm>

#ifdef _WIN32
#define STDVEC_MEMSIZE(NAME, TYPE) NAME.capacity() * ((sizeof(TYPE) + __alignof(TYPE) - 1) / __alignof(TYPE)) * __alignof(TYPE)
#else
#define STDVEC_MEMSIZE(NAME, TYPE) NAME.capacity() * ((sizeof(TYPE) + alignof(TYPE) - 1) / alignof(TYPE)) * alignof(TYPE)
#endif // _WIN32

namespace libvgcode {

extern void add_vertex(const Vec3& position, const Vec3& normal, std::vector<float>& vertices);
extern void add_triangle(uint16_t v1, uint16_t v2, uint16_t v3, std::vector<uint16_t>& indices);
extern Vec3 normalize(const Vec3& v);
extern float dot(const Vec3& v1, const Vec3& v2);
extern float length(const Vec3& v);
extern bool operator == (const Vec3& v1, const Vec3& v2);
extern bool operator != (const Vec3& v1, const Vec3& v2);
extern Vec3 operator + (const Vec3& v1, const Vec3& v2);
extern Vec3 operator - (const Vec3& v1, const Vec3& v2);
extern Vec3 operator * (float f, const Vec3& v);
// The index of the weighted median of the samples (value, weight): in value order, the first
// sample at which the cumulative weight reaches half of the total. Samples that weigh nothing
// between them fall back to the middle one by value; no samples gives their count.
extern std::size_t weighted_median_index(const std::vector<std::pair<float, float>>& samples);
// Whether the reduced set the mode builds keeps the vertex, one the full set shows: all of it on the
// layers at the ends of the range, and between them what the mode keeps of the rest.
extern bool keep_in_reduced_set(EReducedDetailMode mode, bool hide_infill, const PathVertex& v, bool end_layer);
// The extrusions of the role on the layer among the vertices of the range that end inside the
// box: their ids, and a (value, length) sample of each from what the view grades by. Where the
// view grades by nothing, value_of() has no value and the first id found is returned at once.
template<typename ValueOf>
std::optional<std::size_t> collect_role_samples(const std::vector<PathVertex>& vertices, const Interval& range, uint32_t layer_id,
    EGCodeExtrusionRole role, const XYBox& box, const ValueOf& value_of, std::vector<std::pair<float, float>>& samples,
    std::vector<std::size_t>& ids)
{
    for (std::size_t i = range[0]; i <= range[1] && i < vertices.size(); ++i) {
        const PathVertex& v = vertices[i];
        if (v.layer_id != layer_id || !v.is_extrusion() || v.role != role || !box.contains(v.position))
            continue;
        const std::optional<float> value = value_of(v);
        if (!value.has_value())
            return i;
        samples.emplace_back(*value, (i > 0) ? length(v.position - vertices[i - 1].position) : 0.0f);
        ids.push_back(i);
    }
    return std::nullopt;
}
// Up to max_count of the interval's layers that pass has_role(), spread evenly over them from the
// first such layer to the last; the caller bounds the interval by the layer count. A role printed
// on a few layers only, as top surfaces and bridges are, is so sampled where it was printed rather
// than missed between samples.
template<typename HasRole>
std::vector<std::size_t> pick_sample_layers(const Interval& layers, std::size_t max_count, const HasRole& has_role)
{
    std::vector<std::size_t> with_role;
    for (std::size_t id = layers[0]; id <= layers[1]; ++id) {
        if (has_role(id))
            with_role.push_back(id);
    }
    const std::size_t count = with_role.size();
    const std::size_t steps = std::min(count, max_count);
    std::vector<std::size_t> picked;
    picked.reserve(steps);
    for (std::size_t step = 0; step < steps; ++step)
        picked.push_back(with_role[(steps > 1) ? step * (count - 1) / (steps - 1) : 0]);
    return picked;
}
// Samples the role over the interval's layers through sample_layer(), which returns an id to stop
// at, as the first extrusion of a view that grades by nothing, and collects samples otherwise:
// the layers pick_sample_layers() picks, then, while has_samples() is false, the interval's other
// layers with the role in order, since on a plate of several objects the layers another object
// has the role on can take every pick. The walk stops at the first layer that yields samples, so
// its median comes from that layer alone.
template<typename HasRole, typename SampleLayer, typename HasSamples>
std::optional<std::size_t> sample_role_layers(const Interval& layers, std::size_t max_count, const HasRole& has_role,
    const SampleLayer& sample_layer, const HasSamples& has_samples)
{
    const std::vector<std::size_t> picked = pick_sample_layers(layers, max_count, has_role);
    for (const std::size_t layer_id : picked) {
        if (const std::optional<std::size_t> first = sample_layer(layer_id); first.has_value())
            return first;
    }
    for (std::size_t layer_id = layers[0]; !has_samples() && layer_id <= layers[1]; ++layer_id) {
        if (!has_role(layer_id) || std::binary_search(picked.begin(), picked.end(), layer_id))
            continue;
        if (const std::optional<std::size_t> first = sample_layer(layer_id); first.has_value())
            return first;
    }
    return std::nullopt;
}

} // namespace libvgcode

#endif // VGCODE_UTILS_HPP