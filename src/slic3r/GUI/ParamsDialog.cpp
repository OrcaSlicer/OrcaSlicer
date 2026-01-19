#include "ParamsDialog.hpp"
#include "I18N.hpp"
#include "ParamsPanel.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "Tab.hpp"

#include "libslic3r/Utils.hpp"

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
        Hide();
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
        dynamic_cast<Tab *>(m_panel->get_current_tab())->set_just_edit(just_edit);
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
