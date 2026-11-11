#include "IllustratedTipPopup.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/filesystem/path.hpp>

#include <wx/bitmap.h>
#include <wx/brush.h>
#include <wx/colour.h>
#include <wx/dcbuffer.h>
#include <wx/dialog.h>
#include <wx/display.h>
#include <wx/event.h>
#include <wx/eventfilter.h>
#include <wx/font.h>
#include <wx/image.h>
#include <wx/pen.h>
#include <wx/popupwin.h>
#include <wx/textentry.h>
#include <wx/timer.h>
#include <wx/tooltip.h>
#include <wx/window.h>

#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>
#endif
#ifdef __WXGTK__
#include <gtk/gtk.h>
#include "slic3r/GUI/LinuxDisplayBackend.hpp"
#endif

#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/AnimatedTexture.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"

namespace Slic3r::GUI {

namespace {

// The timing of the 3D view's tooltips (IllustratedTooltip.cpp holds the reasoning): a wait before the
// first one, a short one while one has just been up, so that reading several in a row stays quick and a
// sweep across a page does not flash a tooltip per control.
constexpr double TIP_DELAY       = 0.45;
constexpr double TIP_DELAY_AGAIN = 0.10;
constexpr double TIP_STICKY      = 0.70;

// Sizes in DIP. The text wraps at the width of the 3D view's tooltips, 20 of its em, which is 1.5 times
// the wx em_unit (GLCanvas3D::set_imgui_scaling), so 300 DIP; a picture is at most 10 of its em tall.
constexpr int TEXT_WIDTH   = 300;
constexpr int PICTURE_MAX  = 150;
constexpr int PADDING      = 8;
constexpr int SPACING      = 6;
constexpr int CAPTION_GAP  = 2;
constexpr int ANCHOR_GAP   = 4;

// How long a pointer that left the control has to stay away before the tooltip goes, so that crossing
// from a composite widget's frame onto its own text field is not taken for leaving it.
constexpr int LEAVE_MS = 40;
// How often the tooltip checks that its control is still on screen and has not moved.
constexpr int WATCH_MS = 150;

double tip_now()
{
    using clock = std::chrono::steady_clock;
    static const clock::time_point base = clock::now();
    return std::chrono::duration<double>(clock::now() - base).count();
}

bool on_wayland()
{
#ifdef __WXGTK__
    return is_running_on_wayland();
#else
    return false;
#endif
}

// ---------------------------------------------------------------------------------------------------
// Decoded pictures

// A picture decoded once. Plain data, not wx objects, so the cache can be a static that outlives the app.
struct Illustration
{
    int                            width  = 0;
    int                            height = 0;
    std::vector<IllustrationFrame> frames;
    double                         duration = 0.;
};

// Decoded pictures by absolute path, the most recently used first. A GIF decodes to a few MB of RGBA, so
// only the last few are kept; decoding one again costs a few milliseconds when its tooltip opens.
std::shared_ptr<const Illustration> load_illustration(const std::string &path)
{
    constexpr size_t CACHE_SIZE = 24;
    static std::vector<std::pair<std::string, std::shared_ptr<const Illustration>>> cache;

    const auto it = std::find_if(cache.begin(), cache.end(), [&path](const auto &e) { return e.first == path; });
    if (it != cache.end()) {
        std::rotate(cache.begin(), it, it + 1);
        return cache.front().second;
    }

    auto ill = std::make_shared<Illustration>();
    if (decode_illustration(path, ill->width, ill->height, ill->frames) && ill->width > 0 && ill->height > 0) {
        for (const IllustrationFrame &f : ill->frames)
            ill->duration += f.delay;
    } else {
        ill.reset(); // remembered as a failure: a picture not drawn yet costs one lookup, not a decode each time
    }
    cache.insert(cache.begin(), {path, ill});
    if (cache.size() > CACHE_SIZE)
        cache.pop_back();
    return ill;
}

wxImage frame_image(const Illustration &ill, const IllustrationFrame &frame)
{
    const size_t n = size_t(ill.width) * size_t(ill.height);
    wxImage      image(ill.width, ill.height, false);
    image.InitAlpha();
    unsigned char *rgb   = image.GetData();
    unsigned char *alpha = image.GetAlpha();
    for (size_t i = 0; i < n && i * 4 + 3 < frame.rgba.size(); ++i) {
        rgb[i * 3 + 0] = frame.rgba[i * 4 + 0];
        rgb[i * 3 + 1] = frame.rgba[i * 4 + 1];
        rgb[i * 3 + 2] = frame.rgba[i * 4 + 2];
        alpha[i]       = frame.rgba[i * 4 + 3];
    }
    return image;
}

// Breaks `text` into lines no wider than `width`: at its own line breaks, then between words, and inside
// a word only when the word alone is wider (text without spaces, as in Chinese or Japanese).
std::vector<wxString> wrap_text(const wxWindow &win, const wxFont &font, const wxString &text, int width)
{
    const auto extent = [&win, &font](const wxString &s) {
        int w = 0, h = 0;
        win.GetTextExtent(s, &w, &h, nullptr, nullptr, &font);
        return w;
    };

    std::vector<wxString> lines;
    size_t                start = 0;
    while (start <= text.length()) {
        size_t end = text.find('\n', start);
        if (end == wxString::npos)
            end = text.length();
        const wxString paragraph = text.Mid(start, end - start);
        start                    = end + 1;

        wxString line;
        size_t   pos = 0;
        while (pos <= paragraph.length()) {
            size_t word_end = paragraph.find(' ', pos);
            if (word_end == wxString::npos)
                word_end = paragraph.length();
            wxString word = paragraph.Mid(pos, word_end - pos);
            pos           = word_end + 1;

            const wxString joined = line.empty() ? word : line + " " + word;
            if (extent(joined) <= width) {
                line = joined;
                continue;
            }
            if (!line.empty())
                lines.push_back(line);
            // The longest head of the word that fits, at least one character so this always ends.
            while (word.length() > 1 && extent(word) > width) {
                size_t lo = 1, hi = word.length() - 1;
                while (lo < hi) {
                    const size_t mid = (lo + hi + 1) / 2;
                    if (extent(word.Left(mid)) <= width)
                        lo = mid;
                    else
                        hi = mid - 1;
                }
                lines.push_back(word.Left(lo));
                word = word.Mid(lo);
            }
            line = word;
        }
        lines.push_back(line);
        if (end == text.length())
            break;
    }
    while (!lines.empty() && lines.back().empty())
        lines.pop_back();
    return lines;
}

// ---------------------------------------------------------------------------------------------------
// State

struct Tip
{
    wxString              text;
    std::vector<TipMedia> media;
    TipMediaSource        source; // used instead of `media` when set

    bool empty() const { return text.empty() && media.empty() && !source; }
};

// A window given a tooltip with set_illustrated_tooltip().
struct Registered
{
    Tip      tip;
    wxString base_text; // the text as given, to recognise a native tooltip that extends it
};

enum class Phase {
    Idle,      // the pointer is on no control with a tooltip
    Waiting,   // the pointer rests on one, the wait runs
    Shown,     // the tooltip is on screen
    Dismissed, // the control was used (click, key, wheel): nothing until the pointer moves again
    Empty,     // the control's tooltip turned out to have nothing to show: nothing until another control
};

class TipWindow;

struct State
{
    wxWindow *owner = nullptr; // the control hovered: a registered window, or a hover_ window
    wxRect    item;            // for a hover_ window, the hovered control in its client coordinates
    bool      hover_style = false;
    Tip       tip;
    Phase     phase         = Phase::Idle;
    double    last_shown    = -1e9;
    bool      button_down   = false;
    bool      leave_pending = false;
    wxRect    anchor; // where the control was on screen when its tooltip opened
    TipWindow *popup = nullptr;
};

State &state()
{
    static State s;
    return s;
}

std::unordered_map<wxWindow *, Registered> &registry()
{
    static std::unordered_map<wxWindow *, Registered> r;
    return r;
}

// Windows our handlers are bound on, so each is bound once: true for a registered window and the windows
// inside it (pointer moves are followed), false for a hover_ window (its owner reports the moves).
std::unordered_map<wxWindow *, bool> &tracked()
{
    static std::unordered_map<wxWindow *, bool> t;
    return t;
}

// Whether `w` is `ancestor` or inside it, without crossing into another top-level window: a ComboBox's
// drop-down list is a child of the ComboBox but a window of its own.
bool is_within(const wxWindow *w, const wxWindow *ancestor)
{
    for (; w != nullptr; w = w->IsTopLevel() ? nullptr : w->GetParent())
        if (w == ancestor)
            return true;
    return false;
}

wxWindow *registered_owner(wxWindow *w)
{
    const auto &reg = registry();
    for (; w != nullptr; w = w->IsTopLevel() ? nullptr : w->GetParent())
        if (reg.count(w) != 0)
            return w;
    return nullptr;
}

// An editable text field inside the control has the keyboard: the control is being used, as an ImGui
// input field is while it is active.
bool in_use(const wxWindow *owner)
{
    wxWindow *focus = wxWindow::FindFocus();
    if (focus == nullptr || !is_within(focus, owner))
        return false;
    const auto *entry = dynamic_cast<const wxTextEntry *>(focus);
    return entry != nullptr && entry->IsEditable();
}

void strip_descendant_tooltips(wxWindow *w)
{
    for (wxWindow *child : w->GetChildren()) {
        if (child->IsTopLevel())
            continue;
        if (child->GetToolTip() != nullptr)
            child->UnsetToolTip();
        strip_descendant_tooltips(child);
    }
}

// A registered window shows no native tooltip besides ours. Some widgets set one themselves later
// (::ComboBox and PresetComboBox the selected value, TextInput forwards its own to its text field), and
// some code adds to the setting's tooltip the plain way (Tab::update_option_tooltip and
// ObjectTableSettings prepend why an option is disabled). A native tooltip on the window itself that
// still contains the text it was given is such an extension and becomes the text shown; any other is
// the widget's own and is dropped. Run on registration, on every pointer move over the window and
// while it is hovered, which is before a native tooltip's own delay runs out.
void suppress_native_tooltips(wxWindow *owner, Registered &reg)
{
    if (const wxToolTip *native = owner->GetToolTip()) {
        const wxString text = native->GetTip();
        if (!reg.base_text.empty() && text.Find(reg.base_text) != wxNOT_FOUND && text != reg.tip.text) {
            reg.tip.text = text;
            State &st = state();
            if (st.owner == owner && !st.hover_style)
                st.tip.text = text;
        }
        owner->UnsetToolTip();
    }
    strip_descendant_tooltips(owner);
}

void on_window_mouse(wxMouseEvent &event);
void on_window_destroy(wxWindowDestroyEvent &event);

// Hover tracking binds the window and every window inside it, rather than following the pointer with a
// timer and wxFindWindowAtPoint(): that needs the global pointer position, which Wayland does not give.
// The children matter because Orca's composite widgets (TextInput, ::ComboBox, SpinInput) are a frame
// with a native text field in it, and the pointer is over the field. Windows a widget adds later are
// not followed; none of the widgets used in settings pages does that.
void track(wxWindow *w, bool follow_moves)
{
    auto [it, added] = tracked().emplace(w, false);
    if (added) {
        // Free functions, not lambdas capturing state: they look the window up when an event comes, so
        // nothing dangles whichever of the windows involved goes first.
        w->Bind(wxEVT_LEAVE_WINDOW, &on_window_mouse);
        w->Bind(wxEVT_DESTROY, &on_window_destroy);
    }
    if (!follow_moves)
        return;
    if (!it->second) {
        it->second = true;
        w->Bind(wxEVT_ENTER_WINDOW, &on_window_mouse);
        w->Bind(wxEVT_MOTION, &on_window_mouse);
    }
    for (wxWindow *child : w->GetChildren())
        if (!child->IsTopLevel())
            track(child, true);
}

// ---------------------------------------------------------------------------------------------------
// The popup

// The one window all illustrated tooltips are drawn in.
//
// A plain wxPopupWindow, not a wxPopupTransientWindow (Orca's PopupWindow): the transient kind grabs the
// pointer on GTK, captures it on macOS and dismisses itself on its own terms, and a tooltip must do none
// of that. It never takes focus or activation: on macOS wxPopupWindow::Show() orders the panel in without
// activating it (and nothing here calls Raise(), which would make it the key window); on GTK it is a
// GTK_WINDOW_POPUP, override-redirect on X11 and an xdg_popup without a grab of its own on Wayland; on
// MSW Show() is replaced below. Its parent is the top-level window of the control it explains, so on
// Wayland it maps as a popup of the surface the control is on, which is what places it there.
//
// It draws everything itself and has no child windows: nothing in it can take a click or the focus.
// It is also the event filter that hides the tooltip when the user does something.
class TipWindow : public wxPopupWindow, public wxEventFilter
{
public:
    explicit TipWindow(wxWindow *parent);
    ~TipWindow() override;

    void set_content(const wxString &text, const std::vector<TipMedia> &media);
    bool has_content() const { return !m_lines.empty() || !m_pictures.empty(); }
    void show_tip();

    void wait(double seconds) { m_wait.StartOnce(std::max(1, int(std::lround(seconds * 1000.)))); }
    void stop_wait() { m_wait.Stop(); }
    void check_leave() { m_leave.StartOnce(LEAVE_MS); }
    void watch()
    {
        if (!m_watch.IsRunning())
            m_watch.Start(WATCH_MS);
    }
    void stop_timers();

    int FilterEvent(wxEvent &event) override;

#ifdef __WXMSW__
    bool    Show(bool show = true) override;
    WXDWORD MSWGetStyle(long flags, WXDWORD *exstyle) const override;
#endif

private:
    struct Picture
    {
        std::string           key;    // path and size, to keep the scaled frames across openings
        std::vector<wxBitmap> frames; // scaled to the size drawn at, in physical pixels
        std::vector<double>   starts; // seconds into the loop each frame appears at
        double                duration = 0.;
        wxRect                rect;   // where it is drawn
        std::vector<wxString> caption;
        wxPoint               caption_pos;
        int                   cell_width = 0;
    };

    void   on_paint(wxPaintEvent &event);
    void   on_frame_timer(wxTimerEvent &event);
    size_t frame_at(const Picture &picture, double t, double *next_change) const;
    void   schedule_frame(double t);

    wxFont                m_font;
    wxFont                m_caption_font;
    std::vector<wxString> m_lines;
    int                   m_line_height = 0;
    std::vector<Picture>  m_pictures;
    std::string           m_media_key; // the files shown, to tell a reopened tooltip from updated pictures
    double                m_anim_start = 0.;

    wxTimer m_wait;
    wxTimer m_leave;
    wxTimer m_watch;
    wxTimer m_frame;
};

// The timer callbacks of the shared state, defined below.
void on_wait_elapsed();
void on_leave_elapsed();
void on_watch();
void dismiss_by_use();
void reset();

enum TimerId { ID_WAIT = 1, ID_LEAVE, ID_WATCH, ID_FRAME };

TipWindow::TipWindow(wxWindow *parent)
    : m_wait(this, ID_WAIT), m_leave(this, ID_LEAVE), m_watch(this, ID_WATCH), m_frame(this, ID_FRAME)
{
    // Created in two steps so that the MSW style override below takes part in creating the window.
    int flags = wxBORDER_NONE;
#ifdef __WXMSW__
    // An owned WS_POPUP window, like a native tooltip, rather than a child of the desktop: it is hidden
    // with its window when that is minimised. Show() below keeps it from taking the focus anyway.
    flags |= wxPU_CONTAINS_CONTROLS;
#endif
    Create(parent, flags);
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_font         = ::Label::Body_13;
    m_caption_font = ::Label::Body_12;
    SetFont(m_font);

    Bind(wxEVT_PAINT, &TipWindow::on_paint, this);
    Bind(wxEVT_TIMER, [](wxTimerEvent &) { on_wait_elapsed(); }, ID_WAIT);
    Bind(wxEVT_TIMER, [](wxTimerEvent &) { on_leave_elapsed(); }, ID_LEAVE);
    Bind(wxEVT_TIMER, [](wxTimerEvent &) { on_watch(); }, ID_WATCH);
    Bind(wxEVT_TIMER, &TipWindow::on_frame_timer, this, ID_FRAME);
    // The pointer only gets onto the tooltip when it could not be placed clear of its control. Hide it
    // then, as if the control had been used, or it would come back under the pointer again and again.
    Bind(wxEVT_ENTER_WINDOW, [](wxMouseEvent &e) { e.Skip(); dismiss_by_use(); });

    wxEvtHandler::AddFilter(this);
}

TipWindow::~TipWindow()
{
    wxEvtHandler::RemoveFilter(this);
    stop_timers();
    State &st = state();
    if (st.popup == this) {
        st.popup = nullptr;
        st.owner = nullptr;
        st.tip   = {};
        st.phase = Phase::Idle;
    }
}

void TipWindow::stop_timers()
{
    m_wait.Stop();
    m_leave.Stop();
    m_watch.Stop();
    m_frame.Stop();
}

#ifdef __WXMSW__
// wxPopupWindow::Show() activates the popup: ShowWindow(SW_SHOW) with wxPU_CONTAINS_CONTROLS,
// SetForegroundWindow() without. That would take the activation from the drop-down list the pointer is
// on, which closes when it loses it, and the call also makes this the wxCurrentPopupWindow that a click
// elsewhere dismisses, in place of that list. Show it the way a native tooltip is shown instead.
bool TipWindow::Show(bool show)
{
    if (!wxWindowBase::Show(show))
        return false;
    if (HWND hwnd = static_cast<HWND>(GetHWND())) {
        ::ShowWindow(hwnd, show ? SW_SHOWNOACTIVATE : SW_HIDE);
        if (show)
            ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    return true;
}

WXDWORD TipWindow::MSWGetStyle(long flags, WXDWORD *exstyle) const
{
    const WXDWORD style = wxPopupWindow::MSWGetStyle(flags, exstyle);
    if (exstyle != nullptr)
        *exstyle |= WS_EX_NOACTIVATE; // a click on it does not activate it either
    return style;
}
#endif

void TipWindow::set_content(const wxString &text, const std::vector<TipMedia> &media)
{
    const int pad      = FromDIP(PADDING);
    const int spacing  = FromDIP(SPACING);
    const int wrap     = FromDIP(TEXT_WIDTH);
    const int max_h    = FromDIP(PICTURE_MAX);
    const double scale = GetContentScaleFactor(); // physical pixels per logical one (1 on MSW, 2 on Retina)

    m_lines = wrap_text(*this, m_font, text, wrap);
    {
        int w = 0;
        GetTextExtent("Ag", &w, &m_line_height, nullptr, nullptr, &m_font);
    }

    // The pictures that are there: one missing or undecodable drops out of the row rather than leave a
    // gap, and a tooltip whose pictures are all missing is a text tooltip.
    std::vector<std::pair<std::shared_ptr<const Illustration>, const TipMedia *>> shown;
    std::string                                                                   media_key;
    for (const TipMedia &m : media) {
        const std::string path = (boost::filesystem::path(resources_dir()) / "images" / m.file).string();
        if (auto ill = load_illustration(path)) {
            shown.emplace_back(std::move(ill), &m);
            media_key += path + "|";
        }
    }

    int text_w = 0;
    for (const wxString &line : m_lines) {
        int w = 0, h = 0;
        GetTextExtent(line, &w, &h, nullptr, nullptr, &m_font);
        text_w = std::max(text_w, w);
    }
    // One with pictures takes the full width, since a picture is laid out against something; a plain
    // one takes what its text needs, as the 3D view's do.
    const int content_w = shown.empty() ? std::min(wrap, text_w) : wrap;

    int y = pad + int(m_lines.size()) * m_line_height;

    std::vector<Picture> pictures;
    if (!shown.empty()) {
        if (!m_lines.empty())
            y += spacing;
        const int n      = int(shown.size());
        const int cell_w = std::max(1, (content_w - spacing * (n - 1)) / n);
        int       row_h  = 0;
        int       cap_h  = 0;
        for (int i = 0; i < n; ++i) {
            const Illustration &ill = *shown[i].first;
            int w = cell_w;
            int h = int(std::lround(double(cell_w) * ill.height / ill.width));
            if (h > max_h) {
                w = int(std::lround(double(max_h) * ill.width / ill.height));
                h = max_h;
            }
            w = std::max(1, w);
            h = std::max(1, h);
            const int cell_x = pad + i * (cell_w + spacing);

            Picture pic;
            pic.rect       = wxRect(cell_x + (cell_w - w) / 2, y, w, h);
            pic.cell_width = cell_w;
            pic.key        = media_key + std::to_string(i) + ":" + std::to_string(w) + "x" + std::to_string(h) + "@" +
                      std::to_string(scale);
            // Reuse the frames scaled for the last opening when nothing changed: a GIF is a few dozen
            // frames to rescale.
            const auto old = std::find_if(m_pictures.begin(), m_pictures.end(), [&pic](const Picture &p) { return p.key == pic.key; });
            if (old != m_pictures.end()) {
                pic.frames   = old->frames;
                pic.starts   = old->starts;
                pic.duration = old->duration;
            } else {
                const int pw = std::max(1, int(std::lround(w * scale)));
                const int ph = std::max(1, int(std::lround(h * scale)));
                double    t  = 0.;
                for (const IllustrationFrame &frame : ill.frames) {
                    wxImage image = frame_image(ill, frame);
                    if (image.GetWidth() != pw || image.GetHeight() != ph)
                        image.Rescale(pw, ph, wxIMAGE_QUALITY_HIGH);
                    wxBitmap bmp(image);
                    bmp.SetScaleFactor(scale);
                    pic.frames.push_back(bmp);
                    pic.starts.push_back(t);
                    t += frame.delay;
                }
                pic.duration = ill.duration;
            }
            if (!shown[i].second->caption.empty())
                pic.caption = wrap_text(*this, m_caption_font, shown[i].second->caption, cell_w);
            row_h = std::max(row_h, h);
            cap_h = std::max(cap_h, int(pic.caption.size()));
            pictures.push_back(std::move(pic));
        }
        int cap_line_h = 0;
        {
            int w = 0;
            GetTextExtent("Ag", &w, &cap_line_h, nullptr, nullptr, &m_caption_font);
        }
        for (Picture &pic : pictures)
            pic.caption_pos = wxPoint(pic.rect.x - (pic.cell_width - pic.rect.width) / 2, y + row_h + FromDIP(CAPTION_GAP));
        y += row_h + (cap_h > 0 ? FromDIP(CAPTION_GAP) + cap_h * cap_line_h : 0);
    }
    m_pictures = std::move(pictures);

    // An animation plays from its first frame each time its tooltip opens (see show_tip()), but not
    // when the same tooltip is merely refreshed with the same pictures while on screen.
    if (media_key != m_media_key || !IsShown())
        m_anim_start = tip_now();
    m_media_key = media_key;

    SetSize(wxSize(content_w + 2 * pad, y + pad));
    Refresh();
}

void TipWindow::show_tip()
{
    if (IsShown())
        return;
#ifdef __WXGTK__
    // On Wayland GDK refuses to map a popup whose parent is not the topmost popup, and a GTK tooltip
    // (an item tooltip of a drop-down list) is one. PopupWindow::Show() does the same before its popups.
    if (on_wayland()) {
        GList *toplevels = gtk_window_list_toplevels();
        for (GList *l = toplevels; l != nullptr; l = l->next)
            if (gtk_window_get_type_hint(GTK_WINDOW(l->data)) == GDK_WINDOW_TYPE_HINT_TOOLTIP &&
                gtk_widget_get_visible(GTK_WIDGET(l->data)))
                gtk_widget_hide(GTK_WIDGET(l->data));
        g_list_free(toplevels);
    }
#endif
    m_anim_start = tip_now();
    Show(true);
    schedule_frame(0.);
}

size_t TipWindow::frame_at(const Picture &picture, double t, double *next_change) const
{
    if (picture.frames.size() <= 1 || picture.duration <= 0.)
        return 0;
    const double loop_t = t - picture.duration * std::floor(t / picture.duration);
    size_t       i      = 0;
    while (i + 1 < picture.frames.size() && loop_t >= picture.starts[i + 1])
        ++i;
    if (next_change != nullptr) {
        const double end = i + 1 < picture.frames.size() ? picture.starts[i + 1] : picture.duration;
        *next_change     = std::min(*next_change, end - loop_t);
    }
    return i;
}

void TipWindow::schedule_frame(double t)
{
    double next = 1e9;
    for (const Picture &pic : m_pictures)
        frame_at(pic, t, &next);
    if (next < 1e9)
        m_frame.StartOnce(std::max(10, int(std::ceil(next * 1000.))));
    else
        m_frame.Stop();
}

void TipWindow::on_frame_timer(wxTimerEvent &)
{
    if (IsShown())
        Refresh(); // the paint schedules the next frame
}

void TipWindow::on_paint(wxPaintEvent &)
{
    wxAutoBufferedPaintDC dc(this);
    // The dark mode palette of the drop-down lists and input boxes, in both themes: the tooltips of the 3D
    // view are dark in both (ImGuiWrapper::push_tooltip_style() pushes these same colours), and the
    // pictures, drawn on dark, sit the same way in all of them.
    const wxColour bg(0x2D, 0x2D, 0x31);
    const wxColour border(0x4A, 0x4A, 0x51);
    const wxColour text(0xEF, 0xEF, 0xF0);
    const wxColour dimmed(0xB3, 0xB3, 0xB5);

    const wxSize size = GetClientSize();
    dc.SetPen(wxPen(border));
    dc.SetBrush(wxBrush(bg));
    dc.DrawRectangle(0, 0, size.x, size.y);

    const int pad = FromDIP(PADDING);
    dc.SetFont(m_font);
    dc.SetTextForeground(text);
    int y = pad;
    for (const wxString &line : m_lines) {
        dc.DrawText(line, pad, y);
        y += m_line_height;
    }

    const double t = tip_now() - m_anim_start;
    dc.SetFont(m_caption_font);
    dc.SetTextForeground(dimmed);
    for (const Picture &pic : m_pictures) {
        if (!pic.frames.empty())
            dc.DrawBitmap(pic.frames[frame_at(pic, t, nullptr)], pic.rect.GetTopLeft(), true);
        dc.SetPen(wxPen(border));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        wxRect frame = pic.rect;
        dc.DrawRectangle(frame.Inflate(1));
        int cy = pic.caption_pos.y;
        for (const wxString &line : pic.caption) {
            int w = 0, h = 0;
            dc.GetTextExtent(line, &w, &h);
            dc.DrawText(line, pic.caption_pos.x + std::max(0, (pic.cell_width - w) / 2), cy);
            cy += h;
        }
    }
    schedule_frame(t);
}

int TipWindow::FilterEvent(wxEvent &event)
{
    // Every event of the app passes here, so this only compares types.
    const wxEventType type = event.GetEventType();
    State            &st   = state();
    if (type == wxEVT_LEFT_DOWN || type == wxEVT_RIGHT_DOWN || type == wxEVT_MIDDLE_DOWN || type == wxEVT_AUX1_DOWN ||
        type == wxEVT_AUX2_DOWN || type == wxEVT_LEFT_DCLICK || type == wxEVT_RIGHT_DCLICK || type == wxEVT_MIDDLE_DCLICK) {
        st.button_down = true;
        dismiss_by_use();
    } else if (type == wxEVT_LEFT_UP || type == wxEVT_RIGHT_UP || type == wxEVT_MIDDLE_UP || type == wxEVT_AUX1_UP ||
               type == wxEVT_AUX2_UP) {
        st.button_down = false;
    } else if (type == wxEVT_MOUSEWHEEL || type == wxEVT_KEY_DOWN || type == wxEVT_CHAR_HOOK) {
        dismiss_by_use();
    } else if (type == wxEVT_ACTIVATE_APP && !static_cast<wxActivateEvent &>(event).GetActive()) {
        // Another application came forward: the popup would stay above it on X11 and MSW, and macOS
        // hides it without telling wx. Not on Wayland, where it is part of the window it belongs to,
        // and where a tooltip over a drop-down list holds the keyboard for a moment while it is up.
        if (!on_wayland())
            reset();
    }
    return Event_Skip;
}

// ---------------------------------------------------------------------------------------------------
// The shared state machine

TipWindow *ensure_popup(wxWindow *owner)
{
    State    &st  = state();
    wxWindow *top = wxGetTopLevelParent(owner);
    if (top == nullptr)
        return nullptr;
    if (st.popup != nullptr && st.popup->GetParent() != top) {
        // A control in another window: recreate the popup there, so it belongs to that window (its
        // owner on MSW, its parent surface on Wayland). Never from the popup's own event handlers.
        TipWindow *old = st.popup;
        st.popup       = nullptr;
        old->Hide();
        old->Destroy();
    }
    if (st.popup == nullptr)
        st.popup = new TipWindow(top);
    return st.popup;
}

void hide_popup()
{
    State &st = state();
    if (st.popup == nullptr)
        return;
    st.popup->stop_wait();
    if (st.popup->IsShown()) {
        st.popup->Hide();
        st.last_shown = tip_now();
    }
}

void reset()
{
    State &st = state();
    hide_popup();
    if (st.popup != nullptr)
        st.popup->stop_timers();
    st.owner         = nullptr;
    st.item          = wxRect();
    st.hover_style   = false;
    st.tip           = {};
    st.phase         = Phase::Idle;
    st.leave_pending = false;
}

void dismiss_by_use()
{
    State &st = state();
    if (st.phase != Phase::Waiting && st.phase != Phase::Shown)
        return;
    hide_popup();
    st.phase = Phase::Dismissed;
    // The full wait comes back afterwards, not the short one: having just used a control is not a
    // reason to be shown a picture of it.
    st.last_shown = -1e9;
}

void begin(wxWindow *owner, const wxRect &item, bool hover_style, Tip tip)
{
    State &st = state();
    hide_popup();
    st.owner         = owner;
    st.item          = item;
    st.hover_style   = hover_style;
    st.tip           = std::move(tip);
    st.leave_pending = false;

    TipWindow *popup = ensure_popup(owner);
    if (popup == nullptr) {
        st.phase = Phase::Dismissed;
        return;
    }
    popup->watch();
    // Nothing explains a control while it is being used.
    if (st.button_down || in_use(owner)) {
        st.phase = Phase::Dismissed;
        return;
    }
    st.phase = Phase::Waiting;
    popup->wait(tip_now() - st.last_shown < TIP_STICKY ? TIP_DELAY_AGAIN : TIP_DELAY);
}

wxRect anchor_rect(const State &st)
{
    if (st.hover_style)
        return wxRect(st.owner->ClientToScreen(st.item.GetTopLeft()), st.item.GetSize());
    return st.owner->GetScreenRect();
}

// Where a tooltip may go. Elsewhere the work area of the control's display. On Wayland screen
// coordinates are relative to the window's own surface and the displays' are unknown, so it is the
// window the control is in (for a drop-down list, the window the list was opened from).
wxRect bounds_for(wxWindow *owner)
{
    if (on_wayland()) {
        wxWindow *top = wxGetTopLevelParent(owner);
        while (top != nullptr && dynamic_cast<wxPopupWindow *>(top) != nullptr && top->GetParent() != nullptr)
            top = wxGetTopLevelParent(top->GetParent());
        if (top != nullptr)
            return wxRect(top->ClientToScreen(wxPoint(0, 0)), top->GetClientSize());
    }
    const int index = wxDisplay::GetFromWindow(owner);
    return wxDisplay(unsigned(index == wxNOT_FOUND ? 0 : index)).GetClientArea();
}

int clamp_to(int v, int lo, int hi) { return std::max(lo, std::min(v, std::max(lo, hi))); }

// One coordinate of a tooltip `size` long placed after `a_end` or before `a_start`, whichever fits,
// within [b_start, b_end): after when both do, the side with more room when neither does.
int place_axis(int a_start, int a_end, int size, int b_start, int b_end, int gap)
{
    const int after  = a_end + gap;
    const int before = a_start - gap - size;
    int       v;
    if (after + size <= b_end)
        v = after;
    else if (before >= b_start)
        v = before;
    else
        v = (b_end - a_end >= a_start - b_start) ? after : before;
    return clamp_to(v, b_start, b_end - size);
}

// The column a control sits in: the outermost window between the control and its top-level window that
// is a tall, narrow column of controls (the sidebar with the settings pages, a side panel of a tab). A
// tooltip beside the column covers none of its rows. Null when the control is in no such column.
wxWindow *side_column(wxWindow *owner)
{
    wxWindow *top = wxGetTopLevelParent(owner);
    if (top == nullptr)
        return nullptr;
    const int top_width = top->GetClientSize().x;
    wxWindow *column    = nullptr;
    for (wxWindow *w = owner; w != nullptr && w != top; w = w->GetParent()) {
        const wxSize size = w->GetSize();
        if (size.y >= size.x && size.x * 5 <= top_width * 3)
            column = w;
    }
    return column;
}

// Beside `side`, level with the control: after it when that fits in `bounds`, before it when that does,
// or false when neither does.
bool place_beside(const wxRect &side, const wxRect &anchor, const wxSize &size, const wxRect &bounds, int gap, wxPoint &pos)
{
    const int after  = side.x + side.width + gap;
    const int before = side.x - gap - size.x;
    if (after + size.x <= bounds.x + bounds.width)
        pos.x = after;
    else if (before >= bounds.x)
        pos.x = before;
    else
        return false;
    pos.y = clamp_to(anchor.y, bounds.y, bounds.y + bounds.height - size.y);
    return true;
}

// Beside the control rather than under the mouse, never on the control itself, and off the controls
// around it where there is room:
// - In a popup (a drop-down list), beside the popup and level with the item: below the item would cover
//   the next items, which are the ones the user is choosing between.
// - In a column of controls (the sidebar's settings pages), beside the column and level with the control,
//   over the 3D view rather than over the next rows of the page.
// - In a dialog, beside the dialog. Not on Wayland, where a tooltip is kept inside its window.
// - Otherwise (a toolbar, a full-width page), or when there is no room beside: below the control and
//   level with its left edge, or above it when there is no room below.
void place(TipWindow &popup, const State &st)
{
    const wxRect anchor = anchor_rect(st);
    const wxRect bounds = bounds_for(st.owner);
    const wxSize size   = popup.GetSize();
    const int    gap    = popup.FromDIP(ANCHOR_GAP);
    wxPoint      pos;
    wxWindow    *top = wxGetTopLevelParent(st.owner);
    if (top != nullptr && dynamic_cast<wxPopupWindow *>(top) != nullptr) {
        const wxRect side = top->GetScreenRect();
        pos.x = place_axis(side.x, side.x + side.width, size.x, bounds.x, bounds.x + bounds.width, gap);
        pos.y = clamp_to(anchor.y, bounds.y, bounds.y + bounds.height - size.y);
    } else {
        wxWindow *column = side_column(st.owner);
        const bool beside =
            (column != nullptr && place_beside(column->GetScreenRect(), anchor, size, bounds, gap, pos)) ||
            (top != nullptr && dynamic_cast<wxDialog *>(top) != nullptr && !on_wayland() &&
             place_beside(top->GetScreenRect(), anchor, size, bounds, gap, pos));
        if (!beside) {
            pos.x = clamp_to(anchor.x, bounds.x, bounds.x + bounds.width - size.x);
            pos.y = place_axis(anchor.y, anchor.y + anchor.height, size.y, bounds.y, bounds.y + bounds.height, gap);
        }
    }
    popup.SetSize(wxRect(pos, size));
}

void show_now()
{
    State &st = state();
    if (st.owner == nullptr || st.popup == nullptr)
        return;
    TipWindow &popup = *st.popup;
    // Pictures given as a source are worked out now, when the tooltip opens, never per mouse move.
    const std::vector<TipMedia> media = st.tip.source ? st.tip.source() : st.tip.media;
    // The source ran user code: make sure the control and the popup are both still there.
    if (st.owner == nullptr || st.popup != &popup)
        return;
    const bool was_shown = popup.IsShown();
    // An xdg_popup cannot be moved once mapped: on Wayland a tooltip that changes is shown again.
    if (was_shown && on_wayland())
        popup.Hide();
    popup.set_content(st.tip.text, media);
    if (!popup.has_content()) {
        popup.Hide();
        st.phase = Phase::Empty;
        return;
    }
    place(popup, st);
    st.anchor = anchor_rect(st);
    popup.show_tip();
    st.phase      = Phase::Shown;
    st.last_shown = tip_now();
}

void on_wait_elapsed()
{
    State &st = state();
    if (st.phase != Phase::Waiting || st.owner == nullptr)
        return;
    if (st.button_down || in_use(st.owner)) {
        st.phase = Phase::Dismissed;
        return;
    }
    if (!st.owner->IsShownOnScreen()) {
        reset();
        return;
    }
    if (!st.hover_style)
        if (auto it = registry().find(st.owner); it != registry().end())
            suppress_native_tooltips(st.owner, it->second);
    show_now();
}

void on_leave_elapsed()
{
    if (state().leave_pending)
        reset();
}

// While a control is hovered: drop its tooltip when the control goes away without the pointer leaving
// it (its page switched, its window hidden, a list closed by a key), or moves (its window dragged by the
// title bar, which wx sees no click for), and keep native tooltips off it.
void on_watch()
{
    State &st = state();
    if (st.owner == nullptr) {
        if (st.popup != nullptr)
            st.popup->stop_timers();
        return;
    }
    if (!st.owner->IsShownOnScreen() || (st.phase == Phase::Shown && anchor_rect(st) != st.anchor)) {
        reset();
        return;
    }
    if (st.phase == Phase::Shown)
        st.last_shown = tip_now();
    if (!st.hover_style)
        if (auto it = registry().find(st.owner); it != registry().end())
            suppress_native_tooltips(st.owner, it->second);
}

// The pointer left `from` at `pos` (its client coordinates). Inside the hovered control still, it went
// onto a child of it, or from a child back onto the control: a composite widget (TextInput, ComboBox,
// SpinInput) is a frame with a text field in it. Otherwise it left, unless it is back within LEAVE_MS.
void pointer_left(wxWindow *from, const wxPoint &pos)
{
    State &st = state();
    if (st.owner == nullptr || !is_within(from, st.owner))
        return;
    const wxPoint p      = st.owner->ScreenToClient(from->ClientToScreen(pos));
    wxRect        region = st.hover_style ? st.item : wxRect(wxPoint(0, 0), st.owner->GetClientSize());
    if (region.Deflate(1).Contains(p))
        return;
    st.leave_pending = true;
    if (st.popup != nullptr)
        st.popup->check_leave();
    else
        reset();
}

void on_window_mouse(wxMouseEvent &event)
{
    event.Skip();
    auto *window = dynamic_cast<wxWindow *>(event.GetEventObject());
    if (window == nullptr)
        return;
    if (event.GetEventType() == wxEVT_LEAVE_WINDOW) {
        pointer_left(window, event.GetPosition());
        return;
    }

    State &st      = state();
    st.button_down = event.ButtonIsDown(wxMOUSE_BTN_ANY);
    wxWindow *owner = registered_owner(window);
    if (owner == nullptr)
        return;
    Registered &reg = registry()[owner];
    suppress_native_tooltips(owner, reg);
    if (st.owner == owner && !st.hover_style) {
        st.leave_pending = false;
        // After a click or a key the tooltip comes back once the pointer moves again, with the full
        // wait, as the 3D view's do; not while a text field in the control is being typed in.
        if (st.phase == Phase::Dismissed && !st.button_down && !in_use(owner))
            begin(owner, wxRect(), false, reg.tip);
        return;
    }
    begin(owner, wxRect(), false, reg.tip);
}

void on_window_destroy(wxWindowDestroyEvent &event)
{
    event.Skip();
    wxWindow *window = event.GetWindow();
    if (window == nullptr)
        return;
    tracked().erase(window);
    registry().erase(window);
    if (state().owner == window)
        reset();
}

bool same_media(const std::vector<TipMedia> &a, const std::vector<TipMedia> &b)
{
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](const TipMedia &x, const TipMedia &y) { return x.file == y.file && x.caption == y.caption; });
}

void set_tip(wxWindow *window, Tip tip)
{
    if (window == nullptr)
        return;
    State &st = state();
    if (tip.empty()) {
        registry().erase(window);
        if (st.owner == window && !st.hover_style)
            reset();
        return;
    }

    // Called again and again on the same window (on every edit, every preset update): bind once, swap
    // the text and pictures, and update the tooltip in place if it is on screen for this window.
    track(window, true);
    Registered &reg       = registry()[window];
    const bool  unchanged = !tip.source && !reg.tip.source && reg.base_text == tip.text && same_media(reg.tip.media, tip.media);
    reg.base_text         = tip.text;
    reg.tip               = std::move(tip);
    if (window->GetToolTip() != nullptr)
        window->UnsetToolTip();
    strip_descendant_tooltips(window);

    if (st.owner == window && !st.hover_style && !unchanged) {
        st.tip = reg.tip;
        if (st.phase == Phase::Shown)
            show_now();
        else if (st.phase == Phase::Empty)
            begin(window, wxRect(), false, reg.tip);
    }
}

void hover_tip(wxWindow *window, const wxRect &item, Tip tip)
{
    if (window == nullptr)
        return;
    State &st = state();
    if (item.IsEmpty() || tip.empty()) {
        if (st.owner == window)
            reset();
        return;
    }
    track(window, false);
    if (st.owner == window && st.hover_style && st.item == item && st.tip.text == tip.text) {
        st.leave_pending = false;
        st.tip           = std::move(tip);
        if (st.phase == Phase::Dismissed && !st.button_down && !in_use(window))
            begin(window, item, true, st.tip);
        return;
    }
    begin(window, item, true, std::move(tip));
}

} // namespace

void set_illustrated_tooltip(wxWindow *window, const wxString &text, const std::vector<TipMedia> &media)
{
    set_tip(window, Tip{text, media, {}});
}

void set_illustrated_tooltip_deferred(wxWindow *window, const wxString &text, TipMediaSource media)
{
    set_tip(window, Tip{text, {}, std::move(media)});
}

void hover_illustrated_tooltip(wxWindow *window, const wxRect &item, const wxString &text, const std::vector<TipMedia> &media)
{
    hover_tip(window, item, Tip{text, media, {}});
}

void hover_illustrated_tooltip_deferred(wxWindow *window, const wxRect &item, const wxString &text, TipMediaSource media)
{
    hover_tip(window, item, Tip{text, {}, std::move(media)});
}

void hide_illustrated_tooltip()
{
    reset();
    state().last_shown = -1e9;
}

void refresh_illustrated_tooltip(wxWindow *window)
{
    const State &st = state();
    if (window != nullptr && st.owner == window && st.phase == Phase::Shown)
        show_now();
}

wxString illustrated_tooltip_text(const wxWindow *window)
{
    const auto it = registry().find(const_cast<wxWindow *>(window));
    return it == registry().end() ? wxString() : it->second.tip.text;
}

bool is_illustrated_tooltip_shown_for(const wxWindow *window)
{
    const State &st = state();
    return window != nullptr && st.phase == Phase::Shown && st.owner != nullptr && st.popup != nullptr &&
           st.popup->IsShown() && is_within(st.owner, window);
}

} // namespace Slic3r::GUI
