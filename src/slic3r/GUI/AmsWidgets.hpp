#ifndef slic3r_AmsWidgets_hpp_
#define slic3r_AmsWidgets_hpp_


#include <wx/notebook.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/bmpcbox.h>
#include <wx/bmpbuttn.h>
#include <wx/treectrl.h>
#include <wx/imaglist.h>
#include <wx/artprov.h>
#include <wx/xrc/xmlres.h>
#include <wx/string.h>
#include <wx/stattext.h>
#include <wx/gdicmn.h>
#include <wx/font.h>
#include <wx/colour.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/grid.h>
#include <wx/dataview.h>
#include <wx/panel.h>
#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/icon.h>
#include <wx/bmpbuttn.h>
#include <wx/button.h>
#include <wx/statbox.h>
#include <wx/tglbtn.h>
#include <wx/popupwin.h>
#include <wx/spinctrl.h>
#include <wx/artprov.h>


#include <map>
#include <vector>
#include <memory>
#include "Event.hpp"
#include "libslic3r/ProjectTask.hpp"
#include "wxExtensions.hpp"
#include "slic3r/GUI/DeviceManager.hpp"

namespace Slic3r {
namespace GUI {

class TrayListModel : public wxDataViewVirtualListModel
{
public:
	enum
	{
		Col_TrayTitle,
		Col_TrayColor,
		Col_TrayMeterial,
		Col_TrayWeight,
		Col_TrayDiameter,
		Col_TrayTime,
		Col_TraySN,
		Col_TrayManufacturer,
		Col_TraySaturability,
		Col_TrayTransmittance,
		Col_TraySmooth,
		Col_Max,
	};

	TrayListModel();

	virtual unsigned int GetColumnCount() const wxOVERRIDE
	{
		return Col_Max;
	}

	virtual wxString GetColumnType(unsigned int col) const wxOVERRIDE
	{
		return "string";
	}

	virtual void GetValueByRow(wxVariant& variant,
		unsigned int row, unsigned int col) const wxOVERRIDE;
	virtual bool GetAttrByRow(unsigned int row, unsigned int col,
		wxDataViewItemAttr& attr) const wxOVERRIDE;
	virtual bool SetValueByRow(const wxVariant& variant,
		unsigned int row, unsigned int col) wxOVERRIDE;

	void update(MachineObject* obj);
	void clear_data();

private:
	wxArrayString m_titleColValues;
	wxArrayString m_colorColValues;
	wxArrayString m_meterialColValues;
	wxArrayString m_weightColValues;
	wxArrayString m_diameterColValues;
	wxArrayString m_timeColValues;
	wxArrayString m_snColValues;
	wxArrayString m_manufacturerColValues;
	wxArrayString m_saturabilityColValues;
	wxArrayString m_transmittanceColValues;
	wxArrayString m_smoothColValues;

};

} // GUI
} // Slic3r

#endif /* slic3r_Tab_hpp_ */
