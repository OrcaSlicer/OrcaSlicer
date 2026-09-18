#include <catch2/catch_test_macros.hpp>
#include "slic3r/AI/ModelGeneration/SemanticColoring/SemanticMaskRefinement.hpp"

using namespace Slic3r::AI::SemanticColoring;

namespace {

struct Fixture {
    RGBImage image;
    Prediction prediction;
    explicit Fixture(int width = 20, int height = 6) {
        image.width = width; image.height = height;
        image.pixels.assign(size_t(width) * height * 3, 255);
        prediction.labels.assign(size_t(width) * height, Label::Unknown);
        prediction.confidence.assign(size_t(width) * height, 0.f);
        prediction.person_detected = true;
    }
    size_t pixel(int x, int y) const { return size_t(y) * image.width + x; }
    void set(int x, int y, std::array<uint8_t, 3> color, Label label, float confidence) {
        const size_t id = pixel(x, y);
        for (int channel = 0; channel < 3; ++channel) image.pixels[id * 3 + channel] = color[channel];
        prediction.labels[id] = label; prediction.confidence[id] = confidence;
    }
};

constexpr std::array<uint8_t, 3> brown {{55, 38, 29}};
constexpr std::array<uint8_t, 3> skin {{190, 145, 120}};
constexpr std::array<uint8_t, 3> shaded_skin {{175, 135, 107}};

} // namespace

TEST_CASE("A source-continuous dark hair component repairs reliable body-skin holes", "[SemanticMaskRefinement][Regression]")
{
    Fixture fixture;
    for (int x = 1; x < 19; ++x) for (int y = 1; y < 4; ++y)
        fixture.set(x, y, brown, x < 4 ? Label::Hair : Label::BodySkin, .92f);
    REQUIRE(refine_dark_hair_mask(fixture.image, fixture.prediction));
    for (int x = 1; x < 19; ++x) for (int y = 1; y < 4; ++y) {
        CHECK(fixture.prediction.labels[fixture.pixel(x, y)] == Label::Hair);
        CHECK(fixture.prediction.confidence[fixture.pixel(x, y)] >= .82f);
    }
}

TEST_CASE("Hair mask refinement preserves warm skin reliable face skin and reliable clothing", "[SemanticMaskRefinement][Regression]")
{
    Fixture fixture(24, 8);
    for (int x = 1; x < 12; ++x) for (int y = 1; y < 4; ++y)
        fixture.set(x, y, brown, x < 4 ? Label::Hair : Label::BodySkin, .92f);
    for (int x = 12; x < 15; ++x) for (int y = 1; y < 4; ++y)
        fixture.set(x, y, skin, Label::BodySkin, .95f);
    for (int y = 1; y < 4; ++y) {
        fixture.set(15, y, brown, Label::FaceSkin, .95f);
        fixture.set(16, y, brown, Label::Clothes, .95f);
    }
    REQUIRE(refine_dark_hair_mask(fixture.image, fixture.prediction));
    for (int x = 12; x < 15; ++x) for (int y = 1; y < 4; ++y)
        CHECK(fixture.prediction.labels[fixture.pixel(x, y)] == Label::BodySkin);
    for (int y = 1; y < 4; ++y) {
        CHECK(fixture.prediction.labels[fixture.pixel(15, y)] == Label::FaceSkin);
        CHECK(fixture.prediction.labels[fixture.pixel(16, y)] == Label::Clothes);
    }
}

TEST_CASE("An isolated dark region and too little hair support remain unchanged", "[SemanticMaskRefinement]")
{
    Fixture fixture(20, 8);
    for (int x = 1; x < 8; ++x) for (int y = 1; y < 3; ++y)
        fixture.set(x, y, brown, Label::BodySkin, .9f);
    for (int x = 10; x < 14; ++x)
        fixture.set(x, 1, brown, Label::Hair, .9f);
    const auto before = fixture.prediction;
    REQUIRE(refine_dark_hair_mask(fixture.image, fixture.prediction));
    CHECK(fixture.prediction.labels == before.labels);
    CHECK(fixture.prediction.confidence == before.confidence);
}

TEST_CASE("A source-continuous hair mask does not absorb shaded ear and neck skin", "[SemanticMaskRefinement][Regression]")
{
    Fixture fixture(24, 8);
    for (int x = 1; x < 10; ++x) for (int y = 1; y < 5; ++y)
        fixture.set(x, y, brown, Label::Hair, .92f);
    for (int x = 10; x < 20; ++x) for (int y = 1; y < 5; ++y)
        fixture.set(x, y, shaded_skin, Label::BodySkin, .92f);
    REQUIRE(refine_dark_hair_mask(fixture.image, fixture.prediction));
    for (int x = 10; x < 20; ++x) for (int y = 1; y < 5; ++y)
        CHECK(fixture.prediction.labels[fixture.pixel(x, y)] == Label::BodySkin);
}

TEST_CASE("Cancellation leaves the semantic prediction transactional", "[SemanticMaskRefinement]")
{
    Fixture fixture(64, 64);
    for (int x = 0; x < 64; ++x) for (int y = 0; y < 64; ++y)
        fixture.set(x, y, brown, x < 16 ? Label::Hair : Label::BodySkin, .9f);
    const auto before = fixture.prediction;
    size_t calls = 0;
    CHECK_FALSE(refine_dark_hair_mask(fixture.image, fixture.prediction, [&] { return ++calls >= 2; }));
    CHECK(fixture.prediction.labels == before.labels);
    CHECK(fixture.prediction.confidence == before.confidence);
}
