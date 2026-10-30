#include "McpRecorder.hpp"

#include "McpPaths.hpp"
#include "McpService.hpp"
#include "McpTypes.hpp"
#include "MkvWriter.hpp"
#include "ModalTracker.hpp"
#include "RecordingSink.hpp"
#include "ScreenCapture.hpp"
#include "UiInspector.hpp"
#include "VideoEncode.hpp"
#include "VideoOverlay.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"

#include <wx/bitmap.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/control.h>
#include <wx/dcmemory.h>
#include <wx/defs.h>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/eventfilter.h>
#include <wx/font.h>
#include <wx/gdicmn.h>
#include <wx/glcanvas.h>
#include <wx/image.h>
#include <wx/object.h>
#include <wx/radiobut.h>
#include <wx/settings.h>
#include <wx/tglbtn.h>
#include <wx/thread.h>
#include <wx/timer.h>
#include <wx/toplevel.h>
#include <wx/translation.h>
#include <wx/weakref.h>
#include <wx/window.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace fs = boost::filesystem;

namespace {

constexpr auto kHeartbeat     = std::chrono::seconds(1);       // a frame at least this often, changed or not
constexpr auto kPointerLinger = std::chrono::milliseconds(3000); // the pointer fades after this long still
constexpr auto kPointerFade   = std::chrono::milliseconds(500);
constexpr auto kRipple        = std::chrono::milliseconds(600);
constexpr auto kSemanticClick = std::chrono::milliseconds(300); // a command event this soon after a press is that press's

// Encoded frames go to the Matroska file.
class MkvOutput final : public SinkOutput
{
public:
    bool open(const std::string& path, int width, int height, double fps, std::string& err) { return m_writer.open(path, width, height, fps, err); }
    bool write(int64_t ms, const std::vector<uint8_t>& jpeg, std::string& err) override
    {
        return m_writer.add_frame(ms, jpeg.data(), jpeg.size(), err);
    }
    bool     close(int64_t end_ms, std::string& err) override { return m_writer.close(end_ms, err); }
    uint64_t bytes() const override { return m_writer.bytes_written(); }

private:
    MkvWriter m_writer;
};

// Physical pixels per wx coordinate: wxMSW coordinates are physical already.
double physical_scale(wxWindow* w)
{
#ifdef __WXMSW__
    (void) w;
    return 1.;
#else
    return w->GetContentScaleFactor();
#endif
}

double seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

double fade(Clock::duration since, Clock::duration linger, Clock::duration fade_time)
{
    if (since <= linger)
        return 1.;
    return std::max(0., 1. - seconds(since - linger) / seconds(fade_time));
}

} // namespace

wxFont font_with_line_height(const wxFont& font, int line_height)
{
    wxBitmap   probe(1, 1, 24);
    wxMemoryDC dc(probe);
    wxFont     out = font;
    // Twice: a line's height follows the font's size closely, not exactly.
    for (int i = 0; i < 2; ++i) {
        dc.SetFont(out);
        const int h = dc.GetCharHeight();
        if (h <= 0 || h == line_height)
            break;
        out.SetFractionalPointSize(out.GetFractionalPointSize() * line_height / h);
    }
    return out;
}

std::shared_ptr<const TextMask> render_text(const std::string& text, const wxFont& font, int max_width)
{
    wxBitmap   probe(1, 1, 24);
    wxMemoryDC dc(probe);
    dc.SetFont(font);
    const wxString shown = max_width > 0 ? wxControl::Ellipsize(from_u8(text), dc, wxELLIPSIZE_END, max_width) : from_u8(text);
    const wxSize   size  = dc.GetTextExtent(shown);
    if (size.x <= 0 || size.y <= 0)
        return nullptr;
    wxBitmap bitmap(size.x, size.y, 24);
    dc.SelectObject(bitmap);
    dc.SetBackground(*wxBLACK_BRUSH);
    dc.Clear();
    dc.SetFont(font);
    dc.SetTextForeground(*wxWHITE);
    dc.DrawText(shown, 0, 0);
    dc.SelectObject(wxNullBitmap);
    const wxImage image = bitmap.ConvertToImage();
    auto          mask  = std::make_shared<TextMask>();
    mask->width         = image.GetWidth();
    mask->height        = image.GetHeight();
    mask->coverage.resize(std::size_t(mask->width) * std::size_t(mask->height));
    const unsigned char* p = image.GetData();
    // ClearType draws coloured fringes; the brightest channel is the coverage.
    for (uint8_t& c : mask->coverage) {
        c = std::max({p[0], p[1], p[2]});
        p += 3;
    }
    return mask;
}

// One recording. Shared between the recorder, a stop or a script end waiting on it, and its posted cleanup,
// so any of those threads may drop it last. Its wx members and window captures are UI-thread objects that
// cleanup_ui() releases on the UI thread before then; the timer and the input state, which must stay on the
// UI thread, are the recorder's.
struct Recording
{
    RecordingOptions               options;
    std::shared_ptr<RecordingSink> sink;
    uint64_t                       files_owner = 0;
    Clock::time_point              started;
    int                            video_w = 0, video_h = 0;
    std::string                    note;
    std::atomic<bool>              ending{false};
    std::atomic<uint64_t>          skipped{0};
    std::atomic<uint64_t>          captures{0};    // ticks that captured, and their UI-thread time
    std::atomic<uint64_t>          capture_us{0};
    std::string                    reason;          // the recorder's mutex
    bool                           reported = false; // the recorder's mutex
    CaptionBoard                   board;           // the recorder's mutex

    // UI thread only; cleanup_ui() clears them.
    std::shared_ptr<CaptureSessions> windows; // open for the whole recording

    wxImage           previous; // the last frame queued, for repeats
    OverlaySnapshot   previous_overlay;
    Clock::time_point last_queued;
    wxPoint           origin; // the captured area's top left on the screen, wx coordinates
    double            scale = 1.;
    struct CanvasFrame
    {
        wxImage  image;
        uint64_t presented  = 0;
        uint64_t generation = 0;
        wxSize   size;
    };
    std::map<const GLCanvas3D*, CanvasFrame> canvases;
    // The caption's font for `masks_unit`, and its text by text and width (0: whole).
    wxFont                                                                 caption_font;
    double                                                                 masks_unit = 0.;
    std::map<std::pair<std::string, int>, std::shared_ptr<const TextMask>> masks;

    int64_t elapsed_ms(Clock::time_point now) const { return std::chrono::duration_cast<std::chrono::milliseconds>(now - started).count(); }
};

// The pointer and the presses the input filter saw, for the recording running. UI thread only.
struct McpRecorder::Input
{
    bool              pointer_known = false;
    wxPoint           pointer; // screen
    Clock::time_point pointer_at;
    struct Press
    {
        wxPoint           at;
        Clock::time_point when;
        PointerButton     button = PointerButton::Left;
    };
    std::deque<Press>   ripples;
    wxWeakRef<wxWindow> pressed_window; // the last press's, which a command event right after it belongs to
    Clock::time_point   pressed_at;
};

class McpRecorder::InputFilter : public wxEventFilter
{
public:
    explicit InputFilter(McpRecorder& recorder) : m_recorder(recorder) {}
    int FilterEvent(wxEvent& event) override
    {
        m_recorder.on_event(event);
        return Event_Skip;
    }

private:
    McpRecorder& m_recorder;
};

McpRecorder::McpRecorder(McpService& service)
    : m_service(service), m_filter(std::make_unique<InputFilter>(*this)), m_input(std::make_unique<Input>())
{}

McpRecorder::~McpRecorder()
{
    std::shared_ptr<Recording> rec;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        rec = m_rec;
    }
    if (rec)
        cleanup_ui(rec);
    if (m_timer)
        m_timer->Stop();
    if (m_filter_installed)
        wxEvtHandler::RemoveFilter(m_filter.get());
}

namespace {

// The captured area: the main window with the modal dialogs on it, or the 3D view shown. Sets where it
// is on the screen, for the pointer. UI thread.
bool capture_area(McpService& service, Recording& rec, const CaptureOptions& options, wxImage& out, std::string& err)
{
    if (service.suspended()) {
        err = "OrcaSlicer is rebuilding its window";
        return false;
    }
    MainFrame* frame = wxGetApp().mainframe;
    if (frame == nullptr) {
        err = "The main window is not up yet";
        return false;
    }
    wxWindow* area = frame;
    if (rec.options.target == "canvas") {
        GLCanvas3D* canvas = shown_canvas();
        if (canvas == nullptr || canvas->get_wxglcanvas() == nullptr) {
            err = "No 3D view is shown; switch to one with set_view tab";
            return false;
        }
        std::optional<Capture> cap = capture_canvas(*canvas, err, options);
        if (!cap)
            return false;
        out  = cap->image;
        area = canvas->get_wxglcanvas();
    } else {
        std::optional<Capture> cap = capture_window(*frame, err, options);
        if (!cap)
            return false;
        out = cap->image;
        if (capture_reads_screen()) {
            // The capture has whatever of ours is on top, modal dialogs included.
            rec.note = "X11: the windows over the main window show as they are on screen";
        } else if (!window_positions_known()) {
            rec.note = "Wayland: dialogs are not drawn in, since their place on the screen is unknown";
        } else {
            // The modal dialogs, outermost first, which is how they stack.
            const double  scale  = physical_scale(frame);
            const wxPoint origin = frame->ClientToScreen(wxPoint(0, 0));
            for (wxDialog* dialog : service.modals().stack()) {
                if (dialog == nullptr || is_native_dialog(dialog) || hidden_from_automation(dialog) || !dialog->IsShownOnScreen())
                    continue;
                std::string            dialog_err;
                std::optional<Capture> d = capture_window(*dialog, dialog_err, options);
                if (!d)
                    continue;
                wxImage      image = d->image;
                const double own   = physical_scale(dialog);
                if (std::abs(own - scale) > 1e-3)
                    image = image.Scale(std::max(1, int(std::lround(image.GetWidth() * scale / own))),
                                        std::max(1, int(std::lround(image.GetHeight() * scale / own))), wxIMAGE_QUALITY_BILINEAR);
                const wxPoint at = dialog->ClientToScreen(wxPoint(0, 0)) - origin;
                composite(out, image, wxPoint(int(std::lround(at.x * scale)), int(std::lround(at.y * scale))));
            }
        }
    }
    rec.origin = area->ClientToScreen(wxPoint(0, 0));
    rec.scale  = physical_scale(area);
    return true;
}

// What a caption adds to the tool's title. Never throws, so a malformed argument costs only the detail.
std::string safe_caption_detail(const std::string& tool, const json& args)
{
    try {
        return caption_detail(tool, args);
    } catch (const std::exception&) {
        return {};
    }
}

} // namespace

OverlayCaption McpRecorder::caption_overlay(Recording& rec, const CaptionBoard::Line& line, double unit)
{
    // The app's own label font as tall as the app's labels at `unit`: their height is measured on the main
    // window, which draws at the screen's DPI, and matched in the memory DC the text is drawn in, which may
    // draw at another.
    if (rec.masks_unit != unit || !rec.caption_font.IsOk()) {
        rec.masks.clear();
        rec.masks_unit = unit;
        int app_h = 0;
        if (MainFrame* frame = wxGetApp().mainframe; frame != nullptr)
            frame->GetTextExtent("Hg", nullptr, &app_h, nullptr, nullptr, &Label::Body_14);
        const double at_100 = app_h > 0 ? app_h / (wxGetApp().em_unit() / 10.) : 19.;
        rec.caption_font    = font_with_line_height(Label::Body_14, std::max(1, int(std::lround(at_100 * unit))));
    }
    auto mask = [&](const std::string& text, int width) {
        std::shared_ptr<const TextMask>& kept = rec.masks[{text, width}];
        if (!kept)
            kept = render_text(text, rec.caption_font, width);
        std::shared_ptr<const TextMask> out = kept;
        if (rec.masks.size() > 256) // long sessions: keep the cache from growing without end
            rec.masks.clear();
        return out;
    };

    // At most a few hundred bytes of each, more than any caption shows, so a long value costs little to
    // measure; only what fits is drawn.
    std::vector<std::pair<std::string, SpanRole>> parts{{shorten(into_u8(wxGetTranslation(from_u8(line.title))), 400), SpanRole::Title}};
    if (!line.detail.empty())
        parts.push_back({shorten(line.detail, 400), SpanRole::Detail});
    if (!line.error.empty())
        parts.push_back({shorten(line.error, 400), SpanRole::Error});
    wxBitmap   probe(1, 1, 24);
    wxMemoryDC dc(probe);
    dc.SetFont(rec.caption_font);
    std::vector<int> widths;
    for (const auto& [text, role] : parts)
        widths.push_back(text.empty() ? 0 : dc.GetTextExtent(from_u8(text)).x);
    // The title first; on a failure the error before the detail, since it says what went wrong.
    std::vector<std::size_t> priority{0};
    for (std::size_t i = parts.size(); i-- > 1;)
        if (parts[i].second == SpanRole::Error)
            priority.push_back(i);
    for (std::size_t i = 1; i < parts.size(); ++i)
        if (parts[i].second != SpanRole::Error)
            priority.push_back(i);
    const int              three_em = int(std::lround(2.5 * dc.GetCharHeight())); // a line is about 1.2 em
    const std::vector<int> fit      = fit_spans(widths, priority, int(CaptionMetrics::text_budget(rec.video_w, unit)),
                                                int(std::lround(CaptionMetrics::kGap * unit)), three_em);
    OverlayCaption out;
    out.status = line.status;
    out.alpha  = line.alpha;
    for (std::size_t i = 0; i < parts.size(); ++i)
        if (fit[i] > 0)
            if (std::shared_ptr<const TextMask> m = mask(parts[i].first, fit[i] < widths[i] ? fit[i] : 0))
                out.spans.push_back({std::move(m), parts[i].second});
    return out;
}

bool McpRecorder::canvas_frame(Recording& rec, GLCanvas3D& canvas, wxImage& out, std::string& err)
{
    // The frame on screen is the last one the canvas presented: kept until it presents another.
    Recording::CanvasFrame& kept       = rec.canvases[&canvas];
    const uint64_t          generation = m_service.ui_generation();
    const wxSize            size       = canvas.get_wxglcanvas()->GetClientSize();
    if (kept.image.IsOk() && kept.generation == generation && kept.size == size && kept.presented == canvas.presented_frames()) {
        out = kept.image;
        return true;
    }
    if (!canvas.render_and_capture(out, err))
        return false;
    // Read after the capture, which presented a frame of its own.
    kept = {out, canvas.presented_frames(), generation, size};
    return true;
}

json McpRecorder::start(const RecordingOptions& options, uint64_t files_owner)
{
    // The path is reserved for `files_owner`; until the sink takes that over, a failure here releases it.
    struct Release
    {
        RecordingFiles& files;
        uint64_t        owner;
        bool            armed = true;
        ~Release()
        {
            if (armed)
                files.release(owner);
        }
    } release{*m_files, files_owner};

    std::shared_ptr<Recording> previous;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_rec && !m_rec->sink->finalized())
            throw ToolFailure("A recording is already running (" + path_to_utf8(m_rec->options.path) + "); stop_recording ends it");
        previous = m_rec;
    }
    if (previous) {
        // One whose sink ended on its own (write error, size limit) before its timer saw it ends here, as the
        // timer would end it: its `ended` runs and the REC pill goes, even if this start fails.
        end_recording(previous, "");
        cleanup_ui(previous);
    }

    auto rec         = std::make_shared<Recording>();
    rec->options     = options;
    rec->files_owner = files_owner;
    rec->started     = Clock::now();
    rec->windows     = open_capture_sessions();
    CaptureOptions capture_options;
    capture_options.hide_covering = false;
    capture_options.sessions      = rec->windows.get();
    capture_options.canvas_frame  = [this, raw = rec.get()](GLCanvas3D& canvas, wxImage& out, std::string& err) {
        return canvas_frame(*raw, canvas, out, err);
    };
    wxImage     first;
    std::string err;
    if (!capture_area(m_service, *rec, capture_options, first, err))
        throw ToolFailure("Cannot record: " + err);
    std::tie(rec->video_w, rec->video_h) = video_size(first.GetWidth(), first.GetHeight(), options.max_dim);

    auto output = std::make_unique<MkvOutput>();
    if (!output->open(path_to_utf8(options.path), rec->video_w, rec->video_h, options.fps, err))
        throw ToolFailure(err);
    const int  w = rec->video_w, h = rec->video_h, quality = options.quality;
    const auto files        = m_files;
    const auto path         = options.path;
    const auto start_failed = std::make_shared<std::atomic<bool>>(false);
    rec->sink               = std::make_shared<RecordingSink>(
        std::move(output), [w, h, quality](const SinkFrame& f) { return render_frame(f.capture, f.overlay, w, h, quality); }, SinkOptions{},
        // On a sink thread, before wait_finalized() returns: the file is closed, so encodes may read it, and
        // delete it. A start that failed left no recording to encode, frames written or not. The path is
        // released whatever the listing did.
        [files, files_owner, path, start_failed](const SinkResult& r) {
            try {
                if (r.written > 0 && !*start_failed)
                    files->add_finished(path);
            } catch (...) {
                files->release(files_owner);
                throw;
            }
            files->release(files_owner);
        });
    release.armed = false;

    try {
        *m_input = Input();
        capture_frame(*rec, rec->started, &first);
        if (!m_timer) {
            m_timer = std::make_unique<wxTimer>();
            m_timer->Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
                if (std::shared_ptr<Recording> r = m_ticking.lock())
                    tick(r);
            });
        }
        m_ticking = rec;
        m_timer->StartOnce(std::max(1, 1000 / options.fps));
        if (options.pointer && !m_filter_installed) {
            wxEvtHandler::AddFilter(m_filter.get());
            m_filter_installed = true;
        }
        // Published under the mutex end_owned_by() takes: either the script that started it ended first,
        // and the start is refused, or the script's end finds it running and ends it.
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!options.owner.empty() && m_ended_owners.count(options.owner) != 0)
            throw ToolFailure("The script that started the recording has ended");
        m_rec = rec;
        m_captioned.clear();
    } catch (...) {
        // Nothing of it runs: the timer and the input filter stop, and the sink closes what it has and
        // releases the path.
        cleanup_ui(rec);
        *start_failed = true;
        rec->sink->abort("start_failed");
        throw;
    }
    update_indicator();

    json out{{"path", path_to_utf8(options.path)}, {"target", options.target}, {"width", w}, {"height", h}, {"fps", options.fps},
             {"captions", options.captions}, {"pointer", options.pointer}};
    if (!rec->note.empty())
        out["note"] = rec->note;
    return out;
}

void McpRecorder::tick(const std::shared_ptr<Recording>& rec)
{
    if (rec->ending)
        return;
    // Ended on its own (write error, size limit): the sink has its reason.
    if (rec->sink->ending()) {
        end_recording(rec, "");
        return;
    }
    const Clock::time_point now = Clock::now();
    if (now - rec->started >= std::chrono::seconds(rec->options.max_seconds)) {
        end_recording(rec, "max_seconds");
        return;
    }
    capture_frame(*rec, now);
    const auto spent = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - now);
    ++rec->captures;
    rec->capture_us += uint64_t(spent.count());
    // At most about a third of the UI thread, however slow the capture is here.
    m_timer->StartOnce(std::max(1000 / rec->options.fps, int(3 * spent.count() / 1000)));
}

void McpRecorder::capture_frame(Recording& rec, Clock::time_point now, const wxImage* captured)
{
    wxImage image;
    if (captured != nullptr) {
        image = *captured;
    } else {
        CaptureOptions options;
        options.hide_covering = false;
        options.canvas_frame  = [this, &rec](GLCanvas3D& canvas, wxImage& out, std::string& err) { return canvas_frame(rec, canvas, out, err); };
        options.sessions      = rec.windows.get();
        std::string err;
        if (!capture_area(m_service, rec, options, image, err)) {
            ++rec.skipped;
            return;
        }
    }

    OverlaySnapshot overlay;
    const wxSize    size = image.GetSize();
    auto to_capture = [&](const wxPoint& screen) { return std::make_pair((screen.x - rec.origin.x) * rec.scale, (screen.y - rec.origin.y) * rec.scale); };
    auto inside     = [&](double x, double y) { return x >= 0 && y >= 0 && x < size.x && y < size.y; };
    const FrameTransform t = FrameTransform::fit(size.x, size.y, rec.video_w, rec.video_h);
    overlay.unit           = overlay_scale(wxGetApp().em_unit(), rec.scale, t.scale);
    if (rec.options.pointer) {
        Input& in = *m_input;
        if (in.pointer_known) {
            const auto [x, y]     = to_capture(in.pointer);
            const double alpha    = fade(now - in.pointer_at, kPointerLinger, kPointerFade);
            overlay.pointer       = alpha > 0. && inside(x, y);
            overlay.pointer_x     = x;
            overlay.pointer_y     = y;
            overlay.pointer_alpha = alpha;
        }
        while (!in.ripples.empty() && now - in.ripples.front().when >= kRipple)
            in.ripples.pop_front();
        for (const Input::Press& p : in.ripples) {
            const auto [x, y] = to_capture(p.at);
            if (inside(x, y))
                overlay.ripples.push_back({x, y, seconds(now - p.when) / seconds(kRipple), p.button});
        }
    }
    std::optional<CaptionBoard::Line> line;
    if (rec.options.captions) {
        std::lock_guard<std::mutex> lock(m_mutex);
        line = rec.board.current(now);
    }
    if (line)
        overlay.caption = caption_overlay(rec, *line, overlay.unit);

    // A frame like the last one queued, under the same still overlay, adds nothing until the heartbeat.
    const auto same_pixels = [&] {
        return rec.previous.IsOk() && rec.previous.GetSize() == size &&
               (rec.previous.GetData() == image.GetData() ||
                std::memcmp(rec.previous.GetData(), image.GetData(), std::size_t(size.x) * std::size_t(size.y) * 3) == 0);
    };
    if (captured == nullptr && repeats_last_frame(overlay, rec.previous_overlay, now - rec.last_queued, kHeartbeat, same_pixels))
        return;
    SinkFrame frame;
    frame.ms      = rec.elapsed_ms(now);
    frame.capture = RgbImage(size.x, size.y);
    std::memcpy(frame.capture.pixels.data(), image.GetData(), frame.capture.pixels.size());
    frame.overlay = overlay;
    // Only a frame the sink took is the last one queued: one dropped for the memory budget is tried again.
    if (rec.sink->push(std::move(frame))) {
        rec.previous         = image;
        rec.previous_overlay = std::move(overlay);
        rec.last_queued      = now;
    }
}

void McpRecorder::capture_now()
{
    std::shared_ptr<Recording> rec;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        rec = m_rec;
    }
    if (rec && !rec->ending && !rec->sink->ending())
        capture_frame(*rec, Clock::now());
}

void McpRecorder::end_recording(const std::shared_ptr<Recording>& rec, const std::string& reason)
{
    if (!rec || rec->ending.exchange(true))
        return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        rec->reason = reason;
    }
    rec->sink->finish(rec->elapsed_ms(Clock::now()), reason);
    const auto ended = [rec, reason](McpRecorder& recorder) {
        recorder.cleanup_ui(rec);
        recorder.update_indicator(); // a recording started since then keeps it
        if (rec->options.ended)
            rec->options.ended(reason);
    };
    if (wxThread::IsMain())
        ended(*this);
    else
        // Found again on the UI thread: the service may be gone by then.
        m_service.post_ui([ended] {
            if (McpService* service = wxGetApp().mcp_service(); service != nullptr)
                ended(service->recorder());
        });
}

void McpRecorder::end(const std::string& reason)
{
    std::shared_ptr<Recording> rec;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        rec = m_rec;
    }
    end_recording(rec, reason);
}

void McpRecorder::owner_ended(const std::string& owner)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_ended_owners.insert(owner);
}

bool McpRecorder::owner_ended_in(const std::vector<std::string>& ancestry) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::any_of(ancestry.begin(), ancestry.end(), [&](const std::string& job) { return m_ended_owners.count(job) != 0; });
}

std::optional<std::string> McpRecorder::end_owned_by(const std::string& owner, const std::string& reason, bool wait)
{
    std::shared_ptr<Recording> rec;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        // A start of this script's still waiting for the UI thread must not begin after this (start()).
        if (!owner.empty())
            m_ended_owners.insert(owner);
        if (!m_rec || m_rec->options.owner != owner)
            return std::nullopt;
        rec = m_rec;
    }
    end_recording(rec, reason);
    if (wait && !rec->sink->wait_finalized(std::chrono::seconds(30)))
        return path_to_utf8(rec->options.path);
    return std::nullopt;
}

void McpRecorder::cleanup_ui(const std::shared_ptr<Recording>& rec)
{
    // Stopped, not deleted: this may run inside the timer's own handler.
    if (m_timer && m_ticking.lock() == rec)
        m_timer->Stop();
    bool input_left = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        input_left = m_rec && m_rec != rec && !m_rec->ending && m_rec->options.pointer;
    }
    if (m_filter_installed && !input_left) {
        wxEvtHandler::RemoveFilter(m_filter.get());
        m_filter_installed = false;
    }
    rec->windows.reset();
    rec->previous = wxImage();
    rec->canvases.clear();
    rec->masks.clear();
    rec->caption_font = wxFont();
}

json McpRecorder::report(Recording& rec) const
{
    const SinkResult r       = rec.sink->result();
    const double     seconds = r.end_ms / 1000.;
    std::string      ended   = r.ended;
    if (ended.empty()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        ended = rec.reason;
    }
    json out{{"path", path_to_utf8(rec.options.path)},
             {"seconds", std::round(seconds * 10.) / 10.},
             {"frames", r.accepted},
             {"written", r.written},
             {"dropped", r.dropped},
             {"skipped", rec.skipped.load()},
             {"width", rec.video_w},
             {"height", rec.video_h},
             {"size_bytes", r.bytes},
             // Distinct frames: a still window adds none between heartbeats.
             {"effective_fps", std::round(frames_per_second(r.written, seconds) * 10.) / 10.},
             {"capture_ms", rec.captures > 0 ? std::round(rec.capture_us / 100. / rec.captures) / 10. : 0.},
             {"ended", ended}};
    if (!r.error.empty())
        out["error"] = r.error;
    if (!rec.note.empty())
        out["note"] = rec.note;
    return out;
}

json McpRecorder::stop_and_report(std::chrono::milliseconds timeout, json& encode)
{
    std::shared_ptr<Recording> rec;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_rec || m_rec->reported)
            return nullptr;
        rec = m_rec;
    }
    end_recording(rec, "stopped");
    if (!rec->sink->wait_finalized(timeout))
        throw ToolFailure("The recording is still being written to " + path_to_utf8(rec->options.path) + "; call stop_recording again");
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        rec->reported = true;
    }
    encode = rec->options.encode;
    return report(*rec);
}

json McpRecorder::state() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_rec || m_rec->sink->finalized())
        return nullptr;
    json out{{"path", path_to_utf8(m_rec->options.path)},
             {"target", m_rec->options.target},
             {"seconds", std::round(seconds(Clock::now() - m_rec->started) * 10.) / 10.},
             {"frames", m_rec->sink->result().accepted}};
    if (m_rec->ending)
        out["ending"] = true;
    return out;
}

bool McpRecorder::active() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_rec && !m_rec->sink->finalized();
}

bool McpRecorder::wait_closed(const fs::path& path, std::chrono::milliseconds timeout) const
{
    std::shared_ptr<Recording> rec;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        rec = m_rec;
    }
    return !rec || rec->options.path != path || rec->sink->wait_finalized(timeout);
}

void McpRecorder::suspend()
{
    std::shared_ptr<Recording> rec;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        rec = m_rec;
    }
    if (rec)
        rec->canvases.clear();
}

void McpRecorder::resume() { update_indicator(); }

bool McpRecorder::running() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_rec && !m_rec->ending;
}

void McpRecorder::update_indicator() const
{
    if (wxGetApp().is_closing() || m_service.suspended())
        return;
    if (MainFrame* frame = wxGetApp().mainframe; frame != nullptr)
        frame->show_recording_indicator(running());
}

void McpRecorder::shutdown(std::chrono::milliseconds timeout)
{
    std::shared_ptr<Recording> rec;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        rec = m_rec;
    }
    if (!rec)
        return;
    end_recording(rec, "app_closing");
    // The sink's threads finish on their own; past the wait, what is still queued is dropped.
    if (!rec->sink->wait_finalized(timeout))
        rec->sink->abort("app_closing");
}

void McpRecorder::call_started(uint64_t call, const ToolDef& def, const json& args, const std::string& /*parent*/)
{
    // Calls that act on the app; the inspect ones, recording tools included, only look.
    if (def.guard == GuardClass::Inspect)
        return;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_rec || m_rec->ending || !m_rec->options.captions)
        return;
    m_rec->board.call_started(call, def.title, safe_caption_detail(def.name, args), Clock::now());
    m_captioned[call] = {def.name, {args}};
}

void McpRecorder::call_args(uint64_t call, const json& args)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_captioned.find(call);
    if (it == m_captioned.end())
        return;
    it->second.args.push_back(args);
    if (m_rec)
        m_rec->board.call_detail(call, safe_caption_detail(it->second.tool, args));
}

void McpRecorder::call_ended(uint64_t call, const ToolResult& result)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_captioned.find(call);
    if (it == m_captioned.end())
        return;
    std::string error;
    if (result.is_error) {
        // An error may quote an argument: what the caption hides of every version of them stays hidden. Taken
        // out of the whole message before it is shortened, which could cut a value and leave its start.
        error = result.message;
        for (const json& args : it->second.args)
            error = caption_error(it->second.tool, args, error);
        error = shorten(error, 100);
    }
    m_captioned.erase(it);
    if (m_rec)
        m_rec->board.call_ended(call, !result.is_error, error, Clock::now());
}

bool McpRecorder::shows(wxWindow* window) const
{
    if (hidden_from_automation(window))
        return false;
    wxWindow* top = wxGetTopLevelParent(window);
    if (top == nullptr)
        return false;
    if (top == wxGetApp().mainframe)
        return true;
    if (!window_positions_known())
        return false;
    if (capture_reads_screen())
        return top->IsShownOnScreen();
    const auto& stack = m_service.modals().stack();
    return std::find(stack.begin(), stack.end(), top) != stack.end();
}

void McpRecorder::on_event(wxEvent& event)
{
    const wxEventType type   = event.GetEventType();
    const bool        press  = type == wxEVT_LEFT_DOWN || type == wxEVT_RIGHT_DOWN || type == wxEVT_MIDDLE_DOWN ||
                       type == wxEVT_LEFT_DCLICK || type == wxEVT_RIGHT_DCLICK || type == wxEVT_MIDDLE_DCLICK;
    const bool        mouse   = press || type == wxEVT_MOTION || type == wxEVT_MOUSEWHEEL;
    const bool        command = type == wxEVT_BUTTON || type == wxEVT_CHECKBOX || type == wxEVT_TOGGLEBUTTON || type == wxEVT_RADIOBUTTON;
    if ((!mouse && !command) || !wxThread::IsMain())
        return;
    std::shared_ptr<Recording> rec;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        rec = m_rec;
    }
    if (!rec || rec->ending || !rec->options.pointer)
        return;
    auto* window = wxDynamicCast(event.GetEventObject(), wxWindow);
    if (window == nullptr || !shows(window))
        return;
    Input&                  in       = *m_input;
    const Clock::time_point now      = Clock::now();
    auto                    press_at = [&](const wxPoint& screen, PointerButton button) {
        in.pointer_known = true;
        in.pointer       = screen;
        in.pointer_at    = now;
        in.ripples.push_back({screen, now, button});
    };

    if (mouse) {
        const auto&   m      = static_cast<const wxMouseEvent&>(event);
        const wxPoint screen = window->ClientToScreen(m.GetPosition());
        in.pointer_known     = true;
        in.pointer           = screen;
        in.pointer_at        = now;
        if (press) {
            press_at(screen, type == wxEVT_RIGHT_DOWN || type == wxEVT_RIGHT_DCLICK   ? PointerButton::Right :
                             type == wxEVT_MIDDLE_DOWN || type == wxEVT_MIDDLE_DCLICK ? PointerButton::Middle :
                                                                                        PointerButton::Left);
            in.pressed_window = window;
            in.pressed_at     = now;
        }
        return;
    }

    // A real click sent its press first; a command alone is a click made without the mouse (ui_click).
    wxWindow* pressed = in.pressed_window.get();
    if (pressed != nullptr && now - in.pressed_at < kSemanticClick && (pressed == window || window->IsDescendant(pressed)))
        return;
    const wxRect r = window->GetScreenRect();
    press_at(wxPoint(r.x + r.width / 2, r.y + r.height / 2), PointerButton::Left);
}

}}} // namespace Slic3r::GUI::Mcp
