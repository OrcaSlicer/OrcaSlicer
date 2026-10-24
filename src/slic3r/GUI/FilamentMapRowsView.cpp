#include "FilamentMapRowsView.hpp"

#include "3DScene.hpp"       // adjust_color_for_rendering
#include "ActivePrinterSession.hpp"
#include "FilamentInventoryStore.hpp"
#include "FilamentMapPanel.hpp"   // wxEVT_INVALID_MANUAL_MAP
#include "FilamentMapRowsPanel.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
#include "SelectMachine.hpp" // ThumbnailPanel
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"

#include <cstddef>
#include <vector>
#include <string>
#include <libslic3r/FilamentInventory.hpp>
#include <functional>
#include <wx/event.h>
#include <utility>
#include <libslic3r/Config.hpp>
#include <slic3r/GUI/PartPlate.hpp>
#include <wx/gdicmn.h>
#include <wx/colour.h>
#include <libslic3r/Color.hpp>
#include <wx/dialog.h>
#include <optional>
#include <wx/image.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>

#include <algorithm>
#include <unordered_map>
#include <wx/window.h>

namespace Slic3r { namespace GUI {

// Rows shown before the rows panel is wrapped in a scrolled window.
static constexpr size_t MAX_VISIBLE_MAP_ROWS = 8;

// One wrapped tile row: the FromDIP(50) tile plus tiles_sizer's FromDIP(5) padding on each side.
static constexpr int TILE_ROW_HEIGHT_DIP = 60;

FilamentMapRowsView::FilamentMapRowsView(wxWindow                 *parent,
                                         const std::vector<int>   &plate_filaments,
                                         const FilamentInventory  &inventory,
                                         size_t                    tool_count,
                                         std::function<void(bool)> on_state_changed)
    : wxPanel(parent, wxID_ANY)
    , m_plate_filaments(plate_filaments)
    , m_inventory(inventory)
    , m_tool_count(tool_count)
    , m_on_state_changed(std::move(on_state_changed))
{
    SetBackgroundColour(*wxWHITE);

    m_filament_preset = wxGetApp().preset_bundle->filament_presets;
    // full_config() returns by value; keep it alive while option pointers into it are used.
    const DynamicPrintConfig full_config = wxGetApp().preset_bundle->full_config();
    m_filament_color = full_config.option<ConfigOptionStrings>("filament_colour")->values;
    m_filament_type  = full_config.option<ConfigOptionStrings>("filament_type")->values;
    m_filament_names = full_config.option<ConfigOptionStrings>("filament_settings_id")->values;
    if (const auto *vendors = full_config.option<ConfigOptionStrings>("filament_vendor"))
        m_filament_vendor = vendors->values;

    wxBoxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);

    m_inventory_was_all_unset = inventory_all_unset(m_inventory);
    m_rows_panel = new FilamentMapRowsPanel(this, m_filament_color, m_filament_type, m_filament_names, m_plate_filaments,
                                            m_inventory, m_tool_count);
    m_rows_panel->ApplyProposal(compute_physical_map_proposal(m_filament_color, m_filament_type, m_filament_vendor,
                                                              m_filament_preset, m_plate_filaments, m_inventory));
    m_rows_panel->Bind(wxEVT_INVALID_MANUAL_MAP, [this](wxCommandEvent &event) {
        seed_status();
        event.Skip();
    });

    wxWindow *rows_widget = m_rows_panel;
    if (m_plate_filaments.size() > MAX_VISIBLE_MAP_ROWS) {
        wxSize best    = m_rows_panel->GetBestSize();
        int    per_row = FromDIP(TILE_ROW_HEIGHT_DIP);

        auto *scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxNO_BORDER);
        m_rows_panel->Reparent(scroll);
        auto *scroll_sizer = new wxBoxSizer(wxVERTICAL);
        scroll_sizer->Add(m_rows_panel, 1, wxEXPAND);
        scroll->SetSizer(scroll_sizer);
        scroll->SetMinSize(wxSize(best.GetWidth() + wxSystemSettings::GetMetric(wxSYS_VSCROLL_X), per_row * (int) MAX_VISIBLE_MAP_ROWS));
        scroll->FitInside();
        scroll->SetScrollRate(0, per_row);
        rows_widget = scroll;
    }
    main_sizer->Add(rows_widget, 0, wxEXPAND);

    // Offered only when the inventory was empty; seed_status() toggles its visibility.
    {
        wxPanel *record_panel = new wxPanel(this);
        record_panel->SetBackgroundColour(*wxWHITE);
        auto *record_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_record_checkbox  = new CheckBox(record_panel);
        m_record_checkbox->SetValue(true);
        record_sizer->Add(m_record_checkbox, 0, wxALIGN_CENTER_VERTICAL);
        auto *record_label = new Label(record_panel, _L("Remember these as the loaded filaments"));
        record_label->SetFont(Label::Body_12);
        record_sizer->Add(record_label, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(3));
        record_panel->SetSizer(record_sizer);
        m_record_row = record_panel;
        main_sizer->AddSpacer(FromDIP(8));
        main_sizer->Add(m_record_row, 0);
    }

    if (Plater *preview_plater = wxGetApp().plater()) {
        if (PartPlate *curr_plate = preview_plater->get_partplate_list().get_curr_plate()) {
            if (curr_plate->thumbnail_data.is_valid() && curr_plate->no_light_thumbnail_data.is_valid()) {
                m_preview_lit      = curr_plate->thumbnail_data;
                m_preview_no_light = curr_plate->no_light_thumbnail_data;
            }
        }
    }

    main_sizer->AddSpacer(FromDIP(8));
    m_preview_panel = new ThumbnailPanel(this);
    // ThumbnailPanel parents its unused wxStaticBitmap to its parent (this view), where it paints
    // a grey square over the first tiles. set_thumbnail() does not use it.
    if (m_preview_panel->m_staticbitmap != nullptr)
        m_preview_panel->m_staticbitmap->Hide();
    m_preview_panel->SetMinSize(wxSize(FromDIP(180), FromDIP(180)));
    m_preview_panel->SetMaxSize(wxSize(FromDIP(180), FromDIP(180)));
    m_preview_panel->Hide();
    main_sizer->Add(m_preview_panel, 0, wxALIGN_CENTER_HORIZONTAL);

    auto *auto_btn = new Button(this, _L("Automatic"));
    auto_btn->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    auto_btn->SetToolTip(_L("Fill the rows with a freshly computed automatic match."));
    auto_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { apply_automatic(); });
    main_sizer->Add(auto_btn, 0, wxTOP, FromDIP(4));

    SetSizerAndFit(main_sizer);

    // The wrapped content height depends on the width.
    Bind(wxEVT_SIZE, [this](wxSizeEvent &e) {
        e.Skip();
        repin_rows_min_height();
    });

    // The rows panel's construction fires no wxEVT_INVALID_MANUAL_MAP.
    seed_status();

    // Pin the width so repeated host Fit()s don't ratchet the wrapping tiles narrower. After
    // seed_status(), since the unwrapped mismatch warning may be the widest line.
    Fit();
    SetMinSize(wxSize(GetSize().GetWidth(), -1));
}

bool FilamentMapRowsView::AllRowsAssigned() const
{
    return m_rows_panel != nullptr && m_rows_panel->AllRowsAssigned();
}

void FilamentMapRowsView::repin_rows_min_height()
{
    // The tiles' wrap sizer CalcMin assumes one row per tile; the scroll wrapper sizes itself.
    if (!m_rows_panel || m_rows_panel->GetParent() != this)
        return;
    m_rows_panel->Layout(); // wrap the tiles at the panel's current width first
    int bottom = 0;
    for (wxWindow *c : m_rows_panel->GetChildren()) {
        // Skip the tile picker popup, which is a child of the panel while open.
        if (c->IsTopLevel() || !c->IsShown())
            continue;
        bottom = std::max(bottom, c->GetPosition().y + c->GetSize().GetHeight());
    }
    if (bottom <= 0)
        return; // nothing laid out yet
    const int wanted = bottom + FromDIP(6);
    if (m_rows_panel->GetMinSize().GetHeight() != wanted)
        m_rows_panel->SetMinSize(wxSize(-1, wanted));
}

void FilamentMapRowsView::seed_status()
{
    if (!m_rows_panel) return;

    bool all_assigned = m_rows_panel->AllRowsAssigned();

    if (m_record_row)
        m_record_row->Show(m_inventory_was_all_unset && all_assigned);

    update_mismatch_warning();
    update_preview();

    repin_rows_min_height();
    InvalidateBestSize();
    Layout();
    // Notify last: the host re-fits from here, after the record row and preview changed size.
    if (m_on_state_changed)
        m_on_state_changed(all_assigned);

    // MSW does not erase what a moved child used to cover; repaint to clear the ghost.
    if (wxWindow* top = wxGetTopLevelParent(this))
        top->Refresh();
}

void FilamentMapRowsView::update_mismatch_warning()
{
    if (!m_rows_panel) return;

    // Warns, never blocks, when filaments merged onto one slot differ from its preset.
    std::vector<int> physical_map = m_rows_panel->GetPhysicalMaps();

    std::unordered_map<int, int> counts;
    for (int id : physical_map)
        if (id > 0) ++counts[id];

    wxString warning;
    for (size_t i = 0; i < physical_map.size() && warning.empty(); ++i) {
        int id = physical_map[i];
        if (id <= 0 || counts[id] < 2) continue;

        int f = (int) i + 1;
        std::string project_preset = ((size_t) (f - 1) < m_filament_preset.size()) ? m_filament_preset[f - 1] : std::string();
        if (project_preset.empty()) continue;

        const PhysicalFilament *pf = m_inventory.find(id);
        std::string slot_preset = pf ? resolve_slot_preset(*pf, wxGetApp().preset_bundle->filaments) : std::string();
        if (slot_preset.empty() || slot_preset == project_preset) continue;

        warning = wxString::Format(_L("Profile mismatch: %s ≠ %s"), from_u8(project_preset), from_u8(slot_preset));
    }

    m_rows_panel->SetFooterWarning(warning);
}

void FilamentMapRowsView::update_preview()
{
    if (!m_preview_panel || !m_rows_panel)
        return;

    // Recolors the plate snapshot like SyncAmsInfoDialog::change_default_normal, without its
    // edge-pixel smoothing pass. Renders nothing, so it is safe inside a modal loop.
    const ThumbnailData &lit      = m_preview_lit;
    const ThumbnailData &no_light = m_preview_no_light;
    if (!lit.is_valid() || !no_light.is_valid() || lit.width != no_light.width || lit.height != no_light.height) {
        m_preview_panel->Hide();
        Layout();
        return;
    }

    // 0 = no target; those pixels keep their original colour.
    std::vector<int> physical_map = m_rows_panel->GetPhysicalMaps();

    std::vector<bool>     has_target(physical_map.size(), false);
    std::vector<wxColour> target_color(physical_map.size());
    for (size_t i = 0; i < physical_map.size(); ++i) {
        if (physical_map[i] <= 0) continue;
        const PhysicalFilament *pf = m_inventory.find(physical_map[i]);
        if (!pf || pf->color.empty()) continue;
        wxColour c(pf->color);
        if (!c.IsOk()) continue;
        // Match the gamma the GL thumbnail render applied to every other volume.
        Slic3r::ColorRGBA rgba{c.Red() / 255.0f, c.Green() / 255.0f, c.Blue() / 255.0f, 1.0f};
        Slic3r::ColorRGBA adjusted = adjust_color_for_rendering(rgba);
        target_color[i] = wxColour((unsigned char) (adjusted[0] * 255.0f), (unsigned char) (adjusted[1] * 255.0f),
                                    (unsigned char) (adjusted[2] * 255.0f));
        has_target[i] = true;
    }

    wxImage image((int) lit.width, (int) lit.height);
    image.InitAlpha();
    for (unsigned int r = 0; r < lit.height; ++r) {
        unsigned int rr = (lit.height - 1 - r) * lit.width;
        for (unsigned int c = 0; c < lit.width; ++c) {
            const unsigned char *origin_px   = lit.pixels.data() + 4 * (rr + c);
            const unsigned char *no_light_px = no_light.pixels.data() + 4 * (rr + c);
            unsigned char        out[3]      = {origin_px[0], origin_px[1], origin_px[2]};

            if (origin_px[3] > 0) {
                // The no-light pass encodes the 0-based filament as 255 - alpha (GLCanvas3D ban_light).
                int filament_idx = 255 - no_light_px[3];
                if (filament_idx >= 0 && (size_t) filament_idx < has_target.size() && has_target[filament_idx]) {
                    const wxColour &target       = target_color[filament_idx];
                    int             origin_rgb   = origin_px[0] + origin_px[1] + origin_px[2];
                    int             no_light_rgb = no_light_px[0] + no_light_px[1] + no_light_px[2];
                    if (no_light_rgb <= 0) {
                        out[0] = target.Red(); out[1] = target.Green(); out[2] = target.Blue();
                    } else if (origin_rgb >= no_light_rgb) {
                        out[0] = (unsigned char) std::clamp(target.Red()   + (origin_px[0] - no_light_px[0]), 0, 255);
                        out[1] = (unsigned char) std::clamp(target.Green() + (origin_px[1] - no_light_px[1]), 0, 255);
                        out[2] = (unsigned char) std::clamp(target.Blue()  + (origin_px[2] - no_light_px[2]), 0, 255);
                    } else {
                        float ratio = origin_rgb / (float) no_light_rgb;
                        out[0] = (unsigned char) std::clamp((int) (target.Red()   * ratio), 0, 255);
                        out[1] = (unsigned char) std::clamp((int) (target.Green() * ratio), 0, 255);
                        out[2] = (unsigned char) std::clamp((int) (target.Blue()  * ratio), 0, 255);
                    }
                }
            }
            image.SetRGB((int) c, (int) r, out[0], out[1], out[2]);
            image.SetAlpha((int) c, (int) r, origin_px[3]);
        }
    }

    const wxSize target_size = m_preview_panel->GetMinSize();
    if (target_size.GetWidth() > 0 && target_size.GetHeight() > 0)
        image = image.Rescale(target_size.GetWidth(), target_size.GetHeight(), wxIMAGE_QUALITY_BOX_AVERAGE);
    m_preview_panel->set_thumbnail(image);
    m_preview_panel->Show();
    Layout();
}

void FilamentMapRowsView::apply_automatic()
{
    m_rows_panel->ApplyProposal(compute_physical_map_proposal(m_filament_color, m_filament_type, m_filament_vendor,
                                                              m_filament_preset, m_plate_filaments, m_inventory));
    seed_status();
}

void FilamentMapRowsView::Commit()
{
    if (m_rows_panel == nullptr)
        return;
    m_filament_map = m_rows_panel->GetFilamentMaps();

    // Shown only for an empty inventory (bootstrap mode), so every row's physical id is still 0.
    if (m_record_row && m_record_row->IsShown() && m_record_checkbox && m_record_checkbox->GetValue()) {
        // Reload: the disk may have changed since the dialog was built. Existing slot ids are kept.
        FilamentInventories store;
        FilamentInventory  &new_inv = load_active_printer_inventory(store);
        for (int f : m_plate_filaments) {
            if (f < 1 || f > (int) m_filament_map.size()) continue;
            int tool = m_filament_map[f - 1];
            if (tool < 1 || tool > (int) m_tool_count) continue;
            new_inv.ensure_slot_count(tool);
            PhysicalFilament &slot = new_inv.slots[tool - 1];
            slot = build_physical_filament(
                (f - 1 < (int) m_filament_color.size()) ? m_filament_color[f - 1] : std::string(),
                (f - 1 < (int) m_filament_type.size()) ? m_filament_type[f - 1] : std::string(),
                ((size_t) (f - 1) < m_filament_preset.size()) ? m_filament_preset[f - 1] : std::string(),
                slot.id);
        }
        new_inv.ensure_ids();
        save_filament_inventories(store);
    }
}

FilamentMapRowsDialog::FilamentMapRowsDialog(wxWindow                *parent,
                                             const std::vector<int>  &plate_filaments,
                                             const FilamentInventory &inventory,
                                             size_t                   tool_count,
                                             const wxString          &title)
    : wxDialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
{
    SetBackgroundColour(*wxWHITE);
    // No max width: a cap would clip long preset labels.
    SetMinSize(wxSize(FromDIP(560), -1));

    wxBoxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->AddSpacer(FromDIP(20));

    m_view = new FilamentMapRowsView(this, plate_filaments, inventory, tool_count, [this](bool all_assigned) {
        if (m_ok_btn)
            m_ok_btn->Enable(all_assigned);
        Layout();
        Fit();
    });
    main_sizer->Add(m_view, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(15));

    auto *dlg_btns = new DialogButtons(this, {"OK", "Cancel"});
    m_ok_btn       = dlg_btns->GetOK();

    main_sizer->AddSpacer(FromDIP(10));
    main_sizer->Add(dlg_btns, 0, wxEXPAND);

    m_ok_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        m_view->Commit();
        EndModal(wxID_OK);
    });
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
    SetEscapeId(wxID_CANCEL);

    // The view's first state callback ran before m_ok_btn existed.
    m_ok_btn->Enable(m_view->AllRowsAssigned());

    SetSizerAndFit(main_sizer);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

std::optional<std::vector<int>> collect_device_map_table_for_send(wxWindow* parent, const std::vector<int>& plate_filaments, const wxString& title)
{
    FilamentInventories     store;
    const size_t            tool_count = addressable_tool_count_of(active_printer_session().profile());
    const FilamentInventory inventory  = load_active_printer_inventory(store);

    FilamentMapRowsDialog dlg(parent, plate_filaments, inventory, tool_count, title);
    if (dlg.ShowModal() != wxID_OK)
        return std::nullopt;
    return dlg.GetFilamentMaps();
}

}} // namespace Slic3r::GUI
