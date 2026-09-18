#pragma once

#include "SemanticColoring.hpp"
#include <filesystem>
#include <memory>

namespace Slic3r::AI::SemanticColoring {

struct RegionRecognizers {
    std::unique_ptr<IBodyRegionRecognizer> body;
    std::unique_ptr<IFaceRegionRecognizer> face;
    std::string error;
};
using BodyRecognizerFactory = std::function<std::unique_ptr<IBodyRegionRecognizer>(const std::filesystem::path&)>;
using FaceRecognizerFactory = std::function<std::unique_ptr<IFaceRegionRecognizer>(const std::filesystem::path&)>;

// Register at application composition time. Factories are copied under a lock,
// then invoked outside it. Body and face implementations can be replaced alone.
bool register_body_recognizer_factory(const std::string& id, BodyRecognizerFactory);
bool register_face_recognizer_factory(const std::string& id, FaceRecognizerFactory);
RegionRecognizers create_region_recognizers(const std::string& body_provider,
                                           const std::string& face_provider,
                                           const std::filesystem::path& runtime_dir);
RegionRecognizers create_region_recognizers(const std::string& provider,
                                           const std::filesystem::path& runtime_dir);
RegionRecognizers create_mediapipe_recognizers(const std::filesystem::path& runtime_dir);

// Adapter-owned landmark conversion, also available without the native DLL for
// geometry/mask regression tests. Business callers consume Prediction only.
namespace MediaPipeFaceMasks {
struct Landmark { float x {-1.f}, y {-1.f}; };
using FaceLandmarks = std::vector<Landmark>;
inline constexpr size_t maximum_faces = 4;
Prediction from_landmarks(const RGBImage&, const std::vector<FaceLandmarks>&,
                          const Cancel& = {});
}

} // namespace Slic3r::AI::SemanticColoring
