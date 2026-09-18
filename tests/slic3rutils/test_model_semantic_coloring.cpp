#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "test_utils.hpp"
#include "slic3r/AI/ModelGeneration/SemanticColoring/MediaPipeRegionRecognizers.hpp"
#include "slic3r/GUI/AI/ModelGeneration/ModelSemanticColoring.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

using namespace Slic3r;
namespace SC = Slic3r::AI::SemanticColoring;
using Coordinator = Slic3r::GUI::ModelSemanticColoring;
using Catch::Matchers::WithinAbs;
using namespace std::chrono_literals;

namespace {
struct Calls {
    std::atomic<unsigned> body {0}, face {0};
    std::atomic<bool> hold_body {false}, body_entered {false};
};

class FakeBody final : public SC::IBodyRegionRecognizer {
    std::string m_identity;
    std::shared_ptr<Calls> m_calls;
public:
    FakeBody(std::string identity, std::shared_ptr<Calls> calls)
        : m_identity(std::move(identity)), m_calls(std::move(calls)) {}
    std::string identity() const override { return m_identity; }
    SC::Prediction predict(const SC::RGBImage& image, const SC::Cancel&) override {
        ++m_calls->body;
        m_calls->body_entered = true;
        // Simulate a native inference call which finishes after cancellation.
        // The bound also guarantees teardown if a main-thread REQUIRE fails.
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (m_calls->hold_body && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
        SC::Prediction prediction;
        prediction.labels.assign(size_t(image.width) * image.height, SC::Label::FaceSkin);
        prediction.confidence.assign(prediction.labels.size(), .95f);
        prediction.person_detected = true;
        return prediction;
    }
};

class FakeFace final : public SC::IFaceRegionRecognizer {
    std::string m_identity;
    std::shared_ptr<Calls> m_calls;
public:
    FakeFace(std::string identity, std::shared_ptr<Calls> calls)
        : m_identity(std::move(identity)), m_calls(std::move(calls)) {}
    std::string identity() const override { return m_identity; }
    SC::Prediction predict(const SC::RGBImage& image, const SC::Cancel&) override {
        ++m_calls->face;
        SC::Prediction prediction;
        prediction.labels.assign(size_t(image.width) * image.height, SC::Label::Unknown);
        prediction.confidence.assign(prediction.labels.size(), .95f);
        prediction.face_detected = true;
        return prediction;
    }
};

struct Providers {
    std::string body, face;
    std::shared_ptr<Calls> calls = std::make_shared<Calls>();
};

struct Fixture {
    // Reuse the suite's temp guard: cache/configuration never touch user data.
    ScopedTemporaryDir directory {"orca-semantic-coordinator"};
    const std::filesystem::path runtime {directory.path().native()};
    const std::filesystem::path cache {runtime / "cache"};

    Providers add_providers(const std::string& suffix) {
        const std::string prefix = directory.path().filename().string() + suffix;
        Providers result {prefix + ".body", prefix + ".face"};
        const auto calls = result.calls;
        const auto body_id = result.body, face_id = result.face;
        REQUIRE(SC::register_body_recognizer_factory(body_id, [calls, body_id](const std::filesystem::path&) {
            return std::make_unique<FakeBody>(body_id, calls);
        }));
        REQUIRE(SC::register_face_recognizer_factory(face_id, [calls, face_id](const std::filesystem::path&) {
            return std::make_unique<FakeFace>(face_id, calls);
        }));
        return result;
    }

    void configure(const std::string& body, const std::string& face) const {
        std::ofstream file(runtime / "providers.json", std::ios::binary | std::ios::trunc);
        REQUIRE(file.good());
        file << nlohmann::json {{"body_provider", body}, {"face_provider", face}};
        file.close();
        REQUIRE(file.good());
    }
};

const SC::Color skin {.8f, .5f, .3f}, gray {.4f, .4f, .4f}, blue {0.f, 0.f, 1.f};
const std::vector<SC::Color> palette {skin, gray};

std::shared_ptr<const SC::MeshSnapshot> source(const std::string& identity, float offset = 0.f)
{
    auto mesh = std::make_shared<SC::MeshSnapshot>();
    mesh->mesh.vertices = {{offset - 1.f, 0.f, -1.f}, {offset + 1.f, 0.f, -1.f}, {offset, 0.f, 1.f}};
    mesh->mesh.indices.emplace_back(0, 1, 2);
    mesh->vertex_colors.assign(3, {skin[0], skin[1], skin[2], 1.f});
    mesh->geometry_id = identity;
    mesh->content_id = SC::content_fingerprint(*mesh);
    return mesh;
}

template<class Predicate> bool wait_until(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

std::unique_ptr<Coordinator::Result> await_result(Coordinator& coordinator)
{
    std::unique_ptr<Coordinator::Result> result;
    wait_until([&] { result = coordinator.poll(); return bool(result) || !coordinator.busy(); });
    return result;
}

void check_color(const SC::Color& actual, const SC::Color& expected)
{
    for (size_t channel = 0; channel < 3; ++channel)
        CHECK_THAT(actual[channel], WithinAbs(expected[channel], 1e-6f));
}
} // namespace

TEST_CASE("Switching models and recognizers during inference discards the previous semantic result", "[ModelSemanticColoring][Regression]")
{
    Fixture fixture;
    auto old = fixture.add_providers("-old"), current = fixture.add_providers("-current");
    fixture.configure(old.body, old.face);
    old.calls->hold_body = true;
    Coordinator coordinator(fixture.runtime, fixture.cache);
    const auto previous_source = source("previous"), current_source = source("current", 3.f);
    REQUIRE(coordinator.request(previous_source, palette, palette, {}, {}));
    REQUIRE(wait_until([&] { return old.calls->body_entered.load(); }));

    fixture.configure(current.body, current.face);
    REQUIRE(coordinator.request(current_source, palette, palette, {}, {}));
    old.calls->hold_body = false;
    auto result = await_result(coordinator);
    REQUIRE(result);
    REQUIRE(result->error.empty());
    REQUIRE(result->analysis);
    CHECK(result->analysis->content_id == current_source->content_id);
    CHECK(result->analysis->body_identity == current.body);
    CHECK(result->analysis->face_identity == current.face);
    CHECK_FALSE(result->cache_hit);
    CHECK(current.calls->body.load() > 0);
    CHECK_FALSE(coordinator.poll());
    CHECK_FALSE(coordinator.busy());
}

TEST_CASE("Changing one recognizer configuration invalidates completed semantic analysis", "[ModelSemanticColoring][Regression]")
{
    Fixture fixture;
    auto previous = fixture.add_providers("-first"), replacement = fixture.add_providers("-second");
    fixture.configure(previous.body, previous.face);
    Coordinator coordinator(fixture.runtime, fixture.cache);
    const auto mesh = source("unchanged");
    REQUIRE(coordinator.request(mesh, palette, palette, {}, {}));
    auto first = await_result(coordinator);
    REQUIRE(first);
    REQUIRE(first->error.empty());
    REQUIRE(first->analysis);

    fixture.configure(replacement.body, previous.face);
    REQUIRE(coordinator.request(mesh, palette, palette, {}, {}));
    auto second = await_result(coordinator);
    REQUIRE(second);
    REQUIRE(second->error.empty());
    REQUIRE(second->analysis);
    CHECK_FALSE(second->cache_hit);
    CHECK(second->analysis->signature != first->analysis->signature);
    CHECK(second->analysis->body_identity == replacement.body);
    CHECK(second->analysis->face_identity == previous.face);
    CHECK(replacement.calls->body.load() > 0);
    CHECK(replacement.calls->face.load() == 0);
}

TEST_CASE("Editing semantic target colors reuses recognition and preserves the assigned surface", "[ModelSemanticColoring][Regression]")
{
    Fixture fixture;
    auto provider = fixture.add_providers("-palette");
    fixture.configure(provider.body, provider.face);
    Coordinator coordinator(fixture.runtime, fixture.cache);
    const auto mesh = source("palette-edit");
    REQUIRE(coordinator.request(mesh, palette, palette, {}, {}));
    auto first = await_result(coordinator);
    REQUIRE(first);
    REQUIRE(first->error.empty());
    REQUIRE(first->automatic.size() == 1);
    REQUIRE_FALSE(first->geometry.is_empty());
    check_color(first->automatic.front().second, skin);
    const unsigned body_calls = provider.calls->body.load(), face_calls = provider.calls->face.load();
    REQUIRE(body_calls > 0);
    REQUIRE(face_calls > 0);

    REQUIRE(coordinator.request(mesh, palette, {blue, gray}, {}, {}));
    auto second = await_result(coordinator);
    REQUIRE(second);
    REQUIRE(second->error.empty());
    REQUIRE(second->automatic.size() == 1);
    REQUIRE_FALSE(second->geometry.is_empty());
    CHECK(second->cache_hit);
    CHECK(second->automatic.front().first == first->automatic.front().first);
    check_color(second->automatic.front().second, blue);
    CHECK(provider.calls->body.load() == body_calls);
    CHECK(provider.calls->face.load() == face_calls);
    CHECK_FALSE(coordinator.request(mesh, palette, {blue, gray}, {}, {}));
}

TEST_CASE("One through five candidate colors reuse recognition without adding a material",
          "[ModelSemanticColoring][SemanticColoring][Regression]")
{
    Fixture fixture;
    auto provider = fixture.add_providers("-limited-palette");
    fixture.configure(provider.body, provider.face);
    Coordinator coordinator(fixture.runtime, fixture.cache);
    const auto mesh = source("limited-palette");
    const std::vector<SC::Color> candidates {skin, gray, blue, {1.f, 1.f, 1.f}, {1.f, 0.f, 0.f}, {0.f, 0.f, 0.f}};
    unsigned body_calls = 0, face_calls = 0;
    for (size_t count = 1; count <= 6; ++count) {
        INFO("Candidate colors: " << count);
        const std::vector<SC::Color> selected(candidates.begin(), candidates.begin() + count);
        REQUIRE(coordinator.request(mesh, selected, selected, {}, {}));
        auto result = await_result(coordinator);
        REQUIRE(result);
        REQUIRE(result->error.empty());
        REQUIRE(result->analysis);
        REQUIRE(result->automatic.size() == 1);
        REQUIRE_FALSE(result->geometry.is_empty());
        CHECK(result->automatic.front().first == 0);
        CHECK(std::find(selected.begin(), selected.end(), result->automatic.front().second) != selected.end());
        if (count == 1) check_color(result->automatic.front().second, skin);
        if (count > 1) {
            CHECK(result->cache_hit);
            CHECK(provider.calls->body.load() == body_calls);
            CHECK(provider.calls->face.load() == face_calls);
        }
        body_calls = provider.calls->body.load();
        face_calls = provider.calls->face.load();
    }
    CHECK(body_calls > 0);
    CHECK(face_calls > 0);
}

TEST_CASE("Cancelled semantic work cannot return when optimization is enabled again", "[ModelSemanticColoring][Regression]")
{
    Fixture fixture;
    auto provider = fixture.add_providers("-cancel");
    fixture.configure(provider.body, provider.face);
    provider.calls->hold_body = true;
    Coordinator coordinator(fixture.runtime, fixture.cache);
    const auto mesh = source("cancel-and-reenable");
    REQUIRE(coordinator.request(mesh, palette, palette, {}, {}));
    REQUIRE(wait_until([&] { return provider.calls->body_entered.load(); }));
    coordinator.cancel();
    CHECK_FALSE(coordinator.busy());
    CHECK_FALSE(coordinator.poll());
    provider.calls->hold_body = false;
    REQUIRE(coordinator.request(mesh, palette, {blue, gray}, {}, {}));
    auto result = await_result(coordinator);
    REQUIRE(result);
    REQUIRE(result->error.empty());
    REQUIRE(result->automatic.size() == 1);
    check_color(result->automatic.front().second, blue);
    CHECK_FALSE(coordinator.poll());
    CHECK_FALSE(coordinator.busy());
}

TEST_CASE("Semantic preview emits the same sparse midpoint leaves used by MMU persistence",
          "[ModelSemanticColoring][SubfaceColor]")
{
    const auto mesh = source("subface-preview");
    const SC::FaceColors roots {{0, skin}};
    const SC::SubfaceColors leaves {
        {0, {1, 0}, gray, .95f},
        {0, {2, uint8_t((1u << 2) | 3u)}, blue, .90f},
    };
    const auto geometry = Slic3r::GUI::build_semantic_colored_geometry(*mesh, roots, leaves);
    // Root child 1 is split again: three first-level leaves plus four
    // second-level leaves give seven rendered triangles.
    REQUIRE(geometry.vertices_count() == 21);
    REQUIRE(geometry.indices_count() == 21);
    double area = 0.0;
    for (size_t triangle = 0; triangle < geometry.indices_count() / 3; ++triangle) {
        const Vec3f a = geometry.extract_position_3(triangle * 3);
        const Vec3f b = geometry.extract_position_3(triangle * 3 + 1);
        const Vec3f c = geometry.extract_position_3(triangle * 3 + 2);
        area += .5 * double((b - a).cross(c - a).norm());
    }
    CHECK_THAT(area, WithinAbs(2.0, 1e-6));

    const SC::FaceColors manual {{0, blue}};
    const auto overridden = Slic3r::GUI::build_semantic_colored_geometry(
        *mesh, SC::compose(roots, manual, true), SC::compose_subfaces(leaves, manual, true));
    CHECK(overridden.indices_count() == 3);
}
