#include "slic3r/GUI/IMEXFilamentPickerPopover.hpp"

#include <wx/stattext.h>
#include <wx/dcmemory.h>
#include <wx/image.h>

#include "libslic3r/IMEXHelpers.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/BitmapComboBox.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/wxExtensions.hpp"

namespace Slic3r { namespace GUI {

IMEXFilamentPickerPopover::IMEXFilamentPickerPopover(
    wxWindow* parent, PartPlate* plate,
    const ConfigOptionInts& pem, int physical_head,
    CommitCallback on_commit)
    : wxPopupTransientWindow(parent, wxBORDER_SIMPLE)
    , m_plate(plate)
    , m_pem(pem)
    , m_physical_head(physical_head)
    , m_on_commit(std::move(on_commit))
{
    m_root_sizer = new wxBoxSizer(wxHORIZONTAL);
    SetSizer(m_root_sizer);

    build_row();

    m_root_sizer->SetSizeHints(this);
    SetClientSize(m_root_sizer->ComputeFittingClientSize(this));
    Layout();
}

void IMEXFilamentPickerPopover::popup_at_cursor()
{
    Position(wxGetMousePosition(), wxSize(0, 0));
    Popup();
}

void IMEXFilamentPickerPopover::build_row()
{
    m_root_sizer->Clear(true);

    m_root_sizer->AddSpacer(10);

    auto* label = new wxStaticText(this, wxID_ANY,
            wxString::Format("T%d", m_physical_head));
    m_root_sizer->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, 6);

    m_root_sizer->AddSpacer(10);

    std::vector<int> lane_logicals;
    for (size_t L = 0; L < m_pem.values.size(); ++L)
        if (m_pem.values[L] == m_physical_head)
            lane_logicals.push_back((int)L);

    std::vector<wxBitmap*> bmps = get_extruder_color_icons(false /*thin_icon*/);

    auto* choice = new BitmapComboBox(this, wxID_ANY, wxEmptyString,
                                      wxDefaultPosition, wxSize(140, -1),
                                      0, nullptr, wxCB_READONLY);
    const int left_pad_px = 8;
    auto pad_left = [&](const wxBitmap& src) -> wxBitmap {
        if (!src.IsOk()) return src;
        wxImage im(src.GetWidth() + left_pad_px, src.GetHeight());
        im.InitAlpha();
        unsigned char* a = im.GetAlpha();
        std::fill(a, a + im.GetWidth() * im.GetHeight(), (unsigned char)0);
        wxBitmap out(im);
        wxMemoryDC dc(out);
        dc.DrawBitmap(src, left_pad_px, 0, true);
        dc.SelectObject(wxNullBitmap);
        return out;
    };

    for (int L : lane_logicals) {
        const wxString label = wxString::Format("filament %d", L + 1);
        if (L >= 0 && L < (int)bmps.size() && bmps[L] && bmps[L]->IsOk())
            choice->Append(label, pad_left(*bmps[L]));
        else
            choice->Append(label, wxNullBitmap);
    }

    const auto map = m_plate->get_imex_head_filament_map();
    auto it = map.find(m_physical_head);
    int selected_slot_1based = (it != map.end())
        ? it->second
        : (lane_logicals.empty() ? -1 : lane_logicals.front() + 1);

    for (int i = 0; i < (int)lane_logicals.size(); ++i) {
        if (lane_logicals[i] + 1 == selected_slot_1based) {
            choice->SetSelection(i);
            break;
        }
    }

    choice->Bind(wxEVT_COMBOBOX, [this, lane_logicals, choice](wxCommandEvent&) {
        int sel = choice->GetSelection();
        if (sel < 0 || sel >= (int)lane_logicals.size()) return;
        on_filament_selected(lane_logicals[sel] + 1);
        Dismiss();
    });

    m_root_sizer->Add(choice, 0, wxALIGN_CENTER_VERTICAL | wxALL, 6);
    m_root_sizer->AddSpacer(4);
}

void IMEXFilamentPickerPopover::on_filament_selected(int slot_1_based)
{
    auto map = m_plate->get_imex_head_filament_map();
    // Erase when the user reselects the pem default for this head, so the 3mf
    // doesn't carry no-op overrides (same invariant as PR3b).
    const int default_logical = first_filament_for_physical_head(m_pem, m_physical_head);
    if (default_logical >= 0 && slot_1_based == default_logical + 1)
        map.erase(m_physical_head);
    else
        map[m_physical_head] = slot_1_based;
    m_plate->set_imex_head_filament_map(map);
    if (m_on_commit) m_on_commit();
}

}} // namespace Slic3r::GUI
