#ifndef slic3r_calib_dlg_hpp_
#define slic3r_calib_dlg_hpp_

#include "wxExtensions.hpp"
#include "GUI_Utils.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/RoundedRectangle.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/TextInput.hpp"
#include "Widgets/LabeledStaticBox.hpp"
#include "Widgets/LabeledCheckBox.hpp"
#include "Widgets/RadioGroup.hpp"
#include "GUI_App.hpp"
#include "wx/hyperlink.h"
#include <wx/anybutton.h>
#include <wx/checklst.h>
#include <wx/event.h>
#include <wx/radiobox.h>
#include <wx/sizer.h>
#include <functional>
#include <memory>
#include <utility>
#include "libslic3r/calib.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <string>
#include <vector>

namespace Slic3r { namespace GUI {

// The input shaper types the input shaping dialogs offer for the printer's firmware, as
// input_shaping_type values in the order of their radio buttons; the first is their default.
std::vector<std::string> shaper_type_values(const DynamicPrintConfig& printer);

// Filament a calibration prints with, chosen among the project's filaments; hidden with a single filament.
class CalibFilamentSelector
{
public:
    explicit CalibFilamentSelector(wxWindow* parent);
    wxSizer* sizer() const { return m_sizer; }
    int filament_id() const;
    // Extruder the filament prints on, 0 until slicing decides it.
    int extruder_id() const;
    void apply_to(Calib_Params& params) const;
    // The selected filament's preset, with the filament tab's unsaved edits when the tab has it open.
    const Preset* preset() const;
    // Called when the user picks a filament and each time the dialog opens.
    void set_on_change(std::function<void()> on_change) { m_on_change = std::move(on_change); }

private:
    void refresh();

    wxWindow*             m_parent;
    wxBoxSizer*           m_sizer;
    ::ComboBox*           m_combo;
    std::function<void()> m_on_change;
};

class PA_Calibration_Dlg : public DPIDialog
{
public:
    PA_Calibration_Dlg(wxWindow* parent, wxWindowID id, Plater* plater);
    ~PA_Calibration_Dlg();
    void on_dpi_changed(const wxRect& suggested_rect) override;
	void on_show(wxShowEvent& event);
protected:
    void reset_params();
	virtual void on_start(wxCommandEvent& event);
	virtual void on_extruder_type_changed(wxCommandEvent& event);
	virtual void on_method_changed(wxCommandEvent& event);

protected:
	bool m_bDDE;
	Calib_Params m_params;


	RadioGroup* m_rbExtruderType;
	RadioGroup* m_rbMethod;
	TextInput* m_tiStartPA;
	TextInput* m_tiEndPA;
	TextInput* m_tiPAStep;
	CheckBox* m_cbPrintNum;
	TextInput* m_tiBMAccels;
	TextInput* m_tiBMSpeeds;

	Plater* m_plater;
    std::unique_ptr<CalibFilamentSelector> m_filament;
};

class Temp_Calibration_Dlg : public DPIDialog
{
public:
    Temp_Calibration_Dlg(wxWindow* parent, wxWindowID id, Plater* plater);
    void on_dpi_changed(const wxRect& suggested_rect) override;

protected:
    
    virtual void on_start(wxCommandEvent& event);
    void on_show(wxShowEvent& event);
    Calib_Params m_params;

    RadioGroup* m_rbModel = nullptr;
    wxStaticBoxSizer* m_model_box = nullptr;
    TextInput* m_tiStart;
    TextInput* m_tiEnd;
    TextInput* m_tiStep;
    LabeledCheckBox*  m_cbResize;
    Plater* m_plater;
    std::unique_ptr<CalibFilamentSelector> m_filament;
};

class MaxVolumetricSpeed_Test_Dlg : public DPIDialog
{
public:
    MaxVolumetricSpeed_Test_Dlg(wxWindow* parent, wxWindowID id, Plater* plater);
    ~MaxVolumetricSpeed_Test_Dlg();
    void on_dpi_changed(const wxRect& suggested_rect) override;

protected:

    virtual void on_start(wxCommandEvent& event);
    Calib_Params m_params;

    TextInput* m_tiStart;
    TextInput* m_tiEnd;
    TextInput* m_tiStep;
    Plater* m_plater;
    std::unique_ptr<CalibFilamentSelector> m_filament;
};

class VFA_Test_Dlg : public DPIDialog {
public:
    VFA_Test_Dlg(wxWindow* parent, wxWindowID id, Plater* plater);
    ~VFA_Test_Dlg();
    void on_dpi_changed(const wxRect& suggested_rect) override;

protected:
    virtual void on_start(wxCommandEvent& event);
    Calib_Params m_params;

    TextInput* m_tiStart;
    TextInput* m_tiEnd;
    TextInput* m_tiStep;
    LabeledCheckBox*  m_cbAutoAdjust;
    LabeledCheckBox*  m_cbResize;
    Plater* m_plater;
    std::unique_ptr<CalibFilamentSelector> m_filament;
};


class Retraction_Test_Dlg : public DPIDialog
{
public:
    Retraction_Test_Dlg (wxWindow* parent, wxWindowID id, Plater* plater);
    ~Retraction_Test_Dlg ();
    void on_dpi_changed(const wxRect& suggested_rect) override;

protected:

    virtual void on_start(wxCommandEvent& event);
    Calib_Params m_params;

    TextInput* m_tiStart;
    TextInput* m_tiEnd;
    TextInput* m_tiStep;
    Plater* m_plater;
    std::unique_ptr<CalibFilamentSelector> m_filament;
};

class Input_Shaping_Freq_Test_Dlg : public DPIDialog
{
public:
    Input_Shaping_Freq_Test_Dlg (wxWindow* parent, wxWindowID id, Plater* plater);
    ~Input_Shaping_Freq_Test_Dlg ();
    void on_dpi_changed(const wxRect& suggested_rect) override;
    
protected:

    virtual void on_start(wxCommandEvent& event);
    Calib_Params m_params;

    RadioGroup* m_rbModel;
    RadioGroup* m_rbType;
    TextInput* m_tiFreqStartX;
    TextInput* m_tiFreqEndX;
    TextInput* m_tiFreqStartY;
    TextInput* m_tiFreqEndY;
    TextInput* m_tiDampingFactor;
    Plater* m_plater;
    std::unique_ptr<CalibFilamentSelector> m_filament;
};

class Input_Shaping_Damp_Test_Dlg : public DPIDialog
{
public:
    Input_Shaping_Damp_Test_Dlg (wxWindow* parent, wxWindowID id, Plater* plater);
    ~Input_Shaping_Damp_Test_Dlg ();
    void on_dpi_changed(const wxRect& suggested_rect) override;
    
protected:

    virtual void on_start(wxCommandEvent& event);
    Calib_Params m_params;

    RadioGroup* m_rbModel;
    RadioGroup* m_rbType;
    TextInput* m_tiFreqX;
    TextInput* m_tiFreqY;
    TextInput* m_tiDampingFactorStart;
    TextInput* m_tiDampingFactorEnd;
    Plater* m_plater;
    std::unique_ptr<CalibFilamentSelector> m_filament;
};

class Cornering_Test_Dlg : public DPIDialog
{
public:
    Cornering_Test_Dlg(wxWindow* parent, wxWindowID id, Plater* plater);
    ~Cornering_Test_Dlg();
    void on_dpi_changed(const wxRect& suggested_rect) override;
    
protected:
    virtual void on_start(wxCommandEvent& event);
    Calib_Params m_params;

    RadioGroup* m_rbModel;
    TextInput* m_tiJDStart;
    TextInput* m_tiJDEnd;
    Plater* m_plater;
    std::unique_ptr<CalibFilamentSelector> m_filament;
};

class FlowRateCalibrationDialog : public DPIDialog
{
public:
    FlowRateCalibrationDialog(wxWindow* parent, wxWindowID id, Plater* plater);
    ~FlowRateCalibrationDialog();
    void on_dpi_changed(const wxRect& suggested_rect) override;

protected:
    virtual void on_start(wxCommandEvent& event);

    RadioGroup* m_rbType;
    // ORCA: use standard OrcaSlicer ComboBox instead of BitmapComboBox
    ComboBox* m_rbPattern;
    Plater* m_plater;
    std::unique_ptr<CalibFilamentSelector> m_filament;
};
}} // namespace Slic3r::GUI
#endif
