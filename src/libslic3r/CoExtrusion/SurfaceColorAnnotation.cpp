#include "SurfaceColorAnnotation.hpp"

#include "../Model.hpp"

#include <charconv>
#include <system_error>
#include <utility>

namespace Slic3r::CoExtrusion {

void SurfaceColorAnnotation::assign(const SurfaceColorAnnotation &rhs)
{
    if (!timestamp_matches(rhs)) {
        m_color_ids = rhs.m_color_ids;
        copy_timestamp(rhs);
    }
}

void SurfaceColorAnnotation::assign(SurfaceColorAnnotation &&rhs)
{
    if (!timestamp_matches(rhs)) {
        m_color_ids = std::move(rhs.m_color_ids);
        copy_timestamp(rhs);
    }
}

std::optional<SurfaceColorId> SurfaceColorAnnotation::triangle_color(size_t triangle_id) const
{
    if (triangle_id >= m_color_ids.size() || m_color_ids[triangle_id] == UNASSIGNED)
        return std::nullopt;
    return m_color_ids[triangle_id];
}

bool SurfaceColorAnnotation::set_triangle_color(size_t triangle_id, SurfaceColorId color_id)
{
    if (color_id == UNASSIGNED)
        return clear_triangle_color(triangle_id);

    if (triangle_id >= m_color_ids.size())
        m_color_ids.resize(triangle_id + 1, UNASSIGNED);
    if (m_color_ids[triangle_id] == color_id)
        return false;

    m_color_ids[triangle_id] = color_id;
    touch();
    return true;
}

bool SurfaceColorAnnotation::clear_triangle_color(size_t triangle_id)
{
    if (triangle_id >= m_color_ids.size() || m_color_ids[triangle_id] == UNASSIGNED)
        return false;

    m_color_ids[triangle_id] = UNASSIGNED;
    trim_unassigned_tail();
    touch();
    return true;
}

void SurfaceColorAnnotation::reset()
{
    if (!m_color_ids.empty()) {
        m_color_ids.clear();
        touch();
    }
}

void SurfaceColorAnnotation::shrink_to_fit()
{
    trim_unassigned_tail();
    m_color_ids.shrink_to_fit();
}

std::string SurfaceColorAnnotation::get_triangle_as_string(size_t triangle_id) const
{
    const std::optional<SurfaceColorId> color_id = triangle_color(triangle_id);
    return color_id ? std::to_string(*color_id) : std::string{};
}

bool SurfaceColorAnnotation::set_triangle_from_string(size_t triangle_id, const std::string &value)
{
    if (value.empty())
        return false;

    SurfaceColorId color_id = UNASSIGNED;
    const char *begin = value.data();
    const char *end   = begin + value.size();
    const auto [ptr, error] = std::from_chars(begin, end, color_id);
    if (error != std::errc() || ptr != end || color_id == UNASSIGNED)
        return false;

    return set_triangle_color(triangle_id, color_id);
}

void SurfaceColorAnnotation::trim_unassigned_tail()
{
    while (!m_color_ids.empty() && m_color_ids.back() == UNASSIGNED)
        m_color_ids.pop_back();
}

std::optional<SurfaceColorId> configured_surface_color_id(
    const ModelObject &object,
    const ModelVolume &volume)
{
    static constexpr const char *option_key = "coextrusion_surface_color_id";
    const ModelConfig *config = volume.config.has(option_key) ? &volume.config :
        (object.config.has(option_key) ? &object.config : nullptr);
    if (config == nullptr)
        return std::nullopt;
    const int color_id = config->opt_int(option_key);
    return color_id < 0 ? std::nullopt : std::optional<SurfaceColorId>(SurfaceColorId(color_id));
}

} // namespace Slic3r::CoExtrusion
