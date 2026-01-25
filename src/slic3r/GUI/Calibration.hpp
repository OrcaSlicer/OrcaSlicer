#ifndef slic3r_GUI_Calibration_hpp_
#define slic3r_GUI_Calibration_hpp_

#include <wx/wx.h>
#include <wx/intl.h>
#include <wx/collpane.h>
#include <wx/dataview.h>
#include <wx/artprov.h>
#include <wx/xrc/xmlres.h>
#include <wx/dataview.h>
#include <wx/gdicmn.h>
#include <wx/font.h>
#include <wx/colour.h>
#include <wx/settings.h>
#include <wx/string.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/hyperlink.h>
#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/popupwin.h>
#include <wx/spinctrl.h>
#include <wx/artprov.h>
#include <wx/wrapsizer.h>

#include "GUI_Utils.hpp"
#include "wxExtensions.hpp"
#include "DeviceManager.hpp"
#include "Plater.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/StepCtrl.hpp"
#include "Widgets/CheckBox.hpp"

namespace Slic3r { namespace GUI {

class CalibrationDialog : public DPIDialog
{
private:
    std::map<std::string, ::CheckBox*> m_checkbox_list;

    wxWindow* select_xcam_cali { nullptr };
    wxWindow* select_bed_leveling { nullptr };
    wxWindow* select_vibration { nullptr };
    wxWindow* select_motor_noise { nullptr };
    wxWindow* select_nozzle_cali{ nullptr };
    wxWindow* select_heatbed_cali{ nullptr };
    wxWindow* select_clumppos_cali{ nullptr };
    wxWindow* create_check_option(wxString title, wxWindow *parent, wxString tooltip, std::string param);

public:
    CalibrationDialog(Plater *plater = nullptr);
    ~CalibrationDialog();
    void on_dpi_changed(const wxRect &suggested_rect) override;

    StepIndicator *m_calibration_flow;
    Button *       m_calibration_btn;
    MachineObject *m_obj = nullptr;

    std::vector<int> last_stage_list_info; 
    int              m_state{0};
    void             update_cali(MachineObject *obj);
    bool             is_stage_list_info_changed(MachineObject *obj);
    void             on_start_calibration(wxMouseEvent &event);
    void             update_machine_obj(MachineObject *obj);
    bool             Show(bool show) override;
};

}} // namespace Slic3r::GUI

#endif
