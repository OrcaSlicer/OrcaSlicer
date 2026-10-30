#pragma once

#include "McpCore.hpp"
#include "McpTypes.hpp"
#include "VideoEncode.hpp"
#include "VideoOverlay.hpp"

#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

class wxEvent;
class wxFont;
class wxImage;
class wxTimer;
class wxWindow;

namespace Slic3r { namespace GUI {

class GLCanvas3D;

namespace Mcp {

class McpService;
struct Recording;

// `font` resized so that a memory DC, which caption text is drawn in, draws a line of it `line_height`
// pixels tall, whatever DPI that DC draws at. UI thread.
wxFont font_with_line_height(const wxFont& font, int line_height);
// `text` white on black in `font`, cut to `max_width` pixels with an ellipsis (0: whole); its brightness is
// the coverage. Null for no text. UI thread.
std::shared_ptr<const TextMask> render_text(const std::string& text, const wxFont& font, int max_width);

// start_recording's arguments.
struct RecordingOptions
{
    std::string             target = "window"; // window | canvas
    int                     fps         = 10;
    int                     max_dim     = 1920;
    int                     quality     = 80;
    int                     max_seconds = 600;
    bool                    captions    = false; // the current tool call's caption
    bool                    pointer     = false; // the pointer highlighted, its clicks rippling
    boost::filesystem::path path;
    std::string             owner; // the job of the script that started it, "" for a client, kPersonOwner
    // encode_recording's arguments, path included, for the stop_recording that reports this recording to run;
    // null for none.
    nlohmann::json encode;
    // Runs on the UI thread once the recording ends, while its file is still being closed, with the reason it
    // was ended for ("" when the file ended it: a write error or the size limit). Never after a failed start.
    std::function<void(const std::string& reason)> ended;
};

// The owner of a recording the person started from the app's own commands: neither a script's end nor the
// endpoint closing ends it. Job ids never take this form.
inline constexpr const char* kPersonOwner = "person";

// Records what OrcaSlicer shows into an MJPEG Matroska file, with the current tool call as a caption and
// the pointer highlighted when asked for. At most one recording at a time. The UI thread captures a frame
// per tick of a timer (the main window with the modal dialogs on it, or the 3D view shown); everything
// after that, scaling, drawing the captions, JPEG and the file, runs on a RecordingSink's threads. A
// recording ends when stopped, at its time limit, when the script that started it ends, when the
// endpoint closes for a client's, on a write error and at exit; the sink then finalizes the file on its
// own threads, so ending it needs no UI round trip. Created and destroyed on the UI thread with the
// service; the CallObserver methods come from call threads.
class McpRecorder : public CallObserver
{
public:
    explicit McpRecorder(McpService& service);
    ~McpRecorder() override;
    McpRecorder(const McpRecorder&)            = delete;
    McpRecorder& operator=(const McpRecorder&) = delete;

    // UI thread. Takes the first frame, which fixes the video size, then creates the file and starts the
    // timer. The caller has reserved options.path for `files_owner` (RecordingFiles::Use::Recording) on its
    // own thread, since comparing paths may touch the disk; the recording keeps the reservation until its
    // file is closed, and a failure releases it. Throws ToolFailure (and creates no file) when a recording
    // is active or the first capture fails. Returns {path, target, width, height, fps, captions, pointer, note?}.
    nlohmann::json start(const RecordingOptions& options, uint64_t files_owner);
    // UI thread. Captures a frame now, so what the last call did is in the recording before it ends.
    void capture_now();
    // Any thread. Ends the recording, if one runs, for `reason`; returns without waiting for its file.
    void end(const std::string& reason);
    // Any thread. Notes that the script `owner` (a job) has ended, so a start of its that runs later is
    // refused; end_owned_by() notes it too. Call it before the script's scope goes away.
    void owner_ended(const std::string& owner);
    // Any thread. Whether a script in `ancestry` (a call's job chain) has ended: a call from it is no client's.
    bool owner_ended_in(const std::vector<std::string>& ancestry) const;
    // Any thread. Ends the recording `owner` started ("" for a client's), and with `wait`, waits up to 30 s
    // for its file to be closed. Returns the path of a file still being written after that wait.
    std::optional<std::string> end_owned_by(const std::string& owner, const std::string& reason, bool wait);
    // stop_recording: ends the running recording and waits for its file, or takes the result of the last
    // one that ended on its own, once. Null when there is none. `encode` gets the recording's
    // RecordingOptions::encode.
    nlohmann::json stop_and_report(std::chrono::milliseconds timeout, nlohmann::json& encode);
    // get_state's recording: {path, target, seconds, frames} while one runs, else null.
    nlohmann::json state() const;
    // A recording is being written or finalized.
    bool active() const;
    // Any thread. Waits up to `timeout` for the recording at `path` to have its file closed; true once it has,
    // or when that recording is not the latest, since another starts only after it is closed.
    bool wait_closed(const boost::filesystem::path& path, std::chrono::milliseconds timeout) const;
    // The files recordings and encodes use; shared with the sinks' finalizers.
    const std::shared_ptr<RecordingFiles>& files() const { return m_files; }

    // UI thread. The main window is about to be rebuilt: frames kept of its canvases go.
    void suspend();
    // UI thread. The main window has been rebuilt: its tab bar shows the REC pill of the recording that runs.
    void resume();
    // UI thread, at exit: ends the recording and waits up to `timeout` for its file, then drops what is
    // still queued and leaves the rest to the sink's threads.
    void shutdown(std::chrono::milliseconds timeout);

    void call_started(uint64_t call, const ToolDef& def, const nlohmann::json& args, const std::string& parent) override;
    void call_args(uint64_t call, const nlohmann::json& args) override;
    void call_ended(uint64_t call, const ToolResult& result) override;

    // UI thread. Stops the timer and the pointer's event filter of `rec`; called once it ends.
    void cleanup_ui(const std::shared_ptr<Recording>& rec);

private:
    class InputFilter;
    struct Input;

    // Ends `rec` itself, whatever recording is current by then. Any thread.
    void end_recording(const std::shared_ptr<Recording>& rec, const std::string& reason);
    void tick(const std::shared_ptr<Recording>& rec);
    // Captures a frame, or takes `captured`, and queues it with its overlay unless it repeats the last
    // one. UI thread.
    void capture_frame(Recording& rec, std::chrono::steady_clock::time_point now, const wxImage* captured = nullptr);
    // The caption of `line`, its text fitted to the caption's width in video pixels `unit` scales. UI thread.
    OverlayCaption caption_overlay(Recording& rec, const CaptionBoard::Line& line, double unit);
    // The capture's provider of 3D canvas frames: the one kept while the canvas has presented no other.
    bool canvas_frame(Recording& rec, GLCanvas3D& canvas, wxImage& out, std::string& err);
    void on_event(wxEvent& event);
    bool shows(wxWindow* window) const;
    // A recording runs and is not ending.
    bool running() const;
    // Shows the tab bar's REC pill while running(), and hides it otherwise; not while the main window is
    // rebuilt or the app closes. UI thread.
    void update_indicator() const;
    nlohmann::json report(Recording& rec) const;

    McpService&                     m_service;
    std::shared_ptr<RecordingFiles> m_files = std::make_shared<RecordingFiles>();

    mutable std::mutex         m_mutex;
    std::shared_ptr<Recording> m_rec; // the latest recording, running, ending or finished
    // Tool calls on the caption board, by call id: their tool and every version of their arguments, all of
    // whose hidden values a failure's caption must keep hidden.
    struct Captioned
    {
        std::string                 tool;
        std::vector<nlohmann::json> args;
    };
    std::map<uint64_t, Captioned> m_captioned;
    // Scripts whose end end_owned_by() has seen: a start of theirs that runs later is refused. Job ids are
    // never reused.
    std::set<std::string> m_ended_owners;

    // UI thread only, and the recorder's rather than a recording's, so they never leave the UI thread.
    std::unique_ptr<InputFilter> m_filter;
    bool                         m_filter_installed = false;
    std::unique_ptr<Input>       m_input;
    std::unique_ptr<wxTimer>     m_timer;
    std::weak_ptr<Recording>     m_ticking; // the recording the timer captures for
};

}}} // namespace Slic3r::GUI::Mcp
