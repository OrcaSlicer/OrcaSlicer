#include "VideoEncode.hpp"

#include "McpPaths.hpp"
#include "slic3r/Utils/BoostProcessCompat.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <ios>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace Slic3r { namespace GUI { namespace Mcp {

namespace fs = boost::filesystem;

std::optional<fs::path> find_ffmpeg(std::string& err)
{
    boost::system::error_code ec;
    if (const char* env = boost::nowide::getenv("ORCA_FFMPEG"); env != nullptr && *env != '\0') {
        const fs::path p = path_from_utf8(env);
        if (fs::is_regular_file(p, ec))
            return p;
        err = std::string("ORCA_FFMPEG names ") + env + ", which is not a file";
        return std::nullopt;
    }
    try {
        if (const fs::path found = boost::process::search_path("ffmpeg"); !found.empty())
            return found;
    } catch (const std::exception&) {
        // An unreadable PATH entry; the fixed folders below may still have it.
    }
#ifdef __APPLE__
    for (const char* dir : {"/opt/homebrew/bin", "/usr/local/bin", "/opt/local/bin"})
        if (const fs::path p = fs::path(dir) / "ffmpeg"; fs::is_regular_file(p, ec))
            return p;
#endif
    err = "ffmpeg was not found. Install it (a build with libx265) or set ORCA_FFMPEG to its path, then restart OrcaSlicer";
    return std::nullopt;
}

FfmpegVersion parse_ffmpeg_version(const std::string& version_output)
{
    FfmpegVersion v;
    const std::string marker = "version ";
    const std::size_t at     = version_output.find(marker);
    if (at == std::string::npos)
        return v;
    std::size_t i = at + marker.size();
    if (i < version_output.size() && version_output[i] == 'n') // release tags: "n7.0.3"
        ++i;
    auto number = [&](int& out) {
        const std::size_t start = i;
        while (i < version_output.size() && std::isdigit((unsigned char) version_output[i]))
            out = out * 10 + (version_output[i++] - '0');
        return i > start;
    };
    if (!number(v.major))
        return v;
    if (i < version_output.size() && version_output[i] == '.') {
        ++i;
        number(v.minor);
    }
    v.known = true;
    return v;
}

std::set<std::string> parse_encoders(const std::string& encoders_output)
{
    // A legend, a "------" line, then " V....D name  description" per encoder.
    std::set<std::string> out;
    std::istringstream    in(encoders_output);
    std::string           line;
    bool                  listing = false;
    while (std::getline(in, line)) {
        std::istringstream words(line);
        std::string        flags, name;
        words >> flags >> name;
        if (!listing) {
            listing = flags.rfind("------", 0) == 0;
            continue;
        }
        if (flags.size() == 6 && !name.empty())
            out.insert(name);
    }
    return out;
}

std::string encoder_for(const std::string& codec)
{
    if (codec == "h265")
        return "libx265";
    if (codec == "h264")
        return "libx264";
    return {};
}

const std::vector<std::string>& encoder_speeds()
{
    static const std::vector<std::string> speeds{"ultrafast", "superfast", "veryfast", "faster", "fast",
                                                 "medium",    "slow",      "slower",   "veryslow"};
    return speeds;
}

std::vector<std::string> codec_profiles(const std::string& codec)
{
    if (codec == "h265")
        return {"main", "main10"};
    if (codec == "h264")
        return {"high", "main", "baseline"};
    return {};
}

namespace {

bool at_least(const FfmpegVersion& v, int major, int minor) { return v.major > major || (v.major == major && v.minor >= minor); }

} // namespace

std::vector<EncodeTiming> timing_candidates(const FfmpegVersion& version)
{
    if (!version.known)
        return {{true, "demux"}, {true, "-1"}, {false, "-1"}};
    const bool fps_mode = at_least(version, 5, 1);
    if (at_least(version, 6, 1))
        return {{fps_mode, "demux"}};
    if (at_least(version, 3, 4))
        return {{fps_mode, "-1"}};
    return {{fps_mode, ""}};
}

bool refused_timing_option(const std::string& log)
{
    for (const char* sign : {"time base", "fps_mode", "Unrecognized option", "Option not found"})
        if (log.find(sign) != std::string::npos)
            return true;
    return false;
}

std::vector<std::string> encode_args(const EncodeSpec& spec)
{
    std::vector<std::string> args{"-hide_banner", "-nostdin", "-loglevel", "error", "-y", "-i", spec.input, "-map", "0:v:0", "-an"};
    // The recording's own timestamps, gaps included; ffmpeg's default for MP4 differs between versions.
    args.insert(args.end(), {spec.timing.fps_mode ? "-fps_mode" : "-vsync", "vfr"});
    if (!spec.timing.time_base.empty())
        args.insert(args.end(), {"-enc_time_base:v", spec.timing.time_base});
    // JPEG frames are full range; video players expect the limited one.
    args.insert(args.end(), {"-vf", "scale=trunc(iw/2)*2:trunc(ih/2)*2:out_range=tv", "-pix_fmt",
                             spec.profile == "main10" ? "yuv420p10le" : "yuv420p", "-color_range", "tv", "-c:v",
                             encoder_for(spec.codec), "-crf", std::to_string(spec.crf), "-preset", spec.speed});
    if (!spec.profile.empty())
        args.insert(args.end(), {"-profile:v", spec.profile});
    if (spec.codec == "h265")
        args.insert(args.end(), {"-x265-params", "log-level=error", "-tag:v", "hvc1"});
    args.insert(args.end(), {"-movflags", "+faststart", "-f", "mp4", spec.output});
    return args;
}

namespace {

// Terminates the child and returns whether it is gone, so the files it held can be removed. POSIX
// terminate() reaps the child itself. On Windows TerminateProcess() returns before the process exits, and
// boost's terminate() closes the process handle right after it, so the child is ended here on a handle
// that stays valid to wait on.
bool stop(boost::process::child& child)
{
#ifdef _WIN32
    if (!::TerminateProcess(child.native_handle(), 1))
        return false;
    return ::WaitForSingleObject(child.native_handle(), 5000) == WAIT_OBJECT_0;
#else
    child.terminate();
    return true;
#endif
}

} // namespace

ProcessOutcome run_process(const fs::path& exe, const std::vector<std::string>& args, const fs::path& stdout_file, const fs::path& stderr_file,
                           const std::function<bool()>& cancelled, std::chrono::milliseconds timeout)
{
    namespace bp = boost::process;
#ifdef _WIN32
    const fs::path discard("NUL");
#else
    const fs::path discard("/dev/null");
#endif
    ProcessOutcome out;
    try {
        const fs::path out_file = stdout_file.empty() ? discard : stdout_file;
        const fs::path err_file = stderr_file.empty() ? discard : stderr_file;
#ifdef _WIN32
        std::vector<std::wstring> wide;
        for (const std::string& a : args)
            wide.push_back(boost::nowide::widen(a));
        bp::child child(bp::exe = exe, bp::args = wide, bp::std_out > out_file, bp::std_err > err_file, bp::std_in < bp::null,
                        bp::windows::create_no_window);
#else
        bp::child child(bp::exe = exe, bp::args = args, bp::std_out > out_file, bp::std_err > err_file, bp::std_in < bp::null);
#endif
        out.started         = true;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (child.running()) {
            if (cancelled && cancelled()) {
                out.cancelled = true;
                out.gone      = stop(child);
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                out.timed_out = true;
                out.gone      = stop(child);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (out.gone) {
            child.wait();
            out.exit_code = child.exit_code();
        } else {
            child.detach(); // still running: its destructor must not wait for or kill it again
        }
    } catch (const std::exception&) {
        out.exit_code = -1;
    }
    return out;
}

std::optional<double> mp4_duration(const fs::path& file)
{
    boost::nowide::ifstream in(path_to_utf8(file).c_str(), std::ios::binary);
    if (!in)
        return std::nullopt;
    auto read_be = [&](int bytes, uint64_t& value) {
        unsigned char b[8];
        if (!in.read(reinterpret_cast<char*>(b), bytes))
            return false;
        value = 0;
        for (int i = 0; i < bytes; ++i)
            value = (value << 8) | b[i];
        return true;
    };
    // Boxes from `from` to `to`: [size 4][type 4][largesize 8 when size is 1]...; moov holds mvhd.
    std::function<std::optional<double>(uint64_t, uint64_t, bool)> scan = [&](uint64_t from, uint64_t to,
                                                                             bool in_moov) -> std::optional<double> {
        for (uint64_t at = from; at + 8 <= to;) {
            in.clear();
            in.seekg(std::streamoff(at));
            uint64_t size = 0, type = 0;
            if (!read_be(4, size) || !read_be(4, type))
                return std::nullopt;
            uint64_t header = 8;
            if (size == 1) {
                if (!read_be(8, size))
                    return std::nullopt;
                header = 16;
            } else if (size == 0) {
                size = to - at;
            }
            if (size < header || at + size > to)
                return std::nullopt;
            if (!in_moov && type == 0x6D6F6F76) // moov
                return scan(at + header, at + size, true);
            if (in_moov && type == 0x6D766864) { // mvhd: version, flags, two times, timescale, duration
                uint64_t version = 0, skip = 0, timescale = 0, duration = 0;
                if (!read_be(1, version) || !read_be(3, skip))
                    return std::nullopt;
                const int wide = version == 1 ? 8 : 4;
                if (!read_be(wide, skip) || !read_be(wide, skip) || !read_be(4, timescale) || !read_be(wide, duration) || timescale == 0)
                    return std::nullopt;
                return double(duration) / double(timescale);
            }
            at += size;
        }
        return std::nullopt;
    };
    in.seekg(0, std::ios::end);
    const uint64_t end = uint64_t(in.tellg());
    return scan(0, end, false);
}

std::string file_tail(const fs::path& file, int lines)
{
    boost::nowide::ifstream in(path_to_utf8(file).c_str());
    std::deque<std::string> last;
    std::string             line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        last.push_back(line);
        if (int(last.size()) > lines)
            last.pop_front();
    }
    std::string out;
    for (const std::string& l : last)
        out += (out.empty() ? "" : "\n") + l;
    return out;
}

namespace {

const char* use_name(RecordingFiles::Use use)
{
    switch (use) {
    case RecordingFiles::Use::Recording: return "being recorded";
    case RecordingFiles::Use::Source: return "being encoded";
    case RecordingFiles::Use::ExclusiveSource: return "being encoded and then deleted";
    case RecordingFiles::Use::Destination: return "being written by another encode";
    }
    return "in use";
}

} // namespace

bool RecordingFiles::same_file(const fs::path& a, const fs::path& b)
{
    if (path_compare_key(a) == path_compare_key(b))
        return true;
    boost::system::error_code ec;
    return fs::exists(a, ec) && fs::exists(b, ec) && fs::equivalent(a, b, ec) && !ec;
}

uint64_t RecordingFiles::new_owner()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_next++;
}

std::string RecordingFiles::reserve(uint64_t owner, const std::vector<std::pair<fs::path, Use>>& uses)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& [path, use] : uses)
        for (const Reservation& r : m_reserved)
            // Encodes may share a source they only read; every other pair excludes the other.
            if (!(r.use == Use::Source && use == Use::Source) && same_file(r.path, path))
                return path_to_utf8(path) + " is " + use_name(r.use);
    for (const auto& [path, use] : uses)
        m_reserved.push_back({owner, path, use});
    return {};
}

void RecordingFiles::release(uint64_t owner)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_reserved.erase(std::remove_if(m_reserved.begin(), m_reserved.end(), [&](const Reservation& r) { return r.owner == owner; }),
                     m_reserved.end());
}

void RecordingFiles::add_finished(const fs::path& path)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_finished.push_back(path);
}

void RecordingFiles::forget(const fs::path& path)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_finished.erase(std::remove_if(m_finished.begin(), m_finished.end(), [&](const fs::path& p) { return same_file(p, path); }),
                     m_finished.end());
}

bool RecordingFiles::is_finished(const fs::path& path) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::any_of(m_finished.begin(), m_finished.end(), [&](const fs::path& p) { return same_file(p, path); });
}

std::optional<fs::path> RecordingFiles::last_finished() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_finished.empty())
        return std::nullopt;
    return m_finished.back();
}

}}} // namespace Slic3r::GUI::Mcp
