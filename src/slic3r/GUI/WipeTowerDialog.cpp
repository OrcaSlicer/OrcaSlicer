#include <algorithm>
#include <wx/display.h>
#include <wx/sizer.h>
#include "libslic3r/FlushVolCalc.hpp"
#include "WipeTowerDialog.hpp"
#include "GUI.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "MsgDialog.hpp"
#include "format.hpp"
#include "libslic3r/Color.hpp"
#include "Widgets/StaticLine.hpp"
#include "Widgets/DialogButtons.hpp"
#include "libslic3r/Config.hpp"
#include "Widgets/Label.hpp"
#include "MainFrame.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;

int scale(const int val) { return val * Slic3r::GUI::wxGetApp().em_unit() / 10; }

static void update_ui(wxWindow* window)
{
    Slic3r::GUI::wxGetApp().UpdateDarkUI(window);
}

RammingDialog::RammingDialog(wxWindow* parent,const std::string& parameters)
: wxDialog(parent, wxID_ANY, _(L("Ramming customization")), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE/* | wxRESIZE_BORDER*/)
{
    SetBackgroundColour(*wxWHITE);
    m_panel_ramming  = new RammingPanel(this,parameters);
    m_panel_ramming->Show(true);

    auto main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->Add(m_panel_ramming, 1, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, 5);
    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});
    main_sizer->Add(dlg_btns, 0, wxEXPAND);
    SetSizer(main_sizer);
    main_sizer->SetSizeHints(this);

    this->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& e) { EndModal(wxCANCEL); });

    this->Bind(wxEVT_BUTTON,[this](wxCommandEvent&) {
        m_output_data = m_panel_ramming->get_parameters();
        EndModal(wxID_OK);
        },wxID_OK);

    wxGetApp().UpdateDlgDarkUI(this);
    this->Show();

    Slic3r::GUI::MessageDialog dlg(this, _(L("Ramming denotes the rapid extrusion just before a tool change in a single-extruder MM printer. Its purpose is to "
        "properly shape the end of the unloaded filament so it does not prevent insertion of the new filament and can itself "
        "be reinserted later. This phase is important and different materials can require different extrusion speeds to get "
        "the good shape. For this reason, the extrusion rates during ramming are adjustable.\n\nThis is an expert-level "
        "setting, incorrect adjustment will likely lead to jams, extruder wheel grinding into filament etc.")), _(L("Warning")), wxOK | wxICON_EXCLAMATION);// .ShowModal();
    dlg.ShowModal();
}


#ifdef _WIN32
#define style wxSP_ARROW_KEYS | wxBORDER_SIMPLE
#else 
#define style wxSP_ARROW_KEYS
#endif



RammingPanel::RammingPanel(wxWindow* parent, const std::string& parameters)
: wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize/*,wxPoint(50,50), wxSize(800,350),wxBORDER_RAISED*/)
{
    SetBackgroundColour(*wxWHITE);
    update_ui(this);
	auto sizer_chart = new wxBoxSizer(wxVERTICAL);
	auto sizer_param = new wxBoxSizer(wxVERTICAL);

	std::stringstream stream{ parameters };
	stream >> m_ramming_line_width_multiplicator >> m_ramming_step_multiplicator;
	int ramming_speed_size = 0;
	float dummy = 0.f;
	while (stream >> dummy)
		++ramming_speed_size;
	stream.clear();
	stream.get();

	std::vector<std::pair<float, float>> buttons;
	float x = 0.f;
	float y = 0.f;
	while (stream >> x >> y)
		buttons.push_back(std::make_pair(x, y));

	m_chart = new Chart(this, wxRect(scale(10),scale(10),scale(480),scale(360)), buttons, ramming_speed_size, 0.25f, scale(10));
    update_ui(m_chart);
 	sizer_chart->Add(m_chart, 0, wxALL, 5);

    // Create help text for constant flow rate dragging
    std::string ctrl_str = GUI::shortkey_ctrl_prefix();
    if (!ctrl_str.empty() && ctrl_str.back() == '+') 
        ctrl_str.pop_back(); // Remove trailing '+'
    wxString message = format_wxstr(_L("For constant flow rate, hold %1% while dragging."), ctrl_str);
    Label* label = new Label(this, wxEmptyString);
    wxClientDC dc(label);
    wxString multiline_message;
    label->SetFont(Label::Body_14);
    label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#363636")));
    label->split_lines(dc, scale(470), message, multiline_message);
    label->SetLabel(multiline_message);
    sizer_chart->Add(label, 0, wxEXPAND | wxALL, 5);

    m_widget_time                             = new SpinInput(this, wxEmptyString, _L("ms"), wxDefaultPosition, wxSize(scale(120), -1), wxSP_ARROW_KEYS, 0 , 5000 , 3000, 250);
    m_widget_volume                           = new SpinInput(this, wxEmptyString, _L("mm³"), wxDefaultPosition, wxSize(scale(120), -1), wxSP_ARROW_KEYS, 0 , 10000, 0   );
    m_widget_ramming_line_width_multiplicator = new SpinInput(this, wxEmptyString, "%"  , wxDefaultPosition, wxSize(scale(120), -1), wxSP_ARROW_KEYS, 10, 300  , 100 );
    m_widget_ramming_step_multiplicator       = new SpinInput(this, wxEmptyString, "%"  , wxDefaultPosition, wxSize(scale(120), -1), wxSP_ARROW_KEYS, 10, 300  , 100 );

    auto add_title = [this, sizer_param](wxString label){
        auto title = new StaticLine(this, 0, label);
        title->SetFont(Label::Head_14);
        title->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#363636")));
        sizer_param->Add(title, 0, wxEXPAND | wxBOTTOM, scale(8));
    };

    SetFont(Label::Body_14);
    wxSize col_size;
    for(auto label : {"Time", "Volume", "Width", "Spacing"})
        col_size.IncTo(GetTextExtent(_L(label)));
    col_size = wxSize(col_size.x + scale(30) ,-1);

    auto add_spin = [this, sizer_param, col_size](wxString label, SpinInput* spin){
        spin->Bind(wxEVT_KILL_FOCUS, [this](auto &e) {
            e.SetId(GetId());
            ProcessEventLocally(e);
            e.Skip();
        });
        auto h_sizer = new wxBoxSizer(wxHORIZONTAL);
        auto text = new wxStaticText(this, wxID_ANY, label, wxDefaultPosition, col_size);
        text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#363636")));
        h_sizer->Add(text, 0, wxALIGN_CENTER_VERTICAL);
        h_sizer->Add(spin);
        sizer_param->Add(h_sizer, 0, wxEXPAND | wxBOTTOM, scale(2));
    };

    add_title(_L("Total ramming"));
    add_spin( _L("Time")  , m_widget_time  );
    add_spin( _L("Volume"), m_widget_volume);

    sizer_param->AddSpacer(10);

    add_title(_L("Ramming line"));
    add_spin( _L("Width")  , m_widget_ramming_line_width_multiplicator);
    add_spin( _L("Spacing"), m_widget_ramming_step_multiplicator      );

    m_widget_time->SetValue(int(m_chart->get_time() * 1000));
    m_widget_volume->SetValue(m_chart->get_volume());
    m_widget_volume->Disable();
    m_widget_ramming_line_width_multiplicator->SetValue(m_ramming_line_width_multiplicator);
    m_widget_ramming_step_multiplicator->SetValue(m_ramming_step_multiplicator);

    m_widget_ramming_step_multiplicator->Bind(wxEVT_SPINCTRL,[this](wxCommandEvent&) { line_parameters_changed(); });
    m_widget_ramming_line_width_multiplicator->Bind(wxEVT_SPINCTRL,[this](wxCommandEvent&) { line_parameters_changed(); });

	auto sizer = new wxBoxSizer(wxHORIZONTAL);
	sizer->Add(sizer_chart, 0, wxALL, 5);
	sizer->Add(sizer_param, 0, wxALL, 10);

	sizer->SetSizeHints(this);
	SetSizer(sizer);

    m_widget_time->Bind(wxEVT_SPINCTRL,[this](wxCommandEvent&) {
        m_chart->set_xy_range(m_widget_time->GetValue() * 0.001,-1);
    });
    m_widget_time->Bind(wxEVT_CHAR,[](wxKeyEvent&){});      // do nothing - prevents the user to change the value
    m_widget_volume->Bind(wxEVT_CHAR,[](wxKeyEvent&){});    // do nothing - prevents the user to change the value   
    Bind(EVT_WIPE_TOWER_CHART_CHANGED,[this](wxCommandEvent&) {
        m_widget_volume->SetValue(m_chart->get_volume());
        m_widget_time->SetValue(m_chart->get_time() * 1000);
    });
    Refresh(true); // erase background
}

void RammingPanel::line_parameters_changed() {
    m_ramming_line_width_multiplicator = m_widget_ramming_line_width_multiplicator->GetValue();
    m_ramming_step_multiplicator = m_widget_ramming_step_multiplicator->GetValue();
}

std::string RammingPanel::get_parameters()
{
    std::vector<float> speeds = m_chart->get_ramming_speed(0.25f);
    std::vector<std::pair<float,float>> buttons = m_chart->get_buttons();
    std::stringstream stream;
    stream << m_ramming_line_width_multiplicator << " " << m_ramming_step_multiplicator;
    for (const float& speed_value : speeds)
        stream << " " << speed_value;
    stream << "|";    
    for (const auto& button : buttons)
        stream << " " << button.first << " " << button.second;
    return stream.str();
}

static const float g_min_flush_multiplier = 0.f;
static const float g_max_flush_multiplier = 3.f;

// Extract the num_phys×num_phys top-left block from a flat total×total matrix.
// When there are no mixed filaments (total == num_phys) the function is a no-op
// and returns the input unchanged.
static std::vector<double> extract_physical_sub_matrix(
    const std::vector<double>& full, size_t total, size_t num_phys)
{
    if (num_phys >= total || total == 0)
        return full;
    std::vector<double> phys(num_phys * num_phys, 0.0);
    for (size_t row = 0; row < num_phys; ++row)
        for (size_t col = 0; col < num_phys; ++col)
            phys[row * num_phys + col] = full[row * total + col];
    return phys;
}

// Write the edited num_phys×num_phys sub-matrix back into a copy of
// original_full (total×total), preserving the mixed-slot rows and columns.
static std::vector<double> expand_physical_to_full_matrix(
    const std::vector<double>& phys, const std::vector<double>& original_full,
    size_t total, size_t num_phys)
{
    if (num_phys >= total || total == 0)
        return phys;
    std::vector<double> result(original_full);  // preserve mixed rows/cols
    for (size_t row = 0; row < num_phys; ++row)
        for (size_t col = 0; col < num_phys; ++col)
            result[row * total + col] = phys[row * num_phys + col];
    return result;
}

bool is_flush_config_modified()
{
    const auto                &project_config    = wxGetApp().preset_bundle->project_config;
    const std::vector<double> &config_matrix     = (project_config.option<ConfigOptionFloats>("flush_volumes_matrix"))->values;
    const std::vector<double> &config_multiplier = (project_config.option<ConfigOptionFloats>("flush_multiplier"))->values;

    // Physical filament count (excludes mixed virtual slots).
    const size_t num_phys = static_cast<size_t>(wxGetApp().filaments_cnt());
    const size_t nozzle_num = config_multiplier.size();
    // Total filament count stored per nozzle block.
    const size_t total = (nozzle_num > 0 && !config_matrix.empty())
                             ? static_cast<size_t>(std::round(std::sqrt(config_matrix.size() / nozzle_num)))
                             : num_phys;

    bool has_modify = false;
    for (int i = 0; i < (int)nozzle_num; i++) {
        if (config_multiplier[i] != 1) {
            has_modify = true;
            break;
        }
        // Extract the per-nozzle block from the flat full matrix.
        std::vector<double> nozzle_full(config_matrix.begin() + i * (int)(total * total),
                                        config_matrix.begin() + (i + 1) * (int)(total * total));
        // Only compare the physical sub-matrix; mixed-slot rows/cols are computed.
        const std::vector<double> phys_stored = extract_physical_sub_matrix(nozzle_full, total, num_phys);

        std::vector<std::vector<double>> default_matrix = WipingDialog::CalcFlushingVolumes(i);
        // CalcFlushingVolumes also spans total×total; take physical sub-matrix.
        int def_total = (int)default_matrix.size();
        for (int m = 0; m < (int)num_phys; m++) {
            for (int n = 0; n < (int)num_phys; n++) {
                double def_val = (m < def_total && n < (int)default_matrix[m].size())
                                     ? default_matrix[m][n] * config_multiplier[i]
                                     : 0.0;
                if (phys_stored[m * num_phys + n] != def_val) {
                    has_modify = true;
                    break;
                }
            }
            if (has_modify) break;
        }
        if (has_modify) break;
    }
    return has_modify;
}

void open_flushing_dialog(wxEvtHandler *parent, const wxEvent &event)
{
    auto                      &project_config = wxGetApp().preset_bundle->project_config;

    WipingDialog dlg(static_cast<wxWindow *>(wxGetApp().mainframe));
    dlg.ShowModal();
    if (dlg.GetSubmitFlag()) {
        auto matrix = dlg.GetFlattenMatrix();
        auto flush_multipliers = dlg.GetMultipliers();
        (project_config.option<ConfigOptionFloats>("flush_volumes_matrix"))->values = std::vector<double>(matrix.begin(), matrix.end());
        (project_config.option<ConfigOptionFloats>("flush_multiplier"))->values = std::vector<double>(flush_multipliers.begin(), flush_multipliers.end());
        bool flushing_volume_modify = is_flush_config_modified();
        wxGetApp().sidebar().set_flushing_volume_warning(flushing_volume_modify);
        wxGetApp().preset_bundle->export_selections(*wxGetApp().app_config);
        wxGetApp().plater()->update_project_dirty_from_presets();
        wxPostEvent(parent, event);
    }
}

static std::vector<float> MatrixFlatten(const WipingDialog::VolumeMatrix& matrix) {
    std::vector<float> vec;
    for (auto row_elems : matrix) {
        for (auto elem : row_elems)
            vec.emplace_back(elem);
    }
    return vec;
}

wxString WipingDialog::BuildTableObjStr()
{
    auto full_config = wxGetApp().preset_bundle->full_config();
    auto filament_colors = full_config.option<ConfigOptionStrings>("filament_colour")->values;
    auto flush_multiplier = full_config.option<ConfigOptionFloats>("flush_multiplier")->values;
    int nozzle_num = full_config.option<ConfigOptionFloats>("nozzle_diameter")->values.size();
    auto raw_matrix_data = full_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values;
    auto nozzle_flush_dataset = full_config.option<ConfigOptionIntsNullable>("nozzle_flush_dataset")->values;

    // Physical filament count — the editor only shows the P×P physical block.
    const size_t num_phys = static_cast<size_t>(wxGetApp().filaments_cnt());
    const size_t total    = (num_phys > 0 && !filament_colors.empty())
                                ? filament_colors.size()
                                : num_phys;

    // Per-nozzle full matrices (total×total), stored for expand-on-save.
    std::vector<std::vector<double>> full_matrixs;
    for (int idx = 0; idx < nozzle_num; ++idx) {
        full_matrixs.emplace_back(get_flush_volumes_matrix(raw_matrix_data, idx, nozzle_num));
    }
    flush_multiplier.resize(nozzle_num, 1);

    // Physical sub-matrices sent to the web editor (num_phys×num_phys).
    std::vector<std::vector<double>> flush_matrixs;
    for (int idx = 0; idx < nozzle_num; ++idx) {
        flush_matrixs.emplace_back(extract_physical_sub_matrix(full_matrixs[idx], total, num_phys));
    }

    // Default matrices for the auto-calc button — physical sub-matrix only.
    std::vector<std::vector<float>> default_matrixs;
    for (int idx = 0; idx < nozzle_num; ++idx) {
        std::vector<float> def_flat = MatrixFlatten(CalcFlushingVolumes(idx));
        std::vector<double> def_d(def_flat.begin(), def_flat.end());
        std::vector<double> def_phys = extract_physical_sub_matrix(def_d, total, num_phys);
        default_matrixs.emplace_back(def_phys.begin(), def_phys.end());
    }

    // Store full matrices so storeData can expand back when saving.
    m_raw_matrixs = full_matrixs;
    m_flush_multipliers = flush_multiplier;

    // Only send physical filament colours to the editor.
    std::vector<std::string> phys_colors(filament_colors.begin(),
                                          filament_colors.begin() + std::min(num_phys, filament_colors.size()));

    json obj;
    obj["flush_multiplier"] = flush_multiplier;
    obj["extruder_num"] = nozzle_num;
    obj["filament_colors"] = phys_colors;
    obj["flush_volume_matrixs"] = json::array();
    obj["min_flush_volumes"] = json::array();
    obj["max_flush_volumes"] = json::array();
    obj["min_flush_multiplier"] = g_min_flush_multiplier;
    obj["max_flush_multiplier"] = g_max_flush_multiplier;
    obj["is_dark_mode"] = wxGetApp().dark_mode();
    obj["default_matrixs"]      = json::array();

    for (const auto& vec : flush_matrixs) {
        obj["flush_volume_matrixs"].push_back(vec);
    }
    for (const auto &vec : default_matrixs) {
        obj["default_matrixs"].push_back(vec);
    }

    for (int idx = 0; idx < nozzle_num; ++idx) {
        // min_flush_volumes is indexed by physical slot; slice off at num_phys.
        const std::vector<int> &min_flush_volumes = get_min_flush_volumes(full_config, idx);
        int min_flush_from_nozzle_volume = min_flush_volumes.empty()
            ? 0
            : *min_element(min_flush_volumes.begin(),
                           min_flush_volumes.begin() + std::min(num_phys, min_flush_volumes.size()));
        GenericFlushPredictor pd(nozzle_flush_dataset[idx]);
        int min_flush_from_flush_data = pd.get_min_flush_volume();
        obj["min_flush_volumes"].push_back(std::min(min_flush_from_flush_data,min_flush_from_nozzle_volume));
        obj["max_flush_volumes"].push_back(m_max_flush_volume);
    }

    auto obj_str = obj.dump();
    return obj_str;
}

wxString WipingDialog::BuildTextObjStr(bool multi_language)
{
    wxString auto_flush_tip;
    wxString volume_desp_panel;
    wxString volume_range_panel;
    wxString multiplier_range_panel;
    wxString calc_btn_panel;
    wxString extruder_label_0;
    wxString extruder_label_1;
    wxString multiplier_label;
    wxString ok_btn_label;
    wxString cancel_btn_label;

    if (multi_language) {
        auto_flush_tip = _L("Orca would re-calculate your flushing volumes everytime the filaments color changed or filaments changed. You could disable the auto-calculate in Orca Slicer > Preferences");
        volume_desp_panel = _L("Flushing volume (mm³) for each filament pair.");
        volume_range_panel = wxString::Format(_L("Suggestion: Flushing Volume in range [%d, %d]"), 0, 700);
        multiplier_range_panel = wxString::Format(_L("The multiplier should be in range [%.2f, %.2f]."), 0, 3);
        calc_btn_panel = _L("Re-calculate");
        extruder_label_0 = _L("Left extruder");
        extruder_label_1 = _L("Right extruder");
        multiplier_label = _L("Multiplier");
        ok_btn_label = _L("OK");
        cancel_btn_label = _L("Cancel");
    } else {
        auto_flush_tip = "Orca would re-calculate your flushing volumes everytime the filaments color changed or filaments changed. You could disable the auto-calculate in Orca Slicer > Preferences";
        volume_desp_panel = wxString::FromUTF8("Flushing volume (mm³) for each filament pair.");
        volume_range_panel = wxString::Format("Suggestion: Flushing Volume in range [%d, %d]", 0, 700);
        multiplier_range_panel = wxString::Format("The multiplier should be in range [%.2f, %.2f].", 0, 3);
        calc_btn_panel = "Re-calculate";
        extruder_label_0 = "Left extruder";
        extruder_label_1 = "Right extruder";
        multiplier_label = "Multiplier";
        ok_btn_label = "OK";
        cancel_btn_label = "Cancel";
    }

    wxString text_obj = "{";
    text_obj += wxString::Format("\"volume_desp_panel\":\"%s\",", volume_desp_panel);
    text_obj += wxString::Format("\"volume_range_panel\":\"%s\",", volume_range_panel);
    text_obj += wxString::Format("\"multiplier_range_panel\":\"%s\",", multiplier_range_panel);
    text_obj += wxString::Format("\"calc_btn_panel\":\"%s\",", calc_btn_panel);
    text_obj += wxString::Format("\"extruder_label_0\":\"%s\",", extruder_label_0);
    text_obj += wxString::Format("\"extruder_label_1\":\"%s\",", extruder_label_1);
    text_obj += wxString::Format("\"multiplier_label\":\"%s\",", multiplier_label);
    text_obj += wxString::Format("\"ok_btn_label\":\"%s\",", ok_btn_label);
    text_obj += wxString::Format("\"cancel_btn_label\":\"%s\",", cancel_btn_label);
    text_obj += wxString::Format("\"auto_flush_tip\":\"%s\"", auto_flush_tip);
    text_obj += "}";
    return text_obj;
}

WipingDialog::WipingDialog(wxWindow* parent, const int max_flush_volume) :
    wxDialog(parent, wxID_ANY, _(L("Flushing volumes for filament change")),
    wxDefaultPosition, wxDefaultSize,
    wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER ),
    m_max_flush_volume(max_flush_volume)
{
    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);
    this->SetSizer(main_sizer);
    this->SetBackgroundColour(*wxWHITE);
    auto filament_count = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour")->values.size();

    // Estimate table scroll area size based on filament count
    // Each table cell is ~60x25 DIP, plus headers and borders
    wxSize max_scroll_size = { FromDIP(1000), FromDIP(500) };
    wxSize table_size = { (int)(filament_count + 1) * FromDIP(60), (int)(filament_count + 1) * FromDIP(25) + FromDIP(2) };
    wxSize scroll_size = { std::min(max_scroll_size.x, table_size.x), std::min(max_scroll_size.y, table_size.y) };

    // Fixed overhead: padding (~30), tip panel (~70), controls row (~50),
    // description/multiplier section (~130), button row (~45) = ~325 DIP
    wxSize fixed_overhead = { FromDIP(100), FromDIP(325) };
    wxSize applied_size = scroll_size + fixed_overhead;

    // Clamp to screen size (leave some margin for window decorations)
    wxSize scaled_screen_size = wxGetDisplaySize();
    double scale_factor = wxDisplay().GetScaleFactor();
    scaled_screen_size = { (int)(scaled_screen_size.x / scale_factor), (int)(scaled_screen_size.y / scale_factor) };
    wxSize screen_margin = { FromDIP(40), FromDIP(60) };
    scaled_screen_size -= screen_margin;

    applied_size = { std::min(applied_size.x, scaled_screen_size.x), std::min(applied_size.y, scaled_screen_size.y) };

    // Ensure a reasonable minimum size so the dialog is usable even when clamped
    applied_size.x = std::max(applied_size.x, FromDIP(350));
    applied_size.y = std::max(applied_size.y, FromDIP(450));
    m_webview = wxWebView::New(this, wxID_ANY,
        wxEmptyString,
        wxDefaultPosition,
        applied_size,
        wxWebViewBackendDefault,
        wxNO_BORDER);

    m_webview->AddScriptMessageHandler("wipingDialog");
    main_sizer->Add(m_webview, 1, wxEXPAND);

    fs::path filepath = fs::path(resources_dir()) / "web/flush/WipingDialog.html";
    wxString filepath_str = from_path(filepath);
    wxFileName fn(filepath_str);
    if(fn.FileExists()) {
        wxString url = wxFileSystem::FileNameToURL(fn);
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__<< "File exists and load url " << url.ToStdString();
        m_webview->LoadURL(url);
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__<< "Successfully loaded url: " << url.ToStdString();
    } else {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< "WipingDialog.html not exist at path: " << filepath.string();
    }

    main_sizer->SetSizeHints(this);
    main_sizer->Fit(this);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);

    //m_webview->Bind(wxEVT_WEBVIEW_NAVIGATED, [this](auto& evt) {
    //    auto table_obj_str = BuildTableObjStr();
    //    auto text_obj_str = BuildTextObjStr();
    //    CallAfter([table_obj_str, text_obj_str, this] {
    //        wxString script = wxString::Format("buildTable(%s);buildText(%s)", table_obj_str, text_obj_str);
    //        m_webview->RunScript(script);
    //        });
    //    });

    m_webview->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, [this](wxWebViewEvent& evt) {
        std::string message = evt.GetString().ToStdString();
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__<< "Received message: " << message;
        try {
            json j = json::parse(message);
            if (j["msg"].get<std::string>() == "init") {
                auto table_obj_str = BuildTableObjStr();
                auto text_obj_str = BuildTextObjStr(true);
                CallAfter([table_obj_str, text_obj_str, this] {
                    wxString script1 = wxString::Format("buildTable(%s)", table_obj_str);
                    m_webview->RunScript(script1);
                    wxString script2 = wxString::Format("buildText(%s)", text_obj_str);
                    bool result = m_webview->RunScript(script2);
                    if (!result) {
                        BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< "Failed to run buildText, retry without multi language";
                        wxString script3 = wxString::Format("buildText(%s)", BuildTextObjStr(false));
                        m_webview->RunScript(script3);
                    }
                    });
            }
            else if (j["msg"].get<std::string>() == "updateMatrix") {
                int extruder_id = j["extruder_id"].get<int>();
                auto new_matrix = CalcFlushingVolumes(extruder_id);
                json obj;
                obj["flush_volume_matrix"] = MatrixFlatten(new_matrix);
                obj["extruder_id"] = extruder_id;
                CallAfter([obj, this] {
                    std::string flush_volume_matrix_str = obj["flush_volume_matrix"].dump();
                    std::string extruder_id_str = std::to_string(obj["extruder_id"].get<int>());
                    wxString script = wxString::Format("updateTable(%s,%s)", flush_volume_matrix_str, extruder_id_str);
                    m_webview->RunScript(script);
                    });
            }
            else if (j["msg"].get<std::string>() == "storeData") {
                int extruder_num = j["number_of_extruders"].get<int>();
                // The web editor works on the physical sub-matrix (P×P).
                std::vector<std::vector<double>> phys_matrixs;
                for (auto iter = j["raw_matrix"].begin(); iter != j["raw_matrix"].end(); ++iter) {
                    phys_matrixs.emplace_back((*iter).get<std::vector<double>>());
                }
                std::vector<double>store_multipliers = j["flush_multiplier"].get<std::vector<double>>();
                {// limit all matrix value before write to gcode, the limitation is depends on the multipliers
                    size_t cols_temp_matrix = 0;
                    if (!phys_matrixs.empty()) { cols_temp_matrix = phys_matrixs[0].size(); }
                    if (store_multipliers.size() == phys_matrixs.size() && cols_temp_matrix>0) // nuzzles==nuzzles
                    {
                        for (size_t idx = 0; idx < store_multipliers.size(); ++idx) {
                            double m_max_flush_volume_t = (double)m_max_flush_volume, m_store_multipliers=store_multipliers[idx];
                            std::transform(phys_matrixs[idx].begin(), phys_matrixs[idx].end(),
                                           phys_matrixs[idx].begin(),
                                           [m_max_flush_volume_t, m_store_multipliers](double inputx) {
                                               return std::clamp(inputx, 0.0, m_max_flush_volume_t / m_store_multipliers);
                                           });
                        }
                    }
                }
                // Expand physical sub-matrices back to full total×total,
                // preserving the mixed-slot rows/cols from the snapshot taken
                // in BuildTableObjStr.
                const size_t num_phys = static_cast<size_t>(wxGetApp().filaments_cnt());
                std::vector<std::vector<double>> store_matrixs;
                for (size_t idx = 0; idx < phys_matrixs.size(); ++idx) {
                    if (idx < m_raw_matrixs.size() && !m_raw_matrixs[idx].empty()) {
                        const size_t total = static_cast<size_t>(
                            std::round(std::sqrt(static_cast<double>(m_raw_matrixs[idx].size()))));
                        store_matrixs.emplace_back(
                            expand_physical_to_full_matrix(phys_matrixs[idx], m_raw_matrixs[idx], total, num_phys));
                    } else {
                        store_matrixs.emplace_back(phys_matrixs[idx]);
                    }
                }
                this->StoreFlushData(extruder_num, store_matrixs, store_multipliers);
                m_submit_flag = true;
                this->Close();
            }
            else if (j["msg"].get<std::string>() == "quit") {
                this->Close();
            }
        }
        catch (...) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< "Failed to parse json message: " << message;
        }
        });

    m_webview->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
    if (e.GetKeyCode() == WXK_ESCAPE) {
            if (IsModal()) EndModal(wxID_CANCEL);
            else Close();
            return;
    }
        e.Skip();
    });
}


int WipingDialog::CalcFlushingVolume(const wxColour& from, const wxColour& to, int min_flush_volume , int nozzle_flush_dataset)
{
    Slic3r::FlushVolCalculator calculator(min_flush_volume, Slic3r::g_max_flush_volume, nozzle_flush_dataset);
    return calculator.calc_flush_vol(from.Alpha(), from.Red(), from.Green(), from.Blue(), to.Alpha(), to.Red(), to.Green(), to.Blue());
}

WipingDialog::VolumeMatrix WipingDialog::CalcFlushingVolumes(int extruder_id)
{
    auto& preset_bundle = wxGetApp().preset_bundle;
    auto full_config = preset_bundle->full_config();
    auto& ams_multi_color_filament = preset_bundle->ams_multi_color_filment;

    std::vector<std::string> filament_color_strs = full_config.option<ConfigOptionStrings>("filament_colour")->values;
    std::vector<std::vector<wxColour>> multi_colors;
    std::vector<wxColour> filament_colors;
    for (auto color_str : filament_color_strs)
        filament_colors.emplace_back(color_str);

    int flush_dataset_value = full_config.option<ConfigOptionIntsNullable>("nozzle_flush_dataset")->values[extruder_id];
    // Support for multi-color filament
    for (int i = 0; i < filament_colors.size(); ++i) {
        std::vector<wxColour> single_filament;
        if (i < ams_multi_color_filament.size()) {
            if (!ams_multi_color_filament[i].empty()) {
                std::vector<std::string> colors = ams_multi_color_filament[i];
                for (int j = 0; j < colors.size(); ++j) {
                    single_filament.push_back(wxColour(colors[j]));
                }
                multi_colors.push_back(single_filament);
                continue;
            }
        }
        single_filament.push_back(wxColour(filament_colors[i]));
        multi_colors.push_back(single_filament);
    }

    VolumeMatrix matrix;
    const std::vector<int> min_flush_volumes = get_min_flush_volumes(full_config, extruder_id);

    for (int from_idx = 0; from_idx < multi_colors.size(); ++from_idx) {
        bool is_from_support = is_support_filament(from_idx);
        matrix.emplace_back();
        for (int to_idx = 0; to_idx < multi_colors.size(); ++to_idx) {
            if (from_idx == to_idx) {
                matrix.back().emplace_back(0);
                continue;
            }

            bool is_to_support = is_support_filament(to_idx);

            int flushing_volume = 0;
            if (is_to_support) {
                flushing_volume = Slic3r::g_flush_volume_to_support;
            }
            else {
                for (int i = 0; i < multi_colors[from_idx].size(); ++i) {
                    const wxColour& from = multi_colors[from_idx][i];
                    for (int j = 0; j < multi_colors[to_idx].size(); ++j) {
                        const wxColour& to = multi_colors[to_idx][j];
                        int volume = CalcFlushingVolume(from, to, min_flush_volumes[from_idx], flush_dataset_value);
                        flushing_volume = std::max(flushing_volume, volume);
                    }
                }

                if (is_from_support) {
                    flushing_volume = std::max(Slic3r::g_min_flush_volume_from_support, flushing_volume);
                }
            }
            matrix.back().emplace_back(flushing_volume);
        }
    }
    return matrix;
}

void WipingDialog::StoreFlushData(int extruder_num, const std::vector<std::vector<double>>& flush_volume_vecs, const std::vector<double>&flush_multipliers)
{
    m_flush_multipliers = flush_multipliers;
    m_raw_matrixs = flush_volume_vecs;
}

std::vector<double> WipingDialog::GetFlattenMatrix()const
{
    std::vector<double> ret;
    for (auto& matrix : m_raw_matrixs) {
        ret.insert(ret.end(), matrix.begin(), matrix.end());
    }
    return ret;
}


std::vector<double> WipingDialog::GetMultipliers()const
{
    return m_flush_multipliers;
}
