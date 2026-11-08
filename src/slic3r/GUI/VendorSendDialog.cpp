#include "VendorSendDialog.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <thread>

#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>

#include <wx/choice.h>
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <wx/graphics.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/wrapsizer.h>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"

namespace fs = boost::filesystem;

namespace Slic3r { namespace GUI {

// ---------------------------------------------------------------------------------------------
// Vendor detection and backend registry
// ---------------------------------------------------------------------------------------------

static SendVendor vendor_from_name(std::string name)
{
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    if (name == "bbl" || name.find("bambu") != std::string::npos)
        return SendVendor::Bambu;
    if (name.find("elegoo") != std::string::npos)
        return SendVendor::Elegoo;
    if (name.find("creality") != std::string::npos)
        return SendVendor::Creality;
    if (name.find("markforged") != std::string::npos)
        return SendVendor::Markforged;
    return SendVendor::None;
}

SendVendor current_send_vendor()
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return SendVendor::None;
    const Preset& preset = bundle->printers.get_edited_preset();
    if (const auto* model = preset.config.opt<ConfigOptionString>("printer_model"); model != nullptr && !model->value.empty())
        for (const auto& [vendor_id, profile] : bundle->vendors)
            for (const auto& m : profile.models)
                if (m.name == model->value)
                    return vendor_from_name(vendor_id);
    if (const Preset* parent = bundle->printers.get_preset_parent(preset); parent != nullptr && parent->vendor != nullptr)
        return vendor_from_name(parent->vendor->id);
    return SendVendor::None;
}

static std::map<SendVendor, VendorSendBackendFactory>& registry()
{
    static std::map<SendVendor, VendorSendBackendFactory> factories;
    return factories;
}

void register_vendor_send_backend(SendVendor vendor, VendorSendBackendFactory factory) { registry()[vendor] = std::move(factory); }

std::shared_ptr<VendorSendBackend> make_vendor_send_backend(SendVendor vendor, const DynamicPrintConfig& printer_config)
{
    auto it = registry().find(vendor);
    return it == registry().end() || !it->second ? nullptr : it->second(printer_config);
}

namespace {

// ---------------------------------------------------------------------------------------------
// Look of each vendor's app
// ---------------------------------------------------------------------------------------------

struct Theme
{
    wxString title;
    wxColour bg, panel, text, subtext, border, accent, accent_text, error, track_off;
    int      radius{6};
    bool     pill_buttons{false};
};

Theme theme_for(SendVendor vendor, bool dark)
{
    Theme t;
    switch (vendor) {
    case SendVendor::Elegoo: // ElegooSlicer print-send page
        t.title       = _L("Print");
        t.bg          = dark ? wxColour("#242428") : wxColour("#FFFFFF");
        t.panel       = dark ? wxColour("#2F2F34") : wxColour("#F7F7F7");
        t.text        = dark ? wxColour("#E5E5E5") : wxColour("#333333");
        t.subtext     = dark ? wxColour("#8C8C8C") : wxColour("#999999");
        t.border      = dark ? wxColour("#45454A") : wxColour("#DEDEDE");
        t.accent      = wxColour("#1677FF");
        t.accent_text = *wxWHITE;
        t.error       = wxColour("#FF2F14");
        t.track_off   = dark ? wxColour("#55555A") : wxColour("#B8B8B8");
        t.radius      = 6;
        break;
    case SendVendor::Creality: // Creality Print "Send print job"
        t.title       = _L("Send Print Job");
        t.bg          = dark ? wxColour("#1F1F21") : wxColour("#FFFFFF");
        t.panel       = dark ? wxColour("#2C2C2F") : wxColour("#F2F3F5");
        t.text        = dark ? wxColour("#FFFFFF") : wxColour("#1F1F1F");
        t.subtext     = dark ? wxColour("#9A9A9F") : wxColour("#7A7A7F");
        t.border      = dark ? wxColour("#3D3D41") : wxColour("#DCDCE0");
        t.accent      = wxColour("#17CC5F");
        t.accent_text = *wxWHITE;
        t.error       = wxColour("#FF4D4F");
        t.track_off   = dark ? wxColour("#4A4A4F") : wxColour("#C4C4C8");
        t.radius      = 8;
        t.pill_buttons = true;
        break;
    case SendVendor::Markforged: // Eiger print panel
        t.title       = _L("Send to Printer");
        t.bg          = dark ? wxColour("#1C1C1C") : wxColour("#FFFFFF");
        t.panel       = dark ? wxColour("#2A2A2A") : wxColour("#F5F5F5");
        t.text        = dark ? wxColour("#F2F2F2") : wxColour("#1A1A1A");
        t.subtext     = dark ? wxColour("#A0A0A0") : wxColour("#6B6B6B");
        t.border      = dark ? wxColour("#3A3A3A") : wxColour("#E0E0E0");
        t.accent      = dark ? wxColour("#F2F2F2") : wxColour("#1A1A1A");
        t.accent_text = dark ? wxColour("#1A1A1A") : *wxWHITE;
        t.error       = wxColour("#D93025");
        t.track_off   = dark ? wxColour("#4A4A4A") : wxColour("#BDBDBD");
        t.radius      = 2;
        break;
    default:
        t.title       = _L("Print");
        t.bg          = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
        t.panel       = t.bg;
        t.text        = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
        t.subtext     = t.text;
        t.border      = t.text;
        t.accent      = wxColour("#009688");
        t.accent_text = *wxWHITE;
        t.error       = *wxRED;
        t.track_off   = wxColour("#BDBDBD");
        break;
    }
    return t;
}

wxColour contrast_text(const wxColour& c)
{
    const double l = 0.299 * c.Red() + 0.587 * c.Green() + 0.114 * c.Blue();
    return l > 150 ? wxColour("#1A1A1A") : *wxWHITE;
}

wxColour parse_colour(const std::string& s)
{
    wxColour c(wxString::FromUTF8(s.size() == 9 && s[0] == '#' ? s.substr(0, 7) : s)); // drop alpha of "#RRGGBBAA"
    return c.IsOk() ? c : wxColour("#FFFFFF");
}

wxString format_time(float seconds)
{
    const int s = static_cast<int>(std::lround(seconds));
    const int d = s / 86400, h = s % 86400 / 3600, m = s % 3600 / 60;
    if (d > 0)
        return wxString::Format("%dd %dh %dm", d, h, m);
    if (h > 0)
        return wxString::Format("%dh %dm", h, m);
    return wxString::Format("%dm %ds", m, s % 60);
}

wxString format_weight(double g)
{
    if (wxGetApp().app_config->get_bool("use_inches"))
        return wxString::Format("%.2f oz", g * 0.035274);
    return wxString::Format("%.2f g", g);
}

// ---------------------------------------------------------------------------------------------
// Small painted controls, so each dialog can take its vendor's colours on every platform
// ---------------------------------------------------------------------------------------------

class FlatButton : public wxWindow
{
public:
    FlatButton(wxWindow* parent, const wxString& label, const Theme& theme, bool primary)
        : wxWindow(parent, wxID_ANY), m_label(label), m_theme(theme), m_primary(primary)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetFont(GetFont().Bold());
        update_size();
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, &FlatButton::on_paint, this);
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { m_hover = true; Refresh(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { m_hover = false; Refresh(); });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            if (!IsEnabled())
                return;
            wxCommandEvent evt(wxEVT_BUTTON, GetId());
            evt.SetEventObject(this);
            ProcessWindowEvent(evt);
        });
    }
    void SetLabel(const wxString& label) override
    {
        m_label = label;
        update_size();
        Refresh();
    }
    bool Enable(bool enable = true) override
    {
        const bool r = wxWindow::Enable(enable);
        Refresh();
        return r;
    }

private:
    void update_size()
    {
        const wxSize t = GetTextExtent(m_label);
        SetMinSize(wxSize(std::max(t.x + FromDIP(36), FromDIP(96)), t.y + FromDIP(16)));
    }
    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;
        const wxSize sz = GetClientSize();
        wxColour     fill = m_primary ? m_theme.accent : m_theme.bg;
        wxColour     text = m_primary ? m_theme.accent_text : m_theme.text;
        if (!IsEnabled()) {
            fill = m_primary ? m_theme.track_off : m_theme.bg;
            text = m_primary ? m_theme.accent_text : m_theme.subtext;
        } else if (m_hover)
            fill = fill.ChangeLightness(m_primary ? 110 : 96);
        const double r = m_theme.pill_buttons ? sz.y / 2.0 : FromDIP(m_theme.radius);
        gc->SetBrush(wxBrush(fill));
        gc->SetPen(m_primary ? *wxTRANSPARENT_PEN : wxPen(m_theme.border, 1));
        gc->DrawRoundedRectangle(0.5, 0.5, sz.x - 1, sz.y - 1, r);
        gc->SetFont(GetFont(), text);
        double w, h;
        gc->GetTextExtent(m_label, &w, &h);
        gc->DrawText(m_label, (sz.x - w) / 2, (sz.y - h) / 2);
    }

    wxString m_label;
    Theme    m_theme;
    bool     m_primary;
    bool     m_hover{false};
};

class Toggle : public wxWindow
{
public:
    Toggle(wxWindow* parent, const Theme& theme, bool value) : wxWindow(parent, wxID_ANY), m_theme(theme), m_value(value)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(FromDIP(wxSize(36, 20)));
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, &Toggle::on_paint, this);
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            if (!IsEnabled())
                return;
            m_value = !m_value;
            Refresh();
            wxCommandEvent evt(wxEVT_CHECKBOX, GetId());
            evt.SetInt(m_value ? 1 : 0);
            evt.SetEventObject(this);
            ProcessWindowEvent(evt);
        });
    }
    bool GetValue() const { return m_value; }
    void SetValue(bool v)
    {
        m_value = v;
        Refresh();
    }

private:
    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;
        const wxSize sz = GetClientSize();
        const double h  = sz.y - 2;
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(m_value ? m_theme.accent : m_theme.track_off));
        gc->DrawRoundedRectangle(1, 1, sz.x - 2, h, h / 2);
        const double knob = h - 4;
        gc->SetBrush(wxBrush(m_value ? m_theme.accent_text : *wxWHITE));
        gc->DrawEllipse(m_value ? sz.x - 3 - knob : 3, 3, knob, knob);
    }
    Theme m_theme;
    bool  m_value;
};

// A coloured filament block: type on top, weight below (ElegooSlicer / Creality Print style).
class Swatch : public wxWindow
{
public:
    Swatch(wxWindow* parent, const Theme& theme, const wxColour& colour, const wxString& top, const wxString& bottom, bool clickable)
        : wxWindow(parent, wxID_ANY), m_theme(theme), m_colour(colour), m_top(top), m_bottom(bottom)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(FromDIP(wxSize(64, 44)));
        if (clickable)
            SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, &Swatch::on_paint, this);
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            wxCommandEvent evt(wxEVT_BUTTON, GetId());
            evt.SetEventObject(this);
            ProcessWindowEvent(evt);
        });
    }
    void set(const wxColour& colour, const wxString& top, const wxString& bottom, bool empty)
    {
        m_colour = colour;
        m_top    = top;
        m_bottom = bottom;
        m_empty  = empty;
        Refresh();
    }

private:
    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;
        const wxSize sz = GetClientSize();
        gc->SetBrush(m_empty ? wxBrush(m_theme.panel) : wxBrush(m_colour));
        gc->SetPen(wxPen(m_empty ? m_theme.border : m_colour.ChangeLightness(85), 1, m_empty ? wxPENSTYLE_SHORT_DASH : wxPENSTYLE_SOLID));
        gc->DrawRoundedRectangle(0.5, 0.5, sz.x - 1, sz.y - 1, FromDIP(m_theme.radius));
        const wxColour fg = m_empty ? m_theme.subtext : contrast_text(m_colour);
        gc->SetFont(GetFont().Bold(), fg);
        double w, h;
        gc->GetTextExtent(m_top, &w, &h);
        const double y0 = m_bottom.empty() ? (sz.y - h) / 2 : sz.y / 2.0 - h;
        gc->DrawText(m_top, (sz.x - w) / 2, y0);
        if (!m_bottom.empty()) {
            gc->SetFont(GetFont(), fg);
            gc->GetTextExtent(m_bottom, &w, &h);
            gc->DrawText(m_bottom, (sz.x - w) / 2, sz.y / 2.0);
        }
    }
    Theme    m_theme;
    wxColour m_colour;
    wxString m_top, m_bottom;
    bool     m_empty{false};
};

// A selectable printer row (Creality Print device list, Eiger printer table).
class PrinterCard : public wxWindow
{
public:
    PrinterCard(wxWindow* parent, const Theme& theme, const SendPrinter& printer) : wxWindow(parent, wxID_ANY), m_theme(theme), m_printer(printer)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetMinSize(FromDIP(wxSize(260, 52)));
        SetCursor(wxCursor(wxCURSOR_HAND));
        Bind(wxEVT_PAINT, &PrinterCard::on_paint, this);
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
            wxCommandEvent evt(wxEVT_BUTTON, GetId());
            evt.SetEventObject(this);
            ProcessWindowEvent(evt);
        });
        SetToolTip(printer.detail);
    }
    void set_selected(bool s)
    {
        m_selected = s;
        Refresh();
    }
    const SendPrinter& printer() const { return m_printer; }

private:
    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (!gc)
            return;
        const wxSize sz = GetClientSize();
        gc->SetBrush(wxBrush(m_theme.panel));
        gc->SetPen(wxPen(m_selected ? m_theme.accent : m_theme.border, m_selected ? 2 : 1));
        gc->DrawRoundedRectangle(1, 1, sz.x - 2, sz.y - 2, FromDIP(m_theme.radius));
        const int pad = FromDIP(12);
        // status dot
        const wxColour dot = !m_printer.online ? m_theme.subtext : m_printer.busy ? wxColour("#F5A623") : wxColour("#3BB273");
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->SetBrush(wxBrush(dot));
        const double d = FromDIP(8);
        gc->DrawEllipse(pad, (sz.y - d) / 2, d, d);
        const double x = pad + d + FromDIP(10);
        gc->SetFont(GetFont().Bold(), m_theme.text);
        double w, h;
        gc->GetTextExtent(m_printer.name, &w, &h);
        gc->DrawText(m_printer.name, x, sz.y / 2.0 - h - 1);
        gc->SetFont(GetFont(), m_theme.subtext);
        gc->DrawText(m_printer.detail, x, sz.y / 2.0 + 1);
        gc->GetTextExtent(m_printer.status, &w, &h);
        gc->SetFont(GetFont(), m_printer.busy ? wxColour("#F5A623") : m_theme.subtext);
        gc->DrawText(m_printer.status, sz.x - pad - w, (sz.y - h) / 2);
    }
    Theme       m_theme;
    SendPrinter m_printer;
    bool        m_selected{false};
};

wxStaticText* make_text(wxWindow* parent, const wxString& s, const wxColour& c, bool bold = false, int size_delta = 0)
{
    auto*  t = new wxStaticText(parent, wxID_ANY, s);
    wxFont f = t->GetFont();
    if (bold)
        f = f.Bold();
    if (size_delta != 0)
        f.SetPointSize(f.GetPointSize() + size_delta);
    t->SetFont(f);
    t->SetForegroundColour(c);
    return t;
}

// "Title ———————" section header used by ElegooSlicer.
wxSizer* section_header(wxWindow* parent, const Theme& theme, const wxString& title)
{
    auto* s = new wxBoxSizer(wxHORIZONTAL);
    s->Add(make_text(parent, title, theme.text, true), 0, wxALIGN_CENTER_VERTICAL);
    auto* line = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    line->SetBackgroundColour(theme.border);
    s->Add(line, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, parent->FromDIP(10));
    return s;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// The dialog
// ---------------------------------------------------------------------------------------------

class VendorSendDialog : public DPIDialog
{
public:
    VendorSendDialog(wxWindow* parent, SendVendor vendor, std::shared_ptr<VendorSendBackend> backend, const SendJobSummary& job,
                     const fs::path& default_output)
        : DPIDialog(parent, wxID_ANY, theme_for(vendor, wxGetApp().dark_mode()).title, wxDefaultPosition, wxDefaultSize,
                    wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
        , m_vendor(vendor)
        , m_backend_ptr(std::move(backend))
        , m_backend(*m_backend_ptr)
        , m_job(job)
        , m_default_output(default_output)
        , m_theme(theme_for(vendor, wxGetApp().dark_mode()))
    {
        SetBackgroundColour(m_theme.bg);
        m_extension = default_output.extension().string();
        if (default_output.filename().string().size() > 10 &&
            default_output.filename().string().compare(default_output.filename().string().size() - 10, 10, ".gcode.3mf") == 0)
            m_extension = ".gcode.3mf";
        build();
        SetSizerAndFit(m_root);
        SetMinSize(GetSize());
        CentreOnParent();
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) {
            *m_alive = false;
            e.Skip();
        });
        if (m_backend.needs_sign_in() && !m_backend.signed_in() && !m_backend.has_local_printers())
            show_sign_in();
        else
            load_printers();
    }
    ~VendorSendDialog() override { *m_alive = false; }

    PrintHostJob take_job() { return std::move(m_result); }

protected:
    void on_dpi_changed(const wxRect&) override
    {
        Layout();
        Fit();
    }

private:
    std::string cfg_key(const std::string& key) const
    {
        static const char* names[] = {"none", "bambu", "elegoo", "creality", "markforged"};
        return std::string(names[static_cast<int>(m_vendor)]) + "_" + key;
    }
    std::string recall(const std::string& key, const std::string& def) const
    {
        const std::string v = wxGetApp().app_config->get("vendor_send", cfg_key(key));
        return v.empty() ? def : v;
    }
    void remember(const std::string& key, const std::string& value) { wxGetApp().app_config->set("vendor_send", cfg_key(key), value); }

    // -- layout ----------------------------------------------------------------------------------

    void build()
    {
        m_root          = new wxBoxSizer(wxVERTICAL);
        const int  pad  = FromDIP(20);
        wxBoxSizer* body = new wxBoxSizer(wxVERTICAL);

        // Header: thumbnail, name, stats. Creality Print and Eiger put the model on the left of the printer list.
        const bool side_by_side = m_vendor == SendVendor::Creality || m_vendor == SendVendor::Markforged;
        auto*      top          = new wxBoxSizer(side_by_side ? wxHORIZONTAL : wxVERTICAL);
        auto*      model_row    = new wxBoxSizer(side_by_side ? wxVERTICAL : wxHORIZONTAL);
        const int  thumb        = FromDIP(side_by_side ? 180 : 120);
        auto*      thumb_panel  = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(thumb, thumb));
        thumb_panel->SetBackgroundColour(m_theme.panel);
        if (m_job.thumbnail.IsOk()) {
            wxImage img = m_job.thumbnail.Copy();
            img.Rescale(thumb, thumb, wxIMAGE_QUALITY_HIGH);
            auto* bmp = new wxStaticBitmap(thumb_panel, wxID_ANY, wxBitmap(img));
            auto* ts  = new wxBoxSizer(wxVERTICAL);
            ts->Add(bmp, 1, wxEXPAND);
            thumb_panel->SetSizer(ts);
        }
        model_row->Add(thumb_panel, 0, side_by_side ? wxALIGN_CENTER_HORIZONTAL : 0);

        auto* info = new wxBoxSizer(wxVERTICAL);
        m_name     = new wxTextCtrl(this, wxID_ANY, m_job.name, wxDefaultPosition, wxSize(FromDIP(side_by_side ? 180 : 260), -1));
        m_name->SetMaxLength(50);
        m_name->SetBackgroundColour(m_theme.bg);
        m_name->SetForegroundColour(m_theme.text);
        m_name->SetFont(m_name->GetFont().Bold());
        info->Add(m_name, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
        auto* stats = new wxBoxSizer(wxHORIZONTAL);
        auto  stat  = [&](const wxString& glyph, const wxString& value) {
            stats->Add(make_text(this, glyph, m_theme.subtext), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
            stats->Add(make_text(this, value, m_theme.text), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(14));
        };
        stat(wxString::FromUTF8("\xE2\x8F\xB1"), m_job.time_s > 0 ? format_time(m_job.time_s) : wxString("--"));
        stat(wxString::FromUTF8("\xE2\x9A\x96"), format_weight(m_job.weight_g));
        if (m_vendor == SendVendor::Elegoo)
            stat(wxString::FromUTF8("\xE2\x89\xA1"), wxString::Format("%d", m_job.layers));
        info->Add(stats, 0, wxBOTTOM, FromDIP(4));

        if (side_by_side) {
            model_row->Add(info, 0, wxTOP | wxEXPAND, FromDIP(10));
            top->Add(model_row, 0, wxRIGHT, pad);
            auto* right = new wxBoxSizer(wxVERTICAL);
            build_printer_list(right);
            top->Add(right, 1, wxEXPAND);
            body->Add(top, 0, wxEXPAND);
        } else {
            model_row->Add(info, 1, wxLEFT | wxEXPAND, FromDIP(16));
            top->Add(model_row, 0, wxEXPAND);
            body->Add(top, 0, wxEXPAND);
            body->Add(section_header(this, m_theme, _L("Printer")), 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(12));
            build_printer_dropdown(body);
        }

        m_error = make_text(this, wxEmptyString, m_theme.error);
        body->Add(m_error, 0, wxEXPAND | wxTOP, FromDIP(6));

        m_options_host = new wxBoxSizer(wxVERTICAL);
        body->Add(m_options_host, 0, wxEXPAND);

        m_root->Add(body, 1, wxEXPAND | wxALL, pad);

        // Footer
        auto* footer = new wxBoxSizer(wxHORIZONTAL);
        if (m_backend.needs_sign_in()) {
            m_account = make_text(this, wxEmptyString, m_theme.subtext);
            footer->Add(m_account, 0, wxALIGN_CENTER_VERTICAL);
            m_account_btn = make_text(this, wxEmptyString, m_theme.accent);
            m_account_btn->SetCursor(wxCursor(wxCURSOR_HAND));
            m_account_btn->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { on_account_link(); });
            footer->Add(m_account_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
            update_account();
        }
        footer->AddStretchSpacer();
        auto* cancel = new FlatButton(this, _L("Cancel"), m_theme, false);
        cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
        footer->Add(cancel, 0, wxRIGHT, FromDIP(10));
        if (m_vendor == SendVendor::Creality && m_backend.supports_upload_only()) {
            // Creality Print offers "Send Only" (upload) next to "Send" (upload and print).
            m_send_btn = new FlatButton(this, _L("Send Only"), m_theme, false);
            m_send_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { submit(false); });
            footer->Add(m_send_btn, 0, wxRIGHT, FromDIP(10));
        }
        m_print_btn = new FlatButton(this, m_vendor == SendVendor::Creality ? _L("Send") : _L("Print"), m_theme, true);
        m_print_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { submit(m_upload_and_print ? m_upload_and_print->GetValue() : true); });
        footer->Add(m_print_btn, 0);
        m_root->Add(footer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, pad);
        enable_actions(false);
    }

    void build_printer_dropdown(wxSizer* parent)
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        m_printer_choice = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(300), -1));
        m_printer_choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { select_printer(m_printer_choice->GetSelection()); });
        row->Add(m_printer_choice, 1, wxALIGN_CENTER_VERTICAL);
        auto* refresh = make_text(this, wxString::FromUTF8("\xE2\x9F\xB3"), m_theme.accent, true, 3);
        refresh->SetToolTip(_L("Refresh"));
        refresh->SetCursor(wxCursor(wxCURSOR_HAND));
        refresh->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { load_printers(); });
        row->Add(refresh, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
        m_plate_host = new wxBoxSizer(wxHORIZONTAL);
        row->Add(m_plate_host, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
        parent->Add(row, 0, wxEXPAND);
        m_printer_status = make_text(this, wxEmptyString, m_theme.subtext);
        parent->Add(m_printer_status, 0, wxTOP, FromDIP(4));
    }

    void build_printer_list(wxSizer* parent)
    {
        auto* head = new wxBoxSizer(wxHORIZONTAL);
        head->Add(make_text(this, m_vendor == SendVendor::Markforged ? _L("Printers") : _L("Select printer"), m_theme.text, true, 1), 0,
                  wxALIGN_CENTER_VERTICAL);
        head->AddStretchSpacer();
        auto* refresh = make_text(this, _L("Refresh"), m_theme.accent);
        refresh->SetCursor(wxCursor(wxCURSOR_HAND));
        refresh->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { load_printers(); });
        head->Add(refresh, 0, wxALIGN_CENTER_VERTICAL);
        parent->Add(head, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
        m_printer_list = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(320), FromDIP(180)), wxVSCROLL);
        m_printer_list->SetBackgroundColour(m_theme.bg);
        m_printer_list->SetScrollRate(0, FromDIP(10));
        m_printer_list->SetSizer(new wxBoxSizer(wxVERTICAL));
        parent->Add(m_printer_list, 1, wxEXPAND);
        m_printer_status = make_text(this, wxEmptyString, m_theme.subtext);
        parent->Add(m_printer_status, 0, wxTOP, FromDIP(4));
    }

    // -- account -------------------------------------------------------------------------------

    void update_account()
    {
        if (m_account == nullptr)
            return;
        const bool in = m_backend.signed_in();
        m_account->SetLabel(in ? wxString::Format(_L("Signed in as %s"), m_backend.account_label()) : _L("Not signed in"));
        m_account_btn->SetLabel(in ? _L("Sign out") : _L("Sign in"));
        Layout();
    }

    void on_account_link()
    {
        if (m_backend.signed_in()) {
            m_backend.sign_out();
            update_account();
            if (m_backend.has_local_printers())
                load_printers();
            else
                show_sign_in();
        } else if (m_backend.sign_in(this)) {
            update_account();
            load_printers();
        }
    }

    void show_sign_in()
    {
        m_printers.clear();
        populate_printers();
        set_error(wxEmptyString);
        if (m_printer_status)
            m_printer_status->SetLabel(_L("Sign in to see the printers bound to your account."));
        clear_options();
        auto* btn = new FlatButton(this, _L("Sign in"), m_theme, true);
        btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_account_link(); });
        m_options_host->Add(btn, 0, wxTOP, FromDIP(12));
        enable_actions(false);
        relayout();
    }

    // -- printers ------------------------------------------------------------------------------

    void load_printers()
    {
        if (m_printer_status)
            m_printer_status->SetLabel(_L("Looking for printers…"));
        set_error(wxEmptyString);
        enable_actions(false);
        auto alive   = m_alive;
        auto backend = m_backend_ptr;
        std::thread([this, alive, backend]() {
            std::vector<SendPrinter> list;
            wxString                 error;
            try {
                list = backend->printers();
            } catch (const std::exception& e) {
                error = wxString::FromUTF8(e.what());
            }
            wxGetApp().CallAfter([this, alive, list = std::move(list), error]() {
                if (!*alive)
                    return;
                m_printers = list;
                populate_printers();
                if (!error.empty())
                    set_error(error);
                update_account();
            });
        }).detach();
    }

    void populate_printers()
    {
        const std::string last = recall("printer", "");
        int               sel  = m_printers.empty() ? -1 : 0;
        for (size_t i = 0; i < m_printers.size(); ++i)
            if (m_printers[i].id == last)
                sel = static_cast<int>(i);
        if (m_printer_choice) {
            m_printer_choice->Clear();
            for (const SendPrinter& p : m_printers) {
                wxString label = p.name;
                if (!p.detail.empty())
                    label += "  (" + p.detail + ")";
                if (!p.group.empty())
                    label = p.group + ": " + label;
                m_printer_choice->Append(label);
            }
        }
        if (m_printer_list) {
            m_printer_list->GetSizer()->Clear(true);
            m_cards.clear();
            for (size_t i = 0; i < m_printers.size(); ++i) {
                auto* card = new PrinterCard(m_printer_list, m_theme, m_printers[i]);
                card->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent&) { select_printer(static_cast<int>(i)); });
                m_printer_list->GetSizer()->Add(card, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
                m_cards.push_back(card);
            }
            m_printer_list->FitInside();
        }
        if (m_printer_status)
            m_printer_status->SetLabel(m_printers.empty() ? _L("No printer found.") : wxString());
        select_printer(sel);
    }

    void select_printer(int index)
    {
        m_selected = index;
        if (m_printer_choice && index >= 0)
            m_printer_choice->SetSelection(index);
        for (size_t i = 0; i < m_cards.size(); ++i)
            m_cards[i]->set_selected(static_cast<int>(i) == index);
        clear_options();
        if (index < 0 || index >= static_cast<int>(m_printers.size())) {
            enable_actions(false);
            relayout();
            return;
        }
        const SendPrinter& p = m_printers[index];
        remember("printer", p.id);
        if (m_printer_status && m_printer_choice)
            m_printer_status->SetLabel(p.status);
        set_error(p.busy ? _L("The printer is busy. The file can still be sent without printing.") : wxString());
        build_options(p);
        enable_actions(p.online);
        relayout();
        if (!m_job.filaments.empty() && m_backend.has_filament_mapping())
            load_slots(p);
    }

    // -- options -------------------------------------------------------------------------------

    void clear_options()
    {
        m_option_controls.clear();
        m_slot_pickers.clear();
        m_slot_choice.clear();
        m_slots = SendSlots();
        m_upload_and_print = nullptr;
        m_switch_tab       = nullptr;
        m_auto_refill      = nullptr;
        m_use_slots        = nullptr;
        m_slot_grid_host   = nullptr;
        m_slot_area        = nullptr;
        if (m_plate_host)
            m_plate_host->Clear(true);
        m_options_host->Clear(true);
    }

    void add_toggle_row(wxSizer* grid, const wxString& label, Toggle* toggle, const wxString& tooltip = {})
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        auto* t   = make_text(this, label, m_theme.text);
        if (!tooltip.empty())
            t->SetToolTip(tooltip);
        row->Add(t, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
        row->Add(toggle, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    }

    void build_options(const SendPrinter& printer)
    {
        const std::vector<SendOption> opts = m_backend.options(printer);
        const bool elegoo = m_vendor == SendVendor::Elegoo;

        // ElegooSlicer: "Switch to Device tab" and "Upload and print" right under the printer.
        if (m_backend.supports_upload_only() && m_vendor != SendVendor::Creality) {
            auto* send_grid = new wxFlexGridSizer(elegoo ? 2 : 1, 0, FromDIP(24));
            send_grid->AddGrowableCol(0, 1);
            if (elegoo)
                send_grid->AddGrowableCol(1, 1);
            m_switch_tab = new Toggle(this, m_theme, recall("switch_tab", "0") == "1");
            add_toggle_row(send_grid, _L("Switch to Device tab after upload"), m_switch_tab);
            m_upload_and_print = new Toggle(this, m_theme, recall("upload_and_print", "1") == "1");
            m_upload_and_print->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
                m_print_btn->SetLabel(m_upload_and_print->GetValue() ? _L("Print") : _L("Upload"));
                Layout();
            });
            add_toggle_row(send_grid, _L("Upload and print"), m_upload_and_print);
            if (elegoo) {
                auto* sep = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
                sep->SetBackgroundColour(m_theme.border);
                m_options_host->Add(sep, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(10));
            }
            m_options_host->Add(send_grid, 0, wxEXPAND | wxTOP, elegoo ? 0 : FromDIP(10));
            m_print_btn->SetLabel(m_upload_and_print->GetValue() ? _L("Print") : _L("Upload"));
        }

        // Filament mapping (filled in once the printer's slots arrive).
        if (!m_job.filaments.empty() && m_backend.has_filament_mapping()) {
            if (elegoo)
                m_options_host->Add(section_header(this, m_theme, _L("Filament Mapping")), 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(10));
            else
                m_options_host->Add(make_text(this, _L("Filament Mapping"), m_theme.text, true, 1), 0, wxTOP | wxBOTTOM, FromDIP(10));
            m_slot_area = new wxBoxSizer(wxVERTICAL);
            m_slot_area->Add(make_text(this, _L("Reading the printer's filament slots…"), m_theme.subtext));
            m_options_host->Add(m_slot_area, 0, wxEXPAND);
        }

        // Print options.
        std::vector<const SendOption*> print_opts;
        for (const SendOption& o : opts)
            if (o.section != "send")
                print_opts.push_back(&o);
        if (!print_opts.empty()) {
            const wxString title = m_vendor == SendVendor::Markforged ? _L("Print settings") : _L("Print Options");
            if (elegoo)
                m_options_host->Add(section_header(this, m_theme, title), 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(10));
            else
                m_options_host->Add(make_text(this, title, m_theme.text, true, 1), 0, wxTOP | wxBOTTOM, FromDIP(10));
            auto* grid = new wxFlexGridSizer(elegoo ? 2 : 1, 0, FromDIP(24));
            grid->AddGrowableCol(0, 1);
            if (elegoo)
                grid->AddGrowableCol(1, 1);
            for (const SendOption* o : print_opts) {
                if (o->kind == SendOption::Kind::Toggle) {
                    auto* t = new Toggle(this, m_theme, recall(o->key, o->default_on ? "1" : "0") == "1");
                    add_toggle_row(grid, o->label, t, o->tooltip);
                    m_option_controls[o->key] = t;
                } else {
                    auto* c = new wxChoice(this, wxID_ANY);
                    const std::string cur = recall(o->key, o->default_choice);
                    for (size_t i = 0; i < o->choices.size(); ++i) {
                        c->Append(o->choices[i].second);
                        if (o->choices[i].first == cur)
                            c->SetSelection(static_cast<int>(i));
                    }
                    if (c->GetSelection() == wxNOT_FOUND && !o->choices.empty())
                        c->SetSelection(0);
                    if (!o->tooltip.empty())
                        c->SetToolTip(o->tooltip);
                    m_option_controls[o->key] = c;
                    m_choice_values[o->key]   = o->choices;
                    if (elegoo && o->key == "plate_side" && m_plate_host) {
                        // ElegooSlicer shows the build plate next to the printer.
                        m_plate_host->Add(c, 0, wxALIGN_CENTER_VERTICAL);
                        continue;
                    }
                    auto* row = new wxBoxSizer(wxHORIZONTAL);
                    row->Add(make_text(this, o->label, m_theme.text), 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
                    row->Add(c, 0, wxALIGN_CENTER_VERTICAL);
                    grid->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
                }
            }
            m_options_host->Add(grid, 0, wxEXPAND);
        }
    }

    // -- filament slots ------------------------------------------------------------------------

    void load_slots(const SendPrinter& printer)
    {
        auto      alive   = m_alive;
        auto      backend = m_backend_ptr;
        const int sel     = m_selected;
        std::thread([this, alive, backend, printer, sel]() {
            SendSlots slots;
            try {
                slots = backend->slots(printer);
            } catch (const std::exception& e) {
                slots.available = false;
                slots.message   = wxString::FromUTF8(e.what());
            }
            wxGetApp().CallAfter([this, alive, slots, sel]() {
                if (!*alive || sel != m_selected || m_slot_area == nullptr)
                    return;
                m_slots = slots;
                show_slots();
            });
        }).detach();
    }

    void show_slots()
    {
        m_slot_area->Clear(true);
        m_slot_pickers.clear();
        m_slot_choice.clear();
        if (!m_slots.available) {
            m_slot_area->Add(make_text(this, m_slots.message.empty() ? _L("The printer reported no filament slots. It will use the loaded filament.") :
                                                                       m_slots.message,
                                       m_theme.subtext));
            relayout();
            return;
        }
        if (m_slots.supports_auto_refill) {
            auto* row     = new wxFlexGridSizer(1, 0, 0);
            row->AddGrowableCol(0, 1);
            m_auto_refill = new Toggle(this, m_theme, m_slots.auto_refill);
            add_toggle_row(row, _L("Auto refill"), m_auto_refill, _L("Switch to a matching spool when one runs out."));
            m_slot_area->Add(row, 0, wxEXPAND);
        }
        if (!m_slots.enable_label.empty()) {
            // Creality Print: "Enable CFS"; off prints from the external spool.
            auto* row  = new wxFlexGridSizer(1, 0, 0);
            row->AddGrowableCol(0, 1);
            m_use_slots = new Toggle(this, m_theme, recall("use_slots", "1") == "1");
            add_toggle_row(row, m_slots.enable_label, m_use_slots);
            m_slot_area->Add(row, 0, wxEXPAND);
            m_use_slots->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
                m_slot_grid_host->ShowItems(m_use_slots->GetValue());
                relayout();
            });
        }
        auto* grid = new wxWrapSizer(wxHORIZONTAL);
        m_slot_grid_host = grid;
        for (const FilamentInfo& f : m_job.filaments) {
            auto* col = new wxBoxSizer(wxVERTICAL);
            col->Add(new Swatch(this, m_theme, parse_colour(f.color), wxString::FromUTF8(f.type), format_weight(f.used_g), false), 0, wxEXPAND);
            col->Add(make_text(this, wxString::FromUTF8("\xE2\x96\xBC"), m_theme.subtext), 0, wxALIGN_CENTER_HORIZONTAL);
            auto* picker = new Swatch(this, m_theme, *wxWHITE, "?", wxEmptyString, true);
            picker->SetToolTip(_L("Choose the slot this filament prints from"));
            const int fid = f.id;
            picker->Bind(wxEVT_BUTTON, [this, fid, picker](wxCommandEvent&) { pick_slot(fid, picker); });
            col->Add(picker, 0, wxEXPAND);
            grid->Add(col, 0, wxRIGHT | wxBOTTOM, FromDIP(10));
            m_slot_pickers[fid] = picker;
            auto_match(f);
        }
        m_slot_area->Add(grid, 0, wxEXPAND);
        if (m_use_slots)
            grid->ShowItems(m_use_slots->GetValue());
        relayout();
    }

    // Picks the loaded slot with the same type and the nearest colour, like the vendor apps do on open.
    void auto_match(const FilamentInfo& f)
    {
        const wxColour want = parse_colour(f.color);
        int            best = -1;
        double         best_d = 1e18;
        for (size_t i = 0; i < m_slots.slots.size(); ++i) {
            const SendSlot& s = m_slots.slots[i];
            if (!s.loaded)
                continue;
            double d = std::pow(s.color.Red() - want.Red(), 2) + std::pow(s.color.Green() - want.Green(), 2) +
                       std::pow(s.color.Blue() - want.Blue(), 2);
            if (wxString::FromUTF8(s.type).CmpNoCase(wxString::FromUTF8(f.type)) != 0)
                d += 1e9;
            if (d < best_d) {
                best_d = d;
                best   = static_cast<int>(i);
            }
        }
        set_slot(f.id, best);
    }

    void set_slot(int filament_id, int slot_index)
    {
        Swatch* picker = m_slot_pickers[filament_id];
        if (slot_index < 0 || slot_index >= static_cast<int>(m_slots.slots.size())) {
            m_slot_choice.erase(filament_id);
            picker->set(*wxWHITE, "?", wxEmptyString, true);
            return;
        }
        const SendSlot& s          = m_slots.slots[slot_index];
        m_slot_choice[filament_id] = s.id;
        picker->set(s.color, s.label, wxString::FromUTF8(s.type), false);
        picker->SetToolTip(s.name.empty() ? wxString::FromUTF8(s.type) : s.name);
    }

    void pick_slot(int filament_id, wxWindow* anchor)
    {
        wxMenu menu;
        menu.Append(1000, _L("Not mapped (printer chooses)"));
        for (size_t i = 0; i < m_slots.slots.size(); ++i) {
            const SendSlot& s = m_slots.slots[i];
            wxString        label = s.label + "  " + (s.loaded ? wxString::FromUTF8(s.type) : _L("Empty"));
            if (!s.name.empty())
                label += "  " + s.name;
            wxMenuItem* item = menu.Append(1001 + static_cast<int>(i), label);
            wxBitmap    bmp(FromDIP(14), FromDIP(14));
            {
                wxMemoryDC dc(bmp);
                dc.SetBackground(wxBrush(s.color));
                dc.Clear();
            }
            item->SetBitmap(bmp);
            item->Enable(s.loaded);
        }
        const int id = anchor->GetPopupMenuSelectionFromUser(menu, wxPoint(0, anchor->GetSize().y));
        if (id == 1000)
            set_slot(filament_id, -1);
        else if (id > 1000)
            set_slot(filament_id, id - 1001);
    }

    // -- submit --------------------------------------------------------------------------------

    void submit(bool start_print)
    {
        if (m_selected < 0 || m_selected >= static_cast<int>(m_printers.size()))
            return;
        SendRequest req;
        req.printer_id  = m_printers[m_selected].id;
        req.start_print = start_print;
        wxString name   = m_name->GetValue().Trim().Trim(false);
        if (name.empty())
            name = m_job.name;
        req.file_name = name + wxString::FromUTF8(m_extension);
        for (const auto& [key, ctrl] : m_option_controls) {
            if (auto* t = dynamic_cast<Toggle*>(ctrl)) {
                req.options[key] = t->GetValue() ? "1" : "0";
            } else if (auto* c = dynamic_cast<wxChoice*>(ctrl)) {
                const auto& values = m_choice_values[key];
                const int   s      = c->GetSelection();
                if (s >= 0 && s < static_cast<int>(values.size()))
                    req.options[key] = values[s].first;
            }
            remember(key, req.options[key]);
        }
        if (m_auto_refill)
            req.options["auto_refill"] = m_auto_refill->GetValue() ? "1" : "0";
        if (m_use_slots) {
            req.options["use_slots"] = m_use_slots->GetValue() ? "1" : "0";
            remember("use_slots", req.options["use_slots"]);
        }
        if (m_switch_tab) {
            req.options["switch_to_device_tab"] = m_switch_tab->GetValue() ? "1" : "0";
            remember("switch_tab", req.options["switch_to_device_tab"]);
        }
        if (m_upload_and_print)
            remember("upload_and_print", m_upload_and_print->GetValue() ? "1" : "0");
        if (!m_use_slots || m_use_slots->GetValue())
            req.slot_map = m_slot_choice;

        wxString     error;
        PrintHostJob job = m_backend.make_job(req, m_default_output, error);
        if (!error.empty()) {
            set_error(error);
            return;
        }
        if (!job.empty()) {
            job.switch_to_device_tab = m_switch_tab && m_switch_tab->GetValue();
            m_result                 = std::move(job);
            EndModal(wxID_OK);
        } else {
            EndModal(wxID_CANCEL); // handled elsewhere (e.g. the LAN host dialog)
        }
    }

    // -- helpers -------------------------------------------------------------------------------

    void enable_actions(bool enable)
    {
        if (m_print_btn)
            m_print_btn->Enable(enable);
        if (m_send_btn)
            m_send_btn->Enable(enable);
    }
    void set_error(const wxString& msg)
    {
        m_error->SetLabel(msg);
        m_error->Wrap(FromDIP(420));
        m_error->Show(!msg.empty());
        relayout();
    }
    void relayout()
    {
        Layout();
        GetSizer()->SetSizeHints(this);
        Fit();
        Refresh();
    }

    SendVendor                         m_vendor;
    std::shared_ptr<VendorSendBackend> m_backend_ptr;
    VendorSendBackend&                 m_backend;
    SendJobSummary        m_job;
    fs::path              m_default_output;
    std::string           m_extension;
    Theme                 m_theme;
    std::shared_ptr<std::atomic<bool>> m_alive{std::make_shared<std::atomic<bool>>(true)};

    wxBoxSizer*           m_root{nullptr};
    wxBoxSizer*           m_options_host{nullptr};
    wxBoxSizer*           m_plate_host{nullptr};
    wxBoxSizer*           m_slot_area{nullptr};
    wxTextCtrl*           m_name{nullptr};
    wxChoice*             m_printer_choice{nullptr};
    wxScrolledWindow*     m_printer_list{nullptr};
    std::vector<PrinterCard*> m_cards;
    wxStaticText*         m_printer_status{nullptr};
    wxStaticText*         m_error{nullptr};
    wxStaticText*         m_account{nullptr};
    wxStaticText*         m_account_btn{nullptr};
    FlatButton*           m_print_btn{nullptr};
    FlatButton*           m_send_btn{nullptr};
    Toggle*               m_upload_and_print{nullptr};
    Toggle*               m_switch_tab{nullptr};
    Toggle*               m_auto_refill{nullptr};
    Toggle*               m_use_slots{nullptr};
    wxSizer*              m_slot_grid_host{nullptr};

    std::vector<SendPrinter>                                        m_printers;
    int                                                             m_selected{-1};
    std::map<std::string, wxWindow*>                                m_option_controls;
    std::map<std::string, std::vector<std::pair<std::string, wxString>>> m_choice_values;
    SendSlots                                                       m_slots;
    std::map<int, Swatch*>                                          m_slot_pickers;
    std::map<int, std::string>                                      m_slot_choice;
    PrintHostJob                                                    m_result;
};

std::optional<PrintHostJob> show_vendor_send_dialog(wxWindow* parent, SendVendor vendor, std::shared_ptr<VendorSendBackend> backend,
                                                    const SendJobSummary& job, const fs::path& default_output)
{
    VendorSendDialog dlg(parent, vendor, std::move(backend), job, default_output);
    if (dlg.ShowModal() != wxID_OK)
        return std::nullopt;
    return dlg.take_job();
}

}} // namespace Slic3r::GUI
