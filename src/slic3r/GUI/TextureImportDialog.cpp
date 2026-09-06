#include <glad/gl.h>
#include "OpenGLManager.hpp"

#include "TextureImportDialog.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "MsgDialog.hpp"
#include "ColorDecomposeDialog.hpp"
#include "ColorDecomposeSupport.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/StaticLine.hpp"
#include "libslic3r/ColorDecomposeRecipe.hpp"
#include "libslic3r/MeshBoolean.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <wx/button.h>
#include <wx/colour.h>
#include <wx/colordlg.h>
#include <wx/dcclient.h>
#include <wx/dcbuffer.h>
#include <wx/display.h>
#include <wx/evtloop.h>
#include <wx/statline.h>
#include <wx/scrolwin.h>
#include <wx/msgdlg.h>
#include <wx/utils.h>
#include <wx/valtext.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <sstream>

#include <boost/log/trivial.hpp>

static constexpr const char* DEFAULT_VIRTUAL_FILAMENT_BASIC_TYPE = "PLA Basic";
static constexpr const char* DEFAULT_VIRTUAL_FILAMENT_SHORT_TYPE = "PLA";
static constexpr const char* DEFAULT_VIRTUAL_FILAMENT_NAME       = "Bambu PLA Basic";

static bool is_dark() { return Slic3r::GUI::wxGetApp().dark_mode(); }

static wxColour texture_import_gray9000()
{
    return wxColour(38, 46, 48);
}

static wxColour texture_import_text_colour()
{
    return StateColor::darkModeColorFor(texture_import_gray9000());
}

// StaticLine::SetLineColour stores the raw key and resolves it itself when it paints, so those
// sinks take SEPARATOR_COLOUR_KEY directly; only raw wx sinks need the resolved form below.
static constexpr const char* SEPARATOR_COLOUR_KEY = "#CECECE";

static wxColour texture_import_separator_colour()
{
    return StateColor::darkModeColorFor(wxColour(SEPARATOR_COLOUR_KEY));
}

// Orca's confirm palette, applied here rather than through Button::SetStyle because these buttons
// keep custom pill geometry that SetStyle resets. The Disabled entries are load-bearing: without
// one, StateColor::colorForStates falls through to the Normal entry and a disabled button paints
// as a live accent button.
static void apply_accent_button_colours(Button* btn)
{
    btn->SetBackgroundColor(StateColor(
        std::pair<wxColour, int>(wxColour("#CECECE"), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(0, 137, 123), StateColor::Pressed),
        std::pair<wxColour, int>(wxColour(38, 166, 154), StateColor::Hovered),
        std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal)));
    btn->SetBorderColor(StateColor(
        std::pair<wxColour, int>(wxColour("#CECECE"), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal)));
    btn->SetTextColor(StateColor(
        std::pair<wxColour, int>(wxColour("#6B6B6A"), StateColor::Disabled),
        std::pair<wxColour, int>(wxColour("#FFFFFE"), StateColor::Normal)));
}

// The same button while the parameters behind it are dirty: still clickable, but reading as
// "what you see is not what this button would apply".
static void apply_muted_button_colours(Button* btn)
{
    btn->SetBackgroundColor(wxColour("#CECECE"));
    btn->SetBorderColor(wxColour("#CECECE"));
    btn->SetTextColor(wxColour("#6B6B6A"));
}

static wxFont texture_import_section_title_font(wxWindow* win)
{
    wxFont font = win ? win->GetFont() : wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
    font.MakeBold();
    return font;
}

static wxSize gl_viewport_size(wxWindow* win, const wxSize& logical_size)
{
    wxSize viewport_size = logical_size;
#ifdef __APPLE__
    const double scale = win ? win->GetContentScaleFactor() : 1.0;
    if (scale > 0.0) {
        viewport_size.x = std::max(1, (int)std::round(viewport_size.x * scale));
        viewport_size.y = std::max(1, (int)std::round(viewport_size.y * scale));
    }
#else
    (void)win;
#endif
    return viewport_size;
}

class ScopedInteractiveBusyCursorSuspender
{
public:
    ScopedInteractiveBusyCursorSuspender()
    {
#if defined(__WXMSW__) || defined(__APPLE__)
        while (wxIsBusy()) {
            wxEndBusyCursor();
            ++m_suspended_count;
        }
#endif
    }

    ~ScopedInteractiveBusyCursorSuspender()
    {
#if defined(__WXMSW__) || defined(__APPLE__)
        for (int i = 0; i < m_suspended_count; ++i)
            wxBeginBusyCursor();
#endif
    }

private:
    int m_suspended_count = 0;
};

static bool needs_filament_swatch_border(const wxColour& colour)
{
    if (is_dark())
        return colour.Red() < 45 && colour.Green() < 45 && colour.Blue() < 45;
    return colour.Red() > 224 && colour.Green() > 224 && colour.Blue() > 224;
}

static wxColour filament_swatch_border_colour()
{
    return is_dark() ? wxColour(207, 207, 207) : wxColour(130, 130, 128);
}

static void draw_filament_swatch_border(wxDC& dc, const wxColour& colour,
                                        int x, int y, int w, int h, int radius = 0)
{
    if (!needs_filament_swatch_border(colour))
        return;

    dc.SetPen(wxPen(filament_swatch_border_colour(), 1));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    if (radius > 0)
        dc.DrawRoundedRectangle(x, y, w, h, radius);
    else
        dc.DrawRectangle(x, y, w, h);
}

static void draw_filament_swatch_ellipse_border(wxDC& dc, const wxColour& colour,
                                                int x, int y, int w, int h)
{
    if (!needs_filament_swatch_border(colour))
        return;

    dc.SetPen(wxPen(filament_swatch_border_colour(), 1));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawEllipse(x, y, w, h);
}

static wxString ellipsize_text(wxDC& dc, wxString text, int max_width)
{
    if (max_width <= 0)
        return wxEmptyString;
    if (dc.GetTextExtent(text).x <= max_width)
        return text;

    const wxString ellipsis = "...";
    while (!text.empty() && dc.GetTextExtent(text + ellipsis).x > max_width)
        text.RemoveLast();
    if (text.empty() && dc.GetTextExtent(ellipsis).x > max_width)
        return wxString();
    return text + ellipsis;
}

// ============================================================
// AccentSlider — thin track + accent-coloured triangle thumb
// ============================================================

class AccentSlider : public wxPanel {
public:
    AccentSlider(wxWindow* parent, int value, int minVal, int maxVal,
                 const wxPoint& pos = wxDefaultPosition,
                 const wxSize& size = wxDefaultSize);
    ~AccentSlider() override;
    int  GetValue() const;
    void SetValue(int val);
    bool Enable(bool enable = true) override;
private:
    void OnPaint(wxPaintEvent&);
    void OnMouse(wxMouseEvent&);
    int  xFromValue() const;
    int  valueFromX(int x) const;
    int  m_value, m_min, m_max;
    bool m_dragging = false;
};

AccentSlider::AccentSlider(wxWindow* parent, int value, int minVal, int maxVal,
                           const wxPoint& pos, const wxSize& size)
    : wxPanel(parent, wxID_ANY, pos, size.IsFullySpecified() ? size : wxSize(-1, parent->FromDIP(24)),
              wxTAB_TRAVERSAL | wxFULL_REPAINT_ON_RESIZE)
    , m_value(std::clamp(value, minVal, maxVal)), m_min(minVal), m_max(maxVal)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetMinSize(wxSize(-1, FromDIP(24)));

    Bind(wxEVT_PAINT,     &AccentSlider::OnPaint, this);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& evt) {
        evt.Skip();
        Refresh();
    });
    Bind(wxEVT_LEFT_DOWN, &AccentSlider::OnMouse, this);
    Bind(wxEVT_LEFT_UP,   &AccentSlider::OnMouse, this);
    Bind(wxEVT_MOTION,    &AccentSlider::OnMouse, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) { m_dragging = false; });
}

AccentSlider::~AccentSlider()
{
    // See MixedFilamentDialog::~MixedFilamentDialog: a widget destroyed while it
    // still holds the capture wedges mouse input for the whole application.
    if (HasCapture())
        ReleaseMouse();
}

int AccentSlider::GetValue() const { return m_value; }

void AccentSlider::SetValue(int val)
{
    val = std::clamp(val, m_min, m_max);
    if (val != m_value) { m_value = val; Refresh(); }
}

bool AccentSlider::Enable(bool enable)
{
    bool ok = wxPanel::Enable(enable);
    Refresh();
    return ok;
}

int AccentSlider::xFromValue() const
{
    wxSize sz = GetClientSize();
    int margin = FromDIP(6);
    int track_w = sz.x - 2 * margin;
    if (m_max <= m_min || track_w <= 0) return margin;
    return margin + (m_value - m_min) * track_w / (m_max - m_min);
}

int AccentSlider::valueFromX(int x) const
{
    wxSize sz = GetClientSize();
    int margin = FromDIP(6);
    int track_w = sz.x - 2 * margin;
    if (track_w <= 0 || m_max <= m_min) return m_min;
    int val = m_min + (x - margin) * (m_max - m_min) / track_w;
    return std::clamp(val, m_min, m_max);
}

void AccentSlider::OnPaint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    wxSize sz = GetClientSize();

    dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
    dc.Clear();

    int margin = FromDIP(6);
    int track_y = sz.y / 2;
    int ts = FromDIP(8);
    int pen_w = FromDIP(2);

    wxColour accent_clr = StateColor::darkModeColorFor(IsEnabled() ? wxColour("#009688") : wxColour("#ACACAC"));
    wxColour track_clr  = StateColor::darkModeColorFor(IsEnabled() ? wxColour("#CECECE") : wxColour("#DFDFDF"));

    int tx = xFromValue();

    dc.SetPen(wxPen(accent_clr, pen_w));
    dc.DrawLine(margin, track_y, tx, track_y);

    dc.SetPen(wxPen(track_clr, pen_w));
    dc.DrawLine(tx, track_y, sz.x - margin, track_y);

    wxPoint tri[3] = {
        {tx,          track_y + FromDIP(1)},
        {tx - ts / 2, track_y + FromDIP(1) + ts},
        {tx + ts / 2, track_y + FromDIP(1) + ts}
    };
    dc.SetBrush(wxBrush(accent_clr));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawPolygon(3, tri);
}

void AccentSlider::OnMouse(wxMouseEvent& evt)
{
    if (!IsEnabled()) return;

    auto update = [&](int x) {
        int nv = valueFromX(x);
        if (nv != m_value) {
            m_value = nv;
            Refresh();
            wxCommandEvent e(wxEVT_SLIDER, GetId());
            e.SetEventObject(this);
            ProcessWindowEvent(e);
        }
    };

    if (evt.LeftDown()) {
        m_dragging = true;
        if (!HasCapture()) CaptureMouse();
        update(evt.GetX());
    } else if (evt.LeftUp()) {
        m_dragging = false;
        if (HasCapture()) ReleaseMouse();
    } else if (evt.Dragging() && m_dragging) {
        update(evt.GetX());
    }
}

namespace Slic3r { namespace GUI {

wxDEFINE_EVENT(EVT_TEXTURE_COMPUTE_DONE, wxCommandEvent);
wxDEFINE_EVENT(EVT_TEXTURE_COMPUTE_PROGRESS, wxCommandEvent);
wxDEFINE_EVENT(EVT_TEXTURE_COMPUTE_ERROR, wxCommandEvent);
wxDEFINE_EVENT(EVT_TEXTURE_MESH_REPAIR_DECISION, wxCommandEvent);

static std::array<float, 4> parse_color_string(const std::string& hex)
{
    std::array<float, 4> c = {1.f, 1.f, 1.f, 1.f};
    if (hex.size() >= 7 && hex[0] == '#') {
        unsigned long val = std::strtoul(hex.c_str() + 1, nullptr, 16);
        c[0] = ((val >> 16) & 0xFF) / 255.f;
        c[1] = ((val >>  8) & 0xFF) / 255.f;
        c[2] = ((val      ) & 0xFF) / 255.f;
    }
    return c;
}

static wxString rgb_to_hex(const std::array<std::size_t, 3>& c)
{
    return wxString::Format("#%02X%02X%02X",
        (unsigned)c[0], (unsigned)c[1], (unsigned)c[2]);
}

static wxString filament_name_to_wx_string(const std::string& name)
{
    wxString utf8_name = wxString::FromUTF8(name.c_str());
    if (!utf8_name.empty() || name.empty())
        return utf8_name;
    return wxString(name);
}

static std::string texture_normalize_color_hex(std::string hex)
{
    if (hex.empty())
        return "#808080";
    if (hex.front() != '#')
        hex = "#" + hex;
    return decompose_normalize_color_hex(std::move(hex));
}

static std::string texture_rgba_to_hex(const std::array<float, 4>& rgba)
{
    return wxString::Format("#%02X%02X%02X",
        (unsigned char)std::clamp(rgba[0] * 255.f, 0.f, 255.f),
        (unsigned char)std::clamp(rgba[1] * 255.f, 0.f, 255.f),
        (unsigned char)std::clamp(rgba[2] * 255.f, 0.f, 255.f)).ToStdString();
}

static bool texture_entry_is_physical(TextureFilamentKind kind)
{
    return kind == TextureFilamentKind::ExistingPhysical || kind == TextureFilamentKind::NewPhysical;
}

static bool texture_entry_is_mixed(TextureFilamentKind kind)
{
    return kind == TextureFilamentKind::ExistingMixed || kind == TextureFilamentKind::NewMixed;
}

static bool texture_entry_is_pla_basic(const TextureFilamentEntry& entry)
{
    return entry.type == DEFAULT_VIRTUAL_FILAMENT_SHORT_TYPE || entry.type == DEFAULT_VIRTUAL_FILAMENT_BASIC_TYPE ||
           entry.name.find(DEFAULT_VIRTUAL_FILAMENT_BASIC_TYPE) != std::string::npos ||
           entry.preset_name.find(DEFAULT_VIRTUAL_FILAMENT_BASIC_TYPE) != std::string::npos;
}

static bool texture_entry_official_basic(const TextureFilamentEntry& entry)
{
    if (!texture_entry_is_physical(entry.kind))
        return false;
    // NewPhysical entries are created by add_virtual_filament with a fixed Bambu Basic name.
    if (entry.kind == TextureFilamentKind::NewPhysical)
        return !official_basic_type_from_preset_name(entry.name).empty();
    // ExistingPhysical: resolve the filament preset name from project_config_index.
    auto& pb = *wxGetApp().preset_bundle;
    const size_t cfg = entry.project_config_index;
    if (cfg < pb.filament_presets.size())
        return !official_basic_type_from_preset_name(pb.filament_presets[cfg]).empty();
    return false;
}

static Slic3r::ColorDecomposeRecipeMode texture_recipe_mode(TextureAutoMixMode mode)
{
    return mode == TextureAutoMixMode::CMYW ? Slic3r::ColorDecomposeRecipeMode::CMYW :
                                             Slic3r::ColorDecomposeRecipeMode::RYBW;
}

static bool starts_with_preset_name(const std::string& name, const char* prefix)
{
    const size_t prefix_len = std::strlen(prefix);
    return name.size() >= prefix_len && name.compare(0, prefix_len, prefix) == 0;
}

static std::string resolve_default_virtual_filament_preset_name()
{
    auto* preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle)
        return {};

    auto valid_preset_name = [preset_bundle](const std::string& name) -> bool {
        return !name.empty() && preset_bundle->filaments.find_preset(name, false) != nullptr;
    };

    const auto* default_profiles = preset_bundle->printers.get_selected_preset()
        .config.option<ConfigOptionStrings>("default_filament_profile");
    if (default_profiles) {
        for (const std::string& name : default_profiles->values) {
            if (starts_with_preset_name(name, DEFAULT_VIRTUAL_FILAMENT_NAME) && valid_preset_name(name))
                return name;
        }
    }

    for (const Preset& preset : preset_bundle->filaments.get_presets()) {
        if (preset.is_system && preset.is_visible && preset.is_compatible &&
            starts_with_preset_name(preset.name, DEFAULT_VIRTUAL_FILAMENT_NAME)) {
            return preset.name;
        }
    }

    for (const Preset& preset : preset_bundle->filaments.get_presets()) {
        if (preset.is_visible && preset.is_compatible &&
            starts_with_preset_name(preset.name, DEFAULT_VIRTUAL_FILAMENT_NAME)) {
            return preset.name;
        }
    }

    std::string selected = preset_bundle->filaments.get_selected_preset_name();
    return valid_preset_name(selected) ? selected : std::string();
}

static wxString auto_mix_mode_label(TextureAutoMixMode mode)
{
    return mode == TextureAutoMixMode::CMYW ? _L("One-click CMYW auto-mix") :
                                             _L("One-click RYBW auto-mix");
}

static wxPoint constrained_dialog_position(wxWindow* anchor, const wxSize& dialog_size)
{
    if (!anchor)
        return wxDefaultPosition;

    wxSize size = dialog_size;
    if (size.x <= 0 || size.y <= 0)
        size = wxSize(anchor->FromDIP(450), anchor->FromDIP(350));

    wxPoint pos = anchor->ClientToScreen(wxPoint(0, anchor->GetSize().y));
    wxRect display_rect;
    int display_idx = wxDisplay::GetFromPoint(pos);
    if (display_idx != wxNOT_FOUND)
        display_rect = wxDisplay(display_idx).GetClientArea();
    else
        display_rect = wxDisplay().GetClientArea();

    pos.x = std::clamp(pos.x, display_rect.GetLeft(),
                       std::max(display_rect.GetLeft(), display_rect.GetRight() - size.x));
    pos.y = std::clamp(pos.y, display_rect.GetTop(),
                       std::max(display_rect.GetTop(), display_rect.GetBottom() - size.y));
    return pos;
}

// ============================================================
// FilamentSelectPopup
// ============================================================

class FilamentSelectPopup : public PopupWindow
{
public:
    FilamentSelectPopup(wxWindow* parent,
                        const std::vector<TextureFilamentEntry>& entries,
                        const std::vector<std::array<float, 4>>& colors_rgba,
                        const std::vector<std::string>&          names,
                        size_t                                   existing_count,
                        int                                      popup_width,
                        wxWindow*                                dialog_anchor,
                        std::function<void(int)>                 on_select,
                        std::function<void(wxColour)>            on_add_filament,
                        std::function<void()>                    on_decompose_color,
                        std::function<bool()>                    can_add_filament,
                        std::function<void(bool)>                on_close,
                        std::vector<int>                         display_numbers)
        : PopupWindow(parent, wxBORDER_NONE | wxPU_CONTAINS_CONTROLS)
        , m_entries(entries)
        , m_colors_rgba(colors_rgba)
        , m_names(names)
        , m_existing_count(existing_count)
        , m_dialog_anchor(dialog_anchor)
        , m_on_select(std::move(on_select))
        , m_on_add_filament(std::move(on_add_filament))
        , m_on_decompose_color(std::move(on_decompose_color))
        , m_can_add_filament(std::move(can_add_filament))
        , m_on_close(std::move(on_close))
        , m_display_numbers(std::move(display_numbers))
    {
        wxColour pop_bg = StateColor::darkModeColorFor(*wxWHITE);
        SetBackgroundColour(pop_bg);

        m_content = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
        m_content->SetBackgroundColour(pop_bg);
        m_content->SetScrollRate(0, FromDIP(5));
        auto* outer = new wxBoxSizer(wxVERTICAL);

        const int pop_w   = std::max(FromDIP(213), popup_width);
        const int row_h   = FromDIP(32);
        const int pad     = FromDIP(8);
        const int max_visible_rows = 10;
        const wxColour header_clr = StateColor::darkModeColorFor(wxColour("#ACACAC"));

        auto add_section_header = [&](const wxString& label) {
            auto* hdr = new wxStaticText(m_content, wxID_ANY, label);
            wxFont hf = hdr->GetFont();
            hf.SetPointSize(9);
            hdr->SetFont(hf);
            hdr->SetForegroundColour(header_clr);
            outer->Add(hdr, 0, wxLEFT | wxRIGHT | wxTOP, pad);
            auto* line = new StaticLine(m_content);
            line->SetLineColour(wxColour(SEPARATOR_COLOUR_KEY));
            outer->Add(line, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, pad);
        };

        auto add_section = [&](const wxString& label, TextureFilamentKind kind) {
            bool has_any = false;
            for (const auto& entry : m_entries) {
                if (entry.kind == kind) {
                    has_any = true;
                    break;
                }
            }
            if (!has_any)
                return;
            add_section_header(label);
            for (const auto& entry : m_entries) {
                if (entry.kind != kind)
                    continue;
                wxPanel* row = texture_entry_is_mixed(entry.kind) ? create_mixed_item_row(entry, row_h)
                                                                  : create_item_row((size_t)entry.dialog_index, row_h);
                outer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);
            }
        };

        // Section order matches compute_display_numbers() so the visible IDs
        // ascend monotonically (ExistingPhysical -> NewPhysical -> ExistingMixed
        // -> NewMixed) instead of jumping (e.g. 1,2 -> 7 -> 3,4,5,6 -> 8,9,10).
        add_section(_L("Project Physical Filaments"), TextureFilamentKind::ExistingPhysical);
        add_section(_L("New Physical Filaments"), TextureFilamentKind::NewPhysical);
        add_section(_L("Project Mixed Filaments"), TextureFilamentKind::ExistingMixed);
        add_section(_L("New Mixed Filaments"), TextureFilamentKind::NewMixed);

        auto* decompose_label = new wxStaticText(this, wxID_ANY, _L("Decompose Color"));
        auto* add_label = new wxStaticText(this, wxID_ANY, _L("+ Add Material"));
        wxFont af = add_label->GetFont();
        af.SetPointSize(10);
        add_label->SetFont(af);
        decompose_label->SetFont(af);
        const bool add_enabled = !m_can_add_filament || m_can_add_filament();
        const wxColour action_clr = StateColor::darkModeColorFor(wxColour("#009688"));
        add_label->SetForegroundColour(add_enabled ? action_clr : header_clr);
        decompose_label->SetForegroundColour(add_enabled ? action_clr : header_clr);
        add_label->SetCursor(wxCursor(add_enabled ? wxCURSOR_HAND : wxCURSOR_ARROW));
        decompose_label->SetCursor(wxCursor(add_enabled ? wxCURSOR_HAND : wxCURSOR_ARROW));
        if (!add_enabled)
            add_label->SetToolTip(wxString::Format(
                _L("The project supports up to %d filaments. Extra filaments will be discarded."),
                (int)EnforcerBlockerType::ExtruderMax));
        decompose_label->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
            if (m_can_add_filament && !m_can_add_filament())
                return;
            auto on_decompose_color = m_on_decompose_color;
            m_closing_from_action = true;
            Dismiss();
            if (on_decompose_color)
                on_decompose_color();
        });
        add_label->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
            if (m_can_add_filament && !m_can_add_filament()) {
                return;
            }
            auto on_add_filament = m_on_add_filament;
            wxWindow* popup_parent = GetParent();
            wxWindow* color_anchor = m_dialog_anchor ? m_dialog_anchor : popup_parent;
            m_closing_from_action = true;
            Dismiss();
            wxColourData cd;
            cd.SetChooseFull(true);
            wxColourDialog dlg(popup_parent, &cd);
            auto move_color_dialog = [&dlg, color_anchor]() {
                dlg.Move(constrained_dialog_position(color_anchor, dlg.GetBestSize()));
            };
            dlg.Bind(wxEVT_SHOW, [move_color_dialog](wxShowEvent& e) mutable {
                e.Skip();
                if (e.IsShown())
                    move_color_dialog();
            });
            move_color_dialog();
            if (dlg.ShowModal() == wxID_OK) {
                wxColour clr = dlg.GetColourData().GetColour();
                if (on_add_filament) on_add_filament(clr);
            }
        });

        m_content->SetSizer(outer);
        m_content->FitInside();

        auto* top_sizer = new wxBoxSizer(wxVERTICAL);
        int list_h = outer->GetMinSize().y;
        if (m_colors_rgba.size() > max_visible_rows)
            list_h -= ((int)m_colors_rgba.size() - max_visible_rows) * row_h;
        m_content->SetMinSize(wxSize(pop_w, list_h));
        m_content->SetMaxSize(wxSize(pop_w, list_h));
        top_sizer->Add(m_content, 0, wxEXPAND);

        top_sizer->AddSpacer(FromDIP(4));
        auto* sep_line = new StaticLine(this);
        sep_line->SetLineColour(wxColour(SEPARATOR_COLOUR_KEY));
        top_sizer->Add(sep_line, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);
        top_sizer->Add(decompose_label, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, pad);
        auto* sep_line2 = new StaticLine(this);
        sep_line2->SetLineColour(wxColour(SEPARATOR_COLOUR_KEY));
        top_sizer->Add(sep_line2, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);
        top_sizer->Add(add_label, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, pad);
        SetSizerAndFit(top_sizer);

        SetSize(pop_w, top_sizer->GetMinSize().y);
    }

private:
    void OnDismiss() override
    {
        restore_cursor_state();
        if (m_on_close) m_on_close(m_closing_from_action);
        m_closing_from_action = false;
        wxPopupTransientWindow::OnDismiss();
        schedule_destroy();
    }

    void restore_cursor_state()
    {
        SetCursor(wxNullCursor);
        if (m_content)
            m_content->SetCursor(wxNullCursor);
        if (m_dialog_anchor)
            m_dialog_anchor->SetCursor(wxCursor(wxCURSOR_HAND));
        wxSetCursor(wxNullCursor);
    }

    void schedule_destroy()
    {
        if (m_destroy_scheduled)
            return;
        m_destroy_scheduled = true;
        CallAfter([this]() { Destroy(); });
    }

    wxPanel* create_item_row(size_t idx, int row_h)
    {
        wxColour row_bg    = StateColor::darkModeColorFor(*wxWHITE);
        wxColour hover_bg  = StateColor::darkModeColorFor(wxColour("#F4F4F4"));
        wxColour name_fg   = texture_import_text_colour();

        wxPanel* row = new wxPanel(m_content, wxID_ANY, wxDefaultPosition, wxSize(-1, row_h));
        row->SetBackgroundColour(row_bg);
        row->SetBackgroundStyle(wxBG_STYLE_PAINT);
        row->SetCursor(wxCursor(wxCURSOR_HAND));

        const int sq     = row->FromDIP(24);
        const int sq_r   = row->FromDIP(2);
        const int sq_x   = row->FromDIP(4);
        const int gap1   = row->FromDIP(8);

        wxColour fil_clr = idx < m_colors_rgba.size()
            ? wxColour((unsigned char)(m_colors_rgba[idx][0] * 255.f),
                       (unsigned char)(m_colors_rgba[idx][1] * 255.f),
                       (unsigned char)(m_colors_rgba[idx][2] * 255.f))
            : wxColour(128, 128, 128);

        wxString name_str = (idx < m_names.size()) ? filament_name_to_wx_string(m_names[idx])
                                                    : wxString::Format("Filament %d", display_number((int)idx));
        row->SetToolTip(name_str);

        row->Bind(wxEVT_PAINT, [this, idx, sq, sq_r, sq_x, gap1, fil_clr, name_str, row_bg, hover_bg, name_fg](wxPaintEvent& e) {
            auto* p = static_cast<wxPanel*>(e.GetEventObject());
            wxAutoBufferedPaintDC dc(p);
            wxSize sz = p->GetClientSize();

            bool hovered = (m_hover_idx == (int)idx);
            dc.SetBrush(wxBrush(hovered ? hover_bg : row_bg));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(0, 0, sz.x, sz.y);

            int sq_y = (sz.y - sq) / 2;
            wxColour paint_clr = fil_clr;
            if (idx < m_colors_rgba.size()) {
                paint_clr = wxColour((unsigned char)(m_colors_rgba[idx][0] * 255.f),
                                     (unsigned char)(m_colors_rgba[idx][1] * 255.f),
                                     (unsigned char)(m_colors_rgba[idx][2] * 255.f));
            }
            dc.SetBrush(wxBrush(paint_clr));
            dc.DrawRoundedRectangle(sq_x, sq_y, sq, sq, sq_r);
            draw_filament_swatch_border(dc, paint_clr, sq_x, sq_y, sq, sq, sq_r);

            {
                wxFont nf = p->GetFont();
                nf.SetPointSize(9);
                dc.SetFont(nf);
                dc.SetTextForeground(paint_clr.GetLuminance() < 0.6 ? *wxWHITE : texture_import_gray9000());
                wxString ns = wxString::Format("%d", display_number((int)idx));
                wxSize tsz = dc.GetTextExtent(ns);
                dc.DrawText(ns, sq_x + (sq - tsz.x) / 2, sq_y + (sq - tsz.y) / 2);
            }

            // Material name
            {
                wxFont mf = p->GetFont();
                mf.SetPointSize(10);
                dc.SetFont(mf);
                dc.SetTextForeground(name_fg);
                int tx = sq_x + sq + gap1;
                wxString display = ellipsize_text(dc, name_str, sz.x - tx - p->FromDIP(4));
                wxSize tsz = dc.GetTextExtent(display);
                if (!display.empty())
                    dc.DrawText(display, tx, (sz.y - tsz.y) / 2);
            }
        });

        row->Bind(wxEVT_MOTION, [this, idx](wxMouseEvent& evt) {
            if (m_hover_idx != (int)idx) {
                m_hover_idx = (int)idx;
                m_content->Refresh();
            }
            evt.Skip();
        });
        row->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& evt) {
            if (m_hover_idx != -1) {
                m_hover_idx = -1;
                m_content->Refresh();
            }
            evt.Skip();
        });

        row->Bind(wxEVT_LEFT_DOWN, [this, idx](wxMouseEvent&) {
            if (m_on_select) m_on_select((int)idx);
            m_closing_from_action = true;
            Dismiss();
        });

        return row;
    }

    wxPanel* create_mixed_item_row(const TextureFilamentEntry& entry, int row_h)
    {
        wxColour row_bg    = StateColor::darkModeColorFor(*wxWHITE);
        wxColour hover_bg  = StateColor::darkModeColorFor(wxColour("#F4F4F4"));
        wxColour name_fg   = texture_import_text_colour();
        const int idx = entry.dialog_index;

        wxPanel* row = new wxPanel(m_content, wxID_ANY, wxDefaultPosition, wxSize(-1, row_h));
        row->SetBackgroundColour(row_bg);
        row->SetBackgroundStyle(wxBG_STYLE_PAINT);
        row->SetCursor(wxCursor(wxCURSOR_HAND));
        row->SetToolTip(entry.name.empty() ? wxString::Format("Filament %d", display_number(idx)) : filament_name_to_wx_string(entry.name));

        row->Bind(wxEVT_PAINT, [this, entry, idx, row_bg, hover_bg, name_fg](wxPaintEvent& e) {
            auto* p = static_cast<wxPanel*>(e.GetEventObject());
            wxAutoBufferedPaintDC dc(p);
            wxSize sz = p->GetClientSize();
            const bool hovered = (m_hover_idx == idx);
            dc.SetBrush(wxBrush(hovered ? hover_bg : row_bg));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(0, 0, sz.x, sz.y);

            wxFont font = p->GetFont();
            font.SetPointSize(9);
            dc.SetFont(font);
            int x = p->FromDIP(2);
            const int sw = p->FromDIP(22);
            const int sw_r = p->FromDIP(2);
            const int y = (sz.y - sw) / 2;

            for (size_t ci = 0; ci < entry.mixed_components.size() && ci < entry.mixed_ratios.size(); ++ci) {
                if (ci > 0) {
                    dc.SetTextForeground(name_fg);
                    wxString plus = "+";
                    wxSize psz = dc.GetTextExtent(plus);
                    dc.DrawText(plus, x, (sz.y - psz.y) / 2);
                    x += psz.x + p->FromDIP(4);
                }

                const unsigned int comp_id = entry.mixed_components[ci];
                const int comp_dialog_idx = comp_id >= 1 ? (int)comp_id - 1 : -1;
                wxColour comp_clr("#D9D9D9");
                if (comp_dialog_idx >= 0 && comp_dialog_idx < (int)m_colors_rgba.size()) {
                    const auto& c = m_colors_rgba[comp_dialog_idx];
                    comp_clr = wxColour((unsigned char)(c[0] * 255.f),
                                        (unsigned char)(c[1] * 255.f),
                                        (unsigned char)(c[2] * 255.f));
                }

                dc.SetBrush(wxBrush(comp_clr));
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.DrawRoundedRectangle(x, y, sw, sw, sw_r);
                draw_filament_swatch_border(dc, comp_clr, x, y, sw, sw, sw_r);

                wxString num = wxString::Format("%d", display_number(comp_dialog_idx));
                wxSize nsz = dc.GetTextExtent(num);
                dc.SetTextForeground(comp_clr.GetLuminance() < 0.6 ? *wxWHITE : texture_import_gray9000());
                dc.DrawText(num, x + (sw - nsz.x) / 2, y + (sw - nsz.y) / 2);
                x += sw + p->FromDIP(4);

                dc.SetTextForeground(name_fg);
                wxString pct = wxString::Format("%d%%", entry.mixed_ratios[ci]);
                wxSize psz = dc.GetTextExtent(pct);
                dc.DrawText(pct, x, (sz.y - psz.y) / 2);
                x += psz.x + p->FromDIP(4);
            }
        });

        row->Bind(wxEVT_MOTION, [this, idx](wxMouseEvent& evt) {
            if (m_hover_idx != idx) {
                m_hover_idx = idx;
                m_content->Refresh();
            }
            evt.Skip();
        });
        row->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& evt) {
            if (m_hover_idx != -1) {
                m_hover_idx = -1;
                m_content->Refresh();
            }
            evt.Skip();
        });
        row->Bind(wxEVT_LEFT_DOWN, [this, idx](wxMouseEvent&) {
            if (m_on_select) m_on_select(idx);
            m_closing_from_action = true;
            Dismiss();
        });

        return row;
    }

    wxScrolledWindow*                         m_content = nullptr;
    std::vector<TextureFilamentEntry>          m_entries;
    std::vector<std::array<float, 4>>          m_colors_rgba;
    std::vector<std::string>                   m_names;
    size_t                                     m_existing_count = 0;
    wxWindow*                                  m_dialog_anchor = nullptr;
    std::function<void(int)>                   m_on_select;
    std::function<void(wxColour)>              m_on_add_filament;
    std::function<void()>                      m_on_decompose_color;
    std::function<bool()>                      m_can_add_filament;
    std::function<void(bool)>                  m_on_close;
    // 1-based display number per dialog_index, mirroring the post-apply
    // sidebar ordering (ExistingPhysical, NewPhysical, ExistingMixed, NewMixed).
    std::vector<int>                           m_display_numbers;
    int                                        m_hover_idx = -1;
    bool                                       m_closing_from_action = false;
    bool                                       m_destroy_scheduled = false;

    // Returns the display number for a dialog_index, falling back to idx + 1
    // when no mapping is available (e.g. index out of range).
    int display_number(int idx) const {
        return (idx >= 0 && idx < (int)m_display_numbers.size() && m_display_numbers[idx] > 0)
            ? m_display_numbers[idx] : idx + 1;
    }
};

// ============================================================
// AutoMixSelectPopup
// ============================================================

class AutoMixSelectPopup : public PopupWindow
{
public:
    AutoMixSelectPopup(wxWindow* parent,
                       TextureAutoMixMode current_mode,
                       int popup_width,
                       int font_point_size,
                       std::function<void(TextureAutoMixMode)> on_select,
                       std::function<void()> on_close)
        : PopupWindow(parent, wxBORDER_NONE | wxPU_CONTAINS_CONTROLS)
        , m_current_mode(current_mode)
        , m_font_point_size(font_point_size)
        , m_on_select(std::move(on_select))
        , m_on_close(std::move(on_close))
    {
        wxColour pop_bg = StateColor::darkModeColorFor(*wxWHITE);
        SetBackgroundColour(pop_bg);

        auto* content = new wxPanel(this, wxID_ANY);
        content->SetBackgroundColour(pop_bg);
        content->SetBackgroundStyle(wxBG_STYLE_PAINT);

        auto* sizer = new wxBoxSizer(wxVERTICAL);
        const int row_h = FromDIP(36);
        const int pop_w = std::max(FromDIP(216), popup_width);
        sizer->Add(create_item_row(content, TextureAutoMixMode::CMYW, row_h), 0, wxEXPAND);
        sizer->Add(create_item_row(content, TextureAutoMixMode::RYBW, row_h), 0, wxEXPAND);
        content->SetSizer(sizer);
        content->SetMinSize(wxSize(pop_w, row_h * 2));

        auto* top_sizer = new wxBoxSizer(wxVERTICAL);
        top_sizer->Add(content, 0, wxEXPAND | wxALL, FromDIP(4));
        SetSizerAndFit(top_sizer);
        SetSize(pop_w, top_sizer->GetMinSize().y);
    }

private:
    void OnDismiss() override
    {
        if (m_on_close)
            m_on_close();
        wxPopupTransientWindow::OnDismiss();
        CallAfter([this]() { Destroy(); });
    }

    wxPanel* create_item_row(wxWindow* parent, TextureAutoMixMode mode, int row_h)
    {
        wxColour row_bg   = StateColor::darkModeColorFor(*wxWHITE);
        wxColour hover_bg = StateColor::darkModeColorFor(wxColour("#F4F4F4"));
        wxColour text_fg  = texture_import_text_colour();
        wxColour accent   = StateColor::darkModeColorFor(wxColour("#009688"));

        wxPanel* row = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, row_h),
                                   wxTAB_TRAVERSAL | wxFULL_REPAINT_ON_RESIZE);
        row->SetBackgroundColour(row_bg);
        row->SetBackgroundStyle(wxBG_STYLE_PAINT);
        row->SetCursor(wxCursor(wxCURSOR_HAND));

        const int row_idx = mode == TextureAutoMixMode::CMYW ? 0 : 1;
        row->Bind(wxEVT_PAINT, [this, row_bg, hover_bg, text_fg, accent, mode, row_idx](wxPaintEvent& e) {
            auto* p = static_cast<wxPanel*>(e.GetEventObject());
            wxAutoBufferedPaintDC dc(p);
            wxSize sz = p->GetClientSize();
            const bool hovered = (m_hover_idx == row_idx);
            const bool selected = (m_current_mode == mode);

            dc.SetBrush(wxBrush(hovered ? hover_bg : row_bg));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(0, 0, sz.x, sz.y);

            wxFont font = p->GetFont();
            font.SetPointSize(m_font_point_size);
            dc.SetFont(font);
            dc.SetTextForeground(text_fg);
            wxString label = auto_mix_mode_label(mode);
            wxSize tsz = dc.GetTextExtent(label);
            dc.DrawText(label, p->FromDIP(12), (sz.y - tsz.y) / 2);

            if (selected) {
                wxFont check_font = p->GetFont();
                check_font.SetPointSize(12);
                check_font.MakeBold();
                dc.SetFont(check_font);
                dc.SetTextForeground(accent);
                wxString check = wxString::FromUTF8("✓");
                wxSize csz = dc.GetTextExtent(check);
                dc.DrawText(check, sz.x - p->FromDIP(16) - csz.x, (sz.y - csz.y) / 2);
            }
        });

        row->Bind(wxEVT_MOTION, [this, row, row_idx](wxMouseEvent& evt) {
            if (m_hover_idx != row_idx) {
                m_hover_idx = row_idx;
                row->Refresh();
            }
            evt.Skip();
        });
        row->Bind(wxEVT_LEAVE_WINDOW, [this, row](wxMouseEvent& evt) {
            m_hover_idx = -1;
            row->Refresh();
            evt.Skip();
        });
        row->Bind(wxEVT_LEFT_DOWN, [this, mode](wxMouseEvent&) {
            if (m_on_select)
                m_on_select(mode);
            Dismiss();
        });

        return row;
    }

    TextureAutoMixMode m_current_mode;
    int m_font_point_size = 10;
    int m_hover_idx = -1;
    std::function<void(TextureAutoMixMode)> m_on_select;
    std::function<void()> m_on_close;
};

// ============================================================
// TexturePreviewCanvas
// ============================================================

TexturePreviewCanvas::TexturePreviewCanvas(wxWindow* parent, const wxGLAttributes& attrs)
    : wxGLCanvas(parent, attrs, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE)
{
    m_context = new wxGLContext(this);

    Bind(wxEVT_PAINT, &TexturePreviewCanvas::on_paint, this);
    Bind(wxEVT_SIZE,  &TexturePreviewCanvas::on_size,  this);
    Bind(wxEVT_MOUSEWHEEL,  &TexturePreviewCanvas::on_mouse, this);
    Bind(wxEVT_LEFT_DOWN,   &TexturePreviewCanvas::on_mouse, this);
    Bind(wxEVT_LEFT_UP,     &TexturePreviewCanvas::on_mouse, this);
    Bind(wxEVT_RIGHT_DOWN,  &TexturePreviewCanvas::on_mouse, this);
    Bind(wxEVT_RIGHT_UP,    &TexturePreviewCanvas::on_mouse, this);
    Bind(wxEVT_MIDDLE_DOWN, &TexturePreviewCanvas::on_mouse, this);
    Bind(wxEVT_MIDDLE_UP,   &TexturePreviewCanvas::on_mouse, this);
    Bind(wxEVT_MOTION,      &TexturePreviewCanvas::on_mouse, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
        m_drag_mode = DragMode::None;
        m_reset_overlay_pressed = false;
    });
    Bind(wxEVT_LEAVE_WINDOW, &TexturePreviewCanvas::on_mouse, this);
}

TexturePreviewCanvas::~TexturePreviewCanvas()
{
    // See MixedFilamentDialog::~MixedFilamentDialog: a widget destroyed while it
    // still holds the capture wedges mouse input for the whole application.
    if (HasCapture())
        ReleaseMouse();

    if (m_context) {
        SetCurrent(*m_context);
        if (m_tex_id)
            glDeleteTextures(1, &m_tex_id);
        for (unsigned int id : m_gl_tex_ids)
            if (id) glDeleteTextures(1, &id);
        for (unsigned int id : {m_reset_icon_tex, m_reset_icon_hover_tex,
                                m_reset_icon_dark_tex, m_reset_icon_dark_hover_tex})
            if (id) glDeleteTextures(1, &id);
        delete m_context;
    }
}

void TexturePreviewCanvas::set_mesh_data(
    const std::vector<std::array<float, 3>>& vertices,
    const std::vector<std::array<int, 3>>&   indices)
{
    m_vertices = vertices;
    m_indices  = indices;
    update_bounding_box();
    compute_smooth_normals();
    Refresh();
}

void TexturePreviewCanvas::compute_smooth_normals()
{
    m_vertex_normals.clear();
    if (m_vertices.empty() || m_indices.empty()) return;

    m_vertex_normals.resize(m_vertices.size(), {0.f, 0.f, 0.f});

    for (const auto& face : m_indices) {
        int i0 = face[0], i1 = face[1], i2 = face[2];
        if (i0 < 0 || i0 >= (int)m_vertices.size() ||
            i1 < 0 || i1 >= (int)m_vertices.size() ||
            i2 < 0 || i2 >= (int)m_vertices.size())
            continue;

        const auto& v0 = m_vertices[i0];
        const auto& v1 = m_vertices[i1];
        const auto& v2 = m_vertices[i2];

        float nx = (v1[1]-v0[1])*(v2[2]-v0[2]) - (v1[2]-v0[2])*(v2[1]-v0[1]);
        float ny = (v1[2]-v0[2])*(v2[0]-v0[0]) - (v1[0]-v0[0])*(v2[2]-v0[2]);
        float nz = (v1[0]-v0[0])*(v2[1]-v0[1]) - (v1[1]-v0[1])*(v2[0]-v0[0]);

        m_vertex_normals[i0][0] += nx; m_vertex_normals[i0][1] += ny; m_vertex_normals[i0][2] += nz;
        m_vertex_normals[i1][0] += nx; m_vertex_normals[i1][1] += ny; m_vertex_normals[i1][2] += nz;
        m_vertex_normals[i2][0] += nx; m_vertex_normals[i2][1] += ny; m_vertex_normals[i2][2] += nz;
    }

    for (auto& n : m_vertex_normals) {
        float len = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
        if (len > 1e-8f) { n[0] /= len; n[1] /= len; n[2] /= len; }
    }
}

void TexturePreviewCanvas::set_texture_data(
    const std::vector<std::array<float, 2>>& uvs,
    const unsigned char* tex_data, int tex_w, int tex_h, int tex_channels)
{
    m_uvs = uvs;
    m_tex_w = tex_w;
    m_tex_h = tex_h;
    m_tex_channels = tex_channels;
    m_tex_dirty = true;

    size_t sz = (size_t)tex_w * tex_h * tex_channels;
    m_tex_data.assign(tex_data, tex_data + sz);
    Refresh();
}

void TexturePreviewCanvas::set_texture_render_data(
    const std::vector<std::vector<unsigned char>>& tex_pixels_rgb,
    const std::vector<int>& tex_widths,
    const std::vector<int>& tex_heights,
    const std::vector<std::array<std::array<float,2>, 3>>& face_uvs,
    const std::vector<int>& face_tex_ids)
{
    m_tex_pixels_rgb = tex_pixels_rgb;
    m_tex_widths     = tex_widths;
    m_tex_heights    = tex_heights;
    m_face_uvs       = face_uvs;
    m_face_tex_ids   = face_tex_ids;
    m_multi_tex_dirty = true;
    Refresh();
}

void TexturePreviewCanvas::upload_textures()
{
    if (!m_multi_tex_dirty) return;
    m_multi_tex_dirty = false;

    for (unsigned int id : m_gl_tex_ids)
        if (id) glDeleteTextures(1, &id);
    m_gl_tex_ids.clear();

    m_gl_tex_ids.resize(m_tex_pixels_rgb.size(), 0);
    for (size_t i = 0; i < m_tex_pixels_rgb.size(); ++i) {
        if (m_tex_pixels_rgb[i].empty() || m_tex_widths[i] <= 0 || m_tex_heights[i] <= 0)
            continue;
        GLuint tex_id = 0;
        glGenTextures(1, &tex_id);
        glBindTexture(GL_TEXTURE_2D, tex_id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, m_tex_widths[i], m_tex_heights[i],
                     0, GL_RGB, GL_UNSIGNED_BYTE, m_tex_pixels_rgb[i].data());
        m_gl_tex_ids[i] = tex_id;
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

void TexturePreviewCanvas::set_painted_mesh_data(
    const std::vector<std::array<float, 3>>& vertices,
    const std::vector<std::array<int, 3>>&   indices)
{
    m_painted_vertices = vertices;
    m_painted_indices  = indices;
    Refresh();
}

static void convert_face_colors(const std::vector<std::array<std::size_t, 3>>& src,
                                std::vector<std::array<float, 3>>& dst)
{
    dst.resize(src.size());
    for (size_t i = 0; i < src.size(); ++i)
        dst[i] = { src[i][0] / 255.f, src[i][1] / 255.f, src[i][2] / 255.f };
}

void TexturePreviewCanvas::set_face_colors(const std::vector<std::array<std::size_t, 3>>& face_colors)
{
    convert_face_colors(face_colors, m_face_colors_rgb);
    Refresh();
}

void TexturePreviewCanvas::set_original_face_colors(const std::vector<std::array<std::size_t, 3>>& face_colors)
{
    convert_face_colors(face_colors, m_original_face_colors_rgb);
    Refresh();
}

void TexturePreviewCanvas::set_filament_color_map(
    const std::map<std::array<std::size_t, 3>, std::array<float, 3>>& color_map)
{
    m_color_map = color_map;
    m_filament_colors_rgb.resize(m_face_colors_rgb.size());
    for (size_t i = 0; i < m_face_colors_rgb.size(); ++i) {
        std::array<std::size_t, 3> key = {
            (std::size_t)(m_face_colors_rgb[i][0] * 255.f + 0.5f),
            (std::size_t)(m_face_colors_rgb[i][1] * 255.f + 0.5f),
            (std::size_t)(m_face_colors_rgb[i][2] * 255.f + 0.5f)
        };
        auto it = color_map.find(key);
        if (it != color_map.end())
            m_filament_colors_rgb[i] = it->second;
        else
            m_filament_colors_rgb[i] = m_face_colors_rgb[i];
    }
    Refresh();
}

void TexturePreviewCanvas::set_render_mode(RenderMode mode)
{
    if (m_mode != mode) {
        m_mode = mode;
        Refresh();
    }
}

void TexturePreviewCanvas::set_computing_overlay(bool /*show*/)
{
    Refresh();
}

void TexturePreviewCanvas::reset_view()
{
    m_zoom  = 1.0f;
    m_rot_x = -30.0f;
    m_rot_y = 30.0f;
    m_pan_x = 0.0f;
    m_pan_y = 0.0f;
    Refresh();
}

wxRect TexturePreviewCanvas::reset_overlay_rect() const
{
    wxSize sz = GetClientSize();
    const int button_size = FromDIP(40);
    const int margin = FromDIP(20);
    return wxRect(
        std::max(margin, sz.x - button_size - margin),
        std::max(margin, sz.y - button_size - margin),
        button_size,
        button_size);
}

unsigned int TexturePreviewCanvas::upload_reset_icon_texture(const std::string& icon_name)
{
    wxBitmap bmp = create_scaled_bitmap(icon_name, this, 40);
    if (!bmp.IsOk())
        return 0;

    wxImage image = bmp.ConvertToImage();
    if (!image.IsOk())
        return 0;

    const int w = image.GetWidth();
    const int h = image.GetHeight();
    const unsigned char* rgb = image.GetData();
    const unsigned char* alpha = image.HasAlpha() ? image.GetAlpha() : nullptr;
    if (!rgb || w <= 0 || h <= 0)
        return 0;

    std::vector<unsigned char> rgba((size_t)w * h * 4);
    for (int i = 0; i < w * h; ++i) {
        rgba[(size_t)i * 4 + 0] = rgb[i * 3 + 0];
        rgba[(size_t)i * 4 + 1] = rgb[i * 3 + 1];
        rgba[(size_t)i * 4 + 2] = rgb[i * 3 + 2];
        rgba[(size_t)i * 4 + 3] = alpha ? alpha[i] : 255;
    }

    GLuint tex_id = 0;
    glGenTextures(1, &tex_id);
    glBindTexture(GL_TEXTURE_2D, tex_id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex_id;
}

void TexturePreviewCanvas::upload_reset_icon_textures()
{
    if (m_reset_icon_tex && m_reset_icon_hover_tex && m_reset_icon_dark_tex && m_reset_icon_dark_hover_tex)
        return;

    if (!m_reset_icon_tex)
        m_reset_icon_tex = upload_reset_icon_texture("canvas_zoom");
    if (!m_reset_icon_hover_tex)
        m_reset_icon_hover_tex = upload_reset_icon_texture("canvas_zoom_hover");
    if (!m_reset_icon_dark_tex)
        m_reset_icon_dark_tex = upload_reset_icon_texture("canvas_zoom_dark");
    if (!m_reset_icon_dark_hover_tex)
        m_reset_icon_dark_hover_tex = upload_reset_icon_texture("canvas_zoom_dark_hover");
}

bool TexturePreviewCanvas::handle_reset_overlay_mouse(wxMouseEvent& evt)
{
    if (evt.Leaving()) {
        if (m_reset_overlay_pressed) {
            m_reset_overlay_hovered = false;
            m_reset_overlay_pressed = false;
            SetCursor(wxCursor(wxCURSOR_ARROW));
            if (HasCapture())
                ReleaseMouse();
            Refresh();
            return true;
        }
        if (m_reset_overlay_hovered) {
            m_reset_overlay_hovered = false;
            SetCursor(wxCursor(wxCURSOR_ARROW));
            Refresh();
        }
        return false;
    }

    const bool over = reset_overlay_rect().Contains(evt.GetPosition());
    if (over != m_reset_overlay_hovered) {
        m_reset_overlay_hovered = over;
        SetCursor(wxCursor(over ? wxCURSOR_HAND : wxCURSOR_ARROW));
        Refresh();
    }

    if (m_drag_mode != DragMode::None && !m_reset_overlay_pressed)
        return false;

    if (evt.LeftDown() && over) {
        m_reset_overlay_pressed = true;
        if (!HasCapture())
            CaptureMouse();
        Refresh();
        return true;
    }

    if (evt.LeftUp() && m_reset_overlay_pressed) {
        const bool activate = over;
        m_reset_overlay_pressed = false;
        if (HasCapture())
            ReleaseMouse();
        if (activate)
            reset_view();
        else
            Refresh();
        return true;
    }

    return over;
}

void TexturePreviewCanvas::update_bounding_box()
{
    if (m_vertices.empty()) return;
    std::array<float, 3> mn = m_vertices[0], mx = m_vertices[0];
    for (const auto& v : m_vertices) {
        for (int i = 0; i < 3; ++i) {
            mn[i] = std::min(mn[i], v[i]);
            mx[i] = std::max(mx[i], v[i]);
        }
    }
    m_center = { (mn[0]+mx[0])/2, (mn[1]+mx[1])/2, (mn[2]+mx[2])/2 };
    float dx = mx[0]-mn[0], dy = mx[1]-mn[1], dz = mx[2]-mn[2];
    m_radius = std::sqrt(dx*dx + dy*dy + dz*dz) / 2.0f;
    if (m_radius < 1e-6f) m_radius = 1.0f;
}

void TexturePreviewCanvas::ensure_gl_ready()
{
    if (m_gl_initialized) return;

    // BBS loads the GL entry points here with GLEW; Orca loads them centrally in
    // OpenGLManager, so only check that this has already happened (glad leaves unresolved
    // entry points null) and drain any stale error state.
    if (glGetString == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "TexturePreviewCanvas: OpenGL functions are not loaded yet";
        return;
    }
    while (glGetError() != GL_NO_ERROR) {}

    m_gl_initialized = true;

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);

    GLfloat light_pos[]     = { 0.5f, 1.0f, 1.0f, 0.0f };
    GLfloat light_ambient[] = { 0.3f, 0.3f, 0.3f, 1.0f };
    GLfloat light_diffuse[] = { 0.8f, 0.8f, 0.8f, 1.0f };
    glLightfv(GL_LIGHT0, GL_POSITION, light_pos);
    glLightfv(GL_LIGHT0, GL_AMBIENT,  light_ambient);
    glLightfv(GL_LIGHT0, GL_DIFFUSE,  light_diffuse);
}

void TexturePreviewCanvas::on_paint(wxPaintEvent&)
{
    wxPaintDC dc(this);
    if (!m_context) return;
    SetCurrent(*m_context);
    ensure_gl_ready();
    render();
    SwapBuffers();
}

void TexturePreviewCanvas::on_size(wxSizeEvent&)
{
    Refresh();
}

void TexturePreviewCanvas::on_mouse(wxMouseEvent& evt)
{
    if (handle_reset_overlay_mouse(evt))
        return;

    if (evt.LeftDown()) {
        m_drag_mode = DragMode::Rotate;
        m_last_mouse_pos = evt.GetPosition();
        if (!HasCapture()) CaptureMouse();
    }
    else if (evt.LeftUp()) {
        if (m_drag_mode == DragMode::Rotate) {
            m_drag_mode = DragMode::None;
            if (HasCapture()) ReleaseMouse();
        }
    }
    else if (evt.RightDown()) {
        m_drag_mode = DragMode::Pan;
        m_last_mouse_pos = evt.GetPosition();
        if (!HasCapture()) CaptureMouse();
    }
    else if (evt.MiddleDown()) {
        m_drag_mode = DragMode::Pan;
        m_last_mouse_pos = evt.GetPosition();
        if (!HasCapture()) CaptureMouse();
    }
    else if (evt.RightUp() || evt.MiddleUp()) {
        if (m_drag_mode == DragMode::Pan) {
            m_drag_mode = DragMode::None;
            if (HasCapture()) ReleaseMouse();
        }
    }
    else if (evt.Dragging() && m_drag_mode != DragMode::None) {
        wxPoint pos = evt.GetPosition();
        float dx = (float)(pos.x - m_last_mouse_pos.x);
        float dy = (float)(pos.y - m_last_mouse_pos.y);

        if (m_drag_mode == DragMode::Rotate) {
            m_rot_y += dx * 0.5f;
            m_rot_x += dy * 0.5f;
            m_rot_x = std::max(-89.0f, std::min(89.0f, m_rot_x));
        } else if (m_drag_mode == DragMode::Pan) {
            wxSize sz = GetClientSize();
            if (sz.x > 0)
                m_pan_x += dx / (float)sz.x * m_radius * 2.0f / m_zoom;
            if (sz.y > 0)
                m_pan_y -= dy / (float)sz.y * m_radius * 2.0f / m_zoom;
        }

        m_last_mouse_pos = pos;
        Refresh();
    }
    else if (evt.GetWheelRotation() != 0) {
        float delta = evt.GetWheelRotation() > 0 ? 1.1f : 0.9f;
        m_zoom *= delta;
        m_zoom = std::max(0.1f, std::min(20.0f, m_zoom));
        Refresh();
    }
}

void TexturePreviewCanvas::render()
{
    wxSize sz = GetClientSize();
    if (sz.x <= 0 || sz.y <= 0) return;

    wxSize viewport_sz = gl_viewport_size(this, sz);
    glViewport(0, 0, viewport_sz.x, viewport_sz.y);
    // Same palette key as the preview container, so canvas and frame cannot drift apart.
    const wxColour clear_clr = StateColor::darkModeColorFor(wxColour("#EEEEEE"));
    glClearColor(clear_clr.Red() / 255.f, clear_clr.Green() / 255.f, clear_clr.Blue() / 255.f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    float aspect = (float)viewport_sz.x / (float)viewport_sz.y;
    float dist = m_radius * 3.0f / m_zoom;
    float near_plane = dist * 0.01f;
    float far_plane  = dist * 10.0f;
    float fov_rad    = 45.0f * static_cast<float>(M_PI) / 180.0f;
    float f          = 1.0f / std::tan(fov_rad / 2.0f);
    float proj[16]   = {};
    proj[0]  = f / aspect;
    proj[5]  = f;
    proj[10] = (far_plane + near_plane) / (near_plane - far_plane);
    proj[11] = -1.0f;
    proj[14] = (2.0f * far_plane * near_plane) / (near_plane - far_plane);
    glMultMatrixf(proj);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -dist);
    glTranslatef(m_pan_x, m_pan_y, 0.0f);
    glRotatef(m_rot_x, 1.0f, 0.0f, 0.0f);
    glRotatef(m_rot_y, 0.0f, 1.0f, 0.0f);
    glTranslatef(-m_center[0], -m_center[1], -m_center[2]);

    render_mesh();
    render_reset_overlay(sz, viewport_sz);
}

void TexturePreviewCanvas::render_reset_overlay(const wxSize& logical_size, const wxSize& viewport_size)
{
    if (logical_size.x <= 0 || logical_size.y <= 0 || viewport_size.x <= 0 || viewport_size.y <= 0)
        return;

    upload_reset_icon_textures();

    const unsigned int tex_id = is_dark()
        ? (m_reset_overlay_hovered ? m_reset_icon_dark_hover_tex : m_reset_icon_dark_tex)
        : (m_reset_overlay_hovered ? m_reset_icon_hover_tex : m_reset_icon_tex);
    if (!tex_id)
        return;

    wxRect rc = reset_overlay_rect();
    const float sx = (float)viewport_size.x / (float)logical_size.x;
    const float sy = (float)viewport_size.y / (float)logical_size.y;
    const float x0 = rc.GetLeft() * sx;
    const float y0 = rc.GetTop() * sy;
    const float x1 = (rc.GetLeft() + rc.GetWidth()) * sx;
    const float y1 = (rc.GetTop() + rc.GetHeight()) * sy;
    const float alpha = m_reset_overlay_hovered ? 1.0f : 0.78f;

    glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_TEXTURE_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex_id);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0, viewport_size.x, viewport_size.y, 0.0, -1.0, 1.0);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glColor4f(1.0f, 1.0f, 1.0f, alpha);
    glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(x0, y0);
        glTexCoord2f(1.0f, 0.0f); glVertex2f(x1, y0);
        glTexCoord2f(1.0f, 1.0f); glVertex2f(x1, y1);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(x0, y1);
    glEnd();

    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);

    glBindTexture(GL_TEXTURE_2D, 0);
    glPopAttrib();
}

void TexturePreviewCanvas::render_textured_original()
{
    if (m_vertices.empty() || m_indices.empty()) return;
    if (m_face_uvs.empty() || m_face_tex_ids.empty()) return;
    if (m_face_uvs.size() != m_indices.size()) return;

    upload_textures();

    const bool has_smooth = (m_vertex_normals.size() == m_vertices.size());

    // Group faces by texture id for batch rendering
    std::map<int, std::vector<size_t>> tex_groups;
    for (size_t fi = 0; fi < m_indices.size(); ++fi) {
        int tid = (fi < m_face_tex_ids.size()) ? m_face_tex_ids[fi] : -1;
        tex_groups[tid].push_back(fi);
    }

    glEnable(GL_LIGHTING);
    glColor3f(1.0f, 1.0f, 1.0f);

    for (const auto& [tid, face_list] : tex_groups) {
        bool tex_bound = false;
        if (tid >= 0 && tid < (int)m_gl_tex_ids.size() && m_gl_tex_ids[tid] != 0) {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, m_gl_tex_ids[tid]);
            tex_bound = true;
        } else {
            glDisable(GL_TEXTURE_2D);
        }

        glBegin(GL_TRIANGLES);
        for (size_t fi : face_list) {
            const auto& face = m_indices[fi];
            const auto& uvs  = m_face_uvs[fi];

            if (!tex_bound) {
                if (fi < m_original_face_colors_rgb.size())
                    glColor3fv(m_original_face_colors_rgb[fi].data());
                else
                    glColor3f(0.7f, 0.7f, 0.7f);
            }

            for (int vi = 0; vi < 3; ++vi) {
                int idx = face[vi];
                if (idx < 0 || idx >= (int)m_vertices.size()) continue;

                if (has_smooth) {
                    glNormal3fv(m_vertex_normals[idx].data());
                } else if (vi == 0) {
                    const auto& v0 = m_vertices[face[0]];
                    const auto& v1 = m_vertices[face[1]];
                    const auto& v2 = m_vertices[face[2]];
                    float nx = (v1[1]-v0[1])*(v2[2]-v0[2]) - (v1[2]-v0[2])*(v2[1]-v0[1]);
                    float ny = (v1[2]-v0[2])*(v2[0]-v0[0]) - (v1[0]-v0[0])*(v2[2]-v0[2]);
                    float nz = (v1[0]-v0[0])*(v2[1]-v0[1]) - (v1[1]-v0[1])*(v2[0]-v0[0]);
                    float len = std::sqrt(nx*nx + ny*ny + nz*nz);
                    if (len > 1e-8f) { nx /= len; ny /= len; nz /= len; }
                    glNormal3f(nx, ny, nz);
                }

                if (tex_bound)
                    glTexCoord2fv(uvs[vi].data());
                glVertex3fv(m_vertices[idx].data());
            }
        }
        glEnd();
    }

    glDisable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void TexturePreviewCanvas::render_mesh()
{
    if (m_vertices.empty() || m_indices.empty()) return;

    // Original mode with texture data: use proper texture mapping
    if (m_mode == RenderMode::Original && !m_face_uvs.empty()) {
        render_textured_original();
        return;
    }

    // For Multi-Color / FilamentMap, use the painted (remeshed) geometry if available;
    // the face color arrays match the painted mesh, not the original mesh.
    const bool use_painted = (m_mode != RenderMode::Original)
                             && !m_painted_vertices.empty()
                             && !m_painted_indices.empty();

    const auto& verts   = use_painted ? m_painted_vertices : m_vertices;
    const auto& faces   = use_painted ? m_painted_indices  : m_indices;

    const std::vector<std::array<float, 3>>* colors_ptr = nullptr;
    if (m_mode == RenderMode::Original && !m_original_face_colors_rgb.empty()
        && m_original_face_colors_rgb.size() == m_indices.size()) {
        colors_ptr = &m_original_face_colors_rgb;
    } else if (m_mode == RenderMode::FilamentMap && !m_filament_colors_rgb.empty()
               && m_filament_colors_rgb.size() == faces.size()) {
        colors_ptr = &m_filament_colors_rgb;
    } else if (!m_face_colors_rgb.empty() && m_face_colors_rgb.size() == faces.size()) {
        colors_ptr = &m_face_colors_rgb;
    }

    // Use smooth normals for the original mesh when available
    const bool has_smooth = !use_painted
                            && (m_vertex_normals.size() == m_vertices.size());

    glDisable(GL_TEXTURE_2D);
    glEnable(GL_LIGHTING);

    glBegin(GL_TRIANGLES);
    for (size_t fi = 0; fi < faces.size(); ++fi) {
        if (colors_ptr)
            glColor3fv((*colors_ptr)[fi].data());
        else
            glColor3f(0.7f, 0.7f, 0.7f);

        const auto& face = faces[fi];
        for (int vi = 0; vi < 3; ++vi) {
            int idx = face[vi];
            if (idx < 0 || idx >= (int)verts.size()) continue;

            if (has_smooth && idx < (int)m_vertex_normals.size()) {
                glNormal3fv(m_vertex_normals[idx].data());
            } else if (vi == 0) {
                const auto& v0 = verts[face[0]];
                const auto& v1 = verts[face[1]];
                const auto& v2 = verts[face[2]];
                float nx = (v1[1]-v0[1])*(v2[2]-v0[2]) - (v1[2]-v0[2])*(v2[1]-v0[1]);
                float ny = (v1[2]-v0[2])*(v2[0]-v0[0]) - (v1[0]-v0[0])*(v2[2]-v0[2]);
                float nz = (v1[0]-v0[0])*(v2[1]-v0[1]) - (v1[1]-v0[1])*(v2[0]-v0[0]);
                float len = std::sqrt(nx*nx + ny*ny + nz*nz);
                if (len > 1e-8f) { nx /= len; ny /= len; nz /= len; }
                glNormal3f(nx, ny, nz);
            }

            glVertex3fv(verts[idx].data());
        }
    }
    glEnd();
}


// ============================================================
// TextureImportDialog
// ============================================================

wxBEGIN_EVENT_TABLE(TextureImportDialog, DPIDialog)
    EVT_BUTTON(TextureImportDialog::ID_COLOR_4,    TextureImportDialog::on_color_preset_clicked)
    EVT_BUTTON(TextureImportDialog::ID_COLOR_8,    TextureImportDialog::on_color_preset_clicked)
    EVT_BUTTON(TextureImportDialog::ID_COLOR_16,   TextureImportDialog::on_color_preset_clicked)
    EVT_BUTTON(TextureImportDialog::ID_COLOR_AUTO,  TextureImportDialog::on_color_preset_clicked)
    EVT_BUTTON(TextureImportDialog::ID_BTN_APPLY,   TextureImportDialog::on_apply_clicked)
    EVT_BUTTON(TextureImportDialog::ID_BTN_SKIP,    TextureImportDialog::on_skip_clicked)
    EVT_BUTTON(wxID_OK,                              TextureImportDialog::on_ok_clicked)
wxEND_EVENT_TABLE()

TextureImportDialog::TextureImportDialog(
    wxWindow*                        parent,
    const Slic3r::TexturedMesh&      textured_mesh,
    const std::vector<TextureFilamentEntry>& filament_entries,
    std::function<bool()>            initial_cancel_callback,
    std::function<bool(int)>         initial_progress_callback)
    : DPIDialog(parent, wxID_ANY, _L("Import Model"),
                wxDefaultPosition, wxDefaultSize,
                (wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) & ~(wxMINIMIZE_BOX | wxMAXIMIZE_BOX))
    , m_textured_mesh(textured_mesh)
    , m_filament_entries(filament_entries)
    , m_initial_cancel_callback(std::move(initial_cancel_callback))
    , m_initial_progress_callback(std::move(initial_progress_callback))
{
    SetSize(wxSize(FromDIP(960), FromDIP(640)));

    m_filament_colors_rgba.reserve(m_filament_entries.size());
    m_filament_color_strs.reserve(m_filament_entries.size());
    m_filament_names.reserve(m_filament_entries.size());
    for (size_t i = 0; i < m_filament_entries.size(); ++i) {
        auto& entry = m_filament_entries[i];
        entry.dialog_index = (int)i;
        entry.color_hex = texture_normalize_color_hex(entry.color_hex);
        if (entry.name.empty())
            entry.name = "Filament " + std::to_string(i + 1);
        m_filament_color_strs.push_back(entry.color_hex);
        m_filament_names.push_back(entry.name);
        m_filament_colors_rgba.push_back(parse_color_string(entry.color_hex));
    }

    m_existing_filament_count = m_filament_colors_rgba.size();
    m_default_virtual_filament_preset_name = resolve_default_virtual_filament_preset_name();

    Bind(EVT_TEXTURE_COMPUTE_DONE,     &TextureImportDialog::on_computation_complete, this);
    Bind(EVT_TEXTURE_COMPUTE_PROGRESS, &TextureImportDialog::on_computation_progress, this);
    Bind(EVT_TEXTURE_COMPUTE_ERROR,    &TextureImportDialog::on_computation_error,    this);
    Bind(EVT_TEXTURE_MESH_REPAIR_DECISION, &TextureImportDialog::on_mesh_repair_decision_required, this);

    build_ui();
    SetMinSize(wxSize(FromDIP(800), FromDIP(500)));
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);

    m_preview_canvas->set_mesh_data(m_textured_mesh.vertices, m_textured_mesh.indices);

    // Pre-computed face colors (OBJ vertex colors / MTL face colors):
    // use them directly as the Original preview, skip texture decode.
    if (!m_textured_mesh.precomputed_face_colors.empty()) {
        m_preview_canvas->set_original_face_colors(m_textured_mesh.precomputed_face_colors);
    } else if (!m_textured_mesh.textures.empty()) {
        std::vector<std::vector<unsigned char>> tex_pixels_rgb;
        std::vector<int> tex_widths, tex_heights;
        tex_pixels_rgb.reserve(m_textured_mesh.textures.size());
        tex_widths.reserve(m_textured_mesh.textures.size());
        tex_heights.reserve(m_textured_mesh.textures.size());

        for (const auto& ti : m_textured_mesh.textures) {
            std::vector<unsigned char> bgr_pixels;
            int w = 0, h = 0;
            if (Slic3r::decode_texture_to_pixels(ti, bgr_pixels, w, h) && !bgr_pixels.empty()) {
                // Convert BGR to RGB for OpenGL
                for (size_t p = 0; p < bgr_pixels.size(); p += 3)
                    std::swap(bgr_pixels[p], bgr_pixels[p + 2]);
                tex_pixels_rgb.push_back(std::move(bgr_pixels));
            } else {
                tex_pixels_rgb.push_back({});
            }
            tex_widths.push_back(w);
            tex_heights.push_back(h);
        }

        const size_t nf = m_textured_mesh.indices.size();
        const bool has_mapping = !m_textured_mesh.material_texture_map.empty();

        // Build per-face UV array
        std::vector<std::array<std::array<float,2>, 3>> face_uvs(nf);
        for (size_t fi = 0; fi < nf; ++fi) {
            if (m_textured_mesh.has_face_uvs()) {
                const auto& ui = m_textured_mesh.uv_indices[fi];
                for (int vi = 0; vi < 3; ++vi) {
                    int idx = ui[vi];
                    if (idx >= 0 && static_cast<size_t>(idx) < m_textured_mesh.uv_coords.size())
                        face_uvs[fi][vi] = m_textured_mesh.uv_coords[idx];
                    else
                        face_uvs[fi][vi] = {0.f, 0.f};
                }
            } else if (!m_textured_mesh.uvs.empty()) {
                const auto& face = m_textured_mesh.indices[fi];
                for (int vi = 0; vi < 3; ++vi) {
                    int idx = face[vi];
                    if (idx >= 0 && static_cast<size_t>(idx) < m_textured_mesh.uvs.size())
                        face_uvs[fi][vi] = m_textured_mesh.uvs[idx];
                    else
                        face_uvs[fi][vi] = {0.f, 0.f};
                }
            }
        }

        // Build per-face texture index
        std::vector<int> face_tex_ids(nf, 0);
        for (size_t fi = 0; fi < nf; ++fi) {
            int mat_idx = (fi < m_textured_mesh.material_ids.size())
                          ? m_textured_mesh.material_ids[fi] : -1;
            if (has_mapping && mat_idx >= 0
                && static_cast<size_t>(mat_idx) < m_textured_mesh.material_texture_map.size())
                face_tex_ids[fi] = m_textured_mesh.material_texture_map[mat_idx];
            else if (!tex_pixels_rgb.empty())
                face_tex_ids[fi] = 0;
            else
                face_tex_ids[fi] = -1;
        }

        m_preview_canvas->set_texture_render_data(
            tex_pixels_rgb, tex_widths, tex_heights, face_uvs, face_tex_ids);

        // Still sample per-face colors as fallback
        std::vector<std::array<std::size_t, 3>> orig_colors;
        if (Slic3r::sample_original_face_colors(m_textured_mesh, orig_colors))
            m_preview_canvas->set_original_face_colors(orig_colors);
    }

    set_state(TextureImportState::Idle);
}

TextureImportDialog::~TextureImportDialog()
{
    dismiss_auto_mix_popup();
    dismiss_filament_popup();
    m_cancel_flag = true;
    if (m_worker && m_worker->joinable())
        m_worker->join();
}

int TextureImportDialog::ShowModal()
{
    if (m_state == TextureImportState::Idle && m_painted.face_colors.empty()) {
        start_computation(true, true);

        while (m_initial_computation_pending) {
            if (auto* event_loop = wxEventLoopBase::GetActive())
                event_loop->Yield();
            else
                wxYield();
            if (m_progress_dlg && m_progress_dlg->WasCancelled())
                m_cancel_flag = true;
            if (m_initial_cancel_callback && m_initial_cancel_callback())
                m_cancel_flag = true;
            wxMilliSleep(10);
        }

        if (m_worker && m_worker->joinable())
            m_worker->join();
        m_worker.reset();

        if (m_initial_computation_cancelled || m_initial_computation_failed)
            return wxID_CANCEL;
    }

    ScopedInteractiveBusyCursorSuspender busy_cursor_suspender;
    return DPIDialog::ShowModal();
}

void TextureImportDialog::build_ui()
{
    const wxColour dialog_bg = StateColor::darkModeColorFor(*wxWHITE);
    SetBackgroundColour(dialog_bg);
    SetForegroundColour(StateColor::darkModeColorFor(wxColour("#323A3D")));

    wxBoxSizer* root_sizer = new wxBoxSizer(wxVERTICAL);

    auto line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    line_top->SetBackgroundColour(texture_import_separator_colour());
    root_sizer->Add(line_top, 0, wxEXPAND);

    wxBoxSizer* main_sizer = new wxBoxSizer(wxHORIZONTAL);

    wxBoxSizer* left_sizer = new wxBoxSizer(wxVERTICAL);
    build_preview_panel(this, left_sizer);
    main_sizer->Add(left_sizer, 3, wxEXPAND | wxALL, FromDIP(8));

    wxBoxSizer* right_sizer = new wxBoxSizer(wxVERTICAL);
    build_params_panel(this, right_sizer);
    build_mapping_panel(this, right_sizer);
    build_bottom_buttons(right_sizer);
    main_sizer->Add(right_sizer, 2, wxEXPAND | wxALL, FromDIP(8));

    root_sizer->Add(main_sizer, 1, wxEXPAND);

    SetSizer(root_sizer);
    Layout();
    Bind(wxEVT_MOUSEWHEEL, &TextureImportDialog::dismiss_filament_popup_on_wheel, this);

#ifdef __WXMSW__
    wxPanel* size_grip_cover = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    size_grip_cover->SetBackgroundColour(dialog_bg);
    size_grip_cover->SetBackgroundStyle(wxBG_STYLE_COLOUR);

    auto update_size_grip_cover = [this, size_grip_cover]() {
        const int cover_size = FromDIP(20);
        wxSize client_size = GetClientSize();
        size_grip_cover->SetSize(client_size.x - cover_size, client_size.y - cover_size, cover_size, cover_size);
        size_grip_cover->Raise();
    };
    update_size_grip_cover();

    Bind(wxEVT_SIZE, [update_size_grip_cover](wxSizeEvent& e) {
        e.Skip();
        update_size_grip_cover();
    });
#endif
}

void TextureImportDialog::build_preview_panel(wxWindow* parent, wxSizer* sizer)
{
    wxColour preview_bg = StateColor::darkModeColorFor(wxColour("#EEEEEE"));
    wxColour preview_bd = texture_import_separator_colour();

    wxPanel* preview_container = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    preview_container->SetBackgroundColour(preview_bg);
    preview_container->SetBackgroundStyle(wxBG_STYLE_PAINT);
    preview_container->Bind(wxEVT_PAINT, [preview_bg, preview_bd](wxPaintEvent& e) {
        auto* p = static_cast<wxPanel*>(e.GetEventObject());
        wxAutoBufferedPaintDC dc(p);
        wxSize sz = p->GetClientSize();
        dc.SetBrush(wxBrush(p->GetParent()->GetBackgroundColour()));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, 0, sz.x, sz.y);
        dc.SetBrush(wxBrush(preview_bg));
        dc.SetPen(wxPen(preview_bd, 1));
        dc.DrawRoundedRectangle(0, 0, sz.x, sz.y, 4);
    });

    wxBoxSizer* container_sizer = new wxBoxSizer(wxVERTICAL);

    wxGLAttributes canvas_attrs;
    canvas_attrs.PlatformDefaults().RGBA().DoubleBuffer().Depth(24).EndList();
    m_preview_canvas = new TexturePreviewCanvas(preview_container, canvas_attrs);
    container_sizer->Add(m_preview_canvas, 1, wxEXPAND | wxALL, FromDIP(1));

    preview_container->SetSizer(container_sizer);
    sizer->Add(preview_container, 1, wxEXPAND);

    m_tab_panel = new wxPanel(preview_container, wxID_ANY);
    m_tab_panel->SetBackgroundColour(preview_bg);

    m_btn_view_original   = new Button(m_tab_panel, _L("Original"));
    m_btn_view_original->SetId(ID_VIEW_ORIGINAL);
    m_btn_view_multicolor = new Button(m_tab_panel, _L("Multi-Color"));
    m_btn_view_multicolor->SetId(ID_VIEW_MULTICOLOR);

    const int view_button_height = FromDIP(27);
    m_btn_view_original->SetCornerRadius(view_button_height / 2);
    m_btn_view_original->SetMinSize(wxSize(FromDIP(57), view_button_height));
    m_btn_view_original->SetFont(m_btn_view_original->GetFont().Bold());
    m_btn_view_original->SetToolTip(_L("Your input texture model"));
    m_btn_view_multicolor->SetCornerRadius(view_button_height / 2);
    m_btn_view_multicolor->SetMinSize(wxSize(FromDIP(57), view_button_height));
    m_btn_view_multicolor->SetFont(m_btn_view_multicolor->GetFont().Bold());
    m_btn_view_multicolor->SetToolTip(_L("Processed multi-color model"));

    wxBoxSizer* tab_sizer = new wxBoxSizer(wxHORIZONTAL);
    tab_sizer->Add(m_btn_view_original,   0, wxRIGHT, FromDIP(2));
    tab_sizer->Add(m_btn_view_multicolor, 0);
    m_tab_panel->SetSizer(tab_sizer);
    m_tab_panel->Fit();

    m_btn_view_multicolor->Hide();

    auto preview_original = [this](wxMouseEvent& e) {
        if (m_preview_canvas) {
            m_preview_canvas->set_render_mode(TexturePreviewCanvas::RenderMode::Original);
            highlight_view_button(0);
        }
        e.Skip();
    };
    auto preview_multicolor = [this](wxMouseEvent& e) {
        if (m_preview_canvas) {
            m_preview_canvas->set_render_mode(TexturePreviewCanvas::RenderMode::MultiColor);
            highlight_view_button(1);
        }
        e.Skip();
    };
    auto restore_filament_if_outside = [this](wxMouseEvent& e) {
        if (m_preview_canvas && m_tab_panel) {
            wxWindow* event_window = wxDynamicCast(e.GetEventObject(), wxWindow);
            wxPoint screen_pos = event_window ? event_window->ClientToScreen(e.GetPosition()) : wxGetMousePosition();
            wxPoint panel_pos = m_tab_panel->ScreenToClient(screen_pos);
            if (!m_tab_panel->GetClientRect().Contains(panel_pos)) {
                const bool mapping_ready = (m_state == TextureImportState::Ready);
                m_preview_canvas->set_render_mode(mapping_ready ? TexturePreviewCanvas::RenderMode::FilamentMap :
                                                            TexturePreviewCanvas::RenderMode::Original);
                highlight_view_button(-1);
            }
        }
        e.Skip();
    };

    m_btn_view_original->Bind(wxEVT_ENTER_WINDOW, preview_original);
    m_btn_view_multicolor->Bind(wxEVT_ENTER_WINDOW, preview_multicolor);
    m_btn_view_original->Bind(wxEVT_LEAVE_WINDOW, restore_filament_if_outside);
    m_btn_view_multicolor->Bind(wxEVT_LEAVE_WINDOW, restore_filament_if_outside);
    m_tab_panel->Bind(wxEVT_LEAVE_WINDOW, restore_filament_if_outside);

    auto update_preview_overlay_buttons = [this]() {
        if (m_tab_panel) {
            m_tab_panel->Fit();
            m_tab_panel->SetPosition(wxPoint(FromDIP(8), FromDIP(8)));
            m_tab_panel->Raise();
        }
    };

    preview_container->Bind(wxEVT_SIZE, [update_preview_overlay_buttons](wxSizeEvent& e) {
        e.Skip();
        update_preview_overlay_buttons();
    });
    update_preview_overlay_buttons();

    highlight_view_button(-1);
}

void TextureImportDialog::build_params_panel(wxWindow* parent, wxSizer* sizer)
{
    wxColour label_fg = StateColor::darkModeColorFor(wxColour("#323A3D"));

    wxBoxSizer* color_header_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText* lbl_colors = new wxStaticText(parent, wxID_ANY, _L("Color Count"));
    lbl_colors->SetForegroundColour(label_fg);
    lbl_colors->SetFont(lbl_colors->GetFont().Bold());
    color_header_sizer->Add(lbl_colors, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    m_btn_color_4    = new Button(parent, "4");
    m_btn_color_4->SetId(ID_COLOR_4);
    m_btn_color_8    = new Button(parent, "8");
    m_btn_color_8->SetId(ID_COLOR_8);
    m_btn_color_16   = new Button(parent, "16");
    m_btn_color_16->SetId(ID_COLOR_16);
    m_btn_color_auto = new Button(parent, _L("Auto"));
    m_btn_color_auto->SetId(ID_COLOR_AUTO);

    {
        StateColor preset_bg(
            std::pair<wxColour, int>(wxColour(0, 137, 123), StateColor::Pressed | StateColor::Checked),
            std::pair<wxColour, int>(wxColour(38, 166, 154), StateColor::Hovered | StateColor::Checked),
            std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Checked),
            std::pair<wxColour, int>(wxColour("#CECECE"), StateColor::Pressed),
            std::pair<wxColour, int>(wxColour("#EEEEEE"), StateColor::Hovered),
            std::pair<wxColour, int>(*wxWHITE, StateColor::Normal));
        StateColor preset_bd(
            std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Checked),
            std::pair<wxColour, int>(wxColour("#CECECE"), StateColor::Normal));
        StateColor preset_text(
            std::pair<wxColour, int>(wxColour("#FFFFFE"), StateColor::Checked),
            std::pair<wxColour, int>(wxColour("#323A3D"), StateColor::Normal));

        for (auto* btn : {m_btn_color_4, m_btn_color_8, m_btn_color_16}) {
            btn->SetCornerRadius(FromDIP(12));
            btn->SetMinSize(wxSize(FromDIP(28), FromDIP(28)));
            btn->SetBackgroundColor(preset_bg);
            btn->SetBorderColor(preset_bd);
            btn->SetTextColor(preset_text);
        }
    }

    update_color_count_preset_buttons();

    color_header_sizer->Add(m_btn_color_4,  0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(2));
    color_header_sizer->Add(m_btn_color_8,  0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(2));
    color_header_sizer->Add(m_btn_color_16, 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(color_header_sizer, 0, wxBOTTOM, FromDIP(4));

    wxBoxSizer* color_slider_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_color_slider = new AccentSlider(parent, m_param_color_count, 1, (int)max_filament_count());
    m_color_spin = new SpinInput(parent, wxString::Format("%d", m_param_color_count),
                                 wxEmptyString, wxDefaultPosition,
                                 wxSize(FromDIP(60), FromDIP(28)),
                                 wxTE_PROCESS_ENTER, 1, (int)max_filament_count(), m_param_color_count);

    m_color_slider->Bind(wxEVT_SLIDER, &TextureImportDialog::on_color_slider_changed, this);
    m_color_spin->Bind(wxEVT_SPINCTRL, &TextureImportDialog::on_color_spin_changed, this);
    m_color_spin->Bind(EVT_SPINCTRL_TEXT, &TextureImportDialog::on_color_spin_text_changed, this);

    color_slider_sizer->Add(m_color_slider, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    color_slider_sizer->Add(m_color_spin, 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(color_slider_sizer, 0, wxEXPAND | wxBOTTOM, FromDIP(8));

    wxStaticText* lbl_smooth = new wxStaticText(parent, wxID_ANY, _L("Smooth Level"));
    lbl_smooth->SetForegroundColour(label_fg);
    lbl_smooth->SetFont(lbl_smooth->GetFont().Bold());
    sizer->Add(lbl_smooth, 0, wxBOTTOM, FromDIP(4));

    wxBoxSizer* smooth_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_smooth_slider = new AccentSlider(parent, m_param_smooth, 0, 10);
    m_smooth_spin = new SpinInput(parent, wxString::Format("%d", m_param_smooth),
                                  wxEmptyString, wxDefaultPosition,
                                  wxSize(FromDIP(60), FromDIP(28)),
                                  wxTE_PROCESS_ENTER, 0, 10, m_param_smooth);

    m_smooth_slider->Bind(wxEVT_SLIDER, &TextureImportDialog::on_smooth_slider_changed, this);
    m_smooth_spin->Bind(wxEVT_SPINCTRL, &TextureImportDialog::on_smooth_spin_changed, this);
    m_smooth_spin->Bind(EVT_SPINCTRL_TEXT, &TextureImportDialog::on_smooth_spin_text_changed, this);

    smooth_sizer->Add(m_smooth_slider, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    smooth_sizer->Add(m_smooth_spin, 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(smooth_sizer, 0, wxEXPAND | wxBOTTOM, FromDIP(4));

    m_btn_apply = new Button(parent, _L("Apply"));
    m_btn_apply->SetId(ID_BTN_APPLY);

    {
        StateColor btn_bg_white(
            std::pair<wxColour, int>(wxColour("#CECECE"), StateColor::Pressed),
            std::pair<wxColour, int>(wxColour("#EEEEEE"), StateColor::Hovered),
            std::pair<wxColour, int>(*wxWHITE, StateColor::Normal));
        const wxColour btn_bd_accent = wxColour(0, 150, 136);
        const wxColour btn_text_accent = wxColour(0, 150, 136);

        m_btn_color_auto->SetCornerRadius(FromDIP(12));
        m_btn_color_auto->SetMinSize(wxSize(FromDIP(60), FromDIP(28)));
        m_btn_color_auto->SetBackgroundColor(btn_bg_white);
        m_btn_color_auto->SetBorderColor(btn_bd_accent);
        m_btn_color_auto->SetTextColor(btn_text_accent);

        m_btn_apply->SetCornerRadius(FromDIP(12));
        m_btn_apply->SetMinSize(wxSize(FromDIP(60), FromDIP(28)));
        m_btn_apply->SetBackgroundColor(btn_bg_white);
        m_btn_apply->SetBorderColor(btn_bd_accent);
        m_btn_apply->SetTextColor(btn_text_accent);
    }

    // Defer attaching the Auto/Apply tooltips until the dialog has actually
    // been shown. On macOS, AppKit creates NSTrackingArea and dispatches a
    // synthetic mouseEntered: as soon as the window first becomes visible,
    // which would otherwise pop the native tooltip without the user actually
    // hovering when the cursor happens to land on these buttons as the dialog
    // appears.
    Bind(wxEVT_SHOW, [this](wxShowEvent& e) {
        e.Skip();
        if (!e.IsShown() || m_initial_tooltips_set)
            return;
        m_initial_tooltips_set = true;
        CallAfter([this]() {
            if (m_btn_color_auto)
                m_btn_color_auto->SetToolTip(_L("Automatically determine the optimal color count only and recompute filament mapping"));
            if (m_btn_apply)
                m_btn_apply->SetToolTip(_L("Convert texture to painting using the specified color count and smooth level"));
        });
    });

    wxBoxSizer* apply_sizer = new wxBoxSizer(wxHORIZONTAL);
    apply_sizer->Add(m_btn_color_auto, 0, wxRIGHT, FromDIP(4));
    apply_sizer->Add(m_btn_apply, 0);
    sizer->Add(apply_sizer, 0, wxALIGN_RIGHT | wxBOTTOM, FromDIP(8));

    m_hint_label = new wxStaticText(parent, wxID_ANY,
        _L("Reminder: parameters changed, click Apply to take effect"));
    m_hint_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#FF6F00")));
    m_hint_label->SetFont(texture_import_section_title_font(parent));
    m_hint_label->Hide();
    sizer->Add(m_hint_label, 0, wxBOTTOM, FromDIP(4));

    auto* mapping_separator = new StaticLine(parent);
    mapping_separator->SetLineColour(wxColour(SEPARATOR_COLOUR_KEY));
    sizer->Add(mapping_separator, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
}

void TextureImportDialog::build_mapping_panel(wxWindow* parent, wxSizer* sizer)
{
    wxColour secondary_fg = StateColor::darkModeColorFor(wxColour("#6B6B6B"));

    wxBoxSizer* header_sizer = new wxBoxSizer(wxHORIZONTAL);

    wxStaticText* lbl_mapping = new wxStaticText(parent, wxID_ANY, _L("Filament Mapping"));
    lbl_mapping->SetForegroundColour(secondary_fg);
    lbl_mapping->SetFont(texture_import_section_title_font(parent));
    m_auto_mix_font_point_size = lbl_mapping->GetFont().GetPointSize();
    header_sizer->Add(lbl_mapping, 0, wxALIGN_CENTER_VERTICAL);

    m_btn_mix_reset = new Button(parent, "", "revert_btn", wxBORDER_NONE, 16);
    m_btn_mix_reset->SetCanFocus(false);
    m_btn_mix_reset->SetPaddingSize(wxSize(FromDIP(2), FromDIP(2)));
    {
        StateColor reset_bg(
            std::pair<wxColour, int>(wxColour("#F4F4F4"), StateColor::Pressed),
            std::pair<wxColour, int>(wxColour("#F8F8F8"), StateColor::Hovered),
            std::pair<wxColour, int>(*wxWHITE, StateColor::Normal));
        m_btn_mix_reset->SetBackgroundColor(reset_bg);
        m_btn_mix_reset->SetBorderColor(StateColor());
    }
    m_btn_mix_reset->SetToolTip(_L("Reset filament mapping to the state before one-click mixing"));
    m_btn_mix_reset->Bind(wxEVT_BUTTON, [this](wxCommandEvent& evt) {
        reset_auto_mix();
        evt.Skip();
    });
    m_btn_mix_reset->Hide();
    header_sizer->Add(m_btn_mix_reset, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));

    header_sizer->AddStretchSpacer();

    m_btn_auto_mix = new Button(parent, auto_mix_mode_label(m_auto_mix_mode));
    {
        wxFont btn_font = m_btn_auto_mix->GetFont();
        btn_font.SetPointSize(m_auto_mix_font_point_size);
        m_btn_auto_mix->SetFont(btn_font);
    }
    m_btn_auto_mix->SetCornerRadius(FromDIP(14));
    m_btn_auto_mix->SetMinSize(wxSize(FromDIP(178), FromDIP(28)));
    {
        StateColor btn_bg(
            std::pair<wxColour, int>(wxColour("#F4F4F4"), StateColor::Pressed),
            std::pair<wxColour, int>(wxColour("#F8F8F8"), StateColor::Hovered),
            std::pair<wxColour, int>(*wxWHITE, StateColor::Normal));
        const wxColour btn_bd = wxColour("#CECECE");
        const wxColour btn_text = texture_import_gray9000();
        m_btn_auto_mix->SetBackgroundColor(btn_bg);
        m_btn_auto_mix->SetBorderColor(btn_bd);
        m_btn_auto_mix->SetTextColor(btn_text);
    }
    m_btn_auto_mix->SetToolTip(_L("Choose the one-click auto-mix mode for texture color import"));
    m_btn_auto_mix->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& evt) {
        show_auto_mix_popup();
        evt.Skip();
    });
    m_btn_auto_mix->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& evt) {
        show_auto_mix_popup();
        evt.Skip();
    });
    header_sizer->Add(m_btn_auto_mix, 0, wxALIGN_CENTER_VERTICAL);

    sizer->Add(header_sizer, 0, wxEXPAND | wxBOTTOM, FromDIP(4));

    wxBoxSizer* merge_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_auto_merge_cb = new wxCheckBox(parent, wxID_ANY, _L("Auto-merge same filament"));
    m_auto_merge_cb->SetToolTip(_L("Automatically merge identical filaments into existing filaments in the project"));
    m_auto_merge_cb->SetForegroundColour(secondary_fg);
    m_auto_merge_cb->SetValue(true);
    m_auto_merge_cb->Bind(wxEVT_CHECKBOX, &TextureImportDialog::on_auto_merge_toggled, this);
    merge_sizer->Add(m_auto_merge_cb, 0, wxALIGN_CENTER_VERTICAL);

    sizer->Add(merge_sizer, 0, wxEXPAND | wxBOTTOM, FromDIP(8));

    m_mapping_scroll = new wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition,
                                             wxSize(-1, FromDIP(300)));
    m_mapping_scroll->SetScrollRate(0, FromDIP(10));
    m_mapping_scroll->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    m_mapping_scroll->Bind(wxEVT_MOUSEWHEEL, &TextureImportDialog::dismiss_filament_popup_on_wheel, this);

    m_mapping_sizer = new wxBoxSizer(wxVERTICAL);
    m_mapping_scroll->SetSizer(m_mapping_sizer);

    sizer->Add(m_mapping_scroll, 1, wxEXPAND | wxBOTTOM, FromDIP(8));
}

void TextureImportDialog::build_bottom_buttons(wxSizer* sizer)
{
    m_drop_warning_label = new wxStaticText(this, wxID_ANY,
        wxString::Format(
            _L("The project supports up to %d filaments. Extra filaments will be discarded."),
            (int)max_filament_count()));
    m_drop_warning_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#FF6F00")));
    m_drop_warning_label->SetFont(texture_import_section_title_font(this));
    m_drop_warning_label->Hide();
    sizer->Add(m_drop_warning_label, 0, wxALIGN_LEFT | wxBOTTOM, FromDIP(4));

    wxBoxSizer* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_btn_skip = new Button(this, _L("Skip Matching"));
    m_btn_skip->SetId(ID_BTN_SKIP);
    m_btn_skip->SetToolTip(_L("Skip filament mapping and import as a single-color model"));
    m_btn_skip->SetCornerRadius(FromDIP(20));
    m_btn_skip->SetMinSize(wxSize(FromDIP(136), FromDIP(40)));
    {
        StateColor skip_bg(
            std::pair<wxColour, int>(wxColour("#CECECE"), StateColor::Pressed),
            std::pair<wxColour, int>(wxColour("#EEEEEE"), StateColor::Hovered),
            std::pair<wxColour, int>(*wxWHITE, StateColor::Normal));
        const wxColour skip_bd = wxColour("#CECECE");
        const wxColour skip_text = wxColour("#6B6B6A");
        m_btn_skip->SetBackgroundColor(skip_bg);
        m_btn_skip->SetBorderColor(skip_bd);
        m_btn_skip->SetTextColor(skip_text);
    }

    m_btn_ok = new Button(this, _L("Confirm"));
    m_btn_ok->SetId(wxID_OK);
    m_btn_ok->SetCornerRadius(FromDIP(20));
    m_btn_ok->SetMinSize(wxSize(FromDIP(156), FromDIP(40)));
    apply_accent_button_colours(m_btn_ok);

    btn_sizer->AddStretchSpacer();
    btn_sizer->Add(m_btn_skip, 0, wxRIGHT, FromDIP(16));
    btn_sizer->Add(m_btn_ok, 0);

    sizer->Add(btn_sizer, 0, wxEXPAND | wxTOP, FromDIP(8));
}

// ---- State machine ----

void TextureImportDialog::set_state(TextureImportState new_state)
{
    m_state = new_state;
    update_ui_for_state();
}

void TextureImportDialog::update_ui_for_state()
{
    bool computing = (m_state == TextureImportState::Computing);
    bool ready     = (m_state == TextureImportState::Ready);
    bool idle      = (m_state == TextureImportState::Idle);
    bool valid     = has_valid_result();

    m_color_slider->Enable(!computing);
    m_color_spin->Enable(!computing);
    m_smooth_slider->Enable(!computing);
    m_smooth_spin->Enable(!computing);
    m_btn_apply->Enable(!computing);
    m_btn_color_4->Enable(!computing);
    m_btn_color_8->Enable(!computing);
    m_btn_color_16->Enable(!computing);
    m_btn_color_auto->Enable(!computing);
    if (m_btn_auto_mix)
        m_btn_auto_mix->Enable(!computing);
    if (m_btn_mix_reset)
        m_btn_mix_reset->Enable(!computing);
    if (computing)
        dismiss_auto_mix_popup();

    m_btn_ok->Enable(ready && valid);
    m_btn_skip->Enable(ready || idle);

    m_auto_merge_cb->Enable(!computing);

    m_preview_canvas->set_computing_overlay(computing);

    if (ready && valid)
        style_confirm_button(is_params_dirty());
    else if (m_hint_label)
        m_hint_label->Hide();

    m_btn_ok->Refresh();
    Layout();
}

// ---- Async computation ----

void TextureImportDialog::start_computation(bool auto_color, bool initial)
{
    cancel_computation();

    m_cancel_flag = false;
    m_current_computation_initial = initial;
    m_current_computation_auto_color = auto_color;
    if (initial) {
        m_initial_computation_pending = true;
        m_initial_computation_cancelled = false;
        m_initial_computation_failed = false;
    }
    set_state(TextureImportState::Computing);

    bool silent_initial = initial && static_cast<bool>(m_initial_cancel_callback);
    if (!silent_initial) {
        m_progress_dlg = new ProgressDialog(
            _L("Processing"), _L("Computing texture colors..."),
            100, initial ? GetParent() : this, wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_AUTO_HIDE);
    }

    Slic3r::TexturePaintingSettings settings;
    settings.target_colors_num = auto_color ? 0 : (size_t)m_param_color_count;
    settings.smooth_weight     = m_param_smooth / 10.0;
    settings.mesh_repair_decision = m_mesh_repair_decision;
    // BBS repairs the mesh through the Windows 3D SDK, which is only available on Windows
    // builds that ship the SDK. Orca's CGAL-based repair (MeshBoolean::cgal::repair) works
    // on all three platforms, so use that instead.
    settings.mesh_repair_callback = [](const indexed_triangle_set& mesh,
                                       indexed_triangle_set&       repaired_mesh,
                                       std::function<void(const char*, unsigned)> progress_callback,
                                       std::function<bool()>       cancel_callback,
                                       std::string*                error_message) -> bool {
        if (cancel_callback && cancel_callback())
            return false;
        if (progress_callback)
            progress_callback(_u8L("Repairing mesh").c_str(), 0);

        TriangleMesh tm(mesh);
        if (!MeshBoolean::cgal::repair(tm, nullptr, error_message))
            return false;

        if (cancel_callback && cancel_callback())
            return false;
        repaired_mesh = tm.its;
        if (progress_callback)
            progress_callback(_u8L("Repairing mesh").c_str(), 100);
        return true;
    };

    Slic3r::TexturedMesh mesh_copy = m_textured_mesh;
    wxEvtHandler* handler = this;

    m_worker = std::make_unique<std::thread>([this, settings, mesh_copy, handler]() {
        Slic3r::PaintedMesh result;

        auto progress_cb = [handler](int percent, const char*) {
            auto* evt = new wxCommandEvent(EVT_TEXTURE_COMPUTE_PROGRESS);
            evt->SetInt(percent);
            wxQueueEvent(handler, evt);
        };

        auto cancel_cb = [this]() -> bool {
            return m_cancel_flag.load();
        };

        auto worker_settings = settings;
        bool mesh_repair_decision_required = false;
        worker_settings.mesh_repair_decision_required = &mesh_repair_decision_required;
        bool ok;
        if (!mesh_copy.precomputed_face_colors.empty()) {
            ok = Slic3r::face_colors_to_painting(
                mesh_copy, result, worker_settings, progress_cb, cancel_cb);
        } else {
            ok = Slic3r::texture_to_painting(mesh_copy, result, worker_settings, progress_cb, cancel_cb);
        }

        if (m_cancel_flag.load()) {
            wxQueueEvent(handler, new wxCommandEvent(EVT_TEXTURE_COMPUTE_ERROR));
            return;
        }

        if (!ok && mesh_repair_decision_required) {
            wxQueueEvent(handler, new wxCommandEvent(EVT_TEXTURE_MESH_REPAIR_DECISION));
            return;
        }

        {
            std::lock_guard<std::mutex> lock(m_result_mutex);
            m_pending_result = std::move(result);
        }

        if (ok) {
            wxQueueEvent(handler, new wxCommandEvent(EVT_TEXTURE_COMPUTE_DONE));
        } else {
            wxQueueEvent(handler, new wxCommandEvent(EVT_TEXTURE_COMPUTE_ERROR));
        }
    });
}

void TextureImportDialog::cancel_computation()
{
    m_cancel_flag = true;
    if (m_worker && m_worker->joinable())
        m_worker->join();
    m_worker.reset();

    if (m_progress_dlg) {
        m_progress_dlg->Destroy();
        m_progress_dlg = nullptr;
    }

    if (m_current_computation_initial) {
        m_initial_computation_cancelled = true;
        m_initial_computation_pending = false;
        m_current_computation_initial = false;
    }
}

void TextureImportDialog::on_computation_progress(wxCommandEvent& evt)
{
    if (m_progress_dlg) {
        if (!m_progress_dlg->Update(evt.GetInt()))
            m_cancel_flag = true;
    } else if (m_current_computation_initial && m_initial_progress_callback) {
        if (!m_initial_progress_callback(evt.GetInt()))
            m_cancel_flag = true;
    }
}

void TextureImportDialog::on_computation_complete(wxCommandEvent&)
{
    bool initial = m_current_computation_initial;

    if (m_progress_dlg) {
        m_progress_dlg->Destroy();
        m_progress_dlg = nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(m_result_mutex);
        m_painted = std::move(m_pending_result);
    }

    int actual_colors = (int)m_painted.cluster_colors.size();
    if (actual_colors >= 2 && actual_colors <= (int)max_filament_count()) {
        set_color_count_value(actual_colors, true);
    }

    m_preview_canvas->set_painted_mesh_data(m_painted.vertices, m_painted.indices);
    m_preview_canvas->set_face_colors(m_painted.face_colors);

    // A fresh texture computation replaces m_painted, so virtual filaments from
    // the previous computation must not consume capacity when deciding whether
    // this run drops extra colors. Rebuild virtual filaments from this result.
    m_current_matches.clear();
    if (m_filament_colors_rgba.size() > m_existing_filament_count)
        m_filament_colors_rgba.resize(m_existing_filament_count);
    if (m_filament_color_strs.size() > m_existing_filament_count)
        m_filament_color_strs.resize(m_existing_filament_count);
    if (m_filament_names.size() > m_existing_filament_count)
        m_filament_names.resize(m_existing_filament_count);
    if (m_filament_entries.size() > m_existing_filament_count)
        m_filament_entries.resize(m_existing_filament_count);
    while (m_filament_entries.size() < m_existing_filament_count) {
        TextureFilamentEntry entry;
        entry.kind = TextureFilamentKind::ExistingPhysical;
        entry.dialog_index = (int)m_filament_entries.size();
        entry.project_config_index = m_filament_entries.size();
        m_filament_entries.push_back(entry);
    }
    for (size_t i = 0; i < m_filament_entries.size(); ++i) {
        m_filament_entries[i].dialog_index = (int)i;
        m_filament_entries[i].color_hex = i < m_filament_color_strs.size() ?
            texture_normalize_color_hex(m_filament_color_strs[i]) : "#808080";
        m_filament_entries[i].name = i < m_filament_names.size() ?
            m_filament_names[i] : "Filament " + std::to_string(i + 1);
    }
    m_new_filament_colors.clear();
    m_new_filament_preset_names.clear();
    m_new_mixed_filaments.clear();

    do_auto_match();
    compact_used_virtual_filaments();
    sort_current_matches_by_filament_index();
    update_filament_color_map();
    rebuild_mapping_rows();

    m_applied_color_count = m_param_color_count;
    m_applied_smooth      = m_param_smooth;

    set_state(TextureImportState::Ready);
    update_drop_warning_visibility();
    update_auto_mix_reset_visibility();

    m_btn_view_multicolor->Show();
    if (m_tab_panel) {
        m_tab_panel->GetSizer()->Layout();
        m_tab_panel->Fit();
        m_tab_panel->SetPosition(wxPoint(FromDIP(8), FromDIP(8)));
    }
    GetSizer()->Layout();

    m_preview_canvas->set_render_mode(TexturePreviewCanvas::RenderMode::FilamentMap);
    highlight_view_button(-1);

    if (initial) {
        m_initial_computation_pending = false;
        m_current_computation_initial = false;
    }
}

void TextureImportDialog::on_computation_error(wxCommandEvent&)
{
    bool initial = m_current_computation_initial;

    if (m_progress_dlg) {
        m_progress_dlg->Destroy();
        m_progress_dlg = nullptr;
    }

    if (m_cancel_flag.load()) {
        if (initial) {
            m_initial_computation_cancelled = true;
            m_initial_computation_pending = false;
            m_current_computation_initial = false;
            return;
        }
        if (has_valid_result()) {
            if (m_applied_color_count >= 0) {
                m_param_color_count = m_applied_color_count;
                m_color_slider->SetValue(m_param_color_count);
                m_color_spin->SetValue(m_param_color_count);
                update_color_count_preset_buttons();
            }
            if (m_applied_smooth >= 0) {
                m_param_smooth = m_applied_smooth;
                m_smooth_slider->SetValue(m_param_smooth);
                m_smooth_spin->SetValue(m_param_smooth);
            }
            set_state(TextureImportState::Ready);
            return;
        }
        set_state(TextureImportState::Idle);
        return;
    }

    if (initial) {
        m_initial_computation_failed = true;
        m_initial_computation_pending = false;
        m_current_computation_initial = false;
        m_fallback_to_geometry_only = true;
        return;
    }

    set_state(TextureImportState::Error);
    Slic3r::GUI::MessageDialog dlg(initial ? GetParent() : this,
        _L("Computation failed. Please adjust parameters and retry."),
        _L("Error"), wxOK | wxICON_ERROR);
    dlg.ShowModal();
}

void TextureImportDialog::on_mesh_repair_decision_required(wxCommandEvent&)
{
    bool initial = m_current_computation_initial;
    bool auto_color = m_current_computation_auto_color;

    if (m_progress_dlg) {
        m_progress_dlg->Destroy();
        m_progress_dlg = nullptr;
    }

#ifdef HAS_WIN10SDK
    Slic3r::GUI::MessageDialog dlg(initial ? GetParent() : this,
        _L("The mesh has non-manifold geometry or open boundaries. You can import it as-is or repair it with Windows 3D repair service before importing."),
        _L("Mesh repair"), wxYES_NO | wxICON_WARNING | wxYES_DEFAULT);
    dlg.SetButtonLabel(wxID_YES, _L("Import without repair"));
    dlg.SetButtonLabel(wxID_NO, _L("Repair and import"), true);
    // "Repair and import" is the recommended action here, so the accent moves off the default YES
    // button onto NO. MsgDialog::add_button already styled both as ButtonType::Choice, so restyling
    // with the same type swaps only the palette and leaves the geometry alone.
    if (auto* yes_btn = dynamic_cast<Button*>(dlg.FindWindow(wxID_YES))) {
        yes_btn->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
        yes_btn->SetMinSize(wxSize(FromDIP(180), FromDIP(24)));
    }
    if (auto* no_btn = dynamic_cast<Button*>(dlg.FindWindow(wxID_NO))) {
        no_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
        no_btn->SetMinSize(wxSize(FromDIP(160), FromDIP(24)));
    }
    dlg.Layout();
    dlg.Fit();
    dlg.CenterOnParent();
    int ret = dlg.ShowModal();
    m_mesh_repair_decision = (ret == wxID_NO)
        ? Slic3r::TexturePaintingSettings::MeshRepairDecision::RepairAndImport
        : Slic3r::TexturePaintingSettings::MeshRepairDecision::ImportWithoutRepair;
#else
    Slic3r::GUI::MessageDialog dlg(initial ? GetParent() : this,
        _L("Please note that the mesh has non-manifold geometry or open boundaries."),
        _L("Mesh issue"), wxOK | wxCANCEL | wxICON_WARNING | wxOK_DEFAULT);
    dlg.SetButtonLabel(wxID_OK, _L("Continue"), true);
    dlg.SetButtonLabel(wxID_CANCEL, _L("Cancel"));
    int ret = dlg.ShowModal();
    if (ret != wxID_OK) {
        m_cancel_flag = true;
        if (initial) {
            m_initial_computation_cancelled = true;
            m_initial_computation_pending = false;
            m_current_computation_initial = false;
        } else if (has_valid_result()) {
            set_state(TextureImportState::Ready);
        } else {
            set_state(TextureImportState::Idle);
        }
        return;
    }
    m_mesh_repair_decision = Slic3r::TexturePaintingSettings::MeshRepairDecision::ImportWithoutRepair;
#endif

    start_computation(auto_color, initial);
}

// ---- Mapping ----

void TextureImportDialog::update_filament_color_map()
{
    std::map<std::array<std::size_t, 3>, std::array<float, 3>> color_map;
    for (const auto& m : m_current_matches) {
        if (m.filament_index >= 0 && m.filament_index < (int)m_filament_colors_rgba.size()) {
            color_map[m.cluster_color] = {
                m_filament_colors_rgba[m.filament_index][0],
                m_filament_colors_rgba[m.filament_index][1],
                m_filament_colors_rgba[m.filament_index][2]
            };
        }
    }
    m_preview_canvas->set_filament_color_map(color_map);
}

// Canonical ordering used on the very first display after a computation:
// sort ascending by filament_index, and push unmapped (filament_index < 0)
// entries to the end. This gives the user a stable, predictable mapping
// layout regardless of the cluster discovery order.
void TextureImportDialog::sort_current_matches_by_filament_index()
{
    std::stable_sort(m_current_matches.begin(), m_current_matches.end(),
        [](const auto& lhs, const auto& rhs) {
            const bool lhs_valid = lhs.filament_index >= 0;
            const bool rhs_valid = rhs.filament_index >= 0;

            if (lhs_valid != rhs_valid)
                return lhs_valid;
            if (!lhs_valid)
                return false;

            return lhs.filament_index < rhs.filament_index;
        });
}

// Preserve the row order the user is currently looking at across a
// re-computation (e.g. when auto-merge is toggled). We key on cluster_index
// because it survives compact_used_virtual_filaments() and filament-index
// renumbering, whereas filament_index does not.
//
// Behaviour:
//   * Entries whose cluster_index appeared in `previous_matches` keep their
//     previous relative order.
//   * Entries whose cluster_index is new (not in `previous_matches`) are
//     appended at the end, in their current relative order.
//
// Assumption: each cluster_index appears at most once in both vectors. This
// is currently guaranteed by do_auto_match(), which emits exactly one match
// per cluster. If that invariant ever changes, the std::map::emplace below
// silently keeps only the first occurrence and the order will be wrong.
void TextureImportDialog::restore_current_match_order(const std::vector<Slic3r::FilamentMatch>& previous_matches)
{
    if (previous_matches.empty() || m_current_matches.size() < 2)
        return;

    std::map<int, size_t> previous_order_by_cluster;
    for (size_t i = 0; i < previous_matches.size(); ++i) {
        if (previous_matches[i].cluster_index >= 0)
            previous_order_by_cluster.emplace(previous_matches[i].cluster_index, i);
    }

    std::stable_sort(m_current_matches.begin(), m_current_matches.end(),
        [&previous_order_by_cluster](const auto& lhs, const auto& rhs) {
            const auto lhs_it = previous_order_by_cluster.find(lhs.cluster_index);
            const auto rhs_it = previous_order_by_cluster.find(rhs.cluster_index);
            const bool lhs_known = lhs_it != previous_order_by_cluster.end();
            const bool rhs_known = rhs_it != previous_order_by_cluster.end();

            if (lhs_known != rhs_known)
                return lhs_known;
            if (!lhs_known)
                return false;

            return lhs_it->second < rhs_it->second;
        });
}

size_t TextureImportDialog::max_filament_count() const
{
    return static_cast<size_t>(EnforcerBlockerType::ExtruderMax);
}

bool TextureImportDialog::can_add_virtual_filament() const
{
    return m_filament_colors_rgba.size() < max_filament_count();
}

int TextureImportDialog::find_closest_filament_index(const std::array<std::size_t, 3>& color) const
{
    int best_idx = -1;
    double best_delta = std::numeric_limits<double>::max();
    const size_t filament_count = std::min(m_filament_colors_rgba.size(), max_filament_count());
    for (size_t i = 0; i < filament_count; ++i) {
        const double delta = Slic3r::compute_delta_e(color, m_filament_colors_rgba[i]);
        if (delta < best_delta) {
            best_delta = delta;
            best_idx = (int)i;
        }
    }
    return best_idx;
}

int TextureImportDialog::add_virtual_filament(const std::array<float, 4>& rgba, const std::string& hex,
                                              const std::string& preset_name)
{
    if (m_filament_color_strs.size() != m_filament_colors_rgba.size() ||
        m_filament_names.size() != m_filament_colors_rgba.size() ||
        m_filament_entries.size() != m_filament_colors_rgba.size()) {
        return -1;
    }
    if (!can_add_virtual_filament()) {
        // Mark that this do_auto_match() run hit the filament cap and had to
        // drop at least one cluster. The mapping itself still falls back via
        // find_closest_filament_index() below; this flag only drives the
        // inline orange warning above the bottom buttons.
        // Note: only the false -> true transition happens here; the flag is
        // cleared exclusively at the entry of do_auto_match() so it always
        // reflects the most recent match, never an accumulated history.
        m_filaments_dropped = true;
        return -1;
    }

    const int new_idx = (int)m_filament_colors_rgba.size();
    m_filament_colors_rgba.push_back(rgba);
    m_filament_color_strs.push_back(hex);
    m_filament_names.push_back(DEFAULT_VIRTUAL_FILAMENT_NAME);
    TextureFilamentEntry entry;
    entry.kind = TextureFilamentKind::NewPhysical;
    entry.dialog_index = new_idx;
    entry.project_config_index = size_t(-1);
    entry.color_hex = texture_normalize_color_hex(hex);
    entry.name = DEFAULT_VIRTUAL_FILAMENT_NAME;
    entry.preset_name = preset_name.empty() ? m_default_virtual_filament_preset_name : preset_name;
    m_filament_entries.push_back(entry);
    m_new_filament_colors.push_back(rgba);
    m_new_filament_preset_names.push_back(preset_name.empty() ? m_default_virtual_filament_preset_name : preset_name);
    return new_idx;
}

int TextureImportDialog::add_virtual_mixed_filament(const std::string& color_hex,
                                                    const std::vector<int>& component_dialog_indices,
                                                    const std::vector<int>& ratios)
{
    if (m_filament_color_strs.size() != m_filament_colors_rgba.size() ||
        m_filament_names.size() != m_filament_colors_rgba.size() ||
        m_filament_entries.size() != m_filament_colors_rgba.size()) {
        return -1;
    }
    if (component_dialog_indices.size() < 2 || component_dialog_indices.size() != ratios.size())
        return -1;
    for (int idx : component_dialog_indices) {
        if (idx < 0 || idx >= (int)m_filament_entries.size() ||
            !texture_entry_is_physical(m_filament_entries[idx].kind)) {
            return -1;
        }
    }
    if (!can_add_virtual_filament()) {
        m_filaments_dropped = true;
        return -1;
    }

    const int new_idx = (int)m_filament_colors_rgba.size();
    TextureFilamentEntry entry;
    entry.kind = TextureFilamentKind::NewMixed;
    entry.dialog_index = new_idx;
    entry.project_config_index = size_t(-1);
    entry.color_hex = texture_normalize_color_hex(color_hex);
    entry.name = DEFAULT_VIRTUAL_FILAMENT_NAME;
    entry.mixed_ratios = ratios;
    for (int idx : component_dialog_indices)
        entry.mixed_components.push_back((unsigned int)(idx + 1));

    TextureNewMixedFilament mixed;
    mixed.dialog_index = entry.dialog_index;
    mixed.color_hex = entry.color_hex;
    mixed.component_dialog_indices = component_dialog_indices;
    mixed.ratios = ratios;

    m_filament_entries.push_back(entry);
    m_filament_color_strs.push_back(entry.color_hex);
    m_filament_names.push_back(entry.name);
    m_filament_colors_rgba.push_back(parse_color_string(entry.color_hex));
    m_new_mixed_filaments.push_back(mixed);
    return entry.dialog_index;
}

void TextureImportDialog::compact_used_virtual_filaments()
{
    if (m_current_matches.empty())
        return;

    const std::vector<std::array<float, 4>> old_colors = m_filament_colors_rgba;
    const std::vector<std::string> old_color_strs = m_filament_color_strs;
    const std::vector<std::string> old_names = m_filament_names;
    const std::vector<TextureFilamentEntry> old_entries = m_filament_entries;

    auto old_new_mixed_has_valid_components = [&old_entries, &old_colors](const TextureFilamentEntry& entry) {
        if (entry.kind != TextureFilamentKind::NewMixed)
            return true;
        if (entry.mixed_components.size() < 2 || entry.mixed_components.size() != entry.mixed_ratios.size())
            return false;
        for (unsigned int comp : entry.mixed_components) {
            int comp_idx = comp >= 1 ? (int)comp - 1 : -1;
            if (comp_idx < 0 || comp_idx >= (int)old_entries.size() || comp_idx >= (int)old_colors.size() ||
                !texture_entry_is_physical(old_entries[comp_idx].kind)) {
                return false;
            }
        }
        return true;
    };

    std::set<int> used_virtual_indices;
    for (const auto& m : m_current_matches) {
        if (m.filament_index >= (int)m_existing_filament_count &&
            m.filament_index < (int)old_colors.size()) {
            if (m.filament_index < (int)old_entries.size() &&
                old_entries[m.filament_index].kind == TextureFilamentKind::NewMixed &&
                !old_new_mixed_has_valid_components(old_entries[m.filament_index])) {
                continue;
            }
            used_virtual_indices.insert(m.filament_index);
        }
    }
    bool added_dependency = true;
    while (added_dependency) {
        added_dependency = false;
        std::vector<int> current_used(used_virtual_indices.begin(), used_virtual_indices.end());
        for (int used_idx : current_used) {
            if (used_idx < 0 || used_idx >= (int)old_entries.size() ||
                old_entries[used_idx].kind != TextureFilamentKind::NewMixed ||
                !old_new_mixed_has_valid_components(old_entries[used_idx]))
                continue;
            for (unsigned int comp : old_entries[used_idx].mixed_components) {
                int comp_idx = comp >= 1 ? (int)comp - 1 : -1;
                if (comp_idx >= (int)m_existing_filament_count && comp_idx < (int)old_entries.size() &&
                    used_virtual_indices.insert(comp_idx).second) {
                    added_dependency = true;
                }
            }
        }
    }

    std::vector<std::array<float, 4>> compact_colors;
    std::vector<std::string> compact_color_strs;
    std::vector<std::string> compact_names;
    std::vector<TextureFilamentEntry> compact_entries;
    compact_colors.reserve(m_existing_filament_count + used_virtual_indices.size());
    compact_color_strs.reserve(m_existing_filament_count + used_virtual_indices.size());
    compact_names.reserve(m_existing_filament_count + used_virtual_indices.size());
    compact_entries.reserve(m_existing_filament_count + used_virtual_indices.size());

    const size_t existing_count = std::min(m_existing_filament_count, old_colors.size());
    for (size_t i = 0; i < existing_count; ++i) {
        compact_colors.push_back(old_colors[i]);
        compact_color_strs.push_back(i < old_color_strs.size() ? old_color_strs[i] : "");
        compact_names.push_back(i < old_names.size() ? old_names[i] : "Filament " + std::to_string(i + 1));
        TextureFilamentEntry entry = i < old_entries.size() ? old_entries[i] : TextureFilamentEntry{};
        entry.dialog_index = (int)i;
        entry.color_hex = texture_normalize_color_hex(compact_color_strs.back());
        entry.name = compact_names.back();
        compact_entries.push_back(entry);
    }

    std::map<int, int> old_to_new;
    std::vector<std::array<float, 4>> compact_new_colors;
    std::vector<std::string> compact_new_preset_names;
    compact_new_colors.reserve(used_virtual_indices.size());
    compact_new_preset_names.reserve(used_virtual_indices.size());

    for (int old_idx : used_virtual_indices) {
        old_to_new[old_idx] = (int)compact_colors.size();
        compact_colors.push_back(old_colors[old_idx]);
        compact_color_strs.push_back(old_idx < (int)old_color_strs.size() ? old_color_strs[old_idx] : "");
        compact_names.push_back(old_idx < (int)old_names.size() ? old_names[old_idx] : DEFAULT_VIRTUAL_FILAMENT_NAME);
        TextureFilamentEntry entry = old_idx < (int)old_entries.size() ? old_entries[old_idx] : TextureFilamentEntry{};
        entry.dialog_index = (int)compact_entries.size();
        entry.color_hex = texture_normalize_color_hex(compact_color_strs.back());
        entry.name = compact_names.back();
        if (entry.kind == TextureFilamentKind::NewPhysical) {
            compact_new_colors.push_back(old_colors[old_idx]);
            compact_new_preset_names.push_back(entry.preset_name.empty() ? m_default_virtual_filament_preset_name : entry.preset_name);
        }
        compact_entries.push_back(entry);
    }

    m_filament_colors_rgba = std::move(compact_colors);
    m_filament_color_strs = std::move(compact_color_strs);
    m_filament_names = std::move(compact_names);
    m_filament_entries = std::move(compact_entries);
    m_new_filament_colors = std::move(compact_new_colors);
    m_new_filament_preset_names = std::move(compact_new_preset_names);
    m_new_mixed_filaments.clear();
    std::set<int> invalid_compacted_mixed_indices;
    for (auto& entry : m_filament_entries) {
        if (entry.kind != TextureFilamentKind::NewMixed)
            continue;
        TextureNewMixedFilament mixed;
        mixed.dialog_index = entry.dialog_index;
        mixed.color_hex = entry.color_hex;
        mixed.ratios = entry.mixed_ratios;
        mixed.component_dialog_indices.reserve(entry.mixed_components.size());
        bool valid_components = entry.mixed_components.size() >= 2 &&
            entry.mixed_components.size() == entry.mixed_ratios.size();
        for (unsigned int comp : entry.mixed_components) {
            int old_comp_idx = comp >= 1 ? (int)comp - 1 : -1;
            if (old_comp_idx < 0) {
                valid_components = false;
                break;
            }
            auto remap_it = old_to_new.find(old_comp_idx);
            int new_comp_idx = remap_it != old_to_new.end() ? remap_it->second : old_comp_idx;
            if (new_comp_idx < 0 || new_comp_idx >= (int)m_filament_entries.size() ||
                !texture_entry_is_physical(m_filament_entries[new_comp_idx].kind)) {
                valid_components = false;
                break;
            }
            mixed.component_dialog_indices.push_back(new_comp_idx);
        }
        if (!valid_components) {
            invalid_compacted_mixed_indices.insert(entry.dialog_index);
            continue;
        }
        entry.mixed_components.clear();
        for (int comp_idx : mixed.component_dialog_indices)
            entry.mixed_components.push_back((unsigned int)(comp_idx + 1));
        m_new_mixed_filaments.push_back(mixed);
    }

    auto find_closest_physical_filament_index = [this](const std::array<std::size_t, 3>& color) {
        int best_idx = -1;
        double best_delta = std::numeric_limits<double>::max();
        const size_t filament_count = std::min(m_filament_colors_rgba.size(), max_filament_count());
        for (size_t i = 0; i < filament_count && i < m_filament_entries.size(); ++i) {
            if (!texture_entry_is_physical(m_filament_entries[i].kind))
                continue;
            const double delta = Slic3r::compute_delta_e(color, m_filament_colors_rgba[i]);
            if (delta < best_delta) {
                best_delta = delta;
                best_idx = (int)i;
            }
        }
        return best_idx;
    };

    for (auto& m : m_current_matches) {
        auto it = old_to_new.find(m.filament_index);
        if (it != old_to_new.end()) {
            m.filament_index = it->second;
        } else if (m.filament_index >= (int)m_existing_filament_count) {
            m.filament_index = find_closest_filament_index(m.cluster_color);
        }
        if (invalid_compacted_mixed_indices.count(m.filament_index) > 0) {
            int fallback_idx = find_closest_physical_filament_index(m.cluster_color);
            m.filament_index = fallback_idx >= 0 ? fallback_idx : find_closest_filament_index(m.cluster_color);
        }

        if (m.filament_index >= 0 && m.filament_index < (int)m_filament_colors_rgba.size()) {
            m.filament_color = m_filament_colors_rgba[m.filament_index];
            m.delta_e = Slic3r::compute_delta_e(m.cluster_color, m.filament_color);
            if (m.filament_index >= (int)m_existing_filament_count)
                m.delta_e = 0.0;
        }
    }
}

std::vector<int> TextureImportDialog::compute_display_numbers() const
{
    // Assigns each entry a 1-based display number in the order the sidebar will
    // show after apply: ExistingPhysical, NewPhysical, ExistingMixed, NewMixed.
    // This keeps the dialog's visible IDs in sync with the post-apply sidebar,
    // instead of the raw dialog_index (which interleaves physicals and mixeds
    // by processing order and causes e.g. CMYW to show 4,5,6,8 instead of 3,4,5,6).
    // MUST mirror ordering in apply_textured_mesh_import_result (Plater.cpp:9896):
    //   - ExistingPhysical keeps its project_config_index
    //   - NewPhysical is inserted at existing_physical_count + new_order
    //   - ExistingMixed shifts to project_config_index + new_physical_count
    //   - NewMixed is appended after all existing mixeds
    std::vector<int> result(m_filament_entries.size(), 0);
    int next = 1;

    auto assign_group = [&](TextureFilamentKind kind, bool by_project_config_index) {
        if (by_project_config_index) {
            std::vector<const TextureFilamentEntry*> group;
            for (const auto& e : m_filament_entries)
                if (e.kind == kind)
                    group.push_back(&e);
            std::sort(group.begin(), group.end(),
                      [](const TextureFilamentEntry* a, const TextureFilamentEntry* b) {
                          return a->project_config_index < b->project_config_index;
                      });
            for (const auto* e : group) {
                if (e->dialog_index >= 0 && e->dialog_index < (int)result.size())
                    result[e->dialog_index] = next;
                ++next;
            }
        } else {
            for (const auto& e : m_filament_entries) {
                if (e.kind != kind)
                    continue;
                if (e.dialog_index >= 0 && e.dialog_index < (int)result.size())
                    result[e.dialog_index] = next;
                ++next;
            }
        }
    };

    assign_group(TextureFilamentKind::ExistingPhysical, true);
    assign_group(TextureFilamentKind::NewPhysical,      false);
    assign_group(TextureFilamentKind::ExistingMixed,    true);
    assign_group(TextureFilamentKind::NewMixed,         false);
    return result;
}

void TextureImportDialog::dismiss_filament_popup()
{
    if (!m_filament_popup) {
        m_filament_popup_row = -1;
        return;
    }

    FilamentSelectPopup* popup = m_filament_popup;
    m_filament_popup = nullptr;
    m_filament_popup_row = -1;
    if (popup->IsShown())
        popup->Dismiss();
    else
        popup->Destroy();
}

void TextureImportDialog::show_auto_mix_popup()
{
    if (!m_btn_auto_mix || !m_btn_auto_mix->IsEnabled())
        return;

    if (m_auto_mix_popup && m_auto_mix_popup->IsShown())
        return;
    dismiss_auto_mix_popup();

    auto on_select = [this](TextureAutoMixMode mode) {
        set_auto_mix_mode(mode);
    };
    auto on_close = [this]() {
        m_auto_mix_popup = nullptr;
    };

    auto* popup = new AutoMixSelectPopup(this, m_auto_mix_mode, m_btn_auto_mix->GetSize().x,
                                         m_auto_mix_font_point_size,
                                         on_select, on_close);
    wxPoint pos = m_btn_auto_mix->ClientToScreen(wxPoint(0, m_btn_auto_mix->GetSize().y));
    wxRect display_rect;
    int display_idx = wxDisplay::GetFromPoint(pos);
    if (display_idx != wxNOT_FOUND)
        display_rect = wxDisplay(display_idx).GetClientArea();
    else
        display_rect = wxDisplay().GetClientArea();
    pos.x = std::clamp(pos.x, display_rect.GetLeft(),
                       std::max(display_rect.GetLeft(), display_rect.GetRight() - popup->GetSize().x));
    popup->Position(pos, wxSize(0, 0));
    popup->Bind(wxEVT_DESTROY, [this, popup](wxWindowDestroyEvent& e) {
        e.Skip();
        if (m_auto_mix_popup == popup)
            m_auto_mix_popup = nullptr;
    });
    m_auto_mix_popup = popup;
    popup->Popup();
}

void TextureImportDialog::dismiss_auto_mix_popup()
{
    if (!m_auto_mix_popup)
        return;

    AutoMixSelectPopup* popup = m_auto_mix_popup;
    m_auto_mix_popup = nullptr;
    if (popup->IsShown())
        popup->Dismiss();
    else
        popup->Destroy();
}

void TextureImportDialog::set_auto_mix_mode(TextureAutoMixMode mode)
{
    m_auto_mix_mode = mode;
    if (m_btn_auto_mix) {
        m_btn_auto_mix->SetLabel(auto_mix_mode_label(mode));
        m_btn_auto_mix->Refresh();
    }
    apply_auto_standard_mix(mode);
}

void TextureImportDialog::apply_auto_standard_mix(TextureAutoMixMode mode)
{
    if (m_mapping_rows.empty())
        return;
    m_filaments_dropped = false;

    auto find_or_add_base_physical = [this](const std::string& color_hex) -> int {
        const std::string normalized = texture_normalize_color_hex(color_hex);
        for (const auto& entry : m_filament_entries) {
            if (!texture_entry_is_physical(entry.kind))
                continue;
            if (texture_normalize_color_hex(entry.color_hex) != normalized)
                continue;
            if (texture_entry_is_pla_basic(entry))
                return entry.dialog_index;
        }

        std::array<float, 4> rgba = parse_color_string(normalized);
        int idx = add_virtual_filament(rgba, normalized, m_default_virtual_filament_preset_name);
        if (idx >= 0 && idx < (int)m_filament_entries.size()) {
            m_filament_entries[idx].type = DEFAULT_VIRTUAL_FILAMENT_BASIC_TYPE;
            m_filament_entries[idx].name = DEFAULT_VIRTUAL_FILAMENT_NAME;
        }
        return idx;
    };

    auto find_existing_mixed = [this](const std::vector<int>& component_indices, const std::vector<int>& ratios) -> int {
        for (const auto& entry : m_filament_entries) {
            if (!texture_entry_is_mixed(entry.kind) || entry.mixed_components.size() != component_indices.size() ||
                entry.mixed_ratios.size() != ratios.size())
                continue;
            bool same = true;
            for (size_t i = 0; i < component_indices.size(); ++i) {
                if (entry.mixed_components[i] != (unsigned int)(component_indices[i] + 1) ||
                    entry.mixed_ratios[i] != ratios[i]) {
                    same = false;
                    break;
                }
            }
            if (same)
                return entry.dialog_index;
        }
        return -1;
    };

    bool changed = false;
    const auto recipe_mode = texture_recipe_mode(mode);
    for (size_t row_index = 0; row_index < m_mapping_rows.size(); ++row_index) {
        Slic3r::ColorDecomposeRgb target_rgb;
        if (!Slic3r::color_decompose_hex_to_rgb(m_mapping_rows[row_index].source_hex, target_rgb))
            continue;

        auto recipe = Slic3r::lookup_standard_recipe(target_rgb, recipe_mode, DEFAULT_VIRTUAL_FILAMENT_BASIC_TYPE);
        if (!recipe.valid || recipe.components.size() < 2)
            continue;

        std::vector<int> component_dialog_indices;
        std::vector<int> ratios;
        for (const auto& comp : recipe.components) {
            int component_idx = find_or_add_base_physical(comp.color_hex);
            if (component_idx < 0) {
                component_dialog_indices.clear();
                break;
            }
            component_dialog_indices.push_back(component_idx);
            ratios.push_back(comp.ratio);
        }
        if (component_dialog_indices.size() < 2 || component_dialog_indices.size() != ratios.size())
            continue;

        int mixed_idx = find_existing_mixed(component_dialog_indices, ratios);
        if (mixed_idx < 0)
            mixed_idx = add_virtual_mixed_filament(recipe.matched_color_hex, component_dialog_indices, ratios);
        if (mixed_idx < 0)
            continue;

        m_mapping_rows[row_index].target_filament_idx = mixed_idx;
        if (row_index < m_current_matches.size()) {
            m_current_matches[row_index].filament_index = mixed_idx;
            m_current_matches[row_index].filament_color = m_filament_colors_rgba[mixed_idx];
            m_current_matches[row_index].delta_e = Slic3r::compute_delta_e(
                m_current_matches[row_index].cluster_color, m_current_matches[row_index].filament_color);
            if (mixed_idx >= (int)m_existing_filament_count)
                m_current_matches[row_index].delta_e = 0.0;
        }
        changed = true;
    }

    if (!changed)
        return;

    m_auto_mix_applied = true;
    compact_used_virtual_filaments();
    update_filament_color_map();
    rebuild_mapping_rows();
    update_drop_warning_visibility();
    update_auto_mix_reset_visibility();
}

void TextureImportDialog::reset_auto_mix()
{
    if (m_state != TextureImportState::Ready || !m_auto_mix_applied)
        return;

    dismiss_auto_mix_popup();

    // Clear mixed filament references so the compact inside do_auto_match()
    // removes them (and their exclusively-owned base physicals) from the
    // filament arrays, giving the baseline matching a clean starting state.
    for (auto& m : m_current_matches) {
        if (m.filament_index >= 0 && m.filament_index < (int)m_filament_entries.size() &&
            texture_entry_is_mixed(m_filament_entries[m.filament_index].kind)) {
            m.filament_index = -1;
        }
    }

    // Re-run the baseline auto-match (same flow as the auto-merge toggle) so the
    // mapping reverts to the pre-mix state: every colour matches an existing
    // physical filament or a virtual physical filament, with no mixed filaments.
    const auto previous_matches = m_current_matches;
    do_auto_match();
    restore_current_match_order(previous_matches);
    compact_used_virtual_filaments();
    update_filament_color_map();
    rebuild_mapping_rows();
    update_drop_warning_visibility();
    update_auto_mix_reset_visibility();
}

void TextureImportDialog::update_auto_mix_reset_visibility()
{
    if (!m_btn_mix_reset)
        return;
    if (m_btn_mix_reset->Show(m_auto_mix_applied)) {
        if (wxWindow* parent = m_btn_mix_reset->GetParent())
            parent->Layout();
    }
}

bool TextureImportDialog::add_decomposed_mixed_filament(size_t row_index)
{
    if (row_index >= m_mapping_rows.size())
        return false;

    std::vector<std::string> physical_colors;
    std::vector<std::string> physical_names;
    std::vector<std::string> physical_types;
    std::vector<int> physical_dialog_indices;
    std::vector<size_t> physical_config_indices;
    auto& preset_bundle = *wxGetApp().preset_bundle;
    for (const auto& entry : m_filament_entries) {
        if (entry.kind != TextureFilamentKind::ExistingPhysical)
            continue;
        physical_colors.push_back(entry.color_hex);
        physical_names.push_back(entry.name);
        const size_t cfg_idx = entry.project_config_index;
        Preset* preset = nullptr;
        if (cfg_idx < preset_bundle.filament_presets.size())
            preset = preset_bundle.filaments.find_preset(preset_bundle.filament_presets[cfg_idx]);
        physical_types.push_back(filament_type_for_color_decompose(preset));
        physical_dialog_indices.push_back(entry.dialog_index);
        physical_config_indices.push_back(cfg_idx);
    }
    if (physical_colors.empty())
        return false;

    wxColour target(m_mapping_rows[row_index].source_hex);
    ColorDecomposeDialog dlg(this, -1, target, physical_colors, physical_names, physical_types,
                             m_filament_entries.size(), max_filament_count(),
                             std::move(physical_config_indices));
    // Count "new physical filaments" with the exact reuse rule of the write-back
    // loop below: a base color is only new if no existing OR virtual official
    // Bambu Basic filament already carries that color. This keeps the dialog's
    // filament-limit pre-check consistent with what add_decomposed_mixed_filament
    // will actually create, so already-present virtual base colors are not
    // double counted (which previously could wrongly disable OK).
    dlg.set_missing_physical_calculator([this](const ColorDecomposeResult& result) -> size_t {
        size_t missing = 0;
        for (const DecomposeComponent& comp : result.components) {
            if (comp.filament_index > 0)
                continue; // reuses a physical slot passed to the dialog, no new filament
            const std::string comp_hex = texture_normalize_color_hex(
                comp.colour.GetAsString(wxC2S_HTML_SYNTAX).ToStdString());
            bool found = false;
            for (const auto& entry : m_filament_entries) {
                if (!texture_entry_is_physical(entry.kind))
                    continue;
                if (texture_normalize_color_hex(entry.color_hex) != comp_hex)
                    continue;
                if (texture_entry_official_basic(entry)) {
                    found = true;
                    break;
                }
            }
            if (!found)
                ++missing;
        }
        return missing;
    });
    if (dlg.ShowModal() != wxID_OK)
        return false;

    ColorDecomposeResult result = dlg.get_result();
    std::vector<int> component_dialog_indices;
    std::vector<int> ratios;
    for (const DecomposeComponent& comp : result.components) {
        ratios.push_back(comp.ratio);
        if (comp.filament_index > 0) {
            const size_t physical_idx = (size_t)(comp.filament_index - 1);
            if (physical_idx >= physical_dialog_indices.size())
                return false;
            component_dialog_indices.push_back(physical_dialog_indices[physical_idx]);
            continue;
        }

        const std::string comp_hex = texture_normalize_color_hex(comp.colour.GetAsString(wxC2S_HTML_SYNTAX).ToStdString());
        int existing_idx = -1;
        for (const auto& entry : m_filament_entries) {
            if (!texture_entry_is_physical(entry.kind))
                continue;
            if (texture_normalize_color_hex(entry.color_hex) != comp_hex)
                continue;
            if (texture_entry_official_basic(entry)) {
                existing_idx = entry.dialog_index;
                break;
            }
        }
        if (existing_idx < 0) {
            std::array<float, 4> rgba = parse_color_string(comp_hex);
            existing_idx = add_virtual_filament(rgba, comp_hex);
            if (existing_idx < 0)
                return false;
        }
        component_dialog_indices.push_back(existing_idx);
    }

    if (component_dialog_indices.size() < 2 || component_dialog_indices.size() != ratios.size())
        return false;

    const std::string mixed_hex = texture_normalize_color_hex(
        result.matched_color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString());
    int mixed_idx = add_virtual_mixed_filament(mixed_hex, component_dialog_indices, ratios);
    if (mixed_idx < 0)
        return false;

    m_mapping_rows[row_index].target_filament_idx = mixed_idx;
    if (row_index < m_current_matches.size())
        m_current_matches[row_index].filament_index = mixed_idx;
    rebuild_mapping_rows();
    update_filament_color_map();
    return true;
}

void TextureImportDialog::dismiss_filament_popup_on_wheel(wxMouseEvent& evt)
{
    dismiss_filament_popup();
    dismiss_auto_mix_popup();
    evt.Skip();
}

void TextureImportDialog::show_filament_popup(size_t row_index)
{
    if (row_index >= m_mapping_rows.size()) return;

    if (m_skip_next_filament_popup_row == (int)row_index) {
        m_skip_next_filament_popup_row = -1;
        return;
    }

    if (m_filament_popup && m_filament_popup->IsShown()) {
        if (m_filament_popup_row == (int)row_index) {
            dismiss_filament_popup();
            return;
        }
        dismiss_filament_popup();
    }

    const auto display_numbers = compute_display_numbers();
    auto display_number = [display_numbers](int idx) -> int {
        return (idx >= 0 && idx < (int)display_numbers.size() && display_numbers[idx] > 0)
            ? display_numbers[idx] : idx + 1;
    };

    auto on_select = [this, row_index, display_number](int idx) {
        if (row_index >= m_mapping_rows.size()) return;
        m_mapping_rows[row_index].target_filament_idx = idx;
        if (row_index < m_current_matches.size())
            m_current_matches[row_index].filament_index = idx;
        if (m_mapping_rows[row_index].target_panel) {
            wxString label = (idx >= 0 && idx < (int)m_filament_names.size())
                ? filament_name_to_wx_string(m_filament_names[idx])
                : wxString::Format("Filament %d", display_number(idx));
            m_mapping_rows[row_index].target_panel->SetToolTip(label);
            m_mapping_rows[row_index].target_panel->Refresh();
        }
        update_filament_color_map();
    };

    auto on_add_filament = [this, row_index](wxColour clr) {
        std::array<float, 4> rgba = {clr.Red() / 255.f, clr.Green() / 255.f,
                                     clr.Blue() / 255.f, 1.0f};
        std::string hex = wxString::Format("#%02X%02X%02X",
            clr.Red(), clr.Green(), clr.Blue()).ToStdString();
        int new_idx = add_virtual_filament(rgba, hex);
        if (new_idx < 0)
            return;

        if (row_index < m_mapping_rows.size()) {
            m_mapping_rows[row_index].target_filament_idx = new_idx;
            if (row_index < m_current_matches.size())
                m_current_matches[row_index].filament_index = new_idx;
        }
        rebuild_mapping_rows();
        update_filament_color_map();
    };

    auto on_decompose_color = [this, row_index]() {
        CallAfter([this, row_index]() {
            add_decomposed_mixed_filament(row_index);
        });
    };

    wxPanel* tp = m_mapping_rows[row_index].target_panel;
    if (!tp) return;

    auto on_close = [this, row_index](bool closed_by_action) {
        if (m_filament_popup_row == (int)row_index) {
            m_filament_popup = nullptr;
            m_filament_popup_row = -1;
        }
        if (!closed_by_action) {
            m_skip_next_filament_popup_row = (int)row_index;
            CallAfter([this, row_index]() {
                if (m_skip_next_filament_popup_row == (int)row_index)
                    m_skip_next_filament_popup_row = -1;
            });
        }
    };

    auto* popup = new FilamentSelectPopup(
        this, m_filament_entries, m_filament_colors_rgba, m_filament_names,
        m_existing_filament_count, tp->GetSize().x, tp, on_select, on_add_filament,
        on_decompose_color,
        [this]() { return can_add_virtual_filament(); },
        on_close,
        display_numbers);

    wxPoint pos = tp->ClientToScreen(wxPoint(0, tp->GetSize().y));
    wxRect display_rect;
    int display_idx = wxDisplay::GetFromPoint(pos);
    if (display_idx != wxNOT_FOUND)
        display_rect = wxDisplay(display_idx).GetClientArea();
    else
        display_rect = wxDisplay().GetClientArea();
    pos.x = std::clamp(pos.x, display_rect.GetLeft(),
                       std::max(display_rect.GetLeft(), display_rect.GetRight() - popup->GetSize().x));
    popup->Position(pos, wxSize(0, 0));
    popup->Bind(wxEVT_DESTROY, [this, popup](wxWindowDestroyEvent& e) {
        e.Skip();
        if (m_filament_popup == popup) {
            m_filament_popup = nullptr;
            m_filament_popup_row = -1;
        }
    });
    m_filament_popup = popup;
    m_filament_popup_row = (int)row_index;
    popup->Popup();
}

void TextureImportDialog::do_auto_match()
{
    if (m_painted.cluster_colors.empty()) return;

    // do_auto_match() always rebuilds the baseline mapping without any mixed
    // filaments, so it is the common entry for every "revert one-click mix"
    // path. Clear the applied flag here; callers refresh the reset button.
    m_auto_mix_applied = false;

    // Reset the "filaments were dropped" flag at the start of every run, so it
    // strictly reflects what happens during *this* match (no historical
    // accumulation). add_virtual_filament() will flip it back to true if and
    // only if it hits the global filament cap below.
    m_filaments_dropped = false;

    // Drop any virtual filaments left over from previous match runs that the
    // current m_current_matches no longer references. Without this, the
    // residual virtual filaments inflate m_filament_colors_rgba.size() at the
    // entry of this match, which can make add_virtual_filament() fail (and
    // wrongly flip m_filaments_dropped to true) even when the *real* count
    // of needed virtual filaments for this run is well below the global cap.
    // This is purely a state cleanup; it does not change any mapping rule.
    compact_used_virtual_filaments();

    const auto previous_matches = m_current_matches;

    std::map<std::array<std::size_t, 3>, int> previous_virtual_by_cluster;
    for (const auto& match : previous_matches) {
        if (match.filament_index >= (int)m_existing_filament_count &&
            match.filament_index < (int)m_filament_entries.size() &&
            texture_entry_is_physical(m_filament_entries[match.filament_index].kind)) {
            previous_virtual_by_cluster[match.cluster_color] = match.filament_index;
        }
    }

    auto find_virtual_filament_by_color = [this](const std::array<std::size_t, 3>& color) -> int {
        std::string hex = rgb_to_hex(color).ToStdString();
        for (size_t i = m_existing_filament_count; i < m_filament_color_strs.size(); ++i) {
            if (m_filament_color_strs[i] == hex &&
                i < m_filament_entries.size() && texture_entry_is_physical(m_filament_entries[i].kind))
                return (int)i;
        }
        return -1;
    };

    auto get_or_add_virtual_filament = [this, &previous_virtual_by_cluster, &find_virtual_filament_by_color](
        const std::array<std::size_t, 3>& color) -> int {
        auto previous_it = previous_virtual_by_cluster.find(color);
        if (previous_it != previous_virtual_by_cluster.end() &&
            previous_it->second >= (int)m_existing_filament_count &&
            previous_it->second < (int)m_filament_colors_rgba.size()) {
            return previous_it->second;
        }

        int existing_idx = find_virtual_filament_by_color(color);
        if (existing_idx >= 0)
            return existing_idx;

        std::array<float, 4> rgba = {
            color[0] / 255.f,
            color[1] / 255.f,
            color[2] / 255.f,
            1.f
        };
        return add_virtual_filament(rgba, rgb_to_hex(color).ToStdString());
    };

    if (m_auto_merge_cb && m_auto_merge_cb->GetValue()) {
        // Match clusters to closest existing filaments
        std::vector<std::string> names;
        for (size_t i = 0; i < m_existing_filament_count; ++i)
            names.push_back(m_filament_names.size() > i ? m_filament_names[i] : "Filament " + std::to_string(i + 1));

        std::vector<std::array<float, 4>> existing_filament_colors(
            m_filament_colors_rgba.begin(),
            m_filament_colors_rgba.begin() + std::min(m_existing_filament_count, m_filament_colors_rgba.size()));

        m_current_matches = Slic3r::match_clusters_to_filaments(
            m_painted.cluster_colors, existing_filament_colors, names);

        // For clusters with poor match (CIEDE2000 ΔE > 5), create virtual filaments.
        constexpr double NEW_FILAMENT_THRESHOLD = 5.0;
        std::map<std::array<std::size_t, 3>, int> virtual_color_index;

        for (auto& m : m_current_matches) {
            if (m.delta_e <= NEW_FILAMENT_THRESHOLD)
                continue;

            auto it = virtual_color_index.find(m.cluster_color);
            if (it != virtual_color_index.end()) {
                m.filament_index = it->second;
            } else {
                int new_idx = get_or_add_virtual_filament(m.cluster_color);
                if (new_idx >= 0)
                    virtual_color_index[m.cluster_color] = new_idx;
                m.filament_index = new_idx >= 0 ? new_idx : find_closest_filament_index(m.cluster_color);
            }
            if (m.filament_index >= 0 && m.filament_index < (int)m_filament_colors_rgba.size()) {
                m.filament_color = m_filament_colors_rgba[m.filament_index];
                m.delta_e = Slic3r::compute_delta_e(m.cluster_color, m.filament_color);
                if (m.filament_index >= (int)m_existing_filament_count)
                    m.delta_e = 0.0;
            }
        }
    } else {
        // Keep all virtual filaments in this dialog; unused ones are pruned only on OK.
        m_current_matches.clear();
        std::map<std::array<std::size_t, 3>, int> virtual_map;

        for (size_t i = 0; i < m_painted.cluster_colors.size(); ++i) {
            const auto& cc = m_painted.cluster_colors[i];
            Slic3r::FilamentMatch fm;
            fm.cluster_index = (int)i;
            fm.cluster_color = cc;

            auto it = virtual_map.find(cc);
            if (it != virtual_map.end()) {
                fm.filament_index = it->second;
            } else {
                int idx = get_or_add_virtual_filament(cc);
                if (idx >= 0)
                    virtual_map[cc] = idx;
                fm.filament_index = idx >= 0 ? idx : find_closest_filament_index(cc);
            }
            if (fm.filament_index >= 0 && fm.filament_index < (int)m_filament_colors_rgba.size()) {
                fm.filament_color = m_filament_colors_rgba[fm.filament_index];
                fm.delta_e = Slic3r::compute_delta_e(fm.cluster_color, fm.filament_color);
                if (fm.filament_index >= (int)m_existing_filament_count)
                    fm.delta_e = 0.0;
            }
            m_current_matches.push_back(fm);
        }
    }

    update_filament_color_map();
}

void TextureImportDialog::rebuild_mapping_rows()
{
    m_mapping_scroll->Freeze();
    m_mapping_sizer->Clear(true);
    m_mapping_rows.clear();

    if (m_current_matches.empty()) {
        m_mapping_scroll->FitInside();
        m_mapping_scroll->Thaw();
        return;
    }

    auto get_target_wxcolor = [this](int idx) -> wxColour {
        if (idx >= 0 && idx < (int)m_filament_colors_rgba.size()) {
            const auto& c = m_filament_colors_rgba[idx];
            return wxColour((unsigned char)(c[0] * 255.f),
                            (unsigned char)(c[1] * 255.f),
                            (unsigned char)(c[2] * 255.f));
        }
        return wxColour(128, 128, 128);
    };

    const auto display_numbers = compute_display_numbers();
    auto display_number = [display_numbers](int idx) -> int {
        return (idx >= 0 && idx < (int)display_numbers.size() && display_numbers[idx] > 0)
            ? display_numbers[idx] : idx + 1;
    };

    auto get_filament_label = [this, display_number](int idx) -> wxString {
        if (idx >= 0 && idx < (int)m_filament_names.size())
            return filament_name_to_wx_string(m_filament_names[idx]);
        return wxString::Format("Filament %d", display_number(idx));
    };

    const wxColour dash_clr   = StateColor::darkModeColorFor(wxColour("#ACACAC"));
    const wxColour hex_fg     = texture_import_text_colour();
    const wxColour card_bg    = StateColor::darkModeColorFor(wxColour("#E8E8E8"));
    const wxColour card_bd    = StateColor::darkModeColorFor(wxColour("#DBDBDB"));
    const wxColour name_fg    = texture_import_text_colour();
    const wxColour chev_clr   = StateColor::darkModeColorFor(wxColour("#6B6B6A"));

    m_mapping_rows.resize(m_current_matches.size());
    for (size_t ci = 0; ci < m_current_matches.size(); ++ci) {
        auto& row = m_mapping_rows[ci];
        row.cluster_id = m_current_matches[ci].cluster_index;
        row.source_color = m_current_matches[ci].cluster_color;
        row.source_hex = rgb_to_hex(row.source_color).ToStdString();
        row.target_filament_idx = m_current_matches[ci].filament_index;

        wxColour src_wx_color(
            (unsigned char)row.source_color[0],
            (unsigned char)row.source_color[1],
            (unsigned char)row.source_color[2]);

        // --- Row container ---
        wxPanel* row_panel = new wxPanel(m_mapping_scroll, wxID_ANY);
        row_panel->SetBackgroundColour(m_mapping_scroll->GetBackgroundColour());
        row_panel->Bind(wxEVT_MOUSEWHEEL, &TextureImportDialog::dismiss_filament_popup_on_wheel, this);
        wxBoxSizer* row_sizer = new wxBoxSizer(wxHORIZONTAL);

        // --- Source card (dashed border, circle + hex) ---
        const int src_w = FromDIP(138);
        const int target_min_w = FromDIP(239);
        const int row_h = FromDIP(44);
        row.source_panel = new wxPanel(row_panel, wxID_ANY, wxDefaultPosition, wxSize(src_w, row_h),
                                       wxTAB_TRAVERSAL | wxFULL_REPAINT_ON_RESIZE);
        row.source_panel->SetMinSize(wxSize(src_w, row_h));
        row.source_panel->SetMaxSize(wxSize(src_w, row_h));
        row.source_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);

        row.source_panel->Bind(wxEVT_PAINT, [this, ci, src_wx_color, dash_clr, hex_fg](wxPaintEvent& e) {
            auto* p = static_cast<wxPanel*>(e.GetEventObject());
            wxAutoBufferedPaintDC dc(p);
            wxSize sz = p->GetClientSize();

            dc.SetBrush(wxBrush(p->GetParent()->GetBackgroundColour()));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(0, 0, sz.x, sz.y);

            wxPen dash_pen(dash_clr, 1, wxPENSTYLE_SHORT_DASH);
            dc.SetPen(dash_pen);
            dc.SetBrush(wxBrush(p->GetParent()->GetBackgroundColour()));
            int r = p->FromDIP(8);
            dc.DrawRoundedRectangle(0, 0, sz.x, sz.y, r);

            // Color circle 24px
            int cd = p->FromDIP(24);
            int cx = p->FromDIP(10);
            int cy = (sz.y - cd) / 2;
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(src_wx_color));
            dc.DrawEllipse(cx, cy, cd, cd);
            draw_filament_swatch_ellipse_border(dc, src_wx_color, cx, cy, cd, cd);

            if (ci < m_mapping_rows.size()) {
                wxFont hex_font = p->GetFont();
                hex_font.SetPointSize(9);
                dc.SetFont(hex_font);
                dc.SetTextForeground(hex_fg);
                wxString hex_str = wxString::Format("# %s", m_mapping_rows[ci].source_hex.substr(1));
                wxSize tsz = dc.GetTextExtent(hex_str);
                dc.DrawText(hex_str, cx + cd + p->FromDIP(6), (sz.y - tsz.y) / 2);
            }
        });
        row.source_panel->Bind(wxEVT_SIZE, [](wxSizeEvent& e) {
            e.Skip();
            static_cast<wxWindow*>(e.GetEventObject())->Refresh();
        });
        row.source_panel->Bind(wxEVT_MOUSEWHEEL, &TextureImportDialog::dismiss_filament_popup_on_wheel, this);

        row_sizer->Add(row.source_panel, 0, wxEXPAND);

        // --- Arrow panel (dashed arrow) ---
        const int arrow_w = FromDIP(24);
        wxPanel* arrow_panel = new wxPanel(row_panel, wxID_ANY, wxDefaultPosition, wxSize(arrow_w, row_h));
        arrow_panel->SetMinSize(wxSize(arrow_w, row_h));
        arrow_panel->SetMaxSize(wxSize(arrow_w, row_h));
        arrow_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
        arrow_panel->Bind(wxEVT_MOUSEWHEEL, &TextureImportDialog::dismiss_filament_popup_on_wheel, this);
        arrow_panel->Bind(wxEVT_PAINT, [dash_clr](wxPaintEvent& e) {
            auto* p = static_cast<wxPanel*>(e.GetEventObject());
            wxAutoBufferedPaintDC dc(p);
            wxSize sz = p->GetClientSize();

            dc.SetBrush(wxBrush(p->GetParent()->GetBackgroundColour()));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(0, 0, sz.x, sz.y);

            int mid_y = sz.y / 2;
            int margin = p->FromDIP(2);
            int arrow_tip = sz.x - margin;
            int arrow_start = margin;

            wxPen dash_pen(dash_clr, p->FromDIP(1), wxPENSTYLE_SHORT_DASH);
            dc.SetPen(dash_pen);
            dc.DrawLine(arrow_start, mid_y, arrow_tip - p->FromDIP(4), mid_y);

            int ah = p->FromDIP(4);
            wxPoint tri[3] = {
                {arrow_tip, mid_y},
                {arrow_tip - ah, mid_y - ah / 2},
                {arrow_tip - ah, mid_y + ah / 2}
            };
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(dash_clr));
            dc.DrawPolygon(3, tri);
        });

        row_sizer->Add(arrow_panel, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(4));

        // --- Target card (numbered square + material name + chevron) ---
        row.target_panel = new wxPanel(row_panel, wxID_ANY, wxDefaultPosition, wxSize(-1, row_h),
                                       wxTAB_TRAVERSAL | wxFULL_REPAINT_ON_RESIZE);
        row.target_panel->SetMinSize(wxSize(target_min_w, row_h));
        row.target_panel->SetToolTip(get_filament_label(row.target_filament_idx));
        row.target_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
        row.target_panel->SetCursor(wxCursor(wxCURSOR_HAND));

        row.target_panel->Bind(wxEVT_PAINT, [this, ci, get_target_wxcolor, get_filament_label,
                                              display_number, card_bg, card_bd, name_fg, chev_clr](wxPaintEvent& e) {
            auto* p = static_cast<wxPanel*>(e.GetEventObject());
            wxAutoBufferedPaintDC dc(p);
            wxSize sz = p->GetClientSize();

            dc.SetBrush(wxBrush(p->GetParent()->GetBackgroundColour()));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(0, 0, sz.x, sz.y);

            if (ci >= m_mapping_rows.size()) return;
            int fil_idx = m_mapping_rows[ci].target_filament_idx;

            int r = p->FromDIP(8);
            dc.SetBrush(wxBrush(card_bg));
            dc.SetPen(wxPen(card_bd, 1));
            dc.DrawRoundedRectangle(0, 0, sz.x, sz.y, r);

            if (fil_idx >= 0 && fil_idx < (int)m_filament_entries.size() &&
                texture_entry_is_mixed(m_filament_entries[fil_idx].kind)) {
                const TextureFilamentEntry& entry = m_filament_entries[fil_idx];
                wxFont mixed_font = p->GetFont();
                mixed_font.SetPointSize(10);
                dc.SetFont(mixed_font);

                int x = p->FromDIP(10);
                const int sw = p->FromDIP(28);
                const int sw_r = p->FromDIP(6);
                const int sw_y = (sz.y - sw) / 2;
                for (size_t mi = 0; mi < entry.mixed_components.size() && mi < entry.mixed_ratios.size(); ++mi) {
                    if (mi > 0) {
                        dc.SetTextForeground(name_fg);
                        wxString plus = "+";
                        wxSize psz = dc.GetTextExtent(plus);
                        dc.DrawText(plus, x, (sz.y - psz.y) / 2);
                        x += psz.x + p->FromDIP(4);
                    }

                    const unsigned int comp_id = entry.mixed_components[mi];
                    const int comp_idx = comp_id >= 1 ? (int)comp_id - 1 : -1;
                    wxColour comp_clr("#D9D9D9");
                    if (comp_idx >= 0 && comp_idx < (int)m_filament_colors_rgba.size()) {
                        const auto& c = m_filament_colors_rgba[comp_idx];
                        comp_clr = wxColour((unsigned char)(c[0] * 255.f),
                                            (unsigned char)(c[1] * 255.f),
                                            (unsigned char)(c[2] * 255.f));
                    }

                    dc.SetPen(*wxTRANSPARENT_PEN);
                    dc.SetBrush(wxBrush(comp_clr));
                    dc.DrawRoundedRectangle(x, sw_y, sw, sw, sw_r);
                    draw_filament_swatch_border(dc, comp_clr, x, sw_y, sw, sw, sw_r);

                    wxString num_str = wxString::Format("%d", display_number(comp_idx));
                    wxSize nsz = dc.GetTextExtent(num_str);
                    dc.SetTextForeground(comp_clr.GetLuminance() < 0.6 ? *wxWHITE : texture_import_gray9000());
                    dc.DrawText(num_str, x + (sw - nsz.x) / 2, sw_y + (sw - nsz.y) / 2);
                    x += sw + p->FromDIP(5);

                    dc.SetTextForeground(name_fg);
                    wxString pct = wxString::Format("%d%%", entry.mixed_ratios[mi]);
                    wxSize pct_sz = dc.GetTextExtent(pct);
                    dc.DrawText(pct, x, (sz.y - pct_sz.y) / 2);
                    x += pct_sz.x + p->FromDIP(5);
                    if (x > sz.x - p->FromDIP(34))
                        break;
                }

                int chev_cx = sz.x - p->FromDIP(14);
                int chev_cy = sz.y / 2;
                int hw = p->FromDIP(3);
                int hh = p->FromDIP(2);
                dc.SetPen(wxPen(chev_clr, p->FromDIP(1) > 0 ? p->FromDIP(1) : 1));
                dc.DrawLine(chev_cx - hw, chev_cy - hh, chev_cx, chev_cy + hh);
                dc.DrawLine(chev_cx, chev_cy + hh, chev_cx + hw, chev_cy - hh);
                return;
            }

            // Numbered color square 32x32, rounded 6px
            int sq = p->FromDIP(32);
            int sq_x = p->FromDIP(6);
            int sq_y = (sz.y - sq) / 2;
            int sq_r = p->FromDIP(6);
            wxColour fil_clr = get_target_wxcolor(fil_idx);
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(fil_clr));
            dc.DrawRoundedRectangle(sq_x, sq_y, sq, sq, sq_r);
            draw_filament_swatch_border(dc, fil_clr, sq_x, sq_y, sq, sq, sq_r);

            {
                wxFont num_font = p->GetFont();
                num_font.SetPointSize(10);
                dc.SetFont(num_font);
                dc.SetTextForeground(fil_clr.GetLuminance() < 0.6 ? *wxWHITE : texture_import_gray9000());
                wxString num_str = wxString::Format("%d", display_number(fil_idx));
                wxSize nsz = dc.GetTextExtent(num_str);
                dc.DrawText(num_str, sq_x + (sq - nsz.x) / 2, sq_y + (sq - nsz.y) / 2);
            }

            // Material name
            {
                wxFont name_font = p->GetFont();
                name_font.SetPointSize(9);
                dc.SetFont(name_font);
                dc.SetTextForeground(name_fg);
                wxString name_str = get_filament_label(fil_idx);
                int text_x = sq_x + sq + p->FromDIP(8);
                int max_text_w = sz.x - text_x - p->FromDIP(24);
                if (max_text_w > 0) {
                    name_str = ellipsize_text(dc, name_str, max_text_w);
                    wxSize tsz = dc.GetTextExtent(name_str);
                    dc.DrawText(name_str, text_x, (sz.y - tsz.y) / 2);
                }
            }

            // Dropdown chevron at right edge
            {
                int chev_cx = sz.x - p->FromDIP(14);
                int chev_cy = sz.y / 2;
                int hw = p->FromDIP(3);
                int hh = p->FromDIP(2);
                dc.SetPen(wxPen(chev_clr, p->FromDIP(1) > 0 ? p->FromDIP(1) : 1));
                dc.DrawLine(chev_cx - hw, chev_cy - hh, chev_cx, chev_cy + hh);
                dc.DrawLine(chev_cx, chev_cy + hh, chev_cx + hw, chev_cy - hh);
            }
        });

        row.target_panel->Bind(wxEVT_LEFT_DOWN, [this, ci](wxMouseEvent&) {
            show_filament_popup(ci);
        });
        row.target_panel->Bind(wxEVT_SIZE, [](wxSizeEvent& e) {
            e.Skip();
            static_cast<wxWindow*>(e.GetEventObject())->Refresh();
        });
        row.target_panel->Bind(wxEVT_MOUSEWHEEL, &TextureImportDialog::dismiss_filament_popup_on_wheel, this);

        row_sizer->Add(row.target_panel, 1, wxEXPAND);

        row_panel->SetSizer(row_sizer);
        m_mapping_sizer->Add(row_panel, 0, wxEXPAND | wxBOTTOM, FromDIP(12));
    }

    m_mapping_scroll->FitInside();
    m_mapping_scroll->Layout();
    m_mapping_scroll->Thaw();
}

std::vector<Slic3r::FilamentMatch> TextureImportDialog::build_matches_from_rows() const
{
    std::vector<Slic3r::FilamentMatch> matches(m_mapping_rows.size());
    for (size_t i = 0; i < m_mapping_rows.size(); ++i) {
        auto& m = matches[i];
        m.cluster_index = m_mapping_rows[i].cluster_id;
        m.cluster_color = m_mapping_rows[i].source_color;

        int sel = m_mapping_rows[i].target_filament_idx;
        if (sel >= 0 && sel < (int)m_filament_colors_rgba.size()) {
            m.filament_index = sel;
            m.filament_color = m_filament_colors_rgba[sel];
            m.delta_e = Slic3r::compute_delta_e(m.cluster_color, m.filament_color);
        }
    }
    return matches;
}

// ---- Event handlers ----

void TextureImportDialog::update_color_count_preset_buttons()
{
    if (m_btn_color_4)  m_btn_color_4->SetValue(m_param_color_count == 4);
    if (m_btn_color_8)  m_btn_color_8->SetValue(m_param_color_count == 8);
    if (m_btn_color_16) m_btn_color_16->SetValue(m_param_color_count == 16);
}

void TextureImportDialog::set_color_count_value(int value, bool update_spin)
{
    m_param_color_count = std::clamp(value, 1, (int)max_filament_count());
    m_color_slider->SetValue(m_param_color_count);
    if (update_spin)
        m_color_spin->SetValue(m_param_color_count);
    update_color_count_preset_buttons();
    update_confirm_button_state();
}

void TextureImportDialog::set_smooth_value(int value, bool update_spin)
{
    m_param_smooth = std::clamp(value, 0, 10);
    m_smooth_slider->SetValue(m_param_smooth);
    if (update_spin)
        m_smooth_spin->SetValue(m_param_smooth);
    update_confirm_button_state();
}

void TextureImportDialog::preview_spin_text_value(SpinInput* spin, AccentSlider* slider, int& param,
                                                  int min_value, int max_value, const wxString& text,
                                                  std::function<void()> on_value_changed)
{
    long value;
    if (!text.ToLong(&value))
        return;

    wxTextCtrl* tc = spin->GetTextCtrl();
    long parsed = value;
    value = std::clamp((int)parsed, min_value, max_value);

    wxString normalized = text;
    if (parsed > max_value || (text.length() > 1 && text[0] == '0'))
        normalized = wxString::Format("%ld", value);

    if (normalized != text) {
        long pos = tc->GetInsertionPoint();
        tc->ChangeValue(normalized);
        if (parsed > max_value)
            tc->SetInsertionPointEnd();
        else
            tc->SetInsertionPoint(std::min<long>(normalized.length(), std::max(0L, pos - 1)));
    }

    param = (int)value;
    slider->SetValue(param);
    if (on_value_changed)
        on_value_changed();
    update_confirm_button_state();
}

void TextureImportDialog::on_color_preset_clicked(wxCommandEvent& evt)
{
    int id = evt.GetId();
    int color_count = m_param_color_count;
    if (id == ID_COLOR_4)  { color_count = 4; }
    if (id == ID_COLOR_8)  { color_count = 8; }
    if (id == ID_COLOR_16) { color_count = 16; }

    if (id == ID_COLOR_AUTO) {
        start_computation(true);
        return;
    }

    set_color_count_value(color_count, true);
}

void TextureImportDialog::on_color_slider_changed(wxCommandEvent&)
{
    set_color_count_value(m_color_slider->GetValue(), true);
}

void TextureImportDialog::on_color_spin_changed(wxCommandEvent&)
{
    set_color_count_value(m_color_spin->GetValue(), true);
}

void TextureImportDialog::on_color_spin_text_changed(wxCommandEvent& evt)
{
    preview_spin_text_value(m_color_spin, m_color_slider, m_param_color_count,
                            1, (int)max_filament_count(), evt.GetString(),
                            [this]() { update_color_count_preset_buttons(); });
}

void TextureImportDialog::on_smooth_slider_changed(wxCommandEvent&)
{
    set_smooth_value(m_smooth_slider->GetValue(), true);
}

void TextureImportDialog::on_smooth_spin_changed(wxCommandEvent&)
{
    set_smooth_value(m_smooth_spin->GetValue(), true);
}

void TextureImportDialog::on_smooth_spin_text_changed(wxCommandEvent& evt)
{
    preview_spin_text_value(m_smooth_spin, m_smooth_slider, m_param_smooth,
                            0, 10, evt.GetString());
}

void TextureImportDialog::on_apply_clicked(wxCommandEvent&)
{
    start_computation();
}

void TextureImportDialog::on_auto_merge_toggled(wxCommandEvent&)
{
    bool auto_merge_enabled = !m_auto_merge_cb || m_auto_merge_cb->GetValue();
    m_auto_merge_enabled = auto_merge_enabled;

    if (m_state == TextureImportState::Ready) {
        const auto previous_matches = m_current_matches;
        do_auto_match();
        restore_current_match_order(previous_matches);
        compact_used_virtual_filaments();
        update_filament_color_map();
        rebuild_mapping_rows();
        update_drop_warning_visibility();
        update_auto_mix_reset_visibility();
    }
}

void TextureImportDialog::highlight_view_button(int view_index)
{
    Button* btns[] = { m_btn_view_original, m_btn_view_multicolor };

    // The inactive pill lies on m_tab_panel, which is preview_bg (#EEEEEE -> #4C4C55), and has to
    // read as raised above that strip in both themes — so its fill steps away from the strip in
    // opposite directions. gDarkColors pairs one light tone with one dark tone and cannot express
    // an inversion, so the two are picked here the way filament_swatch_border_colour() does.
    const bool dark_pill = is_dark();
    StateColor inactive_bg(
        std::pair<wxColour, int>(dark_pill ? wxColour(0x5C, 0x5C, 0x64) : wxColour("#F4F4F4"), StateColor::Pressed),
        std::pair<wxColour, int>(dark_pill ? wxColour(0x66, 0x66, 0x6E) : wxColour("#F8F8F8"), StateColor::Hovered),
        std::pair<wxColour, int>(dark_pill ? wxColour(0x54, 0x54, 0x5B) : *wxWHITE,            StateColor::Normal));
    const wxColour inactive_bd = dark_pill ? wxColour(0x54, 0x54, 0x5B) : *wxWHITE;
    const wxColour inactive_text = wxColour("#6B6B6A");

    for (int i = 0; i < 2; ++i) {
        if (!btns[i]) continue;
        if (i == view_index) {
            apply_accent_button_colours(btns[i]);
        } else {
            btns[i]->SetBackgroundColor(inactive_bg);
            btns[i]->SetBorderColor(inactive_bd);
            btns[i]->SetTextColor(inactive_text);
        }
        btns[i]->Refresh();
    }
}

void TextureImportDialog::on_skip_clicked(wxCommandEvent&)
{
    m_skipped = true;
    m_new_filament_colors.clear();
    m_new_filament_preset_names.clear();
    m_new_mixed_filaments.clear();
    m_current_matches.clear();
    cancel_computation();
    EndModal(wxID_CANCEL);
}

bool TextureImportDialog::has_valid_result() const
{
    if (m_painted.face_colors.empty() || m_current_matches.empty() || m_mapping_rows.empty())
        return false;

    if (m_mapping_rows.size() != m_current_matches.size())
        return false;

    const int filament_count = (int)std::min(m_filament_colors_rgba.size(), max_filament_count());
    for (const auto& row : m_mapping_rows) {
        if (row.target_filament_idx < 0 || row.target_filament_idx >= filament_count)
            return false;
    }
    return true;
}

bool TextureImportDialog::is_params_dirty() const
{
    if (m_applied_color_count < 0)
        return false;
    return m_param_color_count != m_applied_color_count
        || m_param_smooth      != m_applied_smooth;
}

void TextureImportDialog::update_drop_warning_visibility()
{
    if (!m_drop_warning_label) return;
    // Show only when the most recent do_auto_match() ran into the filament
    // cap AND we are in the Ready state. The flag is reset at every
    // do_auto_match() entry, so any "clean" re-run automatically hides the
    // warning even if a previous run had dropped clusters.
    const bool show = (m_state == TextureImportState::Ready) && m_filaments_dropped;
    if (m_drop_warning_label->IsShown() == show) return;
    m_drop_warning_label->Show(show);
    Layout();
}

void TextureImportDialog::update_confirm_button_state()
{
    if (m_state != TextureImportState::Ready)
        return;

    if (!has_valid_result()) {
        m_btn_ok->Enable(false);
        if (m_hint_label) m_hint_label->Hide();
        m_btn_ok->Refresh();
        Layout();
        return;
    }

    m_btn_ok->Enable(true);
    style_confirm_button(is_params_dirty());

    m_btn_ok->Refresh();
    Layout();
}

// Both state updaters land here: the Confirm button reads as accent only while it would apply
// exactly what the preview shows.
void TextureImportDialog::style_confirm_button(bool dirty)
{
    if (dirty) {
        apply_muted_button_colours(m_btn_ok);
        m_btn_ok->SetToolTip(_L("Reminder: parameters changed, click Apply to take effect"));
    } else {
        apply_accent_button_colours(m_btn_ok);
        m_btn_ok->UnsetToolTip();
    }
    if (m_hint_label)
        m_hint_label->Show(dirty);
}

void TextureImportDialog::on_ok_clicked(wxCommandEvent&)
{
    if (m_state != TextureImportState::Ready || !has_valid_result() || is_params_dirty())
        return;

    m_current_matches = build_matches_from_rows();
    if (m_current_matches.empty())
        return;

    compact_used_virtual_filaments();

    EndModal(wxID_OK);
}

// ---- Result accessors ----

Slic3r::PaintedMesh TextureImportDialog::get_painted_mesh() const
{
    return m_painted;
}

std::vector<Slic3r::FilamentMatch> TextureImportDialog::get_matches() const
{
    if (!m_current_matches.empty())
        return m_current_matches;
    return build_matches_from_rows();
}

void TextureImportDialog::on_dpi_changed(const wxRect&)
{
    // All control sizes below are baked into persistent properties (min size,
    // corner radius, fixed wxSize) using FromDIP() at build time. The base
    // DPIAware::rescale() only rescales fonts; it does not recompute these
    // stored pixel values. Re-apply them here so the layout stays consistent
    // when the dialog is dragged to a screen with a different DPI.
    SetMinSize(wxSize(FromDIP(800), FromDIP(500)));

    const int view_button_height = FromDIP(27);
    for (Button* btn : {m_btn_view_original, m_btn_view_multicolor}) {
        if (btn) {
            btn->SetCornerRadius(view_button_height / 2);
            btn->SetMinSize(wxSize(FromDIP(57), view_button_height));
        }
    }

    for (Button* btn : {m_btn_color_4, m_btn_color_8, m_btn_color_16}) {
        if (btn) {
            btn->SetCornerRadius(FromDIP(12));
            btn->SetMinSize(wxSize(FromDIP(28), FromDIP(28)));
        }
    }

    if (m_btn_color_auto) {
        m_btn_color_auto->SetCornerRadius(FromDIP(12));
        m_btn_color_auto->SetMinSize(wxSize(FromDIP(60), FromDIP(28)));
    }
    if (m_btn_apply) {
        m_btn_apply->SetCornerRadius(FromDIP(12));
        m_btn_apply->SetMinSize(wxSize(FromDIP(60), FromDIP(28)));
    }
    if (m_btn_auto_mix) {
        m_btn_auto_mix->SetCornerRadius(FromDIP(14));
        m_btn_auto_mix->SetMinSize(wxSize(FromDIP(178), FromDIP(28)));
    }
    if (m_btn_mix_reset)
        m_btn_mix_reset->SetPaddingSize(wxSize(FromDIP(2), FromDIP(2)));

    if (m_color_spin)
        m_color_spin->SetMinSize(wxSize(FromDIP(60), FromDIP(28)));
    if (m_smooth_spin)
        m_smooth_spin->SetMinSize(wxSize(FromDIP(60), FromDIP(28)));

    if (m_mapping_scroll) {
        m_mapping_scroll->SetMinSize(wxSize(-1, FromDIP(300)));
        m_mapping_scroll->SetScrollRate(0, FromDIP(10));
    }

    if (m_btn_skip) {
        m_btn_skip->SetCornerRadius(FromDIP(20));
        m_btn_skip->SetMinSize(wxSize(FromDIP(136), FromDIP(40)));
    }
    if (m_btn_ok) {
        m_btn_ok->SetCornerRadius(FromDIP(20));
        m_btn_ok->SetMinSize(wxSize(FromDIP(156), FromDIP(40)));
    }

    // Mapping rows store their panel sizes (source/target/arrow/row height)
    // as fixed FromDIP min/max sizes, so rebuild them to pick up the new DPI.
    rebuild_mapping_rows();

    if (wxSizer* sizer = GetSizer())
        sizer->Layout();
    Layout();
    Refresh();
    wxGetApp().UpdateDlgDarkUI(this);
}

}} // namespace Slic3r::GUI
