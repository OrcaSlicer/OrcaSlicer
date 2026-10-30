#pragma once

#include <boost/filesystem/path.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

// encode_recording's half: finding the system ffmpeg, its command line, running it, and which files
// recordings and encodes are using. The bundled FFmpeg only decodes, so the encode runs the system one.
// wx-free.

// ORCA_FFMPEG when set, else ffmpeg on PATH, else (macOS, where an app started from Finder gets launchd's
// minimal PATH) /opt/homebrew/bin, /usr/local/bin and /opt/local/bin. Never a path a tool call names: a
// tool call must not run an arbitrary program.
std::optional<boost::filesystem::path> find_ffmpeg(std::string& err);

struct FfmpegVersion
{
    int  major = 0, minor = 0;
    bool known = false; // false for a git build ("N-...") or an unreadable banner: taken as current
};
// From `ffmpeg -version`'s first line: "ffmpeg version 7.1-full_build...", "ffmpeg version n7.0.3", ...
FfmpegVersion parse_ffmpeg_version(const std::string& version_output);
// The encoder names `ffmpeg -encoders` lists.
std::set<std::string> parse_encoders(const std::string& encoders_output);

// The libx265 (h265) or libx264 (h264) encoder; empty for another codec name.
std::string encoder_for(const std::string& codec);
// The speed presets libx264 and libx265 share, fastest first; a slower one makes a smaller file of the same
// quality.
const std::vector<std::string>& encoder_speeds();
// The profiles `codec` can encode 4:2:0 frames in, its default first: H.264 high, main, baseline; H.265 main
// and main10, which encodes 10 bits per sample. Empty for another codec name.
std::vector<std::string> codec_profiles(const std::string& codec);

// The options that keep a recording's timing, which ffmpeg spells differently by version: variable frame
// rate as -fps_mode vfr (5.1 on) or -vsync vfr (before), and the encoder in the source's time base, which
// keeps the timestamps to the millisecond (without it they round to whole frames of the MKV's nominal
// rate), as -enc_time_base demux (6.1 on), -1 (3.4 on) or not at all (before).
struct EncodeTiming
{
    bool        fps_mode = true;
    std::string time_base; // empty leaves the option out

    bool operator==(const EncodeTiming& o) const { return fps_mode == o.fps_mode && time_base == o.time_base; }
};
// The timing options to try, in order. A git build's version is unknown: the current spellings first,
// then older ones if ffmpeg refuses them (refused_timing_option()).
std::vector<EncodeTiming> timing_candidates(const FfmpegVersion& version);
// Whether ffmpeg's error output says it refused one of the timing options.
bool refused_timing_option(const std::string& log);

struct EncodeSpec
{
    std::string  input, output; // UTF-8; the output is written as MP4 whatever its extension
    std::string  codec = "h265";
    int          crf   = 28;
    std::string  speed = "medium";
    std::string  profile; // empty: the encoder's default
    EncodeTiming timing;
};
// The arguments after the executable: the capture timestamps kept (VFR, in the source's time base),
// even dimensions, 4:2:0 in the limited range players expect, and for H.265 the hvc1 tag Apple's players
// need.
std::vector<std::string> encode_args(const EncodeSpec& spec);

// An MP4's length in seconds, from its movie header (mvhd), as players show it; empty when the file has
// none.
std::optional<double> mp4_duration(const boost::filesystem::path& file);

struct ProcessOutcome
{
    int  exit_code = -1;
    bool started   = false;
    bool cancelled = false;
    bool timed_out = false;
    // False only when a terminated child was not seen to exit: it may still hold its files.
    bool gone = true;
};
// Runs `exe` with `args` (UTF-8), stdout and stderr into files (an empty path discards), without a
// console window on Windows. Polls every 100 ms and terminates the child once `cancelled` returns true or
// `timeout` passes, then waits up to 5 s for it to be gone.
ProcessOutcome run_process(const boost::filesystem::path& exe, const std::vector<std::string>& args, const boost::filesystem::path& stdout_file,
                           const boost::filesystem::path& stderr_file, const std::function<bool()>& cancelled,
                           std::chrono::milliseconds timeout);

// The last `lines` lines of a text file, for an error message.
std::string file_tail(const boost::filesystem::path& file, int lines);

// Which files the recording and the encodes are using, so no two of them write one file and no encode
// reads a file still being written; and which recordings this session finished, the only files an
// encode may delete. Paths compare after normalization, without case on Windows and macOS, and as the
// same file when both exist. Thread-safe.
class RecordingFiles
{
public:
    enum class Use
    {
        Recording,       // being written by the recorder, until finalized
        Source,          // read by an encode; several may read one file
        ExclusiveSource, // read by an encode that deletes it afterwards
        Destination      // written by an encode
    };

    uint64_t new_owner();
    // Reserves every one of `uses` for `owner`, or none of them: returns why not, empty on success.
    std::string reserve(uint64_t owner, const std::vector<std::pair<boost::filesystem::path, Use>>& uses);
    void        release(uint64_t owner);

    void add_finished(const boost::filesystem::path& path);
    // A finished recording that an encode deleted: no longer the last one, nor deletable again.
    void                                   forget(const boost::filesystem::path& path);
    bool                                   is_finished(const boost::filesystem::path& path) const;
    std::optional<boost::filesystem::path> last_finished() const;

    static bool same_file(const boost::filesystem::path& a, const boost::filesystem::path& b);

private:
    struct Reservation
    {
        uint64_t                owner = 0;
        boost::filesystem::path path;
        Use                     use = Use::Source;
    };
    mutable std::mutex                   m_mutex;
    uint64_t                             m_next = 1;
    std::vector<Reservation>             m_reserved;
    std::vector<boost::filesystem::path> m_finished;
};

}}} // namespace Slic3r::GUI::Mcp
