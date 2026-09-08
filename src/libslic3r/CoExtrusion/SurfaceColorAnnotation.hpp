#pragma once

#include "CoExtrusionTypes.hpp"
#include "../ObjectID.hpp"

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

class ModelObject;
class ModelVolume;

namespace UndoRedo {
class StackImpl;
}

namespace CoExtrusion {

// Stores one stable surface-color id per original mesh triangle. This is
// intentionally independent from FacetsAnnotation: MMU painting describes
// extruder assignment and may split triangles, while co-extrusion color is a
// semantic label consumed by surface-orientation planning.
class SurfaceColorAnnotation final : public ObjectWithTimestamp
{
public:
    static constexpr SurfaceColorId UNASSIGNED = std::numeric_limits<SurfaceColorId>::max();

    void assign(const SurfaceColorAnnotation &rhs);
    void assign(SurfaceColorAnnotation &&rhs);

    bool empty() const { return m_color_ids.empty(); }
    size_t size() const { return m_color_ids.size(); }
    const std::vector<SurfaceColorId>& data() const { return m_color_ids; }

    std::optional<SurfaceColorId> triangle_color(size_t triangle_id) const;
    bool set_triangle_color(size_t triangle_id, SurfaceColorId color_id);
    bool clear_triangle_color(size_t triangle_id);
    bool set_triangle_colors(std::vector<SurfaceColorId> color_ids)
    {
        while (!color_ids.empty() && color_ids.back() == UNASSIGNED)
            color_ids.pop_back();
        if (m_color_ids == color_ids)
            return false;
        m_color_ids = std::move(color_ids);
        touch();
        return true;
    }
    void reset();

    void reserve(size_t triangle_count) { m_color_ids.reserve(triangle_count); }
    void shrink_to_fit();

    // Per-triangle representation used by 3MF. An empty string means that the
    // triangle has no co-extrusion surface-color annotation.
    std::string get_triangle_as_string(size_t triangle_id) const;
    bool set_triangle_from_string(size_t triangle_id, const std::string &value);

    bool equals(const SurfaceColorAnnotation &other) const { return m_color_ids == other.m_color_ids; }

private:
    SurfaceColorAnnotation() = default;
    explicit SurfaceColorAnnotation(int) : ObjectWithTimestamp(-1) {}
    SurfaceColorAnnotation(const SurfaceColorAnnotation&) = default;
    SurfaceColorAnnotation(SurfaceColorAnnotation&&) = default;
    SurfaceColorAnnotation& operator=(const SurfaceColorAnnotation&) = default;
    SurfaceColorAnnotation& operator=(SurfaceColorAnnotation&&) = default;

    void trim_unassigned_tail();

    friend class cereal::access;
    friend class Slic3r::ModelVolume;
    friend class Slic3r::UndoRedo::StackImpl;

    template<class Archive> void serialize(Archive &ar)
    {
        ar(cereal::base_class<ObjectWithTimestamp>(this), m_color_ids);
    }

    std::vector<SurfaceColorId> m_color_ids;
};

// Resolves the model-level fallback used for triangles without an explicit
// SurfaceColorAnnotation. A volume override wins over its parent object.
std::optional<SurfaceColorId> configured_surface_color_id(
    const ModelObject &object,
    const ModelVolume &volume);

} // namespace CoExtrusion
} // namespace Slic3r
