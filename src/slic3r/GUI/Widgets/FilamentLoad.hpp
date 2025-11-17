#ifndef slic3r_GUI_FILAMENTLOAD_hpp_
#define slic3r_GUI_FILAMENTLOAD_hpp_

#include "../wxExtensions.hpp"
#include "StaticBox.hpp"
#include "StepCtrl.hpp"
#include "AMSControl.hpp"
#include "../DeviceManager.hpp"
#include "slic3r/GUI/Event.hpp"
#include "slic3r/GUI/AmsMappingPopup.hpp"
#include <wx/simplebook.h>
#include <wx/hyperlink.h>
#include <wx/animate.h>
#include <wx/dynarray.h>


namespace Slic3r {  namespace GUI {

 //  The filament load panel
 // (1) Load from empty, empty -> filled
 // (2) Switch filament, fila_A -> fila_B*/
class FilamentLoad : public wxSimplebook
{
public:
    FilamentLoad(wxWindow* parent, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize);

protected:
    ::FilamentStepIndicator* m_filament_load_steps = { nullptr };
    ::FilamentStepIndicator* m_filament_unload_steps = { nullptr };
    ::FilamentStepIndicator* m_filament_vt_load_steps = { nullptr };
    int  m_ams_id = { 1 };
    int m_slot_id = { 1 };
    bool is_extrusion = false;


public:
    std::map<FilamentStep, wxString> FILAMENT_CHANGE_STEP_STRING;
    AMSModel m_ams_model{ AMSModel::GENERIC_AMS };
    AMSModel m_ext_model{ AMSModel::AMS_LITE };
    AMSModel m_is_none_ams_mode{ AMSModel::AMS_LITE };

    void SetAmsModel(AMSModel mode, AMSModel ext_mode) { m_ams_model = mode; m_ext_model = ext_mode; };

    void SetFilamentStep(FilamentStep item_idx, FilamentStepType f_type);
    void ShowFilamentTip(bool hasams = true);

    void SetupSteps(bool is_extrusion_exist);

    void show_nofilament_mode(bool show);
    void updateID(int ams_id, int slot_id) { m_ams_id = ams_id; m_slot_id = slot_id; };
    void SetExt(bool ext) { is_extrusion = ext; };

    void set_min_size(const wxSize& minSize);
    void set_max_size(const wxSize& maxSize);
    void set_background_color(const wxColour& colour);
};

}}



#endif // !slic3r_GUI_filamentload_hpp_