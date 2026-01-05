#include "StepMeshDialog.hpp"

#include <thread>
#include <wx/event.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/dcmemory.h>
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/TextInput.hpp"
#include "Widgets/DialogButtons.hpp"
#include <chrono>

using namespace Slic3r;
using namespace Slic3r::GUI;

static int _scale(const int val) { return val * Slic3r::GUI::wxGetApp().em_unit() / 10; }
static int _ITEM_WIDTH() { return _scale(30); }
#define MIN_DIALOG_WIDTH        FromDIP(400)
#define SLIDER_WIDTH            FromDIP(200)
#define SLIDER_HEIGHT           FromDIP(25)
#define TEXT_CTRL_WIDTH         FromDIP(70)
#define BUTTON_SIZE             wxSize(FromDIP(58), FromDIP(24))
#define BUTTON_BORDER           FromDIP(int(400 - 58 * 2) / 8)
#define SLIDER_SCALE(val)       ((val) / 0.001)
#define SLIDER_UNSCALE(val)     ((val) * 0.001)
#define SLIDER_SCALE_10(val)    ((val) / 0.01)
#define SLIDER_UNSCALE_10(val)  ((val) * 0.01)
#define LEFT_RIGHT_PADING       FromDIP(20)
#define FONT_COLOR              wxColour("#6B6B6B")

wxDEFINE_EVENT(wxEVT_THREAD_DONE, wxCommandEvent);

class CenteredStaticText : public wxStaticText
{
public:
    CenteredStaticText(wxWindow* parent, wxWindowID id, const wxString& label, const wxPoint& position, const wxSize& size = wxDefaultSize, long style = 0)
        : wxStaticText(parent, id, label, position, size, style) {
        CenterOnPosition(position);
    }

    void CenterOnPosition(const wxPoint& position) {
        int textWidth, textHeight;
        GetTextExtent(GetLabel(), &textWidth, &textHeight);
        int x = position.x - textWidth / 2;
        int y = position.y - textHeight / 2;
        SetPosition(wxPoint(x, y));
    }
};

void StepMeshDialog::on_dpi_changed(const wxRect& suggested_rect) {
};

bool StepMeshDialog:: validate_number_range(const wxString& value, double min, double max) {
    double num = 0.0;
    if (value.IsEmpty()) {
        return false;
    }
    try {
        if (!value.ToDouble(&num)) {
            return false;
        }
    } catch (...) {
        return false;
    }

    return (num >= min && num <= max);
}

StepMeshDialog::StepMeshDialog(wxWindow* parent, Slic3r::Step& file, double linear_init, double angle_init)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe),
                wxID_ANY,
                _(L("Step file import parameters")),
                wxDefaultPosition,
                wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE /* | wxRESIZE_BORDER*/), m_file(file)
{
    m_linear_last = wxString::Format("%.3f", linear_init);
    m_angle_last = wxString::Format("%.2f", angle_init);

    Bind(wxEVT_THREAD_DONE, &StepMeshDialog::on_task_done, this);

    SetBackgroundColour(*wxWHITE);

    wxBoxSizer* bSizer = new wxBoxSizer(wxVERTICAL);
    bSizer->SetMinSize(wxSize(MIN_DIALOG_WIDTH, -1));

    auto  image_bitmap = create_scaled_bitmap("step_mesh_info", this, FromDIP(120));

    // wxPanel* overlay_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, image_bitmap.GetSize(), wxTAB_TRAVERSAL);
    // overlay_panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
    // wxStaticBitmap *image = new wxStaticBitmap(overlay_panel, wxID_ANY, image_bitmap, wxDefaultPosition, overlay_panel->GetSize(), 0);

    // CenteredStaticText* text_1 = new CenteredStaticText (overlay_panel, wxID_ANY, _L("Smooth"),
    //                                                       wxPoint(overlay_panel->GetSize().GetWidth() / 6,
    //                                                               overlay_panel->GetSize().GetHeight() / 2));
    // CenteredStaticText* text_2 = new CenteredStaticText(overlay_panel, wxID_ANY, _L("Rough"),
    //                                                      wxPoint(overlay_panel->GetSize().GetWidth() * 5 / 6,
    //                                                              overlay_panel->GetSize().GetHeight() / 2));
    // CenteredStaticText* text_3 = new CenteredStaticText(overlay_panel, wxID_ANY, _L("Reduce Linear"),
    //                                                     wxPoint(overlay_panel->GetSize().GetWidth() / 2,
    //                                                             overlay_panel->GetSize().GetHeight() * 1.3 / 3));
    // CenteredStaticText* text_4 = new CenteredStaticText(overlay_panel, wxID_ANY, _L("Reduce Angle"),
    //                                                     wxPoint(overlay_panel->GetSize().GetWidth() / 2,
    //                                                             overlay_panel->GetSize().GetHeight() * 2 / 3));
    // CenteredStaticText* text_5 = new CenteredStaticText(overlay_panel, wxID_ANY, _L("More faces"),
    //                                                     wxPoint(overlay_panel->GetSize().GetWidth() / 6,
    //                                                             overlay_panel->GetSize().GetHeight() * 2.8 / 3));
    // CenteredStaticText* text_6 = new CenteredStaticText(overlay_panel, wxID_ANY, _L("Fewer faces"),
    //                                                     wxPoint(overlay_panel->GetSize().GetWidth() * 5 / 6,
    //                                                             overlay_panel->GetSize().GetHeight() * 2.8 / 3));

    // bSizer->Add(overlay_panel, 0, wxALIGN_CENTER | wxALL, 10);

    wxBoxSizer* tips_sizer = new wxBoxSizer(wxVERTICAL);
    wxStaticText* info = new wxStaticText(this, wxID_ANY, _L("Smaller linear and angular deflections result in higher-quality transformations but increase the processing time."));
    info->SetForegroundColour(StateColor::darkModeColorFor(FONT_COLOR));

    // ORCA standardized HyperLink
    HyperLink *tips = new HyperLink(this, _L("Wiki Guide"), "https://www.orcaslicer.com/wiki/stl-transformation");
    tips->SetFont(::Label::Body_12);
 
    info->Wrap(FromDIP(400));
    tips_sizer->Add(info, 0, wxALIGN_LEFT);
    tips_sizer->Add(tips, 0, wxALIGN_LEFT);
    bSizer->Add(tips_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, LEFT_RIGHT_PADING);

    wxBoxSizer* linear_sizer = new wxBoxSizer(wxHORIZONTAL);
    //linear_sizer->SetMinSize(wxSize(MIN_DIALOG_WIDTH, -1));
    wxStaticText* linear_title = new wxStaticText(this,
                                                  wxID_ANY, _L("Linear Deflection") + ": ");
    linear_title->SetForegroundColour(StateColor::darkModeColorFor(FONT_COLOR));
    linear_sizer->Add(linear_title, 0, wxALIGN_LEFT);
    linear_sizer->AddStretchSpacer(1);
    wxSlider* linear_slider = new wxSlider(this, wxID_ANY,
                                           SLIDER_SCALE(get_linear_defletion()),
                                           1, 100, wxDefaultPosition,
                                           wxSize(SLIDER_WIDTH, SLIDER_HEIGHT),
                                           wxSL_HORIZONTAL);
    linear_sizer->Add(linear_slider, 0, wxALIGN_RIGHT | wxLEFT, FromDIP(5));

    auto linear_input = new ::TextInput(this, m_linear_last, wxEmptyString, wxEmptyString, wxDefaultPosition, wxSize(TEXT_CTRL_WIDTH, -1), wxTE_CENTER);
    linear_input->GetTextCtrl()->SetFont(Label::Body_12);
    linear_input->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    linear_sizer->Add(linear_input, 0, wxALIGN_RIGHT | wxLEFT, FromDIP(5));
    linear_input->Bind(wxEVT_KILL_FOCUS, ([this, linear_input](wxFocusEvent& e) {
        wxString value = linear_input->GetTextCtrl()->GetValue();
        if (validate_number_range(value, 0.001, 0.1)) {
            m_linear_last = value;
            update_mesh_number_text();
        } else {
            MessageDialog msg_dlg(nullptr, _L("Please input a valid value (0.001 < linear deflection < 0.1)"), wxEmptyString, wxICON_WARNING | wxOK);
            msg_dlg.ShowModal();
            linear_input->GetTextCtrl()->SetValue(m_linear_last);
        }
        e.Skip();
    }));
    // textctrl bind slider
    linear_input->Bind(wxEVT_TEXT, ([this, linear_slider, linear_input](wxCommandEvent& e) {
        double slider_value_long;
        int slider_value;
        wxString value = linear_input->GetTextCtrl()->GetValue();
        if (value.ToDouble(&slider_value_long)) {
            slider_value = SLIDER_SCALE(slider_value_long);
            if (slider_value >= linear_slider->GetMin() && slider_value <= linear_slider->GetMax()) {
                linear_slider->SetValue(slider_value);
            }
        }
    }));
    linear_slider->Bind(wxEVT_SLIDER, ([this, linear_slider, linear_input](wxCommandEvent& e) {
        double slider_value = SLIDER_UNSCALE(linear_slider->GetValue());
        linear_input->GetTextCtrl()->SetValue(wxString::Format("%.3f", slider_value));
        m_linear_last = wxString::Format("%.3f", slider_value);
    }));
    linear_slider->Bind(wxEVT_LEFT_UP, ([this](wxMouseEvent& e) {
        update_mesh_number_text();
        e.Skip();
    }));

    bSizer->Add(linear_sizer, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, LEFT_RIGHT_PADING);

    wxBoxSizer* angle_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText* angle_title = new wxStaticText(this,
                                                  wxID_ANY, _L("Angle Deflection") + ": ");
    angle_title->SetForegroundColour(StateColor::darkModeColorFor(FONT_COLOR));
    angle_sizer->Add(angle_title, 0, wxALIGN_LEFT);
    angle_sizer->AddStretchSpacer(1);
    wxSlider* angle_slider = new wxSlider(this, wxID_ANY,
                                           SLIDER_SCALE_10(get_angle_defletion()),
                                           1, 100, wxDefaultPosition,
                                           wxSize(SLIDER_WIDTH, SLIDER_HEIGHT),
                                           wxSL_HORIZONTAL);
    angle_sizer->Add(angle_slider, 0, wxALIGN_RIGHT | wxLEFT, FromDIP(5));

    auto angle_input = new ::TextInput(this, m_angle_last, wxEmptyString, wxEmptyString, wxDefaultPosition, wxSize(TEXT_CTRL_WIDTH, -1), wxTE_CENTER);
    angle_input->GetTextCtrl()->SetFont(Label::Body_12);
    angle_input->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    angle_sizer->Add(angle_input, 0, wxALIGN_RIGHT | wxLEFT, FromDIP(5));
    angle_input->Bind(wxEVT_KILL_FOCUS, ([this, angle_input](wxFocusEvent& e) {
        wxString value = angle_input->GetTextCtrl()->GetValue();
        if (validate_number_range(value, 0.01, 1)) {
            m_angle_last = value;
            update_mesh_number_text();
        } else {
            MessageDialog msg_dlg(nullptr, _L("Please input a valid value (0.01 < angle deflection < 1.0)"), wxEmptyString, wxICON_WARNING | wxOK);
            msg_dlg.ShowModal();
            angle_input->GetTextCtrl()->SetValue(m_angle_last);
        }
        e.Skip();
    }));
    // textctrl bind slider
    angle_input->Bind(wxEVT_TEXT, ([this, angle_slider, angle_input](wxCommandEvent& e) {
        double slider_value_long;
        int slider_value;
        wxString value = angle_input->GetTextCtrl()->GetValue();
        if (value.ToDouble(&slider_value_long)) {
            slider_value = SLIDER_SCALE_10(slider_value_long);
            if (slider_value >= angle_slider->GetMin() && slider_value <= angle_slider->GetMax()) {
                angle_slider->SetValue(slider_value);
            }
        }
    }));

    angle_slider->Bind(wxEVT_SLIDER, ([this, angle_slider, angle_input](wxCommandEvent& e) {
        double slider_value = SLIDER_UNSCALE_10(angle_slider->GetValue());
        angle_input->GetTextCtrl()->SetValue(wxString::Format("%.2f", slider_value));
        m_angle_last = wxString::Format("%.2f", slider_value);
    }));
    angle_slider->Bind(wxEVT_LEFT_UP, ([this](wxMouseEvent& e) {
        update_mesh_number_text();
        e.Skip();
    }));

    bSizer->Add(angle_sizer, 1, wxEXPAND | wxLEFT | wxRIGHT, LEFT_RIGHT_PADING);

    wxBoxSizer* check_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_split_compound_checkbox = new wxCheckBox(this, wxID_ANY, _L("Split compound and compsolid into multiple objects"), wxDefaultPosition, wxDefaultSize, 0);
    m_split_compound_checkbox->SetForegroundColour(StateColor::darkModeColorFor(FONT_COLOR));
    m_split_compound_checkbox->SetValue(wxGetApp().app_config->get_bool("is_split_compound"));
    check_sizer->Add(m_split_compound_checkbox, 0, wxALIGN_LEFT);
    bSizer->Add(check_sizer, 1, wxEXPAND | wxLEFT | wxRIGHT, LEFT_RIGHT_PADING);

    wxBoxSizer* mesh_face_number_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxStaticText *mesh_face_number_title = new wxStaticText(this, wxID_ANY, _L("Number of triangular facets") + ": ");
    mesh_face_number_title->SetForegroundColour(StateColor::darkModeColorFor(FONT_COLOR));
    mesh_face_number_text = new wxStaticText(this, wxID_ANY, "0");
    mesh_face_number_text->SetForegroundColour(StateColor::darkModeColorFor(FONT_COLOR));
    mesh_face_number_text->SetMinSize(wxSize(FromDIP(150), -1));
    mesh_face_number_sizer->Add(mesh_face_number_title, 0, wxALIGN_LEFT);
    mesh_face_number_sizer->Add(mesh_face_number_text, 0, wxALIGN_LEFT);
    bSizer->Add(mesh_face_number_sizer, 1, wxEXPAND | wxLEFT | wxRIGHT, LEFT_RIGHT_PADING);

    wxBoxSizer* bSizer_button = new wxBoxSizer(wxHORIZONTAL);
    bSizer_button->SetMinSize(wxSize(FromDIP(100), -1));
    m_checkbox = new wxCheckBox(this, wxID_ANY, _L("Don't show again"), wxDefaultPosition, wxDefaultSize, 0);
    m_checkbox->SetForegroundColour(StateColor::darkModeColorFor(FONT_COLOR));
    bSizer_button->Add(m_checkbox, 0, wxALIGN_LEFT | wxLEFT | wxALIGN_CENTER_VERTICAL, LEFT_RIGHT_PADING);
    bSizer_button->AddStretchSpacer(1);

    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});

    bSizer_button->Add(dlg_btns, 0, wxEXPAND);

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this, angle_input, linear_input](wxCommandEvent& e) {
        stop_task();
        if (validate_number_range(angle_input->GetTextCtrl()->GetValue(), 0.01, 1) &&
            validate_number_range(linear_input->GetTextCtrl()->GetValue(), 0.001, 0.1)) {
            if (m_checkbox->IsChecked()) {
                wxGetApp().app_config->set_bool("enable_step_mesh_setting", false);
            }
            wxGetApp().app_config->set_bool("is_split_compound", m_split_compound_checkbox->GetValue());
            wxGetApp().app_config->set("linear_defletion", float_to_string_decimal_point(get_linear_defletion(), 3));
            wxGetApp().app_config->set("angle_defletion", float_to_string_decimal_point(get_angle_defletion(), 2));

            EndModal(wxID_OK);
        }
        SetFocusIgnoringChildren();
    });

    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
        stop_task();
        EndModal(wxID_CANCEL);
    });

    bSizer->Add(bSizer_button, 1, wxEXPAND);

    this->SetSizer(bSizer);
    update_mesh_number_text();
    this->Layout();
    bSizer->Fit(this);

    this->Bind(wxEVT_LEFT_DOWN, [this](auto& e) {
        SetFocusIgnoringChildren();
    });
    mesh_face_number_text->Bind(wxEVT_LEFT_DOWN, [this](auto& e) {
        SetFocusIgnoringChildren();
    });

    this->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) {
        stop_task();
        EndModal(wxID_CANCEL);
    });

    wxGetApp().UpdateDlgDarkUI(this);
}

StepMeshDialog::~StepMeshDialog()
{
    stop_task();
}

void StepMeshDialog::on_task_done(wxCommandEvent& event)
{
    wxString text = event.GetString();
    mesh_face_number_text->SetLabel(text);
    if(m_task) {
        if (m_task->joinable()) {
            m_task->join();
            delete m_task;
            m_task = nullptr;
        }
    }
}

void StepMeshDialog::stop_task()
{
    if(m_task) {
        m_file.m_stop_mesh.store(true);
        if (m_task->joinable()) {
            m_task->join();
            delete m_task;
            m_task = nullptr;
        }
        m_file.m_stop_mesh.store(false);
    }

}

void StepMeshDialog::update_mesh_number_text()
{
    if ((m_last_linear == get_linear_defletion()) && (m_last_angle == get_angle_defletion()) && (m_mesh_number != 0))
        return;
    wxString newText = wxString::Format(_L("Calculating, please wait..."));
    mesh_face_number_text->SetLabel(newText);
    stop_task();
    if (!m_task) {
        m_task = new boost::thread(Slic3r::create_thread([this]() -> void {
            m_mesh_number = m_file.get_triangle_num(get_linear_defletion(), get_angle_defletion());
            if (m_mesh_number != 0) {
                wxString number_text = wxString::Format("%d", m_mesh_number);
                wxCommandEvent event(wxEVT_THREAD_DONE);
                event.SetString(number_text);
                wxPostEvent(this, event);
                m_last_linear = get_linear_defletion();
                m_last_angle  = get_angle_defletion();
            }
        }));
    }
}