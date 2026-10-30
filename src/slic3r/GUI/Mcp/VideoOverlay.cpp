#include "VideoOverlay.hpp"

#include "McpScriptSummary.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <jpeglib.h>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

FrameTransform FrameTransform::fit(int src_w, int src_h, int dst_w, int dst_h)
{
    FrameTransform t;
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return t;
    t.scale    = std::min(double(dst_w) / src_w, double(dst_h) / src_h);
    t.offset_x = (dst_w - src_w * t.scale) / 2.;
    t.offset_y = (dst_h - src_h * t.scale) / 2.;
    return t;
}

std::pair<int, int> video_size(int capture_w, int capture_h, int max_dim)
{
    double scale = 1.;
    if (const int longest = std::max(capture_w, capture_h); longest > max_dim && longest > 0)
        scale = double(max_dim) / longest;
    const int w = std::max(2, int(capture_w * scale) & ~1);
    const int h = std::max(2, int(capture_h * scale) & ~1);
    return {w, h};
}

void scale_into(const RgbImage& src, RgbImage& dst, const FrameTransform& t)
{
    std::fill(dst.pixels.begin(), dst.pixels.end(), uint8_t(0));
    if (src.width <= 0 || src.height <= 0 || dst.width <= 0 || dst.height <= 0 || t.scale <= 0.)
        return;
    const int x0 = std::max(0, int(std::ceil(t.offset_x - 1e-9)));
    const int y0 = std::max(0, int(std::ceil(t.offset_y - 1e-9)));
    const int x1 = std::min(dst.width, int(std::floor(t.offset_x + src.width * t.scale + 1e-9)));
    const int y1 = std::min(dst.height, int(std::floor(t.offset_y + src.height * t.scale + 1e-9)));
    if (x0 >= x1 || y0 >= y1)
        return;
    // A capture the size of the video, at a whole-pixel offset, is copied as it is.
    if (t.scale == 1. && t.offset_x == double(x0) && t.offset_y == double(y0)) {
        for (int y = y0; y < y1; ++y)
            std::copy_n(src.at(0, y - y0), std::size_t(x1 - x0) * 3, dst.at(x0, y));
        return;
    }
    // Bilinear: per column, the two source columns and the weight of the second.
    struct Tap { int a, b; float w; };
    auto taps = [&](int from, int to, double offset, int limit) {
        std::vector<Tap> out;
        out.reserve(std::size_t(to - from));
        for (int d = from; d < to; ++d) {
            const double s  = std::clamp((d + 0.5 - offset) / t.scale - 0.5, 0., double(limit - 1));
            const int    a  = int(s);
            out.push_back({a, std::min(a + 1, limit - 1), float(s - a)});
        }
        return out;
    };
    const std::vector<Tap> xs = taps(x0, x1, t.offset_x, src.width);
    const std::vector<Tap> ys = taps(y0, y1, t.offset_y, src.height);
    for (int y = y0; y < y1; ++y) {
        const Tap&     ty   = ys[std::size_t(y - y0)];
        const uint8_t* row0 = src.at(0, ty.a);
        const uint8_t* row1 = src.at(0, ty.b);
        uint8_t*       out  = dst.at(x0, y);
        for (const Tap& tx : xs) {
            const uint8_t* p00 = row0 + std::size_t(tx.a) * 3;
            const uint8_t* p01 = row0 + std::size_t(tx.b) * 3;
            const uint8_t* p10 = row1 + std::size_t(tx.a) * 3;
            const uint8_t* p11 = row1 + std::size_t(tx.b) * 3;
            for (int c = 0; c < 3; ++c) {
                const float top    = p00[c] + (p01[c] - p00[c]) * tx.w;
                const float bottom = p10[c] + (p11[c] - p10[c]) * tx.w;
                *out++             = uint8_t(top + (bottom - top) * ty.w + 0.5f);
            }
        }
    }
}

double overlay_scale(int em_unit, double content_scale, double transform_scale)
{
    return std::max(0.75, em_unit / 10. * content_scale * transform_scale);
}

std::vector<int> fit_spans(const std::vector<int>& widths, const std::vector<std::size_t>& priority, int budget, int gap, int min_width)
{
    std::vector<int> out(widths.size(), 0);
    int              left = budget;
    for (std::size_t i : priority) {
        if (i >= widths.size() || widths[i] <= 0)
            continue;
        const bool first = left == budget;
        const int  room  = first ? left : left - gap;
        if (room <= 0 || (!first && widths[i] > room && room < min_width))
            continue;
        out[i] = std::min(widths[i], room);
        left   = room - out[i];
    }
    return out;
}

namespace {

void blend(uint8_t* p, Rgb color, double a)
{
    if (a <= 0.)
        return;
    a    = std::min(a, 1.);
    p[0] = uint8_t(p[0] + (color.r - p[0]) * a + 0.5);
    p[1] = uint8_t(p[1] + (color.g - p[1]) * a + 0.5);
    p[2] = uint8_t(p[2] + (color.b - p[2]) * a + 0.5);
}

// Runs `coverage(px, py)` (pixel centre) over the pixels of a box, clipped to the image, and blends
// `color` by what it returns times `alpha`.
template<class Coverage> void shade(RgbImage& img, double x0, double y0, double x1, double y1, Rgb color, double alpha, Coverage coverage)
{
    if (alpha <= 0.)
        return;
    const int ix0 = std::max(0, int(std::floor(x0))), iy0 = std::max(0, int(std::floor(y0)));
    const int ix1 = std::min(img.width, int(std::ceil(x1))), iy1 = std::min(img.height, int(std::ceil(y1)));
    for (int y = iy0; y < iy1; ++y)
        for (int x = ix0; x < ix1; ++x)
            if (const double c = coverage(x + 0.5, y + 0.5); c > 0.)
                blend(img.at(x, y), color, alpha * std::min(c, 1.));
}

double edge(double distance) { return std::clamp(0.5 - distance, 0., 1.); }

// The pointer arrow as a 19-pixel-tall cursor's outline, tip at the origin.
constexpr std::array<std::pair<double, double>, 7> kArrow{{{0., 0.}, {0., 16.}, {4., 12.}, {7., 18.}, {9., 17.}, {6., 11.}, {11., 11.}}};
constexpr double kArrowHeight = 19.;

bool inside_polygon(double x, double y, double scale)
{
    bool in = false;
    for (std::size_t i = 0, j = kArrow.size() - 1; i < kArrow.size(); j = i++) {
        const double xi = kArrow[i].first * scale, yi = kArrow[i].second * scale;
        const double xj = kArrow[j].first * scale, yj = kArrow[j].second * scale;
        if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi)
            in = !in;
    }
    return in;
}

double distance_to_outline(double x, double y, double scale)
{
    double best = 1e9;
    for (std::size_t i = 0, j = kArrow.size() - 1; i < kArrow.size(); j = i++) {
        const double ax = kArrow[j].first * scale, ay = kArrow[j].second * scale;
        const double bx = kArrow[i].first * scale, by = kArrow[i].second * scale;
        const double dx = bx - ax, dy = by - ay;
        const double len2 = dx * dx + dy * dy;
        const double t    = len2 > 0. ? std::clamp(((x - ax) * dx + (y - ay) * dy) / len2, 0., 1.) : 0.;
        best              = std::min(best, std::hypot(x - (ax + t * dx), y - (ay + t * dy)));
    }
    return best;
}

// The overlays' palette: a charcoal plate with the teal cast of Orca's UI, light text in two strengths, Orca's
// teal lifted to read on the plate, and an amber pointer that shows on the light UI and the dark bed alike.
constexpr Rgb kInk{15, 26, 25};
constexpr Rgb kPaper{244, 247, 246};
constexpr Rgb kFog{163, 181, 178};
constexpr Rgb kTeal{31, 184, 159};
constexpr Rgb kCoral{255, 107, 94};
constexpr Rgb kErrorText{255, 194, 188};
constexpr Rgb kAmber{255, 178, 36};

Rgb ripple_color(PointerButton b)
{
    switch (b) {
    case PointerButton::Right: return {77, 163, 255};
    case PointerButton::Middle: return {169, 139, 255};
    case PointerButton::Left: break;
    }
    return kAmber;
}

} // namespace

void fill_round_rect(RgbImage& img, double x, double y, double w, double h, double radius, Rgb color, double alpha)
{
    radius            = std::clamp(radius, 0., std::min(w, h) / 2.);
    const double cx   = x + w / 2., cy = y + h / 2.;
    const double hx   = w / 2. - radius, hy = h / 2. - radius;
    shade(img, x, y, x + w, y + h, color, alpha, [&](double px, double py) {
        const double qx = std::abs(px - cx) - hx, qy = std::abs(py - cy) - hy;
        const double d  = std::hypot(std::max(qx, 0.), std::max(qy, 0.)) + std::min(std::max(qx, qy), 0.) - radius;
        return edge(d);
    });
}

void fill_circle(RgbImage& img, double cx, double cy, double radius, Rgb color, double alpha)
{
    shade(img, cx - radius - 1, cy - radius - 1, cx + radius + 1, cy + radius + 1, color, alpha,
          [&](double px, double py) { return edge(std::hypot(px - cx, py - cy) - radius); });
}

void stroke_circle(RgbImage& img, double cx, double cy, double radius, double thickness, Rgb color, double alpha)
{
    const double outer = radius + thickness / 2. + 1;
    shade(img, cx - outer, cy - outer, cx + outer, cy + outer, color, alpha,
          [&](double px, double py) { return edge(std::abs(std::hypot(px - cx, py - cy) - radius) - thickness / 2.); });
}

void blend_mask(RgbImage& img, const TextMask& mask, int x, int y, Rgb color, double alpha)
{
    for (int my = 0; my < mask.height; ++my) {
        const int iy = y + my;
        if (iy < 0 || iy >= img.height)
            continue;
        for (int mx = 0; mx < mask.width; ++mx) {
            const int ix = x + mx;
            if (ix < 0 || ix >= img.width)
                continue;
            if (const uint8_t c = mask.coverage[std::size_t(my) * std::size_t(mask.width) + std::size_t(mx)]; c != 0)
                blend(img.at(ix, iy), color, alpha * c / 255.);
        }
    }
}

void draw_arrow(RgbImage& img, double x, double y, double size, double alpha)
{
    if (alpha <= 0. || size <= 0.)
        return;
    const double scale  = size / kArrowHeight;
    const double border = std::max(1., 1.2 * scale);
    const int    x0 = std::max(0, int(std::floor(x))), y0 = std::max(0, int(std::floor(y)));
    const int    x1 = std::min(img.width, int(std::ceil(x + 12 * scale)) + 1);
    const int    y1 = std::min(img.height, int(std::ceil(y + size)) + 1);
    // 4x4 samples per pixel: inside the outline and within `border` of it is black, deeper is white.
    for (int py = y0; py < y1; ++py)
        for (int px = x0; px < x1; ++px) {
            int white = 0, black = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    const double lx = px + (sx + 0.5) / 4. - x, ly = py + (sy + 0.5) / 4. - y;
                    if (!inside_polygon(lx, ly, scale))
                        continue;
                    (distance_to_outline(lx, ly, scale) < border ? black : white)++;
                }
            if (white + black == 0)
                continue;
            // The samples' mean colour, over the share of the pixel they cover.
            const uint8_t level = uint8_t(255 * white / (white + black));
            blend(img.at(px, py), {level, level, level}, alpha * (white + black) / 16.);
        }
}

bool OverlaySnapshot::animating() const
{
    auto fading = [](double a) { return a > 0. && a < 1.; };
    return !ripples.empty() || (pointer && fading(pointer_alpha)) || (caption && fading(caption->alpha));
}

bool OverlaySnapshot::operator==(const OverlaySnapshot& o) const
{
    return unit == o.unit && pointer == o.pointer && pointer_x == o.pointer_x && pointer_y == o.pointer_y &&
           pointer_alpha == o.pointer_alpha && ripples == o.ripples && caption == o.caption;
}

bool repeats_last_frame(const OverlaySnapshot& overlay, const OverlaySnapshot& last, std::chrono::steady_clock::duration since_last,
                        std::chrono::steady_clock::duration heartbeat, const std::function<bool()>& same_pixels)
{
    return overlay == last && !overlay.animating() && since_last < heartbeat && same_pixels();
}

double CaptionMetrics::text_budget(int video_w, double unit)
{
    const double plate = std::min(std::max(0.4 * video_w, 240. * unit), video_w - 2. * kMargin * unit);
    return plate - (2. * kPadX + 2. * kMark + kMarkGap) * unit;
}

void draw_overlay(RgbImage& video, const OverlaySnapshot& overlay, const FrameTransform& t)
{
    const double s = overlay.unit;

    // A ring per press, growing (easing out), thinning and fading over the ripple's life.
    for (const Ripple& r : overlay.ripples) {
        const double grown = 1. - (1. - r.progress) * (1. - r.progress);
        stroke_circle(video, t.x(r.x), t.y(r.y), (4. + 14. * grown) * s, (2. - 1.25 * r.progress) * s, ripple_color(r.button),
                      0.9 * (1. - r.progress));
    }
    // A ring with nothing inside, which reads on the light UI and the dark bed: a fill would tint the bed.
    if (overlay.pointer) {
        const double x = t.x(overlay.pointer_x), y = t.y(overlay.pointer_y);
        stroke_circle(video, x, y, 11. * s, 2. * s, kAmber, 0.9 * overlay.pointer_alpha);
        draw_arrow(video, x, y, 19. * s, overlay.pointer_alpha);
    }

    if (!overlay.caption || overlay.caption->alpha <= 0. || overlay.caption->spans.empty())
        return;
    using M                = CaptionMetrics;
    const OverlayCaption& c = *overlay.caption;
    double text_w = 0., text_h = 0.;
    for (const OverlaySpan& span : c.spans)
        if (span.text) {
            text_w += (text_w > 0. ? M::kGap * s : 0.) + span.text->width;
            text_h = std::max(text_h, double(span.text->height));
        }
    const double w = (2. * M::kPadX + 2. * M::kMark + M::kMarkGap) * s + text_w;
    const double h = 2. * M::kPadY * s + text_h;
    const double x = (video.width - w) / 2., y = video.height - M::kMargin * s - h;
    fill_round_rect(video, x, y, w, h, M::kRadius * s, kInk, 0.82 * c.alpha);
    const double mark_x = x + (M::kPadX + M::kMark) * s, mark_y = y + h / 2.;
    if (c.status == CaptionStatus::Running)
        stroke_circle(video, mark_x, mark_y, (M::kMark - 0.625) * s, 1.25 * s, kTeal, c.alpha);
    else
        fill_circle(video, mark_x, mark_y, M::kMark * s, c.status == CaptionStatus::Failed ? kCoral : kTeal, c.alpha);
    // The title is the quiet part when something follows it, and says it all when nothing does.
    double text_x = x + (M::kPadX + 2. * M::kMark + M::kMarkGap) * s;
    for (std::size_t i = 0; i < c.spans.size(); ++i) {
        const OverlaySpan& span = c.spans[i];
        if (!span.text)
            continue;
        const Rgb color = span.role == SpanRole::Error ? kErrorText :
                          span.role == SpanRole::Title && i + 1 < c.spans.size() ? kFog :
                                                                                   kPaper;
        blend_mask(video, *span.text, int(std::lround(text_x)), int(std::lround(y + M::kPadY * s)), color, c.alpha);
        text_x += span.text->width + M::kGap * s;
    }
}

namespace {

// libjpeg's state for one encode, on the heap: after its error handler longjmps back, only memory the
// jump did not unwind is read, so nothing hangs on locals changed after setjmp.
struct JpegJob
{
    jpeg_compress_struct info;
    jpeg_error_mgr       err;
    std::jmp_buf         jump;
    unsigned char*       buffer;
    unsigned long        size;
};

// Replaces libjpeg's default, which prints and calls exit(): back to encode_jpeg(), which cleans up.
void jpeg_job_error_exit(j_common_ptr cinfo)
{
    JpegJob* job = reinterpret_cast<JpegJob*>(reinterpret_cast<char*>(cinfo->err) - offsetof(JpegJob, err));
    std::longjmp(job->jump, 1);
}

void jpeg_job_silent(j_common_ptr) {}

} // namespace

bool encode_jpeg(const uint8_t* rgb, int width, int height, int stride, int quality, unsigned char** out, unsigned long* size)
{
    if (rgb == nullptr || out == nullptr || size == nullptr || width <= 0 || height <= 0)
        return false;
    *out  = nullptr;
    *size = 0;
    // C-style on purpose: this frame holds no object with a destructor for the longjmp to skip.
    JpegJob* job = static_cast<JpegJob*>(std::calloc(1, sizeof(JpegJob)));
    if (job == nullptr)
        return false;
    job->info.err              = jpeg_std_error(&job->err);
    job->err.error_exit        = jpeg_job_error_exit;
    job->err.output_message    = jpeg_job_silent;
    if (setjmp(job->jump) != 0) {
        jpeg_destroy_compress(&job->info);
        std::free(job->buffer);
        std::free(job);
        return false;
    }
    jpeg_create_compress(&job->info);
    jpeg_mem_dest(&job->info, &job->buffer, &job->size);
    job->info.image_width      = JDIMENSION(width);
    job->info.image_height     = JDIMENSION(height);
    job->info.input_components = 3;
    job->info.in_color_space   = JCS_RGB;
    jpeg_set_defaults(&job->info);
    jpeg_set_quality(&job->info, quality, TRUE);
    jpeg_start_compress(&job->info, TRUE);
    for (int y = 0; y < height; ++y) {
        JSAMPROW row = const_cast<JSAMPROW>(rgb + std::size_t(y) * std::size_t(stride));
        jpeg_write_scanlines(&job->info, &row, 1);
    }
    jpeg_finish_compress(&job->info);
    jpeg_destroy_compress(&job->info);
    *out  = job->buffer;
    *size = job->size;
    std::free(job);
    return true;
}

std::vector<uint8_t> render_frame(const RgbImage& capture, const OverlaySnapshot& overlay, int video_w, int video_h, int quality)
{
    RgbImage             video(video_w, video_h);
    const FrameTransform t = FrameTransform::fit(capture.width, capture.height, video_w, video_h);
    scale_into(capture, video, t);
    draw_overlay(video, overlay, t);
    unsigned char* out  = nullptr;
    unsigned long  size = 0;
    if (!encode_jpeg(video.pixels.data(), video_w, video_h, video_w * 3, quality, &out, &size))
        return {};
    std::vector<uint8_t> jpeg(out, out + size);
    std::free(out);
    return jpeg;
}

// ---- captions -----------------------------------------------------------------------------------

namespace {

double fade(CaptionBoard::Clock::duration since, CaptionBoard::Clock::duration linger)
{
    if (since <= linger)
        return 1.;
    const double f = std::chrono::duration<double>(since - linger) / std::chrono::duration<double>(CaptionBoard::kFade);
    return std::max(0., 1. - f);
}

} // namespace

void CaptionBoard::call_started(uint64_t id, std::string title, std::string detail, Clock::time_point)
{
    Call c;
    c.id     = id;
    c.title  = std::move(title);
    c.detail = std::move(detail);
    m_calls.push_back(std::move(c));
    // A burst of quick calls, all lingering at once: the oldest finished ones go. Running calls stay, so a
    // script still running shows again once its steps have faded; they are few (a script and its step).
    while (m_calls.size() > 64) {
        const auto finished = std::find_if(m_calls.begin(), m_calls.end(), [](const Call& x) { return x.status != CaptionStatus::Running; });
        if (finished == m_calls.end())
            break;
        m_calls.erase(finished);
    }
}

void CaptionBoard::call_detail(uint64_t id, std::string detail)
{
    for (Call& c : m_calls)
        if (c.id == id)
            c.detail = std::move(detail);
}

void CaptionBoard::call_ended(uint64_t id, bool ok, std::string error, Clock::time_point now)
{
    for (Call& c : m_calls)
        if (c.id == id) {
            c.status = ok ? CaptionStatus::Ok : CaptionStatus::Failed;
            c.error  = std::move(error);
            c.ended  = now;
        }
}

std::optional<CaptionBoard::Line> CaptionBoard::current(Clock::time_point now)
{
    m_calls.erase(std::remove_if(m_calls.begin(), m_calls.end(),
                                 [&](const Call& c) { return c.status != CaptionStatus::Running && now - c.ended >= kLinger + kFade; }),
                  m_calls.end());
    if (m_calls.empty())
        return std::nullopt;
    const Call& c = m_calls.back();
    Line        l;
    l.id     = c.id;
    l.title  = c.title;
    l.detail = c.detail;
    l.error  = c.status == CaptionStatus::Failed ? c.error : std::string();
    l.status = c.status;
    l.alpha  = c.status == CaptionStatus::Running ? 1. : fade(now - c.ended, kLinger);
    return l;
}

bool is_sensitive_key(const std::string& key)
{
    std::string lower(key);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (const char* word : {"password", "apikey", "api_key", "access_code", "token", "secret"})
        if (lower.find(word) != std::string::npos)
            return true;
    return false;
}

namespace {

std::string scalar_text(const json& v)
{
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_object() && v.contains("value")) // a per-variant entry: {"index", "value"}
        return scalar_text(v["value"]);
    return v.dump();
}

std::string path_text(std::string path)
{
    for (std::size_t at = path.find('>'); at != std::string::npos; at = path.find('>', at + 3))
        path.replace(at, 1, " > ");
    return path;
}

std::string str(const json& o, const char* key) { return o.contains(key) && o[key].is_string() ? o[key].get<std::string>() : std::string(); }

// A GUI target as a locator names it; a ref ("w12") says nothing yet: its locator comes with call_args.
std::string target_text(const json& target)
{
    if (!target.is_object())
        return {};
    if (const std::string s = str(target, "option"); !s.empty())
        return s;
    if (const std::string s = str(target, "menu"); !s.empty())
        return path_text(s);
    if (const std::string s = str(target, "row"); !s.empty()) {
        const std::string column = str(target, "column");
        return path_text(s) + (column.empty() ? "" : " (" + column + ")");
    }
    if (const std::string s = str(target, "plate_icon"); !s.empty())
        return s + (target.contains("plate") && target["plate"].is_number_integer() ? " [" + std::to_string(target["plate"].get<int>()) + "]" : "");
    for (const char* key : {"gizmo", "gltoolbar", "imgui", "imgui_name", "tab", "label", "name", "class"})
        if (const std::string s = str(target, key); !s.empty())
            return s;
    return {};
}

std::string join(const std::vector<std::string>& parts)
{
    std::string out;
    for (const std::string& p : parts)
        out += (out.empty() ? "" : ", ") + p;
    return out;
}

} // namespace

namespace {

// Every scalar under `value`, as text.
void scalars(const json& value, std::vector<std::string>& out)
{
    if (value.is_string())
        out.push_back(value.get<std::string>());
    else if (value.is_number())
        out.push_back(value.dump());
    else if (value.is_structured())
        for (const json& v : value)
            scalars(v, out);
}

// The values of sensitive keys anywhere in `args` (set_settings' values, a per-variant entry's value).
void sensitive_values(const json& args, std::vector<std::string>& out)
{
    if (args.is_object()) {
        for (const auto& [key, value] : args.items()) {
            if (is_sensitive_key(key))
                scalars(value, out);
            else
                sensitive_values(value, out);
        }
    } else if (args.is_array()) {
        for (const json& v : args)
            sensitive_values(v, out);
    }
}

// What caption_detail() leaves out of `tool`'s caption on purpose.
std::vector<std::string> hidden_values(const std::string& tool, const json& args)
{
    std::vector<std::string> out;
    sensitive_values(args, out);
    if (!args.is_object())
        return out;
    if (tool == "ui_input") {
        const json&       target = args.contains("target") ? args["target"] : json();
        const std::string option = target.is_object() ? str(target, "option") : std::string();
        if ((option.empty() || is_sensitive_key(option)) && args.contains("value"))
            scalars(args["value"], out);
    }
    if (tool == "ui_key" && args.contains("text"))
        scalars(args["text"], out);
    if (tool == "webview_eval")
        scalars(args, out);
    return out;
}

} // namespace

std::string caption_error(const std::string& tool, const json& args, const std::string& error)
{
    std::vector<std::string> hidden = hidden_values(tool, args);
    // Longest first, so a value inside another goes with it.
    std::sort(hidden.begin(), hidden.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    std::string out = error;
    for (const std::string& value : hidden) {
        if (value.empty())
            continue;
        if (value.size() < 3)
            return {};
        // As it is, and as an error that dumps JSON writes it ("a\"b" for a"b).
        const std::string quoted  = json(value).dump();
        const std::string escaped = quoted.substr(1, quoted.size() - 2);
        for (const std::string& form : {escaped, value})
            for (std::size_t at = out.find(form); at != std::string::npos; at = out.find(form, at + 9))
                out.replace(at, form.size(), "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2");
    }
    return out;
}

std::string shorten(const std::string& text, std::size_t limit)
{
    std::string line = text.substr(0, text.find('\n'));
    if (line.size() <= limit)
        return line;
    std::size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80) // inside a UTF-8 character
        --cut;
    return line.substr(0, cut) + "...";
}

std::string caption_detail(const std::string& tool, const json& args)
{
    if (!args.is_object())
        return {};
    if (tool == "ui_input") {
        std::string out = target_text(args.value("target", json()));
        // Only a setting field's value: a field found by its label, or a ref not yet resolved, may be a
        // password box.
        const json&       target = args.value("target", json());
        const std::string option = target.is_object() ? str(target, "option") : std::string();
        if (!option.empty() && !is_sensitive_key(option) && args.contains("value"))
            out += " = " + scalar_text(args["value"]);
        return out;
    }
    if (tool == "ui_key") {
        // Typed text shows as keys in the input caption, masked where it goes into a password field.
        const std::string keys   = str(args, "keys");
        const std::string target = target_text(args.value("target", json()));
        return keys.empty() ? target : (target.empty() ? keys : keys + " - " + target);
    }
    if (tool == "ui_click" || tool == "ui_mouse" || tool == "dialog_respond") {
        std::string out = target_text(args.value("target", json()));
        if (tool == "ui_mouse")
            out = str(args, "action");
        if (tool == "dialog_respond")
            out = str(args, "button");
        if (const std::string item = str(args, "popup_item"); !item.empty())
            out += (out.empty() ? "" : " > ") + path_text(item);
        return out;
    }
    if (tool == "webview_eval")
        return {}; // a script's text may carry anything
    if (args.contains("values") && args["values"].is_object()) {
        std::vector<std::string> pairs;
        for (const auto& [key, value] : args["values"].items())
            pairs.push_back(key + " = " + (is_sensitive_key(key) ? std::string("\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2") : scalar_text(value)));
        return join(pairs);
    }
    const json what = describe_call(tool, args);
    if (!what.is_object())
        return {};
    const std::string kind = str(what, "kind");
    if (kind == "files" || kind == "settings") {
        std::vector<std::string> parts;
        for (const json& p : what.value(kind == "files" ? "names" : "pairs", json::array()))
            if (p.is_string())
                parts.push_back(p.get<std::string>());
        return join(parts);
    }
    if (kind == "command")
        return str(what, "key") + (what.contains("param") ? " " + scalar_text(what["param"]) : "");
    if (kind == "plates") {
        const json& plates = what["plates"];
        if (plates.is_array()) {
            std::vector<std::string> parts;
            for (const json& p : plates)
                parts.push_back(scalar_text(p));
            return join(parts);
        }
        return plates.is_string() && plates.get<std::string>() == "current" ? std::string() : scalar_text(plates);
    }
    if (kind == "recent_project" || kind == "preset")
        return str(what, "name");
    if (kind == "value")
        return str(what, "text");
    return {};
}

}}} // namespace Slic3r::GUI::Mcp
