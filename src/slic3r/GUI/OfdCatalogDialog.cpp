#include "OfdCatalogDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Widgets/Label.hpp"
#include "wxExtensions.hpp"
#include "../Utils/Http.hpp"

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/dcclient.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/scrolwin.h>

#include <algorithm>
#include <cstddef>
#include <set>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

namespace Slic3r {
namespace GUI {

namespace {

constexpr int k_list_cap = 300;

wxColour hex_to_wx(const std::string &hex)
{
    wxColour c(wxString::FromUTF8(hex.c_str()));
    return c.IsOk() ? c : *wxWHITE;
}

class OfdSwatchPanel : public wxPanel
{
public:
    OfdSwatchPanel(wxWindow *parent, const std::vector<std::string> &hexes, const wxSize &size)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, size)
    {
        for (const std::string &h : hexes)
            m_colors.push_back(hex_to_wx(h));
        if (m_colors.empty())
            m_colors.push_back(*wxWHITE);
        Bind(wxEVT_PAINT, &OfdSwatchPanel::on_paint, this);
    }

private:
    void on_paint(wxPaintEvent &)
    {
        wxPaintDC dc(this);
        const wxSize sz = GetClientSize();
        dc.SetPen(*wxTRANSPARENT_PEN);
        if (m_colors.size() == 1) {
            dc.SetBrush(wxBrush(m_colors[0]));
            dc.DrawRectangle(0, 0, sz.x, sz.y);
            return;
        }
        const int n = int(m_colors.size());
        int       x = 0;
        for (int i = 0; i < n; ++i) {
            const int x1 = (i + 1) * sz.x / n;
            dc.SetBrush(wxBrush(m_colors[size_t(i)]));
            dc.DrawRectangle(x, 0, x1 - x, sz.y);
            x = x1;
        }
    }

    std::vector<wxColour> m_colors;
};

std::string variant_label(const SpectrumOfdVariant &v)
{
    std::string s = v.brand;
    if (!v.filament.empty()) {
        if (!s.empty())
            s += " / ";
        s += v.filament;
    }
    if (!v.variant.empty()) {
        if (!s.empty())
            s += " / ";
        s += v.variant;
    }
    return s;
}

bool write_ndjson_file(const std::string &path, const std::string &body)
{
    try {
        boost::filesystem::path p(path);
        boost::system::error_code ec;
        boost::filesystem::create_directories(p.parent_path(), ec);
        if (ec)
            return false;
        boost::nowide::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs)
            return false;
        ofs << body;
        return ofs.good();
    } catch (...) {
        return false;
    }
}

} // namespace

OfdCatalogDialog::OfdCatalogDialog(wxWindow *parent, int filament_idx)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe),
                wxID_ANY,
                _L("Apply to slot 1"),
                wxDefaultPosition,
                wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_filament_idx(std::max(0, filament_idx))
    , m_refresh_alive(std::make_shared<bool>(true))
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);

    auto *root = new wxBoxSizer(wxVERTICAL);

    size_t n_slots = 1;
    if (PresetBundle *bundle = wxGetApp().preset_bundle)
        n_slots = std::max<size_t>(1, bundle->filament_presets.size());
    if (size_t(m_filament_idx) >= n_slots)
        m_filament_idx = 0;

    auto *filters = new wxBoxSizer(wxHORIZONTAL);
    if (n_slots > 1) {
        filters->Add(new wxStaticText(this, wxID_ANY, _L("Slot")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        m_slot = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(80), -1));
        for (size_t i = 0; i < n_slots; ++i)
            m_slot->Append(wxString::Format("%d", int(i + 1)));
        m_slot->SetSelection(m_filament_idx);
        m_slot->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
            m_filament_idx = std::max(0, m_slot->GetSelection());
            update_title();
        });
        filters->Add(m_slot, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    }

    filters->Add(new wxStaticText(this, wxID_ANY, _L("Brand")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    m_brand = new wxComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(160), -1), 0, nullptr,
                             wxCB_READONLY);
    filters->Add(m_brand, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    filters->Add(new wxStaticText(this, wxID_ANY, _L("Name")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    m_name = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(180), -1));
    filters->Add(m_name, 1, wxALIGN_CENTER_VERTICAL);
    root->Add(filters, 0, wxEXPAND | wxALL, FromDIP(12));

    m_list = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_list->SetBackgroundColour(*wxWHITE);
    m_list->SetScrollRate(0, FromDIP(16));
    m_list->SetMinSize(wxSize(FromDIP(520), FromDIP(280)));
    m_list->SetSizer(new wxBoxSizer(wxVERTICAL));
    root->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

    m_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
    root->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    auto *btns = new wxBoxSizer(wxHORIZONTAL);
    m_btn_refresh = new wxButton(this, wxID_ANY, _L("Refresh catalog"));
    m_btn_refresh->SetToolTip(_L("Download the latest Open Filament Database in the background. Search still works from the seed."));
    m_btn_refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_refresh(); });
    btns->Add(m_btn_refresh, 0, wxALIGN_CENTER_VERTICAL);
    btns->AddStretchSpacer(1);
    auto *cancel = new wxButton(this, wxID_CANCEL, _L("Cancel"));
    m_btn_apply  = new wxButton(this, wxID_ANY, _L("Apply"));
    m_btn_apply->SetDefault();
    m_btn_apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_apply(); });
    btns->Add(cancel, 0, wxRIGHT, FromDIP(8));
    btns->Add(m_btn_apply, 0);
    root->Add(btns, 0, wxEXPAND | wxALL, FromDIP(12));

    m_brand->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &) { refresh_list(); });
    m_name->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { refresh_list(); });

    load_catalog();
    rebuild_brand_filter();
    refresh_list();
    update_title();

    SetSizerAndFit(root);
    SetMinSize(wxSize(FromDIP(560), FromDIP(420)));
    CentreOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

OfdCatalogDialog::~OfdCatalogDialog()
{
    if (m_refresh_alive)
        *m_refresh_alive = false;
}

void OfdCatalogDialog::on_dpi_changed(const wxRect &)
{
    Layout();
    Fit();
}

void OfdCatalogDialog::update_title()
{
    SetTitle(wxString::Format(_L("Apply to slot %d"), m_filament_idx + 1));
}

std::string OfdCatalogDialog::seed_path() const
{
    return (boost::filesystem::path(resources_dir()) / "spectrum" / "ofd_seed.json").string();
}

std::string OfdCatalogDialog::user_ndjson_path() const
{
    const std::string &root = data_dir();
    if (root.empty())
        return {};
    return (boost::filesystem::path(root) / "spectrum" / "ofd_all.ndjson").string();
}

void OfdCatalogDialog::load_catalog()
{
    m_catalog = spectrum_ofd_load_catalog(seed_path(), user_ndjson_path());
}

void OfdCatalogDialog::rebuild_brand_filter()
{
    std::string prev;
    if (m_brand->GetSelection() > 0)
        prev = m_brand->GetStringSelection().ToUTF8().data();
    m_brand->Clear();
    m_brand->Append(_L("All"));
    std::set<std::string> brands;
    for (const SpectrumOfdVariant &v : m_catalog) {
        if (!v.brand.empty())
            brands.insert(v.brand);
    }
    int sel = 0;
    int i   = 1;
    for (const std::string &b : brands) {
        m_brand->Append(wxString::FromUTF8(b.c_str()));
        if (b == prev)
            sel = i;
        ++i;
    }
    m_brand->SetSelection(sel);
}

void OfdCatalogDialog::refresh_list()
{
    std::string brand_filter;
    if (m_brand && m_brand->GetSelection() > 0)
        brand_filter = m_brand->GetStringSelection().ToUTF8().data();
    std::string needle;
    if (m_name)
        needle = m_name->GetValue().ToUTF8().data();

    const auto matches = spectrum_ofd_lookup(m_catalog, brand_filter, needle);
    m_shown.clear();
    const size_t nshow = std::min(matches.size(), size_t(k_list_cap));
    m_shown.assign(matches.begin(), matches.begin() + static_cast<std::ptrdiff_t>(nshow));
    m_sel = m_shown.empty() ? -1 : 0;

    wxSizer *sizer = m_list->GetSizer();
    sizer->Clear(true);

    const wxColour sel_bg(232, 240, 254);
    const wxColour uns_bg(255, 255, 255);
    const int      row_h = FromDIP(28);

    for (size_t i = 0; i < m_shown.size(); ++i) {
        const SpectrumOfdVariant &v = m_shown[i];
        auto *row = new wxPanel(m_list, wxID_ANY);
        row->SetBackgroundColour(int(i) == m_sel ? sel_bg : uns_bg);
        auto *hs = new wxBoxSizer(wxHORIZONTAL);
        auto *swatch = new OfdSwatchPanel(row, v.color_hexes, wxSize(FromDIP(28), FromDIP(16)));
        auto *label  = new Label(row, wxString::FromUTF8(variant_label(v).c_str()));
        hs->Add(swatch, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(8));
        hs->Add(label, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        if (v.color_hexes.size() > 1) {
            auto *badge = new Label(row, _L("Dual-color"));
            hs->Add(badge, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        }
        row->SetSizer(hs);
        row->SetMinSize(wxSize(-1, row_h));

        auto bind_sel = [this, i](wxWindow *w) {
            w->Bind(wxEVT_LEFT_DOWN, [this, i](wxMouseEvent &evt) {
                select_row(int(i));
                evt.Skip();
            });
            w->Bind(wxEVT_LEFT_DCLICK, [this, i](wxMouseEvent &) {
                select_row(int(i));
                on_apply();
            });
        };
        bind_sel(row);
        bind_sel(swatch);
        bind_sel(label);
        sizer->Add(row, 0, wxEXPAND);
    }

    if (matches.size() > m_shown.size()) {
        m_status->SetLabel(wxString::Format(_L("Showing %d of %d. Type a name to narrow."),
                                            int(m_shown.size()), int(matches.size())));
    } else if (m_shown.empty()) {
        m_status->SetLabel(_L("No matching filaments in the catalog."));
    } else {
        m_status->SetLabel(wxString::Format(_L("%d filaments"), int(m_shown.size())));
    }

    m_list->FitInside();
    m_list->Layout();
    Layout();
}

void OfdCatalogDialog::select_row(int idx)
{
    m_sel = idx;
    const wxColour sel_bg(232, 240, 254);
    const wxColour uns_bg(255, 255, 255);
    wxSizer *      sizer = m_list->GetSizer();
    const size_t   n     = sizer->GetItemCount();
    for (size_t j = 0; j < n; ++j) {
        wxSizerItem *item = sizer->GetItem(j);
        if (item == nullptr)
            continue;
        if (wxWindow *row_w = item->GetWindow()) {
            row_w->SetBackgroundColour(int(j) == m_sel ? sel_bg : uns_bg);
            row_w->Refresh();
        }
    }
}

void OfdCatalogDialog::on_apply()
{
    if (m_sel < 0 || size_t(m_sel) >= m_shown.size())
        return;
    m_selected = m_shown[size_t(m_sel)];
    EndModal(wxID_OK);
}

void OfdCatalogDialog::on_refresh()
{
    if (!m_btn_refresh->IsEnabled())
        return;
    m_btn_refresh->Enable(false);
    m_status->SetLabel(_L("Updating…"));

    const std::string path  = user_ndjson_path();
    auto              alive = m_refresh_alive;
    Http::get("https://api.openfilamentdatabase.org/json/all.ndjson")
        .size_limit(80ull * 1024ull * 1024ull)
        .timeout_connect(15)
        .timeout_max(180)
        .on_complete([this, alive, path](std::string body, unsigned status) {
            const bool ok_http = status == 200 && !body.empty();
            const bool ok_write = ok_http && write_ndjson_file(path, body);
            wxGetApp().CallAfter([this, alive, ok_write]() {
                if (!alive || !*alive)
                    return;
                if (ok_write) {
                    load_catalog();
                    rebuild_brand_filter();
                    refresh_list();
                    m_status->SetLabel(_L("Catalog updated."));
                } else {
                    m_status->SetLabel(_L("Update failed; using seed catalog."));
                }
                m_btn_refresh->Enable(true);
            });
        })
        .on_error([this, alive](std::string, std::string, unsigned) {
            wxGetApp().CallAfter([this, alive]() {
                if (!alive || !*alive)
                    return;
                m_status->SetLabel(_L("Update failed; using seed catalog."));
                m_btn_refresh->Enable(true);
            });
        })
        .perform();
}

OfdColorChooserDialog::OfdColorChooserDialog(wxWindow *parent)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe),
                wxID_ANY,
                _L("Filament color"),
                wxDefaultPosition,
                wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);

    auto *root = new wxBoxSizer(wxVERTICAL);
    auto *hint = new wxStaticText(this, wxID_ANY,
                                  _L("Search the filament catalog for a spool color, or pick a custom color."));
    hint->Wrap(FromDIP(360));
    root->Add(hint, 0, wxEXPAND | wxALL, FromDIP(12));

    auto *catalog = new wxButton(this, ID_CATALOG, _L("Search filament catalog…"));
    catalog->SetDefault();
    auto *custom = new wxButton(this, ID_CUSTOM, _L("Custom color…"));
    auto *cancel = new wxButton(this, wxID_CANCEL, _L("Cancel"));
    catalog->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(ID_CATALOG); });
    custom->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(ID_CUSTOM); });

    auto *btns = new wxBoxSizer(wxVERTICAL);
    btns->Add(catalog, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    btns->Add(custom, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    btns->Add(cancel, 0, wxEXPAND);
    root->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

    SetSizerAndFit(root);
    CentreOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void OfdColorChooserDialog::on_dpi_changed(const wxRect &)
{
    Layout();
    Fit();
}

} // namespace GUI
} // namespace Slic3r
