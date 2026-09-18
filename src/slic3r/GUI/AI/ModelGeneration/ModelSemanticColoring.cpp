#include "ModelSemanticColoring.hpp"
#include "ModelColorPreviewShader.hpp"
#include "ModelPreviewNormals.hpp"
#include "slic3r/AI/ModelGeneration/SemanticColoring/MediaPipeRegionRecognizers.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <thread>

namespace Slic3r::GUI {
namespace SC = AI::SemanticColoring;
namespace {
// Freeze provider selection with the request, so edits to the configuration
// cannot be read by a stale worker after the user has selected another model.
std::string provider_configuration(const std::filesystem::path& runtime)
{
    const auto file = runtime / "providers.json";
    std::error_code error;
    const auto bytes = std::filesystem::file_size(file, error);
    if (error == std::errc::no_such_file_or_directory) return "{}";
    if (error || bytes > 65536) return "invalid local provider configuration";
    std::ifstream input(file, std::ios::binary);
    if (!input) return "unreadable local provider configuration";
    return std::string(std::istreambuf_iterator<char>(input), {});
}

}

GLModel::Geometry build_semantic_colored_geometry(const SC::MeshSnapshot& source, const SC::FaceColors& paint,
                                                   const SC::SubfaceColors& subfaces, const SC::Cancel& cancel)
{
    GLModel::Geometry geometry;
    geometry.format = {GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3T2};
    const auto& mesh = source.mesh;
    const auto normals = ModelPreviewNormals::corner_normals(mesh);
    std::vector<int> indices(mesh.indices.size(), -1);
    for (size_t i = 0; i < paint.size(); ++i)
        if (paint[i].first < indices.size()) indices[paint[i].first] = int(i);
    std::map<size_t, std::map<SC::SubfacePath, SC::Color>> children;
    for (const SC::SubfaceColor& item : subfaces) {
        if (item.face_id >= mesh.indices.size() || item.path.depth == 0 || item.path.depth > 2 ||
            unsigned(item.path.value) >= (1u << (2u * item.path.depth))) return {};
        children[item.face_id][item.path] = item.color;
    }
    geometry.reserve_vertices((mesh.indices.size() + subfaces.size() * 3) * 3);
    geometry.reserve_indices((mesh.indices.size() + subfaces.size() * 3) * 3);
    const bool vertex = source.vertex_colors.size() == mesh.vertices.size();
    const bool face = source.face_colors.size() == mesh.indices.size();
    const RGBA fallback {.7f, .7f, .7f, 1.f};
    for (size_t f = 0; f < mesh.indices.size(); ++f) {
        if ((f & 4095) == 0 && cancel && cancel()) return {};
        const auto& triangle = mesh.indices[f];
        std::vector<SC::SubfacePath> leaves;
        const auto child_colors = children.find(f);
        if (child_colors == children.end()) {
            leaves.push_back({0, 0});
        } else {
            std::array<bool, 4> split_child {};
            for (const auto& item : child_colors->second)
                if (item.first.depth == 2) split_child[item.first.value >> 2] = true;
            for (uint8_t child = 0; child < 4; ++child) {
                if (split_child[child])
                    for (uint8_t grandchild = 0; grandchild < 4; ++grandchild)
                        leaves.push_back({2, uint8_t((child << 2) | grandchild)});
                else
                    leaves.push_back({1, child});
            }
        }
        for (const SC::SubfacePath& leaf : leaves) {
            std::array<SC::Barycentric, 3> barycentric;
            if (leaf.depth == 0)
                barycentric = {{{1.f,0.f,0.f}, {0.f,1.f,0.f}, {0.f,0.f,1.f}}};
            else if (!SC::subface_vertices(leaf, barycentric)) return {};
            const SC::Color* leaf_color = nullptr;
            if (child_colors != children.end()) {
                auto found = child_colors->second.find(leaf);
                if (found == child_colors->second.end() && leaf.depth == 2)
                    found = child_colors->second.find({1, uint8_t(leaf.value >> 2)});
                if (found != child_colors->second.end()) leaf_color = &found->second;
            }
            if (leaf_color == nullptr && indices[f] >= 0) leaf_color = &paint[size_t(indices[f])].second;
            const unsigned base = unsigned(geometry.vertices_count());
            for (const SC::Barycentric& bary : barycentric) {
                Vec3f position = Vec3f::Zero(), normal = Vec3f::Zero();
                RGBA original {0.f,0.f,0.f,0.f};
                for (size_t corner = 0; corner < 3; ++corner) {
                    position += bary[corner] * mesh.vertices[triangle[corner]];
                    normal += bary[corner] * normals[f * 3 + corner];
                    const auto& color = vertex ? source.vertex_colors[triangle[corner]] :
                        face ? source.face_colors[f] : fallback;
                    for (size_t channel = 0; channel < 4; ++channel)
                        original[channel] += bary[corner] * color[channel];
                }
                if (normal.squaredNorm() > 1e-12f) normal.normalize();
                const SC::Color rgb = leaf_color == nullptr
                    ? SC::Color {original[0], original[1], original[2]} : *leaf_color;
                geometry.add_vertex(position, normal, Vec2f(float(preview_rgb8(rgb[0], rgb[1], rgb[2])),
                    leaf_color == nullptr ? original[3] : -1.f));
            }
            geometry.add_triangle(base, base + 1, base + 2);
        }
    }
    return geometry;
}

struct ModelSemanticColoring::Impl {
    struct Request {
        std::shared_ptr<const Snapshot> source;
        std::vector<Color> palette, targets, portrait;
        FaceColors manual;
        std::string configuration;
        uint64_t generation {0};
    } request;
    struct Job {
        Request request;
        std::atomic<bool> canceled {false}, done {false};
        std::atomic<int> progress {0};
        std::unique_ptr<Result> result;
    };
    std::filesystem::path runtime, cache;
    SC::RegionRecognizers providers;
    std::string providers_configuration;
    std::shared_ptr<const SC::Analysis> cached_analysis;
    std::shared_ptr<Job> job;
    std::thread worker;
    uint64_t generation {0};
    bool wanted {false}, pending {false};

    void start() {
        if (!pending || job || !wanted) return;
        pending = false;
        job = std::make_shared<Job>(); job->request = request;
        const auto task = job;
        try {
        worker = std::thread([this, task] {
            const auto started = std::chrono::steady_clock::now();
            auto result = std::make_unique<Result>();
            const SC::Cancel cancel = [task] { return task->canceled.load(); };
            try {
                if (!providers.body || !providers.face || providers_configuration != task->request.configuration) {
                    std::string body_id = "mediapipe.cpu.v1", face_id = body_id;
                    const auto doc = nlohmann::json::parse(task->request.configuration);
                    body_id = doc.value("body_provider", body_id);
                    face_id = doc.value("face_provider", face_id);
                    providers = SC::create_region_recognizers(body_id, face_id, runtime);
                    providers_configuration = task->request.configuration;
                    cached_analysis.reset();
                }
                if (!providers.error.empty()) throw std::runtime_error(providers.error);
                if (!providers.body || !providers.face) throw std::runtime_error("Local region recognizers unavailable");
                const auto& source = *task->request.source;
                const auto key = SC::analysis_cache_key(source, providers.body->identity(), providers.face->identity());
                auto analysis = std::make_shared<SC::Analysis>();
                const auto cache_file = cache / (key + ".json");
                if (cached_analysis && cached_analysis->signature == key) {
                    analysis = std::make_shared<SC::Analysis>(*cached_analysis);
                    result->cache_hit = true;
                } else {
                    std::error_code ec;
                    const auto bytes = std::filesystem::file_size(cache_file, ec);
                    if (!ec && bytes <= 128ULL * 1024 * 1024) {
                        std::ifstream input(cache_file, std::ios::binary);
                        const auto doc = nlohmann::json::parse(input, nullptr, false);
                        std::string error;
                        result->cache_hit = SC::decode_analysis(doc, source, providers.body->identity(), providers.face->identity(), *analysis, error);
                    }
                }
                if (!result->cache_hit && !cancel()) {
                    *analysis = SC::analyze(source, *providers.body, *providers.face, cancel,
                        [task](int value, const std::string&) { task->progress.store(std::clamp(value, 0, 95)); });
                    if (!analysis->canceled && analysis->error.empty() && !cancel()) {
                        std::error_code ec;
                        std::filesystem::create_directories(cache, ec);
                        if (!ec) {
                            auto temporary = cache_file; temporary += ".tmp";
                            { std::ofstream output(temporary, std::ios::binary | std::ios::trunc); output << SC::encode_analysis(*analysis); }
                            std::filesystem::rename(temporary, cache_file, ec);
                            if (ec) std::filesystem::remove(temporary, ec);
                        }
                    }
                }
                if (!cancel()) {
                    result->analysis = analysis;
                    result->person_detected = analysis->person_detected;
                    result->error = analysis->error;
                    if (analysis->error.empty() && !analysis->canceled) {
                        cached_analysis = analysis;
                        const auto source_automatic = SC::map_palette(
                            source, *analysis, task->request.palette, task->request.portrait);
                        result->automatic = SC::remap_palette_targets(source_automatic,
                            task->request.palette, task->request.targets);
                        SC::SubfaceBudgetResult source_subfaces;
                        std::string subface_error;
                        if (!SC::map_subface_palette(source, *analysis, source_automatic, task->request.palette,
                                task->request.portrait, {}, source_subfaces, subface_error))
                            throw std::runtime_error(subface_error);
                        if (task->request.palette.size() <= 4) {
                            // Temporary limited-palette safeguard: eye detail
                            // candidates are individually colored from tiny
                            // raster samples and can introduce white/skin/iris
                            // speckles. Keep whole-face eye mapping and all
                            // non-eye boundary repairs (hair, clothing, skin).
                            source_subfaces.accepted.erase(std::remove_if(
                                source_subfaces.accepted.begin(), source_subfaces.accepted.end(),
                                [&analysis = *analysis](const SC::SubfaceColor& item) {
                                    if (item.face_id >= analysis.face_labels.size()) return false;
                                    if (analysis.face_labels[item.face_id] == SC::Label::EyeSclera ||
                                        analysis.face_labels[item.face_id] == SC::Label::Iris ||
                                        analysis.face_labels[item.face_id] == SC::Label::Eyebrow)
                                        return true;
                                    return std::any_of(analysis.subface_labels.begin(), analysis.subface_labels.end(),
                                        [&item](const SC::SubfaceLabelEvidence& evidence) {
                                            return evidence.face_id == item.face_id && evidence.path == item.path &&
                                                (evidence.label == SC::Label::EyeSclera ||
                                                 evidence.label == SC::Label::Iris ||
                                                 evidence.label == SC::Label::Eyebrow);
                                        });
                                }), source_subfaces.accepted.end());
                        }
                        result->automatic_subfaces = SC::remap_subface_palette_targets(
                            source_subfaces.accepted, task->request.palette, task->request.targets);
                        result->subface_added_triangles = source_subfaces.added_triangles;
                        result->subface_rejected_candidates = source_subfaces.rejected_candidates;
                        if (!result->automatic.empty() || !result->automatic_subfaces.empty())
                            result->geometry = build_semantic_colored_geometry(source,
                                SC::compose(result->automatic, task->request.manual, true),
                                SC::compose_subfaces(result->automatic_subfaces, task->request.manual, true), cancel);
                    }
                }
            } catch (const std::exception& error) { result->error = error.what(); }
              catch (...) { result->error = "Unknown local region recognition failure"; }
            result->elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            task->result = std::move(result);
            task->done.store(true);
        });
        } catch (const std::exception& error) {
            task->result = std::make_unique<Result>();
            task->result->error = error.what();
            task->done.store(true);
        }
    }
};

ModelSemanticColoring::ModelSemanticColoring(std::filesystem::path runtime, std::filesystem::path cache)
    : m_impl(std::make_unique<Impl>()) { m_impl->runtime = std::move(runtime); m_impl->cache = std::move(cache); }
ModelSemanticColoring::~ModelSemanticColoring() { cancel(); if (m_impl->worker.joinable()) m_impl->worker.join(); }
bool ModelSemanticColoring::request(std::shared_ptr<const Snapshot> source, std::vector<Color> palette, std::vector<Color> targets,
                                   std::vector<Color> portrait_card, FaceColors manual)
{
    auto& state = *m_impl;
    auto configuration = provider_configuration(state.runtime);
    if (state.wanted && state.request.source == source && state.request.palette == palette &&
        state.request.targets == targets && state.request.portrait == portrait_card &&
        state.request.manual == manual && state.request.configuration == configuration) return false;
    if (state.job) state.job->canceled.store(true);
    state.request = {std::move(source), std::move(palette), std::move(targets), std::move(portrait_card),
        std::move(manual), std::move(configuration), ++state.generation};
    state.wanted = true; state.pending = true; state.start(); return true;
}
void ModelSemanticColoring::cancel() {
    auto& state = *m_impl;
    state.wanted = false; state.pending = false; ++state.generation;
    if (state.job) state.job->canceled.store(true);
}
std::unique_ptr<ModelSemanticColoring::Result> ModelSemanticColoring::poll() {
    auto& state = *m_impl;
    std::unique_ptr<Result> result;
    if (state.job && state.job->done.load()) {
        if (state.worker.joinable()) state.worker.join();
        if (!state.job->canceled.load() && state.wanted && state.job->request.generation == state.generation)
            result = std::move(state.job->result);
        state.job.reset();
    }
    state.start(); return result;
}
bool ModelSemanticColoring::busy() const { return m_impl->wanted && (m_impl->pending || bool(m_impl->job)); }
int ModelSemanticColoring::progress() const { return m_impl->job ? m_impl->job->progress.load() : 0; }
} // namespace Slic3r::GUI
