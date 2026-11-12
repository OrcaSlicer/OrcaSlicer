#include "IllustratedTooltip.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

#include <boost/filesystem/path.hpp>
#include <imgui/imgui_internal.h>

#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"

namespace Slic3r::GUI {

// Seconds on a real clock, for the tooltips' own timing.
//
// Not ImGui::GetTime(): ImGuiWrapper::new_frame() feeds io.DeltaTime the real frame time but caps it
// at 1/60 s, so the idle gaps of the event-driven canvas don't count as time a +/- button is held.
// That cap also means ImGui's clock stalls while the canvas is idle - a pointer resting still on a
// control renders nothing, so the tooltip delay would never run out on ImGui's clock. A real clock
// keeps the tooltip timing and media playback independent of how often the canvas renders.
static double tip_now()
{
    using clock = std::chrono::steady_clock;
    static const clock::time_point base = clock::now();
    return std::chrono::duration<double>(clock::now() - base).count();
}

// How long the pointer has to rest on a control before its tooltip appears. The tooltips are large and
// land on the model, so with no wait at all a sweep across a dense panel or along the toolbar flashes a
// dozen of them over the scene. Once one has just been up the wait drops to almost nothing, so reading
// several in a row stays quick - but not to nothing, or that same sweep would flash again the moment
// one tooltip had been seen.
static constexpr double TIP_DELAY       = 0.45;
static constexpr double TIP_DELAY_AGAIN = 0.10;
static constexpr double TIP_STICKY      = 0.70;

bool IllustratedTooltip::ready(const ImVec2 &control, GLCanvas3D &canvas)
{
    // Nothing explains a control while the control is being used: dragging a slider, typing a value
    // into one, picking from a combo, clicking a tool. The pointer sits on the very thing being edited,
    // so without this the tooltip covers the model exactly while the user is watching it change. The
    // full wait comes back afterwards rather than the short one - having just let go of a slider is not
    // a reason to be shown a picture of it.
    if (ImGui::IsAnyItemActive() || ImGui::IsAnyMouseDown()) {
        m_item       = ImVec2(-1.f, -1.f);
        m_last_shown = -1e9;
        return false;
    }

    // Moving to another control restarts the wait, and staying on this one carries it on. So does coming
    // back to the same control after the pointer was away: a frame on which nothing asked means it left.
    const double now   = tip_now();
    const int    frame = ImGui::GetFrameCount();
    if (frame > m_asked_frame + 1)
        m_item = ImVec2(-1.f, -1.f);
    m_asked_frame = frame;
    if (control.x != m_item.x || control.y != m_item.y) {
        m_item        = control;
        m_hover_since = now;
    }
    if (now - m_hover_since < (now - m_last_shown < TIP_STICKY ? TIP_DELAY_AGAIN : TIP_DELAY)) {
        // A pointer resting on a control produces no events at all, so unless the wait asks for frames
        // itself nothing redraws the canvas and the wait never finishes - the tooltip then appears only
        // on the next pixel of mouse movement.
        //
        // request_extra_frame() and not schedule_extra_frame(): the latter only arranges a timer, whose
        // event an idle event is assumed to follow, and GLCanvas3D::on_idle() renders nothing unless the
        // canvas was already dirty when it ran. request_extra_frame() ends on_idle with
        // evt.RequestMore(), which obliges wx to deliver the next one. It is what 3DBed's fade uses.
        // Costing a few hundred milliseconds of frames is the cheaper half of that trade.
        canvas.request_extra_frame();
        return false;
    }
    m_last_shown = now;

    // ImGui hides a window on the first frame it is submitted, to measure it before showing it - and
    // for a tooltip it does so every time one reappears, because the windows are recycled (see the
    // Popup|Tooltip branch of Begin()). So the frame on which a tooltip is decided is never the frame
    // on which it is seen, and one that then asks for nothing more stays invisible until the next pixel
    // of mouse movement happens to produce another frame. Keep frames coming until it has settled.
    const int shown_frame = ImGui::GetFrameCount();
    m_settled             = (shown_frame == m_shown_frame + 1) ? m_settled + 1 : 0;
    m_shown_frame         = shown_frame;
    if (m_settled < 2)
        canvas.request_extra_frame();
    return true;
}

float IllustratedTooltip::width(const std::string &text, bool with_media, float wrap)
{
    // The width has to be known by the caller, not left to ImGui, because a tooltip is placed by its
    // edges and a picture has to be laid out against something. A plain one takes exactly what its text
    // needs, as it did when ImGui sized it, so a two-word tooltip is not a wide box with two words in it.
    const float text_w = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap).x;
    return (with_media ? wrap : std::min(wrap, std::ceil(text_w))) + 2.f * ImGui::GetStyle().WindowPadding.x;
}

void IllustratedTooltip::item_tooltip(const std::string &text, const std::vector<Media> &media, GLCanvas3D &canvas)
{
    // AllowWhenDisabled: a control that cannot be used is exactly the one whose tooltip has to say why.
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || !ready(ImGui::GetItemRectMin(), canvas))
        return;

    const ImGuiWindow *window = ImGui::GetCurrentWindow()->RootWindow;
    const ImGuiWrapper &imgui  = *wxGetApp().imgui();
    const float        gap    = imgui.scaled(0.5f);
    const float        w      = width(text, !media.empty(), imgui.scaled(20.f));
    const float        left   = window->Pos.x - gap - w;
    const float        right  = window->Pos.x + window->Size.x + gap;
    const bool         fits   = right + w <= ImGui::GetIO().DisplaySize.x;
    render(ImVec2(fits || left < 0.f ? right : left, ImGui::GetItemRectMin().y), w, {}, text, media, canvas);
}

void IllustratedTooltip::render(const ImVec2 &pos, float width, const std::string &title, const std::string &text,
                                const std::vector<Media> &media, GLCanvas3D &canvas)
{
    // ImGui normally keeps a tooltip glued to the mouse; it honours a position set here instead (see the
    // Orca branch in imgui's Begin()), and still clamps the result inside the canvas.
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(ImVec2(width, 0.f)); // height still follows the content

    // Fully opaque even for a control inside a disabled region (ImGuiWrapper::disabled_begin() halves the
    // style alpha), whose tooltip is the one that has to say why it cannot be used.
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.f);
    // The tooltip colours, whatever is pushed at the call site: a tooltip for an item in a combo or popup
    // would otherwise take that popup's background, and the view menu's light text colours.
    ImGuiWrapper::push_tooltip_style();
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(0.f); // the window's own edge, which is what the fixed width above sets
    if (!title.empty()) {
        const size_t eol  = title.find('\n');
        ImGuiWrapper &imgui = *wxGetApp().imgui();
        imgui.push_bold_font();
        ImGui::TextUnformatted(title.c_str(), title.c_str() + std::min(eol, title.size()));
        imgui.pop_bold_font();
        if (eol != std::string::npos)
            ImGui::TextDisabled("%s", title.c_str() + eol + 1);
        if (!text.empty())
            ImGui::Spacing();
    }
    if (!text.empty())
        ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    render_media(media, width - 2.f * ImGui::GetStyle().WindowPadding.x, canvas);
    ImGui::EndTooltip();
    ImGuiWrapper::pop_tooltip_style();
    ImGui::PopStyleVar();
}

void IllustratedTooltip::render_media(const std::vector<Media> &media, float width, GLCanvas3D &canvas)
{
    // Collect what is actually there first: the layout divides the width between the pictures, so one
    // that is missing has to drop out of the count rather than leave a gap.
    std::vector<std::pair<const AnimatedTexture *, const Media *>> shown;
    std::string                                                    key;
    for (const Media &m : media) {
        const AnimatedTexture *texture =
            m_cache.get((boost::filesystem::path(resources_dir()) / "images" / m_media_dir / m.file).string());
        if (texture != nullptr && texture->is_valid() && texture->width() > 0 && texture->height() > 0) {
            shown.emplace_back(texture, &m);
            key += m.file + "|";
        }
    }
    if (shown.empty())
        return;

    // Each animation plays from its first frame when its tooltip opens: these are short loops showing
    // one thing going from one state to another, and joining one halfway through shows the user the
    // end of a change they never saw the start of.
    // Restart on a different tooltip, and on the same one opening again - which is what a gap in the
    // frames it was drawn on means, since it is drawn on every frame for as long as it is on screen.
    const int this_frame = ImGui::GetFrameCount();
    if (key != m_media_key || this_frame != m_media_frame + 1) {
        m_media_key   = key;
        m_media_start = tip_now();
    }
    m_media_frame     = this_frame;
    const double time = tip_now() - m_media_start;

    const ImGuiStyle &style   = ImGui::GetStyle();
    const float       spacing = style.ItemSpacing.x;
    const float       cell_w  = (width - spacing * float(shown.size() - 1)) / float(shown.size());
    const ImU32       frame   = ImGui::GetColorU32(ImGuiCol_Border); // the picture frame of the wx tooltips

    // A single picture would otherwise be drawn at the tooltip's full width, and a square one at full
    // width is a picture as tall as the text beside it is long. Capped and centred in its column.
    const float max_h = wxGetApp().imgui()->scaled(10.f);

    ImGui::Spacing();
    for (size_t i = 0; i < shown.size(); ++i) {
        const AnimatedTexture *texture = shown[i].first;
        const Media           &entry   = *shown[i].second;
        const float            cell_x  = ImGui::GetCursorPosX();

        float w = cell_w;
        float h = std::round(cell_w * float(texture->height()) / float(texture->width()));
        if (h > max_h) {
            w = std::round(max_h * float(texture->width()) / float(texture->height()));
            h = max_h;
        }

        ImGui::BeginGroup();
        const unsigned int id = texture->texture_id(time);
        ImGui::SetCursorPosX(cell_x + std::round(0.5f * (cell_w - w)));
        ImGui::Image((ImTextureID) (intptr_t) id, ImVec2(w, h));
        ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), frame, style.FrameRounding);
        // The canvas only redraws on demand, so an animation has to ask for its own frames. It asks for
        // them as fast as they come rather than pacing itself with a timer: which frame is drawn is
        // decided by the clock above, so a redraw that arrives early simply draws the same frame again,
        // and a guaranteed frame is worth more here than a frugal one that may never arrive.
        if (texture->is_animated())
            canvas.request_extra_frame();

        if (!entry.caption.empty()) {
            // Centred under its own picture, and never wider than its column.
            const std::string caption = into_u8(entry.caption);
            const float       text_w  = ImGui::CalcTextSize(caption.c_str()).x;
            ImGui::SetCursorPosX(cell_x + std::max(0.f, (cell_w - text_w) * 0.5f));
            ImGui::PushTextWrapPos(cell_x + cell_w);
            ImGui::TextDisabled("%s", caption.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndGroup();
        if (i + 1 < shown.size())
            ImGui::SameLine(0.f, spacing);
    }
}

} // namespace Slic3r::GUI
