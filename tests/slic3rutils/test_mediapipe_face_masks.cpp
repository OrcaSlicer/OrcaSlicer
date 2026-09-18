#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "slic3r/AI/ModelGeneration/SemanticColoring/MediaPipeRegionRecognizers.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

using namespace Slic3r;
using namespace Slic3r::AI::SemanticColoring;

namespace {
using MediaPipeFaceMasks::FaceLandmarks;
// Adapter fixtures use the pinned provider topology; no GUI/domain code needs
// these indices. Points follow its ordered eyelid and iris connection loops.
constexpr int right_eye_ids[] = {33,7,163,144,145,153,154,155,133,173,157,158,159,160,161,246};
constexpr int left_eye_ids[] = {263,249,390,373,374,380,381,382,362,398,384,385,386,387,388,466};
constexpr int right_iris_ids[] = {469,470,471,472};
constexpr int left_iris_ids[] = {474,475,476,477};
constexpr int right_eyebrow_ids[] = {46,53,52,65,55,107,66,105,63,70};
constexpr int left_eyebrow_ids[] = {276,283,282,295,285,336,296,334,293,300};
constexpr int outer_lip_ids[] = {61,146,91,181,84,17,314,405,321,375,291,409,270,269,267,0,37,39,40,185};
constexpr int inner_lip_ids[] = {78,95,88,178,87,14,317,402,318,324,308,415,310,311,312,13,82,81,80,191};
constexpr int right_ear_ids[] = {127,234,132};
constexpr int left_ear_ids[] = {356,454,361};
constexpr int skin_sample_ids[] = {50,101,205,280,330,425};
RGBImage image_fixture()
{
    return {256,160,std::vector<uint8_t>(256 * 160 * 3,128)};
}
template<size_t N>
void ellipse(FaceLandmarks& face, const RGBImage& image, const int (&ids)[N],
             float cx, float cy, float rx, float ry, float rotation = 0.f)
{
    constexpr float pi = 3.14159265358979323846f;
    for (size_t i = 0; i < N; ++i) {
        const float angle = 2.f * pi * float(i) / float(N);
        const float x = rx * std::cos(angle), y = ry * std::sin(angle);
        face[size_t(ids[i])] = {(cx + x * std::cos(rotation) - y * std::sin(rotation)) / image.width,
                               (cy + x * std::sin(rotation) + y * std::cos(rotation)) / image.height};
    }
}
FaceLandmarks face_fixture(const RGBImage& image, float scale = 1.f, float offset_x = 0.f)
{
    FaceLandmarks face(478);
    ellipse(face,image,right_eye_ids,offset_x + 70.5f * scale,52.5f * scale,26.f * scale,11.f * scale);
    ellipse(face,image,right_iris_ids,offset_x + 70.5f * scale,52.5f * scale,8.f * scale,8.f * scale);
    ellipse(face,image,left_eye_ids,offset_x + 180.5f * scale,52.5f * scale,26.f * scale,11.f * scale);
    ellipse(face,image,left_iris_ids,offset_x + 180.5f * scale,52.5f * scale,8.f * scale,8.f * scale);
    ellipse(face,image,right_eyebrow_ids,offset_x + 70.5f * scale,27.5f * scale,23.f * scale,4.f * scale);
    ellipse(face,image,left_eyebrow_ids,offset_x + 180.5f * scale,27.5f * scale,23.f * scale,4.f * scale);
    ellipse(face,image,outer_lip_ids,offset_x + 126.5f * scale,112.5f * scale,26.f * scale,10.f * scale);
    ellipse(face,image,inner_lip_ids,offset_x + 126.5f * scale,112.5f * scale,16.f * scale,4.f * scale);
    return face;
}
void add_ear_fixture(FaceLandmarks& face, const RGBImage& image)
{
    const auto set = [&](int id, float x, float y) { face[size_t(id)] = {x / image.width, y / image.height}; };
    set(right_ear_ids[0],34.f,45.f);set(right_ear_ids[1],28.f,75.f);set(right_ear_ids[2],34.f,105.f);
    set(left_ear_ids[0],222.f,45.f);set(left_ear_ids[1],228.f,75.f);set(left_ear_ids[2],222.f,105.f);
    for (size_t i = 0; i < std::size(skin_sample_ids); ++i)
        set(skin_sample_ids[i],100.f + float(i % 2) * 56.f,45.f + float(i / 2) * 25.f);
}
Label at(const Prediction& prediction, const RGBImage& image, int x, int y)
{
    return prediction.labels[size_t(y) * image.width + x];
}

}

TEST_CASE("Native face topology separates both sclera and curved irises while preserving mouth semantics", "[MediaPipeFaceMasks][EyeSemantics]")
{
    const auto image = image_fixture();
    const auto face = face_fixture(image);
    const auto result = MediaPipeFaceMasks::from_landmarks(image,{face});
    REQUIRE(result.valid_for(image));
    CHECK(result.face_detected);
    CHECK(at(result,image,70,52) == Label::Iris);
    CHECK(at(result,image,180,52) == Label::Iris);
    CHECK(at(result,image,51,52) == Label::EyeSclera);
    CHECK(at(result,image,161,52) == Label::EyeSclera);
    // This point is inside an iris circle but outside the four-rim-point diamond.
    CHECK(at(result,image,75,57) == Label::Iris);
    CHECK(at(result,image,70,27) == Label::Eyebrow);
    CHECK(at(result,image,180,27) == Label::Eyebrow);
    CHECK(at(result,image,70,63) == Label::Unknown);
    CHECK(at(result,image,70,65) == Label::Unknown);
    CHECK(at(result,image,126,112) == Label::MouthInterior);
    CHECK(at(result,image,126,120) == Label::Lips);
    CHECK(at(result,image,126,122) == Label::Unknown);
    CHECK(at(result,image,142,112) == Label::Unknown);
}

TEST_CASE("Closed unresolved or invalid eyes cannot whiten the eyelids or disable a valid mouth", "[MediaPipeFaceMasks][EyeSemantics]")
{
    const auto image = image_fixture();
    auto face = face_fixture(image);
    SECTION("Closed eyelids") { ellipse(face,image,left_eye_ids,180.5f,52.5f,26.f,.4f); }
    SECTION("Unresolved small aperture") {
        ellipse(face,image,left_eye_ids,180.5f,52.5f,3.f,1.f);
        ellipse(face,image,left_iris_ids,180.5f,52.5f,1.f,1.f);
    }
    SECTION("Invalid iris coordinates") { face[left_iris_ids[0]].x = std::numeric_limits<float>::quiet_NaN(); }
    SECTION("Collapsed iris geometry") { for (int id : left_iris_ids) face[id] = {180.5f/image.width,52.5f/image.height}; }
    SECTION("Iris outside the aperture") { ellipse(face,image,left_iris_ids,225.5f,52.5f,8.f,8.f); }
    const auto result = MediaPipeFaceMasks::from_landmarks(image,{face});
    REQUIRE(result.valid_for(image));
    CHECK(at(result,image,180,52) == Label::Unknown);
    CHECK(at(result,image,161,52) == Label::Unknown);
    CHECK(at(result,image,70,52) == Label::Iris);
    CHECK(at(result,image,126,112) == Label::MouthInterior);
    CHECK(at(result,image,126,120) == Label::Lips);
}

TEST_CASE("Rotated elliptical irises remain clipped to the eye opening", "[MediaPipeFaceMasks][EyeSemantics]")
{
    const auto image = image_fixture();
    auto face = face_fixture(image);
    ellipse(face,image,right_eye_ids,70.5f,52.5f,26.f,11.f,.5f);
    ellipse(face,image,right_iris_ids,70.5f,52.5f,8.f,5.f,.5f);
    const auto result = MediaPipeFaceMasks::from_landmarks(image,{face});
    REQUIRE(result.valid_for(image));
    CHECK(at(result,image,70,52) == Label::Iris);
    CHECK(at(result,image,86,61) == Label::EyeSclera);
    CHECK(at(result,image,64,67) == Label::Unknown);
}

TEST_CASE("Each detected person receives independent eye and mouth semantics", "[MediaPipeFaceMasks][EyeSemantics]")
{
    const auto image = image_fixture();
    const auto result = MediaPipeFaceMasks::from_landmarks(image,{face_fixture(image,.5f),face_fixture(image,.5f,128.f)});
    REQUIRE(result.valid_for(image));
    CHECK(at(result,image,35,26) == Label::Iris);
    CHECK(at(result,image,90,26) == Label::Iris);
    CHECK(at(result,image,163,26) == Label::Iris);
    CHECK(at(result,image,218,26) == Label::Iris);
    CHECK(at(result,image,26,26) == Label::EyeSclera);
    CHECK(at(result,image,154,26) == Label::EyeSclera);
    CHECK(at(result,image,35,13) == Label::Eyebrow);
    CHECK(at(result,image,218,13) == Label::Eyebrow);
    CHECK(at(result,image,63,56) == Label::MouthInterior);
    CHECK(at(result,image,191,56) == Label::MouthInterior);
}

TEST_CASE("Collapsed or invalid eyebrow contours preserve forehead skin and other face details", "[MediaPipeFaceMasks][EyebrowSemantics]")
{
    const auto image = image_fixture();
    auto face = face_fixture(image);
    SECTION("Collapsed eyebrow") {
        for (int id : right_eyebrow_ids) face[size_t(id)] = {70.5f / image.width, 27.5f / image.height};
    }
    SECTION("Invalid eyebrow landmark") {
        face[size_t(right_eyebrow_ids[0])].x = std::numeric_limits<float>::quiet_NaN();
    }
    const auto result = MediaPipeFaceMasks::from_landmarks(image,{face});
    REQUIRE(result.valid_for(image));
    CHECK(at(result,image,70,27) == Label::Unknown);
    CHECK(at(result,image,180,27) == Label::Eyebrow);
    CHECK(at(result,image,70,52) == Label::Iris);
    CHECK(at(result,image,126,120) == Label::Lips);
}

TEST_CASE("Landmark ear regions add connected skin evidence without painting occluding hair", "[MediaPipeFaceMasks][EarSemantics]")
{
    auto image = image_fixture();
    for (size_t pixel = 0; pixel < image.pixels.size() / 3; ++pixel) {
        image.pixels[pixel * 3] = 194; image.pixels[pixel * 3 + 1] = 140; image.pixels[pixel * 3 + 2] = 105;
    }
    auto face = face_fixture(image); add_ear_fixture(face,image);
    const auto visible = MediaPipeFaceMasks::from_landmarks(image,{face});
    REQUIRE(visible.valid_for(image));
    CHECK(at(visible,image,8,75) == Label::FaceSkin);
    CHECK(at(visible,image,20,75) == Label::FaceSkin);
    CHECK(at(visible,image,236,75) == Label::FaceSkin);

    for (const std::array<uint8_t, 3> hair : {std::array<uint8_t, 3>{30,28,27}, {70,45,30}}) {
        auto occluded_image = image;
        for (int y = 38; y <= 112; ++y) for (int x = 0; x <= 43; ++x) {
            const size_t pixel = size_t(y) * image.width + x;
            for (int channel = 0; channel < 3; ++channel)
                occluded_image.pixels[pixel * 3 + channel] = hair[size_t(channel)];
        }
        const auto occluded = MediaPipeFaceMasks::from_landmarks(occluded_image,{face});
        REQUIRE(occluded.valid_for(occluded_image));
        CHECK(at(occluded,occluded_image,8,75) == Label::Unknown);
        CHECK(at(occluded,occluded_image,20,75) == Label::Unknown);
        CHECK(at(occluded,occluded_image,236,75) == Label::FaceSkin);
    }
}

TEST_CASE("Oblique iris landmarks retain an uncertain boundary and reject unresolved thin ellipses", "[MediaPipeFaceMasks][EyeSemantics]")
{
    const auto image = image_fixture();
    auto face = face_fixture(image);
    const float scale = GENERATE(1.f, .25f);
    const std::array<MediaPipeFaceMasks::Landmark, 4> offsets {{{8.f,0.f}, {6.f,4.f}, {-8.f,0.f}, {-6.f,-4.f}}};
    for (size_t i = 0; i < offsets.size(); ++i)
        face[right_iris_ids[i]] = {(70.5f + offsets[i].x * scale) / image.width,
                                  (52.5f + offsets[i].y * scale) / image.height};
    const auto result = MediaPipeFaceMasks::from_landmarks(image,{face});
    REQUIRE(result.valid_for(image));
    // At full scale this pixel lies at most .2 px from the actual ellipse.
    // At quarter scale the short semi-axis is below the resolvable threshold.
    CHECK(at(result,image,70,55) == Label::Unknown);
    if (scale > .5f) CHECK(at(result,image,70,52) == Label::Iris);
    else CHECK(at(result,image,70,52) == Label::Unknown);
    CHECK(at(result,image,180,52) == Label::Iris);
}

TEST_CASE("Conflicting overlapping face masks abstain independently of face result order", "[MediaPipeFaceMasks][EyeSemantics]")
{
    const auto image = image_fixture();
    const auto first = face_fixture(image);
    auto second = first;
    ellipse(second,image,right_iris_ids,82.5f,52.5f,8.f,8.f);
    const auto forward = MediaPipeFaceMasks::from_landmarks(image,{first,second});
    const auto reverse = MediaPipeFaceMasks::from_landmarks(image,{second,first});
    REQUIRE(forward.valid_for(image));
    REQUIRE(reverse.valid_for(image));
    CHECK(at(forward,image,70,52) == Label::Unknown);
    CHECK(forward.labels == reverse.labels);
    REQUIRE(forward.confidence.size() == reverse.confidence.size());
    float maximum_confidence_difference = 0.f;
    for (size_t i = 0; i < forward.confidence.size(); ++i)
        maximum_confidence_difference = std::max(maximum_confidence_difference,
            std::abs(forward.confidence[i] - reverse.confidence[i]));
    CHECK_THAT(maximum_confidence_difference, Catch::Matchers::WithinAbs(0.f, 1e-7f));
    CHECK(at(forward,image,126,112) == Label::MouthInterior);
}

TEST_CASE("Face mask conversion validates shape and cancellation without requiring a native runtime", "[MediaPipeFaceMasks][EyeSemantics]")
{
    const auto image = image_fixture();
    auto malformed = face_fixture(image); malformed.pop_back();
    CHECK_FALSE(MediaPipeFaceMasks::from_landmarks(image,{malformed}).error.empty());
    CHECK_FALSE(MediaPipeFaceMasks::from_landmarks(image,std::vector<FaceLandmarks>(5,face_fixture(image))).error.empty());
    const auto absent = MediaPipeFaceMasks::from_landmarks(image,{});
    CHECK(absent.valid_for(image));
    CHECK_FALSE(absent.face_detected);
    const auto canceled = MediaPipeFaceMasks::from_landmarks(image,{face_fixture(image)},[] { return true; });
    CHECK(canceled.canceled);
    CHECK_FALSE(canceled.valid_for(image));
}
