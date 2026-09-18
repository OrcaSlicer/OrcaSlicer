#include "MediaPipeRegionRecognizers.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>

#if defined(_WIN32) && defined(ORCA_ENABLE_MEDIAPIPE_NATIVE)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
// The vendor headers are unmodified. We load the C ABI dynamically rather than
// re-exporting its functions or sharing the C++ ABI of the MediaPipe library.
#define MP_EXPORT
#include "mediapipe/tasks/c/vision/image_segmenter/image_segmenter.h"
#include "mediapipe/tasks/c/vision/face_landmarker/face_landmarker.h"
#undef MP_EXPORT
#endif

namespace Slic3r::AI::SemanticColoring {
namespace {
constexpr const char* provider_id = "mediapipe.cpu.v1";
constexpr const char* dll_hash = "a8970c645c8c87c25ec9965cb5c898e803c6c42f7192b7de9a0541c62ae48cef";
constexpr const char* body_hash = "c6748b1253a99067ef71f7e26ca71096cd449baefa8f101900ea23016507e0e0";
constexpr const char* face_hash = "64184e229b263107bc2b804c6625db1341ff2bb731874b0bcc2fe6544e0bc9ff";
bool canceled(const Cancel& cancel) { return cancel && cancel(); }
Prediction empty_prediction(const RGBImage& image)
{
    Prediction out;
    if (image.width > 0 && image.height > 0 && image.width <= 4096 && image.height <= 4096 &&
        image.pixels.size() == size_t(image.width) * size_t(image.height) * 3) {
        out.labels.assign(size_t(image.width) * size_t(image.height), Label::Unknown);
        out.confidence.assign(out.labels.size(), 0.f);
    } else out.error = "Invalid semantic recognition image.";
    return out;
}
template<class Port> class Unavailable final : public Port {
    std::string m_id, m_error;
public:
    Unavailable(std::string id, std::string error) : m_id(std::move(id)), m_error(std::move(error)) {}
    std::string identity() const override { return m_id + "/unavailable"; }
    Prediction predict(const RGBImage& image, const Cancel& cancel) override {
        auto out = empty_prediction(image); out.error = m_error; out.canceled = canceled(cancel); return out;
    }
};

struct Point { float x, y; };
bool inside_polygon(float x, float y, const std::vector<Point>& points)
{
    bool inside = false;
    for (size_t i = 0, j = points.size() - 1; i < points.size(); j = i++) {
        const auto& a = points[i]; const auto& b = points[j];
        if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x) inside = !inside;
    }
    return inside;
}
float edge_distance(float x, float y, const std::vector<Point>& points)
{
    float best = 1e9f;
    for (size_t i = 0; i < points.size(); ++i) {
        const auto& a = points[i]; const auto& b = points[(i + 1) % points.size()];
        float dx = b.x - a.x, dy = b.y - a.y, length = dx * dx + dy * dy;
        float t = length > 0 ? std::clamp(((x - a.x) * dx + (y - a.y) * dy) / length, 0.f, 1.f) : 0.f;
        best = std::min(best, std::hypot(x - a.x - t * dx, y - a.y - t * dy));
    }
    return best;
}
Color image_lab(const RGBImage& image, size_t pixel)
{
    Color color {};
    for (int channel = 0; channel < 3; ++channel) {
        float value = image.pixels[pixel * 3 + channel] / 255.f;
        color[channel] = value <= .04045f ? value / 12.92f : std::pow((value + .055f) / 1.055f, 2.4f);
    }
    const float l = std::cbrt(.4122214708f*color[0] + .5363325363f*color[1] + .0514459929f*color[2]);
    const float m = std::cbrt(.2119034982f*color[0] + .6806995451f*color[1] + .1073969566f*color[2]);
    const float s = std::cbrt(.0883024619f*color[0] + .2817188376f*color[1] + .6299787005f*color[2]);
    return {.2104542553f*l + .793617785f*m - .0040720468f*s,
        1.9779984951f*l - 2.428592205f*m + .4505937099f*s,
        .0259040371f*l + .7827717662f*m - .808675766f*s};
}

#if defined(_WIN32) && defined(ORCA_ENABLE_MEDIAPIPE_NATIVE)
struct ScopeExit {
    std::function<void()> fn;
    ~ScopeExit() { if (fn) fn(); }
};
std::vector<char> verified_file(const std::filesystem::path& path, const char* expected)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("Semantic runtime file is missing: " + path.filename().u8string());
    const auto length = stream.tellg();
    if (length <= 0 || length > 128 * 1024 * 1024) throw std::runtime_error("Invalid semantic runtime file size.");
    std::vector<char> bytes(static_cast<size_t>(length)); stream.seekg(0);
    if (!stream.read(bytes.data(), std::streamsize(bytes.size()))) throw std::runtime_error("Cannot read semantic runtime file.");
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Cannot initialize runtime SHA256 verification.");
    ScopeExit close_alg {[&] { BCryptCloseAlgorithmProvider(alg, 0); }};
    DWORD object_size = 0, written = 0;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &written, 0) < 0)
        throw std::runtime_error("Cannot query runtime SHA256 parameters.");
    std::vector<unsigned char> object(object_size); BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(alg, &hash, object.data(), object_size, nullptr, 0, 0) < 0)
        throw std::runtime_error("Cannot create runtime SHA256 context.");
    ScopeExit close_hash {[&] { BCryptDestroyHash(hash); }};
    unsigned char digest[32] {};
    if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(bytes.data()), ULONG(bytes.size()), 0) < 0 ||
        BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0)
        throw std::runtime_error("Cannot verify semantic runtime SHA256.");
    std::ostringstream hex;
    for (auto c : digest) hex << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
    if (hex.str() != expected) throw std::runtime_error("Semantic runtime checksum mismatch: " + path.filename().u8string());
    return bytes;
}

#define MP_FUNCTIONS(X) \
    X(MpErrorFree) X(MpImageCreateFromUint8Data) X(MpImageFree) X(MpImageGetWidth) X(MpImageGetHeight) \
    X(MpImageDataFloat32) X(MpImageSegmenterCreate) X(MpImageSegmenterSegmentImage) \
    X(MpImageSegmenterCloseResult) X(MpImageSegmenterClose) X(MpFaceLandmarkerCreate) \
    X(MpFaceLandmarkerDetectImage) X(MpFaceLandmarkerCloseResult) X(MpFaceLandmarkerClose)
class NativeApi {
    HMODULE m_dll = nullptr;
public:
#define DECLARE(name) decltype(&name) name = nullptr;
    MP_FUNCTIONS(DECLARE)
#undef DECLARE
    explicit NativeApi(const std::filesystem::path& root) {
        const auto file = std::filesystem::absolute(root / "libmediapipe.dll");
        verified_file(file, dll_hash);
        m_dll = LoadLibraryExW(file.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!m_dll) throw std::runtime_error("Cannot load native semantic runtime (Windows error " + std::to_string(GetLastError()) + ").");
        try {
#define LOAD(name) name = reinterpret_cast<decltype(name)>(GetProcAddress(m_dll, #name)); if (!name) throw std::runtime_error("Incompatible semantic runtime: " #name);
            MP_FUNCTIONS(LOAD)
#undef LOAD
        } catch (...) { FreeLibrary(m_dll); m_dll = nullptr; throw; }
    }
    ~NativeApi() { if (m_dll) FreeLibrary(m_dll); }
    void check(MpStatus status, char* message) const {
        std::string error = message ? message : "Native semantic task failed.";
        if (message) MpErrorFree(message);
        if (status != 0) throw std::runtime_error(error);
    }
    void discard_error(char* message) const { if (message) MpErrorFree(message); }
};
#undef MP_FUNCTIONS
std::shared_ptr<NativeApi> native_api(const std::filesystem::path& root)
{
    static std::mutex mutex;
    static std::map<std::filesystem::path, std::weak_ptr<NativeApi>> libraries;
    std::lock_guard<std::mutex> lock(mutex);
    auto& entry = libraries[std::filesystem::absolute(root).lexically_normal()];
    auto api = entry.lock();
    if (!api) { api = std::make_shared<NativeApi>(root); entry = api; }
    return api;
}
MpBaseOptions base_options(const std::vector<char>& model)
{
    MpBaseOptions options {};
    // Buffer input supports installation paths containing Chinese characters.
    options.model_asset_buffer = model.data(); options.model_asset_buffer_count = unsigned(model.size());
    options.file_descriptor = -1; options.delegate = MP_DELEGATE_CPU;
    options.host_system = MP_HOST_SYSTEM_WINDOWS;
    return options;
}
class BodyRecognizer final : public IBodyRegionRecognizer {
    std::filesystem::path m_root;
    std::shared_ptr<NativeApi> m_api;
    std::vector<char> m_model;
    MpImageSegmenterPtr m_task = nullptr;
    std::mutex m_mutex;
public:
    explicit BodyRecognizer(std::filesystem::path root) : m_root(std::move(root)) {}
    ~BodyRecognizer() {
        if (m_task) { char* error = nullptr; m_api->MpImageSegmenterClose(m_task, &error); m_api->discard_error(error); }
    }
    std::string identity() const override { return std::string(provider_id) + "/1.0.0/" + dll_hash + "/" + body_hash + "/body-map-v1/rgb8-image-v1"; }
    Prediction predict(const RGBImage& image, const Cancel& cancel) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto out = empty_prediction(image);
        if (!out.error.empty()) return out;
        if (canceled(cancel)) { out.canceled = true; return out; }
        try {
            if (!m_task) {
                m_api = native_api(m_root); m_model = verified_file(m_root / "selfie_multiclass_256x256.tflite", body_hash);
                MpImageSegmenterOptions options {}; options.base_options = base_options(m_model);
                options.running_mode = MP_RUNNING_MODE_IMAGE;
                // Unlike other optional strings this C API dereferences locale.
                options.display_names_locale = "en"; options.output_confidence_masks = true;
                options.output_category_mask = false;
                char* error = nullptr; const auto status = m_api->MpImageSegmenterCreate(&options, &m_task, &error); m_api->check(status, error);
            }
            if (canceled(cancel)) { out.canceled = true; return out; }
            MpImagePtr input = nullptr; char* error = nullptr;
            auto status = m_api->MpImageCreateFromUint8Data(kMpImageFormatSrgb, image.width, image.height, image.pixels.data(), int(image.pixels.size()), &input, &error);
            m_api->check(status, error); ScopeExit close_image {[&] { m_api->MpImageFree(input); }};
            MpImageSegmenterResult result {}; ScopeExit close_result {[&] { m_api->MpImageSegmenterCloseResult(&result); }};
            error = nullptr; status = m_api->MpImageSegmenterSegmentImage(m_task, input, nullptr, &result, &error); m_api->check(status, error);
            if (result.confidence_masks_count != 6 || !result.confidence_masks) throw std::runtime_error("Unexpected body model label layout.");
            constexpr Label labels[] = {Label::Background, Label::Hair, Label::BodySkin, Label::FaceSkin, Label::Clothes, Label::Accessories};
            for (size_t channel = 0; channel < 6; ++channel) {
                auto mask = result.confidence_masks[channel];
                if (!mask || m_api->MpImageGetWidth(mask) != image.width || m_api->MpImageGetHeight(mask) != image.height)
                    throw std::runtime_error("Unexpected body mask dimensions.");
                const float* data = nullptr; error = nullptr; status = m_api->MpImageDataFloat32(mask, &data, &error); m_api->check(status, error);
                if (!data) throw std::runtime_error("Empty body confidence mask.");
                for (size_t i = 0; i < out.labels.size(); ++i) {
                    if ((i & 16383) == 0 && canceled(cancel)) { out.canceled = true; return out; }
                    const float confidence = data[i];
                    if (!std::isfinite(confidence) || confidence < 0.f || confidence > 1.0001f)
                        throw std::runtime_error("Invalid body confidence value.");
                    if (confidence > out.confidence[i]) { out.confidence[i] = std::min(confidence, 1.f); out.labels[i] = labels[channel]; }
                }
            }
            size_t skin = 0;
            for (size_t i = 0; i < out.labels.size(); ++i)
                if (out.confidence[i] >= minimum_confidence && (out.labels[i] == Label::FaceSkin || out.labels[i] == Label::BodySkin)) ++skin;
            out.person_detected = skin >= std::max(size_t(16), out.labels.size() / 200);
        } catch (const std::exception& error) { out = empty_prediction(image); out.error = error.what(); }
        return out;
    }
};

class FaceRecognizer final : public IFaceRegionRecognizer {
    std::filesystem::path m_root;
    std::shared_ptr<NativeApi> m_api;
    std::vector<char> m_model;
    MpFaceLandmarkerPtr m_task = nullptr;
    std::mutex m_mutex;
public:
    explicit FaceRecognizer(std::filesystem::path root) : m_root(std::move(root)) {}
    ~FaceRecognizer() {
        if (m_task) { char* error = nullptr; m_api->MpFaceLandmarkerClose(m_task, &error); m_api->discard_error(error); }
    }
    std::string identity() const override { return std::string(provider_id) + "/1.0.0/" + dll_hash + "/" + face_hash + "/face-details-v4/iris-ellipse-v1/eyebrow-contour-v1/ear-skin-v2/multiface4/edge-abstain-v2/rgb8-image-v1"; }
    Prediction predict(const RGBImage& image, const Cancel& cancel) override {
        std::lock_guard<std::mutex> lock(m_mutex); auto out = empty_prediction(image);
        if (!out.error.empty()) return out;
        if (canceled(cancel)) { out.canceled = true; return out; }
        try {
            if (!m_task) {
                m_api = native_api(m_root); m_model = verified_file(m_root / "face_landmarker.task", face_hash);
                MpFaceLandmarkerOptions options {}; options.base_options = base_options(m_model);
                options.running_mode = MP_RUNNING_MODE_IMAGE; options.num_faces = int(MediaPipeFaceMasks::maximum_faces);
                options.min_face_detection_confidence = .7f; options.min_face_presence_confidence = .7f;
                options.output_face_blendshapes = false; options.output_facial_transformation_matrixes = false;
                char* error = nullptr; const auto status = m_api->MpFaceLandmarkerCreate(&options, &m_task, &error); m_api->check(status, error);
            }
            if (canceled(cancel)) { out.canceled = true; return out; }
            MpImagePtr input = nullptr; char* error = nullptr;
            auto status = m_api->MpImageCreateFromUint8Data(kMpImageFormatSrgb, image.width, image.height, image.pixels.data(), int(image.pixels.size()), &input, &error);
            m_api->check(status, error); ScopeExit close_image {[&] { m_api->MpImageFree(input); }};
            MpFaceLandmarkerResult result {}; ScopeExit close_result {[&] { m_api->MpFaceLandmarkerCloseResult(&result); }};
            error = nullptr; status = m_api->MpFaceLandmarkerDetectImage(m_task, input, nullptr, &result, &error); m_api->check(status, error);
            if (canceled(cancel)) { out.canceled = true; return out; }
            if (result.face_landmarks_count == 0) return out;
            if (result.face_landmarks_count > MediaPipeFaceMasks::maximum_faces || !result.face_landmarks)
                throw std::runtime_error("Unexpected face model result count.");
            std::vector<MediaPipeFaceMasks::FaceLandmarks> faces;
            faces.reserve(result.face_landmarks_count);
            for (size_t face = 0; face < result.face_landmarks_count; ++face) {
                const auto& detected = result.face_landmarks[face];
                if (detected.landmarks_count != 478 || !detected.landmarks)
                    throw std::runtime_error("Unexpected face model landmark layout.");
                auto& points = faces.emplace_back(); points.reserve(478);
                for (size_t i = 0; i < 478; ++i) points.push_back({detected.landmarks[i].x, detected.landmarks[i].y});
            }
            out = MediaPipeFaceMasks::from_landmarks(image, faces, cancel);
        } catch (const std::exception& error) { out = empty_prediction(image); out.error = error.what(); }
        return out;
    }
};
#endif

struct Registry {
    std::mutex mutex;
    std::map<std::string, BodyRecognizerFactory> bodies;
    std::map<std::string, FaceRecognizerFactory> faces;
    Registry() {
#if defined(_WIN32) && defined(ORCA_ENABLE_MEDIAPIPE_NATIVE)
        bodies[provider_id] = [](const std::filesystem::path& root) { return std::make_unique<BodyRecognizer>(root); };
        faces[provider_id] = [](const std::filesystem::path& root) { return std::make_unique<FaceRecognizer>(root); };
#else
        bodies[provider_id] = [](const std::filesystem::path&) { return std::make_unique<Unavailable<IBodyRegionRecognizer>>(provider_id, "Native semantic recognition is not enabled in this build."); };
        faces[provider_id] = [](const std::filesystem::path&) { return std::make_unique<Unavailable<IFaceRegionRecognizer>>(provider_id, "Native semantic recognition is not enabled in this build."); };
#endif
    }
};
Registry& registry() { static Registry value; return value; }
} // namespace

Prediction MediaPipeFaceMasks::from_landmarks(const RGBImage& image,
                                             const std::vector<FaceLandmarks>& faces,
                                             const Cancel& cancel)
{
    auto out = empty_prediction(image);
    if (!out.error.empty()) return out;
    if (canceled(cancel)) { out.canceled = true; return out; }
    if (faces.size() > maximum_faces) { out.error = "Too many face landmark results."; return out; }
    for (const auto& face : faces) if (face.size() != 478) {
        out.error = "Unexpected face model landmark layout."; return out;
    }
    std::vector<uint8_t> conflicts(out.labels.size(), 0);
    const auto paint = [&](int x, int y, Label label, float confidence) {
        const size_t pixel = size_t(y) * image.width + x;
        if (conflicts[pixel]) return;
        if (out.labels[pixel] != Label::Unknown && out.labels[pixel] != label) {
            out.labels[pixel] = Label::Unknown; out.confidence[pixel] = 0.f; conflicts[pixel] = 1;
        } else { out.labels[pixel] = label; out.confidence[pixel] = confidence; }
    };
    const auto bounds = [&](const std::vector<Point>& polygon) {
        float left = float(image.width), top = float(image.height), right = 0.f, bottom = 0.f;
        for (const auto& p : polygon) { left = std::min(left, p.x); right = std::max(right, p.x); top = std::min(top, p.y); bottom = std::max(bottom, p.y); }
        return std::array<int, 4>{std::max(0, int(std::floor(left))), std::max(0, int(std::floor(top))),
            std::min(image.width - 1, int(std::ceil(right))), std::min(image.height - 1, int(std::ceil(bottom)))};
    };
    // The ordered contours are private adapter topology from the pinned public
    // FACE_LANDMARKS_* connections (MediaPipe, Apache-2.0):
    // mediapipe/tasks/python/vision/face_landmarker.py at 6d31f1ebc3284db7.
    constexpr int outer_lips[] = {61,146,91,181,84,17,314,405,321,375,291,409,270,269,267,0,37,39,40,185};
    constexpr int inner_lips[] = {78,95,88,178,87,14,317,402,318,324,308,415,310,311,312,13,82,81,80,191};
    constexpr int right_eye[] = {33,7,163,144,145,153,154,155,133,173,157,158,159,160,161,246};
    constexpr int left_eye[] = {263,249,390,373,374,380,381,382,362,398,384,385,386,387,388,466};
    constexpr int right_iris[] = {469,470,471,472};
    constexpr int left_iris[] = {474,475,476,477};
    constexpr int right_eyebrow[] = {46,53,52,65,55,107,66,105,63,70};
    constexpr int left_eyebrow[] = {276,283,282,295,285,336,296,334,293,300};
    constexpr int skin_samples[] = {50,101,205,280,330,425};
    constexpr int right_ear[] = {127,234,132};
    constexpr int left_ear[] = {356,454,361};
    for (const auto& face : faces) {
        if (canceled(cancel)) { out.canceled = true; return out; }
        const auto contour = [&](const auto& ids, std::vector<Point>& points) {
            points.clear();
            for (int id : ids) {
                const auto& p = face[size_t(id)];
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || p.x < 0.f || p.x > 1.f || p.y < 0.f || p.y > 1.f) return false;
                points.push_back({p.x * image.width, p.y * image.height});
            }
            return true;
        };
        std::vector<Point> outer, inner;
        if (contour(outer_lips, outer) && contour(inner_lips, inner)) {
            out.face_detected = true; out.person_detected = true;
            const auto box = bounds(outer);
            for (int y = box[1]; y <= box[3]; ++y) {
                if (canceled(cancel)) { out.canceled = true; return out; }
                for (int x = box[0]; x <= box[2]; ++x) {
                    const float px = x + .5f, py = y + .5f;
                    if (!inside_polygon(px, py, outer) || edge_distance(px, py, outer) < .75f || edge_distance(px, py, inner) < .75f) continue;
                    paint(x, y, inside_polygon(px, py, inner) ? Label::MouthInterior : Label::Lips, .9f);
                }
            }
        }
        const auto eye_mask = [&](const auto& aperture_ids, const auto& iris_ids) {
            std::vector<Point> aperture, rim;
            if (!contour(aperture_ids, aperture) || !contour(iris_ids, rim)) return;
            float twice_area = 0.f, span_squared = 0.f;
            for (size_t i = 0; i < aperture.size(); ++i) {
                const auto& a = aperture[i]; const auto& b = aperture[(i + 1) % aperture.size()];
                twice_area += a.x * b.y - a.y * b.x;
                for (const auto& p : aperture) span_squared = std::max(span_squared, (a.x - p.x) * (a.x - p.x) + (a.y - p.y) * (a.y - p.y));
            }
            const float span = std::sqrt(span_squared);
            // Closed or unresolved eyes do not provide a reliable white region.
            if (span < 7.f || std::abs(twice_area) * .5f < span * 1.4f) return;
            const Point horizontal_mid {(rim[0].x + rim[2].x) * .5f, (rim[0].y + rim[2].y) * .5f};
            const Point vertical_mid {(rim[1].x + rim[3].x) * .5f, (rim[1].y + rim[3].y) * .5f};
            const Point center {(horizontal_mid.x + vertical_mid.x) * .5f, (horizontal_mid.y + vertical_mid.y) * .5f};
            const Point u {(rim[0].x - rim[2].x) * .5f, (rim[0].y - rim[2].y) * .5f};
            const Point v {(rim[1].x - rim[3].x) * .5f, (rim[1].y - rim[3].y) * .5f};
            const float ru = std::hypot(u.x, u.y), rv = std::hypot(v.x, v.y), determinant = u.x * v.y - u.y * v.x;
            // Landmark rim axes may be oblique. Their singular values give the
            // actual ellipse radii and a conservative distance to its boundary.
            const float maximum_radius = std::sqrt(.5f * (ru * ru + rv * rv +
                std::hypot(ru * ru - rv * rv, 2.f * (u.x * v.x + u.y * v.y))));
            const float minimum_radius = maximum_radius > 0.f ? std::abs(determinant) / maximum_radius : 0.f;
            if (minimum_radius < 1.25f || maximum_radius > span * .5f ||
                std::abs(determinant) < ru * rv * .25f ||
                std::hypot(horizontal_mid.x - vertical_mid.x, horizontal_mid.y - vertical_mid.y) > maximum_radius * .45f ||
                !inside_polygon(center.x, center.y, aperture)) return;
            out.face_detected = true; out.person_detected = true;
            const auto box = bounds(aperture);
            for (int y = box[1]; y <= box[3]; ++y) {
                if (canceled(cancel)) { out.canceled = true; return; }
                for (int x = box[0]; x <= box[2]; ++x) {
                    const float px = x + .5f, py = y + .5f;
                    if (!inside_polygon(px, py, aperture) || edge_distance(px, py, aperture) < .6f) continue;
                    const float dx = px - center.x, dy = py - center.y;
                    const float a = (dx * v.y - dy * v.x) / determinant;
                    const float b = (u.x * dy - u.y * dx) / determinant;
                    const float radius = std::hypot(a, b);
                    // Ellipse rather than a four-point diamond preserves the
                    // curved iris. A narrow uncertain ring protects its contour.
                    if (std::abs(radius - 1.f) * minimum_radius < .35f) continue;
                    paint(x, y, radius < 1.f ? Label::Iris : Label::EyeSclera, .94f);
                }
            }
        };
        eye_mask(right_eye, right_iris);
        if (out.canceled) return out;
        eye_mask(left_eye, left_iris);
        if (out.canceled) return out;
        const auto eyebrow_mask = [&](const auto& eyebrow_ids) {
            std::vector<Point> eyebrow;
            if (!contour(eyebrow_ids, eyebrow)) return;
            float twice_area = 0.f, span_squared = 0.f;
            for (size_t i = 0; i < eyebrow.size(); ++i) {
                const auto& a = eyebrow[i]; const auto& b = eyebrow[(i + 1) % eyebrow.size()];
                twice_area += a.x * b.y - a.y * b.x;
                for (const auto& p : eyebrow)
                    span_squared = std::max(span_squared, (a.x - p.x) * (a.x - p.x) + (a.y - p.y) * (a.y - p.y));
            }
            const float span = std::sqrt(span_squared);
            // Small or nearly collapsed contours cannot be separated from
            // forehead skin at the rendered resolution.
            if (span < 5.f || std::abs(twice_area) * .5f < span * .65f) return;
            out.face_detected = true; out.person_detected = true;
            const auto box = bounds(eyebrow);
            for (int y = box[1]; y <= box[3]; ++y) {
                if (canceled(cancel)) { out.canceled = true; return; }
                for (int x = box[0]; x <= box[2]; ++x) {
                    const float px = x + .5f, py = y + .5f;
                    if (inside_polygon(px, py, eyebrow)) paint(x, y, Label::Eyebrow, .92f);
                }
            }
        };
        eyebrow_mask(right_eyebrow);
        if (out.canceled) return out;
        eyebrow_mask(left_eyebrow);
        if (out.canceled) return out;

        // The multiclass body model has no ear class and may merge an exposed
        // ear into adjacent hair. Keep this evidence inside the face adapter:
        // a small landmark-relative ear window accepts only pixels connected
        // to the facial side contour with matching intrinsic skin chroma.
        std::vector<Color> samples;
        for (int id : skin_samples) {
            const auto& landmark = face[size_t(id)];
            if (!std::isfinite(landmark.x) || !std::isfinite(landmark.y) ||
                landmark.x < 0.f || landmark.x > 1.f || landmark.y < 0.f || landmark.y > 1.f) continue;
            const int cx = std::clamp(int(landmark.x * image.width), 0, image.width - 1);
            const int cy = std::clamp(int(landmark.y * image.height), 0, image.height - 1);
            for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                const int x = std::clamp(cx + dx, 0, image.width - 1);
                const int y = std::clamp(cy + dy, 0, image.height - 1);
                samples.push_back(image_lab(image, size_t(y) * image.width + x));
            }
        }
        if (samples.size() < 18) continue;
        Color skin {};
        for (int channel = 0; channel < 3; ++channel) {
            std::sort(samples.begin(), samples.end(), [channel](const Color& a, const Color& b) {
                return a[channel] < b[channel];
            });
            skin[channel] = samples[samples.size() / 2][channel];
        }
        const float skin_chroma = std::hypot(skin[1], skin[2]);
        if (skin_chroma < .015f || skin[1] <= 0.f || skin[2] <= 0.f) continue;
        const auto ear_mask = [&](const int (&ids)[3], int opposite_mid) {
            std::array<Point, 3> points;
            for (size_t i = 0; i < points.size(); ++i) {
                const auto& landmark = face[size_t(ids[i])];
                if (!std::isfinite(landmark.x) || !std::isfinite(landmark.y) ||
                    landmark.x < 0.f || landmark.x > 1.f || landmark.y < 0.f || landmark.y > 1.f) return;
                points[i] = {landmark.x * image.width, landmark.y * image.height};
            }
            const auto& opposite = face[size_t(opposite_mid)];
            if (!std::isfinite(opposite.x) || !std::isfinite(opposite.y) ||
                opposite.x < 0.f || opposite.x > 1.f || opposite.y < 0.f || opposite.y > 1.f) return;
            const Point other {opposite.x * image.width, opposite.y * image.height};
            float hx = points[1].x - other.x, hy = points[1].y - other.y;
            const float face_width = std::hypot(hx, hy);
            if (face_width < 20.f) return;
            hx /= face_width; hy /= face_width;
            float vx = points[2].x - points[0].x, vy = points[2].y - points[0].y;
            const float projection = vx * hx + vy * hy;
            vx -= projection * hx; vy -= projection * hy;
            const float vertical_span = std::hypot(vx, vy);
            if (vertical_span < 8.f) return;
            vx /= vertical_span; vy /= vertical_span;
            // In profile the projected distance between opposite face sides
            // collapses before the visible ear does. Size the outward reach
            // primarily from the local vertical contour, while keeping a
            // smaller inward shoulder on the cheek side.
            const float rx = std::max(face_width * .045f, vertical_span * .42f);
            const float ry = vertical_span * .62f;
            const Point center {points[1].x + hx * rx * .30f, points[1].y + hy * rx * .30f};
            const Point anchor {points[1].x - hx * rx * .60f, points[1].y - hy * rx * .60f};
            const int left = std::max(0, int(std::floor(center.x - rx - ry * std::abs(vx))));
            const int right = std::min(image.width - 1, int(std::ceil(center.x + rx + ry * std::abs(vx))));
            const int top = std::max(0, int(std::floor(center.y - rx - ry * std::abs(vy))));
            const int bottom = std::min(image.height - 1, int(std::ceil(center.y + rx + ry * std::abs(vy))));
            std::vector<uint8_t> candidate(out.labels.size(), 0), reached(out.labels.size(), 0);
            std::vector<size_t> pending;
            for (int y = top; y <= bottom; ++y) for (int x = left; x <= right; ++x) {
                const float dx = x + .5f - center.x, dy = y + .5f - center.y;
                const float u = dx * hx + dy * hy, v = dx * vx + dy * vy;
                if ((u * u) / (rx * rx) + (v * v) / (ry * ry) > 1.f) continue;
                const size_t pixel = size_t(y) * image.width + x;
                const Color color = image_lab(image, pixel);
                const float color_chroma = std::hypot(color[1], color[2]);
                const float alignment = color_chroma > 0.f ?
                    (color[1] * skin[1] + color[2] * skin[2]) / (color_chroma * skin_chroma) : -1.f;
                if (color[0] < skin[0] - .55f || color[0] > skin[0] + .20f ||
                    color_chroma < skin_chroma * .35f || color_chroma > skin_chroma * 2.2f + .015f ||
                    color[1] <= .002f || color[2] <= .002f || alignment < .85f) continue;
                candidate[pixel] = 1;
                const float seed_da = color[1] - skin[1], seed_db = color[2] - skin[2];
                if (std::hypot(x + .5f - anchor.x, y + .5f - anchor.y) <= 2.5f &&
                    std::abs(color[0] - skin[0]) <= .25f && seed_da * seed_da + seed_db * seed_db <= .0009f) {
                    reached[pixel] = 1; pending.push_back(pixel);
                }
            }
            for (size_t cursor = 0; cursor < pending.size(); ++cursor) {
                const size_t pixel = pending[cursor];
                const int x = int(pixel % image.width), y = int(pixel / image.width);
                for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if ((dx == 0 && dy == 0) || nx < left || nx > right || ny < top || ny > bottom) continue;
                    const size_t nearby = size_t(ny) * image.width + nx;
                    if (!candidate[nearby] || reached[nearby]) continue;
                    const Color from = image_lab(image, pixel), to = image_lab(image, nearby);
                    const float dl = (from[0] - to[0]) * .15f;
                    const float da = from[1] - to[1], db = from[2] - to[2];
                    if (dl * dl + da * da + db * db > .0016f) continue;
                    reached[nearby] = 1; pending.push_back(nearby);
                }
            }
            for (size_t pixel : pending) if (out.labels[pixel] == Label::Unknown)
                paint(int(pixel % image.width), int(pixel / image.width), Label::FaceSkin, .88f);
        };
        ear_mask(right_ear, left_ear[1]);
        ear_mask(left_ear, right_ear[1]);
    }
    return out;
}

bool register_body_recognizer_factory(const std::string& id, BodyRecognizerFactory factory)
{
    if (id.empty() || !factory) return false;
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
    return r.bodies.emplace(id, std::move(factory)).second;
}
bool register_face_recognizer_factory(const std::string& id, FaceRecognizerFactory factory)
{
    if (id.empty() || !factory) return false;
    auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
    return r.faces.emplace(id, std::move(factory)).second;
}
RegionRecognizers create_region_recognizers(const std::string& body_provider, const std::string& face_provider,
                                           const std::filesystem::path& root)
{
    BodyRecognizerFactory body; FaceRecognizerFactory face;
    { auto& r = registry(); std::lock_guard<std::mutex> lock(r.mutex);
      auto b = r.bodies.find(body_provider); if (b != r.bodies.end()) body = b->second;
      auto f = r.faces.find(face_provider); if (f != r.faces.end()) face = f->second; }
    RegionRecognizers result;
    // Failure or replacement of one port must not disable the other port.
    try {
        if (!body) throw std::runtime_error("Unknown body recognition provider.");
        result.body = body(root);
        if (!result.body) throw std::runtime_error("Body recognition factory returned no provider.");
    } catch (const std::exception& error) {
        result.error = error.what();
        result.body = std::make_unique<Unavailable<IBodyRegionRecognizer>>(body_provider, error.what());
    }
    try {
        if (!face) throw std::runtime_error("Unknown face recognition provider.");
        result.face = face(root);
        if (!result.face) throw std::runtime_error("Face recognition factory returned no provider.");
    } catch (const std::exception& error) {
        if (!result.error.empty()) result.error += " ";
        result.error += error.what();
        result.face = std::make_unique<Unavailable<IFaceRegionRecognizer>>(face_provider, error.what());
    }
    return result;
}
RegionRecognizers create_region_recognizers(const std::string& provider, const std::filesystem::path& root)
{ return create_region_recognizers(provider, provider, root); }
RegionRecognizers create_mediapipe_recognizers(const std::filesystem::path& root)
{ return create_region_recognizers(provider_id, root); }

} // namespace Slic3r::AI::SemanticColoring
