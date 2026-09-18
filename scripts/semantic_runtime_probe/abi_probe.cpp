// Native production C ABI probe. No transport hooks or firewall changes.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define MP_EXPORT
#include "mediapipe/tasks/c/vision/image_segmenter/image_segmenter.h"
#include "mediapipe/tasks/c/vision/face_landmarker/face_landmarker.h"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char** argv) try {
    if (argc != 4) {
        std::cerr << "Usage: abi_probe runtime_dir input.ppm expected_faces\n";
        return 2;
    }
    std::ifstream input(argv[2], std::ios::binary);
    std::string magic; int width = 0, height = 0, maximum = 0;
    input >> magic >> width >> height >> maximum;
    if (magic != "P6" || width <= 0 || width > 4096 || height <= 0 || height > 4096 || maximum != 255)
        throw std::runtime_error("Expected comment-free RGB8 P6 PPM, maximum 4096x4096");
    if (input.get() != '\n') throw std::runtime_error("Expected LF before pixels");
    std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
    if (!input.read(reinterpret_cast<char*>(rgb.data()), rgb.size())) throw std::runtime_error("Truncated pixels");
    const int expected_faces = std::stoi(argv[3]);
    if (expected_faces < 0 || expected_faces > 4) throw std::runtime_error("Expected face count must be 0..4");
    const auto start = std::chrono::steady_clock::now();
    const std::filesystem::path root = argv[1];
    HMODULE dll = LoadLibraryExW((root / L"libmediapipe.dll").c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!dll) throw std::runtime_error("DLL load failed: " + std::to_string(GetLastError()));
#define FN(name) auto fn_##name = reinterpret_cast<decltype(&name)>(GetProcAddress(dll, #name)); \
    if (!fn_##name) throw std::runtime_error("Missing export: " #name)
    FN(MpErrorFree); FN(MpImageCreateFromUint8Data); FN(MpImageFree);
    FN(MpImageGetWidth); FN(MpImageGetHeight); FN(MpImageDataFloat32);
    FN(MpImageSegmenterCreate); FN(MpImageSegmenterSegmentImage);
    FN(MpImageSegmenterCloseResult); FN(MpImageSegmenterClose);
    FN(MpFaceLandmarkerCreate); FN(MpFaceLandmarkerDetectImage);
    FN(MpFaceLandmarkerCloseResult); FN(MpFaceLandmarkerClose);
#undef FN
    auto check = [&](MpStatus status, char*& error) {
        const std::string message = error ? error : "";
        if (error) fn_MpErrorFree(error);
        error = nullptr;
        if (status != 0) throw std::runtime_error("C ABI status " + std::to_string(status) + ": " + message);
    };
    char* error = nullptr; MpImagePtr image = nullptr;
    auto status = fn_MpImageCreateFromUint8Data(kMpImageFormatSrgb, width, height,
        rgb.data(), static_cast<int>(rgb.size()), &image, &error);
    check(status, error);
    const auto body = (root / "selfie_multiclass_256x256.tflite").string();
    MpImageSegmenterOptions so{};
    so.base_options.model_asset_path = body.c_str(); so.base_options.file_descriptor = -1;
    so.base_options.delegate = MP_DELEGATE_CPU; so.base_options.host_system = MP_HOST_SYSTEM_WINDOWS;
    so.running_mode = MP_RUNNING_MODE_IMAGE; so.display_names_locale = "en";
    so.output_confidence_masks = true; so.output_category_mask = true;
    MpImageSegmenterPtr segmenter = nullptr;
    status = fn_MpImageSegmenterCreate(&so, &segmenter, &error); check(status, error);
    MpImageSegmenterResult sr{};
    status = fn_MpImageSegmenterSegmentImage(segmenter, image, nullptr, &sr, &error); check(status, error);
    if (sr.confidence_masks_count != 6) throw std::runtime_error("Expected six body confidence masks");
    std::vector<double> sums(static_cast<size_t>(width) * height, 0.0);
    for (uint32_t mask = 0; mask < sr.confidence_masks_count; ++mask) {
        if (fn_MpImageGetWidth(sr.confidence_masks[mask]) != width || fn_MpImageGetHeight(sr.confidence_masks[mask]) != height)
            throw std::runtime_error("Mask dimensions do not match original RGB input");
        const float* confidence = nullptr;
        status = fn_MpImageDataFloat32(sr.confidence_masks[mask], &confidence, &error); check(status, error);
        if (!confidence) throw std::runtime_error("Empty confidence data");
        for (size_t pixel = 0; pixel < sums.size(); ++pixel) {
            if (!std::isfinite(confidence[pixel]) || confidence[pixel] < -0.001f || confidence[pixel] > 1.001f)
                throw std::runtime_error("Confidence is nonfinite or outside 0..1");
            sums[pixel] += confidence[pixel];
        }
    }
    double sum_error = 0;
    for (const auto value : sums) sum_error = (std::max)(sum_error, std::abs(value - 1.0));
    if (sum_error > 0.01) throw std::runtime_error("Multiclass confidence sum differs from 1 by >0.01");
    fn_MpImageSegmenterCloseResult(&sr);
    status = fn_MpImageSegmenterClose(segmenter, &error); check(status, error);
    const auto face = (root / "face_landmarker.task").string();
    MpFaceLandmarkerOptions fo{};
    fo.base_options.model_asset_path = face.c_str(); fo.base_options.file_descriptor = -1;
    fo.base_options.delegate = MP_DELEGATE_CPU; fo.base_options.host_system = MP_HOST_SYSTEM_WINDOWS;
    fo.running_mode = MP_RUNNING_MODE_IMAGE; fo.num_faces = 4;
    MpFaceLandmarkerPtr landmarker = nullptr;
    status = fn_MpFaceLandmarkerCreate(&fo, &landmarker, &error); check(status, error);
    MpFaceLandmarkerResult fr{};
    status = fn_MpFaceLandmarkerDetectImage(landmarker, image, nullptr, &fr, &error); check(status, error);
    if (static_cast<int>(fr.face_landmarks_count) != expected_faces)
        throw std::runtime_error("Face count: " + std::to_string(fr.face_landmarks_count) + ", expected " + std::to_string(expected_faces));
    for (uint32_t face_index = 0; face_index < fr.face_landmarks_count; ++face_index) {
        const auto& landmarks = fr.face_landmarks[face_index];
        if (landmarks.landmarks_count != 478) throw std::runtime_error("Expected 478 points for every detected face");
        for (uint32_t point = 0; point < landmarks.landmarks_count; ++point) {
            const auto& p = landmarks.landmarks[point];
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
                throw std::runtime_error("Nonfinite landmark");
        }
    }
    fn_MpFaceLandmarkerCloseResult(&fr);
    status = fn_MpFaceLandmarkerClose(landmarker, &error); check(status, error);
    fn_MpImageFree(image); FreeLibrary(dll);
    std::cout << "{\"passed\":true,\"rgb_width\":" << width << ",\"rgb_height\":" << height
        << ",\"body_masks\":6,\"maximum_confidence_sum_error\":" << sum_error
        << ",\"faces\":" << expected_faces << ",\"landmarks_per_face\":478,\"maximum_faces_setting\":4,\"elapsed_ms\":"
        << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() << "}\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "Probe failed: " << error.what() << '\n';
    return 1;
}
