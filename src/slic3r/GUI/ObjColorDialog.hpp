#ifndef _OBJ_COLOR_DIALOG_H_
#define _OBJ_COLOR_DIALOG_H_

#include "GUI_Utils.hpp"
#include "Camera.hpp"
#include "GuiColor.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/checkbox.h>
#include <wx/msgdlg.h>
#include "Widgets/SpinInput.hpp"
class Button;
class Label;
class ComboBox;

class ObjColorPanel : public wxPanel
{
public:
    // BBS
    ObjColorPanel(wxWindow *parent, Slic3r::ObjDialogInOut &in_out, const std::vector<std::string> &extruder_colours);
    ~ObjColorPanel();
    void msw_rescale();
    bool is_ok();
    void send_new_filament_to_ui();
    void cancel_paint_color();
    void update_filament_ids();
    struct ButtonState
    {
        ComboBox*   bitmap_combox{nullptr};
        bool      is_map{false};//int id{0};
    };
    typedef std::function<void()> LayoutChanggeCallback;
    void set_layout_callback(LayoutChanggeCallback);
    void do_layout_callback();
    bool do_show(bool show);
    void clear_instance_and_revert_offset();

private:
    wxBoxSizer *create_approximate_match_btn_sizer(wxWindow *parent);
    wxBoxSizer *create_add_btn_sizer(wxWindow *parent);
    wxBoxSizer *create_reset_btn_sizer(wxWindow *parent);
    wxBoxSizer *create_extruder_icon_and_rgba_sizer(wxWindow *parent, int id, const wxColour& color);
    std::string get_color_str(const wxColour &color);
    wxBoxSizer *create_color_icon_map_rgba_sizer(wxWindow *parent, int id, const wxColour &color);//for display map
    ComboBox* CreateEditorCtrl(wxWindow *parent,int id);
    void draw_new_table();
    void update_new_add_final_colors();
    void show_sizer(wxSizer *sizer, bool show);
    void deal_approximate_match_btn();
    bool deal_add_btn();
    void deal_reset_btn();
    void deal_algo(char cluster_number,bool redraw_ui =false);
    void deal_default_strategy();
    void deal_thumbnail();
    void generate_thumbnail();
    void set_view_angle_type(int);
private:
    //view ui
    Slic3r::ObjDialogInOut &  m_obj_in_out;
    Slic3r::GUI::Camera::ViewAngleType m_camera_view_angle_type{Slic3r::GUI::Camera::ViewAngleType::Iso};
    wxPanel *                 m_page_simple  = nullptr;
    wxBoxSizer *              m_sizer        = nullptr;
    wxBoxSizer *              m_sizer_simple = nullptr;
    wxBoxSizer *               m_sizer_current_filaments = nullptr;
    SpinInput *                m_color_cluster_num_by_user_ebox{nullptr};
    wxStaticText *             m_warning_text{nullptr};
    wxGridSizer *              m_new_grid_sizer{nullptr};
    wxScrolledWindow *         m_scrolledWindow{nullptr};
    Button *    m_quick_approximate_match_btn{nullptr};
    Button *    m_quick_add_btn{nullptr};
    Button *    m_quick_reset_btn{nullptr};
    std::vector<wxButton*> m_extruder_icon_list;
    std::vector<wxButton*> m_color_cluster_icon_list;//need modeify
    std::vector<wxStaticText*> m_color_cluster_text_list;//need modeify
    std::vector<wxGridSizer*> m_row_sizer_list;         // control show or not
    std::vector<wxBoxSizer *> m_row_col_boxsizer_list;
    std::vector<ButtonState*> m_result_icon_list;
    int                       m_last_cluster_num{-1};
    const int               m_combox_width{50};
    int                     m_combox_icon_width;
    int                     m_combox_icon_height;
    wxButton *              m_image_button = nullptr;
    LayoutChanggeCallback   m_layout_callback;
    //data
    char                       m_last_cluster_number{-2};
    std::vector<Slic3r::RGBA>& m_input_colors;
    int m_color_num_recommend{0};
    int m_color_cluster_num_by_algo{0};
    int m_input_colors_size{0};
    std::vector<wxColour> m_colours;//from project and show right
    std::vector<int>      m_cluster_map_filaments;//show middle
    int                   m_max_filament_index = 0;
    std::vector<wxColour> m_cluster_colours;//from_algo and show left
    bool                  m_can_add_filament{true};
    bool                  m_deal_thumbnail_flag{false};
    std::vector<wxColour> m_new_add_colors;
    std::vector<wxColour> m_new_add_final_colors;
    //algo result
    std::vector<Slic3r::RGBA> m_cluster_colors_from_algo;
    std::vector<int>          m_cluster_labels_from_algo;
    //result
    bool                        m_is_add_filament{false};
    unsigned char&             m_first_extruder_id;
    std::vector<unsigned char> &m_filament_ids;

    Slic3r::Vec3d m_thumbnail_offset;
};

class ObjColorDialog : public Slic3r::GUI::DPIDialog
{
public:
    ObjColorDialog(wxWindow *parent, Slic3r::ObjDialogInOut &in_out, const std::vector<std::string> &extruder_colours);
    wxBoxSizer *create_btn_sizer(long flags, bool exist_error);
    void on_dpi_changed(const wxRect &suggested_rect) override;
    void update_layout();
    bool Show(bool show) override;

private:
    ObjColorPanel*  m_panel_ObjColor  = nullptr;
    wxBoxSizer *                      m_main_sizer     = nullptr;
    wxBoxSizer *                      m_buttons_sizer   = nullptr;
    std::unordered_map<int, Button *> m_button_list;
    std::vector<unsigned char>&      m_filament_ids;
    unsigned char &                  m_first_extruder_id;
};

#endif  // _WIPE_TOWER_DIALOG_H_