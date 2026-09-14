#pragma once

#include "../ModelGeneration/ModelPreviewPalette.hpp"
#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::AI::ColorTrialPersistence {

// Pure saved snapshot of the controls. Source/target pairs retain their order:
// target colors alone cannot reconstruct which original colors they replace.
// A saved project/pack choice should be converted to source=2 (manual snapshot)
// by the host; dynamic pack indices and project signatures are not persisted.
struct State {
    std::vector<GUI::PreviewPalette::Color> colors, mapping_colors;
    std::array<bool, 6> locks {};
    int source {0}, count {6};
    bool enabled {false}, fidelity {true}, lighting {false};
};

namespace detail {
inline bool valid_fingerprint(const std::string& value)
{
    if (value.size() != 64) return false;
    for (char c : value) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

inline bool valid_state(const State& state)
{
    if (state.source < 0 || state.source > 2 || state.count < 1 || state.count > 6 ||
        state.colors.size() > size_t(state.count) || state.colors.size() != state.mapping_colors.size() ||
        (state.enabled && state.colors.empty())) return false;
    for (const auto* palette : {&state.colors, &state.mapping_colors})
        for (const auto& color : *palette)
            for (float channel : color)
                if (!std::isfinite(channel) || channel < 0.0f || channel > 1.0f) return false;
    return true;
}

inline bool read_unsigned(const nlohmann::json& value, uint64_t& result)
{
    if (value.is_number_unsigned()) { result = value.get<uint64_t>(); return true; }
    if (!value.is_number_integer() || value.get<int64_t>() < 0) return false;
    result = uint64_t(value.get<int64_t>());
    return true;
}

inline bool read_palette(const nlohmann::json& value, std::vector<GUI::PreviewPalette::Color>& palette)
{
    if (!value.is_array() || value.size() > 6) return false;
    for (const auto& entry : value) {
        if (!entry.is_array() || entry.size() != 3) return false;
        GUI::PreviewPalette::Color color;
        for (size_t channel = 0; channel < color.size(); ++channel) {
            if (!entry[channel].is_number()) return false;
            const double number = entry[channel].get<double>();
            if (!std::isfinite(number) || number < 0.0 || number > 1.0) return false;
            color[channel] = float(number);
        }
        palette.push_back(color);
    }
    return true;
}
} // namespace detail

inline nlohmann::json encode(const State& state, size_t actual_face_count, const std::string& fingerprint)
{
    if (!detail::valid_fingerprint(fingerprint)) throw std::invalid_argument("Invalid trial color geometry fingerprint.");
    if (!detail::valid_state(state)) throw std::invalid_argument("Invalid trial color state.");
    return {{"schema", "orca.color-trial/v1"}, {"geometry_sha256", fingerprint}, {"face_count", actual_face_count},
            {"colors", state.colors}, {"mapping_colors", state.mapping_colors}, {"locks", state.locks},
            {"source", state.source}, {"count", state.count}, {"enabled", state.enabled},
            {"fidelity", state.fidelity}, {"lighting", state.lighting}};
}

// Reject mismatched geometry or malformed fields before returning any state.
// No palette is re-derived, reordered, or fitted to edited vertex colors.
// A disabled snapshot still retains its paired centers for later toggling.
inline bool decode(const nlohmann::json& doc, size_t actual_face_count, const std::string& fingerprint,
                   State& output, std::string& error)
{
    error.clear();
    auto fail = [&](const char* message) { error = message; return false; };
    if (!doc.is_object() || !doc.contains("schema") || doc["schema"] != "orca.color-trial/v1")
        return fail("Unsupported trial color schema.");
    if (!detail::valid_fingerprint(fingerprint) || !doc.contains("geometry_sha256") ||
        !doc["geometry_sha256"].is_string() || doc["geometry_sha256"].get_ref<const std::string&>() != fingerprint)
        return fail("The saved trial colors belong to different geometry.");
    uint64_t face_count, source, count;
    if (!doc.contains("face_count") || !detail::read_unsigned(doc["face_count"], face_count) || face_count != actual_face_count)
        return fail("The saved trial color face count does not match the loaded mesh.");
    if (!doc.contains("source") || !detail::read_unsigned(doc["source"], source) || source > 2 ||
        !doc.contains("count") || !detail::read_unsigned(doc["count"], count) || count < 1 || count > 6)
        return fail("Invalid trial color source or color count.");
    for (const char* field : {"enabled", "fidelity", "lighting"})
        if (!doc.contains(field) || !doc[field].is_boolean()) return fail("Trial color options must be explicit booleans.");
    if (!doc.contains("locks") || !doc["locks"].is_array() || doc["locks"].size() != 6)
        return fail("Trial colors require six lock flags.");
    State restored;
    for (size_t i = 0; i < restored.locks.size(); ++i) {
        if (!doc["locks"][i].is_boolean()) return fail("Trial color locks must be booleans.");
        restored.locks[i] = doc["locks"][i].get<bool>();
    }
    if (!doc.contains("colors") || !doc.contains("mapping_colors") ||
        !detail::read_palette(doc["colors"], restored.colors) ||
        !detail::read_palette(doc["mapping_colors"], restored.mapping_colors))
        return fail("Trial colors require normalized RGB palettes with at most six colors.");
    restored.source = int(source);
    restored.count = int(count);
    restored.enabled = doc["enabled"].get<bool>();
    restored.fidelity = doc["fidelity"].get<bool>();
    restored.lighting = doc["lighting"].get<bool>();
    if (!detail::valid_state(restored)) return fail("Trial color centers and targets must form valid pairs.");
    output = std::move(restored);
    return true;
}
} // namespace Slic3r::AI::ColorTrialPersistence
