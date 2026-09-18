#include <catch2/catch_all.hpp>

#include "slic3r/GUI/AI/Model/ColorTrialState.hpp"

#include <limits>

using namespace Slic3r;
namespace trial = Slic3r::AI::ColorTrialPersistence;

namespace {
const std::string geometry_id = "0537362e094bab981ceddf4d6341b427c97c1412526c38eccc3bfd18724af2ab";
trial::State saved_trial()
{
    trial::State state;
    state.mapping_colors = {{0.1f, 0.7f, 0.15f}, {0.1f, 0.1f, 0.1f}, {0.9f, 0.7f, 0.5f}};
    state.colors = {{0.1f, 0.2f, 0.9f}, {0.05f, 0.05f, 0.05f}, {1.0f, 0.7f, 0.8f}};
    state.source = 2;
    state.count = 6;
    state.enabled = true;
    state.fidelity = false;
    state.lighting = true;
    state.locks = {true, false, true, false, false, false};
    return state;
}
void require_same(const trial::State& a, const trial::State& b)
{
    REQUIRE(a.colors == b.colors);
    REQUIRE(a.mapping_colors == b.mapping_colors);
    REQUIRE(a.locks == b.locks);
    REQUIRE(a.source == b.source);
    REQUIRE(a.count == b.count);
    REQUIRE(a.enabled == b.enabled);
    REQUIRE(a.fidelity == b.fidelity);
    REQUIRE(a.lighting == b.lighting);
    REQUIRE(a.semantic_optimization == b.semantic_optimization);
    REQUIRE(a.semantic_palette == b.semantic_palette);
    REQUIRE(a.semantic_mapping_palette == b.semantic_mapping_palette);
    REQUIRE(a.semantic_portrait_card == b.semantic_portrait_card);
}
}

TEST_CASE("Semantic trial saves the full candidate palette separately from repeated global targets", "[ColorTrialState][SemanticColoring]")
{
    auto saved = saved_trial();
    saved.colors = {{1.f, 1.f, 1.f}, {1.f, 1.f, 1.f}, {.9f, .7f, .6f}};
    saved.semantic_palette = {{1.f, 1.f, 1.f}, {.5f, .5f, .5f}, {.9f, .7f, .6f}, {.1f, .1f, .1f}};
    saved.semantic_mapping_palette = {{.8f, .8f, .8f}, {.2f, .5f, .1f}, {.9f, .7f, .6f}, {.1f, .1f, .1f}};
    saved.semantic_optimization = false;
    trial::State restored; std::string error;
    REQUIRE(trial::decode(trial::encode(saved, 20, geometry_id), 20, geometry_id, restored, error));
    require_same(saved, restored);
    REQUIRE(restored.semantic_palette.size() == 4);
    REQUIRE(restored.colors[0] == restored.colors[1]);
}

TEST_CASE("Legacy trials keep their existing assignments until semantic optimization is enabled", "[ColorTrialState][SemanticColoring]")
{
    auto doc = trial::encode(saved_trial(), 20, geometry_id);
    doc.erase("semantic_optimization"); doc.erase("semantic_palette"); doc.erase("semantic_portrait_card");
    doc.erase("semantic_mapping_palette");
    trial::State restored; std::string error;
    REQUIRE(trial::decode(doc, 20, geometry_id, restored, error));
    REQUIRE_FALSE(restored.semantic_optimization);
    REQUIRE(restored.colors == saved_trial().colors);
}

TEST_CASE("Invalid semantic candidates cannot partially replace a saved trial", "[ColorTrialState][SemanticColoring]")
{
    const auto saved = saved_trial();
    auto doc = trial::encode(saved, 20, geometry_id);
    doc["semantic_palette"] = {{1.01, .3, .2}};
    auto restored = saved; std::string error;
    REQUIRE_FALSE(trial::decode(doc, 20, geometry_id, restored, error));
    require_same(restored, saved);
}

TEST_CASE("Semantic target edits restore their original candidate assignments and reject partial pairs", "[ColorTrialState][SemanticColoring]")
{
    auto saved = saved_trial();
    saved.semantic_mapping_palette = {{0.f, .5f, 0.f}, {.4f, .4f, .4f}};
    saved.semantic_palette = {{0.f, 0.f, 1.f}, {.4f, .4f, .4f}};
    auto doc = trial::encode(saved, 20, geometry_id);
    auto restored = saved; std::string error;
    REQUIRE(trial::decode(doc, 20, geometry_id, restored, error));
    require_same(restored, saved);
    doc["semantic_mapping_palette"].erase(0);
    REQUIRE_FALSE(trial::decode(doc, 20, geometry_id, restored, error));
    require_same(restored, saved);
}

TEST_CASE("Saved trial colors preserve original group centers separately from edited targets", "[ColorTrialState]")
{
    const auto saved = saved_trial();
    const auto doc = trial::encode(saved, 1906896, geometry_id);
    trial::State restored;
    std::string error;
    REQUIRE(trial::decode(nlohmann::json::parse(doc.dump()), 1906896, geometry_id, restored, error));
    REQUIRE(error.empty());
    require_same(restored, saved);
    // Green membership still uses the original green center after its target
    // is changed to blue. Reconstructing centers from target colors loses it.
    std::vector<GUI::PreviewPalette::Color> centers;
    for (const auto& color : restored.mapping_colors) centers.push_back(GUI::PreviewPalette::to_lab(color));
    const size_t group = GUI::PreviewPalette::nearest_lab_index(GUI::PreviewPalette::to_lab(saved.mapping_colors[0]), centers);
    REQUIRE(group == 0);
    REQUIRE(restored.colors[group] == saved.colors[0]);
    REQUIRE(restored.mapping_colors != restored.colors);
}

TEST_CASE("A disabled saved trial retains its centers targets and lighting preference", "[ColorTrialState]")
{
    auto saved = saved_trial();
    saved.enabled = false;
    trial::State restored;
    std::string error;
    REQUIRE(trial::decode(trial::encode(saved, 20, geometry_id), 20, geometry_id, restored, error));
    require_same(restored, saved);
    REQUIRE_FALSE(restored.enabled);
    REQUIRE(restored.lighting);
}

TEST_CASE("A requested color limit can exceed the number of colors present in the model", "[ColorTrialState]")
{
    const auto saved = saved_trial();
    REQUIRE(saved.count > int(saved.colors.size()));
    trial::State restored;
    std::string error;
    REQUIRE(trial::decode(trial::encode(saved, 20, geometry_id), 20, geometry_id, restored, error));
    require_same(restored, saved);
}

TEST_CASE("A thirty two color trial preserves every color and lock", "[ColorTrialState]")
{
    auto saved = saved_trial();
    saved.colors.clear();
    saved.mapping_colors.clear();
    saved.count = int(GUI::PreviewPalette::max_preview_colors);
    for (size_t i = 0; i < GUI::PreviewPalette::max_preview_colors; ++i) {
        const float channel = float(i) / float(GUI::PreviewPalette::max_preview_colors - 1);
        saved.colors.push_back({channel, 1.f - channel, .25f});
        saved.mapping_colors.push_back({.5f * channel, channel, 1.f - channel});
        saved.locks[i] = i % 3 == 0;
    }
    trial::State restored;
    std::string error;
    REQUIRE(trial::decode(trial::encode(saved, 20, geometry_id), 20, geometry_id, restored, error));
    require_same(restored, saved);
}

TEST_CASE("An oversized trial retains the requested semantic option while its candidates are paused",
          "[ColorTrialState][SemanticColoring][Regression]")
{
    auto saved = saved_trial();
    saved.colors.assign(7, {.5f, .5f, .5f});
    saved.mapping_colors = saved.colors;
    saved.count = 7;
    saved.semantic_optimization = true;
    saved.semantic_palette.clear();
    saved.semantic_mapping_palette.clear();
    trial::State restored;
    std::string error;
    REQUIRE(trial::decode(trial::encode(saved, 20, geometry_id), 20, geometry_id, restored, error));
    require_same(restored, saved);
    CHECK(restored.semantic_optimization);
    CHECK(restored.semantic_palette.empty());
}

TEST_CASE("Legacy six lock snapshots remain readable", "[ColorTrialState]")
{
    const auto saved = saved_trial();
    auto doc = trial::encode(saved, 20, geometry_id);
    doc["locks"] = nlohmann::json::array({true, false, true, false, false, false});
    trial::State restored;
    std::string error;
    REQUIRE(trial::decode(doc, 20, geometry_id, restored, error));
    REQUIRE(restored.locks[0]);
    REQUIRE(restored.locks[2]);
    for (size_t i = 6; i < restored.locks.size(); ++i) REQUIRE_FALSE(restored.locks[i]);
}

TEST_CASE("An empty disabled trial is valid but an enabled trial requires paired colors", "[ColorTrialState]")
{
    trial::State empty, restored;
    std::string error;
    REQUIRE(trial::decode(trial::encode(empty, 20, geometry_id), 20, geometry_id, restored, error));
    require_same(restored, empty);
    auto invalid = trial::encode(empty, 20, geometry_id);
    invalid["enabled"] = true;
    REQUIRE_FALSE(trial::decode(invalid, 20, geometry_id, restored, error));
    require_same(restored, empty);
}

TEST_CASE("Trial state rejects a different model or an oversized claimed face count atomically", "[ColorTrialState]")
{
    const auto saved = saved_trial();
    auto doc = trial::encode(saved, 20, geometry_id);
    auto restored = saved;
    std::string error;
    REQUIRE_FALSE(trial::decode(doc, 20, std::string(64, '0'), restored, error));
    REQUIRE_FALSE(error.empty());
    require_same(restored, saved);
    doc["face_count"] = std::numeric_limits<uint64_t>::max();
    REQUIRE_FALSE(trial::decode(doc, 20, geometry_id, restored, error));
    require_same(restored, saved);
    doc["face_count"] = 20.0;
    REQUIRE_FALSE(trial::decode(doc, 20, geometry_id, restored, error));
    require_same(restored, saved);
    REQUIRE_FALSE(trial::decode(nlohmann::json::array(), 20, geometry_id, restored, error));
    require_same(restored, saved);
}

TEST_CASE("Every persisted trial field is explicit instead of silently rebuilding missing centers", "[ColorTrialState]")
{
    const auto saved = saved_trial();
    for (const char* field : {"schema", "geometry_sha256", "face_count", "colors", "mapping_colors", "locks",
                             "source", "count", "enabled", "fidelity", "lighting"}) {
        DYNAMIC_SECTION("Missing field " << field) {
            auto doc = trial::encode(saved, 20, geometry_id);
            doc.erase(field);
            auto restored = saved;
            std::string error;
            REQUIRE_FALSE(trial::decode(doc, 20, geometry_id, restored, error));
            REQUIRE_FALSE(error.empty());
            require_same(restored, saved);
        }
    }
}

TEST_CASE("Trial options and locks reject numeric string and null coercions", "[ColorTrialState]")
{
    const auto saved = saved_trial();
    for (const char* field : {"enabled", "fidelity", "lighting"}) {
        for (const nlohmann::json bad : {nlohmann::json(1), nlohmann::json("true"), nlohmann::json(nullptr)}) {
            DYNAMIC_SECTION(field << " with " << bad.dump()) {
                auto doc = trial::encode(saved, 20, geometry_id);
                doc[field] = bad;
                auto restored = saved;
                std::string error;
                REQUIRE_FALSE(trial::decode(doc, 20, geometry_id, restored, error));
                require_same(restored, saved);
            }
        }
    }
    auto doc = trial::encode(saved, 20, geometry_id);
    doc["locks"][0] = 1;
    auto restored = saved;
    std::string error;
    REQUIRE_FALSE(trial::decode(doc, 20, geometry_id, restored, error));
    require_same(restored, saved);
    doc["locks"] = nlohmann::json::array({true, false});
    REQUIRE_FALSE(trial::decode(doc, 20, geometry_id, restored, error));
    require_same(restored, saved);
}

TEST_CASE("Trial source count and paired palette sizes stay inside their supported bounds", "[ColorTrialState]")
{
    const auto saved = saved_trial();
    const std::vector<std::pair<std::string, nlohmann::json>> invalid {
        {"source", -1}, {"source", 3}, {"source", 1.0}, {"source", "2"},
        {"count", 0}, {"count", 33}, {"count", 1.5}, {"count", 2},
        {"schema", "orca.color-trial/v2"}, {"geometry_sha256", 123},
        {"mapping_colors", nlohmann::json::array()},
        {"colors", nlohmann::json::array({{0, 0, 0}})}
    };
    for (size_t i = 0; i < invalid.size(); ++i) {
        DYNAMIC_SECTION("Invalid field " << i) {
            auto doc = trial::encode(saved, 20, geometry_id);
            doc[invalid[i].first] = invalid[i].second;
            auto restored = saved;
            std::string error;
            REQUIRE_FALSE(trial::decode(doc, 20, geometry_id, restored, error));
            require_same(restored, saved);
        }
    }
}

TEST_CASE("Both source and target palettes reject malformed nonfinite and out of range RGB", "[ColorTrialState]")
{
    const auto saved = saved_trial();
    const std::vector<nlohmann::json> invalid {
        nlohmann::json::array({{-0.1, 0, 0}}),
        nlohmann::json::array({{1.1, 0, 0}}),
        nlohmann::json::array({{"0.5", 0, 0}}),
        nlohmann::json::array({{true, 0, 0}}),
        nlohmann::json::array({{std::numeric_limits<double>::infinity(), 0, 0}}),
        nlohmann::json::array({{std::numeric_limits<double>::quiet_NaN(), 0, 0}}),
        nlohmann::json::array({{0, 0}}),
        nlohmann::json::array({{0, 0, 0, 1}}),
        nlohmann::json::array({{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
                               {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
                               {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
                               {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
                               {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}}),
        nlohmann::json::object()
    };
    for (const char* field : {"colors", "mapping_colors"}) {
        for (size_t i = 0; i < invalid.size(); ++i) {
            DYNAMIC_SECTION(field << " invalid palette " << i) {
                auto doc = trial::encode(saved, 20, geometry_id);
                doc[field] = invalid[i];
                auto restored = saved;
                std::string error;
                REQUIRE_FALSE(trial::decode(doc, 20, geometry_id, restored, error));
                require_same(restored, saved);
            }
        }
    }
}

TEST_CASE("Invalid outgoing trial states cannot be saved as apparently valid snapshots", "[ColorTrialState]")
{
    auto state = saved_trial();
    state.source = 3;
    REQUIRE_THROWS_AS(trial::encode(state, 20, geometry_id), std::invalid_argument);
    state = saved_trial(); state.count = 33;
    REQUIRE_THROWS_AS(trial::encode(state, 20, geometry_id), std::invalid_argument);
    state = saved_trial(); state.mapping_colors.pop_back();
    REQUIRE_THROWS_AS(trial::encode(state, 20, geometry_id), std::invalid_argument);
    state = saved_trial(); state.colors[0][0] = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_THROWS_AS(trial::encode(state, 20, geometry_id), std::invalid_argument);
    REQUIRE_THROWS_AS(trial::encode(saved_trial(), 20, ""), std::invalid_argument);
}
