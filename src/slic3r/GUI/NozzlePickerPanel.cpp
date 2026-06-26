#include "NozzlePickerPanel.hpp"

#include <iomanip>
#include <sstream>

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/statbox.h>

#include "Widgets/ComboBox.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/StaticBox.hpp"
#include "Widgets/StaticLine.hpp"
#include "Widgets/Label.hpp"
#include "wxExtensions.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"

namespace Slic3r { namespace GUI {

// Two-column grid, matching the Filament section. Nozzle i goes into column (i % NOZZLE_COLUMNS) so
// they read row-major (1 2 / 3 4); this fits the narrow printer sidebar and scales to more nozzles.
// The panel lives inside the sidebar's scrolled window, so extra rows scroll with it -- no wrapper here.
static const int NOZZLE_COLUMNS = 2;

// Spacing literals copied from SidebarProps (Plater.hpp) so the nozzle rows + title match the Filament
// section without pulling in the heavy Plater.hpp: TitlebarMargin()=8 (title icon inset),
// ElementSpacing()=5 (related controls, e.g. icon<->label and label<->combo).
static const int kTitlebarMargin = 8;
static const int kElementSpacing = 5;

// "0.4" / "0.4mm" -> 0.4 . Strict: after an optional trailing "mm" unit, the whole string must be a
// single number. Composite/combined variants like "0.4+0.6" are rejected (return 0), not silently
// truncated to their leading value. The "mm" suffix is accepted because the dropdowns display it.
static double parse_diameter(const wxString& value)
{
    try {
        std::string s = value.ToStdString();
        if (s.size() >= 2 && s.compare(s.size() - 2, 2, "mm") == 0)
            s.erase(s.size() - 2);
        size_t consumed = 0;
        double d = std::stod(s, &consumed);
        return consumed == s.size() ? d : 0.0;
    } catch (...) {
        return 0.0;
    }
}

// Format a diameter as a trimmed "0.6mm" string for display in a dropdown.
static wxString format_diameter(double mm)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2) << mm;
    std::string s = stream.str();
    if (s.find('.') != std::string::npos) {
        s.erase(s.find_last_not_of('0') + 1);
        if (s.back() == '.') s += '0';
    }
    return wxString(s) + "mm";
}

// A printer_variant is offered as a per-extruder nozzle size only if it is a single diameter.
// A model can ship a *combined* variant such as "0.4+0.6" (the old single-preset way to describe a
// mixed-nozzle machine, which this picker replaces); one toolhead is "0.4" or "0.6", never "0.4+0.6",
// so combined variants are filtered out here. parse_diameter rejects them by returning 0.
static bool is_single_diameter(const std::string& variant)
{
    return parse_diameter(wxString(variant)) > 0.0;
}

NozzlePickerPanel::NozzlePickerPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    // Inherit the surrounding menu's background (the printer-content panel we're parented to) rather
    // than hardcoding a colour, so the area behind the nozzle rows is the same shade as the rest of the
    // sidebar by construction -- in any theme. Without this the panel kept wx's default grey and read as
    // a lighter block. The build site also runs UpdateDarkUI on this panel (as the sibling panels get).
    if (parent)
        SetBackgroundColour(parent->GetBackgroundColour());

    auto* root = new wxBoxSizer(wxVERTICAL);

    // Title bar matching the Filament section's `m_panel_filament_title` (Plater.cpp): a gradient
    // StaticBox holding a ScalableButton icon + a Label, bracketed top and bottom by thin StaticLine
    // separators. Icon = "single_nozzle_n" (the nozzle glyph) where filament uses "filament".
    auto* title = new StaticBox(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE);
    title->SetBackgroundColor(wxColour(248, 248, 248));
    title->SetBackgroundColor2(0xF1F1F1);

    auto* title_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto* icon  = new ScalableButton(title, wxID_ANY, "single_nozzle_n");
    auto* label = new Label(title, _L("Nozzles"));
    title_sizer->Add(icon,  0, wxALIGN_CENTER | wxLEFT, FromDIP(kTitlebarMargin));
    title_sizer->Add(label, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP(kElementSpacing));
    title_sizer->SetMinSize(-1, FromDIP(30));
    title->SetSizer(title_sizer);

    auto* sep_top = new StaticLine(this);
    sep_top->SetLineColour("#A6A9AA");
    root->Add(sep_top, 0, wxEXPAND);
    root->Add(title, 0, wxEXPAND);
    auto* sep_bottom = new StaticLine(this);
    sep_bottom->SetLineColour("#A6A9AA");
    root->Add(sep_bottom, 0, wxEXPAND);

    // Rows sit flush on the panel -- no inner frame -- like the Filament section. The sizer holds
    // NOZZLE_COLUMNS vertical column sizers side by side, built in rebuild().
    m_grid_sizer = new wxBoxSizer(wxHORIZONTAL);
    root->Add(m_grid_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(kElementSpacing));

    auto* apply_btn = new Button(this, _L("Apply"));
    apply_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_apply_clicked(); });
    root->Add(apply_btn, 0, wxALIGN_RIGHT | wxTOP | wxRIGHT, FromDIP(4));

    SetSizer(root);
}

void NozzlePickerPanel::rebuild(const std::vector<double>& current, const std::vector<std::string>& allowed)
{
    // Destroy the previous columns/rows and recreate to the new N. Clearing the sizer with
    // delete_windows=true destroys the child combos/labels, so stale pointers are not reused.
    m_selectors.clear();
    m_grid_sizer->Clear(true);

    // Keep only single-diameter entries from the variant list (see is_single_diameter).
    m_allowed.clear();
    for (const std::string& d : allowed)
        if (is_single_diameter(d))
            m_allowed.push_back(d);

    // NOZZLE_COLUMNS vertical column sizers side by side, each an equal share (proportion 1).
    std::vector<wxBoxSizer*> columns;
    for (int c = 0; c < NOZZLE_COLUMNS; ++c) {
        auto* col = new wxBoxSizer(wxVERTICAL);
        m_grid_sizer->Add(col, 1, wxEXPAND);
        columns.push_back(col);
    }

    for (size_t i = 0; i < current.size(); ++i) {
        // One compact inline row, same shape as a filament row (label where filament puts its colour
        // badge, then a stretchy combo): "Nozzle N"  [ 0.4mm v ]. No stacking, no dividers -- that is
        // what kept the panel as short as the Filament section below it.
        auto* row = new wxBoxSizer(wxHORIZONTAL);

        auto* label = new wxStaticText(this, wxID_ANY,
            wxString::Format(_L("Nozzle %d"), static_cast<int>(i + 1)));
        row->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(kElementSpacing));

        // Orca's read-only ComboBox hides its inner text control, so it needs a real size; the filament
        // combo pins its height to 30 * em / 10 and lets the sizer stretch the width (proportion 1).
        auto* combo = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                   wxDefaultSize, 0, nullptr, wxCB_READONLY);
        for (const std::string& d : m_allowed)
            combo->Append(format_diameter(parse_diameter(wxString(d))));

        // Initialise to the current per-nozzle diameter (independent; no sync-all).
        combo->SetValue(format_diameter(current[i]));

        m_selectors.push_back(combo);
        row->Add(combo, 1, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(2))
            ->SetMinSize(wxSize(-1, 30 * wxGetApp().em_unit() / 10));

        // Add the row flush (no border), like the Filament section. The only inter-row gap is the
        // combo's own FromDIP(2) top/bottom, so the rows pack as tightly as the filament list.
        columns[i % NOZZLE_COLUMNS]->Add(row, 0, wxEXPAND);
    }

    // Report the new best height to the parent sizer. rebuild() can change the row count (e.g. a
    // 4-nozzle U1 vs an 8-nozzle machine = 2 vs 4 rows), and Layout() alone only arranges children
    // inside the existing panel rect -- it does not tell the parent sizer the panel now needs a
    // different height, so without this the panel keeps its stale size and the extra rows are clipped.
    GetSizer()->Fit(this);
    SetMinSize(wxSize(-1, GetSizer()->GetMinSize().y));
    Layout();
}

std::vector<double> NozzlePickerPanel::pending_diameters() const
{
    std::vector<double> out;
    out.reserve(m_selectors.size());
    for (const ComboBox* combo : m_selectors)
        out.push_back(parse_diameter(combo->GetValue()));
    return out;
}

void NozzlePickerPanel::on_apply_clicked()
{
    if (m_selectors.empty() || !m_on_apply)
        return;
    m_on_apply(pending_diameters());
}

}} // namespace Slic3r::GUI
