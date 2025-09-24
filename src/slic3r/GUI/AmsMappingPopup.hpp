#ifndef slic3r_GUI_AmsMappingPopup_hpp_
#define slic3r_GUI_AmsMappingPopup_hpp_

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
#include "BBLStatusBar.hpp"
#include "BBLStatusBarSend.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/ScrolledWindow.hpp"
#include "Widgets/SwitchButton.hpp"
#include "Widgets/PopupWindow.hpp"
#include <wx/simplebook.h>
#include <wx/hashmap.h>

#include "slic3r/GUI/DeviceCore/DevUtil.h"

#define MAPPING_ITEM_INVALID_REMAIN -1

namespace Slic3r { namespace GUI {


#define AMS_TOTAL_COUNT 4

enum TrayType {
    NORMAL,
    THIRD,
    EMPTY
};

enum ShowType {
    LEFT,   //  only show left ams and left ext
    RIGHT,  //only show right ams and right ext
    LEFT_AND_RIGHT  //show left and right ams at the same time
};

struct TrayData
{
    TrayType        type;
    int             id;
    int             ctype = 0;
    int             remain = MAPPING_ITEM_INVALID_REMAIN;
    std::string     name;
    std::string     filament_type;
    wxColour        colour;
    std::vector<wxColour> material_cols = std::vector<wxColour>();

    int             ams_id = 0;
    int             slot_id = 0;
};

class MaterialItem: public wxPanel
{
protected:
    int m_text_pos_x =  0;
    int m_text_pos_y = -1;
    bool m_dropdown_allow_painted = true;

public:
    MaterialItem(wxWindow *parent, wxColour mcolour, wxString mname);
    ~MaterialItem();

    wxPanel*    m_main_panel;
    wxColour    m_material_coloul;
    wxString    m_material_name;

    //info
    wxColour m_ams_coloul;
    wxString m_ams_name;
    int      m_ams_ctype = 0;
    std::vector<wxColour> m_ams_cols = std::vector<wxColour>();
    //reset
    wxColour              m_back_ams_coloul;
    wxString              m_back_ams_name;
    int                   m_back_ams_ctype = 0;
    std::vector<wxColour> m_back_ams_cols  = std::vector<wxColour>();

    ScalableBitmap m_arraw_bitmap_gray;
    ScalableBitmap m_arraw_bitmap_white;
    ScalableBitmap m_transparent_mitem;
    ScalableBitmap m_filament_wheel_transparent;
    ScalableBitmap m_ams_wheel_mitem;
    ScalableBitmap m_ams_not_match;

    bool m_selected {false};
    bool m_warning{false};
    bool m_match {true};
    bool m_enable {true};

    void msw_rescale();
    void allow_paint_dropdown(bool flag);
    void set_ams_info(wxColour col, wxString txt, int ctype=0, std::vector<wxColour> cols= std::vector<wxColour>(),bool record_back_info = false);
    void reset_ams_info();

    void disable();
    void enable();
    void on_normal();
    void on_selected();
    void on_warning();

    void paintEvent(wxPaintEvent &evt);
    virtual void render(wxDC &dc);
    void match(bool mat);
    virtual void doRender(wxDC &dc);
    virtual void reset_valid_info();
};

class MaterialSyncItem : public MaterialItem
{
public:
    MaterialSyncItem(wxWindow *parent, wxColour mcolour, wxString mname);
    ~MaterialSyncItem();
    int  get_real_offset();
    void render(wxDC &dc) override;
    void doRender(wxDC &dc) override;
    void set_material_index_str(std::string str);
    const std::string &get_material_index_str() { return m_material_index; }
private:
    std::string m_material_index;
};

class MappingItem : public wxPanel
{
public:
    MappingItem(wxWindow *parent);
    ~MappingItem();

    wxWindow*send_win{nullptr};
    wxString m_tray_index;
    wxColour m_coloul;
    wxString m_name;
    TrayData m_tray_data;
    ScalableBitmap m_transparent_mapping_item;
    ScalableBitmap mapping_item_checked;
    bool     m_unmatch{false};

    int     m_ams_id{255};
    int     m_slot_id{255};

public:
    void update_data(TrayData data);
    void send_event(int fliament_id);
    void set_data(const wxString& tag_name, wxColour colour, wxString name, bool remain_detect, TrayData data, bool unmatch = false);
    void set_checked(bool checked);
    void set_tray_index(wxString t_index) { m_tray_index = t_index; };

    void msw_rescale();

private:
    void paintEvent(wxPaintEvent &evt);
    void render(wxDC &dc);
    void doRender(wxDC &dc);

    int get_remain_area_height() const;

private:
    bool m_checked = false;
    bool m_support_remain_detect = false;/*paint the area as 100 percent*/
    bool m_to_paint_remain = false;/*do not paint the area*/
};

class MappingContainer : public wxPanel
{
private:
    int       m_slots_num = 4;/*1 or 4*/
    wxString  m_ams_type;
    wxBitmap  ams_mapping_item_container;

public:
    MappingContainer(wxWindow* parent, const wxString& ams_type, int slots_num = 4);
    ~MappingContainer();

public:
    int   get_slots_num() const { return m_slots_num;}

    void  msw_rescale();

protected:
    void paintEvent(wxPaintEvent& evt);
    void render(wxDC& dc);
    void doRender(wxDC& dc);
};

class AmsMapingPopup : public PopupWindow
{
    bool m_use_in_sync_dialog = false;
    bool m_ams_remain_detect_flag = false;
    bool m_ext_mapping_filatype_check = true;
    wxStaticText* m_title_text{ nullptr };

public:
    AmsMapingPopup(wxWindow *parent,bool use_in_sync_dialog = false);
    ~AmsMapingPopup() {};

    MaterialItem* m_parent_item{ nullptr };

    wxWindow* send_win{ nullptr };
    std::vector<std::string> m_materials_list;
    std::vector<wxBoxSizer*> m_amsmapping_container_sizer_list;
    std::vector<MappingContainer*> m_amsmapping_container_list;
    std::vector<MappingItem*> m_mapping_item_list;

    bool        m_has_unmatch_filament {false};
    int         m_current_filament_id;
    ShowType    m_show_type{ShowType::RIGHT};
    std::string m_tag_material;
    wxBoxSizer *m_sizer_main{nullptr};
    wxBoxSizer *m_sizer_ams{nullptr};
    wxBoxSizer *m_sizer_ams_left{nullptr};
    wxBoxSizer *m_sizer_ams_right{nullptr};
    wxBoxSizer *m_sizer_ams_left_horizonal{nullptr};
    wxBoxSizer *m_sizer_ams_right_horizonal{nullptr};
    wxBoxSizer* m_sizer_ams_basket_left{ nullptr };
    wxBoxSizer* m_sizer_ams_basket_right{ nullptr };
    wxBoxSizer *m_sizer_list{nullptr};

    MappingItem* m_left_extra_slot{nullptr};
    MappingItem* m_right_extra_slot{nullptr};

    wxPanel *    m_left_marea_panel{nullptr};
    wxPanel *    m_right_marea_panel{nullptr};
    wxPanel *    m_left_first_text_panel{nullptr};
    wxPanel *    m_right_first_text_panel{nullptr};
    wxBoxSizer * m_left_split_ams_sizer{nullptr};
    wxBoxSizer * m_right_split_ams_sizer{nullptr};
    Label *      m_left_tips{nullptr};
    Label *      m_right_tips{nullptr};
    ScalableButton* m_reset_btn{nullptr};
    wxString     m_single_tip_text;
    wxString     m_left_tip_text;
    wxString     m_right_tip_text;
    wxBoxSizer* m_sizer_split_ams_left;
    wxBoxSizer* m_sizer_split_ams_right;
    bool        m_mapping_from_multi_machines {false};

    void         set_sizer_title(wxBoxSizer *sizer, wxString text);
    wxBoxSizer*  create_split_sizer(wxWindow* parent, wxString text);
    void         set_send_win(wxWindow* win) {send_win = win;};
    void         update_materials_list(std::vector<std::string> list);
    void         set_tag_texture(std::string texture);
    void         update(MachineObject* obj, const std::vector<FilamentInfo>& ams_mapping_result);
    void         update_title(MachineObject* obj);
    void         update_items_check_state(const std::vector<FilamentInfo>& ams_mapping_result);
    void         update_ams_data_multi_machines();
    void         add_ams_mapping(std::vector<TrayData> tray_data, bool remain_detect_flag, wxWindow *container, wxBoxSizer *sizer);
    void         add_ext_ams_mapping(TrayData tray_data, MappingItem *item);
    void         set_current_filament_id(int id) { m_current_filament_id = id; };
    int          get_current_filament_id(){return m_current_filament_id;};
    bool         is_match_material(std::string material) const;
    void         on_left_down(wxMouseEvent &evt);
    virtual void OnDismiss() wxOVERRIDE;
    virtual bool ProcessLeftDown(wxMouseEvent &event) wxOVERRIDE;
    void         paintEvent(wxPaintEvent &evt);
    void         set_parent_item(MaterialItem* item) {m_parent_item = item;};
    void         set_show_type(ShowType type) { m_show_type = type; };
    std::vector<TrayData> parse_ams_mapping(const std::map<std::string, DevAms*, NumericStrCompare>& amsList);

    using ResetCallback = std::function<void(const std::string&)>;
    void reset_ams_info();
    void set_reset_callback(ResetCallback callback);
    void  show_reset_button();
    void  set_material_index_str(std::string str) { m_material_index = str; }
    const std::string &get_material_index_str() { return m_material_index; }
    void  set_only_show_ext_spool(bool flag);

public:
    void msw_rescale();

    void EnableExtMappingFilaTypeCheck(bool to_check = true) { m_ext_mapping_filatype_check = to_check;} ;

private:
    ResetCallback m_reset_callback{nullptr};
    std::string m_material_index;
    bool m_only_show_ext_spool{false};
};

class AmsMapingTipPopup : public PopupWindow
{
public:
    AmsMapingTipPopup(wxWindow *parent);
    ~AmsMapingTipPopup(){};
    void paintEvent(wxPaintEvent &evt);

    virtual void OnDismiss() wxOVERRIDE;
    virtual bool ProcessLeftDown(wxMouseEvent &event) wxOVERRIDE;

public:
    wxPanel *        m_panel_enable_ams;
    wxStaticText *   m_title_enable_ams;
    wxStaticText *   m_tip_enable_ams;
    wxPanel *        m_split_lines;
    wxPanel *        m_panel_disable_ams;
    wxStaticText *   m_title_disable_ams;
    wxStaticText *   m_tip_disable_ams;
};

class AmsHumidityLevelList : public wxPanel
{
public:
    AmsHumidityLevelList(wxWindow* parent);
    ~AmsHumidityLevelList() {};

public:
    void msw_rescale();

private:
    void paintEvent(wxPaintEvent& evt);
    void render(wxDC& dc);
    void doRender(wxDC& dc);

private:
    ScalableBitmap background_img;
    std::vector<ScalableBitmap> hum_level_img_light;
    std::vector<ScalableBitmap> hum_level_img_dark;
};

class AmsHumidityTipPopup : public PopupWindow
{
public:
    AmsHumidityTipPopup(wxWindow* parent);
    ~AmsHumidityTipPopup() {};

public:
    void set_humidity_level(int level);
    void msw_rescale();

private:
    virtual void OnDismiss() wxOVERRIDE {};
    virtual bool ProcessLeftDown(wxMouseEvent& event) wxOVERRIDE { return PopupWindow::ProcessLeftDown(event);  };

    void paintEvent(wxPaintEvent& evt);
    void render(wxDC& dc);
    void doRender(wxDC& dc);

private:
    int current_humidity_level = 0;

    ScalableBitmap close_img;

    wxStaticBitmap* curr_humidity_img;
    wxStaticBitmap* m_img;

    Label* m_staticText;;
    Label* m_staticText_note;

    AmsHumidityLevelList* humidity_level_list{nullptr};
};

class AmsTutorialPopup : public PopupWindow
{
public:
    Label* text_title;
    wxStaticBitmap* img_top;
    wxStaticBitmap* arrows_top;
    wxStaticText* tip_top;
    wxStaticBitmap* arrows_bottom;
    wxStaticText* tip_bottom;
    wxStaticBitmap* img_middle;
    wxStaticText* tip_middle;
    wxStaticBitmap* img_botton;

    AmsTutorialPopup(wxWindow* parent);
    ~AmsTutorialPopup() {};

    void paintEvent(wxPaintEvent& evt);
    virtual void OnDismiss() wxOVERRIDE;
    virtual bool ProcessLeftDown(wxMouseEvent& event) wxOVERRIDE;
};


class AmsIntroducePopup : public PopupWindow
{
public:
    bool          is_enable_ams = {false};
    Label* m_staticText_top;
    Label* m_staticText_bottom;
    wxStaticBitmap* m_img_enable_ams;
    wxStaticBitmap* m_img_disable_ams;

    AmsIntroducePopup(wxWindow* parent);
    ~AmsIntroducePopup() {};

    void set_mode(bool enable_ams);
    void paintEvent(wxPaintEvent& evt);
    virtual void OnDismiss() wxOVERRIDE;
    virtual bool ProcessLeftDown(wxMouseEvent& event) wxOVERRIDE;
};


class AmsRMGroup : public wxWindow
{
public:
    AmsRMGroup(wxWindow* parent, std::map<std::string, wxColour> group_info, wxString mname, wxString group_index);
    ~AmsRMGroup() {};

public:
    void set_index(std::string index) {m_selected_index = index;};
    void paintEvent(wxPaintEvent& evt);
    void render(wxDC& dc);
    void doRender(wxDC& dc);
    void on_mouse_move(wxMouseEvent& evt);

    double GetAngle(wxPoint pointA, wxPoint pointB);
    wxPoint CalculateEndpoint(const wxPoint& startPoint, int angle, int length);
private:
    std::map<std::string, wxColour> m_group_info;
    std::string     m_selected_index;
    ScalableBitmap  backup_current_use_white;
    ScalableBitmap  backup_current_use_black;
    ScalableBitmap  bitmap_backup_tips_0;
    ScalableBitmap  bitmap_backup_tips_1;
    ScalableBitmap  bitmap_editable;
    ScalableBitmap  bitmap_bg;
    ScalableBitmap  bitmap_editable_light;
    wxString        m_material_name;
    wxString        m_group_index;
};

class AmsReplaceMaterialDialog : public DPIDialog
{
public:
    AmsReplaceMaterialDialog(wxWindow* parent);
    ~AmsReplaceMaterialDialog() {};

public:
    void        update_machine_obj(MachineObject* obj);
    void        paintEvent(wxPaintEvent& evt);
    void        on_dpi_changed(const wxRect& suggested_rect) override;

public:
    MachineObject* m_obj{ nullptr };

    wxScrolledWindow* m_scrollview_groups{ nullptr };
    wxBoxSizer* m_scrollview_sizer{ nullptr };
    wxBoxSizer* m_main_sizer{ nullptr };
    wxWrapSizer* m_groups_sizer{ nullptr };
    SwitchBoard* m_nozzle_btn_panel { nullptr};

    std::vector<std::string> m_tray_used;
    Label* label_txt{nullptr};
    Label* identical_filament;

private:
    void        create();
    AmsRMGroup* create_backup_group(wxString gname, std::map<std::string, wxColour> group_info, wxString material);

    // update to nozzle
    void  on_nozzle_selected(wxCommandEvent& event) { update_to_nozzle(event.GetInt()); };
    void  update_to_nozzle(int nozzle_id);
};


wxDECLARE_EVENT(EVT_SET_FINISH_MAPPING, wxCommandEvent);

}} // namespace Slic3r::GUI

#endif
