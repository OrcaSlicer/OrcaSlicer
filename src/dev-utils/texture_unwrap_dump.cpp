// Diagnostic for the LSCM unwrap of a texture displacement layer.
//
// It exists because the defect it hunts only shows up on a real painted patch: the paint mask is built
// by TriangleSelector splitting base triangles, so the patch topology cannot be written down by hand,
// and reasoning about it from a screenshot of the 3D view had already produced three wrong diagnoses.
// This loads a saved project, rebuilds exactly the patch the bake would act on, runs the same unwrap,
// and reports what came out - per chart, so a bad one can be pointed at rather than guessed at.
//
//   texture_unwrap_dump <project.3mf>

// nanosvg is header-only and libslic3r's 3mf import references it without carrying the implementation,
// so every executable that links libslic3r has to supply it. Must precede any include that pulls the
// header in, or its include guard suppresses the implementation. Same pattern as the other dev tools.
#define NANOSVG_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <functional>
#include <unordered_map>
#include <vector>

#include "libslic3r/Model.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>

using namespace Slic3r;

namespace {

uint64_t edge_key(int a, int b)
{
    if (a > b)
        std::swap(a, b);
    return (uint64_t(uint32_t(a)) << 32) | uint32_t(b);
}

// Boundary loops and the Euler characteristic of a face set, which together say whether a chart is the
// topological disk LSCM needs (one loop, V - E + F == 1).
void chart_topology(const indexed_triangle_set &mesh, const std::vector<int> &faces, int &loops, int &euler)
{
    std::unordered_map<uint64_t, int> edge_use;
    std::unordered_map<int, int>      local;
    for (const int f : faces) {
        const stl_triangle_vertex_indices &t = mesh.indices[size_t(f)];
        for (int i = 0; i < 3; ++i) {
            ++edge_use[edge_key(t[i], t[(i + 1) % 3])];
            local.emplace(t[i], int(local.size()));
        }
    }
    euler = int(local.size()) - int(edge_use.size()) + int(faces.size());

    std::unordered_map<int, int> parent;
    const std::function<int(int)> find = [&](int x) {
        while (parent[x] != x)
            x = parent[x] = parent[parent[x]];
        return x;
    };
    for (const auto &[key, uses] : edge_use)
        if (uses == 1)
            for (const int v : { int(key >> 32), int(uint32_t(key)) })
                parent.emplace(v, v);
    for (const auto &[key, uses] : edge_use)
        if (uses == 1) {
            const int a = find(int(key >> 32)), b = find(int(uint32_t(key)));
            if (a != b)
                parent[b] = a;
        }
    std::unordered_map<int, int> roots;
    for (const auto &[v, p] : parent)
        roots[find(v)] = 1;
    loops = int(roots.size());
}

float signed_area_2d(const Vec2f &a, const Vec2f &b, const Vec2f &c)
{
    return 0.5f * ((b.x() - a.x()) * (c.y() - a.y()) - (c.x() - a.x()) * (b.y() - a.y()));
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::printf("usage: texture_unwrap_dump <project.3mf>\n");
        return 2;
    }

    Model                     model;
    DynamicPrintConfig        config;
    ConfigSubstitutionContext ctx(ForwardCompatibilitySubstitutionRule::Enable);
    PlateDataPtrs             plate_data;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false, is_orca_3mf = false;
    Semver                    file_version;
    // The importer writes a backup copy under the data dir and silently loses objects without one.
    const boost::filesystem::path tmp = boost::filesystem::temp_directory_path() / "texture_unwrap_dump";
    boost::filesystem::create_directories(tmp);
    set_data_dir(tmp.string());

    // LoadModel so the meshes come through; AddDefaultInstances because an object with no instance is
    // dropped by the plate mapping, which is what "skip this object" in the log means.
    if (!load_bbs_3mf(argv[1], &config, &ctx, &model, &plate_data, &project_presets, &is_bbl_3mf, &is_orca_3mf,
                      &file_version, nullptr,
                      LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances |
                          LoadStrategy::Silence)) {
        std::printf("failed to load %s\n", argv[1]);
        return 1;
    }

    std::printf("loaded: %zu object(s)\n", model.objects.size());

    for (const ModelObject *object : model.objects)
        for (const ModelVolume *volume : object->volumes) {
            if (volume->texture_displacement_layers.empty()) {
                std::printf("volume \"%s\": no texture displacement layers; paint masks per slot:",
                            volume->name.c_str());
                for (int i = 0; i < int(TEXTURE_DISPLACEMENT_MAX_LAYERS); ++i)
                    std::printf(" %zu", volume->texture_displacement_facet(i).get_data().triangles_to_split.size());
                std::printf("\n");
                continue;
            }
            std::printf("volume \"%s\": %zu base triangles, %zu layer(s)\n", volume->name.c_str(),
                        volume->mesh().its.indices.size(), volume->texture_displacement_layers.size());

            for (const TextureDisplacementLayer &layer : volume->texture_displacement_layers) {
                std::printf("\n  layer %d \"%s\"  mapping=%d  seam_angle=%.1f  connect=%d  islands_stored=%zu\n",
                            layer.slot, layer.name.c_str(), int(layer.projection_method),
                            layer.lscm_seam_angle_deg, int(layer.auto_connect_islands), layer.islands.size());
                if (layer.projection_method != TextureProjectionMethod::LSCM)
                    continue;

                const indexed_triangle_set patch =
                    extract_painted_patch(volume->mesh().its, volume->texture_displacement_facet(layer.slot).get_data());
                std::printf("  patch: %zu vertices, %zu triangles\n", patch.vertices.size(), patch.indices.size());
                if (patch.indices.empty())
                    continue;

                const auto t0 = std::chrono::steady_clock::now();
                const PatchUnwrap unwrap = compute_patch_unwrap(patch, layer.lscm_seam_angle_deg, 0.f,
                                                                layer.lscm_seam_edges);
                const auto t1 = std::chrono::steady_clock::now();
                std::printf("  TIMING compute_patch_unwrap: %.0f ms\n",
                            std::chrono::duration<double, std::milli>(t1 - t0).count());
                std::printf("  unwrap: %d charts, %zu unwrapped triangles\n", unwrap.chart_count,
                            unwrap.indices.size());

                // Group the patch's faces by chart so each can be examined on its own.
                std::vector<std::vector<int>> chart_faces(size_t(std::max(unwrap.chart_count, 0)));
                for (size_t i = 0; i < unwrap.indices.size(); ++i) {
                    const int chart = unwrap.vertex_chart[size_t(unwrap.indices[i][0])];
                    if (chart >= 0 && size_t(chart) < chart_faces.size())
                        chart_faces[size_t(chart)].push_back(unwrap.source_face[i]);
                }

                int bad_charts = 0;
                for (size_t c = 0; c < chart_faces.size(); ++c) {
                    int loops = 0, euler = 0;
                    chart_topology(patch, chart_faces[c], loops, euler);

                    // Flipped triangles: the unwrap folded over itself, which is what a planar fallback
                    // does to a chart that is not flat. Measured on the unwrap's own triangles.
                    int pos = 0, neg = 0;
                    for (size_t i = 0; i < unwrap.indices.size(); ++i) {
                        const stl_triangle_vertex_indices &t = unwrap.indices[i];
                        if (unwrap.vertex_chart[size_t(t[0])] != int(c))
                            continue;
                        const float a = signed_area_2d(unwrap.uvs[size_t(t[0])], unwrap.uvs[size_t(t[1])],
                                                       unwrap.uvs[size_t(t[2])]);
                        if (a > 0.f) ++pos; else if (a < 0.f) ++neg;
                    }
                    const int flipped = std::min(pos, neg);
                    const bool disk   = loops == 1 && euler == 1;
                    if (!disk || flipped > 0) {
                        ++bad_charts;
                        std::printf("    chart %2zu: %4zu faces  loops=%d euler=%d%s  flipped=%d/%d%s\n", c,
                                    chart_faces[c].size(), loops, euler, disk ? "" : "  NOT A DISK", flipped,
                                    pos + neg, flipped ? "  FOLDED" : "");
                    }
                }
                std::printf("  charts with a defect: %d / %d\n", bad_charts, unwrap.chart_count);

                // What the eye actually sees. Every patch edge shared by two charts should carry the same
                // UV on both sides once the islands are laid out as a connected net; where it does not,
                // the texture jumps across that seam. Measured through compute_lscm_uvs(), i.e. the exact
                // coordinates the bake and the checker overlay sample.
                {
                    const auto n0 = std::chrono::steady_clock::now();
                    const std::vector<TextureIsland> net = compute_connected_net(unwrap);
                    const auto n1 = std::chrono::steady_clock::now();
                    std::printf("  TIMING compute_connected_net: %.0f ms (%zu islands)\n",
                                std::chrono::duration<double, std::milli>(n1 - n0).count(), net.size());
                }
                const auto t2 = std::chrono::steady_clock::now();
                const std::vector<Vec2f> uv = compute_lscm_uvs(patch, layer);
                const auto t3 = std::chrono::steady_clock::now();
                std::printf("  TIMING compute_lscm_uvs:     %.0f ms  (called on every preview, overlay and bake)\n",
                            std::chrono::duration<double, std::milli>(t3 - t2).count());
                if (uv.size() != patch.vertices.size()) {
                    std::printf("  compute_lscm_uvs returned %zu uvs for %zu vertices\n", uv.size(),
                                patch.vertices.size());
                    continue;
                }
                // Per-corner UVs carry each chart's own placement, so an edge shared by two charts shows
                // the jump directly: the same mesh vertex lands at two different UVs. That is exactly what
                // the eye reads as the texture breaking.
                const auto t4 = std::chrono::steady_clock::now();
                const std::vector<Vec2f> corner = compute_lscm_corner_uvs(patch, layer);
                const auto t5 = std::chrono::steady_clock::now();
                std::printf("  TIMING compute_lscm_corner_uvs: %.0f ms\n",
                            std::chrono::duration<double, std::milli>(t5 - t4).count());
                // Keyed by edge, holding the UV each incident face gives to the edge's *lower-numbered*
                // endpoint. Comparing that same vertex on both sides is the point: indexing by corner
                // position instead compares opposite ends of the edge, because the two faces wind it in
                // opposite directions.
                std::unordered_map<uint64_t, std::vector<Vec2f>> edge_seen;
                if (corner.size() == patch.indices.size() * 3)
                    for (size_t f = 0; f < patch.indices.size(); ++f) {
                        const stl_triangle_vertex_indices &t = patch.indices[f];
                        for (int k = 0; k < 3; ++k) {
                            const int a = t[k], b = t[(k + 1) % 3];
                            const int probe = std::min(a, b);
                            const int local = (a == probe) ? k : (k + 1) % 3;
                            edge_seen[edge_key(a, b)].push_back(corner[f * 3 + size_t(local)]);
                        }
                    }
                // Which chart each patch face belongs to, so a broken edge can be attributed to a pair.
                std::vector<int> chart_of_face(patch.indices.size(), -1);
                for (size_t i = 0; i < unwrap.indices.size(); ++i)
                    chart_of_face[size_t(unwrap.source_face[i])] = unwrap.vertex_chart[size_t(unwrap.indices[i][0])];

                std::unordered_map<uint64_t, std::vector<int>> edge_faces;
                for (size_t f = 0; f < patch.indices.size(); ++f) {
                    const stl_triangle_vertex_indices &t = patch.indices[f];
                    for (int k = 0; k < 3; ++k)
                        edge_faces[edge_key(t[k], t[(k + 1) % 3])].push_back(int(f));
                }

                int adjacent = 0, broken = 0, broken_same_chart = 0;
                float worst = 0.f;
                std::map<std::pair<int, int>, std::pair<int, float>> by_pair;
                for (const auto &[key, seen] : edge_seen) {
                    if (seen.size() != 2)
                        continue;
                    ++adjacent;
                    const float d = (seen[0] - seen[1]).norm();
                    if (d <= 1e-4f)
                        continue;
                    ++broken;
                    worst = std::max(worst, d);
                    const auto &faces_here = edge_faces[key];
                    int c1 = -1, c2 = -1;
                    if (faces_here.size() == 2) {
                        c1 = chart_of_face[size_t(faces_here[0])];
                        c2 = chart_of_face[size_t(faces_here[1])];
                    }
                    if (c1 == c2)
                        ++broken_same_chart;
                    auto &slot = by_pair[{ std::min(c1, c2), std::max(c1, c2) }];
                    ++slot.first;
                    slot.second = std::max(slot.second, d);
                }
                std::printf("  broken edges inside a single chart: %d\n", broken_same_chart);
                std::printf("  broken by chart pair:");
                for (const auto &[pk, v] : by_pair)
                    std::printf(" (%d,%d)x%d/%.1f", pk.first, pk.second, v.first, v.second);
                std::printf("\n");
                // Total length of the seams left broken, in mm: how much visibly torn edge the layout has,
                // which is what the eye adds up. A count alone hides whether the breaks are hairlines or
                // whole sides of an island.
                float seam_mm = 0.f;
                for (const auto &[key, seen] : edge_seen) {
                    if (seen.size() != 2 || (seen[0] - seen[1]).norm() <= 1e-4f)
                        continue;
                    seam_mm += (patch.vertices[size_t(key >> 32)] - patch.vertices[size_t(uint32_t(key))]).norm();
                }
                std::printf("  interior edges: %d, discontinuous: %d, total torn seam: %.2f mm (worst jump %.3f)\n",
                            adjacent, broken, seam_mm, worst);
                std::printf("  stored islands %zu vs charts %d -> %s\n", layer.islands.size(),
                            unwrap.chart_count,
                            layer.islands.size() == size_t(unwrap.chart_count) ? "stored placements used"
                                                                              : "net rebuilt");
            }
        }
    return 0;
}
