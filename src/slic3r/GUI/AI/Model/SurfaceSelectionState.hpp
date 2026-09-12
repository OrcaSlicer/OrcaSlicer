#pragma once

#include "libslic3r/TriangleMesh.hpp"
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::AI::SurfaceSelectionPersistence {

struct SelectionState {
    std::vector<uint8_t> selected, protected_faces, foreground, domain;
};

using FaceColorOverrides = std::vector<std::pair<size_t, std::array<float, 3>>>;

namespace detail {
inline bool valid_fingerprint(const std::string& value)
{
    if (value.size() != 64) return false;
    for (char c : value) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
inline bool read_size(const nlohmann::json& value, size_t& output)
{
    uint64_t number;
    if (value.is_number_unsigned()) number = value.get<uint64_t>();
    else if (value.is_number_integer() && value.get<int64_t>() >= 0) number = uint64_t(value.get<int64_t>());
    else return false;
    if (number > std::numeric_limits<size_t>::max()) return false;
    output = size_t(number);
    return true;
}
inline uint8_t flags(const SelectionState& state, size_t face)
{
    return uint8_t((state.selected.empty() ? 0 : state.selected[face]) |
        ((state.protected_faces.empty() ? 0 : state.protected_faces[face]) << 1) |
        ((state.foreground.empty() ? 0 : state.foreground[face]) << 2) |
        ((state.domain.empty() ? 0 : state.domain[face]) << 3));
}
inline void assign(SelectionState& state, size_t face, uint8_t value)
{
    state.selected[face] = value & 1;
    state.protected_faces[face] = (value >> 1) & 1;
    state.foreground[face] = (value >> 2) & 1;
    state.domain[face] = (value >> 3) & 1;
}
} // namespace detail

// Hash the canonical face/corner position sequence, not vertex identifiers or
// vertex count. Splitting a vertex into coincident copies for sharp material
// boundaries preserves identity; reordered faces or changed positions do not.
// The encoding is little-endian IEEE float32 and normalizes negative zero.
// No rendered color, camera, or semantic labels participate in this identity.
inline std::string geometry_fingerprint(const indexed_triangle_set& mesh)
{
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
                  "Surface selection identity requires IEEE float32.");
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1) return {};
    constexpr char schema[] = "orca.surface-selection.geometry/v1";
    if (EVP_DigestUpdate(digest.get(), schema, sizeof(schema)) != 1) return {};
    std::array<unsigned char, 8> count_bytes {};
    for (size_t i = 0; i < count_bytes.size(); ++i) count_bytes[i] = static_cast<unsigned char>(uint64_t(mesh.indices.size()) >> (i * 8));
    if (EVP_DigestUpdate(digest.get(), count_bytes.data(), count_bytes.size()) != 1) return {};
    // Hash 1024 complete faces per EVP call, keeping memory independent of mesh
    // size and avoiding millions of EVP calls for a production-sized model.
    std::array<unsigned char, 1024 * 3 * 3 * 4> buffer {};
    size_t used = 0;
    for (const auto& face : mesh.indices) {
        for (int corner = 0; corner < 3; ++corner) {
            const int vertex = face[corner];
            if (vertex < 0 || size_t(vertex) >= mesh.vertices.size() || !mesh.vertices[vertex].allFinite()) return {};
            for (int axis = 0; axis < 3; ++axis) {
                const float value = mesh.vertices[vertex][axis] == 0.0f ? 0.0f : mesh.vertices[vertex][axis];
                uint32_t bits;
                std::memcpy(&bits, &value, sizeof(bits));
                for (size_t byte = 0; byte < 4; ++byte) buffer[used++] = static_cast<unsigned char>(bits >> (byte * 8));
            }
        }
        if (used == buffer.size()) {
            if (EVP_DigestUpdate(digest.get(), buffer.data(), used) != 1) return {};
            used = 0;
        }
    }
    if (used && EVP_DigestUpdate(digest.get(), buffer.data(), used) != 1) return {};
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes {};
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(digest.get(), bytes.data(), &length) != 1 || length != 32) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(length * 2);
    for (unsigned int i = 0; i < length; ++i) { result += hex[bytes[i] >> 4]; result += hex[bytes[i] & 15]; }
    return result;
}

// Empty optional masks mean all zero. Nonempty masks must have exactly the
// actual face count and binary values. All four labels are preserved as given;
// their product meaning is owned by the editor, not inferred by serialization.
inline nlohmann::json encode(const SelectionState& state, size_t actual_face_count, const std::string& fingerprint)
{
    if (!detail::valid_fingerprint(fingerprint)) throw std::invalid_argument("Invalid selection geometry fingerprint.");
    for (const auto* mask : {&state.selected, &state.protected_faces, &state.foreground, &state.domain}) {
        if (!mask->empty() && mask->size() != actual_face_count) throw std::invalid_argument("Selection mask size does not match geometry.");
        for (uint8_t value : *mask) if (value > 1) throw std::invalid_argument("Selection mask values must be binary.");
    }
    nlohmann::json doc = {{"schema", "orca.surface-selection/v1"}, {"geometry_sha256", fingerprint}, {"face_count", actual_face_count}};
    size_t runs = 0;
    uint8_t previous = 0;
    for (size_t i = 0; i < actual_face_count; ++i) {
        const uint8_t value = detail::flags(state, i);
        if (value && value != previous) ++runs;
        previous = value;
    }
    // Dense alternating selections should not expand into millions of JSON
    // arrays. One hex digit holds all four flags; sparse regions use intervals.
    if (runs > actual_face_count / 16) {
        constexpr char hex[] = "0123456789abcdef";
        std::string data(actual_face_count, '0');
        for (size_t i = 0; i < actual_face_count; ++i) data[i] = hex[detail::flags(state, i)];
        doc["encoding"] = "hex4";
        doc["data"] = std::move(data);
    } else {
        doc["encoding"] = "rle4";
        auto intervals = nlohmann::json::array();
        size_t start = 0;
        while (start < actual_face_count) {
            const uint8_t value = detail::flags(state, start);
            size_t end = start + 1;
            while (end < actual_face_count && detail::flags(state, end) == value) ++end;
            if (value) intervals.push_back({start, end - start, value});
            start = end;
        }
        doc["runs"] = std::move(intervals);
    }
    return doc;
}

// Validate the complete payload before allocating masks or changing output.
// The only allocation size comes from the currently loaded, trusted mesh.
inline bool decode(const nlohmann::json& doc, size_t actual_face_count, const std::string& fingerprint,
                   SelectionState& output, std::string& error)
{
    error.clear();
    auto fail = [&](const char* message) { error = message; return false; };
    if (!doc.is_object() || !doc.contains("schema") || doc["schema"] != "orca.surface-selection/v1")
        return fail("Unsupported surface selection schema.");
    if (!detail::valid_fingerprint(fingerprint) || !doc.contains("geometry_sha256") ||
        !doc["geometry_sha256"].is_string() || doc["geometry_sha256"].get_ref<const std::string&>() != fingerprint)
        return fail("The saved selection belongs to different geometry.");
    size_t count;
    if (!doc.contains("face_count") || !detail::read_size(doc["face_count"], count) || count != actual_face_count)
        return fail("The saved selection face count does not match the loaded mesh.");
    if (!doc.contains("encoding") || !doc["encoding"].is_string()) return fail("Missing selection encoding.");
    const auto& encoding = doc["encoding"].get_ref<const std::string&>();
    if (encoding == "rle4") {
        if (!doc.contains("runs") || !doc["runs"].is_array() || doc["runs"].size() > actual_face_count)
            return fail("Invalid selection intervals.");
        size_t previous_end = 0;
        for (const auto& run : doc["runs"]) {
            size_t start, length, flags;
            if (!run.is_array() || run.size() != 3 || !detail::read_size(run[0], start) ||
                !detail::read_size(run[1], length) || !detail::read_size(run[2], flags) ||
                !length || !flags || flags > 15 || start < previous_end || start >= actual_face_count ||
                length > actual_face_count - start)
                return fail("Invalid or overlapping selection intervals.");
            previous_end = start + length;
        }
    } else if (encoding == "hex4") {
        if (!doc.contains("data") || !doc["data"].is_string()) return fail("Invalid packed selection.");
        const auto& data = doc["data"].get_ref<const std::string&>();
        if (data.size() != actual_face_count) return fail("Packed selection size does not match geometry.");
        for (char c : data) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return fail("Invalid packed selection flags.");
    } else return fail("Unsupported selection encoding.");

    SelectionState restored;
    for (auto* mask : {&restored.selected, &restored.protected_faces, &restored.foreground, &restored.domain}) mask->assign(actual_face_count, 0);
    if (encoding == "rle4") {
        for (const auto& run : doc["runs"]) {
            const size_t start = run[0].get<size_t>(), end = start + run[1].get<size_t>();
            const uint8_t flags = run[2].get<uint8_t>();
            for (size_t face = start; face < end; ++face) detail::assign(restored, face, flags);
        }
    } else {
        const auto& data = doc["data"].get_ref<const std::string&>();
        for (size_t face = 0; face < data.size(); ++face) {
            const char c = data[face];
            detail::assign(restored, face, uint8_t(c <= '9' ? c - '0' : c - 'a' + 10));
        }
    }
    output = std::move(restored);
    return true;
}

// Persist explicit local color intent independently of preview quantization.
// Each face appears once; these normalized source RGB values do not imply a
// particular filament slot, palette role, or any generated intermediate color.
inline nlohmann::json encode_colors(const FaceColorOverrides& overrides, size_t actual_face_count,
                                   const std::string& fingerprint)
{
    if (!detail::valid_fingerprint(fingerprint)) throw std::invalid_argument("Invalid color override geometry fingerprint.");
    if (overrides.size() > actual_face_count) throw std::invalid_argument("Too many face color overrides.");
    const auto less = [](const auto& a, const auto& b) { return a.first < b.first; };
    FaceColorOverrides sorted;
    const FaceColorOverrides* ordered = &overrides;
    if (!std::is_sorted(overrides.begin(), overrides.end(), less)) {
        sorted = overrides;
        std::sort(sorted.begin(), sorted.end(), less);
        ordered = &sorted;
    }
    size_t runs = 0;
    for (size_t i = 0; i < ordered->size(); ++i) {
        const auto& item = (*ordered)[i];
        if (item.first >= actual_face_count || (i && (*ordered)[i - 1].first == item.first))
            throw std::invalid_argument("Face color override identifiers must be unique and inside the mesh.");
        for (float value : item.second) if (!std::isfinite(value) || value < 0.0f || value > 1.0f)
            throw std::invalid_argument("Face color overrides require finite normalized RGB values.");
        if (!i || (*ordered)[i - 1].first + 1 != item.first || (*ordered)[i - 1].second != item.second) ++runs;
    }
    const bool use_runs = double(runs) * 5.0 < double(ordered->size()) * 4.0;
    nlohmann::json doc = {{"schema", "orca.local-face-colors/v1"}, {"geometry_sha256", fingerprint},
                         {"face_count", actual_face_count}, {"encoding", use_runs ? "runs" : "entries"},
                         {"colors", nlohmann::json::array()}};
    for (size_t i = 0; i < ordered->size();) {
        const auto& item = (*ordered)[i];
        if (use_runs) {
            size_t end = i + 1;
            while (end < ordered->size() && (*ordered)[end - 1].first + 1 == (*ordered)[end].first &&
                   item.second == (*ordered)[end].second) ++end;
            doc["colors"].push_back({item.first, end - i, item.second[0], item.second[1], item.second[2]});
            i = end;
        } else {
            doc["colors"].push_back({item.first, item.second[0], item.second[1], item.second[2]});
            ++i;
        }
    }
    return doc;
}

// Identity/count validation precedes any allocation based on the payload.
// Temporary storage is bounded by the actual loaded mesh's face count and the
// caller's previous overrides remain untouched when any later entry is bad.
inline bool decode_colors(const nlohmann::json& doc, size_t actual_face_count, const std::string& fingerprint,
                          FaceColorOverrides& output, std::string& error)
{
    error.clear();
    auto fail = [&](const char* message) { error = message; return false; };
    if (!doc.is_object() || !doc.contains("schema") || doc["schema"] != "orca.local-face-colors/v1")
        return fail("Unsupported local face color schema.");
    if (!detail::valid_fingerprint(fingerprint) || !doc.contains("geometry_sha256") ||
        !doc["geometry_sha256"].is_string() || doc["geometry_sha256"].get_ref<const std::string&>() != fingerprint)
        return fail("The saved local colors belong to different geometry.");
    size_t count;
    if (!doc.contains("face_count") || !detail::read_size(doc["face_count"], count) || count != actual_face_count)
        return fail("The saved local color face count does not match the loaded mesh.");
    if (!doc.contains("colors") || !doc["colors"].is_array() || doc["colors"].size() > actual_face_count)
        return fail("Invalid local face color entries.");
    if (doc.contains("encoding") && !doc["encoding"].is_string()) return fail("Invalid local color encoding.");
    const std::string encoding = doc.contains("encoding") ? doc["encoding"].get<std::string>() : "entries";
    const bool use_runs = encoding == "runs";
    if (!use_runs && encoding != "entries") return fail("Unsupported local color encoding.");
    size_t restored_count = doc["colors"].size();
    if (use_runs) {
        restored_count = 0;
        size_t end = 0;
        for (const auto& entry : doc["colors"]) {
            size_t face, length;
            if (!entry.is_array() || entry.size() != 5 || !detail::read_size(entry[0], face) ||
                !detail::read_size(entry[1], length) || !length || face < end || face >= actual_face_count ||
                length > actual_face_count - face)
                return fail("Local color runs must not overlap or exceed the mesh.");
            end = face + length;
            restored_count += length; // Nonoverlapping runs keep this <= actual_face_count.
        }
    }
    FaceColorOverrides restored;
    restored.reserve(restored_count);
    size_t previous_end = 0;
    for (const auto& entry : doc["colors"]) {
        size_t face, length = 1;
        if (!entry.is_array() || entry.size() != (use_runs ? 5 : 4) || !detail::read_size(entry[0], face) ||
            face >= actual_face_count)
            return fail("Local color face identifiers must be inside the mesh.");
        if (use_runs && (!detail::read_size(entry[1], length) || !length || face < previous_end || length > actual_face_count - face))
            return fail("Local color runs must not overlap or exceed the mesh.");
        previous_end = face + length;
        std::array<float, 3> rgb;
        for (size_t channel = 0; channel < rgb.size(); ++channel) {
            const auto& encoded = entry[channel + (use_runs ? 2 : 1)];
            if (!encoded.is_number()) return fail("Local colors require numeric RGB values.");
            const double value = encoded.get<double>();
            if (!std::isfinite(value) || value < 0.0 || value > 1.0)
                return fail("Local colors require finite normalized RGB values.");
            rgb[channel] = float(value);
        }
        for (size_t i = 0; i < length; ++i) restored.emplace_back(face + i, rgb);
    }
    if (!use_runs) {
        std::sort(restored.begin(), restored.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t i = 1; i < restored.size(); ++i) if (restored[i - 1].first == restored[i].first)
            return fail("Local color face identifiers must be unique.");
    }
    output = std::move(restored);
    return true;
}
} // namespace Slic3r::AI::SurfaceSelectionPersistence
