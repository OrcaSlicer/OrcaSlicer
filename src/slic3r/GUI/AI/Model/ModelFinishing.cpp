#include "ModelFinishing.hpp"
#include "ModelArtifact.hpp"
#include "ModelColorCleanup.hpp"
#include "VertexColorRegionEditor.hpp"

#include "libslic3r/Point.hpp"
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <openssl/evp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <charconv>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace Slic3r::AI {
namespace {
struct Canceled {};
struct Corner { size_t vertex; int normal {-1}; std::string token; };
struct Face { std::array<Corner, 3> corners; size_t line; bool removed {false}; bool reversed {false}; };
struct Edge { size_t first {0}, second {0}; unsigned count {0}; bool same_direction {false}; size_t from {0}; };
using EdgeMap = std::unordered_map<uint64_t, Edge>;

std::string file_hash(const boost::filesystem::path& path, const std::function<bool()>& canceled) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    boost::filesystem::ifstream file(path, std::ios::binary);
    if (!file || !digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("Unable to verify the model file.");
    std::array<char, 65536> buffer;
    while (file) {
        if (canceled && canceled()) throw Canceled {};
        file.read(buffer.data(), buffer.size());
        if (file.gcount() && EVP_DigestUpdate(digest.get(), buffer.data(), size_t(file.gcount())) != 1)
            throw std::runtime_error("Unable to verify the model file.");
    }
    unsigned char bytes[EVP_MAX_MD_SIZE]; unsigned length = 0;
    if (!file.eof() || EVP_DigestFinal_ex(digest.get(), bytes, &length) != 1)
        throw std::runtime_error("Unable to verify the model file.");
    std::ostringstream hex;
    for (unsigned i = 0; i < length; ++i) hex << std::hex << std::setw(2) << std::setfill('0') << unsigned(bytes[i]);
    return hex.str();
}

uint64_t edge_key(size_t a, size_t b) {
    return (uint64_t(std::min(a, b)) << 32) | uint64_t(std::max(a, b));
}
Vec3d face_normal(const Face& f, const std::vector<Vec3d>& vertices) {
    return (vertices[f.corners[1].vertex] - vertices[f.corners[0].vertex]).cross(
        vertices[f.corners[2].vertex] - vertices[f.corners[0].vertex]);
}
int obj_index(std::string_view text, size_t count) {
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
        if (!text.empty() && text.front() == '-')
            throw std::runtime_error("OBJ contains an invalid or forward index.");
    }
    long long parsed = 0;
    const auto converted = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (converted.ec != std::errc() || converted.ptr != text.data() + text.size())
        throw std::runtime_error("OBJ contains an invalid or forward index.");
    const long long index = parsed > 0 ? parsed - 1 : static_cast<long long>(count) + parsed;
    if (parsed == 0 || index < 0 || index >= static_cast<long long>(count))
        throw std::runtime_error("OBJ contains an invalid or forward index.");
    return static_cast<int>(index);
}
EdgeMap edges_of(const std::vector<Face>& faces, const std::vector<size_t>& surface,
    const std::function<bool()>& canceled) {
    EdgeMap edges;
    edges.reserve(surface.size() * 2);
    size_t visited = 0;
    for (const size_t i : surface) {
        if ((visited++ & 4095) == 0 && canceled && canceled()) throw Canceled {};
        const Face& f = faces[i];
        if (f.removed) continue;
        for (int k = 0; k < 3; ++k) {
            const size_t a = f.corners[k].vertex, b = f.corners[(k + 1) % 3].vertex;
            Edge& edge = edges[edge_key(a, b)];
            if (edge.count == 0) { edge.first = i; edge.from = a; }
            else if (edge.count == 1) { edge.second = i; edge.same_direction = edge.from == a; }
            ++edge.count;
        }
    }
    return edges;
}

ModelFinishingResult finish_recolored_artifact(const boost::filesystem::path& source,
    const boost::filesystem::path& destination, const ModelFinishingOptions& options,
    const std::function<bool()>& canceled)
{
    ModelFinishingResult result;
    boost::filesystem::path temporary;
    bool owns_temporary = false;
    auto check_cancel = [&] { if (canceled && canceled()) throw Canceled {}; };
    try {
        if (options.selected_faces.empty())
            throw std::runtime_error("Select a local surface before changing its color.");
        if (options.smooth_surface || options.repair_mesh || options.clean_color_spots)
            throw std::runtime_error("Local recoloring is a separate color-only operation; disable smoothing, repair and color cleanup.");
        for (const float channel : options.target_color)
            if (!std::isfinite(channel) || channel < 0 || channel > 1)
                throw std::runtime_error("Local recoloring requires a normalized finite RGBA color.");
        if (!is_model_artifact(source) || model_artifact_format(destination).empty())
            throw std::runtime_error("Choose an available OBJ or GLB source and output model.");
        if (boost::filesystem::exists(destination))
            throw std::runtime_error("The output already exists; choose a new model version.");
        if (boost::filesystem::canonical(source.parent_path()) != boost::filesystem::canonical(destination.parent_path()))
            throw std::runtime_error("Save the edited model beside its source to preserve material and texture paths.");
        check_cancel();
        result.source_sha256 = file_hash(source, canceled);
        TriangleMesh mesh;
        ObjInfo colors;
        std::string error;
        if (!load_model_artifact(source, mesh, colors, error))
            throw std::runtime_error(error);
        if (mesh.its.vertices.size() > 6000000 || mesh.its.indices.size() > 2000000)
            throw std::runtime_error("This model exceeds the local recoloring limit of two million triangles.");
        for (const size_t face : options.selected_faces)
            if (face >= mesh.its.indices.size())
                throw std::runtime_error("The selected face index is outside this model. Reload it and select its surface again.");
        check_cancel();
        Vec3f minimum = mesh.its.vertices.front(), maximum = minimum;
        for (const Vec3f& position : mesh.its.vertices) {
            minimum = minimum.cwiseMin(position);
            maximum = maximum.cwiseMax(position);
        }
        for (size_t axis = 0; axis < 3; ++axis)
            result.dimensions[axis] = double(maximum[axis]) - double(minimum[axis]);
        result.faces_before = mesh.its.indices.size();
        VertexColorRegionEditor editor;
        if (!editor.initialize(std::move(mesh.its), std::move(colors.vertex_colors), error))
            throw std::runtime_error(error);
        if (editor.select_faces(options.selected_faces) == 0)
            throw std::runtime_error("No model region is selected.");
        for (size_t face = 0; face < editor.selected_faces().size(); ++face) {
            if ((face & 4095) == 0) check_cancel();
            if (!editor.selected_faces()[face]) continue;
            for (size_t corner = 0; corner < 3; ++corner)
                if (editor.corner_color(face, corner) != options.target_color) {
                    ++result.recolored_faces;
                    break;
                }
        }
        temporary = destination.parent_path() / boost::filesystem::unique_path(
            ".recolor-%%%%-%%%%-%%%%" + destination.extension().string());
        auto partial = temporary; partial += ".partial";
        auto obj_temporary = temporary; obj_temporary += ".tmp";
        if (boost::filesystem::exists(temporary) || boost::filesystem::exists(partial) ||
            boost::filesystem::exists(obj_temporary))
            throw std::runtime_error("The temporary output already exists; try a new model version.");
        owns_temporary = true;
        if (!editor.apply_color_to_obj_copy(options.target_color, source, temporary, error))
            throw std::runtime_error(error);
        check_cancel();
        TriangleMesh output;
        ObjInfo output_colors;
        if (!load_model_artifact(temporary, output, output_colors, error))
            throw std::runtime_error(error);
        result.vertices = output.its.vertices.size();
        result.faces_after = output.its.indices.size();
        if (result.faces_after != result.faces_before)
            throw std::runtime_error("The recolored model did not preserve its triangle surface.");
        if (file_hash(source, canceled) != result.source_sha256)
            throw std::runtime_error("The source model changed during recoloring. Please reload it.");
        result.output_sha256 = file_hash(temporary, canceled);
        check_cancel();
        if (boost::filesystem::exists(destination))
            throw std::runtime_error("The output already exists; choose a new model version.");
        boost::filesystem::rename(temporary, destination);
        owns_temporary = false;
        result.success = true;
    } catch (const Canceled&) {
        result.canceled = true;
    } catch (const std::exception& error) {
        result.error = error.what();
    }
    if (owns_temporary) {
        boost::system::error_code ignored;
        boost::filesystem::remove(temporary, ignored);
        auto partial = temporary; partial += ".partial";
        boost::filesystem::remove(partial, ignored);
        auto obj_temporary = temporary; obj_temporary += ".tmp";
        boost::filesystem::remove(obj_temporary, ignored);
    }
    return result;
}
}

ModelFinishingResult finish_model_obj(const boost::filesystem::path& source,
    const boost::filesystem::path& destination, const ModelFinishingOptions& options,
    const std::function<bool()>& canceled)
{
    if (options.recolor_selected)
        return finish_recolored_artifact(source, destination, options, canceled);
    ModelFinishingResult result;
    boost::filesystem::path temporary = destination;
    temporary += ".partial";
    bool owns_temporary = false;
    auto check_cancel = [&] { if (canceled && canceled()) throw Canceled {}; };
    try {
        if (!std::isfinite(options.strength) || options.strength < 0.0 || options.strength > 1.0)
            throw std::runtime_error("Surface strength must be between 0 and 1.");
        if (!options.smooth_surface && !options.repair_mesh && !options.clean_color_spots)
            throw std::runtime_error("Choose surface smoothing or mesh repair.");
        const bool local_selection = !options.selected_faces.empty();
        if (options.clean_color_spots) {
            if (!local_selection)
                throw std::runtime_error("Select a local surface before cleaning color spots.");
            if (options.smooth_surface || options.repair_mesh)
                throw std::runtime_error("Color cleanup is a separate color-only operation; disable smoothing and mesh repair.");
            if (options.cleanup_palette.size() < 2 || options.cleanup_palette.size() > 6)
                throw std::runtime_error("Color cleanup requires two to six source palette colors.");
            for (const auto& color : options.cleanup_palette) for (const float channel : color)
                if (!std::isfinite(channel) || channel < 0 || channel > 1)
                    throw std::runtime_error("Color cleanup requires normalized source palette colors.");
        }
        if (local_selection && options.repair_mesh)
            throw std::runtime_error("Local surface smoothing cannot include whole-model mesh repair. Disable mesh repair for a selection.");
        if (!boost::filesystem::is_regular_file(source) || boost::filesystem::file_size(source) > 512ull * 1024 * 1024)
            throw std::runtime_error("The source OBJ is missing or exceeds 512 MB.");
        if (boost::filesystem::exists(destination) || boost::filesystem::exists(temporary))
            throw std::runtime_error("The output already exists; choose a new model version.");
        if (boost::filesystem::canonical(source.parent_path()) != boost::filesystem::canonical(destination.parent_path()))
            throw std::runtime_error("Save the edited OBJ beside its source to preserve material and texture paths.");
        check_cancel();
        result.source_sha256 = file_hash(source, canceled);
        boost::filesystem::ifstream input(source, options.clean_color_spots ? std::ios::binary : std::ios::in);
        input.imbue(std::locale::classic());
        if (!input) throw std::runtime_error("Unable to read the source OBJ.");
        std::vector<std::string> lines, tails;
        std::vector<size_t> vertex_lines, normal_lines;
        std::vector<Vec3d> vertices, normals;
        std::vector<ColorCleanup::VertexColor> vertex_colors;
        std::vector<Face> faces;
        size_t texture_count = 0;
        std::string line;
        bool source_final_newline = true;
        while (std::getline(input, line)) {
            source_final_newline = !input.eof();
            if ((lines.size() & 4095) == 0) check_cancel();
            size_t at = 0;
            const auto tag = ObjText::next(line, at);
            if (tag == "v" || tag == "vn") {
                Vec3d p;
                if (!ObjText::number(ObjText::next(line, at), p.x()) ||
                    !ObjText::number(ObjText::next(line, at), p.y()) ||
                    !ObjText::number(ObjText::next(line, at), p.z()))
                    throw std::runtime_error("OBJ contains an invalid vertex or normal.");
                if (tag == "v") {
                    if (options.clean_color_spots) vertex_colors.push_back(ColorCleanup::parse(line));
                    vertices.push_back(p); vertex_lines.push_back(lines.size());
                    tails.push_back(line.substr(at));
                } else { normals.push_back(p); normal_lines.push_back(lines.size()); }
            } else if (tag == "vt") {
                ++texture_count;
            } else if (tag == "f") {
                Face f; f.line = lines.size();
                for (Corner& c : f.corners) {
                    const auto token = ObjText::next(line, at);
                    if (token.empty())
                        throw std::runtime_error("Model finishing requires a triangle OBJ.");
                    c.token = std::string(token);
                    const auto slash = token.find('/');
                    c.vertex = obj_index(token.substr(0, slash), vertices.size());
                    if (slash != std::string::npos) {
                        const auto next = token.find('/', slash + 1);
                        const auto uv = token.substr(slash + 1, next == std::string::npos ? next : next - slash - 1);
                        if (!uv.empty()) obj_index(uv, texture_count);
                        if (next != std::string::npos && next + 1 < token.size())
                            c.normal = obj_index(token.substr(next + 1), normals.size());
                    }
                }
                if (!ObjText::next(line, at).empty())
                    throw std::runtime_error("Model finishing requires a triangle OBJ; polygon faces are unchanged.");
                faces.push_back(std::move(f));
            }
            lines.push_back(std::move(line));
            if (vertices.size() > 4000000 || faces.size() > 2000000)
                throw std::runtime_error("This model exceeds the local finishing limit of two million triangles.");
        }
        if (!input.eof()) throw std::runtime_error("The source OBJ could not be read completely.");
        if (vertices.empty() || faces.empty()) throw std::runtime_error("The OBJ has no triangle surface.");
        result.vertices = vertices.size(); result.faces_before = faces.size();
        std::vector<bool> selected(faces.size(), !local_selection);
        for (const size_t index : options.selected_faces) {
            if (index >= faces.size())
                throw std::runtime_error("The selected face index is outside this OBJ. Reload the model and select its surface again.");
            selected[index] = true;
        }
        std::vector<size_t> surface;
        surface.reserve(local_selection ? options.selected_faces.size() : faces.size());
        for (size_t i = 0; i < faces.size(); ++i)
            if (selected[i]) surface.push_back(i);
        const auto original = vertices;
        Vec3d minimum = vertices.front(), maximum = minimum;
        for (const auto& p : vertices) { minimum = minimum.cwiseMin(p); maximum = maximum.cwiseMax(p); }
        const double diagonal = (maximum - minimum).norm();
        if (!std::isfinite(diagonal) || diagonal <= 0) throw std::runtime_error("The model has invalid dimensions.");
        const double degenerate_area_squared = std::pow(diagonal, 4) * 1e-24;
        if (options.repair_mesh) {
            std::set<std::array<size_t, 3>> unique;
            for (size_t i = 0; i < faces.size(); ++i) {
                if ((i & 4095) == 0) check_cancel();
                Face& f = faces[i];
                if (face_normal(f, vertices).squaredNorm() <= degenerate_area_squared) {
                    f.removed = true; ++result.removed_degenerate_faces; continue;
                }
                std::array<size_t, 3> key {f.corners[0].vertex, f.corners[1].vertex, f.corners[2].vertex};
                std::sort(key.begin(), key.end());
                if (!unique.insert(key).second) { f.removed = true; ++result.removed_duplicate_faces; }
            }
        }
        check_cancel();
        // Outside vertices are pinned below. Their topology never participates
        // in local diffusion, so do not build millions of unused hash entries.
        const EdgeMap edges = edges_of(faces, surface, canceled);
        std::vector<std::vector<std::pair<size_t, bool>>> adjacent(options.repair_mesh ? faces.size() : 0);
        std::vector<bool> pinned(vertices.size(), false), open_face(faces.size(), false);
        if (local_selection) {
            // A vertex may move only when every incident face is selected.
            // Pinning the entire unselected one-ring also fixes the shared
            // boundary, so the outside surface remains exactly unchanged.
            std::vector<bool> touched(vertices.size(), false);
            for (size_t i = 0; i < faces.size(); ++i) {
                if ((i & 4095) == 0) check_cancel();
                for (const Corner& c : faces[i].corners) {
                    touched[c.vertex] = true;
                    if (!selected[i]) pinned[c.vertex] = true;
                }
            }
            for (size_t i = 0; i < vertices.size(); ++i)
                if (!touched[i]) pinned[i] = true;
        }
        std::vector<std::vector<size_t>> neighbors(vertices.size());
        std::vector<Vec3d> face_normals(faces.size(), Vec3d::Zero());
        std::vector<double> local_limit(vertices.size(), std::numeric_limits<double>::max());
        for (const size_t i : surface) {
            if ((i & 4095) == 0) check_cancel();
            if (!faces[i].removed) face_normals[i] = face_normal(faces[i], vertices);
        }
        size_t visited_edges = 0;
        for (const auto& item : edges) {
            if ((visited_edges++ & 4095) == 0) check_cancel();
            const size_t a = item.first >> 32, b = item.first & 0xffffffff;
            const Edge& edge = item.second;
            neighbors[a].push_back(b); neighbors[b].push_back(a);
            const double limit = 0.2 * (vertices[a] - vertices[b]).norm();
            local_limit[a] = std::min(local_limit[a], limit);
            local_limit[b] = std::min(local_limit[b], limit);
            if (edge.count != 2) {
                pinned[a] = pinned[b] = true;
                open_face[edge.first] = true;
                if (edge.count > 1) open_face[edge.second] = true;
                if (edge.count == 1) ++result.boundary_edges;
                else ++result.nonmanifold_edges;
                continue;
            }
            if (options.repair_mesh) {
                adjacent[edge.first].emplace_back(edge.second, edge.same_direction);
                adjacent[edge.second].emplace_back(edge.first, edge.same_direction);
            }
            const Vec3d& n1 = face_normals[edge.first];
            const Vec3d& n2 = face_normals[edge.second];
            // A crease above 55 degrees, an open boundary or a nonmanifold
            // junction stays fixed. This does not infer semantic face regions.
            const double dot = n1.dot(n2);
            if (n1.squaredNorm() <= degenerate_area_squared || n2.squaredNorm() <= degenerate_area_squared ||
                dot < 0 ||
                dot * dot < 0.573576436 * 0.573576436 * n1.squaredNorm() * n2.squaredNorm())
                pinned[a] = pinned[b] = true;
        }
        if (options.repair_mesh) {
            std::vector<int> orientation(faces.size(), -1);
            for (size_t start = 0; start < faces.size(); ++start) {
                if (faces[start].removed || orientation[start] != -1) continue;
                check_cancel();
                std::vector<size_t> component {start}; orientation[start] = 0;
                bool closed = true, consistent = true;
                double volume = 0.0;
                for (size_t j = 0; j < component.size(); ++j) {
                    if ((j & 4095) == 0) check_cancel();
                    const size_t i = component[j];
                    const Face& f = faces[i];
                    closed = closed && !open_face[i];
                    volume += (orientation[i] ? -1.0 : 1.0) *
                        (vertices[f.corners[0].vertex] - minimum).dot(
                        (vertices[f.corners[1].vertex] - minimum).cross(vertices[f.corners[2].vertex] - minimum));
                    for (const auto& link : adjacent[i]) {
                        const int expected = orientation[i] ^ int(link.second);
                        if (orientation[link.first] == -1) {
                            orientation[link.first] = expected; component.push_back(link.first);
                        } else if (orientation[link.first] != expected) consistent = false;
                    }
                }
                if (!consistent) throw std::runtime_error("The mesh has conflicting face orientation; use a topology repair tool.");
                for (const size_t i : component) {
                    if (orientation[i] ^ int(closed && volume < 0)) {
                        std::swap(faces[i].corners[1], faces[i].corners[2]);
                        face_normals[i] = -face_normals[i];
                        faces[i].reversed = true; ++result.reversed_faces;
                    }
                }
            }
        }
        result.displacement_limit = diagonal * 0.005 * options.strength;
        if (options.clean_color_spots) {
            std::vector<double> areas(vertices.size(), 0);
            for (size_t i : surface) {
                if ((i & 4095) == 0) check_cancel();
                const double third_area = face_normals[i].norm() / 6;
                for (const Corner& corner : faces[i].corners) areas[corner.vertex] += third_area;
            }
            const auto replacements = ColorCleanup::replacements(vertex_colors, options.cleanup_palette,
                vertices, neighbors, pinned, areas, options.strength, result.cleaned_color_regions, check_cancel);
            std::vector<std::string> original_rgb(vertices.size());
            for (size_t i = 0; i < vertices.size(); ++i) {
                if ((i & 4095) == 0) check_cancel();
                const auto& color = vertex_colors[i];
                original_rgb[i] = lines[vertex_lines[i]].substr(color.begin, color.end - color.begin);
            }
            for (size_t i = 0; i < vertices.size(); ++i) {
                if ((i & 4095) == 0) check_cancel();
                if (replacements[i] == vertices.size()) continue;
                const auto& color = vertex_colors[i];
                lines[vertex_lines[i]].replace(color.begin, color.end - color.begin, original_rgb[replacements[i]]);
                ++result.recolored_vertices;
            }
        }
        if (options.smooth_surface && options.strength > 0) {
            const int iterations = 2 + int(std::ceil(10 * options.strength));
            // Only movable samples need iteration buffers. A cheek selection
            // must not copy/scan a million fixed vertices on every pass.
            std::vector<size_t> active;
            std::vector<Vec3d> directions(vertices.size(), Vec3d::Zero());
            for (const size_t i : surface) {
                if ((i & 4095) == 0) check_cancel();
                if (faces[i].removed) continue;
                for (const Corner& c : faces[i].corners) directions[c.vertex] += face_normals[i];
            }
            for (size_t i = 0; i < vertices.size(); ++i) {
                const double length = directions[i].norm();
                if (length > 0) directions[i] /= length;
                else pinned[i] = true;
                local_limit[i] = std::min(local_limit[i], result.displacement_limit);
                if (!pinned[i] && !neighbors[i].empty()) active.push_back(i);
            }
            std::vector<Vec3d> next(active.size());
            for (int iteration = 0; iteration < iterations; ++iteration) {
                // Alternating diffusion reduces the systematic shrink of a
                // positive-only Laplacian step. Every vertex also has an
                // absolute displacement cap relative to the source model.
                for (const double factor : {0.5, -0.53}) {
                    for (size_t index = 0; index < active.size(); ++index) {
                        if ((index & 4095) == 0) check_cancel();
                        const size_t i = active[index];
                        next[index] = vertices[i];
                        Vec3d average = Vec3d::Zero();
                        for (const size_t j : neighbors[i]) average += vertices[j];
                        // Smooth surface height rather than sliding samples
                        // sideways across an irregular triangulation/texture.
                        const Vec3d laplacian = average / double(neighbors[i].size()) - vertices[i];
                        next[index] += factor * laplacian.dot(directions[i]) * directions[i];
                        Vec3d movement = next[index] - original[i];
                        const double length = movement.norm();
                        if (length > local_limit[i])
                            next[index] = original[i] + movement * (local_limit[i] / length);
                    }
                    for (size_t index = 0; index < active.size(); ++index)
                        vertices[active[index]] = next[index];
                }
            }
            if (local_selection) {
                // Fade the displacement across the first three graph rings.
                // Pinned borders remain exact; the transition does not create
                // a hard strength step or blur colors/UVs across the brush edge.
                std::vector<unsigned char> distance(vertices.size(), 3);
                std::queue<size_t> frontier;
                for (const size_t i : active)
                    for (const size_t j : neighbors[i]) if (pinned[j]) {
                        distance[i] = 1; frontier.push(i); break;
                    }
                while (!frontier.empty()) {
                    check_cancel();
                    const size_t i = frontier.front(); frontier.pop();
                    for (const size_t j : neighbors[i])
                        if (!pinned[j] && distance[j] > distance[i] + 1) {
                            distance[j] = distance[i] + 1; frontier.push(j);
                        }
                }
                for (const size_t i : active) {
                    const double t = distance[i] / 3.0;
                    vertices[i] = original[i] + (t * t * (3.0 - 2.0 * t)) * (vertices[i] - original[i]);
                }
            }
            // Retain the original geometry only around a fold/collapse. A
            // restored vertex can affect adjacent faces, so revisit those
            // until every affected triangle is safe. Each vertex is restored
            // at most once; unrelated smooth regions keep their improvement.
            auto folded = [&](size_t i) {
                if (faces[i].removed || face_normals[i].squaredNorm() <= degenerate_area_squared) return false;
                const Vec3d after = face_normal(faces[i], vertices);
                return after.squaredNorm() <= degenerate_area_squared || face_normals[i].dot(after) <= 0;
            };
            std::vector<size_t> pending;
            std::vector<bool> queued(faces.size(), false);
            for (const size_t i : surface) {
                if ((i & 4095) == 0) check_cancel();
                if (folded(i)) { pending.push_back(i); queued[i] = true; }
            }
            if (!pending.empty()) {
                std::vector<std::vector<size_t>> incident(vertices.size());
                for (const size_t i : surface)
                    if (!faces[i].removed) for (const Corner& c : faces[i].corners) incident[c.vertex].push_back(i);
                std::vector<bool> restored(vertices.size(), false);
                while (!pending.empty()) {
                    check_cancel();
                    const size_t i = pending.back(); pending.pop_back(); queued[i] = false;
                    if (!folded(i)) continue;
                    for (const Corner& c : faces[i].corners) {
                        const size_t v = c.vertex;
                        if (restored[v]) continue;
                        restored[v] = true;
                        if ((vertices[v] - original[v]).squaredNorm() == 0) continue;
                        vertices[v] = original[v]; ++result.protected_vertices;
                        for (const size_t neighbor : incident[v]) if (!queued[neighbor]) {
                            pending.push_back(neighbor); queued[neighbor] = true;
                        }
                    }
                }
            }
        }
        for (size_t i = 0; i < vertices.size(); ++i) {
            const double displacement = (vertices[i] - original[i]).norm();
            if (displacement > diagonal * 1e-12) ++result.moved_vertices;
            result.max_displacement = std::max(result.max_displacement, displacement);
        }
        minimum = maximum = vertices.front();
        for (const Vec3d& v : vertices) { minimum = minimum.cwiseMin(v); maximum = maximum.cwiseMax(v); }
        const Vec3d extent = maximum - minimum;
        result.dimensions = {extent.x(), extent.y(), extent.z()};
        result.faces_after = faces.size() - result.removed_degenerate_faces - result.removed_duplicate_faces;
        if (result.faces_after == 0) throw std::runtime_error("Repair would remove every face; the source model is preserved.");
        check_cancel();
        if (result.changed() && !options.clean_color_spots) {
            std::vector<Vec3d> adjusted_normals(normals.size(), Vec3d::Zero());
            std::vector<bool> preserve_normal(normals.size(), false), affected_normal(normals.size(), false);
            for (size_t i = 0; i < faces.size(); ++i) {
                if ((i & 4095) == 0) check_cancel();
                const Face& f = faces[i];
                if (f.removed) { lines[f.line].clear(); continue; }
                if (f.reversed) {
                    lines[f.line] = "f " + f.corners[0].token + " " + f.corners[1].token + " " + f.corners[2].token;
                }
                if (normals.empty()) continue;
                const Vec3d n = face_normal(f, vertices);
                const bool moved = local_selection && !normals.empty() && std::any_of(f.corners.begin(), f.corners.end(), [&](const Corner& c) {
                    return (vertices[c.vertex] - original[c.vertex]).squaredNorm() > 0;
                });
                for (const Corner& c : f.corners) if (c.normal >= 0) {
                    adjusted_normals[c.normal] += n;
                    if (local_selection && !selected[i]) preserve_normal[c.normal] = true;
                    if (moved || f.reversed) affected_normal[c.normal] = true;
                }
            }
            for (size_t i = 0; i < vertices.size(); ++i) {
                if ((i & 4095) == 0) check_cancel();
                if ((vertices[i] - original[i]).squaredNorm() == 0) continue;
                std::ostringstream out; out.imbue(std::locale::classic());
                out << std::setprecision(17) << "v " << vertices[i].x() << ' ' << vertices[i].y() << ' ' << vertices[i].z() << tails[i];
                lines[vertex_lines[i]] = out.str();
            }
            for (size_t i = 0; i < normals.size(); ++i) {
                // Explicit normals can be shared across the selection edge.
                // Keep those untouched rather than changing outside shading.
                if (local_selection && (preserve_normal[i] || !affected_normal[i])) continue;
                if (adjusted_normals[i].squaredNorm() <= 0) continue;
                const Vec3d n = adjusted_normals[i].normalized();
                std::ostringstream out; out.imbue(std::locale::classic());
                out << std::setprecision(17) << "vn " << n.x() << ' ' << n.y() << ' ' << n.z();
                lines[normal_lines[i]] = out.str();
            }
        }
        boost::filesystem::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Unable to create a new model version.");
        owns_temporary = true;
        for (size_t i = 0; i < lines.size(); ++i) {
            if ((i & 4095) == 0) check_cancel();
            if (!lines[i].empty() || options.clean_color_spots) {
                output << lines[i];
                if (!options.clean_color_spots || i + 1 < lines.size() || source_final_newline) output << '\n';
            }
        }
        output.close();
        if (!output) throw std::runtime_error("The edited OBJ could not be written completely.");
        check_cancel();
        if (file_hash(source, canceled) != result.source_sha256)
            throw std::runtime_error("The source model changed during finishing. Please reload it.");
        result.output_sha256 = file_hash(temporary, canceled);
        boost::filesystem::rename(temporary, destination);
        owns_temporary = false;
        result.success = true;
    } catch (const Canceled&) {
        result.canceled = true;
    } catch (const std::exception& error) {
        result.error = error.what();
    }
    if (owns_temporary) {
        boost::system::error_code ignored;
        boost::filesystem::remove(temporary, ignored);
    }
    return result;
}
ModelFinishingResult finish_model_artifact(const boost::filesystem::path& source,
    const boost::filesystem::path& destination, const ModelFinishingOptions& options,
    const std::function<bool()>& canceled)
{
    if (options.recolor_selected)
        return finish_recolored_artifact(source, destination, options, canceled);
    if (model_artifact_format(source) == "obj" && model_artifact_format(destination) == "obj")
        return finish_model_obj(source, destination, options, canceled);
    ModelFinishingResult result;
    auto input_obj = destination; input_obj += ".source.obj";
    auto output_obj = destination; output_obj += ".edited.obj";
    bool owns_input = false, owns_output = false, owns_destination = false;
    try {
        if (boost::filesystem::exists(destination) || boost::filesystem::exists(input_obj) || boost::filesystem::exists(output_obj))
            throw std::runtime_error("The output already exists; choose a new model version.");
        if (canceled && canceled()) throw Canceled {};
        const auto source_hash = file_hash(source, canceled);
        TriangleMesh mesh; ObjInfo colors; std::string error;
        if (!load_model_artifact(source, mesh, colors, error) ||
            !write_model_artifact(input_obj, mesh.its, colors.vertex_colors, error))
            throw std::runtime_error(error);
        owns_input = true;
        result = finish_model_obj(input_obj, output_obj, options, canceled);
        owns_output = result.success;
        result.source_sha256 = source_hash;
        if (result.success) {
            result.success = false;
            if (canceled && canceled()) throw Canceled {};
            if (!load_model_artifact(output_obj, mesh, colors, error)) throw std::runtime_error(error);
            if (file_hash(source, canceled) != source_hash) throw std::runtime_error("The source model changed during finishing. Please reload it.");
            if (!write_model_artifact(destination, mesh.its, colors.vertex_colors, error)) throw std::runtime_error(error);
            owns_destination = true;
            result.output_sha256 = file_hash(destination, canceled);
            result.success = true;
        }
    } catch (const Canceled&) { result.success = false; result.canceled = true; }
      catch (const std::exception& error) { result.success = false; result.error = error.what(); }
    boost::system::error_code ignored;
    if (owns_input) boost::filesystem::remove(input_obj, ignored);
    if (owns_output) boost::filesystem::remove(output_obj, ignored);
    if (owns_destination && !result.success) boost::filesystem::remove(destination, ignored);
    return result;
}
} // namespace Slic3r::AI
