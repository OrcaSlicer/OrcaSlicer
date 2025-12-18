#include <algorithm>
#include <sstream>
//#include "libslic3r/FlushVolCalc.hpp"
#include "ObjColorDialog.hpp"
#include "BitmapCache.hpp"
#include "GUI.hpp"//for ICON_SIZE
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "MsgDialog.hpp"
#include "Widgets/Button.hpp"
#include "MainFrame.hpp"
#include "libslic3r/Config.hpp"
#include "BitmapComboBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include <wx/sizer.h>

#include "libslic3r/ObjColorUtils.hpp"
#include "libslic3r/Model.hpp"
using namespace Slic3r;
using namespace Slic3r::GUI;

int objcolor_scale(const int val) { return val * Slic3r::GUI::wxGetApp().em_unit() / 10; }
int OBJCOLOR_ITEM_WIDTH() { return objcolor_scale(30); }
static const wxColour g_text_color = wxColour(107, 107, 107, 255);
static const wxColour g_undefined_color_in_obj   = wxColour(0, 255, 0, 255);
const int HEADER_BORDER  = 5;
const int CONTENT_BORDER = 3;
const int PANEL_WIDTH = 400;
const int COLOR_LABEL_WIDTH = 180;
const int  IMAGE_SIZE_WIDTH = 300;
#define MIN_OBJCOLOR_DIALOG_WIDTH FromDIP(400)
#define FIX_SCROLL_HEIGTH         FromDIP(400)
#define BTN_SIZE                wxSize(FromDIP(58), FromDIP(24))
#define BTN_GAP                 FromDIP(15)
#define FIX_SCROLL_IMAGE_WIDTH FromDIP(270)
static void update_ui(wxWindow* window)
{
    Slic3r::GUI::wxGetApp().UpdateDarkUI(window);
}

static const char g_min_cluster_color = 1;
static const char g_max_color = (int) EnforcerBlockerType::ExtruderMax;

wxBoxSizer* ObjColorDialog::create_btn_sizer(long flags,bool exist_error)
{
    auto btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    if (!exist_error) {
        btn_sizer->AddSpacer(FromDIP(25));
        wxStaticText *tips = new wxStaticText(this, wxID_ANY, _L("Open Wiki for more information >"));
        /* wxFont        font(10, wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false);
         font.SetUnderlined(true);
         tips->SetFont(font);*/
        auto font = tips->GetFont();
        font.SetUnderlined(true);
        tips->SetFont(font);
        tips->SetForegroundColour(wxColour("#009687"));
        tips->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &e) {
            bool is_zh = wxGetApp().app_config->get("language") == "zh_CN";
            if (is_zh) {
                wxLaunchDefaultBrowser("https://wiki.bambulab.com/zh/software/bambu-studio/import_obj");
            } else {
                wxLaunchDefaultBrowser("https://wiki.bambulab.com/en/software/bambu-studio/import_obj");
            }
        });
        btn_sizer->Add(tips, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);
    }
    btn_sizer->AddStretchSpacer();

    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});
    m_button_list[wxOK]     = dlg_btns->GetOK();
    m_button_list[wxCANCEL] = dlg_btns->GetCANCEL();

    btn_sizer->Add(dlg_btns, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    return btn_sizer;
}

void ObjColorDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    m_panel_ObjColor->msw_rescale();
    this->Refresh();
}
void ObjColorDialog::update_layout() {
    m_main_sizer->Layout();
    SetSizerAndFit(m_main_sizer);
}

bool ObjColorDialog::Show(bool show) {
    if (m_panel_ObjColor->do_show(show)) {
        return DPIDialog::Show(true);
    } else {
        return DPIDialog::Show(false);
    }
};

ObjColorDialog::ObjColorDialog(wxWindow *parent, Slic3r::ObjDialogInOut &in_out, const std::vector<std::string> &extruder_colours)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe),
                wxID_ANY,
                _(L("OBJ file import color")),
                wxDefaultPosition,
                wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE /* | wxRESIZE_BORDER*/)
    , m_filament_ids(in_out.filament_ids)
    , m_first_extruder_id(in_out.first_extruder_id)
{
    auto m_line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    m_line_top->SetBackgroundColour(wxColour(166, 169, 170));

    this->SetBackgroundColour(*wxWHITE);
    this->SetMinSize(wxSize(MIN_OBJCOLOR_DIALOG_WIDTH, -1));

    m_main_sizer = new wxBoxSizer(wxVERTICAL);
    m_main_sizer->Add(m_line_top, 0, wxEXPAND, 0);
    // set min sizer width according to extruders count
    auto sizer_width = (int) (2.8 * OBJCOLOR_ITEM_WIDTH());
    sizer_width      = sizer_width > MIN_OBJCOLOR_DIALOG_WIDTH ? sizer_width : MIN_OBJCOLOR_DIALOG_WIDTH;
    m_main_sizer->SetMinSize(wxSize(sizer_width, -1));
    bool some_face_no_color = false;
    if (!in_out.deal_vertex_color) {
        auto temp0 = in_out.input_colors.size();
        auto temp1 = in_out.model->objects[0]->volumes[0]->mesh_ptr()->facets_count();
        some_face_no_color = temp0 < temp1;
    }
    bool ok        = in_out.lost_material_name.empty() && !some_face_no_color;
    if (ok) {
        m_panel_ObjColor = new ObjColorPanel(this, in_out, extruder_colours);
        m_panel_ObjColor->set_layout_callback([this]() { update_layout(); });
        m_main_sizer->Add(m_panel_ObjColor, 1, wxEXPAND | wxALL, 0);
    }
    else {
        wxBoxSizer *  error_mtl_sizer       = new wxBoxSizer(wxVERTICAL);

        wxStaticText *error_mtl_title       = new wxStaticText(this, wxID_ANY, _L("Some faces don't have color defined."));
        if (!in_out.lost_material_name.empty()) {
            error_mtl_title->SetLabel(_L("MTL file exist error, could not find the material:") + " " + in_out.lost_material_name + ".");
        }
        error_mtl_title->SetFont(Label::Head_12);
        error_mtl_sizer->Add(error_mtl_title, 0, wxALIGN_LEFT | wxBOTTOM | wxTOP, FromDIP(5));

        wxStaticText *tip_title = new wxStaticText(this, wxID_ANY, _L("Please check OBJ or MTL file."));
        tip_title->SetFont(Label::Head_12);
        error_mtl_sizer->Add(tip_title, 0, wxALIGN_LEFT | wxBOTTOM | wxTOP, FromDIP(5));

        m_main_sizer->Add(error_mtl_sizer, 1, wxEXPAND | wxLEFT, FromDIP(25));
    }

    m_buttons_sizer = create_btn_sizer(wxOK | wxCANCEL, !ok);
    if (!ok) {
        m_button_list[wxCANCEL]->Hide();
        m_button_list[wxOK]->Enable(true);
        // ORCA no need to set colors again
    } else {
        m_button_list[wxOK]->Bind(wxEVT_UPDATE_UI, ([this](wxUpdateUIEvent &e) {
           if (m_panel_ObjColor->is_ok() == m_button_list[wxOK]->IsEnabled()) { return; }
           m_button_list[wxOK]->Enable(m_panel_ObjColor->is_ok());
           // ORCA no need to set colors again
         }));
    }
    m_main_sizer->Add(m_buttons_sizer, 0, wxBOTTOM | wxTOP | wxRIGHT | wxEXPAND, BTN_GAP);
    SetSizer(m_main_sizer);
    m_main_sizer->SetSizeHints(this);

    if (this->FindWindowById(wxID_OK, this)) {
        this->FindWindowById(wxID_OK, this)->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {// if OK button is clicked..
              if (!m_panel_ObjColor) {
                  EndModal(wxCANCEL);
                  return;
              }
              m_panel_ObjColor->clear_instance_and_revert_offset();
              m_panel_ObjColor->send_new_filament_to_ui();
              EndModal(wxID_OK);
            }, wxID_OK);
    }
    if (this->FindWindowById(wxID_CANCEL, this)) {
        update_ui(static_cast<wxButton*>(this->FindWindowById(wxID_CANCEL, this)));
        this->FindWindowById(wxID_CANCEL, this)->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (m_panel_ObjColor) {
                m_panel_ObjColor->cancel_paint_color();
            }
            EndModal(wxCANCEL);
            });
    }
    this->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &e) {
        if (m_panel_ObjColor) {
            m_panel_ObjColor->cancel_paint_color();
        }
        EndModal(wxCANCEL);
    });

    wxGetApp().UpdateDlgDarkUI(this);
    CenterOnParent();
}

// This panel contains all control widgets for both simple and advanced mode (these reside in separate sizers)
ObjColorPanel::ObjColorPanel(wxWindow *parent, Slic3r::ObjDialogInOut &in_out, const std::vector<std::string> &extruder_colours)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize /*,wxBORDER_RAISED*/)
    , m_obj_in_out(in_out)
    , m_input_colors(in_out.input_colors)
    , m_filament_ids(in_out.filament_ids)
    , m_first_extruder_id(in_out.first_extruder_id)
{
    if (in_out.input_colors.size() == 0) { return; }
    for (const std::string& color : extruder_colours) {
        m_colours.push_back(wxColor(color));
    }
    //deal input_colors
    m_input_colors_size = in_out.input_colors.size();
    for (size_t i = 0; i < in_out.input_colors.size(); i++) {
        if (color_is_equal(in_out.input_colors[i], UNDEFINE_COLOR)) { // not define color range:0~1
            in_out.input_colors[i] = convert_to_rgba(m_colours[0]);
        }
    }
    if (in_out.is_single_color && in_out.input_colors.size() >= 1) {
        m_cluster_colors_from_algo.emplace_back(in_out.input_colors[0]);
        m_cluster_colours.emplace_back(convert_to_wxColour(in_out.input_colors[0]));
        m_cluster_labels_from_algo.reserve(m_input_colors_size);
        for (size_t i = 0; i < m_input_colors_size; i++) {
            m_cluster_labels_from_algo.emplace_back(0);
        }
        m_cluster_map_filaments.resize(m_cluster_colors_from_algo.size());
        m_new_add_colors.resize(m_cluster_map_filaments.size());
        m_color_num_recommend = m_color_cluster_num_by_algo = m_cluster_colors_from_algo.size();
    } else {//cluster deal
        deal_algo(-1);
    }
    //end first cluster
    //draw ui
    auto sizer_width = FromDIP(300);
    // Create two switched panels with their own sizers
    m_sizer_simple          = new wxBoxSizer(wxVERTICAL);
    m_page_simple			= new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_page_simple->SetSizer(m_sizer_simple);
    m_page_simple->SetBackgroundColour(*wxWHITE);

    update_ui(m_page_simple);
    // BBS
    m_sizer_simple->AddSpacer(FromDIP(10));
    // BBS: for tunning flush volumes
    {
        //color cluster results
        wxBoxSizer *  specify_cluster_sizer       = new wxBoxSizer(wxHORIZONTAL);
        wxStaticText *specify_color_cluster_title = new wxStaticText(m_page_simple, wxID_ANY, _L("Specify number of colors:"));
        specify_color_cluster_title->SetFont(Label::Head_14);
        specify_cluster_sizer->Add(specify_color_cluster_title, 0, wxALIGN_CENTER | wxALL, FromDIP(5));

        m_color_cluster_num_by_user_ebox = new SpinInput(m_page_simple, "", wxEmptyString, wxDefaultPosition, wxSize(FromDIP(45), -1), wxTE_PROCESS_ENTER);
        m_color_cluster_num_by_user_ebox->SetValue(std::to_string(m_color_cluster_num_by_algo).c_str());
        m_color_cluster_num_by_user_ebox->SetToolTip(_L("Enter or click the adjustment button to modify number again"));
        {//event
            auto on_apply_color_cluster_text_modify = [this](wxEvent &e) {
                int number = m_color_cluster_num_by_user_ebox->GetValue();
                if (number > m_color_num_recommend || number < g_min_cluster_color) {
                    number   = number < g_min_cluster_color ? g_min_cluster_color : m_color_num_recommend;
                    auto str = wxString::Format(("%d"), number);
                    m_color_cluster_num_by_user_ebox->SetValue(str);
                    // m_color_cluster_num_by_user_ebox->SetInsertionPointEnd();
                }
                if (m_last_cluster_num != number) {
                    wxBusyCursor busy;
                    deal_algo(number, true);
                    Layout();
                    Refresh();
                    Update();
                    m_last_cluster_num = number;
                }
            };
            m_color_cluster_num_by_user_ebox->Bind(wxEVT_TEXT_ENTER, on_apply_color_cluster_text_modify);
            m_color_cluster_num_by_user_ebox->Bind(wxEVT_SPINCTRL, on_apply_color_cluster_text_modify);

            m_color_cluster_num_by_user_ebox->Bind(wxEVT_CHAR, [this](wxKeyEvent &e) {
                int keycode = e.GetKeyCode();
                wxString input_char = wxString::Format("%c", keycode);
                long     value;
                if (!input_char.ToLong(&value))
                    return;
                e.Skip();
            });
        }
        specify_cluster_sizer->AddSpacer(FromDIP(2));
        specify_cluster_sizer->Add(m_color_cluster_num_by_user_ebox, 0, wxALIGN_CENTER | wxALL, 0);
        specify_cluster_sizer->AddSpacer(FromDIP(15));
        wxStaticText *recommend_color_cluster_title = new wxStaticText(m_page_simple, wxID_ANY, "(" + std::to_string(m_color_num_recommend) + " " + _L("Recommended ") + ")");
        specify_cluster_sizer->Add(recommend_color_cluster_title, 0, wxALIGN_CENTER | wxALL, 0);

        m_sizer_simple->Add(specify_cluster_sizer, 0, wxEXPAND | wxLEFT, FromDIP(20));
        {//add combox
            auto      icon_sizer     = new wxBoxSizer(wxHORIZONTAL);
            auto plater     = wxGetApp().plater();
            {
                auto mo = m_obj_in_out.model->objects[0];
                mo->add_instance();
                auto mv  = mo->volumes[0];
                m_thumbnail_offset = Slic3r::Vec3d::Zero();
                auto box = mo->bounding_box_exact();
                if (box.min.x() < 0 || box.min.y() < 0 || box.min.z() < 0) {
                    m_thumbnail_offset = Slic3r::Vec3d(box.min.x() < 0 ? -box.min.x() : 0, box.min.y() < 0 ? -box.min.y() : 0, box.min.z() < 0 ? -box.min.z() : 0);
                    mv->translate(m_thumbnail_offset);
                }
            }

                wxStaticText *combox_title = new wxStaticText(m_page_simple, wxID_ANY, _L("view"), wxPoint(FromDIP(216), FromDIP(312)));
                // combox_title->SetTransparent(true);
                combox_title->SetBackgroundColour(wxColour(240, 240, 240, 0));
                combox_title->SetForegroundColour(wxColour(107, 107, 107, 100));
                auto cur_combox = new ComboBox(m_page_simple, wxID_ANY, wxEmptyString, wxPoint(FromDIP(250), FromDIP(310)), wxSize(FromDIP(100), -1), 0, NULL, wxCB_READONLY);
                wxArrayString choices = get_all_camera_view_type();
                for (size_t i = 0; i < choices.size(); i++) { cur_combox->Append(choices[i]); }
                cur_combox->SetSelection(0);
                cur_combox->Bind(wxEVT_COMBOBOX, [this](auto &e) {
                    set_view_angle_type(e.GetSelection());
                    Layout();
                    Fit();
                });
                // add image
                wxImage image(IMAGE_SIZE_WIDTH, IMAGE_SIZE_WIDTH);
                image.InitAlpha();
                for (unsigned int r = 0; r < IMAGE_SIZE_WIDTH; ++r) {
                    for (unsigned int c = 0; c < IMAGE_SIZE_WIDTH; ++c) {
                        image.SetRGB((int) c, (int) r, 0, 255, 0);
                        image.SetAlpha((int) c, (int) r, 255);
                    }
                }
                m_image_button = new wxButton(m_page_simple, wxID_ANY, {}, wxDefaultPosition, wxSize(FromDIP(IMAGE_SIZE_WIDTH), FromDIP(IMAGE_SIZE_WIDTH)),
                                              wxBORDER_NONE | wxBU_AUTODRAW);
                m_image_button->SetBitmap(image);
                m_image_button->SetCanFocus(false);
                icon_sizer->Add(m_image_button, 0, wxALIGN_CENTER_HORIZONTAL | wxALIGN_CENTER_VERTICAL | wxEXPAND | wxALL,
                                FromDIP(0)); // wxALIGN_CENTER_HORIZONTAL | wxALIGN_CENTER_VERTICAL | wxEXPAND | wxALL
                cur_combox->Raise();//for mac

            m_sizer_simple->Add(icon_sizer, FromDIP(0), wxALIGN_CENTER | wxALL, FromDIP(0));
        }
        wxBoxSizer *  current_filaments_title_sizer  = new wxBoxSizer(wxHORIZONTAL);
        wxStaticText *current_filaments_title = new wxStaticText(m_page_simple, wxID_ANY, _L("Current filament colors"));
        current_filaments_title->SetFont(Label::Head_14);
        current_filaments_title_sizer->Add(current_filaments_title, 0, wxALIGN_CENTER | wxALL, FromDIP(5));
        m_sizer_simple->Add(current_filaments_title_sizer, 0, wxEXPAND | wxLEFT, FromDIP(20));
        int current_filament_row = 1;
        int current_filament_col = 1;
        const int one_row_max    = 16;
        if (m_colours.size() > 1) {
            current_filament_row = (m_colours.size() - 1) / one_row_max + 1;
            if (current_filament_row >= 2) {
                current_filament_col = one_row_max;
            } else {
                current_filament_col = m_colours.size();
            }
        }
        m_sizer_current_filaments            = new wxBoxSizer(wxHORIZONTAL);
        wxGridSizer *current_filaments_sizer = new wxGridSizer(current_filament_row, current_filament_col, FromDIP(5), FromDIP(7)); //(int rows, int cols, int vgap, int hgap );
        for (size_t i = 0; i < m_colours.size(); i++) {
            auto extruder_icon_sizer = create_extruder_icon_and_rgba_sizer(m_page_simple, i, m_colours[i]);
            current_filaments_sizer->Add(extruder_icon_sizer, 0, wxALIGN_LEFT);
        }
        m_sizer_current_filaments->AddSpacer(FromDIP(1));
        m_sizer_current_filaments->Add(current_filaments_sizer, 0, wxEXPAND);
        m_sizer_current_filaments->AddStretchSpacer();
        m_sizer_simple->Add(m_sizer_current_filaments, 0, wxEXPAND | wxLEFT, FromDIP(25));
        //colors table title
        wxBoxSizer *  matching_title_sizer = new wxBoxSizer(wxHORIZONTAL);
        matching_title_sizer->AddSpacer(FromDIP(25));
        wxStaticText *matching_title       = new wxStaticText(m_page_simple, wxID_ANY, _L("Matching"));
        matching_title->SetFont(Label::Head_14);
        matching_title_sizer->Add(matching_title, 0, wxEXPAND , 0);
        m_sizer_simple->Add(matching_title_sizer, 0, wxEXPAND | wxTOP, FromDIP(15));// wxTop has FromDIP(10) margin
        //new color table
        m_scrolledWindow = new wxScrolledWindow(m_page_simple, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
        m_scrolledWindow->SetBackgroundColour(*wxWHITE);
        m_scrolledWindow->SetScrollRate(0, 20);
        m_scrolledWindow->EnableScrolling(false, true);
        m_scrolledWindow->ShowScrollbars(wxScrollbarVisibility::wxSHOW_SB_NEVER, wxScrollbarVisibility::wxSHOW_SB_DEFAULT);
        draw_new_table();

        m_sizer_simple->Add(m_scrolledWindow, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(5));
        //buttons
        wxBoxSizer *quick_set_sizer = new wxBoxSizer(wxHORIZONTAL);
        quick_set_sizer->AddSpacer(FromDIP(25));
        wxStaticText *quick_set_title = new wxStaticText(m_page_simple, wxID_ANY, _L("Quick set"));
        quick_set_title->SetFont(Label::Head_12);
        quick_set_sizer->Add(quick_set_title, 0, wxALIGN_CENTER | wxALL, 0);
        quick_set_sizer->AddSpacer(FromDIP(10));

        auto calc_approximate_match_btn_sizer = create_approximate_match_btn_sizer(m_page_simple);
        auto calc_add_btn_sizer = create_add_btn_sizer(m_page_simple);
        auto calc_reset_btn_sizer      = create_reset_btn_sizer(m_page_simple);
        quick_set_sizer->Add(calc_add_btn_sizer, 0, wxALIGN_CENTER | wxALL, 0);
        quick_set_sizer->AddSpacer(FromDIP(10));
        quick_set_sizer->Add(calc_approximate_match_btn_sizer, 0, wxALIGN_CENTER | wxALL, 0);
        quick_set_sizer->AddSpacer(FromDIP(10));
        quick_set_sizer->Add(calc_reset_btn_sizer, 0, wxALIGN_CENTER | wxALL, 0);
        quick_set_sizer->AddSpacer(FromDIP(10));
        m_sizer_simple->Add(quick_set_sizer, 0, wxEXPAND | wxTOP, FromDIP(10));

        wxBoxSizer *warning_sizer = new wxBoxSizer(wxHORIZONTAL);
        m_warning_text = new wxStaticText(m_page_simple, wxID_ANY, "");
        m_warning_text->SetForegroundColour(wxColour(107, 107, 107, 100));
        warning_sizer->Add(m_warning_text, 0, wxALIGN_CENTER | wxALL, 0);
        m_sizer_simple->Add(warning_sizer, 0, wxEXPAND | wxLEFT, FromDIP(25));

        m_sizer_simple->AddSpacer(15);
    }
    deal_default_strategy();
    deal_thumbnail();
    //page_simple//page_advanced
    m_sizer = new wxBoxSizer(wxVERTICAL);
    m_sizer->Add(m_page_simple, 0, wxEXPAND, 0);

    m_sizer->SetSizeHints(this);
    SetSizer(m_sizer);
    this->Layout();
}

ObjColorPanel::~ObjColorPanel() {
}

void ObjColorPanel::msw_rescale()
{
    for (unsigned int i = 0; i < m_extruder_icon_list.size(); ++i) {
        auto bitmap = *get_extruder_color_icon(m_colours[i].GetAsString(wxC2S_HTML_SYNTAX).ToStdString(), std::to_string(i + 1), FromDIP(16), FromDIP(16));
        m_extruder_icon_list[i]->SetBitmap(bitmap);
    }
   /* for (unsigned int i = 0; i < m_color_cluster_icon_list.size(); ++i) {
        auto bitmap = *get_extruder_color_icon(m_cluster_colours[i].GetAsString(wxC2S_HTML_SYNTAX).ToStdString(), std::to_string(i + 1), FromDIP(16), FromDIP(16));
        m_color_cluster_icon_list[i]->SetBitmap(bitmap);
    }*/
}

bool ObjColorPanel::is_ok() {
    for (auto item : m_result_icon_list) {
        if (item->bitmap_combox->IsShown()) {
            auto selection = item->bitmap_combox->GetSelection();
            if (selection < 1) {
                return false;
            }
        }
    }
    return true;
}

void ObjColorPanel::send_new_filament_to_ui()
{
    update_new_add_final_colors();
    if (m_is_add_filament) {
        for (auto c : m_new_add_final_colors) {
            /*auto evt = new ColorEvent(EVT_ADD_CUSTOM_FILAMENT, c);
            wxQueueEvent(wxGetApp().plater(), evt);*/
            wxGetApp().sidebar().add_custom_filament(c);
        }
    }
}

void ObjColorPanel::cancel_paint_color() {
    m_filament_ids.clear();
    auto mo = m_obj_in_out.model->objects[0];
    mo->config.set("extruder", 1);
    clear_instance_and_revert_offset();
    auto mv = mo->volumes[0];
    mv->mmu_segmentation_facets.reset();
    mv->config.set("extruder", 1);
    m_first_extruder_id = 1;
}

void ObjColorPanel::update_filament_ids()
{
   //deal m_filament_ids
   m_filament_ids.clear();
   m_filament_ids.reserve(m_input_colors_size);
   for (size_t i = 0; i < m_input_colors_size; i++) {
       auto label = m_cluster_labels_from_algo[i];
       if (m_cluster_map_filaments[label] > 0) {
           m_filament_ids.emplace_back(m_cluster_map_filaments[label]);
       } else {
           m_filament_ids.emplace_back(1);//min filament_id is 1
       }
   }
   m_first_extruder_id = m_cluster_map_filaments[0];
}

void ObjColorPanel::set_layout_callback(LayoutChanggeCallback callback) {
    m_layout_callback = callback;
}

void ObjColorPanel::do_layout_callback() {
    if (m_layout_callback) {
        m_layout_callback();
    }
}

wxBoxSizer *ObjColorPanel::create_approximate_match_btn_sizer(wxWindow *parent)
{
    auto       btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_quick_approximate_match_btn = new Button(parent, _L("Color match"));
    m_quick_approximate_match_btn->SetToolTip(_L("Approximate color matching."));
    m_quick_approximate_match_btn->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    m_quick_approximate_match_btn->SetFocus();
    btn_sizer->Add(m_quick_approximate_match_btn, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 0);
    m_quick_approximate_match_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        deal_approximate_match_btn();
        deal_thumbnail();
    });
    return btn_sizer;
}

wxBoxSizer *ObjColorPanel::create_add_btn_sizer(wxWindow *parent)
{
    auto       btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_quick_add_btn = new Button(parent, _L("Append"));
    m_quick_add_btn->SetToolTip(_L("Append to existing filaments"));
    auto cur_btn    = m_quick_add_btn;
    cur_btn->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    cur_btn->SetFocus();
    btn_sizer->Add(cur_btn, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 0);
    cur_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        deal_add_btn();
        deal_thumbnail();
    });
    return btn_sizer;
}

wxBoxSizer *ObjColorPanel::create_reset_btn_sizer(wxWindow *parent)
{
    auto       btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_quick_reset_btn = new Button(parent, _L("Reset"));
    m_quick_reset_btn->SetToolTip(_L("Reset mapped extruders."));
    auto cur_btn      = m_quick_reset_btn;
    cur_btn->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    cur_btn->SetFocus();
    btn_sizer->Add(cur_btn, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 0);
    cur_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        deal_reset_btn();
        deal_thumbnail();
    });
    return btn_sizer;
}

wxBoxSizer *ObjColorPanel::create_extruder_icon_and_rgba_sizer(wxWindow *parent, int id, const wxColour &color)
{
    auto icon_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxButton *icon       = new wxButton(parent, wxID_ANY, {}, wxDefaultPosition, ICON_SIZE, wxBORDER_NONE | wxBU_AUTODRAW);
    icon->SetBitmap(*get_extruder_color_icon(color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString(), std::to_string(id + 1), FromDIP(16), FromDIP(16)));
    icon->SetCanFocus(false);
    m_extruder_icon_list.emplace_back(icon);
    icon_sizer->Add(icon, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL, 0); // wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM
    //icon_sizer->AddSpacer(FromDIP(5));
    return icon_sizer;
}

std::string ObjColorPanel::get_color_str(const wxColour &color) {
    std::string str = ("R:" + std::to_string(color.Red()) +
                          std::string(" G:") + std::to_string(color.Green()) +
                          std::string(" B:") + std::to_string(color.Blue()) +
                          std::string(" A:") + std::to_string(color.Alpha()));
    return str;
}

ComboBox *ObjColorPanel::CreateEditorCtrl(wxWindow *parent, int id) // wxRect labelRect,, const wxVariant &value
{
    std::vector<wxBitmap *> icons = get_extruder_color_icons();
    const double            em          = Slic3r::GUI::wxGetApp().em_unit();
    bool                    thin_icon   = false;
    const int               icon_width  = lround((thin_icon ? 2 : 4.4) * em);
    const int               icon_height = lround(2 * em);
    m_combox_icon_width                 = icon_width;
    m_combox_icon_height                = icon_height;

    icons.insert(icons.begin(), get_extruder_color_icon(g_undefined_color_in_obj.GetAsString(wxC2S_HTML_SYNTAX).ToStdString(), std::to_string(-1), icon_width, icon_height));
    if (icons.empty())
        return nullptr;

    ::ComboBox *c_editor = new ::ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(m_combox_width), -1), 0, nullptr,
                                          wxCB_READONLY | CB_NO_DROP_ICON | CB_NO_TEXT);
    c_editor->SetMinSize(wxSize(FromDIP(m_combox_width), -1));
    c_editor->SetMaxSize(wxSize(FromDIP(m_combox_width), -1));
    c_editor->GetDropDown().SetUseContentWidth(false);
    for (size_t i = 0; i < icons.size(); i++) {
        c_editor->Append(wxString::Format("%d", i), *icons[i]);
        if (i == 0) {
            c_editor->SetItemTooltip(i, g_undefined_color_in_obj.GetAsString(wxC2S_HTML_SYNTAX));
        } else {
            c_editor->SetItemTooltip(i, m_colours[i-1].GetAsString(wxC2S_HTML_SYNTAX));
        }
    }
    c_editor->SetSelection(0);
    c_editor->SetName(wxString::Format("%d", id));
    c_editor->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &evt) {
        auto *com_box = static_cast<ComboBox *>(evt.GetEventObject());
        int   i       = atoi(com_box->GetName().c_str());
        if (i < m_cluster_map_filaments.size()) {
            m_cluster_map_filaments[i] = com_box->GetSelection();
            m_new_add_colors[i]        = com_box->GetItemTooltip(com_box->GetSelection());
            deal_thumbnail();
        }
        evt.StopPropagation();
    });
    return c_editor;
}

void ObjColorPanel::deal_approximate_match_btn()
{
    m_warning_text->SetLabelText("");
    if (m_result_icon_list.size() == 0) { return; }
    auto map_count = m_result_icon_list[0]->bitmap_combox->GetCount() -1;
    if (map_count < 1) { return; }
    for (size_t i = 0; i < m_cluster_colours.size(); i++) {
        auto    c = m_cluster_colours[i];
        std::vector<ColorDistValue> color_dists;
        color_dists.resize(map_count);
        for (size_t j = 0; j < map_count; j++) {
            auto tip_color       = m_result_icon_list[0]->bitmap_combox->GetItemTooltip(j+1);
            wxColour candidate_c(tip_color);
            color_dists[j].distance = calc_color_distance(c, candidate_c);
            color_dists[j].id = j + 1;
        }
        std::sort(color_dists.begin(), color_dists.end(), [](ColorDistValue &a, ColorDistValue& b) {
            return a.distance < b.distance;
            });
        auto new_index= color_dists[0].id;
        m_result_icon_list[i]->bitmap_combox->SetSelection(new_index);
        m_new_add_colors[i]        = m_result_icon_list[i]->bitmap_combox->GetItemTooltip(new_index);
        m_cluster_map_filaments[i] = new_index;
    }
}

void ObjColorPanel::show_sizer(wxSizer *sizer, bool show)
{
    wxSizerItemList items = sizer->GetChildren();
    for (wxSizerItemList::iterator it = items.begin(); it != items.end(); ++it) {
        wxSizerItem *item   = *it;
        if (wxWindow *window = item->GetWindow()) {
            window->Show(show);
        }
        if (wxSizer *son_sizer = item->GetSizer()) {
            show_sizer(son_sizer, show);
        }
    }
}


void ObjColorPanel::draw_new_table()
{
    auto cluster_count = m_cluster_colours.size();
    auto col       = 3;
    auto row       = cluster_count / col;
    auto remainder = cluster_count % col;
    if (remainder > 0) {
        row++;
    }
    int old_row = 0;
    bool first_draw = false;
    if (m_new_grid_sizer) {
        old_row = m_new_grid_sizer->GetRows();
        m_new_grid_sizer->SetRows(row);
    }
    else {
        first_draw       = true;
        m_new_grid_sizer = new wxGridSizer(row, 1, 1, 3); //(int rows, int cols, int vgap, int hgap );
        m_scrolledWindow->SetSizer(m_new_grid_sizer);
    }
    if (!first_draw) {
        for (size_t i = old_row; i < row; i++) { show_sizer(m_row_sizer_list[i], true); }
        for (size_t i = row; i < old_row; i++) { show_sizer(m_row_sizer_list[i], false); }
        for (size_t i = 0; i < cluster_count; i++) { show_sizer(m_row_col_boxsizer_list[i], true); }
        for (size_t i = cluster_count; i < m_color_num_recommend; i++) { show_sizer(m_row_col_boxsizer_list[i], false); }
        for (size_t ii = 0; ii < row; ii++) {
            for (size_t j = 0; j < col; j++) {
                auto id = ii * col + j;
                if (id >= cluster_count) {
                    break;
                }
                auto color = m_cluster_colours[id];
                m_color_cluster_icon_list[id]->SetBitmap(*get_extruder_color_icon(color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString(), "", FromDIP(16), FromDIP(16)));
            }
        }
    }
    else {//first draw
        for (size_t ii = 0; ii < row; ii++) {
            wxPanel *row_panel = new wxPanel(m_scrolledWindow);
            //row_panel->SetBackgroundColour(ii % 2 == 0 ? *wxWHITE : wxColour(238, 238, 238));
            auto row_sizer = new wxGridSizer(1, col, 1, 1);
            row_panel->SetSizer(row_sizer);

            row_panel->SetMinSize(wxSize(FromDIP(PANEL_WIDTH), -1));
            row_panel->SetMaxSize(wxSize(FromDIP(PANEL_WIDTH), -1));
            for (size_t j = 0; j < col; j++) {
                auto id = ii * col + j;
                if (id >= cluster_count) {
                    break;
                }
                auto cluster_color_icon_sizer = create_color_icon_map_rgba_sizer(row_panel, id, m_cluster_colours[id]);
                m_row_col_boxsizer_list.emplace_back(cluster_color_icon_sizer);
                row_sizer->Add(cluster_color_icon_sizer, 0, wxALIGN_CENTER | wxALIGN_CENTER_VERTICAL, 0);
            }
            m_row_sizer_list.emplace_back(row_sizer);
            m_new_grid_sizer->Add(row_panel, 0, wxALIGN_LEFT | wxALL, FromDIP(HEADER_BORDER));
        }
    }
    m_new_grid_sizer->Layout();
    auto height = m_new_grid_sizer->GetMinSize().GetY();
    if (row <= 4 || height < FromDIP(185)) {
        m_scrolledWindow->SetMinSize(wxSize(-1, height));
    } else {
        m_scrolledWindow->SetMinSize(wxSize(-1, FromDIP(185)));
        m_scrolledWindow->SetMaxSize(wxSize(-1, FromDIP(185)));
    }
    m_scrolledWindow->FitInside();
    if (!first_draw) {
        do_layout_callback();
    }
}

void ObjColorPanel::update_new_add_final_colors()
{
    m_new_add_final_colors = m_new_add_colors;
    if (!m_cluster_map_filaments.empty()) {
        m_max_filament_index = *std::max_element(m_cluster_map_filaments.begin(), m_cluster_map_filaments.end());
    } else {
        m_max_filament_index = 0;
    }

    if (m_max_filament_index <= m_colours.size()) { // Fix 20240904
        m_new_add_final_colors.clear();
    }
    else {
        m_new_add_final_colors.resize(m_max_filament_index - m_colours.size());
        for (int ii = m_colours.size() ; ii < m_max_filament_index; ii++) {
            for (int j = 0; j < m_cluster_map_filaments.size(); j++) {
                if (m_cluster_map_filaments[j] == (ii+ 1) && j < m_new_add_colors.size()) {
                    auto index                = ii - m_colours.size();
                    if (index < m_new_add_final_colors.size()) {
                        m_new_add_final_colors[index] = m_new_add_colors[j];
                    }
                }
            }
        }
    }
    if (m_new_add_final_colors.size() > 0) {
        m_is_add_filament = true;
    }
}

void ObjColorPanel::deal_algo(char cluster_number, bool redraw_ui)
{
    if (m_last_cluster_number == cluster_number) {
        return;
    }
    wxBusyCursor cursor;
    m_last_cluster_number = cluster_number;
    obj_color_deal_algo(m_input_colors, m_cluster_colors_from_algo, m_cluster_labels_from_algo, cluster_number,g_max_color);

    m_cluster_colours.clear();
    m_cluster_colours.reserve(m_cluster_colors_from_algo.size());
    for (size_t i = 0; i < m_cluster_colors_from_algo.size(); i++) {
        m_cluster_colours.emplace_back(convert_to_wxColour(m_cluster_colors_from_algo[i]));
    }
    if (m_cluster_colours.size() == 0) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",m_cluster_colours.size() = 0\n";
        return;
    }
    m_cluster_map_filaments.resize(m_cluster_colors_from_algo.size());
    m_new_add_colors.resize(m_cluster_map_filaments.size());
    m_color_cluster_num_by_algo = m_cluster_colors_from_algo.size();
    if (cluster_number == -1) {
        m_color_num_recommend = m_color_cluster_num_by_algo;
    }
    //redraw ui
    if (redraw_ui) {
        deal_reset_btn();
        draw_new_table();
        deal_default_strategy();
        deal_thumbnail();
    }
}

void ObjColorPanel::deal_default_strategy()
{
    bool is_exceed = deal_add_btn();
    if (!is_exceed) {
        deal_approximate_match_btn();
    }
    m_warning_text->SetLabelText(_L("Note") + ": " + _L("The color has been selected, you can choose OK \n to continue or manually adjust it."));
}

void ObjColorPanel::deal_thumbnail() {
    update_filament_ids();
    // generate model volume
    if (m_obj_in_out.deal_vertex_color) {
        if (m_obj_in_out.filament_ids.size() > 0) {
            m_deal_thumbnail_flag = Model::obj_import_vertex_color_deal(m_obj_in_out.filament_ids, m_obj_in_out.first_extruder_id, m_obj_in_out.model);
        }
    } else {
        if (m_obj_in_out.filament_ids.size() > 0) {
            m_deal_thumbnail_flag = Model::obj_import_face_color_deal(m_obj_in_out.filament_ids, m_obj_in_out.first_extruder_id, m_obj_in_out.model);
        }
    }
    generate_thumbnail();
}

void ObjColorPanel::generate_thumbnail()
{
    if (m_deal_thumbnail_flag && m_obj_in_out.model->objects.size() == 1) {
        std::vector<Slic3r::ColorRGBA> colors = GUI::wxGetApp().plater()->get_extruders_colors();
        for (size_t i = 0; i < m_new_add_colors.size(); i++) {
            Slic3r::ColorRGBA temp_color;
            temp_color[0] = m_new_add_colors[i].Red() / 255.f;
            temp_color[1] = m_new_add_colors[i].Green() / 255.f;
            temp_color[2] = m_new_add_colors[i].Blue() / 255.f;
            temp_color[3] = m_new_add_colors[i].Alpha() / 255.f;
            colors.emplace_back(temp_color);
        }

            auto mo = m_obj_in_out.model->objects[0];
            wxGetApp().plater()->update_obj_preview_thumbnail(mo, 0, 0, colors, (int) m_camera_view_angle_type);
            // get thumbnail image
            PartPlate *plate = wxGetApp().plater()->get_partplate_list().get_plate(0);
            auto &     data  = plate->obj_preview_thumbnail_data;
            if (data.is_valid()) {
                wxImage image(data.width, data.height);
                image.InitAlpha();
                for (unsigned int r = 0; r < data.height; ++r) {
                    unsigned int rr = (data.height - 1 - r) * data.width;
                    for (unsigned int c = 0; c < data.width; ++c) {
                        unsigned char *px = (unsigned char *) data.pixels.data() + 4 * (rr + c);
                        image.SetRGB((int) c, (int) r, px[0], px[1], px[2]);
                        image.SetAlpha((int) c, (int) r, px[3]);
                    }
                }
                image = image.Rescale(FromDIP(IMAGE_SIZE_WIDTH), FromDIP(IMAGE_SIZE_WIDTH));
                m_image_button->SetBitmap(image);
            }

    }
    else {
#ifdef _WIN32
        __debugbreak();
#endif
    }
}

void ObjColorPanel::set_view_angle_type(int value)
{
    if (value >= 0) {
        m_camera_view_angle_type = (Slic3r::GUI::Camera::ViewAngleType) value;
        generate_thumbnail();
    }
}


void ObjColorPanel::clear_instance_and_revert_offset()
{
    auto mo = m_obj_in_out.model->objects[0];
    mo->clear_instances();
    auto mv  = mo->volumes[0];
    auto box = mo->bounding_box_exact();
    if (!m_thumbnail_offset.isApprox(Slic3r::Vec3d::Zero())) {
        mv->translate(-m_thumbnail_offset);
    }
}

bool ObjColorPanel::do_show(bool show) {
    if (show) {
        return true;
    } else {
        return false;
    }
}

bool ObjColorPanel::deal_add_btn()
{
    if (m_colours.size() > g_max_color) { return false; }
    deal_reset_btn();
    std::vector<wxBitmap *> new_icons;
    auto  new_color_size = m_cluster_colors_from_algo.size();
    new_icons.reserve(new_color_size);
    m_new_add_colors.clear();
    m_new_add_colors.reserve(new_color_size);
    int new_index = m_colours.size() + 1;
    bool is_exceed = false;
    for (size_t i = 0; i < new_color_size; i++) {
        if (m_colours.size() + new_icons.size() >= g_max_color) {
            is_exceed = true;
            break;
        }
        wxColour cur_color = convert_to_wxColour(m_cluster_colors_from_algo[i]);
        m_new_add_colors.emplace_back(cur_color);
        new_icons.emplace_back(get_extruder_color_icon(cur_color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString(),
                std::to_string(new_index), m_combox_icon_width, m_combox_icon_height));
        new_index++;
    }
    new_index = m_colours.size() + 1;
    for (size_t i = 0; i < m_result_icon_list.size(); i++) {
        auto item = m_result_icon_list[i];
        for (size_t k = 0; k < new_icons.size(); k++) {
            item->bitmap_combox->Append(wxString::Format("%d", item->bitmap_combox->GetCount()), *new_icons[k]);
            item->bitmap_combox->SetItemTooltip(item->bitmap_combox->GetCount() -1,m_new_add_colors[k].GetAsString(wxC2S_HTML_SYNTAX));
        }
        item->bitmap_combox->SetSelection(new_index);
        m_cluster_map_filaments[i] = new_index;
        new_index++;
    }
    if (is_exceed) {
        deal_approximate_match_btn();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ",Waring:The count of newly added and \n current extruders exceeds 32.";
        return true;
    }
    return false;
}

void ObjColorPanel::deal_reset_btn()
{
    for (auto item : m_result_icon_list) {
        // delete redundant bitmap
        while (item->bitmap_combox->GetCount() > m_colours.size()+ 1) {
            item->bitmap_combox->DeleteOneItem(item->bitmap_combox->GetCount() - 1);
        }
        item->bitmap_combox->SetSelection(0);
    }
    for (int i = 0; i < m_new_add_colors.size(); i++) {
        m_new_add_colors[i] = g_undefined_color_in_obj;
    }
    m_warning_text->SetLabelText("");
}

wxBoxSizer *ObjColorPanel::create_color_icon_map_rgba_sizer(wxWindow *parent, int id, const wxColour &color)
{
    auto icon_sizer = new wxBoxSizer(wxHORIZONTAL);
    //icon_sizer->AddSpacer(FromDIP(40));
    wxButton *icon = new wxButton(parent, wxID_ANY, {}, wxDefaultPosition, ICON_SIZE, wxBORDER_NONE | wxBU_AUTODRAW);
    icon->SetBitmap(*get_extruder_color_icon(color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString(), "", FromDIP(16), FromDIP(16)));
    icon->SetCanFocus(false);
    m_color_cluster_icon_list.emplace_back(icon);
    icon_sizer->Add(icon, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL, 0); // wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM
    icon_sizer->AddSpacer(FromDIP(10));

    wxStaticText *map_text = new wxStaticText(parent, wxID_ANY, u8"—> ");
    map_text->SetFont(Label::Head_12);
    icon_sizer->Add(map_text, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL, 0);

    for (size_t i = m_result_icon_list.size(); i < id + 1; i++) {
        m_result_icon_list.emplace_back(new ButtonState());
    }
    m_result_icon_list[id]->bitmap_combox = CreateEditorCtrl(parent, id);
    icon_sizer->Add(m_result_icon_list[id]->bitmap_combox, 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL, 0);
    return icon_sizer;
}
