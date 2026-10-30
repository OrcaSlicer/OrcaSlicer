#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

// What a recording draws over its frames, and the frame work its encoder threads do: scaling to the
// video size, the overlays and JPEG. Everything here is wx-free, so encoder threads use nothing wx
// owns and the drawing is unit-testable.

// Tightly packed 8-bit RGB.
struct RgbImage
{
    int                  width  = 0;
    int                  height = 0;
    std::vector<uint8_t> pixels;

    RgbImage() = default;
    RgbImage(int w, int h) : width(w), height(h), pixels(std::size_t(w) * std::size_t(h) * 3, 0) {}
    uint8_t*       at(int x, int y) { return pixels.data() + (std::size_t(y) * std::size_t(width) + std::size_t(x)) * 3; }
    const uint8_t* at(int x, int y) const { return pixels.data() + (std::size_t(y) * std::size_t(width) + std::size_t(x)) * 3; }
};

// Coverage of rasterized text, 0..255 per pixel.
struct TextMask
{
    int                  width  = 0;
    int                  height = 0;
    std::vector<uint8_t> coverage;
};

struct Rgb
{
    uint8_t r = 0, g = 0, b = 0;
};

// Capture pixels to video pixels: the capture scaled to fit the video and centred in it, the rest
// black (letterboxed). One transform serves the image and every position drawn over it.
struct FrameTransform
{
    double scale    = 1.;
    double offset_x = 0.;
    double offset_y = 0.;

    static FrameTransform fit(int src_w, int src_h, int dst_w, int dst_h);
    double x(double capture_x) const { return capture_x * scale + offset_x; }
    double y(double capture_y) const { return capture_y * scale + offset_y; }
};

// The video size for a capture: scaled to fit `max_dim` on its long edge, rounded down to even numbers
// (H.264/H.265 in 4:2:0 need them), at least 2x2.
std::pair<int, int> video_size(int capture_w, int capture_h, int max_dim);

// `src` drawn into `dst` through `t` (bilinear), black where it does not reach.
void scale_into(const RgbImage& src, RgbImage& dst, const FrameTransform& t);

// The overlays' unit: video pixels per pixel of the app at 100 % scaling, from the app's em unit (10 at
// 100 %), the captured window's physical pixels per wx coordinate, and the capture-to-video transform's
// scale. At least 0.75, so a small video's caption stays readable.
double overlay_scale(int em_unit, double content_scale, double transform_scale);

// The width each span of a caption line may take, 0 for one left out. `widths` are the spans' natural
// widths in the order they are drawn, `priority` the order they are given room: the first with a width
// always gets it, cut to `budget`; each next one gets what is left after `gap`, cut to it, and is left out
// when that is under `min_width` and it doesn't fit whole. An empty span takes no room and no gap.
std::vector<int> fit_spans(const std::vector<int>& widths, const std::vector<std::size_t>& priority, int budget, int gap, int min_width);

// Anti-aliased shapes blended over `img` with opacity `alpha` (0..1); everything is clipped to the image.
void fill_round_rect(RgbImage& img, double x, double y, double w, double h, double radius, Rgb color, double alpha);
void fill_circle(RgbImage& img, double cx, double cy, double radius, Rgb color, double alpha);
void stroke_circle(RgbImage& img, double cx, double cy, double radius, double thickness, Rgb color, double alpha);
void blend_mask(RgbImage& img, const TextMask& mask, int x, int y, Rgb color, double alpha);
// The usual pointer arrow, white with a black outline, its tip at (x, y), `size` pixels tall.
void draw_arrow(RgbImage& img, double x, double y, double size, double alpha);

enum class CaptionStatus { Running, Ok, Failed };

// What a piece of a caption says, which sets its colour.
enum class SpanRole { Title, Detail, Error };

struct OverlaySpan
{
    std::shared_ptr<const TextMask> text;
    SpanRole                        role = SpanRole::Detail;

    bool operator==(const OverlaySpan& o) const { return text == o.text && role == o.role; }
};

// A tool call's caption: its title, detail and error, fitted to the caption's width already.
struct OverlayCaption
{
    std::vector<OverlaySpan> spans;
    CaptionStatus            status = CaptionStatus::Running;
    double                   alpha  = 1.;

    bool operator==(const OverlayCaption& o) const { return spans == o.spans && status == o.status && alpha == o.alpha; }
};

enum class PointerButton { Left, Right, Middle };

struct Ripple
{
    double        x = 0., y = 0.;   // capture pixels
    double        progress = 0.;    // 0 when the press happened .. 1 when the ripple is gone
    PointerButton button   = PointerButton::Left;

    bool operator==(const Ripple& o) const { return x == o.x && y == o.y && progress == o.progress && button == o.button; }
};

// Everything a frame shows over the captured pixels, as of the tick that captured it.
struct OverlaySnapshot
{
    double                        unit = 1.;          // overlay_scale()
    bool                          pointer = false;    // drawn at all
    double                        pointer_x = 0., pointer_y = 0.; // capture pixels
    double                        pointer_alpha = 1.;
    std::vector<Ripple>           ripples;
    std::optional<OverlayCaption> caption;            // bottom centre

    // A ripple in progress, or the caption or the pointer part way through fading: the frame changes
    // with time even when nothing else does.
    bool animating() const;
    bool operator==(const OverlaySnapshot& o) const;
    bool operator!=(const OverlaySnapshot& o) const { return !(*this == o); }
};

// Whether a captured frame adds nothing to the recording: its overlay is the last queued frame's and
// still, the heartbeat is not due, and its pixels are the last frame's (`same_pixels`, asked last since
// comparing them costs most).
bool repeats_last_frame(const OverlaySnapshot& overlay, const OverlaySnapshot& last, std::chrono::steady_clock::duration since_last,
                        std::chrono::steady_clock::duration heartbeat, const std::function<bool()>& same_pixels);

// Draws `overlay` over `video`, positions mapped through `t`, sizes in its unit.
void draw_overlay(RgbImage& video, const OverlaySnapshot& overlay, const FrameTransform& t);

// The caption's geometry in units of overlay_scale(), shared by the recorder, which fits the text to the
// width, and the encoder, which draws it.
struct CaptionMetrics
{
    static constexpr double kMargin  = 12.; // from the video's bottom edge, and the least from its sides
    static constexpr double kPadX    = 8.;
    static constexpr double kPadY    = 4.;
    static constexpr double kRadius  = 5.;
    static constexpr double kMark    = 3.;  // the status mark's radius
    static constexpr double kMarkGap = 6.;  // between the mark and the text
    static constexpr double kGap     = 5.;  // between spans
    // The room for the spans in a video `video_w` wide: the caption is at most 40 % of the width or 240
    // units, and never wider than the video less its margins.
    static double text_budget(int video_w, double unit);
};

// JPEG of `rgb` (rows `stride` bytes apart) into a buffer libjpeg allocates, which the caller frees with
// free(). libjpeg's errors end only this call: it returns false and leaves nothing allocated.
bool encode_jpeg(const uint8_t* rgb, int width, int height, int stride, int quality, unsigned char** out, unsigned long* size);

// The work an encoder thread does for a frame: scale the capture to `video_w` x `video_h`, draw the
// overlay, encode. Re-entrant: it reads nothing but its arguments. Empty when the encode failed.
std::vector<uint8_t> render_frame(const RgbImage& capture, const OverlaySnapshot& overlay, int video_w, int video_h, int quality);

// The caption a recording shows: the current tool call's. Of the calls that run or linger (kLinger after
// they end, then fading over kFade), the one started last, so a newer call replaces it at once and an
// older one still running comes back when the newer has faded. Not thread-safe; times are passed in, so it
// is testable.
class CaptionBoard
{
public:
    using Clock = std::chrono::steady_clock;

    static constexpr auto kLinger = std::chrono::milliseconds(4000); // after a call ends
    static constexpr auto kFade   = std::chrono::milliseconds(500);

    void call_started(uint64_t id, std::string title, std::string detail, Clock::time_point now);
    void call_detail(uint64_t id, std::string detail);
    void call_ended(uint64_t id, bool ok, std::string error, Clock::time_point now);

    struct Line
    {
        uint64_t      id = 0;
        std::string   title; // untranslated tool title
        std::string   detail;
        std::string   error; // a failed call's
        CaptionStatus status = CaptionStatus::Running;
        double        alpha  = 1.;
    };
    // The current call's caption, if any. Drops the calls that neither run nor linger any more.
    std::optional<Line> current(Clock::time_point now);

private:
    struct Call
    {
        uint64_t          id = 0;
        std::string       title, detail, error;
        CaptionStatus     status = CaptionStatus::Running;
        Clock::time_point ended;
    };
    std::deque<Call> m_calls; // oldest started first
};

// Whether a setting's value must not be shown: its key names a password, API key, access code, token
// or secret (printhost_apikey and printhost_password are plain text fields).
bool is_sensitive_key(const std::string& key);

// What a tool call acts on, worded for its caption from its arguments: a GUI tool's target, a
// setting field's value when it is not sensitive, a key chord, files, settings, a command. Values of
// sensitive settings show as "•••"; typed text and the value of a field reached any other way than by
// its setting key never show. Empty when there is nothing to add to the tool's title.
std::string caption_detail(const std::string& tool, const nlohmann::json& args);

// A failed call's error for its caption. An error may quote an argument, so every value caption_detail()
// hides for `tool` and `args` is taken out of it, as it is and as JSON escapes it inside a quoted string, and
// shows as "•••"; when such a value is too short to take out safely (under three characters), the error is
// left out altogether. Give it the whole error, and shorten() it afterwards.
std::string caption_error(const std::string& tool, const nlohmann::json& args, const std::string& error);

// The first line of `text`, cut to at most `limit` bytes at a character boundary and marked "..." when cut.
std::string shorten(const std::string& text, std::size_t limit);

}}} // namespace Slic3r::GUI::Mcp
