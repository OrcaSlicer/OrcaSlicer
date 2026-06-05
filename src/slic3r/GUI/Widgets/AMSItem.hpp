#ifndef slic3r_GUI_AMSITEM_hpp_
#define slic3r_GUI_AMSITEM_hpp_

#include "../wxExtensions.hpp"
#include "StaticBox.hpp"
#include "StepCtrl.hpp"
#include "Button.hpp"
#include "../DeviceManager.hpp"
#include "slic3r/GUI/Event.hpp"
#include "slic3r/GUI/AmsMappingPopup.hpp"
#include <wx/simplebook.h>
#include <wx/hyperlink.h>
#include <wx/animate.h>
#include <wx/dynarray.h>


#define AMS_CONTROL_BRAND_COLOUR wxColour(0, 150, 136)
#define AMS_CONTROL_GRAY700 wxColour(107, 107, 107)
#define AMS_CONTROL_GRAY800 wxColour(50, 58, 61)
#define AMS_CONTROL_GRAY500 wxColour(172, 172, 172)
#define AMS_CONTROL_DISABLE_COLOUR wxColour(206, 206, 206)
#define AMS_CONTROL_DISABLE_TEXT_COLOUR wxColour(144, 144, 144)
#define AMS_CONTROL_WHITE_COLOUR wxColour(255, 255, 255)
#define AMS_CONTROL_BLACK_COLOUR wxColour(0, 0, 0)
#define AMS_CONTROL_DEF_BLOCK_BK_COLOUR wxColour(238, 238, 238)
#define AMS_CONTROL_DEF_LIB_BK_COLOUR wxColour(248, 248, 248)
#define AMS_EXTRUDER_DEF_COLOUR wxColour(234, 234, 234)
#define AMS_CONTROL_MAX_COUNT 4
#define AMS_CONTRO_CALIBRATION_BUTTON_SIZE wxSize(FromDIP(150), FromDIP(28))
#define AMS_CONTROL_DEF_HUMIDITY_BK_COLOUR wxColour(238, 238, 238)


namespace Slic3r { namespace GUI {

enum AMSModel {
    EXT_AMS             = 0,    //ext
    GENERIC_AMS         = 1,
    AMS_LITE            = 2,    //ams-lite
    N3F_AMS             = 3,
    N3S_AMS             = 4     //n3s  single_ams
};

enum AMSModelOriginType {
    GENERIC_EXT,
    LITE_EXT
};

enum ActionButton {
    ACTION_BTN_CALI     = 0,
    ACTION_BTN_LOAD     = 1,
    ACTION_BTN_UNLOAD   = 2,
    ACTION_BTN_COUNT    = 3
};

enum class AMSRoadMode : int {
    AMS_ROAD_MODE_LEFT,
    AMS_ROAD_MODE_LEFT_RIGHT,
    AMS_ROAD_MODE_END,
    AMS_ROAD_MODE_END_ONLY,
    AMS_ROAD_MODE_NONE,
    AMS_ROAD_MODE_NONE_ANY_ROAD,
    AMS_ROAD_MODE_VIRTUAL_TRAY
};

enum class AMSPanelPos : int {
    SINGLE_PANEL,
    LEFT_PANEL,
    RIGHT_PANEL,
};

enum class AMSRoadShowMode : int {
    AMS_ROAD_MODE_FOUR,
    AMS_ROAD_MODE_DOUBLE,
    AMS_ROAD_MODE_SINGLE,
    AMS_ROAD_MODE_SINGLE_N3S,
    AMS_ROAD_MODE_AMS_LITE,
    AMS_ROAD_MODE_NONE
};

enum class AMSPassRoadMode : int {
    AMS_ROAD_MODE_NONE,
    AMS_ROAD_MODE_LEFT,
    AMS_ROAD_MODE_LEFT_RIGHT,
    AMS_ROAD_MODE_END_TOP,
    AMS_ROAD_MODE_END_RIGHT,
    AMS_ROAD_MODE_END_BOTTOM,
};

enum class AMSAction : int {
    AMS_ACTION_NONE,
    AMS_ACTION_LOAD,
    AMS_ACTION_UNLOAD,
    AMS_ACTION_CALI,
    AMS_ACTION_PRINTING,
    AMS_ACTION_NORMAL,
    AMS_ACTION_NOAMS,
};

enum class AMSPassRoadSTEP : int {
    AMS_ROAD_STEP_NONE = 0,
    AMS_ROAD_STEP_1 = 1, // lib -> extrusion
    AMS_ROAD_STEP_2 = 2, // extrusion->buffer
    AMS_ROAD_STEP_3 = 4, // extrusion

    AMS_ROAD_STEP_COMBO_LOAD_STEP1,
    AMS_ROAD_STEP_COMBO_LOAD_STEP2,
    AMS_ROAD_STEP_COMBO_LOAD_STEP3,
};

enum class AMSPassRoadType : int {
    AMS_ROAD_TYPE_NONE,
    AMS_ROAD_TYPE_LOAD,
    AMS_ROAD_TYPE_UNLOAD,
};

enum class AMSCanType : int {
    AMS_CAN_TYPE_NONE,
    AMS_CAN_TYPE_BRAND,
    AMS_CAN_TYPE_THIRDBRAND,
    AMS_CAN_TYPE_EMPTY,
    AMS_CAN_TYPE_VIRTUAL,
};

enum FilamentStep {
    STEP_IDLE,
    STEP_HEAT_NOZZLE,
    STEP_CUT_FILAMENT,
    STEP_PULL_CURR_FILAMENT,
    STEP_PUSH_NEW_FILAMENT,
    STEP_GRAB_NEW_FILAMENT,
    STEP_PURGE_OLD_FILAMENT,
    STEP_CONFIRM_EXTRUDED,
    STEP_CHECK_POSITION,
    STEP_COUNT,
};


enum FilamentStepType {
    STEP_TYPE_LOAD      = 0,
    STEP_TYPE_UNLOAD    = 1,
    STEP_TYPE_VT_LOAD   = 2,
};

#define AMS_ITEM_CUBE_SIZE wxSize(FromDIP(9), FromDIP(14))
#define AMS_ITEM_CUBE_SIZE2 wxSize(FromDIP(6), FromDIP(12))
#define AMS_PREVIEW_SIZE wxSize(FromDIP(82), FromDIP(27))
#define AMS_ITEM_SIZE wxSize(FromDIP(78), FromDIP(184))
#define AMS_PREV_FOUR_SIZE wxSize(FromDIP(52), FromDIP(32))
#define AMS_PREV_SINGLE_SIZE wxSize(FromDIP(28), FromDIP(32))
#define AMS_ITEM_HUMIDITY_SIZE wxSize(FromDIP(120), FromDIP(27))
#define AMS_CAN_LIB_SIZE wxSize(FromDIP(52), FromDIP(80))
#define AMS_LITE_CAN_LIB_SIZE wxSize(FromDIP(49), FromDIP(72))
#define AMS_CAN_ROAD_SIZE wxSize(FromDIP(264), FromDIP(50))
#define AMS_ITEMS_PANEL_SIZE wxSize(FromDIP(264), FromDIP(44))
//#define AMS_CANS_SIZE wxSize(FromDIP(284), FromDIP(184))
//#define AMS_CANS_WINDOW_SIZE wxSize(FromDIP(264), FromDIP(196))
#define AMS_STEP_SIZE wxSize(FromDIP(172), FromDIP(196))
#define AMS_REFRESH_SIZE wxSize(FromDIP(28), FromDIP(28))
#define AMS_EXTRUDER_SIZE wxSize(FromDIP(29), FromDIP(37))
#define AMS_EXTRUDER_DOUBLE_NOZZLE_BITMAP_SIZE wxSize(FromDIP(13), FromDIP(36))
#define AMS_EXTRUDER_SINGLE_NOZZLE_N_SIZE wxSize(FromDIP(18), FromDIP(36))
#define AMS_EXTRUDER_SINGLE_NOZZLE_XP_SIZE wxSize(FromDIP(25), FromDIP(36))
#define AMS_BODY_SIZE wxSize(FromDIP(36), FromDIP(55))
#define AMS_DOWN_ROAD_SIZE wxSize(FromDIP(566), FromDIP(10))

#define AMS_HUMIDITY_SIZE wxSize(FromDIP(93), FromDIP(26))
#define AMS_HUMIDITY_NO_PERCENT_SIZE wxSize(FromDIP(60), FromDIP(26))
#define AMS_HUMIDITY_DRY_WIDTH FromDIP(35)

#define AMS_PANEL_SIZE wxSize(FromDIP(264), FromDIP(150))

#define GENERIC_AMS_SLOT_NUM 4
#define MAX_AMS_NUM_IN_PANEL 2

struct Caninfo
{
    std::string     can_id;
    wxString        material_name;
    wxColour        material_colour = {*wxWHITE};
    AMSCanType      material_state;
    int             ctype=0;
    int             material_remain = 100;
    int             cali_idx = -1;
    std::string     filament_id;
    float           k = 0.0f;
    float           n = 0.0f;
    std::vector<wxColour> material_cols;

public:
    bool operator==(const Caninfo& other) const
    {
        if (can_id == other.can_id &&
            material_name == other.material_name &&
            material_colour == other.material_colour &&
            material_state == other.material_state &&
            ctype == other.ctype &&
            material_remain == other.material_remain &&
            cali_idx == other.cali_idx &&
            filament_id == other.filament_id &&
            k == other.k &&
            n == other.n &&
            material_cols == other.material_cols)
        {
            return true;
        }

        return false;
    };
};

struct AMSinfo
{
public:
    std::string             ams_id;
    std::vector<Caninfo>    cans;
    int                     nozzle_id = 0;
    std::string             current_can_id;
    AMSPassRoadSTEP         current_step = AMSPassRoadSTEP::AMS_ROAD_STEP_NONE;
    AMSAction               current_action;
    int                     curreent_filamentstep;
    int                     ams_humidity = 0;
    int                     humidity_raw = -1;
    int                     left_dray_time = 0;
    float                   current_temperature = INVALID_AMS_TEMPERATURE;
    AMSModel                ams_type = AMSModel::GENERIC_AMS;
    AMSModelOriginType      ext_type = AMSModelOriginType::GENERIC_EXT;

public:
    bool operator== (const AMSinfo& other) const
    {
        if (ams_id == other.ams_id &&
            cans == other.cans &&
            nozzle_id == other.nozzle_id &&
            current_can_id == other.current_can_id &&
            current_step == other.current_step &&
            current_action == other.current_action &&
            curreent_filamentstep == other.curreent_filamentstep &&
            ams_humidity == other.ams_humidity &&
            left_dray_time == other.left_dray_time &&
            current_temperature == other.current_temperature &&
            ams_type == other.ams_type &&
            ext_type == other.ext_type)
        {
            return true;
        }

        return false;
    };

    bool operator!=(const AMSinfo &other) const
    {
        if (operator==(other))
        {
            return false;
        }

        return true;
    };

    bool parse_ams_info(MachineObject* obj, DevAms *ams, bool remain_flag = false, bool humidity_flag = false);
    void parse_ext_info(MachineObject* obj, DevAmsTray tray);

    bool support_drying() const { return (ams_type == AMSModel::N3S_AMS) || (ams_type == AMSModel::N3F_AMS); };
    bool support_humidity() const { return  1 <= get_humidity_display_idx() && get_humidity_display_idx() <= 5; }
    Caninfo get_caninfo(const std::string& can_id, bool& found) const;

    int  get_humidity_display_idx() const;
};

/*************************************************
Description:AMSExtText
**************************************************/

class AMSExtText : public wxWindow
{
public:
    void msw_rescale();
    void paintEvent(wxPaintEvent& evt);

    void            render(wxDC& dc);
    void            doRender(wxDC& dc);
    AMSExtText(wxWindow* parent, wxWindowID id, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize);
    ~AMSExtText();
};


/*************************************************
Description:AMSrefresh
**************************************************/
#define AMS_REFRESH_PLAY_LOADING_TIMER 100
class AMSrefresh : public wxWindow
{
public:
    AMSrefresh();
    AMSrefresh(wxWindow *parent, std::string ams_id, wxString can_id, Caninfo info, const wxPoint &pos = wxDefaultPosition, const wxSize &size = wxDefaultSize);
    AMSrefresh(wxWindow *parent, std::string ams_id, int can_id, Caninfo info, const wxPoint &pos = wxDefaultPosition, const wxSize &size = wxDefaultSize);
    ~AMSrefresh();

public:
    void        Update(std::string ams_id, Caninfo info);

    std::string GetCanId() const { return m_info.can_id; };

    void    PlayLoading();
    void    StopLoading();

    void    msw_rescale();

protected:
    void create(wxWindow *parent, wxWindowID id, const wxPoint &pos, const wxSize &size);

    void on_timer(wxTimerEvent &event);
    void OnEnterWindow(wxMouseEvent &evt);
    void OnLeaveWindow(wxMouseEvent &evt);
    void OnClick(wxMouseEvent &evt);
    void post_event(wxCommandEvent &&event);
    void paintEvent(wxPaintEvent &evt);

protected:
    wxTimer *m_playing_timer= {nullptr};
    int      m_rotation_angle = 0;
    bool             m_play_loading = {false};
    bool             m_selected      = {false};

    std::string      m_ams_id;
    std::string      m_can_id;
    Caninfo          m_info;

    ScalableBitmap   m_bitmap_normal;
    ScalableBitmap   m_bitmap_selected;
    ScalableBitmap   m_bitmap_ams_rfid_0;
    ScalableBitmap   m_bitmap_ams_rfid_1;
    ScalableBitmap   m_bitmap_ams_rfid_2;
    ScalableBitmap   m_bitmap_ams_rfid_3;
    ScalableBitmap   m_bitmap_ams_rfid_4;
    ScalableBitmap   m_bitmap_ams_rfid_5;
    ScalableBitmap   m_bitmap_ams_rfid_6;
    ScalableBitmap   m_bitmap_ams_rfid_7;
    std::vector<ScalableBitmap> m_rfid_bitmap_list;

    wxString         m_refresh_id;
    wxBoxSizer *     m_size_body;
    virtual void     DoSetSize(int x, int y, int width, int height, int sizeFlags = wxSIZE_AUTO);

    bool m_disable_mode{ false };
};

/*************************************************
Description:AMSextruder
**************************************************/
class AMSextruderImage: public wxWindow
{
public:
    void OnAmsLoading(bool load, wxColour col);
    void TurnOff();
    void setShowState(bool show_state) { m_show_state = show_state; };
    void msw_rescale();
    void paintEvent(wxPaintEvent &evt);

	void            render(wxDC &dc);
    bool            m_show_state = {false};
    wxColour        m_colour;
    ScalableBitmap  m_ams_extruder;
    string m_file_name;
    bool            m_ams_loading{ false };
    void            doRender(wxDC &dc);
    AMSextruderImage(wxWindow *parent, wxWindowID id, string file_name, const wxSize& size, const wxPoint &pos = wxDefaultPosition);
    ~AMSextruderImage();
};

//AMSExtImage upon ext lib
class AMSExtImage : public wxWindow
{
private:
    std::string m_series_name;
    std::string m_printer_type_name;

    bool    m_show_ams_ext = false;
    bool    m_show_ext     = false;

    AMSPanelPos m_ext_pos;
    int         m_ext_num = 1;

    ScalableBitmap m_ext_image;

public:
    AMSExtImage(wxWindow *parent, AMSPanelPos ext_pos, int total_ext_num, bool over_ext, wxWindowID id = wxID_ANY, const wxPoint &pos = wxDefaultPosition);
    ~AMSExtImage();

    void msw_rescale();
    void setShowAmsExt(bool show);
    void setTotalExtNum(const std::string &series_name, const std::string &printer_type, int num);

private:
    void paintEvent(wxPaintEvent &evt);
    void render(wxDC &dc);
    void doRender(wxDC &dc);

    const wxBitmap &get_bmp(const std::string &printer_type, bool is_ams_ext, AMSPanelPos pos);
};


class AMSextruder : public wxWindow
{
private:
    int    m_nozzle_num = -1;
    string m_series_name;

public:
    void TurnOn(wxColour col);
    void TurnOff();
    void OnVamsLoading(bool load, wxColour col = AMS_CONTROL_GRAY500);
    void OnAmsLoading(bool load, int nozzle_id = 0, wxColour col = AMS_CONTROL_GRAY500);
    void msw_rescale();
    void has_ams(bool hams) {m_has_vams = hams; Refresh();};
    void no_ams_mode(bool mode) {m_none_ams_mode = mode; Refresh();};
    bool updateNozzleNum(int nozzle_num, const std::string& series_name = string());

    bool            m_none_ams_mode{true};
    bool            m_has_vams{false};
    bool            m_vams_loading{false};
    bool            m_ams_loading{false};
    wxColour        m_current_colur;
    wxColour        m_current_colur_deputy;

    wxBoxSizer *    m_bitmap_sizer{nullptr};
    wxPanel *       m_bitmap_panel{nullptr};
    //AMSextruderImage *m_amsSextruder{nullptr};
    AMSextruderImage* m_left_extruder = nullptr;
    AMSextruderImage* m_right_extruder = nullptr;
    AMSextruder(wxWindow *parent, wxWindowID id, int nozzle_num, const wxPoint &pos = wxDefaultPosition, const wxSize &size = wxDefaultSize);
    ~AMSextruder();

private:
    void create(wxWindow* parent, wxWindowID id, const wxPoint& pos, const wxSize& size, int nozzle_num);
};

/*************************************************
Description:AMSLib
**************************************************/
class AMSLib : public wxWindow
{
public:
    AMSLib(wxWindow *parent, std::string ams_idx, Caninfo info, AMSModelOriginType ext_type = AMSModelOriginType::GENERIC_EXT);
    ~AMSLib();
    void create(wxWindow* parent, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize);
public:
    wxColour     GetLibColour();
    Caninfo      m_info;
    MachineObject* m_obj = { nullptr };

    std::string  m_ams_id;
    std::string  m_slot_id;

    int          m_can_index = 0;
    bool         transparent_changed = { false };
    AMSModel     m_ams_model;
    AMSModelOriginType m_ext_type = { AMSModelOriginType::GENERIC_EXT };

    void         Update(Caninfo info, std::string ams_idx, bool refresh = true);
    void         UnableSelected() { m_unable_selected = true; };
    void         EableSelected() { m_unable_selected = false; };
    void         OnSelected();
    void         UnSelected();
    bool         is_selected() {return m_selected;};
    void         post_event(wxCommandEvent &&event);
    void         show_kn_value(bool show) { m_show_kn = show; };
    void         support_cali(bool sup) { m_support_cali = sup; Refresh(); };
    virtual bool Enable(bool enable = true);
    void         set_disable_mode(bool disable) { m_disable_mode = disable; }
    void         msw_rescale();
    void         on_pass_road(bool pass);

protected:
    wxStaticBitmap *m_edit_bitmp       = {nullptr};
    wxStaticBitmap *m_edit_bitmp_light = {nullptr};
    ScalableBitmap  m_bitmap_editable;
    ScalableBitmap  m_bitmap_editable_light;
    ScalableBitmap  m_bitmap_readonly;
    ScalableBitmap  m_bitmap_readonly_light;
    ScalableBitmap  m_bitmap_transparent;
    ScalableBitmap  m_bitmap_transparent_def;
    ScalableBitmap  m_bitmap_transparent_lite;

    ScalableBitmap  m_bitmap_extra_tray_left;
    ScalableBitmap  m_bitmap_extra_tray_right;
    ScalableBitmap  m_bitmap_extra_tray_mid;

    ScalableBitmap  m_bitmap_extra_tray_left_hover;
    ScalableBitmap  m_bitmap_extra_tray_right_hover;
    ScalableBitmap  m_bitmap_extra_tray_mid_hover;

    ScalableBitmap  m_bitmap_extra_tray_left_selected;
    ScalableBitmap  m_bitmap_extra_tray_right_selected;
    ScalableBitmap  m_bitmap_extra_tray_mid_selected;

    bool            m_unable_selected = {false};
    bool            m_enable          = {false};
    bool            m_selected        = {false};
    bool            m_hover           = {false};
    bool            m_show_kn         = {false};
    bool            m_support_cali    = {false};


    double   m_radius = {4};
    wxColour m_border_color;
    wxColour m_road_def_color;
    wxColour m_lib_color;
    bool m_disable_mode{ false };
    bool m_pass_road{false};

    void on_enter_window(wxMouseEvent &evt);
    void on_leave_window(wxMouseEvent &evt);
    void on_left_down(wxMouseEvent &evt);
    void paintEvent(wxPaintEvent &evt);
    void render(wxDC &dc);
    void render_lite_text(wxDC& dc);
    void render_generic_text(wxDC& dc);
    void doRender(wxDC& dc);
    void render_lite_lib(wxDC& dc);
    void render_generic_lib(wxDC& dc);
};

/*************************************************
Description:AMSRoad
**************************************************/
class AMSRoad : public wxWindow
{
public:
    AMSRoad();
    AMSRoad(wxWindow *parent, wxWindowID id, Caninfo info, int canindex, int maxcan, const wxPoint &pos = wxDefaultPosition, const wxSize &size = wxDefaultSize);
    void create(wxWindow *parent, wxWindowID id = wxID_ANY, const wxPoint &pos = wxDefaultPosition, const wxSize &size = wxDefaultSize);

public:
    AMSinfo                      m_amsinfo;
    Caninfo                      m_info;
    int                          m_canindex       = {0};
    AMSRoadMode                  m_rode_mode      = {AMSRoadMode::AMS_ROAD_MODE_LEFT_RIGHT};
    std::vector<AMSPassRoadMode> m_pass_rode_mode = {AMSPassRoadMode::AMS_ROAD_MODE_NONE};
    bool                         m_selected       = {false};
    int                          m_passroad_width = {6};
    double                       m_radius         = {4};
    wxColour                     m_road_def_color;
    wxColour                     m_road_color;
    void                         Update(AMSinfo amsinfo, Caninfo info, int canindex, int maxcan);

    std::vector<ScalableBitmap> ams_humidity_img;


    int      m_humidity = { 0 };
    bool     m_show_humidity = { false };
    bool     m_vams_loading{false};
    AMSModel m_ams_model;

    void OnVamsLoading(bool load, wxColour col = AMS_CONTROL_GRAY500);
    void SetPassRoadColour(wxColour col);
    void SetMode(AMSRoadMode mode);
    void OnPassRoad(std::vector<AMSPassRoadMode> prord_list);
    void UpdatePassRoad(int tag_index, AMSPassRoadType type, AMSPassRoadSTEP step);

    void paintEvent(wxPaintEvent &evt);
    void render(wxDC &dc);
    void doRender(wxDC &dc);
};


/*************************************************
Description:AMSRoadUpPart
**************************************************/
class AMSRoadUpPart : public wxWindow
{
public:
    AMSRoadUpPart();
    AMSRoadUpPart(wxWindow* parent, wxWindowID id, AMSinfo info, AMSModel mode, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize);
    void create(wxWindow* parent, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize);

public:
    void Update(AMSinfo amsinfo);

    void OnVamsLoading(bool load, wxColour col = AMS_CONTROL_GRAY500);
    void SetPassRoadColour(wxColour col);
    void SetMode(AMSRoadShowMode mode);
    void OnPassRoad(std::vector<AMSPassRoadMode> prord_list);
    void UpdatePassRoad(std::string ams_index, std::string slot_index, AMSPassRoadType type, AMSPassRoadSTEP step);

    void paintEvent(wxPaintEvent& evt);
    void render(wxDC& dc);
    void doRender(wxDC& dc);

    void msw_rescale();

private:
    AMSinfo m_amsinfo;

    // Caninfo                      m_info;

    int m_load_slot_index = {0};
    int m_load_ams_index  = {0};

    // AMSRoadMode                  m_rode_mode = { AMSRoadMode::AMS_ROAD_MODE_LEFT_RIGHT };
    std::vector<AMSPassRoadMode> m_pass_rode_mode = {AMSPassRoadMode::AMS_ROAD_MODE_NONE};
    AMSRoadShowMode              m_road_mode      = {AMSRoadShowMode::AMS_ROAD_MODE_FOUR};
    AMSPassRoadSTEP              m_load_step      = {AMSPassRoadSTEP::AMS_ROAD_STEP_NONE};

    bool     m_selected       = {false};
    int      m_passroad_width = {6};
    double   m_radius         = {4};
    wxColour m_road_def_color;
    wxColour m_road_color;

    std::vector<ScalableBitmap> ams_humidity_img;

    int      m_humidity      = {0};
    bool     m_show_humidity = {false};
    bool     m_vams_loading{false};
    AMSModel m_ams_model;
};


/*************************************************
Description:AMSRoadDownPart
**************************************************/
class AMSRoadDownPart : public wxWindow
{
public:
    AMSRoadDownPart();
    AMSRoadDownPart(wxWindow* parent, wxWindowID id, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize);
    void create(wxWindow* parent, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize);

public:
    // void                         Update(AMSRoadDownPartMode nozzle, AMSRoadShowMode left_mode, AMSRoadShowMode right_mode, int left_len, int right_len);
    void UpdateLeft(int nozzle_num, AMSRoadShowMode mode);
    void UpdateRight(int nozzle_num, AMSRoadShowMode mode);

    void OnVamsLoading(bool load, wxColour col = AMS_CONTROL_GRAY500);
    void SetPassRoadColour(bool left, wxColour col);
    void SetShowMode(AMSRoadShowMode left_mode, AMSRoadShowMode right_mode);
    void UpdatePassRoad(AMSPanelPos pos, int len, AMSPassRoadSTEP step);

    void paintEvent(wxPaintEvent& evt);
    void render(wxDC& dc);
    void doRender(wxDC& dc);

    void msw_rescale();

private:
    int             m_nozzle_num           = {1};
    AMSRoadShowMode m_single_ext_rode_mode = {AMSRoadShowMode::AMS_ROAD_MODE_FOUR};
    AMSRoadShowMode m_left_rode_mode       = {AMSRoadShowMode::AMS_ROAD_MODE_FOUR};
    AMSRoadShowMode m_right_rode_mode      = {AMSRoadShowMode::AMS_ROAD_MODE_FOUR};
    bool            m_selected             = {false};

    int             m_left_road_length     = {-1};
    int             m_right_road_length    = {-1};
    int             m_passroad_width       = {6};
    double          m_radius               = {4};
    AMSPassRoadType m_pass_road_type       = {AMSPassRoadType::AMS_ROAD_TYPE_NONE};
    AMSPassRoadSTEP m_pass_road_left_step  = {AMSPassRoadSTEP::AMS_ROAD_STEP_NONE};
    AMSPassRoadSTEP m_pass_road_right_step = {AMSPassRoadSTEP::AMS_ROAD_STEP_NONE};

    std::map<int, wxColour> m_road_color;
    bool m_vams_loading{false};
    AMSModel m_ams_model;
};

/*************************************************
Description:AMSPreview
**************************************************/
class AMSPreview : public wxWindow
{
public:
    AMSPreview();
    AMSPreview(wxWindow *parent, wxWindowID id, AMSinfo amsinfo, AMSModel itemType = AMSModel::GENERIC_AMS, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize);

    bool m_open = {false};
    void Open();
    void Close();

    void         Update(AMSinfo amsinfo);
    void         create(wxWindow *parent, wxWindowID id, const wxPoint &pos, const wxSize &size);
    void         OnEnterWindow(wxMouseEvent &evt);
    void         OnLeaveWindow(wxMouseEvent &evt);
    void         OnSelected();
    void         UnSelected();
    virtual bool Enable(bool enable = true);
    void         msw_rescale();
    bool         IsSelected() const;


    std::string  get_ams_id() const { return m_amsinfo.ams_id; };
    int          get_nozzle_id() const { return m_amsinfo.nozzle_id; };

protected:
    AMSinfo  m_amsinfo;

    wxSize   m_cube_size;
    wxColour m_background_colour = { AMS_CONTROL_DEF_LIB_BK_COLOUR };
    float    m_padding;
    float    m_space;
    bool     m_hover             = {false};
    bool     m_selected          = {false};
    AMSModel m_ams_item_type = AMSModel::GENERIC_AMS;

    ScalableBitmap m_ts_bitmap_cube;
    ScalableBitmap m_ts_bitmap_cube_dark;
    ScalableBitmap m_four_slot_bitmap;
    ScalableBitmap m_four_slot_bitmap_dark;
    ScalableBitmap m_single_slot_bitmap;
    ScalableBitmap m_single_slot_bitmap_dark;

    void         paintEvent(wxPaintEvent &evt);
    void         render(wxDC &dc);
    void         doRender(wxDC &dc);
};


/*************************************************
Description:AMSHumidity
**************************************************/
class AMSHumidity : public wxWindow
{
public:
    AMSHumidity();
    AMSHumidity(wxWindow* parent, wxWindowID id, AMSinfo info, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize);
    void create(wxWindow* parent, wxWindowID id = wxID_ANY, const wxPoint& pos = wxDefaultPosition, const wxSize& size = wxDefaultSize);

public:
    AMSinfo                      m_amsinfo;
    int                          m_canindex = { 0 };
    bool                         m_selected = { false };
    double                       m_radius = { 12 };
    void                         Update(AMSinfo amsinfo);

    std::vector<ScalableBitmap> ams_humidity_imgs;
    std::vector<ScalableBitmap> ams_humidity_dark_imgs;

    std::vector<ScalableBitmap> ams_humidity_no_num_imgs;
    std::vector<ScalableBitmap> ams_humidity_no_num_dark_imgs;

    ScalableBitmap ams_sun_img;
    ScalableBitmap ams_drying_img;

    bool     m_vams_loading{ false };
    AMSModel m_ams_model;

    void paintEvent(wxPaintEvent& evt);
    void render(wxDC& dc);
    void doRender(wxDC& dc);
    void msw_rescale();

private:
    void update_size();
};


/*************************************************
Description:AmsItem
**************************************************/
class AmsItem : public wxWindow
{
public:
    AmsItem(wxWindow *parent, AMSinfo info, AMSModel model, AMSPanelPos pos);
    ~AmsItem();

    void     Update(AMSinfo info);
    void     create(wxWindow *parent);
    void     AddCan(Caninfo caninfo, int canindex, int maxcan, wxBoxSizer* sizer);
    void     AddLiteCan(Caninfo caninfo, int canindex, wxGridSizer* sizer);
    void     SetDefSelectCan();
    void     SelectCan(std::string canid);
    void     PlayRridLoading(wxString canid);
    void     StopRridLoading(wxString canid);
    void     msw_rescale();
    void     show_sn_value(bool show);
    void     SetAmsStepExtra(wxString canid, AMSPassRoadType type, AMSPassRoadSTEP step);
    void     SetAmsStep(std::string amsid, std::string canid, AMSPassRoadType type, AMSPassRoadSTEP step);
    void     SetAmsStep(std::string can_id);
    void     paintEvent(wxPaintEvent& evt);
    void     render(wxDC& dc);
    void     doRender(wxDC& dc);
    void     RenderLiteRoad(wxDC& dc, wxSize size);
    wxColour GetTagColr(wxString canid);
    std::string GetCurrentCan();

public:
    AMSinfo             get_ams_info() const { return m_info; };

    std::string         get_ams_id() const { return m_info.ams_id; };
    AMSModel            get_ams_model() const { return m_info.ams_type; };

    AMSModelOriginType  get_ext_type() const { return m_info.ext_type; };
    AMSExtImage        *get_ext_image() const { return m_ext_image; };

    size_t                         get_can_count() const { return m_info.cans.size(); };
    std::map<std::string, AMSLib*> get_can_lib_list() const { return m_can_lib_list; };

    int  get_selection() const { return m_selection; };
    void set_selection(int selection) { m_selection = selection; };

    AMSPanelPos get_panel_pos() const { return m_panel_pos; };
    int         get_nozzle_id() const { return m_info.nozzle_id; };

private:
    ScalableBitmap  m_bitmap_extra_framework;
    int             m_canlib_selection = { -1 };
    int             m_selection = { 0 };
    int             m_can_count = { 0 };

    AMSModel        m_ams_model;
    AMSPanelPos     m_panel_pos;
    std::string     m_canlib_id;

    std::string     m_road_canid;
    wxColour        m_road_colour;

    std::map<std::string, AMSLib*>      m_can_lib_list;
    //std::map<std::string, AMSRoad*>     m_can_road_list;
    AMSRoadUpPart* m_panel_road = { nullptr };
    std::map<std::string, AMSrefresh*>  m_can_refresh_list;
    AMSHumidity* m_humidity = { nullptr };

    AMSinfo         m_info;
    wxBoxSizer *    sizer_can = {nullptr};
    wxGridSizer*    sizer_can_extra = { nullptr };
    wxBoxSizer *    sizer_humidity = { nullptr };
    wxBoxSizer *    sizer_item = { nullptr };
    wxBoxSizer *    sizer_can_middle = {nullptr};
    wxBoxSizer *    sizer_can_left = {nullptr};
    wxBoxSizer *    sizer_can_right = {nullptr};
    AMSExtImage*    m_ext_image = { nullptr };      //the ext image upon the ext ams
    AMSExtText* m_ext_text = { nullptr };       //the ext text upon the ext ams
};

wxDECLARE_EVENT(EVT_AMS_EXTRUSION_CALI, wxCommandEvent);
wxDECLARE_EVENT(EVT_AMS_LOAD, SimpleEvent);
wxDECLARE_EVENT(EVT_AMS_UNLOAD, SimpleEvent);
wxDECLARE_EVENT(EVT_AMS_SETTINGS, SimpleEvent);
wxDECLARE_EVENT(EVT_AMS_FILAMENT_BACKUP, SimpleEvent);
wxDECLARE_EVENT(EVT_AMS_REFRESH_RFID, wxCommandEvent);
wxDECLARE_EVENT(EVT_AMS_ON_SELECTED, wxCommandEvent);
wxDECLARE_EVENT(EVT_AMS_ON_FILAMENT_EDIT, wxCommandEvent);
wxDECLARE_EVENT(EVT_AMS_CLIBRATION_AGAIN, wxCommandEvent);
wxDECLARE_EVENT(EVT_AMS_CLIBRATION_CANCEL, wxCommandEvent);
wxDECLARE_EVENT(EVT_AMS_GUIDE_WIKI, wxCommandEvent);
wxDECLARE_EVENT(EVT_AMS_RETRY, wxCommandEvent);
wxDECLARE_EVENT(EVT_AMS_SHOW_HUMIDITY_TIPS, wxCommandEvent);
wxDECLARE_EVENT(EVT_AMS_UNSELETED_VAMS, wxCommandEvent);
wxDECLARE_EVENT(EVT_AMS_UNSELETED_AMS, wxCommandEvent);
wxDECLARE_EVENT(EVT_VAMS_ON_FILAMENT_EDIT, wxCommandEvent);
wxDECLARE_EVENT(EVT_AMS_SWITCH, SimpleEvent);

}} // namespace Slic3r::GUI

#endif // !slic3r_GUI_amscontrol_hpp_
