#include "TextureBakeIndex.hpp"

#include <algorithm>

namespace Slic3r {
namespace TextureBake {

WeldResult weld_vertices(const std::vector<Vec3f> &positions, double quant)
{
    WeldResult        out;
    QuantizedPointMap map(quant, std::min<size_t>(positions.size(), size_t(1) << 22));
    out.vertex_id.resize(positions.size());
    int next_id = 0;
    for (size_t i = 0; i < positions.size(); ++i) {
        const int id = map.get_or_set(positions[i], next_id);
        if (map.inserted())
            ++next_id;
        out.vertex_id[i] = id;
    }
    out.unique_count = next_id;
    return out;
}

} // namespace TextureBake
} // namespace Slic3r
