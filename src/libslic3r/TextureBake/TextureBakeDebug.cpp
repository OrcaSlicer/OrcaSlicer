#include "TextureBakeDebug.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r {

namespace {

// Half-edge key, low index first so both sides of an edge form the same one.
inline uint64_t edge_key(int a, int b)
{
    const uint32_t lo = uint32_t(std::min(a, b)), hi = uint32_t(std::max(a, b));
    return (uint64_t(lo) << 32) | uint64_t(hi);
}

} // namespace

void bake_stage_topology(const BakeStageMesh &mesh, size_t &open_edges, size_t &non_manifold_edges,
                         size_t &degenerate)
{
    open_edges = non_manifold_edges = degenerate = 0;

    // Sorted half-edges rather than a hash map: same answer, but it is one allocation and a sort
    // instead of three million node allocations, which on a stage this size is the whole cost.
    std::vector<uint64_t> keys;
    keys.reserve(mesh.indices.size() * 3);
    for (const Vec3i32 &t : mesh.indices) {
        if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) {
            ++degenerate;
            continue; // a collapsed face has no edges worth counting
        }
        const Vec3f &a = mesh.vertices[size_t(t[0])];
        if ((mesh.vertices[size_t(t[1])] - a).cross(mesh.vertices[size_t(t[2])] - a).squaredNorm() <= 0.f)
            ++degenerate; // zero area but three distinct corners: still counted as an edge carrier
        for (int e = 0; e < 3; ++e)
            keys.push_back(edge_key(t[e], t[(e + 1) % 3]));
    }
    std::sort(keys.begin(), keys.end());
    for (size_t i = 0; i < keys.size();) {
        size_t j = i + 1;
        while (j < keys.size() && keys[j] == keys[i])
            ++j;
        const size_t incident = j - i;
        if (incident == 1)
            ++open_edges;
        else if (incident > 2)
            ++non_manifold_edges;
        i = j;
    }
}

void BakeStageRecorder::finish(BakeStageSnapshot &s)
{
    s.triangles = s.mesh.indices.size();
    s.vertices  = s.mesh.vertices.size();
    if (m_check_topology)
        bake_stage_topology(s.mesh, s.open_edges, s.non_manifold_edges, s.degenerate);
    s.topology_checked = m_check_topology;
    if (s.triangles > m_mesh_cap) {
        s.mesh_dropped = true;
        s.mesh.vertices.clear();
        s.mesh.vertices.shrink_to_fit();
        s.mesh.indices.clear();
        s.mesh.indices.shrink_to_fit();
    }
    m_stages.push_back(std::move(s));
}

void BakeStageRecorder::capture(const char *name, const TextureBake::TriSoup &soup, double ms,
                                const std::string &detail)
{
    if (!m_enabled)
        return;

    BakeStageSnapshot s;
    s.name   = name;
    s.detail = detail;
    s.ms     = ms;

    // The pipeline works on non-indexed soup, so welding here is what turns it back into something
    // renderable. The geometry grid, matching to_indexed_triangle_set(), so the debug view shows the
    // same sharing the bake's own output would have.
    const size_t                 n = soup.pos.size();
    TextureBake::QuantizedPointMap map(TextureBake::WELD_GRID_GEOMETRY, std::min(n, size_t(1) << 22));
    std::vector<int>             id(n);
    s.mesh.vertices.reserve(n / 3);
    for (size_t i = 0; i < n; ++i) {
        id[i] = map.get_or_set(soup.pos[i], int(s.mesh.vertices.size()));
        if (map.inserted())
            s.mesh.vertices.push_back(soup.pos[i]);
    }
    s.mesh.indices.reserve(n / 3);
    for (size_t t = 0; t + 2 < n; t += 3) {
        // Corners that welded together carry no area; the bake's own conversion drops them too, so
        // dropping them here keeps the stage count honest against what would be committed.
        if (id[t] == id[t + 1] || id[t + 1] == id[t + 2] || id[t] == id[t + 2])
            continue;
        s.mesh.indices.emplace_back(id[t], id[t + 1], id[t + 2]);
    }
    finish(s);
}

void BakeStageRecorder::capture(const char *name, const std::vector<Vec3f> &vertices,
                                const std::vector<Vec3i32> &indices, double ms,
                                const std::string &detail)
{
    if (!m_enabled)
        return;
    BakeStageSnapshot s;
    s.name          = name;
    s.detail        = detail;
    s.ms            = ms;
    s.mesh.vertices = vertices;
    s.mesh.indices  = indices;
    finish(s);
}

void BakeStageRecorder::capture_note(const char *name, double ms, const std::string &detail)
{
    if (!m_enabled)
        return;
    BakeStageSnapshot s;
    s.name             = name;
    s.detail           = detail;
    s.ms               = ms;
    s.topology_checked = false;
    m_stages.push_back(std::move(s));
}

void BakeStageRecorder::rebase(size_t from, const Transform3d *to_local, bool flip_winding)
{
    for (size_t i = from; i < m_stages.size(); ++i) {
        BakeStageMesh &m = m_stages[i].mesh;
        if (to_local != nullptr)
            for (Vec3f &v : m.vertices)
                v = (*to_local * v.cast<double>()).cast<float>();
        if (flip_winding)
            for (Vec3i32 &t : m.indices)
                std::swap(t[1], t[2]);
    }
}

double BakeStageRecorder::total_ms() const
{
    double sum = 0.0;
    for (const BakeStageSnapshot &s : m_stages)
        sum += s.ms;
    return sum;
}

size_t dump_bake_stages(const std::vector<BakeStageSnapshot> &stages, const std::string &dir)
{
    boost::system::error_code ec;
    boost::filesystem::create_directories(dir, ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "BakeStageRecorder: cannot create " << dir << ": " << ec.message();
        return 0;
    }

    size_t written = 0;
    for (size_t i = 0; i < stages.size(); ++i) {
        const BakeStageSnapshot &s = stages[i];
        if (s.mesh.empty())
            continue;

        // Stage names carry spaces and punctuation; keep the filename to what every shell and viewer
        // handles without quoting.
        std::string safe;
        for (const char c : s.name)
            safe += (std::isalnum(static_cast<unsigned char>(c)) != 0) ? c : '_';

        char path[1024];
        std::snprintf(path, sizeof(path), "%s/%02zu_%s.obj", dir.c_str(), i, safe.c_str());
        std::FILE *f = std::fopen(path, "wb");
        if (f == nullptr) {
            BOOST_LOG_TRIVIAL(error) << "BakeStageRecorder: cannot write " << path;
            continue;
        }
        std::fprintf(f, "# texture bake stage %zu: %s\n", i, s.name.c_str());
        if (!s.detail.empty())
            std::fprintf(f, "# %s\n", s.detail.c_str());
        std::fprintf(f, "# %zu triangles, %.2f ms\n", s.triangles, s.ms);
        for (const Vec3f &v : s.mesh.vertices)
            std::fprintf(f, "v %.6f %.6f %.6f\n", double(v.x()), double(v.y()), double(v.z()));
        for (const Vec3i32 &t : s.mesh.indices) // OBJ indices are 1-based
            std::fprintf(f, "f %d %d %d\n", t[0] + 1, t[1] + 1, t[2] + 1);
        std::fclose(f);
        ++written;
    }

    char summary[1024];
    std::snprintf(summary, sizeof(summary), "%s/stages.txt", dir.c_str());
    if (std::FILE *f = std::fopen(summary, "wb"); f != nullptr) {
        std::fprintf(f, "%-3s %-24s %10s %12s %10s %8s %8s %8s  %s\n", "#", "stage", "ms", "triangles",
                     "vertices", "open", "nonman", "degen", "detail");
        double total = 0.0;
        for (size_t i = 0; i < stages.size(); ++i) {
            const BakeStageSnapshot &s = stages[i];
            total += s.ms;
            std::fprintf(f, "%-3zu %-24s %10.2f %12zu %10zu ", i, s.name.c_str(), s.ms, s.triangles,
                         s.vertices);
            if (s.topology_checked)
                std::fprintf(f, "%8zu %8zu %8zu", s.open_edges, s.non_manifold_edges, s.degenerate);
            else
                std::fprintf(f, "%8s %8s %8s", "-", "-", "-");
            std::fprintf(f, "  %s%s\n", s.detail.c_str(), s.mesh_dropped ? " [mesh over cap, not written]" : "");
        }
        std::fprintf(f, "\ntotal %.2f ms across %zu stages\n", total, stages.size());
        std::fclose(f);
    }
    return written;
}

} // namespace Slic3r
