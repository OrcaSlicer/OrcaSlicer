#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <vector>

namespace Slic3r {
class AppConfig;
}

namespace Slic3r { namespace GUI { namespace Mcp {

// What the person's Start Recording asks for: the frames' JPEG quality, whether and how the recording is
// encoded to MP4 once it ends, and the folder both go in. The Start Recording dialog shows them and
// AppConfig's "recording" section remembers them; the command's parameter overrides them for one recording.
struct RecordingSettings
{
    int         quality = 80; // the frames' JPEG quality, 30-95, as start_recording's
    bool        encode  = false;
    std::string codec   = "h265"; // and the rest as encode_recording takes them
    int         crf     = 28;
    std::string speed   = "medium";
    std::string profile = "main";
    std::string folder; // absolute, UTF-8; empty for the default folder, the person's Downloads

    bool operator==(const RecordingSettings& o) const
    {
        return quality == o.quality && encode == o.encode && codec == o.codec && crf == o.crf && speed == o.speed && profile == o.profile &&
               folder == o.folder;
    }
};

// A built-in set of the encoding fields. `name` is an untranslated msgid.
struct EncodePreset
{
    const char* key;
    const char* name;
    const char* codec;
    int         crf;
    const char* speed;
    const char* profile;
};
const std::vector<EncodePreset>& encode_presets();
// The preset `key` names, or nullptr.
const EncodePreset* find_encode_preset(const std::string& key);
void                apply_encode_preset(RecordingSettings& settings, const EncodePreset& preset);
// The key of the preset whose fields `settings` has, or "custom".
std::string matching_encode_preset(const RecordingSettings& settings);

// The remembered settings. A value that is missing or out of range keeps its default, and a profile the
// codec has not becomes the codec's default.
RecordingSettings load_recording_settings(const AppConfig& config);
void              save_recording_settings(AppConfig& config, const RecordingSettings& settings);

// Applies Start Recording's parameter, an object with any of quality, encode, encode_preset, codec, crf,
// speed, profile, folder (an existing directory, absolute) and silent: encode_preset's fields first, then the
// fields it names. A codec named without a profile takes the codec's default profile when the one it had does
// not fit. Returns what is wrong, "" when nothing; `settings` is then unchanged. `silent` is true when the
// parameter says so.
std::string apply_recording_overrides(RecordingSettings& settings, const nlohmann::json& overrides, bool& silent);

// encode_recording's arguments for the recording at `path` (UTF-8).
nlohmann::json encode_arguments(const RecordingSettings& settings, const std::string& path);

}}} // namespace Slic3r::GUI::Mcp
