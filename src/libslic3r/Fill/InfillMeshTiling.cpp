#include "InfillMeshTiling.hpp"

#include "../Model.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <cmath>
#include <map>
#include <mutex>
#include <unordered_map>

namespace Slic3r {

InfillCellTiling infill_cell_tiling(const BoundingBoxf3 &cell_bbox, const BoundingBoxf3 &target, size_t max_tiles)
{
    InfillCellTiling out;
    // A flat cell (a sheet) has a zero-extent axis and therefore counts as "undefined" to
    // BoundingBox3Base; only an inverted box is rejected here.
    if ((cell_bbox.size().array() < 0.).any() || ! target.defined)
        return out;
    const Vec3d pitch = cell_bbox.size();
    for (int a = 0; a < 3; ++ a) {
        if (pitch(a) <= EPSILON) {
            out.lo(a) = 0;
            out.hi(a) = 0;
            continue;
        }
        // First / last tile whose extent [min + i*pitch, max + i*pitch] overlaps the target by
        // more than a touching face (a tile that merely touches contributes nothing).
        out.lo(a) = int(std::floor((target.min(a) - cell_bbox.max(a)) / pitch(a) + EPSILON)) + 1;
        out.hi(a) = int(std::ceil ((target.max(a) - cell_bbox.min(a)) / pitch(a) - EPSILON)) - 1;
        if (out.hi(a) < out.lo(a)) {
            out.lo(a) = 0;
            out.hi(a) = 0;
        }
    }
    if (out.count() > max_tiles) {
        out.lo = Vec3i32(0, 0, 0);
        out.hi = Vec3i32(0, 0, 0);
        out.truncated = true;
    }
    return out;
}

indexed_triangle_set tile_infill_cell(const indexed_triangle_set &cell, const BoundingBoxf3 &target_in_cell_frame,
                                      size_t max_tiles, InfillCellTiling *tiling_out)
{
    indexed_triangle_set out;
    if (cell.indices.empty())
        return out;
    const BoundingBoxf3    cell_bbox = bounding_box(cell);
    const InfillCellTiling tiling    = infill_cell_tiling(cell_bbox, target_in_cell_frame, max_tiles);
    if (tiling_out != nullptr)
        *tiling_out = tiling;
    const Vec3d pitch = cell_bbox.size();
    const size_t n    = tiling.count();
    out.vertices.reserve(cell.vertices.size() * n);
    out.indices.reserve(cell.indices.size() * n);
    for (int i = tiling.lo.x(); i <= tiling.hi.x(); ++ i)
        for (int j = tiling.lo.y(); j <= tiling.hi.y(); ++ j)
            for (int k = tiling.lo.z(); k <= tiling.hi.z(); ++ k) {
                const Vec3f shift(float(i * pitch.x()), float(j * pitch.y()), float(k * pitch.z()));
                const int   base = int(out.vertices.size());
                for (const Vec3f &v : cell.vertices)
                    out.vertices.emplace_back(v + shift);
                for (const Vec3i32 &f : cell.indices)
                    out.indices.emplace_back(f.x() + base, f.y() + base, f.z() + base);
            }
    return out;
}

indexed_triangle_set build_infill_lattice(const indexed_triangle_set &cell, const Transform3d &cell_to_target,
                                          const BoundingBoxf3 &target, size_t max_tiles, InfillCellTiling *tiling)
{
    if (cell.indices.empty() || ! target.defined)
        return {};
    // The target box as seen from the cell: its eight corners through the inverse map.
    const BoundingBoxf3 target_in_cell = target.transformed(cell_to_target.inverse());
    indexed_triangle_set lattice = tile_infill_cell(cell, target_in_cell, max_tiles, tiling);
    its_transform(lattice, cell_to_target, false);
    return lattice;
}

bool its_is_open_surface(const indexed_triangle_set &its)
{
    // Count directed edges; an undirected edge used once is a boundary.
    std::unordered_map<uint64_t, int> edges;
    edges.reserve(its.indices.size() * 3);
    auto key = [](int a, int b) { if (a > b) std::swap(a, b); return (uint64_t(uint32_t(a)) << 32) | uint32_t(b); };
    for (const Vec3i32 &f : its.indices)
        for (int i = 0; i < 3; ++ i)
            ++ edges[key(f(i), f((i + 1) % 3))];
    for (const auto &e : edges)
        if (e.second == 1)
            return true;
    return false;
}

void join_polylines_at_endpoints(Polylines &polylines, coord_t tolerance)
{
    if (polylines.size() < 2)
        return;
    tolerance = std::max<coord_t>(tolerance, 1);
    // Endpoints hashed on a grid of the tolerance; a lookup checks the 3x3 neighbourhood.
    auto cell = [tolerance](const Point &p) {
        const int64_t cx = int64_t(std::floor(double(p.x()) / double(tolerance)));
        const int64_t cy = int64_t(std::floor(double(p.y()) / double(tolerance)));
        return std::make_pair(cx, cy);
    };
    auto cell_key = [](int64_t cx, int64_t cy) { return uint64_t(uint32_t(cx)) << 32 | uint32_t(cy); };
    // value: polyline index * 2 + (0 = front, 1 = back)
    std::unordered_multimap<uint64_t, size_t> ends;
    std::vector<bool> used(polylines.size(), false);
    auto add_ends = [&](size_t i) {
        const auto cf = cell(polylines[i].points.front()), cb = cell(polylines[i].points.back());
        ends.emplace(cell_key(cf.first, cf.second), i * 2);
        ends.emplace(cell_key(cb.first, cb.second), i * 2 + 1);
    };
    for (size_t i = 0; i < polylines.size(); ++ i)
        if (polylines[i].points.size() >= 2)
            add_ends(i);
        else
            used[i] = true;
    auto is_near = [tolerance](const Point &a, const Point &b) {
        return std::abs(a.x() - b.x()) <= tolerance && std::abs(a.y() - b.y()) <= tolerance;
    };
    // Find an unused polyline (other than `self`) with an end near `p`; returns index * 2 + end.
    auto find_end = [&](const Point &p, size_t self) -> size_t {
        const auto c = cell(p);
        for (int64_t dx = -1; dx <= 1; ++ dx)
            for (int64_t dy = -1; dy <= 1; ++ dy) {
                auto range = ends.equal_range(cell_key(c.first + dx, c.second + dy));
                for (auto it = range.first; it != range.second; ++ it) {
                    const size_t i = it->second / 2;
                    if (i == self || used[i])
                        continue;
                    const Point &q = (it->second & 1) ? polylines[i].points.back() : polylines[i].points.front();
                    if (is_near(p, q))
                        return it->second;
                }
            }
        return size_t(-1);
    };
    Polylines out;
    out.reserve(polylines.size());
    for (size_t start = 0; start < polylines.size(); ++ start) {
        if (used[start])
            continue;
        used[start] = true;
        Points chain = std::move(polylines[start].points);
        // Extend forward from the back, then backward from the front.
        for (int dir = 0; dir < 2; ++ dir) {
            for (;;) {
                if (is_near(chain.front(), chain.back()) && chain.size() > 2)
                    break; // closed
                const size_t hit = find_end(chain.back(), size_t(-1));
                if (hit == size_t(-1))
                    break;
                const size_t i = hit / 2;
                used[i] = true;
                Points &pts = polylines[i].points;
                if (hit & 1)
                    std::reverse(pts.begin(), pts.end());
                chain.insert(chain.end(), pts.begin() + 1, pts.end());
            }
            std::reverse(chain.begin(), chain.end());
        }
        out.emplace_back(std::move(chain));
    }
    polylines = std::move(out);
}

std::vector<Polylines> slice_sheet_mesh(const indexed_triangle_set &its, const std::vector<float> &zs, coord_t join_tolerance)
{
    std::vector<Polylines> out(zs.size());
    if (its.indices.empty() || zs.empty())
        return out;
    // Facets sorted by their z range would let us skip; for lattice cells the count is modest,
    // so a plain sweep per z keeps this simple.
    for (size_t iz = 0; iz < zs.size(); ++ iz) {
        const float z = zs[iz];
        Polylines segments;
        for (const Vec3i32 &f : its.indices) {
            const Vec3f *v[3] = { &its.vertices[f(0)], &its.vertices[f(1)], &its.vertices[f(2)] };
            const bool above[3] = { v[0]->z() >= z, v[1]->z() >= z, v[2]->z() >= z };
            if (above[0] == above[1] && above[1] == above[2])
                continue;
            Points pts;
            for (int i = 0; i < 3; ++ i) {
                const int j = (i + 1) % 3;
                if (above[i] == above[j])
                    continue;
                const Vec3f &a = *v[i], &b = *v[j];
                const float  t = (z - a.z()) / (b.z() - a.z());
                pts.emplace_back(scaled<coord_t>(a.x() + t * (b.x() - a.x())), scaled<coord_t>(a.y() + t * (b.y() - a.y())));
            }
            if (pts.size() == 2 && pts[0] != pts[1])
                segments.emplace_back(std::move(pts));
        }
        join_polylines_at_endpoints(segments, join_tolerance);
        out[iz] = std::move(segments);
    }
    return out;
}

indexed_triangle_set infill_mesh_cell_from_file(const std::string &path)
{
    struct CacheEntry
    {
        std::time_t          mtime;
        indexed_triangle_set cell;
    };
    static std::mutex                        cache_mutex;
    static std::map<std::string, CacheEntry> cache;
    if (path.empty())
        return {};
    boost::system::error_code ec;
    const std::time_t mtime = boost::filesystem::last_write_time(path, ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(warning) << "mesh infill: cannot read infill mesh file " << path << ": " << ec.message();
        return {};
    }
    std::lock_guard<std::mutex> lock(cache_mutex);
    if (auto it = cache.find(path); it != cache.end() && it->second.mtime == mtime)
        return it->second.cell;
    indexed_triangle_set cell;
    try {
        Model model = Model::read_from_file(path, nullptr, nullptr, LoadStrategy::LoadModel);
        for (const ModelObject *object : model.objects)
            for (const ModelVolume *volume : object->volumes)
                if (volume->is_model_part()) {
                    indexed_triangle_set vits = volume->mesh().its;
                    its_transform(vits, volume->get_matrix(), false);
                    its_merge(cell, vits);
                }
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(warning) << "mesh infill: cannot load infill mesh file " << path << ": " << ex.what();
        cell.clear();
    }
    if (cell.indices.empty())
        BOOST_LOG_TRIVIAL(warning) << "mesh infill: infill mesh file " << path << " contains no geometry";
    cache[path] = CacheEntry{ mtime, cell };
    return cell;
}

indexed_triangle_set infill_mesh_cell(const ModelObject &mo, const std::string &fallback_path, Transform3d &cell_to_object)
{
    indexed_triangle_set cell;
    cell_to_object = Transform3d::Identity();
    bool first = true;
    for (const ModelVolume *volume : mo.volumes) {
        if (! volume->is_infill_mesh())
            continue;
        indexed_triangle_set vits = volume->mesh().its;
        if (first) {
            cell_to_object = volume->get_matrix();
            first = false;
        } else
            its_transform(vits, cell_to_object.inverse() * volume->get_matrix(), false);
        its_merge(cell, vits);
    }
    if (cell.indices.empty() && ! fallback_path.empty())
        cell = infill_mesh_cell_from_file(fallback_path);
    return cell;
}

} // namespace Slic3r
