#include "AuxiliaryDialog.hpp"
#include "I18N.hpp"
#include "GUI_AuxiliaryList.hpp"

#include "libslic3r/Utils.hpp"

#include <boost/property_tree/ptree.hpp>

namespace pt = boost::property_tree;
typedef pt::ptree JSON;

namespace Slic3r { 
namespace GUI {


AuxiliaryDialog::AuxiliaryDialog(wxWindow * parent)
	: DPIDialog(parent, wxID_ANY,  _L("Auxiliaryies"), wxDefaultPosition,
		wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
	m_aux_list = new AuxiliaryList(this);

	SetSizerAndFit(m_aux_list->get_top_sizer());
	SetSize({80 * em_unit(), 50 * em_unit()});

	Layout();
	Center();
}

void AuxiliaryDialog::on_dpi_changed(const wxRect& suggested_rect)
{
	Fit();
	SetSize({80 * em_unit(), 50 * em_unit()});
	//m_aux_list->msw_rescale();
	Refresh();
}

} // namespace GUI
} // namespace Slic3r
