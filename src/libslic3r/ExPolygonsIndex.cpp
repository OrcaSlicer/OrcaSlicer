#include "ExPolygonsIndex.hpp"
using namespace Slic3r;

// IMPROVE: use one dimensional vector for polygons offset with searching by std::lower_bound
ExPolygonsIndices::ExPolygonsIndices(const ExPolygons &shapes)
{
    // prepare offsets
    m_offsets.reserve(shapes.size());
    uint32_t offset = 0;
    for (const ExPolygon &shape : shapes) {
        assert(!shape.contour.points.empty());
        std::vector<uint32_t> shape_offsets;
        shape_offsets.reserve(shape.holes.size() + 1);
        shape_offsets.push_back(offset);
        offset += shape.contour.points.size();
        for (const Polygon &hole: shape.holes) {
            shape_offsets.push_back(offset);
            offset += hole.points.size();
        }
        m_offsets.push_back(std::move(shape_offsets));
    }
    m_count = offset;
}

uint32_t ExPolygonsIndices::cvt(const ExPolygonsIndex &id) const
{
    assert(id.expolygons_index < m_offsets.size());
    const std::vector<uint32_t> &shape_offset = m_offsets[id.expolygons_index];
    assert(id.polygon_index < shape_offset.size());
    uint32_t res = shape_offset[id.polygon_index] + id.point_index;
    assert(res < m_count);
    return res;
}

ExPolygonsIndex ExPolygonsIndices::cvt(uint32_t index) const
{
    assert(index < m_count);
    ExPolygonsIndex result{0, 0, 0};
    // find expolygon index
    auto fn = [](const std::vector<uint32_t> &offsets, uint32_t index) { return offsets[0] < index; };
    auto it = std::lower_bound(m_offsets.begin() + 1, m_offsets.end(), index, fn);
    result.expolygons_index = it - m_offsets.begin();
    if (it == m_offsets.end() || it->at(0) != index) --result.expolygons_index;

    // find polygon index
    const std::vector<uint32_t> &shape_offset = m_offsets[result.expolygons_index];
    auto it2 = std::lower_bound(shape_offset.begin() + 1, shape_offset.end(), index);
    result.polygon_index = it2 - shape_offset.begin();
    if (it2 == shape_offset.end() || *it2 != index) --result.polygon_index;

    // calculate point index
    uint32_t polygon_offset = shape_offset[result.polygon_index];
    assert(index >= polygon_offset);
    result.point_index = index - polygon_offset;
    return result;
}

bool ExPolygonsIndices::is_last_point(const ExPolygonsIndex &id) const { 
    assert(id.expolygons_index < m_offsets.size());
    const std::vector<uint32_t> &shape_offset = m_offsets[id.expolygons_index];
    assert(id.polygon_index < shape_offset.size());
    uint32_t index = shape_offset[id.polygon_index] + id.point_index;
    assert(index < m_count);
    // next index
    uint32_t next_point_index = index + 1;
    uint32_t next_poly_index = id.polygon_index + 1;
    uint32_t next_expoly_index = id.expolygons_index + 1;
    // is last expoly?
    if (next_expoly_index == m_offsets.size()) {
        // is last expoly last poly?
        if (next_poly_index == shape_offset.size()) 
            return next_point_index == m_count;
    } else {
        // (not last expoly) is expoly last poly?
        if (next_poly_index == shape_offset.size()) 
            return next_point_index == m_offsets[next_expoly_index][0];
    }
    // Not last polygon in expolygon
    return next_point_index == shape_offset[next_poly_index];
}

uint32_t ExPolygonsIndices::get_count() const { return m_count; }		
