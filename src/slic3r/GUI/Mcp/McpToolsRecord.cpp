// start_recording, stop_recording and encode_recording: a video of what OrcaSlicer shows, captioned with the
// current tool call and its pointer highlighted, and its encode to MP4.

#include "McpPaths.hpp"
#include "McpRecorder.hpp"
#include "McpService.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"
#include "VideoEncode.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <ios>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;
namespace fs = boost::filesystem;

namespace {

// The recording tools only read the app, so they run behind dialogs and during operations, but each
// is a job: ownership is read from the job's ancestry, and an encode outlives the reply budget.
ToolDef record_tool(std::string name, std::string title, std::string description, json schema, ToolHandler handler)
{
    ToolDef d;
    d.name         = std::move(name);
    d.title        = std::move(title);
    d.description  = std::move(description);
    d.input_schema = std::move(schema);
    d.guard        = GuardClass::Inspect;
    d.inline_call  = false;
    d.journal      = true;
    d.handler      = std::move(handler);
    return d;
}

json int_prop(int minimum, int maximum, const std::string& description)
{
    return {{"type", "integer"}, {"minimum", minimum}, {"maximum", maximum}, {"description", description}};
}

std::string read_text(const fs::path& path)
{
    boost::nowide::ifstream in(path_to_utf8(path).c_str(), std::ios::binary);
    std::ostringstream      ss;
    ss << in.rdbuf();
    return ss.str();
}

void remove_quietly(const fs::path& path)
{
    boost::system::error_code ec;
    fs::remove(path, ec);
}

int process_id()
{
#ifdef _WIN32
    return _getpid();
#else
    return int(getpid());
#endif
}

} // namespace

void register_record_tools(McpService& service)
{
    ToolRegistry& reg = service.registry();

    reg.add(record_tool(
        "start_recording", L("Start a recording"),
        "Record a video of OrcaSlicer until stop_recording: the main window with any modal dialog on it (target window), or "
        "the 3D view shown (canvas). Frames are MJPEG in a Matroska (.mkv) file with their real timestamps. The tool call "
        "acting on the app shows as a caption at the bottom (secret setting values never show), and the pointer is "
        "highlighted, its clicks rippling; overlays false draws neither. Capture takes at most about a third of the UI thread, so "
        "the frame rate drops where capturing is slow. One recording at a time: one a script starts ends with the script, a "
        "client's when the server is turned off. encode_recording makes an MP4 of it.",
        object_schema({{"target", enum_prop({"window", "canvas"}, "window (default): the main window and its dialogs; canvas: the 3D view shown.")},
                       {"fps", int_prop(1, 30, "Frames per second at most (default 10).")},
                       {"max_dim", int_prop(320, 3840, "Long edge of the video in pixels (default 1920).")},
                       {"quality", int_prop(30, 95, "JPEG quality of the frames (default 80).")},
                       {"max_seconds", int_prop(1, 3600, "The recording ends on its own after this long (default 600).")},
                       {"captions", prop("boolean", "Caption the tool call acting on the app (default true).")},
                       {"pointer", prop("boolean", "Highlight the pointer and show its clicks (default true).")},
                       {"overlays", prop("boolean", "false: no caption and no pointer, whatever captions and pointer say (default true).")},
                       {"save_path", prop("string", "Absolute .mkv path; default the session's output folder.")},
                       {"overwrite", prop("boolean", "Replace an existing save_path.")}}),
        [&service](const json& args, ToolContext& ctx) {
            RecordingOptions o;
            o.target      = arg_string(args, "target", "window");
            o.fps         = arg_int(args, "fps", 10);
            o.max_dim     = arg_int(args, "max_dim", 1920);
            o.quality     = arg_int(args, "quality", 80);
            o.max_seconds = arg_int(args, "max_seconds", 600);
            const bool overlays = arg_bool(args, "overlays", true);
            o.captions          = overlays && arg_bool(args, "captions", true);
            o.pointer           = overlays && arg_bool(args, "pointer", true);
            if (args.contains("save_path")) {
                const std::string path = arg_string(args, "save_path");
                if (!boost::algorithm::iends_with(path, ".mkv"))
                    throw ToolFailure("save_path must end with .mkv: the recording is Matroska");
                std::string err;
                auto        p = resolve_output_path(path, arg_bool(args, "overwrite", false), err);
                if (!p)
                    throw ToolFailure(err);
                o.path = *p;
            } else {
                o.path = timestamped_path(output_dir(), "recording", ".mkv", {".mp4"});
            }
            // Like auto-answer rules: a script's recording ends with the script, a client's with the endpoint.
            const std::vector<std::string> ancestry = service.jobs().ancestry(ctx.job_id());
            o.owner                                 = service.script_owner(ancestry);
            // A step of a script that has ended (its scope gone before this ran): not a client's call.
            if (o.owner.empty() && service.recorder().owner_ended_in(ancestry))
                throw ToolFailure("The script that started the recording has ended");
            // Reserved here, off the UI thread: comparing paths may touch the disk. start() keeps the
            // reservation for the recording or releases it.
            const std::shared_ptr<RecordingFiles>& files = service.recorder().files();
            const uint64_t                         owner = files->new_owner();
            if (const std::string why = files->reserve(owner, {{o.path, RecordingFiles::Use::Recording}}); !why.empty())
                throw ToolFailure("Cannot record there: " + why);
            bool handed_over = false;
            struct Release
            {
                RecordingFiles& files;
                uint64_t        owner;
                const bool&     handed_over;
                ~Release()
                {
                    if (!handed_over)
                        files.release(owner);
                }
            } release{*files, owner, handed_over};
            json out = ctx.run_ui([&] {
                ui_plater(); // refuses while OrcaSlicer starts or closes
                // On the UI thread, as stop_serving() runs: a client's start queued while the endpoint
                // closed must not begin a recording nothing would end.
                if (o.owner.empty() && !service.endpoint_open())
                    throw ToolFailure("The MCP server was turned off; the recording was not started");
                handed_over = true; // start() owns the reservation from here, success or not
                return service.recorder().start(o, owner);
            });
            // The path used, so a replay records into its own output folder under the same name and its
            // encode finds it there.
            json recorded         = args;
            recorded["save_path"] = out["path"];
            ctx.set_journal_args(std::move(recorded));
            return ToolResult::ok(out);
        }));

    reg.add(record_tool("stop_recording", L("Stop the recording"),
                        "Stop the running recording and return its file once it is written: seconds, frames captured and "
                        "written, frames dropped when encoding fell behind or skipped when the window could not be captured, "
                        "the size, the mean UI-thread time a capture took (capture_ms), and why it ended. A recording that ended on its own (max_seconds, its script ended, a "
                        "write error) is reported once by the next stop_recording.",
                        object_schema({}), [&service](const json&, ToolContext& ctx) {
                            // The last state too, even if the timer has not ticked since the last call. Without the
                            // UI thread (a native dialog holds it), the recording ends with its last frame.
                            try {
                                ctx.run_ui([&] {
                                    service.recorder().capture_now();
                                    return json();
                                });
                            } catch (const ToolFailure&) {
                            }
                            const json report = service.recorder().stop_and_report(std::chrono::seconds(60));
                            if (report.is_null())
                                throw ToolFailure("No recording is running");
                            return ToolResult::ok(report);
                        }));

    ToolDef encode = record_tool(
        "encode_recording", L("Encode a recording"),
        "Encode a recording into an H.265 (or H.264) MP4 with the system ffmpeg (ORCA_FFMPEG, else ffmpeg on PATH; it "
        "needs libx265 or libx264), keeping its timing. Takes this session's last finished recording unless path names "
        "one, and writes beside it unless save_path says where. A long encode returns a job: wait for it, or cancel {job} "
        "stops it. delete_source removes the .mkv afterwards, only for a recording this session made.",
        object_schema({{"path", prop("string", "Absolute .mkv of a finished recording; default this session's last.")},
                       {"save_path", prop("string", "Absolute .mp4 path; default the recording's, with .mp4.")},
                       {"overwrite", prop("boolean", "Replace an existing save_path.")},
                       {"codec", enum_prop({"h265", "h264"}, "Default h265.")},
                       {"crf", int_prop(0, 51, "Constant rate factor, lower is better and larger (default 28 for h265, 23 for h264).")},
                       {"speed", enum_prop(encoder_speeds(), "The encoder's preset: slower makes a smaller file of the same quality (default medium).")},
                       {"profile", enum_prop({"high", "main", "baseline", "main10"},
                                             "h264: high, main or baseline; h265: main, or main10 for 10 bits per sample. Default the "
                                             "encoder's (high, main).")},
                       {"delete_source", prop("boolean", "Delete the .mkv once encoded (a recording this session made).")}}),
        [&service](const json& args, ToolContext& ctx) {
            const std::shared_ptr<RecordingFiles>& files = service.recorder().files();
            std::string                            err;
            fs::path                               source;
            if (args.contains("path")) {
                auto p = resolve_input_path(arg_string(args, "path"), err);
                if (!p)
                    throw ToolFailure(err);
                source = *p;
            } else if (auto last = files->last_finished()) {
                source = *last;
            } else {
                throw ToolFailure("No recording of this session has finished; give path");
            }
            if (!boost::algorithm::iends_with(path_to_utf8(source), ".mkv"))
                throw ToolFailure("path must be a .mkv recording");
            if (boost::system::error_code ec; !fs::is_regular_file(source, ec))
                throw ToolFailure("No such recording: " + path_to_utf8(source));

            const bool overwrite = arg_bool(args, "overwrite", false);
            fs::path   dest;
            if (args.contains("save_path")) {
                auto p = resolve_output_path(arg_string(args, "save_path"), overwrite, err);
                if (!p)
                    throw ToolFailure(err);
                dest = *p;
            } else {
                dest = source;
                dest.replace_extension(".mp4");
                if (boost::system::error_code ec; fs::exists(dest, ec) && !overwrite)
                    throw ToolFailure(path_to_utf8(dest) + " exists; set overwrite or give save_path");
            }
            if (RecordingFiles::same_file(source, dest))
                throw ToolFailure("save_path is the recording itself");
            const bool remove = arg_bool(args, "delete_source", false);
            if (remove && !files->is_finished(source))
                throw ToolFailure("delete_source deletes only a recording this session made; " + path_to_utf8(source) + " is not one");

            const std::string codec   = arg_string(args, "codec", "h265");
            const std::string encoder = encoder_for(codec);
            const int         crf     = arg_int(args, "crf", codec == "h265" ? 28 : 23);
            const std::string profile = arg_string(args, "profile", "");
            if (const std::vector<std::string> profiles = codec_profiles(codec);
                !profile.empty() && std::find(profiles.begin(), profiles.end(), profile) == profiles.end())
                throw ToolFailure(codec + " has no " + profile + " profile");
            const auto        ffmpeg  = find_ffmpeg(err);
            if (!ffmpeg)
                throw ToolFailure(err);

            const uint64_t owner = files->new_owner();
            if (const std::string why = files->reserve(owner, {{source, remove ? RecordingFiles::Use::ExclusiveSource : RecordingFiles::Use::Source},
                                                               {dest, RecordingFiles::Use::Destination}});
                !why.empty())
                throw ToolFailure("Cannot encode now: " + why);
            // Every file this call makes is its own, so failing or a cancel removes only those.
            const std::string tag   = "." + std::to_string(process_id()) + "-" + std::to_string(owner);
            const fs::path    part  = dest.parent_path() / (dest.filename().native() + path_from_utf8(tag + ".part").native());
            const fs::path    log   = dest.parent_path() / (dest.filename().native() + path_from_utf8(tag + ".log").native());
            const fs::path    probe = dest.parent_path() / (dest.filename().native() + path_from_utf8(tag + ".probe").native());
            struct Cleanup
            {
                RecordingFiles&       files;
                uint64_t              owner;
                std::vector<fs::path> paths;
                bool                  release = true;
                ~Cleanup()
                {
                    for (const fs::path& p : paths)
                        remove_quietly(p);
                    if (release)
                        files.release(owner);
                }
            } cleanup{*files, owner, {part, log, probe}};
            const auto cancelled = [&ctx] { return ctx.cancelled(); };

            // What this ffmpeg is: the options that keep the timing depend on its version, and the encoder
            // must be built in.
            auto listing = [&](const char* what) {
                const ProcessOutcome r = run_process(*ffmpeg, {"-hide_banner", what}, probe, {}, cancelled, std::chrono::seconds(20));
                if (ctx.cancelled())
                    throw ToolFailure("The encode was cancelled");
                if (!r.started || r.timed_out || r.exit_code != 0)
                    throw ToolFailure("Cannot run " + path_to_utf8(*ffmpeg) + " " + what);
                return read_text(probe);
            };
            const FfmpegVersion version = parse_ffmpeg_version(listing("-version"));
            if (parse_encoders(listing("-encoders")).count(encoder) == 0)
                throw ToolFailure(path_to_utf8(*ffmpeg) + " has no " + encoder +
                                  " encoder. Install an ffmpeg built with it (Homebrew, winget Gyan.FFmpeg, Debian/Ubuntu ffmpeg) or "
                                  "point ORCA_FFMPEG at one");

            EncodeSpec spec;
            spec.input  = path_to_utf8(source);
            spec.output = path_to_utf8(part);
            spec.codec   = codec;
            spec.crf     = crf;
            spec.speed   = arg_string(args, "speed", "medium");
            spec.profile = profile;
            ProcessOutcome run;
            // A build whose version is unknown may refuse the current spelling of the timing options: the next.
            const std::vector<EncodeTiming> timings = timing_candidates(version);
            for (std::size_t i = 0; i < timings.size(); ++i) {
                spec.timing = timings[i];
                run         = run_process(*ffmpeg, encode_args(spec), {}, log, cancelled, std::chrono::hours(4));
                const bool refused = run.started && run.gone && !run.cancelled && !run.timed_out && run.exit_code != 0 &&
                                     refused_timing_option(file_tail(log, 6));
                if (!refused || i + 1 == timings.size())
                    break;
            }
            if (!run.gone) {
                // It may still read the source and write its files: they are left for it, and the source and
                // destination stay reserved for the session, so no later encode deletes or writes them under it.
                cleanup.paths   = {probe};
                cleanup.release = false;
                throw ToolFailure("ffmpeg did not exit after it was stopped; " + path_to_utf8(part) + " and " + path_to_utf8(log) +
                                  " are left, and " + path_to_utf8(source) + " stays in use until OrcaSlicer restarts");
            }
            if (run.cancelled)
                throw ToolFailure("The encode was cancelled");
            if (!run.started)
                throw ToolFailure("Cannot run " + path_to_utf8(*ffmpeg));
            if (run.timed_out || run.exit_code != 0) {
                const std::string tail = file_tail(log, 6);
                throw ToolFailure(run.timed_out ? "ffmpeg did not finish within 4 hours" :
                                                  "ffmpeg failed" + (tail.empty() ? std::string() : ":\n" + tail));
            }
            const double seconds = mp4_duration(part).value_or(0.);

            boost::system::error_code ec;
            if (!overwrite && fs::exists(dest, ec))
                throw ToolFailure(path_to_utf8(dest) + " appeared while encoding; set overwrite to replace it");
            fs::rename(part, dest, ec); // replaces an existing file on every platform
            if (ec)
                throw ToolFailure("Cannot write " + path_to_utf8(dest) + ": " + ec.message());
            if (remove && fs::remove(source, ec))
                files->forget(source);
            const uintmax_t size = fs::file_size(dest, ec);
            json            out{{"path", path_to_utf8(dest)},
                                {"source", path_to_utf8(source)},
                                {"codec", codec},
                                {"encoder", encoder},
                                {"seconds", std::round(seconds * 10.) / 10.},
                                {"size_bytes", ec ? 0 : size}};
            if (remove)
                out["source_deleted"] = !fs::exists(source, ec);
            return ToolResult::ok(out);
        });
    encode.destructive = true; // delete_source
    encode.cancellable = true;
    reg.add(encode);
}

}}} // namespace Slic3r::GUI::Mcp
