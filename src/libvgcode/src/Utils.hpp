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

} // namespace libvgcode

#endif // VGCODE_UTILS_HPP