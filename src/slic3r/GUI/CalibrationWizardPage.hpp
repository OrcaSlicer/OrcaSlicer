#ifndef slic3r_GUI_CalibrationWizardPage_hpp_
#define slic3r_GUI_CalibrationWizardPage_hpp_

#include "wx/event.h"
#include "Widgets/Button.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/TextInput.hpp"
#include "Widgets/AMSControl.hpp"
#include "Widgets/ProgressBar.hpp"
#include "Widgets/HyperLink.hpp"
#include "wxExtensions.hpp"
#include "PresetComboBoxes.hpp"

#include "slic3r/Utils/CalibUtils.hpp"
#include "../../libslic3r/calib.hpp"

namespace Slic3r { namespace GUI {


#define MIN_CALIBRATION_PAGE_WIDTH         FromDIP(1100)
#define PRESET_GAP                         FromDIP(25)
#define CALIBRATION_COMBOX_SIZE            wxSize(FromDIP(500), FromDIP(24))
#define CALIBRATION_FILAMENT_COMBOX_SIZE   wxSize(FromDIP(250), FromDIP(24))
#define CALIBRATION_OPTIMAL_INPUT_SIZE     wxSize(FromDIP(300), FromDIP(24))
#define CALIBRATION_FROM_TO_INPUT_SIZE     wxSize(FromDIP(160), FromDIP(24))
#define CALIBRATION_FGSIZER_HGAP           FromDIP(50)
#define CALIBRATION_TEXT_MAX_LENGTH        FromDIP(90) + CALIBRATION_FGSIZER_HGAP + 2 * CALIBRATION_FILAMENT_COMBOX_SIZE.x
#define CALIBRATION_PROGRESSBAR_LENGTH     FromDIP(690)

class CalibrationWizard;

enum class CalibrationStyle : int
{
    CALI_STYLE_DEFAULT = 0,
    CALI_STYLE_X1,
    CALI_STYLE_P1P,
};

CalibrationStyle get_cali_style(MachineObject* obj);

wxString get_cali_mode_caption_string(CalibMode mode);

enum CalibrationFilamentMode {
    /* calibration single filament at once */
    CALI_MODEL_SINGLE = 0,
    /* calibration multi filament at once */
    CALI_MODEL_MULITI,
};

enum CalibrationMethod {
    CALI_METHOD_MANUAL = 0,
    CALI_METHOD_AUTO,
    CALI_METHOD_NEW_AUTO,
    CALI_METHOD_NONE,
};

wxString get_calibration_wiki_page(CalibMode cali_mode);

CalibrationFilamentMode get_cali_filament_mode(MachineObject* obj, CalibMode mode);

CalibMode get_obj_calibration_mode(const MachineObject* obj);

CalibMode get_obj_calibration_mode(const MachineObject* obj, int& cali_stage);

CalibMode get_obj_calibration_mode(const MachineObject* obj, CalibrationMethod& method, int& cali_stage);


enum class CaliPageType {
    CALI_PAGE_START = 0,
    CALI_PAGE_PRESET,
    CALI_PAGE_CALI,
    CALI_PAGE_COARSE_SAVE,
    CALI_PAGE_FINE_CALI,
    CALI_PAGE_FINE_SAVE,
    CALI_PAGE_PA_SAVE,
    CALI_PAGE_FLOW_SAVE,
    CALI_PAGE_COMMON_SAVE,
};

class FilamentComboBox : public wxPanel
{
public:
    FilamentComboBox(wxWindow* parent, int index, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize);
    ~FilamentComboBox() {};

    void set_select_mode(CalibrationFilamentMode mode);
    CalibrationFilamentMode get_select_mode() { return m_mode; }
    void load_tray_from_ams(int id, DynamicPrintConfig& tray);
    void update_from_preset();
    int get_index() { return m_index; }
    int get_tray_id() { return m_tray_id; }
    bool is_bbl_filament() { return m_is_bbl_filamnet; }
    std::string get_tray_name() { return m_tray_name; }
    CalibrateFilamentComboBox* GetComboBox() { return m_comboBox; }
    CheckBox* GetCheckBox() { return m_checkBox; }
    void SetCheckBox(CheckBox* cb) { m_checkBox = cb; }
    wxRadioButton* GetRadioBox() { return m_radioBox; }
    void SetRadioBox(wxRadioButton* btn) { m_radioBox = btn; }
    virtual bool Show(bool show = true);
    virtual bool Enable(bool enable);
    virtual void SetValue(bool value, bool send_event = true);
    void msw_rescale();

    void ShowPanel();
    void HidePanel();

protected:
    int m_index{0};
    int m_tray_id { -1 };
    std::string m_tray_name;
    bool m_is_bbl_filamnet{ false };

    CheckBox* m_checkBox{ nullptr };
    wxRadioButton* m_radioBox{ nullptr };
    CalibrateFilamentComboBox* m_comboBox{ nullptr };
    CalibrationFilamentMode m_mode { CalibrationFilamentMode::CALI_MODEL_SINGLE };
};

typedef std::vector<FilamentComboBox*> FilamentComboBoxList;

class CaliPageCaption : public wxPanel
{
public:
    CaliPageCaption(wxWindow* parent,
        CalibMode cali_mode,
        wxWindowID id = wxID_ANY,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long style = wxTAB_TRAVERSAL);

    void show_prev_btn(bool show = true);
    void show_help_icon(bool show = true);
    void on_sys_color_changed();
    void msw_rescale();

protected:
    ScalableButton* m_prev_btn;
    ScalableButton* m_help_btn;

private:
    void init_bitmaps();
    void create_wiki(wxWindow* parent);

    HyperLink* m_wiki_text; // ORCA
    wxString  m_wiki_url;
    ScalableBitmap m_prev_bmp_normal;
    ScalableBitmap m_prev_bmp_hover;
    ScalableBitmap m_help_bmp_normal;
    ScalableBitmap m_help_bmp_hover;
};

class CaliPageStepGuide : public wxPanel
{
public:
    CaliPageStepGuide(wxWindow* parent,
        wxArrayString steps,
        wxWindowID id = wxID_ANY,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long style = wxTAB_TRAVERSAL);

    void set_steps(int index);
    void set_steps_string(wxArrayString steps);
protected:
    wxArrayString m_steps;
    wxBoxSizer* m_step_sizer;
    std::vector<Label*> m_text_steps;
};

class CaliPagePicture : public wxPanel
{
public:
    CaliPagePicture(wxWindow* parent,
        wxWindowID id = wxID_ANY,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long style = wxTAB_TRAVERSAL);

    void set_bmp(const ScalableBitmap& bmp);
    void paint_on_img();
    void msw_rescale();

protected:
    ScalableBitmap m_bmp;
    wxStaticBitmap* m_img;
};

class PAPageHelpPanel : public wxPanel
{
public:
    PAPageHelpPanel(wxWindow* parent,
        bool ground_panel = true,
        wxWindowID id = wxID_ANY,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long style = wxTAB_TRAVERSAL);
    void msw_rescale();

protected:
    void create_pop_window();

    ScalableButton* m_help_btn;
    PopupWindow* m_pop_win;
    ScalableBitmap m_bmp;
    wxStaticBitmap* m_img;
};

enum class CaliPageActionType : int
{
    CALI_ACTION_MANAGE_RESULT = 0,
    CALI_ACTION_MANUAL_CALI,
    CALI_ACTION_AUTO_CALI,
    CALI_ACTION_START,
    CALI_ACTION_CALI,
    CALI_ACTION_FLOW_CALI_STAGE_2,
    CALI_ACTION_RECALI,
    CALI_ACTION_PREV,
    CALI_ACTION_NEXT,
    CALI_ACTION_CALI_NEXT,
    CALI_ACTION_PA_SAVE,
    CALI_ACTION_FLOW_SAVE,
    CALI_ACTION_FLOW_COARSE_SAVE,
    CALI_ACTION_FLOW_FINE_SAVE,
    CALI_ACTION_COMMON_SAVE,
    CALI_ACTION_GO_HOME,
    CALI_ACTION_COUNT
};

class CaliPageButton : public Button
{
public:
    CaliPageButton(wxWindow* parent, CaliPageActionType type, wxString text = wxEmptyString);

    CaliPageActionType get_action_type() { return m_action_type; }

    void msw_rescale();

private:
    CaliPageActionType m_action_type;
};

class CaliPageSendingPanel : public wxPanel
{
public:
    CaliPageSendingPanel(wxWindow* parent,
        wxWindowID id = wxID_ANY,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long style = wxTAB_TRAVERSAL);
    void create(wxWindow* parent);
    void update_print_error_info(int code, const std::string& msg, const std::string& extra);
    void show_send_failed_info(bool show, int code = 0, wxString description = wxEmptyString, wxString extra = wxEmptyString);
    std::shared_ptr<BBLStatusBarSend> get_sending_progress_bar();
    void reset();

private:
    std::shared_ptr<BBLStatusBarSend> m_send_progress_bar;
    wxScrolledWindow* m_sw_print_failed_info{ nullptr };
    Label* m_st_txt_error_code{ nullptr };
    Label* m_st_txt_error_desc{ nullptr };
    Label* m_st_txt_extra_info{ nullptr };
    int                               m_print_error_code;
    std::string                       m_print_error_msg;
    std::string                       m_print_error_extra;
};

class CaliPageActionPanel : public wxPanel
{
public:
    CaliPageActionPanel(wxWindow* parent,
        CalibMode cali_mode,
        CaliPageType page_type,
        wxWindowID id = wxID_ANY,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long style = wxTAB_TRAVERSAL);

    void bind_button(CaliPageActionType action_type, bool is_block);
    void show_button(CaliPageActionType action_type, bool show = true);
    void enable_button(CaliPageActionType action_type, bool enable = true);
    void msw_rescale();

protected:
    std::vector<CaliPageButton*> m_action_btns;
};

class CalibrationWizardPage : public wxPanel
{
public:
    CalibrationWizardPage(wxWindow* parent, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize, long style = wxTAB_TRAVERSAL);
    ~CalibrationWizardPage() {};

    CaliPageType get_page_type() { return m_page_type; }

    CalibrationWizardPage* get_prev_page() { return m_prev_page; }
    CalibrationWizardPage* get_next_page() { return m_next_page; }
    void set_prev_page(CalibrationWizardPage* prev) { m_prev_page = prev; }
    void set_next_page(CalibrationWizardPage* next) { m_next_page = next; }
    CalibrationWizardPage* chain(CalibrationWizardPage* next)
    {
        set_next_page(next);
        next->set_prev_page(this);
        return next;
    }

    virtual void update(MachineObject* obj) { curr_obj = obj; }
    /* device changed and connected */
    virtual void on_device_connected(MachineObject* obj) { curr_obj = obj; }

    virtual void on_reset_page() {}

    virtual void set_cali_filament_mode(CalibrationFilamentMode mode) {
        m_cali_filament_mode = mode;
    }

    virtual void set_cali_method(CalibrationMethod method) {
        m_cali_method = method;
    }

    virtual void msw_rescale();
    virtual void on_sys_color_changed();

protected:
    CalibMode             m_cali_mode;
    CaliPageType          m_page_type;
    CalibrationFilamentMode  m_cali_filament_mode;
    CalibrationMethod     m_cali_method{ CalibrationMethod::CALI_METHOD_MANUAL };
    MachineObject*        curr_obj { nullptr };

    wxWindow*             m_parent { nullptr };
    CaliPageCaption*      m_page_caption { nullptr };
    CaliPageActionPanel*  m_action_panel { nullptr };
    Label*         m_statictext_printer_msg{ nullptr };

private:
    CalibrationWizardPage* m_prev_page {nullptr};
    CalibrationWizardPage* m_next_page {nullptr};
};

wxDECLARE_EVENT(EVT_CALI_ACTION, wxCommandEvent);
wxDECLARE_EVENT(EVT_CALI_TRAY_CHANGED, wxCommandEvent);


}} // namespace Slic3r::GUI

#endif