#include "RecordingSettings.hpp"
#include "McpPaths.hpp"
#include "VideoEncode.hpp"

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

constexpr const char* kSection = "recording";
constexpr int         kMinQuality = 30, kMaxQuality = 95, kMaxCrf = 51;

bool listed(const std::vector<std::string>& values, const std::string& value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

// The codec's default profile when `settings` has one the codec has not.
void fit_profile(RecordingSettings& settings)
{
    const std::vector<std::string> profiles = codec_profiles(settings.codec);
    if (!profiles.empty() && !listed(profiles, settings.profile))
        settings.profile = profiles.front();
}

} // namespace

const std::vector<EncodePreset>& encode_presets()
{
    // H.265 at the speed that suits a short screen recording; H.264 where a player lacks H.265.
    static const std::vector<EncodePreset> presets{
        {"small", L("Small file"), "h265", 32, "slow", "main"},
        {"balanced", L("Balanced"), "h265", 28, "medium", "main"},
        {"quality", L("High quality"), "h265", 22, "slow", "main"},
        {"compatible", L("Most compatible"), "h264", 23, "medium", "high"},
    };
    return presets;
}

const EncodePreset* find_encode_preset(const std::string& key)
{
    const std::vector<EncodePreset>& presets = encode_presets();
    const auto it = std::find_if(presets.begin(), presets.end(), [&](const EncodePreset& p) { return key == p.key; });
    return it == presets.end() ? nullptr : &*it;
}

void apply_encode_preset(RecordingSettings& settings, const EncodePreset& preset)
{
    settings.codec   = preset.codec;
    settings.crf     = preset.crf;
    settings.speed   = preset.speed;
    settings.profile = preset.profile;
}

std::string matching_encode_preset(const RecordingSettings& settings)
{
    for (const EncodePreset& p : encode_presets()) {
        RecordingSettings s = settings;
        apply_encode_preset(s, p);
        if (s == settings)
            return p.key;
    }
    return "custom";
}

RecordingSettings load_recording_settings(const AppConfig& config)
{
    RecordingSettings s;
    const auto number = [&](const char* key, int min, int max, int& out) {
        try {
            const int v = std::stoi(config.get(kSection, key));
            if (v >= min && v <= max)
                out = v;
        } catch (const std::exception&) {
        }
    };
    number("quality", kMinQuality, kMaxQuality, s.quality);
    number("crf", 0, kMaxCrf, s.crf);
    s.captions = config.get(kSection, "captions") == "true";
    s.pointer  = config.get(kSection, "pointer") == "true";
    s.encode   = config.get(kSection, "encode") == "true";
    if (const std::string codec = config.get(kSection, "codec"); !encoder_for(codec).empty())
        s.codec = codec;
    if (const std::string speed = config.get(kSection, "speed"); listed(encoder_speeds(), speed))
        s.speed = speed;
    if (const std::string profile = config.get(kSection, "profile"); !profile.empty())
        s.profile = profile;
    fit_profile(s);
    s.folder = config.get(kSection, "folder");
    return s;
}

void save_recording_settings(AppConfig& config, const RecordingSettings& settings)
{
    config.set(kSection, "quality", std::to_string(settings.quality));
    config.set(kSection, "captions", settings.captions);
    config.set(kSection, "pointer", settings.pointer);
    config.set(kSection, "encode", settings.encode);
    config.set(kSection, "codec", settings.codec);
    config.set(kSection, "crf", std::to_string(settings.crf));
    config.set(kSection, "speed", settings.speed);
    config.set(kSection, "profile", settings.profile);
    config.set(kSection, "folder", settings.folder);
}

std::string apply_recording_overrides(RecordingSettings& settings, const json& overrides, bool& silent)
{
    if (!overrides.is_object())
        return "the parameter must be a JSON object";
    static const std::vector<std::string> keys{"quality", "captions", "pointer", "encode", "encode_preset", "codec",
                                               "crf",     "speed",    "profile", "folder", "silent"};
    for (const auto& [key, value] : overrides.items())
        if (!listed(keys, key))
            return "unknown key \"" + key +
                   "\": quality, captions, pointer, encode, encode_preset, codec, crf, speed, profile, folder or silent";
    const auto integer = [&](const char* key, int min, int max, int& out) -> std::string {
        if (!overrides.contains(key))
            return {};
        const json& v = overrides[key];
        if (!v.is_number_integer() || v.get<int>() < min || v.get<int>() > max)
            return std::string(key) + " must be a whole number from " + std::to_string(min) + " to " + std::to_string(max);
        out = v.get<int>();
        return {};
    };
    const auto text = [&](const char* key, const std::vector<std::string>& allowed, std::string& out) -> std::string {
        if (!overrides.contains(key))
            return {};
        const json& v = overrides[key];
        if (!v.is_string() || !listed(allowed, v.get<std::string>()))
            return std::string(key) + " must be one of " + json(allowed).dump();
        out = v.get<std::string>();
        return {};
    };
    const auto flag = [&](const char* key, bool& out) -> std::string {
        if (!overrides.contains(key))
            return {};
        if (!overrides[key].is_boolean())
            return std::string(key) + " must be true or false";
        out = overrides[key].get<bool>();
        return {};
    };

    RecordingSettings s = settings;
    bool              quiet = false;
    std::string       err;
    if (overrides.contains("encode_preset")) {
        const json&         v = overrides["encode_preset"];
        const EncodePreset* p = v.is_string() ? find_encode_preset(v.get<std::string>()) : nullptr;
        if (p == nullptr) {
            std::vector<std::string> names;
            for (const EncodePreset& preset : encode_presets())
                names.emplace_back(preset.key);
            return "encode_preset must be one of " + json(names).dump();
        }
        apply_encode_preset(s, *p);
    }
    if (!(err = integer("quality", kMinQuality, kMaxQuality, s.quality)).empty() || !(err = flag("captions", s.captions)).empty() ||
        !(err = flag("pointer", s.pointer)).empty() || !(err = flag("encode", s.encode)).empty() ||
        !(err = text("codec", {"h265", "h264"}, s.codec)).empty() || !(err = integer("crf", 0, kMaxCrf, s.crf)).empty() ||
        !(err = text("speed", encoder_speeds(), s.speed)).empty() || !(err = flag("silent", quiet)).empty())
        return err;
    if (overrides.contains("profile")) {
        if (!(err = text("profile", codec_profiles(s.codec), s.profile)).empty())
            return err + " for " + s.codec;
    } else {
        fit_profile(s);
    }
    if (overrides.contains("folder")) {
        const json& v = overrides["folder"];
        if (!v.is_string())
            return "folder must be the path of an existing directory";
        const auto dir = resolve_output_dir(v.get<std::string>(), err);
        if (!dir)
            return "folder: " + err;
        s.folder = path_to_utf8(*dir);
    }
    settings = s;
    silent   = quiet;
    return {};
}

json encode_arguments(const RecordingSettings& settings, const std::string& path)
{
    return {{"path", path}, {"codec", settings.codec}, {"crf", settings.crf}, {"speed", settings.speed}, {"profile", settings.profile}};
}

}}} // namespace Slic3r::GUI::Mcp
