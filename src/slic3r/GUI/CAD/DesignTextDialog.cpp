#include "DesignTextDialog.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Widgets/DialogButtons.hpp"
#include "slic3r/GUI/Widgets/TextInput.hpp"
#include "slic3r/GUI/Widgets/ComboBox.hpp"
#include "slic3r/GUI/Widgets/CheckBox.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/Utils/WxFontUtils.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Emboss.hpp"
#include "libslic3r/CAD/SketchImport.hpp"
#include "libslic3r/Point.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"

#include <wx/arrstr.h>
#include <wx/dcbuffer.h>
#include <wx/event.h>
#include <wx/font.h>
#include <wx/fontenc.h>
#include <wx/fontenum.h>
#include <wx/gdicmn.h>
#include <wx/graphics.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/textctrl.h>
#include <wx/tglbtn.h>
#include <wx/toplevel.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>

namespace Slic3r { namespace GUI {

static const char* kFontKey   = "cad_text_font";     // WxFontUtils::store_wxFont descriptor
static const char* kHeightKey = "cad_text_height";   // mm

// Installed scalable faces, sorted, enumerated once per session: listing them takes long enough on
// a machine with many fonts to be felt each time the dialog opened.
static const wxArrayString& font_faces()
{
    static wxArrayString faces = [] {
        // '@' faces are the vertical-writing aliases GTK/MSW list twice.
        wxArrayString f = wxFontEnumerator::GetFacenames(wxFONTENCODING_SYSTEM, false);
        f.erase(std::remove_if(f.begin(), f.end(), [](const wxString& n) { return n.StartsWith("@"); }), f.end());
        f.Sort();
        return f;
    }();
    return faces;
}

static bool parse_mm(wxString t, double& out)
{
    t.Replace(",", ".");
    return t.Trim(true).Trim(false).ToCDouble(&out) && out >= 0.5 && out <= 500.0;
}

DesignTextDialog::DesignTextDialog(wxWindow* parent, const Spec* initial)
    : DPIDialog(parent, wxID_ANY, _L("Text"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    SetFont(Label::Body_14);
    SetBackgroundColour(*wxWHITE);   // light palette colour, dark-mapped by UpdateDlgDarkUI
    const int em = em_unit();

    // The text being edited, else the last used font and height, else the GUI font at 10 mm.
    wxFont font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
    double height = 10.0;
    if (initial != nullptr) {
        wxFont f = WxFontUtils::load_wxFont(initial->font);
        if (f.IsOk()) font = f;
        if (initial->height > 0.0) height = std::clamp(initial->height, 0.5, 500.0);
    } else if (AppConfig* cfg = wxGetApp().app_config) {
        const std::string desc = cfg->get(kFontKey);
        if (!desc.empty()) {
            wxFont f = WxFontUtils::load_wxFont(desc);
            if (f.IsOk()) font = f;
        }
        const std::string h = cfg->get(kHeightKey);
        if (!h.empty()) {
            try { height = std::clamp(std::stod(h), 0.5, 500.0); } catch (...) {}
        }
    }
    m_last_height = height;

    auto* form = new wxFlexGridSizer(2, em / 2, em);
    form->AddGrowableCol(1, 1);
    auto label = [this, form](const wxString& text) {
        form->Add(new wxStaticText(this, wxID_ANY, text), 0, wxALIGN_CENTER_VERTICAL);
    };

    m_text = new ::TextInput(this, initial ? initial->text : wxString(), "", "", wxDefaultPosition,
                             wxSize(30 * em, -1), wxTE_PROCESS_ENTER);
    m_text->GetTextCtrl()->SetHint(_L("Type the text to insert"));
    label(_L("Text"));
    form->Add(m_text, 1, wxEXPAND);

    m_face = new ::ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(30 * em, -1), 0, nullptr,
                            wxCB_READONLY);
    const wxArrayString& faces = font_faces();
    for (const wxString& f : faces)
        m_face->Append(f);
    const int sel = faces.Index(font.GetFaceName());
    m_face->SetSelection(sel != wxNOT_FOUND ? sel : 0);
    label(_L("Font"));
    form->Add(m_face, 1, wxEXPAND);

    // Orca's CheckBox carries no label: each one sits beside its own text.
    auto* style = new wxBoxSizer(wxHORIZONTAL);
    auto check = [this, style, em](const wxString& text, bool value) {
        auto* c = new ::CheckBox(this);
        c->SetValue(value);
        style->Add(c, 0, wxALIGN_CENTER_VERTICAL);
        style->Add(new wxStaticText(this, wxID_ANY, text), 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, em / 2);
        return c;
    };
    m_bold   = check(_L("Bold"), WxFontUtils::is_bold(font));
    m_italic = check(_L("Italic"), WxFontUtils::is_italic(font));
    form->AddSpacer(0);
    form->Add(style, 0);

    m_height = new ::TextInput(this, wxString::FromCDouble(height, 1), _L("mm"), "", wxDefaultPosition,
                               wxSize(10 * em, -1), wxTE_PROCESS_ENTER);
    label(_L("Height"));
    form->Add(m_height, 0);

    // The outline of what will be inserted, fitted to the box, with its real size under it.
    // A thumbnail only: the text itself is drawn in the canvas, where it will be.
    m_preview = new wxWindow(this, wxID_ANY, wxDefaultPosition, wxSize(30 * em, 5 * em));
    m_preview->SetBackgroundStyle(wxBG_STYLE_PAINT);
    m_preview->Bind(wxEVT_PAINT, [this](wxPaintEvent&) { draw_preview(m_preview); });
    m_preview->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { m_preview->Refresh(); e.Skip(); });
    m_size = new wxStaticText(this, wxID_ANY, wxEmptyString);

    auto* buttons = new DialogButtons(this, {"OK", "Cancel"});
    m_ok = buttons->GetOK();

    auto* top = new wxBoxSizer(wxVERTICAL);
    top->Add(form, 0, wxEXPAND | wxALL, em);
    top->Add(m_preview, 1, wxEXPAND | wxLEFT | wxRIGHT, em);
    top->Add(m_size, 0, wxLEFT | wxRIGHT | wxTOP, em);
    top->Add(buttons, 0, wxEXPAND);

    m_text->Bind(wxEVT_TEXT, [this](wxCommandEvent& e) { update_preview(); e.Skip(); });
    m_text->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { accept(); });   // Enter = OK, as everywhere in the tab
    m_face->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& e) { load_font(); update_preview(); e.Skip(); });
    for (::CheckBox* c : {m_bold, m_italic})
        c->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) { e.Skip(); CallAfter([this] { load_font(); update_preview(); }); });
    m_height->Bind(wxEVT_TEXT, [this](wxCommandEvent& e) { update_preview(); e.Skip(); });
    m_height->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { accept(); });
    buttons->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { accept(); });
    buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { cancel(); });
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { cancel(); });
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
        if (e.GetKeyCode() == WXK_ESCAPE) cancel();
        else                              e.Skip();
    });

    SetSizerAndFit(top);
    // Out of the middle of the window, where the text is being placed: top right of the parent.
    if (parent != nullptr) {
        const wxRect pr = parent->GetScreenRect();
        SetPosition(wxPoint(std::max(pr.GetLeft(), pr.GetRight() - GetSize().x - 2 * em), pr.GetTop() + 8 * em));
    } else {
        CenterOnParent();
    }
    wxGetApp().UpdateDlgDarkUI(this);

    load_font();
    update_preview();
    m_text->GetTextCtrl()->SetFocus();
}

double DesignTextDialog::height_mm() const
{
    double h = 0.0;
    if (parse_mm(m_height->GetTextCtrl()->GetValue(), h))
        const_cast<DesignTextDialog*>(this)->m_last_height = h;
    return m_last_height;
}

wxString DesignTextDialog::text() const { return m_text->GetTextCtrl()->GetValue(); }

DesignTextDialog::Spec DesignTextDialog::spec() const
{
    return { text(), WxFontUtils::store_wxFont(current_font()), height_mm() };
}

void DesignTextDialog::accept()
{
    if (m_done || m_regions.empty()) return;        // nothing to insert: the size line says why
    m_done = true;
    if (AppConfig* cfg = wxGetApp().app_config) {
        cfg->set(kFontKey, WxFontUtils::store_wxFont(current_font()));
        cfg->set(kHeightKey, std::to_string(height_mm()));
    }
    if (on_accept) on_accept();
}

void DesignTextDialog::cancel()
{
    if (m_done) return;
    m_done = true;
    if (on_cancel) on_cancel();
}

wxFont DesignTextDialog::current_font() const
{
    wxFontInfo info(12);
    if (m_face->GetSelection() != wxNOT_FOUND)
        info.FaceName(m_face->GetString(m_face->GetSelection()));
    info.Bold(m_bold->GetValue()).Italic(m_italic->GetValue());
    return wxFont(info);
}

void DesignTextDialog::load_font()
{
    const wxFont font = current_font();
    m_font_file.reset();
    if (font.IsOk() && WxFontUtils::can_load(font))
        m_font_file = WxFontUtils::create_font_file(font);
}

void DesignTextDialog::update_preview()
{
    const std::string utf8(text().ToUTF8().data());
    m_regions.clear();
    if (m_font_file && !utf8.empty())
        m_regions = text_to_regions(utf8, height_mm(), m_font_file);

    wxString line;
    if (!m_font_file)
        line = _L("This font cannot be used for text — pick another one");
    else if (utf8.empty())
        line = _L("Type the text to see its size");
    else if (m_regions.empty())
        line = _L("This font has no outline for this text");
    else {
        double lo_x = std::numeric_limits<double>::max(), lo_y = lo_x;
        double hi_x = -lo_x, hi_y = -lo_x;
        for (const auto& r : m_regions)
            for (const auto& c : r)
                for (const Vec2d& p : c) {
                    lo_x = std::min(lo_x, p.x()); hi_x = std::max(hi_x, p.x());
                    lo_y = std::min(lo_y, p.y()); hi_y = std::max(hi_y, p.y());
                }
        line = wxString::Format(_L("Size: %.1f × %.1f mm"), hi_x - lo_x, hi_y - lo_y);
    }
    m_size->SetLabel(line);
    if (m_ok) m_ok->Enable(!m_regions.empty());
    m_preview->Refresh();
    if (on_change) on_change();
}

void DesignTextDialog::draw_preview(wxWindow* canvas)
{
    wxAutoBufferedPaintDC dc(canvas);
    const bool dark = wxGetApp().dark_mode();
    dc.SetBackground(wxBrush(dark ? wxColour(0x1e, 0x1e, 0x22) : wxColour(0xf4, 0xf4, 0xf6)));
    dc.Clear();
    if (m_regions.empty()) return;

    double lo_x = std::numeric_limits<double>::max(), lo_y = lo_x, hi_x = -lo_x, hi_y = -lo_x;
    for (const auto& r : m_regions)
        for (const auto& c : r)
            for (const Vec2d& p : c) {
                lo_x = std::min(lo_x, p.x()); hi_x = std::max(hi_x, p.x());
                lo_y = std::min(lo_y, p.y()); hi_y = std::max(hi_y, p.y());
            }
    const wxSize sz  = canvas->GetClientSize();
    const double pad = 0.08 * std::min(sz.x, sz.y);
    const double w = std::max(hi_x - lo_x, 1e-6), h = std::max(hi_y - lo_y, 1e-6);
    const double k = std::min((sz.x - 2 * pad) / w, (sz.y - 2 * pad) / h);
    const double ox = 0.5 * (sz.x - k * w), oy = 0.5 * (sz.y + k * h);   // y up -> screen y down

    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc) return;
    wxGraphicsPath path = gc->CreatePath();
    for (const auto& r : m_regions)
        for (const auto& c : r) {
            if (c.empty()) continue;
            path.MoveToPoint(ox + k * (c[0].x() - lo_x), oy - k * (c[0].y() - lo_y));
            for (size_t i = 1; i < c.size(); ++i)
                path.AddLineToPoint(ox + k * (c[i].x() - lo_x), oy - k * (c[i].y() - lo_y));
            path.CloseSubpath();
        }
    gc->SetBrush(wxBrush(wxColour(0x00, 0x96, 0x88)));   // Orca accent
    gc->SetPen(*wxTRANSPARENT_PEN);
    gc->FillPath(path, wxODDEVEN_RULE);                    // holes (the counter of an "o") stay open
}

void DesignTextDialog::on_dpi_changed(const wxRect&)
{
    m_text->Rescale();
    m_face->Rescale();
    m_height->Rescale();
    m_bold->Rescale();
    m_italic->Rescale();
    GetSizer()->SetSizeHints(this);
    Refresh();
}

void DesignTextDialog::on_sys_color_changed()
{
    SetBackgroundColour(*wxWHITE);
    wxGetApp().UpdateDlgDarkUI(this);
    m_preview->Refresh();   // its colours are read at paint time
    Refresh();
}

}} // namespace Slic3r::GUI
