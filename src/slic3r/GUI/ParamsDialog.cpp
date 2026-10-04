#include "ParamsDialog.hpp"
#include "I18N.hpp"
#include "ParamsPanel.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "Tab.hpp"

#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "libslic3r/Preset.hpp"
#include <wx/wx.h>
#include <wx/gdicmn.h>
#include <wx/toplevel.h>
#include <wx/bookctrl.h>
#include <wx/sizer.h>
#include <cstddef>
#include <wx/event.h>
#include <wx/utils.h>
#include "slic3r/GUI/Event.hpp"

namespace pt = boost::property_tree;
typedef pt::ptree JSON;

namespace Slic3r { 
namespace GUI {


ParamsDialog::ParamsDialog(wxWindow * parent)
	: DPIDialog(parent, wxID_ANY,  "", wxDefaultPosition,
		wxDefaultSize, wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
{
	m_panel = new ParamsPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBK_LEFT | wxTAB_TRAVERSAL);
	auto* topsizer = new wxBoxSizer(wxVERTICAL);
	topsizer->Add(m_panel, 1, wxALL | wxEXPAND, 0, NULL);

	SetSizerAndFit(topsizer);
	SetSize({75 * em_unit(), 60 * em_unit()});

	Layout();
	Center();
    Bind(wxEVT_SHOW, [this](auto &event) {
        if (IsShown()) {
            m_winDisabler = new wxWindowDisabler(this);
        } else {
            delete m_winDisabler;
            m_winDisabler = nullptr;
        }
    });
	Bind(wxEVT_CLOSE_WINDOW, [this](auto& event) {
#if 0
		auto tab = dynamic_cast<Tab *>(m_panel->get_current_tab());
        if (event.CanVeto() && tab->m_presets->current_is_dirty()) {
			bool ok = tab->may_discard_current_dirty_preset();
			if (!ok)
				event.Veto();
            else {
                tab->m_presets->discard_current_changes();
                tab->load_current_preset();
                Hide();
            }
        } else {
            Hide();
        }
#else
        auto tab = dynamic_cast<Tab *>(m_panel->get_current_tab());
        // ORCA: Validate filament temperature pairs before closing the material settings dialog.
        if (tab && !tab->validate_filament_temperature_pairs()) {
            if (event.CanVeto())
                event.Veto();
            return;
        }

        Hide();
        if (tab && tab->type() == Preset::TYPE_PRINTER) {
            // Normalize only after the dialog closes, when the final capability is known.
            auto &preset_bundle = *wxGetApp().preset_bundle;
            const bool supports_multiple_bed_types = preset_bundle.is_bbl_vendor() ||
                preset_bundle.printers.get_edited_preset().config.opt_bool("support_multi_bed_types");
            if (m_initial_multi_bed_types != supports_multiple_bed_types) {
                wxGetApp().plater()->normalize_bed_types(true);
                if (auto *plate_tab = dynamic_cast<TabPrintPlate *>(wxGetApp().get_plate_tab()))
                    plate_tab->update_model_config();
            }
        }

        if (!m_editing_filament_id.empty()) {
            Filamentinformation *filament_info = new Filamentinformation();
            filament_info->filament_id        = m_editing_filament_id;
            wxQueueEvent(wxGetApp().plater(), new SimpleEvent(EVT_MODIFY_FILAMENT, filament_info));
            m_editing_filament_id.clear();
        }
#endif
        wxGetApp().sidebar().finish_param_edit();
    });

    //wxGetApp().UpdateDlgDarkUI(this);
}

void ParamsDialog::Popup()
{
    wxGetApp().UpdateDlgDarkUI(this);
#ifdef __WIN32__
    Reparent(wxGetApp().mainframe);
#endif
    Center();
    if (m_panel && m_panel->get_current_tab()) {
        bool just_edit = false;
        if (!m_editing_filament_id.empty()) just_edit = true;
        auto *tab = dynamic_cast<Tab *>(m_panel->get_current_tab());
        tab->set_just_edit(just_edit);
        if (tab->type() == Preset::TYPE_PRINTER) {
            // Remember the initial capability and compare it when the dialog closes.
            // Bambu profiles support multiple bed types even when this option is unset.
            auto &preset_bundle = *wxGetApp().preset_bundle;
            m_initial_multi_bed_types = preset_bundle.is_bbl_vendor() ||
                preset_bundle.printers.get_edited_preset().config.opt_bool("support_multi_bed_types");
        }
    }
    Show();
}

void ParamsDialog::on_dpi_changed(const wxRect &suggested_rect)
{
	Fit();
	SetSize({75 * em_unit(), 60 * em_unit()});
	m_panel->msw_rescale();
	Refresh();
}

} // namespace GUI
} // namespace Slic3r
