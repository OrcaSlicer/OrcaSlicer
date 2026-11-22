// recording_start, recording_stop and recording_encode: a video of what OrcaSlicer shows, optionally captioned
// with the current tool call and its pointer highlighted, and its encode to MP4.

#include "McpCore.hpp"
#include "McpPaths.hpp"
#include "McpRecorder.hpp"
#include "AutomationRuntime.hpp"
#include "McpTools.hpp"
#include "RecordingSettings.hpp"
#include "VideoEncode.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/NativeCommands.hpp"

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
NativeCommand record_tool(std::string name, std::string title, std::string description, json schema, CommandFn handler)
{
    NativeCommand d = NativeCommands::long_command(std::move(name), std::move(title), {}, std::move(schema), std::move(handler));
    d.description   = std::move(description);
    d.listed        = true;
    d.guard         = GuardClass::Inspect;
    d.journal       = true;
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

std::vector<std::string> encode_preset_keys()
{
    std::vector<std::string> keys;
    for (const EncodePreset& p : encode_presets())
        keys.emplace_back(p.key);
    return keys;
}

} // namespace

ToolResult start_encode(AutomationRuntime& runtime, const json& args)
{
    const NativeCommand* encode = NativeCommands::instance().find("recording_encode");
    if (encode == nullptr || runtime.stopping())
        return ToolResult::error("OrcaSlicer is shutting down");
    NativeCommand def = *encode;
    def.journal   = false;
    def.budget_ms = 0; // the envelope at once; the result comes from app_wait
    return runtime.core().call(def, args);
}

void register_record_tools(CommandCatalog& catalog)
{
    // The Speed Dial's Start Recording is its action: with no arguments it opens the action's dialog.
    NativeCommand start = record_tool(
        "recording_start", L("Start Recording"),
        "Record a video of OrcaSlicer until `recording_stop`: MJPEG frames with their real timestamps in a Matroska (.mkv) "
        "file. Captions show the tool call acting on the app at the bottom (secret setting values never show); pointer "
        "highlights the pointer, its clicks rippling. Capture takes at most about a third of the UI thread, so the frame "
        "rate drops where capturing is slow. One recording at a time: a replay's or script's ends with it, a client's when "
        "the server is turned off. With encode, the `recording_stop` that reports it also writes an MP4 beside it and "
        "deletes the .mkv unless `keep_raw_video_file`; `recording_encode` encodes any recording later. With no arguments it "
        "opens the Start Recording dialog (`blocked_by_dialog`), whose controls are named after their settings for "
        "`widget_set_value`; its \"Start recording\" starts the person's recording, which neither a replay's or script's end "
        "nor the server turning off ends, and the dialog remembers the settings. `silent: true` starts one at once with the "
        "defaults.",
        object_schema({{"target", enum_prop({"main", "canvas"}, "main (default): the main window and its dialogs; canvas: the 3D view shown.")},
                       {"fps", int_prop(1, 30, "Frames per second at most (default 10).")},
                       {"max_dim", int_prop(320, 3840, "Long edge of the video in pixels (default 1920).")},
                       {"jpeg_quality", int_prop(30, 95, "JPEG quality of the frames (default 80).")},
                       {"max_seconds", int_prop(1, 3600, "The recording ends on its own after this long (default 3600).")},
                       {"captions", prop("boolean", "Caption the tool call acting on the app (default false).")},
                       {"pointer", prop("boolean", "Highlight the pointer and show its clicks (default false).")},
                       {"encode", prop("boolean", "Encode it to an MP4 once stopped; needs an ffmpeg (default false).")},
                       {"encode_preset", enum_prop(encode_preset_keys(), "With encode: the settings the fields below change "
                                                                         "(default balanced: h265, crf 28, medium, main).")},
                       {"codec", enum_prop({"h265", "h264"}, "With encode: the codec (default the preset's).")},
                       {"crf", int_prop(0, 51, "With encode: constant rate factor, lower is better and larger (default the preset's).")},
                       {"speed", enum_prop(encoder_speeds(), "With encode: slower makes a smaller file (default the preset's).")},
                       {"profile", enum_prop({"high", "main", "baseline", "main10"},
                                             "With encode: h264 high, main or baseline; h265 main or main10 (default the preset's, else "
                                             "the codec's first).")},
                       {"keep_raw_video_file", prop("boolean", "With encode: keep the .mkv beside the MP4 (default false).")},
                       {"save_path", prop("string", "Absolute .mkv path; default the session's output folder.")},
                       {"overwrite", prop("boolean", "Replace an existing `save_path`, and with encode its MP4.")}}),
        [](const json& args, ToolContext& ctx) {
            AutomationRuntime& runtime = ctx.runtime();
            RecordingOptions o;
            o.target      = arg_string(args, "target", "main");
            o.fps         = arg_int(args, "fps", 10);
            o.max_dim     = arg_int(args, "max_dim", 1920);
            o.quality     = arg_int(args, "jpeg_quality", 80);
            o.max_seconds = arg_int(args, "max_seconds", 3600);
            o.captions    = arg_bool(args, "captions", false);
            o.pointer     = arg_bool(args, "pointer", false);
            // The encode with the Start Recording dialog's defaults, the Balanced preset.
            RecordingSettings settings;
            if (const std::string err = apply_recording_overrides(settings, args); !err.empty())
                throw ToolFailure(err);
            if (std::string err; settings.encode && !find_ffmpeg(err))
                throw ToolFailure("Cannot encode: " + err);
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
            if (settings.encode) {
                o.encode = encode_arguments(settings, path_to_utf8(o.path));
                if (arg_bool(args, "overwrite", false))
                    o.encode["overwrite"] = true;
            }
            // Like auto-answer rules: a run's recording ends with the run, a client's with the endpoint.
            const std::vector<std::string> ancestry = runtime.jobs().ancestry(ctx.job_id());
            o.owner                                 = runtime.run_owner(ancestry);
            // A call of a run that has ended (its scope gone before this ran): not a client's call.
            if (o.owner.empty() && runtime.recorder().owner_ended_in(ancestry))
                throw ToolFailure("The replay or script that started the recording has ended");
            // Reserved here, off the UI thread: comparing paths may touch the disk. start() keeps the
            // reservation for the recording or releases it.
            const std::shared_ptr<RecordingFiles>& files = runtime.recorder().files();
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
                // On the UI thread, as close_client_scope() runs: a client's start queued while the endpoint
                // closed must not begin a recording nothing would end.
                if (o.owner.empty() && !runtime.client_scope_open())
                    throw ToolFailure("The MCP server was turned off; the recording was not started");
                handed_over = true; // start() owns the reservation from here, success or not
                return runtime.recorder().start(o, owner);
            });
            if (settings.encode)
                out["encode"] = {{"codec", settings.codec},
                                 {"crf", settings.crf},
                                 {"speed", settings.speed},
                                 {"profile", settings.profile},
                                 {"keep_raw_video_file", settings.keep_raw_video_file}};
            // The limit it runs under, which a call that leaves it out did not choose.
            out["max_seconds"] = o.max_seconds;
            // The path used, so a replay records into its own output folder under the same name and its
            // encode finds it there.
            json recorded         = args;
            recorded["save_path"] = out["path"];
            ctx.set_journal_args(std::move(recorded));
            return ToolResult::ok(out);
        });
    start.actions = json::array({NativeCommands::action("start_recording", L("Start Recording"), L("Commands"), "recording_start",
                                                        json::object(),
                                                        "Runs recording_start with no arguments, as Start Recording does: the Start "
                                                        "Recording dialog, whose Start recording starts the person's recording, "
                                                        "which neither the end of a replay or script nor the server turning off ends.")});
    catalog.add(NativeCommands::with_person_form(std::move(start), [](const json&) { return NativeCommands::start_person_recording(); }));

    // The Speed Dial's Stop Recording is its action: with no arguments it stops as the action does.
    NativeCommand stop = record_tool("recording_stop", L("Stop Recording"),
                        "Stop the running recording. Call it with `silent: true` for the report: it waits for the file and "
                        "returns it with its seconds, frames captured and "
                        "written, frames dropped when encoding fell behind, failed when they could not be encoded, or "
                        "skipped when the window could not be captured, "
                        "the size, the mean UI-thread time a capture took (`capture_ms`), and why it ended. A recording that ended on its own (`max_seconds`, the replay or script that started it ended, a "
                        "write error) is reported once by the next `recording_stop`. For a recording started with encode, it then "
                        "starts the MP4 encode and returns it under encode: a job, whose `app_wait {for: \"job\"}` returns "
                        "`recording_encode`'s result, whose `raw_video_file_deleted` says whether the .mkv was deleted. "
                        "Called with no arguments, it stops "
                        "the recording as Stop Recording does, whoever started it: without waiting for its "
                        "file, it shows the person a notification naming it and replies with its path; the next "
                        "`recording_stop` with `silent: true` reports it as ended `stopped_in_app`.",
                        object_schema({}), [](const json&, ToolContext& ctx) {
                            AutomationRuntime& runtime = ctx.runtime();
                            // The last state too, even if the timer has not ticked since the last call. Without the
                            // UI thread (a native dialog holds it), the recording ends with its last frame.
                            try {
                                ctx.run_ui([&] {
                                    runtime.recorder().capture_now();
                                    return json();
                                });
                            } catch (const ToolFailure&) {
                            }
                            json encode;
                            json report = runtime.recorder().stop_and_report(std::chrono::seconds(60), encode);
                            if (report.is_null())
                                throw ToolFailure("No recording is running");
                            if (!encode.is_null()) {
                                const ToolResult r = start_encode(runtime, encode);
                                report["encode"]   = r.is_error ? json{{"error", r.message}} : r.data;
                            }
                            return ToolResult::ok(report);
                        });
    stop.actions = json::array({NativeCommands::action("stop_recording", L("Stop Recording"), L("Commands"), "recording_stop",
                                                       json::object(),
                                                       "Runs recording_stop with no arguments, as Stop Recording does: it stops the "
                                                       "recording, whoever started it, and shows the person a notification naming its "
                                                       "file.")});
    // The person's stop opens nothing: like the silent one, it stops a recording behind a dialog.
    stop = NativeCommands::with_person_form(std::move(stop), [](const json&) { return NativeCommands::stop_recording_in_app(); },
                                            /*person_form_behind_dialogs=*/true);
    // It has no other argument, and neither form asks: silent picks the form that waits and reports.
    stop.input_schema["properties"]["silent"]["description"] =
        "true waits for the file and returns the report, and the encode job when the recording has one; a call with no "
        "arguments stops as Stop Recording does.";
    catalog.add(stop);

    NativeCommand encode = record_tool(
        "recording_encode", L("Encode a recording"),
        "Encode a recording into an H.265 (or H.264) MP4 with the system ffmpeg (ORCA_FFMPEG, else ffmpeg on PATH; it "
        "needs libx265 or libx264), keeping its timing. Takes this session's last finished recording unless path names "
        "one, and writes beside it unless `save_path` says where. A long encode returns a job: `app_wait` waits for it, or `job_cancel {job}` "
        "stops it. Once the MP4 is written, it deletes the .mkv if this session recorded it, unless `keep_raw_video_file`; "
        "any other .mkv is kept. `raw_video_file_deleted` in the reply says which.",
        object_schema({{"path", prop("string", "Absolute .mkv of a finished recording; default this session's last.")},
                       {"save_path", prop("string", "Absolute .mp4 path; default the recording's, with .mp4.")},
                       {"overwrite", prop("boolean", "Replace an existing `save_path`.")},
                       {"codec", enum_prop({"h265", "h264"}, "Default h265.")},
                       {"crf", int_prop(0, 51, "Constant rate factor, lower is better and larger (default 28 for h265, 23 for h264).")},
                       {"speed", enum_prop(encoder_speeds(), "The encoder's preset: slower makes a smaller file of the same quality (default medium).")},
                       {"profile", enum_prop({"high", "main", "baseline", "main10"},
                                             "h264: high, main or baseline; h265: main, or main10 for 10 bits per sample. Default the "
                                             "encoder's (high, main).")},
                       {"keep_raw_video_file", prop("boolean", "Keep the .mkv beside the MP4 (default false: a recording this "
                                                               "session made is deleted once the MP4 is written).")}}),
        [](const json& args, ToolContext& ctx) {
            AutomationRuntime& runtime = ctx.runtime();
            const std::shared_ptr<RecordingFiles>& files = runtime.recorder().files();
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
            // The .mkv goes once the MP4 is written unless it is to be kept, and only a recording this session made:
            // any other .mkv is the person's own file, so it stays.
            const bool keep_asked = arg_bool(args, "keep_raw_video_file", false);
            const bool ours       = files->is_finished(source);
            const bool remove     = !keep_asked && ours;

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
            out["raw_video_file_deleted"] = remove && !fs::exists(source, ec);
            if (!keep_asked && !ours)
                out["note"] = "The .mkv was kept: an encode deletes only a recording this session made";
            return ToolResult::ok(out);
        });
    encode.cancellable = true;
    catalog.add(encode);
}

}}} // namespace Slic3r::GUI::Mcp
