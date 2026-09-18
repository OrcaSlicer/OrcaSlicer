#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "slic3r/AI/ModelGeneration/SemanticColoring/SemanticColoring.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

using namespace Slic3r;
using namespace Slic3r::AI::SemanticColoring;
using Catch::Matchers::WithinAbs;

namespace {
MeshSnapshot triangles(const std::vector<Color>& colors)
{
    MeshSnapshot source;
    for (size_t i = 0; i < colors.size(); ++i) {
        const float x = float(i) * 3;
        const int base = int(source.mesh.vertices.size());
        source.mesh.vertices.emplace_back(x - 1.f, 0.f, -1.f);
        source.mesh.vertices.emplace_back(x + 1.f, 0.f, -1.f);
        source.mesh.vertices.emplace_back(x, 0.f, 1.f);
        source.mesh.indices.emplace_back(base, base + 1, base + 2);
        for (int corner = 0; corner < 3; ++corner)
            source.vertex_colors.push_back({colors[i][0], colors[i][1], colors[i][2], 1.f});
    }
    source.geometry_id = "fixture-canonical-geometry";
    source.content_id = content_fingerprint(source);
    return source;
}
MeshSnapshot connected_triangles(const std::vector<Color>& colors)
{
    auto source = triangles(colors);
    source.vertex_colors.clear();
    for (size_t id = 0; id < colors.size(); ++id) {
        for (int corner = 0; corner < 3; ++corner)
            source.mesh.vertices[id * 3 + corner] = source.mesh.vertices[corner];
        source.mesh.indices[id][0] = 0;
        source.face_colors.push_back({colors[id][0], colors[id][1], colors[id][2], 1.f});
    }
    source.content_id = content_fingerprint(source);
    return source;
}
MeshSnapshot surface_strip(const std::vector<Color>& colors)
{
    MeshSnapshot source;
    for (size_t vertex = 0; vertex < colors.size() + 2; ++vertex)
        source.mesh.vertices.emplace_back(float(vertex / 2), 0.f, float(vertex % 2));
    for (size_t face = 0; face < colors.size(); ++face) {
        const int first = int(face);
        source.mesh.indices.emplace_back(first, first + (face % 2 ? 2 : 1), first + (face % 2 ? 1 : 2));
        source.face_colors.push_back({colors[face][0], colors[face][1], colors[face][2], 1.f});
    }
    source.geometry_id = "fixture-connected-surface-strip";
    source.content_id = content_fingerprint(source);
    return source;
}
MeshSnapshot hair_edge_strip(const std::vector<Color>& colors, float scale = .02f)
{
    auto source = surface_strip(colors);
    for (auto& vertex : source.mesh.vertices) vertex *= scale;
    // A small surface patch on a larger model; the disconnected triangle makes
    // the spatial repair limit realistic without helping connectivity.
    const int remote = int(source.mesh.vertices.size());
    source.mesh.vertices.emplace_back(100.f, 0.f, 0.f);
    source.mesh.vertices.emplace_back(100.f, 1.f, 0.f);
    source.mesh.vertices.emplace_back(100.f, 0.f, 1.f);
    source.mesh.indices.emplace_back(remote, remote + 1, remote + 2);
    source.face_colors.push_back({.9f,.9f,.9f,1.f});
    source.content_id = content_fingerprint(source);
    return source;
}
Analysis labeled(const MeshSnapshot& source, const std::vector<Label>& labels)
{
    Analysis result;
    result.geometry_id = source.geometry_id; result.content_id = source.content_id;
    result.body_identity = "body-fixture-v1"; result.face_identity = "face-fixture-v1";
    result.signature = analysis_cache_key(source, result.body_identity, result.face_identity);
    result.face_labels = labels; result.face_confidence.assign(labels.size(), .95f);
    result.person_detected = true; result.rendered_views = 8; result.face_views = 1;
    result.observed_faces = result.reliable_faces = labels.size();
    return result;
}
Analysis hair_edge_labels(const MeshSnapshot& source, std::vector<Label> labels)
{
    labels.push_back(Label::Background);
    auto analysis = labeled(source, labels);
    analysis.face_confidence.assign(labels.size(), .50f);
    analysis.face_confidence.front() = .95f;
    analysis.face_confidence.back() = .95f;
    return analysis;
}
std::vector<Color> portrait_card()
{
    return {{247.f/255,226.f/255,218.f/255}, {40.f/255,38.f/255,41.f/255},
        {246.f/255,247.f/255,249.f/255}, {234.f/255,154.f/255,146.f/255},
        {102.f/255,140.f/255,182.f/255}, {149.f/255,139.f/255,134.f/255}};
}
class BodyFixture final : public IBodyRegionRecognizer {
public:
    size_t calls {0}; bool alternate {false}; float certainty {.95f};
    Label primary {Label::FaceSkin}, alternate_label {Label::Clothes}; std::string failure;
    std::string identity() const override { return "body-fixture-v1"; }
    Prediction predict(const RGBImage& image, const Cancel&) override {
        ++calls;
        Prediction prediction;
        prediction.error = failure;
        prediction.labels.assign(size_t(image.width) * image.height,
            alternate && calls > 4 ? alternate_label : primary);
        prediction.confidence.assign(prediction.labels.size(), certainty);
        prediction.person_detected = true;
        return prediction;
    }
};
class FaceFixture final : public IFaceRegionRecognizer {
public:
    size_t calls {0}; bool detects_face {true}; Label detail {Label::Unknown};
    std::vector<std::pair<int, int>> image_sizes;
    std::vector<Label> sequence;
    std::string identity() const override { return "face-fixture-v1"; }
    Prediction predict(const RGBImage& image, const Cancel&) override {
        ++calls;
        image_sizes.emplace_back(image.width, image.height);
        Prediction prediction;
        const Label current = sequence.empty() ? detail : sequence[std::min(calls - 1, sequence.size() - 1)];
        prediction.labels.assign(size_t(image.width) * image.height, current);
        prediction.confidence.assign(prediction.labels.size(), .96f);
        prediction.face_detected = detects_face;
        return prediction;
    }
};
class SplitEyeFixture final : public IFaceRegionRecognizer {
public:
    std::string identity() const override { return "split-eye-fixture-v1"; }
    Prediction predict(const RGBImage& image, const Cancel&) override {
        Prediction prediction;
        prediction.labels.resize(size_t(image.width) * image.height);
        prediction.confidence.assign(prediction.labels.size(), .96f);
        for (int y = 0; y < image.height; ++y)
            for (int x = 0; x < image.width; ++x)
                prediction.labels[size_t(y) * image.width + x] =
                    x < image.width / 2 ? Label::EyeSclera : Label::Iris;
        prediction.face_detected = true;
        return prediction;
    }
};
}

TEST_CASE("Canonical semantic rendering resolves the closest original face without averaging occlusion", "[SemanticColoring]")
{
    auto source = triangles({{0,1,0}, {1,0,0}});
    for (int corner = 0; corner < 3; ++corner) {
        source.mesh.vertices[3 + corner] = source.mesh.vertices[corner];
        source.mesh.vertices[3 + corner].y() = -.4f;
    }
    const auto view = render_view(source, 0, 64);
    REQUIRE(view.error.empty());
    REQUIRE(view.image.valid());
    const size_t center = 32 * 64 + 32;
    REQUIRE(view.face_ids[center] == 1);
    CHECK(view.image.pixels[center * 3] == 255);
    CHECK(view.image.pixels[center * 3 + 1] == 0);
    CHECK(view.image.pixels[center * 3 + 2] == 0);
    CHECK(view.face_ids[0] == std::numeric_limits<uint32_t>::max());
}

TEST_CASE("Semantic rendering supports face colors without modifying the source", "[SemanticColoring]")
{
    auto source = triangles({{.2f,.4f,.8f}});
    source.vertex_colors.clear(); source.face_colors.push_back({.2f,.4f,.8f,1.f});
    const auto before = content_fingerprint(source);
    const auto view = render_view(source, 0, 32);
    REQUIRE(view.error.empty());
    const size_t center = 16 * 32 + 16;
    CHECK(view.image.pixels[center * 3] == 51);
    CHECK(view.image.pixels[center * 3 + 1] == 102);
    CHECK(view.image.pixels[center * 3 + 2] == 204);
    CHECK(content_fingerprint(source) == before);
    CHECK_FALSE(before.empty());
}

TEST_CASE("Raster barycentric coordinates address the same sixteen midpoint leaves as persistence",
          "[SemanticColoring][SubfaceColor]")
{
    const auto source = triangles({{.2f,.4f,.8f}});
    const auto view = render_view(source, 0, 64);
    REQUIRE(view.error.empty());
    const size_t center = 32 * 64 + 32;
    REQUIRE(view.face_ids[center] == 0);
    CHECK_THAT(view.barycentric[center][0] + view.barycentric[center][1] + view.barycentric[center][2],
               WithinAbs(1.f, 1e-5f));

    for (uint8_t value = 0; value < 16; ++value) {
        const SubfacePath expected {2, value};
        std::array<Barycentric, 3> vertices;
        REQUIRE(subface_vertices(expected, vertices));
        Barycentric centroid {};
        for (const auto& vertex : vertices)
            for (size_t channel = 0; channel < 3; ++channel)
                centroid[channel] += vertex[channel] / 3.f;
        SubfacePath located;
        REQUIRE(locate_subface(centroid, 2, located));
        CHECK(located == expected);
    }
}

TEST_CASE("Independent eye masks replace broad skin predictions without model-specific landmarks", "[SemanticColoring][Regression][EyeColor]")
{
    const auto source = triangles({{.7f,.6f,.55f}});
    for (Label detail : {Label::EyeSclera, Label::Iris, Label::Eyebrow}) {
        BodyFixture body; FaceFixture face; face.detail = detail;
        const auto analysis = analyze(source, body, face);
        REQUIRE(analysis.error.empty());
        REQUIRE(analysis.face_labels.size() == 1);
        CHECK(analysis.face_labels.front() == detail);
        CHECK(analysis.face_confidence.front() >= minimum_confidence);
        Analysis restored; std::string error;
        REQUIRE(decode_analysis(encode_analysis(analysis), source, body.identity(), face.identity(), restored, error));
        CHECK(restored.face_labels == analysis.face_labels);
        CHECK(restored.subface_labels == analysis.subface_labels);
        REQUIRE_FALSE(analysis.subface_labels.empty());
        CHECK(std::all_of(analysis.subface_labels.begin(), analysis.subface_labels.end(), [detail](const auto& item) {
            return item.path.depth == 2 && item.label == detail && item.confidence >= minimum_confidence;
        }));
    }
}

TEST_CASE("A clear brow view survives Unknown side views without weakening eye conflict rules",
          "[SemanticColoring][Regression][EyeColor][EyebrowColor]")
{
    const auto source = triangles({{.7f,.6f,.55f}});
    for (Label detail : {Label::EyeSclera, Label::Iris}) {
        BodyFixture body; FaceFixture face;
        face.sequence = {detail, Label::Unknown, Label::Unknown, Label::Unknown,
                         Label::Unknown, Label::Unknown, Label::Unknown, Label::Unknown};
        const auto analysis = analyze(source, body, face);
        REQUIRE(analysis.error.empty());
        REQUIRE(face.calls >= 2);
        CHECK(analysis.face_labels.front() == Label::FaceSkin);
    }

    BodyFixture brow_body; FaceFixture brow;
    brow.sequence = {Label::Eyebrow, Label::Unknown, Label::Unknown, Label::Unknown,
                     Label::Unknown, Label::Unknown, Label::Unknown, Label::Unknown};
    const auto brow_analysis = analyze(source, brow_body, brow);
    REQUIRE(brow_analysis.error.empty());
    REQUIRE(brow.calls >= 2);
    CHECK(brow_analysis.face_labels.front() == Label::Eyebrow);
    CHECK(brow_analysis.face_confidence.front() >= minimum_confidence);

    BodyFixture body; FaceFixture conflict;
    conflict.sequence = {Label::EyeSclera, Label::Iris, Label::EyeSclera, Label::Iris,
                         Label::EyeSclera, Label::Iris, Label::EyeSclera, Label::Iris};
    const auto analysis = analyze(source, body, conflict);
    REQUIRE(analysis.error.empty());
    REQUIRE(conflict.calls >= 2);
    CHECK(analysis.face_labels.front() == Label::FaceSkin);
}

TEST_CASE("Portrait eyebrows use one neutral midtone without coloring adjacent skin", "[SemanticColoring][Regression][EyebrowColor]")
{
    const auto source = connected_triangles({{.16f,.11f,.09f}, {.20f,.14f,.11f}, {.74f,.57f,.47f}});
    const auto analysis = labeled(source, {Label::Eyebrow, Label::Eyebrow, Label::FaceSkin});
    const auto card = portrait_card();
    const auto mapped = map_palette(source, analysis, card, card);
    REQUIRE(mapped.size() == 3);
    CHECK(mapped[0].second == card[5]);
    CHECK(mapped[1].second == card[5]);
    CHECK(mapped[2].second == card[0]);
}

TEST_CASE("Eyebrow overlays preserve the surrounding face-skin material topology", "[SemanticColoring][Regression][EyebrowColor]")
{
    const auto source = connected_triangles({
        {.72f,.55f,.46f}, {.18f,.12f,.10f}, {.74f,.57f,.47f}, {.76f,.59f,.49f}});
    const auto card = portrait_card();
    const auto baseline = map_palette(source,
        labeled(source, {Label::FaceSkin, Label::FaceSkin, Label::FaceSkin, Label::FaceSkin}), card, card);
    const auto candidate = map_palette(source,
        labeled(source, {Label::FaceSkin, Label::Eyebrow, Label::FaceSkin, Label::FaceSkin}), card, card);
    std::map<size_t, Color> old_colors, new_colors;
    for (const auto& entry : baseline) old_colors[entry.first] = entry.second;
    for (const auto& entry : candidate) new_colors[entry.first] = entry.second;
    REQUIRE(new_colors.at(1) == card[5]);
    for (size_t face : {size_t(0), size_t(2), size_t(3)}) {
        REQUIRE(old_colors.count(face) == 1);
        REQUIRE(new_colors.count(face) == 1);
        CHECK(new_colors.at(face) == old_colors.at(face));
    }
}

TEST_CASE("Sclera shadows and warm reflections use one light eye material", "[SemanticColoring][Regression][EyeColor]")
{
    const auto source = connected_triangles({{.55f,.55f,.56f}, {.73f,.65f,.60f}, {.8f,.8f,.8f}});
    const auto analysis = labeled(source, {Label::EyeSclera, Label::EyeSclera, Label::EyeSclera});
    const auto card = portrait_card();
    for (bool known_card : {false, true}) {
        const auto mapped = map_palette(source, analysis, card, known_card ? card : std::vector<Color>{});
        REQUIRE(mapped.size() == 3);
        for (const auto& entry : mapped) CHECK(entry.second == card[2]);
    }
    CHECK(content_fingerprint(source) == source.content_id);
}

TEST_CASE("Dark eyeliner and isolated warm skin cannot prove themselves to be sclera", "[SemanticColoring][Regression][EyeColor]")
{
    const auto card = portrait_card();
    const auto source = triangles({{.12f,.08f,.07f}, {.62f,.48f,.42f}});
    const auto analysis = labeled(source, {Label::EyeSclera, Label::EyeSclera});
    CHECK(map_palette(source, analysis, card, card).empty());

    const auto supported = connected_triangles({
        {.82f,.82f,.80f}, {.72f,.71f,.69f}, {.48f,.47f,.46f}, {.12f,.08f,.07f}});
    const auto supported_analysis = labeled(supported,
        {Label::EyeSclera, Label::EyeSclera, Label::EyeSclera, Label::EyeSclera});
    const auto mapped = map_palette(supported, supported_analysis, card, card);
    REQUIRE(mapped.size() == 3);
    CHECK(std::none_of(mapped.begin(), mapped.end(), [](const auto& entry) { return entry.first == 3; }));
    for (const auto& entry : mapped) CHECK(entry.second == card[2]);
}

TEST_CASE("Weak isolated eye-white evidence does not repaint nearby skin", "[SemanticColoring][Regression][EyeColor]")
{
    const auto card = portrait_card();
    const auto source = connected_triangles({{.52f,.51f,.50f}, {.38f,.37f,.36f}});
    const auto mapped = map_palette(source,
        labeled(source, {Label::EyeSclera, Label::EyeSclera}), card, card);
    CHECK(mapped.empty());
}

TEST_CASE("A supported eye-white region keeps a warm shadow without whitening dark eyelid", "[SemanticColoring][Regression][EyeColor]")
{
    const auto card = portrait_card();
    const auto source = connected_triangles({
        {.82f,.82f,.80f}, {.78f,.78f,.77f}, {.57f,.47f,.39f}, {.16f,.12f,.10f}});
    const auto mapped = map_palette(source,
        labeled(source, {Label::EyeSclera, Label::EyeSclera, Label::EyeSclera, Label::EyeSclera}), card, card);
    REQUIRE(mapped.size() == 3);
    for (const auto& entry : mapped) CHECK(entry.second == card[2]);
    CHECK(std::none_of(mapped.begin(), mapped.end(), [](const auto& entry) { return entry.first == 3; }));
}

TEST_CASE("Bright eye-white support keeps a darker warm sclera edge", "[SemanticColoring][Regression][EyeColor]")
{
    const auto card = portrait_card();
    const auto source = connected_triangles({
        {.826144f,.746405f,.703268f}, {.675817f,.594771f,.549020f},
        {.592157f,.462745f,.388235f}, {.316340f,.185621f,.115033f}});
    const auto mapped = map_palette(source,
        labeled(source, std::vector<Label>(4, Label::EyeSclera)), card, card);
    REQUIRE(mapped.size() == 3);
    CHECK(mapped[0].second == card[2]);
    CHECK(mapped[1].second == card[2]);
    CHECK(mapped[2].second == card[2]);
    CHECK(std::none_of(mapped.begin(), mapped.end(), [](const auto& entry) { return entry.first == 3; }));
}

TEST_CASE("Uncertain ear and hair boundary faces do not borrow skin from a cheek", "[SemanticColoring][Regression][FaceSkin]")
{
    const auto source = hair_edge_strip({
        {.76f,.55f,.41f}, {.64f,.46f,.37f}, {.57f,.40f,.33f}, {.68f,.26f,.24f}});
    auto analysis = labeled(source, {
        Label::FaceSkin, Label::Hair, Label::Unknown, Label::Lips, Label::Background});
    analysis.face_confidence = {.95f, .80f, .50f, .95f, .95f};
    const auto card = portrait_card();
    const auto mapped = map_palette(source, analysis, card, card);
    std::map<size_t, Color> colors;
    for (const auto& entry : mapped) colors[entry.first] = entry.second;
    REQUIRE(colors.count(0) == 1);
    CHECK(colors.at(0) == card[0]);
    for (size_t face : {size_t(1), size_t(2)})
        if (colors.count(face)) CHECK(colors.at(face) != card[0]);
    REQUIRE(colors.count(3) == 1);
    CHECK(colors.at(3) == card[3]);
    CHECK(colors.count(4) == 0);
}

TEST_CASE("A colored iris uses its material center instead of baked white highlights", "[SemanticColoring][Regression][EyeColor]")
{
    const Color blue {.2f,.45f,.7f};
    const auto source = connected_triangles({blue, blue, {.15f,.35f,.55f}, {.98f,.98f,.98f}});
    const auto analysis = labeled(source, {Label::Iris, Label::Iris, Label::Iris, Label::Iris});
    const std::vector<Color> palette {{.02f,.02f,.02f}, {.98f,.98f,.98f}, blue, {.9f,.7f,.6f}};
    const auto mapped = map_palette(source, analysis, palette);
    REQUIRE(mapped.size() == 4);
    for (const auto& entry : mapped) CHECK(entry.second == blue);
}


TEST_CASE("A brown iris retains its available material beside a light sclera", "[SemanticColoring][Regression][EyeColor]")
{
    const Color brown {.42f,.27f,.15f}, white {.96f,.97f,.98f};
    const auto source = triangles({{.86f,.85f,.84f}, brown});
    const auto analysis = labeled(source, {Label::EyeSclera, Label::Iris});
    const std::vector<Color> palette {{.97f,.88f,.84f}, {.10f,.09f,.08f}, white, brown};
    const auto mapped = map_palette(source, analysis, palette);
    REQUIRE(mapped.size() == 2);
    CHECK(mapped[0].second == white);
    CHECK(mapped[1].second == brown);
}

TEST_CASE("Low confidence eye masks retain the existing color and manual correction has priority", "[SemanticColoring][Regression][EyeColor]")
{
    const auto source = triangles({{.7f,.6f,.55f}, {.3f,.2f,.16f}});
    auto analysis = labeled(source, {Label::EyeSclera, Label::Iris});
    analysis.face_confidence.front() = minimum_confidence - .01f;
    const auto card = portrait_card();
    const auto automatic = map_palette(source, analysis, card, card);
    REQUIRE(automatic.size() == 1);
    CHECK(automatic.front().first == 1);
    CHECK(automatic.front().second == card[1]);
    const FaceColors manual {{1, card[4]}};
    CHECK(compose(automatic, manual, true) == manual);
}

TEST_CASE("Semantic subface budgeting counts unique midpoint split nodes and keeps stronger evidence",
          "[SemanticColoring][SubfaceColor]")
{
    const Color white {.95f,.95f,.95f}, dark {.08f,.07f,.06f};
    const SubfaceColors candidates {
        {2, {2, 6}, white, .99f},  // root + first-level child 1: six added triangles
        {2, {2, 7}, white, .98f},  // shares both split nodes
        {2, {2, 11}, dark, .97f},  // one additional first-level split: +3
        {3, {1, 0}, dark, .96f},   // one root split: +3
        {4, {1, 0}, white, .95f},  // would exceed the 12-triangle budget
        {5, {1, 0}, white, .50f},  // below the evidence threshold
        {2, {2, 6}, dark, .40f},   // duplicate path loses to stronger evidence
    };
    SubfaceBudget budget;
    budget.maximum_added_ratio = .12f;
    SubfaceBudgetResult result;
    std::string error;
    REQUIRE(enforce_subface_budget(candidates, 100, budget, result, error));
    CHECK(error.empty());
    CHECK(result.added_triangles == 12);
    CHECK(result.accepted.size() == 4);
    CHECK(result.rejected_candidates == 2);
    CHECK(result.accepted.front().face_id == 2);
    CHECK((result.accepted.front().path == SubfacePath {2, 6}));
    CHECK(result.accepted.front().color == white);
}

TEST_CASE("Subface palette remapping is independent of recognition and manual whole-face paint wins",
          "[SemanticColoring][SubfaceColor]")
{
    const Color source_white {.95f,.95f,.95f}, source_dark {.08f,.07f,.06f};
    const Color target_white {.88f,.90f,.94f}, target_dark {.03f,.03f,.04f};
    const SubfaceColors automatic {
        {3, {1, 0}, source_white, .92f},
        {3, {2, 3}, source_dark, .88f},
        {4, {1, 2}, source_dark, .91f},
    };
    const auto remapped = remap_subface_palette_targets(
        automatic, {source_white, source_dark}, {target_white, target_dark});
    REQUIRE(remapped.size() == automatic.size());
    CHECK(remapped[0].color == target_white);
    CHECK(remapped[1].color == target_dark);
    CHECK(remapped[2].color == target_dark);

    const FaceColors manual {{3, {.2f,.4f,.6f}}};
    const auto composed = compose_subfaces(remapped, manual, true);
    REQUIRE(composed.size() == 1);
    CHECK(composed.front().face_id == 4);
    CHECK(compose_subfaces(remapped, manual, false).empty());
}

TEST_CASE("Supported low-chroma sclera leaves refine a skin face without whitening an isolated leaf",
          "[SemanticColoring][SubfaceColor][EyeColor]")
{
    std::vector<Color> colors(60, {.82f,.74f,.70f});
    colors[0] = colors[1] = {.90f,.91f,.92f};
    colors[3] = {.64f,.50f,.44f};
    auto source = surface_strip(colors);
    std::vector<Label> labels(colors.size(), Label::FaceSkin);
    labels[0] = labels[1] = Label::EyeSclera;
    auto analysis = labeled(source, labels);
    analysis.subface_labels = {
        {2, {2, 0}, Label::EyeSclera, .94f, 2},
        {2, {2, 1}, Label::EyeSclera, .92f, 1},
        {3, {2, 0}, Label::EyeSclera, .93f, 1},
        {59, {2, 0}, Label::EyeSclera, .93f, 16},
    };
    const auto card = portrait_card();
    const auto whole = map_palette(source, analysis, card, card);
    SubfaceBudgetResult result;
    std::string error;
    REQUIRE(map_subface_palette(source, analysis, whole, card, card, {}, result, error));
    CHECK(error.empty());
    REQUIRE(result.added_triangles <= 15);
    CHECK(result.accepted.size() == 7);
    CHECK(std::all_of(result.accepted.begin(), result.accepted.end(), [&](const auto& leaf) {
        return leaf.face_id == 2 && leaf.color == card[2];
    }));
    CHECK(std::none_of(result.accepted.begin(), result.accepted.end(), [](const auto& leaf) {
        return leaf.face_id == 3 || leaf.face_id == 59;
    }));
}

TEST_CASE("Sclera subfaces close two same-root edge gaps without crossing explicit skin or iris detail",
          "[SemanticColoring][SubfaceColor][EyeColor]")
{
    const auto card = portrait_card();
    std::vector<Color> colors(80, {.82f,.74f,.70f});
    colors[0] = colors[1] = colors[2] = {.90f,.91f,.92f};
    auto source = surface_strip(colors);
    std::vector<Label> labels(colors.size(), Label::FaceSkin);
    labels[0] = labels[1] = Label::EyeSclera;
    auto analysis = labeled(source, labels);
    analysis.subface_labels = {{2, {2, 0}, Label::EyeSclera, .94f, 2}};
    const FaceColors whole {{0, card[2]}, {1, card[2]}, {2, card[0]}};
    SubfaceBudgetResult result;
    std::string error;
    REQUIRE(map_subface_palette(source, analysis, whole, card, card, {}, result, error));
    REQUIRE(error.empty());
    REQUIRE(result.accepted.size() == 4);
    CHECK(std::all_of(result.accepted.begin(), result.accepted.end(), [&](const auto& leaf) {
        return leaf.face_id == 2 && leaf.color == card[2];
    }));
    for (uint8_t path : {uint8_t(0), uint8_t(1), uint8_t(2), uint8_t(3)})
        CHECK(std::any_of(result.accepted.begin(), result.accepted.end(), [path](const auto& leaf) {
            return leaf.path == SubfacePath {2, path};
        }));
    CHECK(std::none_of(result.accepted.begin(), result.accepted.end(), [](const auto& leaf) {
        return leaf.path == SubfacePath {2, 12};
    }));

    analysis.subface_labels.push_back({2, {2, 3}, Label::FaceSkin, .95f, 2});
    REQUIRE(map_subface_palette(source, analysis, whole, card, card, {}, result, error));
    const auto protected_skin = std::find_if(result.accepted.begin(), result.accepted.end(), [](const auto& leaf) {
        return leaf.face_id == 2 && leaf.path == SubfacePath {2, 3};
    });
    CHECK(protected_skin == result.accepted.end());
    CHECK(std::none_of(result.accepted.begin(), result.accepted.end(), [](const auto& leaf) {
        return leaf.face_id == 2 && (leaf.path == SubfacePath {2, 1} || leaf.path == SubfacePath {2, 2});
    }));

    analysis.subface_labels.back() = {2, {2, 3}, Label::Iris, .95f, 2};
    REQUIRE(map_subface_palette(source, analysis, whole, card, card, {}, result, error));
    const auto protected_iris = std::find_if(result.accepted.begin(), result.accepted.end(), [](const auto& leaf) {
        return leaf.face_id == 2 && leaf.path == SubfacePath {2, 3};
    });
    REQUIRE(protected_iris != result.accepted.end());
    CHECK(protected_iris->color == card[1]);
    CHECK(std::none_of(result.accepted.begin(), result.accepted.end(), [](const auto& leaf) {
        return leaf.face_id == 2 && (leaf.path == SubfacePath {2, 1} || leaf.path == SubfacePath {2, 2});
    }));
}

TEST_CASE("Sclera subfaces cross one exact original-face edge, close a supported landing ring, and stop at iris detail",
          "[SemanticColoring][SubfaceColor][EyeColor]")
{
    const auto card = portrait_card();
    std::vector<Color> colors(200,{.82f,.74f,.70f});
    colors[2] = colors[3] = {.90f,.91f,.92f};
    auto source = surface_strip(colors);
    std::vector<Label> labels(200,Label::FaceSkin);
    labels[2] = labels[3] = Label::EyeSclera;
    auto analysis = labeled(source,labels);
    for (uint8_t path=0;path<16;++path)
        analysis.subface_labels.push_back({0,{2,path},Label::EyeSclera,.94f,2});
    const auto whole = map_palette(source,analysis,card,card);
    SubfaceBudgetResult result;
    std::string error;
    REQUIRE(map_subface_palette(source,analysis,whole,card,card,{},result,error));
    CHECK(std::any_of(result.accepted.begin(),result.accepted.end(),[&](const auto& leaf) {
        return leaf.face_id==1 && leaf.color==card[2];
    }));
    CHECK(std::any_of(result.accepted.begin(),result.accepted.end(),[&](const auto& leaf) {
        return leaf.face_id==1 && leaf.color==card[2] &&
            leaf.path.depth==2 && leaf.path.value!=0 && leaf.path.value!=2 &&
            leaf.path.value!=9 && leaf.path.value!=10;
    }));
    CHECK(std::none_of(result.accepted.begin(),result.accepted.end(),[](const auto& leaf) {
        return leaf.face_id==2;
    }));

    analysis.subface_labels.push_back({1,{2,15},Label::Iris,.95f,2});
    REQUIRE(map_subface_palette(source,analysis,whole,card,card,{},result,error));
    CHECK(std::any_of(result.accepted.begin(),result.accepted.end(),[&](const auto& leaf) {
        return leaf.face_id==1 && leaf.color==card[2] && leaf.confidence>.77f;
    }));
    CHECK(std::none_of(result.accepted.begin(),result.accepted.end(),[&](const auto& leaf) {
        return leaf.face_id==1 && leaf.color==card[2] && leaf.confidence<.77f;
    }));

    for (uint8_t path=0;path<16;++path)
        if (path!=15) analysis.subface_labels.push_back({1,{2,path},Label::Iris,.95f,2});
    REQUIRE(map_subface_palette(source,analysis,whole,card,card,{},result,error));
    CHECK(std::none_of(result.accepted.begin(),result.accepted.end(),[&](const auto& leaf) {
        return leaf.face_id==1 && leaf.color==card[2];
    }));
}

TEST_CASE("A mature local sclera component admits supported direct neutral shadows but rejects a warm singleton",
          "[SemanticColoring][SubfaceColor][EyeColor]")
{
    const auto card = portrait_card();
    auto source = surface_strip(std::vector<Color>(200, {.82f,.74f,.70f}));
    source.face_colors.clear();
    source.vertex_colors.assign(source.mesh.vertices.size(), {.82f,.74f,.70f,1.f});
    const auto set_vertex = [&](size_t vertex, const Color& color) {
        source.vertex_colors[vertex] = {color[0], color[1], color[2], 1.f};
    };
    const Color bright {.70f,.71f,.72f};
    for (size_t vertex : {size_t(0), size_t(1), size_t(2), size_t(4), size_t(5),
                          size_t(6), size_t(10), size_t(11), size_t(12)})
        set_vertex(vertex, bright);
    set_vertex(3, {.35f,.35f,.35f});
    set_vertex(7, {.45f,.34f,.28f});
    source.content_id = content_fingerprint(source);
    std::vector<Label> labels(200, Label::FaceSkin);
    labels[0] = labels[4] = labels[10] = Label::EyeSclera;
    auto analysis = labeled(source, labels);
    for (size_t face_id : {size_t(1), size_t(5)})
        for (uint8_t path : {uint8_t(0), uint8_t(2), uint8_t(3)})
            analysis.subface_labels.push_back({face_id, {2, path}, Label::EyeSclera, .94f, 2});
    analysis.subface_labels.push_back({1, {2, 5}, Label::EyeSclera, .94f, 2});
    analysis.subface_labels.push_back({1, {2, 7}, Label::Iris, .94f, 1});
    analysis.subface_labels.push_back({5, {2, 5}, Label::EyeSclera, .94f, 1});

    const auto whole = map_palette(source, analysis, card, card);
    SubfaceBudgetResult result;
    std::string error;
    REQUIRE(map_subface_palette(source, analysis, whole, card, card, {}, result, error));
    CHECK(std::any_of(result.accepted.begin(), result.accepted.end(), [&](const auto& leaf) {
        return leaf.face_id == 1 && leaf.path == SubfacePath {2, 5} && leaf.color == card[2];
    }));
    CHECK(std::none_of(result.accepted.begin(), result.accepted.end(), [&](const auto& leaf) {
        return leaf.face_id == 5 && leaf.path == SubfacePath {2, 5} && leaf.color == card[2];
    }));
}

TEST_CASE("Reliable skin leaves cut back an eye-white root only at a supported eyelid boundary",
          "[SemanticColoring][SubfaceColor][EyeColor]")
{
    const auto card = portrait_card();
    std::vector<Color> colors(60, {.40f,.28f,.22f});
    colors[0] = colors[1] = {.90f,.91f,.92f};
    colors[2] = {.40f,.28f,.22f};
    auto source = surface_strip(colors);
    std::vector<Label> labels(colors.size(), Label::FaceSkin);
    labels[0] = labels[1] = labels[2] = Label::EyeSclera;
    auto analysis = labeled(source, labels);
    analysis.subface_labels = {{2, {2, 7}, Label::FaceSkin, .88f, 2}};
    Analysis restored;
    std::string cache_error;
    REQUIRE(decode_analysis(encode_analysis(analysis), source,
        analysis.body_identity, analysis.face_identity, restored, cache_error));
    CHECK(restored.subface_labels == analysis.subface_labels);
    const FaceColors whole {{0, card[2]}, {1, card[2]}, {2, card[2]}};
    SubfaceBudgetResult result;
    std::string error;
    REQUIRE(map_subface_palette(source, analysis, whole, card, card, {}, result, error));
    REQUIRE(error.empty());
    REQUIRE_FALSE(result.accepted.empty());
    const auto direct = std::find_if(result.accepted.begin(), result.accepted.end(), [](const auto& leaf) {
        return leaf.face_id == 2 && leaf.path == SubfacePath {2, 7};
    });
    REQUIRE(direct != result.accepted.end());
    CHECK(direct->confidence == .88f);
    CHECK(std::all_of(result.accepted.begin(), result.accepted.end(), [&](const auto& leaf) {
        return leaf.face_id == 2 && leaf.color == card[0];
    }));

    analysis.subface_labels.clear();
    REQUIRE(map_subface_palette(source, analysis, whole, card, card, {}, result, error));
    REQUIRE_FALSE(result.accepted.empty());
    CHECK(std::all_of(result.accepted.begin(), result.accepted.end(), [&](const auto& leaf) {
        return leaf.face_id == 2 && leaf.color == card[0];
    }));

    auto isolated = triangles(colors);
    analysis = labeled(isolated, labels);
    analysis.subface_labels = {{2, {2, 7}, Label::FaceSkin, .88f, 2}};
    REQUIRE(map_subface_palette(isolated, analysis, whole, card, card, {}, result, error));
    CHECK(result.accepted.empty());
}

TEST_CASE("Reliable ear skin leaves cut back a hair root only beside supported skin",
          "[SemanticColoring][SubfaceColor][HairBoundary][FaceSkin]")
{
    const auto card = portrait_card();
    std::vector<Color> colors(40, {.18f,.12f,.10f});
    colors[0] = colors[1] = colors[3] = {.76f,.55f,.41f};
    auto source = triangles(colors);
    source.mesh.indices[1] = source.mesh.indices[0];
    source.content_id = content_fingerprint(source);
    std::vector<Label> labels(colors.size(), Label::Hair);
    labels[0] = Label::FaceSkin;
    auto analysis = labeled(source, labels);
    analysis.subface_labels = {
        {1, {2, 7}, Label::FaceSkin, .88f, 2},
        {3, {2, 7}, Label::FaceSkin, .88f, 2},
    };
    const FaceColors whole {{0, card[0]}, {1, card[1]}, {3, card[1]}};
    SubfaceBudgetResult result;
    std::string error;
    REQUIRE(map_subface_palette(source, analysis, whole, card, card, {}, result, error));
    CHECK(error.empty());
    REQUIRE(result.accepted.size() == 4);
    CHECK(std::all_of(result.accepted.begin(), result.accepted.end(), [&](const auto& leaf) {
        return leaf.face_id == 1 && leaf.color == card[0];
    }));
    CHECK(std::any_of(result.accepted.begin(), result.accepted.end(), [](const auto& leaf) {
        return leaf.path == SubfacePath {2, 7};
    }));
}

TEST_CASE("Ear skin subfaces close one same-root edge gap without crossing explicit face detail",
          "[SemanticColoring][SubfaceColor][HairBoundary][FaceSkin]")
{
    const auto card = portrait_card();
    auto source = triangles(std::vector<Color>(80, {.18f,.12f,.10f}));
    // Face 0 is reliable skin. Face 1 shares its 1-2 edge, but its remaining
    // corner retains the original dark hair appearance.
    source.mesh.vertices[3] = source.mesh.vertices[0] + Vec3f(0.f, 0.f, 2.f);
    source.mesh.indices[1] = {1, 3, 2};
    source.vertex_colors[0] = source.vertex_colors[1] = source.vertex_colors[2] =
        {0.76f, 0.55f, 0.41f, 1.f};
    source.vertex_colors[3] = {0.18f, 0.12f, 0.10f, 1.f};
    source.content_id = content_fingerprint(source);
    std::vector<Label> labels(80, Label::Hair);
    labels[0] = Label::FaceSkin;
    auto analysis = labeled(source, labels);
    analysis.subface_labels = {{1, {2, 0}, Label::FaceSkin, .90f, 2}};
    const FaceColors whole {{0, card[0]}, {1, card[1]}};
    SubfaceBudgetResult result;
    std::string error;
    REQUIRE(map_subface_palette(source, analysis, whole, card, card, {}, result, error));
    REQUIRE(error.empty());
    REQUIRE(result.accepted.size() > 1);
    CHECK(std::any_of(result.accepted.begin(), result.accepted.end(), [](const auto& leaf) {
        return leaf.face_id == 1 && !(leaf.path == SubfacePath {2, 0});
    }));
    CHECK(std::all_of(result.accepted.begin(), result.accepted.end(), [&](const auto& leaf) {
        return leaf.face_id == 1 && leaf.color == card[0];
    }));

    analysis.subface_labels.push_back({1, {2, 3}, Label::Eyebrow, .95f, 2});
    REQUIRE(map_subface_palette(source, analysis, whole, card, card, {}, result, error));
    const auto protected_leaf = std::find_if(result.accepted.begin(), result.accepted.end(), [](const auto& leaf) {
        return leaf.face_id == 1 && leaf.path == SubfacePath {2, 3};
    });
    REQUIRE(protected_leaf != result.accepted.end());
    CHECK(protected_leaf->color == card[5]);
}

TEST_CASE("Ear skin subfaces bridge one nearby disconnected hair surface without propagation",
          "[SemanticColoring][SubfaceColor][HairBoundary][FaceSkin]")
{
    const auto card = portrait_card();
    std::vector<Color> colors(200, {.18f,.12f,.10f});
    colors[0] = colors[1] = colors[2] = {.76f,.55f,.41f};
    auto source = triangles(colors);
    source.mesh.indices[1] = source.mesh.indices[0];
    for (int corner = 0; corner < 3; ++corner)
        source.mesh.vertices[6 + corner] = source.mesh.vertices[corner] + Vec3f(.15f, .02f, 0.f);
    source.content_id = content_fingerprint(source);
    std::vector<Label> labels(colors.size(), Label::Hair);
    labels[0] = Label::FaceSkin;
    auto analysis = labeled(source, labels);
    analysis.subface_labels = {{1, {2, 7}, Label::FaceSkin, .90f, 2}};
    const FaceColors whole {{0, card[0]}, {1, card[1]}, {2, card[1]}, {3, card[1]}};
    SubfaceBudgetResult result;
    std::string error;
    REQUIRE(map_subface_palette(source, analysis, whole, card, card, {}, result, error));
    REQUIRE(error.empty());
    CHECK(std::any_of(result.accepted.begin(), result.accepted.end(), [&](const auto& leaf) {
        return leaf.face_id == 2 && leaf.color == card[0];
    }));
    CHECK(std::none_of(result.accepted.begin(), result.accepted.end(), [](const auto& leaf) {
        return leaf.face_id == 3;
    }));

    std::swap(source.mesh.indices[2][1], source.mesh.indices[2][2]);
    source.content_id = content_fingerprint(source);
    analysis = labeled(source, labels);
    analysis.subface_labels = {{1, {2, 7}, Label::FaceSkin, .90f, 2}};
    REQUIRE(map_subface_palette(source, analysis, whole, card, card, {}, result, error));
    CHECK(std::none_of(result.accepted.begin(), result.accepted.end(), [](const auto& leaf) {
        return leaf.face_id == 2;
    }));
}

TEST_CASE("Warm skin rejected by whole-face sclera mapping is recovered as skin leaves",
          "[SemanticColoring][SubfaceColor][EyeColor]")
{
    const auto card = portrait_card();
    std::vector<Color> colors(80, {.52f,.36f,.26f});
    colors[0] = colors[1] = {.90f,.91f,.92f};
    colors[2] = {.42f,.28f,.21f};
    auto source = surface_strip(colors);
    std::vector<Label> labels(colors.size(), Label::FaceSkin);
    labels[0] = labels[1] = labels[2] = Label::EyeSclera;
    const auto analysis = labeled(source, labels);
    const FaceColors whole {{0, card[2]}, {1, card[2]}};
    SubfaceBudgetResult result;
    std::string error;
    REQUIRE(map_subface_palette(source, analysis, whole, card, card, {}, result, error));
    CHECK(error.empty());
    REQUIRE(result.accepted.size() == 16);
    CHECK(result.added_triangles == 15);
    CHECK(std::all_of(result.accepted.begin(), result.accepted.end(), [&](const auto& leaf) {
        return leaf.face_id == 2 && leaf.path.depth == 2 && leaf.color == card[0];
    }));

    colors[2] = {.58f,.58f,.57f};
    source = surface_strip(colors);
    const auto neutral_analysis = labeled(source, labels);
    REQUIRE(map_subface_palette(source, neutral_analysis, whole, card, card, {}, result, error));
    CHECK(result.accepted.empty());
}

TEST_CASE("Portrait iris subfaces never borrow skin or lip materials",
          "[SemanticColoring][SubfaceColor][EyeColor]")
{
    const auto card = portrait_card();
    std::vector<Color> colors(60, {.72f,.58f,.50f});
    colors[0] = {.72f,.36f,.22f};
    colors[1] = {.20f,.42f,.72f};
    auto source = triangles(colors);
    auto analysis = labeled(source, std::vector<Label>(colors.size(), Label::FaceSkin));
    analysis.subface_labels = {
        {0, {2, 0}, Label::Iris, .94f, 1},
        {1, {2, 0}, Label::Iris, .94f, 1},
    };
    const auto whole = map_palette(source, analysis, card, card);
    SubfaceBudgetResult result;
    std::string error;
    REQUIRE(map_subface_palette(source, analysis, whole, card, card, {}, result, error));
    REQUIRE(result.accepted.size() == 2);
    CHECK(result.accepted[0].color == card[1]);
    CHECK(result.accepted[1].color == card[4]);
}

TEST_CASE("Portrait eyebrow subfaces use the same neutral midtone as whole eyebrows",
          "[SemanticColoring][SubfaceColor][EyebrowColor]")
{
    const auto card = portrait_card();
    std::vector<Color> colors(60, {.72f,.58f,.50f});
    colors[0] = {.05f,.05f,.05f};
    auto source = triangles(colors);
    auto analysis = labeled(source, std::vector<Label>(colors.size(), Label::FaceSkin));
    analysis.subface_labels = {{0, {2, 0}, Label::Eyebrow, .94f, 2}};
    const FaceColors whole {{0, card[0]}};
    SubfaceBudgetResult result;
    std::string error;
    REQUIRE(map_subface_palette(source, analysis, whole, card, card, {}, result, error));
    REQUIRE(result.accepted.size() == 1);
    CHECK(result.accepted.front().face_id == 0);
    CHECK(result.accepted.front().color == card[5]);
}

TEST_CASE("Malformed subface evidence is rejected transactionally", "[SemanticColoring][SubfaceColor]")
{
    SubfaceBudgetResult result;
    result.accepted.push_back({0, {1, 0}, {.1f,.2f,.3f}, .9f});
    result.added_triangles = 3;
    std::string error;
    REQUIRE_FALSE(enforce_subface_budget({{0, {3, 0}, {.1f,.2f,.3f}, .9f}}, 10, {}, result, error));
    CHECK_FALSE(error.empty());
    CHECK(result.accepted.empty());
    CHECK(result.added_triangles == 0);
}

TEST_CASE("Face region rendering resolves original subpixel faces instead of enlarging old face ids", "[SemanticColoring][Regression]")
{
    auto source = triangles({{0,1,0}, {1,0,0}});
    source.mesh.vertices[3] = Vec3f(-.0002f, -.1f, .0098f);
    source.mesh.vertices[4] = Vec3f(.0002f, -.1f, .0098f);
    source.mesh.vertices[5] = Vec3f(0.f, -.1f, .0102f);
    const auto full = render_view(source, 0, 512);
    const auto detail = render_region(source, 0, {.495f,.495f,.01f,.01f}, 512);
    REQUIRE(full.error.empty());
    REQUIRE(detail.error.empty());
    CHECK(std::count(full.face_ids.begin(), full.face_ids.end(), uint32_t(1)) == 0);
    CHECK(std::count(detail.face_ids.begin(), detail.face_ids.end(), uint32_t(1)) > 10);
    const auto red = std::find(detail.face_ids.begin(), detail.face_ids.end(), uint32_t(1));
    REQUIRE(red != detail.face_ids.end());
    CHECK(detail.image.pixels[size_t(red - detail.face_ids.begin()) * 3] == 255);
    CHECK_FALSE(render_region(source, 0, {0,0,0,1}, 512).error.empty());
}

TEST_CASE("Subpixel evidence follows an inclined surface and rejects an occluded back layer at different scales", "[SemanticColoring][Regression]")
{
    for (float scale : {.1f, 1.f, 100.f}) {
        DYNAMIC_SECTION("model scale " << scale) {
            auto source = triangles({{.5f,.5f,.5f}, {.5f,.5f,.5f}});
            source.mesh.vertices[3] = Vec3f(-.002f, 0.f, -.002f);
            source.mesh.vertices[4] = Vec3f(.002f, 0.f, -.002f);
            source.mesh.vertices[5] = Vec3f(0.f, 0.f, .002f);
            for (auto& vertex : source.mesh.vertices) { vertex.y() = vertex.z() * .3f; vertex *= scale; }
            const auto same_surface = render_view(source, 0, 512);
            REQUIRE(same_surface.error.empty());
            CHECK(std::count(same_surface.face_ids.begin(), same_surface.face_ids.end(), uint32_t(1)) == 0);
            REQUIRE(same_surface.surface_samples.size() == 2);
            CHECK(same_surface.surface_samples[1] != std::numeric_limits<uint32_t>::max());
            for (size_t vertex = 3; vertex < 6; ++vertex) source.mesh.vertices[vertex].y() += scale;
            const auto back_layer = render_view(source, 0, 512);
            REQUIRE(back_layer.error.empty());
            CHECK(std::count(back_layer.face_ids.begin(), back_layer.face_ids.end(), uint32_t(1)) == 0);
            CHECK(back_layer.surface_samples[1] == std::numeric_limits<uint32_t>::max());
        }
    }
}

TEST_CASE("Subpixel label evidence requires matching original colors as well as a visible surface", "[SemanticColoring][Regression]")
{
    auto source = triangles({{.76f,.55f,.41f}, {.76f,.55f,.41f}});
    source.mesh.vertices[3] = Vec3f(-.0005f, 0.f, -.0005f);
    source.mesh.vertices[4] = Vec3f(.0005f, 0.f, -.0005f);
    source.mesh.vertices[5] = Vec3f(0.f, 0.f, .0005f);
    for (int view = 0; view < 8; ++view) {
        const auto rendered = render_view(source, view * 45.f, 512);
        REQUIRE(rendered.error.empty());
        REQUIRE(std::count(rendered.face_ids.begin(), rendered.face_ids.end(), uint32_t(1)) == 0);
    }
    source.content_id = content_fingerprint(source);
    BodyFixture body; FaceFixture face;
    const auto supported = analyze(source, body, face);
    REQUIRE(supported.error.empty());
    REQUIRE(supported.face_labels.size() == 2);
    CHECK(supported.face_labels[1] == Label::FaceSkin);
    CHECK(supported.face_confidence[1] >= minimum_confidence);
    for (size_t vertex = 3; vertex < 6; ++vertex) source.vertex_colors[vertex] = {.05f,.05f,.05f,1.f};
    source.content_id = content_fingerprint(source);
    const auto unsupported = analyze(source, body, face);
    REQUIRE(unsupported.error.empty());
    REQUIRE(unsupported.face_labels.size() == 2);
    CHECK(unsupported.face_confidence[1] < minimum_confidence);
}

TEST_CASE("Original color identity survives seam splits and changes when original colors change", "[SemanticColoring]")
{
    auto source = triangles({{.2f,.4f,.8f}});
    const auto before = content_fingerprint(source);
    source.mesh.vertices.push_back(source.mesh.vertices[0]);
    source.vertex_colors.push_back(source.vertex_colors[0]);
    source.mesh.indices[0][0] = int(source.mesh.vertices.size() - 1);
    CHECK(content_fingerprint(source) == before);
    source.vertex_colors.back()[0] = .21f;
    CHECK(content_fingerprint(source) != before);
    source.geometry_id += "-moved";
    CHECK(content_fingerprint(source) != before);
}

TEST_CASE("Semantic rendering rejects invalid geometry and supports cancellation", "[SemanticColoring]")
{
    auto source = triangles({{.2f,.4f,.8f}});
    CHECK(render_view(source, 0, 64, [] { return true; }).canceled);
    CHECK_FALSE(render_view(source, 0, 0).error.empty());
    source.mesh.indices[0][0] = -1;
    CHECK_FALSE(render_view(source, 0, 64).error.empty());
    CHECK(content_fingerprint(source).empty());
}

TEST_CASE("Known portrait skin roles keep warm skin apart from pink despite identical clothing RGB", "[SemanticColoring][Regression]")
{
    const auto source = triangles({{.76f,.70f,.67f}, {.76f,.70f,.67f}, {.45f,.25f,.19f}});
    const auto analysis = labeled(source, {Label::FaceSkin, Label::Clothes, Label::BodySkin});
    const auto card = portrait_card();
    const auto output = map_palette(source, analysis, card, card);
    REQUIRE(output.size() == 3);
    CHECK(output[0].second == card[0]);
    CHECK(output[2].second == card[0]);
    CHECK(output[1].second != card[0]);
    CHECK(output[0].second != card[3]);
    auto shuffled = card; std::reverse(shuffled.begin(), shuffled.end());
    CHECK(map_palette(source, analysis, shuffled, card) == output);
}

TEST_CASE("Regional source colors preserve red hair and gray clothing without forcing white clothing or red lips", "[SemanticColoring][Regression]")
{
    const Color gray {.45f,.45f,.45f}, red {.8f,.05f,.04f}, black {.1f,.1f,.1f}, white {.98f,.98f,.98f};
    const auto source = triangles({red, gray, gray, black});
    const auto analysis = labeled(source, {Label::Hair, Label::Clothes, Label::Lips, Label::MouthInterior});
    const auto output = map_palette(source, analysis, {black, white, gray, red});
    REQUIRE(output.size() == 4);
    CHECK(output[0].second == red);
    CHECK(output[1].second == gray);
    CHECK(output[2].second == gray);
    CHECK(output[3].second == black);
}

TEST_CASE("Named portrait cards do not turn gray lips pink or gray clothes white", "[SemanticColoring][Regression]")
{
    const auto source = triangles({{.48f,.48f,.48f}, {.48f,.48f,.48f}, {.13f,.11f,.10f}});
    const auto analysis = labeled(source, {Label::Lips, Label::Clothes, Label::Hair});
    const auto card = portrait_card();
    const auto output = map_palette(source, analysis, card, card);
    REQUIRE(output.size() == 3);
    CHECK(output[0].second == card[5]);
    CHECK(output[1].second == card[5]);
    CHECK(output[2].second == card[1]);
}

TEST_CASE("Dark brown hair keeps warm shading and highlights in one portrait material", "[SemanticColoring][Regression][HairColor]")
{
    // One natural brown material, with a dark majority and progressively lighter
    // warm highlights. These are synthetic source colors, not screenshot samples.
    const auto source = surface_strip({
        {.18f,.12f,.09f}, {.20f,.13f,.10f}, {.22f,.15f,.11f},
        {.24f,.16f,.12f}, {.22f,.15f,.11f}, {.24f,.16f,.12f},
        {89.f/255,64.f/255,46.f/255}, {115.f/255,75.f/255,51.f/255},
        {143.f/255,98.f/255,72.f/255}, {170.f/255,121.f/255,84.f/255}});
    auto analysis = labeled(source, std::vector<Label>(source.mesh.indices.size(), Label::Hair));
    // Confidence close to the acceptance threshold must not split a material
    // that has already been accepted as part of the same hair surface.
    analysis.face_confidence[7] = minimum_confidence + .001f;
    analysis.face_confidence[8] = minimum_confidence + .01f;
    const auto card = portrait_card();
    const auto output = map_palette(source, analysis, card, card);
    REQUIRE(output.size() == source.mesh.indices.size());
    for (size_t face = 0; face < output.size(); ++face) {
        INFO("source face " << face);
        CHECK(output[face].first == face);
        CHECK(output[face].second == card[1]);
    }
    auto reordered = card;
    std::reverse(reordered.begin(), reordered.end());
    CHECK(map_palette(source, analysis, reordered, card) == output);
}

TEST_CASE("Brown hair coherence does not recolor adjacent skin or white clothing", "[SemanticColoring][Regression][HairColor]")
{
    const auto source = surface_strip({
        {.20f,.13f,.10f}, {.22f,.15f,.11f}, {.24f,.16f,.12f},
        {.22f,.15f,.11f}, {.45f,.29f,.20f}, {.67f,.47f,.33f},
        {.76f,.55f,.41f}, {.96f,.96f,.96f}});
    const auto analysis = labeled(source, {Label::Hair, Label::Hair, Label::Hair,
        Label::Hair, Label::Hair, Label::Hair, Label::FaceSkin, Label::Clothes});
    const auto card = portrait_card();
    const auto output = map_palette(source, analysis, card, card);
    REQUIRE(output.size() == 8);
    for (size_t face = 0; face < 6; ++face) CHECK(output[face].second == card[1]);
    CHECK(output[6].first == 6);
    CHECK(output[6].second == card[0]);
    CHECK(output[7].first == 7);
    CHECK(output[7].second == card[2]);
}

TEST_CASE("Uncertain brown hair cannot seed automatic color into skin clothing or unknown faces", "[SemanticColoring][Regression][HairColor]")
{
    const auto source = surface_strip({
        {.22f,.15f,.11f}, {.23f,.16f,.12f}, {.24f,.17f,.13f},
        {.76f,.55f,.41f}, {.96f,.96f,.96f}});
    auto analysis = labeled(source, {Label::Hair, Label::Hair, Label::Unknown, Label::FaceSkin, Label::Clothes});
    analysis.face_confidence[0] = .60f;
    analysis.face_confidence[1] = minimum_confidence - .001f;
    const auto card = portrait_card();
    const FaceColors expected {{3, card[0]}, {4, card[2]}};
    CHECK(map_palette(source, analysis, card, card) == expected);
}

TEST_CASE("Hair color protection preserves clearly dyed and achromatic hair when matching materials exist", "[SemanticColoring][Regression][HairColor]")
{
    const Color dark {.10f,.08f,.07f};
    const std::vector<Color> distinctive {{.80f,.05f,.04f}, {.90f,.72f,.24f},
        {.55f,.55f,.55f}, {.97f,.97f,.97f}};
    for (const auto& color : distinctive) {
        INFO("distinctive hair RGB " << color[0] << ',' << color[1] << ',' << color[2]);
        // A supported separate dye/streak shares the hair topology; it must not
        // be mistaken for a warm highlight of the dominant dark material.
        const auto source = surface_strip({dark, dark, dark, dark, color, color});
        const auto analysis = labeled(source, std::vector<Label>(6, Label::Hair));
        const std::vector<Color> palette {dark, color};
        const auto output = map_palette(source, analysis, palette);
        REQUIRE(output.size() == 6);
        for (size_t face = 0; face < 4; ++face) CHECK(output[face].second == dark);
        CHECK(output[4].second == color);
        CHECK(output[5].second == color);
    }
}

TEST_CASE("Small white highlights on brown or red hair retain the material color instead of another filament", "[SemanticColoring][Regression][HairColor]")
{
    const auto card = portrait_card();
    const std::vector<Color> materials {{.22f,.15f,.11f}, {.80f,.05f,.04f}};
    for (size_t material = 0; material < materials.size(); ++material) {
        const auto color = materials[material];
        auto source = surface_strip({color, color, color, color, {.98f,.98f,.98f}});
        // The highlight has one fifth of the faces but only about 1.2% of the
        // surface area. Vertex/face counts must not promote it to a dyed patch.
        const auto& last = source.mesh.indices.back();
        const Vec3f midpoint = (source.mesh.vertices[last[0]] + source.mesh.vertices[last[1]]) * .5f;
        source.mesh.vertices.back() = midpoint + (source.mesh.vertices.back() - midpoint) * .05f;
        source.content_id = content_fingerprint(source);
        const auto analysis = labeled(source, std::vector<Label>(5, Label::Hair));
        const auto output = map_palette(source, analysis, card, card);
        REQUIRE(output.size() == 5);
        const auto& expected = card[material == 0 ? 1 : 3];
        for (const auto& face : output) CHECK(face.second == expected);
    }
}

TEST_CASE("Portrait hair protection does not force red blond gray or white hair into the dark role", "[SemanticColoring][Regression][HairColor]")
{
    const auto source = triangles({{.80f,.05f,.04f}, {.90f,.72f,.24f},
        {.55f,.55f,.55f}, {.97f,.97f,.97f}, {.10f,.08f,.07f}});
    const auto analysis = labeled(source, std::vector<Label>(5, Label::Hair));
    const auto card = portrait_card();
    const auto output = map_palette(source, analysis, card, card);
    REQUIRE(output.size() == 5);
    CHECK(output[0].second == card[3]);
    // This card has no golden material. It may approximate blond with a light
    // warm or white material, but the dark-hair guard must not darken it.
    CHECK((output[1].second == card[0] || output[1].second == card[2]));
    CHECK(output[2].second == card[5]);
    CHECK(output[3].second == card[2]);
    CHECK(output[4].second == card[1]);
}

TEST_CASE("Independent neutral gray hair does not become black because the available gray filament is lighter", "[SemanticColoring][Regression][HairColor]")
{
    const auto source = surface_strip({{.35f,.35f,.35f}, {.36f,.36f,.36f},
        {.38f,.38f,.38f}, {.40f,.40f,.40f}});
    const auto analysis = labeled(source, std::vector<Label>(4, Label::Hair));
    const auto card = portrait_card();
    const auto output = map_palette(source, analysis, card, card);
    REQUIRE(output.size() == 4);
    for (const auto& face : output) CHECK(face.second == card[5]);
}

TEST_CASE("A matching brown material remains available for natural hair in an ordinary palette", "[SemanticColoring][Regression][HairColor]")
{
    const Color brown {.42f,.27f,.18f}, black {.10f,.09f,.08f}, white {.97f,.97f,.97f};
    const auto source = surface_strip({brown, {.40f,.26f,.17f}, {.44f,.29f,.20f}, brown});
    const auto analysis = labeled(source, std::vector<Label>(4, Label::Hair));
    const std::vector<Color> palette {{.97f,.88f,.84f}, black, white, {.90f,.58f,.55f}, brown};
    const auto output = map_palette(source, analysis, palette);
    REQUIRE(output.size() == 4);
    for (const auto& face : output) CHECK(face.second == brown);
    auto reordered = palette;
    std::reverse(reordered.begin(), reordered.end());
    CHECK(map_palette(source, analysis, reordered) == output);
}

TEST_CASE("Hair edge completion reaches three shared edges but preserves the fourth and original face identity", "[SemanticColoring][Regression][HairBoundary]")
{
    const Color brown {.22f,.15f,.11f};
    const auto source = hair_edge_strip({brown, brown, brown, brown, brown, brown});
    const auto analysis = hair_edge_labels(source, {Label::Hair, Label::Hair, Label::Unknown,
        Label::Clothes, Label::Background, Label::Unknown});
    const auto before_analysis = encode_analysis(analysis);
    const auto before_vertices = source.mesh.vertices;
    const auto before_indices = source.mesh.indices;
    const auto before_colors = source.face_colors;
    const auto card = portrait_card();
    const FaceColors expected {{0,card[1]}, {1,card[1]}, {2,card[1]}, {3,card[1]}};
    CHECK(map_palette(source, analysis, card, card) == expected);
    CHECK(encode_analysis(analysis) == before_analysis);
    CHECK(source.mesh.vertices == before_vertices);
    CHECK(source.mesh.indices == before_indices);
    CHECK(source.face_colors == before_colors);
    CHECK(content_fingerprint(source) == source.content_id);

    auto reversed = source;
    std::reverse(reversed.mesh.indices.begin(), reversed.mesh.indices.end());
    std::reverse(reversed.face_colors.begin(), reversed.face_colors.end());
    reversed.content_id = content_fingerprint(reversed);
    auto labels = analysis.face_labels;
    std::reverse(labels.begin(), labels.end());
    auto reversed_analysis = labeled(reversed, labels);
    reversed_analysis.face_confidence = analysis.face_confidence;
    std::reverse(reversed_analysis.face_confidence.begin(), reversed_analysis.face_confidence.end());
    FaceColors reversed_expected;
    for (const auto& item : expected)
        reversed_expected.emplace_back(source.mesh.indices.size() - 1 - item.first, item.second);
    std::sort(reversed_expected.begin(), reversed_expected.end());
    CHECK(map_palette(reversed, reversed_analysis, card, card) == reversed_expected);
}

TEST_CASE("Uncertain hair cannot start completion and accepted weak hair cannot act as a seed", "[SemanticColoring][Regression][HairBoundary]")
{
    const Color brown {.22f,.15f,.11f};
    const auto source = hair_edge_strip({brown, brown, brown});
    auto analysis = hair_edge_labels(source, {Label::Hair, Label::Unknown, Label::Hair});
    const auto card = portrait_card();
    analysis.face_confidence[0] = .69f;
    CHECK(map_palette(source, analysis, card, card).empty());
    analysis.face_confidence[0] = .75f;
    const FaceColors only_accepted {{0,card[1]}};
    CHECK(map_palette(source, analysis, card, card) == only_accepted);
}

TEST_CASE("Hair edge completion does not follow gradual color drift beyond its original seed", "[SemanticColoring][Regression][HairBoundary]")
{
    // Every adjacent shade is close, but accepting all three steps would cross
    // the allowed total appearance change from the reliable hair boundary.
    const auto source = hair_edge_strip({{.22f,.15f,.11f}, {.28f,.20f,.15f},
        {.34f,.25f,.19f}, {.40f,.30f,.23f}});
    const auto analysis = hair_edge_labels(source, {Label::Hair, Label::Unknown, Label::Unknown, Label::Unknown});
    const auto card = portrait_card();
    const FaceColors expected {{0,card[1]}, {1,card[1]}, {2,card[1]}};
    CHECK(map_palette(source, analysis, card, card) == expected);
}

TEST_CASE("Hair edge completion stops at reliable other regions and strong source color changes", "[SemanticColoring][Regression][HairBoundary]")
{
    const Color brown {.22f,.15f,.11f};
    const auto card = portrait_card();
    for (const Label protected_label : {Label::FaceSkin, Label::BodySkin, Label::Clothes,
                                       Label::Lips, Label::MouthInterior, Label::Accessories, Label::Background}) {
        INFO("protected label " << int(protected_label));
        const auto source = hair_edge_strip({brown, brown, brown});
        auto analysis = hair_edge_labels(source, {Label::Hair, protected_label, Label::Unknown});
        analysis.face_confidence[1] = .95f;
        const auto output = map_palette(source, analysis, card, card);
        CHECK(std::none_of(output.begin(), output.end(), [](const auto& item) { return item.first == 2; }));
    }
    for (const Color changed : {Color{.96f,.96f,.96f}, Color{.76f,.55f,.41f},
                                Color{.08f,.20f,.50f}, Color{.42f,.29f,.20f}}) {
        INFO("source barrier " << changed[0] << ',' << changed[1] << ',' << changed[2]);
        const auto source = hair_edge_strip({brown, changed, brown});
        const auto analysis = hair_edge_labels(source, {Label::Hair, Label::Unknown, Label::Unknown});
        const FaceColors expected {{0,card[1]}};
        CHECK(map_palette(source, analysis, card, card) == expected);
    }
}

TEST_CASE("Hair edge completion requires a narrow smooth manifold edge rather than a point or duplicate face", "[SemanticColoring][Regression][HairBoundary]")
{
    const Color brown {.22f,.15f,.11f};
    auto source = hair_edge_strip({brown, brown});
    std::vector<Label> labels {Label::Hair, Label::Unknown};
    SECTION("Coarse triangles exceed the spatial radius") {
        source = hair_edge_strip({brown, brown}, 2.f);
    }
    SECTION("Sharing only one original vertex cannot bridge a gap") {
        const int separate = int(source.mesh.vertices.size());
        source.mesh.vertices.emplace_back(0.f, 0.f, .04f);
        source.mesh.indices[1][0] = separate;
    }
    SECTION("A sharp fold is not a continuous hair edge") {
        source.mesh.vertices[3].y() = .10f;
    }
    SECTION("A nonmanifold edge is not a safe bridge") {
        const auto duplicate = source.mesh.indices[1];
        source.mesh.indices.insert(source.mesh.indices.end() - 1, duplicate);
        source.face_colors.insert(source.face_colors.end() - 1, {brown[0],brown[1],brown[2],1.f});
        labels.push_back(Label::Unknown);
    }
    SECTION("Two coincident duplicate faces do not count as a manifold bridge") {
        source.mesh.indices[1] = source.mesh.indices[0];
    }
    SECTION("Reverse winding duplicate faces do not count as a manifold bridge") {
        source.mesh.indices[1] = source.mesh.indices[0];
        std::swap(source.mesh.indices[1][1], source.mesh.indices[1][2]);
    }
    source.content_id = content_fingerprint(source);
    const auto analysis = hair_edge_labels(source, labels);
    const auto card = portrait_card();
    const FaceColors expected {{0,card[1]}};
    CHECK(map_palette(source, analysis, card, card) == expected);
}

TEST_CASE("Hair edge completion preserves reliable eyelids and white or colored eye details", "[SemanticColoring][Regression][HairBoundary]")
{
    const Color brown {.22f,.15f,.11f};
    const auto card = portrait_card();
    for (const Color eye_detail : {Color{.97f,.97f,.97f}, Color{.08f,.20f,.50f}, Color{.22f,.15f,.11f}}) {
        INFO("eye detail " << eye_detail[0] << ',' << eye_detail[1] << ',' << eye_detail[2]);
        const auto source = hair_edge_strip({brown, eye_detail, {.76f,.55f,.41f}});
        auto analysis = hair_edge_labels(source, {Label::Hair, Label::FaceSkin, Label::FaceSkin});
        analysis.face_confidence[1] = .95f;
        analysis.face_confidence[2] = .95f;
        auto without_seed = analysis;
        without_seed.face_labels[0] = Label::Background;
        const auto protected_colors = map_palette(source, without_seed, card, card);
        const auto output = map_palette(source, analysis, card, card);
        FaceColors expected {{0,card[1]}};
        expected.insert(expected.end(), protected_colors.begin(), protected_colors.end());
        CHECK(output == expected);
    }
}

TEST_CASE("An uncertain brown eye contour beside reliable face detail is not swallowed by nearby hair", "[SemanticColoring][Regression][HairBoundary]")
{
    const Color brown {.22f,.15f,.11f};
    const auto source = hair_edge_strip({brown, {.23f,.16f,.12f}, {.97f,.97f,.97f}});
    auto analysis = hair_edge_labels(source, {Label::Hair, Label::Unknown, Label::FaceSkin});
    analysis.face_confidence[2] = .95f;
    const auto card = portrait_card();
    const FaceColors expected {{0,card[1]}, {2,card[2]}};
    CHECK(map_palette(source, analysis, card, card) == expected);
}

TEST_CASE("A neutral source classified as skin does not acquire a portrait flesh color", "[SemanticColoring][Regression]")
{
    const auto source = triangles({{.48f,.48f,.48f}, {.96f,.96f,.96f}});
    const auto analysis = labeled(source, {Label::FaceSkin, Label::BodySkin});
    const auto card = portrait_card();
    const auto output = map_palette(source, analysis, card, card);
    REQUIRE(output.size() == 2);
    CHECK(output[0].second == card[5]);
    CHECK(output[1].second == card[2]);
}

TEST_CASE("Known card clothing uses appearance roles instead of selecting pastel flesh by RGB distance", "[SemanticColoring][Regression]")
{
    const auto source = triangles({{.86f,.82f,.79f}, {.24f,.39f,.38f}, {.40f,.39f,.38f}});
    const auto analysis = labeled(source, {Label::Clothes, Label::Clothes, Label::Clothes});
    const auto card = portrait_card();
    const auto output = map_palette(source, analysis, card, card);
    REQUIRE(output.size() == 3);
    CHECK(output[0].second == card[2]);
    CHECK(output[1].second == card[4]);
    CHECK(output[2].second == card[5]);
}

TEST_CASE("Face skin suggestions preserve source eyebrows lips and white details inside the same connected mask", "[SemanticColoring][Regression]")
{
    const Color skin {.76f,.55f,.41f};
    std::vector<Color> colors(8, skin);
    colors.push_back({.18f,.12f,.10f});
    colors.push_back({.68f,.26f,.24f});
    colors.push_back({.98f,.98f,.98f});
    auto source = triangles(colors);
    source.vertex_colors.clear();
    for (size_t id = 0; id < colors.size(); ++id) {
        for (int corner = 0; corner < 3; ++corner)
            source.mesh.vertices[id * 3 + corner] = source.mesh.vertices[corner];
        source.mesh.indices[id][0] = 0;
        source.face_colors.push_back({colors[id][0], colors[id][1], colors[id][2], 1.f});
    }
    source.content_id = content_fingerprint(source);
    const auto analysis = labeled(source, std::vector<Label>(colors.size(), Label::FaceSkin));
    const auto card = portrait_card();
    const auto output = map_palette(source, analysis, card, card);
    CHECK(std::none_of(output.begin(), output.end(), [](const auto& item) { return item.first == 8; }));
    CHECK(std::none_of(output.begin(), output.end(), [](const auto& item) { return item.first == 9; }));
    const auto teeth = std::find_if(output.begin(), output.end(), [](const auto& item) { return item.first == 10; });
    REQUIRE(teeth != output.end());
    CHECK(teeth->second == card[2]);
    REQUIRE_FALSE(output.empty());
    CHECK(output.front().second == card[0]);
}

TEST_CASE("Neutral stripes keep a neutral material inside a colored clothing region", "[SemanticColoring][Regression]")
{
    const std::vector<Color> colors {{.24f,.39f,.38f}, {.24f,.39f,.38f}, {.24f,.39f,.38f}, {.45f,.45f,.45f}};
    auto source = triangles(colors); source.vertex_colors.clear();
    for (size_t id = 0; id < colors.size(); ++id) {
        for (int corner = 0; corner < 3; ++corner)
            source.mesh.vertices[id * 3 + corner] = source.mesh.vertices[corner];
        source.mesh.indices[id][0] = 0;
        source.face_colors.push_back({colors[id][0], colors[id][1], colors[id][2], 1.f});
    }
    source.content_id = content_fingerprint(source);
    const auto analysis = labeled(source, std::vector<Label>(colors.size(), Label::Clothes));
    const auto card = portrait_card();
    const auto output = map_palette(source, analysis, card, card);
    REQUIRE(output.size() == 4);
    CHECK(output[0].second == card[4]);
    CHECK(output[3].second == card[5]);
}

TEST_CASE("A coherent gray green garment keeps one neutral material across small shading and chroma changes", "[SemanticColoring][Regression]")
{
    // Source shades have Oklab hue 160 degrees, L .34-.39 and C .012-.020,
    // matching the actual garment that previously alternated black/gray/blue.
    const auto source = connected_triangles({
        {.199289f,.227163f,.210808f}, {.209958f,.244136f,.224186f},
        {.218272f,.255104f,.233640f}, {.229018f,.272379f,.247230f},
        {.235582f,.284218f,.256119f}, {.205818f,.238681f,.219484f},
        {.45f,.45f,.45f}, {.8f,.05f,.04f}});
    const auto analysis = labeled(source, std::vector<Label>(8, Label::Clothes));
    const auto card = portrait_card();
    const auto output = map_palette(source, analysis, card, card);
    REQUIRE(output.size() == 8);
    for (size_t face = 0; face < 6; ++face) {
        CHECK(output[face].second == output[0].second);
        CHECK((output[face].second == card[1] || output[face].second == card[5]));
    }
    CHECK(output[6].second == card[5]); // A real gray stripe remains gray.
    CHECK(output[7].second == card[3]); // A supported red patch remains red.
}

TEST_CASE("Body skin accepts warm neck shadows while rejecting red clothing and preserving facial detail rules", "[SemanticColoring][Regression]")
{
    std::vector<Color> colors(8, {.847451f,.569326f,.425459f});
    colors.push_back({.509356f,.327569f,.233013f}); // Same skin hue, L lowered .23.
    colors.push_back({.946937f,.175030f,.158082f}); // Saturated red cloth.
    const auto source = connected_triangles(colors);
    const auto card = portrait_card();
    const auto body = labeled(source, std::vector<Label>(colors.size(), Label::BodySkin));
    const auto output = map_palette(source, body, card, card);
    REQUIRE(output.size() == 9);
    CHECK(output.back().first == 8);
    CHECK(output.back().second == card[0]);
    const auto face = labeled(source, std::vector<Label>(colors.size(), Label::FaceSkin));
    const auto facial_output = map_palette(source, face, card, card);
    CHECK(std::none_of(facial_output.begin(), facial_output.end(), [](const auto& item) { return item.first >= 8; }));
    const auto red = triangles({colors.back()});
    CHECK(map_palette(red, labeled(red, {Label::BodySkin}), card, card).empty());
}

TEST_CASE("Automatic semantic paint falls back for uncertain or nonhuman surfaces and never invents a filament", "[SemanticColoring]")
{
    const auto source = triangles({{.8f,.5f,.3f}, {.1f,.1f,.1f}, {.9f,.1f,.2f}});
    auto analysis = labeled(source, {Label::FaceSkin, Label::Hair, Label::Unknown});
    analysis.face_confidence[0] = .5f;
    const Color only_material {.3f,.3f,.3f};
    const auto output = map_palette(source, analysis, {only_material});
    REQUIRE(output.size() == 1);
    CHECK(output[0].first == 1);
    CHECK(output[0].second == only_material);
    analysis.person_detected = false;
    CHECK(map_palette(source, analysis, {only_material}).empty());
    analysis.person_detected = true; analysis.content_id += "changed";
    CHECK(map_palette(source, analysis, {only_material}).empty());
}

TEST_CASE("Manual face choices override automatic colors and survive disabling semantic coloring", "[SemanticColoring]")
{
    const FaceColors automatic {{0,{1,0,0}}, {1,{0,1,0}}};
    const FaceColors manual {{1,{0,0,1}}, {3,{.4f,.4f,.4f}}};
    const FaceColors expected {{0,{1,0,0}}, {1,{0,0,1}}, {3,{.4f,.4f,.4f}}};
    CHECK(compose(automatic, manual, true) == expected);
    CHECK(compose(automatic, manual, false) == manual);
}

TEST_CASE("Changing an assigned red material to blue retains its faces without nearest color competition", "[SemanticColoring][Regression]")
{
    const Color red {.8f,.1f,.1f}, blue {.1f,.2f,.8f}, gray {.4f,.4f,.4f};
    const FaceColors assigned {{3,red}, {5,gray}, {9,red}};
    const FaceColors expected {{3,blue}, {5,gray}, {9,blue}};
    const auto remapped = remap_palette_targets(assigned, {red, gray}, {blue, gray});
    CHECK(remapped == expected);
    CHECK(assigned[0].second == red);
    const FaceColors manual {{9,red}};
    CHECK(compose(remapped, manual, true).back().second == red);
}

TEST_CASE("Invalid candidate edits cannot invent or partially remap automatic material assignments", "[SemanticColoring]")
{
    const Color red {.8f,.1f,.1f}, blue {.1f,.2f,.8f}, gray {.4f,.4f,.4f};
    const FaceColors assigned {{3,red}, {5,gray}};
    CHECK(remap_palette_targets(assigned, {red,gray}, {blue}).empty());
    CHECK(remap_palette_targets(assigned, {}, {}).empty());
    CHECK(remap_palette_targets(assigned, {red,gray}, {{-1,0,0},gray}).empty());
    CHECK(remap_palette_targets(assigned, {red,blue}, {blue,gray}).empty());
    CHECK(remap_palette_targets({{3,red}}, {red,red}, {blue,gray}).empty());
}

TEST_CASE("Eight semantic views and face crops recognize a person while preserving source face correspondence", "[SemanticColoring]")
{
    const auto source = triangles({{.8f,.5f,.3f}});
    BodyFixture body; FaceFixture face;
    const auto result = analyze(source, body, face);
    REQUIRE(result.error.empty());
    CHECK_FALSE(result.canceled);
    CHECK(result.rendered_views == 8);
    CHECK(body.calls == 8);
    CHECK(face.calls > 0);
    CHECK(result.person_detected);
    REQUIRE(result.face_labels.size() == 1);
    CHECK(result.face_labels[0] == Label::FaceSkin);
    CHECK(result.face_confidence[0] >= minimum_confidence);
}

TEST_CASE("A body mask alone cannot give an animal human skin colors", "[SemanticColoring][Regression]")
{
    const auto source = triangles({{.8f,.5f,.3f}});
    BodyFixture body; FaceFixture face; face.detects_face = false;
    const auto result = analyze(source, body, face);
    REQUIRE(result.error.empty());
    CHECK_FALSE(result.person_detected);
    const auto card = portrait_card();
    CHECK(map_palette(source, result, card, card).empty());
}

TEST_CASE("Disagreeing semantic views keep uncertain faces on the existing color matching path", "[SemanticColoring]")
{
    const auto source = triangles({{.8f,.5f,.3f}});
    BodyFixture body; body.alternate = true; FaceFixture face;
    const auto result = analyze(source, body, face);
    REQUIRE(result.error.empty());
    REQUIRE(result.face_confidence.size() == 1);
    CHECK(result.face_confidence[0] < minimum_confidence);
    CHECK(map_palette(source, result, portrait_card()).empty());
}

TEST_CASE("Reliable face detail masks can distinguish lips from the surrounding skin region", "[SemanticColoring]")
{
    const auto source = triangles({{.6f,.2f,.2f}});
    BodyFixture body; FaceFixture face; face.detail = Label::Lips;
    const auto result = analyze(source, body, face);
    REQUIRE(result.error.empty());
    REQUIRE(result.face_labels.size() == 1);
    CHECK(result.face_labels[0] == Label::Lips);
}

TEST_CASE("Four-color pale clothing and shaded ears reuse material slots without consuming lip red",
          "[SemanticColoring][Regression]")
{
    const Color skin {247.f/255, 226.f/255, 218.f/255};
    const Color dark {40.f/255, 38.f/255, 41.f/255};
    const Color white {246.f/255, 247.f/255, 249.f/255};
    const Color red {234.f/255, 154.f/255, 146.f/255};
    const std::vector<Color> palette {skin, dark, white, red};
    const auto source = triangles({
        {.76f,.55f,.41f}, {.61f,.43f,.35f},
        {.86f,.82f,.79f}, {.89f,.86f,.83f}, {.80f,.78f,.76f}, {.62f,.60f,.58f},
        {.68f,.26f,.24f}, {.8f,.05f,.04f}});
    const auto analysis = labeled(source, {
        Label::FaceSkin, Label::FaceSkin,
        Label::Clothes, Label::Clothes, Label::Clothes, Label::Clothes,
        Label::Lips, Label::Clothes});
    const auto mapped = map_palette(source, analysis, palette);
    REQUIRE(mapped.size() == 8);
    CHECK(mapped[0].second == skin);
    CHECK(mapped[1].second == skin);
    for (size_t face : {size_t(2), size_t(3), size_t(4), size_t(5)}) CHECK(mapped[face].second == white);
    CHECK(mapped[6].second == red);
    CHECK(mapped[7].second == red);
}

TEST_CASE("Four-color warm cream clothing keeps its matching filament", "[SemanticColoring][Regression]")
{
    const Color cream {.91f,.84f,.77f};
    const Color white {.97f,.97f,.98f};
    const Color skin {.87f,.62f,.49f};
    const Color dark {.1f,.1f,.1f};
    const auto source = triangles({cream, {.6f,.6f,.6f}});
    const auto analysis = labeled(source, {Label::Clothes, Label::Clothes});
    const auto mapped = map_palette(source, analysis, {dark, skin, white, cream});
    REQUIRE(mapped.size() == 2);
    CHECK(mapped[0].second == cream);
    CHECK(mapped[1].second == white);
}

TEST_CASE("Nearby uncertain neutral shirt faces borrow white without crossing a stripe or skin",
          "[SemanticColoring][Regression]")
{
    const Color white {.97f,.97f,.98f};
    const Color skin {.87f,.62f,.49f};
    const Color dark {.1f,.1f,.1f};
    const Color red {.85f,.25f,.22f};
    const auto source = surface_strip({
        {.87f,.86f,.88f}, {.86f,.85f,.87f}, {.84f,.83f,.85f},
        {.82f,.81f,.83f}, {.80f,.79f,.81f}, {.78f,.77f,.79f},
        {.45f,.45f,.45f}, {.85f,.61f,.48f}});
    auto analysis = labeled(source, {Label::Clothes, Label::Clothes, Label::Clothes,
        Label::Clothes, Label::Clothes, Label::Clothes, Label::Clothes, Label::FaceSkin});
    for (size_t id = 2; id <= 6; ++id) analysis.face_confidence[id] = .5f;
    const auto mapped = map_palette(source, analysis, {dark, skin, white, red});
    const auto target = [&](size_t id) {
        const auto found = std::find_if(mapped.begin(), mapped.end(), [id](const auto& item) { return item.first == id; });
        return found == mapped.end() ? Color{} : found->second;
    };
    CHECK(target(2) == white);
    CHECK(target(3) == white);
    CHECK(target(4) == white);
    CHECK(target(6) != white);
    CHECK(target(7) == skin);
}

TEST_CASE("Face recognizer skin evidence can correct a body hair label", "[SemanticColoring][Regression][FaceSkin]")
{
    const auto source = triangles({{.6f,.4f,.3f}});
    BodyFixture body; body.primary = Label::Hair; body.alternate = true; body.alternate_label = Label::FaceSkin;
    FaceFixture face; face.detail = Label::FaceSkin;
    const auto result = analyze(source, body, face);
    REQUIRE(result.error.empty());
    REQUIRE(result.face_labels.size() == 1);
    CHECK(result.face_labels[0] == Label::FaceSkin);
    CHECK(result.face_confidence[0] >= minimum_confidence);
}

TEST_CASE("Diagnostic face ROI resolution cannot enter cached or material-mapped product results",
          "[SemanticColoring][FaceResolution]")
{
    const auto source = triangles({{.8f,.5f,.3f}});
    BodyFixture body; FaceFixture face; face.detail = Label::EyeSclera;
    AnalysisDiagnostics diagnostics;
    diagnostics.face_regions.push_back({99, {}, false, {}});
    const auto result = analyze_with_face_roi_size(source, body, face, 1024, {}, {}, &diagnostics);
    REQUIRE(result.error.empty());
    CHECK(result.face_roi_size == 1024);
    REQUIRE_FALSE(face.image_sizes.empty());
    CHECK(std::all_of(face.image_sizes.begin(), face.image_sizes.end(), [](const auto& size) {
        return size.first == 1024 && size.second == 1024;
    }));
    CHECK(result.signature != analysis_cache_key(source, body.identity(), face.identity()));
    REQUIRE_FALSE(diagnostics.face_regions.empty());
    CHECK(std::none_of(diagnostics.face_regions.begin(), diagnostics.face_regions.end(), [](const auto& region) {
        return region.view_index == 99;
    }));
    CHECK(std::all_of(diagnostics.face_regions.begin(), diagnostics.face_regions.end(), [](const auto& region) {
        return !region.face_detected || !region.leaves.empty();
    }));
    CHECK(map_palette(source, result, portrait_card(), portrait_card()).empty());
    SubfaceBudgetResult subfaces;
    std::string error;
    CHECK_FALSE(map_subface_palette(source, result, {}, portrait_card(), portrait_card(), {}, subfaces, error));
    CHECK_THROWS_AS(encode_analysis(result), std::invalid_argument);

    FaceFixture filtered_face; filtered_face.detail = Label::EyeSclera;
    diagnostics.include_leaf = [](size_t, const SubfacePath&) { return false; };
    const auto filtered = analyze_with_face_roi_size(source, body, filtered_face, 1024, {}, {}, &diagnostics);
    REQUIRE(filtered.error.empty());
    CHECK(std::all_of(diagnostics.face_regions.begin(), diagnostics.face_regions.end(), [](const auto& region) {
        return region.leaves.empty();
    }));

    diagnostics.yaw_offset_degrees = 23.f;
    FaceFixture offset_face;
    const auto invalid_offset = analyze_with_face_roi_size(source, body, offset_face, 1024, {}, {}, &diagnostics);
    CHECK(invalid_offset.error.find("yaw offset") != std::string::npos);
    CHECK(offset_face.calls == 0);

    FaceFixture invalid_face;
    const auto invalid = analyze_with_face_roi_size(source, body, invalid_face, 1000);
    CHECK_FALSE(invalid.error.empty());
    CHECK(invalid_face.calls == 0);
}

TEST_CASE("Subpixel eye leaves use only uniform depth-compatible semantic support",
          "[SemanticColoring][SubfaceColor][EyeBoundary]")
{
    MeshSnapshot source;
    source.mesh.vertices = {
        {-100.f, 0.f, -100.f}, {100.f, 0.f, -100.f}, {0.f, 0.f, 100.f},
        {7.113f, -.01f, 3.446f}, {7.133f, -.01f, 3.446f}, {7.123f, -.01f, 3.466f}};
    source.mesh.indices = {{0, 1, 2}, {3, 4, 5}};
    source.face_colors = {{.65f, .65f, .65f, 1.f}, {.65f, .65f, .65f, 1.f}};
    source.geometry_id = "fixture-subpixel-eye-surface";
    source.content_id = content_fingerprint(source);
    const auto rendered = render_view(source, 0.f, 512);
    REQUIRE(rendered.error.empty());
    CHECK(std::count(rendered.face_ids.begin(), rendered.face_ids.end(), uint32_t(1)) == 0);
    BodyFixture body;
    FaceFixture uniform; uniform.detail = Label::EyeSclera;
    const auto supported = analyze(source, body, uniform);
    REQUIRE(supported.error.empty());
    const size_t supported_tiny_leaves = std::count_if(
        supported.subface_labels.begin(), supported.subface_labels.end(), [](const auto& evidence) {
            return evidence.face_id == 1 && evidence.label == Label::EyeSclera;
        });
    CHECK(supported_tiny_leaves == 16);

    SplitEyeFixture split;
    const auto conflicted = analyze(source, body, split);
    REQUIRE(conflicted.error.empty());
    const size_t conflicted_tiny_leaves = std::count_if(
        conflicted.subface_labels.begin(), conflicted.subface_labels.end(), [](const auto& evidence) {
            return evidence.face_id == 1;
        });
    CHECK(conflicted_tiny_leaves < supported_tiny_leaves);
}

TEST_CASE("Recognizer failures and cancellation cannot apply or persist partial semantic analysis", "[SemanticColoring]")
{
    const auto source = triangles({{.8f,.5f,.3f}});
    BodyFixture body; FaceFixture face;
    const auto canceled = analyze(source, body, face, [] { return true; });
    CHECK(canceled.canceled);
    CHECK(body.calls == 0);
    CHECK_THROWS_AS(encode_analysis(canceled), std::invalid_argument);
    body.failure = "Model unavailable";
    const auto failed = analyze(source, body, face);
    CHECK(failed.error == body.failure);
    CHECK(failed.face_labels.empty());
    CHECK(map_palette(source, failed, portrait_card()).empty());
    CHECK_THROWS_AS(encode_analysis(failed), std::invalid_argument);
}

TEST_CASE("Semantic cache preserves confidence decisions and invalidates either model or recognizer changes", "[SemanticColoring]")
{
    auto source = triangles({{.5f,.5f,.5f}, {.6f,.6f,.6f}});
    auto result = labeled(source, {Label::Clothes, Label::Hair});
    result.face_confidence[0] = minimum_confidence;
    result.face_confidence[1] = std::nextafter(minimum_confidence, 0.f);
    const auto doc = encode_analysis(result);
    Analysis restored; std::string error;
    REQUIRE(decode_analysis(doc, source, result.body_identity, result.face_identity, restored, error));
    CHECK(restored.face_labels == result.face_labels);
    CHECK_THAT(restored.face_confidence[0], WithinAbs(result.face_confidence[0], 0));
    CHECK(map_palette(source, restored, portrait_card()) == map_palette(source, result, portrait_card()));
    const auto old_signature = restored.signature;
    CHECK_FALSE(decode_analysis(doc, source, "changed-body", result.face_identity, restored, error));
    CHECK(restored.signature == old_signature);
    CHECK_FALSE(decode_analysis(doc, source, result.body_identity, "changed-face", restored, error));
    source.content_id += "changed-colors";
    CHECK_FALSE(decode_analysis(doc, source, result.body_identity, result.face_identity, restored, error));
    CHECK(restored.signature == old_signature);
}

TEST_CASE("Malformed semantic caches leave the current analysis intact", "[SemanticColoring]")
{
    const auto source = triangles({{.5f,.5f,.5f}});
    auto current = labeled(source, {Label::Hair});
    auto doc = encode_analysis(current);
    doc["labels"] = "f";
    std::string error;
    CHECK_FALSE(decode_analysis(doc, source, current.body_identity, current.face_identity, current, error));
    CHECK(current.face_labels == std::vector<Label>{Label::Hair});
    doc = encode_analysis(current); doc["confidence_f32"] = "xxxxxxxx";
    CHECK_FALSE(decode_analysis(doc, source, current.body_identity, current.face_identity, current, error));
    CHECK(current.face_labels == std::vector<Label>{Label::Hair});
    doc = encode_analysis(current); doc["face_count"] = std::numeric_limits<uint64_t>::max();
    CHECK_FALSE(decode_analysis(doc, source, current.body_identity, current.face_identity, current, error));
}
