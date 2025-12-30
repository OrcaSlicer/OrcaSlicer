#include "Plater.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r_version.h"

#include <cstddef>
#include <algorithm>
#include <numeric>
#include <limits>
#include <vector>
#include <string>
#include <regex>
#include <future>
#include <boost/algorithm/string.hpp>
#include <boost/iterator/counting_iterator.hpp>
#include <boost/optional.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/button.h>
#include <wx/bmpcbox.h>
#include <wx/display.h>
#include <wx/statbox.h>
#include <wx/statbmp.h>
#include <wx/filedlg.h>
#include <wx/dnd.h>
#include <wx/progdlg.h>
#include <wx/string.h>
#include <wx/wupdlock.h>
#include <wx/numdlg.h>
#include <wx/debug.h>
#include <wx/busyinfo.h>
#include <wx/event.h>
#include <wx/wrapsizer.h>
#ifdef _WIN32
#include <wx/richtooltip.h>
#include <wx/custombgwin.h>
#include <wx/popupwin.h>
#endif
#include <wx/clrpicker.h>
#include <wx/tokenzr.h>
#include <wx/aui/aui.h>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Format/STEP.hpp"
#include "libslic3r/Format/AMF.hpp"
//#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/SLA/Hollowing.hpp"
#include "libslic3r/SLA/SupportPoint.hpp"
#include "libslic3r/SLA/ReprojectPointsOnMesh.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ObjColorUtils.hpp"
// For stl export
#include "libslic3r/CSGMesh/ModelToCSGMesh.hpp"
#include "libslic3r/CSGMesh/PerformCSGMeshBooleans.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GuiColor.hpp"
#include "GUI_ObjectList.hpp"
#include "GUI_Utils.hpp"
#include "GUI_Factories.hpp"
#include "wxExtensions.hpp"
#include "MainFrame.hpp"
#include "format.hpp"
#include "3DScene.hpp"
#include "GLCanvas3D.hpp"
#include "Selection.hpp"
#include "GLToolbar.hpp"
#include "GUI_Preview.hpp"
#include "3DBed.hpp"
#include "PartPlate.hpp"
#include "Camera.hpp"
#include "Mouse3DController.hpp"
#include "Tab.hpp"
#include "Jobs/OrientJob.hpp"
#include "Jobs/ArrangeJob.hpp"
#include "Jobs/FillBedJob.hpp"
#include "Jobs/RotoptimizeJob.hpp"
#include "Jobs/SLAImportJob.hpp"
#include "Jobs/SLAImportDialog.hpp"
#include "Jobs/PrintJob.hpp"
#include "Jobs/NotificationProgressIndicator.hpp"
#include "Jobs/PlaterWorker.hpp"
#include "Jobs/BoostThreadWorker.hpp"
#include "BackgroundSlicingProcess.hpp"
#include "SelectMachine.hpp"
#include "SendMultiMachinePage.hpp"
#include "SendToPrinter.hpp"
#include "PublishDialog.hpp"
#include "ModelMall.hpp"
#include "ConfigWizard.hpp"
#include "SyncAmsInfoDialog.hpp"
#include "../Utils/ASCIIFolding.hpp"
#include "../Utils/FixModelByWin10.hpp"
#include "../Utils/UndoRedo.hpp"
#include "../Utils/PresetUpdater.hpp"
#include "../Utils/Process.hpp"
#include "RemovableDriveManager.hpp"
#include "InstanceCheck.hpp"
#include "NotificationManager.hpp"
#include "PresetComboBoxes.hpp"
#include "MsgDialog.hpp"
#include "ProjectDirtyStateManager.hpp"
#include "Gizmos/GLGizmoSimplify.hpp" // create suggestion notification
#include "Gizmos/GLGizmoSVG.hpp" // Drop SVG file
#include "Gizmos/GizmoObjectManipulation.hpp"

// BBS
#include "Widgets/ProgressDialog.hpp"
#include "BBLStatusBar.hpp"
#include "BitmapCache.hpp"
#include "ParamsDialog.hpp"
#include "ImageDPIFrame.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/RoundedRectangle.hpp"
#include "Widgets/RadioGroup.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/StaticGroup.hpp"

#include "GUI_ObjectTable.hpp"
#include "libslic3r/Thread.hpp"

#ifdef __APPLE__
#include "Gizmos/GLGizmosManager.hpp"
#endif // __APPLE__

#include <libslic3r/CutUtils.hpp>
#include <wx/glcanvas.h>    // Needs to be last because reasons :-/
#include <libslic3r/miniz_extension.hpp>
#include "WipeTowerDialog.hpp"
#include "ObjColorDialog.hpp"

#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/Platform.hpp"
#include "nlohmann/json.hpp"

#include "PhysicalPrinterDialog.hpp"
#include "PrintHostDialogs.hpp"
#include "PlateSettingsDialog.hpp"
#include "DailyTips.hpp"
#include "CreatePresetsDialog.hpp"
#include "FileArchiveDialog.hpp"
#include "StepMeshDialog.hpp"
#include "FilamentMapDialog.hpp"
#include "CloneDialog.hpp"

#include "DeviceCore/DevFilaSystem.h"
#include "DeviceCore/DevManager.h"

using boost::optional;
namespace fs = boost::filesystem;
using Slic3r::_3DScene;
using Slic3r::Preset;
using Slic3r::GUI::format_wxstr;
using namespace nlohmann;

static const std::pair<unsigned int, unsigned int> THUMBNAIL_SIZE_3MF = { 512, 512 };

namespace Slic3r {
namespace GUI {

wxDEFINE_EVENT(EVT_SCHEDULE_BACKGROUND_PROCESS,     SimpleEvent);
wxDEFINE_EVENT(EVT_SLICING_UPDATE,                  SlicingStatusEvent);
wxDEFINE_EVENT(EVT_SLICING_COMPLETED,               wxCommandEvent);
wxDEFINE_EVENT(EVT_PROCESS_COMPLETED,               SlicingProcessCompletedEvent);
wxDEFINE_EVENT(EVT_EXPORT_BEGAN,                    wxCommandEvent);
wxDEFINE_EVENT(EVT_EXPORT_FINISHED,                 wxCommandEvent);
wxDEFINE_EVENT(EVT_IMPORT_MODEL_ID,                 wxCommandEvent);
wxDEFINE_EVENT(EVT_DOWNLOAD_PROJECT,                wxCommandEvent);
wxDEFINE_EVENT(EVT_PUBLISH,                         wxCommandEvent);
wxDEFINE_EVENT(EVT_OPEN_PLATESETTINGSDIALOG,        wxCommandEvent);
wxDEFINE_EVENT(EVT_OPEN_FILAMENT_MAP_SETTINGS_DIALOG, wxCommandEvent);
// BBS: backup & restore
wxDEFINE_EVENT(EVT_RESTORE_PROJECT,                 wxCommandEvent);
wxDEFINE_EVENT(EVT_PRINT_FINISHED,                  wxCommandEvent);
wxDEFINE_EVENT(EVT_SEND_CALIBRATION_FINISHED,       wxCommandEvent);
wxDEFINE_EVENT(EVT_SEND_FINISHED,                   wxCommandEvent);
wxDEFINE_EVENT(EVT_PUBLISH_FINISHED,                wxCommandEvent);
//BBS: repair model
wxDEFINE_EVENT(EVT_REPAIR_MODEL,                    wxCommandEvent);
wxDEFINE_EVENT(EVT_FILAMENT_COLOR_CHANGED,          wxCommandEvent);
wxDEFINE_EVENT(EVT_INSTALL_PLUGIN_NETWORKING,       wxCommandEvent);
wxDEFINE_EVENT(EVT_UPDATE_PLUGINS_WHEN_LAUNCH,       wxCommandEvent);
wxDEFINE_EVENT(EVT_INSTALL_PLUGIN_HINT,             wxCommandEvent);
wxDEFINE_EVENT(EVT_PREVIEW_ONLY_MODE_HINT,          wxCommandEvent);
//BBS: change light/dark mode
wxDEFINE_EVENT(EVT_GLCANVAS_COLOR_MODE_CHANGED,     SimpleEvent);
//BBS: print
wxDEFINE_EVENT(EVT_PRINT_FROM_SDCARD_VIEW,          SimpleEvent);

wxDEFINE_EVENT(EVT_CREATE_FILAMENT, SimpleEvent);
wxDEFINE_EVENT(EVT_MODIFY_FILAMENT, SimpleEvent);
wxDEFINE_EVENT(EVT_ADD_FILAMENT, SimpleEvent);
wxDEFINE_EVENT(EVT_DEL_FILAMENT, SimpleEvent);
wxDEFINE_EVENT(EVT_ADD_CUSTOM_FILAMENT, ColorEvent);
wxDEFINE_EVENT(EVT_NOTICE_CHILDE_SIZE_CHANGED, SimpleEvent);
wxDEFINE_EVENT(EVT_NOTICE_FULL_SCREEN_CHANGED, IntEvent);
#define PRINTER_THUMBNAIL_SIZE (wxSize(40, 40)) // ORCA
#define PRINTER_PANEL_SIZE (    wxSize(70, 60)) // ORCA
#define PRINTER_PANEL_RADIUS (6) // ORCA
#define BTN_SYNC_SIZE (wxSize(FromDIP(96), FromDIP(98)))

static string get_diameter_string(float diameter)
{
    std::ostringstream stream; // ORCA ensure 0.25 returned as 0.25. previous code returned as 0.2 because of std::setprecision(1)
    stream << std::fixed << std::setprecision(2) << diameter;  // Use 2 decimals to capture 0.25 / 0.15 reliably
    std::string s = stream.str();
    if (s.find('.') != std::string::npos) {   // Remove trailing zeros, but keep at least one decimal if needed
        s.erase(s.find_last_not_of('0') + 1);
        if (s.back() == '.') s += '0';        // Ensure "1." → "1.0"
    }
    return s;
}

bool Plater::has_illegal_filename_characters(const wxString& wxs_name)
{
    std::string name = into_u8(wxs_name);
    return has_illegal_filename_characters(name);
}

bool Plater::has_illegal_filename_characters(const std::string& name)
{
    const char* illegal_characters = "<>:/\\|?*\"";
    for (size_t i = 0; i < std::strlen(illegal_characters); i++)
        if (name.find_first_of(illegal_characters[i]) != std::string::npos)
            return true;

    return false;
}

void Plater::show_illegal_characters_warning(wxWindow* parent)
{
    show_error(parent, _L("Invalid name, the following characters are not allowed:") + " <>:/\\|?*\"");
}

static std::map<BedType, std::string> bed_type_thumbnails = {
    {BedType::btPC,        "bed_cool"           },
    {BedType::btEP,        "bed_engineering"    },
    {BedType::btPEI,       "bed_high_templ"     },
    {BedType::btPTE,       "bed_pei"            },
    {BedType::btPCT,       "bed_pei_cool"       },
    {BedType::btSuperTack, "bed_cool_supertack" }
};

enum SlicedInfoIdx
{
    siFilament_m,
    siFilament_mm3,
    siFilament_g,
    siMateril_unit,
    siCost,
    siEstimatedTime,
    siWTNumbetOfToolchanges,
    siCount
};

enum class LoadFilesType {
    NoFile,
    Single3MF,
    SingleOther,
    Multiple3MF,
    MultipleOther,
    Multiple3MFOther,
};

enum class LoadType : unsigned char
{
    Unknown,
    OpenProject,
    LoadGeometry,
    LoadConfig
};

class SlicedInfo : public wxStaticBoxSizer
{
public:
    SlicedInfo(wxWindow *parent);
    void SetTextAndShow(SlicedInfoIdx idx, const wxString& text, const wxString& new_label="");

private:
    std::vector<std::pair<wxStaticText*, wxStaticText*>> info_vec;
};

SlicedInfo::SlicedInfo(wxWindow *parent) :
    wxStaticBoxSizer(new wxStaticBox(parent, wxID_ANY, _L("Sliced Info")), wxVERTICAL)
{
    GetStaticBox()->SetFont(wxGetApp().bold_font());
    wxGetApp().UpdateDarkUI(GetStaticBox());

    auto *grid_sizer = new wxFlexGridSizer(2, 5, 15);
    grid_sizer->SetFlexibleDirection(wxVERTICAL);

    info_vec.reserve(siCount);

    auto init_info_label = [this, parent, grid_sizer](wxString text_label) {
        auto *text = new wxStaticText(parent, wxID_ANY, text_label);
        text->SetForegroundColour(*wxBLACK);
        text->SetFont(wxGetApp().small_font());
        auto info_label = new wxStaticText(parent, wxID_ANY, "N/A");
        info_label->SetForegroundColour(*wxBLACK);
        info_label->SetFont(wxGetApp().small_font());
        grid_sizer->Add(text, 0);
        grid_sizer->Add(info_label, 0);
        info_vec.push_back(std::pair<wxStaticText*, wxStaticText*>(text, info_label));
    };

    init_info_label(_L("Used Filament (m)"));
    init_info_label(_L("Used Filament (mm³)"));
    init_info_label(_L("Used Filament (g)"));
    init_info_label(_L("Used Materials"));
    init_info_label(_L("Cost"));
    init_info_label(_L("Estimated time"));
    init_info_label(_L("Filament changes"));

    Add(grid_sizer, 0, wxEXPAND);
    this->Show(false);
}

void SlicedInfo::SetTextAndShow(SlicedInfoIdx idx, const wxString& text, const wxString& new_label/*=""*/)
{
    const bool show = text != "N/A";
    if (show)
        info_vec[idx].second->SetLabelText(text);
    if (!new_label.IsEmpty())
        info_vec[idx].first->SetLabelText(new_label);
    info_vec[idx].first->Show(show);
    info_vec[idx].second->Show(show);
}

static wxString temp_dir;

// Sidebar / private

enum class ActionButtonType : int {
    abReslice,
    abExport,
    abSendGCode
};

struct ExtruderGroup : StaticGroup
{
    ExtruderGroup(wxWindow * parent, int index, wxString const &title);
    wxStaticBoxSizer *sizer        = nullptr;
    ScalableButton *  btn_edit     = nullptr;
    ComboBox *        combo_diameter = nullptr;
    ComboBox *        combo_flow = nullptr;
    AMSPreview *      ams[4]       = {nullptr};
    wxStaticText     *ams_not_installed_msg{nullptr};
    ScalableButton *  btn_up{nullptr};
    ScalableButton *  btn_down{nullptr};
    wxBoxSizer *hsizer_ams { nullptr };
    size_t               page_cur{0};
    size_t               page_num{3};
    size_t               ams_n4 = 0;
    size_t               ams_n1 = 0;
    std::vector<AMSinfo> ams_4;
    std::vector<AMSinfo> ams_1;
    wxString          diameter;

    void set_ams_count(int n4, int n1)
    {
        if (n4 == ams_n4 && n1 == ams_n1)
            return;
        ams_n4 = n4;
        ams_n1 = n1;
        if (btn_edit) {
            update_ams();
        }
    }

    void update_ams();

    void sync_ams(MachineObject const *obj, std::vector<DevAms *> const &ams4, std::vector<DevAms *> const &ams1);

    void Rescale()
    {
        if (btn_edit)
            btn_edit->msw_rescale();
        btn_up->msw_rescale();
        btn_down->msw_rescale();
        combo_diameter->Rescale();
        combo_flow->Rescale();
        for (int i = 0; i < 4; ++i)
            ams[i]->msw_rescale();
    }
};

struct Sidebar::priv
{
    Plater *plater;

    wxPanel *scrolled = nullptr;
    PlaterPresetComboBox *combo_sla_print = nullptr;
    PlaterPresetComboBox *combo_sla_material = nullptr;

    // Printer
    wxSizer *             vsizer_printer      = nullptr;
    wxBoxSizer *          extruder_dual_sizer  = nullptr;
    wxBoxSizer *          extruder_single_sizer = nullptr;
    // Printer - preset
    StaticBox * panel_printer_preset = nullptr;
    wxStaticBitmap *      image_printer       = nullptr;
    PlaterPresetComboBox *combo_printer       = nullptr;
    ScalableButton *      btn_edit_printer    = nullptr;
    ScalableButton *      btn_connect_printer = nullptr;

    // Nozzle diameter
    StaticBox *     panel_nozzle_dia  = nullptr;
    Label *         label_nozzle_title= nullptr;
    ComboBox *      combo_nozzle_dia  = nullptr;
    Label *         label_nozzle_type = nullptr;

    // Printer - bed
    StaticBox *     panel_printer_bed = nullptr;
    wxStaticBitmap *image_printer_bed = nullptr;
    ComboBox *      combo_printer_bed = nullptr;

    ImageDPIFrame *big_bed_image_popup = nullptr;
    // Printer - sync
    //Button *btn_sync_printer;
    std::shared_ptr<int> counter_sync_printer = std::make_shared<int>();
    wxTimer *            timer_sync_printer = new wxTimer();
    // Printer - ams
    ExtruderGroup *left_extruder = nullptr;
    ExtruderGroup *right_extruder = nullptr;
    ExtruderGroup *single_extruder = nullptr;

    int  FromDIP(int n) { return plater->FromDIP(n); }
    void layout_printer(bool isBBL, bool isDual);

    void flush_printer_sync(bool restart = false);

    PlaterPresetComboBox *combo_print = nullptr;
    std::vector<PlaterPresetComboBox*> combos_filament;
    int editing_filament = -1;
    wxBoxSizer *sizer_filaments = nullptr;

    //BBS Sidebar widgets
    wxPanel* m_panel_print_title;
    wxStaticText* m_staticText_print_title;
    wxPanel* m_panel_print_content;
    wxBoxSizer *sizer_params;

    //wxComboBox *                m_comboBox_print_preset;
    wxStaticLine *              m_staticline1;
    StaticBox* m_panel_filament_title;
    wxStaticText* m_staticText_filament_settings;
    ScalableButton *  m_bpButton_add_filament;
    ScalableButton *  m_bpButton_del_filament;
    ScalableButton *  m_bpButton_ams_filament;
    ScalableButton *  m_bpButton_set_filament;
    int m_menu_filament_id = -1;
    wxScrolledWindow* m_panel_filament_content;
    wxScrolledWindow* m_scrolledWindow_filament_content;
    wxStaticLine* m_staticline2;
    wxPanel* m_panel_project_title;
    ScalableButton* m_filament_icon = nullptr;
    Button * m_flushing_volume_btn = nullptr;
    TextInput* m_search_item = nullptr;
    StaticBox* m_search_bar = nullptr;
    Search::SearchObjectDialog* dia = nullptr;

    // BBS printer config
    StaticBox* m_panel_printer_title = nullptr;
    ScalableButton* m_printer_icon = nullptr;
    ScalableButton* m_printer_connect = nullptr;
    ScalableButton* m_printer_bbl_sync = nullptr;
    ScalableButton* m_printer_setting = nullptr;
    wxStaticText *  m_text_printer_settings = nullptr;
    wxPanel* m_panel_printer_content = nullptr;

    ObjectList          *m_object_list{ nullptr };
    ObjectSettings      *object_settings{ nullptr };
    ObjectLayers        *object_layers{ nullptr };

    wxButton *btn_export_gcode;
    wxButton *btn_reslice;
    ScalableButton *btn_send_gcode;
    //ScalableButton *btn_eject_device;
    ScalableButton* btn_export_gcode_removable; //exports to removable drives (appears only if removable drive is connected)

    bool                    is_switching_diameter{false};
    Search::OptionsSearcher     searcher;
    std::string ams_list_device;

    priv(Plater *plater) : plater(plater) {}
    ~priv();

    void show_preset_comboboxes();
    void jump_to_object(ObjectDataViewModelNode* item);
    void can_search();

    bool sync_extruder_list(bool &only_external_material);
    bool switch_diameter(bool single);
    void update_sync_status(const MachineObject* obj);

#ifdef _WIN32
    wxString btn_reslice_tip;
    void show_rich_tip(const wxString& tooltip, wxButton* btn);
    void hide_rich_tip(wxButton* btn);
#endif
};

void Sidebar::priv::layout_printer(bool isBBL, bool isDual)
{
    // Printer - preset
    if (auto sizer = static_cast<wxBoxSizer *>(panel_printer_preset->GetSizer());
            sizer == nullptr /*|| isBBL != (sizer->GetOrientation() == wxVERTICAL)*/) {

        //if (isBBL) {
            wxBoxSizer *hsizer = new wxBoxSizer(wxHORIZONTAL);
            hsizer->Add(image_printer, 0, wxLEFT  | wxALIGN_LEFT  | wxALIGN_CENTER_VERTICAL, FromDIP(10));
            hsizer->Add(combo_printer, 1, wxEXPAND | wxALL | wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL, FromDIP(2));
            hsizer->AddSpacer(FromDIP(2));
            hsizer->Add(btn_edit_printer, 0, wxRIGHT | wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(SidebarProps::IconSpacing()));
            //hsizer->Add(btn_connect_printer, 0, wxRIGHT | wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(SidebarProps::IconSpacing()));
            panel_printer_preset->SetSizer(hsizer);
        //} else {
        //    wxBoxSizer *hsizer = new wxBoxSizer(wxHORIZONTAL);
        //    hsizer->Add(image_printer, 0, wxLEFT | wxALIGN_CENTER, FromDIP(4));
        //    hsizer->Add(combo_printer, 1, wxALIGN_CENTRE | wxLEFT | wxRIGHT, FromDIP(6));
        //    hsizer->Add(hsizer_printer_btn, 0, wxALIGN_TOP | wxTOP | wxRIGHT, FromDIP(4));
        //    hsizer->AddSpacer(FromDIP(10));
        //    panel_printer_preset->SetSizer(hsizer);
        //}
    }

    if (vsizer_printer->GetItemCount() == 0) {
        wxBoxSizer *hsizer_printer = new wxBoxSizer(wxHORIZONTAL);
        hsizer_printer->Add(panel_printer_preset, 1, wxEXPAND, 0);
        hsizer_printer->Add(panel_nozzle_dia , 0, wxLEFT, FromDIP(4));
        hsizer_printer->Add(panel_printer_bed, 0, wxLEFT, FromDIP(4));
        //hsizer_printer->Add(btn_sync_printer , 0, wxLEFT, FromDIP(4));
        vsizer_printer->AddSpacer(FromDIP(SidebarProps::ContentMarginV()));
        vsizer_printer->Add(hsizer_printer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(SidebarProps::ContentMargin()));

        // Printer - extruder

        // double
        extruder_dual_sizer = new wxBoxSizer(wxHORIZONTAL);
        extruder_dual_sizer->Add(left_extruder->sizer, 1, wxEXPAND, 0);
        extruder_dual_sizer->AddSpacer(FromDIP(4));
        extruder_dual_sizer->Add(right_extruder->sizer, 1, wxEXPAND, 0);

        // single
        extruder_single_sizer = single_extruder->sizer;
        wxBoxSizer * extruder_sizer = new wxBoxSizer(wxVERTICAL);
        extruder_sizer->Add(extruder_dual_sizer  , 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(SidebarProps::ContentMargin()));
        extruder_sizer->Add(extruder_single_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(SidebarProps::ContentMargin()));

        vsizer_printer->Add(extruder_sizer, 1, wxEXPAND | wxTOP, FromDIP(2));

        vsizer_printer->AddSpacer(FromDIP(SidebarProps::ContentMarginV()));
    }

    //btn_connect_printer->Show(!isBBL);
    m_printer_connect->Show(!isBBL);
    //btn_sync_printer->Show(isBBL);
    m_printer_bbl_sync->Show(isBBL);

    // ORCA show plate type combo box only when its supported
    PresetBundle &preset_bundle = *wxGetApp().preset_bundle;
    auto cfg = preset_bundle.printers.get_edited_preset().config;
    // Orca: we use preset_bundle.is_bbl_vendor() instead of isBBL to determine if the plate type combo box should be shown
    // ref: https://github.com/OrcaSlicer/OrcaSlicer/pull/11610#discussion_r2607411847
    panel_printer_bed->Show(preset_bundle.is_bbl_vendor() || cfg.opt_bool("support_multi_bed_types"));

    extruder_dual_sizer->Show(isDual);

    // NEEDFIX requires AMS check or any type of ???
    // Single nozzle & non ams
    panel_nozzle_dia->Show(!isDual && preset_bundle.get_printer_extruder_count() < 2);
    extruder_single_sizer->Show(false);
}

void Sidebar::priv::flush_printer_sync(bool restart)
{
    if (restart) {
        *counter_sync_printer = 6;
        timer_sync_printer->Start(500);
    }
    //btn_sync_printer->SetBackgroundColorNormal((*counter_sync_printer & 1) ? "#F8F8F8" :"#009688");
    m_printer_bbl_sync->SetBitmap_((*counter_sync_printer & 1) ? "printer_sync_not" : "printer_sync_ok");
    if (--*counter_sync_printer <= 0)
        timer_sync_printer->Stop();
}

Sidebar::priv::~priv()
{
    // BBS
    //delete object_manipulation;
    delete object_settings;
    // BBS
#if 0
    delete frequently_changed_parameters;
#endif
}

void Sidebar::priv::show_preset_comboboxes()
{
    const bool showSLA = wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptSLA;

//BBS
#if 0
    for (size_t i = 0; i < 4; ++i)
        sizer_presets->Show(i, !showSLA);

    for (size_t i = 4; i < 8; ++i) {
        if (sizer_presets->IsShown(i) != showSLA)
            sizer_presets->Show(i, showSLA);
    }

    frequently_changed_parameters->Show(!showSLA);
#endif

    scrolled->GetParent()->Layout();
    scrolled->Refresh();
}

void Sidebar::priv::jump_to_object(ObjectDataViewModelNode* item)
{
    m_object_list->selected_object(item);
}

void Sidebar::priv::can_search()
{
    if (m_search_bar->IsShown()) {
        m_search_item->SetFocus();
    }
}

#ifdef _WIN32
using wxRichToolTipPopup = wxCustomBackgroundWindow<wxPopupTransientWindow>;
static wxRichToolTipPopup* get_rtt_popup(wxButton* btn)
{
    auto children = btn->GetChildren();
    for (auto child : children)
        if (child->IsShown())
            return dynamic_cast<wxRichToolTipPopup*>(child);
    return nullptr;
}

void Sidebar::priv::show_rich_tip(const wxString& tooltip, wxButton* btn)
{
    if (tooltip.IsEmpty())
        return;
    wxRichToolTip tip(tooltip, "");
    tip.SetIcon(wxICON_NONE);
    tip.SetTipKind(wxTipKind_BottomRight);
    tip.SetTitleFont(wxGetApp().normal_font());
    tip.SetBackgroundColour(wxGetApp().get_window_default_clr());

    tip.ShowFor(btn);
    // Every call of the ShowFor() creates new RichToolTip and show it.
    // Every one else are hidden.
    // So, set a text color just for the shown rich tooltip
    if (wxRichToolTipPopup* popup = get_rtt_popup(btn)) {
        auto children = popup->GetChildren();
        for (auto child : children) {
            child->SetForegroundColour(wxGetApp().get_label_clr_default());
            // we neen just first text line for out rich tooltip
            return;
        }
    }
}

void Sidebar::priv::hide_rich_tip(wxButton* btn)
{
    if (wxRichToolTipPopup* popup = get_rtt_popup(btn))
        popup->Dismiss();
}
#endif

std::vector<int> get_min_flush_volumes(const DynamicPrintConfig &full_config, size_t nozzle_id)
{
    std::vector<int>extra_flush_volumes;
    //const auto& full_config = wxGetApp().preset_bundle->full_config();
    //auto& printer_config = wxGetApp().preset_bundle->printers.get_edited_preset().config;

    const ConfigOptionFloatsNullable* nozzle_volume_opt = full_config.option<ConfigOptionFloatsNullable>("nozzle_volume");
    int nozzle_volume_val = nozzle_volume_opt ? (int)nozzle_volume_opt->get_at(nozzle_id) : 0;

    const ConfigOptionInt* enable_long_retraction_when_cut_opt = full_config.option<ConfigOptionInt>("enable_long_retraction_when_cut");
    int machine_enabled_level = 0;
    if (enable_long_retraction_when_cut_opt) {
        machine_enabled_level = enable_long_retraction_when_cut_opt->value;
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": get enable_long_retraction_when_cut from config, value=%1%")%machine_enabled_level;
    }
    const ConfigOptionBools* long_retractions_when_cut_opt = full_config.option<ConfigOptionBools>("long_retractions_when_cut");
    bool machine_activated = false;
    if (long_retractions_when_cut_opt) {
        machine_activated = long_retractions_when_cut_opt->values[nozzle_id] == 1;
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": get long_retractions_when_cut from config, value=%1%, activated=%2%")%long_retractions_when_cut_opt->values[0] %machine_activated;
    }

    size_t filament_size = full_config.option<ConfigOptionFloats>("filament_diameter")->values.size();
    std::vector<double> filament_retraction_distance_when_cut(filament_size, 18.0f), printer_retraction_distance_when_cut(filament_size, 18.0f);
    std::vector<unsigned char> filament_long_retractions_when_cut(filament_size, 0);
    const ConfigOptionFloats* filament_retraction_distances_when_cut_opt = full_config.option<ConfigOptionFloats>("filament_retraction_distances_when_cut");
    if (filament_retraction_distances_when_cut_opt) {
        filament_retraction_distance_when_cut = filament_retraction_distances_when_cut_opt->values;
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": get filament_retraction_distance_when_cut from config, size=%1%, values=%2%")%filament_retraction_distance_when_cut.size() %filament_retraction_distances_when_cut_opt->serialize();
    }

    const ConfigOptionFloats* printer_retraction_distance_when_cut_opt = full_config.option<ConfigOptionFloats>("retraction_distances_when_cut");
    if (printer_retraction_distance_when_cut_opt) {
        printer_retraction_distance_when_cut = printer_retraction_distance_when_cut_opt->values;
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": get retraction_distances_when_cut from config, size=%1%, values=%2%")%printer_retraction_distance_when_cut.size() %printer_retraction_distance_when_cut_opt->serialize();
    }

    const ConfigOptionBools* filament_long_retractions_when_cut_opt = full_config.option<ConfigOptionBools>("filament_long_retractions_when_cut");
    if (filament_long_retractions_when_cut_opt) {
        filament_long_retractions_when_cut = filament_long_retractions_when_cut_opt->values;
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": get filament_long_retractions_when_cut from config, size=%1%, values=%2%")%filament_long_retractions_when_cut.size() %filament_long_retractions_when_cut_opt->serialize();
    }

    for (size_t idx = 0; idx < filament_size; ++idx) {
        int extra_flush_volume = nozzle_volume_val;
        int retract_length = machine_enabled_level && machine_activated ? printer_retraction_distance_when_cut[nozzle_id] : 0;

        unsigned char filament_activated = filament_long_retractions_when_cut[idx];
        double filament_retract_length = filament_retraction_distance_when_cut[idx];

        if (filament_activated == 0)
            retract_length = 0;
        else if (filament_activated == 1 && machine_enabled_level == LongRectrationLevel::EnableFilament) {
            if (!std::isnan(filament_retract_length))
                retract_length = (int)filament_retraction_distance_when_cut[idx];
            else
                retract_length = printer_retraction_distance_when_cut[nozzle_id];
        }

        extra_flush_volume -= PI * 1.75 * 1.75 / 4 * retract_length;
        extra_flush_volumes.emplace_back(extra_flush_volume);
    }
    return extra_flush_volumes;
}

// Sidebar / public

struct DynamicFilamentList : DynamicList
{
    std::vector<std::pair<wxString, wxBitmap *>> items;

    void apply_on(Choice *c) override
    {
        if (items.empty())
            update(true);
        auto cb = dynamic_cast<ComboBox *>(c->window);
        wxString old_selection = cb->GetStringSelection();
        int old_index  = cb->GetSelection();
        cb->Clear();
        cb->Append(_L("Default"));
        for (auto i : items) {
            cb->Append(i.first, i.second ? *i.second : wxNullBitmap);
        }

        if (old_index >= 0 && (unsigned int) old_index < cb->GetCount()) {
            cb->SetSelection(old_index);
            return;
        }

        int new_index = cb->FindString(old_selection);
        if (old_index == cb->GetCount()) {
            cb->SetSelection(old_index - 1);
        } else if (new_index != wxNOT_FOUND) {
            cb->SetSelection(new_index);
        } else {
            cb->SetSelection(0);
        }
    }
    wxString get_value(int index) override
    {
        wxString str;
        str << index;
        return str;
    }
    int index_of(wxString value) override
    {
        long n = 0;
        return (value.ToLong(&n) && n <= items.size()) ? int(n) : -1;
    }
    void update(bool force = false)
    {
        items.clear();
        if (!force && m_choices.empty())
            return;
        auto icons = get_extruder_color_icons(true);
        auto presets = wxGetApp().preset_bundle->filament_presets;
        for (int i = 0; i < presets.size(); ++i) {
            wxString str;
            std::string type;
            wxGetApp().preset_bundle->filaments.find_preset(presets[i])->get_filament_type(type);
            str << type;
            items.push_back({str, i < icons.size() ? icons[i] : nullptr});
        }
        DynamicList::update();
    }
};

struct DynamicFilamentList1Based : DynamicFilamentList
{
    void apply_on(Choice *c) override
    {
        if (items.empty())
            update(true);
        auto cb = dynamic_cast<ComboBox *>(c->window);
        auto n  = cb->GetSelection();
        cb->Clear();
        for (auto i : items) {
            cb->Append(i.first, *i.second);
        }
        if (n < cb->GetCount())
            cb->SetSelection(n);
    }
    wxString get_value(int index) override
    {
        wxString str;
        str << index+1;
        return str;
    }
    int index_of(wxString value) override
    {
        long n = 0;
        if(!value.ToLong(&n))
            return -1;
        --n;
        return (n >= 0 && n <= items.size()) ? int(n) : -1;
    }
    void update(bool force = false)
    {
        items.clear();
        if (!force && m_choices.empty())
            return;
        auto icons = get_extruder_color_icons(true);
        auto presets = wxGetApp().preset_bundle->filament_presets;
        for (int i = 0; i < presets.size(); ++i) {
            wxString str;
            std::string type;
            wxGetApp().preset_bundle->filaments.find_preset(presets[i])->get_filament_type(type);
            str << type;
            items.push_back({str, i < icons.size() ? icons[i] : nullptr});
        }
        DynamicList::update();
    }

};

// Check if the machine supports Junction Deviation (Marlin firmware with machine_max_junction_deviation > 0)
static bool has_junction_deviation(const DynamicPrintConfig* printer_config)
{
    if (!printer_config) {
        return false;
    }
    const auto gcode_flavor = printer_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor");
    const auto junction_dev = printer_config->option<ConfigOptionFloats>("machine_max_junction_deviation");
    return gcode_flavor &&
           gcode_flavor->value == GCodeFlavor::gcfMarlinFirmware &&
           junction_dev &&
           !junction_dev->values.empty() &&
           junction_dev->values.front() > 0.0;
}

static DynamicFilamentList dynamic_filament_list;
static DynamicFilamentList1Based dynamic_filament_list_1_based;

class AMSCountPopupWindow : public PopupWindow
{
public:
    AMSCountPopupWindow(ExtruderGroup *extruder, int index)
        : PopupWindow(extruder, wxBORDER_NONE | wxPU_CONTAINS_CONTROLS)
    {
        SetBackgroundColour(*wxWHITE);
        auto msg  = new wxStaticText(this, wxID_ANY, _L("Set the number of AMS installed on the nozzle."));
        msg->SetFont(Label::Body_14);
        msg->SetForegroundColour("#262E30");
        msg->Wrap(FromDIP(280));
        auto box = new StaticBox(this, wxID_ANY);
        box->SetBackgroundColor(0xF8F8F8);
        box->SetBorderWidth(0);
        auto img4 = new ScalableButton(box, wxID_ANY, "ams_4_tray", {}, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, false, 44);
        //img4->SetBackgroundColour(*wxWHITE);
        auto img1 = new ScalableButton(box, wxID_ANY, "ams_1_tray", {}, wxDefaultSize, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, false, 44);
        //img1->SetBackgroundColour(*wxWHITE);
        auto txt4 = new wxStaticText(box, wxID_ANY, _L("AMS(4 slots)"));
        txt4->SetFont(Label::Body_14);
        txt4->SetBackgroundColour(0xF8F8F8);
        txt4->SetForegroundColour("#262E30");
        auto txt1 = new wxStaticText(box, wxID_ANY, _L("AMS(1 slot)"));
        txt1->SetFont(Label::Body_14);
        txt1->SetBackgroundColour(0xF8F8F8);
        txt1->SetForegroundColour("#262E30");
        int ams4 = 0, ams1 = 0;
        int oth4 = 0, oth1 = 0;
        GetAMSCount(index, ams4, ams1);
        GetAMSCount(1 - index, oth4, oth1);
        auto val4          = new SpinInput(box, {}, {}, wxDefaultPosition, {FromDIP(60), -1}, 0, 0, 4 - oth4, ams4);
        auto val1          = new SpinInput(box, {}, {}, wxDefaultPosition, {FromDIP(60), -1}, 0, 0, 8 - oth1, ams1);
        auto event_handler = [index, val4, val1, extruder](auto &evt) {
            SetAMSCount(index, val4->GetValue(), val1->GetValue());
            UpdateAMSCount(index, extruder);
        };
        val4->Bind(wxEVT_SPINCTRL, event_handler);
        val1->Bind(wxEVT_SPINCTRL, event_handler);

        wxSizer * sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(msg, 0, wxTOP | wxLEFT | wxRIGHT, FromDIP(10));
            wxSizer *sizer2  = new wxBoxSizer(wxVERTICAL);
                wxSizer *sizer21 = new wxBoxSizer(wxHORIZONTAL);
                sizer21->Add(img4, 0, wxALIGN_CENTRE);
                sizer21->Add(txt4, 2, wxLEFT | wxALIGN_CENTRE, FromDIP(10));
                sizer21->Add(val4, 1, wxLEFT | wxALIGN_CENTRE, FromDIP(10));
            sizer2->Add(sizer21, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, FromDIP(14));
            sizer2->AddSpacer(FromDIP(6));
                wxSizer *sizer22 = new wxBoxSizer(wxHORIZONTAL);
                sizer22->Add(img1, 0, wxALIGN_CENTRE);
                sizer22->Add(txt1, 2, wxLEFT | wxALIGN_CENTRE, FromDIP(10));
                sizer22->Add(val1, 1, wxLEFT | wxALIGN_CENTRE, FromDIP(10));
            sizer2->Add(sizer22, 0, wxLEFT | wxRIGHT | wxBOTTOM | wxEXPAND, FromDIP(14));
            box->SetSizer(sizer2);
        sizer->Add(box, 0, wxTOP | wxBOTTOM | wxLEFT | wxRIGHT | wxEXPAND, FromDIP(14));
        SetSizer(sizer);

        Layout();
        Fit();

        Bind(wxEVT_PAINT, [this](wxPaintEvent& evt) {
                wxPaintDC dc(this);
                dc.SetPen(StateColor::darkModeColorFor(wxColour("#DBDBDB"))); // ORCA match popup border color
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                dc.DrawRoundedRectangle(0, 0, GetSize().x, GetSize().y, 0);
            });

        SetBackgroundColour(*wxWHITE);
        wxGetApp().UpdateDarkUIWin(this);
    }

    static void SetAMSCount(int index, int ams4, int ams1)
    {
        PresetBundle &preset_bundle = *wxGetApp().preset_bundle;
        preset_bundle.extruder_ams_counts.resize(2);
        auto &ams_map = preset_bundle.extruder_ams_counts[index];
        ams_map[4] = ams4;
        ams_map[1] = ams1;

        std::vector<std::string> extruder_ams_count     = save_extruder_ams_count_to_string(preset_bundle.extruder_ams_counts);
        std::string              extruder_ams_count_str = boost::algorithm::join(extruder_ams_count, ",");
        wxGetApp().app_config->set("presets", "extruder_ams_count", extruder_ams_count_str);
        wxGetApp().plater()->update(); // update slice status
    }

    static void GetAMSCount(int index, int & ams4, int & ams1)
    {
        PresetBundle &preset_bundle = *wxGetApp().preset_bundle;
        if (preset_bundle.extruder_ams_counts.empty()) {
            ams4 = 0;
            ams1 = 0;
        }
        else {
            assert(preset_bundle.extruder_ams_counts.size() == 2);
            ams4 = preset_bundle.extruder_ams_counts[index][4];
            ams1 = preset_bundle.extruder_ams_counts[index][1];
        }
    }

    static void UpdateAMSCount(int index, ExtruderGroup *extruder)
    {
        std::vector<std::map<int, int>> &ams_counts = wxGetApp().preset_bundle->extruder_ams_counts;
        ams_counts.resize(2);
        std::map<int, int>& ams_map = ams_counts[index];
        if (ams_map.find(4) == ams_map.end()) {
            ams_map[4] = 0;
        }
        if (ams_map.find(1) == ams_map.end()) {
            ams_map[1] = 0;
        }

        extruder->set_ams_count(ams_map[4], ams_map[1]);
    }
};

ExtruderGroup::ExtruderGroup(wxWindow * parent, int index, wxString const &title)
    : StaticGroup(parent, wxID_ANY, title)
{
    SetFont(Label::Body_10);
    SetForegroundColour(wxColour("#CECECE"));
    SetBorderColor(wxColour("#EEEEEE"));
    SetCornerRadius(FromDIP(PRINTER_PANEL_RADIUS)); // ORCA match radius with other boxes
    ShowBadge(true);
    // Nozzle
    wxStaticText *label_diameter = new wxStaticText(this, wxID_ANY, _L("Diameter"));
    label_diameter->SetFont(Label::Body_14);
    label_diameter->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#262E30")));
    if (index >= 0) label_diameter->SetMinSize({FromDIP(80), -1});
    auto combo_diameter = new ComboBox(this, wxID_ANY, wxString(""), wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
    this->combo_diameter = combo_diameter;
    wxStaticText *label_flow = new wxStaticText(this, wxID_ANY, _L("Flow"));
    label_flow->SetFont(Label::Body_14);
    label_flow->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#262E30")));
    if (index >= 0) label_flow->SetMinSize({FromDIP(80), -1});
    auto combo_flow = new ComboBox(this, wxID_ANY, wxString(""), wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
    combo_flow->GetDropDown().SetUseContentWidth(true);
    combo_flow->Bind(wxEVT_COMBOBOX, [this, index, combo_flow](wxCommandEvent &evt) {
        auto printer_tab = dynamic_cast<TabPrinter *>(wxGetApp().get_tab(Preset::TYPE_PRINTER));
        printer_tab->set_extruder_volume_type(index, NozzleVolumeType(intptr_t(combo_flow->GetClientData(evt.GetInt()))));
        if (GUI::wxGetApp().plater())
            GUI::wxGetApp().plater()->update_machine_sync_status();
    });
    this->combo_flow = combo_flow;

    // AMS
    wxStaticText *label_ams  = new wxStaticText(this, wxID_ANY, _L("AMS"));
    label_ams->SetFont(Label::Body_14);
    label_ams->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#262E30")));
    //label_ams->SetMinSize({FromDIP(70), -1});
    if (index >= 0) {
        btn_edit = new ScalableButton(this, wxID_ANY, "dot");
#ifdef __WXOSX__
        btn_edit->SetBackgroundColour("#F7F7F7");
#else
        btn_edit->SetBackgroundColour(*wxWHITE);
#endif
        btn_edit->Hide();
        btn_edit->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this, index](auto &evt) {
            PopupWindow *window = new AMSCountPopupWindow(this, index);
            auto         size   = GetSize();
            auto         pos    = ClientToScreen({0, size.y + 12});
            size.SetWidth(size.GetWidth() + FromDIP(10));
            window->Position(pos, {0, 0});
            window->Popup();
        });

        auto hovered = std::make_shared<wxWindow *>();
        for (wxWindow *w : std::initializer_list<wxWindow *>{this, label_diameter, combo_diameter, label_flow, combo_flow, btn_edit, label_ams}) {
            w->Bind(wxEVT_ENTER_WINDOW, [w, hovered, this](wxMouseEvent &evt) { *hovered = w; btn_edit->SetBitmap_("edit"); });
            w->Bind(wxEVT_LEAVE_WINDOW, [w, hovered, this](wxMouseEvent &evt) { if (*hovered == w) { btn_edit->SetBitmap_("dot"); *hovered = nullptr; } });
        }
    }

    // AMS not installed message
    ams_not_installed_msg = new wxStaticText(this, wxID_ANY, _L("Not installed"));
    ams_not_installed_msg->SetFont(Label::Body_14);
    ams_not_installed_msg->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#262E30")));

    // AMS group
    for (size_t i = 0; i < 4; ++i) {
        ams[i] = new AMSPreview(this, wxID_ANY, AMSinfo(), AMSModel::GENERIC_AMS);
        ams[i]->Close();
    }

    hsizer_ams = new wxBoxSizer(wxHORIZONTAL);
    hsizer_ams->SetMinSize(0, ams[0]->GetMinHeight());
    hsizer_ams->Add(label_ams, 0, wxALIGN_CENTER);
    if (btn_edit)
        hsizer_ams->Add(btn_edit, 0, wxLEFT | wxALIGN_CENTER, FromDIP(2));
    hsizer_ams->Add(ams_not_installed_msg, 0, wxALIGN_CENTER);

    btn_up = new ScalableButton(this, wxID_ANY, "page_up", "", {FromDIP(14), FromDIP(14)}, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, false, 14);
    btn_up->SetBackgroundColour(*wxWHITE);
    btn_up->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this, index](auto &evt) {
        if (page_cur > 0)
            --page_cur;
        update_ams();
    });
    btn_up->Hide();
    btn_down = new ScalableButton(this, wxID_ANY, "page_down", "", {FromDIP(14), FromDIP(14)}, wxDefaultPosition, wxBU_EXACTFIT | wxNO_BORDER, false, 14);
    btn_down->SetBackgroundColour(*wxWHITE);
    btn_down->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this, index](auto &evt) {
        if (page_cur + 1 < page_num)
            ++page_cur;
        update_ams();
    });
    btn_down->Hide();

    wxBoxSizer *hsizer_diameter = new wxBoxSizer(wxHORIZONTAL);
    hsizer_diameter->Add(label_diameter, 0, wxALIGN_CENTER);
    hsizer_diameter->Add(combo_diameter, 1, wxEXPAND);
    wxBoxSizer * hsizer_nozzle = new wxBoxSizer(wxHORIZONTAL);
    hsizer_nozzle->Add(label_flow, 0, wxALIGN_CENTER);
    hsizer_nozzle->Add(combo_flow, 1, wxEXPAND);
    label_flow->Hide(); // TODO: Orca hack, hide flow selection
    combo_flow->Hide();
    if (index < 0) {
        label_ams->Hide();
        ams_not_installed_msg->Hide();
        wxStaticBoxSizer *hsizer     = new wxStaticBoxSizer(this, wxHORIZONTAL);
        hsizer->Add(hsizer_diameter, 1, wxEXPAND | wxTOP| wxBOTTOM, FromDIP(8));
        //hsizer->Add(hsizer_nozzle, 1, wxEXPAND | wxALL, FromDIP(8));
        hsizer->AddSpacer(FromDIP(2)); // Avoid badge
        this->sizer = hsizer;
    } else {
        wxStaticBoxSizer *vsizer = new wxStaticBoxSizer(this, wxVERTICAL);
        vsizer->Add(hsizer_ams, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(2));
        vsizer->Add(hsizer_diameter, 0, wxEXPAND | wxLEFT | wxTOP | wxRIGHT | wxBOTTOM, FromDIP(2));
        //vsizer->Add(hsizer_nozzle, 0, wxEXPAND | wxALL, FromDIP(2));
        this->sizer = vsizer;
    }
    AMSCountPopupWindow::UpdateAMSCount(index < 0 ? 0 : index, this);
}

void ExtruderGroup::update_ams()
{
    static AMSinfo info4;
    static AMSinfo info1;
    if (info4.cans.empty()) {
        for (size_t i = 0; i < 4; ++i) info4.cans.push_back({});
        info1.ams_type = AMSModel::N3S_AMS;
        info1.cans.push_back({});
    }

    if (btn_edit == nullptr)
        return;

    page_num  = (ams_n4 * 2 + ams_n1 + 3) / 4;
    size_t i4 = page_cur * 2;
    size_t i1 = 0;
    if (i4 > ams_n4) {
        i1 = (i4 - ams_n4) * 2;
        i4 = ams_n4;
    }

    size_t left  = 4;
    size_t index = 0;
    for (size_t i = i4; i < ams_n4 && left > 0; ++i, ++index, left -= 2) {
        ams[index]->Update(i < ams_4.size() ? ams_4[i] : info4);
        ams[index]->Refresh();
        ams[index]->Open();
    }
    for (size_t i = i1; i < ams_n1 && left > 0; ++i, ++index, --left) {
        ams[index]->Update(i < ams_1.size() ? ams_1[i] : info1);
        ams[index]->Refresh();
        ams[index]->Open();
    }
    for (; index < 4; ++index)
        ams[index]->Close();

    ams_not_installed_msg->Show(ams_n4 == 0 && ams_n1 == 0);
    btn_up->Show(page_cur > 0);
    btn_down->Show(page_cur + 1 < page_num);

    while (hsizer_ams->GetItemCount() > 2)
        hsizer_ams->Remove(2);
    if (ams_not_installed_msg->IsShown()) {
        hsizer_ams->AddStretchSpacer(1);
        hsizer_ams->Add(ams_not_installed_msg, 0, wxALIGN_CENTER);
        hsizer_ams->AddStretchSpacer(1);
    }
    for (size_t i = 0; i < 4; ++i) {
        if (ams[i]->IsShown())
            hsizer_ams->Add(this->ams[i], 0, wxLEFT, FromDIP(1));
    }
    if (btn_up->IsShown() || btn_down->IsShown()) {
        if (btn_edit)
            hsizer_ams->AddStretchSpacer(1);
        if (btn_up->IsShown() && btn_down->IsShown()) {
            auto vsizer_btn = new wxBoxSizer(wxVERTICAL);
            auto size = btn_up->GetSize();
            vsizer_btn->Add(btn_up, 0);
            vsizer_btn->Add(btn_down, 0);
            hsizer_ams->Add(vsizer_btn, 0, wxALIGN_CENTER | wxLEFT, FromDIP(2));
        } else if (btn_up->IsShown()) {
            hsizer_ams->Add(btn_up, 0, wxALIGN_CENTER | wxLEFT, FromDIP(2));
        } else {
            hsizer_ams->Add(btn_down, 0, wxALIGN_CENTER | wxLEFT, FromDIP(2));
        }
    }

    sizer->Layout();
}

void ExtruderGroup::sync_ams(MachineObject const *obj, std::vector<DevAms *> const &ams4, std::vector<DevAms *> const &ams1)
{
    if (ams_4.empty() && ams4.empty()
            && ams_1.empty() && ams1.empty())
        return;
    auto sync = [obj](std::vector<AMSinfo> &infos, std::vector<DevAms *> const &ams) -> bool {
        std::vector<AMSinfo> infos2;
        for (auto a : ams) {
            AMSinfo ams_info;
            ams_info.parse_ams_info(const_cast<MachineObject*>(obj), a, obj->GetFilaSystem()->IsDetectRemainEnabled(), obj->is_support_ams_humidity);
            infos2.push_back(ams_info);
        }
        if (infos == infos2)
            return false;
        infos.swap(infos2);
        return true;
    };
    if (sync(ams_4, ams4) || sync(ams_1, ams1))
        update_ams();
}

bool Sidebar::priv::switch_diameter(bool single)
{
    wxString diameter;
    if (single) {
        diameter = single_extruder->combo_diameter->GetValue();
    } else {
        auto diameter_left = left_extruder->combo_diameter->GetValue();
        auto diameter_right = right_extruder->combo_diameter->GetValue();
        if (diameter_left != diameter_right) {
            MessageDialog dlg(this->plater,
                              _L("The software does not support using different diameter of nozzles for one print. "
                                 "If the left and right nozzles are inconsistent, we can only proceed with single-head printing. "
                                 "Please confirm which nozzle you would like to use for this project."),
                              _L("Switch diameter"), wxYES_NO | wxNO_DEFAULT);
            dlg.SetButtonLabel(wxID_YES, wxString::Format(_L("Left nozzle: %smm"), diameter_left));
            dlg.SetButtonLabel(wxID_NO, wxString::Format(_L("Right nozzle: %smm"), diameter_right));
            int result = dlg.ShowModal();
            if (result == wxID_YES)
                diameter = diameter_left;
            else if (result == wxID_NO)
                diameter = diameter_right;
            else
                return false;
        }
        else {
            diameter = diameter_left;
        }
    }
    auto preset          = wxGetApp().preset_bundle->get_similar_printer_preset({}, diameter.ToStdString());
    if (preset == nullptr) {
        // ORCA add a text. this appears when user tries to change nozzle value but config doesnt have a inherited or compatible preset
        MessageDialog dlg(this->plater, _L("Configuration incompatible"), _L("Warning"), wxICON_WARNING | wxOK);
        dlg.ShowModal();
        return false;
    }
    preset->is_visible = true; // force visible
    return wxGetApp().get_tab(Preset::TYPE_PRINTER)->select_preset(preset->name);
}

bool Sidebar::priv::sync_extruder_list(bool &only_external_material)
{
    MachineObject *obj = wxGetApp().getDeviceManager()->get_selected_machine();
    auto           printer_name = plater->get_selected_printer_name_in_combox();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << __LINE__ << " begin sync_extruder_list";
    if (obj == nullptr) {
        plater->pop_warning_and_go_to_device_page(printer_name, Plater::PrinterWarningType::NOT_CONNECTED, _L("Sync printer information"));
        return false;
    }
    //if (obj->get_extder_system()->extders.size() != 2) {//wxString(obj->get_preset_printer_model_name(machine_print_name))
    //    plater->pop_warning_and_go_to_device_page(printer_name, Plater::PrinterWarningType::INCONSISTENT, _L("Sync printer information"));
    //    return false;
    //}

    if (!plater->check_printer_initialized(obj)) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << __LINE__ << " check_printer_initialized fail";
        return false;
    }

    std::string machine_print_name = obj->get_show_printer_type();
    PresetBundle *preset_bundle = wxGetApp().preset_bundle;
    std::string target_model_id  = preset_bundle->printers.get_selected_preset().get_printer_type(preset_bundle);
    Preset* machine_preset = get_printer_preset(obj);
    if (!machine_preset) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << __LINE__ << "check error: machine_preset empty";
        return false;
    }
    if (machine_print_name != target_model_id) {
        MessageDialog dlg(this->plater, _L("The currently selected machine preset is inconsistent with the connected printer type.\n"
                                            "Are you sure to continue syncing?"), _L("Sync printer information"), wxICON_WARNING | wxYES | wxNO);
        if (dlg.ShowModal() == wxID_NO) {
            return false;
        }

        if (!this->plater)
            return false;

        this->plater->update_objects_position_when_select_preset([&obj, machine_preset]() {
            Tab *printer_tab = GUI::wxGetApp().get_tab(Preset::Type::TYPE_PRINTER);
            printer_tab->select_preset(machine_preset->name);
        });
    }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << __LINE__ << " go on sync_extruder_list";
    const Preset &cur_preset  = preset_bundle->printers.get_selected_preset();
    int extruder_nums = preset_bundle->get_printer_extruder_count();
    std::vector<int> extruder_map(extruder_nums);
    std::iota(extruder_map.begin(), extruder_map.end(), 0);
    const ConfigOptionInts *physical_extruder_map = cur_preset.config.option<ConfigOptionInts>("physical_extruder_map");
    if (physical_extruder_map != nullptr) {
        assert(physical_extruder_map->values.size() == extruder_nums);
        extruder_map = physical_extruder_map->values;
    }
    assert(obj->GetExtderSystem()->GetTotalExtderCount() == extruder_nums);

    std::vector<float> nozzle_diameters;
    nozzle_diameters.resize(extruder_nums);
    for (size_t index = 0; index < extruder_nums; ++index) {
        int extruder_id = extruder_map[index];
        nozzle_diameters[extruder_id] = obj->GetExtderSystem()->GetNozzleDiameter(index);
        NozzleVolumeType target_type = NozzleVolumeType::nvtStandard;
        auto printer_tab = dynamic_cast<TabPrinter *>(wxGetApp().get_tab(Preset::TYPE_PRINTER));
        if (obj->is_nozzle_flow_type_supported()) {
            if (obj->GetExtderSystem()->GetNozzleFlowType(index) == NozzleFlowType::NONE_FLOWTYPE) {
                MessageDialog dlg(this->plater, _L("There are unset nozzle types. Please set the nozzle types of all extruders before synchronizing."),
                                  _L("Sync extruder infomation"), wxICON_WARNING | wxOK);
                dlg.ShowModal();
                continue;
            }
            // hack code, only use standard flow for 0.2
            if (std::fabs(nozzle_diameters[extruder_id] - 0.2) > EPSILON)
                target_type = NozzleVolumeType(obj->GetExtderSystem()->GetNozzleFlowType(extruder_id) - 1);
        }
        printer_tab->set_extruder_volume_type(index, target_type);
    }

    int deputy_4 = 0, main_4 = 0, deputy_1 = 0, main_1 = 0;
    for (auto ams : obj->GetFilaSystem()->GetAmsList()) {
        // Main (first) extruder at right
        if (ams.second->GetExtruderId() == 0) {
            if (ams.second->GetAmsType() == DevAms::N3S) // N3S
                ++main_1;
            else
                ++main_4;
        } else if (ams.second->GetExtruderId() == 1) {
            if (ams.second->GetAmsType() == DevAms::N3S) // N3S
                ++deputy_1;
            else
                ++deputy_4;
        }
    }
    only_external_material = !obj->GetFilaSystem()->HasAms();
    int main_index = obj->is_main_extruder_on_left() ? 0 : 1;
    int deputy_index = obj->is_main_extruder_on_left() ? 1 : 0;

    if (extruder_nums > 1) {
        int left_index  = left_extruder->combo_diameter->FindString(get_diameter_string(nozzle_diameters[0]));
        int right_index = left_extruder->combo_diameter->FindString(get_diameter_string(nozzle_diameters[1]));
        assert(left_index != -1 && right_index != -1);
        left_extruder->combo_diameter->SetSelection(left_index);
        right_extruder->combo_diameter->SetSelection(right_index);
        is_switching_diameter = true;
        switch_diameter(false);
        is_switching_diameter = false;
        AMSCountPopupWindow::SetAMSCount(deputy_index, deputy_4, deputy_1);
        AMSCountPopupWindow::SetAMSCount(main_index, main_4, main_1);
        AMSCountPopupWindow::UpdateAMSCount(0, left_extruder);
        AMSCountPopupWindow::UpdateAMSCount(1, right_extruder);
    } else {
        int index = single_extruder->combo_diameter->FindString(get_diameter_string(nozzle_diameters[0]));
        assert(index != -1);
        single_extruder->combo_diameter->SetSelection(index);
        is_switching_diameter = true;
        switch_diameter(true);
        is_switching_diameter = false;
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << __LINE__ << " finish sync_extruder_list";
    return true;
}

void Sidebar::priv::update_sync_status(const MachineObject *obj)
{
    StateColor not_synced_colour(std::pair<wxColour, int>(wxColour("#009688"), StateColor::Normal));
    auto clear_all_sync_status = [this, &not_synced_colour]() {
        panel_printer_preset->ShowBadge(false);
        panel_printer_bed->ShowBadge(false);
        panel_nozzle_dia->ShowBadge(false); // ORCA add support for nozzle sync
        left_extruder->ShowBadge(false);
        left_extruder->sync_ams(nullptr, {}, {});
        right_extruder->ShowBadge(false);
        right_extruder->sync_ams(nullptr, {}, {});
        single_extruder->ShowBadge(false);
        single_extruder->sync_ams(nullptr, {}, {});
        //btn_sync_printer->SetBorderColor(not_synced_colour);
        //btn_sync_printer->SetIcon("printer_sync");
        m_printer_bbl_sync->SetBitmap_("printer_sync_not");
    };

    if (!obj || !obj->is_info_ready()) {
        clear_all_sync_status();
        return;
    }

    PresetBundle *preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle) {
        clear_all_sync_status();
        return;
    }

    bool printer_synced = false;
    // 1. update printer status
    const Preset &cur_preset = wxGetApp().preset_bundle->printers.get_edited_preset();
    if (preset_bundle && preset_bundle->printers.get_edited_preset().get_printer_type(preset_bundle) == obj->get_show_printer_type()) {
        panel_printer_preset->ShowBadge(true);
        printer_synced = true;

        wxGetApp().plater()->sidebar().udpate_combos_filament_badge();
    } else {
        clear_all_sync_status();

        wxGetApp().plater()->sidebar().clear_combos_filament_badge();
        return;
    }

    struct ExtruderInfo
    {
        float diameter{0.4};
        //int   nozzle_volue_type{0};
        int   ams_4{0};
        int   ams_1{0};
        std::vector<DevAms *> ams_v4;
        std::vector<DevAms *> ams_v1;

        bool operator==(const ExtruderInfo &other) const
        {
            return abs(diameter - other.diameter) < EPSILON
                && /*nozzle_volue_type == other.nozzle_volue_type
                &&*/ ams_4 == other.ams_4
                && ams_1 == other.ams_1;
        }
    };

    auto is_same_nozzle_info = [obj](const ExtruderInfo &left, const ExtruderInfo &right) {
        bool is_same_nozzle_type = true;
        if (obj->is_nozzle_flow_type_supported())
            is_same_nozzle_type = true;//left.nozzle_volue_type == right.nozzle_volue_type; // TODO: Orca hack
        return abs(left.diameter - right.diameter) < EPSILON && is_same_nozzle_type;
    };

    // 2. update extruder status
    int extruder_nums = preset_bundle->get_printer_extruder_count();
    if (extruder_nums != obj->GetExtderSystem()->GetTotalExtderCount())
        return;

    std::vector<ExtruderInfo> extruder_infos(extruder_nums);
    std::vector<int> nozzle_volume_types = wxGetApp().preset_bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type")->values;
    //for (size_t i = 0; i < nozzle_volume_types.size(); ++i) {
    //    extruder_infos[i].nozzle_volue_type = nozzle_volume_types[i];
    //}

    std::vector<std::map<int, int>> extruder_ams_counts = wxGetApp().preset_bundle->extruder_ams_counts;
    if (extruder_ams_counts.size() >= extruder_nums) {
        for (size_t i = 0; i < extruder_nums; ++i) {
            for (auto iter = extruder_ams_counts[i].begin(); iter != extruder_ams_counts[i].end(); ++iter) {
                if (iter->first == 4)
                    extruder_infos[i].ams_4 = iter->second;
                if (iter->first == 1)
                    extruder_infos[i].ams_1 = iter->second;
            }
        }
    }

    if (extruder_nums == 1) {
        double value = 0.0;
        single_extruder->diameter.ToDouble(&value);
        extruder_infos[0].diameter = float(value);
    }
    else if(extruder_nums == 2){
        double value = 0.0;
        left_extruder->diameter.ToDouble(&value);
        extruder_infos[0].diameter = float(value);
    
        value = 0.0;
        right_extruder->diameter.ToDouble(&value);
        extruder_infos[1].diameter = float(value);
    }

    std::vector<ExtruderInfo> machine_extruder_infos(obj->GetExtderSystem()->GetTotalExtderCount());

    const auto& extruders = obj->GetExtderSystem()->GetExtruders();
    for (const DevExtder &extruder : extruders) {
        //machine_extruder_infos[extruder.GetExtId()].nozzle_volue_type = int(extruder.GetNozzleFlowType()) - 1;
        machine_extruder_infos[extruder.GetExtId()].diameter          = extruder.GetNozzleDiameter();
    }
    for (auto &item : obj->GetFilaSystem()->GetAmsList()) {
        if (item.second->GetExtruderId() >= machine_extruder_infos.size())
            continue;

        if (item.second->GetAmsType() == DevAms::N3S)
        { // N3S
            machine_extruder_infos[item.second->GetExtruderId()].ams_1++;
            machine_extruder_infos[item.second->GetExtruderId()].ams_v1.push_back(item.second);
        } else {
            machine_extruder_infos[item.second->GetExtruderId()].ams_4++;
            machine_extruder_infos[item.second->GetExtruderId()].ams_v4.push_back(item.second);
        }
    }

    std::reverse(machine_extruder_infos.begin(), machine_extruder_infos.end());

    std::vector<bool> extruder_synced(extruder_nums, false);
    if (extruder_nums == 1) {
        if (is_same_nozzle_info(extruder_infos[0], machine_extruder_infos[0])) {
            single_extruder->ShowBadge(true);
            panel_nozzle_dia->ShowBadge(true); // ORCA add support for nozzle sync
            single_extruder->sync_ams(obj, machine_extruder_infos[0].ams_v4, machine_extruder_infos[0].ams_v1);
            extruder_synced[0] = true;
        }
        else {
            single_extruder->ShowBadge(false);
            panel_nozzle_dia->ShowBadge(false); // ORCA add support for nozzle sync
            single_extruder->sync_ams(obj, {}, {});
        }
    }
    else if (extruder_nums == 2) {
        if (extruder_infos[0] == machine_extruder_infos[0]) {
            left_extruder->ShowBadge(true);
            left_extruder->sync_ams(obj, machine_extruder_infos[0].ams_v4, machine_extruder_infos[0].ams_v1);
            extruder_synced[0] = true;
        }
        else {
            left_extruder->ShowBadge(false);
            left_extruder->sync_ams(obj, {}, {});
        }

        if (extruder_infos[1] == machine_extruder_infos[1]) {
            right_extruder->ShowBadge(true);
            right_extruder->sync_ams(obj, machine_extruder_infos[1].ams_v4, machine_extruder_infos[1].ams_v1);
            extruder_synced[1] = true;
        }
        else {
            right_extruder->ShowBadge(false);
            right_extruder->sync_ams(obj, {}, {});
        }
    }

    StateColor synced_colour(std::pair<wxColour, int>(wxColour("#CECECE"), StateColor::Normal));
    bool all_extruder_synced = std::all_of(extruder_synced.begin(), extruder_synced.end(), [](bool value) { return value; });
    if (printer_synced && all_extruder_synced) {
    //    btn_sync_printer->SetBorderColor(synced_colour);
    //    btn_sync_printer->SetIcon("ams_nozzle_sync");
        m_printer_bbl_sync->SetBitmap_("printer_sync_ok");
    }
    else {
    //    btn_sync_printer->SetBorderColor(not_synced_colour);
    //    btn_sync_printer->SetIcon("printer_sync");
        m_printer_bbl_sync->SetBitmap_("printer_sync_not");
    }
 }

void Sidebar::update_sync_ams_btn_enable(wxUpdateUIEvent &e)
 {
     if (m_last_slice_state != p->plater->is_background_process_slicing()) {
         m_last_slice_state = p->plater->is_background_process_slicing();
         //btn_sync->Enable(!m_last_slice_state);
         p->m_printer_bbl_sync->Enable(!m_last_slice_state);
         ams_btn->Enable(!m_last_slice_state);
         Refresh();
     }
 }

Sidebar::Sidebar(Plater *parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(42 * wxGetApp().em_unit(), -1)), p(new priv(parent))
{
    Choice::register_dynamic_list("support_filament", &dynamic_filament_list);
    Choice::register_dynamic_list("support_interface_filament", &dynamic_filament_list);
    Choice::register_dynamic_list("wall_filament", &dynamic_filament_list_1_based);
    Choice::register_dynamic_list("sparse_infill_filament", &dynamic_filament_list_1_based);
    Choice::register_dynamic_list("solid_infill_filament", &dynamic_filament_list_1_based);
    Choice::register_dynamic_list("wipe_tower_filament", &dynamic_filament_list);

    p->scrolled = new wxPanel(this);
    //    p->scrolled->SetScrollbars(0, 100, 1, 2); // ys_DELETE_after_testing. pixelsPerUnitY = 100
    // but this cause the bad layout of the sidebar, when all infoboxes appear.
    // As a result we can see the empty block at the bottom of the sidebar
    // But if we set this value to 5, layout will be better
    //p->scrolled->SetScrollRate(0, 5);
    p->scrolled->SetBackgroundColour(*wxWHITE);


    SetFont(wxGetApp().normal_font());
#ifndef __APPLE__
#ifdef _WIN32
    wxGetApp().UpdateDarkUI(this);
    wxGetApp().UpdateDarkUI(p->scrolled);
#else
    SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
#endif
#endif

    int em = wxGetApp().em_unit();
    //BBS refine layout and styles
    // Sizer in the scrolled area
    auto* scrolled_sizer = m_scrolled_sizer = new wxBoxSizer(wxVERTICAL);
    p->scrolled->SetSizer(scrolled_sizer);

    wxColour title_bg = wxColour(248, 248, 248);
    wxColour inactive_text = wxColour(86, 86, 86);
    wxColour active_text = wxColour(0, 0, 0);
    wxColour static_line_col = wxColour(166, 169, 170);

#ifdef __WINDOWS__
    p->scrolled->SetDoubleBuffered(true);
#endif //__WINDOWS__

    // add printer
    {
        /***************** 1. create printer title bar    **************/
        // 1.1 create title bar resources
        p->m_panel_printer_title = new StaticBox(p->scrolled, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE);
        p->m_panel_printer_title->SetBackgroundColor(title_bg);
        p->m_panel_printer_title->SetBackgroundColor2(0xF1F1F1);

        p->m_printer_icon = new ScalableButton(p->m_panel_printer_title, wxID_ANY, "printer");
        p->m_text_printer_settings = new Label(p->m_panel_printer_title, _L("Printer"), LB_PROPAGATE_MOUSE_EVENT);

        p->m_printer_icon->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
            //auto wizard_t = new ConfigWizard(wxGetApp().mainframe);
            //wizard_t->run(ConfigWizard::RR_USER, ConfigWizard::SP_CUSTOM);
            });

        // ORCA use connect button on titlebar
        p->m_printer_connect = new ScalableButton(p->m_panel_printer_title, wxID_ANY, "monitor_signal_strong");
        p->m_printer_connect->SetToolTip(_L("Connection"));
        p->m_printer_connect->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
            PhysicalPrinterDialog dlg(this->GetParent());
            dlg.ShowModal();
        });

        // ORCA use sync button on titlebar
        p->m_printer_bbl_sync = new ScalableButton(p->m_panel_printer_title, wxID_ANY, "printer_sync_not");
        p->m_printer_bbl_sync->SetToolTip(_L("Synchronize nozzle information and the number of AMS"));
        p->m_printer_bbl_sync->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
            deal_btn_sync();
        });

        p->m_printer_setting = new ScalableButton(p->m_panel_printer_title, wxID_ANY, "settings");
        p->m_printer_setting->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
            // p->editing_filament = -1;
            // wxGetApp().params_dialog()->Popup();
            // wxGetApp().get_tab(Preset::TYPE_FILAMENT)->restore_last_select_item();
            wxGetApp().run_wizard(ConfigWizard::RR_USER, ConfigWizard::SP_PRINTERS);
            });

        wxBoxSizer* h_sizer_title = new wxBoxSizer(wxHORIZONTAL);
        h_sizer_title->Add(p->m_printer_icon, 0, wxALIGN_CENTRE | wxLEFT, FromDIP(SidebarProps::TitlebarMargin()));
        h_sizer_title->AddSpacer(FromDIP(SidebarProps::ElementSpacing()));
        h_sizer_title->Add(p->m_text_printer_settings, 0, wxALIGN_CENTER);
        h_sizer_title->AddStretchSpacer();
        h_sizer_title->Add(p->m_printer_connect , 0, wxALIGN_CENTER | wxRIGHT, FromDIP(SidebarProps::WideSpacing())); // used larger margin to prevent accidental clicks
        h_sizer_title->Add(p->m_printer_bbl_sync, 0, wxALIGN_CENTER | wxRIGHT, FromDIP(SidebarProps::WideSpacing())); // used larger margin to prevent accidental clicks
        h_sizer_title->Add(p->m_printer_setting, 0, wxALIGN_CENTER);
        h_sizer_title->AddSpacer(FromDIP(SidebarProps::TitlebarMargin()));
        h_sizer_title->SetMinSize(-1, 3 * em);

        p->m_panel_printer_title->SetSizer(h_sizer_title);
        p->m_panel_printer_title->Layout();

        // 1.2 Add spliters around title bar
        // add spliter 1
        //auto spliter_1 = new ::StaticLine(p->scrolled);
        //spliter_1->SetBackgroundColour("#A6A9AA");
        //scrolled_sizer->Add(spliter_1, 0, wxEXPAND);

        // add printer title
        scrolled_sizer->Add(p->m_panel_printer_title, 0, wxEXPAND | wxALL, 0);
        p->m_panel_printer_title->Bind(wxEVT_LEFT_UP, [this] (auto & e) {
            p->m_panel_printer_content->Show(!p->m_panel_printer_content->IsShown());
            m_scrolled_sizer->Layout();
        });

        // add spliter 2
        auto spliter_2 = new ::StaticLine(p->scrolled);
        spliter_2->SetLineColour("#CECECE");
        scrolled_sizer->Add(spliter_2, 0, wxEXPAND);


        /*************************** 2. add printer content ************************/

        p->m_panel_printer_content = new wxPanel(p->scrolled, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
        p->m_panel_printer_content->SetBackgroundColour(wxColour(255, 255, 255));

        struct PanelColors {
            wxColour bg_normal = "#FFFFFF";
            wxColour bg_focus  = "#E5F0EE";
            wxColour bd_normal = "#DBDBDB";
            wxColour bd_hover  = "#009688";
            wxColour bd_focus  = "#009688";
        };
        PanelColors panel_color;

        p->panel_printer_preset = new StaticBox(p->m_panel_printer_content);
        p->panel_printer_preset->SetCornerRadius(FromDIP(PRINTER_PANEL_RADIUS));
        p->panel_printer_preset->SetBorderColor(panel_color.bd_normal);
        p->panel_printer_preset->SetMinSize(FromDIP(PRINTER_PANEL_SIZE));
        p->panel_printer_preset->Bind(wxEVT_LEFT_DOWN, [this](auto & evt) {
            p->combo_printer->wxEvtHandler::ProcessEvent(evt);
        });
        // ORCA Hide Cover automatically if there is not enough space
        p->panel_printer_preset->Bind(wxEVT_SIZE, [this](auto & e) {
            auto current_width = e.GetSize().GetWidth();
            auto narrow_width  = FromDIP(235);
            auto label_width   = p->combo_printer->GetTextExtent(p->combo_printer->GetStringSelection()).GetWidth(); 
            auto min_width     = label_width + FromDIP(25  + PRINTER_PANEL_SIZE.GetWidth());
            if(((min_width < narrow_width && min_width > current_width) || (current_width < narrow_width && min_width > narrow_width)) && p->image_printer->IsShown())
                p->image_printer->Hide();
            else if((current_width > min_width || !(current_width < narrow_width)) && !p->image_printer->IsShown())
                p->image_printer->Show();
            e.Skip();
        });

        p->btn_edit_printer = new ScalableButton(p->panel_printer_preset, wxID_ANY, "edit");
        p->btn_edit_printer->SetToolTip(_L("Click to edit preset"));
        p->btn_edit_printer->Hide(); // hide for first launch
        p->btn_edit_printer->Bind(wxEVT_BUTTON, [this, panel_color](wxCommandEvent){
            p->editing_filament = -1;
            if (p->combo_printer->switch_to_tab())
                p->editing_filament = 0;
            // ORCA clicking edit button not triggers wxEVT_KILL_FOCUS wxEVT_LEAVE_WINDOW make changes manually to prevent stucked colors when opening printer settings
            p->panel_printer_preset->SetBorderColor(panel_color.bd_normal);
            p->btn_edit_printer->Hide();
            p->panel_printer_preset->Layout();
        });

        ScalableBitmap bitmap_printer(p->panel_printer_preset, "printer_placeholder", PRINTER_THUMBNAIL_SIZE.GetHeight());
        p->image_printer = new wxStaticBitmap(p->panel_printer_preset, wxID_ANY, bitmap_printer.bmp(), wxDefaultPosition, FromDIP(PRINTER_THUMBNAIL_SIZE), 0);
        p->image_printer->Bind(wxEVT_LEFT_DOWN, [this](auto &evt) {
            p->combo_printer->wxEvtHandler::ProcessEvent(evt);
        });

        p->combo_printer = new PlaterPresetComboBox(p->panel_printer_preset, Preset::TYPE_PRINTER);
        p->combo_printer->SetBorderWidth(0);
        p->combo_printer->SetMaxSize(wxSize(-1, FromDIP(30))); // limiting height makes badge visible
        // ORCA paint whole combobox on focus
        auto printer_focus_bg = [this, panel_color](bool focused){
            auto bg_color = StateColor::darkModeColorFor(focused ? panel_color.bg_focus : panel_color.bg_normal);
            p->panel_printer_preset->SetBackgroundColor(bg_color);
            p->panel_printer_preset->SetBorderColor(focused ? panel_color.bd_focus : panel_color.bd_normal);
            p->btn_edit_printer->SetBackgroundColour(bg_color);
            p->image_printer->SetBackgroundColour(bg_color);
            p->combo_printer->SetBackgroundColour(bg_color); // paints margins instead combo background
        };
        p->combo_printer->Bind(wxEVT_SET_FOCUS,  [this, printer_focus_bg](auto& e) {printer_focus_bg(true ); e.Skip();});
        p->combo_printer->Bind(wxEVT_KILL_FOCUS, [this, printer_focus_bg](auto& e) {printer_focus_bg(false); e.Skip();});

        /* ORCA This part moved to titlebar
        p->btn_connect_printer = new ScalableButton(p->panel_printer_preset, wxID_ANY, "monitor_signal_strong");
        p->btn_connect_printer->SetBackgroundColour(wxColour(255, 255, 255));
        p->btn_connect_printer->SetToolTip(_L("Connection"));
        p->btn_connect_printer->Bind(wxEVT_BUTTON, [this, combo_printer](wxCommandEvent)
            {
                PhysicalPrinterDialog dlg(this->GetParent());
                dlg.ShowModal();
            });
        */
        // ORCA use Show/Hide to gain text area instead using blank icon. also manages hover effect for border
        for (wxWindow *w : std::initializer_list<wxWindow *>{p->panel_printer_preset, p->btn_edit_printer, p->image_printer, p->combo_printer}) {
            w->Bind(wxEVT_ENTER_WINDOW, [this, panel_color](wxMouseEvent &e) {
                if(!p->combo_printer->HasFocus())
                    p->panel_printer_preset->SetBorderColor(panel_color.bd_hover);
                if(!p->btn_edit_printer->IsShown()){
                    p->btn_edit_printer->Show();
                    p->panel_printer_preset->Layout();
                }
                e.Skip();
            });
            w->Bind(wxEVT_LEAVE_WINDOW, [this, panel_color](wxMouseEvent &e) {
                wxWindow* next_w = wxFindWindowAtPoint(wxGetMousePosition());
                if (!next_w || !p->panel_printer_preset->IsDescendant(next_w)){
                    if(!p->combo_printer->HasFocus())
                        p->panel_printer_preset->SetBorderColor(panel_color.bd_normal);
                    p->btn_edit_printer->Hide();
                    p->panel_printer_preset->Layout();
                }
                e.Skip();
            });
        }

        // ORCA unified Nozzle diameter selection
        p->panel_nozzle_dia = new StaticBox(p->m_panel_printer_content);
        p->panel_nozzle_dia->SetCornerRadius(FromDIP(PRINTER_PANEL_RADIUS));
        p->panel_nozzle_dia->SetBorderColor(panel_color.bd_normal);
        p->panel_nozzle_dia->SetMinSize(FromDIP(PRINTER_PANEL_SIZE));
        p->panel_nozzle_dia->Bind(wxEVT_LEFT_DOWN, [this](auto & evt) {
            p->combo_nozzle_dia->wxEvtHandler::ProcessEvent(evt);
        });

        p->label_nozzle_title = new Label(p->panel_nozzle_dia, _L("Nozzle"), LB_PROPAGATE_MOUSE_EVENT);
        p->label_nozzle_title->SetFont(Label::Body_10);

        p->combo_nozzle_dia = new ComboBox(p->panel_nozzle_dia, wxID_ANY, wxString(""), wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
        p->combo_nozzle_dia->SetBorderWidth(0);
        p->combo_nozzle_dia->GetDropDown().SetUseContentWidth(true);
        p->combo_nozzle_dia->SetMinSize(FromDIP(wxSize(PRINTER_PANEL_SIZE.GetWidth() - 4, 26))); // requires a static value in here
        p->combo_nozzle_dia->SetMaxSize(FromDIP(wxSize(PRINTER_PANEL_SIZE.GetWidth() - 4, 26))); // using -1 with wxEXPAND has issues
        p->combo_nozzle_dia->Bind(wxEVT_COMBOBOX, [this](auto &e) {
            auto evt_combo = (*p->single_extruder).combo_diameter;
            evt_combo->SetSelection(e.GetSelection());
            wxCommandEvent evt(wxEVT_COMBOBOX, evt_combo->GetId());
            evt.SetEventObject(evt_combo);
            evt.SetInt(e.GetSelection());
            wxPostEvent(evt_combo, evt);
            e.Skip();
        });
        // ORCA paint whole combobox on focus
        auto nozzle_focus_bg = [this, panel_color](bool focused){
            auto bg_color = StateColor::darkModeColorFor(focused ? panel_color.bg_focus : panel_color.bg_normal);
            p->panel_nozzle_dia->SetBackgroundColor(bg_color);
            p->panel_nozzle_dia->SetBorderColor(focused ? panel_color.bd_focus : panel_color.bd_normal);
            p->label_nozzle_title->SetBackgroundColour(bg_color);
            p->label_nozzle_type->SetBackgroundColour(bg_color);
            p->combo_nozzle_dia->SetBackgroundColour(bg_color); // paints margins instead combo background
        };
        p->combo_nozzle_dia->Bind(wxEVT_SET_FOCUS,  [this, nozzle_focus_bg](auto& e) {nozzle_focus_bg(true ); e.Skip();});
        p->combo_nozzle_dia->Bind(wxEVT_KILL_FOCUS, [this, nozzle_focus_bg](auto& e) {nozzle_focus_bg(false); e.Skip();});

        p->label_nozzle_type = new Label(p->panel_nozzle_dia, "Brass", LB_PROPAGATE_MOUSE_EVENT | wxST_ELLIPSIZE_END | wxALIGN_CENTRE_HORIZONTAL);
        p->label_nozzle_type->SetFont(Label::Body_10);
        p->label_nozzle_type->SetMinSize(FromDIP(wxSize(56, -1)));
        p->label_nozzle_type->SetMaxSize(FromDIP(wxSize(56, -1)));

        // highlight border on hover
        for (wxWindow *w : std::initializer_list<wxWindow *>{p->panel_nozzle_dia, p->label_nozzle_title, p->label_nozzle_type, p->combo_nozzle_dia}) {
            w->Bind(wxEVT_ENTER_WINDOW, [this, panel_color](wxMouseEvent &e) {
                if(!p->combo_nozzle_dia->HasFocus())
                    p->panel_nozzle_dia->SetBorderColor(panel_color.bd_hover);
                e.Skip();
            });
            w->Bind(wxEVT_LEAVE_WINDOW, [this, panel_color](wxMouseEvent &e) {
                wxWindow* next_w = wxFindWindowAtPoint(wxGetMousePosition());
                if (!p->combo_nozzle_dia->HasFocus() && (!next_w || !p->panel_nozzle_dia->IsDescendant(next_w)))
                    p->panel_nozzle_dia->SetBorderColor(panel_color.bd_normal);
                e.Skip();
            });
        }

        wxGridSizer *nozzle_dia_sizer = new wxGridSizer(3, 1, FromDIP(2), 0);
        nozzle_dia_sizer->Add(p->label_nozzle_title, 0, wxALIGN_CENTER | wxTOP, FromDIP(4));
        nozzle_dia_sizer->Add(p->combo_nozzle_dia  , 0, wxALIGN_CENTER | wxTOP | wxBOTTOM, FromDIP(2));
        nozzle_dia_sizer->Add(p->label_nozzle_type , 0, wxALIGN_CENTER);

        p->panel_nozzle_dia->SetSizer(nozzle_dia_sizer);

        // Bed type selection
        p->panel_printer_bed = new StaticBox(p->m_panel_printer_content);
        p->panel_printer_bed->SetCornerRadius(FromDIP(PRINTER_PANEL_RADIUS));
        p->panel_printer_bed->SetBorderColor(panel_color.bd_normal);
        p->panel_printer_bed->SetMinSize(FromDIP(PRINTER_PANEL_SIZE));
        p->panel_printer_bed->Bind(wxEVT_LEFT_DOWN, [this](auto &evt) {
            on_leave_image_printer_bed(evt);
            p->combo_printer_bed->wxEvtHandler::ProcessEvent(evt);
        });

        //ScalableButton *wiki_bed = new ScalableButton(p->panel_printer_bed, wxID_ANY, "help");
        //wiki_bed->Bind(wxEVT_BUTTON, [](wxCommandEvent) {
        //    wxLaunchDefaultBrowser("https://wiki.bambulab.com/en/x1/manual/compatibility-and-parameter-settings-of-filaments");
        //});

        ScalableBitmap bitmap_bed(p->panel_printer_bed, "printer_placeholder", PRINTER_THUMBNAIL_SIZE.GetHeight());
        p->image_printer_bed = new wxStaticBitmap(p->panel_printer_bed, wxID_ANY, bitmap_bed.bmp(), wxDefaultPosition, wxDefaultSize, 0);
        p->image_printer_bed->Bind(wxEVT_LEFT_DOWN, [this](auto &evt) {
            on_leave_image_printer_bed(evt);
            p->combo_printer_bed->wxEvtHandler::ProcessEvent(evt);
        });

        p->combo_printer_bed = new ComboBox(p->panel_printer_bed, wxID_ANY, wxString(""), wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
        p->combo_printer_bed->SetBorderWidth(0);
        p->combo_printer_bed->GetDropDown().SetUseContentWidth(true);
        p->combo_printer_bed->SetMinSize(FromDIP(wxSize(18,-1))); // ORCA show only arrow
        p->combo_printer_bed->SetMaxSize(FromDIP(wxSize(18,-1))); // ORCA show only arrow
        reset_bed_type_combox_choices(true);

        p->combo_printer_bed->Bind(wxEVT_COMBOBOX, [this](auto &e) {
            auto image_path        = get_cur_select_bed_image();
            p->image_printer_bed->SetBitmap(create_scaled_bitmap(image_path, this, PRINTER_THUMBNAIL_SIZE.GetHeight()));
            e.Skip();
        });

        // ORCA paint whole combobox on focus
        auto bed_focus_bg = [this, panel_color](bool focused){
            auto bg_color = StateColor::darkModeColorFor(focused ? panel_color.bg_focus : panel_color.bg_normal);
            p->panel_printer_bed->SetBackgroundColor(bg_color);
            p->panel_printer_bed->SetBorderColor(focused ? panel_color.bd_focus : panel_color.bd_normal);
            p->image_printer_bed->SetBackgroundColour(bg_color);
            p->combo_printer_bed->SetBackgroundColour(bg_color); // paints margins instead combo background
        };
        p->combo_printer_bed->Bind(wxEVT_SET_FOCUS,  [this, bed_focus_bg](auto& e) {bed_focus_bg(true ); e.Skip();});
        p->combo_printer_bed->Bind(wxEVT_KILL_FOCUS, [this, bed_focus_bg](auto& e) {bed_focus_bg(false); e.Skip();});

        // highlight border on hover
        for (wxWindow *w : std::initializer_list<wxWindow *>{p->panel_printer_bed, p->image_printer_bed, p->combo_printer_bed}) {
            w->Bind(wxEVT_ENTER_WINDOW, [this, w, panel_color](wxMouseEvent &e) {
                if(!p->combo_printer_bed->HasFocus())
                    p->panel_printer_bed->SetBorderColor(panel_color.bd_hover);
                if(w == p->image_printer_bed && !p->combo_printer_bed->is_drop_down()) // dont trigger while combo open
                    on_enter_image_printer_bed(e);
                e.Skip();
            });
            w->Bind(wxEVT_LEAVE_WINDOW, [this, w, panel_color](wxMouseEvent &e) {
                wxWindow* next_w = wxFindWindowAtPoint(wxGetMousePosition());
                if (!p->combo_printer_bed->HasFocus() && (!next_w || !p->panel_printer_bed->IsDescendant(next_w)))
                    p->panel_printer_bed->SetBorderColor(panel_color.bd_normal);
                if(w == p->image_printer_bed)
                    on_leave_image_printer_bed(e);
                e.Skip();
            });
        }

        wxBoxSizer *bed_type_sizer = new wxBoxSizer(wxHORIZONTAL);
        bed_type_sizer->Add(p->combo_printer_bed, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
        bed_type_sizer->Add(p->image_printer_bed, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));

        p->panel_printer_bed->SetSizer(bed_type_sizer);

        AppConfig *app_config = wxGetApp().app_config;
        std::string str_bed_type = app_config->get("curr_bed_type");
        int bed_type_value = atoi(str_bed_type.c_str());
        // hotfix: btDefault is added as the first one in BedType, and app_config should not be btDefault
        if (bed_type_value == 0) {
            app_config->set("curr_bed_type", "1");
            bed_type_value = 1;
        }

        int bed_type_idx = bed_type_value - 1;
        p->combo_printer_bed->Select(bed_type_idx);

        auto& project_config = wxGetApp().preset_bundle->project_config;
        /*const t_config_enum_values* keys_map = print_config_def.get("curr_bed_type")->enum_keys_map;
        BedType bed_type = btCount;
        for (auto item : *keys_map) {
            if (item.first == str_bed_type)
                bed_type = (BedType)item.second;
        }*/
        BedType bed_type = (BedType)bed_type_value;
        project_config.set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(bed_type));

        /* ORCA THIS PART MOVED TO TITLEBAR
        // Sync printer information
        btn_sync = new Button(p->m_panel_printer_content, _L("Sync info"), "printer_sync", 0, 32);
        //btn_sync->SetFont(Label::Body_8);
        btn_sync->SetToolTip(_L("Synchronize nozzle information and the number of AMS"));
        btn_sync->SetCornerRadius(8);
        StateColor btn_sync_bg_col(
                std::pair<wxColour, int>(wxColour("#CECECE"), StateColor::Pressed),
                std::pair<wxColour, int>(wxColour("#F8F8F8"), StateColor::Hovered),
                std::pair<wxColour, int>(wxColour("#F8F8F8"), StateColor::Normal));
        StateColor btn_sync_bd_col(
                std::pair<wxColour, int>(wxColour("#009688"), StateColor::Pressed),
                std::pair<wxColour, int>(wxColour("#009688"), StateColor::Hovered),
                std::pair<wxColour, int>(wxColour("#EEEEEE"), StateColor::Normal));
        btn_sync->SetBackgroundColor(btn_sync_bg_col);
        btn_sync->SetBorderColor(btn_sync_bd_col);
        btn_sync->SetCanFocus(false);
        btn_sync->SetPaddingSize({FromDIP(6), FromDIP(12)});
        btn_sync->SetMinSize(BTN_SYNC_SIZE);
        btn_sync->SetMaxSize(BTN_SYNC_SIZE);
        btn_sync->SetVertical();
        btn_sync->Bind(wxEVT_UPDATE_UI, &Sidebar::update_sync_ams_btn_enable, this);
        btn_sync->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
            deal_btn_sync();
        });
        p->btn_sync_printer = btn_sync;
        */
        p->timer_sync_printer->Bind(wxEVT_TIMER, [this] (wxTimerEvent & e) {
            p->flush_printer_sync();
        });
       

        p->left_extruder  = new ExtruderGroup(p->m_panel_printer_content, 0, _L("Left Nozzle"));
        p->right_extruder = new ExtruderGroup(p->m_panel_printer_content, 1, _L("Right Nozzle"));
        p->single_extruder = new ExtruderGroup(p->m_panel_printer_content, -1, _L("Nozzle"));
        auto switch_diameter = [this](wxCommandEvent & evt) {
            auto extruder = dynamic_cast<ExtruderGroup *>(dynamic_cast<ComboBox *>(evt.GetEventObject())->GetParent());
            p->is_switching_diameter = true;
            p->switch_diameter(extruder == p->single_extruder);
            p->is_switching_diameter = false;
        };
        p->left_extruder->combo_diameter->Bind(wxEVT_COMBOBOX, switch_diameter);
        p->right_extruder->combo_diameter->Bind(wxEVT_COMBOBOX, switch_diameter);
        p->single_extruder->combo_diameter->Bind(wxEVT_COMBOBOX, switch_diameter);

        p->vsizer_printer = new wxBoxSizer(wxVERTICAL);
        p->layout_printer(true, true);
        p->m_panel_printer_content->SetSizer(p->vsizer_printer);
        p->m_panel_printer_content->Layout();
        scrolled_sizer->Add(p->m_panel_printer_content, 0, wxEXPAND, 0);
    }

    {
    // add filament title
    p->m_panel_filament_title = new StaticBox(p->scrolled, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE);
    p->m_panel_filament_title->SetBackgroundColor(title_bg);
    p->m_panel_filament_title->SetBackgroundColor2(0xF1F1F1);
    p->m_panel_filament_title->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &e) {
        if (e.GetPosition().x > (p->m_flushing_volume_btn->IsShown()
                ? p->m_flushing_volume_btn->GetPosition().x : (p->m_bpButton_add_filament->GetPosition().x - FromDIP(30)))) // ORCA exclude area of del button from titlebar collapse/expand feature to fix undesired collapse when user spams del filament button 
            return;
        p->m_panel_filament_content->Show(!p->m_panel_filament_content->IsShown());
        m_scrolled_sizer->Layout();
    });

    wxBoxSizer* bSizer39;
    bSizer39 = new wxBoxSizer( wxHORIZONTAL );
    p->m_filament_icon = new ScalableButton(p->m_panel_filament_title, wxID_ANY, "filament");
    p->m_staticText_filament_settings = new Label(p->m_panel_filament_title, _L("Project Filaments"), LB_PROPAGATE_MOUSE_EVENT);
    bSizer39->Add(p->m_filament_icon, 0, wxALIGN_CENTER | wxLEFT, FromDIP(SidebarProps::TitlebarMargin()));
    bSizer39->AddSpacer(FromDIP(SidebarProps::ElementSpacing()));
    bSizer39->Add( p->m_staticText_filament_settings, 0, wxALIGN_CENTER );
    bSizer39->Add(FromDIP(10), 0, 0, 0, 0);
    bSizer39->SetMinSize(-1, FromDIP(30));

    p->m_panel_filament_title->SetSizer( bSizer39 );
    p->m_panel_filament_title->Layout();
    auto spliter_1 = new ::StaticLine(p->scrolled);
    spliter_1->SetLineColour("#A6A9AA");
    scrolled_sizer->Add(spliter_1, 0, wxEXPAND);
    scrolled_sizer->Add(p->m_panel_filament_title, 0, wxEXPAND | wxALL, 0);
    auto spliter_2 = new ::StaticLine(p->scrolled);
    spliter_2->SetLineColour("#CECECE");
    scrolled_sizer->Add(spliter_2, 0, wxEXPAND);

    bSizer39->AddStretchSpacer(1);

    // BBS
    // add wiping dialog
    //wiping_dialog_button->SetFont(wxGetApp().normal_font());
    p->m_flushing_volume_btn = new Button(p->m_panel_filament_title, _L("Flushing volumes"));
    p->m_flushing_volume_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Compact);
    p->m_flushing_volume_btn->SetId(wxID_RESET);
    auto has_modify = is_flush_config_modified();
    set_flushing_volume_warning(has_modify);

    p->m_flushing_volume_btn->Bind(wxEVT_BUTTON, ([parent, this](wxCommandEvent &e) {
            open_flushing_dialog(parent, SimpleEvent(EVT_SCHEDULE_BACKGROUND_PROCESS, parent));
            p->plater->get_view3D_canvas3D()->reload_scene(true);
            p->plater->update();
        }));

    bSizer39->Add(p->m_flushing_volume_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
    bSizer39->Hide(p->m_flushing_volume_btn); // ORCA Ensure button is hidden on launch while 1 filament exist

    ScalableButton* add_btn = new ScalableButton(p->m_panel_filament_title, wxID_ANY, "add_filament");
    add_btn->SetToolTip(_L("Add one filament"));
    add_btn->Bind(wxEVT_BUTTON, [this, scrolled_sizer](wxCommandEvent& e){
        add_filament();
    });
    p->m_bpButton_add_filament = add_btn;

    // ORCA Moved add button after delete button to prevent add button position change when remove icon automatically hidden

    ScalableButton* del_btn = new ScalableButton(p->m_panel_filament_title, wxID_ANY, "delete_filament");
    del_btn->SetToolTip(_L("Remove last filament"));
    del_btn->Bind(wxEVT_BUTTON, [this, scrolled_sizer](wxCommandEvent &e) {
        delete_filament();
    });
    p->m_bpButton_del_filament = del_btn;

    bSizer39->Add(del_btn, 0, wxALIGN_CENTER | wxLEFT, FromDIP(SidebarProps::IconSpacing()));
    bSizer39->Add(add_btn, 0, wxALIGN_CENTER | wxLEFT, FromDIP(SidebarProps::IconSpacing())); // ORCA Moved add button after delete button to prevent add button position change when remove icon automatically hidden

    bSizer39->Hide(p->m_bpButton_del_filament); // ORCA Ensure button is hidden on launch while 1 filament exist

    ams_btn = new ScalableButton(p->m_panel_filament_title, wxID_ANY, "ams_fila_sync", wxEmptyString, wxDefaultSize, wxDefaultPosition,
                                                 wxBU_EXACTFIT | wxNO_BORDER, false, 16); // ORCA match icon size with other icons as 16x16
    ams_btn->SetToolTip(_L("Synchronize filament list from AMS"));
    ams_btn->Bind(wxEVT_BUTTON, [this, scrolled_sizer](wxCommandEvent &e) {
        sync_ams_list();
    });

    ams_btn->Bind(wxEVT_UPDATE_UI, &Sidebar::update_sync_ams_btn_enable, this);
    p->m_bpButton_ams_filament = ams_btn;

    bSizer39->Add(ams_btn, 0, wxALIGN_CENTER | wxLEFT, FromDIP(SidebarProps::WideSpacing()));
    //bSizer39->Add(FromDIP(10), 0, 0, 0, 0 );

    ScalableButton* set_btn = new ScalableButton(p->m_panel_filament_title, wxID_ANY, "settings");
    set_btn->SetToolTip(_L("Set filaments to use"));
    set_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        p->editing_filament = -1;
        // wxGetApp().params_dialog()->Popup();
        // wxGetApp().get_tab(Preset::TYPE_FILAMENT)->restore_last_select_item();
        wxGetApp().run_wizard(ConfigWizard::RR_USER, ConfigWizard::SP_FILAMENTS);
        });
    p->m_bpButton_set_filament = set_btn;

    bSizer39->Add(set_btn, 0, wxALIGN_CENTER | wxLEFT, FromDIP(SidebarProps::WideSpacing()));
    bSizer39->AddSpacer(FromDIP(SidebarProps::TitlebarMargin()));

    // add filament content
    p->m_panel_filament_content = new wxScrolledWindow( p->scrolled, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL );
    p->m_panel_filament_content->SetScrollbars(0, 100, 1, 2);
    p->m_panel_filament_content->SetScrollRate(0, 5);
    p->m_panel_filament_content->SetMaxSize(wxSize{-1, FromDIP(174)});
    p->m_panel_filament_content->SetBackgroundColour(wxColour(255, 255, 255));

    //wxBoxSizer* bSizer_filament_content;
    //bSizer_filament_content = new wxBoxSizer( wxHORIZONTAL );

    // BBS:  filament double columns
    p->sizer_filaments = new wxBoxSizer(wxHORIZONTAL);
    p->sizer_filaments->Add(new wxBoxSizer(wxVERTICAL), 1, wxEXPAND);
    p->sizer_filaments->Add(new wxBoxSizer(wxVERTICAL), 1, wxEXPAND);

    p->combos_filament.push_back(nullptr);

    /* first filament item */
    init_filament_combo(&p->combos_filament[0], 0);

    //bSizer_filament_content->Add(p->sizer_filaments, 1, wxALIGN_CENTER | wxALL);
    wxSizer *sizer_filaments2 = new wxBoxSizer(wxVERTICAL);
    sizer_filaments2->Add(p->sizer_filaments, 0, wxEXPAND, 0);
    p->m_panel_filament_content->SetSizer(sizer_filaments2);
    p->m_panel_filament_content->Layout();
    auto min_size = sizer_filaments2->GetMinSize();
    if (min_size.y > p->m_panel_filament_content->GetMaxHeight())
        min_size.y = p->m_panel_filament_content->GetMaxHeight();
    p->m_panel_filament_content->SetMinSize(min_size);
    scrolled_sizer->Add(p->m_panel_filament_content, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(SidebarProps::ContentMarginV())); // ORCA use vertical margin on parent otherwise it shows scrollbar even on 1 filament
    }

    {
    //add project title
    auto params_panel = ((MainFrame*)parent->GetParent())->m_param_panel;
    if (params_panel) {
        params_panel->get_top_panel()->Reparent(p->scrolled);
        auto spliter_1 = new ::StaticLine(p->scrolled);
        spliter_1->SetLineColour("#A6A9AA");
        scrolled_sizer->Add(spliter_1, 0, wxEXPAND);
        scrolled_sizer->Add(params_panel->get_top_panel(), 0, wxEXPAND);
        auto spliter_2 = new ::StaticLine(p->scrolled);
        spliter_2->SetLineColour("#CECECE");
        scrolled_sizer->Add(spliter_2, 0, wxEXPAND);
    }

    //add project content
    p->sizer_params = new wxBoxSizer(wxVERTICAL);

    // ORCA: Update search box to modern style
    p->m_search_bar = new StaticBox(p->scrolled);
    p->m_search_bar->SetCornerRadius(0);
    p->m_search_bar->SetBorderColor(wxColour("#CECECE"));

    p->m_search_item = new TextInput(p->m_search_bar, wxEmptyString, wxEmptyString, "", wxDefaultPosition, wxDefaultSize, 0 | wxBORDER_NONE);
    p->m_search_item->SetIcon(*BitmapCache().load_svg("search", FromDIP(16), FromDIP(16))); // ORCA: Add search icon to search box

    wxTextCtrl* text_ctrl = p->m_search_item->GetTextCtrl();
    text_ctrl->SetHint(_L("Search plate, object and part."));
    text_ctrl->SetForegroundColour(wxColour("#262E30"));
    text_ctrl->SetFont(Label::Body_13);
    text_ctrl->SetSize(wxSize(-1, FromDIP(16))); // Centers text vertically

    text_ctrl->Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& e) {
        if (p->dia->IsShown()) {
            e.Skip();
            return;
        }
        p->m_search_bar->SetBorderColor(wxColour("#009688"));
        wxPoint pos = this->p->m_search_bar->ClientToScreen(wxPoint(0, 0));
#ifndef __WXGTK__
        pos.y += this->p->m_search_bar->GetRect().height;
#else
        this->p->m_search_item->Enable(false);
#endif
        p->dia->SetPosition(pos);
        p->dia->Popup();
        e.Skip(); // required to show caret
    });

    auto search_sizer = new wxBoxSizer(wxHORIZONTAL);
    search_sizer->Add(new wxWindow(p->m_search_bar, wxID_ANY, wxDefaultPosition, wxSize(0, 0)), 0, wxEXPAND|wxLEFT|wxRIGHT, FromDIP(1));
    search_sizer->Add(p->m_search_item, 1, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
    p->m_search_bar->SetSizer(search_sizer);
    p->m_search_bar->Layout();
    search_sizer->Fit(p->m_search_bar);

    p->m_object_list = new ObjectList(p->scrolled);
    p->m_object_list->Bind(wxCUSTOMEVT_EXIT_SEARCH, [this](wxCommandEvent&) {
#ifdef __WXGTK__
        this->p->m_search_item->Enable(true);
#endif
        this->p->m_search_bar->SetBorderColor(wxColour("#CECECE"));
        this->p->m_search_item->GetTextCtrl()->SetValue(""); // reset value when close
    });

    p->sizer_params->Add(p->m_search_bar, 0, wxALL | wxEXPAND, 0);
    p->sizer_params->Add(p->m_object_list, 1, wxEXPAND | wxTOP, 0);
    scrolled_sizer->Add(p->sizer_params, 2, wxEXPAND | wxLEFT, 0);
    p->m_object_list->Hide();
    p->m_search_bar->Hide();
    // Frequently Object Settings
    p->object_settings = new ObjectSettings(p->scrolled);

    p->dia = new Search::SearchObjectDialog(p->m_object_list, p->scrolled->GetParent(), p->m_search_item);
#if !NEW_OBJECT_SETTING
    p->object_settings->Hide();
    p->sizer_params->Add(p->object_settings->get_sizer(), 0, wxEXPAND | wxTOP, 5 * em / 10);
#else
    if (params_panel) {
        params_panel->Reparent(p->scrolled);
        scrolled_sizer->Add(params_panel, 3, wxEXPAND);
    }
#endif
    }

    p->object_layers = new ObjectLayers(p->scrolled);
    p->object_layers->Hide();
    p->sizer_params->Add(p->object_layers->get_sizer(), 0, wxEXPAND | wxTOP, 0);

    auto *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(p->scrolled, 1, wxEXPAND);
    SetSizer(sizer);
}

Sidebar::~Sidebar() {}

void Sidebar::on_enter_image_printer_bed(wxMouseEvent &evt) {
    //p->image_printer_bed->Bind(wxEVT_LEAVE_WINDOW, &Sidebar::on_leave_image_printer_bed, this);
    auto    pos  = p->panel_printer_bed->GetScreenPosition();
    auto    rect = p->panel_printer_bed->GetRect();
    wxPoint temp_pos(pos.x + rect.GetWidth() +  FromDIP(3), pos.y);
    if (p->big_bed_image_popup == nullptr)
        p->big_bed_image_popup = new ImageDPIFrame();
    auto image_path        = get_cur_select_bed_image();
    p->big_bed_image_popup->set_bitmap(create_scaled_bitmap("big_" + image_path, p->big_bed_image_popup, p->big_bed_image_popup->get_image_px()));
    p->big_bed_image_popup->set_title(p->combo_printer_bed->GetString(p->combo_printer_bed->GetSelection()));
    p->big_bed_image_popup->SetCanFocus(false);
    p->big_bed_image_popup->SetPosition(temp_pos);
    p->big_bed_image_popup->on_show();
}

void Sidebar::on_leave_image_printer_bed(wxMouseEvent &evt) {
    //auto pos_x = evt.GetX();
    //auto pos_y = evt.GetY();
    //auto rect  = p->image_printer_bed->GetRect();
    //if ((pos_x <= 0 || pos_y <= 0 || pos_x >= rect.GetWidth()) && p->big_bed_image_popup) {
    if (p->big_bed_image_popup) {
        bool was_visible = p->big_bed_image_popup->IsShown();
        p->big_bed_image_popup->on_hide();
        if(!p->combo_printer_bed->is_drop_down() && was_visible)
            p->combo_printer_bed->SetFocus();     // set focus back to bed type combo. this prevents weird look if focus on other item
    }
}
void Sidebar::on_change_color_mode(bool is_dark) {
    const ModelObjectPtrs &mos = wxGetApp().model().objects;
    for (int i = 0; i < mos.size(); i++) {
        wxGetApp().obj_list()->update_info_items(i,nullptr,false,true);
    }

}

void Sidebar::create_printer_preset()
{
    CreatePrinterPresetDialog dlg(wxGetApp().mainframe);
    int                       res = dlg.ShowModal();
    if (wxID_OK == res) {
        wxGetApp().load_current_presets();
        wxGetApp().mainframe->update_side_preset_ui();
        update_ui_from_settings();
        update_all_preset_comboboxes();
        CreatePresetSuccessfulDialog success_dlg(wxGetApp().mainframe, SuccessType::PRINTER);
        int                          res = success_dlg.ShowModal();
        if (res == wxID_OK) {
            p->editing_filament = -1;
            if (p->combo_printer->switch_to_tab()) p->editing_filament = 0;
        }
    }
}

void Sidebar::init_filament_combo(PlaterPresetComboBox **combo, const int filament_idx)
{
    *combo = new PlaterPresetComboBox(p->m_panel_filament_content, Slic3r::Preset::TYPE_FILAMENT);
    (*combo)->set_filament_idx(filament_idx);

    auto combo_and_btn_sizer = new wxBoxSizer(wxHORIZONTAL);

    // BBS:  filament double columns

    // int em = wxGetApp().em_unit();
    if ((filament_idx % 2) == 0) // Dont add right column item. this one create equal spacing on left, right & middle
        combo_and_btn_sizer->AddSpacer(FromDIP((filament_idx % 2) == 0 ? 12 : 3)); // Content Margin

    (*combo)->clr_picker->SetLabel(wxString::Format("%d", filament_idx + 1));
    combo_and_btn_sizer->Add((*combo)->clr_picker, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(SidebarProps::ElementSpacing()) - FromDIP(2)); // ElementSpacing - 2 (from combo box))
    combo_and_btn_sizer->Add(*combo, 1, wxALL | wxEXPAND, FromDIP(2))->SetMinSize({-1, FromDIP(30)});

    /* BBS hide del_btn
    ScalableButton* del_btn = new ScalableButton(p->m_panel_filament_content, wxID_ANY, "delete_filament");
    del_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e){
        int extruder_count = std::max(1, (int)p->combos_filament.size() - 1);

        update_objects_list_filament_column(std::max(1, extruder_count - 1));
        on_filament_count_change(extruder_count);
        wxGetApp().preset_bundle->printers.get_edited_preset().set_num_extruders(extruder_count);
        wxGetApp().preset_bundle->update_multi_material_filament_presets();
    });

    combo_and_btn_sizer->Add(32 * em / 10, 0, 0, 0, 0);
    combo_and_btn_sizer->Add(del_btn, 0, wxALIGN_CENTER_VERTICAL, 5 * em / 10);
    */
    ScalableButton* edit_btn = new ScalableButton(p->m_panel_filament_content, wxID_ANY, "menu_filament");
    edit_btn->SetToolTip(_L("Click to edit preset"));

    PlaterPresetComboBox* combobox = (*combo);
    edit_btn->Bind(wxEVT_BUTTON, [this, edit_btn, filament_idx](wxCommandEvent) {
        auto menu = p->plater->filament_action_menu(filament_idx);
        wxPoint pt { 0, edit_btn->GetSize().GetHeight() + 10 };
        pt = edit_btn->ClientToScreen(pt);
        pt = wxGetApp().mainframe->ScreenToClient(pt);
        p->m_menu_filament_id = filament_idx;
        p->plater->PopupMenu(menu, (int) pt.x, pt.y);
    });
    combobox->edit_btn = edit_btn;

    combo_and_btn_sizer->Add(edit_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(SidebarProps::ElementSpacing()) - FromDIP(2)); // ElementSpacing - 2 (from combo box))

    combo_and_btn_sizer->AddSpacer(FromDIP(SidebarProps::ContentMargin()));

    // BBS:  filament double columns
    auto side = filament_idx % 2;
    auto /***/sizer_filaments = this->p->sizer_filaments->GetItem(side)->GetSizer();
    if (side == 1 && filament_idx > 1) sizer_filaments->Remove(filament_idx / 2);
    sizer_filaments->Add(combo_and_btn_sizer, 1, wxEXPAND);
    if (side == 0 && filament_idx > 0) {
        sizer_filaments = this->p->sizer_filaments->GetItem(1)->GetSizer();
        sizer_filaments->AddStretchSpacer(1);
    }
}

void Sidebar::remove_unused_filament_combos(const size_t current_extruder_count)
{
    if (current_extruder_count >= p->combos_filament.size())
        return;
    while (p->combos_filament.size() > current_extruder_count) {
        const int last = p->combos_filament.size() - 1;
        auto sizer_filaments = this->p->sizer_filaments->GetItem(last % 2)->GetSizer();
        sizer_filaments->Remove(last / 2);
        (*p->combos_filament[last]).Destroy();
        p->combos_filament.pop_back();
    }
    // BBS:  filament double columns
    auto sizer_filaments0 = this->p->sizer_filaments->GetItem((size_t)0)->GetSizer();
    auto sizer_filaments1 = this->p->sizer_filaments->GetItem(1)->GetSizer();
    if (current_extruder_count < 2) {
        sizer_filaments1->Clear();
    } else {
        size_t c0 = sizer_filaments0->GetChildren().GetCount();
        size_t c1 = sizer_filaments1->GetChildren().GetCount();
        if (c0 < c1)
            sizer_filaments1->Remove(c1 - 1);
        else if (c0 > c1)
            sizer_filaments1->AddStretchSpacer(1);
    }
}

void Sidebar::update_all_preset_comboboxes()
{
    PresetBundle &preset_bundle = *wxGetApp().preset_bundle;
    const auto print_tech = preset_bundle.printers.get_edited_preset().printer_technology();

    bool is_bbl_vendor = preset_bundle.is_bbl_vendor();

    auto p_mainframe = wxGetApp().mainframe;
    auto cfg = preset_bundle.printers.get_edited_preset().config;

    if (preset_bundle.use_bbl_network()) {
        //only show connection button for not-BBL printer
        //p->btn_connect_printer->Hide();
        p->m_printer_connect->Hide();
        //only show sync-ams button for BBL printer
        p->m_bpButton_ams_filament->Show();
        //update print button default value for bbl or third-party printer
        p_mainframe->set_print_button_to_default(MainFrame::PrintSelectType::ePrintPlate);
    } else {
        //p->btn_connect_printer->Show();
        p->m_printer_connect->Show();
        p->m_bpButton_ams_filament->Hide();
        auto print_btn_type = MainFrame::PrintSelectType::eExportGcode;
        wxString url = cfg.opt_string("print_host_webui").empty() ? cfg.opt_string("print_host") : cfg.opt_string("print_host_webui");
        wxString apikey;
        if(url.empty())
            url = wxString::Format("file://%s/web/orca/missing_connection.html", from_u8(resources_dir()));
        else {
            if (!url.Lower().starts_with("http"))
                url = wxString::Format("http://%s", url);
            const auto host_type = cfg.option<ConfigOptionEnum<PrintHostType>>("host_type")->value;
            if (cfg.has("printhost_apikey") && (host_type != htSimplyPrint))
                apikey = cfg.opt_string("printhost_apikey");
            print_btn_type = preset_bundle.is_bbl_vendor() ? MainFrame::PrintSelectType::ePrintPlate : MainFrame::PrintSelectType::eSendGcode;
        }

        p_mainframe->load_printer_url(url, apikey);


        p_mainframe->set_print_button_to_default(print_btn_type);

    }

    if (cfg.opt_bool("pellet_modded_printer")) {
		p->m_staticText_filament_settings->SetLabel(_L("Pellets"));
        p->m_filament_icon->SetBitmap_("pellets");
    } else {
		p->m_staticText_filament_settings->SetLabel(_L("Filament"));
        p->m_filament_icon->SetBitmap_("filament");
    }

    show_SEMM_buttons(should_show_SEMM_buttons());

    //p->m_staticText_filament_settings->Update();

    if (is_bbl_vendor || cfg.opt_bool("support_multi_bed_types")) {
        p->combo_printer_bed->Enable();
        // Orca: don't update bed type if loading project
        if (!p->plater->is_loading_project()) {
            bool has_changed = reset_bed_type_combox_choices();
            bool flag         = m_begin_sync_printer_status && !has_changed;
            if (!(flag)) {
                auto str_bed_type = wxGetApp().app_config->get_printer_setting(wxGetApp().preset_bundle->printers.get_selected_preset_name(),
                                                                               "curr_bed_type");
                if (!str_bed_type.empty()) {
                    int bed_type_value = atoi(str_bed_type.c_str());
                    if (bed_type_value <= 0 || bed_type_value >= btCount) {
                        bed_type_value = preset_bundle.printers.get_edited_preset().get_default_bed_type(&preset_bundle);
                    }

                   set_bed_type_accord_combox((BedType) bed_type_value);
                } else {
                    BedType bed_type = preset_bundle.printers.get_edited_preset().get_default_bed_type(&preset_bundle);
                    set_bed_type_accord_combox(bed_type);
                }
            } else {
                BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":no need reset_bed_type_combox_choices";
            }
        }
    } else {
        // m_bed_type_list->SelectAndNotify(btPEI - 1);
        BedType bed_type = preset_bundle.printers.get_edited_preset().get_default_bed_type(&preset_bundle);
        set_bed_type_accord_combox(bed_type);
        p->combo_printer_bed->Disable();
    }

    // ORCA Hide plate selector if not supported by printer
    p->panel_printer_bed->Show(is_bbl_vendor || cfg.opt_bool("support_multi_bed_types"));

    // Update the print choosers to only contain the compatible presets, update the dirty flags.
    //BBS

    // Update the printer choosers, update the dirty flags.
    //p->combo_printer->update();
    // Update the filament choosers to only contain the compatible presets, update the color preview,
    // update the dirty flags.
    if (print_tech == ptFFF) {
        for (PlaterPresetComboBox* cb : p->combos_filament)
            cb->update();
    }

    if (p->combo_printer) {
        p->combo_printer->update();
        update_printer_thumbnail();
    }

    // Orca:: show device tab based on vendor type
    p_mainframe->show_device(preset_bundle.use_bbl_device_tab());
    p_mainframe->m_tabpanel->SetSelection(p_mainframe->m_tabpanel->GetSelection());
}

void Sidebar::update_presets(Preset::Type preset_type)
{
    PresetBundle &preset_bundle = *wxGetApp().preset_bundle;
    const auto print_tech = preset_bundle.printers.get_edited_preset().printer_technology();

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": enter, preset_type %1%")%preset_type;
    switch (preset_type) {
    case Preset::TYPE_FILAMENT:
    {
        // BBS
#if 0
        const size_t extruder_cnt = print_tech != ptFFF ? 1 :
                                dynamic_cast<ConfigOptionFloats*>(preset_bundle.printers.get_edited_preset().config.option("nozzle_diameter"))->values.size();
        const size_t filament_cnt = p->combos_filament.size() > extruder_cnt ? extruder_cnt : p->combos_filament.size();
#else
        const size_t filament_cnt = p->combos_filament.size();
#endif
        const std::string &name = preset_bundle.filaments.get_selected_preset_name();
        if (p->editing_filament >= 0) {
            preset_bundle.set_filament_preset(p->editing_filament, name);
        } else if (filament_cnt == 1) {
            // Single filament printer, synchronize the filament presets.
            Preset *preset = preset_bundle.filaments.find_preset(name, false);
            if (preset) {
                if (preset->is_compatible) preset_bundle.set_filament_preset(0, name);
            }

        }

        for (size_t i = 0; i < filament_cnt; i++)
            p->combos_filament[i]->update();

        update_dynamic_filament_list();
        break;
    }

    case Preset::TYPE_PRINT:
        //wxGetApp().mainframe->m_param_panel;
        //p->combo_print->update();
        {
        Tab* print_tab = wxGetApp().get_tab(Preset::TYPE_PRINT);
        if (print_tab) {
            print_tab->get_combo_box()->update();
        }
        break;
        }
    case Preset::TYPE_SLA_PRINT:
        ;// p->combo_sla_print->update();
        break;

    case Preset::TYPE_SLA_MATERIAL:
        ;// p->combo_sla_material->update();
        break;

    case Preset::TYPE_PRINTER:
    {
        update_all_preset_comboboxes();
        p->show_preset_comboboxes();

        /* update bed shape */
        Tab* printer_tab = wxGetApp().get_tab(Preset::TYPE_PRINTER);
        if (printer_tab) {
            printer_tab->update();
            printer_tab->on_preset_loaded();
        }

        Preset& printer_preset = wxGetApp().preset_bundle->printers.get_edited_preset();

        bool isBBL = preset_bundle.is_bbl_vendor();

        if (auto printer_structure_opt = printer_preset.config.option<ConfigOptionEnum<PrinterStructure>>("printer_structure")) {
            wxGetApp().plater()->get_current_canvas3D()->get_arrange_settings().align_to_y_axis = (printer_structure_opt->value == PrinterStructure::psI3);
        }
        else
            wxGetApp().plater()->get_current_canvas3D()->get_arrange_settings().align_to_y_axis = false;

        // Update dual extrudes
        auto* nozzle_diameter = dynamic_cast<const ConfigOptionFloats*>(printer_preset.config.option("nozzle_diameter"));

        bool is_dual_extruder = nozzle_diameter->size() == 2;
        p->layout_printer(preset_bundle.use_bbl_network(), isBBL && is_dual_extruder);
        auto diameters = wxGetApp().preset_bundle->printers.diameters_of_selected_printer();
        auto diameter = printer_preset.config.opt_string("printer_variant");
        auto update_extruder_diameter = [&diameters, &diameter, &nozzle_diameter](int extruder_index,ExtruderGroup & extruder) {
            extruder.combo_diameter->Clear();
            int select = -1;
            // ORCA if user defined a custom nozzle in printer config select it instead inherited one. this will show correct nozzle diameter in combobox if its exist in nozzle diameters list
            auto nozzle_dia = get_diameter_string(nozzle_diameter->values[extruder_index]);
            if(nozzle_dia != diameter && std::find(diameters.begin(), diameters.end(), nozzle_dia) != diameters.end())
                diameter = nozzle_dia;
            // ORCA try to add nozzle diameter from config if list is empty. fixes blank nozzle combo box when preset has no alias
            if(diameters[0].empty() && !nozzle_dia.empty()){
                diameters[0] = nozzle_dia;
                diameter = nozzle_dia;
            }
            for (size_t i = 0; i < diameters.size(); ++i) {
                if (diameters[i] == diameter)
                    select = extruder.combo_diameter->GetCount();
                extruder.combo_diameter->Append(diameters[i], {});
            }
            extruder.combo_diameter->SetSelection(select);
            extruder.diameter = diameter;
        };
        auto image_path = get_cur_select_bed_image();
        if (is_dual_extruder) {
            AMSCountPopupWindow::UpdateAMSCount(0, p->left_extruder);
            AMSCountPopupWindow::UpdateAMSCount(1, p->right_extruder);
            //if (!p->is_switching_diameter) {
                update_extruder_diameter(0, *p->left_extruder);
                update_extruder_diameter(1, *p->right_extruder);
            //}
            p->image_printer_bed->SetBitmap(create_scaled_bitmap(image_path, this, PRINTER_THUMBNAIL_SIZE.GetHeight()));
        } else {
            AMSCountPopupWindow::UpdateAMSCount(0, p->single_extruder);
            //if (!p->is_switching_diameter)
                update_extruder_diameter(0, *p->single_extruder);

            // ORCA sync unified nozzle combo box
            p->combo_nozzle_dia->Clear();
            for (size_t i = 0; i < diameters.size(); ++i)
                p->combo_nozzle_dia->Append(diameters[i], {});
            p->combo_nozzle_dia->SetSelection((*p->single_extruder).combo_diameter->GetSelection());
            
            // ORCA update nozzle type
            const auto& full_config = wxGetApp().preset_bundle->full_config();
            wxString nozzle_type = "-";
            const ConfigOptionEnumsGenericNullable* cfg_nozzle_type = full_config.option<ConfigOptionEnumsGenericNullable>("nozzle_type");
            if(cfg_nozzle_type != nullptr){
                std::vector<NozzleType> nozzle_types(cfg_nozzle_type->size());
                for (size_t idx = 0; idx < cfg_nozzle_type->size(); ++idx)
                    nozzle_types[idx] = NozzleType(cfg_nozzle_type->values[idx]);
                nozzle_type = _L( // NEEDFIX this part can be replaced with shorter names
                    nozzle_types[0] == ntHardenedSteel   ? "Hardened Steel" :
                    nozzle_types[0] == ntStainlessSteel  ? "Stainless Steel" :
                    nozzle_types[0] == ntTungstenCarbide ? "Tungsten Carbide" :
                    nozzle_types[0] == ntBrass           ? "Brass"
                                                         : "-" // Undefined
                );
            }
            p->label_nozzle_type->SetLabel(nozzle_type);
            p->label_nozzle_type->SetToolTip(nozzle_type == "-" ? "" : nozzle_type);

            p->image_printer_bed->SetBitmap(create_scaled_bitmap(image_path, this, PRINTER_THUMBNAIL_SIZE.GetHeight()));
        }

        if (GUI::wxGetApp().plater())
            GUI::wxGetApp().plater()->update_machine_sync_status();

        Layout();

        break;
    }

    default: break;
    }

    // Synchronize config.ini with the current selections.
    wxGetApp().preset_bundle->export_selections(*wxGetApp().app_config);

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": exit.");
}

//BBS
void Sidebar::update_presets_from_to(Slic3r::Preset::Type preset_type, std::string from, std::string to)
{
    PresetBundle &preset_bundle = *wxGetApp().preset_bundle;

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": enter, preset_type %1%, from %2% to %3%")%preset_type %from %to;

    switch (preset_type) {
    case Preset::TYPE_FILAMENT:
    {
        const size_t filament_cnt = p->combos_filament.size();
        for (auto it = preset_bundle.filament_presets.begin(); it != preset_bundle.filament_presets.end(); it++)
        {
            if ((*it).compare(from) == 0) {
                (*it) = to;
            }
        }
        for (size_t i = 0; i < filament_cnt; i++)
            p->combos_filament[i]->update();
        break;
    }

    default: break;
    }

    // Synchronize config.ini with the current selections.
    wxGetApp().preset_bundle->export_selections(*wxGetApp().app_config);

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": exit!");
}

BedType Sidebar::get_cur_select_bed_type() {
    int selection = p->combo_printer_bed->GetSelection();
    if (selection < 0 && selection >= m_cur_combox_bed_types.size()) {
        p->combo_printer_bed->SetSelection(0);
        selection = 0;
    }
    auto select_bed_type = m_cur_combox_bed_types[selection];
    return select_bed_type;
}

std::string Sidebar::get_cur_select_bed_image()
{
    auto select_bed_type   = get_cur_select_bed_type();
    //auto series_suffix_str = m_cur_image_bed_type.empty() ? "" : ("_" + m_cur_image_bed_type);
    auto image_path        = bed_type_thumbnails[select_bed_type];// + series_suffix_str;
    return image_path;
}

void Sidebar::set_bed_type_accord_combox(BedType bed_type) {
    for (size_t i = 0; i < m_cur_combox_bed_types.size(); i++) {
        if (m_cur_combox_bed_types[i] == bed_type) {
            p->combo_printer_bed->SelectAndNotify(i);
            return;
        }
    }
    p->combo_printer_bed->SelectAndNotify(0);
}

bool Sidebar::reset_bed_type_combox_choices(bool is_sidebar_init)
{
    if (!p->combo_printer_bed) {
        return false;
    }

    auto pm = p->plater->get_curr_printer_model();
    if (pm) {
        if (m_cur_image_bed_type != pm->image_bed_type) {
            m_cur_image_bed_type = pm->image_bed_type;
        }
    }
    if (m_last_combo_bedtype_count != 0 && pm) {
        auto cur_count = (int) BedType::btCount - 1 - pm->not_support_bed_types.size();
        if (cur_count == m_last_combo_bedtype_count) {//no change
            return false;
        }
    }
    const ConfigOptionDef *bed_type_def = print_config_def.get("curr_bed_type");
    p->combo_printer_bed->Clear();
    m_cur_combox_bed_types.clear();
    if (pm &&bed_type_def && bed_type_def->enum_keys_map) {
        int index = 0;
        for (auto item : bed_type_def->enum_labels) {
            index++;
            bool find = std::find(pm->not_support_bed_types.begin(), pm->not_support_bed_types.end(), item) != pm->not_support_bed_types.end();
            if (find) {
                continue;
            }
            m_cur_combox_bed_types.emplace_back(BedType(index));//BedType //btPC =1
            p->combo_printer_bed->AppendString(_L(item));
        }
    }
    else {
        m_cur_image_bed_type = "";
        int index = 0;
        for (auto item : bed_type_def->enum_labels) {
            index++;
            m_cur_combox_bed_types.emplace_back(BedType(index)); // BedType //btPC =1
            p->combo_printer_bed->AppendString(_L(item));
        }
    }
    m_last_combo_bedtype_count = p->combo_printer_bed->GetCount();
    if (!is_sidebar_init && &p->plater->get_partplate_list()) {
        p->plater->get_partplate_list().check_all_plate_local_bed_type(m_cur_combox_bed_types);
    }
    return true;
}

void Sidebar::change_top_border_for_mode_sizer(bool increase_border)
{
    // BBS
#if 0
    if (p->mode_sizer) {
        p->mode_sizer->set_items_flag(increase_border ? wxTOP : 0);
        p->mode_sizer->set_items_border(increase_border ? int(0.5 * wxGetApp().em_unit()) : 0);
    }
#endif
}

void Sidebar::msw_rescale()
{
    SetMinSize(wxSize(42 * wxGetApp().em_unit(), -1));
    p->m_panel_printer_title->GetSizer()->SetMinSize(-1, 3 * wxGetApp().em_unit());
    p->m_panel_filament_title->GetSizer()
        ->SetMinSize(-1, 3 * wxGetApp().em_unit());
    p->m_printer_icon->msw_rescale();
    p->m_printer_connect->msw_rescale();
    p->m_printer_bbl_sync->msw_rescale();
    p->m_printer_icon->msw_rescale();
    p->m_printer_setting->msw_rescale();

    p->panel_printer_preset->SetMinSize(FromDIP(PRINTER_PANEL_SIZE));
    p->panel_printer_preset->SetCornerRadius(FromDIP(PRINTER_PANEL_RADIUS));
    p->image_printer->SetSize(FromDIP(PRINTER_THUMBNAIL_SIZE));
    update_printer_thumbnail();
    p->combo_printer->Rescale();
    p->combo_printer->SetMaxSize(wxSize(-1, FromDIP(30))); // limiting height makes badge visible
    p->btn_edit_printer->msw_rescale();

    p->panel_nozzle_dia->SetMinSize(FromDIP(PRINTER_PANEL_SIZE));
    p->panel_nozzle_dia->SetCornerRadius(FromDIP(PRINTER_PANEL_RADIUS));
    p->combo_nozzle_dia->Rescale();

    p->panel_printer_bed->SetMinSize(FromDIP(PRINTER_PANEL_SIZE));
    p->panel_printer_bed->SetCornerRadius(FromDIP(PRINTER_PANEL_RADIUS));
    p->combo_printer_bed->Rescale();
    p->combo_printer_bed->SetMinSize(FromDIP(wxSize(18,-1))); // ORCA show only arrow
    p->combo_printer_bed->SetMaxSize(FromDIP(wxSize(18,-1))); // ORCA show only arrow
    bool isDual     = static_cast<wxBoxSizer *>(p->panel_printer_preset->GetSizer())->GetOrientation() == wxVERTICAL;
    auto image_path = get_cur_select_bed_image();
    p->image_printer_bed->SetBitmap(create_scaled_bitmap(image_path, this, PRINTER_THUMBNAIL_SIZE.GetHeight()));
    if (p->big_bed_image_popup){ // ORCA force rebuild frame. current wxwidget version not supports wxBITMAP_SCALE_FILL flag on wxStaticBitmap
                                 // also     wxImage scaledImage = bit_map.ConvertToImage(); scaledImage.Rescale(FromDIP(m_image_px), FromDIP(m_image_px), wxIMAGE_QUALITY_HIGH);
                                 // didnt worked as expected and it requires use on set_bitmap. so that will try to scale everytime
        p->big_bed_image_popup->Destroy();
        p->big_bed_image_popup = nullptr;
    }

    p->m_filament_icon->msw_rescale();
    p->m_bpButton_add_filament->msw_rescale();
    p->m_bpButton_del_filament->msw_rescale();
    p->m_bpButton_ams_filament->msw_rescale();
    p->m_bpButton_set_filament->msw_rescale();
    p->m_flushing_volume_btn->Rescale();
    set_flushing_volume_warning(is_flush_config_modified()); // ORCA reapply appearance

    //BBS
    p->left_extruder->Rescale();
    p->right_extruder->Rescale();
    p->single_extruder->Rescale();

    //p->btn_sync_printer->SetPaddingSize({FromDIP(6), FromDIP(12)});
    //p->btn_sync_printer->SetMinSize(BTN_SYNC_SIZE);
    //p->btn_sync_printer->Rescale();
#if 0
    if (p->mode_sizer)
        p->mode_sizer->msw_rescale();
#endif

    //for (PlaterPresetComboBox* combo : std::vector<PlaterPresetComboBox*> { p->combo_print,
    //                                                            //p->combo_sla_print,
    //                                                            //p->combo_sla_material,
    //                                                            //p->combo_printer
    //                                                            } )
    //    combo->msw_rescale();

    for (PlaterPresetComboBox* combo : p->combos_filament)
        combo->msw_rescale();

    // BBS
    //p->frequently_changed_parameters->msw_rescale();
    //obj_list()->msw_rescale();
    // BBS TODO: add msw_rescale for newly added windows
    // BBS
    //p->object_manipulation->msw_rescale();
    p->object_settings->msw_rescale();
    p->m_search_item->Rescale();
    p->m_search_item->GetTextCtrl()->SetSize(wxSize(-1, FromDIP(16)));
    p->m_search_bar->Layout();

    // BBS
#if 0
    p->object_info->msw_rescale();

    p->btn_send_gcode->msw_rescale();
//    p->btn_eject_device->msw_rescale();
    p->btn_export_gcode_removable->msw_rescale();
#ifdef _WIN32
    const int scaled_height = p->btn_export_gcode_removable->GetBitmapHeight();
#else
    const int scaled_height = p->btn_export_gcode_removable->GetBitmapHeight() + 4;
#endif
    p->btn_export_gcode->SetMinSize(wxSize(-1, scaled_height));
    p->btn_reslice     ->SetMinSize(wxSize(-1, scaled_height));
#endif
    p->scrolled->Layout();

    p->searcher.dlg_msw_rescale();
}

void Sidebar::sys_color_changed()
{
    wxWindowUpdateLocker noUpdates(this);

#if 0
    for (wxWindow* win : std::vector<wxWindow*>{ this, p->sliced_info->GetStaticBox(), p->object_info->GetStaticBox(), p->btn_reslice, p->btn_export_gcode })
        wxGetApp().UpdateDarkUI(win);
    p->object_info->msw_rescale();

    for (wxWindow* win : std::vector<wxWindow*>{ p->scrolled, p->presets_panel })
        wxGetApp().UpdateAllStaticTextDarkUI(win);
#endif
    //p->btn_sync_printer->SetIcon("printer_sync");
    p->m_printer_bbl_sync->msw_rescale();
    p->m_printer_connect->msw_rescale();
    // for (wxWindow* btn : std::vector<wxWindow*>{ p->btn_reslice, p->btn_export_gcode })
    //    wxGetApp().UpdateDarkUI(btn, true);
    p->m_printer_icon->msw_rescale();
    p->m_printer_setting->msw_rescale();
    p->m_printer_setting->msw_rescale();
    p->m_filament_icon->msw_rescale();
    p->m_bpButton_add_filament->msw_rescale();
    p->m_bpButton_del_filament->msw_rescale();
    p->m_bpButton_ams_filament->msw_rescale();
    p->m_bpButton_set_filament->msw_rescale();
    p->m_flushing_volume_btn->Rescale();
    set_flushing_volume_warning(is_flush_config_modified()); // ORCA reapply appearance

    // BBS
#if 0
    if (p->mode_sizer)
        p->mode_sizer->msw_rescale();
    p->frequently_changed_parameters->sys_color_changed();
#endif
    p->object_settings->sys_color_changed();

    //BBS: remove print related combos
#if 0
    for (PlaterPresetComboBox* combo : std::vector<PlaterPresetComboBox*>{  p->combo_print,
                                                                p->combo_sla_print,
                                                                p->combo_sla_material,
                                                                p->combo_printer })
        combo->sys_color_changed();
#endif
    p->combo_printer->sys_color_changed();
    for (PlaterPresetComboBox* combo : p->combos_filament)
        combo->sys_color_changed();

    if (p->big_bed_image_popup) // ORCA
        p->big_bed_image_popup->sys_color_changed();

    p->btn_edit_printer->msw_rescale();
    p->image_printer->SetSize(FromDIP(PRINTER_THUMBNAIL_SIZE));
    p->image_printer_bed->SetSize(FromDIP(PRINTER_THUMBNAIL_SIZE));

    // call a kill focus event to ensure new colors applied
    for (ComboBox* combo : std::vector<ComboBox*>{p->combo_printer, p->combo_nozzle_dia, p->combo_printer_bed}){
        wxFocusEvent fakeEvent(wxEVT_KILL_FOCUS);
        fakeEvent.SetEventObject(combo);
        combo->HandleWindowEvent(fakeEvent);
    }

    // BBS
    obj_list()->sys_color_changed();
    obj_layers()->sys_color_changed();
    // BBS
    //p->object_manipulation->sys_color_changed();

    // btn...->msw_rescale() updates icon on button, so use it
    //p->btn_send_gcode->msw_rescale();
//    p->btn_eject_device->msw_rescale();
    //p->btn_export_gcode_removable->msw_rescale();

    p->scrolled->Layout();

    p->searcher.dlg_sys_color_changed();
}

void Sidebar::search()
{
    p->searcher.search();
}

void Sidebar::jump_to_option(const std::string& opt_key, Preset::Type type, const std::wstring& category)
{
    //const Search::Option& opt = p->searcher.get_option(opt_key, type);
    if (type == Preset::TYPE_PRINT) {
        auto tab = dynamic_cast<TabPrintModel*>(wxGetApp().params_panel()->get_current_tab());
        if (tab && tab->has_key(opt_key)) {
            tab->activate_option(opt_key, category);
            return;
        }
        wxGetApp().params_panel()->switch_to_global();
    }
    wxGetApp().get_tab(type)->activate_option(opt_key, category);
}

void Sidebar::jump_to_option(size_t selected)
{
    const Search::Option& opt = p->searcher.get_option(selected);
    jump_to_option(opt.opt_key(), opt.type, opt.category);

    // Switch to the Settings NotePad
//    wxGetApp().mainframe->select_tab();
}

// BBS. Move logic from Plater::on_extruders_change() to Sidebar::on_filament_count_change().
void Sidebar::on_filament_count_change(size_t num_filaments)
{
    auto& choices = combos_filament();

    if (num_filaments == choices.size())
        return;

    if (choices.size() == 1 || num_filaments == 1)
        choices[0]->GetDropDown().Invalidate();

    wxWindowUpdateLocker noUpdates_scrolled_panel(this);

    size_t i = choices.size();
    while (i < num_filaments)
    {
        PlaterPresetComboBox* choice/*{ nullptr }*/;
        init_filament_combo(&choice, i);
        int last_selection = choices.back()->GetSelection();
        choices.push_back(choice);

        // initialize selection
        choice->update();
        choice->SetSelection(last_selection);
        ++i;
    }

    // remove unused choices if any
    remove_unused_filament_combos(num_filaments);

    auto sizer = p->m_panel_filament_title->GetSizer();
    if (p->m_flushing_volume_btn != nullptr && sizer != nullptr) {
        if (num_filaments > 1) {
            sizer->Show(p->m_flushing_volume_btn);
            sizer->Show(p->m_bpButton_del_filament); // ORCA: Show delete filament button if multiple filaments
        } else {
            sizer->Hide(p->m_flushing_volume_btn);
            sizer->Hide(p->m_bpButton_del_filament); // ORCA: Hide delete filament button if there is only one filament
        }
    }

    auto min_size = p->m_panel_filament_content->GetSizer()->GetMinSize();
    if (min_size.y > p->m_panel_filament_content->GetMaxHeight())
        min_size.y = p->m_panel_filament_content->GetMaxHeight();
    p->m_panel_filament_content->SetMinSize(min_size);

    Layout();
    p->m_panel_filament_title->Refresh();
    update_ui_from_settings();
    update_dynamic_filament_list();
}

void Sidebar::on_filaments_delete(size_t filament_id)
{
    auto &choices = combos_filament();

    if (filament_id >= choices.size())
        return;

    if (choices.size() == 1)
        choices[0]->GetDropDown().Invalidate();

    wxWindowUpdateLocker noUpdates_scrolled_panel(this);

    // delete UI item
    if (filament_id < p->combos_filament.size()) {
        const int last            = p->combos_filament.size() - 1;
        auto      sizer_filaments = this->p->sizer_filaments->GetItem(last % 2)->GetSizer();
        sizer_filaments->Remove(last / 2);

        PlaterPresetComboBox* to_delete_combox = p->combos_filament[filament_id];
        (*p->combos_filament[last]).Destroy();
        p->combos_filament.pop_back();

        // BBS:  filament double columns
        auto sizer_filaments0 = this->p->sizer_filaments->GetItem((size_t) 0)->GetSizer();
        auto sizer_filaments1 = this->p->sizer_filaments->GetItem(1)->GetSizer();
        if (p->combos_filament.size() < 2) {
            sizer_filaments1->Clear();
        } else {
            size_t c0 = sizer_filaments0->GetChildren().GetCount();
            size_t c1 = sizer_filaments1->GetChildren().GetCount();
            if (c0 < c1)
                sizer_filaments1->Remove(c1 - 1);
            else if (c0 > c1)
                sizer_filaments1->AddStretchSpacer(1);
        }
    }

    auto sizer = p->m_panel_filament_title->GetSizer();
    if (p->m_flushing_volume_btn != nullptr && sizer != nullptr) {
        if (p->combos_filament.size() > 1) {
            sizer->Show(p->m_flushing_volume_btn);
            sizer->Show(p->m_bpButton_del_filament); // ORCA: Show delete filament button if multiple filaments
        } else {
            sizer->Hide(p->m_flushing_volume_btn);
            sizer->Hide(p->m_bpButton_del_filament); // ORCA: Hide delete filament button if there is only one filament
        }
    }

    for (size_t idx = filament_id ; idx < p->combos_filament.size(); ++idx) {
        p->combos_filament[idx]->update();
    }

    auto min_size = p->m_panel_filament_content->GetSizer()->GetMinSize();
    if (min_size.y > p->m_panel_filament_content->GetMaxHeight())
        min_size.y = p->m_panel_filament_content->GetMaxHeight();
    p->m_panel_filament_content->SetMinSize(min_size);

    Layout();
    p->m_panel_filament_title->Refresh();
    update_ui_from_settings();
    dynamic_filament_list.update();
}

void Sidebar::add_filament() {
    if (p->combos_filament.size() >= MAXIMUM_EXTRUDER_NUMBER) return;
    wxColour    new_col        = Plater::get_next_color_for_filament();
    add_custom_filament(new_col);
}

void Sidebar::delete_filament(size_t filament_id, int replace_filament_id) {
    if (is_new_project_in_gcode3mf()) { return; }
    if (p->combos_filament.size() <= 1) return;

    size_t filament_count = p->combos_filament.size() - 1;
    if (filament_id == size_t(-2)) {
        filament_id = p->m_menu_filament_id;
    }
    if (filament_id == size_t(-1)) {
        filament_id = filament_count;
    }

    if (filament_id > filament_count)
        return;

    if (wxGetApp().preset_bundle->is_the_only_edited_filament(filament_id) || (filament_id == 0)) {
        wxGetApp().get_tab(Preset::TYPE_FILAMENT)->select_preset(wxGetApp().preset_bundle->filament_presets[0], false, "", true);
    }

    if (p->editing_filament == filament_id || p->editing_filament >= filament_count) {
        p->editing_filament = -1;
    }

    wxGetApp().preset_bundle->update_num_filaments(filament_id);
    wxGetApp().plater()->get_partplate_list().on_filament_deleted(filament_count, filament_id);
    wxGetApp().plater()->on_filaments_delete(filament_count, filament_id, replace_filament_id > (int)filament_id ? (replace_filament_id - 1) : replace_filament_id);
    wxGetApp().get_tab(Preset::TYPE_PRINT)->update();
    wxGetApp().preset_bundle->export_selections(*wxGetApp().app_config);

    wxGetApp().plater()->update();
}

void Sidebar::change_filament(size_t from_id, size_t to_id)
{
    delete_filament(from_id, int(to_id));
}

void Sidebar::edit_filament()
{
    p->editing_filament = -1;
    if (p->m_menu_filament_id >= 0 && p->m_menu_filament_id < p->combos_filament.size()
            && p->combos_filament[p->m_menu_filament_id]->switch_to_tab())
        p->editing_filament = p->m_menu_filament_id; // sync with TabPresetComboxBox's m_filament_idx
}

void Sidebar::add_custom_filament(wxColour new_col) {
    if (is_new_project_in_gcode3mf()) { return; }
    if (p->combos_filament.size() >= MAXIMUM_EXTRUDER_NUMBER) return;

    int         filament_count = p->combos_filament.size() + 1;
    std::string new_color      = new_col.GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
    wxGetApp().preset_bundle->set_num_filaments(filament_count, new_color);
    wxGetApp().plater()->get_partplate_list().on_filament_added(filament_count);
    wxGetApp().plater()->on_filament_count_change(filament_count);
    wxGetApp().get_tab(Preset::TYPE_PRINT)->update();
    wxGetApp().preset_bundle->export_selections(*wxGetApp().app_config);
    auto_calc_flushing_volumes(filament_count - 1);
}

bool Sidebar::is_new_project_in_gcode3mf()
{
    if (p->plater->using_exported_file()) {
        auto          filename = p->plater->get_preview_only_filename();
        auto          text     = wxString::Format(_L("After completing your operation, %s project will be closed and create a new project."), filename);
        MessageDialog dlg(wxGetApp().plater(), text, _L("Warning"),
                          wxOK | wxICON_WARNING);
        dlg.ShowModal();
        p->plater->new_project();
        return true;
    }
    return false;
}

void Sidebar::on_bed_type_change(BedType bed_type)
{
    // btDefault option is not included in global bed type setting
    int sel_idx = (int)bed_type - 1;
    if (p->combo_printer_bed != nullptr) p->combo_printer_bed->SetSelection(sel_idx);
}

std::map<int, DynamicPrintConfig> Sidebar::build_filament_ams_list(MachineObject* obj)
{
    std::map<int, DynamicPrintConfig> filament_ams_list;
    if (!obj) return filament_ams_list;

    auto build_tray_config = [](DevAmsTray const &tray, std::string const &name, std::string ams_id, std::string slot_id) {
        BOOST_LOG_TRIVIAL(info) << boost::format("build_filament_ams_list: name %1% setting_id %2% type %3% color %4%")
                    % name % tray.setting_id % tray.m_fila_type % tray.color;
        DynamicPrintConfig tray_config;
        tray_config.set_key_value("filament_id", new ConfigOptionStrings{tray.setting_id});
        tray_config.set_key_value("tag_uid", new ConfigOptionStrings{tray.tag_uid});
        tray_config.set_key_value("ams_id", new ConfigOptionStrings{ams_id});
        tray_config.set_key_value("slot_id", new ConfigOptionStrings{slot_id});
        tray_config.set_key_value("filament_type", new ConfigOptionStrings{tray.m_fila_type});
        tray_config.set_key_value("tray_name", new ConfigOptionStrings{ name });
        tray_config.set_key_value("filament_colour", new ConfigOptionStrings{into_u8(wxColour("#" + tray.color).GetAsString(wxC2S_HTML_SYNTAX))});
        tray_config.set_key_value("filament_multi_colour", new ConfigOptionStrings{});
        tray_config.set_key_value("filament_colour_type", new ConfigOptionStrings{std::to_string(tray.ctype)});
        tray_config.set_key_value("filament_exist", new ConfigOptionBools{tray.is_exists});
        std::optional<FilamentBaseInfo> info;
        if (wxGetApp().preset_bundle) {
            info = wxGetApp().preset_bundle->get_filament_by_filament_id(tray.setting_id);
        }
        tray_config.set_key_value("filament_is_support", new ConfigOptionBools{ info.has_value() ? info->is_support : false});
        for (int i = 0; i < tray.cols.size(); ++i) {
            tray_config.opt<ConfigOptionStrings>("filament_multi_colour")->values.push_back(into_u8(wxColour("#" + tray.cols[i]).GetAsString(wxC2S_HTML_SYNTAX)));
        }
        return tray_config;
    };

    if (obj->ams_support_virtual_tray) {
        int extruder = 0x10000; // Main (first) extruder at right
        for (auto & vt_tray : obj->vt_slot) {
            filament_ams_list.emplace(extruder + stoi(vt_tray.id), build_tray_config(vt_tray, "Ext",vt_tray.id, "0"));//254 or 255
            extruder = 0;
        }
    }

    auto get_ams_name = [](int ams_id, int slot_id)->std::string {
        if (ams_id >= 0 && ams_id < 26) {
            char slot_name = slot_id + '1';
            return std::string(1, 'A' + ams_id) + std::string(1, slot_name);
        } else if (ams_id >= 128 && ams_id < 153) {
            return "HT-" + std::string(1, 'A' + (ams_id - 128));
        } else {
            assert(false);
        }
        return std::string();
    };

    auto list = obj->GetFilaSystem()->GetAmsList();
    for (auto ams : list) {
        int ams_id   = std::stoi(ams.first);
        int extruder = ams.second->GetExtruderId() ? 0 : 0x10000; // Main (first) extruder at right
        for (auto tray : ams.second->GetTrays()) {
            int  slot_id = std::stoi(tray.first);
            filament_ams_list.emplace(extruder + (ams_id * 4 + slot_id),
                                      build_tray_config(*tray.second, get_ams_name(ams_id, slot_id), std::to_string(ams_id), std::to_string(slot_id)));
        }
    }
    return filament_ams_list;
}

bool Sidebar::sync_extruder_list()
{
    bool only_external_material;
    return p->sync_extruder_list(only_external_material);
}

bool Sidebar::need_auto_sync_extruder_list_after_connect_priner(const MachineObject *obj)
{
    if(!obj)
        return false;

    std::string   machine_print_name = obj->get_show_printer_type();
    PresetBundle *preset_bundle      = wxGetApp().preset_bundle;
    std::string   target_model_id    = preset_bundle->printers.get_selected_preset().get_printer_type(preset_bundle);
    if (machine_print_name != target_model_id) {
        return false;
    }

    if (preset_bundle->get_printer_extruder_count() <= 1 || !obj->is_multi_extruders())
        return false;

    return true;
}

void Sidebar::update_sync_status(const MachineObject *obj)
{
    p->update_sync_status(obj);
}

int Sidebar::get_sidebar_pos_right_x()
{
    return this->GetScreenPosition().x + this->GetSize().x;
}

void Sidebar::on_size(SimpleEvent &e) {
    if (m_sna_dialog && m_sna_dialog->IsShown()) {
        pop_sync_nozzle_and_ams_dialog();
    }
    if (m_fna_dialog && m_fna_dialog->IsShown()) {
        pop_finsish_sync_ams_dialog();
    }
}

void Sidebar::on_full_screen(IntEvent &e) {
    if (m_sna_dialog) { m_sna_dialog->on_full_screen(e); }
    if (m_fna_dialog) { m_fna_dialog->on_full_screen(e); }
}

void Sidebar::get_big_btn_sync_pos_size(wxPoint &pt, wxSize &size)
{
    size = p->m_printer_bbl_sync->GetSize();
    pt = p->m_printer_bbl_sync->GetScreenPosition();
}

void Sidebar::get_small_btn_sync_pos_size(wxPoint &pt, wxSize &size) {
    size = ams_btn->GetSize();
    pt   = ams_btn->GetScreenPosition();
}

void Sidebar::load_ams_list(MachineObject* obj)
{
    std::map<int, DynamicPrintConfig> filament_ams_list = build_filament_ams_list(obj);

    bool device_change     = false;
    const std::string& device = obj ? obj->get_dev_id() : "";
    if (p->ams_list_device != device) {
        p->ams_list_device = device;
        device_change      = true;
    }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": %1% items") % filament_ams_list.size();
    if (wxGetApp().preset_bundle->filament_ams_list == filament_ams_list && !device_change)
    {
        return;
    }
    wxGetApp().preset_bundle->filament_ams_list = filament_ams_list;

    for (auto c : p->combos_filament){
        c->update();
        if (device_change) {
            c->ShowBadge(false);//change printer,then clear badge
        }
    }

    p->combo_printer->update();
}

void Sidebar::sync_ams_list(bool is_from_big_sync_btn)
{
    wxBusyCursor cursor;
    // Force load ams list
    auto obj = wxGetApp().getDeviceManager()->get_selected_machine();
    if (obj)
        GUI::wxGetApp().sidebar().load_ams_list(obj);

    auto & list = wxGetApp().preset_bundle->filament_ams_list;
    if (list.empty()) {
        auto printer_name = p->plater->get_selected_printer_name_in_combox();
        p->plater->pop_warning_and_go_to_device_page(printer_name, Plater::PrinterWarningType::NOT_CONNECTED, _L("Sync printer information"));
        return;
    }
    bool exist_at_list_one_filament =false;
    for (auto &cur : list) {
        auto temp_config    = cur.second;
        auto filament_type  = temp_config.opt_string("filament_type", 0u);
        auto filament_color = temp_config.opt_string("filament_colour", 0u);
        if (!filament_type.empty() || temp_config.opt_bool("filament_exist", 0u)) {
            exist_at_list_one_filament = true;
            break;
        }
    }
    if (!exist_at_list_one_filament) {
        if (!obj->is_filament_installed()) {
            p->plater->pop_warning_and_go_to_device_page("", Plater::PrinterWarningType::UNINSTALL_FILAMENT, _L("Sync printer information"));
            return;
        }
        p->plater->pop_warning_and_go_to_device_page("", Plater::PrinterWarningType::EMPTY_FILAMENT, _L("Sync printer information"));
        return;
    }
    if (!wxGetApp().plater()->is_same_printer_for_connected_and_selected()) {
        return;
    }
    std::string ams_filament_ids = wxGetApp().app_config->get("ams_filament_ids", p->ams_list_device);
    std::vector<std::string> list2;
    if (!ams_filament_ids.empty()) {
        boost::algorithm::split(list2, ams_filament_ids, boost::algorithm::is_any_of(","));
    }
    wxGetApp().plater()->update_all_plate_thumbnails(true);//preview thumbnail for sync_dlg
    SyncAmsInfoDialog::SyncInfo temp_info;
    temp_info.use_dialog_pos = false;
    temp_info.cancel_text_to_later = is_from_big_sync_btn;
    if (m_sync_dlg == nullptr) {
        m_sync_dlg = new SyncAmsInfoDialog(this, temp_info);
    } else {
        m_sync_dlg->set_info(temp_info);
    }
    int dlg_res{(int) wxID_CANCEL};
    if (m_sync_dlg->is_need_show()) {
        m_sync_dlg->deal_only_exist_ext_spool(obj);
        if (m_sync_dlg->is_dirty_filament()) {
            wxGetApp().get_tab(Preset::TYPE_FILAMENT)->select_preset(wxGetApp().preset_bundle->filament_presets[0], false, "", false, true);
            wxGetApp().preset_bundle->export_selections(*wxGetApp().app_config);
            dynamic_filament_list.update();
        }
        m_sync_dlg->set_check_dirty_fialment(false);
        dlg_res = m_sync_dlg->ShowModal();
    } else {
        dlg_res =(int) wxID_YES;
    }
    if (dlg_res == wxID_CANCEL)
        return;
    auto sync_result = m_sync_dlg->get_result();
    if (!sync_result.is_same_printer) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "check error: sync_result.is_same_printer value is false";
        return;
    }
    list2.resize(list.size());
    auto iter = list.begin();
    for (int i = 0; i < list.size(); ++i, ++iter) {
        auto & ams = iter->second;
        auto filament_id = ams.opt_string("filament_id", 0u);
        ams.set_key_value("filament_changed", new ConfigOptionBool{dlg_res == wxID_YES || list2[i] != filament_id});
        list2[i] = filament_id;
    }

    // BBS:Record consumables information before synchronization
    std::vector<string> color_before_sync;
    std::vector<bool>   is_support_before;
    DynamicPrintConfig& project_config = wxGetApp().preset_bundle->project_config;
    ConfigOptionStrings* color_opt = project_config.option<ConfigOptionStrings>("filament_colour");
    for (int i = 0; i < p->combos_filament.size(); ++i) {
        is_support_before.push_back(is_support_filament(i));
        color_before_sync.push_back(color_opt->values[i]);
    }
    MergeFilamentInfo merge_info;
    std::vector<std::pair<DynamicPrintConfig *,std::string>> unknowns;
    auto enable_append = wxGetApp().app_config->get_bool("enable_append_color_by_sync_ams");
    auto n             = wxGetApp().preset_bundle->sync_ams_list(unknowns, !sync_result.direct_sync, sync_result.sync_maps, enable_append, merge_info);
    wxString detail;
    for (auto & uk : unknowns) {
        auto tray_name     = uk.first->opt_string("tray_name", 0u);
        auto filament_type = uk.first->opt_string("filament_type", 0u);
        detail += from_u8("\n- " + tray_name + "(" + filament_type + ") ") + _L(uk.second);
    }
    if (n == 0) {
        MessageDialog dlg(this,
            _L("There are no compatible filaments, and sync is not performed.") + detail,
            _L("Sync filaments with AMS"), wxOK);
        dlg.ShowModal();
        return;
    }
    ams_filament_ids = boost::algorithm::join(list2, ",");
    wxGetApp().app_config ->set("ams_filament_ids", p->ams_list_device, ams_filament_ids);
    if (!unknowns.empty()) {
        MessageDialog dlg(this,
            _L("There are some unknown or incompatible filaments mapped to generic preset.\nPlease update Orca Slicer or restart Orca Slicer to check if there is an update to system presets.") + detail,
            _L("Sync filaments with AMS"), wxOK);
        dlg.ShowModal();
    }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "on_filament_count_change";
    wxGetApp().plater()->on_filament_count_change(n);
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "finish on_filament_count_change";
    for (auto& c : p->combos_filament)
        c->update();
    // Expand filament list
    p->m_panel_filament_content->SetMaxSize({-1, FromDIP(174)});
    auto min_size = p->m_panel_filament_content->GetSizer()->GetMinSize();
    if (min_size.y > p->m_panel_filament_content->GetMaxHeight())
        min_size.y = p->m_panel_filament_content->GetMaxHeight();
    p->m_panel_filament_content->SetMinSize({-1, min_size.y});
    // BBS:Synchronized consumables information
    // auto calculation of flushing volumes
    for (int i = 0; i < p->combos_filament.size(); ++i) {
        if (i >= color_before_sync.size()) {
            auto_calc_flushing_volumes(i);
        }
        else if(color_before_sync[i] != color_opt->values[i] && wxGetApp().app_config->get("auto_calculate_flush") != "disabled"){
            auto_calc_flushing_volumes(i);
        }
        else if(is_support_filament(i) !=is_support_before[i] && wxGetApp().app_config->get("auto_calculate_flush") == "all"){
            auto_calc_flushing_volumes(i);
        }
    }
    auto badge_combox_filament = [](PlaterPresetComboBox *c) {
        auto tip     = _L("Filament type and color information have been synchronized, but slot information is not included.");
        c->SetToolTip(tip);
        c->ShowBadge(true);
    };
    { // badge ams filament
        clear_combos_filament_badge();
        if (sync_result.direct_sync) {
            for (auto &c : p->combos_filament) {
                badge_combox_filament(c);
            }
        }
    }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "prepare enable_merge_color_by_sync_ams";
    if (!merge_info.is_empty() && wxGetApp().app_config->get_bool("enable_merge_color_by_sync_ams")) { // merge same color and preset filament//use same ams
        auto reduce_index = [](MergeFilamentInfo &merge_info,int value) {
            for (size_t i = 0; i < merge_info.merges.size(); i++) {
                auto &cur = merge_info.merges[i];
                for (size_t j = 0; j < cur.size(); j++) {
                    if (value < cur[j]) {
                        cur[j] = cur[j] - 1;
                    }
                }
            }
        };
        std::vector<bool> sync_ams_badges;
        for (auto iter : sync_result.sync_maps) {
            sync_ams_badges.push_back(false);
            if (iter.second.ams_id == "" || iter.second.slot_id == "") {
                continue;
            }
            sync_ams_badges.back() = true;
        }

        for (size_t i = 0; i < merge_info.merges.size(); i++) {
            auto& cur = merge_info.merges[i];
            for (int j = cur.size() -1; j >= 1 ; j--) {
                auto last_index = cur[j];
                change_filament(last_index, cur[0]);
                cur.erase(cur.begin() + j);
                sync_ams_badges.erase(sync_ams_badges.begin() + last_index);
                reduce_index(merge_info, last_index);
            }
        }
        for (size_t i = 0; i < sync_ams_badges.size(); i++) {
            if (sync_ams_badges[i] == true) {
                if (i < p->combos_filament.size()) {
                    auto &c = p->combos_filament[i];
                    badge_combox_filament(c);
                } else {
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << __LINE__ << "check error: p->combos_filament array out of bound";
                }
            }
        }
    } else {
        for (auto iter : sync_result.sync_maps) {
            if (iter.second.ams_id == "" || iter.second.slot_id == "") {
                continue;
            }
            auto temp_index = iter.first;
            if (temp_index < p->combos_filament.size() && temp_index >= 0) {
                auto &c        = p->combos_filament[temp_index];
                badge_combox_filament(c);
            }
        }
    }
    Layout();

    wxGetApp().get_tab(Preset::TYPE_FILAMENT)->select_preset(wxGetApp().preset_bundle->filament_presets[0]);
    wxGetApp().preset_bundle->export_selections(*wxGetApp().app_config);
    update_dynamic_filament_list();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "begin pop_finsish_sync_ams_dialog";
    pop_finsish_sync_ams_dialog();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "finish pop_finsish_sync_ams_dialog";
}

bool Sidebar::should_show_SEMM_buttons()
{
    PresetBundle &preset_bundle = *wxGetApp().preset_bundle;
    bool is_bbl_vendor = preset_bundle.is_bbl_vendor();
    auto cfg = preset_bundle.printers.get_edited_preset().config;

    return cfg.opt_bool("single_extruder_multi_material") || is_bbl_vendor;
}

void Sidebar::show_SEMM_buttons(bool bshow)
{
    if(p->m_bpButton_add_filament)
        p->m_bpButton_add_filament->Show(bshow);
    if (p->m_bpButton_del_filament && p->combos_filament.size() > 1) // ORCA add filament count as condition to prevent showing Flushing volumes and Del Filament icon visible while only 1 filament exist
        p->m_bpButton_del_filament->Show(bshow);
    if (p->m_flushing_volume_btn && p->combos_filament.size() > 1) // ORCA add filament count as condition to prevent showing Flushing volumes and Del Filament icon visible while only 1 filament exist
        p->m_flushing_volume_btn->Show(bshow);
    Layout();
}

void Sidebar::update_dynamic_filament_list()
{
    dynamic_filament_list.update();
    dynamic_filament_list_1_based.update();
}

PlaterPresetComboBox* Sidebar::printer_combox()
{
    return p->combo_printer;
}

ObjectList* Sidebar::obj_list()
{
    // BBS
    //return obj_list();
    return p->m_object_list;
}

ObjectSettings* Sidebar::obj_settings()
{
    return p->object_settings;
}

ObjectLayers* Sidebar::obj_layers()
{
    return p->object_layers;
}

wxPanel* Sidebar::scrolled_panel()
{
    return p->scrolled;
}

wxPanel* Sidebar::print_panel()
{
    return p->m_panel_print_content;
}

wxPanel* Sidebar::filament_panel()
{
    return p->m_panel_filament_content;
}

ConfigOptionsGroup* Sidebar::og_freq_chng_params(const bool is_fff)
{
    // BBS
#if 0
    return p->frequently_changed_parameters->get_og(is_fff);
#endif
    return NULL;
}

wxButton* Sidebar::get_wiping_dialog_button()
{
#if 0
    return p->frequently_changed_parameters->get_wiping_dialog_button();
#endif
    return NULL;
}

void Sidebar::set_flushing_volume_warning(const bool flushing_volume_modify)
{
    if(flushing_volume_modify){
        p->m_flushing_volume_btn->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
        p->m_flushing_volume_btn->SetBorderColor(wxColour("#FF6F00"));
    }
    else
        p->m_flushing_volume_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Compact);
}

void Sidebar::enable_buttons(bool enable)
{
#if 0
    p->btn_reslice->Enable(enable);
    p->btn_export_gcode->Enable(enable);
    p->btn_send_gcode->Enable(enable);
//    p->btn_eject_device->Enable(enable);
    p->btn_export_gcode_removable->Enable(enable);
#endif
}

bool Sidebar::show_reslice(bool show)          const { return p->btn_reslice->Show(show); }
bool Sidebar::show_export(bool show)           const { return p->btn_export_gcode->Show(show); }
bool Sidebar::show_send(bool show)             const { return p->btn_send_gcode->Show(show); }
bool Sidebar::show_export_removable(bool show) const { return p->btn_export_gcode_removable->Show(show); }
//bool Sidebar::show_eject(bool show)            const { return p->btn_eject_device->Show(show); }
//bool Sidebar::get_eject_shown()                const { return p->btn_eject_device->IsShown(); }

bool Sidebar::is_multifilament()
{
    return p->combos_filament.size() > 1;
}

void Sidebar::deal_btn_sync() {
    m_begin_sync_printer_status = true;
    bool only_external_material;
    auto ok = p->sync_extruder_list(only_external_material);
    if (ok) {
        pop_sync_nozzle_and_ams_dialog();
    }
    m_begin_sync_printer_status = false;
    wxGetApp().plater()->update_machine_sync_status();
}

template<typename T> void setup_dialog_position(T& info)
{
    auto  plater   = wxGetApp().plater();
    auto& sidebar  = plater->sidebar();
    auto  docking  = plater->get_sidebar_docking_state();
    bool  on_right = true;

    if (docking == Sidebar::Left) {
        on_right = true;
    } else if (docking == Sidebar::Right) {
        on_right = false;
    } else {
        // If sidebar is too close to screen right edge, then move the dialog to the left side instead

        auto screen_width = wxDisplay(&sidebar).GetClientArea().GetSize().x;
        auto right_space  = screen_width - sidebar.get_sidebar_pos_right_x();
        if (right_space < sidebar.FromDIP(400)) {
            on_right = false;
        }
    }

    if (on_right) {
        info.dialog_pos.x           = sidebar.get_sidebar_pos_right_x() + sidebar.FromDIP(5);
        info.dialog_pos_align_right = true;
    } else {
        info.dialog_pos.x           = sidebar.GetScreenPosition().x - sidebar.FromDIP(5);
        info.dialog_pos_align_right = false;
    }
}

void Sidebar::pop_sync_nozzle_and_ams_dialog() {
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " begin pop_sync_nozzle_and_ams_dialog";
    wxTheApp->CallAfter([this]() {
        SyncNozzleAndAmsDialog::InputInfo temp_na_info;
        wxPoint                           big_btn_pt;
        wxSize                            big_btn_size;
        wxGetApp().plater()->sidebar().get_big_btn_sync_pos_size(big_btn_pt, big_btn_size);
        temp_na_info.dialog_pos = big_btn_pt + wxPoint(big_btn_size.x, big_btn_size.y) + wxPoint(FromDIP(big_btn_size.x / 10.f - 5), FromDIP(big_btn_size.y / 10.f));

        temp_na_info.dialog_pos.y += FromDIP(2);
        setup_dialog_position(temp_na_info);

        wxPoint small_btn_pt;
        wxSize  small_btn_size;
        get_small_btn_sync_pos_size(small_btn_pt, small_btn_size);
        temp_na_info.ams_btn_pos = small_btn_pt + wxPoint(small_btn_size.x / 2, small_btn_size.y / 2);
        if (m_fna_dialog) { m_fna_dialog->on_hide(); }
        if (m_sna_dialog) {
            m_sna_dialog->Destroy();
            m_sna_dialog = nullptr;
        }
        m_sna_dialog = new SyncNozzleAndAmsDialog(temp_na_info);
        m_sna_dialog->on_show();
    });
}

void Sidebar::pop_finsish_sync_ams_dialog()
{
    wxTheApp->CallAfter([this]() {
        wxPoint small_btn_pt;
        wxSize  small_btn_size;
        get_small_btn_sync_pos_size(small_btn_pt, small_btn_size);

        FinishSyncAmsDialog::InputInfo temp_fsa_info;
        temp_fsa_info.dialog_pos.y                       = small_btn_pt.y;
        setup_dialog_position(temp_fsa_info);
        temp_fsa_info.ams_btn_pos                        = small_btn_pt + wxPoint(small_btn_size.x / 2, small_btn_size.y / 2);
        if (m_sna_dialog) { m_sna_dialog->on_hide(); }
        if (m_fna_dialog) {
            m_fna_dialog->Destroy();
            m_fna_dialog = nullptr;
        }
        m_fna_dialog = new FinishSyncAmsDialog(temp_fsa_info);
        m_fna_dialog->on_show();
    });

}

static std::vector<Search::InputInfo> get_search_inputs(ConfigOptionMode mode)
{
    std::vector<Search::InputInfo> ret {};

    auto& tabs_list = wxGetApp().tabs_list;
    auto print_tech = wxGetApp().preset_bundle->printers.get_selected_preset().printer_technology();
    for (auto tab : tabs_list)
        if (tab->supports_printer_technology(print_tech))
            ret.emplace_back(Search::InputInfo {tab->get_config(), tab->type(), mode});

    return ret;
}

void Sidebar::update_searcher()
{
    p->searcher.init(get_search_inputs(m_mode));
}

void Sidebar::update_mode()
{
    m_mode = wxGetApp().get_mode();

    //BBS: remove print related combos
    update_searcher();

    wxWindowUpdateLocker noUpdates(this);

    // BBS
    //obj_list()->get_sizer()->Show(m_mode > comSimple);

    obj_list()->unselect_objects();
    obj_list()->update_selections();
//    obj_list()->update_object_menu();

    Layout();
}

bool Sidebar::is_collapsed() { return p->plater->is_sidebar_collapsed(); }

void Sidebar::collapse(bool collapse) { p->plater->collapse_sidebar(collapse); }

#ifdef _MSW_DARK_MODE
void Sidebar::show_mode_sizer(bool show)
{
    //p->mode_sizer->Show(show);
}
#endif

void Sidebar::update_ui_from_settings()
{
    // BBS
    //p->object_manipulation->update_ui_from_settings();
    // update Cut gizmo, if it's open
    p->plater->canvas3D()->update_gizmos_on_off_state();
    p->plater->set_current_canvas_as_dirty();
    p->plater->get_current_canvas3D()->request_extra_frame();
#if 0
    p->object_list->apply_volumes_order();
#endif
}

bool Sidebar::show_object_list(bool show) const
{
    p->m_search_bar->Show(show);
    if (!p->m_object_list->Show(show))
        return false;
    if (!show)
        p->object_layers->Show(false);
    else
        p->m_object_list->part_selection_changed();
    p->scrolled->Layout();
    return true;
}

void Sidebar::finish_param_edit() { p->editing_filament = -1; }

std::vector<PlaterPresetComboBox*>& Sidebar::combos_filament()
{
    return p->combos_filament;
}

void Sidebar::clear_combos_filament_badge()
{
    auto &combos_filament = p->combos_filament;
    for (auto &c : combos_filament) { // clear flag
        c->ShowBadge(false);
    }
}

void Sidebar::udpate_combos_filament_badge() {
    auto &combos_filament = p->combos_filament;
    for (auto &c : combos_filament) {
        auto selection   = c->GetSelection();
        auto select_flag = c->GetFlag(selection);
        auto ok          = select_flag == (int) PresetComboBox::FilamentAMSType::FROM_AMS;
        c->ShowBadge(ok);
    }

}

Search::OptionsSearcher& Sidebar::get_searcher()
{
    return p->searcher;
}

std::string& Sidebar::get_search_line()
{
    return p->searcher.search_string();
}

static std::map<std::string, std::string> printer_thumbnails = {};

void Sidebar::update_printer_thumbnail()
{
    auto& preset_bundle = wxGetApp().preset_bundle;
    Preset & selected_preset = preset_bundle->printers.get_edited_preset();
    std::string printer_type    = selected_preset.get_current_printer_type(preset_bundle);
    if (printer_thumbnails.find(printer_type) != printer_thumbnails.end()) // Use known cache first
        p->image_printer->SetBitmap(create_scaled_bitmap(printer_thumbnails[printer_type], this, PRINTER_THUMBNAIL_SIZE.GetHeight()));
    else {
        try {
            // No cache, try dedicated printer preview
            p->image_printer->SetBitmap(create_scaled_bitmap("printer_preview_" + printer_type, this, 48));
            // Success, cache it
            printer_thumbnails[printer_type] = "printer_preview_" + printer_type;
            return;
        } catch (...) {}

        // Orca: try to use the printer model cover as the thumbnail
        const auto model_name = selected_preset.config.opt_string("printer_model");
        std::string cover_file = model_name + "_cover.png";
        for (auto vendor_profile : preset_bundle->vendors) {
            for (auto vendor_model : vendor_profile.second.models) {
                if (vendor_model.name == model_name) {
                    // Try to find the printer cover
                    boost::filesystem::path cover_path = boost::filesystem::absolute(boost::filesystem::path(resources_dir()) /
                                                                                     "/profiles/" / vendor_profile.second.id / cover_file)
                                                             .make_preferred();
                    if (boost::filesystem::exists(cover_path)) {
                        try {
                            p->image_printer->SetBitmap(create_scaled_bitmap(cover_path.string(), this, PRINTER_THUMBNAIL_SIZE.GetHeight()));
                            printer_thumbnails[printer_type] = cover_path.string(); // Cache the path so we don't look up again
                            return;
                        } catch (...) {}
                    }
                }
            }
        }
        p->image_printer->SetBitmap(create_scaled_bitmap("printer_placeholder", this, PRINTER_THUMBNAIL_SIZE.GetHeight()));
        printer_thumbnails[printer_type] = "printer_placeholder"; // Avoid unnecessary try
    }
}

void Sidebar::auto_calc_flushing_volumes(const int filament_idx, const int extruder_id) {

    std::vector<int> filament_indices;
    std::vector<int> extruder_indices;

    auto& preset_bundle = wxGetApp().preset_bundle;
    auto filament_ptr = preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    int filament_count = filament_ptr ? filament_ptr->size() : 0;
    int extruder_count = preset_bundle->get_printer_extruder_count();

    if (filament_idx < 0) {
        filament_indices.resize(filament_count);
        std::iota(filament_indices.begin(), filament_indices.end(), 0);
    }
    else {
        filament_indices.emplace_back(filament_idx);
    }

    if (extruder_id < 0) {
        extruder_indices.resize(extruder_count);
        std::iota(extruder_indices.begin(), extruder_indices.end(), 0);
    }
    else {
        extruder_indices.emplace_back(extruder_id);
    }

    for (auto eidx : extruder_indices) {
        for (auto fidx : filament_indices) {
            auto_calc_flushing_volumes_internal(fidx, eidx);
        }
    }

    wxGetApp().preset_bundle->export_selections(*wxGetApp().app_config);
    wxGetApp().plater()->update_project_dirty_from_presets();
    wxPostEvent(this, SimpleEvent(EVT_SCHEDULE_BACKGROUND_PROCESS, this));
    auto has_modify = is_flush_config_modified();
    set_flushing_volume_warning(has_modify);
    p->plater->update();
}


void Sidebar::auto_calc_flushing_volumes_internal(const int modify_id, const int extruder_id)
{
    auto& preset_bundle = wxGetApp().preset_bundle;
    auto& project_config = preset_bundle->project_config;
    const auto& full_config = wxGetApp().preset_bundle->full_config();
    auto& ams_multi_color_filament = preset_bundle->ams_multi_color_filment;
    size_t extruder_nums = preset_bundle->get_printer_extruder_count();
    int nozzle_flush_dataset = full_config.option<ConfigOptionIntsNullable>("nozzle_flush_dataset")->values[extruder_id];
    std::vector<double> init_matrix = get_flush_volumes_matrix((project_config.option<ConfigOptionFloats>("flush_volumes_matrix"))->values, extruder_id, extruder_nums);

    const std::vector<int>& min_flush_volumes = get_min_flush_volumes(full_config, extruder_id);

    ConfigOptionFloat* flush_multi_opt = project_config.option<ConfigOptionFloat>("flush_multiplier");
    float flush_multiplier = flush_multi_opt ? flush_multi_opt->getFloat() : 1.f;
    std::vector<double> matrix = init_matrix;
    int m_max_flush_volume = Slic3r::g_max_flush_volume;
    unsigned int m_number_of_extruders = (int)(sqrt(init_matrix.size()) + 0.001);

    const std::vector<std::string> extruder_colours = wxGetApp().plater()->get_extruder_colors_from_plater_config();
    std::vector<std::vector<wxColour>> multi_colours;

    // Support for multi-color filament
    for (int i = 0; i < extruder_colours.size(); ++i) {
        std::vector<wxColour> single_filament;
        if (i < ams_multi_color_filament.size()) {
            if (!ams_multi_color_filament[i].empty()) {
                std::vector<std::string> colors = ams_multi_color_filament[i];
                for (int j = 0; j < colors.size(); ++j) {
                    single_filament.push_back(wxColour(colors[j]));
                }
                multi_colours.push_back(single_filament);
                continue;
            }
        }

        single_filament.push_back(wxColour(extruder_colours[i]));
        multi_colours.push_back(single_filament);
    }

    if (modify_id >= 0 && modify_id < multi_colours.size()) {
        for (int i = 0; i < multi_colours.size(); ++i) {
            // from to modify
            int from_idx = i;
            if (from_idx != modify_id) {
                Slic3r::FlushVolCalculator calculator(min_flush_volumes[from_idx], m_max_flush_volume, nozzle_flush_dataset);
                int flushing_volume = 0;
                bool is_from_support = is_support_filament(from_idx);
                bool is_to_support = is_support_filament(modify_id);
                if (is_to_support) {
                    flushing_volume = Slic3r::g_flush_volume_to_support;
                }
                else {
                    for (int j = 0; j < multi_colours[from_idx].size(); ++j) {
                        const wxColour& from = multi_colours[from_idx][j];
                        for (int k = 0; k < multi_colours[modify_id].size(); ++k) {
                            const wxColour& to = multi_colours[modify_id][k];
                            int volume = calculator.calc_flush_vol(from.Alpha(), from.Red(), from.Green(), from.Blue(), to.Alpha(), to.Red(), to.Green(), to.Blue());
                            flushing_volume = std::max(flushing_volume, volume);
                        }
                    }
                    if (is_from_support)
                        flushing_volume = std::max(flushing_volume, Slic3r::g_min_flush_volume_from_support);
                }
                matrix[m_number_of_extruders * from_idx + modify_id] = flushing_volume;
            }

            // modify to to
            int to_idx = i;
            if (to_idx != modify_id) {
                Slic3r::FlushVolCalculator calculator(min_flush_volumes[modify_id], m_max_flush_volume, nozzle_flush_dataset);
                bool is_from_support = is_support_filament(modify_id);
                bool is_to_support = is_support_filament(to_idx);
                int flushing_volume = 0;
                if (is_to_support) {
                    flushing_volume = Slic3r::g_flush_volume_to_support;
                }
                else {
                    for (int j = 0; j < multi_colours[modify_id].size(); ++j) {
                        const wxColour& from = multi_colours[modify_id][j];
                        for (int k = 0; k < multi_colours[to_idx].size(); ++k) {
                            const wxColour& to = multi_colours[to_idx][k];
                            int volume = calculator.calc_flush_vol(from.Alpha(), from.Red(), from.Green(), from.Blue(), to.Alpha(), to.Red(), to.Green(), to.Blue());
                            flushing_volume = std::max(flushing_volume, volume);
                        }
                    }
                    if (is_from_support)
                        flushing_volume = std::max(flushing_volume, Slic3r::g_min_flush_volume_from_support);

                    matrix[m_number_of_extruders * modify_id + to_idx] = flushing_volume;
                }
            }
        }
    }
    set_flush_volumes_matrix((project_config.option<ConfigOptionFloats>("flush_volumes_matrix"))->values, matrix, extruder_id, extruder_nums);
}

void Sidebar::jump_to_object(ObjectDataViewModelNode* item)
{
    p->jump_to_object(item);
}

void Sidebar::can_search()
{
    p->can_search();
}

class PlaterDropTarget : public wxFileDropTarget
{
public:
    PlaterDropTarget(MainFrame& mainframe, Plater& plater) : m_mainframe(mainframe), m_plater(plater) {
        this->SetDefaultAction(wxDragCopy);
    }

    virtual bool OnDropFiles(wxCoord x, wxCoord y, const wxArrayString &filenames);

private:
    MainFrame& m_mainframe;
    Plater& m_plater;
};

namespace {
bool emboss_svg(Plater& plater, const wxString &svg_file, const Vec2d& mouse_drop_position)
{
    std::string svg_file_str = into_u8(svg_file);
    GLCanvas3D* canvas = plater.canvas3D();
    if (canvas == nullptr)
        return false;
    auto base_svg = canvas->get_gizmos_manager().get_gizmo(GLGizmosManager::Svg);
    if (base_svg == nullptr)
        return false;
    GLGizmoSVG* svg = dynamic_cast<GLGizmoSVG *>(base_svg);
    if (svg == nullptr)
        return false;

    // Refresh hover state to find surface point under mouse
    wxMouseEvent evt(wxEVT_MOTION);
    evt.SetPosition(wxPoint(mouse_drop_position.x(), mouse_drop_position.y()));
    canvas->on_mouse(evt); // call render where is call GLCanvas3D::_picking_pass()

    return svg->create_volume(svg_file_str, mouse_drop_position, ModelVolumeType::MODEL_PART);
}
}

// State to manage showing after export notifications and device ejecting
enum ExportingStatus{
    NOT_EXPORTING,
    EXPORTING_TO_REMOVABLE,
    EXPORTING_TO_LOCAL
};


// TODO: listen on dark ui change
class FloatFrame : public wxAuiFloatingFrame
{
public:
    FloatFrame(wxWindow* parent, wxAuiManager* ownerMgr, const wxAuiPaneInfo& pane) : wxAuiFloatingFrame(parent, ownerMgr, pane)
    {
        wxGetApp().UpdateFrameDarkUI(this);
    }
};

class AuiMgr : public wxAuiManager
{
public:
    AuiMgr() : wxAuiManager(){}

    virtual wxAuiFloatingFrame* CreateFloatingFrame(wxWindow* parent, const wxAuiPaneInfo& p) override
    {
        return new FloatFrame(parent, this, p);
    }
};

// Plater / private
struct Plater::priv
{
    // PIMPL back pointer ("Q-Pointer")
    Plater *q;
    Sidebar *  sidebar;
    MainFrame *main_frame;

    MenuFactory menus;

    SelectMachineDialog* m_select_machine_dlg = nullptr;
    SendMultiMachinePage* m_send_multi_dlg = nullptr;
    SendToPrinterDialog* m_send_to_sdcard_dlg = nullptr;
    PublishDialog *m_publish_dlg = nullptr;

    // Data
    Slic3r::DynamicPrintConfig *config;        // FIXME: leak?
    Slic3r::Print               fff_print;
    Slic3r::SLAPrint            sla_print;
    Slic3r::Model               model;
    PrinterTechnology           printer_technology = ptFFF;
    Slic3r::GCodeProcessorResult gcode_result;

    // GUI elements
    AuiMgr m_aui_mgr;
    wxString m_default_window_layout;
    wxPanel* current_panel{ nullptr };
    std::vector<wxPanel*> panels;

    struct SidebarLayout
    {
        bool                  is_enabled{false};
        bool                  is_collapsed{false};
        bool                  show{false};
    } sidebar_layout;
    Bed3D bed;
    Camera camera;
    //BBS: partplate related structure
    PartPlateList partplate_list;
    //BBS: add a flag to ignore cancel event
    bool m_ignore_event{false};
    bool m_slice_all{false};
    bool m_is_slicing {false};
    bool auto_reslice_pending {false};
    bool auto_reslice_after_cancel {false};
    bool m_is_publishing {false};
    int m_is_RightClickInLeftUI{-1};
    int m_cur_slice_plate;
    //BBS: m_slice_all in .gcode.3mf file case, set true when slice all
    bool m_slice_all_only_has_gcode{ false };

    bool m_need_update{false};
    //BBS: add popup object table logic
    //ObjectTableDialog* m_popup_table{ nullptr };

#if ENABLE_ENVIRONMENT_MAP
    GLTexture environment_texture;
#endif // ENABLE_ENVIRONMENT_MAP
    Mouse3DController mouse3d_controller;
    View3D* view3D;
    // BBS
    //GLToolbar view_toolbar;
    GLToolbar collapse_toolbar;
    Preview *preview;
    AssembleView* assemble_view { nullptr };
    bool first_enter_assemble{ true };
    std::unique_ptr<NotificationManager> notification_manager;

    ProjectDirtyStateManager dirty_state;

    BackgroundSlicingProcess    background_process;
    bool suppressed_backround_processing_update { false };

    // TODO: A mechanism would be useful for blocking the plater interactions:
    // objects would be frozen for the user. In case of arrange, an animation
    // could be shown, or with the optimize orientations, partial results
    // could be displayed.
    //
    // UIThreadWorker can be used as a replacement for BoostThreadWorker if
    // no additional worker threads are desired (useful for debugging or profiling)
    PlaterWorker<BoostThreadWorker> m_worker;
    SLAImportDialog *               m_sla_import_dlg;

    int                         m_job_prepare_state;

    bool                        delayed_scene_refresh;
    std::string                 delayed_error_message;

    wxTimer                     background_process_timer;
    wxTimer                     auto_reslice_timer;

    std::string                 label_btn_export;
    std::string                 label_btn_send;

    bool                        show_render_statistic_dialog{ false };
    bool                        show_wireframe{ false };
    bool                        wireframe_enabled{ true };

    static const std::regex pattern_bundle;
    static const std::regex pattern_3mf;
    static const std::regex pattern_zip_amf;
    static const std::regex pattern_any_amf;
    static const std::regex pattern_prusa;

    bool m_is_dark = false;

    priv(Plater *q, MainFrame *main_frame);
    ~priv();


    bool need_update() const { return m_need_update; }
    void set_need_update(bool need_update) { m_need_update = need_update; }

    void set_plater_dirty(bool is_dirty) { dirty_state.set_plater_dirty(is_dirty); }
    bool is_project_dirty() const { return dirty_state.is_dirty(); }
    bool is_presets_dirty() const { return dirty_state.is_presets_dirty(); }
    void update_project_dirty_from_presets()
    {
        // BBS: backup
        Slic3r::put_other_changes();
        dirty_state.update_from_presets();
    }
    int save_project_if_dirty(const wxString& reason) {
        int res = wxID_NO;
        if (dirty_state.is_dirty()) {
            MainFrame* mainframe = wxGetApp().mainframe;
            if (mainframe->can_save_as()) {
                wxString suggested_project_name;
                wxString project_name = suggested_project_name = get_project_filename(".3mf");
                if (suggested_project_name.IsEmpty()) {
                    fs::path output_file = get_export_file_path(FT_3MF);
                    suggested_project_name = output_file.empty() ? _L("Untitled") : from_u8(output_file.stem().string());
                }
                res = MessageDialog(mainframe, reason + "\n" + format_wxstr(_L("Do you want to save changes to \"%1%\"?"), suggested_project_name),
                                    wxString(SLIC3R_APP_FULL_NAME), wxYES_NO | wxCANCEL).ShowModal();
                if (res == wxID_YES)
                    if (!mainframe->save_project_as(project_name))
                        res = wxID_CANCEL;
            }
        }
        return res;
    }
    void reset_project_dirty_after_save() { m_undo_redo_stack_main.mark_current_as_saved(); dirty_state.reset_after_save(); }
    void reset_project_dirty_initial_presets() { dirty_state.reset_initial_presets(); }

#if ENABLE_PROJECT_DIRTY_STATE_DEBUG_WINDOW
    void render_project_state_debug_window() const { dirty_state.render_debug_window(); }
#endif // ENABLE_PROJECT_DIRTY_STATE_DEBUG_WINDOW

    enum class UpdateParams {
        FORCE_FULL_SCREEN_REFRESH          = 1,
        FORCE_BACKGROUND_PROCESSING_UPDATE = 2,
        POSTPONE_VALIDATION_ERROR_MESSAGE  = 4,
    };
    void update(unsigned int flags = 0);
    void select_view(const std::string& direction);
    //BBS: add no_slice option
    void select_view_3D(const std::string& name, bool no_slice = true);
    void select_next_view_3D();

    bool is_preview_shown() const { return current_panel == preview; }
    bool is_preview_loaded() const { return preview->is_loaded(); }
    bool is_view3D_shown() const { return current_panel == view3D; }
    bool is_assemble_view_show() const { return current_panel == assemble_view; }

    bool are_view3D_labels_shown() const { return (current_panel == view3D) && view3D->get_canvas3d()->are_labels_shown(); }
    void show_view3D_labels(bool show) { if (current_panel == view3D) view3D->get_canvas3d()->show_labels(show); }

    bool is_view3D_overhang_shown() const { return (current_panel == view3D) && view3D->get_canvas3d()->is_overhang_shown(); }
    void show_view3D_overhang(bool show)
    {
        if (current_panel == view3D) view3D->get_canvas3d()->show_overhang(show);
    }

    void enable_sidebar(bool enabled);
    void collapse_sidebar(bool collapse);
    void update_sidebar(bool force_update = false);
    void reset_window_layout();
    Sidebar::DockingState get_sidebar_docking_state();

    bool is_view3D_layers_editing_enabled() const { return (current_panel == view3D) && view3D->get_canvas3d()->is_layers_editing_enabled(); }

    void set_current_canvas_as_dirty();
    GLCanvas3D* get_current_canvas3D(bool exclude_preview = false);
    void unbind_canvas_event_handlers();
    void reset_canvas_volumes();
    bool check_ams_status_impl(bool is_slice_all);  // Check whether the printer and ams status are consistent, for grouping algorithm
    bool get_machine_sync_status(); // check whether the printer is linked and the printer type is same as selected profile

    // BBS
    bool init_collapse_toolbar();

    // BBS
    void hide_select_machine_dlg()
    {
        if (m_select_machine_dlg)
            m_select_machine_dlg->EndModal(wxID_OK);
    }

    void enter_prepare_mode()
    {
        if (m_select_machine_dlg)
            m_select_machine_dlg->prepare_mode();
    }

    void hide_send_to_printer_dlg() { m_send_to_sdcard_dlg->EndModal(wxID_OK); }

    void update_preview_bottom_toolbar();

    void reset_gcode_toolpaths();

    void reset_all_gizmos();
    void apply_free_camera_correction(bool apply = true);
    void update_ui_from_settings();
    // BBS
    std::shared_ptr<BBLStatusBar> statusbar();
    std::string get_config(const std::string &key) const;
    BoundingBoxf bed_shape_bb() const;
    BoundingBox scaled_bed_shape_bb() const;

    // BBS: backup & restore
    std::vector<size_t> load_files(const std::vector<fs::path>& input_files, LoadStrategy strategy, bool ask_multi = false);
    std::vector<size_t> load_model_objects(const ModelObjectPtrs& model_objects, bool allow_negative_z = false, bool split_object = false);

    fs::path get_export_file_path(GUI::FileType file_type);
    wxString get_export_file(GUI::FileType file_type);

    // BBS
    void load_auxiliary_files();

    const Selection& get_selection() const;
    Selection& get_selection();
    Selection& get_curr_selection();

    int get_selected_object_idx() const;
    int get_selected_volume_idx() const;
    void selection_changed();
    void object_list_changed();

    // BBS
    void select_curr_plate_all();
    void remove_curr_plate_all();

    void select_all();
    void deselect_all();
    void exit_gizmo();
    void remove(size_t obj_idx);
    bool delete_object_from_model(size_t obj_idx, bool refresh_immediately = true); //BBS
    void delete_all_objects_from_model();
    void reset(bool apply_presets_change = false);
    void center_selection();
    void drop_selection();
    void mirror(Axis axis);
    void split_object();
    void split_volume();
    void scale_selection_to_fit_print_volume();

    // Return the active Undo/Redo stack. It may be either the main stack or the Gimzo stack.
    Slic3r::UndoRedo::Stack& undo_redo_stack() { assert(m_undo_redo_stack_active != nullptr); return *m_undo_redo_stack_active; }
    Slic3r::UndoRedo::Stack& undo_redo_stack_main() { return m_undo_redo_stack_main; }
    void enter_gizmos_stack();
    bool leave_gizmos_stack();

    void take_snapshot(const std::string& snapshot_name, UndoRedo::SnapshotType snapshot_type = UndoRedo::SnapshotType::Action);
    /*void take_snapshot(const wxString& snapshot_name, UndoRedo::SnapshotType snapshot_type = UndoRedo::SnapshotType::Action)
        { this->take_snapshot(std::string(snapshot_name.ToUTF8().data()), snapshot_type); }*/
    int  get_active_snapshot_index();

    void undo();
    void redo();
    void undo_redo_to(size_t time_to_load);

    // BBS: backup
    bool up_to_date(bool saved, bool backup);

    void suppress_snapshots()   { m_prevent_snapshots++; }
    void allow_snapshots()      { m_prevent_snapshots--; }
    // BBS: single snapshot
    void single_snapshots_enter(SingleSnapshot *single)
    {
        if (m_single == nullptr) m_single = single;
    }
    void single_snapshots_leave(SingleSnapshot *single)
    {
        if (m_single == single) m_single = nullptr;
    }

    void process_validation_warning(StringObjectException const &warning) const;

    bool background_processing_enabled() const {
#ifdef SUPPORT_BACKGROUND_PROCESSING
        return this->get_config("background_processing") == "1";
#else
        return false;
#endif
    }
    std::vector<std::vector<DynamicPrintConfig>> get_extruder_filament_info();
    void update_print_volume_state();
    void schedule_background_process();
    void schedule_auto_reslice_if_needed();
    void trigger_auto_reslice_now();
    int  auto_slice_delay_seconds() const;
    // Update background processing thread from the current config and Model.
    enum UpdateBackgroundProcessReturnState {
        // update_background_process() reports, that the Print / SLAPrint was updated in a way,
        // that the background process was invalidated and it needs to be re-run.
        UPDATE_BACKGROUND_PROCESS_RESTART = 1,
        // update_background_process() reports, that the Print / SLAPrint was updated in a way,
        // that a scene needs to be refreshed (you should call _3DScene::reload_scene(canvas3Dwidget, false))
        UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE = 2,
        // update_background_process() reports, that the Print / SLAPrint is invalid, and the error message
        // was sent to the status line.
        UPDATE_BACKGROUND_PROCESS_INVALID = 4,
        // Restart even if the background processing is disabled.
        UPDATE_BACKGROUND_PROCESS_FORCE_RESTART = 8,
        // Restart for G-code (or SLA zip) export or upload.
        UPDATE_BACKGROUND_PROCESS_FORCE_EXPORT = 16,
    };
    // returns bit mask of UpdateBackgroundProcessReturnState
    unsigned int update_background_process(bool force_validation = false, bool postpone_error_messages = false, bool switch_print = true);
    // Restart background processing thread based on a bitmask of UpdateBackgroundProcessReturnState.
    bool restart_background_process(unsigned int state);
    // returns bit mask of UpdateBackgroundProcessReturnState
    unsigned int update_restart_background_process(bool force_scene_update, bool force_preview_update);
    void show_delayed_error_message() {
        if (!this->delayed_error_message.empty()) {
            std::string msg = std::move(this->delayed_error_message);
            this->delayed_error_message.clear();
            GUI::show_error(this->q, msg);
        }
    }
    void export_gcode(fs::path output_path, bool output_path_on_removable_media);
    void export_gcode(fs::path output_path, bool output_path_on_removable_media, PrintHostJob upload_job);

    void reload_from_disk();
    bool replace_volume_with_stl(int object_idx, int volume_idx, const fs::path& new_path, const std::string& snapshot = "");
    void replace_with_stl();
    void replace_all_with_stl();
    void reload_all_from_disk();

    //BBS: add no_slice option
    void set_current_panel(wxPanel* panel, bool no_slice = true);

    void on_combobox_select(wxCommandEvent&);
    void on_select_bed_type(wxCommandEvent&);
    void on_select_preset(wxCommandEvent&);
    void on_slicing_update(SlicingStatusEvent&);
    void on_slicing_completed(wxCommandEvent&);
    void on_process_completed(SlicingProcessCompletedEvent&);
    void on_export_began(wxCommandEvent&);
    void on_export_finished(wxCommandEvent&);
    void on_slicing_began();

    void clear_warnings();
    void add_warning(const Slic3r::PrintStateBase::Warning &warning, size_t oid);
    // Update notification manager with the current state of warnings produced by the background process (slicing).
    void actualize_slicing_warnings(const PrintBase &print);
    void actualize_object_warnings(const PrintBase& print);
    // Displays dialog window with list of warnings.
    // Returns true if user clicks OK.
    // Returns true if current_warnings vector is empty without showning the dialog
    bool warnings_dialog();

    void on_action_add(SimpleEvent&);
    void on_action_add_plate(SimpleEvent&);
    void on_action_del_plate(SimpleEvent&);
    void on_action_split_objects(SimpleEvent&);
    void on_action_split_volumes(SimpleEvent&);
    void on_action_layersediting(SimpleEvent&);
    void on_create_filament(SimpleEvent &);
    void on_modify_filament(SimpleEvent &);
    void on_add_filament(SimpleEvent &);
    void on_delete_filament(SimpleEvent &);
    void on_add_custom_filament(ColorEvent &);

    void on_object_select(SimpleEvent&);
    void show_right_click_menu(Vec2d mouse_position, wxMenu *menu);
    void on_right_click(RBtnEvent&);
    //BBS: add model repair
    void on_repair_model(wxCommandEvent &event);
    void on_filament_color_changed(wxCommandEvent &event);
    void show_install_plugin_hint(wxCommandEvent &event);
    void install_network_plugin(wxCommandEvent &event);
    void show_preview_only_hint(wxCommandEvent &event);
    //BBS: add part plate related logic
    void on_plate_right_click(RBtnPlateEvent&);
    void on_plate_selected(SimpleEvent&);
    void on_action_request_model_id(wxCommandEvent& evt);
    void on_action_download_project(wxCommandEvent& evt);
    void on_slice_button_status(bool enable);
    //BBS: GUI refactor: GLToolbar
    void on_action_open_project(SimpleEvent&);
    void on_action_slice_plate(SimpleEvent&);
    void on_action_slice_all(SimpleEvent&);
    void on_action_publish(wxCommandEvent &evt);
    void on_action_print_plate(SimpleEvent&);
    void on_action_print_all(SimpleEvent&);
    void on_action_export_gcode(SimpleEvent&);
    void on_action_send_gcode(SimpleEvent&);
    void on_action_export_sliced_file(SimpleEvent&);
    void on_action_export_all_sliced_file(SimpleEvent&);
    void on_action_select_sliced_plate(wxCommandEvent& evt);
    //BBS: change dark/light mode
    void on_change_color_mode(SimpleEvent& evt);
    void on_apple_change_color_mode(wxSysColourChangedEvent& evt);
    void apply_color_mode();
    void on_update_geometry(Vec3dsEvent<2>&);
    void on_3dcanvas_mouse_dragging_started(SimpleEvent&);
    void on_3dcanvas_mouse_dragging_finished(SimpleEvent&);

    //void show_action_buttons(const bool is_ready_to_slice) const;
    bool show_publish_dlg(bool show = true);
    void update_publish_dialog_status(wxString &msg, int percent = -1);
    void on_action_print_plate_from_sdcard(SimpleEvent&);

    void on_tab_selection_changing(wxBookCtrlEvent&);

    // Set the bed shape to a single closed 2D polygon(array of two element arrays),
    // triangulate the bed and store the triangles into m_bed.m_triangles,
    // fills the m_bed.m_grid_lines and sets m_bed.m_origin.
    // Sets m_bed.m_polygon to limit the object placement.
    //BBS: add bed exclude area
    void set_bed_shape(const Pointfs       &shape,
                       const Pointfs       &exclude_areas,
                       const Pointfs       &wrapping_exclude_areas,
                       const double         printable_height,
                       std::vector<Pointfs> extruder_areas,
                       std::vector<double>  extruder_heights,
                       const std::string   &custom_texture,
                       const std::string   &custom_model,
                       bool                 force_as_custom = false);

    bool can_delete() const;
    bool can_delete_all() const;
    bool can_add_plate() const;
    bool can_delete_plate() const;
    bool can_increase_instances() const;
    bool can_decrease_instances() const;
    bool can_split_to_objects() const;
    bool can_split_to_volumes() const;
    bool can_arrange() const;
    bool can_layers_editing() const;
    bool can_fix_through_netfabb() const;
    bool can_simplify() const;
    bool can_set_instance_to_object() const;
    bool can_mirror() const;
    bool can_reload_from_disk() const;
    //BBS:
    bool can_fillcolor() const;
    bool has_assemble_view() const;
    bool can_replace_with_stl() const;
    bool can_replace_all_with_stl() const;
    bool can_split(bool to_objects) const;
#if ENABLE_ENHANCED_PRINT_VOLUME_FIT
    bool can_scale_to_print_volume() const;
#endif // ENABLE_ENHANCED_PRINT_VOLUME_FIT

    //BBS: add plate_id for thumbnail
    void generate_thumbnail(ThumbnailData& data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params,
                                      Camera::EType           camera_type,
                                      Camera::ViewAngleType   camera_view_angle_type = Camera::ViewAngleType::Iso,
                                      bool                    for_picking            = false,
                                      bool                    ban_light              = false);
    ThumbnailsList generate_thumbnails(const ThumbnailsParams& params, Camera::EType camera_type);
    //BBS
    void generate_calibration_thumbnail(ThumbnailData& data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params);
    PlateBBoxData generate_first_layer_bbox();

    void bring_instance_forward() const;

    // returns the path to project file with the given extension (none if extension == wxEmptyString)
    // extension should contain the leading dot, i.e.: ".3mf"
    wxString get_project_filename(const wxString& extension = wxEmptyString) const;
    wxString get_export_gcode_filename(const wxString& extension = wxEmptyString, bool only_filename = false, bool export_all = false) const;
    void set_project_filename(const wxString& filename);

    //BBS store bbs project name
    wxString get_project_name();
    void set_project_name(const wxString& project_name);
    void update_title_dirty_status();

    // Call after plater and Canvas#D is initialized
    void init_notification_manager();

    void update_objects_position_when_select_preset(const std::function<void()>& select_prest);

    // Caching last value of show_action_buttons parameter for show_action_buttons(), so that a callback which does not know this state will not override it.
    //mutable bool    			ready_to_slice = { false };
    // Flag indicating that the G-code export targets a removable device, therefore the show_action_buttons() needs to be called at any case when the background processing finishes.
    ExportingStatus             exporting_status { NOT_EXPORTING };
    std::string                 last_output_path;
    std::string                 last_output_dir_path;
    //BBS store machine_sn and 3mf_path for PrintJob
    PrintPrepareData            m_print_job_data;
    bool                        inside_snapshot_capture() { return m_prevent_snapshots != 0; }
    int                         process_completed_with_error { -1 }; //-1 means no error

    //BBS: project
    BBLProject                  project;

    //BBS: add print project related logic
    void update_fff_scene_only_shells(bool only_shells = true);
    //BBS: add popup object table logic
    bool PopupObjectTable(int object_id, int volume_id, const wxPoint& position);
    void on_action_send_to_printer(bool isall = false);
    void on_action_send_to_multi_machine(SimpleEvent&);
    int update_print_required_data(Slic3r::DynamicPrintConfig config, Slic3r::Model model, Slic3r::PlateDataPtrs plate_data_list, std::string file_name, std::string file_path);
private:
    bool layers_height_allowed() const;

    void update_fff_scene();
    void update_sla_scene();

    void undo_redo_to(std::vector<UndoRedo::Snapshot>::const_iterator it_snapshot);
    void update_after_undo_redo(const UndoRedo::Snapshot& snapshot, bool temp_snapshot_was_taken = false);
    void on_action_export_to_sdcard(SimpleEvent&);
    void on_action_export_to_sdcard_all(SimpleEvent&);
    void update_plugin_when_launch(wxCommandEvent& event);
    // path to project folder stored with no extension
    boost::filesystem::path     m_project_folder;

    /* display project name */
    wxString                    m_project_name;

    Slic3r::UndoRedo::Stack 	m_undo_redo_stack_main;
    Slic3r::UndoRedo::Stack 	m_undo_redo_stack_gizmos;
    Slic3r::UndoRedo::Stack    *m_undo_redo_stack_active = &m_undo_redo_stack_main;
    int                         m_prevent_snapshots = 0;     /* Used for avoid of excess "snapshoting".
                                                              * Like for "delete selected" or "set numbers of copies"
                                                              * we should call tack_snapshot just ones
                                                              * instead of calls for each action separately
                                                              * */
    // BBS: single snapshot
    Plater::SingleSnapshot     *m_single = nullptr;
    // BBS: backup
    size_t m_saved_timestamp = 0;
    size_t m_backup_timestamp = 0;
    std::string 				m_last_fff_printer_profile_name;
    std::string 				m_last_sla_printer_profile_name;

    // vector of all warnings generated by last slicing
    std::vector<std::pair<Slic3r::PrintStateBase::Warning, size_t>> current_warnings;
    bool show_warning_dialog { false };
};

const std::regex Plater::priv::pattern_bundle(".*[.](amf|amf[.]xml|zip[.]amf|3mf)", std::regex::icase);
const std::regex Plater::priv::pattern_3mf(".*3mf", std::regex::icase);
const std::regex Plater::priv::pattern_zip_amf(".*[.]zip[.]amf", std::regex::icase);
const std::regex Plater::priv::pattern_any_amf(".*[.](amf|amf[.]xml|zip[.]amf)", std::regex::icase);
const std::regex Plater::priv::pattern_prusa(".*bbl", std::regex::icase);

bool PlaterDropTarget::OnDropFiles(wxCoord x, wxCoord y, const wxArrayString &filenames)
{
#ifdef WIN32
    // hides the system icon
    this->MSWUpdateDragImageOnLeave();
#endif // WIN32

    m_mainframe.Raise();
    m_mainframe.select_tab(size_t(MainFrame::tp3DEditor));
    if (wxGetApp().is_editor())
        m_plater.select_view_3D("3D");

    // When only one .svg file is dropped on scene
    if (filenames.size() == 1) {
        const wxString &filename = filenames.Last();
        const wxString  file_extension = filename.substr(filename.length() - 4);
        if (file_extension.CmpNoCase(".svg") == 0) {
            // BBS: GUI refactor: move sidebar to the left
            const wxPoint offset  = m_plater.GetPosition() + m_plater.p->current_panel->GetPosition();
            Vec2d mouse_position(x - offset.x, y - offset.y);
            // Scale for retina displays
            const GLCanvas3D *canvas = m_plater.canvas3D();
            canvas->apply_retina_scale(mouse_position);
            return emboss_svg(m_plater, filename, mouse_position);
        }
    }
    bool res = m_plater.load_files(filenames);
    m_mainframe.update_title();
    return res;
}

Plater::priv::priv(Plater *q, MainFrame *main_frame)
    : q(q)
    , main_frame(main_frame)
    //BBS: add bed_exclude_area
    , config(Slic3r::DynamicPrintConfig::new_from_defaults_keys({
        "printable_area", "bed_exclude_area", "wrapping_exclude_area", "extruder_printable_area", "bed_custom_texture", "bed_custom_model", "print_sequence",
        "extruder_clearance_radius",
        "extruder_clearance_height_to_lid", "extruder_clearance_height_to_rod",
		"nozzle_height", "skirt_type", "skirt_loops", "skirt_speed","min_skirt_length", "skirt_distance", "skirt_start_angle",
        "brim_width", "brim_object_gap", "brim_use_efc_outline", "brim_type", "nozzle_diameter", "single_extruder_multi_material", "preferred_orientation",
        "enable_prime_tower", "wipe_tower_x", "wipe_tower_y", "prime_tower_width", "prime_tower_brim_width", "prime_tower_skip_points", "prime_tower_enable_framework",
        "prime_tower_infill_gap", "prime_volume",
        "extruder_colour", "filament_colour", "filament_type", "material_colour", "printable_height", "extruder_printable_height", "printer_model", "printer_technology",
        // These values are necessary to construct SlicingParameters by the Canvas3D variable layer height editor.
        "layer_height", "initial_layer_print_height", "min_layer_height", "max_layer_height",
        "brim_width", "wall_loops", "wall_filament", "sparse_infill_density", "sparse_infill_filament", "top_shell_layers",
        "enable_support", "support_filament", "support_interface_filament",
        "support_top_z_distance", "support_bottom_z_distance", "raft_layers",
        "wipe_tower_rotation_angle", "wipe_tower_cone_angle", "wipe_tower_extra_spacing", "wipe_tower_extra_flow", "wipe_tower_max_purge_speed",
        "wipe_tower_wall_type", "wipe_tower_extra_rib_length","wipe_tower_rib_width","wipe_tower_fillet_wall",
        "wipe_tower_filament",
        "best_object_pos",  "master_extruder_id"
        }))
    , sidebar(new Sidebar(q))
    , notification_manager(std::make_unique<NotificationManager>(q))
    , m_worker{q, std::make_unique<NotificationProgressIndicator>(notification_manager.get()), "ui_worker"}
    , m_sla_import_dlg{new SLAImportDialog{q}}
    , m_job_prepare_state(Job::JobPrepareState::PREPARE_STATE_DEFAULT)
    , delayed_scene_refresh(false)
    , collapse_toolbar(GLToolbar::Normal, "Collapse")
    //BBS :partplatelist construction
    , partplate_list(this->q, &model)
{
    m_is_dark = wxGetApp().app_config->get("dark_color_mode") == "1";

    m_aui_mgr.SetManagedWindow(q);
    m_aui_mgr.SetDockSizeConstraint(1, 1);
    //m_aui_mgr.GetArtProvider()->SetMetric(wxAUI_DOCKART_PANE_BORDER_SIZE, 0);
    //m_aui_mgr.GetArtProvider()->SetMetric(wxAUI_DOCKART_SASH_SIZE, 2);
    m_aui_mgr.GetArtProvider()->SetMetric(wxAUI_DOCKART_CAPTION_SIZE, 18);
    m_aui_mgr.GetArtProvider()->SetMetric(wxAUI_DOCKART_GRADIENT_TYPE, wxAUI_GRADIENT_NONE);

    this->q->SetFont(Slic3r::GUI::wxGetApp().normal_font());

    //BBS: use the first partplate's print for background process
    partplate_list.update_slice_context_to_current_plate(background_process);
    /*
    background_process.set_fff_print(&fff_print);
    background_process.set_sla_print(&sla_print);
    background_process.set_gcode_result(&gcode_result);
    background_process.set_thumbnail_cb([this](const ThumbnailsParams& params) { return this->generate_thumbnails(params, Camera::EType::Ortho); });
    background_process.set_slicing_completed_event(EVT_SLICING_COMPLETED);
    background_process.set_finished_event(EVT_PROCESS_COMPLETED);
    background_process.set_export_began_event(EVT_EXPORT_BEGAN);
    // Default printer technology for default config.
    background_process.select_technology(this->printer_technology);
    // Register progress callback from the Print class to the Plater.

    auto statuscb = [this](const Slic3r::PrintBase::SlicingStatus &status) {
        wxQueueEvent(this->q, new Slic3r::SlicingStatusEvent(EVT_SLICING_UPDATE, 0, status));
    };
    fff_print.set_status_callback(statuscb);
    sla_print.set_status_callback(statuscb); */

    // BBS: to be checked. Not follow patch.
    background_process.set_thumbnail_cb([this](const ThumbnailsParams& params) { return this->generate_thumbnails(params, Camera::EType::Ortho); });
    background_process.set_slicing_completed_event(EVT_SLICING_COMPLETED);
    background_process.set_finished_event(EVT_PROCESS_COMPLETED);
    background_process.set_export_began_event(EVT_EXPORT_BEGAN);
    background_process.set_export_finished_event(EVT_EXPORT_FINISHED);
    this->q->Bind(EVT_SLICING_UPDATE, &priv::on_slicing_update, this);
    this->q->Bind(EVT_PUBLISH, &priv::on_action_publish, this);
    this->q->Bind(EVT_REPAIR_MODEL, &priv::on_repair_model, this);
    this->q->Bind(EVT_FILAMENT_COLOR_CHANGED, &priv::on_filament_color_changed, this);
    this->q->Bind(EVT_INSTALL_PLUGIN_NETWORKING, &priv::install_network_plugin, this);
    this->q->Bind(EVT_INSTALL_PLUGIN_HINT, &priv::show_install_plugin_hint, this);
    this->q->Bind(EVT_UPDATE_PLUGINS_WHEN_LAUNCH, &priv::update_plugin_when_launch, this);
    this->q->Bind(EVT_PREVIEW_ONLY_MODE_HINT, &priv::show_preview_only_hint, this);
    this->q->Bind(EVT_GLCANVAS_COLOR_MODE_CHANGED, &priv::on_change_color_mode, this);
    this->q->Bind(wxEVT_SYS_COLOUR_CHANGED, &priv::on_apple_change_color_mode, this);
    this->q->Bind(EVT_CREATE_FILAMENT, &priv::on_create_filament, this);
    this->q->Bind(EVT_MODIFY_FILAMENT, &priv::on_modify_filament, this);
    this->q->Bind(EVT_NOTICE_CHILDE_SIZE_CHANGED, &Sidebar::on_size, sidebar);
    this->q->Bind(EVT_NOTICE_FULL_SCREEN_CHANGED, &Sidebar::on_full_screen, sidebar);
    this->q->Bind(EVT_ADD_CUSTOM_FILAMENT, &priv::on_add_custom_filament, this);
    main_frame->m_tabpanel->Bind(wxEVT_NOTEBOOK_PAGE_CHANGING, &priv::on_tab_selection_changing, this);

    auto* panel_3d = new wxPanel(q);
    view3D = new View3D(panel_3d, bed, &model, config, &background_process);
    //BBS: use partplater's gcode
    preview = new Preview(panel_3d, bed, &model, config, &background_process, partplate_list.get_current_slice_result(), [this]() { schedule_background_process(); });

    assemble_view = new AssembleView(panel_3d, bed, &model, config, &background_process);

#ifdef __APPLE__
    // BBS
    // set default view_toolbar icons size equal to GLGizmosManager::Default_Icons_Size
    //view_toolbar.set_icons_size(GLGizmosManager::Default_Icons_Size);
#endif // __APPLE__

    panels.push_back(view3D);
    panels.push_back(preview);
    panels.push_back(assemble_view);

    this->background_process_timer.SetOwner(this->q, 0);
    this->auto_reslice_timer.SetOwner(this->q, 0);
    this->q->Bind(wxEVT_TIMER, [this](wxTimerEvent &evt)
    {
        if (&evt.GetTimer() == &this->background_process_timer) {
            if (!this->suppressed_backround_processing_update)
                this->update_restart_background_process(false, false);
        } else if (&evt.GetTimer() == &this->auto_reslice_timer) {
            this->auto_reslice_timer.Stop();
            this->trigger_auto_reslice_now();
        } else {
            evt.Skip();
        }
    });

    update();

    // Orca: Make sidebar dockable
    m_aui_mgr.AddPane(sidebar, wxAuiPaneInfo()
                                   .Name("sidebar")
                                   .Left()
                                   .CloseButton(false)
                                   .TopDockable(false)
                                   .BottomDockable(false)
                                   .Floatable(true)
                                   .BestSize(wxSize(42 * wxGetApp().em_unit(), 90 * wxGetApp().em_unit())));

    auto* panel_sizer = new wxBoxSizer(wxHORIZONTAL);
    panel_sizer->Add(view3D, 1, wxEXPAND | wxALL, 0);
    panel_sizer->Add(preview, 1, wxEXPAND | wxALL, 0);
    panel_sizer->Add(assemble_view, 1, wxEXPAND | wxALL, 0);
    panel_3d->SetSizer(panel_sizer);
    m_aui_mgr.AddPane(panel_3d, wxAuiPaneInfo().Name("main").CenterPane().PaneBorder(false));

    m_default_window_layout = m_aui_mgr.SavePerspective();

    {
        auto& sidebar = m_aui_mgr.GetPane(this->sidebar);

        // Load previous window layout
        const auto cfg    = wxGetApp().app_config;
        wxString   layout = wxString::FromUTF8(cfg->get("window_layout"));
        if (!layout.empty()) {
            m_aui_mgr.LoadPerspective(layout, false);
            sidebar_layout.is_collapsed = !sidebar.IsShown();
        }

        // Keep tracking the current sidebar size, by storing it using `best_size`, which will be stored
        // in the config and re-applied when the app is opened again.
        this->sidebar->Bind(wxEVT_IDLE, [&sidebar, this](wxIdleEvent& e) {
            if (sidebar.IsShown() && sidebar.IsDocked() && sidebar.rect.GetWidth() > 0) {
                sidebar.BestSize(sidebar.rect.GetWidth(), sidebar.best_size.GetHeight());
            }
            e.Skip();
        });

        // Hide sidebar initially, will re-show it after initialization when we got proper window size
        sidebar.Hide();
        m_aui_mgr.Update();
    }

    menus.init(main_frame);


    // Events:

    if (wxGetApp().is_editor()) {
        // Preset change event
        sidebar->Bind(wxEVT_COMBOBOX, &priv::on_combobox_select, this);
        sidebar->Bind(EVT_OBJ_LIST_OBJECT_SELECT, [this](wxEvent&) { priv::selection_changed(); });
        // BBS: should bind BACKGROUND_PROCESS event to plater
        q->Bind(EVT_SCHEDULE_BACKGROUND_PROCESS, [this](SimpleEvent&) { this->schedule_background_process(); });
        // jump to found option from SearchDialog
        q->Bind(wxCUSTOMEVT_JUMP_TO_OPTION, [this](wxCommandEvent& evt) { sidebar->jump_to_option(evt.GetInt()); });
        q->Bind(wxCUSTOMEVT_JUMP_TO_OBJECT, [this](wxCommandEvent& evt) {
            auto client_data = evt.GetClientData();
            ObjectDataViewModelNode* data = static_cast<ObjectDataViewModelNode*>(client_data);
            sidebar->jump_to_object(data);
            }
        );
    }

    wxGLCanvas* view3D_canvas = view3D->get_wxglcanvas();
    //BBS: GUI refactor
    wxGLCanvas* preview_canvas = preview->get_wxglcanvas();

    if (wxGetApp().is_editor()) {
        // 3DScene events:
        view3D_canvas->Bind(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS, [this](SimpleEvent&) {
            delayed_error_message.clear();
            this->background_process_timer.Start(500, wxTIMER_ONE_SHOT);
            });
        view3D_canvas->Bind(EVT_GLCANVAS_OBJECT_SELECT, &priv::on_object_select, this);
        view3D_canvas->Bind(EVT_GLCANVAS_RIGHT_CLICK, &priv::on_right_click, this);
        //BBS: add part plate related logic
        view3D_canvas->Bind(EVT_GLCANVAS_PLATE_RIGHT_CLICK, &priv::on_plate_right_click, this);
        view3D_canvas->Bind(EVT_GLCANVAS_REMOVE_OBJECT, [q](SimpleEvent&) { q->remove_selected(); });
        view3D_canvas->Bind(EVT_GLCANVAS_ARRANGE, [this](SimpleEvent& evt) {
            //BBS arrange from EVT set default state.
            this->q->set_prepare_state(Job::PREPARE_STATE_DEFAULT);
            this->q->arrange(); });
        view3D_canvas->Bind(EVT_GLCANVAS_ARRANGE_PARTPLATE, [this](SimpleEvent& evt) {
            //BBS arrange from EVT set default state.
            this->q->set_prepare_state(Job::PREPARE_STATE_MENU);
            this->q->arrange(); });
        view3D_canvas->Bind(EVT_GLCANVAS_ORIENT, [this](SimpleEvent& evt) {
            //BBS orient from EVT set default state.
            this->q->set_prepare_state(Job::PREPARE_STATE_DEFAULT);
            this->q->orient(); });
        view3D_canvas->Bind(EVT_GLCANVAS_ORIENT_PARTPLATE, [this](SimpleEvent& evt) {
            //BBS orient from EVT set default state.
            this->q->set_prepare_state(Job::PREPARE_STATE_MENU);
            this->q->orient(); });
        //BBS
        view3D_canvas->Bind(EVT_GLCANVAS_SELECT_CURR_PLATE_ALL, [this](SimpleEvent&) {this->q->select_curr_plate_all(); });

        view3D_canvas->Bind(EVT_GLCANVAS_SELECT_ALL, [this](SimpleEvent&) { this->q->select_all(); });
        view3D_canvas->Bind(EVT_GLCANVAS_QUESTION_MARK, [](SimpleEvent&) { wxGetApp().keyboard_shortcuts(); });
        view3D_canvas->Bind(EVT_GLCANVAS_INCREASE_INSTANCES, [this](Event<int>& evt)
            { if (evt.data == 1) this->q->increase_instances(); else if (this->can_decrease_instances()) this->q->decrease_instances(); });
        view3D_canvas->Bind(EVT_GLCANVAS_INSTANCE_MOVED, [this](SimpleEvent&) { update(); });
        view3D_canvas->Bind(EVT_GLCANVAS_FORCE_UPDATE, [this](SimpleEvent&) { update(); });
        view3D_canvas->Bind(EVT_GLCANVAS_INSTANCE_ROTATED, [this](SimpleEvent&) { update(); });
        view3D_canvas->Bind(EVT_GLCANVAS_INSTANCE_SCALED, [this](SimpleEvent&) { update(); });
        // BBS
        //view3D_canvas->Bind(EVT_GLCANVAS_ENABLE_ACTION_BUTTONS, [this](Event<bool>& evt) { this->sidebar->enable_buttons(evt.data); });
        view3D_canvas->Bind(EVT_GLCANVAS_ENABLE_ACTION_BUTTONS, [this](Event<bool>& evt) { on_slice_button_status(evt.data); });
        view3D_canvas->Bind(EVT_GLCANVAS_UPDATE_GEOMETRY, &priv::on_update_geometry, this);
        view3D_canvas->Bind(EVT_GLCANVAS_MOUSE_DRAGGING_STARTED, &priv::on_3dcanvas_mouse_dragging_started, this);
        view3D_canvas->Bind(EVT_GLCANVAS_MOUSE_DRAGGING_FINISHED, &priv::on_3dcanvas_mouse_dragging_finished, this);
        view3D_canvas->Bind(EVT_GLCANVAS_TAB, [this](SimpleEvent&) { select_next_view_3D(); });
        view3D_canvas->Bind(EVT_GLCANVAS_RESETGIZMOS, [this](SimpleEvent&) { reset_all_gizmos(); });
        view3D_canvas->Bind(EVT_GLCANVAS_UNDO, [this](SimpleEvent&) { this->undo(); });
        view3D_canvas->Bind(EVT_GLCANVAS_REDO, [this](SimpleEvent&) { this->redo(); });
        view3D_canvas->Bind(EVT_GLCANVAS_COLLAPSE_SIDEBAR, [this](SimpleEvent&) { this->q->collapse_sidebar(!this->q->is_sidebar_collapsed());  });
        view3D_canvas->Bind(EVT_GLCANVAS_RESET_LAYER_HEIGHT_PROFILE, [this](SimpleEvent&) { this->view3D->get_canvas3d()->reset_layer_height_profile(); });
        view3D_canvas->Bind(EVT_GLCANVAS_ADAPTIVE_LAYER_HEIGHT_PROFILE, [this](Event<float>& evt) { this->view3D->get_canvas3d()->adaptive_layer_height_profile(evt.data); });
        view3D_canvas->Bind(EVT_GLCANVAS_SMOOTH_LAYER_HEIGHT_PROFILE, [this](HeightProfileSmoothEvent& evt) { this->view3D->get_canvas3d()->smooth_layer_height_profile(evt.data); });
        view3D_canvas->Bind(EVT_GLCANVAS_RELOAD_FROM_DISK, [this](SimpleEvent&) { this->reload_all_from_disk(); });

        // 3DScene/Toolbar:
        view3D_canvas->Bind(EVT_GLTOOLBAR_ADD, &priv::on_action_add, this);
        view3D_canvas->Bind(EVT_GLTOOLBAR_DELETE, [q](SimpleEvent&) { q->remove_selected(); });
        view3D_canvas->Bind(EVT_GLTOOLBAR_DELETE_ALL, [this](SimpleEvent&) { delete_all_objects_from_model(); });
//        view3D_canvas->Bind(EVT_GLTOOLBAR_DELETE_ALL, [q](SimpleEvent&) { q->reset_with_confirm(); });

        view3D_canvas->Bind(EVT_GLTOOLBAR_ADD_PLATE, &priv::on_action_add_plate, this);
        view3D_canvas->Bind(EVT_GLTOOLBAR_DEL_PLATE, &priv::on_action_del_plate, this);
        view3D_canvas->Bind(EVT_GLTOOLBAR_ORIENT, [this](SimpleEvent&) {
            //BBS arrange from EVT set default state.
            this->q->set_prepare_state(Job::PREPARE_STATE_DEFAULT);
            this->q->orient(); });
        view3D_canvas->Bind(EVT_GLTOOLBAR_ARRANGE, [this](SimpleEvent&) {
            //BBS arrange from EVT set default state.
            this->q->set_prepare_state(Job::PREPARE_STATE_DEFAULT);
            this->q->arrange();
            });
        view3D_canvas->Bind(EVT_GLTOOLBAR_CUT, [q](SimpleEvent&) { q->cut_selection_to_clipboard(); });
        view3D_canvas->Bind(EVT_GLTOOLBAR_COPY, [q](SimpleEvent&) { q->copy_selection_to_clipboard(); });
        view3D_canvas->Bind(EVT_GLTOOLBAR_PASTE, [q](SimpleEvent&) { q->paste_from_clipboard(); });
        view3D_canvas->Bind(EVT_GLTOOLBAR_LAYERSEDITING, &priv::on_action_layersediting, this);
        //BBS: add clone
        view3D_canvas->Bind(EVT_GLTOOLBAR_CLONE, [q](SimpleEvent&) { q->clone_selection(); });
        view3D_canvas->Bind(EVT_GLTOOLBAR_MORE, [q](SimpleEvent&) { q->increase_instances(); });
        view3D_canvas->Bind(EVT_GLTOOLBAR_FEWER, [q](SimpleEvent&) { q->decrease_instances(); });
        view3D_canvas->Bind(EVT_GLTOOLBAR_SPLIT_OBJECTS, &priv::on_action_split_objects, this);
        view3D_canvas->Bind(EVT_GLTOOLBAR_SPLIT_VOLUMES, &priv::on_action_split_volumes, this);
        //BBS: GUI refactor: GLToolbar
        view3D_canvas->Bind(EVT_GLTOOLBAR_OPEN_PROJECT, &priv::on_action_open_project, this);
        //view3D_canvas->Bind(EVT_GLTOOLBAR_SLICE_PLATE, &priv::on_action_slice_plate, this);
        //view3D_canvas->Bind(EVT_GLTOOLBAR_SLICE_ALL, &priv::on_action_slice_all, this);
        //view3D_canvas->Bind(EVT_GLTOOLBAR_PRINT_PLATE, &priv::on_action_print_plate, this);
        //view3D_canvas->Bind(EVT_GLTOOLBAR_PRINT_ALL, &priv::on_action_print_all, this);
        //view3D_canvas->Bind(EVT_GLTOOLBAR_EXPORT_GCODE, &priv::on_action_export_gcode, this);
        view3D_canvas->Bind(EVT_GLVIEWTOOLBAR_ASSEMBLE, [q](SimpleEvent&) { q->select_view_3D("Assemble"); });
        //preview also send these events
        //preview_canvas->Bind(EVT_GLTOOLBAR_SLICE_PLATE, &priv::on_action_slice_plate, this);
        //preview_canvas->Bind(EVT_GLTOOLBAR_PRINT_PLATE, &priv::on_action_print_plate, this);
        //preview_canvas->Bind(EVT_GLTOOLBAR_PRINT_ALL, &priv::on_action_print_all, this);
        //review_canvas->Bind(EVT_GLTOOLBAR_EXPORT_GCODE, &priv::on_action_export_gcode, this);
        view3D_canvas->Bind(EVT_GLCANVAS_SWITCH_TO_OBJECT, [main_frame](SimpleEvent&) {
                if (main_frame->m_param_panel) {
                    main_frame->m_param_panel->switch_to_object(false);
                }
            });
        view3D_canvas->Bind(EVT_GLCANVAS_SWITCH_TO_GLOBAL, [main_frame](SimpleEvent&) {
                if (main_frame->m_param_panel) {
                    main_frame->m_param_panel->switch_to_global();
                }
            });
    }
    view3D_canvas->Bind(EVT_GLCANVAS_UPDATE_BED_SHAPE, [q](SimpleEvent&) { q->set_bed_shape(); });

    // Preview events:
    preview->get_wxglcanvas()->Bind(EVT_GLCANVAS_QUESTION_MARK, [](SimpleEvent&) { wxGetApp().keyboard_shortcuts(); });
    preview->get_wxglcanvas()->Bind(EVT_GLCANVAS_UPDATE_BED_SHAPE, [q](SimpleEvent&) { q->set_bed_shape(); });
    preview->get_wxglcanvas()->Bind(EVT_GLCANVAS_UPDATE, [this](SimpleEvent &) {
            preview->get_canvas3d()->set_as_dirty();
        });
    if (wxGetApp().is_editor()) {
        preview->get_wxglcanvas()->Bind(EVT_GLCANVAS_TAB, [this](SimpleEvent&) { select_next_view_3D(); });
        preview->get_wxglcanvas()->Bind(EVT_GLCANVAS_COLLAPSE_SIDEBAR, [this](SimpleEvent&) { this->q->collapse_sidebar(!this->q->is_sidebar_collapsed());  });
        preview->get_wxglcanvas()->Bind(EVT_CUSTOMEVT_TICKSCHANGED, [this](wxCommandEvent& event) {
            Type tick_event_type = (Type)event.GetInt();
            Model& model = wxGetApp().plater()->model();
            //BBS: replace model custom gcode with current plate custom gcode
            model.plates_custom_gcodes[model.curr_plate_index] = preview->get_canvas3d()->get_gcode_viewer().get_layers_slider()->GetTicksValues();

            // BBS set to invalid state only
            if (tick_event_type == Type::ToolChange || tick_event_type == Type::Custom || tick_event_type == Type::Template || tick_event_type == Type::PausePrint) {
                PartPlate *plate = this->q->get_partplate_list().get_curr_plate();
                if (plate) {
                    plate->update_slice_result_valid_state(false);
                }
            }
            set_plater_dirty(true);

            preview->on_tick_changed(tick_event_type);

            // update slice and print button
            wxGetApp().mainframe->update_slice_print_status(MainFrame::SlicePrintEventType::eEventSliceUpdate, true, false);
            set_need_update(true);
        });
    }
    if (wxGetApp().is_gcode_viewer())
        preview->Bind(EVT_GLCANVAS_RELOAD_FROM_DISK, [this](SimpleEvent&) { this->q->reload_gcode_from_disk(); });

    //BBS
    wxGLCanvas* assemble_canvas = assemble_view->get_wxglcanvas();
    if (wxGetApp().is_editor()) {
        assemble_canvas->Bind(EVT_GLTOOLBAR_FILLCOLOR, [q](IntEvent& evt) { q->fill_color(evt.get_data()); });
        assemble_canvas->Bind(EVT_GLCANVAS_OBJECT_SELECT, &priv::on_object_select, this);
        assemble_canvas->Bind(EVT_GLVIEWTOOLBAR_3D, [q](SimpleEvent&) { q->select_view_3D("3D"); });
        assemble_canvas->Bind(EVT_GLCANVAS_RIGHT_CLICK, &priv::on_right_click, this);
        assemble_canvas->Bind(EVT_GLCANVAS_FORCE_UPDATE, [this](SimpleEvent&) { update(); });
        assemble_canvas->Bind(EVT_GLCANVAS_UNDO, [this](SimpleEvent&) { this->undo(); });
        assemble_canvas->Bind(EVT_GLCANVAS_REDO, [this](SimpleEvent&) { this->redo(); });
    }

    if (wxGetApp().is_editor()) {
        q->Bind(EVT_SLICING_COMPLETED, &priv::on_slicing_completed, this);
        q->Bind(EVT_PROCESS_COMPLETED, &priv::on_process_completed, this);
        q->Bind(EVT_EXPORT_BEGAN, &priv::on_export_began, this);
        q->Bind(EVT_EXPORT_FINISHED, &priv::on_export_finished, this);
        q->Bind(EVT_GLVIEWTOOLBAR_3D, [q](SimpleEvent&) { q->select_view_3D("3D"); });
        //BBS: set on_slice to false
        q->Bind(EVT_GLVIEWTOOLBAR_PREVIEW, [q](SimpleEvent&) { q->select_view_3D("Preview", false); });
        q->Bind(EVT_GLTOOLBAR_SLICE_PLATE, &priv::on_action_slice_plate, this);
        q->Bind(EVT_GLTOOLBAR_SLICE_ALL, &priv::on_action_slice_all, this);
        q->Bind(EVT_GLTOOLBAR_PRINT_PLATE, &priv::on_action_print_plate, this);
        q->Bind(EVT_PRINT_FROM_SDCARD_VIEW, &priv::on_action_print_plate_from_sdcard, this);
        q->Bind(EVT_GLTOOLBAR_SELECT_SLICED_PLATE, &priv::on_action_select_sliced_plate, this);
        q->Bind(EVT_GLTOOLBAR_PRINT_ALL, &priv::on_action_print_all, this);
        q->Bind(EVT_GLTOOLBAR_EXPORT_GCODE, &priv::on_action_export_gcode, this);
        q->Bind(EVT_GLTOOLBAR_SEND_GCODE, &priv::on_action_send_gcode, this);
        q->Bind(EVT_GLTOOLBAR_EXPORT_SLICED_FILE, &priv::on_action_export_sliced_file, this);
        q->Bind(EVT_GLTOOLBAR_EXPORT_ALL_SLICED_FILE, &priv::on_action_export_all_sliced_file, this);
        q->Bind(EVT_GLTOOLBAR_SEND_TO_PRINTER, &priv::on_action_export_to_sdcard, this);
        q->Bind(EVT_GLTOOLBAR_SEND_TO_PRINTER_ALL, &priv::on_action_export_to_sdcard_all, this);
        q->Bind(EVT_GLTOOLBAR_PRINT_MULTI_MACHINE, &priv::on_action_send_to_multi_machine, this);
        q->Bind(EVT_GLCANVAS_PLATE_SELECT, &priv::on_plate_selected, this);
        q->Bind(EVT_DOWNLOAD_PROJECT, &priv::on_action_download_project, this);
        q->Bind(EVT_IMPORT_MODEL_ID, &priv::on_action_request_model_id, this);
        q->Bind(EVT_PRINT_FINISHED, [q](wxCommandEvent& evt) { q->print_job_finished(evt); });
        q->Bind(EVT_SEND_CALIBRATION_FINISHED, [q](wxCommandEvent& evt) { q->send_calibration_job_finished(evt); });
        q->Bind(EVT_SEND_FINISHED, [q](wxCommandEvent& evt) { q->send_job_finished(evt); });
        q->Bind(EVT_PUBLISH_FINISHED, [q](wxCommandEvent& evt) { q->publish_job_finished(evt);});
        q->Bind(EVT_OPEN_PLATESETTINGSDIALOG, [q](wxCommandEvent& evt) { q->open_platesettings_dialog(evt);});
        q->Bind(EVT_OPEN_FILAMENT_MAP_SETTINGS_DIALOG, [q](wxCommandEvent &evt) { q->open_filament_map_setting_dialog(evt); });
        //q->Bind(EVT_GLVIEWTOOLBAR_ASSEMBLE, [q](SimpleEvent&) { q->select_view_3D("Assemble"); });
    }

    // Drop target:
    q->SetDropTarget(new PlaterDropTarget(*main_frame, *q));   // if my understanding is right, wxWindow takes the owenership
    q->Layout();

    apply_color_mode();

    set_current_panel(wxGetApp().is_editor() ? static_cast<wxPanel*>(view3D) : static_cast<wxPanel*>(preview));

    // updates camera type from .ini file
    camera.enable_update_config_on_type_change(true);
    // BBS set config
    bool use_perspective_camera = get_config("use_perspective_camera").compare("true") == 0;
    if (use_perspective_camera) {
        camera.set_type(Camera::EType::Perspective);
    } else {
        camera.set_type(Camera::EType::Ortho);
    }

    // Load the 3DConnexion device database.
    mouse3d_controller.load_config(*wxGetApp().app_config);
    // Start the background thread to detect and connect to a HID device (Windows and Linux).
    // Connect to a 3DConnextion driver (OSX).
    mouse3d_controller.init();
#ifdef _WIN32
    // Register an USB HID (Human Interface Device) attach event. evt contains Win32 path to the USB device containing VID, PID and other info.
    // This event wakes up the Mouse3DController's background thread to enumerate HID devices, if the VID of the callback event
    // is one of the 3D Mouse vendors (3DConnexion or Logitech).
    this->q->Bind(EVT_HID_DEVICE_ATTACHED, [this](HIDDeviceAttachedEvent &evt) {
        mouse3d_controller.device_attached(evt.data);
        });
    this->q->Bind(EVT_HID_DEVICE_DETACHED, [this](HIDDeviceAttachedEvent& evt) {
        mouse3d_controller.device_detached(evt.data);
        });
#endif /* _WIN32 */
    //notification_manager = new NotificationManager(this->q);

    if (wxGetApp().is_editor()) {
        this->q->Bind(EVT_EJECT_DRIVE_NOTIFICAION_CLICKED, [this](EjectDriveNotificationClickedEvent&) { this->q->eject_drive(); });
        this->q->Bind(EVT_EXPORT_GCODE_NOTIFICAION_CLICKED, [this](ExportGcodeNotificationClickedEvent&) { this->q->export_gcode(true); });
        this->q->Bind(EVT_PRESET_UPDATE_AVAILABLE_CLICKED, [](PresetUpdateAvailableClickedEvent&) {  wxGetApp().get_preset_updater()->on_update_notification_confirm(); });
        this->q->Bind(EVT_PRINTER_CONFIG_UPDATE_AVAILABLE_CLICKED, [](PrinterConfigUpdateAvailableClickedEvent&) {
            wxGetApp().get_preset_updater()->do_printer_config_update();
            wxGetApp().getDeviceManager()->reload_printer_settings(); });

        /* BBS do not handle removeable driver event */
        this->q->Bind(EVT_REMOVABLE_DRIVE_EJECTED, [this](RemovableDriveEjectEvent &evt) {
            if (evt.data.second) {
                // BBS
                //this->show_action_buttons(this->ready_to_slice);
                notification_manager->close_notification_of_type(NotificationType::ExportFinished);
                notification_manager->push_notification(NotificationType::CustomNotification,
                                                        NotificationManager::NotificationLevel::RegularNotificationLevel,
                                                        format(_L("Successfully unmounted. The device %s (%s) can now be safely removed from the computer."), evt.data.first.name, evt.data.first.path)
                    );
            } else {
                notification_manager->push_notification(NotificationType::CustomNotification,
                                                        NotificationManager::NotificationLevel::ErrorNotificationLevel,
                                                        format(_L("Ejecting of device %s (%s) has failed."), evt.data.first.name, evt.data.first.path)
                    );
            }
        });
        this->q->Bind(EVT_REMOVABLE_DRIVES_CHANGED, [this](RemovableDrivesChangedEvent &) {
            // BBS
            //this->show_action_buttons(this->ready_to_slice);
            // Close notification ExportingFinished but only if last export was to removable
            notification_manager->device_ejected();
        });
        // Start the background thread and register this window as a target for update events.
        wxGetApp().removable_drive_manager()->init(this->q);
#ifdef _WIN32
        //Trigger enumeration of removable media on Win32 notification.
        this->q->Bind(EVT_VOLUME_ATTACHED, [this](VolumeAttachedEvent &evt) { wxGetApp().removable_drive_manager()->volumes_changed(); });
        this->q->Bind(EVT_VOLUME_DETACHED, [this](VolumeDetachedEvent &evt) { wxGetApp().removable_drive_manager()->volumes_changed(); });
#endif /* _WIN32 */
    }

    // Initialize the Undo / Redo stack with a first snapshot.
    //this->take_snapshot("New Project", UndoRedo::SnapshotType::ProjectSeparator);
    // Reset the "dirty project" flag.
    m_undo_redo_stack_main.mark_current_as_saved();
    dirty_state.update_from_undo_redo_stack(false);
    //this->take_snapshot("New Project");
    // BBS: save project confirm
    up_to_date(true, false);
    up_to_date(true, true);
    model.set_need_backup();

    // BBS: restore project
    if (wxGetApp().is_editor()) {
        auto last_backup = wxGetApp().app_config->get_last_backup_dir();
        this->q->Bind(EVT_RESTORE_PROJECT, [this, last = last_backup](wxCommandEvent& e) {
            std::string last_backup = last;
            std::string originfile;
            if (Slic3r::has_restore_data(last_backup, originfile)) {
                auto result = MessageDialog(this->q, _L("Previous unsaved project detected, do you want to restore it?"), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Restore"), wxYES_NO | wxYES_DEFAULT | wxCENTRE).ShowModal();
                if (result == wxID_YES) {
                    this->q->load_project(from_path(last_backup), from_path(originfile));
                    Slic3r::backup_soon();
                    return;
                }
            }

            try {
                if (originfile != "<lock>") // see bbs_3mf.cpp for lock detail
                    boost::filesystem::remove_all(last);
            }

            catch (...) {}
            
            if (this->q->get_project_filename().IsEmpty() && this->q->is_empty_project()) {
                int skip_confirm = e.GetInt();
                this->q->new_project(skip_confirm, true);
            }
        });
        //wxPostEvent(this->q, wxCommandEvent{EVT_RESTORE_PROJECT});
    }

    this->q->Bind(EVT_LOAD_MODEL_OTHER_INSTANCE, [this](LoadFromOtherInstanceEvent& evt) {
        BOOST_LOG_TRIVIAL(trace) << "Received load from other instance event.";
        wxArrayString input_files;
        for (size_t i = 0; i < evt.data.size(); ++i) {
            input_files.push_back(from_u8(evt.data[i].string()));
        }
        wxGetApp().mainframe->Raise();
        this->q->load_files(input_files);
    });
    
    this->q->Bind(EVT_START_DOWNLOAD_OTHER_INSTANCE, [](StartDownloadOtherInstanceEvent& evt) {
        BOOST_LOG_TRIVIAL(trace) << "Received url from other instance event.";
        wxGetApp().mainframe->Raise();
        for (size_t i = 0; i < evt.data.size(); ++i) {
            wxGetApp().start_download(evt.data[i]);
        }
       
    });
    this->q->Bind(EVT_INSTANCE_GO_TO_FRONT, [this](InstanceGoToFrontEvent &) {
        bring_instance_forward();
    });
    wxGetApp().other_instance_message_handler()->init(this->q);

    // collapse sidebar according to saved value
    //if (wxGetApp().is_editor()) {
    //    bool is_collapsed = wxGetApp().app_config->get("collapsed_sidebar") == "1";
    //    sidebar->collapse(is_collapsed);
    //}
    update_sidebar(true);
}

Plater::priv::~priv()
{
    if (config != nullptr)
        delete config;
    // Saves the database of visited (already shown) hints into hints.ini.
    notification_manager->deactivate_loaded_hints();
    main_frame->m_tabpanel->Unbind(wxEVT_NOTEBOOK_PAGE_CHANGING, &priv::on_tab_selection_changing, this);
}

void Plater::priv::update(unsigned int flags)
{
    // the following line, when enabled, causes flickering on NVIDIA graphics cards
//    wxWindowUpdateLocker freeze_guard(q);
#ifdef SUPPORT_AUTOCENTER
    if (get_config("autocenter") == "true")
        model.center_instances_around_point(this->bed.build_volume().bed_center());
#endif

    unsigned int update_status = 0;
    const bool force_background_processing_restart = this->printer_technology == ptSLA || (flags & (unsigned int)UpdateParams::FORCE_BACKGROUND_PROCESSING_UPDATE);
    if (force_background_processing_restart)
        // Update the SLAPrint from the current Model, so that the reload_scene()
        // pulls the correct data.
        update_status = this->update_background_process(false, flags & (unsigned int)UpdateParams::POSTPONE_VALIDATION_ERROR_MESSAGE);
    //BBS TODO reload_scene
    this->view3D->reload_scene(false, flags & (unsigned int)UpdateParams::FORCE_FULL_SCREEN_REFRESH);
    this->preview->reload_print();
    //BBS assemble view
    this->assemble_view->reload_scene(false, flags);

    if (current_panel && q->is_preview_shown()) {
        q->force_update_all_plate_thumbnails();
        //update_fff_scene_only_shells(true);
    }

    if (force_background_processing_restart)
        this->restart_background_process(update_status);
    else
        this->schedule_background_process();

    // BBS
#if 0
    if (get_config("autocenter") == "true" && this->sidebar->obj_manipul()->IsShown())
        this->sidebar->obj_manipul()->UpdateAndShow(true);
#endif

    update_sidebar();
}

void Plater::priv::select_view(const std::string& direction)
{
    if (current_panel == view3D) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << __LINE__ << "select view3D";
        view3D->select_view(direction);
        wxGetApp().update_ui_from_settings();
    }
    else if (current_panel == preview) {
        BOOST_LOG_TRIVIAL(info) << "select preview";
        preview->select_view(direction);
        wxGetApp().update_ui_from_settings();
    }
    else if (current_panel == assemble_view) {
        BOOST_LOG_TRIVIAL(info) << "select assemble view";
        assemble_view->select_view(direction);
    }
}

const VendorProfile::PrinterModel *Plater::get_curr_printer_model()
{
    auto bundle = wxGetApp().preset_bundle;
    if (bundle) {
        const Preset *curr = &bundle->printers.get_selected_preset();
        if (curr) {
            const VendorProfile::PrinterModel *pm = PresetUtils::system_printer_model(*curr);
            if (!pm) {
                auto curr_parent = bundle->printers.get_selected_preset_parent();
                if (curr_parent) {
                    pm = PresetUtils::system_printer_model(*curr_parent);
                }
            }
            return pm;
        }
    }
    return nullptr;
}

std::map<std::string, std::string> Plater::get_bed_texture_maps()
{
    auto pm = get_curr_printer_model();
    if (pm) {
        std::map<std::string, std::string> maps;
        if (pm->use_double_extruder_default_texture.size() > 0) {
            maps["use_double_extruder_default_texture"] = pm->use_double_extruder_default_texture;
        }
        if (pm->bottom_texture_end_name.size() > 0) {
            maps["bottom_texture_end_name"] = pm->bottom_texture_end_name;
        }
        if (pm->bottom_texture_rect.size() > 0) {
            maps["bottom_texture_rect"] = pm->bottom_texture_rect;
        }
        if (pm->middle_texture_rect.size() > 0) {
            maps["middle_texture_rect"] = pm->middle_texture_rect;
        }
        return maps;
    }
    return {};
}

bool Plater::get_enable_wrapping_detection()
{
   const DynamicPrintConfig & print_config = wxGetApp().preset_bundle->prints.get_edited_preset().config;
   const ConfigOptionBool *  wrapping_detection = print_config.option<ConfigOptionBool>("enable_wrapping_detection");
   return  (wrapping_detection != nullptr) && wrapping_detection->value;
}

wxColour Plater::get_next_color_for_filament()
{
    static int curr_color_filamenet = 0;
    // refs to https://www.ebaomonthly.com/window/photo/lesson/colorList.htm
    wxColour colors[FILAMENT_SYSTEM_COLORS_NUM] = {
        // ORCA updated all color palette
        wxColour("#00C1AE"),
        wxColour("#F4E2C1"),
        wxColour("#ED1C24"),
        wxColour("#00FF7F"),
        wxColour("#F26722"),
        wxColour("#FFEB31"),
        wxColour("#7841CE"),
        wxColour("#115877"),
        wxColour("#ED1E79"),
        wxColour("#2EBDEF"),
        wxColour("#345B2F"),
        wxColour("#800080"),
        wxColour("#FA8173"),
        wxColour("#800000"),
        wxColour("#F7B763"),
        wxColour("#A4C41E"),
    };
    return colors[curr_color_filamenet++ % FILAMENT_SYSTEM_COLORS_NUM];
}

wxString Plater::get_slice_warning_string(GCodeProcessorResult::SliceWarning& warning)
{
    if (warning.msg == BED_TEMP_TOO_HIGH_THAN_FILAMENT) {
        return _L("The current hot bed temperature is relatively high. The nozzle may be clogged when printing this filament in a closed enclosure. Please open the front door and/or remove the upper glass.");
    } else if (warning.msg == NOZZLE_HRC_CHECKER) {
        return _L("The nozzle hardness required by the filament is higher than the default nozzle hardness of the printer. Please replace the hardened nozzle or filament, otherwise, the nozzle will be attrited or damaged.");
    } else if (warning.msg == NOT_SUPPORT_TRADITIONAL_TIMELAPSE) {
        return _L("Enabling traditional timelapse photography may cause surface imperfections. It is recommended to change to smooth mode.");
    } else if (warning.msg == NOT_GENERATE_TIMELAPSE) {
        return wxString();
    } else if (warning.msg == SMOOTH_TIMELAPSE_WITHOUT_PRIME_TOWER) {
        return _L("Smooth mode for timelapse is enabled, but the prime tower is off, which may cause print defects. Please enable the prime tower, re-slice and print again.");
    }
    else {
        return wxString(warning.msg);
    }
}

void Plater::priv::apply_free_camera_correction(bool apply/* = true*/)
{
    bool use_perspective_camera = get_config("use_perspective_camera").compare("true") == 0;
    if (use_perspective_camera)
        camera.set_type(Camera::EType::Perspective);
    else
        camera.set_type(Camera::EType::Ortho);
    if (apply && wxGetApp().app_config->get_bool("use_free_camera"))
        camera.recover_from_free_camera();
}

//BBS: add no slice option
void Plater::priv::select_view_3D(const std::string& name, bool no_slice)
{
    if (name == "3D") {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << __LINE__ << "select view3D";
        if (q->only_gcode_mode() || q->using_exported_file()) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format("goto preview page when loading gcode/exported_3mf");
        }
        set_current_panel(view3D, no_slice);
    }
    else if (name == "Preview") {
        BOOST_LOG_TRIVIAL(info) << "select preview";
        //BBS update extruder params and speed table before slicing
        const Slic3r::DynamicPrintConfig& config = wxGetApp().preset_bundle->full_config();
        auto& print = q->get_partplate_list().get_current_fff_print();
        auto print_config = print.config();
        int numExtruders = wxGetApp().preset_bundle->filament_presets.size();

        Model::setExtruderParams(config, numExtruders);
        Model::setPrintSpeedTable(config, print_config);
        set_current_panel(preview, no_slice);
    }
    else if (name == "Assemble") {
        BOOST_LOG_TRIVIAL(info) << "select assemble view";
        set_current_panel(assemble_view, no_slice);
    }

    //BBS update selection
    wxGetApp().obj_list()->update_selections();
    selection_changed();

    apply_free_camera_correction(false);
}

void Plater::priv::select_next_view_3D()
{
    
    if (current_panel == view3D)
        wxGetApp().mainframe->select_tab(size_t(MainFrame::tpPreview));
    else if (current_panel == preview)
        wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));
//    else if (current_panel == assemble_view)
//        set_current_panel(view3D);
}

void Plater::priv::enable_sidebar(bool enabled)
{
    if (q->m_only_gcode)
        enabled = false;

    sidebar_layout.is_enabled = enabled;
    update_sidebar();
}

void Plater::priv::collapse_sidebar(bool collapse)
{
    if (q->m_only_gcode)
        return;

    sidebar_layout.is_collapsed = collapse;

    // Now update the tooltip in the toolbar.
    std::string new_tooltip = collapse
                              ? _u8L("Expand sidebar")
                              : _u8L("Collapse sidebar");
    new_tooltip += " [" + _u8L("Shift+") + _u8L("Tab") + "]";
    int id = collapse_toolbar.get_item_id("collapse_sidebar");
    collapse_toolbar.set_tooltip(id, new_tooltip);

    update_sidebar();
}

void Plater::priv::update_sidebar(bool force_update) {
    auto& sidebar = m_aui_mgr.GetPane(this->sidebar);
    if (!sidebar.IsOk() || this->current_panel == nullptr) {
        return;
    }
    bool  needs_update = force_update;

    if (!sidebar_layout.is_enabled) {
        if (sidebar.IsShown()) {
            sidebar.Hide();
            needs_update = true;
        }
    } else {
        // Only hide if collapsed or is floating and is not 3d view
        const bool should_hide = sidebar_layout.is_collapsed || (sidebar.IsFloating() && !sidebar_layout.show);
        const bool should_show = !should_hide;
        if (should_show != sidebar.IsShown()) {
            sidebar.Show(should_show);
            needs_update = true;
        }
    }

    if (needs_update) {
        notification_manager->set_sidebar_collapsed(sidebar.IsShown());
        m_aui_mgr.Update();
    }
}

void Plater::priv::reset_window_layout()
{
    m_aui_mgr.LoadPerspective(m_default_window_layout, false);
    sidebar_layout.is_collapsed = false;
    update_sidebar(true);
}

Sidebar::DockingState Plater::priv::get_sidebar_docking_state() {
    if (!sidebar_layout.is_enabled) {
        return Sidebar::None;
    }

    const auto& sidebar = m_aui_mgr.GetPane(this->sidebar);
    if(sidebar.IsFloating()) {
        return Sidebar::None;
    }

    return sidebar.dock_direction == wxAUI_DOCK_RIGHT ? Sidebar::Right : Sidebar::Left;
}

void Plater::priv::reset_all_gizmos()
{
    view3D->get_canvas3d()->reset_all_gizmos();
}

// Called after the Preferences dialog is closed and the program settings are saved.
// Update the UI based on the current preferences.
void Plater::priv::update_ui_from_settings()
{
    apply_free_camera_correction();

    view3D->get_canvas3d()->update_ui_from_settings();
    preview->get_canvas3d()->update_ui_from_settings();

    sidebar->update_ui_from_settings();
}

// BBS
std::shared_ptr<BBLStatusBar> Plater::priv::statusbar()
{
    return nullptr;
}

std::string Plater::priv::get_config(const std::string &key) const
{
    return wxGetApp().app_config->get(key);
}

BoundingBoxf Plater::priv::bed_shape_bb() const
{
    BoundingBox bb = scaled_bed_shape_bb();
    return BoundingBoxf(unscale(bb.min), unscale(bb.max));
}

BoundingBox Plater::priv::scaled_bed_shape_bb() const
{
    const auto *bed_shape_opt = config->opt<ConfigOptionPoints>("printable_area");
    const auto printable_area = Slic3r::Polygon::new_scale(bed_shape_opt->values);
    return printable_area.bounding_box();
}


void read_binary_stl(const std::string& filename, std::string& model_id, std::string& code) {
    std::ifstream file( encode_path(filename.c_str()), std::ios::binary);
    if (!file) {
        return;
    }

    try {
        // Read the first 80 bytes
        char data[80];
        file.read(data, 80);
        if (!file) {
            file.close();
            return;
        }

        if (data[0] == '\0' || data[0] == ' ') {
            file.close();
            return;
        }

        char magic[2] = { data[0], data[1] };
        if (magic[0] != 'M' || magic[1] != 'W') {
            file.close();
            return;
        }

        if (data[2] != ' ') {
            file.close();
            return;
        }

        char protocol_version[3] = { data[3], data[4], data[5] };

        //version
        if (protocol_version[0] != '1' || protocol_version[1] != '.' || protocol_version[2] != '0') {
            file.close();
            return;
        }

        std::vector<char*> tokens;
        std::istringstream iss(data);
        std::string token;
        while (std::getline(iss, token, ' ')) {
            char* tokenPtr = new char[token.length() + 1];
            std::strcpy(tokenPtr, token.c_str());
            tokens.push_back(tokenPtr);
        }

        //model id
        if (tokens.size() < 4) {
            file.close();
            return;
        }

        model_id = tokens[2];
        code = tokens[3];
        file.close();
    }
    catch (...) {
    }
    return;
}

// BBS: backup & restore
std::vector<size_t> Plater::priv::load_files(const std::vector<fs::path>& input_files, LoadStrategy strategy, bool ask_multi)
{
    std::vector<size_t> empty_result;
    bool dlg_cont = true;
    bool is_user_cancel = false;
    bool translate_old = false;
    int current_width = 0, current_depth = 0, current_height = 0, project_filament_count = 1;

    if (input_files.empty())
        return std::vector<size_t>();

    if (!input_files.empty())
       q->m_3mf_path = input_files[0].string();
    
    // SoftFever: ugly fix so we can exist pa calib mode
    background_process.fff_print()->calib_mode() = CalibMode::Calib_None;


    // BBS
    int filaments_cnt = config->opt<ConfigOptionStrings>("filament_colour")->values.size();
    bool one_by_one = input_files.size() == 1 || printer_technology == ptSLA/* || filaments_cnt <= 1*/;
    if (! one_by_one) {
        for (const auto &path : input_files) {
            if (std::regex_match(path.string(), pattern_bundle)) {
                one_by_one = true;
                break;
            }
        }
    }

    bool load_model = strategy & LoadStrategy::LoadModel;
    bool load_config = strategy & LoadStrategy::LoadConfig;
    bool imperial_units = strategy & LoadStrategy::ImperialUnits;
    bool silence = strategy & LoadStrategy::Silence;

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": load_model %1%, load_config %2%, input_files size %3%")%load_model %load_config %input_files.size();

    const auto loading = _L("Loading") + dots;
    ProgressDialog dlg(loading, "", 100, find_toplevel_parent(q), wxPD_AUTO_HIDE | wxPD_CAN_ABORT | wxPD_APP_MODAL);
    wxBusyCursor busy;

    auto *new_model = (!load_model || one_by_one) ? nullptr : new Slic3r::Model();
    std::vector<size_t> obj_idxs;

    std::string  designer_model_id;
    std::string  designer_country_code;

    int answer_convert_from_meters          = wxOK_DEFAULT;
    int answer_convert_from_imperial_units  = wxOK_DEFAULT;
    int tolal_model_count                   = 0;

    int progress_percent = 0;
    int total_files = input_files.size();
    const int stage_percent[IMPORT_STAGE_MAX+1] = {
            5,      // IMPORT_STAGE_RESTORE
            10,     // IMPORT_STAGE_OPEN
            30,     // IMPORT_STAGE_READ_FILES
            50,     // IMPORT_STAGE_EXTRACT
            60,     // IMPORT_STAGE_LOADING_OBJECTS
            70,     // IMPORT_STAGE_LOADING_PLATES
            80,     // IMPORT_STAGE_FINISH
            85,     // IMPORT_STAGE_ADD_INSTANCE
            90,      // IMPORT_STAGE_UPDATE_GCODE
            92,     // IMPORT_STAGE_CHECK_MODE_GCODE
            95,     // UPDATE_GCODE_RESULT
            98,     // IMPORT_LOAD_CONFIG
            99,     // IMPORT_LOAD_MODEL_OBJECTS
            100
     };
    const int step_percent[LOAD_STEP_STAGE_NUM+1] = {
            5,     // LOAD_STEP_STAGE_READ_FILE
            30,     // LOAD_STEP_STAGE_GET_SOLID
            60,     // LOAD_STEP_STAGE_GET_MESH
            100
     };

    const float INPUT_FILES_RATIO            = 0.7;
    const float INIT_MODEL_RATIO             = 0.75;
    const float CENTER_AROUND_ORIGIN_RATIO   = 0.8;
    const float LOAD_MODEL_RATIO             = 0.9;

    for (size_t i = 0; i < input_files.size(); ++i) {
        int file_percent = 0;

#ifdef _WIN32
        auto path = input_files[i];
        // On Windows, we swap slashes to back slashes, see GH #6803 as read_from_file() does not understand slashes on Windows thus it assignes full path to names of loaded objects.
        path.make_preferred();
#else  // _WIN32
       // Don't make a copy on Posix. Slash is a path separator, back slashes are not accepted as a substitute.
        const auto &path = input_files[i];
#endif // _WIN32
        const auto filename         = path.filename();
        int  progress_percent = static_cast<int>(100.0f * static_cast<float>(i) / static_cast<float>(input_files.size()));
        const auto real_filename    = (strategy & LoadStrategy::Restore) ? input_files[++i].filename() : filename;
        const auto dlg_info         = _L("Loading file") + ": " + from_path(real_filename);
        BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << boost::format(": load file %1%") % filename;
        dlg_cont = dlg.Update(progress_percent, dlg_info);
        if (!dlg_cont) return empty_result;

        const bool type_3mf = std::regex_match(path.string(), pattern_3mf);
        // const bool type_zip_amf = !type_3mf && std::regex_match(path.string(), pattern_zip_amf);
        const bool type_any_amf = !type_3mf && std::regex_match(path.string(), pattern_any_amf);
        // const bool type_prusa   = std::regex_match(path.string(), pattern_prusa);

        Slic3r::Model model;
        // BBS: add auxiliary files related logic
        bool load_aux = strategy & LoadStrategy::LoadAuxiliary, load_old_project = false;
        if (load_model && load_config && type_3mf) {
            load_aux = true;
            strategy = strategy | LoadStrategy::LoadAuxiliary;
        }
        if (load_config) strategy = strategy | LoadStrategy::CheckVersion;
        bool is_project_file = false;
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": is_project_file %1%, type_3mf %2%") % is_project_file % type_3mf;
        try {
            if (type_3mf) {
                DynamicPrintConfig config;
                Semver             file_version;
                En3mfType          en_3mf_file_type = En3mfType::From_BBS;
                {
                    DynamicPrintConfig config_loaded;

                    // BBS: add part plate related logic
                    PlateDataPtrs             plate_data;
                    ConfigSubstitutionContext config_substitutions{ForwardCompatibilitySubstitutionRule::Enable};
                    std::vector<Preset *>     project_presets;
                    // BBS: backup & restore
                    q->skip_thumbnail_invalid = true;
                    model = Slic3r::Model::read_from_archive(path.string(), &config_loaded, &config_substitutions, en_3mf_file_type, strategy, &plate_data, &project_presets,
                                                             &file_version,
                                                             [this, &dlg, real_filename, &progress_percent, &file_percent, stage_percent, INPUT_FILES_RATIO, total_files, i,
                                                              &is_user_cancel](int import_stage, int current, int total, bool &cancel) {
                                                                 bool     cont = true;
                                                                 float percent_float = (100.0f * (float)i / (float)total_files) + INPUT_FILES_RATIO * ((float)stage_percent[import_stage] + (float)current * (float)(stage_percent[import_stage + 1] - stage_percent[import_stage]) /(float) total) / (float)total_files;
                                                                 BOOST_LOG_TRIVIAL(trace) << "load_3mf_file: percent(float)=" << percent_float << ", stage = " << import_stage << ", curr = " << current << ", total = " << total;
                                                                 progress_percent = (int)percent_float;
                                                                 wxString msg  = wxString::Format(_L("Loading file: %s"), from_path(real_filename));
                                                                 cont          = dlg.Update(progress_percent, msg);
                                                                 cancel        = !cont;
                                                                 if (cancel)
                                                                     is_user_cancel = cancel;
                                                             });
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__
                                            << boost::format(", plate_data.size %1%, project_preset.size %2%, is_bbs_3mf %3%, file_version %4% \n") % plate_data.size() %
                                                   project_presets.size() % (en_3mf_file_type == En3mfType::From_BBS) % file_version.to_string();

                    // 1. add extruder for prusa model if the number of existing extruders is not enough
                    // 2. add extruder for BBS or Other model if only import geometry
                    if (en_3mf_file_type == En3mfType::From_Prusa || (load_model && !load_config)) {
                        std::set<int> extruderIds;
                        for (ModelObject *o : model.objects) {
                            if (o->config.option("extruder")) extruderIds.insert(o->config.extruder());
                            for (auto volume : o->volumes) {
                                if (volume->config.option("extruder")) extruderIds.insert(volume->config.extruder());
                                for (int extruder : volume->get_extruders()) { extruderIds.insert(extruder); }
                            }
                        }
                        int size = extruderIds.size() == 0 ? 0 : *(extruderIds.rbegin());

                        int filament_size = sidebar->combos_filament().size();
                        while (filament_size < MAXIMUM_EXTRUDER_NUMBER && filament_size < size) {
                            int         filament_count = filament_size + 1;
                            wxColour    new_col        = Plater::get_next_color_for_filament();
                            std::string new_color      = new_col.GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
                            wxGetApp().preset_bundle->set_num_filaments(filament_count, new_color);
                            wxGetApp().plater()->on_filament_count_change(filament_count);
                            ++filament_size;
                        }
                        wxGetApp().get_tab(Preset::TYPE_PRINT)->update();
                    }

                    std::string import_project_action = wxGetApp().app_config->get("import_project_action");
                    LoadType load_type;
                    if (import_project_action.empty())
                        load_type = LoadType::Unknown;
                    else
                        load_type  = static_cast<LoadType>(std::stoi(import_project_action));

                    // BBS: version check
                    Semver app_version = *(Semver::parse(SoftFever_VERSION));
                    if (en_3mf_file_type == En3mfType::From_Prusa) {
                        // do not reset the model config
                        load_config = false;
                        if(load_type != LoadType::LoadGeometry)
                            show_info(q, _L("The 3MF is not supported by OrcaSlicer, loading geometry data only."), _L("Load 3MF"));
                    }
                    // else if (load_config && (file_version.maj() != app_version.maj())) {
                    //     // version mismatch, only load geometries
                    //     load_config = false;
                    //     if (!load_model) {
                    //         // only load config case, return directly
                    //         show_info(q, _L("The Config cannot be loaded."), _L("Load 3MF"));
                    //         q->skip_thumbnail_invalid = false;
                    //         return empty_result;
                    //     }
                    //     load_old_project = true;
                    //     // select view to 3D
                    //     q->select_view_3D("3D");
                    //     // select plate 0 as default
                    //     q->select_plate(0);
                    //     if (load_type != LoadType::LoadGeometry) {
                    //         if (en_3mf_file_type == En3mfType::From_BBS)
                    //             show_info(q, _L("The 3MF was generated by an old OrcaSlicer, loading geometry data only."), _L("Load 3MF"));
                    //         else
                    //             show_info(q, _L("The 3MF is not supported by OrcaSlicer, loading geometry data only."), _L("Load 3MF"));
                    //     }
                    //     for (ModelObject *model_object : model.objects) {
                    //         model_object->config.reset();
                    //         // Is there any modifier or advanced config data?
                    //         for (ModelVolume *model_volume : model_object->volumes) model_volume->config.reset();
                    //     }
                    // }
                    // Orca: check if the project is created with OrcaSlicer 2.3.1-alpha and use the sparse infill rotation template for non-safe infill patterns
                    else if (load_config && (file_version < app_version) && file_version == Semver("2.3.1-alpha")) {
                        if (!config_loaded.opt_string("sparse_infill_rotate_template").empty()) {
                            const auto _sparse_infill_pattern =
                                config_loaded.option<ConfigOptionEnum<InfillPattern>>("sparse_infill_pattern")->value;
                            bool is_safe_to_rotate = _sparse_infill_pattern == ipRectilinear || _sparse_infill_pattern == ipLine ||
                                                     _sparse_infill_pattern == ipZigZag || _sparse_infill_pattern == ipCrossZag ||
                                                     _sparse_infill_pattern == ipLockedZag;
                            if (!is_safe_to_rotate) {
                                wxString msg_text = _(
                                    L("This project was created with an OrcaSlicer 2.3.1-alpha and uses "
                                      "infill rotation template settings that may not work properly with your current infill pattern. "
                                      "This could result in weak support or print quality issues."));
                                msg_text += "\n\n" +
                                            _(L("Would you like OrcaSlicer to automatically fix this by clearing the rotation template settings?"));
                                MessageDialog dialog(wxGetApp().plater(), msg_text, "", wxICON_WARNING | wxYES | wxNO);
                                dialog.SetButtonLabel(wxID_YES, _L("Yes"));
                                dialog.SetButtonLabel(wxID_NO, _L("No"));
                                if (dialog.ShowModal() == wxID_YES) {
                                    config_loaded.opt_string("sparse_infill_rotate_template") = "";
                                }
                            }
                        }

                    } else if (load_config && (file_version > app_version)) {
                        if (config_substitutions.unrecogized_keys.size() > 0) {
                            wxString text  = wxString::Format(_L("The 3MF file version %s is newer than %s's version %s, found the following unrecognized keys:"),
                                                             file_version.to_string(), std::string(SLIC3R_APP_FULL_NAME), app_version.to_string());
                            text += "\n";
                            bool     first = true;
                            // std::string context = into_u8(text);
                            wxString context = text;
                            // if (wxGetApp().app_config->get("user_mode") == "develop") {
                            //     for (auto &key : config_substitutions.unrecogized_keys) {
                            //         context += "  -";
                            //         context += key;
                            //         context += ";\n";
                            //         first = false;
                            //     }
                            // }
                            wxString append = _L("You'd better upgrade your software.\n");
                            context += "\n\n";
                            // context += into_u8(append);
                            context += append;
                            show_info(q, context, _L("Newer 3MF version"));
                        }
                        else {
                            //if the minor version is not matched
                            if (file_version.min() != app_version.min()) {
                                wxString text  = wxString::Format(_L("The 3MF file version %s is newer than %s's version %s, we suggest to upgrade your software."),
                                                 file_version.to_string(), std::string(SLIC3R_APP_FULL_NAME), app_version.to_string());
                                text += "\n";
                                show_info(q, text, _L("Newer 3MF version"));
                            }
                        }
                    } 
                    else if (load_config && config_loaded.empty()) {
                        load_config = false;
                        show_info(q, _L("The 3MF file was generated by an old OrcaSlicer version, loading geometry data only."), _L("Load 3MF"));
                    }
                    else if (!load_config) {
                        // reset config except color
                        for (ModelObject *model_object : model.objects) {
                            bool has_extruder = model_object->config.has("extruder");
                            int  extruder_id  = -1;
                            // save the extruder information before reset
                            if (has_extruder) { extruder_id = model_object->config.extruder(); }

                            model_object->config.reset();

                            // restore the extruder after reset
                            if (has_extruder) { model_object->config.set("extruder", extruder_id); }

                            // Is there any modifier or advanced config data?
                            for (ModelVolume *model_volume : model_object->volumes) {
                                has_extruder = model_volume->config.has("extruder");
                                if (has_extruder) { extruder_id = model_volume->config.extruder(); }

                                model_volume->config.reset();

                                if (has_extruder) { model_volume->config.set("extruder", extruder_id); }
                            }
                        }
                    }

                    // plate data
                    if (plate_data.size() > 0) {
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", import 3mf UPDATE_GCODE_RESULT \n");
                        wxString msg = wxString::Format(_L("Loading file: %s"), from_path(real_filename));
                        dlg_cont     = dlg.Update(progress_percent, msg);
                        if (!dlg_cont) {
                            q->skip_thumbnail_invalid = false;
                            return empty_result;
                        }

                        Semver old_version(1, 5, 9);
                        if ((en_3mf_file_type == En3mfType::From_BBS) && (file_version < old_version) && load_model && load_config && !config_loaded.empty()) {
                            translate_old = true;
                            partplate_list.get_plate_size(current_width, current_depth, current_height);
                        }

                        if (load_config) {
                            if (translate_old) {
                                //set the size back
                                partplate_list.reset_size(current_width + Bed3D::Axes::DefaultTipRadius, current_depth + Bed3D::Axes::DefaultTipRadius, current_height, false);
                            }
                            project_filament_count = config_loaded.option<ConfigOptionStrings>("filament_colour")->size();
                            partplate_list.load_from_3mf_structure(plate_data, project_filament_count);
                            partplate_list.update_slice_context_to_current_plate(background_process);
                            this->preview->update_gcode_result(partplate_list.get_current_slice_result());
                            release_PlateData_list(plate_data);
                            sidebar->obj_list()->reload_all_plates();
                            q->suppress_background_process(true);
                        } else {
                            partplate_list.reload_all_objects();
                        }
                    }

                    // BBS:: project embedded presets
                    if ((project_presets.size() > 0) && load_config) {
                        // load project embedded presets
                        PresetsConfigSubstitutions preset_substitutions;
                        PresetBundle &             preset_bundle = *wxGetApp().preset_bundle;
                        preset_substitutions                     = preset_bundle.load_project_embedded_presets(project_presets, ForwardCompatibilitySubstitutionRule::Enable);
                        if (!preset_substitutions.empty()) show_substitutions_info(preset_substitutions);
                    }
                    if (project_presets.size() > 0) {
                        for (unsigned int i = 0; i < project_presets.size(); i++) { delete project_presets[i]; }
                        project_presets.clear();
                    }

                    if (load_config && !config_loaded.empty()) {
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", import 3mf IMPORT_LOAD_CONFIG \n");
                        wxString msg = wxString::Format(_L("Loading file: %s"), from_path(real_filename));
                        dlg_cont     = dlg.Update(progress_percent, msg);
                        if (!dlg_cont) {
                            q->skip_thumbnail_invalid = false;
                            return empty_result;
                        }

                        // Based on the printer technology field found in the loaded config, select the base for the config,
                        PrinterTechnology printer_technology = Preset::printer_technology(config_loaded);

                        config.apply(static_cast<const ConfigBase &>(FullPrintConfig::defaults()));
                        // and place the loaded config over the base.
                        config += std::move(config_loaded);
                        std::map<std::string, std::string> validity = config.validate();
                        if (!validity.empty()) {
                            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format("Param values in 3mf error: ");
                            for (std::map<std::string, std::string>::iterator it=validity.begin(); it!=validity.end(); ++it)
                                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format("%1%: %2%")%it->first %it->second;
                            //
                            NotificationManager *notify_manager = q->get_notification_manager();
                            std::string error_message = L("Invalid values found in the 3MF:");
                            error_message += "\n";
                            for (std::map<std::string, std::string>::iterator it=validity.begin(); it!=validity.end(); ++it)
                                error_message += "-" + it->first + ": " + it->second + "\n";
                            error_message += "\n";
                            error_message += L("Please correct them in the param tabs");
                            notify_manager->bbl_show_3mf_warn_notification(error_message);
                        }
                    }
                    if (!config_substitutions.empty()) show_substitutions_info(config_substitutions.substitutions, filename.string());

                    // BBS
                    if (load_model && !load_config) {
                        ;
                    }
                    else {
                        this->model.plates_custom_gcodes = model.plates_custom_gcodes;
                        this->model.design_info = model.design_info;
                        this->model.model_info = model.model_info;
                    }
                }

                if (load_config) {
                    if (!config.empty()) {
                        Preset::normalize(config);
                        PresetBundle *preset_bundle = wxGetApp().preset_bundle;

                        {
                            // BBS: modify the prime tower params for old version file
                            Semver old_version3(2, 0, 0);
                            if (en_3mf_file_type == En3mfType::From_BBS && file_version < old_version3) {
                                double old_filament_prime_volume = 0.;
                                int    filament_count            = 0;
                                {
                                    ConfigOptionFloats  *filament_prime_volume_option = config.option<ConfigOptionFloats>("filament_prime_volume");
                                    ConfigOptionStrings *filament_colors_option       = config.option<ConfigOptionStrings>("filament_colour", true);
                                    filament_count                                    = filament_colors_option->values.size();
                                    if (filament_prime_volume_option) {
                                        std::vector<double> &filament_prime_volume_values = filament_prime_volume_option->values;
                                        if (!filament_prime_volume_values.empty()) {
                                            old_filament_prime_volume = filament_prime_volume_values[0];
                                            if (filament_count > 1) filament_prime_volume_values.resize(filament_count, old_filament_prime_volume);
                                        }
                                    }
                                }
                                ConfigOptionEnum<WipeTowerWallType> *prime_tower_rib_wall_option = config.option<ConfigOptionEnum<WipeTowerWallType>>("wipe_tower_wall_type", true);
                                prime_tower_rib_wall_option->value            = WipeTowerWallType::wtwRectangle;

                                ConfigOptionPercent *prime_tower_infill_gap_option = config.option<ConfigOptionPercent>("prime_tower_infill_gap", true);
                                prime_tower_infill_gap_option->value               = 100;

                                ConfigOptionInts *filament_adhesiveness_category_option = config.option<ConfigOptionInts>("filament_adhesiveness_category", true);
                                std::vector<int> &filament_adhesiveness_category_values = filament_adhesiveness_category_option->values;
                                filament_adhesiveness_category_values.resize(filament_count);
                                for (int index = 0; index < filament_count; index++)
                                    filament_adhesiveness_category_values[index] = 100;

                                std::vector<std::string> &diff_settings = config.option<ConfigOptionStrings>("different_settings_to_system", true)->values;
                                diff_settings.resize(filament_count + 2);

                                std::vector<std::string> diff_process_keys;
                                std::string              diff_process_settings = diff_settings[0];
                                Slic3r::unescape_strings_cstyle(diff_process_settings, diff_process_keys);
                                diff_process_keys.emplace_back("wipe_tower_wall_type");
                                diff_process_keys.emplace_back("prime_tower_infill_gap");
                                diff_process_settings = Slic3r::escape_strings_cstyle(diff_process_keys);
                                diff_settings[0] = diff_process_settings;

                                for (int index = 0; index < filament_count; index++) {
                                    std::vector<std::string> diff_filament_keys;
                                    std::string              diff_filament_settings = diff_settings[index + 1];
                                    Slic3r::unescape_strings_cstyle(diff_filament_settings, diff_filament_keys);
                                    diff_filament_keys.emplace_back("filament_adhesiveness_category");
                                    diff_filament_settings   = Slic3r::escape_strings_cstyle(diff_filament_keys);
                                    diff_settings[index + 1] = diff_filament_settings;
                                }
                            }
                        }

                        auto choise = wxGetApp().app_config->get("no_warn_when_modified_gcodes");
                        if (choise.empty() || choise != "true") {
                            // BBS: first validate the printer
                            // validate the system profiles
                            std::set<std::string> modified_gcodes;
                            int validated = preset_bundle->validate_presets(filename.string(), config, modified_gcodes);
                            if (validated == VALIDATE_PRESETS_MODIFIED_GCODES) {
                                std::string warning_message;
                                warning_message += "\n";
                                for (std::set<std::string>::iterator it=modified_gcodes.begin(); it!=modified_gcodes.end(); ++it)
                                    warning_message += "-" + *it + "\n";
                                warning_message += "\n";
                                //show_info(q, _L("The 3MF has the following modified G-code in filament or printer presets:") + warning_message + _L("Please confirm that all modified G-code is safe to prevent any damage to the machine!"), _L("Modified G-code"));
                                MessageDialog dlg(q, _L("The 3MF has the following modified G-code in filament or printer presets:") + warning_message + _L("Please confirm that all modified G-code is safe to prevent any damage to the machine!"), _L("Modified G-code"));
                                dlg.show_dsa_button();
                                auto  res = dlg.ShowModal();
                                if (dlg.get_checkbox_state())
                                    wxGetApp().app_config->set("no_warn_when_modified_gcodes", "true");
                            }
                            else if ((validated == VALIDATE_PRESETS_PRINTER_NOT_FOUND) || (validated == VALIDATE_PRESETS_FILAMENTS_NOT_FOUND)) {
                                std::string warning_message;
                                warning_message += "\n";
                                for (std::set<std::string>::iterator it=modified_gcodes.begin(); it!=modified_gcodes.end(); ++it)
                                    warning_message += "-" + *it + "\n";
                                warning_message += "\n";
                                //show_info(q, _L("The 3MF has the following customized filament or printer presets:") + warning_message + _L("Please confirm that the G-code within these presets is safe to prevent any damage to the machine!"), _L("Customized Preset"));
                                MessageDialog dlg(q, _L("The 3MF has the following customized filament or printer presets:") + from_u8(warning_message)+ _L("Please confirm that the G-code within these presets is safe to prevent any damage to the machine!"), _L("Customized Preset"));
                                dlg.show_dsa_button();
                                auto  res = dlg.ShowModal();
                                if (dlg.get_checkbox_state())
                                    wxGetApp().app_config->set("no_warn_when_modified_gcodes", "true");
                            }
                        }

                        //always load config
                        {
                            // BBS: save the wipe tower pos in file here, will be used later
                            ConfigOptionFloats* wipe_tower_x_opt = config.opt<ConfigOptionFloats>("wipe_tower_x");
                            ConfigOptionFloats* wipe_tower_y_opt = config.opt<ConfigOptionFloats>("wipe_tower_y");
                            std::optional<ConfigOptionFloats>file_wipe_tower_x;
                            std::optional<ConfigOptionFloats>file_wipe_tower_y;
                            if (wipe_tower_x_opt)
                                file_wipe_tower_x = *wipe_tower_x_opt;
                            if (wipe_tower_y_opt)
                                file_wipe_tower_y = *wipe_tower_y_opt;

                            preset_bundle->load_config_model(filename.string(), std::move(config), file_version);

                            ConfigOption* bed_type_opt = preset_bundle->project_config.option("curr_bed_type");
                            if (bed_type_opt != nullptr) {
                                BedType bed_type = (BedType)bed_type_opt->getInt();
                                // update app config for bed type
                                bool is_bbl_preset = preset_bundle->is_bbl_vendor();
                                if (is_bbl_preset) {
                                    AppConfig* app_config = wxGetApp().app_config;
                                    if (app_config)
                                        app_config->set("curr_bed_type", std::to_string(int(bed_type)));
                                }
                                q->on_bed_type_change(bed_type);
                            }

                            // BBS: moved this logic to presetcollection
                            //{
                            //    // After loading of the presets from project, check if they are visible.
                            //    // Set them to visible if they are not.

                            //    auto update_selected_preset_visibility = [](PresetCollection& presets, std::vector<std::string>& names) {
                            //        if (!presets.get_selected_preset().is_visible) {
                            //            assert(presets.get_selected_preset().name == presets.get_edited_preset().name);
                            //            presets.get_selected_preset().is_visible = true;
                            //            presets.get_edited_preset().is_visible = true;
                            //            names.emplace_back(presets.get_selected_preset().name);
                            //        }
                            //    };

                            //    std::vector<std::string> names;
                            //    if (printer_technology == ptFFF) {
                            //        update_selected_preset_visibility(preset_bundle->prints, names);
                            //        for (const std::string& filament : preset_bundle->filament_presets) {
                            //            Preset* preset = preset_bundle->filaments.find_preset(filament);
                            //            if (preset && !preset->is_visible) {
                            //                preset->is_visible = true;
                            //                names.emplace_back(preset->name);
                            //                if (preset->name == preset_bundle->filaments.get_edited_preset().name)
                            //                    preset_bundle->filaments.get_selected_preset().is_visible = true;
                            //            }
                            //        }
                            //    }
                            //    else {
                            //        update_selected_preset_visibility(preset_bundle->sla_prints, names);
                            //        update_selected_preset_visibility(preset_bundle->sla_materials, names);
                            //    }
                            //    update_selected_preset_visibility(preset_bundle->printers, names);

                            //    preset_bundle->update_compatible(PresetSelectCompatibleType::Never);

                            //    // show notification about temporarily installed presets
                            //    if (!names.empty()) {
                            //        std::string notif_text = into_u8(_L_PLURAL("The preset below was temporarily installed on the active instance of PrusaSlicer",
                            //                                                   "The presets below were temporarily installed on the active instance of PrusaSlicer",
                            //                                                   names.size())) + ":";
                            //        for (std::string& name : names)
                            //            notif_text += "\n - " + name;
                            //        notification_manager->push_notification(NotificationType::CustomNotification,
                            //            NotificationManager::NotificationLevel::PrintInfoNotificationLevel, notif_text);
                            //    }
                            //}

                            // BBS
                            // if (printer_technology == ptFFF)
                            //    CustomGCode::update_custom_gcode_per_print_z_from_config(model.custom_gcode_per_print_z, &preset_bundle->project_config);

                            // For exporting from the amf/3mf we shouldn't check printer_presets for the containing information about "Print Host upload"
                            // BBS: add preset combo box re-active logic
                            // currently found only needs re-active here
                            wxGetApp().load_current_presets(false, false);
                            // Update filament colors for the MM-printer profile in the full config
                            // to avoid black (default) colors for Extruders in the ObjectList,
                            // when for extruder colors are used filament colors
                            q->on_filament_count_change(preset_bundle->filament_presets.size());
                            is_project_file = true;

                            DynamicConfig& proj_cfg = preset_bundle->project_config;
                            // do some post process after loading config
                            {
                                //BBS: rewrite wipe tower pos stored in 3mf file , the code above should be seriously reconsidered
                                 ConfigOptionFloats* wipe_tower_x = proj_cfg.opt<ConfigOptionFloats>("wipe_tower_x");
                                ConfigOptionFloats* wipe_tower_y = proj_cfg.opt<ConfigOptionFloats>("wipe_tower_y");
                                if (file_wipe_tower_x)
                                    *wipe_tower_x = *file_wipe_tower_x;
                                if (file_wipe_tower_y)
                                    *wipe_tower_y = *file_wipe_tower_y;

                                ConfigOptionStrings* filament_color = proj_cfg.opt<ConfigOptionStrings>("filament_colour");
                                if (filament_color) {
                                    size_t filament_count = filament_color->size();

                                    // Sync filament map
                                    ConfigOptionInts* filament_map = proj_cfg.opt<ConfigOptionInts>("filament_map", true);
                                    if (filament_map->size() != filament_count) {
                                        filament_map->values.resize(filament_count, 1);
                                    }

                                    // Sync filament multi colour
                                    ConfigOptionStrings* filament_multi_color = proj_cfg.opt<ConfigOptionStrings>("filament_multi_colour", true);
                                    if (filament_multi_color->size() != filament_count) {
                                        filament_multi_color->values.resize(filament_count);
                                    }
                                    // If there is no multi-color data or color is not match, use single color as default value
                                    for (size_t i = 0; i < filament_count; i++) {
                                        std::vector<std::string> colors = Slic3r::split_string(filament_multi_color->values[i], ' ');
                                        if (i >= filament_multi_color->values.size() || colors.empty() || colors[0] != filament_color->values[i] ) {
                                            filament_multi_color->values[i] = filament_color->values[i];
                                        }
                                    }
                                    // Sync filament colour type
                                    ConfigOptionStrings* filament_color_type = proj_cfg.opt<ConfigOptionStrings>("filament_colour_type", true);
                                    if (filament_color_type && filament_color_type->size() != filament_count) {
                                        filament_color_type->values.resize(filament_count);

                                        for (size_t i = 0; i < filament_count; i++) {
                                            if (i >= filament_color_type->values.size() || filament_color_type->values[i].empty()) {
                                                filament_color_type->values[i] = "1";
                                            }
                                        }
                                    }
                                }
                            }
                            // Update filament combobox after loading config
                            wxGetApp().plater()->sidebar().update_presets(Preset::TYPE_FILAMENT);
                        }
                    }
                    if (!silence) wxGetApp().app_config->update_config_dir(path.parent_path().string());
                }
            } else {
                // BBS: add plate data related logic
                PlateDataPtrs plate_data;
                // BBS: project embedded settings
                std::vector<Preset *> project_presets;
                bool                  is_xxx;
                Semver                file_version;

                //ObjImportColorFn obj_color_fun=nullptr;
                auto obj_color_fun = [this, &path](ObjDialogInOut &in_out) {

                    if (!boost::iends_with(path.string(), ".obj")) { return; }
                    const std::vector<std::string> extruder_colours = wxGetApp().plater()->get_extruder_colors_from_plater_config();
                    ObjColorDialog                 color_dlg(nullptr, in_out, extruder_colours);
                    if (color_dlg.ShowModal() != wxID_OK) {
                        in_out.filament_ids.clear();
                    }
                };
                if (boost::iends_with(path.string(), ".stp") ||
                    boost::iends_with(path.string(), ".step")) {
                        double linear = string_to_double_decimal_point(wxGetApp().app_config->get("linear_defletion"));
                        if (linear <= 0) linear = 0.003;
                        double angle = string_to_double_decimal_point(wxGetApp().app_config->get("angle_defletion"));
                        if (angle <= 0) angle = 0.5;
                        bool split_compound = wxGetApp().app_config->get_bool("is_split_compound");
                        model = Slic3r::Model:: read_from_step(path.string(), strategy,
                        [this, &dlg, real_filename, &progress_percent, &file_percent, step_percent, INPUT_FILES_RATIO, total_files, i](int load_stage, int current, int total, bool &cancel)
                        {
                                bool     cont = true;
                                float percent_float = (100.0f * (float)i / (float)total_files) + INPUT_FILES_RATIO * ((float)step_percent[load_stage] + (float)current * (float)(step_percent[load_stage + 1] - step_percent[load_stage]) / (float)total) / (float)total_files;
                                BOOST_LOG_TRIVIAL(trace) << "load_step_file: percent(float)=" << percent_float << ", stage = " << load_stage << ", curr = " << current << ", total = " << total;
                                progress_percent = (int)percent_float;
                                wxString msg  = wxString::Format(_L("Loading file: %s"), from_path(real_filename));
                                cont          = dlg.Update(progress_percent, msg);
                                cancel        = !cont;
                        },
                        [](int isUtf8StepFile) {
                            if (!isUtf8StepFile) {
                                const auto no_warn = wxGetApp().app_config->get_bool("step_not_utf8_no_warn");
                                if (!no_warn) {
                                    MessageDialog dlg(nullptr, _L("Name of components inside STEP file is not UTF8 format!") + "\n\n" + _L("The name may show garbage characters!"),
                                                      wxString(SLIC3R_APP_FULL_NAME " - ") + _L("Attention!"), wxOK | wxICON_INFORMATION);
                                    dlg.show_dsa_button(_L("Remember my choice."));
                                    dlg.ShowModal();
                                    if (dlg.get_checkbox_state()) {
                                        wxGetApp().app_config->set_bool("step_not_utf8_no_warn", true);
                                    }
                                }
                            }
                        },
                        [this, &path, &is_user_cancel, &linear, &angle, &split_compound](Slic3r::Step& file, double& linear_value, double& angle_value, bool& is_split)-> int {
                            if (wxGetApp().app_config->get_bool("enable_step_mesh_setting")) {
                                StepMeshDialog mesh_dlg(nullptr, file, linear, angle);
                                if (mesh_dlg.ShowModal() == wxID_OK) {
                                    linear_value = mesh_dlg.get_linear_defletion();
                                    angle_value  = mesh_dlg.get_angle_defletion();
                                    is_split     = mesh_dlg.get_split_compound_value();
                                    return 1;
                                }
                            }else {
                                linear_value = linear;
                                angle_value = angle;
                                is_split = split_compound;
                                return 1;
                            }
                            is_user_cancel = true;
                            return -1;
                        }, linear, angle, split_compound);
                }else {
                    model = Slic3r::Model:: read_from_file(
                    path.string(), nullptr, nullptr, strategy, &plate_data, &project_presets, &is_xxx, &file_version, nullptr,
                    [this, &dlg, real_filename, &progress_percent, &file_percent, INPUT_FILES_RATIO, total_files, i, &designer_model_id, &designer_country_code](int current, int total, bool &cancel, std::string &mode_id, std::string &code)
                    {
                            designer_model_id = mode_id;
                            designer_country_code = code;

                            bool     cont = true;
                            float percent_float = (100.0f * (float)i / (float)total_files) + INPUT_FILES_RATIO * 100.0f * ((float)current / (float)total) / (float)total_files;
                            BOOST_LOG_TRIVIAL(trace) << "load_stl_file: percent(float)=" << percent_float << ", curr = " << current << ", total = " << total;
                            progress_percent = (int)percent_float;
                            wxString msg  = wxString::Format(_L("Loading file: %s"), from_path(real_filename));
                            cont          = dlg.Update(progress_percent, msg);
                            cancel        = !cont;
                    },
                    nullptr, 0, obj_color_fun);
                }

                if (designer_model_id.empty() && boost::algorithm::iends_with(path.string(), ".stl")) {
                    read_binary_stl(path.string(), designer_model_id, designer_country_code);
                }

                if (type_any_amf && is_xxx) imperial_units = true;

                for (auto obj : model.objects) {
                    if (obj->name.empty()) {
                        obj->name = fs::path(obj->input_file).filename().string();
                    }
                    obj->rotate(Geometry::deg2rad(config->opt_float("preferred_orientation")), Axis::Z);
                }

                if (plate_data.size() > 0) {
                    partplate_list.load_from_3mf_structure(plate_data, project_filament_count);
                    partplate_list.update_slice_context_to_current_plate(background_process);
                    this->preview->update_gcode_result(partplate_list.get_current_slice_result());
                    release_PlateData_list(plate_data);
                    sidebar->obj_list()->reload_all_plates();
                }

                // BBS:: project embedded presets
                if (project_presets.size() > 0) {
                    // load project embedded presets
                    PresetsConfigSubstitutions preset_substitutions;
                    PresetBundle &             preset_bundle = *wxGetApp().preset_bundle;
                    preset_substitutions                     = preset_bundle.load_project_embedded_presets(project_presets, ForwardCompatibilitySubstitutionRule::Enable);
                    if (!preset_substitutions.empty()) show_substitutions_info(preset_substitutions);

                    for (unsigned int i = 0; i < project_presets.size(); i++) { delete project_presets[i]; }
                    project_presets.clear();
                }
            }
        } catch (const ConfigurationError &e) {
            std::string message = GUI::format(_L("Failed loading file \"%1%\". An invalid configuration was found."), filename.string()) + "\n\n" + e.what();
            GUI::show_error(q, message);
            continue;
        } catch (const std::exception &e) {
            if (!is_user_cancel)
                GUI::show_error(q, e.what());
            continue;
        }

        progress_percent = 100.0f * (float)i / (float)total_files + INIT_MODEL_RATIO * 100.0f / (float)total_files;
        dlg_cont = dlg.Update(progress_percent);
        if (!dlg_cont) {
            q->skip_thumbnail_invalid = false;
            return empty_result;
        }

        if (load_model) {
            // The model should now be initialized
            auto convert_from_imperial_units = [](Model &model, bool only_small_volumes) { model.convert_from_imperial_units(only_small_volumes); };

            // BBS: add load_old_project logic
            if ((!is_project_file) && (!load_old_project)) {
                // if (!is_project_file) {
                if (int deleted_objects = model.removed_objects_with_zero_volume(); deleted_objects > 0) {
                    MessageDialog(q, _L("Objects with zero volume removed"), _L("The volume of the object is zero"), wxICON_INFORMATION | wxOK).ShowModal();
                }
                if (imperial_units)
                    // Convert even if the object is big.
                    convert_from_imperial_units(model, false);
                else if (model.looks_like_saved_in_meters()) {
                    // BBS do not handle look like in meters
                    MessageDialog dlg(q,
                                      format_wxstr(_L("The object from file %s is too small, and maybe in meters or inches.\n Do you want to scale to millimeters?"),
                                                   from_path(filename)),
                                      _L("Object too small"), wxICON_QUESTION | wxYES_NO);
                    int           answer = dlg.ShowModal();
                    if (answer == wxID_YES) model.convert_from_meters(true);
                } else if (model.looks_like_imperial_units()) {
                    // BBS do not handle look like in meters
                    MessageDialog dlg(q,
                                      format_wxstr(_L("The object from file %s is too small, and maybe in meters or inches.\n Do you want to scale to millimeters?"),
                                                   from_path(filename)),
                                      _L("Object too small"), wxICON_QUESTION | wxYES_NO);
                    int           answer = dlg.ShowModal();
                    if (answer == wxID_YES) convert_from_imperial_units(model, true);
                }
                // else if (model.looks_like_imperial_units()) {
                // BBS do not handle look like in imperial
                // auto convert_model_if = [convert_from_imperial_units](Model& model, bool condition) {
                //    if (condition)
                //        //FIXME up-scale only the small parts?
                //        convert_from_imperial_units(model, true);
                //};
                // if (answer_convert_from_imperial_units == wxOK_DEFAULT) {
                //    RichMessageDialog dlg(q, format_wxstr(_L_PLURAL(
                //        "The dimensions of the object from file %s seem to be defined in inches.\n"
                //        "The internal unit of PrusaSlicer is a millimeter. Do you want to recalculate the dimensions of the object?",
                //        "The dimensions of some objects from file %s seem to be defined in inches.\n"
                //        "The internal unit of PrusaSlicer is a millimeter. Do you want to recalculate the dimensions of these objects?", model.objects.size()), from_path(filename))
                //        + "\n", _L("The object is too small"), wxICON_QUESTION | wxYES_NO);
                //    dlg.ShowCheckBox(_L("Apply to all the remaining small objects being loaded."));
                //    int answer = dlg.ShowModal();
                //    if (dlg.IsCheckBoxChecked())
                //        answer_convert_from_imperial_units = answer;
                //    else
                //        convert_model_if(model, answer == wxID_YES);
                //}
                // convert_model_if(model, answer_convert_from_imperial_units == wxID_YES);
            }

             if (!is_project_file && model.looks_like_multipart_object()) {
               MessageDialog msg_dlg(q, _L(
                    "This file contains several objects positioned at multiple heights.\n"
                    "Instead of considering them as multiple objects, should \n"
                    "the file be loaded as a single object having multiple parts?") + "\n",
                    _L("Multi-part object detected"), wxICON_WARNING | wxYES | wxNO);
                if (msg_dlg.ShowModal() == wxID_YES) {
                    model.convert_multipart_object(filaments_cnt);
                }
            }
        }
        // else if ((wxGetApp().get_mode() == comSimple) && (type_3mf || type_any_amf) && model_has_advanced_features(model)) {
        //    MessageDialog msg_dlg(q, _L("This file cannot be loaded in a simple mode. Do you want to switch to an advanced mode?")+"\n",
        //        _L("Detected advanced data"), wxICON_WARNING | wxYES | wxNO);
        //    if (msg_dlg.ShowModal() == wxID_YES) {
        //        Slic3r::GUI::wxGetApp().save_mode(comAdvanced);
        //        view3D->set_as_dirty();
        //    }
        //    else
        //        return obj_idxs;
        //}

        progress_percent = 100.0f * (float)i / (float)total_files + CENTER_AROUND_ORIGIN_RATIO * 100.0f / (float)total_files;
        dlg_cont = dlg.Update(progress_percent);
        if (!dlg_cont) {
            q->skip_thumbnail_invalid = false;
            return empty_result;
        }

        int model_idx = 0;
        for (ModelObject *model_object : model.objects) {
            if (!type_3mf && !type_any_amf)
                model_object->center_around_origin(false);

            // BBS
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format("import 3mf IMPORT_LOAD_MODEL_OBJECTS \n");
            wxString msg = wxString::Format("Loading file: %s", from_path(real_filename));
            model_idx++;
            dlg_cont = dlg.Update(progress_percent, msg);
            if (!dlg_cont) {
                q->skip_thumbnail_invalid = false;
                return empty_result;
            }

            if (!model_object->instances.empty())
                model_object->ensure_on_bed(is_project_file);
        }

        tolal_model_count += model_idx;

        progress_percent = 100.0f * (float)i / (float)total_files + LOAD_MODEL_RATIO * 100.0f / (float)total_files;
        dlg_cont = dlg.Update(progress_percent);
        if (!dlg_cont) {
            q->skip_thumbnail_invalid = false;
            return empty_result;
        }

        if (one_by_one) {
            // BBS: add load_old_project logic
            if (type_3mf && !is_project_file && !load_old_project)
                // if (type_3mf && !is_project_file)
                model.center_instances_around_point(this->bed.build_volume().bed_center());
            // BBS: add auxiliary files logic
            // BBS: backup & restore
            if (load_aux) {
                q->model().load_from(model);
                load_auxiliary_files();
            }
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", before load_model_objects, count %1%")%model.objects.size();
            auto loaded_idxs = load_model_objects(model.objects, is_project_file);
            obj_idxs.insert(obj_idxs.end(), loaded_idxs.begin(), loaded_idxs.end());

            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", finished load_model_objects");
            wxString msg = wxString::Format(_L("Loading file: %s"), from_path(real_filename));
            dlg_cont     = dlg.Update(progress_percent, msg);
            if (!dlg_cont) {
                q->skip_thumbnail_invalid = false;
                return empty_result;
            }
        } else {
            // This must be an .stl or .obj file, which may contain a maximum of one volume.
            for (const ModelObject *model_object : model.objects) {
                new_model->add_object(*model_object);

                BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":" << __LINE__ << boost::format(", added object %1%")%model_object->name;
                wxString msg = wxString::Format(_L("Loading file: %s"), from_path(real_filename));
                dlg_cont     = dlg.Update(progress_percent, msg);
                if (!dlg_cont) {
                    q->skip_thumbnail_invalid = false;
                    return empty_result;
                }
            }
        }
    }

    if (new_model != nullptr && new_model->objects.size() > 1) {
        //BBS do not popup this dialog

        if (ask_multi) {
            MessageDialog msg_dlg(q, _L("Load these files as a single object with multiple parts?\n"), _L("Object with multiple parts was detected"),
                                  wxICON_WARNING | wxYES | wxNO);
            if (msg_dlg.ShowModal() == wxID_YES) { new_model->convert_multipart_object(filaments_cnt); }
        }

        auto loaded_idxs = load_model_objects(new_model->objects);
        obj_idxs.insert(obj_idxs.end(), loaded_idxs.begin(), loaded_idxs.end());
    }

    if (load_config) {
        DeviceManager *dev = Slic3r::GUI::wxGetApp().getDeviceManager();
        if (dev) {
            MachineObject *obj = dev->get_selected_machine();
            if (obj && obj->is_info_ready()) {
                if (obj->GetExtderSystem()->GetTotalExtderCount() > 0) {
                    PresetBundle *preset_bundle  = wxGetApp().preset_bundle;
                    Preset       &printer_preset = preset_bundle->printers.get_selected_preset();

                    double              preset_nozzle_diameter = 0.4;
                    const ConfigOption *opt                    = printer_preset.config.option("nozzle_diameter");
                    if (opt) preset_nozzle_diameter = static_cast<const ConfigOptionFloats *>(opt)->values[0];
                    float machine_nozzle_diameter = obj->GetExtderSystem()->GetNozzleDiameter(0);

                    std::string machine_type = obj->printer_type;
                    if (obj->is_support_upgrade_kit && obj->installed_upgrade_kit) machine_type = "C12";

                    if (printer_preset.get_current_printer_type(preset_bundle) != machine_type || !is_approx((float) preset_nozzle_diameter, machine_nozzle_diameter)) {
                        Preset *machine_preset = get_printer_preset(obj);
                        if (machine_preset != nullptr) {
                            std::string printer_model = machine_preset->config.option<ConfigOptionString>("printer_model")->value;
                            bool sync_printer_info = false;
                            if (!wxGetApp().app_config->has("sync_after_load_file_show_flag")) {
                                wxString tips = from_u8((boost::format(_u8L("Connected printer is %s. It must match the project preset for printing.\n")) % printer_model).str());

                                tips += _L("Do you want to sync the printer information and automatically switch the preset?");
                                TipsDialog dlg(wxGetApp().mainframe, _L("Tips"), tips, "sync_after_load_file_show_flag", wxYES_NO);
                                if (dlg.ShowModal() == wxID_YES) { sync_printer_info = true; }
                            }
                            else {
                                sync_printer_info = wxGetApp().app_config->get("sync_after_load_file_show_flag") == "true";
                            }
                            if (sync_printer_info) {
                                Tab *printer_tab = GUI::wxGetApp().get_tab(Preset::Type::TYPE_PRINTER);
                                printer_tab->select_preset(machine_preset->name);
                                if (obj->is_multi_extruders()) GUI::wxGetApp().sidebar().sync_extruder_list();
                            }
                        }
                    }
                }
            }
        }
    }

    if (new_model) delete new_model;

    //BBS: translate old 3mf to correct positions
    if (translate_old) {
        //translate the objects
        int plate_count = partplate_list.get_plate_count();
        for (int index = 1; index < plate_count; index ++) {
            PartPlate* cur_plate = (PartPlate *)partplate_list.get_plate(index);

            Vec3d cur_origin = cur_plate->get_origin();
            Vec3d new_origin = partplate_list.compute_origin_using_new_size(index, current_width, current_depth);

            cur_plate->translate_all_instance(new_origin - cur_origin);
        }
        view3D->get_canvas3d()->remove_raycasters_for_picking(SceneRaycaster::EType::Bed);
        partplate_list.reset_size(current_width, current_depth, current_height, true, true);
        partplate_list.register_raycasters_for_picking(*view3D->get_canvas3d());
    }

    //BBS: add gcode loading logic in the end
    q->m_exported_file = false;
    q->skip_thumbnail_invalid = false;
    if (load_model && load_config) {
        if (model.objects.empty()) {
            partplate_list.load_gcode_files();
            PartPlate * first_plate = nullptr, *cur_plate = nullptr;
            int plate_cnt = partplate_list.get_plate_count();
            int index = 0, first_plate_index = 0;
            q->m_valid_plates_count = 0;
            for (index = 0; index < plate_cnt; index ++)
            {
                cur_plate = partplate_list.get_plate(index);
                if (!first_plate && cur_plate->is_slice_result_valid()) {
                    first_plate = cur_plate;
                    first_plate_index = index;
                }
                if (cur_plate->is_slice_result_valid())
                    q->m_valid_plates_count ++;
            }
            if (first_plate&&first_plate->is_slice_result_valid()) {
                q->m_exported_file = true;
                //select plate 0 as default
                q->select_plate(first_plate_index);
                //set to 3d tab
                q->select_view_3D("Preview");
                wxGetApp().mainframe->select_tab(MainFrame::tpPreview);
            }
            else {
                //set to 3d tab
                q->select_view_3D("3D");
                //select plate 0 as default
                q->select_plate(0);
            }
        }
        else {
            //set to 3d tab
            q->select_view_3D("3D");
            //select plate 0 as default
            q->select_plate(0);
        }
    }
    else {
        //always set to 3D after loading files
        q->select_view_3D("3D");
        wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);
    }

    if (load_model) {
        if (!silence) wxGetApp().app_config->update_skein_dir(input_files[input_files.size() - 1].parent_path().make_preferred().string());
        // XXX: Plater.pm had @loaded_files, but didn't seem to fill them with the filenames...
    }

    // automatic selection of added objects
    if (!obj_idxs.empty() && view3D != nullptr) {
        // update printable state for new volumes on canvas3D
        wxGetApp().plater()->canvas3D()->update_instance_printable_state_for_objects(obj_idxs);

        if (!load_config) {
            Selection& selection = view3D->get_canvas3d()->get_selection();
            selection.clear();
            for (size_t idx : obj_idxs) {
                selection.add_object((unsigned int)idx, false);
            }
        }
        // BBS: update object list selection
        this->sidebar->obj_list()->update_selections();

        if (view3D->get_canvas3d()->get_gizmos_manager().is_enabled())
            // this is required because the selected object changed and the flatten on face an sla support gizmos need to be updated accordingly
            view3D->get_canvas3d()->update_gizmos_on_off_state();
    }

    GLGizmoSimplify::add_simplify_suggestion_notification(
        obj_idxs, model.objects, *notification_manager);

    //set designer_model_id
    q->model().stl_design_id = designer_model_id;
    q->model().stl_design_country = designer_country_code;
    //if (!designer_model_id.empty() && q->model().stl_design_id.empty() && !designer_country_code.empty()) {
    //    q->model().stl_design_id = designer_model_id;
    //    q->model().stl_design_country = designer_country_code;
    //}
    //else {
    //    q->model().stl_design_id = "";
    //    q->model().stl_design_country = "";
    //}

    if (tolal_model_count <= 0 && !q->m_exported_file) {
        dlg.Hide();
        if (!is_user_cancel) {
            MessageDialog msg(wxGetApp().mainframe, _L("The file does not contain any geometry data."), _L("Warning"), wxYES | wxICON_WARNING);
            if (msg.ShowModal() == wxID_YES) {}
        }
    }
    q->schedule_background_process(true);
    return obj_idxs;
}

 #define AUTOPLACEMENT_ON_LOAD

std::vector<size_t> Plater::priv::load_model_objects(const ModelObjectPtrs& model_objects, bool allow_negative_z, bool split_object)
{
    const Vec3d bed_size = Slic3r::to_3d(this->bed.build_volume().bounding_volume2d().size(), 1.0) - 2.0 * Vec3d::Ones();

#ifndef AUTOPLACEMENT_ON_LOAD
    // bool need_arrange = false;
#endif /* AUTOPLACEMENT_ON_LOAD */
    bool scaled_down = false;
    std::vector<size_t> obj_idxs;
    unsigned int obj_count = model.objects.size();

#ifdef AUTOPLACEMENT_ON_LOAD
    ModelInstancePtrs new_instances;
#endif /* AUTOPLACEMENT_ON_LOAD */
    for (ModelObject *model_object : model_objects) {
        auto *object = model.add_object(*model_object);
        object->sort_volumes(true);
        std::string object_name = object->name.empty() ? fs::path(object->input_file).filename().string() : object->name;
        obj_idxs.push_back(obj_count++);

        if (model_object->instances.empty()) {
#ifdef AUTOPLACEMENT_ON_LOAD
            object->center_around_origin();
            new_instances.emplace_back(object->add_instance());
#else /* AUTOPLACEMENT_ON_LOAD */
            // if object has no defined position(s) we need to rearrange everything after loading
            // need_arrange = true;
             // add a default instance and center object around origin
            object->center_around_origin();  // also aligns object to Z = 0
            ModelInstance* instance = object->add_instance();

            //BBS calc transformation
            Geometry::Transformation t = instance->get_transformation();
            instance->set_offset(Slic3r::to_3d(this->bed.build_volume().bed_center(), -object->origin_translation(2)));
#endif /* AUTOPLACEMENT_ON_LOAD */
        }

        //BBS: when the object is too large, let the user choose whether to scale it down
        for (size_t i = 0; i < object->instances.size(); ++i) {
            ModelInstance* instance = object->instances[i];
            const Vec3d size = object->instance_bounding_box(i).size();
            const Vec3d ratio = size.cwiseQuotient(bed_size);
            const double max_ratio = std::max(ratio(0), ratio(1));
            if (max_ratio > 10000) {
                MessageDialog dlg(q, _L("Your object appears to be too large, do you want to scale it down to fit the print bed automatically?"), _L("Object too large"),
                                  wxICON_QUESTION | wxYES);
                int           answer = dlg.ShowModal();
                // the size of the object is too big -> this could lead to overflow when moving to clipper coordinates,
                // so scale down the mesh
                object->scale_mesh_after_creation(1. / max_ratio);
                object->origin_translation = Vec3d::Zero();
                object->center_around_origin();
                scaled_down = true;
                break;
            }
            else if (max_ratio > 10) {
                MessageDialog dlg(q, _L("Your object appears to be too large, do you want to scale it down to fit the print bed automatically?"), _L("Object too large"),
                                  wxICON_QUESTION | wxYES_NO);
                int           answer = dlg.ShowModal();
                if (answer == wxID_YES) {
                    instance->set_scaling_factor(instance->get_scaling_factor() / max_ratio);
                    scaled_down = true;
                }
            }
        }

        object->ensure_on_bed(allow_negative_z);
        if (!split_object) {
            //BBS initial assemble transformation
            for (ModelObject* model_object : model.objects) {
                //BBS initialize assemble transformation
                for (int i = 0; i < model_object->instances.size(); i++) {
                    if (!model_object->instances[i]->is_assemble_initialized()) {
                        model_object->instances[i]->set_assemble_transformation(model_object->instances[i]->get_transformation());
                    }
                }
            }
        }
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", loaded objects, begin to auto placement");
#ifdef AUTOPLACEMENT_ON_LOAD
#if 0
    // FIXME distance should be a config value /////////////////////////////////
    auto min_obj_distance = static_cast<coord_t>(6/SCALING_FACTOR);
    const auto *bed_shape_opt = config->opt<ConfigOptionPoints>("printable_area");
    assert(bed_shape_opt);
    auto& bedpoints = bed_shape_opt->values;
    Polyline bed; bed.points.reserve(bedpoints.size());
    for(auto& v : bedpoints) bed.append(Point::new_scale(v(0), v(1)));

    // BBS: get wipe tower of current plate
    int cur_plate_idx = partplate_list.get_curr_plate_index();
    std::pair<bool, GLCanvas3D::WipeTowerInfo> wti = view3D->get_canvas3d()->get_wipe_tower_info(cur_plate_idx);

    arr::find_new_position(model, new_instances, min_obj_distance, bed, wti);

    // it remains to move the wipe tower:
    view3D->get_canvas3d()->arrange_wipe_tower(wti);
#else
    // BBS: find an empty cell to put the copied object
    for (auto& instance : new_instances) {
        auto offset = instance->get_offset();
        auto start_point = this->bed.build_volume().bounding_volume2d().center();
        bool plate_empty = partplate_list.get_curr_plate()->empty();
        Vec3d displacement;
        if (plate_empty)
            displacement = {start_point(0), start_point(1), offset(2)};
        else {
            auto empty_cell = wxGetApp().plater()->canvas3D()->get_nearest_empty_cell({start_point(0), start_point(1)});
            displacement    = {empty_cell.x(), empty_cell.y(), offset(2)};
        }
        instance->set_offset(displacement);
    }
#endif

#endif /* AUTOPLACEMENT_ON_LOAD */

    //BBS: remove the auto scaled_down logic when load models
    //if (scaled_down) {
    //    GUI::show_info(q,
    //        _L("Your object appears to be too large, so it was automatically scaled down to fit your print bed."),
    //        _L("Object too large?"));
    //}

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", finished auto placement, before add_objects_to_list");
    notification_manager->close_notification_of_type(NotificationType::UpdatedItemsInfo);

    if (obj_idxs.size() > 1) {
        std::vector<size_t> obj_idxs_1 (obj_idxs.begin(), obj_idxs.end() - 1);

        wxGetApp().obj_list()->add_objects_to_list(obj_idxs_1, false);
        wxGetApp().obj_list()->add_object_to_list(obj_idxs[obj_idxs.size() - 1]);
    }
    else
        wxGetApp().obj_list()->add_objects_to_list(obj_idxs);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":" << __LINE__ << boost::format(", after add_objects_to_list");
    update();
    // Update InfoItems in ObjectList after update() to use of a correct value of the GLCanvas3D::is_sinking(),
    // which is updated after a view3D->reload_scene(false, flags & (unsigned int)UpdateParams::FORCE_FULL_SCREEN_REFRESH) call
    for (const size_t idx : obj_idxs)
        wxGetApp().obj_list()->update_info_items(idx);

    object_list_changed();

    this->schedule_background_process();

    return obj_idxs;
}

// BBS
void Plater::priv::load_auxiliary_files()
{
    std::string auxiliary_path = encode_path(q->model().get_auxiliary_file_temp_path().c_str());
    //wxGetApp().mainframe->m_project->Reload(auxiliary_path);
}

fs::path Plater::priv::get_export_file_path(GUI::FileType file_type)
{
    // Update printbility state of each of the ModelInstances.
    this->update_print_volume_state();

    const Selection& selection = get_selection();
    int obj_idx = selection.get_object_idx();

    fs::path output_file;
    if (file_type == FT_3MF)
        // for 3mf take the path from the project filename, if any
        output_file = into_path(get_project_filename(".3mf"));
    else if (file_type == FT_STL) {
        if (obj_idx > 0 && obj_idx < this->model.objects.size() && selection.is_single_full_object()) {
            output_file = this->model.objects[obj_idx]->get_export_filename();
        }
        else {
            output_file = into_path(get_project_name());
        }
    }
    //bbs  name the project using the part name
    if (output_file.empty()) {
        if (get_project_name() != _L("Untitled")) {
            output_file = into_path(get_project_name() + ".3mf");
        }
    }

    if (output_file.empty())
    {
        // first try to get the file name from the current selection
        if ((0 <= obj_idx) && (obj_idx < (int)this->model.objects.size()))
            output_file = this->model.objects[obj_idx]->get_export_filename();

        if (output_file.empty())
            // Find the file name of the first printable object.
            output_file = this->model.propose_export_file_name_and_path();

        if (output_file.empty() && !model.objects.empty())
            // Find the file name of the first object.
            output_file = this->model.objects[0]->get_export_filename();

        if (output_file.empty())
            // Use _L("Untitled") name
            output_file = into_path(_L("Untitled"));
    }
    return output_file;
}

wxString Plater::priv::get_export_file(GUI::FileType file_type)
{
    wxString wildcard;
    switch (file_type) {
        case FT_STL:
        case FT_AMF:
        case FT_3MF:
        case FT_GCODE:
        case FT_OBJ:
            wildcard = file_wildcards(file_type);
        break;
        default:
            wildcard = file_wildcards(FT_MODEL);
        break;
    }

    fs::path output_file = get_export_file_path(file_type);

    wxString dlg_title;
    switch (file_type) {
        case FT_STL:
        {
            output_file.replace_extension("stl");
            dlg_title = _L("Export STL file:");
            break;
        }
        case FT_AMF:
        {
            // XXX: Problem on OS X with double extension?
            output_file.replace_extension("zip.amf");
            dlg_title = _L("Export AMF file:");
            break;
        }
        case FT_3MF:
        {
            output_file.replace_extension("3mf");
            dlg_title = _L("Save file as:");
            break;
        }
        case FT_OBJ:
        {
            output_file.replace_extension("obj");
            dlg_title = _L("Export OBJ file:");
            break;
        }
        default: break;
    }

    std::string out_dir = (boost::filesystem::path(output_file).parent_path()).string();

    wxFileDialog dlg(q, dlg_title,
        is_shapes_dir(out_dir) ? from_u8(wxGetApp().app_config->get_last_dir()) : from_path(output_file.parent_path()), from_path(output_file.filename()),
        wildcard, wxFD_SAVE | wxFD_OVERWRITE_PROMPT | wxPD_APP_MODAL);

    int result = dlg.ShowModal();
    if (result == wxID_CANCEL)
        return "<cancel>";
    if (result != wxID_OK)
        return wxEmptyString;

    wxString out_path = dlg.GetPath();
    fs::path path(into_path(out_path));
#ifdef __WXMSW__
    if (boost::iequals(path.extension().string(), output_file.extension().string()) == false) {
        out_path += output_file.extension().string();
        boost::system::error_code ec;
        if (boost::filesystem::exists(into_u8(out_path), ec)) {
            auto result = MessageBox(q->GetHandle(),
                wxString::Format(_L("The file %s already exists\nDo you want to replace it?"), out_path),
                _L("Confirm Save As"),
                MB_YESNO | MB_ICONWARNING);
            if (result != IDYES)
                return wxEmptyString;
        }
    }
#endif
    wxGetApp().app_config->update_last_output_dir(path.parent_path().string());

    return out_path;
}

const Selection& Plater::priv::get_selection() const
{
    return view3D->get_canvas3d()->get_selection();
}

Selection& Plater::priv::get_selection()
{
    return view3D->get_canvas3d()->get_selection();
}

Selection& Plater::priv::get_curr_selection()
{
    return get_current_canvas3D()->get_selection();
}

int Plater::priv::get_selected_object_idx() const
{
    int idx = get_selection().get_object_idx();
    return ((0 <= idx) && (idx < 1000)) ? idx : -1;
}

int Plater::priv::get_selected_volume_idx() const
{
    auto& selection = get_selection();
    int idx = selection.get_object_idx();
    if ((0 > idx) || (idx > 1000))
        return-1;
    const GLVolume* v = selection.get_first_volume();
    if (model.objects[idx]->volumes.size() > 1)
        return v->volume_idx();
    return -1;
}

void Plater::priv::selection_changed()
{
    // if the selection is not valid to allow for layer editing, we need to turn off the tool if it is running
    if (!layers_height_allowed() && view3D->is_layers_editing_enabled()) {
        SimpleEvent evt(EVT_GLTOOLBAR_LAYERSEDITING);
        on_action_layersediting(evt);
    }

    // forces a frame render to update the view (to avoid a missed update if, for example, the context menu appears)
    if (get_current_canvas3D()->get_canvas_type() == GLCanvas3D::CanvasAssembleView) {
        assemble_view->render();
    } else {
        view3D->render();
    }
}

void Plater::priv::object_list_changed()
{
    const bool export_in_progress = this->background_process.is_export_scheduled(); // || ! send_gcode_file.empty());
    // XXX: is this right?
    //const bool model_fits = view3D->get_canvas3d()->check_volumes_outside_state() == ModelInstancePVS_Inside;
    ObjectFilamentResults object_results;
    bool model_fits = view3D->get_canvas3d()->check_volumes_outside_state(&object_results) != ModelInstancePVS_Partly_Outside;
    model_fits = model_fits && object_results.filaments.empty();

    PartPlate* part_plate = partplate_list.get_curr_plate();

    // BBS
    //sidebar->enable_buttons(!model.objects.empty() && !export_in_progress && model_fits && part_plate->has_printable_instances());
    bool can_slice = !model.objects.empty() && !export_in_progress && model_fits && part_plate->has_printable_instances();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": can_slice %1%, model_fits= %2%, export_in_progress %3%, has_printable_instances %4% ")%can_slice %model_fits %export_in_progress %part_plate->has_printable_instances();
    main_frame->update_slice_print_status(MainFrame::eEventObjectUpdate, can_slice);

    wxGetApp().params_panel()->notify_object_config_changed();
}

void Plater::priv::select_curr_plate_all()
{
    view3D->select_curr_plate_all();
    this->sidebar->obj_list()->update_selections();
}

void Plater::priv::remove_curr_plate_all()
{
    SingleSnapshot ss(q);
    view3D->remove_curr_plate_all();
    this->sidebar->obj_list()->update_selections();
}

void Plater::priv::select_all()
{
    view3D->select_all();
    this->sidebar->obj_list()->update_selections();
}

void Plater::priv::deselect_all()
{
    view3D->deselect_all();
}

void Plater::priv::exit_gizmo()
{
    view3D->exit_gizmo();
}

void Plater::priv::remove(size_t obj_idx)
{
    if (view3D->is_layers_editing_enabled())
        view3D->enable_layers_editing(false);

    m_worker.cancel_all();
    model.delete_object(obj_idx);
    //BBS: notify partplate the instance removed
    partplate_list.notify_instance_removed(obj_idx, -1);
    update();
    // Delete object from Sidebar list. Do it after update, so that the GLScene selection is updated with the modified model.
    sidebar->obj_list()->delete_object_from_list(obj_idx);
    object_list_changed();
}


bool Plater::priv::delete_object_from_model(size_t obj_idx, bool refresh_immediately)
{
    // check if object isn't cut
    // show warning message that "cut consistancy" will not be supported any more
    ModelObject *obj = model.objects[obj_idx];
    if (obj->is_cut()) {
        InfoDialog dialog(q, _L("Delete object which is a part of cut object"),
                          _L("You try to delete an object which is a part of a cut object.\n"
                             "This action will break a cut correspondence.\n"
                             "After that model consistency can't be guaranteed."),
                          false, wxYES | wxCANCEL | wxCANCEL_DEFAULT | wxICON_WARNING);
        dialog.SetButtonLabel(wxID_YES, _L("Delete"));
        if (dialog.ShowModal() == wxID_CANCEL)
            return false;
    }

    std::string snapshot_label = "Delete Object";
    if (!obj->name.empty())
        snapshot_label += ": " + obj->name;
    Plater::TakeSnapshot snapshot(q, snapshot_label);
    m_worker.cancel_all();

    if (obj->is_cut())
        sidebar->obj_list()->invalidate_cut_info_for_object(obj_idx);

    model.delete_object(obj_idx);
    //BBS: notify partplate the instance removed
    partplate_list.notify_instance_removed(obj_idx, -1);

    //BBS
    if (refresh_immediately) {
        update();
        object_list_changed();
    }

    return true;
}

void Plater::priv::delete_all_objects_from_model()
{
    Plater::TakeSnapshot snapshot(q, "Delete All Objects");

    if (view3D->is_layers_editing_enabled())
        view3D->enable_layers_editing(false);

    reset_gcode_toolpaths();
    gcode_result.reset();

    view3D->get_canvas3d()->reset_sequential_print_clearance();

    m_worker.cancel_all();

    // Stop and reset the Print content.
    background_process.reset();

    //BBS: update partplate
    partplate_list.clear();

    model.clear_objects();
    update();
    // Delete object from Sidebar list. Do it after update, so that the GLScene selection is updated with the modified model.
    sidebar->obj_list()->delete_all_objects_from_list();
    object_list_changed();

    //BBS
    model.calib_pa_pattern.reset();
    model.plates_custom_gcodes.clear();
}

void Plater::priv::reset(bool apply_presets_change)
{
    Plater::TakeSnapshot snapshot(q, "Reset Project", UndoRedo::SnapshotType::ProjectSeparator);

    clear_warnings();

    set_project_filename("");
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << __LINE__ << " call set_project_filename: empty";

    if (view3D->is_layers_editing_enabled())
        view3D->get_canvas3d()->force_main_toolbar_left_action(view3D->get_canvas3d()->get_main_toolbar_item_id("layersediting"));
    view3D->get_canvas3d()->reset_all_gizmos();

    reset_gcode_toolpaths();
    //BBS: update gcode to current partplate's
    //GCodeProcessorResult* current_result = this->background_process.get_current_plate()->get_slice_result();
    //current_result->reset();
    //gcode_result.reset();

    view3D->get_canvas3d()->reset_sequential_print_clearance();

    m_worker.cancel_all();

    //BBS: clear the partplate list's object before object cleared
    partplate_list.reinit();
    partplate_list.update_slice_context_to_current_plate(background_process);
    preview->update_gcode_result(partplate_list.get_current_slice_result());

    // Stop and reset the Print content.
    this->background_process.reset();
    model.clear_objects();
    assemble_view->get_canvas3d()->reset_explosion_ratio();
    update();

    //BBS
    if (wxGetApp().is_editor()) {
        // Delete object from Sidebar list. Do it after update, so that the GLScene selection is updated with the modified model.
        sidebar->obj_list()->delete_all_objects_from_list();
        object_list_changed();
    }

    project.reset();

    wxGetApp().sidebar().printer_combox()->clear_selected_dev_id();
    //BBS: reset all project embedded presets
    wxGetApp().preset_bundle->reset_project_embedded_presets();
    if (apply_presets_change)
        wxGetApp().apply_keeped_preset_modifications();
    else
        wxGetApp().load_current_presets(false, false);

    //BBS
    model.calib_pa_pattern.reset();
    model.plates_custom_gcodes.clear();

    // BBS
    m_saved_timestamp = m_backup_timestamp = size_t(-1);

    // Save window layout
    if (sidebar_layout.is_enabled) {
        // Reset show state
        auto& sidebar = m_aui_mgr.GetPane(this->sidebar);
        if (!sidebar_layout.is_collapsed && !sidebar.IsShown()) {
            sidebar.Show();
        }
        auto layout = m_aui_mgr.SavePerspective();
        wxGetApp().app_config->set("window_layout", layout.utf8_string());
    }
}

void Plater::priv::center_selection()
{
    view3D->center_selected();
}

void Plater::priv::drop_selection()
{
    view3D->drop_selected();
}

void Plater::priv::mirror(Axis axis)
{
    view3D->mirror_selection(axis);
}

void Plater::find_new_position(const ModelInstancePtrs &instances)
{
    arrangement::ArrangePolygons movable, fixed;
    arrangement::ArrangeParams arr_params = init_arrange_params(this);

    for (const ModelObject *mo : p->model.objects)
        for (ModelInstance *inst : mo->instances) {
            auto it = std::find(instances.begin(), instances.end(), inst);
            arrangement::ArrangePolygon arrpoly;
            inst->get_arrange_polygon(&arrpoly);

            if (it == instances.end())
                fixed.emplace_back(std::move(arrpoly));
            else {
                arrpoly.setter = [it](const arrangement::ArrangePolygon &p) {
                    if (p.is_arranged() && p.bed_idx == 0) {
                        Vec2d t = p.translation.cast<double>();
                        (*it)->apply_arrange_result(t, p.rotation);
                    }
                };
                movable.emplace_back(std::move(arrpoly));
            }
        }

    if (auto wt = get_wipe_tower_arrangepoly(*this))
        fixed.emplace_back(*wt);

    arrangement::arrange(movable, fixed, this->build_volume().polygon(), arr_params);

    for (auto & m : movable)
        m.apply();
}

void Plater::priv::split_object()
{
    int obj_idx = get_selected_object_idx();
    if (obj_idx == -1)
        return;

    // we clone model object because split_object() adds the split volumes
    // into the same model object, thus causing duplicates when we call load_model_objects()
    Model new_model = model;
    ModelObject* current_model_object = new_model.objects[obj_idx];

    wxBusyCursor wait;
    ModelObjectPtrs new_objects;
    current_model_object->split(&new_objects);
    if (new_objects.size() == 1)
        // #ysFIXME use notification
        Slic3r::GUI::warning_catcher(q, _L("The selected object couldn't be split."));
    else
    {
        // BBS no solid parts removed
        // If we splited object which is contain some parts/modifiers then all non-solid parts (modifiers) were deleted
        //if (current_model_object->volumes.size() > 1 && current_model_object->volumes.size() != new_objects.size())
        //    notification_manager->push_notification(NotificationType::CustomNotification,
        //        NotificationManager::NotificationLevel::PrintInfoNotificationLevel,
        //        _u8L("All non-solid parts (modifiers) were deleted"));

        Plater::TakeSnapshot snapshot(q, "Split to Objects");

        remove(obj_idx);

        // load all model objects at once, otherwise the plate would be rearranged after each one
        // causing original positions not to be kept
        //BBS: set split_object to true to avoid re-compute assemble matrix
        std::vector<size_t> idxs = load_model_objects(new_objects, false, true);

        // select newly added objects
        for (size_t idx : idxs)
        {
            get_selection().add_object((unsigned int)idx, false);
        }
    }
}

void Plater::priv::split_volume()
{
    wxGetApp().obj_list()->split();
}

void Plater::priv::scale_selection_to_fit_print_volume()
{
#if ENABLE_ENHANCED_PRINT_VOLUME_FIT
    this->view3D->get_canvas3d()->get_selection().scale_to_fit_print_volume(this->bed.build_volume());
#else
    this->view3D->get_canvas3d()->get_selection().scale_to_fit_print_volume(*config);
#endif // ENABLE_ENHANCED_PRINT_VOLUME_FIT
}

void Plater::priv::schedule_background_process()
{
    delayed_error_message.clear();
    // Trigger the timer event after 0.5s
    this->background_process_timer.Start(500, wxTIMER_ONE_SHOT);
    // Notify the Canvas3D that something has changed, so it may invalidate some of the layer editing stuff.
    this->view3D->get_canvas3d()->set_config(this->config);
}

void Plater::priv::schedule_auto_reslice_if_needed()
{
    AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr || !cfg->get_bool("auto_slice_after_change"))
        return;

    if (model.objects.empty())
        return;

    PartPlate* plate = partplate_list.get_curr_plate();
    if (plate == nullptr || !plate->has_printable_instances())
        return;

    if (background_process.running() || m_is_slicing) {
        // Remember to restart once the current slice stops and cancel it now.
        auto_reslice_after_cancel = true;
        background_process.stop();
        return;
    }

    const int delay_seconds = auto_slice_delay_seconds();
    if (delay_seconds > 0) {
        auto_reslice_pending = true;
        auto_reslice_timer.Stop();
        auto_reslice_timer.Start(delay_seconds * 1000, wxTIMER_ONE_SHOT);
        return;
    }

    if (auto_reslice_pending)
        return;

    auto_reslice_pending = true;
    auto_reslice_timer.Stop();
    wxGetApp().CallAfter([this]() { this->trigger_auto_reslice_now(); });
}

void Plater::priv::trigger_auto_reslice_now()
{
    this->auto_reslice_pending = false;

    AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr || !cfg->get_bool("auto_slice_after_change"))
        return;

    if (this->model.objects.empty())
        return;

    if (this->background_process.running() || this->m_is_slicing)
        return;

    PartPlate* plate = this->partplate_list.get_curr_plate();
    if (plate == nullptr || !plate->has_printable_instances())
        return;

    this->q->reslice();
}

int Plater::priv::auto_slice_delay_seconds() const
{
    AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return 0;

    std::string delay_str = cfg->get("auto_slice_change_delay_seconds");
    if (delay_str.empty())
        return 0;

    long delay_seconds = 0;
    try {
        delay_seconds = std::stol(delay_str);
    } catch (...) {
        delay_seconds = 0;
    }

    if (delay_seconds < 0)
        delay_seconds = 0;

    const long max_seconds = std::numeric_limits<int>::max() / 1000;
    if (delay_seconds > max_seconds)
        delay_seconds = max_seconds;

    return static_cast<int>(delay_seconds);
}

std::vector<std::vector<DynamicPrintConfig>> Plater::priv::get_extruder_filament_info()
{
    std::vector<std::vector<DynamicPrintConfig>> filament_infos;
    DeviceManager *dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev)
        return filament_infos;

    MachineObject *obj_ = dev->get_selected_machine();
    if (obj_ == nullptr)
        return filament_infos;

    if (!obj_->is_multi_extruders())
        return filament_infos;

    filament_infos = wxGetApp().preset_bundle->get_extruder_filament_info();
    return filament_infos;
}

void Plater::priv::update_print_volume_state()
{
    //BBS: use the plate's bounding box instead of the bed's
    PartPlate* pp = partplate_list.get_curr_plate();
    BuildVolume build_volume(pp->get_shape(), this->bed.build_volume().printable_height(), this->bed.build_volume().extruder_areas(), this->bed.build_volume().extruder_heights());
    this->model.update_print_volume_state(build_volume);
}

void Plater::priv::process_validation_warning(StringObjectException const &warning) const
{
    if (warning.string.empty())
        notification_manager->close_notification_of_type(NotificationType::ValidateWarning);
    else {
        std::string text = warning.string;
        auto po = dynamic_cast<PrintObjectBase const *>(warning.object);
        auto mo = po ? po->model_object() : dynamic_cast<ModelObject const *>(warning.object);
        auto action_fn = (mo || !warning.opt_key.empty()) ? [id = mo ? mo->id() : 0, opt = warning.opt_key](wxEvtHandler *) {
		    auto & objects = wxGetApp().model().objects;
		    auto iter = id.id ? std::find_if(objects.begin(), objects.end(), [id](auto o) { return o->id() == id; }) : objects.end();
            if (iter != objects.end()) {
                wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);
			    wxGetApp().obj_list()->select_items({{*iter, nullptr}});
            }
            if (!opt.empty()) {
                if (iter != objects.end())
				    wxGetApp().params_panel()->switch_to_object();
                wxGetApp().sidebar().jump_to_option(opt, Preset::TYPE_PRINT, L"");
		    }
		    return false;
	    } : std::function<bool(wxEvtHandler *)>();
        auto hypertext = (mo || !warning.opt_key.empty()) ? _u8L("Jump to") : "";
        if (mo) hypertext += std::string(" [") + mo->name + "]";
        if (!warning.opt_key.empty()) hypertext += std::string(" (") + warning.opt_key + ")";

        // BBS disable support enforcer
        //if (text == "_SUPPORTS_OFF") {
        //    text = _u8L("An object has custom support enforcers which will not be used "
        //                "because supports are disabled.")+"\n";
        //    hypertext = _u8L("Enable supports for enforcers only");

        //    action_fn = [](wxEvtHandler*) {
        //        Tab* print_tab = wxGetApp().get_tab(Preset::TYPE_PRINT);
        //        assert(print_tab);
        //        DynamicPrintConfig& config = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        //        config.set_key_value("enable_support", new ConfigOptionBool(true));
        //        config.set_key_value("auto_support_type", new ConfigOptionEnum<SupportType>(stNormalAuto));
        //        print_tab->on_value_change("enable_support", config.opt_bool("enable_support"));
        //        print_tab->on_value_change("support_material_auto", config.opt_bool("support_material_auto"));
        //        return true;
        //    };
        //}

        notification_manager->push_notification(
            NotificationType::ValidateWarning,
            NotificationManager::NotificationLevel::WarningNotificationLevel,
            _u8L("WARNING:") + "\n" + text, hypertext, action_fn
        );
    }
}


// Update background processing thread from the current config and Model.
// Returns a bitmask of UpdateBackgroundProcessReturnState.
unsigned int Plater::priv::update_background_process(bool force_validation, bool postpone_error_messages, bool switch_print)
{
    // bitmap of enum UpdateBackgroundProcessReturnState
    unsigned int return_state = 0;

    // If the update_background_process() was not called by the timer, kill the timer,
    // so the update_restart_background_process() will not be called again in vain.
    background_process_timer.Stop();
    // Update the "out of print bed" state of ModelInstances.
    update_print_volume_state();
    // Apply new config to the possibly running background task.
    bool               was_running = background_process.running();
    //BBS: add the switch print logic before Print::Apply
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": enter, force_validation=%1% postpone_error_messages=%2%, switch_print=%3%, was_running=%4%")%force_validation %postpone_error_messages %switch_print %was_running;
    if (switch_print)
    {
        //BBS: update the current print to the current plate
        this->partplate_list.update_slice_context_to_current_plate(background_process);
        this->preview->update_gcode_result(partplate_list.get_current_slice_result());
    }

    background_process.fff_print()->set_check_multi_filaments_compatibility(wxGetApp().app_config->get("enable_high_low_temp_mixed_printing") == "false");

    Print::ApplyStatus invalidated;
    const auto& preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle->get_printer_extruder_count() > 1) {
        PartPlate* cur_plate = background_process.get_current_plate();
        std::vector<int> f_maps = cur_plate->get_real_filament_maps(preset_bundle->project_config);
        invalidated = background_process.apply(this->model, preset_bundle->full_config(false, f_maps));
        background_process.fff_print()->set_extruder_filament_info(get_extruder_filament_info());
    }
    else
        invalidated = background_process.apply(this->model, preset_bundle->full_config(false));

    if ((invalidated == Print::APPLY_STATUS_CHANGED) || (invalidated == Print::APPLY_STATUS_INVALIDATED))
        // BBS: add only gcode mode
        q->set_only_gcode(false);

    //BBS: add slicing related logs
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": background process apply result=%1%")%invalidated;
    if (background_process.empty())
        view3D->get_canvas3d()->reset_sequential_print_clearance();

    if (invalidated == Print::APPLY_STATUS_INVALIDATED) {
        //BBS: update current plater's slicer result to invalid
        this->background_process.get_current_plate()->update_slice_result_valid_state(false);

        //no need, should be done in background_process.apply
        //this->background_process.get_current_gcode_result()->reset();
        // Reset preview canvases. If the print has been invalidated, the preview canvases will be cleared.
        // Otherwise they will be just refreshed.
        if (preview != nullptr) {
            // If the preview is not visible, the following line just invalidates the preview,
            // but the G-code paths or SLA preview are calculated first once the preview is made visible.
            reset_gcode_toolpaths();
            preview->reload_print();
        }
        // In FDM mode, we need to reload the 3D scene because of the wipe tower preview box.
        // In SLA mode, we need to reload the 3D scene every time to show the support structures.
        if (printer_technology == ptSLA || (printer_technology == ptFFF && config->opt_bool("enable_prime_tower")))
            return_state |= UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE;

        notification_manager->set_slicing_progress_hidden();
    }
    else {
        if (preview && preview->get_reload_paint_after_background_process_apply()) {
            preview->set_reload_paint_after_background_process_apply(false);
            preview->reload_print();
        }
    }

    if ((invalidated != Print::APPLY_STATUS_UNCHANGED || force_validation) && ! background_process.empty()) {
        // The delayed error message is no more valid.
        delayed_error_message.clear();
        // The state of the Print changed, and it is non-zero. Let's validate it and give the user feedback on errors.

        //BBS: add is_warning logic
        StringObjectException warning;
        //BBS: refine seq-print logic
        Polygons polygons;
        std::vector<std::pair<Polygon, float>> height_polygons;
        StringObjectException err = background_process.validate(&warning, &polygons, &height_polygons);
        // update string by type
        q->post_process_string_object_exception(err);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": validate err=%1%, warning=%2%")%err.string%warning.string;

        if (err.string.empty()) {
            this->partplate_list.get_curr_plate()->update_apply_result_invalid(false);
            notification_manager->set_all_slicing_errors_gray(true);
            notification_manager->close_notification_of_type(NotificationType::ValidateError);
            notification_manager->bbl_close_3mf_warn_notification();

            if (invalidated != Print::APPLY_STATUS_UNCHANGED && background_processing_enabled())
                return_state |= UPDATE_BACKGROUND_PROCESS_RESTART;

            // Pass a warning from validation and either show a notification,
            // or hide the old one.
            process_validation_warning(warning);
            if (printer_technology == ptFFF) {
                view3D->get_canvas3d()->reset_sequential_print_clearance();
                view3D->get_canvas3d()->set_as_dirty();
                view3D->get_canvas3d()->request_extra_frame();
            }
        }
        else {
            this->partplate_list.get_curr_plate()->update_apply_result_invalid(true);
            // The print is not valid.
            // Show error as notification.
            notification_manager->push_validate_error_notification(err);
            //also update the warnings
            process_validation_warning(warning);
            return_state |= UPDATE_BACKGROUND_PROCESS_INVALID;
            if (printer_technology == ptFFF) {
                const Print* print = background_process.fff_print();
                //Polygons polygons;
                //if (print->config().print_sequence == PrintSequence::ByObject)
                //    Print::sequential_print_clearance_valid(*print, &polygons);
                view3D->get_canvas3d()->set_sequential_print_clearance_visible(true);
                view3D->get_canvas3d()->set_sequential_print_clearance_render_fill(true);
                view3D->get_canvas3d()->set_sequential_print_clearance_polygons(polygons, height_polygons);
            }
        }
    }
    else if (! this->delayed_error_message.empty()) {
        // Reusing the old state.
        return_state |= UPDATE_BACKGROUND_PROCESS_INVALID;
    }

    //actualizate warnings
    if (invalidated != Print::APPLY_STATUS_UNCHANGED || background_process.empty()) {
        if (background_process.empty())
            process_validation_warning({});
        actualize_slicing_warnings(*this->background_process.current_print());
        actualize_object_warnings(*this->background_process.current_print());
        show_warning_dialog = false;
        process_completed_with_error = -1;
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: was_running = %2%, running %3%, invalidated=%4%, return_state=%5%, internal_cancel=%6%")
        % __LINE__ % was_running % this->background_process.running() % invalidated % return_state % this->background_process.is_internal_cancelled();
    if (was_running && ! this->background_process.running() && (return_state & UPDATE_BACKGROUND_PROCESS_RESTART) == 0) {
        if (invalidated != Print::APPLY_STATUS_UNCHANGED || this->background_process.is_internal_cancelled())
        {
            // The background processing was killed and it will not be restarted.
            // Post the "canceled" callback message, so that it will be processed after any possible pending status bar update messages.
            SlicingProcessCompletedEvent evt(EVT_PROCESS_COMPLETED, 0,
                SlicingProcessCompletedEvent::Cancelled, nullptr);
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" %1%, post an EVT_PROCESS_COMPLETED to main, status %2%")%__LINE__ %evt.status();
            wxQueueEvent(q, evt.Clone());
        }
    }

    if ((return_state & UPDATE_BACKGROUND_PROCESS_INVALID) != 0)
    {
        // Validation of the background data failed.
        //BBS: add slice&&print status update logic
        this->main_frame->update_slice_print_status(MainFrame::eEventSliceUpdate, false);

        process_completed_with_error = partplate_list.get_curr_plate_index();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: set to process_completed_with_error, return_state=%2%")%__LINE__%return_state;
    }
    else
    {
        // Background data is valid.
        if ((return_state & UPDATE_BACKGROUND_PROCESS_RESTART) != 0 ||
            (return_state & UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE) != 0 )
            notification_manager->set_slicing_progress_hidden();

        //BBS: add slice&&print status update logic
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: background data valid, return_state=%2%")%__LINE__%return_state;
        PartPlate* cur_plate = background_process.get_current_plate();
        if (background_process.finished() && cur_plate && cur_plate->is_slice_result_valid())
        {
            //ready_to_slice = false;
            this->main_frame->update_slice_print_status(MainFrame::eEventSliceUpdate, false);
        }
        else if (!background_process.empty() &&
                 !background_process.running()) /* Do not update buttons if background process is running
                                                 * This condition is important for SLA mode especially,
                                                 * when this function is called several times during calculations
                                                 * */
        {
            if (cur_plate->can_slice()) {
                //ready_to_slice = true;
                this->main_frame->update_slice_print_status(MainFrame::eEventSliceUpdate, true);
                process_completed_with_error = -1;
            }
            else {
                //ready_to_slice = false;
                this->main_frame->update_slice_print_status(MainFrame::eEventSliceUpdate, false);
                process_completed_with_error = partplate_list.get_curr_plate_index();
            }
        }
#if 0
        //sidebar->set_btn_label(ActionButtonType::abExport, _(label_btn_export));
        //sidebar->set_btn_label(ActionButtonType::abSendGCode, _(label_btn_send));

        //const wxString slice_string = background_process.running() && wxGetApp().get_mode() == comSimple ?
        //                              _L("Slicing") + dots : _L("Slice now");
        //sidebar->set_btn_label(ActionButtonType::abReslice, slice_string);

        //if (background_process.finished())
        //    show_action_buttons(false);
        //else if (!background_process.empty() &&
        //         !background_process.running()) /* Do not update buttons if background process is running
        //                                         * This condition is important for SLA mode especially,
        //                                         * when this function is called several times during calculations
        //                                         * */
        //    show_action_buttons(true);
#endif
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: exit, return_state=%2%")%__LINE__%return_state;
    return return_state;
}

// Restart background processing thread based on a bitmask of UpdateBackgroundProcessReturnState.
bool Plater::priv::restart_background_process(unsigned int state)
{
    if (!m_worker.is_idle()) {
        // Avoid a race condition
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", Line %1%: ui jobs running, return false")%__LINE__;
        return false;
    }

    if ( ! this->background_process.empty() &&
         (state & priv::UPDATE_BACKGROUND_PROCESS_INVALID) == 0 &&
         ( ((state & UPDATE_BACKGROUND_PROCESS_FORCE_RESTART) != 0 && ! this->background_process.finished()) ||
           (state & UPDATE_BACKGROUND_PROCESS_FORCE_EXPORT) != 0 ||
           (state & UPDATE_BACKGROUND_PROCESS_RESTART) != 0 ) ) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: print is valid, try to start it now")%__LINE__;
        // The print is valid and it can be started.
        if (this->background_process.start()) {
            if (!show_warning_dialog)
                on_slicing_began();
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: start successfully")%__LINE__;
            return true;
        }
    }
    else if (this->background_process.empty()) {
        PartPlate* cur_plate = background_process.get_current_plate();
        if (cur_plate->is_slice_result_valid() && ((state & UPDATE_BACKGROUND_PROCESS_FORCE_RESTART) != 0)) {
            if (this->background_process.start()) {
                if (!show_warning_dialog)
                    on_slicing_began();
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: start successfully")%__LINE__;
                return true;
            }
        }
    }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: not started")%__LINE__;
    return false;
}

void Plater::priv::export_gcode(fs::path output_path, bool output_path_on_removable_media)
{
    wxCHECK_RET(!(output_path.empty()), "export_gcode: output_path and upload_job empty");

    BOOST_LOG_TRIVIAL(trace) << boost::format("export_gcode: output_path %1%")%output_path.string();
    if (model.objects.empty())
        return;

    if (background_process.is_export_scheduled()) {
        GUI::show_error(q, _L("Another export job is running."));
        return;
    }

    // bitmask of UpdateBackgroundProcessReturnState
    unsigned int state = update_background_process(true);
    if (state & priv::UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE)
        view3D->reload_scene(false);

    if ((state & priv::UPDATE_BACKGROUND_PROCESS_INVALID) != 0)
        return;

    show_warning_dialog = true;
    if (! output_path.empty()) {
        background_process.schedule_export(output_path.string(), output_path_on_removable_media);
        notification_manager->push_delayed_notification(NotificationType::ExportOngoing, []() {return true; }, 1000, 0);
    } else {
        BOOST_LOG_TRIVIAL(info) << "output_path  is empty";
    }

    // If the SLA processing of just a single object's supports is running, restart slicing for the whole object.
    this->background_process.set_task(PrintBase::TaskParams());
    this->restart_background_process(priv::UPDATE_BACKGROUND_PROCESS_FORCE_EXPORT);
}
void Plater::priv::export_gcode(fs::path output_path, bool output_path_on_removable_media, PrintHostJob upload_job)
{
    wxCHECK_RET(!(output_path.empty() && upload_job.empty()), "export_gcode: output_path and upload_job empty");

    if (model.objects.empty())
        return;

    if (background_process.is_export_scheduled()) {
        GUI::show_error(q, _L("Another export job is running."));
        return;
    }

    // bitmask of UpdateBackgroundProcessReturnState
    unsigned int state = update_background_process(true);
    if (state & priv::UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE)
        view3D->reload_scene(false);

    if ((state & priv::UPDATE_BACKGROUND_PROCESS_INVALID) != 0)
        return;

    show_warning_dialog = true;
    if (! output_path.empty()) {
        background_process.schedule_export(output_path.string(), output_path_on_removable_media);
        notification_manager->push_delayed_notification(NotificationType::ExportOngoing, []() {return true; }, 1000, 0);
    } else {
        background_process.schedule_upload(std::move(upload_job));
    }

    // If the SLA processing of just a single object's supports is running, restart slicing for the whole object.
    this->background_process.set_task(PrintBase::TaskParams());
    this->restart_background_process(priv::UPDATE_BACKGROUND_PROCESS_FORCE_EXPORT);
}
unsigned int Plater::priv::update_restart_background_process(bool force_update_scene, bool force_update_preview)
{
    bool switch_print = true;
    //BBS: judge whether can switch print or not
    if ((partplate_list.get_plate_count() > 1) && !this->background_process.can_switch_print())
    {
        //can not switch print currently
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": plate count %1%, can not switch") % partplate_list.get_plate_count();
        switch_print = false;
    }
    // bitmask of UpdateBackgroundProcessReturnState
    unsigned int state = this->update_background_process(false, false, switch_print);
    if (force_update_scene || (state & UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE) != 0)
        view3D->reload_scene(false);

    if (force_update_preview)
        this->preview->reload_print();
    this->restart_background_process(state);
    return state;
}

void Plater::priv::update_fff_scene()
{
    if (this->preview != nullptr)
        this->preview->reload_print();
    // In case this was MM print, wipe tower bounding box on 3D tab might need redrawing with exact depth:
    view3D->reload_scene(true);
    //BBS: add assemble view related logic
    assemble_view->reload_scene(true);
}

//BBS: add print project related logic
void Plater::priv::update_fff_scene_only_shells(bool only_shells)
{
    if (this->preview != nullptr)
    {
        const Print* current_print = this->background_process.fff_print();
        if (current_print)
        {
            //this->preview->reset_shells();
            this->preview->load_shells(*current_print);
        }
    }

    if (!only_shells) {
        view3D->reload_scene(true);
        assemble_view->reload_scene(true);
    }
}

void Plater::priv::update_sla_scene()
{
    // Update the SLAPrint from the current Model, so that the reload_scene()
    // pulls the correct data.
    delayed_scene_refresh = false;
    this->update_restart_background_process(true, true);
}

bool Plater::priv::replace_volume_with_stl(int object_idx, int volume_idx, const fs::path& new_path, const std::string& snapshot)
{
    const std::string path = new_path.string();
    wxBusyCursor wait;

    Model new_model;
    try {
        const bool is_step = boost::algorithm::iends_with(path, ".stp") || boost::algorithm::iends_with(path, ".step");
        if (is_step) {
            auto config = wxGetApp().app_config;
            double linear = std::max(0.003, string_to_double_decimal_point(config->get("linear_defletion")));
            double angle = std::max(0.5, string_to_double_decimal_point(config->get("angle_defletion")));
            bool split_compound = config->get_bool("is_split_compound");
            bool is_user_cancel = false;

            auto callback = [&is_user_cancel, linear, angle, split_compound](Slic3r::Step &file, double &linear_value, double &angle_value, bool &is_split) -> int {
                if (wxGetApp().app_config->get_bool("enable_step_mesh_setting")) {
                    StepMeshDialog mesh_dlg(nullptr, file, linear, angle);
                    if (mesh_dlg.ShowModal() == wxID_OK) {
                        linear_value = mesh_dlg.get_linear_defletion();
                        angle_value  = mesh_dlg.get_angle_defletion();
                        is_split     = mesh_dlg.get_split_compound_value();
                        return 1;
                    }
                } else {
                    linear_value = linear;
                    angle_value  = angle;
                    is_split     = split_compound;
                    return 1;
                }
                is_user_cancel = true;
                return -1;
            };

            new_model = Model::read_from_step(path, LoadStrategy::AddDefaultInstances | LoadStrategy::LoadModel, nullptr, nullptr, callback, linear, angle, split_compound);
            if (is_user_cancel) return false;
        } else {
            new_model = Model::read_from_file(path, nullptr, nullptr, LoadStrategy::AddDefaultInstances | LoadStrategy::LoadModel);
        }
        for (ModelObject* model_object : new_model.objects) {
            model_object->center_around_origin();
            model_object->ensure_on_bed();
        }
    }
    catch (std::exception&) {
        // error while loading
        return false;
    }

    if (new_model.objects.size() > 1 || new_model.objects.front()->volumes.size() > 1) {
        MessageDialog dlg(q, _L("Unable to replace with more than one volume"), _L("Error during replace"), wxOK | wxOK_DEFAULT | wxICON_WARNING);
        dlg.ShowModal();
        return false;
    }

    wxBusyInfo info(_L("Replace from:") + " " + from_u8(path), q->get_current_canvas3D()->get_wxglcanvas());

    if (!snapshot.empty())
        q->take_snapshot(snapshot);

    ModelObject* old_model_object = model.objects[object_idx];
    ModelVolume* old_volume = old_model_object->volumes[volume_idx];

    bool sinking = old_model_object->min_z() < SINKING_Z_THRESHOLD;

    ModelObject* new_model_object = new_model.objects.front();
    old_model_object->add_volume(*new_model_object->volumes.front());
    ModelVolume* new_volume = old_model_object->volumes.back();
    new_volume->set_new_unique_id();
    new_volume->config.apply(old_volume->config);
    new_volume->set_type(old_volume->type());
    new_volume->set_material_id(old_volume->material_id());
    new_volume->set_transformation(old_volume->get_transformation());
    new_volume->translate(new_volume->get_transformation().get_matrix_no_offset() * (new_volume->source.mesh_offset - old_volume->source.mesh_offset));
    assert(!old_volume->source.is_converted_from_inches || !old_volume->source.is_converted_from_meters);
    if (old_volume->source.is_converted_from_inches)
        new_volume->convert_from_imperial_units();
    else if (old_volume->source.is_converted_from_meters)
        new_volume->convert_from_meters();
    new_volume->supported_facets.assign(old_volume->supported_facets);
    new_volume->seam_facets.assign(old_volume->seam_facets);
    new_volume->mmu_segmentation_facets.assign(old_volume->mmu_segmentation_facets);
    new_volume->fuzzy_skin_facets.assign(old_volume->fuzzy_skin_facets);
    std::swap(old_model_object->volumes[volume_idx], old_model_object->volumes.back());
    old_model_object->delete_volume(old_model_object->volumes.size() - 1);
    if (!sinking)
        old_model_object->ensure_on_bed();
    old_model_object->sort_volumes(true);

    // if object has just one volume, rename object too
    if (old_model_object->volumes.size() == 1)
        old_model_object->name = old_model_object->volumes.front()->name;

    // update new name in ObjectList
    sidebar->obj_list()->update_name_in_list(object_idx, volume_idx);

    sla::reproject_points_and_holes(old_model_object);

    return true;
}

void Plater::priv::replace_with_stl()
{
    if (! q->get_view3D_canvas3D()->get_gizmos_manager().check_gizmos_closed_except(GLGizmosManager::EType::Undefined))
        return;

    const Selection& selection = get_selection();

    if (selection.is_wipe_tower() || get_selection().get_volume_idxs().size() != 1)
        return;

    const GLVolume* v = selection.get_first_volume();
    int object_idx = v->object_idx();
    int volume_idx = v->volume_idx();

    // collects paths of files to load

    const ModelObject* object = model.objects[object_idx];
    const ModelVolume* volume = object->volumes[volume_idx];

    fs::path input_path;
    if (!volume->source.input_file.empty() && fs::exists(volume->source.input_file))
        input_path = volume->source.input_file;

    wxString title = _L("Select a new file");
    title += ":";
    wxFileDialog dialog(q, title, "", from_u8(input_path.filename().string()), file_wildcards(FT_MODEL), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK)
        return;

    fs::path out_path = dialog.GetPath().ToUTF8().data();
    if (out_path.empty()) {
        MessageDialog dlg(q, _L("File for the replace wasn't selected"), _L("Error during replace"), wxOK | wxOK_DEFAULT | wxICON_WARNING);
        dlg.ShowModal();
        return;
    }

    if (!replace_volume_with_stl(object_idx, volume_idx, out_path, "Replace with 3D file"))
        return;

    // update 3D scene
    update();

    // new GLVolumes have been created at this point, so update their printable state
    for (size_t i = 0; i < model.objects.size(); ++i) {
        view3D->get_canvas3d()->update_instance_printable_state_for_object(i);
    }
}

void Plater::priv::replace_all_with_stl()
{
    if (! q->get_view3D_canvas3D()->get_gizmos_manager().check_gizmos_closed_except(GLGizmosManager::EType::Undefined))
        return;

    const Selection& selection = get_selection();

    if (selection.is_wipe_tower())
        return;

    fs::path input_path;
    Selection::IndicesList volume_idxs = selection.get_volume_idxs();

    // when plates are selected instead of volumes
    // then selection is inaccurate, we need to
    // find volumes contained in selected plates

    if (selection.is_empty() || volume_idxs.empty()) {
        std::vector<int> selected_plate_idxs;

        wxDataViewItemArray sels;
        wxGetApp().obj_list()->GetSelections(sels);
        for (const wxDataViewItem& item : sels) {
            Slic3r::GUI::ItemType item_type = wxGetApp().obj_list()->GetModel()->GetItemType(item);
            if (item_type & itPlate) {
                if (item.IsOk()) {
                    ObjectDataViewModelNode *node = static_cast<ObjectDataViewModelNode *>(item.GetID());
                    selected_plate_idxs.push_back(node->GetPlateIdx());
                }
            }
        }
        PartPlateList& plate_list = wxGetApp().plater()->get_partplate_list();
        for (int obj_idx = 0; obj_idx < selection.get_model()->objects.size(); obj_idx++) {
            for (int plate_idx : selected_plate_idxs) {
                PartPlate* plate = plate_list.get_plate(plate_idx);
                if (plate && plate->contain_instance_totally(obj_idx, 0)) {
                    std::vector<unsigned int> indices = selection.get_volume_idxs_from_object(obj_idx);
                    volume_idxs.insert(indices.begin(), indices.end());
                }
            }
        }
    }

    // find path for initializing the file selection dialog

    for (unsigned int idx : volume_idxs) {
        const GLVolume* v = selection.get_volume(idx);
        int object_idx = v->object_idx();
        int volume_idx = v->volume_idx();

        const ModelObject* object = model.objects[object_idx];
        const ModelVolume* volume = object->volumes[volume_idx];

        if (!volume->source.input_file.empty() && fs::exists(volume->source.input_file)) {
            input_path = volume->source.input_file;
            break;
        }
    }

    wxString title = _L("Select folder to replace from");
    title += ":";
    wxDirDialog dialog(q, title, from_u8(input_path.parent_path().string()), wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK)
        return;

    fs::path out_path = dialog.GetPath().ToUTF8().data();
    if (out_path.empty()) {
        MessageDialog dlg(q, _L("Directory for the replace wasn't selected"), _L("Error during replace"), wxOK | wxOK_DEFAULT | wxICON_WARNING);
        dlg.ShowModal();
        return;
    }

    std::string status = _L("Replaced with 3D files from directory:\n").ToStdString() + out_path.string() + "\n\n";

    for (unsigned int idx : volume_idxs) {
        const GLVolume* v = selection.get_volume(idx);
        int object_idx = v->object_idx();
        int volume_idx = v->volume_idx();

        const ModelObject* object = model.objects[object_idx];
        const ModelVolume* volume = object->volumes[volume_idx];

        if (volume->source.input_file.empty())
            continue;

        input_path = volume->source.input_file;

        fs::path new_path = out_path / input_path.filename();

        std::string volume_name = volume->name;

        if (new_path == input_path) {
            status += boost::str(boost::format(_L("✖ Skipped %1%: same file.\n").ToStdString()) % volume_name);
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " skipping replace volume : same filename " << new_path;
            continue;
        }

        if (!fs::exists(new_path)) {
            status += boost::str(boost::format(_L("✖ Skipped %1%: file does not exist.\n").ToStdString()) % volume_name);
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " cannot replace volume : filen does not exist " << new_path;
            continue;
        }

        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " replacing volume : " << input_path << " with " << new_path;

        if (!replace_volume_with_stl(object_idx, volume_idx, new_path, "Replace with 3D file")) {
            status += boost::str(boost::format(_L("✖ Skipped %1%: failed to replace.\n").ToStdString()) % volume_name);
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " cannot replace volume : failed to replace with " << new_path;
            continue;
        }

        status += boost::str(boost::format(_L("✔ Replaced %1%.\n").ToStdString()) % volume_name);
    }

    // update 3D scene
    update();

    // new GLVolumes have been created at this point, so update their printable state
    for (size_t i = 0; i < model.objects.size(); ++i) {
        view3D->get_canvas3d()->update_instance_printable_state_for_object(i);
    }

    MessageDialog dlg(q, status, _L("Replaced volumes"), wxOK | wxOK_DEFAULT | wxICON_INFORMATION);
    dlg.ShowModal();
}

#if ENABLE_RELOAD_FROM_DISK_REWORK
static std::vector<std::pair<int, int>> reloadable_volumes(const Model &model, const Selection &selection)
{
    std::vector<std::pair<int, int>> ret;
    const std::set<unsigned int> &   selected_volumes_idxs = selection.get_volume_idxs();
    for (unsigned int idx : selected_volumes_idxs) {
        const GLVolume &v     = *selection.get_volume(idx);
        const int       o_idx = v.object_idx();
        if (0 <= o_idx && o_idx < int(model.objects.size())) {
            const ModelObject *obj   = model.objects[o_idx];
            const int          v_idx = v.volume_idx();
            if (0 <= v_idx && v_idx < int(obj->volumes.size())) {
                const ModelVolume *vol = obj->volumes[v_idx];
                if (!vol->source.is_from_builtin_objects && !vol->source.input_file.empty() && !fs::path(vol->source.input_file).extension().string().empty())
                    ret.push_back({o_idx, v_idx});
            }
        }
    }
    return ret;
}
#endif // ENABLE_RELOAD_FROM_DISK_REWORK

void Plater::priv::reload_from_disk()
{
#if ENABLE_RELOAD_FROM_DISK_REWORK
    // collect selected reloadable ModelVolumes
    std::vector<std::pair<int, int>> selected_volumes = reloadable_volumes(model, get_selection());
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " entry, and reloadable volumes number is: " << selected_volumes.size();
    // nothing to reload, return
    if (selected_volumes.empty())
        return;

    std::sort(selected_volumes.begin(), selected_volumes.end(), [](const std::pair<int, int> &v1, const std::pair<int, int> &v2) {
        return (v1.first < v2.first) || (v1.first == v2.first && v1.second < v2.second);
        });
    selected_volumes.erase(std::unique(selected_volumes.begin(), selected_volumes.end(), [](const std::pair<int, int> &v1, const std::pair<int, int> &v2) {
        return (v1.first == v2.first) && (v1.second == v2.second);
        }), selected_volumes.end());
#else
    Plater::TakeSnapshot snapshot(q, "Reload from disk");

    const Selection& selection = get_selection();

    if (selection.is_wipe_tower())
        return;

    // struct to hold selected ModelVolumes by their indices
    struct SelectedVolume
    {
        int object_idx;
        int volume_idx;

        // operators needed by std::algorithms
        bool operator < (const SelectedVolume& other) const { return object_idx < other.object_idx || (object_idx == other.object_idx && volume_idx < other.volume_idx); }
        bool operator == (const SelectedVolume& other) const { return object_idx == other.object_idx && volume_idx == other.volume_idx; }
    };
    std::vector<SelectedVolume> selected_volumes;

    // collects selected ModelVolumes
    const std::set<unsigned int>& selected_volumes_idxs = selection.get_volume_idxs();
    for (unsigned int idx : selected_volumes_idxs) {
        const GLVolume* v = selection.get_volume(idx);
        int v_idx = v->volume_idx();
        if (v_idx >= 0) {
            int o_idx = v->object_idx();
            if (0 <= o_idx && o_idx < (int)model.objects.size())
                selected_volumes.push_back({ o_idx, v_idx });
        }
    }
    std::sort(selected_volumes.begin(), selected_volumes.end());
    selected_volumes.erase(std::unique(selected_volumes.begin(), selected_volumes.end()), selected_volumes.end());
#endif // ENABLE_RELOAD_FROM_DISK_REWORK

    // collects paths of files to load
    std::vector<fs::path> input_paths;
    std::vector<fs::path> missing_input_paths;
#if ENABLE_RELOAD_FROM_DISK_REWORK
    std::vector<std::pair<fs::path, fs::path>> replace_paths;
    for (auto [obj_idx, vol_idx] : selected_volumes) {
        const ModelObject *object = model.objects[obj_idx];
        const ModelVolume *volume = object->volumes[vol_idx];
        if (fs::exists(volume->source.input_file))
            input_paths.push_back(volume->source.input_file);
        else {
            // searches the source in the same folder containing the object
            bool found = false;
            if (!object->input_file.empty()) {
                fs::path object_path = fs::path(object->input_file).remove_filename();
                if (!object_path.empty()) {
                    object_path /= fs::path(volume->source.input_file).filename();
                    if (fs::exists(object_path)) {
                        input_paths.push_back(object_path);
                        found = true;
                    }
                }
            }
            if (!found)
                missing_input_paths.push_back(volume->source.input_file);
        }
    }
#else
    std::vector<fs::path> replace_paths;
    for (const SelectedVolume& v : selected_volumes) {
        const ModelObject* object = model.objects[v.object_idx];
        const ModelVolume* volume = object->volumes[v.volume_idx];

        if (!volume->source.input_file.empty()) {
            if (fs::exists(volume->source.input_file))
                input_paths.push_back(volume->source.input_file);
            else {
                // searches the source in the same folder containing the object
                bool found = false;
                if (!object->input_file.empty()) {
                    fs::path object_path = fs::path(object->input_file).remove_filename();
                    if (!object_path.empty()) {
                        object_path /= fs::path(volume->source.input_file).filename();
                        const std::string source_input_file = object_path.string();
                        if (fs::exists(source_input_file)) {
                            input_paths.push_back(source_input_file);
                            found = true;
                        }
                    }
                }
                if (!found)
                    missing_input_paths.push_back(volume->source.input_file);
            }
        }
        else if (!object->input_file.empty() && volume->is_model_part() && !volume->name.empty() && !volume->source.is_from_builtin_objects)
            missing_input_paths.push_back(volume->name);
    }
#endif // ENABLE_RELOAD_FROM_DISK_REWORK

    std::sort(missing_input_paths.begin(), missing_input_paths.end());
    missing_input_paths.erase(std::unique(missing_input_paths.begin(), missing_input_paths.end()), missing_input_paths.end());

    while (!missing_input_paths.empty()) {
        // ask user to select the missing file
        fs::path search = missing_input_paths.back();
        wxString title = _L("Please select a file");
#if defined(__APPLE__)
        title += " (" + from_u8(search.filename().string()) + ")";
#endif // __APPLE__
        title += ":";
        wxFileDialog dialog(q, title, "", from_u8(search.filename().string()), file_wildcards(FT_MODEL), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK)
            return;

        std::string sel_filename_path = dialog.GetPath().ToUTF8().data();
        std::string sel_filename = fs::path(sel_filename_path).filename().string();
        if (boost::algorithm::iequals(search.filename().string(), sel_filename)) {
            input_paths.push_back(sel_filename_path);
            missing_input_paths.pop_back();

            fs::path sel_path = fs::path(sel_filename_path).remove_filename().string();

            std::vector<fs::path>::iterator it = missing_input_paths.begin();
            while (it != missing_input_paths.end()) {
                // try to use the path of the selected file with all remaining missing files
                fs::path repathed_filename = sel_path;
                repathed_filename /= it->filename();
                if (fs::exists(repathed_filename)) {
                    input_paths.push_back(repathed_filename.string());
                    it = missing_input_paths.erase(it);
                }
                else
                    ++it;
            }
        }
        else {
            wxString      message = _L("Do you want to replace it") + " ?";
            MessageDialog dlg(q, message, _L("Message"), wxYES_NO | wxYES_DEFAULT | wxICON_QUESTION);
            if (dlg.ShowModal() == wxID_YES)
#if ENABLE_RELOAD_FROM_DISK_REWORK
                replace_paths.emplace_back(search, sel_filename_path);
#else
                replace_paths.emplace_back(sel_filename_path);
#endif // ENABLE_RELOAD_FROM_DISK_REWORK
            missing_input_paths.pop_back();
        }
    }

    std::sort(input_paths.begin(), input_paths.end());
    input_paths.erase(std::unique(input_paths.begin(), input_paths.end()), input_paths.end());

    std::sort(replace_paths.begin(), replace_paths.end());
    replace_paths.erase(std::unique(replace_paths.begin(), replace_paths.end()), replace_paths.end());

#if ENABLE_RELOAD_FROM_DISK_REWORK
    Plater::TakeSnapshot snapshot(q, "Reload from disk");
#endif // ENABLE_RELOAD_FROM_DISK_REWORK

    std::vector<wxString> fail_list;

    // load one file at a time
    for (size_t i = 0; i < input_paths.size(); ++i) {
        const auto& path = input_paths[i].string();
        auto        obj_color_fun = [this, &path](ObjDialogInOut &in_out) {
            if (!boost::iends_with(path, ".obj")) { return; }
            const std::vector<std::string> extruder_colours = wxGetApp().plater()->get_extruder_colors_from_plater_config();
            ObjColorDialog                 color_dlg(nullptr, in_out, extruder_colours);
            if (color_dlg.ShowModal() != wxID_OK) {
                in_out.filament_ids.clear();
            }
        };
        wxBusyCursor wait;
        wxBusyInfo info(_L("Reload from:") + " " + from_u8(path), q->get_current_canvas3D()->get_wxglcanvas());

        Model new_model;
        try
        {
            //BBS: add plate data related logic
            PlateDataPtrs plate_data;
            //BBS: project embedded settings
            std::vector<Preset*> project_presets;

            // BBS: backup
            if (boost::iends_with(path, ".stp") ||
                boost::iends_with(path, ".step")) {
                double linear = string_to_double_decimal_point(wxGetApp().app_config->get("linear_defletion"));
                double angle = string_to_double_decimal_point(wxGetApp().app_config->get("angle_defletion"));
                bool   is_split = wxGetApp().app_config->get_bool("is_split_compound");
                new_model       = Model::read_from_step(path, LoadStrategy::AddDefaultInstances | LoadStrategy::LoadModel, nullptr, nullptr, nullptr, linear, angle, is_split);
            }else {
                new_model = Model::read_from_file(path, nullptr, nullptr, LoadStrategy::AddDefaultInstances | LoadStrategy::LoadModel, &plate_data, &project_presets, nullptr, nullptr, nullptr, nullptr, nullptr, 0, obj_color_fun);
            }


            for (ModelObject* model_object : new_model.objects)
            {
                model_object->center_around_origin();
                model_object->ensure_on_bed();
            }

            if (plate_data.size() > 0)
            {
                //partplate_list.load_from_3mf_structure(plate_data);
                partplate_list.update_slice_context_to_current_plate(background_process);
                this->preview->update_gcode_result(partplate_list.get_current_slice_result());
                release_PlateData_list(plate_data);
                sidebar->obj_list()->reload_all_plates();
            }
        }
        catch (std::exception&)
        {
            // error while loading
            return;
        }

#if ENABLE_RELOAD_FROM_DISK_REWORK
        for (auto [obj_idx, vol_idx] : selected_volumes) {
            ModelObject *old_model_object = model.objects[obj_idx];
            ModelVolume *old_volume       = old_model_object->volumes[vol_idx];

            bool sinking = old_model_object->min_z() < SINKING_Z_THRESHOLD;

            bool has_source = !old_volume->source.input_file.empty() &&
                              boost::algorithm::iequals(fs::path(old_volume->source.input_file).filename().string(), fs::path(path).filename().string());
            bool has_name = !old_volume->name.empty() && boost::algorithm::iequals(old_volume->name, fs::path(path).filename().string());
            if (has_source || has_name) {
                int  new_volume_idx = -1;
                int  new_object_idx = -1;
                bool match_found    = false;
                // take idxs from the matching volume
                if (has_source && old_volume->source.object_idx < int(new_model.objects.size())) {
                    const ModelObject *obj = new_model.objects[old_volume->source.object_idx];
                    if (old_volume->source.volume_idx < int(obj->volumes.size())) {
                        if (obj->volumes[old_volume->source.volume_idx]->source.input_file == old_volume->source.input_file) {
                            new_volume_idx = old_volume->source.volume_idx;
                            new_object_idx = old_volume->source.object_idx;
                            match_found    = true;
                        }
                    }
                }

                if (!match_found && has_name) {
                    // take idxs from the 1st matching volume
                    for (size_t o = 0; o < new_model.objects.size(); ++o) {
                        ModelObject *obj   = new_model.objects[o];
                        bool         found = false;
                        for (size_t v = 0; v < obj->volumes.size(); ++v) {
                            if (obj->volumes[v]->name == old_volume->name) {
                                new_volume_idx = (int) v;
                                new_object_idx = (int) o;
                                found          = true;
                                break;
                            }
                        }
                        if (found) break;
                        // BBS: step model,object loaded as a volume. GUI_ObfectList.cpp load_modifier()
                        if (obj->name == old_volume->name) {
                            new_object_idx = (int) o;
                            break;
                        }
                    }
                }

                if (new_object_idx < 0 || int(new_model.objects.size()) <= new_object_idx) {
                    fail_list.push_back(from_u8(has_source ? old_volume->source.input_file : old_volume->name));
                    continue;
                }
                ModelObject *new_model_object = new_model.objects[new_object_idx];
                if (int(new_model_object->volumes.size()) <= new_volume_idx) {
                    fail_list.push_back(from_u8(has_source ? old_volume->source.input_file : old_volume->name));
                    continue;
                }

                ModelVolume *new_volume = nullptr;
                // BBS: step model
                if (new_volume_idx < 0 && new_object_idx >= 0) {
                    TriangleMesh mesh = new_model_object->mesh();
                    new_volume = old_model_object->add_volume(std::move(mesh));
                    new_volume->name  = new_model_object->name;
                    new_volume->source.input_file = new_model_object->input_file;
                }else {
                    new_volume = old_model_object->add_volume(*new_model_object->volumes[new_volume_idx]);
                    // new_volume = old_model_object->volumes.back();
                }
                
                new_volume->set_new_unique_id();
                new_volume->config.apply(old_volume->config);
                new_volume->set_type(old_volume->type());
                new_volume->set_material_id(old_volume->material_id());

                new_volume->source.mesh_offset = old_volume->source.mesh_offset;
                new_volume->set_transformation(old_volume->get_transformation());

                new_volume->source.object_idx = old_volume->source.object_idx;
                new_volume->source.volume_idx = old_volume->source.volume_idx;
                assert(!old_volume->source.is_converted_from_inches || !old_volume->source.is_converted_from_meters);
                if (old_volume->source.is_converted_from_inches)
                    new_volume->convert_from_imperial_units();
                else if (old_volume->source.is_converted_from_meters)
                    new_volume->convert_from_meters();
                std::swap(old_model_object->volumes[vol_idx], old_model_object->volumes.back());
                old_model_object->delete_volume(old_model_object->volumes.size() - 1);
                if (!sinking) old_model_object->ensure_on_bed();
                old_model_object->sort_volumes(wxGetApp().app_config->get("order_volumes") == "1");

                sla::reproject_points_and_holes(old_model_object);

                // Fix warning icon in object list
                wxGetApp().obj_list()->update_item_error_icon(obj_idx, vol_idx);
            }
        }
#else
        // update the selected volumes whose source is the current file
        for (const SelectedVolume& sel_v : selected_volumes) {
            ModelObject* old_model_object = model.objects[sel_v.object_idx];
            ModelVolume* old_volume = old_model_object->volumes[sel_v.volume_idx];

            bool sinking = old_model_object->bounding_box().min.z() < SINKING_Z_THRESHOLD;

            bool has_source = !old_volume->source.input_file.empty() && boost::algorithm::iequals(fs::path(old_volume->source.input_file).filename().string(), fs::path(path).filename().string());
            bool has_name = !old_volume->name.empty() && boost::algorithm::iequals(old_volume->name, fs::path(path).filename().string());
            if (has_source || has_name) {
                int new_volume_idx = -1;
                int new_object_idx = -1;
//                if (has_source) {
//                    // take idxs from source
//                    new_volume_idx = old_volume->source.volume_idx;
//                    new_object_idx = old_volume->source.object_idx;
//                }
//                else {
                    // take idxs from the 1st matching volume
                    for (size_t o = 0; o < new_model.objects.size(); ++o) {
                        ModelObject* obj = new_model.objects[o];
                        bool found = false;
                        for (size_t v = 0; v < obj->volumes.size(); ++v) {
                            if (obj->volumes[v]->name == old_volume->name) {
                                new_volume_idx = (int)v;
                                new_object_idx = (int)o;
                                found = true;
                                break;
                            }
                        }
                        if (found)
                            break;
                    }
//                }

                if (new_object_idx < 0 || int(new_model.objects.size()) <= new_object_idx) {
                    fail_list.push_back(from_u8(has_source ? old_volume->source.input_file : old_volume->name));
                    continue;
                }
                ModelObject* new_model_object = new_model.objects[new_object_idx];
                if (new_volume_idx < 0 || int(new_model_object->volumes.size()) <= new_volume_idx) {
                    fail_list.push_back(from_u8(has_source ? old_volume->source.input_file : old_volume->name));
                    continue;
                }

                old_model_object->add_volume(*new_model_object->volumes[new_volume_idx]);
                ModelVolume* new_volume = old_model_object->volumes.back();
                new_volume->set_new_unique_id();
                new_volume->config.apply(old_volume->config);
                new_volume->set_type(old_volume->type());
                new_volume->set_material_id(old_volume->material_id());
                new_volume->set_transformation(old_volume->get_transformation());
                new_volume->translate(new_volume->get_transformation().get_matrix_no_offset() * (new_volume->source.mesh_offset - old_volume->source.mesh_offset));
                new_volume->source.object_idx = old_volume->source.object_idx;
                new_volume->source.volume_idx = old_volume->source.volume_idx;
                assert(! old_volume->source.is_converted_from_inches || ! old_volume->source.is_converted_from_meters);
                if (old_volume->source.is_converted_from_inches)
                    new_volume->convert_from_imperial_units();
                else if (old_volume->source.is_converted_from_meters)
                    new_volume->convert_from_meters();
                std::swap(old_model_object->volumes[sel_v.volume_idx], old_model_object->volumes.back());
                old_model_object->delete_volume(old_model_object->volumes.size() - 1);
                if (!sinking)
                    old_model_object->ensure_on_bed();
                old_model_object->sort_volumes(true);

                sla::reproject_points_and_holes(old_model_object);
            }
        }
#endif // ENABLE_RELOAD_FROM_DISK_REWORK
    }

#if ENABLE_RELOAD_FROM_DISK_REWORK
    for (auto [src, dest] : replace_paths) {
        for (auto [obj_idx, vol_idx] : selected_volumes) {
            if (boost::algorithm::iequals(model.objects[obj_idx]->volumes[vol_idx]->source.input_file, src.string()))
                // When an error occurs, either the dest parsing error occurs, or the number of objects in the dest is greater than 1 and cannot be replaced, and cannot be replaced in this loop.
                if (!replace_volume_with_stl(obj_idx, vol_idx, dest, "")) break;
        }
    }
#else
    for (size_t i = 0; i < replace_paths.size(); ++i) {
        const auto& path = replace_paths[i].string();
        for (const SelectedVolume& sel_v : selected_volumes) {
            ModelObject* old_model_object = model.objects[sel_v.object_idx];
            ModelVolume* old_volume = old_model_object->volumes[sel_v.volume_idx];
            bool has_source = !old_volume->source.input_file.empty() && boost::algorithm::iequals(fs::path(old_volume->source.input_file).filename().string(), fs::path(path).filename().string());
            if (!replace_volume_with_stl(sel_v.object_idx, sel_v.volume_idx, path, "")) {
                fail_list.push_back(from_u8(has_source ? old_volume->source.input_file : old_volume->name));
            }
        }
    }
#endif // ENABLE_RELOAD_FROM_DISK_REWORK

    if (!fail_list.empty()) {
        wxString message = _L("Unable to reload:") + "\n";
        for (const wxString& s : fail_list) {
            message += s + "\n";
        }
        MessageDialog dlg(q, message, _L("Error during reload"), wxOK | wxOK_DEFAULT | wxICON_WARNING);
        dlg.ShowModal();
    }

    // update 3D scene
    update();

    // new GLVolumes have been created at this point, so update their printable state
    for (size_t i = 0; i < model.objects.size(); ++i) {
        view3D->get_canvas3d()->update_instance_printable_state_for_object(i);
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " finish.";
}

void Plater::priv::reload_all_from_disk()
{
    if (model.objects.empty())
        return;

    Plater::TakeSnapshot snapshot(q, "Reload all");
    Plater::SuppressSnapshots suppress(q);

    Selection& selection = get_selection();
    Selection::IndicesList curr_idxs = selection.get_volume_idxs();
    // reload from disk uses selection
    select_all();
    reload_from_disk();
    // restore previous selection
    selection.clear();
    for (unsigned int idx : curr_idxs) {
        selection.add(idx, false);
    }
}

//BBS: add no_slice logic
void Plater::priv::set_current_panel(wxPanel* panel, bool no_slice)
{
    if (std::find(panels.begin(), panels.end(), panel) == panels.end())
        return;

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": current_panel %1%, new_panel %2%")%current_panel%panel;
#ifdef __WXMAC__
    bool force_render = (current_panel != nullptr);
#endif // __WXMAC__

    //BBS: add slice logic when switch to preview page
    auto do_reslice = [this, no_slice]() {
            // see: Plater::priv::object_list_changed()
            // FIXME: it may be better to have a single function making this check and let it be called wherever needed
            bool export_in_progress = this->background_process.is_export_scheduled();
            ObjectFilamentResults object_results;
            bool model_fits = this->view3D->get_canvas3d()->check_volumes_outside_state(&object_results) != ModelInstancePVS_Partly_Outside;
            model_fits = model_fits&&object_results.filaments.empty();
            //BBS: add partplate logic
            PartPlate * current_plate = this->partplate_list.get_curr_plate();
            bool only_has_gcode_need_preview = false;
            bool current_has_print_instances = current_plate->has_printable_instances();
            if (current_plate->is_slice_result_valid() && this->model.objects.empty() && !current_has_print_instances)
                only_has_gcode_need_preview = true;

            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": from set_current_panel, no_slice %1%, export_in_progress %2%, model_fits %3%, m_is_slicing %4%")%no_slice%export_in_progress%model_fits%m_is_slicing;

            if (!no_slice && !this->model.objects.empty() && !export_in_progress && model_fits && current_has_print_instances)
            {
                //if already running in background, not relice here
                //BBS: add more judge for slicing
                if (!this->background_process.running() && !this->m_is_slicing)
                {
                    this->m_slice_all = false;
                    this->q->reslice();
                }
                else {
                    //reset current plate to the slicing plate
                    int plate_index = this->background_process.get_current_plate()->get_index();
                    this->partplate_list.select_plate(plate_index);
                }
            }
            else if (only_has_gcode_need_preview)
            {
                this->m_slice_all = false;
                this->q->reslice();
            }
            //BBS: process empty plate, reset previous toolpath
            else
            {
                //if (!this->m_slice_all)
                if (!current_has_print_instances)
                    reset_gcode_toolpaths();
                //this->q->refresh_print();
                if (!preview->get_canvas3d()->is_initialized())
                {
                    preview->get_canvas3d()->render(true);
                }
            }
            //TODO: turn off this switch currently
            /*auto canvas_w = float(preview->get_canvas3d()->get_canvas_size().get_width());
            auto canvas_h = float(preview->get_canvas3d()->get_canvas_size().get_height());
            Point screen_center(canvas_w/2, canvas_h/2);
            auto center_point = preview->get_canvas3d()->_mouse_to_3d(screen_center);
            center_point(2) = 0.f;
            if (!current_plate->contains(center_point))
                this->partplate_list.select_plate_view();*/

            // keeps current gcode preview, if any
            if (this->m_slice_all) {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": slicing all, just reload shells");
                this->update_fff_scene_only_shells();
            }
            else {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": single slice, reload print");
                if (model_fits)
                    this->preview->reload_print(true);
                else
                    this->update_fff_scene_only_shells();
            }

            preview->set_as_dirty();
        };

    // Add sidebar and toolbar collapse logic
    if (panel == view3D || panel == preview) {
        this->enable_sidebar(!q->only_gcode_mode());
    }
    if (panel == preview) {
        if (q->only_gcode_mode()) {
            preview->get_canvas3d()->enable_select_plate_toolbar(false);
        } else if (q->using_exported_file() && (q->m_valid_plates_count <= 1)) {
            preview->get_canvas3d()->enable_select_plate_toolbar(false);
        } else {
            preview->get_canvas3d()->enable_select_plate_toolbar(true);
        }
    }
    else {
        preview->get_canvas3d()->enable_select_plate_toolbar(false);
    }

    if (current_panel == panel)
    {
        //BBS: add slice logic when switch to preview page
        //BBS: add only gcode mode
        if (!q->only_gcode_mode() && (current_panel == preview) && (wxGetApp().is_editor())) {
            do_reslice();
        }
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": the same panel, exit");
        return;
    }

    //BBS: wish to reset all plates stats item selected state when back to View3D Tab
    preview->get_canvas3d()->reset_select_plate_toolbar_selection();

    wxPanel* old_panel = current_panel;
//#if BBL_HAS_FIRST_PAGE
    if (!old_panel) {
        //BBS: only switch to the first panel when visible
        panel->Show();
        //dynamic_cast<View3D *>(panel)->get_canvas3d()->render();
        if (!panel->IsShownOnScreen())
            return;
    }
//#endif
    current_panel = panel;

    // to reduce flickering when changing view, first set as visible the new current panel
    for (wxPanel* p : panels) {
        if (p == current_panel) {
#ifdef __WXMAC__
            // On Mac we need also to force a render to avoid flickering when changing view
            if (force_render) {
                if (p == view3D)
                    dynamic_cast<View3D*>(p)->get_canvas3d()->render();
                else if (p == preview)
                    dynamic_cast<Preview*>(p)->get_canvas3d()->render();
            }
#endif // __WXMAC__
            p->Show();
        }
    }
    // then set to invisible the other
    for (wxPanel* p : panels) {
        if (p != current_panel)
            p->Hide();
    }

    update_sidebar(true);

    if (wxGetApp().plater()) {
        Camera& cam = wxGetApp().plater()->get_camera();
        if (old_panel == preview || old_panel == view3D) {
            view3D->get_canvas3d()->get_camera().load_camera_view(cam);
        } else if (old_panel == assemble_view) {
            assemble_view->get_canvas3d()->get_camera().load_camera_view(cam);
        }
        if (current_panel == view3D || current_panel == preview) {
            cam.load_camera_view(view3D->get_canvas3d()->get_camera());
        }
        else if (current_panel == assemble_view) {
            cam.load_camera_view(assemble_view->get_canvas3d()->get_camera());
        }
    }

    if (current_panel == view3D) {
        if (old_panel == preview)
            preview->get_canvas3d()->unbind_event_handlers();
        else if (old_panel == assemble_view) {
            assemble_view->get_canvas3d()->unbind_event_handlers();

            GLCanvas3D* assemble_canvas = assemble_view->get_canvas3d();
            Selection::IndicesList select_idxs = assemble_canvas->get_selection().get_volume_idxs();
            Selection& view3d_selection = view3D->get_canvas3d()->get_selection();
            view3d_selection.clear();
            for (unsigned int idx : select_idxs) {
                auto v = assemble_canvas->get_selection().get_volume(idx);
                auto real_idx = view3d_selection.query_real_volume_idx_from_other_view(v->object_idx(), v->instance_idx(), v->volume_idx());
                if (real_idx >= 0) {
                    view3d_selection.add(real_idx, false);
                }
            }
        }

        view3D->get_canvas3d()->bind_event_handlers();

        if (view3D->is_reload_delayed()) {
            // Delayed loading of the 3D scene.
            if (printer_technology == ptSLA) {
                // Update the SLAPrint from the current Model, so that the reload_scene()
                // pulls the correct data.
                update_restart_background_process(true, false);
            } else
                view3D->reload_scene(true);
        }

        // sets the canvas as dirty to force a render at the 1st idle event (wxWidgets IsShownOnScreen() is buggy and cannot be used reliably)
        view3D->set_as_dirty();
        // reset cached size to force a resize on next call to render() to keep imgui in synch with canvas size
        view3D->get_canvas3d()->reset_old_size();
        // BBS
        //view_toolbar.select_item("3D");
        if (notification_manager != nullptr)
            notification_manager->set_in_preview(false);
    }
    else if (current_panel == preview) {
        q->invalid_all_plate_thumbnails();
        if (old_panel == view3D)
            view3D->get_canvas3d()->unbind_event_handlers();
        else if (old_panel == assemble_view)
            assemble_view->get_canvas3d()->unbind_event_handlers();

        preview->get_canvas3d()->bind_event_handlers();

        GLGizmosManager& gizmos = view3D->get_canvas3d()->get_gizmos_manager();
        if (gizmos.is_running()) {
            gizmos.reset_all_states();
            gizmos.update_data();
        }

        if (wxGetApp().is_editor()) {
            // see: Plater::priv::object_list_changed()
            // FIXME: it may be better to have a single function making this check and let it be called wherever needed
            /*bool export_in_progress = this->background_process.is_export_scheduled();
            bool model_fits = view3D->get_canvas3d()->check_volumes_outside_state() != ModelInstancePVS_Partly_Outside;
            //BBS: add partplate logic
            PartPlate* current_plate = partplate_list.get_curr_plate();
            if (!no_slice && !model.objects.empty() && !export_in_progress && model_fits && current_plate->has_printable_instances()) {
                preview->get_canvas3d()->init_gcode_viewer();
                // BBS
                //if already running in background, not relice here
                if (!this->background_process.running())
                {
                    m_slice_all = false;
                    this->q->reslice();
                }
            }
            // keeps current gcode preview, if any
            preview->reload_print(true);

            preview->set_as_dirty();*/
            if (wxGetApp().is_editor() && !q->only_gcode_mode())
                do_reslice();
        }

        // reset cached size to force a resize on next call to render() to keep imgui in synch with canvas size
        preview->get_canvas3d()->reset_old_size();
        // BBS
        //view_toolbar.select_item("Preview");
        if (notification_manager != nullptr)
            notification_manager->set_in_preview(true);
    }
    else if (current_panel == assemble_view) {
        if (old_panel == view3D) {
            view3D->get_canvas3d()->unbind_event_handlers();
        }
        else if (old_panel == preview)
            preview->get_canvas3d()->unbind_event_handlers();

        assemble_view->get_canvas3d()->bind_event_handlers();
        assemble_view->reload_scene(true);

        if (old_panel == view3D) {
            GLCanvas3D* view3D_canvas = view3D->get_canvas3d();
            Selection::IndicesList select_idxs = view3D_canvas->get_selection().get_volume_idxs();
            Selection& assemble_selection = assemble_view->get_canvas3d()->get_selection();
            assemble_selection.clear();
            for (unsigned int idx : select_idxs) {
                auto v        = view3D_canvas->get_selection().get_volume(idx);
                auto real_idx = assemble_selection.query_real_volume_idx_from_other_view(v->object_idx(), v->instance_idx(), v->volume_idx());
                if (real_idx >= 0) {
                    assemble_selection.add(real_idx, false);
                }
            }
        }

        // BBS set default view and zoom
        if (first_enter_assemble) {
            wxGetApp().plater()->get_camera().requires_zoom_to_volumes = true;
            first_enter_assemble = false;
        }

        assemble_view->set_as_dirty();
        // BBS
        //view_toolbar.select_item("Assemble");
    }

    current_panel->SetFocusFromKbd();

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": successfully, exit");
}

// BBS
void Plater::priv::on_combobox_select(wxCommandEvent &evt)
{
    PlaterPresetComboBox* preset_combo_box = dynamic_cast<PlaterPresetComboBox*>(evt.GetEventObject());
    if (preset_combo_box) {
        this->on_select_preset(evt);
        sidebar->update_printer_thumbnail();
    }
    else {
        this->on_select_bed_type(evt);
    }
}

void Plater::priv::on_select_bed_type(wxCommandEvent &evt)
{
    ComboBox* combo = static_cast<ComboBox*>(evt.GetEventObject());
    auto        select_bed_type = sidebar->get_cur_select_bed_type();
    std::string bed_type_name = print_config_def.get("curr_bed_type")->enum_values[(int)select_bed_type - 1];

    PresetBundle& preset_bundle = *wxGetApp().preset_bundle;
    DynamicPrintConfig& proj_config = wxGetApp().preset_bundle->project_config;
    const t_config_enum_values* keys_map = print_config_def.get("curr_bed_type")->enum_keys_map;

    if (keys_map) {
        BedType new_bed_type = btCount;
        for (auto item : *keys_map) {
            if (item.first == bed_type_name) {
                new_bed_type = (BedType)item.second;
                break;
            }
        }

        if (new_bed_type != btCount) {
            BedType old_bed_type = proj_config.opt_enum<BedType>("curr_bed_type");
            if (old_bed_type != new_bed_type) {
                proj_config.set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(new_bed_type));

                wxGetApp().plater()->update_project_dirty_from_presets();

                // update plater with new config
                q->on_config_change(wxGetApp().preset_bundle->full_config());

                // update app_config
                AppConfig* app_config = wxGetApp().app_config;
                app_config->set("curr_bed_type", std::to_string(int(new_bed_type)));
                app_config->set_printer_setting(wxGetApp().preset_bundle->printers.get_selected_preset_name(),
                                                "curr_bed_type", std::to_string(int(new_bed_type)));

                //update slice status
                auto plate_list = partplate_list.get_plate_list();
                for (auto plate : plate_list) {
                    if (plate->get_bed_type() == btDefault) {
                        plate->update_slice_result_valid_state(false);
                    }
                }

                // update render
                view3D->get_canvas3d()->render();
                preview->msw_rescale();
            }
        }
    }
}

void Plater::priv::on_select_preset(wxCommandEvent &evt)
{
    PlaterPresetComboBox* combo = static_cast<PlaterPresetComboBox*>(evt.GetEventObject());
    Preset::Type preset_type    = combo->get_type();

    // Under OSX: in case of use of a same names written in different case (like "ENDER" and "Ender"),
    // m_presets_choice->GetSelection() will return first item, because search in PopupListCtrl is case-insensitive.
    // So, use GetSelection() from event parameter
    int selection = evt.GetSelection();

    auto marker = reinterpret_cast<size_t>(combo->GetClientData(selection));
    if (PresetComboBox::LabelItemType::LABEL_ITEM_WIZARD_ADD_PRINTERS == marker) {
        sidebar->create_printer_preset();
        return;
    }

    auto idx = combo->get_filament_idx();

    // BBS:Save the plate parameters before switching
    PartPlateList& old_plate_list = this->partplate_list;
    PartPlate* old_plate = old_plate_list.get_selected_plate();
    Vec3d old_plate_pos = old_plate->get_center_origin();

    // BBS: Save the model in the current platelist
    std::vector<vector<int> > plate_object;
    for (size_t i = 0; i < old_plate_list.get_plate_count(); ++i) {
        PartPlate* plate = old_plate_list.get_plate(i);
        std::vector<int> obj_idxs;
        for (int obj_idx = 0; obj_idx < model.objects.size(); obj_idx++) {
            if (plate && plate->contain_instance(obj_idx, 0)) {
                obj_idxs.emplace_back(obj_idx);
            }
        }
        plate_object.emplace_back(obj_idxs);
    }

    bool flag = is_support_filament(idx);
    //! Because of The MSW and GTK version of wxBitmapComboBox derived from wxComboBox,
    //! but the OSX version derived from wxOwnerDrawnCombo.
    //! So, to get selected string we do
    //!     combo->GetString(combo->GetSelection())
    //! instead of
    //!     combo->GetStringSelection().ToUTF8().data());

    wxString wx_name = combo->GetString(selection);
    // if (preset_type == Preset::TYPE_PRINTER) {
    //     wx_name = combo->get_preset_item_name(selection); }

    std::string preset_name = wxGetApp().preset_bundle->get_preset_name_by_alias(preset_type,
        Preset::remove_suffix_modified(wx_name.ToUTF8().data()));

    if (preset_type == Preset::TYPE_FILAMENT) {
        wxGetApp().preset_bundle->set_filament_preset(idx, preset_name);
        wxGetApp().plater()->update_project_dirty_from_presets();
        wxGetApp().preset_bundle->export_selections(*wxGetApp().app_config);
        sidebar->update_dynamic_filament_list();
        bool flag_is_change = is_support_filament(idx);
        if (flag != flag_is_change && wxGetApp().app_config->get("auto_calculate_flush") == "all") {
            sidebar->auto_calc_flushing_volumes(idx);
        }
        auto select_flag = combo->GetFlag(selection);
        combo->ShowBadge(select_flag == (int)PresetComboBox::FilamentAMSType::FROM_AMS);
        q->on_filament_change(idx);
    }
    bool select_preset = !combo->selection_is_changed_according_to_physical_printers();
    // TODO: ?
    if (preset_type == Preset::TYPE_FILAMENT && sidebar->is_multifilament()) {
        // Only update the plater UI for the 2nd and other filaments.
        combo->update();
    }
    else if (select_preset) {
        if (preset_type == Preset::TYPE_PRINTER) {
            PhysicalPrinterCollection& physical_printers = wxGetApp().preset_bundle->physical_printers;
            if(combo->is_selected_physical_printer())
                preset_name = physical_printers.get_selected_printer_preset_name();
            else
                physical_printers.unselect_printer();

            if (combo->is_selected_printer_model()) {
                auto preset = wxGetApp().preset_bundle->get_similar_printer_preset(preset_name, {});
                if (preset == nullptr) {
                    MessageDialog dlg(this->sidebar, "", "");
                    dlg.ShowModal();
                }
                preset->is_visible = true; // force visible
                preset_name = preset->name;
            }
            std::string old_preset_name = wxGetApp().preset_bundle->printers.get_edited_preset().name;

            update_objects_position_when_select_preset([this, &preset_type, &preset_name]() {
                wxWindowUpdateLocker noUpdates2(sidebar->filament_panel());
                wxGetApp().get_tab(preset_type)->select_preset(preset_name);
                // update plater with new config
                q->on_config_change(wxGetApp().preset_bundle->full_config());
            });


            if (old_preset_name != preset_name && wxGetApp().app_config->get("auto_calculate_flush") == "all") {
                wxGetApp().plater()->sidebar().auto_calc_flushing_volumes(-1);
            }

            // sync extruder info when select multi_extruder preset
            if (Slic3r::DeviceManager *dev = Slic3r::GUI::wxGetApp().getDeviceManager()) {
                MachineObject *obj = dev->get_selected_machine();
                if (obj && obj->is_multi_extruders()) {
                    PresetBundle *preset_bundle = wxGetApp().preset_bundle;
                    Preset& cur_preset = preset_bundle->printers.get_edited_preset();
                    if (cur_preset.get_printer_type(preset_bundle) == obj->get_show_printer_type()) {
                        double preset_nozzle_diameter = cur_preset.config.option<ConfigOptionFloats>("nozzle_diameter")->values[0];
                        bool   same_nozzle_diameter   = true;

                        const auto& extruders = obj->GetExtderSystem()->GetExtruders();
                        for (const DevExtder &extruder : extruders) {
                            if (!is_approx(extruder.GetNozzleDiameter(), float(preset_nozzle_diameter))) {
                                same_nozzle_diameter = false;
                            }
                        }

                        if (cur_preset.is_system || (!cur_preset.is_system && same_nozzle_diameter)) {
                            GUI::wxGetApp().sidebar().sync_extruder_list();
                        }
                    }
                }
            }
        }
        //BBS
        //wxWindowUpdateLocker noUpdates1(sidebar->print_panel());
        wxWindowUpdateLocker noUpdates2(sidebar->filament_panel());
        wxGetApp().get_tab(preset_type)->select_preset(preset_name);
    }

    // update plater with new config
    q->on_config_change(wxGetApp().preset_bundle->full_config());
    if (preset_type == Preset::TYPE_PRINTER) {
    /* Settings list can be changed after printer preset changing, so
     * update all settings items for all item had it.
     * Furthermore, Layers editing is implemented only for FFF printers
     * and for SLA presets they should be deleted
     */
        wxGetApp().obj_list()->update_object_list_by_printer_technology();

        // BBS:Model reset by plate center
        PartPlateList& cur_plate_list = this->partplate_list;
        PartPlate* cur_plate = cur_plate_list.get_curr_plate();
        Vec3d cur_plate_pos = cur_plate->get_center_origin();

        if (old_plate_pos.x() != cur_plate_pos.x() || old_plate_pos.y() != cur_plate_pos.y()) {
            for (int i = 0; i < plate_object.size(); ++i) {
                view3D->select_object_from_idx(plate_object[i]);
                this->sidebar->obj_list()->update_selections();
                view3D->center_selected_plate(i);
            }

            view3D->deselect_all();
        }
#if 0   // do not toggle auto calc when change printer
        // update flush matrix
        size_t filament_size = wxGetApp().plater()->get_extruder_colors_from_plater_config().size();
        for (size_t idx = 0; idx < filament_size; ++idx)
            wxGetApp().plater()->sidebar().auto_calc_flushing_volumes(idx);
#endif
    }

#ifdef __WXMSW__
    // From the Win 2004 preset combobox lose a focus after change the preset selection
    // and that is why the up/down arrow doesn't work properly
    // So, set the focus to the combobox explicitly
    combo->SetFocus();
#endif
    if (preset_type == Preset::TYPE_FILAMENT && wxGetApp().app_config->get("auto_calculate_flush") == "all") {
        wxGetApp().plater()->sidebar().auto_calc_flushing_volumes(idx);
    }

    // BBS: log modify of filament selection
    Slic3r::put_other_changes();

    // update slice state and set bedtype default for 3rd-party printer
    auto plate_list = partplate_list.get_plate_list();
    for (auto plate : plate_list) {
         plate->update_slice_result_valid_state(false);
    }
}

void Plater::priv::on_slicing_update(SlicingStatusEvent &evt)
{
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": event_type %1%, percent %2%, text %3%") % evt.GetEventType() % evt.status.percent % evt.status.text;
    //BBS: add slice project logic
    std::string title_text = _u8L("Slicing");
    evt.status.text = title_text + evt.status.text;
    if (evt.status.percent >= 0) {
         if (!m_worker.is_idle()) {
            // Avoid a race condition
            return;
        }

        notification_manager->set_slicing_progress_percentage(evt.status.text, (float)evt.status.percent / 100.0f);

        // update slicing percent
        PartPlateList& plate_list = wxGetApp().plater()->get_partplate_list();
        //slicing parallel, only update if percent is greater than before
        if (evt.status.percent > plate_list.get_curr_plate()->get_slicing_percent())
            plate_list.get_curr_plate()->update_slicing_percent(evt.status.percent);
    }

    if (evt.status.flags & (PrintBase::SlicingStatus::RELOAD_SCENE | PrintBase::SlicingStatus::RELOAD_SLA_SUPPORT_POINTS)) {
        switch (this->printer_technology) {
        case ptFFF:
            //BBS: add slice project logic, only display shells at the beginning
            if (!m_slice_all || (m_cur_slice_plate == (partplate_list.get_plate_count() - 1)))
                //this->update_fff_scene();
                this->update_fff_scene_only_shells();
            break;
        case ptSLA:
            // If RELOAD_SLA_SUPPORT_POINTS, then the SLA gizmo is updated (reload_scene calls update_gizmos_data)
            if (view3D->is_dragging())
                delayed_scene_refresh = true;
            else
                this->update_sla_scene();
            break;
        default: break;
        }
    } else if (evt.status.flags & PrintBase::SlicingStatus::RELOAD_SLA_PREVIEW) {
        // Update the SLA preview. Only called if not RELOAD_SLA_SUPPORT_POINTS, as the block above will refresh the preview anyways.
        this->preview->reload_print();
    }

    if (evt.status.flags & (PrintBase::SlicingStatus::UPDATE_PRINT_STEP_WARNINGS | PrintBase::SlicingStatus::UPDATE_PRINT_OBJECT_STEP_WARNINGS)) {
        // Update notification center with warnings of object_id and its warning_step.
        ObjectID object_id = evt.status.warning_object_id;
        int warning_step = evt.status.warning_step;
        PrintStateBase::StateWithWarnings state;
        ModelObject const * model_object = nullptr;

        //BBS: add partplate related logic, use the print in background process
        if (evt.status.flags & PrintBase::SlicingStatus::UPDATE_PRINT_STEP_WARNINGS) {
            state = this->printer_technology == ptFFF ?
                this->background_process.m_fff_print->step_state_with_warnings(static_cast<PrintStep>(warning_step)) :
                this->background_process.m_sla_print->step_state_with_warnings(static_cast<SLAPrintStep>(warning_step));
        } else if (this->printer_technology == ptFFF) {
            const PrintObject *print_object = this->background_process.m_fff_print->get_object(object_id);
            if (print_object) {
                state = print_object->step_state_with_warnings(static_cast<PrintObjectStep>(warning_step));
                model_object = print_object->model_object();
            }
        } else {
            const SLAPrintObject *print_object = this->background_process.m_sla_print->get_object(object_id);
            if (print_object) {
                state = print_object->step_state_with_warnings(static_cast<SLAPrintObjectStep>(warning_step));
                model_object = print_object->model_object();
            }
        }
        // Now process state.warnings.
        for (auto const& warning : state.warnings) {
            if (warning.current) {
                NotificationManager::NotificationLevel notif_level = NotificationManager::NotificationLevel::WarningNotificationLevel;
                if (evt.status.message_type == PrintStateBase::SlicingNotificationType::SlicingReplaceInitEmptyLayers || evt.status.message_type == PrintStateBase::SlicingNotificationType::SlicingEmptyGcodeLayers) {
                    notif_level = NotificationManager::NotificationLevel::SeriousWarningNotificationLevel;
                }
                notification_manager->push_slicing_warning_notification(warning.message, false, model_object, object_id, warning_step, warning.message_id, notif_level);
                add_warning(warning, object_id.id);
            }
        }
    }
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format("exit.");
}

void Plater::priv::on_slicing_completed(wxCommandEvent & evt)
{
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": event_type %1%, string %2%") % evt.GetEventType() % evt.GetString();
    //BBS: add slice project logic
    if (m_slice_all && (m_cur_slice_plate < (partplate_list.get_plate_count() - 1))) {
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format("slicing all, finished plate %1%, will continue next.")%m_cur_slice_plate;
        return;
    }

    if (view3D->is_dragging()) // updating scene now would interfere with the gizmo dragging
        delayed_scene_refresh = true;
    else {
        if (this->printer_technology == ptFFF) {
            //BBS: only reload shells
            this->update_fff_scene_only_shells(false);
            //this->update_fff_scene();
        }
        else
            this->update_sla_scene();
    }
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format("exit.");
}

void Plater::priv::on_export_began(wxCommandEvent& evt)
{
    if (show_warning_dialog)
        warnings_dialog();
}

void Plater::priv::on_export_finished(wxCommandEvent& evt)
{
#if 0
    //BBS: also export 3mf to the same directory for debugging
    std::string gcode_path_str(evt.GetString().ToUTF8().data());
    fs::path gcode_path(gcode_path_str);

    if (q) {
        q->export_3mf(gcode_path.replace_extension(".3mf"), SaveStrategy::Silence); // BBS: silence
    }
#endif
}

void Plater::priv::on_slicing_began()
{
    clear_warnings();
    notification_manager->close_notification_of_type(NotificationType::SignDetected);
    notification_manager->close_notification_of_type(NotificationType::ExportFinished);
    bool is_first_plate = m_cur_slice_plate == 0;
    bool slice_all = q->m_only_gcode ? m_slice_all_only_has_gcode : m_slice_all;
    bool need_change_dailytips = !(slice_all && !is_first_plate);
    notification_manager->set_slicing_progress_began();
    notification_manager->update_slicing_notif_dailytips(need_change_dailytips);
}
void Plater::priv::add_warning(const Slic3r::PrintStateBase::Warning& warning, size_t oid)
{
    for (auto& it : current_warnings) {
        if (warning.message_id == it.first.message_id) {
            if (warning.message_id != 0 || (warning.message_id == 0 && warning.message == it.first.message))
            {
                if (warning.message_id != 0)
                    it.first.message = warning.message;
                return;
            }
        }
    }
    current_warnings.emplace_back(std::pair<Slic3r::PrintStateBase::Warning, size_t>(warning, oid));
}
void Plater::priv::actualize_slicing_warnings(const PrintBase &print)
{
    std::vector<ObjectID> ids = print.print_object_ids();
    if (ids.empty()) {
        clear_warnings();
        return;
    }
    ids.emplace_back(print.id());
    std::sort(ids.begin(), ids.end());
    notification_manager->remove_slicing_warnings_of_released_objects(ids);
    notification_manager->set_all_slicing_warnings_gray(true);
}
void Plater::priv::actualize_object_warnings(const PrintBase& print)
{
    std::vector<ObjectID> ids;
    for (const ModelObject* object : print.model().objects )
    {
        ids.push_back(object->id());
    }
    std::sort(ids.begin(), ids.end());
    notification_manager->remove_simplify_suggestion_of_released_objects(ids);
}
void Plater::priv::clear_warnings()
{
    notification_manager->close_slicing_errors_and_warnings();
    this->current_warnings.clear();
}
bool Plater::priv::warnings_dialog()
{
    if (current_warnings.empty())
        return true;
    std::string text = _u8L("There are warnings after slicing models:") + "\n";
    for (auto const& it : current_warnings) {
        size_t next_n = it.first.message.find_first_of('\n', 0);
        text += "\n";
        if (next_n != std::string::npos)
            text += it.first.message.substr(0, next_n);
        else
            text += it.first.message;
    }
    //text += "\n\nDo you still wish to export?";
    MessageDialog msg_window(this->q, from_u8(text), _L("warnings"), wxOK);
    const auto    res = msg_window.ShowModal();
    return res == wxID_OK;

}

//BBS: add project slice logic
void Plater::priv::on_process_completed(SlicingProcessCompletedEvent &evt)
{
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(": enter, m_ignore_event %1%, status %2%")%m_ignore_event %evt.status();
    //BBS:ignore cancel event for some special case
    if (m_ignore_event)
    {
        m_ignore_event = false;
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": ignore this event %1%") % evt.status();
        return;
    }
    //BBS: add project slice logic
    bool is_finished = !m_slice_all || (m_cur_slice_plate == (partplate_list.get_plate_count() - 1));

    //BBS: slice .gcode.3mf file related logic, assign is_finished again
    bool only_has_gcode_need_preview = false;
    auto plate_list = this->partplate_list.get_plate_list();
    bool has_print_instances = false;
    for (auto plate : plate_list)
        has_print_instances = has_print_instances || plate->has_printable_instances();
    if (this->model.objects.empty() && !has_print_instances)
        only_has_gcode_need_preview = true;
    if (only_has_gcode_need_preview && m_slice_all_only_has_gcode) {
        is_finished = (m_cur_slice_plate == (partplate_list.get_plate_count() - 1));
        if (is_finished)
            m_slice_all_only_has_gcode = false;
    }

    // Stop the background task, wait until the thread goes into the "Idle" state.
    // At this point of time the thread should be either finished or canceled,
    // so the following call just confirms, that the produced data were consumed.
    this->background_process.stop();
    notification_manager->set_slicing_progress_export_possible();

    // Reset the "export G-code path" name, so that the automatic background processing will be enabled again.
    this->background_process.reset_export();
    // This bool stops showing export finished notification even when process_completed_with_error is false
    bool has_error = false;
    if (evt.error()) {
        auto message = evt.format_error_message();
        if (evt.critical_error()) {
            if (q->m_tracking_popup_menu) {
                // We don't want to pop-up a message box when tracking a pop-up menu.
                // We postpone the error message instead.
                q->m_tracking_popup_menu_error_message = message.first;
            } else {
                show_error(q, message.first, message.second.size() != 0 && message.second[0] != 0);
                notification_manager->set_slicing_progress_hidden();
            }
        } else {
            std::vector<const ModelObject *> ptrs;
            for (auto oid : message.second)
            {
                const PrintObject *print_object = this->background_process.m_fff_print->get_object(ObjectID(oid));
                if (print_object) { ptrs.push_back(print_object->model_object()); }
            }
            notification_manager->push_slicing_error_notification(message.first, ptrs);
        }
        if (evt.invalidate_plater())
        {
            // BBS
#if 0
            const wxString invalid_str = _L("Invalid data");
            for (auto btn : { ActionButtonType::abReslice, ActionButtonType::abSendGCode, ActionButtonType::abExport })
                sidebar->set_btn_label(btn, invalid_str);
#endif
            process_completed_with_error = partplate_list.get_curr_plate_index();;
        }
        has_error = true;
        is_finished = true;
    }
    if (evt.cancelled()) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", cancel event, status: %1%") % evt.status();
        this->notification_manager->set_slicing_progress_canceled(_u8L("Slicing Canceled"));
        is_finished = true;
    }

    //BBS: set the current plater's slice result to valid
    if (!this->background_process.empty())
        this->background_process.get_current_plate()->update_slice_result_valid_state(evt.success());

    //BBS: update the action button according to the current plate's status
    bool ready_to_slice = !this->partplate_list.get_curr_plate()->is_slice_result_valid();

    // BBS
#if 0
    this->sidebar->show_sliced_info_sizer(evt.success());
#endif

    // This updates the "Slice now", "Export G-code", "Arrange" buttons status.
    // Namely, it refreshes the "Out of print bed" property of all the ModelObjects, and it enables
    // the "Slice now" and "Export G-code" buttons based on their "out of bed" status.
    //BBS: remove this update here, will be updated in update_fff_scene later
    //this->object_list_changed();

    // refresh preview
    if (view3D->is_dragging()) // updating scene now would interfere with the gizmo dragging
        delayed_scene_refresh = true;
    else {
        if (this->printer_technology == ptFFF) {
            if (is_finished)
                this->update_fff_scene();
        }
        else
            this->update_sla_scene();
    }

    //BBS: add slice&&print status update logic
    if (evt.cancelled()) {
        /*if (wxGetApp().get_mode() == comSimple)
            sidebar->set_btn_label(ActionButtonType::abReslice, "Slice now");
        show_action_buttons(true);*/
        ready_to_slice = true;
        //this->main_frame->update_slice_print_status(MainFrame::eEventSliceUpdate, true, true);

        //BBS
        if (m_is_publishing) {
            m_publish_dlg->cancel();
        }
    } else {
        if((ready_to_slice) || (wxGetApp().get_mode() == comSimple)) {
            //this means the current plate is not the slicing plate
            //show_action_buttons(ready_to_slice);
            //this->main_frame->update_slice_print_status(MainFrame::eEventSliceUpdate, ready_to_slice, true);
        }
        if (exporting_status != ExportingStatus::NOT_EXPORTING && !has_error) {
            notification_manager->stop_delayed_notifications_of_type(NotificationType::ExportOngoing);
            notification_manager->close_notification_of_type(NotificationType::ExportOngoing);
        }
        // If writing to removable drive was scheduled, show notification with eject button
        if (exporting_status == ExportingStatus::EXPORTING_TO_REMOVABLE && !has_error) {
            //show_action_buttons(ready_to_slice);
            this->main_frame->update_slice_print_status(MainFrame::eEventSliceUpdate, ready_to_slice, true);
            notification_manager->push_exporting_finished_notification(last_output_path, last_output_dir_path,
                // Don't offer the "Eject" button on ChromeOS, the Linux side has no control over it.
                platform_flavor() != PlatformFlavor::LinuxOnChromium);
            wxGetApp().removable_drive_manager()->set_exporting_finished(true);
        }else
        if (exporting_status == ExportingStatus::EXPORTING_TO_LOCAL && !has_error)
            notification_manager->push_exporting_finished_notification(last_output_path, last_output_dir_path, false);

        // BBS, Generate calibration thumbnail for current plate
        if (!has_error && preview) {
            // generate calibration data
            /* BBS generate calibration data by printer
            preview->reload_print();
            ThumbnailData* calibration_data = &partplate_list.get_curr_plate()->cali_thumbnail_data;
            const ThumbnailsParams calibration_params = { {}, false, true, true, true, partplate_list.get_curr_plate_index() };
            generate_calibration_thumbnail(*calibration_data, PartPlate::cali_thumbnail_width, PartPlate::cali_thumbnail_height, calibration_params);
            preview->get_canvas3d()->reset_gcode_toolpaths();*/

            // generate bbox data
            PlateBBoxData* plate_bbox_data = &partplate_list.get_curr_plate()->cali_bboxes_data;
            *plate_bbox_data = generate_first_layer_bbox();
        }
    }

    exporting_status = ExportingStatus::NOT_EXPORTING;


    // BBS stop publishing if error occur
    //if (m_is_publishing) {
    //    GCodeProcessorResult *gcode_result = background_process.get_current_gcode_result();
    //    m_publish_dlg->UpdateStatus(_L("Error occurred during slicing"), -1, false);
    //    // if toolpath is outside
    //    if (!gcode_result || gcode_result->toolpath_outside) {
    //        m_is_publishing = false;
    //    }
    //}


    if (is_finished)
    {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":finished, reload print soon");
        m_is_slicing = false;
        this->preview->reload_print(false);
        /* BBS if in publishing progress */
        if (m_is_publishing) {
            if (m_publish_dlg && !m_publish_dlg->was_cancelled()) {
                if (m_publish_dlg->IsShown()) {
                    q->publish_project();
                } else {
                    m_is_publishing = false;
                }
            }
        }
        q->SetDropTarget(new PlaterDropTarget(*main_frame, *q));
    }
    else
    {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":slicing all, plate %1% finished, start next slice...")%m_cur_slice_plate;
        m_cur_slice_plate++;

        q->Freeze();
        q->select_plate(m_cur_slice_plate);
        partplate_list.select_plate_view();
        int ret = q->start_next_slice();
        if (ret) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(":slicing all, plate %1% can not be sliced, will stop")%m_cur_slice_plate;
            m_is_slicing = false;
        }
        //not the last plate
        update_fff_scene_only_shells();
        q->Thaw();
        if (m_is_publishing) {
            if (m_publish_dlg && !m_publish_dlg->was_cancelled()) {
                wxString msg = wxString::Format(_L("Slicing Plate %d"), m_cur_slice_plate + 1);
                int percent  = 70 * m_cur_slice_plate / partplate_list.get_plate_count();
                m_publish_dlg->UpdateStatus(msg, percent, false);
            }
        }
    }
    if (auto_reslice_after_cancel) {
        auto_reslice_after_cancel = false;
        schedule_auto_reslice_if_needed();
    }

    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(", exit.");
}

void Plater::priv::on_action_add(SimpleEvent&)
{
    if (q != nullptr) {
        //q->add_model();
        //BBS open file in toolbar add
        q->add_file();
    }
}

//BBS: add plate from toolbar
void Plater::priv::on_action_add_plate(SimpleEvent&)
{
    if (q != nullptr) {
        take_snapshot("add partplate");
        this->partplate_list.create_plate();
        int new_plate = this->partplate_list.get_plate_count() - 1;
        this->partplate_list.select_plate(new_plate);
        update();

        // BBS set default view
        //q->get_camera().select_view("topfront");
        q->get_camera().requires_zoom_to_plate = REQUIRES_ZOOM_TO_ALL_PLATE;
    }
}

//BBS: remove plate from toolbar
void Plater::priv::on_action_del_plate(SimpleEvent&)
{
    if (q != nullptr) {
        q->delete_plate();
        //q->get_camera().select_view("topfront");
        //q->get_camera().requires_zoom_to_plate = REQUIRES_ZOOM_TO_ALL_PLATE;
    }
}

//BBS: GUI refactor: GLToolbar
void Plater::priv::on_action_open_project(SimpleEvent&)
{
    if (q != nullptr) {
        q->load_project();
    }
}

//BBS: GUI refactor: slice plate
void Plater::priv::on_action_slice_plate(SimpleEvent&)
{
    if (q != nullptr) {
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received slice plate event\n" ;
        //BBS update extruder params and speed table before slicing
        const Slic3r::DynamicPrintConfig& config = wxGetApp().preset_bundle->full_config();
        auto& print = q->get_partplate_list().get_current_fff_print();
        auto print_config = print.config();
        int numExtruders = wxGetApp().preset_bundle->filament_presets.size();

        Model::setExtruderParams(config, numExtruders);
        Model::setPrintSpeedTable(config, print_config);
        m_slice_all = false;
        q->reslice();
        q->select_view_3D("Preview");
    }
}

//BBS: GUI refactor: slice all
void Plater::priv::on_action_slice_all(SimpleEvent&)
{
    if (q != nullptr) {
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received slice project event\n" ;
        //BBS update extruder params and speed table before slicing
        const Slic3r::DynamicPrintConfig& config = wxGetApp().preset_bundle->full_config();
        auto& print = q->get_partplate_list().get_current_fff_print();
        auto print_config = print.config();
        int numExtruders = wxGetApp().preset_bundle->filament_presets.size();

        Model::setExtruderParams(config, numExtruders);
        Model::setPrintSpeedTable(config, print_config);
        m_slice_all = true;
        m_slice_all_only_has_gcode = true;
        m_cur_slice_plate = 0;
        //select plate
        q->select_plate(m_cur_slice_plate);
        q->reslice();
        if (!m_is_publishing)
            q->select_view_3D("Preview");
        //BBS: wish to select all plates stats item
        preview->get_canvas3d()->_update_select_plate_toolbar_stats_item(true);
    }
}

void Plater::priv::on_action_publish(wxCommandEvent &event)
{
    if (q != nullptr) {
        if (event.GetInt() == EVT_PUBLISHING_START) {
            // update by background slicing process
            if (process_completed_with_error >= 0) {
                wxString msg = _L("Please resolve the slicing errors and publish again.");
                this->m_publish_dlg->UpdateStatus(msg, false);
                return;
            }

            m_is_publishing = true;
            // if slicing is ready publish project, else slicing first
            if (partplate_list.is_all_slice_results_valid()) {
                q->publish_project();
            } else {
                BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received slice project in background event\n";
                SimpleEvent evt = SimpleEvent(EVT_GLTOOLBAR_SLICE_ALL);
                this->on_action_slice_all(evt);
            }
        } else {
            m_is_publishing = false;
            show_publish_dlg(false);
        }
    }
}

void Plater::priv::on_action_print_plate(SimpleEvent&)
{
    if (q != nullptr) {
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received print plate event\n" ;
    }

    PresetBundle& preset_bundle = *wxGetApp().preset_bundle;
    if (preset_bundle.use_bbl_network()) {
        // BBS
        if (!m_select_machine_dlg)
            m_select_machine_dlg = new SelectMachineDialog(q);
        m_select_machine_dlg->set_print_type(PrintFromType::FROM_NORMAL);
        m_select_machine_dlg->prepare(partplate_list.get_curr_plate_index());
        m_select_machine_dlg->ShowModal();
    } else {
        q->send_gcode_legacy(PLATE_CURRENT_IDX, nullptr, true);
    }
}

void Plater::priv::on_action_send_to_multi_machine(SimpleEvent&)
{
    if (!m_send_multi_dlg)
        m_send_multi_dlg = new SendMultiMachinePage(q);
    m_send_multi_dlg->prepare(partplate_list.get_curr_plate_index());
    m_send_multi_dlg->ShowModal();
}

void Plater::priv::on_action_print_plate_from_sdcard(SimpleEvent&)
{
    if (q != nullptr) {
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received print plate event\n";
    }

    //BBS
    if (!m_select_machine_dlg) m_select_machine_dlg = new SelectMachineDialog(q);
    m_select_machine_dlg->set_print_type(PrintFromType::FROM_SDCARD_VIEW);
    m_select_machine_dlg->prepare(0);
    m_select_machine_dlg->ShowModal();
}

void Plater::priv::on_tab_selection_changing(wxBookCtrlEvent& e)
{
    // Ignore event raised by child controls
    if (!(main_frame->m_tabpanel && e.GetId() == main_frame->m_tabpanel->GetId())) {
        e.Skip();
        return;
    }

    const int new_sel = e.GetSelection();
    sidebar_layout.show = new_sel == MainFrame::tp3DEditor || new_sel == MainFrame::tpPreview;
    update_sidebar();
    int old_sel = e.GetOldSelection();
    if (wxGetApp().preset_bundle && wxGetApp().preset_bundle->use_bbl_device_tab() && new_sel == MainFrame::tpMonitor) {
        if (!wxGetApp().getAgent()) {
            e.Veto();
            BOOST_LOG_TRIVIAL(info) << boost::format("skipped tab switch from %1% to %2%, lack of network plugins") % old_sel % new_sel;
            if (q) {
                wxCommandEvent* evt = new wxCommandEvent(EVT_INSTALL_PLUGIN_HINT);
                wxQueueEvent(q, evt);
            }
        }
    } else {
        if (new_sel == MainFrame::tpMonitor && wxGetApp().preset_bundle != nullptr) {
            auto     cfg = wxGetApp().preset_bundle->printers.get_edited_preset().config;
            wxString url = cfg.opt_string("print_host_webui").empty() ? cfg.opt_string("print_host") : cfg.opt_string("print_host_webui");
            if (main_frame->m_printer_view && url.empty()) {
                // It's missing_connection page, reload so that we can replay the gif image
                main_frame->m_printer_view->reload();
            }
        }
    }
}

int Plater::priv::update_print_required_data(Slic3r::DynamicPrintConfig config, Slic3r::Model model, Slic3r::PlateDataPtrs plate_data_list, std::string file_name, std::string file_path)
{
    if (!m_select_machine_dlg) m_select_machine_dlg = new SelectMachineDialog(q);
    return m_select_machine_dlg->update_print_required_data(config, model, plate_data_list, file_name, file_path);
}

void Plater::priv::on_action_send_to_printer(bool isall)
{
	if (!m_send_to_sdcard_dlg) m_send_to_sdcard_dlg = new SendToPrinterDialog(q);
    if (isall) {
        m_send_to_sdcard_dlg->prepare(PLATE_ALL_IDX);
    }
    else {
        m_send_to_sdcard_dlg->prepare(partplate_list.get_curr_plate_index());
    }

	m_send_to_sdcard_dlg->ShowModal();
}


void Plater::priv::on_action_select_sliced_plate(wxCommandEvent &evt)
{
    if (q != nullptr) {
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received select sliced plate event\n" ;
    }
    q->select_sliced_plate(evt.GetInt());
}

void Plater::priv::on_action_print_all(SimpleEvent&)
{
    if (q != nullptr) {
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received print all event\n" ;
    }

    PresetBundle& preset_bundle = *wxGetApp().preset_bundle;
    if (preset_bundle.use_bbl_network()) {
        // BBS
        if (!m_select_machine_dlg)
            m_select_machine_dlg = new SelectMachineDialog(q);
        m_select_machine_dlg->set_print_type(PrintFromType::FROM_NORMAL);
        m_select_machine_dlg->prepare(PLATE_ALL_IDX);
        m_select_machine_dlg->ShowModal();
    } else {
        q->send_gcode_legacy(PLATE_ALL_IDX, nullptr, true);
    }
}

void Plater::priv::on_action_export_gcode(SimpleEvent&)
{
    if (q != nullptr) {
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received export gcode event\n" ;
        q->export_gcode(false);
    }
}

void Plater::priv::on_action_send_gcode(SimpleEvent&)
{
    if (q != nullptr) {
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received export gcode event\n" ;
        q->send_gcode_legacy();
    }
}

void Plater::priv::on_action_export_sliced_file(SimpleEvent&)
{
    if (q != nullptr) {
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received export sliced file event\n" ;
        q->export_gcode_3mf();
    }
}

void Plater::priv::on_action_export_all_sliced_file(SimpleEvent &)
{
    if (q != nullptr) {
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received export all sliced file event\n";
        q->export_gcode_3mf(true);
    }
}

void Plater::priv::on_action_export_to_sdcard(SimpleEvent&)
{
	if (q != nullptr) {
		BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received export sliced file event\n";
		q->send_to_printer();
	}
}

void Plater::priv::on_action_export_to_sdcard_all(SimpleEvent&)
{
    if (q != nullptr) {
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received export sliced file event\n";
        q->send_to_printer(true);
    }
}

//BBS: add plate select logic
void Plater::priv::on_plate_selected(SimpleEvent&)
{
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received plate selected event\n" ;
    sidebar->obj_list()->on_plate_selected(partplate_list.get_curr_plate_index());
}

void Plater::priv::on_action_request_model_id(wxCommandEvent& evt)
{
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received import model id event\n" ;
    if (q != nullptr) {
        q->import_model_id(evt.GetString());
    }
}

void Plater::priv::on_action_download_project(wxCommandEvent& evt)
{
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received download project event\n" ;
    if (q != nullptr) {
        q->download_project(evt.GetString());
    }
}

//BBS: add slice button status update logic
void Plater::priv::on_slice_button_status(bool enable)
{
    BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ": enable = "<<enable<<"\n";
    if (!background_process.running())
        main_frame->update_slice_print_status(MainFrame::eEventObjectUpdate, enable);
}

void Plater::priv::on_action_split_objects(SimpleEvent&)
{
    split_object();
}

void Plater::priv::on_action_split_volumes(SimpleEvent&)
{
    split_volume();
}

void Plater::priv::on_object_select(SimpleEvent& evt)
{
    wxGetApp().obj_list()->update_selections();
    selection_changed();
}

//BBS: repair model through netfabb
void Plater::priv::on_repair_model(wxCommandEvent &event)
{
    wxGetApp().obj_list()->fix_through_netfabb();
}

void Plater::priv::on_filament_color_changed(wxCommandEvent &event)
{
    //q->update_all_plate_thumbnails(true);
    //q->get_preview_canvas3D()->update_plate_thumbnails();
    int modify_id = event.GetInt();

    auto& ams_multi_color_filment = wxGetApp().preset_bundle->ams_multi_color_filment;
    if (modify_id >= 0 && modify_id < ams_multi_color_filment.size())
        ams_multi_color_filment[modify_id].clear();

    if (wxGetApp().app_config->get("auto_calculate_flush") != "disabled") {
        sidebar->auto_calc_flushing_volumes(modify_id);
    }
}

void Plater::priv::install_network_plugin(wxCommandEvent &event)
{
    wxGetApp().ShowDownNetPluginDlg();
    return;
}

void Plater::priv::update_plugin_when_launch(wxCommandEvent &event)
{
    std::string data_dir_str = data_dir();
    boost::filesystem::path data_dir_path(data_dir_str);
    auto cache_folder = data_dir_path / "ota";
    std::string changelog_file = cache_folder.string() + "/plugins/network_plugins.json";

    UpdatePluginDialog dlg(wxGetApp().mainframe);
    dlg.update_info(changelog_file);
    auto result = dlg.ShowModal();

    auto app_config = wxGetApp().app_config;
    if (!app_config) return;

    if (result == wxID_OK) {
        app_config->set("update_network_plugin", "true");
    }
    else if (result == wxID_NO) {
        app_config->set("update_network_plugin", "false");
    }
}

void Plater::priv::show_install_plugin_hint(wxCommandEvent &event)
{
    notification_manager->bbl_show_plugin_install_notification(into_u8(_L("Network Plug-in is not detected. Network related features are unavailable.")));
}

void Plater::priv::show_preview_only_hint(wxCommandEvent &event)
{
    notification_manager->bbl_show_preview_only_notification(into_u8(_L("Preview only mode:\nThe loaded file contains G-code only, cannot enter the Prepare page.")));
}

void Plater::priv::on_apple_change_color_mode(wxSysColourChangedEvent& evt) {
    m_is_dark = wxSystemSettings::GetAppearance().IsDark();
    if (view3D->get_canvas3d() && view3D->get_canvas3d()->is_initialized()) {
        view3D->get_canvas3d()->on_change_color_mode(m_is_dark);
        preview->get_canvas3d()->on_change_color_mode(m_is_dark);
        assemble_view->get_canvas3d()->on_change_color_mode(m_is_dark);
    }

    apply_color_mode();
}

void Plater::priv::on_change_color_mode(SimpleEvent& evt) {
    m_is_dark = wxGetApp().app_config->get("dark_color_mode") == "1";
    sidebar->on_change_color_mode(m_is_dark);
    view3D->get_canvas3d()->on_change_color_mode(m_is_dark);
    preview->get_canvas3d()->on_change_color_mode(m_is_dark);
    assemble_view->get_canvas3d()->on_change_color_mode(m_is_dark);
    if (m_send_to_sdcard_dlg) m_send_to_sdcard_dlg->on_change_color_mode();

    apply_color_mode();
}

void Plater::priv::apply_color_mode()
{
    const bool is_dark         = wxGetApp().dark_mode();
    wxColour   orca_color      = wxColour(59, 68, 70);//wxColour(ColorRGBA::ORCA().r_uchar(), ColorRGBA::ORCA().g_uchar(), ColorRGBA::ORCA().b_uchar());
    orca_color                 = is_dark ? StateColor::darkModeColorFor(orca_color) : StateColor::lightModeColorFor(orca_color);
    wxColour sash_color = is_dark ? wxColour(38, 46, 48) : wxColour(206, 206, 206);
    m_aui_mgr.GetArtProvider()->SetColour(wxAUI_DOCKART_INACTIVE_CAPTION_COLOUR, sash_color);
    m_aui_mgr.GetArtProvider()->SetColour(wxAUI_DOCKART_INACTIVE_CAPTION_TEXT_COLOUR, *wxWHITE);
    m_aui_mgr.GetArtProvider()->SetColour(wxAUI_DOCKART_SASH_COLOUR, sash_color);
    m_aui_mgr.GetArtProvider()->SetColour(wxAUI_DOCKART_BORDER_COLOUR, is_dark ? *wxBLACK : wxColour(165, 165, 165));
}

static void get_position(wxWindowBase* child, wxWindowBase* until_parent, int& x, int& y) {
    int res_x = 0, res_y = 0;

    while (child != until_parent && child != nullptr) {
        int _x, _y;
        child->GetPosition(&_x, &_y);
        res_x += _x;
        res_y += _y;

        child = child->GetParent();
    }

    x = res_x;
    y = res_y;
}

void Plater::priv::show_right_click_menu(Vec2d mouse_position, wxMenu *menu)
{
    // BBS: GUI refactor: move sidebar to the left
    int x, y;
    get_position(current_panel, wxGetApp().mainframe, x, y);
    wxPoint position(static_cast<int>(mouse_position.x() + x), static_cast<int>(mouse_position.y() + y));
#ifdef __linux__
    // For some reason on Linux the menu isn't displayed if position is
    // specified (even though the position is sane).
    position = wxDefaultPosition;
#endif
    GLCanvas3D &canvas = *q->canvas3D();
    canvas.apply_retina_scale(mouse_position);
    canvas.set_popup_menu_position(mouse_position);
    q->PopupMenu(menu, position);
    canvas.clear_popup_menu_position();
}

void Plater::priv::on_right_click(RBtnEvent& evt)
{
    int obj_idx = get_selected_object_idx();

    wxMenu* menu = nullptr;

    if (obj_idx == -1) { // no one or several object are selected
        if (evt.data.second) { // right button was clicked on empty space
            if (!get_selection().is_empty()) // several objects are selected in 3DScene
                return;
            menu = menus.default_menu();
        }
        else {
            if (current_panel == assemble_view) {
                menu = menus.assemble_multi_selection_menu();
            }
            else {
                menu = menus.multi_selection_menu();
            }
        }
    }
    else {
        // If in 3DScene is(are) selected volume(s), but right button was clicked on empty space
        if (evt.data.second)
            return;

        // Each context menu respects to the selected item in ObjectList,
        // so this selection should be updated before menu agyuicreation
        wxGetApp().obj_list()->update_selections();

        if (printer_technology == ptSLA)
            menu = menus.sla_object_menu();
        else {
            const Selection& selection = get_selection();
            // show "Object menu" for each one or several FullInstance instead of FullObject
            const bool is_some_full_instances = selection.is_single_full_instance() ||
                                                selection.is_single_full_object() ||
                                                selection.is_multiple_full_instance();
            const bool is_part = selection.is_single_volume() || selection.is_single_modifier();

            //BBS get assemble view menu
            if (current_panel == assemble_view) {
                menu = is_some_full_instances   ? menus.assemble_object_menu() :
                   is_part                  ? menus.assemble_part_menu()   : menus.assemble_multi_selection_menu();
            } else {
                if (is_some_full_instances)
                    menu = printer_technology == ptSLA ? menus.sla_object_menu() : menus.object_menu();
                else if (is_part) {
                    const GLVolume* gl_volume = selection.get_first_volume();
                    const ModelVolume *model_volume = get_model_volume(*gl_volume, selection.get_model()->objects);
                    menu = (model_volume != nullptr && model_volume->is_text()) ? menus.text_part_menu() :
                           (model_volume != nullptr && model_volume->is_svg()) ? menus.svg_part_menu() : 
                        menus.part_menu();
                } else
                    menu = menus.multi_selection_menu();
            }
        }
    }

    if (q != nullptr && menu) {
        show_right_click_menu(evt.data.first, menu);
    }
}

//BBS: add part plate related logic
void Plater::priv::on_plate_right_click(RBtnPlateEvent& evt)
{
    wxMenu *menu = menus.plate_menu();
    show_right_click_menu(evt.data.first, menu);
}

void Plater::priv::on_update_geometry(Vec3dsEvent<2>&)
{
    // TODO
}

void Plater::priv::on_3dcanvas_mouse_dragging_started(SimpleEvent&)
{
    view3D->get_canvas3d()->reset_sequential_print_clearance();
}

// Update the scene from the background processing,
// if the update message was received during mouse manipulation.
void Plater::priv::on_3dcanvas_mouse_dragging_finished(SimpleEvent&)
{
    if (delayed_scene_refresh) {
        delayed_scene_refresh = false;
        update_sla_scene();
    }

    //partplate_list.reload_all_objects();
}

//BBS: add plate id for thumbnail generate param
void Plater::priv::generate_thumbnail(ThumbnailData &         data,
                                      unsigned int            w,
                                      unsigned int            h,
                                      const ThumbnailsParams &thumbnail_params,
                                      Camera::EType           camera_type,
                                      Camera::ViewAngleType   camera_view_angle_type,
                                      bool                    for_picking,
                                      bool                    ban_light)
{
    view3D->get_canvas3d()->render_thumbnail(data, w, h, thumbnail_params, camera_type, camera_view_angle_type, for_picking, ban_light);
}

//BBS: add plate id for thumbnail generate param
ThumbnailsList Plater::priv::generate_thumbnails(const ThumbnailsParams& params, Camera::EType camera_type)
{
    ThumbnailsList thumbnails;
    for (const Vec2d& size : params.sizes) {
        thumbnails.push_back(ThumbnailData());
        Point isize(size); // round to ints
        generate_thumbnail(thumbnails.back(), isize.x(), isize.y(), params, camera_type);
        if (!thumbnails.back().is_valid())
            thumbnails.pop_back();
    }
    return thumbnails;
}

void Plater::priv::generate_calibration_thumbnail(ThumbnailData& data, unsigned int w, unsigned int h, const ThumbnailsParams& thumbnail_params)
{
    preview->get_canvas3d()->render_calibration_thumbnail(data, w, h, thumbnail_params);
}

PlateBBoxData Plater::priv::generate_first_layer_bbox()
{
    PlateBBoxData bboxdata;
    std::vector<BBoxData>& id_bboxes = bboxdata.bbox_objs;
    BoundingBoxf bbox_all;
    auto                   print = this->background_process.m_fff_print;
    auto curr_plate = this->partplate_list.get_curr_plate();
    auto curr_plate_seq = curr_plate->get_real_print_seq();
    bboxdata.is_seq_print = (curr_plate_seq == PrintSequence::ByObject);
    bboxdata.first_extruder = print->get_tool_ordering().first_extruder();
    bboxdata.bed_type       = bed_type_to_gcode_string(print->config().curr_bed_type.value);
    bboxdata.first_layer_time = partplate_list.get_curr_plate()->get_slice_result()->initial_layer_time;
    // get nozzle diameter
    auto opt_nozzle_diameters = print->config().option<ConfigOptionFloats>("nozzle_diameter");
    if (opt_nozzle_diameters != nullptr)
        bboxdata.nozzle_diameter = float(opt_nozzle_diameters->get_at(bboxdata.first_extruder));
    //PrintObjectPtrs objects;
    //if (this->printer_technology == ptFFF) {
    //    objects = this->background_process.m_fff_print->objects().vector();
    //}
    //else {
    //    objects = this->background_process.m_sla_print->objects();
    //}
    auto objects = print->objects();
    auto orig = this->partplate_list.get_curr_plate()->get_origin();
    Vec2d orig2d = { orig[0], orig[1] };

    BBoxData data;
    for (auto obj : objects)
    {
        auto bb_scaled = obj->get_first_layer_bbox(data.area, data.layer_height, data.name);
        auto bb = unscaled(bb_scaled);
        bb.min -= orig2d;
        bb.max -= orig2d;
        bbox_all.merge(bb);
        data.area *= (SCALING_FACTOR * SCALING_FACTOR); // unscale area
        data.id = obj->id().id;
        data.bbox = { bb.min.x(),bb.min.y(),bb.max.x(),bb.max.y() };
        id_bboxes.emplace_back(data);
    }

    // add wipe tower bounding box
    if (print->has_wipe_tower()) {
        auto   wt_corners = print->first_layer_wipe_tower_corners();
        // when loading gcode.3mf, wipe tower info may not be correct
        if (!wt_corners.empty()) {
            BoundingBox bb_scaled = {wt_corners[0], wt_corners[2]};
            auto        bb        = unscaled(bb_scaled);
            bb.min -= orig2d;
            bb.max -= orig2d;
            bbox_all.merge(bb);
            data.name = "wipe_tower";
            data.id   = partplate_list.get_curr_plate()->get_index() + 1000;
            data.bbox = {bb.min.x(), bb.min.y(), bb.max.x(), bb.max.y()};
            id_bboxes.emplace_back(data);
        }
    }

    bboxdata.bbox_all = { bbox_all.min.x(),bbox_all.min.y(),bbox_all.max.x(),bbox_all.max.y() };
    return bboxdata;
}

wxString Plater::priv::get_project_filename(const wxString& extension) const
{
    if (m_project_name.empty())
        return "";
    else {
        auto full_filename = m_project_folder / std::string((m_project_name + extension).mb_str(wxConvUTF8));
        return m_project_folder.empty() ? "" : from_path(full_filename);
    }
}

wxString Plater::priv::get_export_gcode_filename(const wxString& extension, bool only_filename, bool export_all) const
{
    std::string plate_index_str;
    auto plate_name = partplate_list.get_curr_plate()->get_plate_name();
    if (!plate_name.empty())
        plate_index_str = (boost::format("_%1%") % plate_name).str();
    else if (partplate_list.get_plate_count() > 1)
        plate_index_str = (boost::format("_plate_%1%") % std::to_string(partplate_list.get_curr_plate_index() + 1)).str();

    if (!m_project_folder.empty()) {
        if (!only_filename) {
            if (export_all) {
                auto full_filename = m_project_folder / std::string((m_project_name + extension).mb_str(wxConvUTF8));
                return from_path(full_filename);
            } else {
                auto full_filename = m_project_folder / std::string((m_project_name + from_u8(plate_index_str) + extension).mb_str(wxConvUTF8));
                return from_path(full_filename);
            }
        } else {
            if (export_all)
                return m_project_name + extension;
            else
                return m_project_name + from_u8(plate_index_str) + extension;
        }
    } else {
        if (only_filename) {
            if(!model.objects.empty() && m_project_name == _L("Untitled"))
                return wxString(fs::path(model.objects.front()->name).replace_extension().c_str()) + from_u8(plate_index_str) + extension;

            if (export_all)
                return m_project_name + extension;
            else
                return m_project_name + from_u8(plate_index_str) + extension;
        }
        else
            return "";
    }
}

wxString Plater::priv::get_project_name()
{
    return m_project_name;
}

//BBS
void Plater::priv::set_project_name(const wxString& project_name)
{
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << __LINE__ << " project is:" << project_name;
    m_project_name = project_name;
    //update topbar title
#ifdef __WINDOWS__
    wxGetApp().mainframe->SetTitle(m_project_name + " - OrcaSlicer");
    wxGetApp().mainframe->topbar()->SetTitle(m_project_name);
#else
    wxGetApp().mainframe->SetTitle(m_project_name);
    if (!m_project_name.IsEmpty())
        wxGetApp().mainframe->update_title_colour_after_set_title();
#endif
}

void Plater::priv::update_title_dirty_status()
{
    if (m_project_name.empty())
        return;

    wxString title;
    if (is_project_dirty())
        title = "*" + m_project_name;
    else
        title = m_project_name;

#ifdef __WINDOWS__
    wxGetApp().mainframe->topbar()->SetTitle(title);
#else
    wxGetApp().mainframe->SetTitle(title);
    wxGetApp().mainframe->update_title_colour_after_set_title();    
#endif    
}

void Plater::priv::set_project_filename(const wxString& filename)
{
    boost::filesystem::path full_path = into_path(filename);
    boost::filesystem::path ext = full_path.extension();
    //if (boost::iequals(ext.string(), ".amf")) {
    //    // Remove the first extension.
    //    full_path.replace_extension("");
    //    // It may be ".zip.amf".
    //    if (boost::iequals(full_path.extension().string(), ".zip"))
    //        // Remove the 2nd extension.
    //        full_path.replace_extension("");
    //} else {
    //    // Remove just one extension.
    //    full_path.replace_extension("");
    //}
    full_path.replace_extension("");

    m_project_folder = full_path.parent_path();
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << __LINE__ << " project folder is:" << m_project_folder.string();

    //BBS
    wxString project_name = from_u8(full_path.filename().string());
    set_project_name(project_name);
    // record filename for hint when open exported file/.gcode
    if (q->m_only_gcode)
        q->m_preview_only_filename = std::string((project_name + ".gcode").mb_str());
    if (q->m_exported_file)
        q->m_preview_only_filename = std::string((project_name + ".3mf").mb_str());

    wxGetApp().mainframe->update_title();

    if (!m_project_folder.empty() && !q->m_only_gcode)
        wxGetApp().mainframe->add_to_recent_projects(filename);
}

void Plater::priv::init_notification_manager()
{
    if (!notification_manager)
        return;
    notification_manager->init();

    auto cancel_callback = [this]() {
        if (this->background_process.idle())
            return false;
        this->background_process.stop();
        return true;
    };
    notification_manager->init_slicing_progress_notification(cancel_callback);
    notification_manager->set_fff(printer_technology == ptFFF);
    notification_manager->init_progress_indicator();
}

void Plater::priv::update_objects_position_when_select_preset(const std::function<void()> &select_prest)
{
    // TODO: Orca hack
    select_prest();

    wxGetApp().obj_list()->update_object_list_by_printer_technology();

    update();
}

void Plater::orient()
{
    auto &w = get_ui_job_worker();
    if (w.is_idle()) {
        p->take_snapshot(_u8L("Orient"));
        replace_job(w, std::make_unique<OrientJob>());
    }
}

//BBS: add job state related functions
void Plater::set_prepare_state(int state)
{
    p->m_job_prepare_state = state;
}

int Plater::get_prepare_state()
{
    return p->m_job_prepare_state;
}

void Plater::get_print_job_data(PrintPrepareData* data)
{
    if (data) {
        data->plate_idx = p->m_print_job_data.plate_idx;
        data->_3mf_path = p->m_print_job_data._3mf_path;
        data->_3mf_config_path = p->m_print_job_data._3mf_config_path;
    }
}

void Plater::set_print_job_plate_idx(int plate_idx)
{
    if (plate_idx == PLATE_CURRENT_IDX) {
        p->m_print_job_data.plate_idx = get_partplate_list().get_curr_plate_index();
    }
    else {
        p->m_print_job_data.plate_idx = plate_idx;
    }
}


int Plater::get_send_calibration_finished_event()
{
    return EVT_SEND_CALIBRATION_FINISHED;
}

int Plater::get_print_finished_event()
{
    return EVT_PRINT_FINISHED;
}

int Plater::get_send_finished_event()
{
    return EVT_SEND_FINISHED;
}

int Plater::get_publish_finished_event()
{
    return EVT_PUBLISH_FINISHED;
}

void Plater::priv::set_current_canvas_as_dirty()
{
    if (current_panel == view3D)
        view3D->set_as_dirty();
    else if (current_panel == preview)
        preview->set_as_dirty();
    else if (current_panel == assemble_view)
        assemble_view->set_as_dirty();
}

GLCanvas3D* Plater::priv::get_current_canvas3D(bool exclude_preview)
{
    if (current_panel == view3D)
        return view3D->get_canvas3d();
    else if (!exclude_preview && (current_panel == preview))
        return preview->get_canvas3d();
    else if (current_panel == assemble_view)
        return assemble_view->get_canvas3d();
    else //BBS default set to view3D
        return view3D->get_canvas3d();

    //return (current_panel == view3D) ? view3D->get_canvas3d() : ((current_panel == preview) ? preview->get_canvas3d() : nullptr);
}

void Plater::priv::unbind_canvas_event_handlers()
{
    if (view3D != nullptr)
        view3D->get_canvas3d()->unbind_event_handlers();

    if (preview != nullptr)
        preview->get_canvas3d()->unbind_event_handlers();

    if (assemble_view != nullptr)
        assemble_view->get_canvas3d()->unbind_event_handlers();
}

void Plater::priv::reset_canvas_volumes()
{
    if (view3D != nullptr)
        view3D->get_canvas3d()->reset_volumes();

    if (preview != nullptr)
        preview->get_canvas3d()->reset_volumes();
}

bool Plater::priv::check_ams_status_impl(bool is_slice_all)
{
    Slic3r::DeviceManager *dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev)
        return true;

    MachineObject* obj = dev->get_selected_machine();
    if (!obj || !obj->is_multi_extruders())
        return true;
    if (q->is_gcode_3mf() || q->only_gcode_mode() || q->get_partplate_list().get_curr_plate()->get_objects().empty()) {
        return true;
    }
    PresetBundle *preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle && preset_bundle->printers.get_edited_preset().get_printer_type(preset_bundle) == obj->get_show_printer_type()) {
        bool is_same_as_printer = true;
        auto nozzle_volumes_values = preset_bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type")->values;
        assert(obj->GetExtderSystem()->GetTotalExtderCount() == 2 && nozzle_volumes_values.size() == 2);
        if (obj->GetExtderSystem()->GetTotalExtderCount() == 2 && nozzle_volumes_values.size() == 2) {
            NozzleVolumeType right_nozzle_type = NozzleVolumeType(obj->GetExtderSystem()->GetNozzleFlowType(0) - 1);
            NozzleVolumeType left_nozzle_type = NozzleVolumeType(obj->GetExtderSystem()->GetNozzleFlowType(1) - 1);
            NozzleVolumeType preset_left_type  = NozzleVolumeType(nozzle_volumes_values[0]);
            NozzleVolumeType preset_right_type  = NozzleVolumeType(nozzle_volumes_values[1]);
            is_same_as_printer = (left_nozzle_type == preset_left_type && right_nozzle_type == preset_right_type);
        }

        std::vector<std::map<int, int>> ams_count_info;
        ams_count_info.resize(2);
        int deputy_4 = 0, main_4 = 0, deputy_1 = 0, main_1 = 0;
        for (auto ams : obj->GetFilaSystem()->GetAmsList()) {
            // Main (first) extruder at right
            if (ams.second->GetExtruderId() == 0) {
                if (ams.second->GetAmsType() == DevAms::N3S) // N3S
                    ++main_1;
                else
                    ++main_4;
            } else if (ams.second->GetExtruderId() == 1) {
                if (ams.second->GetAmsType() == DevAms::N3S) // N3S
                    ++deputy_1;
                else
                    ++deputy_4;
            }
        }

        int left_4  = main_4;
        int left_1  = main_1;
        int right_4 = deputy_4;
        int right_1 = deputy_1;
        if (!obj->is_main_extruder_on_left()) {
            left_4  = deputy_4;
            left_1  = deputy_1;
            right_4 = main_4;
            right_1 = main_1;
        }

        if (!preset_bundle->extruder_ams_counts.empty() && !preset_bundle->extruder_ams_counts.front().empty()) {
            is_same_as_printer &= preset_bundle->extruder_ams_counts[0][4] == left_4
            && preset_bundle->extruder_ams_counts[0][1] == left_1
            && preset_bundle->extruder_ams_counts[1][4] == right_4
            && preset_bundle->extruder_ams_counts[1][1] == right_1;
        }

        if (!is_same_as_printer) {
            struct SyncInfoDialog : MessageDialog
            {
                SyncInfoDialog(wxWindow *parent)
                    : MessageDialog(parent,
                                    _L("The nozzle type and AMS quantity information has not been synced from the connected printer.\n"
                                       "After syncing, software can optimize printing time and filament usage when slicing.\n"
                                       "Would you like to sync now ?"),
                                    _L("Warning"), 0)
                {
                    add_button(wxID_YES, true, _L("Sync now"));
                    add_button(wxID_NO, true, _L("Later"));
                }
            } dlg(q);
            dlg.Fit();
            if (dlg.ShowModal() == wxID_YES) {
                if (GUI::wxGetApp().sidebar().sync_extruder_list()) {
                    if (is_slice_all)
                        wxPostEvent(q, SimpleEvent(EVT_GLTOOLBAR_SLICE_ALL));
                    else
                        wxPostEvent(q, SimpleEvent(EVT_GLTOOLBAR_SLICE_PLATE));
                    wxGetApp().mainframe->m_tabpanel->SetSelection(MainFrame::TabPosition::tpPreview);
                }
                return false;
            }
        }
    }

    return true;
}

bool Plater::priv::get_machine_sync_status()
{
    Slic3r::DeviceManager *dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev)
        return false;

    MachineObject* obj = dev->get_selected_machine();
    if (!obj)
        return false;

    PresetBundle *preset_bundle = wxGetApp().preset_bundle;
    return preset_bundle && preset_bundle->printers.get_edited_preset().get_printer_type(preset_bundle) == obj->get_show_printer_type();
}

bool Plater::priv::init_collapse_toolbar()
{
    if (wxGetApp().is_gcode_viewer())
        return true;

    if (collapse_toolbar.get_items_count() > 0)
        // already initialized
        return true;

    BackgroundTexture::Metadata background_data;
    background_data.filename = m_is_dark ? "toolbar_background_dark.png" : "toolbar_background.png";
    background_data.left = 16;
    background_data.top = 16;
    background_data.right = 16;
    background_data.bottom = 16;

    if (!collapse_toolbar.init(background_data))
        return false;

    collapse_toolbar.set_layout_type(GLToolbar::Layout::Vertical);
    collapse_toolbar.set_horizontal_orientation(GLToolbar::Layout::HO_Right);
    collapse_toolbar.set_vertical_orientation(GLToolbar::Layout::VO_Top);
    collapse_toolbar.set_border(4.0f);
    collapse_toolbar.set_separator_size(4);
    collapse_toolbar.set_gap_size(2);

    collapse_toolbar.del_all_item();

    GLToolbarItem::Data item;

    item.name = "collapse_sidebar";
    // set collapse svg name
    item.icon_filename = "collapse.svg";
    item.sprite_id = 0;
    item.left.action_callback = []() {
        wxGetApp().plater()->collapse_sidebar(!wxGetApp().plater()->is_sidebar_collapsed());
    };

    if (!collapse_toolbar.add_item(item))
        return false;

    // Now "collapse" sidebar to current state. This is done so the tooltip
    // is updated before the toolbar is first used.
    wxGetApp().plater()->collapse_sidebar(wxGetApp().plater()->is_sidebar_collapsed());
    return true;
}

void Plater::priv::update_preview_bottom_toolbar()
{
    ;
}

#if 0
void Plater::update_partplate()
{
    sidebar().update_partplate(p->partplate_list);
}
#endif

void Plater::priv::reset_gcode_toolpaths()
{
    preview->get_canvas3d()->reset_gcode_toolpaths();
}

bool Plater::priv::can_set_instance_to_object() const
{
    const int obj_idx = get_selected_object_idx();
    return 0 <= obj_idx && obj_idx < (int)model.objects.size() && model.objects[obj_idx]->instances.size() > 1;
}

bool Plater::priv::can_split(bool to_objects) const
{
    return sidebar->obj_list()->is_splittable(to_objects);
}

bool Plater::priv::can_fillcolor() const
{
    //BBS TODO
    return true;
}

bool Plater::priv::has_assemble_view() const
{
    for (auto object: model.objects)
    {
        for (auto instance : object->instances)
            if (instance->is_assemble_initialized())
                return true;

        int part_cnt = 0;
        for (auto volume : object->volumes) {
            if (volume->is_model_part())
                part_cnt++;
        }

        if (part_cnt > 1)
            return true;
    }
    return false;
}

#if ENABLE_ENHANCED_PRINT_VOLUME_FIT
bool Plater::priv::can_scale_to_print_volume() const
{
    const BuildVolume_Type type = this->bed.build_volume().type();
    return !sidebar->obj_list()->has_selected_cut_object()
        && !view3D->get_canvas3d()->get_selection().is_empty()
        && (type == BuildVolume_Type::Rectangle || type == BuildVolume_Type::Circle);
}
#endif // ENABLE_ENHANCED_PRINT_VOLUME_FIT

bool Plater::priv::can_mirror() const
{
    return !sidebar->obj_list()->has_selected_cut_object()
        && get_selection().is_from_single_instance();
}

bool Plater::priv::can_replace_with_stl() const
{
    return !sidebar->obj_list()->has_selected_cut_object()
        && get_selection().get_volume_idxs().size() == 1;
}

bool Plater::priv::can_replace_all_with_stl() const
{
    return !sidebar->obj_list()->has_selected_cut_object()
        && get_selection().get_volume_idxs().size() != 1;
}

bool Plater::priv::can_reload_from_disk() const
{
    if (sidebar->obj_list()->has_selected_cut_object())
        return false;

#if ENABLE_RELOAD_FROM_DISK_REWORK
    // collect selected reloadable ModelVolumes
    std::vector<std::pair<int, int>> selected_volumes = reloadable_volumes(model, get_selection());
    // nothing to reload, return
    if (selected_volumes.empty())
        return false;
#else
    // struct to hold selected ModelVolumes by their indices
    struct SelectedVolume
    {
        int object_idx;
        int volume_idx;

        // operators needed by std::algorithms
        bool operator < (const SelectedVolume& other) const { return (object_idx < other.object_idx) || ((object_idx == other.object_idx) && (volume_idx < other.volume_idx)); }
        bool operator == (const SelectedVolume& other) const { return (object_idx == other.object_idx) && (volume_idx == other.volume_idx); }
    };
    std::vector<SelectedVolume> selected_volumes;

    const Selection& selection = get_selection();

    // collects selected ModelVolumes
    const std::set<unsigned int>& selected_volumes_idxs = selection.get_volume_idxs();
    for (unsigned int idx : selected_volumes_idxs) {
        const GLVolume* v = selection.get_volume(idx);
        int v_idx = v->volume_idx();
        if (v_idx >= 0) {
            int o_idx = v->object_idx();
            if (0 <= o_idx && o_idx < (int)model.objects.size())
                selected_volumes.push_back({ o_idx, v_idx });
        }
    }
#endif // ENABLE_RELOAD_FROM_DISK_REWORK

#if ENABLE_RELOAD_FROM_DISK_REWORK
    std::sort(selected_volumes.begin(), selected_volumes.end(), [](const std::pair<int, int> &v1, const std::pair<int, int> &v2) {
        return (v1.first < v2.first) || (v1.first == v2.first && v1.second < v2.second);
        });
    selected_volumes.erase(std::unique(selected_volumes.begin(), selected_volumes.end(), [](const std::pair<int, int> &v1, const std::pair<int, int> &v2) {
        return (v1.first == v2.first) && (v1.second == v2.second);
        }), selected_volumes.end());

    // collects paths of files to load
    std::vector<fs::path> paths;
    for (auto [obj_idx, vol_idx] : selected_volumes) {
        paths.push_back(model.objects[obj_idx]->volumes[vol_idx]->source.input_file);
    }
#else
    std::sort(selected_volumes.begin(), selected_volumes.end());
    selected_volumes.erase(std::unique(selected_volumes.begin(), selected_volumes.end()), selected_volumes.end());

    // collects paths of files to load
    std::vector<fs::path> paths;
    for (const SelectedVolume& v : selected_volumes) {
        const ModelObject* object = model.objects[v.object_idx];
        const ModelVolume* volume = object->volumes[v.volume_idx];
        if (!volume->source.input_file.empty())
            paths.push_back(volume->source.input_file);
        else if (!object->input_file.empty() && !volume->name.empty() && !volume->source.is_from_builtin_objects)
            paths.push_back(volume->name);
    }
#endif // ENABLE_RELOAD_FROM_DISK_REWORK
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());

    return !paths.empty();
}

void Plater::priv::update_publish_dialog_status(wxString &msg, int percent)
{
    if (m_publish_dlg)
        m_publish_dlg->UpdateStatus(msg, percent);
}

bool Plater::priv::show_publish_dlg(bool show)
{
    if (q != nullptr) { BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":recevied publish event\n"; }

    if (!m_publish_dlg) m_publish_dlg = new PublishDialog(q);
    if (show) {
        m_publish_dlg->reset();
        m_publish_dlg->start_slicing();
        //m_publish_dlg->Show();
        m_publish_dlg->ShowModal();
    } else {
        m_publish_dlg->EndModal(wxID_OK);
        //cancel the slicing
        if (this->background_process.running())
            this->background_process.stop();
    }
    return true;
}

//BBS: add bed exclude area
void Plater::priv::set_bed_shape(const Pointfs       &shape,
                                 const Pointfs       &exclude_areas,
                                 const Pointfs       &wrapping_exclude_areas,
                                 const double         printable_height,
                                 std::vector<Pointfs> extruder_areas,
                                 std::vector<double>  extruder_heights,
                                 const std::string   &custom_texture,
                                 const std::string   &custom_model,
                                 bool                 force_as_custom)
{
    //Orca: reduce resolution for large bed printer
    BoundingBoxf bed_size = get_extents(shape);
    if (bed_size.size().maxCoeff() <= LARGE_BED_THRESHOLD)
        SCALING_FACTOR = SCALING_FACTOR_INTERNAL;
    else
        SCALING_FACTOR = SCALING_FACTOR_INTERNAL_LARGE_PRINTER;

    //BBS: add shape position
    Vec2d shape_position = partplate_list.get_current_shape_position();
    bool new_shape = bed.set_shape(shape, printable_height, extruder_areas, extruder_heights, custom_model, force_as_custom, shape_position);

    float prev_height_lid, prev_height_rod;
    partplate_list.get_height_limits(prev_height_lid, prev_height_rod);
    double height_to_lid = config->opt_float("extruder_clearance_height_to_lid");
    double height_to_rod = config->opt_float("extruder_clearance_height_to_rod");

    Pointfs prev_exclude_areas = partplate_list.get_exclude_area();
    Pointfs prev_wrapping_exclude_areas = partplate_list.get_wrapping_exclude_area();
    new_shape |= (height_to_lid != prev_height_lid) || (height_to_rod != prev_height_rod) || (prev_exclude_areas != exclude_areas)
        || (prev_wrapping_exclude_areas != wrapping_exclude_areas);
    if (!new_shape && partplate_list.get_logo_texture_filename() != custom_texture) {
        partplate_list.update_logo_texture_filename(custom_texture);
    }
    if (new_shape) {
        if (view3D) view3D->bed_shape_changed();
        if (preview) preview->bed_shape_changed();

        //BBS: update part plate's size
        // BBS: to be checked
        Vec3d max = bed.printable_bounding_box().max;
        Vec3d min = bed.printable_bounding_box().min;
        double z = config->opt_float("printable_height");

        partplate_list.reset_size(max.x() - min.x() - Bed3D::Axes::DefaultTipRadius, max.y() - min.y() - Bed3D::Axes::DefaultTipRadius, z);
        partplate_list.set_shapes(shape, exclude_areas, wrapping_exclude_areas, extruder_areas, extruder_heights, custom_texture, height_to_lid, height_to_rod);

        Vec2d new_shape_position = partplate_list.get_current_shape_position();
        if (shape_position != new_shape_position)
            bed.set_shape(shape, printable_height, extruder_areas, extruder_heights, custom_model, force_as_custom, new_shape_position);
    }
}

bool Plater::priv::can_delete() const
{
    return !get_selection().is_empty() && !get_selection().is_wipe_tower();
}

bool Plater::priv::can_delete_all() const
{
    return !model.objects.empty();
}

bool Plater::priv::can_add_plate() const
{
    return q->get_partplate_list().get_plate_count() < PartPlateList::MAX_PLATES_COUNT;
}

bool Plater::priv::can_delete_plate() const
{
    return q->get_partplate_list().get_plate_count() > 1;
}

bool Plater::priv::can_fix_through_netfabb() const
{
    std::vector<int> obj_idxs, vol_idxs;
    sidebar->obj_list()->get_selection_indexes(obj_idxs, vol_idxs);

#if FIX_THROUGH_NETFABB_ALWAYS
    // Fixing always.
    return ! obj_idxs.empty() || ! vol_idxs.empty();
#else // FIX_THROUGH_NETFABB_ALWAYS
    // Fixing only if the model is not manifold.
    if (vol_idxs.empty()) {
        for (auto obj_idx : obj_idxs)
            if (model.objects[obj_idx]->get_repaired_errors_count() > 0)
                return true;
        return false;
    }

    int obj_idx = obj_idxs.front();
    for (auto vol_idx : vol_idxs)
        if (model.objects[obj_idx]->get_repaired_errors_count(vol_idx) > 0)
            return true;
    return false;
#endif // FIX_THROUGH_NETFABB_ALWAYS
}

bool Plater::priv::can_simplify() const
{
    // is object for simplification selected
    if (get_selected_object_idx() < 0) return false;
    // is already opened?
    if (q->get_view3D_canvas3D()->get_gizmos_manager().get_current_type() ==
        GLGizmosManager::EType::Simplify)
        return false;
    return true;
}

bool Plater::priv::can_increase_instances() const
{
    if (!m_worker.is_idle()
     || q->get_view3D_canvas3D()->get_gizmos_manager().is_in_editing_mode())
            return false;

    int obj_idx = get_selected_object_idx();
    return (0 <= obj_idx) && (obj_idx < (int)model.objects.size())
        && !sidebar->obj_list()->has_selected_cut_object()
        && std::all_of(model.objects[obj_idx]->instances.begin(), model.objects[obj_idx]->instances.end(), [](auto& inst) {return inst->printable; });
}

bool Plater::priv::can_decrease_instances() const
{
    if (!m_worker.is_idle()
     || q->get_view3D_canvas3D()->get_gizmos_manager().is_in_editing_mode())
            return false;

    int obj_idx = get_selected_object_idx();
    return (0 <= obj_idx) && (obj_idx < (int)model.objects.size()) && (model.objects[obj_idx]->instances.size() > 1)
        && !sidebar->obj_list()->has_selected_cut_object();
}

bool Plater::priv::can_split_to_objects() const
{
    return q->can_split(true);
}

bool Plater::priv::can_split_to_volumes() const
{
    return (printer_technology != ptSLA) && q->can_split(false);
}

bool Plater::priv::can_arrange() const
{
    return !model.objects.empty() && m_worker.is_idle();
}

bool Plater::priv::layers_height_allowed() const
{
    if (printer_technology != ptFFF)
        return false;

    int obj_idx = get_selected_object_idx();
    return 0 <= obj_idx && obj_idx < (int)model.objects.size() && model.objects[obj_idx]->max_z() > SINKING_Z_THRESHOLD && view3D->is_layers_editing_allowed();
}

bool Plater::priv::can_layers_editing() const
{
    return layers_height_allowed();
}

void Plater::priv::on_action_layersediting(SimpleEvent&)
{
    view3D->enable_layers_editing(!view3D->is_layers_editing_enabled());
    notification_manager->set_move_from_overlay(view3D->is_layers_editing_enabled());
}

void Plater::priv::on_create_filament(SimpleEvent &)
{
    CreateFilamentPresetDialog dlg(wxGetApp().mainframe);
    int res = dlg.ShowModal();
    if (wxID_OK == res) {
        wxGetApp().mainframe->update_side_preset_ui();
        update_ui_from_settings();
        sidebar->update_all_preset_comboboxes();
        CreatePresetSuccessfulDialog success_dlg(wxGetApp().mainframe, SuccessType::FILAMENT);
        int                          res = success_dlg.ShowModal();
    }
}

void Plater::priv::on_modify_filament(SimpleEvent &evt)
{
    Filamentinformation *filament_info = static_cast<Filamentinformation *>(evt.GetEventObject());
    int                 res;
    std::shared_ptr<Preset> need_edit_preset;
    {
        EditFilamentPresetDialog dlg(wxGetApp().mainframe, filament_info);
        res = dlg.ShowModal();
        need_edit_preset = dlg.get_need_edit_preset();
    }
    wxGetApp().mainframe->update_side_preset_ui();
    update_ui_from_settings();
    sidebar->update_all_preset_comboboxes();
    if (wxID_EDIT == res) {
        Tab *tab = wxGetApp().get_tab(Preset::Type::TYPE_FILAMENT);
        //tab->restore_last_select_item();
        if (tab == nullptr) { return; }
        // Popup needs to be called before "restore_last_select_item", otherwise the page may not be updated
        wxGetApp().params_dialog()->Popup();
        tab->restore_last_select_item();
        // Opening Studio and directly accessing the Filament settings interface through the edit preset button will not take effect and requires manual settings.
        tab->set_just_edit(true);
        tab->select_preset(need_edit_preset->name);
        // when some preset have modified, if the printer is not need_edit_preset_name compatible printer, the preset will jump to other preset, need select again
        if (!need_edit_preset->is_compatible) tab->select_preset(need_edit_preset->name);
    }

}

void Plater::priv::on_add_filament(SimpleEvent &evt) {
    sidebar->add_filament();
}

void Plater::priv::on_delete_filament(SimpleEvent &evt) {
    sidebar->delete_filament();
}

void Plater::priv::on_add_custom_filament(ColorEvent &evt)
{
    sidebar->add_custom_filament(evt.data);
}

void Plater::priv::enter_gizmos_stack()
{
    assert(m_undo_redo_stack_active == &m_undo_redo_stack_main);
    if (m_undo_redo_stack_active == &m_undo_redo_stack_main) {
        m_undo_redo_stack_active = &m_undo_redo_stack_gizmos;
        assert(m_undo_redo_stack_active->empty());
        // Take the initial snapshot of the gizmos.
        // Not localized on purpose, the text will never be shown to the user.
        this->take_snapshot(std::string("Gizmos-Initial"));
    }
}

bool Plater::priv::leave_gizmos_stack()
{
    bool changed = false;
    assert(m_undo_redo_stack_active == &m_undo_redo_stack_gizmos);
    if (m_undo_redo_stack_active == &m_undo_redo_stack_gizmos) {
        assert(! m_undo_redo_stack_active->empty());
        changed = m_undo_redo_stack_gizmos.has_undo_snapshot();
        m_undo_redo_stack_active->clear();
        m_undo_redo_stack_active = &m_undo_redo_stack_main;
    }
    return changed;
}

int Plater::priv::get_active_snapshot_index()
{
    const size_t active_snapshot_time = this->undo_redo_stack().active_snapshot_time();
    const std::vector<UndoRedo::Snapshot>& ss_stack = this->undo_redo_stack().snapshots();
    const auto it = std::lower_bound(ss_stack.begin(), ss_stack.end(), UndoRedo::Snapshot(active_snapshot_time));
    return it - ss_stack.begin();
}

void Plater::priv::take_snapshot(const std::string& snapshot_name, const UndoRedo::SnapshotType snapshot_type)
{
    if (m_prevent_snapshots > 0)
        return;
    assert(m_prevent_snapshots >= 0);
    // BBS: single snapshot
    if (m_single && !m_single->check(snapshot_modifies_project(snapshot_type) && (snapshot_name.empty() || snapshot_name.back() != '!')))
        return;
    UndoRedo::SnapshotData snapshot_data;
    snapshot_data.snapshot_type      = snapshot_type;
    snapshot_data.printer_technology = this->printer_technology;
    if (this->view3D->is_layers_editing_enabled())
        snapshot_data.flags |= UndoRedo::SnapshotData::VARIABLE_LAYER_EDITING_ACTIVE;
    if (this->sidebar->obj_list()->is_selected(itSettings)) {
        snapshot_data.flags |= UndoRedo::SnapshotData::SELECTED_SETTINGS_ON_SIDEBAR;
        snapshot_data.layer_range_idx = this->sidebar->obj_list()->get_selected_layers_range_idx();
    }
    else if (this->sidebar->obj_list()->is_selected(itLayer)) {
        snapshot_data.flags |= UndoRedo::SnapshotData::SELECTED_LAYER_ON_SIDEBAR;
        snapshot_data.layer_range_idx = this->sidebar->obj_list()->get_selected_layers_range_idx();
    }
    else if (this->sidebar->obj_list()->is_selected(itLayerRoot))
        snapshot_data.flags |= UndoRedo::SnapshotData::SELECTED_LAYERROOT_ON_SIDEBAR;

    // If SLA gizmo is active, ask it if it wants to trigger support generation
    // on loading this snapshot.
    if (view3D->get_canvas3d()->get_gizmos_manager().wants_reslice_supports_on_undo())
        snapshot_data.flags |= UndoRedo::SnapshotData::RECALCULATE_SLA_SUPPORTS;

    //FIXME updating the Wipe tower config values at the ModelWipeTower from the Print config.
    // This is a workaround until we refactor the Wipe Tower position / orientation to live solely inside the Model, not in the Print config.
    // BBS: add partplate logic
    if (this->printer_technology == ptFFF) {
        const DynamicPrintConfig& config = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        const DynamicPrintConfig& proj_cfg = wxGetApp().preset_bundle->project_config;
        const ConfigOptionFloats* tower_x_opt = proj_cfg.option<ConfigOptionFloats>("wipe_tower_x");
        const ConfigOptionFloats* tower_y_opt = proj_cfg.option<ConfigOptionFloats>("wipe_tower_y");
        assert(tower_x_opt->values.size() == tower_y_opt->values.size());
        model.wipe_tower.positions.clear();
        model.wipe_tower.positions.resize(tower_x_opt->values.size());
        for (int plate_idx = 0; plate_idx < tower_x_opt->values.size(); plate_idx++) {
            ModelWipeTower& tower = model.wipe_tower;

            tower.positions[plate_idx] = Vec2d(tower_x_opt->get_at(plate_idx), tower_y_opt->get_at(plate_idx));
            tower.rotation = proj_cfg.opt_float("wipe_tower_rotation_angle");
        }
    }
    const GLGizmosManager& gizmos = get_current_canvas3D()->get_canvas_type() == GLCanvas3D::CanvasAssembleView ? assemble_view->get_canvas3d()->get_gizmos_manager() : view3D->get_canvas3d()->get_gizmos_manager();

    if (snapshot_type == UndoRedo::SnapshotType::ProjectSeparator)
        this->undo_redo_stack().clear();
    this->undo_redo_stack().take_snapshot(snapshot_name, model, get_current_canvas3D()->get_canvas_type() == GLCanvas3D::CanvasAssembleView ? assemble_view->get_canvas3d()->get_selection() : view3D->get_canvas3d()->get_selection(), gizmos, partplate_list, snapshot_data);
    if (snapshot_type == UndoRedo::SnapshotType::LeavingGizmoWithAction) {
        // Filter all but the last UndoRedo::SnapshotType::GizmoAction in a row between the last UndoRedo::SnapshotType::EnteringGizmo and UndoRedo::SnapshotType::LeavingGizmoWithAction.
        // The remaining snapshot will be renamed to a more generic name,
        // depending on what gizmo is being left.
        if (gizmos.get_current() != nullptr) {
            std::string new_name = gizmos.get_current()->get_action_snapshot_name();
            this->undo_redo_stack().reduce_noisy_snapshots(new_name);
        }
    } else if (snapshot_type == UndoRedo::SnapshotType::ProjectSeparator) {
        // Reset the "dirty project" flag.
        m_undo_redo_stack_main.mark_current_as_saved();
    }
    //BBS: add PartPlateList as the paremeter for take_snapshot
    this->undo_redo_stack().release_least_recently_used();

    dirty_state.update_from_undo_redo_stack(m_undo_redo_stack_main.project_modified());

    // Save the last active preset name of a particular printer technology.
    ((this->printer_technology == ptFFF) ? m_last_fff_printer_profile_name : m_last_sla_printer_profile_name) = wxGetApp().preset_bundle->printers.get_selected_preset_name();
    BOOST_LOG_TRIVIAL(info) << "Undo / Redo snapshot taken: " << snapshot_name << ", Undo / Redo stack memory: " << Slic3r::format_memsize_MB(this->undo_redo_stack().memsize()) << log_memory_info();
}

void Plater::priv::undo()
{
    const std::vector<UndoRedo::Snapshot> &snapshots = this->undo_redo_stack().snapshots();
    auto it_current = std::lower_bound(snapshots.begin(), snapshots.end(), UndoRedo::Snapshot(this->undo_redo_stack().active_snapshot_time()));
    // BBS: undo-redo until modify record
    while (--it_current != snapshots.begin() && !snapshot_modifies_project(*it_current));
    if (it_current == snapshots.begin()) return;
    if (get_current_canvas3D()->get_canvas_type() == GLCanvas3D::CanvasAssembleView) {
        if (it_current->snapshot_data.snapshot_type != UndoRedo::SnapshotType::GizmoAction &&
            it_current->snapshot_data.snapshot_type != UndoRedo::SnapshotType::EnteringGizmo &&
            it_current->snapshot_data.snapshot_type != UndoRedo::SnapshotType::LeavingGizmoNoAction &&
            it_current->snapshot_data.snapshot_type != UndoRedo::SnapshotType::LeavingGizmoWithAction)
            return;
    }
    this->undo_redo_to(it_current);
}

void Plater::priv::redo()
{
    const std::vector<UndoRedo::Snapshot> &snapshots = this->undo_redo_stack().snapshots();
    auto it_current = std::lower_bound(snapshots.begin(), snapshots.end(), UndoRedo::Snapshot(this->undo_redo_stack().active_snapshot_time()));
    // BBS: undo-redo until modify record
    while (it_current != snapshots.end() && !snapshot_modifies_project(*it_current++));
    if (it_current != snapshots.end()) {
        while (it_current != snapshots.end() && !snapshot_modifies_project(*it_current++));
        this->undo_redo_to(--it_current);
    }
}

void Plater::priv::undo_redo_to(size_t time_to_load)
{
    const std::vector<UndoRedo::Snapshot> &snapshots = this->undo_redo_stack().snapshots();
    auto it_current = std::lower_bound(snapshots.begin(), snapshots.end(), UndoRedo::Snapshot(time_to_load));
    assert(it_current != snapshots.end());
    this->undo_redo_to(it_current);
}

// BBS: check need save or backup
bool Plater::priv::up_to_date(bool saved, bool backup)
{
    size_t& last_time = backup ? m_backup_timestamp : m_saved_timestamp;
    if (saved) {
        last_time = undo_redo_stack_main().active_snapshot_time();
        if (!backup)
            undo_redo_stack_main().mark_current_as_saved();
        return true;
    }
    else {
        return !undo_redo_stack_main().has_real_change_from(last_time);
    }
}

void Plater::priv::undo_redo_to(std::vector<UndoRedo::Snapshot>::const_iterator it_snapshot)
{
    // Make sure that no updating function calls take_snapshot until we are done.
    SuppressSnapshots snapshot_supressor(q);

    bool 				temp_snapshot_was_taken 	= this->undo_redo_stack().temp_snapshot_active();
    PrinterTechnology 	new_printer_technology 		= it_snapshot->snapshot_data.printer_technology;
    bool 				printer_technology_changed 	= this->printer_technology != new_printer_technology;
    if (printer_technology_changed) {
        //BBS do not support SLA
    }
    // Save the last active preset name of a particular printer technology.
    ((this->printer_technology == ptFFF) ? m_last_fff_printer_profile_name : m_last_sla_printer_profile_name) = wxGetApp().preset_bundle->printers.get_selected_preset_name();
    //FIXME updating the Wipe tower config values at the ModelWipeTower from the Print config.
    // This is a workaround until we refactor the Wipe Tower position / orientation to live solely inside the Model, not in the Print config.
    // BBS: add partplate logic
    if (this->printer_technology == ptFFF) {
        const DynamicPrintConfig& config = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        const DynamicPrintConfig& proj_cfg = wxGetApp().preset_bundle->project_config;
        const ConfigOptionFloats* tower_x_opt = proj_cfg.option<ConfigOptionFloats>("wipe_tower_x");
        const ConfigOptionFloats* tower_y_opt = proj_cfg.option<ConfigOptionFloats>("wipe_tower_y");
        assert(tower_x_opt->values.size() == tower_y_opt->values.size());
        model.wipe_tower.positions.clear();
        model.wipe_tower.positions.resize(tower_x_opt->values.size());
        for (int plate_idx = 0; plate_idx < tower_x_opt->values.size(); plate_idx++) {
            ModelWipeTower& tower = model.wipe_tower;

            tower.positions[plate_idx] = Vec2d(tower_x_opt->get_at(plate_idx), tower_y_opt->get_at(plate_idx));
            tower.rotation = proj_cfg.opt_float("wipe_tower_rotation_angle");
        }
    }
    const int layer_range_idx = it_snapshot->snapshot_data.layer_range_idx;
    // Flags made of Snapshot::Flags enum values.
    unsigned int new_flags = it_snapshot->snapshot_data.flags;
    UndoRedo::SnapshotData top_snapshot_data;
    top_snapshot_data.printer_technology = this->printer_technology;
    if (this->view3D->is_layers_editing_enabled())
        top_snapshot_data.flags |= UndoRedo::SnapshotData::VARIABLE_LAYER_EDITING_ACTIVE;
    if (this->sidebar->obj_list()->is_selected(itSettings)) {
        top_snapshot_data.flags |= UndoRedo::SnapshotData::SELECTED_SETTINGS_ON_SIDEBAR;
        top_snapshot_data.layer_range_idx = this->sidebar->obj_list()->get_selected_layers_range_idx();
    }
    else if (this->sidebar->obj_list()->is_selected(itLayer)) {
        top_snapshot_data.flags |= UndoRedo::SnapshotData::SELECTED_LAYER_ON_SIDEBAR;
        top_snapshot_data.layer_range_idx = this->sidebar->obj_list()->get_selected_layers_range_idx();
    }
    else if (this->sidebar->obj_list()->is_selected(itLayerRoot))
        top_snapshot_data.flags |= UndoRedo::SnapshotData::SELECTED_LAYERROOT_ON_SIDEBAR;
    bool   		 new_variable_layer_editing_active = (new_flags & UndoRedo::SnapshotData::VARIABLE_LAYER_EDITING_ACTIVE) != 0;
    bool         new_selected_settings_on_sidebar  = (new_flags & UndoRedo::SnapshotData::SELECTED_SETTINGS_ON_SIDEBAR) != 0;
    bool         new_selected_layer_on_sidebar     = (new_flags & UndoRedo::SnapshotData::SELECTED_LAYER_ON_SIDEBAR) != 0;
    bool         new_selected_layerroot_on_sidebar = (new_flags & UndoRedo::SnapshotData::SELECTED_LAYERROOT_ON_SIDEBAR) != 0;

    if (this->view3D->get_canvas3d()->get_gizmos_manager().wants_reslice_supports_on_undo())
        top_snapshot_data.flags |= UndoRedo::SnapshotData::RECALCULATE_SLA_SUPPORTS;

    // Disable layer editing before the Undo / Redo jump.
    if (!new_variable_layer_editing_active && view3D->is_layers_editing_enabled())
        view3D->get_canvas3d()->force_main_toolbar_left_action(view3D->get_canvas3d()->get_main_toolbar_item_id("layersediting"));

    // Make a copy of the snapshot, undo/redo could invalidate the iterator
    const UndoRedo::Snapshot snapshot_copy = *it_snapshot;
    // Do the jump in time.
    if (it_snapshot->timestamp < this->undo_redo_stack().active_snapshot_time() ?
        this->undo_redo_stack().undo(model, get_current_canvas3D()->get_canvas_type() == GLCanvas3D::CanvasAssembleView ? assemble_view->get_canvas3d()->get_selection() : this->view3D->get_canvas3d()->get_selection(), get_current_canvas3D()->get_canvas_type() == GLCanvas3D::CanvasAssembleView ? assemble_view->get_canvas3d()->get_gizmos_manager() : this->view3D->get_canvas3d()->get_gizmos_manager(), this->partplate_list, top_snapshot_data, it_snapshot->timestamp) :
        this->undo_redo_stack().redo(model, get_current_canvas3D()->get_canvas_type() == GLCanvas3D::CanvasAssembleView ? assemble_view->get_canvas3d()->get_gizmos_manager() : this->view3D->get_canvas3d()->get_gizmos_manager(), this->partplate_list, it_snapshot->timestamp)) {
        if (printer_technology_changed) {
            // Switch to the other printer technology. Switch to the last printer active for that particular technology.
            AppConfig *app_config = wxGetApp().app_config;
            app_config->set("presets", PRESET_PRINTER_NAME, (new_printer_technology == ptFFF) ? m_last_fff_printer_profile_name : m_last_sla_printer_profile_name);
            //FIXME Why are we reloading the whole preset bundle here? Please document. This is fishy and it is unnecessarily expensive.
            // Anyways, don't report any config value substitutions, they have been already reported to the user at application start up.
            wxGetApp().preset_bundle->load_presets(*app_config, ForwardCompatibilitySubstitutionRule::EnableSilent);
            // load_current_presets() calls Tab::load_current_preset() -> TabPrint::update() -> Object_list::update_and_show_object_settings_item(),
            // but the Object list still keeps pointer to the old Model. Avoid a crash by removing selection first.
            this->sidebar->obj_list()->unselect_objects();
            // Load the currently selected preset into the GUI, update the preset selection box.
            // This also switches the printer technology based on the printer technology of the active printer profile.
            wxGetApp().load_current_presets();
        }
        //FIXME updating the Print config from the Wipe tower config values at the ModelWipeTower.
        // This is a workaround until we refactor the Wipe Tower position / orientation to live solely inside the Model, not in the Print config.
        // BBS: add partplate logic
        if (this->printer_technology == ptFFF) {
            const DynamicPrintConfig& config = wxGetApp().preset_bundle->prints.get_edited_preset().config;
            const DynamicPrintConfig& proj_cfg = wxGetApp().preset_bundle->project_config;
            ConfigOptionFloats* tower_x_opt = const_cast<ConfigOptionFloats*>(proj_cfg.option<ConfigOptionFloats>("wipe_tower_x"));
            ConfigOptionFloats* tower_y_opt = const_cast<ConfigOptionFloats*>(proj_cfg.option<ConfigOptionFloats>("wipe_tower_y"));
            // BBS: don't support wipe tower rotation
            //double current_rotation = proj_cfg.opt_float("wipe_tower_rotation_angle");
            bool need_update = false;
            if (tower_x_opt->values.size() != model.wipe_tower.positions.size()) {
                tower_x_opt->clear();
                ConfigOptionFloat default_tower_x(40.f);
                tower_x_opt->resize(model.wipe_tower.positions.size(), &default_tower_x);
                need_update = true;
            }

            if (tower_y_opt->values.size() != model.wipe_tower.positions.size()) {
                tower_y_opt->clear();
                ConfigOptionFloat default_tower_y(200.f);
                tower_y_opt->resize(model.wipe_tower.positions.size(), &default_tower_y);
                need_update = true;
            }

            for (int plate_idx = 0; plate_idx < model.wipe_tower.positions.size(); plate_idx++) {
                if (Vec2d(tower_x_opt->get_at(plate_idx), tower_y_opt->get_at(plate_idx)) != model.wipe_tower.positions[plate_idx]) {
                    ConfigOptionFloat tower_x_new(model.wipe_tower.positions[plate_idx].x());
                    ConfigOptionFloat tower_y_new(model.wipe_tower.positions[plate_idx].y());
                    tower_x_opt->set_at(&tower_x_new, plate_idx, 0);
                    tower_y_opt->set_at(&tower_y_new, plate_idx, 0);
                    need_update = true;
                    break;
                }
            }

            if (need_update) {
                // update print to current plate (preview->m_process)
                this->partplate_list.update_slice_context_to_current_plate(this->background_process);
                this->preview->update_gcode_result(this->partplate_list.get_current_slice_result());
                this->update();
            }
        }
        // set selection mode for ObjectList on sidebar
        this->sidebar->obj_list()->set_selection_mode(new_selected_settings_on_sidebar  ? ObjectList::SELECTION_MODE::smSettings :
                                                      new_selected_layer_on_sidebar     ? ObjectList::SELECTION_MODE::smLayer :
                                                      new_selected_layerroot_on_sidebar ? ObjectList::SELECTION_MODE::smLayerRoot :
                                                                                          ObjectList::SELECTION_MODE::smUndef);
        if (new_selected_settings_on_sidebar || new_selected_layer_on_sidebar)
            this->sidebar->obj_list()->set_selected_layers_range_idx(layer_range_idx);

        this->update_after_undo_redo(snapshot_copy, temp_snapshot_was_taken);
        // Enable layer editing after the Undo / Redo jump.
        if (!view3D->is_layers_editing_enabled() && this->layers_height_allowed() && new_variable_layer_editing_active)
            view3D->get_canvas3d()->force_main_toolbar_left_action(view3D->get_canvas3d()->get_main_toolbar_item_id("layersediting"));
    }

    dirty_state.update_from_undo_redo_stack(m_undo_redo_stack_main.project_modified());
    update_title_dirty_status();
}

void Plater::priv::update_after_undo_redo(const UndoRedo::Snapshot& snapshot, bool /* temp_snapshot_was_taken */)
{
    get_current_canvas3D()->get_canvas_type() == GLCanvas3D::CanvasAssembleView ? assemble_view->get_canvas3d()->get_selection().clear() : this->view3D->get_canvas3d()->get_selection().clear();
    // Update volumes from the deserializd model, always stop / update the background processing (for both the SLA and FFF technologies).
    this->update((unsigned int)UpdateParams::FORCE_BACKGROUND_PROCESSING_UPDATE | (unsigned int)UpdateParams::POSTPONE_VALIDATION_ERROR_MESSAGE);
    // Release old snapshots if the memory allocated is excessive. This may remove the top most snapshot if jumping to the very first snapshot.
    //if (temp_snapshot_was_taken)
    // Release the old snapshots always, as it may have happened, that some of the triangle meshes got deserialized from the snapshot, while some
    // triangle meshes may have gotten released from the scene or the background processing, therefore now being calculated into the Undo / Redo stack size.
        this->undo_redo_stack().release_least_recently_used();
    //YS_FIXME update obj_list from the deserialized model (maybe store ObjectIDs into the tree?) (no selections at this point of time)
        get_current_canvas3D()->get_canvas_type() == GLCanvas3D::CanvasAssembleView ?
            assemble_view->get_canvas3d()->get_selection().set_deserialized(GUI::Selection::EMode(this->undo_redo_stack().selection_deserialized().mode), this->undo_redo_stack().selection_deserialized().volumes_and_instances) :
            this->view3D->get_canvas3d()->get_selection().set_deserialized(GUI::Selection::EMode(this->undo_redo_stack().selection_deserialized().mode), this->undo_redo_stack().selection_deserialized().volumes_and_instances);
    get_current_canvas3D()->get_canvas_type() == GLCanvas3D::CanvasAssembleView ?
        assemble_view->get_canvas3d()->get_gizmos_manager().update_after_undo_redo(snapshot) :
        this->view3D->get_canvas3d()->get_gizmos_manager().update_after_undo_redo(snapshot);

    wxGetApp().obj_list()->update_after_undo_redo();

    if (wxGetApp().get_mode() == comSimple && model_has_advanced_features(this->model)) {
        // If the user jumped to a snapshot that require user interface with advanced features, switch to the advanced mode without asking.
        // There is a little risk of surprising the user, as he already must have had the advanced or advanced mode active for such a snapshot to be taken.
        Slic3r::GUI::wxGetApp().save_mode(comAdvanced);
        view3D->set_as_dirty();
    }

    // this->update() above was called with POSTPONE_VALIDATION_ERROR_MESSAGE, so that if an error message was generated when updating the back end, it would not open immediately,
    // but it would be saved to be show later. Let's do it now. We do not want to display the message box earlier, because on Windows & OSX the message box takes over the message
    // queue pump, which in turn executes the rendering function before a full update after the Undo / Redo jump.
    this->show_delayed_error_message();

    //FIXME what about the state of the manipulators?
    //FIXME what about the focus? Cursor in the side panel?

    BOOST_LOG_TRIVIAL(info) << "Undo / Redo snapshot reloaded. Undo / Redo stack memory: " << Slic3r::format_memsize_MB(this->undo_redo_stack().memsize()) << log_memory_info();
}

void Plater::priv::bring_instance_forward() const
{
#ifdef __APPLE__
    wxGetApp().other_instance_message_handler()->bring_instance_forward();
    return;
#endif //__APPLE__
    if (main_frame == nullptr) {
        BOOST_LOG_TRIVIAL(debug) << "Couldnt bring instance forward - mainframe is null";
        return;
    }
    BOOST_LOG_TRIVIAL(debug) << "Orca Slicer window going forward";
    //this code maximize app window on Fedora
    {
        main_frame->Iconize(false);
        if (main_frame->IsMaximized())
            main_frame->Maximize(true);
        else
            main_frame->Maximize(false);
    }
    //this code maximize window on Ubuntu
    {
        main_frame->Restore();
        wxGetApp().GetTopWindow()->SetFocus();  // focus on my window
        wxGetApp().GetTopWindow()->Raise();  // bring window to front
        wxGetApp().GetTopWindow()->Show(true); // show the window
    }
}

//BBS: popup object table
bool Plater::priv::PopupObjectTable(int object_id, int volume_id, const wxPoint& position)
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" enter, create ObjectTableDialog");
    int max_width{1920}, max_height{1080};

    max_width = q->GetMaxWidth();
    max_height = q->GetMaxHeight();
    ObjectTableDialog table_dialog(q, q, &model, wxSize(max_width, max_height));
    //m_popup_table = new ObjectTableDialog(q, q,  &model);

    wxRect rect = sidebar->GetRect();
    wxPoint pos = sidebar->ClientToScreen(wxPoint(rect.x, rect.y));

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": show ObjectTableDialog");
    table_dialog.Popup(object_id, volume_id, pos);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" finished, will destroy ObjectTableDialog");
    return true;
}

void Sidebar::set_btn_label(const ActionButtonType btn_type, const wxString& label) const
{
    switch (btn_type)
    {
        case ActionButtonType::abReslice:   p->btn_reslice->SetLabelText(label);        break;
        case ActionButtonType::abExport:    p->btn_export_gcode->SetLabelText(label);   break;
        case ActionButtonType::abSendGCode: /*p->btn_send_gcode->SetLabelText(label);*/     break;
    }
}

// Plater / Public

Plater::Plater(wxWindow *parent, MainFrame *main_frame)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxGetApp().get_min_size())
    , p(new priv(this, main_frame))
{
    // Initialization performed in the private c-tor
    enable_wireframe(true);
    m_only_gcode = false;
}

bool Plater::Show(bool show)
{
    if (wxGetApp().mainframe)
        wxGetApp().mainframe->show_option(show);
    return wxPanel::Show(show);
}

bool Plater::is_project_dirty() const { return p->is_project_dirty(); }
bool Plater::is_presets_dirty() const { return p->is_presets_dirty(); }
void Plater::set_plater_dirty(bool is_dirty) { p->set_plater_dirty(is_dirty); }
void Plater::update_project_dirty_from_presets() { p->update_project_dirty_from_presets(); }
int  Plater::save_project_if_dirty(const wxString& reason) { return p->save_project_if_dirty(reason); }
void Plater::reset_project_dirty_after_save() { p->reset_project_dirty_after_save(); }
void Plater::reset_project_dirty_initial_presets() { p->reset_project_dirty_initial_presets(); }
#if ENABLE_PROJECT_DIRTY_STATE_DEBUG_WINDOW
void Plater::render_project_state_debug_window() const { p->render_project_state_debug_window(); }
#endif // ENABLE_PROJECT_DIRTY_STATE_DEBUG_WINDOW

Sidebar&        Plater::sidebar()           { return *p->sidebar; }
const Model&    Plater::model() const       { return p->model; }
Model&          Plater::model()             { return p->model; }
const Print&    Plater::fff_print() const   { return p->fff_print; }
Print&          Plater::fff_print()         { return p->fff_print; }
const SLAPrint& Plater::sla_print() const   { return p->sla_print; }
SLAPrint&       Plater::sla_print()         { return p->sla_print; }

int Plater::new_project(bool skip_confirm, bool silent, const wxString& project_name)
{
    model().calib_pa_pattern.reset(nullptr);
    model().plates_custom_gcodes.clear();

    bool transfer_preset_changes = false;
    // BBS: save confirm
    auto check = [this,&transfer_preset_changes](bool yes_or_no) {
        m_new_project_and_check_state = true;
        wxString header = _L("Some presets are modified.") + "\n" +
            (yes_or_no ? _L("You can keep the modified presets to the new project or discard them") :
                _L("You can keep the modified presets to the new project, discard or save changes as new presets."));
        int act_buttons = ActionButtons::KEEP | ActionButtons::REMEMBER_CHOISE;
        if (!yes_or_no)
            act_buttons |= ActionButtons::SAVE;
        if (m_exported_file) { //.gcode.3mf ignore presets modify
            m_exported_file = false;
        }
        bool result = wxGetApp().check_and_keep_current_preset_changes(_L("Creating a new project"), header, act_buttons, &transfer_preset_changes);
        m_new_project_and_check_state = false;
        return result;
    };
    int result;
    if (!skip_confirm && (result = close_with_confirm(check)) == wxID_CANCEL)
        return wxID_CANCEL;

    m_only_gcode = false;
    m_exported_file = false;
    m_loading_project = false;
    get_notification_manager()->clear_all();

    if (!silent)
        wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);

    //get_partplate_list().reinit();
    //get_partplate_list().update_slice_context_to_current_plate(p->background_process);
    //p->preview->update_gcode_result(p->partplate_list.get_current_slice_result());
    reset(transfer_preset_changes);
    reset_project_dirty_after_save();
    reset_project_dirty_initial_presets();
    wxGetApp().update_saved_preset_from_current_preset();
    update_project_dirty_from_presets();

    //reset project
    p->project.reset();
    //set project name
    if (project_name.empty())
        p->set_project_name(_L("Untitled"));
    else
        p->set_project_name(project_name);

    Plater::TakeSnapshot snapshot(this, "New Project", UndoRedo::SnapshotType::ProjectSeparator);

    Model m;
    model().load_from(m); // new id avoid same path name

    //select first plate
    get_partplate_list().select_plate(0);
    SimpleEvent event(EVT_GLCANVAS_PLATE_SELECT);
    p->on_plate_selected(event);

    p->load_auxiliary_files();
    wxGetApp().app_config->update_last_backup_dir(model().get_backup_path());

    // BBS set default view and zoom
    p->select_view_3D("3D");
    p->select_view("topfront");
    p->camera.requires_zoom_to_bed = true;
    enable_sidebar(!m_only_gcode);

    up_to_date(true, false);
    up_to_date(true, true);
    return wxID_YES;
}

LoadType determine_load_type(std::string filename, std::string override_setting = "");

// BBS: FIXME, missing resotre logic
void Plater::load_project(wxString const& filename2,
    wxString const& originfile)
{
    model().calib_pa_pattern.reset(nullptr);
    model().plates_custom_gcodes.clear();

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "filename is: " << filename2 << "and originfile is: " << originfile;
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__;
    auto filename = filename2;
    auto check = [&filename, this] (bool yes_or_no) {
        if (!yes_or_no && !wxGetApp().check_and_save_current_preset_changes(_L("Load project"),
                _L("Some presets are modified.")))
            return false;
        if (filename.empty()) {
            // Ask user for a project file name.
            wxGetApp().load_project(this, filename);
        }
        return !filename.empty();
    };

    // BSS: save project, force close
    int result;
    if ((result = close_with_confirm(check)) == wxID_CANCEL) {
        return;
    }

    // BBS
    if (m_loading_project) {
        //some error cases happens
        //return directly
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": current loading other project, return directly");
        return;
    }
    else
        m_loading_project = true;

    m_only_gcode = false;
    m_exported_file = false;
    get_notification_manager()->bbl_close_plateinfo_notification();
    get_notification_manager()->bbl_close_preview_only_notification();
    get_notification_manager()->bbl_close_3mf_warn_notification();
    get_notification_manager()->close_notification_of_type(NotificationType::PlaterError);
    get_notification_manager()->close_notification_of_type(NotificationType::PlaterWarning);
    get_notification_manager()->close_notification_of_type(NotificationType::SlicingError);
    get_notification_manager()->close_notification_of_type(NotificationType::SlicingSeriousWarning);
    get_notification_manager()->close_notification_of_type(NotificationType::SlicingWarning);

    auto path     = into_path(filename);

    auto strategy = LoadStrategy::LoadModel | LoadStrategy::LoadConfig;
    if (originfile == "<silence>") {
        strategy = strategy | LoadStrategy::Silence;
    } else if (originfile == "<loadall>") {
        // Do nothing
    } else if (originfile != "-") {
        strategy = strategy | LoadStrategy::Restore;
    } else {
        switch (determine_load_type(filename.ToStdString())) {
            case LoadType::OpenProject: break; // Do nothing
            case LoadType::LoadGeometry:; strategy = LoadStrategy::LoadModel; break;
            default: return; // User cancelled
        }
    }
    bool load_restore = strategy & LoadStrategy::Restore;

    // Take the Undo / Redo snapshot.
    reset();

    Plater::TakeSnapshot snapshot(this, "Load Project", UndoRedo::SnapshotType::ProjectSeparator);

    std::vector<fs::path> input_paths;
    input_paths.push_back(path);
    if (strategy & LoadStrategy::Restore)
        input_paths.push_back(into_u8(originfile));

    std::vector<size_t> res = load_files(input_paths, strategy);

    reset_project_dirty_initial_presets();
    update_project_dirty_from_presets();
    wxGetApp().preset_bundle->export_selections(*wxGetApp().app_config);

    // if res is empty no data has been loaded
    if (!res.empty() && (load_restore || !(strategy & LoadStrategy::Silence))) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << __LINE__ << " call set_project_filename: " << (load_restore ? originfile : filename);
        p->set_project_filename(load_restore ? originfile : filename);
        if (load_restore && originfile.IsEmpty()) {
        p->set_project_name(_L("Untitled"));
        }

    } else {
        if (using_exported_file()) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << __LINE__ << " using ecported set project filename: " << filename;
            p->set_project_filename(filename);
        }

    }

    // BBS set default 3D view and direction after loading project
    //p->select_view_3D("3D");
    if (!m_exported_file) {
        p->select_view("topfront");
        p->camera.requires_zoom_to_plate = REQUIRES_ZOOM_TO_ALL_PLATE;
        wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);
    }
    else {
        p->partplate_list.select_plate_view();
    }

    enable_sidebar(!m_only_gcode);

    wxGetApp().app_config->update_last_backup_dir(model().get_backup_path());
    if (load_restore && !originfile.empty()) {
        wxGetApp().app_config->update_skein_dir(into_path(originfile).parent_path().string());
        wxGetApp().app_config->update_config_dir(into_path(originfile).parent_path().string());
    }

    if (!load_restore)
        up_to_date(true, false);
    else
        p->dirty_state.update_from_undo_redo_stack(true);
    up_to_date(true, true);

    wxGetApp().params_panel()->switch_to_object_if_has_object_configs();

    auto       has_modify = is_flush_config_modified();
    sidebar().set_flushing_volume_warning(has_modify);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << __LINE__ << " load project done";
    m_loading_project = false;
}

// BBS: save logic
int Plater::save_project(bool saveAs)
{
    //if (up_to_date(false, false)) // should we always save
    //    return;
    auto filename = get_project_filename(".3mf");
    if (!saveAs && filename.IsEmpty())
        saveAs = true;
    if (saveAs)
        filename = p->get_export_file(FT_3MF);
    if (filename.empty())
        return wxID_NO;
    if (filename == "<cancel>")
        return wxID_CANCEL;

    //BBS export 3mf without gcode
    auto save_strategy = SaveStrategy::SplitModel | SaveStrategy::ShareMesh;
    bool full_pathnames = wxGetApp().app_config->get_bool("export_sources_full_pathnames");
    if (full_pathnames) {
        save_strategy = save_strategy | SaveStrategy::FullPathSources;
    }
    if (export_3mf(into_path(filename), save_strategy) < 0) {
        MessageDialog(this, _L("Failed to save the project.\nPlease check whether the folder exists online or if other programs open the project file."),
            _L("Save project"), wxOK | wxICON_WARNING).ShowModal();
        return wxID_CANCEL;
    }

    Slic3r::remove_backup(model(), false);

    p->set_project_filename(filename);
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << __LINE__ << " call set_project_filename: " << filename;

    up_to_date(true, false);
    up_to_date(true, true);

    wxGetApp().update_saved_preset_from_current_preset();
    reset_project_dirty_after_save();
    try {
        json j;
        boost::uintmax_t size = boost::filesystem::file_size(into_path(filename));
        j["file_size"] = size;
        j["file_name"] = std::string(filename.mb_str());

        NetworkAgent* agent = wxGetApp().getAgent();
    }
    catch (...) {}

    update_title_dirty_status();
    return wxID_YES;
}

//BBS import model by model id
void Plater::import_model_id(wxString download_info)
{
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << __LINE__ << " download info: " << download_info;

    wxString download_origin_url = download_info;
    wxString download_url;
    wxString filename;
    wxString separator = "&name=";

    try
    {
        size_t namePos = download_info.Find(separator);
        if (namePos != wxString::npos) {
            download_url = download_info.Mid(0, namePos);
            filename = download_info.Mid(namePos + separator.Length());

        }
        else {
            fs::path download_path = fs::path(download_origin_url.wx_str());
            download_url = download_origin_url;
            filename = download_path.filename().string();
        }

    }
    catch (const std::exception&)
    {
        //wxString sError = error.what();
    }

    bool download_ok = false;
    int retry_count = 0;
    const int max_retries = 3;

    /* jump to 3D eidtor */
    wxGetApp().mainframe->select_tab((size_t)MainFrame::TabPosition::tp3DEditor);

    /* prepare progress dialog */
    bool cont = true;
    bool cont_dlg = true;
    bool cancel = false;
    wxString msg;
    wxString dlg_title = _L("Importing Model");

    int percent = 0;
    ProgressDialog dlg(dlg_title,
        wxString(' ', 100) + "\n\n\n\n",
        100,    // range
        this,   // parent
        wxPD_CAN_ABORT |
        wxPD_APP_MODAL |
        wxPD_AUTO_HIDE |
        wxPD_SMOOTH);

    boost::filesystem::path target_path;

    //reset params
    p->project.reset();

    /* prepare project and profile */
    boost::thread import_thread = Slic3r::create_thread([&percent, &cont, &cancel, &retry_count, max_retries, &msg, &target_path, &download_ok, download_url, &filename] {

        // Orca: NetworkAgent is not needed and only prevents this from running
//        NetworkAgent* m_agent = Slic3r::GUI::wxGetApp().getAgent();
//        if (!m_agent) return;

        int res = 0;
        std::string http_body;

        msg = _L("prepare 3MF file...");

        //gets the number of files with the same name
        std::vector<wxString>   vecFiles;
        bool                    is_already_exist = false;


        target_path = fs::path(wxGetApp().app_config->get("download_path"));

        try
        {
            vecFiles.clear();
            wxString extension = fs::path(filename.wx_str()).extension().c_str();


            //check file suffix
            if (!extension.Contains(".3mf")) {
                msg = _L("Download failed, unknown file format.");
                return;
            }

            auto name = filename.substr(0, filename.length() - extension.length() - 1);

            for (const auto& iter : boost::filesystem::directory_iterator(target_path))
            {
                if (boost::filesystem::is_directory(iter.path()))
                    continue;

                wxString sFile = iter.path().filename().string().c_str();
                if (strstr(sFile.c_str(), name.c_str()) != NULL) {
                    vecFiles.push_back(sFile);
                }

                if (sFile == filename) is_already_exist = true;
            }
        }
        catch (const std::exception&)
        {
            //wxString sError = error.what();
        }

        //update filename
        if (is_already_exist && vecFiles.size() >= 1) {
            wxString extension = fs::path(filename.wx_str()).extension().c_str();
            wxString name = filename.substr(0, filename.length() - extension.length());
            filename = wxString::Format("%s(%d)%s", name, vecFiles.size() + 1, extension).ToStdString();
        }


        msg = _L("downloading project...");

        //target_path = wxStandardPaths::Get().GetTempDir().utf8_str().data();


        //target_path = wxGetApp().get_local_models_path().c_str();
        boost::uuids::uuid uuid = boost::uuids::random_generator()();
        std::string unique = to_string(uuid).substr(0, 6);

        if (filename.empty()) {
            filename = "untitled.3mf";
        }

        //target_path /= (boost::format("%1%_%2%.3mf") % filename % unique).str();
        target_path /= fs::path(filename.wc_str());

        fs::path tmp_path = target_path;
        tmp_path += format(".%1%", ".download");

        auto filesize = 0;
        bool size_limit = false;
        auto http = Http::get(download_url.ToStdString());

        while (cont && retry_count < max_retries) {
            retry_count++;
            http.on_progress([&percent, &cont, &msg, &filesize, &size_limit](Http::Progress progress, bool& cancel) {

                    if (!cont) cancel = true;
                    if (progress.dltotal != 0) {

                        if (filesize == 0) {
                            filesize = progress.dltotal;
                            double megabytes = static_cast<double>(progress.dltotal) / (1024 * 1024);
                            //The maximum size of a 3mf file is 500mb
                            if (megabytes > 500) {
                                cont = false;
                                size_limit = true;
                            }
                        }
                        percent = progress.dlnow * 100 / progress.dltotal;
                    }

                    if (size_limit) {
                        msg = _L("Download failed, File size exception.");
                    }
                    else {
                        msg = wxString::Format(_L("Project downloaded %d%%"), percent);
                    }
                })
                .on_error([&msg, &cont, &retry_count, max_retries](std::string body, std::string error, unsigned http_status) {
                    (void)body;
                    BOOST_LOG_TRIVIAL(error) << format("Error getting: `%1%`: HTTP %2%, %3%",
                        body,
                        http_status,
                        error);

                    if (retry_count == max_retries) {
                        msg = _L("Importing to Orca Slicer failed. Please download the file and manually import it.");
                        cont = false;
                    }
                })
                .on_complete([&cont, &download_ok, tmp_path, target_path](std::string body, unsigned /* http_status */) {
                        fs::fstream file(tmp_path, std::ios::out | std::ios::binary | std::ios::trunc);
                        file.write(body.c_str(), body.size());
                        file.close();
                        fs::rename(tmp_path, target_path);
                        cont = false;
                        download_ok = true;
                }).perform_sync();

                // for break while
                //cont = false;
        }

    });

    while (cont && cont_dlg) {
        wxMilliSleep(50);
        cont_dlg = dlg.Update(percent, msg);
        if (!cont_dlg) {
            cont = cont_dlg;
            cancel = true;
        }

        if (download_ok)
            break;
    }

    if (import_thread.joinable())
        import_thread.join();

    dlg.Hide();

#ifdef _WIN32
    dlg.Close();
#endif

    if (download_ok) {
        BOOST_LOG_TRIVIAL(trace) << "import_model_id: target_path = " << target_path.string();
        /* load project */
        // Orca: If download is a zip file, treat it as if file has been drag and dropped on the plater
        if (target_path.extension() == ".zip")
            this->load_files(wxArrayString(1, target_path.string()));
        else
            this->load_project(target_path.wstring());
        /*BBS set project info after load project, project info is reset in load project */
        //p->project.project_model_id = model_id;
        //p->project.project_design_id = design_id;
        AppConfig* config = wxGetApp().app_config;
        if (config) {
            p->project.project_country_code = config->get_country_code();
        }

        // show save new project
        p->set_project_filename(target_path.wstring());
        p->notification_manager->push_import_finished_notification(target_path.string(), target_path.parent_path().string(), false);
    }
    else {
        if (!msg.empty()) {
            MessageDialog msg_wingow(nullptr, msg, wxEmptyString, wxICON_WARNING | wxOK);
            msg_wingow.SetSize(wxSize(FromDIP(480), -1));
            msg_wingow.ShowModal();
        }
        return;
    }
}
//BBS download project by project id
void Plater::download_project(const wxString& project_id)
{
    return;
}

void Plater::request_model_download(wxString url)
{
    wxCommandEvent* event = new wxCommandEvent(EVT_IMPORT_MODEL_ID);
    event->SetString(url);
    wxQueueEvent(this, event);
}

void Plater::request_download_project(std::string project_id)
{
    wxCommandEvent* event = new wxCommandEvent(EVT_DOWNLOAD_PROJECT);
    event->SetString(project_id);
    wxQueueEvent(this, event);
}

// BBS: save logic
bool Plater::up_to_date(bool saved, bool backup)
{
    if (saved) {
        Slic3r::clear_other_changes(backup);
        return p->up_to_date(saved, backup);
    }
    return p->model.objects.empty() || (p->up_to_date(saved, backup) &&
                                        !Slic3r::has_other_changes(backup));
}

void Plater::add_model(bool imperial_units, std::string fname)
{
    wxArrayString input_files;

    std::vector<fs::path> paths;
    if (fname.empty()) {
        wxGetApp().import_model(this, input_files);
        if (input_files.empty())
            return;

        for (const auto& file : input_files)
            paths.emplace_back(into_path(file));
    }
    else {
        paths.emplace_back(fname);
    }

    std::string snapshot_label;
    assert(! paths.empty());
    if (paths.size() == 1) {
        snapshot_label = "Import Object";
        snapshot_label += ": ";
        snapshot_label += encode_path(paths.front().filename().string().c_str());
    } else {
        snapshot_label = "Import Objects";
        snapshot_label += ": ";
        snapshot_label += paths.front().filename().string().c_str();
        for (size_t i = 1; i < paths.size(); ++ i) {
            snapshot_label += ", ";
            snapshot_label += encode_path(paths[i].filename().string().c_str());
        }
    }

    Plater::TakeSnapshot snapshot(this, snapshot_label);

    // BBS: check file types
    auto loadfiles_type  = LoadFilesType::NoFile;
    auto amf_files_count = get_3mf_file_count(paths);

    if (paths.size() > 1 && amf_files_count < paths.size()) { loadfiles_type = LoadFilesType::Multiple3MFOther; }
    if (paths.size() > 1 && amf_files_count == paths.size()) { loadfiles_type = LoadFilesType::Multiple3MF; }
    if (paths.size() > 1 && amf_files_count == 0) { loadfiles_type = LoadFilesType::MultipleOther; }
    if (paths.size() == 1 && amf_files_count == 1) { loadfiles_type = LoadFilesType::Single3MF; };
    if (paths.size() == 1 && amf_files_count == 0) { loadfiles_type = LoadFilesType::SingleOther; };

    bool ask_multi = false;

    if (loadfiles_type == LoadFilesType::MultipleOther)
        ask_multi = true;

    auto strategy = LoadStrategy::LoadModel;
    if (imperial_units) strategy = strategy | LoadStrategy::ImperialUnits;
    if (!load_files(paths, strategy, ask_multi).empty()) {

        if (get_project_name() == _L("Untitled") && paths.size() > 0) {
            boost::filesystem::path full_path(paths[0].string());
            p->set_project_name(from_u8(full_path.stem().string()));
        }

        wxGetApp().mainframe->update_title();
    }
}

void Plater::calib_pa(const Calib_Params& params)
{
    const auto calib_pa_name = wxString::Format(L"Pressure Advance Test");
    new_project(false, false, calib_pa_name);
    wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));
    switch (params.mode) {
        case CalibMode::Calib_PA_Line:
            add_model(false, Slic3r::resources_dir() + "/calib/pressure_advance/pressure_advance_test.stl");
            break;
        case CalibMode::Calib_PA_Pattern:
            _calib_pa_pattern(params);
            break;
        case CalibMode::Calib_PA_Tower:
            _calib_pa_tower(params);
            break;
        default: break;
    }
    auto printer_config = &wxGetApp().preset_bundle->printers.get_edited_preset().config;
    printer_config->set_key_value("resonance_avoidance", new ConfigOptionBool{false});
    p->background_process.fff_print()->set_calib_params(params);
}

void Plater::_calib_pa_pattern(const Calib_Params& params)
{
    std::vector<double> speeds{params.speeds};
    std::vector<double> accels{params.accelerations};
    std::vector<size_t> object_idxs{};
    /* Set common parameters */
    auto printer_config = &wxGetApp().preset_bundle->printers.get_edited_preset().config;
    DynamicPrintConfig& print_config = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    auto filament_config = &wxGetApp().preset_bundle->filaments.get_edited_preset().config;
    double nozzle_diameter = printer_config->option<ConfigOptionFloats>("nozzle_diameter")->get_at(0);
    filament_config->set_key_value("filament_retract_when_changing_layer", new ConfigOptionBoolsNullable{false});
    filament_config->set_key_value("filament_wipe", new ConfigOptionBoolsNullable{false});
    printer_config->set_key_value("wipe", new ConfigOptionBools{false});
    printer_config->set_key_value("retract_when_changing_layer", new ConfigOptionBools{false});
    printer_config->set_key_value("resonance_avoidance", new ConfigOptionBool{false});

    //Orca: find acceleration to use in the test
    auto accel = print_config.option<ConfigOptionFloat>("outer_wall_acceleration")->value; // get the outer wall acceleration
    if (accel == 0) // if outer wall accel isnt defined, fall back to inner wall accel
        accel = print_config.option<ConfigOptionFloat>("inner_wall_acceleration")->value;
    if (accel == 0) // if inner wall accel is not defined fall back to default accel
        accel = print_config.option<ConfigOptionFloat>("default_acceleration")->value;
    // Orca: Set all accelerations except first layer, as the first layer accel doesnt affect the PA test since accel
    // is set to the travel accel before printing the pattern.
    if (accels.empty()) {
        accels.assign({accel});
        const auto msg{_L("INFO:") + "\n" +
                       _L("No accelerations provided for calibration. Use default acceleration value ") + std::to_string(long(accel)) + _L(u8"mm/s²")};
        get_notification_manager()->push_notification(msg.ToStdString());
    } else {
        // set max acceleration in case of batch mode to get correct test pattern size
        accel = *std::max_element(accels.begin(), accels.end());
    }
    print_config.set_key_value( "outer_wall_acceleration", new ConfigOptionFloat(accel));
    print_config.set_key_value( "print_sequence", new ConfigOptionEnum(PrintSequence::ByLayer));
    
    //Orca: find jerk value to use in the test
    if(!has_junction_deviation(printer_config) && print_config.option<ConfigOptionFloat>("default_jerk")->value > 0){ // we have set a jerk value
        auto jerk = print_config.option<ConfigOptionFloat>("outer_wall_jerk")->value; // get outer wall jerk
        if (jerk == 0) // if outer wall jerk is not defined, get inner wall jerk
            jerk = print_config.option<ConfigOptionFloat>("inner_wall_jerk")->value;
        if (jerk == 0) // if inner wall jerk is not defined, get the default jerk
            jerk = print_config.option<ConfigOptionFloat>("default_jerk")->value;
        
        //Orca: Set jerk values. Again first layer jerk should not matter as it is reset to the travel jerk before the
        // first PA pattern is printed.
        print_config.set_key_value( "default_jerk", new ConfigOptionFloat(jerk));
        print_config.set_key_value( "outer_wall_jerk", new ConfigOptionFloat(jerk));
        print_config.set_key_value( "inner_wall_jerk", new ConfigOptionFloat(jerk));
        print_config.set_key_value( "top_surface_jerk", new ConfigOptionFloat(jerk));
        print_config.set_key_value( "infill_jerk", new ConfigOptionFloat(jerk));
        print_config.set_key_value( "travel_jerk", new ConfigOptionFloat(jerk));
    }

    if (has_junction_deviation(printer_config)){
        print_config.set_key_value("default_junction_deviation", new ConfigOptionFloat(0));
    }

    for (const auto& opt : SuggestedConfigCalibPAPattern().float_pairs) {
        print_config.set_key_value(
            opt.first,
            new ConfigOptionFloat(opt.second)
        );
    }

    for (const auto& opt : SuggestedConfigCalibPAPattern().nozzle_ratio_pairs) {
        print_config.set_key_value(
            opt.first,
            new ConfigOptionFloatOrPercent(nozzle_diameter * opt.second / 100, false)
        );
    }

    for (const auto& opt : SuggestedConfigCalibPAPattern().int_pairs) {
        print_config.set_key_value(
            opt.first,
            new ConfigOptionInt(opt.second)
        );
    }

    print_config.set_key_value(SuggestedConfigCalibPAPattern().brim_pair.first,
        new ConfigOptionEnum<BrimType>(SuggestedConfigCalibPAPattern().brim_pair.second));

    print_config.set_key_value("enable_wrapping_detection", new ConfigOptionBool(false));

    // Orca: Set the outer wall speed to the optimal speed for the test, cap it with max volumetric speed
    if (speeds.empty()) {
        double speed = CalibPressureAdvance::find_optimal_PA_speed(
            wxGetApp().preset_bundle->full_config(),
            print_config.get_abs_value("line_width", nozzle_diameter),
            print_config.get_abs_value("layer_height"), 0, 0);
        print_config.set_key_value("outer_wall_speed", new ConfigOptionFloat(speed));

        speeds.assign({speed});
        const auto msg{_L("INFO:") + "\n" +
                       _L("No speeds provided for calibration. Use default optimal speed ") + std::to_string(long(speed)) + _L("mm/s")};
        get_notification_manager()->push_notification(msg.ToStdString());
    } else if (speeds.size() == 1) {
        // If we have single value provided, set speed using global configuration.
        // per-object config is not set in this case
        print_config.set_key_value("outer_wall_speed", new ConfigOptionFloat(speeds.front()));
    }

    wxGetApp().get_tab(Preset::TYPE_PRINT)->update_dirty();
    wxGetApp().get_tab(Preset::TYPE_FILAMENT)->update_dirty();
    wxGetApp().get_tab(Preset::TYPE_PRINTER)->update_dirty();
    wxGetApp().get_tab(Preset::TYPE_PRINT)->reload_config();
    wxGetApp().get_tab(Preset::TYPE_FILAMENT)->reload_config();
    wxGetApp().get_tab(Preset::TYPE_PRINTER)->reload_config();

    const DynamicPrintConfig full_config = wxGetApp().preset_bundle->full_config();
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    const bool is_bbl_machine = preset_bundle->is_bbl_vendor();
    auto cur_plate = get_partplate_list().get_plate(0);

    // add "handle" cube
    sidebar().obj_list()->load_generic_subobject("Cube", ModelVolumeType::INVALID);
    auto *cube = model().objects[0];

    CalibPressureAdvancePattern pa_pattern(
        params,
        full_config,
        is_bbl_machine,
        *cube,
        cur_plate->get_origin()
    );

    /* Having PA pattern configured, we could make a set of polygons resembling N test patterns.
     * We'll arrange this set of polygons, so we would know position of each test pattern and
     * could position test cubes later on
     *
     * We'll take advantage of already existing cube: scale it up to test pattern size to use
     * as a reference for objects arrangement. Polygon is slightly oversized to add spaces between patterns.
     * That arrangement will be used to place 'handle cubes' for each test. */
    auto cube_bb = cube->raw_bounding_box();
    cube->scale((pa_pattern.print_size_x() + 4) / cube_bb.size().x(),
                (pa_pattern.print_size_y() + 4) / cube_bb.size().y(),
                pa_pattern.max_layer_z() / cube_bb.size().z());

    arrangement::ArrangePolygons arranged_items;
    {
        arrangement::ArrangeParams ap;
        Points bedpts = arrangement::get_shrink_bedpts(&full_config, ap);

        for(size_t i = 0; i < speeds.size() * accels.size(); i++) {
            arrangement::ArrangePolygon p;
            cube->instances[0]->get_arrange_polygon(&p);
            p.bed_idx = 0;
            arranged_items.emplace_back(p);
        }

        arrangement::arrange(arranged_items, bedpts, ap);
    }

    /* scale cube back to the size of test pattern 'handle' */
    cube_bb = cube->raw_bounding_box();
    cube->scale(pa_pattern.handle_xy_size() / cube_bb.size().x(),
                pa_pattern.handle_xy_size() / cube_bb.size().y(),
                pa_pattern.max_layer_z() / cube_bb.size().z());

    /* Set speed and acceleration on per-object basis and arrange anchor object on the plates.
     * Test gcode will be genecated during plate slicing */
    for(size_t test_idx = 0; test_idx < arranged_items.size(); test_idx++) {
        const auto &ai = arranged_items[test_idx];
        size_t plate_idx = arranged_items[test_idx].bed_idx;
        auto tspd = speeds[test_idx % speeds.size()];
        auto tacc = accels[test_idx / speeds.size()];

        /* make an own copy of anchor cube for each test */
        auto obj = test_idx == 0 ? cube : model().add_object(*cube);
        auto obj_idx = std::distance(model().objects.begin(), std::find(model().objects.begin(), model().objects.end(), obj));
        obj->name.assign(std::string("pa_pattern_") + std::to_string(int(tspd)) + std::string("_") + std::to_string(int(tacc)));

        auto &obj_config = obj->config;
        if (speeds.size() > 1)
            obj_config.set_key_value("outer_wall_speed", new ConfigOptionFloat(tspd));
        if (accels.size() > 1)
            obj_config.set_key_value("outer_wall_acceleration", new ConfigOptionFloat(tacc));

        auto cur_plate = get_partplate_list().get_plate(plate_idx);
        if (!cur_plate) {
            plate_idx = get_partplate_list().create_plate();
            cur_plate = get_partplate_list().get_plate(plate_idx);
        }

        object_idxs.emplace_back(obj_idx);
        get_partplate_list().add_to_plate(obj_idx, 0, plate_idx);
        const Vec3d obj_offset{unscale<double>(ai.translation(X)),
                               unscale<double>(ai.translation(Y)),
                               0};
        obj->instances[0]->set_offset(cur_plate->get_origin() + obj_offset + pa_pattern.handle_pos_offset());
        obj->ensure_on_bed();

        if (obj_idx == 0)
            sidebar().obj_list()->update_name_for_items();
        else
            sidebar().obj_list()->add_object_to_list(obj_idx);
    }

    model().calib_pa_pattern = std::make_unique<CalibPressureAdvancePattern>(pa_pattern);
    changed_objects(object_idxs);
}

void Plater::_calib_pa_pattern_gen_gcode()
{
    if (!model().calib_pa_pattern)
        return;

    auto cur_plate = get_partplate_list().get_curr_plate();
    if (cur_plate->empty())
        return;

    /* Container to store custom g-codes genereted by the test generator.
     * We'll store gcode for all tests on a single plate here. Once the plate handling is done,
     * all the g-codes will be merged into a single one on per-layer basis */
    std::vector<CustomGCode::Info> mgc;
    PresetBundle *preset_bundle = wxGetApp().preset_bundle;

    /* iterate over all cubes on current plate and generate gcode for them */
    for (auto obj : cur_plate->get_objects_on_this_plate()) {
        auto gcode = model().calib_pa_pattern->generate_custom_gcodes(
                                preset_bundle->full_config(),
                                preset_bundle->is_bbl_vendor(),
                                *obj,
                                cur_plate->get_origin()
        );
        mgc.emplace_back(gcode);
    }

    // move first item into model custom gcode
    auto &pcgc = model().plates_custom_gcodes[get_partplate_list().get_curr_plate_index()];
    pcgc = std::move(mgc[0]);
    mgc.erase(mgc.begin());

    // concat layer gcodes for each test
    for (size_t i = 0; i < pcgc.gcodes.size(); i++) {
        for (auto &gc : mgc) {
            pcgc.gcodes[i].extra += gc.gcodes[i].extra;
        }
    }
}

void Plater::cut_horizontal(size_t obj_idx, size_t instance_idx, double z, ModelObjectCutAttributes attributes)
{
    wxCHECK_RET(obj_idx < p->model.objects.size(), "obj_idx out of bounds");
    auto *object = p->model.objects[obj_idx];

    wxCHECK_RET(instance_idx < object->instances.size(), "instance_idx out of bounds");

    if (! attributes.has(ModelObjectCutAttribute::KeepUpper) && ! attributes.has(ModelObjectCutAttribute::KeepLower))
        return;

    wxBusyCursor wait;

    const Vec3d instance_offset = object->instances[instance_idx]->get_offset();
    Cut         cut(object, instance_idx, Geometry::translation_transform(z * Vec3d::UnitZ() - instance_offset), attributes);
    const auto  new_objects = cut.perform_with_plane();

    apply_cut_object_to_model(obj_idx, new_objects);
}

void Plater::_calib_pa_tower(const Calib_Params& params) {
    add_model(false, Slic3r::resources_dir() + "/calib/pressure_advance/tower_with_seam.stl");

    auto& print_config = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    auto printer_config = &wxGetApp().preset_bundle->printers.get_edited_preset().config;
    auto filament_config = &wxGetApp().preset_bundle->filaments.get_edited_preset().config;

    const double nozzle_diameter = printer_config->option<ConfigOptionFloats>("nozzle_diameter")->get_at(0);

    print_config.set_key_value("enable_wrapping_detection", new ConfigOptionBool(false));
    filament_config->set_key_value("slow_down_layer_time", new ConfigOptionFloats{ 1.0f });


    auto& obj_cfg = model().objects[0]->config;

    obj_cfg.set_key_value("alternate_extra_wall", new ConfigOptionBool(false));
    auto full_config = wxGetApp().preset_bundle->full_config();
    auto wall_speed = CalibPressureAdvance::find_optimal_PA_speed(
        full_config, full_config.get_abs_value("line_width", nozzle_diameter),
        full_config.get_abs_value("layer_height"), 0, 0);
    obj_cfg.set_key_value("outer_wall_speed", new ConfigOptionFloat(wall_speed));
    obj_cfg.set_key_value("inner_wall_speed", new ConfigOptionFloat(wall_speed));
    obj_cfg.set_key_value("seam_position", new ConfigOptionEnum<SeamPosition>(spRear));
    obj_cfg.set_key_value("wall_loops", new ConfigOptionInt(2));
    obj_cfg.set_key_value("top_shell_layers", new ConfigOptionInt(0));
    obj_cfg.set_key_value("bottom_shell_layers", new ConfigOptionInt(0));
    obj_cfg.set_key_value("sparse_infill_density", new ConfigOptionPercent(0));
    obj_cfg.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btEar));
    obj_cfg.set_key_value("brim_object_gap", new ConfigOptionFloat(.0f));
    obj_cfg.set_key_value("brim_ears_max_angle", new ConfigOptionFloat(135.f));
    obj_cfg.set_key_value("brim_width", new ConfigOptionFloat(6.f));
    obj_cfg.set_key_value("seam_slope_type", new ConfigOptionEnum<SeamScarfType>(SeamScarfType::None));
    print_config.set_key_value("max_volumetric_extrusion_rate_slope", new ConfigOptionFloat(0));

    changed_objects({ 0 });
    wxGetApp().get_tab(Preset::TYPE_PRINT)->update_dirty();
    wxGetApp().get_tab(Preset::TYPE_FILAMENT)->update_dirty();
    wxGetApp().get_tab(Preset::TYPE_PRINTER)->update_dirty();
    wxGetApp().get_tab(Preset::TYPE_PRINT)->reload_config();
    wxGetApp().get_tab(Preset::TYPE_FILAMENT)->reload_config();
    wxGetApp().get_tab(Preset::TYPE_PRINTER)->reload_config();

    auto new_height = std::ceil((params.end - params.start) / params.step) + 1;
    auto obj_bb = model().objects[0]->bounding_box_exact();
    if (new_height < obj_bb.size().z()) {
        cut_horizontal(0, 0, new_height, ModelObjectCutAttribute::KeepLower);
    }

    _calib_pa_select_added_objects();
}

void Plater::_calib_pa_select_added_objects() {
    // update printable state for new volumes on canvas3D
    wxGetApp().plater()->canvas3D()->update_instance_printable_state_for_objects({0});

    Selection& selection = p->view3D->get_canvas3d()->get_selection();
    selection.clear();
    selection.add_object(0, false);

    // BBS: update object list selection
    p->sidebar->obj_list()->update_selections();
    selection.notify_instance_update(-1, -1);
    if (p->view3D->get_canvas3d()->get_gizmos_manager().is_enabled()) {
        // this is required because the selected object changed and the flatten on face an sla support gizmos need to be updated accordingly
        p->view3D->get_canvas3d()->update_gizmos_on_off_state();
    }
}

// Adjust settings for flowrate calibration
// For linear mode, pass 1 means normal version while pass 2 mean "for perfectionists" version
void adjust_settings_for_flowrate_calib(ModelObjectPtrs& objects, bool linear, int pass)
{
    auto print_config = &wxGetApp().preset_bundle->prints.get_edited_preset().config;
    auto printerConfig = &wxGetApp().preset_bundle->printers.get_edited_preset().config;
    auto filament_config = &wxGetApp().preset_bundle->filaments.get_edited_preset().config;

    /// --- scale ---
    // model is created for a 0.4 nozzle, scale z with nozzle size.
    const ConfigOptionFloats* nozzle_diameter_config = printerConfig->option<ConfigOptionFloats>("nozzle_diameter");
    assert(nozzle_diameter_config->values.size() > 0);
    float nozzle_diameter = nozzle_diameter_config->values[0];
    float xyScale = nozzle_diameter / 0.6;
    //scale z to have 10 layers
    // 2 bottom, 5 top, 3 sparse infill
    double first_layer_height = print_config->option<ConfigOptionFloat>("initial_layer_print_height")->value;
    double layer_height = nozzle_diameter / 2.0; // prefer 0.2 layer height for 0.4 nozzle
    first_layer_height = std::max(first_layer_height, layer_height);

    const auto canvas    = wxGetApp().plater()->canvas3D();
    auto&      selection = canvas->get_selection();
    selection.setup_cache();
    TransformationType transformation_type;
    transformation_type.set_relative();
    float zscale = (first_layer_height + 9 * layer_height) / 2;
    // only enlarge
    if (xyScale > 1.2) {
        selection.scale({xyScale, xyScale, zscale}, transformation_type);
    } else {
        selection.scale({1, 1, zscale}, transformation_type);
    }
    canvas->do_scale("");

    auto cur_flowrate = filament_config->option<ConfigOptionFloats>("filament_flow_ratio")->get_at(0);
    Flow infill_flow = Flow(nozzle_diameter * 1.2f, layer_height, nozzle_diameter);
    double filament_max_volumetric_speed = filament_config->option<ConfigOptionFloats>("filament_max_volumetric_speed")->get_at(0);
    double max_infill_speed;
    if (linear)
        max_infill_speed = filament_max_volumetric_speed /
                           (infill_flow.mm3_per_mm() * (cur_flowrate + (pass == 2 ? 0.035 : 0.05)) / cur_flowrate);
    else
        max_infill_speed = filament_max_volumetric_speed / (infill_flow.mm3_per_mm() * (pass == 1 ? 1.2 : 1));
    double internal_solid_speed = std::floor(std::min(print_config->opt_float("internal_solid_infill_speed"), max_infill_speed));
    double top_surface_speed = std::floor(std::min(print_config->opt_float("top_surface_speed"), max_infill_speed));

    // adjust parameters
    for (auto _obj : objects) {
        _obj->ensure_on_bed();
        _obj->config.set_key_value("wall_loops", new ConfigOptionInt(1));
        _obj->config.set_key_value("only_one_wall_top", new ConfigOptionBool(true));
        _obj->config.set_key_value("thick_internal_bridges", new ConfigOptionBool(false));
        _obj->config.set_key_value("enable_extra_bridge_layer", new ConfigOptionEnum<EnableExtraBridgeLayer>(eblDisabled));
        _obj->config.set_key_value("internal_bridge_density", new ConfigOptionPercent(100));
        _obj->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(35));
        _obj->config.set_key_value("min_width_top_surface", new ConfigOptionFloatOrPercent(100,true));
        _obj->config.set_key_value("bottom_shell_layers", new ConfigOptionInt(2));
        _obj->config.set_key_value("top_shell_layers", new ConfigOptionInt(5));
        _obj->config.set_key_value("top_shell_thickness", new ConfigOptionFloat(0));
        _obj->config.set_key_value("bottom_shell_thickness", new ConfigOptionFloat(0));
        _obj->config.set_key_value("detect_thin_wall", new ConfigOptionBool(true));
        _obj->config.set_key_value("filter_out_gap_fill", new ConfigOptionFloat(0));
        _obj->config.set_key_value("sparse_infill_pattern", new ConfigOptionEnum<InfillPattern>(ipRectilinear));
        _obj->config.set_key_value("top_surface_line_width", new ConfigOptionFloatOrPercent(nozzle_diameter * 1.2f, false));
        _obj->config.set_key_value("internal_solid_infill_line_width", new ConfigOptionFloatOrPercent(nozzle_diameter * 1.2f, false));
        _obj->config.set_key_value("top_surface_pattern", new ConfigOptionEnum<InfillPattern>(ipArchimedeanChords));
        _obj->config.set_key_value("top_solid_infill_flow_ratio", new ConfigOptionFloat(1.0f));
        _obj->config.set_key_value("infill_direction", new ConfigOptionFloat(45));
        _obj->config.set_key_value("solid_infill_direction", new ConfigOptionFloat(135));
        _obj->config.set_key_value("align_infill_direction_to_model", new ConfigOptionBool(true));
        _obj->config.set_key_value("ironing_type", new ConfigOptionEnum<IroningType>(IroningType::NoIroning));
        _obj->config.set_key_value("internal_solid_infill_speed", new ConfigOptionFloat(internal_solid_speed));
        _obj->config.set_key_value("top_surface_speed", new ConfigOptionFloat(top_surface_speed));
        _obj->config.set_key_value("seam_slope_type", new ConfigOptionEnum<SeamScarfType>(SeamScarfType::None));
        _obj->config.set_key_value("gap_fill_target", new ConfigOptionEnum<GapFillTarget>(GapFillTarget::gftNowhere));
        print_config->set_key_value("max_volumetric_extrusion_rate_slope", new ConfigOptionFloat(0));
        _obj->config.set_key_value("calib_flowrate_topinfill_special_order", new ConfigOptionBool(true));

        // extract flowrate from name, filename format: flowrate_xxx
        std::string obj_name = _obj->name;
        assert(obj_name.length() > 9);
        obj_name = obj_name.substr(9);
        if (obj_name[0] == 'm')
            obj_name[0] = '-';
        // Orca: force set locale to C to avoid parsing error
        const std::string _loc = std::setlocale(LC_NUMERIC, nullptr);
        std::setlocale(LC_NUMERIC,"C");
        auto              modifier  = 1.0f;
        try {
            modifier = stof(obj_name);
        } catch (...) {
        }
        // restore locale
        std::setlocale(LC_NUMERIC, _loc.c_str());

        if(linear)
            _obj->config.set_key_value("print_flow_ratio", new ConfigOptionFloat((cur_flowrate + modifier)/cur_flowrate));
        else
            _obj->config.set_key_value("print_flow_ratio", new ConfigOptionFloat(1.0f + modifier/100.f));

    }

    print_config->set_key_value("layer_height", new ConfigOptionFloat(layer_height));
    print_config->set_key_value("alternate_extra_wall", new ConfigOptionBool(false));
    print_config->set_key_value("initial_layer_print_height", new ConfigOptionFloat(first_layer_height));
    print_config->set_key_value("reduce_crossing_wall", new ConfigOptionBool(true));
    print_config->set_key_value("enable_wrapping_detection", new ConfigOptionBool(false));

    wxGetApp().get_tab(Preset::TYPE_PRINT)->update_dirty();
    wxGetApp().get_tab(Preset::TYPE_FILAMENT)->update_dirty();
    wxGetApp().get_tab(Preset::TYPE_PRINTER)->update_dirty();
    wxGetApp().get_tab(Preset::TYPE_PRINT)->reload_config();
    wxGetApp().get_tab(Preset::TYPE_FILAMENT)->reload_config();
    wxGetApp().get_tab(Preset::TYPE_PRINTER)->reload_config();
}

void Plater::calib_flowrate(bool is_linear, int pass) {
    if (pass != 1 && pass != 2)
        return;
    wxString calib_name;
    if (is_linear) {
        calib_name = L"Orca YOLO Flow Calibration";
        if (pass == 2)
            calib_name += L" - Perfectionist version";
    } else
        calib_name = wxString::Format(L"Flowrate Test - Pass%d", pass);

    if (new_project(false, false, calib_name) == wxID_CANCEL)
        return;

    wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));

    if (is_linear) {
        if (pass == 1)
            add_model(false,
                      (boost::filesystem::path(Slic3r::resources_dir()) / "calib" / "filament_flow" / "Orca-LinearFlow.3mf").string());
        else
            add_model(false,
                      (boost::filesystem::path(Slic3r::resources_dir()) / "calib" / "filament_flow" / "Orca-LinearFlow_fine.3mf").string());
    } else {
        if (pass == 1)
            add_model(false,
                      (boost::filesystem::path(Slic3r::resources_dir()) / "calib" / "filament_flow" / "flowrate-test-pass1.3mf").string());
        else
            add_model(false,
                      (boost::filesystem::path(Slic3r::resources_dir()) / "calib" / "filament_flow" / "flowrate-test-pass2.3mf").string());
    }

    adjust_settings_for_flowrate_calib(model().objects, is_linear, pass);
    wxGetApp().get_tab(Preset::TYPE_PRINTER)->reload_config();
    auto printer_config = &wxGetApp().preset_bundle->printers.get_edited_preset().config;
    printer_config->set_key_value("resonance_avoidance", new ConfigOptionBool{false});

    // Refresh object after scaling
    const std::vector<size_t> object_idx(boost::counting_iterator<size_t>(0), boost::counting_iterator<size_t>(model().objects.size()));
    changed_objects(object_idx);
}


void Plater::calib_temp(const Calib_Params& params) {
    const auto calib_temp_name = wxString::Format(L"Nozzle temperature test");
    new_project(false, false, calib_temp_name);
    wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));
    if (params.mode != CalibMode::Calib_Temp_Tower)
        return;
    
    add_model(false, Slic3r::resources_dir() + "/calib/temperature_tower/temperature_tower.stl");
    auto printer_config = &wxGetApp().preset_bundle->printers.get_edited_preset().config;
    auto filament_config = &wxGetApp().preset_bundle->filaments.get_edited_preset().config;
    auto start_temp = lround(params.start);
    printer_config->set_key_value("resonance_avoidance", new ConfigOptionBool{false});
    filament_config->set_key_value("nozzle_temperature_initial_layer", new ConfigOptionInts(1,(int)start_temp));
    filament_config->set_key_value("nozzle_temperature", new ConfigOptionInts(1,(int)start_temp));
    model().objects[0]->config.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btOuterOnly));
    model().objects[0]->config.set_key_value("brim_width", new ConfigOptionFloat(5.0));
    model().objects[0]->config.set_key_value("brim_object_gap", new ConfigOptionFloat(0.0));
    model().objects[0]->config.set_key_value("alternate_extra_wall", new ConfigOptionBool(false));
    model().objects[0]->config.set_key_value("seam_slope_type", new ConfigOptionEnum<SeamScarfType>(SeamScarfType::None));

    auto print_config = &wxGetApp().preset_bundle->prints.get_edited_preset().config;
    print_config->set_key_value("enable_wrapping_detection", new ConfigOptionBool(false));

    changed_objects({ 0 });
    wxGetApp().get_tab(Preset::TYPE_PRINT)->update_dirty();
    wxGetApp().get_tab(Preset::TYPE_FILAMENT)->update_dirty();
    wxGetApp().get_tab(Preset::TYPE_PRINT)->reload_config();
    wxGetApp().get_tab(Preset::TYPE_FILAMENT)->reload_config();

    // cut upper
    auto obj_bb = model().objects[0]->bounding_box_exact();
    auto block_count = lround((350 - params.end) / 5 + 1);
    if(block_count > 0){
        // add EPSILON offset to avoid cutting at the exact location where the flat surface is
        auto new_height = block_count * 10.0 + EPSILON;
        if (new_height < obj_bb.size().z()) {
            cut_horizontal(0, 0, new_height, ModelObjectCutAttribute::KeepLower);
        }
    }
    
    // cut bottom
    obj_bb = model().objects[0]->bounding_box_exact();
    block_count = lround((350 - params.start) / 5);
    if(block_count > 0){
        auto new_height = block_count * 10.0 + EPSILON;
        if (new_height < obj_bb.size().z()) {
            cut_horizontal(0, 0, new_height, ModelObjectCutAttribute::KeepUpper);
        }
    }
    
    p->background_process.fff_print()->set_calib_params(params);
}

void Plater::calib_max_vol_speed(const Calib_Params& params)
{
    const auto calib_vol_speed_name = wxString::Format(L"Max volumetric speed test");
    new_project(false, false, calib_vol_speed_name);
    wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));
    if (params.mode != CalibMode::Calib_Vol_speed_Tower)
        return;

    add_model(false, Slic3r::resources_dir() + "/calib/volumetric_speed/SpeedTestStructure.step");

    auto print_config = &wxGetApp().preset_bundle->prints.get_edited_preset().config;
    auto filament_config = &wxGetApp().preset_bundle->filaments.get_edited_preset().config;
    auto printer_config = &wxGetApp().preset_bundle->printers.get_edited_preset().config;
    auto obj = model().objects[0];
    auto& obj_cfg = obj->config;

    auto bed_shape = printer_config->option<ConfigOptionPoints>("printable_area")->values;
    BoundingBoxf bed_ext = get_extents(bed_shape);
    auto scale_obj = (bed_ext.size().x() - 10) / obj->bounding_box_exact().size().x();
    if (scale_obj < 1.0)
        obj->scale(scale_obj, 1, 1);

    const ConfigOptionFloats* nozzle_diameter_config = printer_config->option<ConfigOptionFloats>("nozzle_diameter");
    assert(nozzle_diameter_config->values.size() > 0);
    double nozzle_diameter = nozzle_diameter_config->values[0];
    double line_width = nozzle_diameter * 1.75;
    double layer_height = nozzle_diameter * 0.8;

    auto max_lh = printer_config->option<ConfigOptionFloats>("max_layer_height");
    if (max_lh->values[0] < layer_height)
        max_lh->values[0] = { layer_height };

    filament_config->set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats { 200 });
    filament_config->set_key_value("slow_down_layer_time", new ConfigOptionFloats{0.0});
    printer_config->set_key_value("resonance_avoidance", new ConfigOptionBool{false});
    obj_cfg.set_key_value("enable_overhang_speed", new ConfigOptionBool { false });
    obj_cfg.set_key_value("wall_loops", new ConfigOptionInt(1));
    obj_cfg.set_key_value("alternate_extra_wall", new ConfigOptionBool(false));
    obj_cfg.set_key_value("top_shell_layers", new ConfigOptionInt(0));
    obj_cfg.set_key_value("bottom_shell_layers", new ConfigOptionInt(0));
    obj_cfg.set_key_value("sparse_infill_density", new ConfigOptionPercent(0));
    obj_cfg.set_key_value("overhang_reverse", new ConfigOptionBool(false));
    obj_cfg.set_key_value("outer_wall_line_width", new ConfigOptionFloatOrPercent(line_width, false));
    obj_cfg.set_key_value("layer_height", new ConfigOptionFloat(layer_height));
    obj_cfg.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btOuterAndInner));
    obj_cfg.set_key_value("brim_width", new ConfigOptionFloat(5.0));
    obj_cfg.set_key_value("brim_object_gap", new ConfigOptionFloat(0.0));
    print_config->set_key_value("timelapse_type", new ConfigOptionEnum<TimelapseType>(tlTraditional));
    print_config->set_key_value("spiral_mode", new ConfigOptionBool(true));
    print_config->set_key_value("max_volumetric_extrusion_rate_slope", new ConfigOptionFloat(0));
    print_config->set_key_value("enable_wrapping_detection", new ConfigOptionBool(false));

    changed_objects({ 0 });
    wxGetApp().get_tab(Preset::TYPE_PRINT)->update_dirty();
    wxGetApp().get_tab(Preset::TYPE_FILAMENT)->update_dirty();
    wxGetApp().get_tab(Preset::TYPE_PRINTER)->update_dirty();
    wxGetApp().get_tab(Preset::TYPE_PRINT)->reload_config();
    wxGetApp().get_tab(Preset::TYPE_FILAMENT)->reload_config();
    wxGetApp().get_tab(Preset::TYPE_PRINTER)->reload_config();

    //  cut upper
    auto obj_bb = obj->bounding_box_exact();
    auto height = (params.end - params.start + 1) / params.step;
    if (height < obj_bb.size().z()) {
        cut_horizontal(0, 0, height, ModelObjectCutAttribute::KeepLower);
    }

    auto new_params  = params;
    auto mm3_per_mm  = Flow(line_width, layer_height, nozzle_diameter).mm3_per_mm() * filament_config->option<ConfigOptionFloatsNullable>("filament_flow_ratio")->get_at(0);
    new_params.end   = params.end / mm3_per_mm;
    new_params.start = params.start / mm3_per_mm;
    new_params.step  = params.step / mm3_per_mm;

    p->background_process.fff_print()->set_calib_params(new_params);
}

void Plater::calib_retraction(const Calib_Params& params)
{
    const auto calib_retraction_name = wxString::Format(L"Retraction test");
    new_project(false, false, calib_retraction_name);
    wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));
    if (params.mode != CalibMode::Calib_Retraction_tower)
        return;

    add_model(false, Slic3r::resources_dir() + "/calib/retraction/retraction_tower.stl");

    auto print_config = &wxGetApp().preset_bundle->prints.get_edited_preset().config;
    auto filament_config = &wxGetApp().preset_bundle->filaments.get_edited_preset().config;
    auto printer_config = &wxGetApp().preset_bundle->printers.get_edited_preset().config;
    auto obj = model().objects[0];

    print_config->set_key_value("enable_wrapping_detection", new ConfigOptionBool(false));

    double layer_height = 0.2;

    auto max_lh = printer_config->option<ConfigOptionFloats>("max_layer_height");
    if (max_lh->values[0] < layer_height)
        max_lh->values[0] = { layer_height };

    printer_config->set_key_value("resonance_avoidance", new ConfigOptionBool{false});
    printer_config->set_key_value("use_firmware_retraction", new ConfigOptionBool(false));
    obj->config.set_key_value("wall_loops", new ConfigOptionInt(2));
    obj->config.set_key_value("top_shell_layers", new ConfigOptionInt(0));
    obj->config.set_key_value("bottom_shell_layers", new ConfigOptionInt(3));
    obj->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(0));
    print_config->set_key_value("initial_layer_print_height", new ConfigOptionFloat(layer_height));
    obj->config.set_key_value("layer_height", new ConfigOptionFloat(layer_height));
    obj->config.set_key_value("alternate_extra_wall", new ConfigOptionBool(false));
    obj->config.set_key_value("seam_position", new ConfigOptionEnum<SeamPosition>(spAligned));
    obj->config.set_key_value("wall_sequence", new ConfigOptionEnum<WallSequence>(WallSequence::InnerOuter));

    changed_objects({ 0 });

    //  cut upper
    auto obj_bb = obj->bounding_box_exact();
    auto height = 1.0 + 0.4 + ((params.end - params.start)) / params.step;
    if (height < obj_bb.size().z()) {
        cut_horizontal(0, 0, height, ModelObjectCutAttribute::KeepLower);
    }

    p->background_process.fff_print()->set_calib_params(params);
}

void Plater::calib_VFA(const Calib_Params& params)
{
    const auto calib_vfa_name = wxString::Format(L"VFA test");
    new_project(false, false, calib_vfa_name);
    wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));
    if (params.mode != CalibMode::Calib_VFA_Tower)
        return;

    add_model(false, Slic3r::resources_dir() + "/calib/vfa/VFA.stl");
    auto print_config = &wxGetApp().preset_bundle->prints.get_edited_preset().config;
    auto filament_config = &wxGetApp().preset_bundle->filaments.get_edited_preset().config;
    auto printer_config  = &wxGetApp().preset_bundle->printers.get_edited_preset().config;
    printer_config->set_key_value("resonance_avoidance", new ConfigOptionBool{false});
    filament_config->set_key_value("slow_down_layer_time", new ConfigOptionFloats { 0.0 });
    print_config->set_key_value("enable_overhang_speed", new ConfigOptionBool { false });
    print_config->set_key_value("timelapse_type", new ConfigOptionEnum<TimelapseType>(tlTraditional));
    print_config->set_key_value("wall_loops", new ConfigOptionInt(1));
    print_config->set_key_value("alternate_extra_wall", new ConfigOptionBool(false));
    print_config->set_key_value("top_shell_layers", new ConfigOptionInt(0));
    print_config->set_key_value("bottom_shell_layers", new ConfigOptionInt(1));
    print_config->set_key_value("sparse_infill_density", new ConfigOptionPercent(0));
    print_config->set_key_value("overhang_reverse", new ConfigOptionBool(false));
    print_config->set_key_value("detect_thin_wall", new ConfigOptionBool(false));
    print_config->set_key_value("spiral_mode", new ConfigOptionBool(true));
    print_config->set_key_value("enable_wrapping_detection", new ConfigOptionBool(false));
    model().objects[0]->config.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btOuterOnly));
    model().objects[0]->config.set_key_value("brim_width", new ConfigOptionFloat(3.0));
    model().objects[0]->config.set_key_value("brim_object_gap", new ConfigOptionFloat(0.0));

    changed_objects({ 0 });
    wxGetApp().get_tab(Preset::TYPE_PRINT)->update_dirty();
    wxGetApp().get_tab(Preset::TYPE_FILAMENT)->update_dirty();
    wxGetApp().get_tab(Preset::TYPE_PRINT)->update_ui_from_settings();
    wxGetApp().get_tab(Preset::TYPE_FILAMENT)->update_ui_from_settings();

    // cut upper
    auto obj_bb = model().objects[0]->bounding_box_exact();
    auto height = 5 * ((params.end - params.start) / params.step + 1);
    if (height < obj_bb.size().z()) {
        cut_horizontal(0, 0, height, ModelObjectCutAttribute::KeepLower);
    }

    p->background_process.fff_print()->set_calib_params(params);
}

void Plater::calib_input_shaping_freq(const Calib_Params& params)
{
    const auto calib_input_shaping_name = wxString::Format(L"Input shaping Frequency test");
    new_project(false, false, calib_input_shaping_name);
    wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));
    if (params.mode != CalibMode::Calib_Input_shaping_freq)
        return;

    add_model(false, Slic3r::resources_dir() + (params.test_model < 1 ? "/calib/input_shaping/ringing_tower.stl" : "/calib/input_shaping/fast_tower_test.stl"));
    auto print_config = &wxGetApp().preset_bundle->prints.get_edited_preset().config;
    auto filament_config = &wxGetApp().preset_bundle->filaments.get_edited_preset().config;
    auto printer_config  = &wxGetApp().preset_bundle->printers.get_edited_preset().config;
    const auto gcode_flavor_option = printer_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor");

    if (has_junction_deviation(printer_config)) {
        printer_config->set_key_value("machine_max_junction_deviation", new ConfigOptionFloats {(std::max(printer_config->option<ConfigOptionFloats>("machine_max_junction_deviation")->values.front(), 0.25))});
        print_config->set_key_value("default_junction_deviation", new ConfigOptionFloat(0));
    } else {
        const double jerk_value = (gcode_flavor_option && gcode_flavor_option->value == GCodeFlavor::gcfKlipper) ? 5.0 : 10.0;
        printer_config->set_key_value("machine_max_jerk_x", new ConfigOptionFloats{std::max(printer_config->option<ConfigOptionFloats>("machine_max_jerk_x")->values.front(), jerk_value)});
        printer_config->set_key_value("machine_max_jerk_y", new ConfigOptionFloats{std::max(printer_config->option<ConfigOptionFloats>("machine_max_jerk_y")->values.front(), jerk_value)});
        print_config->set_key_value("default_jerk", new ConfigOptionFloat(0));
    }

    if (!filament_config->option<ConfigOptionBools>("enable_pressure_advance")->get_at(0)) {
        filament_config->set_key_value("enable_pressure_advance", new ConfigOptionBools {true});
        filament_config->set_key_value("pressure_advance", new ConfigOptionFloats { 0.0 });
        filament_config->set_key_value("adaptive_pressure_advance", new ConfigOptionBools{false});
    }

    printer_config->set_key_value("resonance_avoidance", new ConfigOptionBool{false});
    filament_config->set_key_value("slow_down_layer_time", new ConfigOptionFloats { 0.0 });
    filament_config->set_key_value("slow_down_min_speed", new ConfigOptionFloats { 0.0 });
    filament_config->set_key_value("slow_down_for_layer_cooling", new ConfigOptionBools{false});
    print_config->set_key_value("layer_height", new ConfigOptionFloat(0.2));
    print_config->set_key_value("enable_overhang_speed", new ConfigOptionBool { false });
    print_config->set_key_value("timelapse_type", new ConfigOptionEnum<TimelapseType>(tlTraditional));
    print_config->set_key_value("wall_loops", new ConfigOptionInt(1));
    print_config->set_key_value("top_shell_layers", new ConfigOptionInt(0));
    print_config->set_key_value("bottom_shell_layers", new ConfigOptionInt(1));
    print_config->set_key_value("sparse_infill_density", new ConfigOptionPercent(0));
    print_config->set_key_value("detect_thin_wall", new ConfigOptionBool(false));
    print_config->set_key_value("spiral_mode", new ConfigOptionBool(true));
    print_config->set_key_value("spiral_mode_smooth", new ConfigOptionBool(false));
    print_config->set_key_value("bottom_surface_pattern", new ConfigOptionEnum<InfillPattern>(ipRectilinear));
    print_config->set_key_value("outer_wall_speed", new ConfigOptionFloat(200));
    print_config->set_key_value("default_acceleration", new ConfigOptionFloat(20000));
    print_config->set_key_value("outer_wall_acceleration", new ConfigOptionFloat(20000));
    model().objects[0]->config.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btOuterOnly));
    model().objects[0]->config.set_key_value("brim_width", new ConfigOptionFloat(3.0));
    model().objects[0]->config.set_key_value("brim_object_gap", new ConfigOptionFloat(0.0));

    changed_objects({ 0 });
    wxGetApp().get_tab(Preset::TYPE_PRINT)->update_dirty();
    wxGetApp().get_tab(Preset::TYPE_FILAMENT)->update_dirty();
    wxGetApp().get_tab(Preset::TYPE_PRINT)->update_ui_from_settings();
    wxGetApp().get_tab(Preset::TYPE_FILAMENT)->update_ui_from_settings();

    p->background_process.fff_print()->set_calib_params(params);
}

void Plater::calib_input_shaping_damp(const Calib_Params& params)
{
    const auto calib_input_shaping_name = wxString::Format(L"Input shaping Damping test");
    new_project(false, false, calib_input_shaping_name);
    wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));
    if (params.mode != CalibMode::Calib_Input_shaping_damp)
        return;

    add_model(false, Slic3r::resources_dir() + (params.test_model < 1 ? "/calib/input_shaping/ringing_tower.stl" : "/calib/input_shaping/fast_tower_test.stl"));
    auto print_config = &wxGetApp().preset_bundle->prints.get_edited_preset().config;
    auto filament_config = &wxGetApp().preset_bundle->filaments.get_edited_preset().config;
    auto printer_config  = &wxGetApp().preset_bundle->printers.get_edited_preset().config;
    const auto gcode_flavor_option = printer_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor");

    if (has_junction_deviation(printer_config)) {
        printer_config->set_key_value("machine_max_junction_deviation", new ConfigOptionFloats {(std::max(printer_config->option<ConfigOptionFloats>("machine_max_junction_deviation")->values.front(), 0.25))});
        print_config->set_key_value("default_junction_deviation", new ConfigOptionFloat(0));
    } else {
        const double jerk_value = (gcode_flavor_option && gcode_flavor_option->value == GCodeFlavor::gcfKlipper) ? 5.0 : 10.0;
        printer_config->set_key_value("machine_max_jerk_x", new ConfigOptionFloats{std::max(printer_config->option<ConfigOptionFloats>("machine_max_jerk_x")->values.front(), jerk_value)});
        printer_config->set_key_value("machine_max_jerk_y", new ConfigOptionFloats{std::max(printer_config->option<ConfigOptionFloats>("machine_max_jerk_y")->values.front(), jerk_value)});
        print_config->set_key_value("default_jerk", new ConfigOptionFloat(0));
    }

    if (!filament_config->option<ConfigOptionBools>("enable_pressure_advance")->get_at(0)) {
        filament_config->set_key_value("enable_pressure_advance", new ConfigOptionBools {true});
        filament_config->set_key_value("pressure_advance", new ConfigOptionFloats { 0.0 });
        filament_config->set_key_value("adaptive_pressure_advance", new ConfigOptionBools{false});
    }

    printer_config->set_key_value("resonance_avoidance", new ConfigOptionBool{false});
    filament_config->set_key_value("slow_down_layer_time", new ConfigOptionFloats { 0.0 });
    filament_config->set_key_value("slow_down_min_speed", new ConfigOptionFloats { 0.0 });
    filament_config->set_key_value("slow_down_for_layer_cooling", new ConfigOptionBools{false});
    print_config->set_key_value("layer_height", new ConfigOptionFloat(0.2));
    print_config->set_key_value("enable_overhang_speed", new ConfigOptionBool{false});
    print_config->set_key_value("timelapse_type", new ConfigOptionEnum<TimelapseType>(tlTraditional));
    print_config->set_key_value("wall_loops", new ConfigOptionInt(1));
    print_config->set_key_value("top_shell_layers", new ConfigOptionInt(0));
    print_config->set_key_value("bottom_shell_layers", new ConfigOptionInt(1));
    print_config->set_key_value("sparse_infill_density", new ConfigOptionPercent(0));
    print_config->set_key_value("detect_thin_wall", new ConfigOptionBool(false));
    print_config->set_key_value("spiral_mode", new ConfigOptionBool(true));
    print_config->set_key_value("spiral_mode_smooth", new ConfigOptionBool(false));
    print_config->set_key_value("bottom_surface_pattern", new ConfigOptionEnum<InfillPattern>(ipRectilinear));
    print_config->set_key_value("outer_wall_speed", new ConfigOptionFloat(200));
    print_config->set_key_value("default_acceleration", new ConfigOptionFloat(20000));
    print_config->set_key_value("outer_wall_acceleration", new ConfigOptionFloat(20000));
    model().objects[0]->config.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btOuterOnly));
    model().objects[0]->config.set_key_value("brim_width", new ConfigOptionFloat(3.0));
    model().objects[0]->config.set_key_value("brim_object_gap", new ConfigOptionFloat(0.0));

    changed_objects({ 0 });
    wxGetApp().get_tab(Preset::TYPE_PRINT)->update_dirty();
    wxGetApp().get_tab(Preset::TYPE_FILAMENT)->update_dirty();
    wxGetApp().get_tab(Preset::TYPE_PRINT)->update_ui_from_settings();
    wxGetApp().get_tab(Preset::TYPE_FILAMENT)->update_ui_from_settings();

    p->background_process.fff_print()->set_calib_params(params);
}

void Plater::Calib_Cornering(const Calib_Params& params)
{
    const auto Calib_Cornering = wxString::Format(L"Cornering test");
    new_project(false, false, Calib_Cornering);
    wxGetApp().mainframe->select_tab(size_t(MainFrame::tp3DEditor));
    if (params.mode != CalibMode::Calib_Cornering)
        return;

    const std::string cornering_model_path = params.test_model == 0
        ? "/calib/input_shaping/ringing_tower.stl"
        : (params.test_model == 1 ? "/calib/input_shaping/fast_tower_test.stl" : "/calib/cornering/SCV-V2.stl");
    add_model(false, Slic3r::resources_dir() + cornering_model_path);
    auto print_config = &wxGetApp().preset_bundle->prints.get_edited_preset().config;
    auto filament_config = &wxGetApp().preset_bundle->filaments.get_edited_preset().config;
    auto printer_config  = &wxGetApp().preset_bundle->printers.get_edited_preset().config;

    if (has_junction_deviation(printer_config)) {
        printer_config->set_key_value("machine_max_junction_deviation", new ConfigOptionFloats{params.end});
        print_config->set_key_value("default_junction_deviation", new ConfigOptionFloat(0.0));
    } else {
        printer_config->set_key_value("machine_max_jerk_x", new ConfigOptionFloats{params.end});
        printer_config->set_key_value("machine_max_jerk_y", new ConfigOptionFloats{params.end});
        print_config->set_key_value("default_jerk", new ConfigOptionFloat(0));
    }

    if (!filament_config->option<ConfigOptionBools>("enable_pressure_advance")->get_at(0)) {
        filament_config->set_key_value("enable_pressure_advance", new ConfigOptionBools {true});
        filament_config->set_key_value("pressure_advance", new ConfigOptionFloats { 0.0 });
        filament_config->set_key_value("adaptive_pressure_advance", new ConfigOptionBools{false});
    }

    printer_config->set_key_value("resonance_avoidance", new ConfigOptionBool{false});
    filament_config->set_key_value("slow_down_layer_time", new ConfigOptionFloats { 0.0 });
    filament_config->set_key_value("slow_down_min_speed", new ConfigOptionFloats { 0.0 });
    filament_config->set_key_value("slow_down_for_layer_cooling", new ConfigOptionBools{false});
    filament_config->set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats{200});
    print_config->set_key_value("layer_height", new ConfigOptionFloat(0.2));
    print_config->set_key_value("enable_overhang_speed", new ConfigOptionBool{false});
    print_config->set_key_value("timelapse_type", new ConfigOptionEnum<TimelapseType>(tlTraditional));
    print_config->set_key_value("wall_loops", new ConfigOptionInt(1));
    print_config->set_key_value("top_shell_layers", new ConfigOptionInt(0));
    print_config->set_key_value("bottom_shell_layers", new ConfigOptionInt(1));
    print_config->set_key_value("sparse_infill_density", new ConfigOptionPercent(0));
    print_config->set_key_value("detect_thin_wall", new ConfigOptionBool(false));
    print_config->set_key_value("spiral_mode", new ConfigOptionBool(true));
    print_config->set_key_value("spiral_mode_smooth", new ConfigOptionBool(false));
    print_config->set_key_value("bottom_surface_pattern", new ConfigOptionEnum<InfillPattern>(ipRectilinear));
    print_config->set_key_value("outer_wall_speed", new ConfigOptionFloat(200));
    print_config->set_key_value("default_acceleration", new ConfigOptionFloat(2000));
    print_config->set_key_value("outer_wall_acceleration", new ConfigOptionFloat(2000));
    model().objects[0]->config.set_key_value("brim_type", new ConfigOptionEnum<BrimType>(btOuterOnly));
    model().objects[0]->config.set_key_value("brim_width", new ConfigOptionFloat(3.0));
    model().objects[0]->config.set_key_value("brim_object_gap", new ConfigOptionFloat(0.0));

    changed_objects({ 0 });
    wxGetApp().get_tab(Preset::TYPE_PRINT)->update_dirty();
    wxGetApp().get_tab(Preset::TYPE_FILAMENT)->update_dirty();
    wxGetApp().get_tab(Preset::TYPE_PRINT)->update_ui_from_settings();
    wxGetApp().get_tab(Preset::TYPE_FILAMENT)->update_ui_from_settings();

    p->background_process.fff_print()->set_calib_params(params);
}

BuildVolume_Type Plater::get_build_volume_type() const { return p->bed.get_build_volume_type(); }

void Plater::import_zip_archive()
{
    wxString input_file;
    wxGetApp().import_zip(this, input_file);
    if (input_file.empty())
        return;

    wxArrayString arr;
    arr.Add(input_file);
    load_files(arr);
}

void Plater::import_sl1_archive()
{
    auto &w = get_ui_job_worker();
    if (w.is_idle() && p->m_sla_import_dlg->ShowModal() == wxID_OK) {
        p->take_snapshot(_u8L("Import SLA archive"));
        replace_job(w, std::make_unique<SLAImportJob>(p->m_sla_import_dlg));
    }
}

void Plater::extract_config_from_project()
{
    wxString input_file;
    wxGetApp().load_project(this, input_file);

    if (! input_file.empty())
        load_files({ into_path(input_file) }, LoadStrategy::LoadConfig);
}

void Plater::load_gcode()
{
    // Ask user for a gcode file name.
    wxString input_file;
    wxGetApp().load_gcode(this, input_file);
    // And finally load the gcode file.
    load_gcode(input_file);
}

//BBS: remove GCodeViewer as seperate APP logic
void Plater::load_gcode(const wxString& filename)
{
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << __LINE__ << " entry and filename: " << filename;
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__;
    if (! is_gcode_file(into_u8(filename))
        || (m_last_loaded_gcode == filename && m_only_gcode)
        )
        return;

    m_last_loaded_gcode = filename;

    // BSS: create a new project when load_gcode, force close previous one
    if (new_project(false, true) != wxID_YES)
        return;

    m_only_gcode = true;

    // cleanup view before to start loading/processing
    //BBS: update gcode to current partplate's
    GCodeProcessorResult* current_result = p->partplate_list.get_current_slice_result();
    Print& current_print = p->partplate_list.get_current_fff_print();
    //BBS:already reset in new_project
    //current_result->reset();
    //p->gcode_result.reset();
    //reset_gcode_toolpaths();
    p->preview->reload_print(false, m_only_gcode);
    wxGetApp().mainframe->select_tab(MainFrame::tpPreview);
    p->set_current_panel(p->preview, true);
    p->get_current_canvas3D()->render();
    //p->notification_manager->bbl_show_plateinfo_notification(into_u8(_L("Preview only mode for gcode file.")));

    wxBusyCursor wait;

    // process gcode
    GCodeProcessor processor;
    processor.init_filament_maps_and_nozzle_type_when_import_only_gcode();
    try
    {
        GCodeProcessor::s_IsBBLPrinter = wxGetApp().preset_bundle->is_bbl_vendor();
        processor.process_file(filename.ToUTF8().data());
    }
    catch (const std::exception& ex)
    {
        show_error(this, ex.what());
        return;
    }
    *current_result = std::move(processor.extract_result());
    //current_result->filename = filename;

    BedType bed_type = current_result->bed_type;
    if (bed_type != BedType::btCount) {
        DynamicPrintConfig &proj_config = wxGetApp().preset_bundle->project_config;
        proj_config.set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(bed_type));
        on_bed_type_change(bed_type);
    }

    current_print.apply(this->model(), wxGetApp().preset_bundle->full_config());

    current_print.apply_config_for_render(processor.export_config_for_render());

    //BBS: add cost info when drag in gcode
    auto& ps = current_result->print_statistics;
    double total_cost = 0.0;
    for (auto volume : ps.total_volumes_per_extruder) {
        size_t extruder_id = volume.first;
        double density = current_result->filament_densities.at(extruder_id);
        double cost = current_result->filament_costs.at(extruder_id);
        double weight = volume.second * density * 0.001;
        total_cost += weight * cost * 0.001;
    }
    current_print.print_statistics().total_cost = total_cost;

    current_print.set_gcode_file_ready();

    // show results
    p->preview->reload_print(false, m_only_gcode);
    //BBS: zoom to bed 0 for gcode preview
    //p->preview->get_canvas3d()->zoom_to_gcode();
    p->preview->get_canvas3d()->zoom_to_plate(0);

    if (p->preview->get_canvas3d()->get_gcode_layers_zs().empty()) {
        MessageDialog(this, _L("The selected file") + ":\n" + filename + "\n" + _L("does not contain valid G-code."),
            wxString(GCODEVIEWER_APP_NAME) + " - " + _L("Error occurs while loading G-code file"), wxCLOSE | wxICON_WARNING | wxCENTRE).ShowModal();
        set_project_filename(DEFAULT_PROJECT_NAME);
    } else {
        set_project_filename(filename);
    }

    // Orca: Fix crash when loading gcode file multiple times
    if (m_only_gcode) {
        p->view3D->get_canvas3d()->remove_raycasters_for_picking(SceneRaycaster::EType::Bed);
    }

    p->main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, false, false); //20250416 ban gcode to send print
}

void Plater::reload_gcode_from_disk()
{
    wxString filename(m_last_loaded_gcode);
    m_last_loaded_gcode.clear();
    load_gcode(filename);
}

void Plater::refresh_print()
{
    p->preview->refresh_print();
}

// BBS
wxString Plater::get_project_name()
{
    return p->get_project_name();
}

void Plater::update_all_plate_thumbnails(bool force_update)
{
    for (int i = 0; i < get_partplate_list().get_plate_count(); i++) {
        PartPlate* plate = get_partplate_list().get_plate(i);
        ThumbnailsParams thumbnail_params = { {}, false, true, true, true, i};
        if (force_update || !plate->thumbnail_data.is_valid()) {
            get_view3D_canvas3D()->render_thumbnail(plate->thumbnail_data, plate->plate_thumbnail_width, plate->plate_thumbnail_height, thumbnail_params, Camera::EType::Ortho);
        }
        if (force_update || !plate->no_light_thumbnail_data.is_valid()) {
            get_view3D_canvas3D()->render_thumbnail(plate->no_light_thumbnail_data, plate->plate_thumbnail_width, plate->plate_thumbnail_height, thumbnail_params,
                                                    Camera::EType::Ortho, Camera::ViewAngleType::Iso, false, true);
        }
    }
}

void Plater::update_obj_preview_thumbnail(ModelObject *mo, int obj_idx, int vol_idx, std::vector<Slic3r::ColorRGBA> colors, int camera_view_angle_type)
{
    PartPlate *      plate            = get_partplate_list().get_plate(0);
    ThumbnailsParams thumbnail_params = {{}, false, true, true, true, 0, false};
    GLVolumeCollection cur_volumes;
    cur_volumes.load_object_volume(mo, obj_idx, vol_idx, 0, "volume", true, false, false, false);
    ModelObjectPtrs model_objects;
    model_objects.emplace_back(mo);
    get_view3D_canvas3D()->render_thumbnail(plate->obj_preview_thumbnail_data, colors, plate->plate_thumbnail_width, plate->plate_thumbnail_height, thumbnail_params,
                                            model_objects, cur_volumes, Camera::EType::Ortho, (Camera::ViewAngleType) camera_view_angle_type, false, false);
}

//invalid all plate's thumbnails
void Plater::invalid_all_plate_thumbnails()
{
    if (using_exported_file() || skip_thumbnail_invalid)
        return;
    BOOST_LOG_TRIVIAL(info) << "thumb: invalid all";
    for (int i = 0; i < get_partplate_list().get_plate_count(); i++) {
        PartPlate* plate = get_partplate_list().get_plate(i);
        plate->thumbnail_data.reset();
        plate->no_light_thumbnail_data.reset();
    }
}

void Plater::force_update_all_plate_thumbnails()
{
    if (using_exported_file() || skip_thumbnail_invalid) {
    }
    else {
        invalid_all_plate_thumbnails();
        update_all_plate_thumbnails(true);
    }
    get_preview_canvas3D()->update_plate_thumbnails();
}

// BBS: backup
std::vector<size_t> Plater::load_files(const std::vector<fs::path>& input_files, LoadStrategy strategy, bool ask_multi) {
    //BBS: wish to reset state when load a new file
    p->m_slice_all_only_has_gcode = false;
    //BBS: wish to reset all plates stats item selected state when load a new file
    p->preview->get_canvas3d()->reset_select_plate_toolbar_selection();
    return p->load_files(input_files, strategy, ask_multi);
}

// To be called when providing a list of files to the GUI slic3r on command line.
std::vector<size_t> Plater::load_files(const std::vector<std::string>& input_files, LoadStrategy strategy,  bool ask_multi)
{
    std::vector<fs::path> paths;
    paths.reserve(input_files.size());
    for (const std::string& path : input_files)
        paths.emplace_back(path);
    return p->load_files(paths, strategy, ask_multi);
}

bool Plater::preview_zip_archive(const boost::filesystem::path& archive_path)
{
    //std::vector<fs::path> unzipped_paths;
    std::vector<fs::path> non_project_paths;
    std::vector<fs::path> project_paths;
    try
    {
        mz_zip_archive archive;
        mz_zip_zero_struct(&archive);

        if (!open_zip_reader(&archive, archive_path.string())) {
            // TRN %1% is archive path
            std::string err_msg = GUI::format(_u8L("Loading of a ZIP archive on path %1% has failed."), archive_path.string());
            throw Slic3r::FileIOError(err_msg);
        }
        mz_uint num_entries = mz_zip_reader_get_num_files(&archive);
        mz_zip_archive_file_stat stat;
        // selected_paths contains paths and its uncompressed size. The size is used to distinguish between files with same path.
        std::vector<std::pair<fs::path, size_t>> selected_paths;
        FileArchiveDialog dlg(static_cast<wxWindow*>(wxGetApp().mainframe), &archive, selected_paths);
        if (dlg.ShowModal() == wxID_OK)
        {
            std::string archive_path_string = archive_path.string();
            archive_path_string = archive_path_string.substr(0, archive_path_string.size() - 4);
            fs::path archive_dir(wxStandardPaths::Get().GetTempDir().utf8_str().data());

            for (auto& path_w_size : selected_paths) {
                const fs::path& path = path_w_size.first;
                size_t size = path_w_size.second;
                // find path in zip archive
                for (mz_uint i = 0; i < num_entries; ++i) {
                    if (mz_zip_reader_file_stat(&archive, i, &stat)) {
                        if (size != stat.m_uncomp_size) // size must fit
                            continue;
                        wxString wname = boost::nowide::widen(stat.m_filename);
                        std::string name = into_u8(wname);
                        fs::path archive_path(name);

                        std::string extra(1024, 0);
                        size_t extra_size = mz_zip_reader_get_filename_from_extra(&archive, i, extra.data(), extra.size());
                        if (extra_size > 0) {
                            archive_path = fs::path(extra.substr(0, extra_size));
                            name = archive_path.string();
                        }

                        if (archive_path.empty())
                            continue;
                        if (path != archive_path)
                            continue;
                        // decompressing
                        try
                        {
                            std::replace(name.begin(), name.end(), '\\', '/');
                            // rename if file exists
                            std::string filename = path.filename().string();
                            std::string extension = path.extension().string();
                            std::string just_filename = filename.substr(0, filename.size() - extension.size());
                            std::string final_filename = just_filename;

                            size_t version = 0;
                            while (fs::exists(archive_dir / (final_filename + extension)))
                            {
                                ++version;
                                final_filename = just_filename + "(" + std::to_string(version) + ")";
                            }
                            filename = final_filename + extension;
                            fs::path final_path = archive_dir / filename;
                            std::string buffer((size_t)stat.m_uncomp_size, 0);
                            // Decompress action. We already has correct file index in stat structure.
                            mz_bool res = mz_zip_reader_extract_to_mem(&archive, stat.m_file_index, (void*)buffer.data(), (size_t)stat.m_uncomp_size, 0);
                            if (res == 0) {
                                // TRN: First argument = path to file, second argument = error description
                                wxString error_log = GUI::format_wxstr(_L("Failed to unzip file to %1%: %2%"), final_path.string(), mz_zip_get_error_string(mz_zip_get_last_error(&archive)));
                                BOOST_LOG_TRIVIAL(error) << error_log;
                                show_error(nullptr, error_log);
                                break;
                            }
                            // write buffer to file
                            fs::fstream file(final_path, std::ios::out | std::ios::binary | std::ios::trunc);
                            file.write(buffer.c_str(), buffer.size());
                            file.close();
                            if (!fs::exists(final_path)) {
                                wxString error_log = GUI::format_wxstr(_L("Failed to find unzipped file at %1%. Unzipping of file has failed."), final_path.string());
                                BOOST_LOG_TRIVIAL(error) << error_log;
                                show_error(nullptr, error_log);
                                break;
                            }
                            BOOST_LOG_TRIVIAL(info) << "Unzipped " << final_path;
                            if (!boost::algorithm::iends_with(filename, ".3mf") && !boost::algorithm::iends_with(filename, ".amf")) {
                                non_project_paths.emplace_back(final_path);
                                break;
                            }
                            // if 3mf - read archive headers to find project file
                            if (/*(boost::algorithm::iends_with(filename, ".3mf") && !is_project_3mf(final_path.string())) ||*/
                                (boost::algorithm::iends_with(filename, ".amf") && !boost::algorithm::iends_with(filename, ".zip.amf"))) {
                                non_project_paths.emplace_back(final_path);
                                break;
                            }

                            project_paths.emplace_back(final_path);
                            break;
                        }
                        catch (const std::exception& e)
                        {
                            // ensure the zip archive is closed and rethrow the exception
                            close_zip_reader(&archive);
                            throw Slic3r::FileIOError(e.what());
                        }
                    }
                }
            }
            close_zip_reader(&archive);
            if (non_project_paths.size() + project_paths.size() != selected_paths.size())
                BOOST_LOG_TRIVIAL(error) << "Decompresing of archive did not retrieve all files. Expected files: "
                                         << selected_paths.size()
                                         << " Decopressed files: "
                                         << non_project_paths.size() + project_paths.size();
        } else {
            close_zip_reader(&archive);
            return false;
        }

    }
    catch (const Slic3r::FileIOError& e) {
        // zip reader should be already closed or not even opened
        GUI::show_error(this, e.what());
        return false;
    }
    // none selected
    if (project_paths.empty() && non_project_paths.empty())
    {
        return false;
    }

    // 1 project file and some models - behave like drag n drop of 3mf and then load models
    if (project_paths.size() == 1)
    {
        wxArrayString aux;
        aux.Add(from_u8(project_paths.front().string()));
        bool loaded3mf = load_files(aux);
        load_files(non_project_paths, LoadStrategy::LoadModel);
        boost::system::error_code ec;
        if (loaded3mf) {
            fs::remove(project_paths.front(), ec);
            if (ec)
                BOOST_LOG_TRIVIAL(error) << ec.message();
        }
        for (const fs::path& path : non_project_paths) {
            // Delete file from temp file (path variable), it will stay only in app memory.
            boost::system::error_code ec;
            fs::remove(path, ec);
            if (ec)
                BOOST_LOG_TRIVIAL(error) << ec.message();
        }
        return true;
    }

    // load all projects and all models as geometry
    load_files(project_paths, LoadStrategy::LoadModel);
    load_files(non_project_paths, LoadStrategy::LoadModel);


    for (const fs::path& path : project_paths) {
        // Delete file from temp file (path variable), it will stay only in app memory.
        boost::system::error_code ec;
        fs::remove(path, ec);
        if (ec)
            BOOST_LOG_TRIVIAL(error) << ec.message();
    }
    for (const fs::path& path : non_project_paths) {
        // Delete file from temp file (path variable), it will stay only in app memory.
        boost::system::error_code ec;
        fs::remove(path, ec);
        if (ec)
            BOOST_LOG_TRIVIAL(error) << ec.message();
    }

    return true;
}

#define PROJECT_DROP_DIALOG_SELECT_PLANE_SIZE wxSize(FromDIP(350), FromDIP(120))

class ProjectDropDialog : public DPIDialog
{
private:
    wxColour          m_def_color = wxColour(255, 255, 255);
    int               m_action{1};
    bool              m_remember_choice{false};

public:
    ProjectDropDialog(const std::string &filename);

    wxPanel *     m_top_line;
    wxStaticText *m_fname_title;
    wxStaticText *m_fname_f;
    wxStaticText *m_fname_s;
    StaticBox * m_panel_select;

    void      on_select_ok(wxCommandEvent &event);
    void      on_select_cancel(wxCommandEvent &event);

    int       get_action() const { return m_action; }
    void      set_action(int index) { m_action = index; }

    wxBoxSizer *create_remember_checkbox(wxString title, wxWindow* parent, wxString tooltip);
    wxBoxSizer *create_item_radiobox(wxString title, wxWindow *parent, int select_id, int groupid);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;
};

ProjectDropDialog::ProjectDropDialog(const std::string &filename)
    : DPIDialog(static_cast<wxWindow *>(wxGetApp().mainframe),
                wxID_ANY,
                from_u8((boost::format(_utf8(L("Drop project file")))).str()),
                wxDefaultPosition,
                wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
    , m_action(2)
{
    // def setting
    SetBackgroundColour(m_def_color);

    // icon
    std::string icon_path = (boost::format("%1%/images/OrcaSlicerTitle.ico") % resources_dir()).str();
    SetIcon(wxIcon(encode_path(icon_path.c_str()), wxBITMAP_TYPE_ICO));

    wxBoxSizer *m_sizer_main = new wxBoxSizer(wxVERTICAL);

    m_top_line = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_top_line->SetBackgroundColour(wxColour(166, 169, 170));

    m_sizer_main->Add(m_top_line, 0, wxEXPAND, 0);

    m_sizer_main->Add(0, 0, 0, wxEXPAND | wxTOP, 20);

    wxBoxSizer *m_sizer_name = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer *m_sizer_fline = new wxBoxSizer(wxHORIZONTAL);

    m_fname_title = new wxStaticText(this, wxID_ANY, _L("Please select an action"), wxDefaultPosition, wxDefaultSize, 0);
    m_fname_title->Wrap(-1);
    m_fname_title->SetFont(::Label::Body_14);
    m_fname_title->SetForegroundColour(wxColour(107, 107, 107));
    m_fname_title->SetBackgroundColour(wxColour(255, 255, 255));

    m_sizer_fline->Add(m_fname_title, 0, wxALL, 0);
    m_sizer_fline->Add(0, 0, 0, wxEXPAND | wxLEFT, 5);

    m_fname_f = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0);
    m_fname_f->SetFont(::Label::Head_14);
    m_fname_f->Wrap(-1);
    m_fname_f->SetForegroundColour(wxColour(38, 46, 48));

    m_sizer_fline->Add(m_fname_f, 1, wxALL, 0);

    m_sizer_name->Add(m_sizer_fline, 1, wxEXPAND, 0);

    m_fname_s = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0);
    m_fname_s->SetFont(::Label::Head_14);
    m_fname_s->Wrap(-1);
    m_fname_s->SetForegroundColour(wxColour(38, 46, 48));

    m_sizer_name->Add(m_fname_s, 1, wxALL, 0);

    m_sizer_main->Add(m_sizer_name, 1, wxEXPAND | wxLEFT | wxRIGHT, 20);

    auto radio_group = new RadioGroup(this, {
        _L("Open as project"),     // 0
        _L("Import geometry only") // 1
    }, wxVERTICAL);
    radio_group->SetMinSize(wxSize(FromDIP(300),-1));
    radio_group->SetSelection(get_action() - 1);
    radio_group->Bind(wxEVT_COMMAND_RADIOBOX_SELECTED, [this, radio_group](wxCommandEvent &e) {
        set_action(radio_group->GetSelection() + 1);
    });

    m_sizer_main->Add(radio_group, 0, wxEXPAND | wxLEFT | wxRIGHT, 20);

    m_sizer_main->Add(0, 0, 0, wxEXPAND | wxTOP, 10);

    // wxBoxSizer *m_sizer_bottom = new wxBoxSizer(wxHORIZONTAL);
    // Orca: hide the "Don't show again" checkbox, people keeps accidentally checked this then forgot
    // wxBoxSizer *m_sizer_left = new wxBoxSizer(wxHORIZONTAL);
    //
    // auto dont_show_again = create_remember_checkbox(_L("Remember my choice."), this, _L("This option can be changed later in preferences, under 'Load Behaviour'."));
    // m_sizer_left->Add(dont_show_again, 0, wxALL, 5);
    //
    // m_sizer_bottom->Add(m_sizer_left, 0, wxEXPAND, 5);

    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, &ProjectDropDialog::on_select_ok, this);

    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, &ProjectDropDialog::on_select_cancel, this);

    m_sizer_main->Add(dlg_btns, 0, wxEXPAND);

    SetSizer(m_sizer_main);
    Layout();
    Fit();
    Centre(wxBOTH);


    auto limit_width   = m_fname_f->GetSize().GetWidth() - 2;
    auto current_width = 0;
    auto cut_index     = 0;
    auto fstring       = wxString("");
    auto bstring       = wxString("");

    //auto file_name = from_u8(filename.c_str());
    auto file_name = wxString(filename);
    for (int x = 0; x < file_name.length(); x++) {
        current_width += m_fname_s->GetTextExtent(file_name[x]).GetWidth();
        cut_index = x;

        if (current_width > limit_width) {
            bstring += file_name[x];
        } else {
            fstring += file_name[x];
        }
    }

    m_fname_f->SetLabel(fstring);
    m_fname_s->SetLabel(bstring);

    wxGetApp().UpdateDlgDarkUI(this);
}

wxBoxSizer *ProjectDropDialog::create_remember_checkbox(wxString title, wxWindow *parent, wxString tooltip)
{
    wxBoxSizer *m_sizer_checkbox = new wxBoxSizer(wxHORIZONTAL);
    m_sizer_checkbox->Add(0, 0, 0, wxEXPAND | wxLEFT, 5);

    auto checkbox = new ::CheckBox(parent);
    checkbox->SetValue(m_remember_choice);
    checkbox->SetToolTip(tooltip);
    m_sizer_checkbox->Add(checkbox, 0, wxALIGN_CENTER, 0);
    m_sizer_checkbox->Add(0, 0, 0, wxEXPAND | wxLEFT, 8);

    auto checkbox_title = new wxStaticText(parent, wxID_ANY, title, wxDefaultPosition, wxSize(-1, -1), 0);
    checkbox_title->SetForegroundColour(wxColour(144,144,144));
    checkbox_title->SetFont(::Label::Body_13);
    checkbox_title->Wrap(-1);
    checkbox_title->SetToolTip(tooltip);
    m_sizer_checkbox->Add(checkbox_title, 0, wxALIGN_CENTER | wxALL, 3);

    checkbox->Bind(wxEVT_TOGGLEBUTTON, [this, checkbox](wxCommandEvent &e) {
        m_remember_choice = checkbox->GetValue();
        e.Skip();
    });

    return m_sizer_checkbox;
}

void ProjectDropDialog::on_select_ok(wxCommandEvent &event)
{
    if (m_remember_choice) {
        LoadType load_type = static_cast<LoadType>(get_action());
        switch (load_type)
        {
            case LoadType::OpenProject:
                wxGetApp().app_config->set(SETTING_PROJECT_LOAD_BEHAVIOUR, OPTION_PROJECT_LOAD_BEHAVIOUR_LOAD_ALL);
                break;
            case LoadType::LoadGeometry:
                wxGetApp().app_config->set(SETTING_PROJECT_LOAD_BEHAVIOUR, OPTION_PROJECT_LOAD_BEHAVIOUR_LOAD_GEOMETRY);
                break;
        }
    }

    EndModal(wxID_OK);
}

void ProjectDropDialog::on_select_cancel(wxCommandEvent &event)
{
    EndModal(wxID_CANCEL);
}

void ProjectDropDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    Fit();
    Refresh();
}

//BBS: remove GCodeViewer as seperate APP logic
bool Plater::load_files(const wxArrayString& filenames)
{
    const std::regex pattern_drop(".*[.](stp|step|stl|oltp|obj|amf|3mf|svg|zip)", std::regex::icase);
    const std::regex pattern_gcode_drop(".*[.](gcode|g)", std::regex::icase);

    std::vector<fs::path> normal_paths;
    std::vector<fs::path> gcode_paths;

    for (const auto& filename : filenames) {
        fs::path path(into_path(filename));
        if (std::regex_match(path.string(), pattern_drop))
            normal_paths.push_back(std::move(path));
        else if (std::regex_match(path.string(), pattern_gcode_drop))
            gcode_paths.push_back(std::move(path));
        else
            continue;
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": normal_paths %1%, gcode_paths %2%")%normal_paths.size() %gcode_paths.size();
    if (normal_paths.empty() && gcode_paths.empty()) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": can not find valid path, return directly");
        // Likely no supported files
        return false;
    }
    else if (normal_paths.empty()){
        //only gcode files
        if (gcode_paths.size() > 1) {
            show_info(this, _L("Only one G-code file can be opened at the same time."), _L("G-code loading"));
            return false;
        }
        load_gcode(from_path(gcode_paths.front()));
        return true;
    }

    if (!gcode_paths.empty()) {
        show_info(this, _L("G-code files cannot be loaded with models together!"), _L("G-code loading"));
        return false;
    }

    //// searches for project files
    //for (std::vector<fs::path>::const_reverse_iterator it = normal_paths.rbegin(); it != normal_paths.rend(); ++it) {
    //    std::string filename = (*it).filename().string();
    //    ////BBS: only 3mf will be treated as project file
    //    if (open_3mf_file((*it)))
    //        return true;
    //}

    //// other files
    std::string snapshot_label;
    assert(!normal_paths.empty());
    if (normal_paths.size() == 1) {
        snapshot_label = "Load File";
        snapshot_label += ": ";
        snapshot_label += encode_path(normal_paths.front().filename().string().c_str());
    } else {
        snapshot_label = "Load Files";
        snapshot_label += ": ";
        snapshot_label += encode_path(normal_paths.front().filename().string().c_str());
        for (size_t i = 1; i < normal_paths.size(); ++i) {
            snapshot_label += ", ";
            snapshot_label += encode_path(normal_paths[i].filename().string().c_str());
        }
    }

    //Plater::TakeSnapshot snapshot(this, snapshot_label);
    //load_files(normal_paths, LoadStrategy::LoadModel);

    // BBS: check file types
    std::sort(normal_paths.begin(), normal_paths.end(), [](fs::path obj1, fs::path obj2) { return obj1.filename().string() < obj2.filename().string(); });

    auto loadfiles_type  = LoadFilesType::NoFile;
    auto amf_files_count = get_3mf_file_count(normal_paths);

    if (normal_paths.size() > 1 && amf_files_count < normal_paths.size()) { loadfiles_type = LoadFilesType::Multiple3MFOther; }
    if (normal_paths.size() > 1 && amf_files_count == normal_paths.size()) { loadfiles_type = LoadFilesType::Multiple3MF; }
    if (normal_paths.size() > 1 && amf_files_count == 0) { loadfiles_type = LoadFilesType::MultipleOther; }
    if (normal_paths.size() == 1 && amf_files_count == 1) { loadfiles_type = LoadFilesType::Single3MF; };
    if (normal_paths.size() == 1 && amf_files_count == 0) { loadfiles_type = LoadFilesType::SingleOther; };

    auto first_file = std::vector<fs::path>{};
    auto tmf_file   = std::vector<fs::path>{};
    auto other_file = std::vector<fs::path>{};
    auto res        = true;

    if (this->m_only_gcode || this->m_exported_file) {
        if ((loadfiles_type == LoadFilesType::SingleOther)
            || (loadfiles_type == LoadFilesType::MultipleOther)) {
            show_info(this, _L("Cannot add models when in preview mode!"), _L("Add Models"));
            return false;
        }
    }

    // Orca: Iters through given paths and imports files from zip then remove zip from paths
    // returns true if zip files were found
    auto handle_zips = [this](vector<fs::path>& paths) { // NOLINT(*-no-recursion) - Recursion is intended and should be managed properly
        bool res = false;
        for (auto it = paths.begin(); it != paths.end();) {
            if (boost::algorithm::iends_with(it->string(), ".zip")) {
                res = true;
                preview_zip_archive(*it);
                it = paths.erase(it);
            } else
                it++;
        }
        return res;
    };

    switch (loadfiles_type) {
    case LoadFilesType::Single3MF:
        open_3mf_file(normal_paths[0]);
        break;

    case LoadFilesType::SingleOther: {
        Plater::TakeSnapshot snapshot(this, snapshot_label);
        if (handle_zips(normal_paths)) return true;
        if (load_files(normal_paths, LoadStrategy::LoadModel, false).empty()) { res = false; }
        break;
    }
    case LoadFilesType::Multiple3MF:
        first_file = std::vector<fs::path>{normal_paths[0]};
        for (auto i = 0; i < normal_paths.size(); i++) {
            if (i > 0) { other_file.push_back(normal_paths[i]); }
        };

        open_3mf_file(first_file[0]);
        if (load_files(other_file, LoadStrategy::LoadModel).empty()) {  res = false;  }
        break;

    case LoadFilesType::MultipleOther: {
        Plater::TakeSnapshot snapshot(this, snapshot_label);
        if (handle_zips(normal_paths)) {
            if (normal_paths.empty()) return true;
        }
        if (load_files(normal_paths, LoadStrategy::LoadModel, true).empty()) { res = false; }
        break;
    }

    case LoadFilesType::Multiple3MFOther:
        for (const auto &path : normal_paths) {
            if (boost::iends_with(path.filename().string(), ".3mf")){
                if (first_file.size() <= 0)
                    first_file.push_back(path);
                else
                    tmf_file.push_back(path);
            } else {
                other_file.push_back(path);
            }
        }

        open_3mf_file(first_file[0]);
        if (load_files(tmf_file, LoadStrategy::LoadModel).empty()) {  res = false;  }
        if (res && handle_zips(other_file)) {
            if (normal_paths.empty()) return true;
        }
        if (load_files(other_file, LoadStrategy::LoadModel, false).empty()) {  res = false;  }
        break;
    default: break;
    }

    return res;
}

LoadType determine_load_type(std::string filename, std::string override_setting)
{
    std::string setting;

    if (override_setting != "") {
        setting = override_setting;
    } else {
        setting = wxGetApp().app_config->get(SETTING_PROJECT_LOAD_BEHAVIOUR);
    }

    if (setting == OPTION_PROJECT_LOAD_BEHAVIOUR_LOAD_GEOMETRY) {
        return LoadType::LoadGeometry;
    } else if (setting == OPTION_PROJECT_LOAD_BEHAVIOUR_ALWAYS_ASK) {
        ProjectDropDialog dlg(filename);
        if (dlg.ShowModal() == wxID_OK) {
            int      choice    = dlg.get_action();
            LoadType load_type = static_cast<LoadType>(choice);
            wxGetApp().app_config->set("import_project_action", std::to_string(choice));

            // BBS: jump to plater panel
            wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);
            return load_type;
        }

        return LoadType::Unknown; // Cancel
    } else {
        return LoadType::OpenProject;
    }
}

bool Plater::open_3mf_file(const fs::path &file_path)
{
    std::string filename = encode_path(file_path.filename().string().c_str());
    if (!boost::algorithm::iends_with(filename, ".3mf")) {
        return false;
    }

    bool not_empty_plate = !model().objects.empty();
    bool load_setting_ask_when_relevant = wxGetApp().app_config->get(SETTING_PROJECT_LOAD_BEHAVIOUR) == OPTION_PROJECT_LOAD_BEHAVIOUR_ASK_WHEN_RELEVANT;
    LoadType load_type = determine_load_type(filename, (not_empty_plate && load_setting_ask_when_relevant) ? OPTION_PROJECT_LOAD_BEHAVIOUR_ALWAYS_ASK : "");

    if (load_type == LoadType::Unknown) return false;

    switch (load_type) {
        case LoadType::OpenProject: {
            if (wxGetApp().can_load_project())
                load_project(from_path(file_path), "<loadall>");
            break;
        }
        case LoadType::LoadGeometry: {
            Plater::TakeSnapshot snapshot(this, "Import Object");
            load_files({file_path}, LoadStrategy::LoadModel);
            break;
        }
        case LoadType::LoadConfig: {
            load_files({file_path}, LoadStrategy::LoadConfig);
            break;
        }
        case LoadType::Unknown: {
            assert(false);
            break;
        }
    }

    return true;
}

int Plater::get_3mf_file_count(std::vector<fs::path> paths)
{
    auto count = 0;
    for (const auto &path : paths) {
        if (boost::iends_with(path.filename().string(), ".3mf")) {
            count++;
        }
    }
    return count;
}

void Plater::add_file()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << __LINE__ << " entry";
    wxArrayString input_files;
    wxGetApp().import_model(this, input_files);
    if (input_files.empty()) return;

    std::vector<fs::path> paths;
    for (const auto &file : input_files) paths.emplace_back(into_path(file));

    std::string snapshot_label;
    assert(!paths.empty());

    snapshot_label = "Import Objects";
    snapshot_label += ": ";
    snapshot_label += encode_path(paths.front().filename().string().c_str());
    for (size_t i = 1; i < paths.size(); ++i) {
        snapshot_label += ", ";
        snapshot_label += encode_path(paths[i].filename().string().c_str());
    }

    // BBS: check file types
    auto loadfiles_type  = LoadFilesType::NoFile;
    auto amf_files_count = get_3mf_file_count(paths);

    if (paths.size() > 1 && amf_files_count < paths.size()) { loadfiles_type = LoadFilesType::Multiple3MFOther; }
    if (paths.size() > 1 && amf_files_count == paths.size()) { loadfiles_type = LoadFilesType::Multiple3MF; }
    if (paths.size() > 1 && amf_files_count == 0) { loadfiles_type = LoadFilesType::MultipleOther; }
    if (paths.size() == 1 && amf_files_count == 1) { loadfiles_type = LoadFilesType::Single3MF; };
    if (paths.size() == 1 && amf_files_count == 0) { loadfiles_type = LoadFilesType::SingleOther; };

    auto first_file = std::vector<fs::path>{};
    auto tmf_file   = std::vector<fs::path>{};
    auto other_file = std::vector<fs::path>{};

    switch (loadfiles_type)
    {
    case LoadFilesType::Single3MF:
        open_3mf_file(paths[0]);
    	break;

    case LoadFilesType::SingleOther: {
        Plater::TakeSnapshot snapshot(this, snapshot_label);
        if (!load_files(paths, LoadStrategy::LoadModel, false).empty()) {
            if (get_project_name() == _L("Untitled") && paths.size() > 0) {
                boost::filesystem::path full_path(paths[0].string());
                p->set_project_name(from_u8(full_path.stem().string()));
            }
            wxGetApp().mainframe->update_title();
            if (wxGetApp().app_config->get("recent_models") == "true")
                wxGetApp().mainframe->add_to_recent_projects(paths[0].wstring());
        }
        break;
    }
    case LoadFilesType::Multiple3MF:
        first_file = std::vector<fs::path>{paths[0]};
        for (auto i = 0; i < paths.size(); i++) {
            if (i > 0) { other_file.push_back(paths[i]); }
        };

        open_3mf_file(first_file[0]);
        if (!load_files(other_file, LoadStrategy::LoadModel).empty()) { wxGetApp().mainframe->update_title(); }
        break;

    case LoadFilesType::MultipleOther: {
        Plater::TakeSnapshot snapshot(this, snapshot_label);
        if (!load_files(paths, LoadStrategy::LoadModel, true).empty()) {
            if (get_project_name() == _L("Untitled") && paths.size() > 0) {
                boost::filesystem::path full_path(paths[0].string());
                p->set_project_name(from_u8(full_path.stem().string()));
            }
            wxGetApp().mainframe->update_title();
            if (wxGetApp().app_config->get("recent_models") == "true")
                for (auto &path : paths)
                    wxGetApp().mainframe->add_to_recent_projects(path.wstring());
        }
        break;
    }
    case LoadFilesType::Multiple3MFOther:
        for (const auto &path : paths) {
            if (boost::iends_with(path.filename().string(), ".3mf")) {
                if (first_file.size() <= 0)
                    first_file.push_back(path);
                else
                    tmf_file.push_back(path);
            } else {
                other_file.push_back(path);
            }
        }

        open_3mf_file(first_file[0]);
        load_files(tmf_file, LoadStrategy::LoadModel);
        if (!load_files(other_file, LoadStrategy::LoadModel, false).empty()) {
            wxGetApp().mainframe->update_title();
            if (wxGetApp().app_config->get("recent_models") == "true")
                for (auto &file : other_file)
                    wxGetApp().mainframe->add_to_recent_projects(file.wstring());
        }
        break;
    default:break;
    }
}

void Plater::update(bool conside_update_flag, bool force_background_processing_update)
{
    if (is_new_project_and_check_state()) {
        return;
    }
    unsigned int flag = force_background_processing_update ? (unsigned int)Plater::priv::UpdateParams::FORCE_BACKGROUND_PROCESSING_UPDATE : 0;
    if (conside_update_flag) {
        if (need_update()) {
            p->update(flag);
            p->set_need_update(false);
        }
    }
    else
        p->update(flag);
}

void Plater::object_list_changed() { p->object_list_changed(); }

Worker &Plater::get_ui_job_worker() { return p->m_worker; }

const Worker &Plater::get_ui_job_worker() const { return p->m_worker; }

void Plater::update_ui_from_settings() { p->update_ui_from_settings(); }

void Plater::select_view(const std::string& direction) { p->select_view(direction); }

//BBS: add no_slice logic
void Plater::select_view_3D(const std::string& name, bool no_slice) { p->select_view_3D(name, no_slice); }

void Plater::reload_paint_after_background_process_apply() {
    p->preview->set_reload_paint_after_background_process_apply(true);
}

bool Plater::is_preview_shown() const { return p->is_preview_shown(); }
bool Plater::is_preview_loaded() const { return p->is_preview_loaded(); }
bool Plater::is_view3D_shown() const { return p->is_view3D_shown(); }

bool Plater::are_view3D_labels_shown() const { return p->are_view3D_labels_shown(); }
void Plater::show_view3D_labels(bool show) { p->show_view3D_labels(show); }

bool Plater::is_view3D_overhang_shown() const { return p->is_view3D_overhang_shown(); }
void Plater::show_view3D_overhang(bool show)  {  p->show_view3D_overhang(show); }

bool Plater::is_sidebar_enabled() const { return p->sidebar_layout.is_enabled; }
void Plater::enable_sidebar(bool enabled) { p->enable_sidebar(enabled); }
bool Plater::is_sidebar_collapsed() const { return p->sidebar_layout.is_collapsed; }
void Plater::collapse_sidebar(bool collapse) { p->collapse_sidebar(collapse); }
Sidebar::DockingState Plater::get_sidebar_docking_state() const { return p->get_sidebar_docking_state(); }

void Plater::reset_window_layout() { p->reset_window_layout(); }

//BBS
void Plater::select_curr_plate_all() { p->select_curr_plate_all(); }
void Plater::remove_curr_plate_all() { p->remove_curr_plate_all(); }

void Plater::select_all() { p->select_all(); }
void Plater::deselect_all() { p->deselect_all(); }
void Plater::exit_gizmo() { p->exit_gizmo(); }

void Plater::remove(size_t obj_idx) { p->remove(obj_idx); }
void Plater::reset(bool apply_presets_change) { p->reset(apply_presets_change); }
void Plater::reset_with_confirm()
{
    if (p->model.objects.empty() || MessageDialog(static_cast<wxWindow *>(this), _L("All objects will be removed, continue?"),
                                                  wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Delete all"), wxYES_NO | wxCANCEL | wxYES_DEFAULT | wxCENTRE)
                                            .ShowModal() == wxID_YES) {
        reset();
        // BBS: jump to plater panel
        wxGetApp().mainframe->select_tab(size_t(0));
    }
}

// BBS: save logic
int GUI::Plater::close_with_confirm(std::function<bool(bool)> second_check)
{
    if (up_to_date(false, false)) {
        if (second_check && !second_check(false)) return wxID_CANCEL;
        model().set_backup_path("");
        return wxID_NO;
    }

    MessageDialog dlg(static_cast<wxWindow*>(this), _L("The current project has unsaved changes, save it before continue?"),
        wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Save"), wxYES_NO | wxCANCEL | wxYES_DEFAULT | wxCENTRE);
    dlg.show_dsa_button(_L("Remember my choice."));
    auto choise = wxGetApp().app_config->get("save_project_choise");
    auto result = choise.empty() ? dlg.ShowModal() : choise == "yes" ? wxID_YES : wxID_NO;
    if (result == wxID_CANCEL)
        return result;
    else {
        if (dlg.get_checkbox_state())
            wxGetApp().app_config->set("save_project_choise", result == wxID_YES ? "yes" : "no");
        if (result == wxID_YES) {
            result = save_project();
            if (result == wxID_CANCEL) {
                if (choise.empty())
                    return result;
                else
                    result = wxID_NO;
            }
        }
    }

    if (second_check && !second_check(result == wxID_YES)) return wxID_CANCEL;

    model().set_backup_path("");
    up_to_date(true, false);
    up_to_date(true, true);

    return result;
}

//BBS: trigger a restore project event
void Plater::trigger_restore_project(int skip_confirm)
{
    auto evt = new wxCommandEvent(EVT_RESTORE_PROJECT, this->GetId());
    evt->SetInt(skip_confirm);
    wxQueueEvent(this, evt);
    //wxPostEvent(this, *evt);
}

//BBS
bool Plater::delete_object_from_model(size_t obj_idx, bool refresh_immediately) { return p->delete_object_from_model(obj_idx, refresh_immediately); }

//BBS: delete all from model
void Plater::delete_all_objects_from_model()
{
    p->delete_all_objects_from_model();
}

void Plater::set_selected_visible(bool visible)
{
    if (p->get_curr_selection().is_empty())
        return;

    Plater::TakeSnapshot snapshot(this, "Set Selected Objects Visible in AssembleView");
    get_ui_job_worker().cancel_all();

    p->get_current_canvas3D()->set_selected_visible(visible);
}


void Plater::remove_selected()
{
    /*if (p->get_selection().is_empty())
        return;*/
    if (p->get_curr_selection().is_empty())
        return;

    // BBS: check before deleting object
    if (!p->can_delete())
        return;

    Plater::TakeSnapshot snapshot(this, "Delete Selected Objects");
    get_ui_job_worker().cancel_all();

    //BBS delete current selected
    // p->view3D->delete_selected();
    p->get_current_canvas3D()->delete_selected();
}

void Plater::increase_instances(size_t num)
{
    if (! can_increase_instances()) { return; }

    Plater::TakeSnapshot snapshot(this, "Increase Instances");

    int obj_idx = p->get_selected_object_idx();

    ModelObject* model_object = p->model.objects[obj_idx];
    ModelInstance* model_instance = model_object->instances.back();

    bool was_one_instance = model_object->instances.size()==1;

    double offset_base = canvas3D()->get_size_proportional_to_max_bed_size(0.05);
    double offset = offset_base;
    for (size_t i = 0; i < num; i++, offset += offset_base) {
        Vec3d offset_vec = model_instance->get_offset() + Vec3d(offset, offset, 0.0);
        model_object->add_instance(offset_vec, model_instance->get_scaling_factor(), model_instance->get_rotation(), model_instance->get_mirror());
//        p->print.get_object(obj_idx)->add_copy(Slic3r::to_2d(offset_vec));
    }

#ifdef SUPPORT_AUTO_CENTER
    if (p->get_config("autocenter") == "true")
        arrange();
#endif

    p->update();

    p->get_selection().add_instance(obj_idx, (int)model_object->instances.size() - 1);

    sidebar().obj_list()->increase_object_instances(obj_idx, was_one_instance ? num + 1 : num);

    p->selection_changed();
    this->p->schedule_background_process();
    //if (wxGetApp().app_config->get("auto_arrange") == "true") {
    //    this->set_prepare_state(Job::PREPARE_STATE_MENU);
    //    this->arrange();
    //}
}

void Plater::decrease_instances(size_t num)
{
    if (! can_decrease_instances()) { return; }

    Plater::TakeSnapshot snapshot(this, "Decrease Instances");

    int obj_idx = p->get_selected_object_idx();

    ModelObject* model_object = p->model.objects[obj_idx];
    if (model_object->instances.size() > num) {
        for (size_t i = 0; i < num; ++ i)
            model_object->delete_last_instance();
        p->update();
        // Delete object from Sidebar list. Do it after update, so that the GLScene selection is updated with the modified model.
        sidebar().obj_list()->decrease_object_instances(obj_idx, num);
    }
    else {
        remove(obj_idx);
    }

    if (!model_object->instances.empty())
        p->get_selection().add_instance(obj_idx, (int)model_object->instances.size() - 1);

    p->selection_changed();
    this->p->schedule_background_process();
    //if (wxGetApp().app_config->get("auto_arrange") == "true") {
    //    this->set_prepare_state(Job::PREPARE_STATE_MENU);
    //    this->arrange();
    //}
}

static long GetNumberFromUser(  const wxString& msg,
                                const wxString& prompt,
                                const wxString& title,
                                long value,
                                long min,
                                long max,
                                wxWindow* parent)
{
#ifdef _WIN32
    wxNumberEntryDialog dialog(parent, msg, prompt, title, value, min, max, wxDefaultPosition);
    wxGetApp().UpdateDlgDarkUI(&dialog);
    if (dialog.ShowModal() == wxID_OK)
        return dialog.GetValue();

    return -1;
#else
    return wxGetNumberFromUser(msg, prompt, title, value, min, max, parent);
#endif
}

void Plater::set_number_of_copies(/*size_t num*/)
{
    int obj_idx = p->get_selected_object_idx();
    if (obj_idx == -1)
        return;

    ModelObject* model_object = p->model.objects[obj_idx];

    const int num = GetNumberFromUser( " ", _L("Number of copies:"),
                                    _L("Copies of the selected object"), model_object->instances.size(), 0, 1000, this );
    if (num < 0)
        return;

    Plater::TakeSnapshot snapshot(this, (boost::format("Set numbers of copies to %1%")%num).str());

    int diff = num - (int)model_object->instances.size();
    if (diff > 0)
        increase_instances(diff);
    else if (diff < 0)
        decrease_instances(-diff);
}

void Plater::fill_bed_with_copies()
{
    auto &w = get_ui_job_worker();
    if (w.is_idle()) {
        p->take_snapshot(_u8L("Arrange"));
        replace_job(w, std::make_unique<FillBedJob>());
    }
}

void Plater::fill_bed_with_instances()
{
    auto &w = get_ui_job_worker();
    if (w.is_idle()) {
        p->take_snapshot(_u8L("Arrange"));
        replace_job(w, std::make_unique<FillBedJob>(true));
    }
}

bool Plater::is_selection_empty() const
{
    return p->get_selection().is_empty() || p->get_selection().is_wipe_tower();
}

void Plater::scale_selection_to_fit_print_volume()
{
    p->scale_selection_to_fit_print_volume();
}

void Plater::convert_unit(ConversionType conv_type)
{
    std::vector<int> obj_idxs, volume_idxs;
    wxGetApp().obj_list()->get_selection_indexes(obj_idxs, volume_idxs);
    if (obj_idxs.empty() && volume_idxs.empty())
        return;

    TakeSnapshot snapshot(this, conv_type == ConversionType::CONV_FROM_INCH  ? "Convert from imperial units" :
                                conv_type == ConversionType::CONV_TO_INCH    ? "Revert conversion from imperial units" :
                                conv_type == ConversionType::CONV_FROM_METER ? "Convert from meters" : "Revert conversion from meters");
    wxBusyCursor wait;

    ModelObjectPtrs objects;
    std::reverse(obj_idxs.begin(), obj_idxs.end());
    for (int obj_idx : obj_idxs) {
        ModelObject *object = p->model.objects[obj_idx];
        object->convert_units(objects, conv_type, volume_idxs);
        remove(obj_idx);
    }
    std::reverse(objects.begin(), objects.end());
    p->load_model_objects(objects);

    Selection& selection = p->view3D->get_canvas3d()->get_selection();
    size_t last_obj_idx = p->model.objects.size() - 1;

    if (volume_idxs.empty()) {
        for (size_t i = 0; i < objects.size(); ++i)
            selection.add_object((unsigned int)(last_obj_idx - i), i == 0);
    }
    else {
        for (int vol_idx : volume_idxs)
            selection.add_volume(last_obj_idx, vol_idx, 0, false);
    }
}

void Plater::apply_cut_object_to_model(size_t obj_idx, const ModelObjectPtrs& new_objects)
{
    model().delete_object(obj_idx);
    sidebar().obj_list()->delete_object_from_list(obj_idx);

    // suppress to call selection update for Object List to avoid call of early Gizmos on/off update
    p->load_model_objects(new_objects, false, false);

    // now process all updates of the 3d scene
    update();
    // Update InfoItems in ObjectList after update() to use of a correct value of the GLCanvas3D::is_sinking(),
    // which is updated after a view3D->reload_scene(false, flags & (unsigned int)UpdateParams::FORCE_FULL_SCREEN_REFRESH) call
    for (size_t idx = 0; idx < p->model.objects.size(); idx++)
        wxGetApp().obj_list()->update_info_items(idx);

    Selection& selection = p->get_selection();
    size_t last_id = p->model.objects.size() - 1;
    for (size_t i = 0; i < new_objects.size(); ++i)
        selection.add_object((unsigned int)(last_id - i), i == 0);

    // UIThreadWorker w;
    // arrange(w, true);
    // w.wait_for_idle();
}

void Plater::export_gcode(bool prefer_removable)
{
    if (p->model.objects.empty())
        return;

    //if (get_view3D_canvas3D()->get_gizmos_manager().is_in_editing_mode(true))
    //    return;

    if (p->process_completed_with_error == p->partplate_list.get_curr_plate_index())
        return;

    // If possible, remove accents from accented latin characters.
    // This function is useful for generating file names to be processed by legacy firmwares.
    fs::path default_output_file;
    try {
        // Update the background processing, so that the placeholder parser will get the correct values for the ouput file template.
        // Also if there is something wrong with the current configuration, a pop-up dialog will be shown and the export will not be performed.
        unsigned int state = this->p->update_restart_background_process(false, false);
        if (state & priv::UPDATE_BACKGROUND_PROCESS_INVALID)
            return;
        default_output_file = this->p->background_process.output_filepath_for_project("");
    } catch (const Slic3r::PlaceholderParserError &ex) {
        // Show the error with monospaced font.
        show_error(this, ex.what(), true);
        return;
    } catch (const std::exception &ex) {
        show_error(this, ex.what(), false);
        return;
    }
    default_output_file = fs::path(Slic3r::fold_utf8_to_ascii(default_output_file.string()));
    AppConfig 				&appconfig 				 = *wxGetApp().app_config;
    RemovableDriveManager 	&removable_drive_manager = *wxGetApp().removable_drive_manager();
    // Get a last save path, either to removable media or to an internal media.
    std::string      		 start_dir 				 = appconfig.get_last_output_dir(default_output_file.parent_path().string(), prefer_removable);
    if (prefer_removable) {
        // Returns a path to a removable media if it exists, prefering start_dir. Update the internal removable drives database.
        start_dir = removable_drive_manager.get_removable_drive_path(start_dir);
        if (start_dir.empty())
            // Direct user to the last internal media.
            start_dir = appconfig.get_last_output_dir(default_output_file.parent_path().string(), false);
    }

    fs::path output_path;
    {
        std::string ext = default_output_file.extension().string();
        wxFileDialog dlg(this, (printer_technology() == ptFFF) ? _L("Save G-code file as:") : _L("Save SLA file as:"),
            start_dir,
            from_path(default_output_file.filename()),
            GUI::file_wildcards((printer_technology() == ptFFF) ? FT_GCODE : FT_SL1, ext),
            wxFD_SAVE | wxFD_OVERWRITE_PROMPT
        );
        if (dlg.ShowModal() == wxID_OK) {
            output_path = into_path(dlg.GetPath());
            while (has_illegal_filename_characters(output_path.filename().string())) {
                show_error(this, _L("The provided file name is not valid.") + "\n" +
                    _L("The following characters are not allowed by a FAT file system:") + " <>:/\\|?*\"");
                dlg.SetFilename(from_path(output_path.filename()));
                if (dlg.ShowModal() == wxID_OK)
                    output_path = into_path(dlg.GetPath());
                else {
                    output_path.clear();
                    break;
                }
            }
        }
    }

    if (! output_path.empty()) {
        bool path_on_removable_media = removable_drive_manager.set_and_verify_last_save_path(output_path.string());
        //bool path_on_removable_media = false;
        p->notification_manager->new_export_began(path_on_removable_media);
        p->exporting_status = path_on_removable_media ? ExportingStatus::EXPORTING_TO_REMOVABLE : ExportingStatus::EXPORTING_TO_LOCAL;
        p->last_output_path = output_path.string();
        p->last_output_dir_path = output_path.parent_path().string();
        p->export_gcode(output_path, path_on_removable_media);
        // Storing a path to AppConfig either as path to removable media or a path to internal media.
        // is_path_on_removable_drive() is called with the "true" parameter to update its internal database as the user may have shuffled the external drives
        // while the dialog was open.
        appconfig.update_last_output_dir(output_path.parent_path().string(), path_on_removable_media);

        try {
            json j;
            auto printer_config = Slic3r::GUI::wxGetApp().preset_bundle->printers.get_edited_preset_with_vendor_profile().preset;
            if (printer_config.is_system) {
                j["printer_preset"] = printer_config.name;
            } else {
                j["printer_preset"] = printer_config.config.opt_string("inherits");
            }

            PresetBundle *preset_bundle = wxGetApp().preset_bundle;
            if (preset_bundle) {
                j["gcode_printer_model"] = preset_bundle->printers.get_edited_preset().get_printer_type(preset_bundle);
            }
            NetworkAgent *agent = wxGetApp().getAgent();
        } catch (...) {}

    }
}

void Plater::send_to_printer(bool isall)
{
    p->on_action_send_to_printer(isall);
}

//BBS export gcode 3mf to file
void Plater::export_gcode_3mf(bool export_all)
{
    if (p->model.objects.empty())
        return;

    if (p->process_completed_with_error == p->partplate_list.get_curr_plate_index())
        return;

    //calc default_output_file, get default output file from background process
    fs::path default_output_file;
    AppConfig& appconfig = *wxGetApp().app_config;
    std::string start_dir;
    try {
        // Update the background processing, so that the placeholder parser will get the correct values for the ouput file template.
        // Also if there is something wrong with the current configuration, a pop-up dialog will be shown and the export will not be performed.
        unsigned int state = this->p->update_restart_background_process(false, false);
        if (state & priv::UPDATE_BACKGROUND_PROCESS_INVALID)
            return;
        default_output_file = this->p->background_process.output_filepath_for_project("");
    }
    catch (const Slic3r::PlaceholderParserError& ex) {
        // Show the error with monospaced font.
        show_error(this, ex.what(), true);
        return;
    }
    catch (const std::exception& ex) {
        show_error(this, ex.what(), false);
        return;
    }
    default_output_file.replace_extension(".gcode.3mf");
    default_output_file = fs::path(Slic3r::fold_utf8_to_ascii(default_output_file.string()));

    //Get a last save path
    start_dir = appconfig.get_last_output_dir(default_output_file.parent_path().string(), false);

    fs::path output_path;
    {
        std::string ext = default_output_file.extension().string();
        wxFileDialog dlg(this, _L("Save Sliced file as:"),
            start_dir,
            from_path(default_output_file.filename()),
            GUI::file_wildcards(FT_GCODE_3MF, ""),
            wxFD_SAVE | wxFD_OVERWRITE_PROMPT
        );
        if (dlg.ShowModal() == wxID_OK) {
            output_path = into_path(dlg.GetPath());
            ext = output_path.extension().string();
            if (ext != ".3mf")
                output_path = output_path.string() + ".3mf";
        }
    }

    if (!output_path.empty()) {
        //BBS do not set to removable media path
        bool path_on_removable_media = false;
        p->notification_manager->new_export_began(path_on_removable_media);
        p->exporting_status = path_on_removable_media ? ExportingStatus::EXPORTING_TO_REMOVABLE : ExportingStatus::EXPORTING_TO_LOCAL;
        //BBS do not save last output path
        p->last_output_path = output_path.string();
        p->last_output_dir_path = output_path.parent_path().string();
        int plate_idx = get_partplate_list().get_curr_plate_index();
        if (export_all)
            plate_idx = PLATE_ALL_IDX;
        export_3mf(output_path, SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::WithGcode | SaveStrategy::SkipModel, plate_idx); // BBS: silence

        RemovableDriveManager& removable_drive_manager = *wxGetApp().removable_drive_manager();


        bool on_removable = removable_drive_manager.is_path_on_removable_drive(p->last_output_dir_path);


        // update last output dir
        appconfig.update_last_output_dir(output_path.parent_path().string(), false);
        p->notification_manager->push_exporting_finished_notification(output_path.string(), p->last_output_dir_path, on_removable);
    }
}

void Plater::send_gcode_finish(wxString name)
{
    auto out_str = GUI::format(_L("The file %s has been sent to the printer's storage space and can be viewed on the printer."), name);
    p->notification_manager->push_exporting_finished_notification(out_str, "", false);
}

void Plater::export_core_3mf()
{
    wxString path = p->get_export_file(FT_3MF);
    if (path.empty()) { return; }
    const std::string path_u8 = into_u8(path);
    export_3mf(path_u8, SaveStrategy::Silence);
}

Preset *get_printer_preset(const MachineObject *obj)
{
    if (!obj)
        return nullptr;

    Preset       *printer_preset = nullptr;
    float machine_nozzle_diameter = obj->GetExtderSystem()->GetNozzleDiameter(0);
    PresetBundle *preset_bundle  = wxGetApp().preset_bundle;
    for (auto printer_it = preset_bundle->printers.begin(); printer_it != preset_bundle->printers.end(); printer_it++) {
        // only use system printer preset
        if (!printer_it->is_system)
            continue;

        ConfigOption       *printer_nozzle_opt  = printer_it->config.option("nozzle_diameter");
        ConfigOptionFloats *printer_nozzle_vals = nullptr;
        if (printer_nozzle_opt) printer_nozzle_vals = dynamic_cast<ConfigOptionFloats *>(printer_nozzle_opt);
        std::string model_id = printer_it->get_current_printer_type(preset_bundle);

        std::string printer_type = obj->get_show_printer_type();
        if (model_id.compare(printer_type) == 0 && printer_nozzle_vals && abs(printer_nozzle_vals->get_at(0) - machine_nozzle_diameter) < 1e-3) {
            printer_preset = &(*printer_it);
        }
    }
    return printer_preset;
}

bool Plater::check_printer_initialized(MachineObject *obj, bool only_warning, bool popup_warning)
{
    if (!obj)
        return false;

    bool has_been_initialized = true;

    const auto& extruders = obj->GetExtderSystem()->GetExtruders();
    for (const DevExtder& extruder : extruders) {
        if (obj->is_multi_extruders()) {
            if (extruder.GetNozzleFlowType() == NozzleFlowType::NONE_FLOWTYPE) {
                has_been_initialized = false;
                break;
            }
        }
        if (extruder.GetNozzleFlowType() == NozzleFlowType::NONE_FLOWTYPE) {
            has_been_initialized = false;
            break;
        }
    }

    if (!has_been_initialized) {
        if (popup_warning) {
            if (!only_warning) {
                if (DevPrinterConfigUtil::get_printer_can_set_nozzle(obj->get_show_printer_type())) {
                    MessageDialog dlg(wxGetApp().plater(), _L("The nozzle type is not set. Please set the nozzle and try again."), _L("Warning"), wxOK | wxICON_WARNING);
                    dlg.ShowModal();
                } else {
                    MessageDialog dlg(wxGetApp().plater(), _L("The nozzle type is not set. Please check."), _L("Warning"), wxOK | wxICON_WARNING);
                    dlg.ShowModal();
                }

                PrinterPartsDialog *print_parts_dlg = new PrinterPartsDialog(nullptr);
                print_parts_dlg->update_machine_obj(obj);
                print_parts_dlg->ShowModal();
            } else {
                auto printer_name = get_selected_printer_name_in_combox(); // wxString(obj->get_preset_printer_model_name(machine_print_name))
                pop_warning_and_go_to_device_page(printer_name, Plater::PrinterWarningType::NOT_CONNECTED, _L("Sync printer information"));
            }
        }
        return false;
    }
    return true;
}

// Following lambda generates a combined mesh for export with normals pointing outwards.
TriangleMesh Plater::combine_mesh_fff(const ModelObject& mo, int instance_id, std::function<void(const std::string&)> notify_func)
{
    TriangleMesh mesh;

    std::vector<csg::CSGPart> csgmesh;
    csgmesh.reserve(2 * mo.volumes.size());
    bool has_splitable_volume = csg::model_to_csgmesh(mo, Transform3d::Identity(), std::back_inserter(csgmesh),
        csg::mpartsPositive | csg::mpartsNegative);
        
    std::string fail_msg = _u8L("Unable to perform boolean operation on model meshes. "
        "Only positive parts will be kept. You may fix the meshes and try again.");
    if (auto fail_reason_name = csg::check_csgmesh_booleans(Range{ std::begin(csgmesh), std::end(csgmesh) }); std::get<0>(fail_reason_name) != csg::BooleanFailReason::OK) {
        std::string name = std::get<1>(fail_reason_name);
        std::map<csg::BooleanFailReason, std::string> fail_reasons = {
            {csg::BooleanFailReason::OK, "OK"},
            {csg::BooleanFailReason::MeshEmpty, Slic3r::format( _u8L("Reason: part \"%1%\" is empty."), name)},
            {csg::BooleanFailReason::NotBoundAVolume, Slic3r::format(_u8L("Reason: part \"%1%\" does not bound a volume."), name)},
            {csg::BooleanFailReason::SelfIntersect, Slic3r::format(_u8L("Reason: part \"%1%\" has self intersection."), name)},
            {csg::BooleanFailReason::NoIntersection, Slic3r::format(_u8L("Reason: \"%1%\" and another part have no intersection."), name)} };
        fail_msg += " " + fail_reasons[std::get<0>(fail_reason_name)];
    }
    else {
        try {
            MeshBoolean::mcut::McutMeshPtr meshPtr = csg::perform_csgmesh_booleans_mcut(Range{ std::begin(csgmesh), std::end(csgmesh) });
            mesh = MeshBoolean::mcut::mcut_to_triangle_mesh(*meshPtr);
        }
        catch (...) {}
#if 0
        // if mcut fails, try again with CGAL
        if (mesh.empty()) {
            try {
                auto meshPtr = csg::perform_csgmesh_booleans(Range{ std::begin(csgmesh), std::end(csgmesh) });
                mesh = MeshBoolean::cgal::cgal_to_triangle_mesh(*meshPtr);
                }
            catch (...) {}
        }
#endif
    }

    if (mesh.empty()) {
        if (notify_func)
            notify_func(fail_msg);

        for (const ModelVolume* v : mo.volumes)
            if (v->is_model_part()) {
                TriangleMesh vol_mesh(v->mesh());
                vol_mesh.transform(v->get_matrix(), true);
                mesh.merge(vol_mesh);
            }
    }

    if (instance_id == -1) {
        TriangleMesh vols_mesh(mesh);
        mesh = TriangleMesh();
        for (const ModelInstance* i : mo.instances) {
            TriangleMesh m = vols_mesh;
            m.transform(i->get_matrix(), true);
            mesh.merge(m);
        }
    }
    else if (0 <= instance_id && instance_id < int(mo.instances.size()))
        mesh.transform(mo.instances[instance_id]->get_matrix(), true);
    return mesh;
}

// BBS export with/without boolean, however, stil merge mesh
#define EXPORT_WITH_BOOLEAN 0
void Plater::export_stl(bool extended, bool selection_only, bool multi_stls)
{
    if (p->model.objects.empty()) { return; }

    wxString path;
    if (multi_stls) {
        wxDirDialog dlg(this, _L("Choose a directory"), from_u8(wxGetApp().app_config->get_last_dir()),
                        wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK) {
            path = dlg.GetPath() + "/";
        }
    } else {
        path = p->get_export_file(FT_STL);
    }
    if (path.empty()) { return; }
    const std::string path_u8 = into_u8(path);

    wxBusyCursor wait;
    const auto& selection = p->get_selection();
    const auto obj_idx = selection.get_object_idx();

#if EXPORT_WITH_BOOLEAN
    if (selection_only && (obj_idx == -1 || selection.is_wipe_tower()))
        return;
#else
    // BBS support selecting multiple objects
    if (selection_only && selection.is_wipe_tower()) return;

    // BBS
    if (selection_only) {
        // only support selection single full object and mulitiple full object
        if (!selection.is_single_full_object() && !selection.is_multiple_full_object()) return;
    }

    // Following lambda generates a combined mesh for export with normals pointing outwards.
    auto mesh_to_export_fff_no_boolean = [this](const ModelObject &mo, int instance_id) {
        TriangleMesh mesh;

        //Prusa export negative parts
        std::vector<csg::CSGPart> csgmesh;
        csgmesh.reserve(2 * mo.volumes.size());
        csg::model_to_csgmesh(mo, Transform3d::Identity(), std::back_inserter(csgmesh),
                              csg::mpartsPositive | csg::mpartsNegative | csg::mpartsDoSplits);

        auto csgrange = range(csgmesh);
        if (csg::is_all_positive(csgrange)) {
            mesh = TriangleMesh{csg::csgmesh_merge_positive_parts(csgrange)};
        } else if (std::get<2>(csg::check_csgmesh_booleans(csgrange)) == csgrange.end()) {
            try {
                auto cgalm = csg::perform_csgmesh_booleans(csgrange);
                mesh = MeshBoolean::cgal::cgal_to_triangle_mesh(*cgalm);
            } catch (...) {}
        }

        if (mesh.empty()) {
            get_notification_manager()->push_plater_error_notification(
                _u8L("Unable to perform boolean operation on model meshes. "
                     "Only positive parts will be exported."));

            for (const ModelVolume* v : mo.volumes)
                if (v->is_model_part()) {
                    TriangleMesh vol_mesh(v->mesh());
                    vol_mesh.transform(v->get_matrix(), true);
                    mesh.merge(vol_mesh);
                }
        }
        if (instance_id == -1) {
            TriangleMesh vols_mesh(mesh);
            mesh = TriangleMesh();
            for (const ModelInstance *i : mo.instances) {
                TriangleMesh m = vols_mesh;
                m.transform(i->get_matrix(), true);
                mesh.merge(m);
            }
        } else if (0 <= instance_id && instance_id < int(mo.instances.size()))
            mesh.transform(mo.instances[instance_id]->get_matrix(), true);
        return mesh;
    };
#endif
    auto mesh_to_export_sla = [&, this](const ModelObject& mo, int instance_id) {
        TriangleMesh mesh;

        const SLAPrintObject *object = this->p->sla_print.get_print_object_by_model_object_id(mo.id());

        if (auto m = object->get_mesh_to_print(); m.empty())
            mesh = combine_mesh_fff(mo, instance_id, [this](const std::string& msg) {return get_notification_manager()->push_general_error_notification(msg); });
        else {
            const Transform3d mesh_trafo_inv = object->trafo().inverse();
            const bool is_left_handed = object->is_left_handed();

            auto pad_mesh = extended? object->pad_mesh() : TriangleMesh{};
            pad_mesh.transform(mesh_trafo_inv);

            auto supports_mesh = extended ? object->support_mesh() : TriangleMesh{};
            supports_mesh.transform(mesh_trafo_inv);

            const std::vector<SLAPrintObject::Instance>& obj_instances = object->instances();
            for (const SLAPrintObject::Instance& obj_instance : obj_instances) {
                auto it = std::find_if(object->model_object()->instances.begin(), object->model_object()->instances.end(),
                                       [&obj_instance](const ModelInstance *mi) { return mi->id() == obj_instance.instance_id; });
                assert(it != object->model_object()->instances.end());

                if (it != object->model_object()->instances.end()) {
                    const bool one_inst_only = selection_only && ! selection.is_single_full_object();

                    const int instance_idx = it - object->model_object()->instances.begin();
                    const Transform3d& inst_transform = one_inst_only
                                                            ? Transform3d::Identity()
                                                            : object->model_object()->instances[instance_idx]->get_transformation().get_matrix();

                    TriangleMesh inst_mesh;

                    if (!pad_mesh.empty()) {
                        TriangleMesh inst_pad_mesh = pad_mesh;
                        inst_pad_mesh.transform(inst_transform, is_left_handed);
                        inst_mesh.merge(inst_pad_mesh);
                    }

                    if (!supports_mesh.empty()) {
                        TriangleMesh inst_supports_mesh = supports_mesh;
                        inst_supports_mesh.transform(inst_transform, is_left_handed);
                        inst_mesh.merge(inst_supports_mesh);
                    }

                    TriangleMesh inst_object_mesh = object->get_mesh_to_print();

                    inst_object_mesh.transform(mesh_trafo_inv);
                    inst_object_mesh.transform(inst_transform, is_left_handed);

                    inst_mesh.merge(inst_object_mesh);

                           // ensure that the instance lays on the bed
                    inst_mesh.translate(0.0f, 0.0f, -inst_mesh.bounding_box().min.z());

                           // merge instance with global mesh
                    mesh.merge(inst_mesh);

                    if (one_inst_only)
                        break;
                }
            }
        }

        return mesh;
    };

    std::function<TriangleMesh(const ModelObject& mo, int instance_id)>
        mesh_to_export;

    if (p->printer_technology == ptFFF)
#if EXPORT_WITH_BOOLEAN
        mesh_to_export = [this](const ModelObject& mo, int instance_id) {return Plater::combine_mesh_fff(mo, instance_id,
            [this](const std::string& msg) {return get_notification_manager()->push_general_error_notification(msg); }); };
#else
        mesh_to_export = mesh_to_export_fff_no_boolean;
#endif
    else
        mesh_to_export = mesh_to_export_sla;

    auto get_save_file = [](std::string const & dir, std::string const & name) {
        auto path = dir + name + ".stl";
        int n = 1;
        while (boost::filesystem::exists(path))
            path = dir + name + "(" + std::to_string(n++) + ").stl";
        return path;
    };

    TriangleMesh mesh;
    if (selection_only) {
        if (selection.is_single_full_object()) {
            const auto obj_idx = selection.get_object_idx();
            const ModelObject* model_object = p->model.objects[obj_idx];
            if (selection.get_mode() == Selection::Instance)
                mesh = mesh_to_export(*model_object, (model_object->instances.size() > 1) ? -1 : selection.get_instance_idx());
            else {
                const GLVolume* volume = selection.get_first_volume();
                mesh = model_object->volumes[volume->volume_idx()]->mesh();
                mesh.transform(volume->get_volume_transformation().get_matrix(), true);
            }

            if (model_object->instances.size() == 1) mesh.translate(-model_object->origin_translation.cast<float>());
        }
        else if (selection.is_multiple_full_object() && !multi_stls) {
            const std::set<std::pair<int, int>>& instances_idxs = p->get_selection().get_selected_object_instances();
            for (const std::pair<int, int>& i : instances_idxs) {
                ModelObject* object = p->model.objects[i.first];
                mesh.merge(mesh_to_export(*object, i.second));
            }
        }
        else if (selection.is_multiple_full_object() && multi_stls) {
            const std::set<std::pair<int, int>> &instances_idxs = p->get_selection().get_selected_object_instances();
            for (const std::pair<int, int> &i : instances_idxs) {
                ModelObject *object = p->model.objects[i.first];
                auto mesh = mesh_to_export(*object, i.second);
                mesh.translate(-object->origin_translation.cast<float>());

                Slic3r::store_stl(get_save_file(path_u8, object->name).c_str(), &mesh, true);
            }
            return;
        }
    }
    else if (!multi_stls) {
        for (const ModelObject* o : p->model.objects) {
            mesh.merge(mesh_to_export(*o, -1));
        }
    } else {
        for (const ModelObject* o : p->model.objects) {
            auto mesh = mesh_to_export(*o, -1);
            mesh.translate(-o->origin_translation.cast<float>());
            Slic3r::store_stl(get_save_file(path_u8, o->name).c_str(), &mesh, true);
        }
        return;
    }

    Slic3r::store_stl(path_u8.c_str(), &mesh, true);
}

//BBS: remove amf export
/*void Plater::export_amf()
{
    if (p->model.objects.empty()) { return; }

    wxString path = p->get_export_file(FT_AMF);
    if (path.empty()) { return; }
    const std::string path_u8 = into_u8(path);

    wxBusyCursor wait;
    bool export_config = true;
    DynamicPrintConfig cfg = wxGetApp().preset_bundle->full_config_secure();
    bool full_pathnames = false;
    if (Slic3r::store_amf(path_u8.c_str(), &p->model, export_config ? &cfg : nullptr, full_pathnames)) {
        ; //store success
    } else {
        ; // store failed
    }
}*/

namespace {
std::string get_file_name(const std::string &file_path)
{
    size_t pos_last_delimiter = file_path.find_last_of("/\\");
    size_t pos_point          = file_path.find_last_of('.');
    size_t offset             = pos_last_delimiter + 1;
    size_t count              = pos_point - pos_last_delimiter - 1;
    return file_path.substr(offset, count);
}
using SvgFile = EmbossShape::SvgFile;
using SvgFiles = std::vector<SvgFile*>;
std::string create_unique_3mf_filepath(const std::string &file, const SvgFiles svgs)
{
    // const std::string MODEL_FOLDER = "3D/"; // copy from file 3mf.cpp
    std::string path_in_3mf = "3D/" + file + ".svg";
    size_t suffix_number = 0;
    bool is_unique = false;
    do{
        is_unique = true;
        path_in_3mf = "3D/" + file + ((suffix_number++)? ("_" + std::to_string(suffix_number)) : "") + ".svg";
        for (SvgFile *svgfile : svgs) {
            if (svgfile->path_in_3mf.empty())
                continue;
            if (svgfile->path_in_3mf.compare(path_in_3mf) == 0) {
                is_unique = false;
                break;
            }
        } 
    } while (!is_unique);
    return path_in_3mf;
}

bool set_by_local_path(SvgFile &svg, const SvgFiles& svgs)
{
    // Try to find already used svg file
    for (SvgFile *svg_ : svgs) {
        if (svg_->path_in_3mf.empty())
            continue;
        if (svg.path.compare(svg_->path) == 0) {
            svg.path_in_3mf = svg_->path_in_3mf;
            return true;
        }
    }
    return false;
}

/// <summary>
/// Function to secure private data before store to 3mf
/// </summary>
/// <param name="model">Data(also private) to clean before publishing</param>
void publish(Model &model, SaveStrategy strategy) {

    // SVG file publishing
    bool exist_new = false;
    SvgFiles svgfiles;
    for (ModelObject *object: model.objects){
        for (ModelVolume *volume : object->volumes) {
            if (!volume->emboss_shape.has_value())
                continue;
            if (volume->text_configuration.has_value())
                continue; // text dosen't have svg path

            assert(volume->emboss_shape->svg_file.has_value());
            if (!volume->emboss_shape->svg_file.has_value())
                continue;

            SvgFile* svg = &(*volume->emboss_shape->svg_file);
            if (svg->path_in_3mf.empty())
                exist_new = true;
            svgfiles.push_back(svg);
        }
    }

    for (SvgFile *svgfile : svgfiles) {
        if (!svgfile->path_in_3mf.empty())
            continue; // already suggested path (previous save)

        // create unique name for svgs, when local path differ
        std::string filename = "unknown";
        if (!svgfile->path.empty()) {
            if (set_by_local_path(*svgfile, svgfiles))
                continue;
            // check whether original filename is already in:
            filename = get_file_name(svgfile->path);
        }
        svgfile->path_in_3mf = create_unique_3mf_filepath(filename, svgfiles);        
    }
}
}

// BBS: backup
int Plater::export_3mf(const boost::filesystem::path& output_path, SaveStrategy strategy, int export_plate_idx, Export3mfProgressFn proFn)
{
    int ret = 0;
    //if (p->model.objects.empty()) {
    //    MessageDialog dialog(nullptr, _L("No objects to export."), _L("Save project"), wxYES);
    //    if (dialog.ShowModal() == wxYES)
    //        return -1;
    //}

    if (output_path.empty())
        return -1;

    bool export_config = true;
    wxString path = from_path(output_path);

    if (!path.Lower().EndsWith(".3mf"))
        return -1;

    // take care about private data stored into .3mf
    // modify model
    publish(p->model, strategy);

    DynamicPrintConfig cfg = wxGetApp().preset_bundle->full_config_secure();
    const std::string path_u8 = into_u8(path);
    wxBusyCursor wait;

    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << boost::format(": path=%1%, backup=%2%, export_plate_idx=%3%, SaveStrategy=%4%")
        %output_path.string()%(strategy & SaveStrategy::Backup)%export_plate_idx %(unsigned int)strategy;

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": path=%1%, backup=%2%, export_plate_idx=%3%, SaveStrategy=%4%")
        % std::string("") % (strategy & SaveStrategy::Backup) % export_plate_idx % (unsigned int)strategy;

    //BBS: add plate logic for thumbnail generate
    std::vector<ThumbnailData*> thumbnails;
    std::vector<ThumbnailData*> no_light_thumbnails;
    std::vector<ThumbnailData*> calibration_thumbnails;
    std::vector<ThumbnailData*> top_thumbnails;
    std::vector<ThumbnailData*> picking_thumbnails;
    std::vector<PlateBBoxData*> plate_bboxes;
    // BBS: backup
    if (!(strategy & SaveStrategy::Backup)) {
        for (int i = 0; i < p->partplate_list.get_plate_count(); i++) {
            ThumbnailData* thumbnail_data = &p->partplate_list.get_plate(i)->thumbnail_data;
            if (p->partplate_list.get_plate(i)->thumbnail_data.is_valid() &&  using_exported_file()) {
                //no need to generate thumbnail
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": non need to re-generate thumbnail for gcode/exported mode of plate %1%")%i;
            }
            else {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": re-generate thumbnail for plate %1%") % i;
                const ThumbnailsParams thumbnail_params = { {}, false, true, true, true, i };
                p->generate_thumbnail(p->partplate_list.get_plate(i)->thumbnail_data, THUMBNAIL_SIZE_3MF.first, THUMBNAIL_SIZE_3MF.second,
                                    thumbnail_params, Camera::EType::Ortho);
            }
            thumbnails.push_back(thumbnail_data);

            ThumbnailData *no_light_thumbnail_data = &p->partplate_list.get_plate(i)->no_light_thumbnail_data;
            if (p->partplate_list.get_plate(i)->no_light_thumbnail_data.is_valid() && using_exported_file()) {
                // no need to generate thumbnail
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": non need to re-generate thumbnail for gcode/exported mode of plate %1%") % i;
            } else {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": re-generate thumbnail for plate %1%") % i;
                const ThumbnailsParams thumbnail_params = {{}, false, true, true, true, i};
                p->generate_thumbnail(p->partplate_list.get_plate(i)->no_light_thumbnail_data, THUMBNAIL_SIZE_3MF.first, THUMBNAIL_SIZE_3MF.second, thumbnail_params,
                                      Camera::EType::Ortho,  Camera::ViewAngleType::Iso, false, true);
            }
            no_light_thumbnails.push_back(no_light_thumbnail_data);
            //ThumbnailData* calibration_data = &p->partplate_list.get_plate(i)->cali_thumbnail_data;
            //calibration_thumbnails.push_back(calibration_data);
            PlateBBoxData* plate_bbox_data = &p->partplate_list.get_plate(i)->cali_bboxes_data;
            plate_bboxes.push_back(plate_bbox_data);

            //generate top and picking thumbnails
            ThumbnailData* top_thumbnail = &p->partplate_list.get_plate(i)->top_thumbnail_data;
            if (top_thumbnail->is_valid() &&  using_exported_file()) {
                //no need to generate thumbnail
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": non need to re-generate top_thumbnail for gcode/exported mode of plate %1%")%i;
            }
            else {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": re-generate top_thumbnail for plate %1%") % i;
                const ThumbnailsParams thumbnail_params = { {}, false, true, false, true, i };
                p->generate_thumbnail(p->partplate_list.get_plate(i)->top_thumbnail_data, THUMBNAIL_SIZE_3MF.first, THUMBNAIL_SIZE_3MF.second, thumbnail_params,
                                      Camera::EType::Ortho, Camera::ViewAngleType::Top_Plate, false);
            }
            top_thumbnails.push_back(top_thumbnail);

            ThumbnailData* picking_thumbnail = &p->partplate_list.get_plate(i)->pick_thumbnail_data;
            if (picking_thumbnail->is_valid() &&  using_exported_file()) {
                //no need to generate thumbnail
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": non need to re-generate pick_thumbnail for gcode/exported mode of plate %1%")%i;
            }
            else {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": re-generate pick_thumbnail for plate %1%") % i;
                const ThumbnailsParams thumbnail_params = { {}, false, true, false, true, i };
                p->generate_thumbnail(p->partplate_list.get_plate(i)->pick_thumbnail_data, THUMBNAIL_SIZE_3MF.first, THUMBNAIL_SIZE_3MF.second, thumbnail_params,
                                      Camera::EType::Ortho, Camera::ViewAngleType::Top_Plate, true,true);
            }
            picking_thumbnails.push_back(picking_thumbnail);
        }

        if (p->partplate_list.get_curr_plate()->is_slice_result_valid()) {
            //BBS generate BBS calibration thumbnails
            int index = p->partplate_list.get_curr_plate_index();
            //ThumbnailData* calibration_data = calibration_thumbnails[index];
            //const ThumbnailsParams calibration_params = { {}, false, true, true, true, p->partplate_list.get_curr_plate_index() };
            //p->generate_calibration_thumbnail(*calibration_data, PartPlate::cali_thumbnail_width, PartPlate::cali_thumbnail_height, calibration_params);
            if (using_exported_file()) {
                //do nothing
            }
            else
                *plate_bboxes[index] = p->generate_first_layer_bbox();
        }
    }

    //BBS: add bbs 3mf logic
    PlateDataPtrs plate_data_list;
    p->partplate_list.store_to_3mf_structure(plate_data_list, (strategy & SaveStrategy::WithGcode || strategy & SaveStrategy::WithSliceInfo), export_plate_idx);

    // BBS: backup
    PresetBundle& preset_bundle = *wxGetApp().preset_bundle;
    std::vector<Preset*> project_presets = preset_bundle.get_current_project_embedded_presets();

    StoreParams store_params;
    store_params.path  = path_u8.c_str();
    store_params.model = &p->model;
    store_params.plate_data_list = plate_data_list;
    store_params.export_plate_idx = export_plate_idx;
    store_params.project_presets = project_presets;
    store_params.config = export_config ? &cfg : nullptr;
    store_params.thumbnail_data = thumbnails;
    store_params.no_light_thumbnail_data  = no_light_thumbnails;
    store_params.top_thumbnail_data = top_thumbnails;
    store_params.pick_thumbnail_data = picking_thumbnails;
    store_params.calibration_thumbnail_data = calibration_thumbnails;
    store_params.proFn = proFn;
    store_params.id_bboxes = plate_bboxes;//BBS
    store_params.project = &p->project;
    store_params.strategy = strategy | SaveStrategy::Zip64;


    // get type and color for platedata
    auto* filament_color = dynamic_cast<const ConfigOptionStrings*>(cfg.option("filament_colour"));
    auto* nozzle_diameter_option = dynamic_cast<const ConfigOptionFloats*>(cfg.option("nozzle_diameter"));
    auto* filament_id_opt = dynamic_cast<const ConfigOptionStrings*>(cfg.option("filament_ids"));
    std::string nozzle_diameter_str;
    if (nozzle_diameter_option)
        nozzle_diameter_str = nozzle_diameter_option->serialize();

    std::string printer_model_id = preset_bundle.printers.get_edited_preset().get_printer_type(&preset_bundle);

    for (int i = 0; i < plate_data_list.size(); i++) {
        PlateData *plate_data = plate_data_list[i];
        plate_data->printer_model_id = printer_model_id;
        plate_data->nozzle_diameters = nozzle_diameter_str;
        for (auto it = plate_data->slice_filaments_info.begin(); it != plate_data->slice_filaments_info.end(); it++) {
            std::string display_filament_type;
            it->type  = cfg.get_filament_type(display_filament_type, it->id);
            it->filament_id = filament_id_opt ? filament_id_opt->get_at(it->id) : "";
            it->color = filament_color ? filament_color->get_at(it->id) : "#FFFFFF";
            // save filament info used in curr plate
            int index = p->partplate_list.get_curr_plate_index();
            if (store_params.id_bboxes.size() > index) {
                store_params.id_bboxes[index]->filament_ids.push_back(it->id);
                store_params.id_bboxes[index]->filament_colors.push_back(it->color);
            }
        }
    }

    // handle Design Info
    bool has_design_info = false;
    ModelDesignInfo designInfo;
    if (p->model.design_info != nullptr) {
        if (!p->model.design_info->Designer.empty()) {
            BOOST_LOG_TRIVIAL(trace) << "design_info, found designer = " << p->model.design_info->Designer;
            has_design_info = true;
        }
    }
    if (!has_design_info) {
        // add Designed Info
        if (p->model.design_info == nullptr) {
            // set designInfo before export and reset after export
            if (wxGetApp().is_user_login()) {
                p->model.design_info                 = std::make_shared<ModelDesignInfo>();
                //p->model.design_info->Designer       = wxGetApp().getAgent()->get_user_nickanme();
                p->model.design_info->Designer       = "";
                p->model.design_info->DesignerUserId = wxGetApp().getAgent()->get_user_id();
                BOOST_LOG_TRIVIAL(trace) << "design_info prepare, designer = "<< "";
                BOOST_LOG_TRIVIAL(trace) << "design_info prepare, designer_user_id = " << p->model.design_info->DesignerUserId;
            }
        }
    }

    bool store_result = Slic3r::store_bbs_3mf(store_params);
    // reset designed info
    if (!has_design_info)
        p->model.design_info = nullptr;

    if (store_result) {
        if (!(store_params.strategy & SaveStrategy::Silence)) {
            // Success
            p->set_project_filename(path);
            BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << __LINE__ << " call set_project_filename: " << path;
        }
    }
    else {
        ret = -1;
    }

    if (project_presets.size() > 0)
    {
        for (unsigned int i = 0; i < project_presets.size(); i++)
        {
            delete project_presets[i];
        }
        project_presets.clear();
    }

    release_PlateData_list(plate_data_list);

    for (unsigned int i = 0; i < calibration_thumbnails.size(); i++)
    {
        //release the data here, as it will always be generated when export
        calibration_thumbnails[i]->reset();
    }
    for (unsigned int i = 0; i < no_light_thumbnails.size(); i++) {
        // release the data here, as it will always be generated when export
        no_light_thumbnails[i]->reset();
    }
    for (unsigned int i = 0; i < top_thumbnails.size(); i++)
    {
        //release the data here, as it will always be generated when export
        top_thumbnails[i]->reset();
    }
    top_thumbnails.clear();
    for (unsigned int i = 0; i < picking_thumbnails.size(); i++)
    {
        //release the data here, as it will always be generated when export
        picking_thumbnails[i]->reset();;
    }
    picking_thumbnails.clear();

    return ret;
}

void Plater::publish_project()
{
    return;
}


void Plater::reload_from_disk()
{
    p->reload_from_disk();
}

void Plater::replace_with_stl()
{
    p->replace_with_stl();
}

void Plater::replace_all_with_stl()
{
    p->replace_all_with_stl();
}

void Plater::reload_all_from_disk()
{
    p->reload_all_from_disk();
}

bool Plater::has_toolpaths_to_export() const
{
    return  p->preview->get_canvas3d()->has_toolpaths_to_export();
}

void Plater::export_toolpaths_to_obj() const
{
    if ((printer_technology() != ptFFF) || !is_preview_loaded())
        return;

    wxString path = p->get_export_file(FT_OBJ);
    if (path.empty())
        return;

    wxBusyCursor wait;
    p->preview->get_canvas3d()->export_toolpaths_to_obj(into_u8(path).c_str());
}

bool Plater::is_empty_project() {
    return model().objects.empty();
}
bool Plater::is_multi_extruder_ams_empty()
{
    std::vector<std::string>        extruder_ams_count_str = p->config->option<ConfigOptionStrings>("extruder_ams_count", true)->values;
    std::vector<std::map<int, int>> extruder_ams_counts    = get_extruder_ams_count(extruder_ams_count_str);
    for (auto extruder_ams_count : extruder_ams_counts) {
        for (auto iter = extruder_ams_count.begin(); iter != extruder_ams_count.end(); ++iter) {
            if (iter->second != 0)
                return false;
        }
    }
    return true;
}

//BBS: add multiple plate reslice logic
void Plater::reslice()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: enter, process_completed_with_error=%2%")%__LINE__ %p->process_completed_with_error;
    // There is "invalid data" button instead "slice now"
    if (p->process_completed_with_error == p->partplate_list.get_curr_plate_index())
    {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": process_completed_with_error, return directly");
        reset_gcode_toolpaths();
        return;
    }

    // In case SLA gizmo is in editing mode, refuse to continue
    // and notify user that he should leave it first.
    if (get_view3D_canvas3D()->get_gizmos_manager().is_in_editing_mode(true))
        return;
    
    // Stop the running (and queued) UI jobs and only proceed if they actually
    // get stopped.
    unsigned timeout_ms = 10000;
    if (!stop_queue(this->get_ui_job_worker(), timeout_ms)) {
        BOOST_LOG_TRIVIAL(error) << "Could not stop UI job within "
                                 << timeout_ms << " milliseconds timeout!";
        return;
    }

    // Orca: regenerate CalibPressureAdvancePattern custom G-code to apply changes
    if (model().calib_pa_pattern) {
        _calib_pa_pattern_gen_gcode();
    }

    if (printer_technology() == ptSLA) {
        for (auto& object : model().objects)
            if (object->sla_points_status == sla::PointsStatus::NoPoints)
                object->sla_points_status = sla::PointsStatus::Generating;
    }

    //FIXME Don't reslice if export of G-code or sending to OctoPrint is running.
    // bitmask of UpdateBackgroundProcessReturnState
    unsigned int state = this->p->update_background_process(true);
    if (state & priv::UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE)
        this->p->view3D->reload_scene(false);
    // If the SLA processing of just a single object's supports is running, restart slicing for the whole object.
    this->p->background_process.set_task(PrintBase::TaskParams());
    // Only restarts if the state is valid.
    //BBS: jusdge the result
    bool result = this->p->restart_background_process(state | priv::UPDATE_BACKGROUND_PROCESS_FORCE_RESTART);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: restart background,state=%2%, result=%3%")%__LINE__%state %result;
    if ((state & priv::UPDATE_BACKGROUND_PROCESS_INVALID) != 0)
    {
        //BBS: add logs
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": state %1% is UPDATE_BACKGROUND_PROCESS_INVALID, can not slice") % state;
        p->update_fff_scene_only_shells();
        return;
    }

    if ((!result) && p->m_slice_all && (p->m_cur_slice_plate < (p->partplate_list.get_plate_count() - 1)))
    {
        //slice next
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": in slicing all, current plate %1% already sliced, skip to next") % p->m_cur_slice_plate ;
        SlicingProcessCompletedEvent evt(EVT_PROCESS_COMPLETED, 0,
            SlicingProcessCompletedEvent::Finished, nullptr);
        // Post the "complete" callback message, so that it will slice the next plate soon
        wxQueueEvent(this, evt.Clone());
        p->m_is_slicing = true;
        if (p->m_cur_slice_plate == 0)
            reset_gcode_toolpaths();
        return;
    }

    if (result) {
        p->m_is_slicing = true;
    }

    bool clean_gcode_toolpaths = true;
    // BBS
    if (p->background_process.running())
    {
        //p->ready_to_slice = false;
        p->main_frame->update_slice_print_status(MainFrame::eEventSliceUpdate, false);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": background process is running, m_is_slicing is true");
    }
    else if (!p->background_process.empty() && !p->background_process.idle()) {
        //p->show_action_buttons(true);
        //p->ready_to_slice = true;
        p->main_frame->update_slice_print_status(MainFrame::eEventSliceUpdate, true);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": background process changes to not_idle, set ready_to_slice back to true");
    }
    else {
        //BBS: add reset logic for empty plate
        PartPlate * current_plate = p->background_process.get_current_plate();

        if (!current_plate->has_printable_instances()) {
            clean_gcode_toolpaths = true;
            current_plate->update_slice_result_valid_state(false);
        }
        else {
            clean_gcode_toolpaths = false;
            current_plate->update_slice_result_valid_state(true);
        }
        p->main_frame->update_slice_print_status(MainFrame::eEventSliceUpdate, false);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": background process in idle state, use previous result, clean_gcode_toolpaths=%1%")%clean_gcode_toolpaths;
    }

    if (clean_gcode_toolpaths)
        reset_gcode_toolpaths();

    p->preview->reload_print(!clean_gcode_toolpaths);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": finished, started slicing for plate %1%") % p->partplate_list.get_curr_plate_index();

    record_slice_preset("slicing");
}

void Plater::record_slice_preset(std::string action)
{
    // record slice preset
    try
    {
        json j;
        auto printer_preset = wxGetApp().preset_bundle->printers.get_edited_preset_with_vendor_profile().preset;
        if (printer_preset.is_system) {
            j["printer_preset_name"] = printer_preset.name;
        }
        else {
            j["printer_preset_name"] = printer_preset.config.opt_string("inherits");
        }
        const t_config_enum_values* keys_map = print_config_def.get("curr_bed_type")->enum_keys_map;
        if (keys_map) {
            for (auto item : *keys_map) {
                if (item.second == wxGetApp().preset_bundle->project_config.opt_enum<BedType>("curr_bed_type")) {
                    j["curr_bed_type"] = item.first;
                    break;
                }
            }
        }
        auto filament_presets = wxGetApp().preset_bundle->filament_presets;
        for (int i = 0; i < filament_presets.size(); ++i) {
            auto filament_preset = wxGetApp().preset_bundle->filaments.find_preset(filament_presets[i]);
            if (filament_preset->is_system) {
                j["filament_preset_" + std::to_string(i)] = filament_preset->name;
            }
            else {
                j["filament_preset_" + std::to_string(i)] = filament_preset->config.opt_string("inherits");
            }
        }

        Preset& print_preset = wxGetApp().preset_bundle->prints.get_edited_preset();
        if (print_preset.is_system) {
            j["process_preset"] = print_preset.name;
        }
        else {
            j["process_preset"] = print_preset.config.opt_string("inherits");
        }
        j["support_type"] = ConfigOptionEnum<SupportType>::get_enum_names().at(print_preset.config.opt_enum<SupportType>("support_type"));
        j["sparse_infill_pattern"] = ConfigOptionEnum<InfillPattern>::get_enum_names().at(print_preset.config.opt_enum<InfillPattern>("sparse_infill_pattern"));
        j["sparse_infill_density"] = print_preset.config.opt<ConfigOptionPercent>("sparse_infill_density")->value;

        j["brim_type"] = ConfigOptionEnum<BrimType>::get_enum_names().at(print_preset.config.opt_enum<BrimType>("brim_type"));
        j["user_mode"] = wxGetApp().get_mode_str();

        if (p->background_process.fff_print()) {
            const DynamicPrintConfig& full_config = p->background_process.fff_print()->full_print_config();
            json values = json::array();
            if (full_config.has("different_settings_to_system")) {
                std::vector<std::string> different_values = full_config.option<ConfigOptionStrings>("different_settings_to_system")->values;
                for (auto& item : different_values) {
                    values.push_back(item);
                }
            }
            j["different_settings_to_system"] = values;
        }

        j["record_event"] = action;
        NetworkAgent* agent = wxGetApp().getAgent();
    }
    catch (...)
    {
        return;
    }
}

//BBS: add project slicing related logic
int Plater::start_next_slice()
{
    // Stop arrange and (or) optimize rotation tasks.
    //this->stop_jobs();

    //FIXME Don't reslice if export of G-code or sending to OctoPrint is running.
    // bitmask of UpdateBackgroundProcessReturnState
    unsigned int state = this->p->update_background_process(true, false, false);
    if (state & priv::UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE)
        this->p->view3D->reload_scene(false);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": update_background_process returns %1%")%state;
    if (!p->partplate_list.get_curr_plate()->can_slice()) {
        p->process_completed_with_error = p->partplate_list.get_curr_plate_index();
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(": found invalidated apply in update_background_process.");
        return -1;
    }

    // Only restarts if the state is valid.
    bool result = this->p->restart_background_process(state | priv::UPDATE_BACKGROUND_PROCESS_FORCE_RESTART);
    if (!result)
    {
        //slice next
        SlicingProcessCompletedEvent evt(EVT_PROCESS_COMPLETED, 0,
                SlicingProcessCompletedEvent::Finished, nullptr);
        // Post the "complete" callback message, so that it will slice the next plate soon
        wxQueueEvent(this, evt.Clone());
    }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": restart_background_process returns %1%")%result;

    return 0;
}


void Plater::reslice_SLA_supports(const ModelObject &object, bool postpone_error_messages)
{
    reslice_SLA_until_step(slaposPad, object, postpone_error_messages);
}

void Plater::reslice_SLA_hollowing(const ModelObject &object, bool postpone_error_messages)
{
    reslice_SLA_until_step(slaposDrillHoles, object, postpone_error_messages);
}

void Plater::reslice_SLA_until_step(SLAPrintObjectStep step, const ModelObject &object, bool postpone_error_messages)
{
    //FIXME Don't reslice if export of G-code or sending to OctoPrint is running.
    // bitmask of UpdateBackgroundProcessReturnState
    unsigned int state = this->p->update_background_process(true, postpone_error_messages);
    if (state & priv::UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE)
        this->p->view3D->reload_scene(false);

    if (this->p->background_process.empty() || (state & priv::UPDATE_BACKGROUND_PROCESS_INVALID))
        // Nothing to do on empty input or invalid configuration.
        return;

    // Limit calculation to the single object only.
    PrintBase::TaskParams task;
    task.single_model_object = object.id();
    // If the background processing is not enabled, calculate supports just for the single instance.
    // Otherwise calculate everything, but start with the provided object.
    if (!this->p->background_processing_enabled()) {
        task.single_model_instance_only = true;
        task.to_object_step = step;
    }
    this->p->background_process.set_task(task);
    // and let the background processing start.
    this->p->restart_background_process(state | priv::UPDATE_BACKGROUND_PROCESS_FORCE_RESTART);
}
void Plater::send_gcode_legacy(int plate_idx, Export3mfProgressFn proFn, bool use_3mf)
{
    // if physical_printer is selected, send gcode for this printer
    // DynamicPrintConfig* physical_printer_config = wxGetApp().preset_bundle->physical_printers.get_selected_printer_config();
    DynamicPrintConfig* physical_printer_config = &Slic3r::GUI::wxGetApp().preset_bundle->printers.get_edited_preset().config;
    if (! physical_printer_config || p->model.objects.empty())
        return;

    PrintHostJob upload_job(physical_printer_config);
    if (upload_job.empty())
        return;

    upload_job.upload_data.use_3mf = use_3mf;

    // Obtain default output path
    fs::path default_output_file;
    try {
        // Update the background processing, so that the placeholder parser will get the correct values for the ouput file template.
        // Also if there is something wrong with the current configuration, a pop-up dialog will be shown and the export will not be performed.
        unsigned int state = this->p->update_restart_background_process(false, false);
        if (state & priv::UPDATE_BACKGROUND_PROCESS_INVALID)
            return;
        default_output_file = this->p->background_process.output_filepath_for_project("");
    } catch (const Slic3r::PlaceholderParserError& ex) {
        // Show the error with monospaced font.
        show_error(this, ex.what(), true);
        return;
    } catch (const std::exception& ex) {
        show_error(this, ex.what(), false);
        return;
    }
    default_output_file = fs::path(Slic3r::fold_utf8_to_ascii(default_output_file.string()));
    if (use_3mf) {
        default_output_file.replace_extension("3mf");
    }

    // Repetier specific: Query the server for the list of file groups.
    wxArrayString groups;
    {
        wxBusyCursor wait;
        upload_job.printhost->get_groups(groups);
    }

    // PrusaLink specific: Query the server for the list of file groups.
    wxArrayString storage_paths;
    wxArrayString storage_names;
    {
        wxBusyCursor wait;
        try {
            upload_job.printhost->get_storage(storage_paths, storage_names);
        } catch (const Slic3r::IOError& ex) {
            show_error(this, ex.what(), false);
            return;
        }
    }

    {
        auto        preset_bundle = wxGetApp().preset_bundle;
        const auto  opt           = physical_printer_config->option<ConfigOptionEnum<PrintHostType>>("host_type");
        const auto  host_type     = opt != nullptr ? opt->value : htElegooLink;
        auto        config        = get_app_config();

        std::unique_ptr<PrintHostSendDialog> pDlg;
        if (host_type == htElegooLink) {
            pDlg = std::make_unique<ElegooPrintHostSendDialog>(default_output_file, upload_job.printhost->get_post_upload_actions(), groups,
                                                               storage_paths, storage_names,
                                                               config->get_bool("open_device_tab_post_upload"));
        } else {
            pDlg = std::make_unique<PrintHostSendDialog>(default_output_file, upload_job.printhost->get_post_upload_actions(), groups,
                                                         storage_paths, storage_names, config->get_bool("open_device_tab_post_upload"));
        }

        pDlg->init();
        if (pDlg->ShowModal() != wxID_OK) {
            return;
        }

        config->set_bool("open_device_tab_post_upload", pDlg->switch_to_device_tab());
        // PrintHostUpload upload_data;
        upload_job.switch_to_device_tab    = pDlg->switch_to_device_tab();
        upload_job.upload_data.upload_path = pDlg->filename();
        upload_job.upload_data.post_action = pDlg->post_action();
        upload_job.upload_data.group       = pDlg->group();
        upload_job.upload_data.storage     = pDlg->storage();
        upload_job.upload_data.extended_info = pDlg->extendedInfo();
    }

    // Show "Is printer clean" dialog for PrusaConnect - Upload and print.
    if (std::string(upload_job.printhost->get_name()) == "PrusaConnect" && upload_job.upload_data.post_action == PrintHostPostUploadAction::StartPrint) {
        GUI::MessageDialog dlg(nullptr, _L("Is the printer ready? Is the print sheet in place, empty and clean?"), _L("Upload and Print"), wxOK | wxCANCEL);
        if (dlg.ShowModal() != wxID_OK)
            return;
    }

    if (use_3mf) {
        // Process gcode
        const int result = send_gcode(plate_idx, nullptr);

        if (result < 0) {
            wxString msg = _L("Abnormal print file data. Please slice again");
            show_error(this, msg, false);
            return;
        }

        upload_job.upload_data.source_path = p->m_print_job_data._3mf_path;
    }

    p->export_gcode(fs::path(), false, std::move(upload_job));
}
int Plater::send_gcode(int plate_idx, Export3mfProgressFn proFn)
{
    int result = 0;
    /* generate 3mf */
    set_print_job_plate_idx(plate_idx);

    PartPlate* plate = get_partplate_list().get_curr_plate();
    try {
        p->m_print_job_data._3mf_path = fs::path(plate->get_tmp_gcode_path());
        p->m_print_job_data._3mf_path.replace_extension("3mf");
    }
    catch (std::exception&) {
        BOOST_LOG_TRIVIAL(error) << "generate 3mf path failed";
        return -1;
    }

    SaveStrategy strategy = SaveStrategy::Silence | SaveStrategy::SkipModel | SaveStrategy::WithGcode | SaveStrategy::SkipAuxiliary;
#if !BBL_RELEASE_TO_PUBLIC
    //only save model in QA environment
    std::string sel = get_app_config()->get("iot_environment");
    if (sel == ENV_PRE_HOST)
        strategy = SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::WithGcode;
#endif

    result = export_3mf(p->m_print_job_data._3mf_path, strategy, plate_idx, proFn);

    return result;
}

int Plater::export_config_3mf(int plate_idx, Export3mfProgressFn proFn)
{
    int result = 0;
    /* generate 3mf */
    set_print_job_plate_idx(plate_idx);

    PartPlate* plate = get_partplate_list().get_curr_plate();
    try {
        p->m_print_job_data._3mf_config_path = fs::path(plate->get_temp_config_3mf_path());
    }
    catch (std::exception&) {
        BOOST_LOG_TRIVIAL(error) << "generate 3mf path failed";
        return -1;
    }

    SaveStrategy strategy = SaveStrategy::Silence | SaveStrategy::SkipModel | SaveStrategy::WithSliceInfo | SaveStrategy::SkipAuxiliary;
    result = export_3mf(p->m_print_job_data._3mf_config_path, strategy, plate_idx, proFn);

    return result;
}

//BBS
void Plater::send_calibration_job_finished(wxCommandEvent & evt)
{
    p->main_frame->request_select_tab(MainFrame::TabPosition::tpCalibration);
    auto calibration_panel = p->main_frame->m_calibration;
    if (calibration_panel) {
        auto curr_wizard = static_cast<CalibrationWizard*>(calibration_panel->get_tabpanel()->GetPage(evt.GetInt()));
        wxCommandEvent event(EVT_CALIBRATION_JOB_FINISHED);
        event.SetString(evt.GetString());
        event.SetEventObject(curr_wizard);
        wxPostEvent(curr_wizard, event);
    }
    evt.Skip();
}

void Plater::print_job_finished(wxCommandEvent &evt)
{
    //start print failed
    if (p) {
#ifdef __APPLE__
        p->hide_select_machine_dlg();
#else
        if (Slic3r::GUI::wxGetApp().get_inf_dialog_contect().empty()) {
            p->hide_select_machine_dlg();
        } else {
            p->enter_prepare_mode();
        }
#endif // __APPLE__
    }


    Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return;

    dev->set_selected_machine(evt.GetString().ToStdString());
    p->main_frame->request_select_tab(MainFrame::TabPosition::tpMonitor);
    //jump to monitor and select device status panel
    MonitorPanel* curr_monitor = p->main_frame->m_monitor;
    if(curr_monitor)
       curr_monitor->get_tabpanel()->ChangeSelection(MonitorPanel::PrinterTab::PT_STATUS);
}

void Plater::send_job_finished(wxCommandEvent& evt)
{
    Slic3r::DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return;
    //dev->set_selected_machine(evt.GetString().ToStdString());

    send_gcode_finish(evt.GetString());
    p->hide_send_to_printer_dlg();
    //p->main_frame->request_select_tab(MainFrame::TabPosition::tpMonitor);
    ////jump to monitor and select device status panel
    //MonitorPanel* curr_monitor = p->main_frame->m_monitor;
    //if (curr_monitor)
    //    curr_monitor->get_tabpanel()->ChangeSelection(MonitorPanel::PrinterTab::PT_STATUS);
}

void Plater::publish_job_finished(wxCommandEvent &evt)
{
    p->m_publish_dlg->EndModal(wxID_OK);
   // GUI::wxGetApp().load_url(evt.GetString());
   //GUI::wxGetApp().open_publish_page_dialog(evt.GetString());
}

// Called when the Eject button is pressed.
void Plater::eject_drive()
{
	wxBusyCursor wait;
    wxGetApp().removable_drive_manager()->set_and_verify_last_save_path(p->last_output_dir_path);
	wxGetApp().removable_drive_manager()->eject_drive();
}

void Plater::take_snapshot(const std::string &snapshot_name) { p->take_snapshot(snapshot_name); }
//void Plater::take_snapshot(const wxString &snapshot_name) { p->take_snapshot(snapshot_name); }
void Plater::take_snapshot(const std::string &snapshot_name, UndoRedo::SnapshotType snapshot_type) { p->take_snapshot(snapshot_name, snapshot_type); }
//void Plater::take_snapshot(const wxString &snapshot_name, UndoRedo::SnapshotType snapshot_type) { p->take_snapshot(snapshot_name, snapshot_type); }
void Plater::suppress_snapshots() { p->suppress_snapshots(); }
void Plater::allow_snapshots() { p->allow_snapshots(); }
// BBS: single snapshot
void Plater::single_snapshots_enter(SingleSnapshot *single)
{
    p->single_snapshots_enter(single);
}
void Plater::single_snapshots_leave(SingleSnapshot *single)
{
    p->single_snapshots_leave(single);
}
void Plater::undo() { p->undo(); }
void Plater::redo() { p->redo(); }
void Plater::undo_to(int selection)
{
    if (selection == 0) {
        p->undo();
        return;
    }

    const int idx = p->get_active_snapshot_index() - selection - 1;
    p->undo_redo_to(p->undo_redo_stack().snapshots()[idx].timestamp);
}
void Plater::redo_to(int selection)
{
    if (selection == 0) {
        p->redo();
        return;
    }

    const int idx = p->get_active_snapshot_index() + selection + 1;
    p->undo_redo_to(p->undo_redo_stack().snapshots()[idx].timestamp);
}
bool Plater::undo_redo_string_getter(const bool is_undo, int idx, const char** out_text)
{
    const std::vector<UndoRedo::Snapshot>& ss_stack = p->undo_redo_stack().snapshots();
    const int idx_in_ss_stack = p->get_active_snapshot_index() + (is_undo ? -(++idx) : idx);

    if (0 < idx_in_ss_stack && (size_t)idx_in_ss_stack < ss_stack.size() - 1) {
        *out_text = ss_stack[idx_in_ss_stack].name.c_str();
        return true;
    }

    return false;
}

int Plater::update_print_required_data(Slic3r::DynamicPrintConfig config, Slic3r::Model model, Slic3r::PlateDataPtrs plate_data_list, std::string file_name, std::string file_path)
{
    return p->update_print_required_data(config, model, plate_data_list, file_name, file_path);
}


void Plater::undo_redo_topmost_string_getter(const bool is_undo, std::string& out_text)
{
    const std::vector<UndoRedo::Snapshot>& ss_stack = p->undo_redo_stack().snapshots();
    const int idx_in_ss_stack = p->get_active_snapshot_index() + (is_undo ? -1 : 0);

    if (0 < idx_in_ss_stack && (size_t)idx_in_ss_stack < ss_stack.size() - 1) {
        out_text = ss_stack[idx_in_ss_stack].name;
        return;
    }

    out_text = "";
}

bool Plater::search_string_getter(int idx, const char** label, const char** tooltip)
{
    const Search::OptionsSearcher& search_list = p->sidebar->get_searcher();

    if (0 <= idx && (size_t)idx < search_list.size()) {
        search_list[idx].get_marked_label_and_tooltip(label, tooltip);
        return true;
    }

    return false;
}

void Plater::on_filament_change(size_t filament_idx)
{
    auto& filament_presets = wxGetApp().preset_bundle->filament_presets;
    if (filament_idx >= filament_presets.size())
        return;
    Slic3r::Preset* filament = wxGetApp().preset_bundle->filaments.find_preset(filament_presets[filament_idx]);
    if (filament == nullptr)
        return;
    std::string filament_type = filament->config.option<ConfigOptionStrings>("filament_type")->values[0];
}

// BBS.
void Plater::on_filament_count_change(size_t num_filaments)
{
    // only update elements in plater
    update_filament_colors_in_full_config();
    sidebar().on_filament_count_change(num_filaments);
    sidebar().obj_list()->update_objects_list_filament_column(num_filaments);

    Slic3r::GUI::PartPlateList &plate_list = get_partplate_list();
    plate_list.set_filament_count(num_filaments);
    for (int i = 0; i < plate_list.get_plate_count(); ++i) {
        PartPlate* part_plate = plate_list.get_plate(i);
        part_plate->update_first_layer_print_sequence(num_filaments);
    }

    for (ModelObject* mo : wxGetApp().model().objects) {
        for (ModelVolume* mv : mo->volumes) {
            mv->update_extruder_count(num_filaments);
        }
    }
}

void Plater::on_filaments_delete(size_t num_filaments, size_t filament_id, int replace_filament_id)
{
    // only update elements in plater
    update_filament_colors_in_full_config();

    // update fisrt print sequence and other layer sequence
    //move to partplate->on_filament_deleted
    /*Slic3r::GUI::PartPlateList &plate_list = get_partplate_list();
    for (int i = 0; i < plate_list.get_plate_count(); ++i) {
        PartPlate *part_plate = plate_list.get_plate(i);
        part_plate->update_first_layer_print_sequence_when_delete_filament(filament_id);
    }*/

    // update mmu info
    for (ModelObject *mo : wxGetApp().model().objects) {
        for (ModelVolume *mv : mo->volumes) {
            mv->update_extruder_count_when_delete_filament(num_filaments, filament_id + 1, replace_filament_id + 1);  // this function is 1 base
        }
    }

    // update UI
    sidebar().on_filaments_delete(filament_id);

    // update global support filament
    static const char *keys[] = {"support_filament", "support_interface_filament"};
    for (auto key : keys)
        if (p->config->has(key)) {
            if(p->config->opt_int(key) == filament_id + 1)
                (*(p->config)).erase(key);
            else {
                int new_value = p->config->opt_int(key) > filament_id ? p->config->opt_int(key) - 1 : p->config->opt_int(key);
                (*(p->config)).set_key_value(key, new ConfigOptionInt(new_value));
            }
        }

    // update object/volume/support(object and volume) filament id
    sidebar().obj_list()->update_objects_list_filament_column_when_delete_filament(filament_id, num_filaments, replace_filament_id);

    // update customize gcode
    for (auto item = p->model.plates_custom_gcodes.begin(); item != p->model.plates_custom_gcodes.end(); ++item) {
        auto iter = std::remove_if(item->second.gcodes.begin(), item->second.gcodes.end(), [filament_id](const Item& gcode_item) {
            return (gcode_item.type == CustomGCode::Type::ToolChange && gcode_item.extruder == filament_id + 1);
        });
        if (replace_filament_id == -1)
            item->second.gcodes.erase(iter, item->second.gcodes.end());
        else if(iter != item->second.gcodes.end()) {
            iter->extruder = replace_filament_id + 1;
        }

        for (auto& item : item->second.gcodes) {
            if (item.type == CustomGCode::Type::ToolChange && item.extruder > filament_id)
                item.extruder--;
        }
    }
}

std::vector<Slic3r::ColorRGBA> Plater::get_extruders_colors()
{
    unsigned char                     rgba_color[4] = {};
    std::vector<std::string>          colors        = get_extruder_colors_from_plater_config();
    std::vector<Slic3r::ColorRGBA> colors_out(colors.size());
    for (const std::string &color : colors) {
        Slic3r::GUI::BitmapCache::parse_color4(color, rgba_color);
        size_t color_idx      = &color - &colors.front();
        colors_out[color_idx] = {
            float(rgba_color[0]) / 255.f,
            float(rgba_color[1]) / 255.f,
            float(rgba_color[2]) / 255.f,
            float(rgba_color[3]) / 255.f,
        };
    }
    return colors_out;
}

void Plater::on_bed_type_change(BedType bed_type)
{
    sidebar().on_bed_type_change(bed_type);
}

bool Plater::update_filament_colors_in_full_config()
{
    DynamicPrintConfig& project_config = wxGetApp().preset_bundle->project_config;
    const auto& full_config = wxGetApp().preset_bundle->full_config();
    ConfigOptionStrings* color_opt = project_config.option<ConfigOptionStrings>("filament_colour");
    const ConfigOptionStrings* type_opt = full_config.option<ConfigOptionStrings>("filament_type");

    p->config->option<ConfigOptionStrings>("filament_colour")->values = color_opt->values;
    p->config->option<ConfigOptionStrings>("filament_type")->values = type_opt->values;
    return true;
}

void Plater::config_change_notification(const DynamicPrintConfig &config, const std::string& key)
{
    GLCanvas3D* view3d_canvas = get_view3D_canvas3D();
    if (key == std::string("print_sequence")) {
        auto seq_print = config.option<ConfigOptionEnum<PrintSequence>>("print_sequence");
        if (seq_print && view3d_canvas && view3d_canvas->is_initialized() && view3d_canvas->is_rendering_enabled()) {
            NotificationManager* notify_manager = get_notification_manager();
            if (seq_print->value == PrintSequence::ByObject) {
                std::string info_text = _u8L("Print By Object: \nSuggest to use auto-arrange to avoid collisions when printing.");
                notify_manager->bbl_show_seqprintinfo_notification(info_text);
            }
            else
                notify_manager->bbl_close_seqprintinfo_notification();
        }
    }
    // notification for more options
}

void Plater::on_config_change(const DynamicPrintConfig &config)
{
    bool update_scheduled = false;
    bool bed_shape_changed = false;
    //bool print_sequence_changed = false;
    t_config_option_keys diff_keys = p->config->diff(config);

    size_t old_nozzle_size = 1, new_nozzle_size = 1;
    auto * opt_old = p->config->option<ConfigOptionFloats>("nozzle_diameter");
    auto * opt_new = config.option<ConfigOptionFloats>("nozzle_diameter");
    if (opt_old && opt_new) {
        old_nozzle_size = opt_old->values.size();
        new_nozzle_size = opt_new->values.size();
    }

    for (auto opt_key : diff_keys) {
        if (opt_key == "filament_colour") {
            update_scheduled = true; // update should be scheduled (for update 3DScene) #2738

            if (update_filament_colors_in_full_config()) {
                p->sidebar->obj_list()->update_filament_colors();
                p->sidebar->update_dynamic_filament_list();
                continue;
            }
        }
        if (opt_key == "filament_type") {
            update_filament_colors_in_full_config();
            continue;
        }
        if (opt_key == "material_colour") {
            update_scheduled = true; // update should be scheduled (for update 3DScene)
        }

        p->config->set_key_value(opt_key, config.option(opt_key)->clone());
        if (opt_key == "printer_technology") {
            this->set_printer_technology(config.opt_enum<PrinterTechnology>(opt_key));
            // print technology is changed, so we should to update a search list
            p->sidebar->update_searcher();
            p->reset_gcode_toolpaths();
            p->view3D->get_canvas3d()->reset_sequential_print_clearance();
            //BBS: invalid all the slice results
            p->partplate_list.invalid_all_slice_result();
        }
        //BBS: add bed_exclude_area
        else if (opt_key == "printable_area" || opt_key == "bed_exclude_area"
            || opt_key == "bed_custom_texture" || opt_key == "bed_custom_model"
            || opt_key == "extruder_clearance_height_to_lid"
            || opt_key == "extruder_clearance_height_to_rod") {
            bed_shape_changed = true;
            update_scheduled = true;
        }
        else if (opt_key == "bed_shape" || opt_key == "bed_custom_texture" || opt_key == "bed_custom_model") {
            bed_shape_changed = true;
            update_scheduled = true;
        }
        else if (boost::starts_with(opt_key, "enable_prime_tower") ||
            boost::starts_with(opt_key, "prime_tower") ||
            boost::starts_with(opt_key, "wipe_tower") ||
            opt_key == "filament_minimal_purge_on_wipe_tower" ||
            opt_key == "single_extruder_multi_material" ||
            // BBS
            opt_key == "prime_volume") {
            update_scheduled = true;
        }
        else if(opt_key == "extruder_colour") {
            update_scheduled = true;
            //p->sidebar->obj_list()->update_extruder_colors();
        }
        else if (opt_key == "printable_height") {
            bed_shape_changed = true;
            update_scheduled = true;
        }
        else if (opt_key == "print_sequence") {
            update_scheduled = true;
            //print_sequence_changed = true;
        }
        else if (opt_key == "printer_model") {
            p->reset_gcode_toolpaths();
            if (old_nozzle_size != new_nozzle_size) {
                update_flush_volume_matrix(old_nozzle_size, new_nozzle_size);
            }

            // update to force bed selection(for texturing)
            bed_shape_changed = true;
            update_scheduled = true;
        }
        // Orca: update when *_filament changed
        else if (opt_key == "support_interface_filament" || opt_key == "support_filament" || opt_key == "wall_filament" ||
                 opt_key == "sparse_infill_filament" || opt_key == "solid_infill_filament") {
            update_scheduled = true;
        }
    }

    if (bed_shape_changed)
        set_bed_shape();

    config_change_notification(config, std::string("print_sequence"));

    if (update_scheduled)
        update();

    if (p->main_frame->is_loaded()) {
        this->p->schedule_background_process();
        update_title_dirty_status();
        p->schedule_auto_reslice_if_needed();
    }
}

void Plater::update_flush_volume_matrix(size_t old_nozzle_size, size_t new_nozzle_size)
{
    size_t nozzle_nums = wxGetApp().preset_bundle->get_printer_extruder_count();
    Slic3r::DynamicPrintConfig *project_config = &wxGetApp().preset_bundle->project_config;

    // Verify whether it is the first time start Studio
    size_t filament_nums = project_config->option<ConfigOptionStrings>("filament_colour")->values.size();
    size_t flush_volume_size = project_config->option<ConfigOptionFloats>("flush_volumes_matrix")->values.size();

    assert(nozzle_nums == new_nozzle_size);
    if (old_nozzle_size < new_nozzle_size) {

        std::vector<double>  first_flush_volume_mtx = get_flush_volumes_matrix(project_config->option<ConfigOptionFloats>("flush_volumes_matrix")->values, -1, old_nozzle_size);
        if (first_flush_volume_mtx.size() == filament_nums * filament_nums * new_nozzle_size) { // load file
            set_flush_volumes_matrix(project_config->option<ConfigOptionFloats>("flush_volumes_matrix")->values, first_flush_volume_mtx, -1, new_nozzle_size);
        } else {
            first_flush_volume_mtx.resize(filament_nums * filament_nums, 0);
            std::vector<double> flush_volume_mtx;
            for (size_t i = 0; i < new_nozzle_size; ++i) {
                flush_volume_mtx.insert(flush_volume_mtx.end(), first_flush_volume_mtx.begin(), first_flush_volume_mtx.end());
            }
            set_flush_volumes_matrix(project_config->option<ConfigOptionFloats>("flush_volumes_matrix")->values, flush_volume_mtx, -1, new_nozzle_size);
        }
        std::vector<double> flush_multipliers = project_config->option<ConfigOptionFloats>("flush_multiplier")->values;
        flush_multipliers.resize(nozzle_nums, 1.f);
        project_config->option<ConfigOptionFloats>("flush_multiplier")->values = flush_multipliers;
    }
    else if (old_nozzle_size > new_nozzle_size) {
        std::vector<double> new_flush_volume_mtx;
        for (size_t i = 0; i < new_nozzle_size; ++i) {
            std::vector<double> flush_volume_mtx = get_flush_volumes_matrix(project_config->option<ConfigOptionFloats>("flush_volumes_matrix")->values, -1, old_nozzle_size);
            flush_volume_mtx.resize(filament_nums * filament_nums, 0);
            new_flush_volume_mtx.insert(new_flush_volume_mtx.end(), flush_volume_mtx.begin(), flush_volume_mtx.end());
        }

        std::vector<double> flush_multipliers = project_config->option<ConfigOptionFloats>("flush_multiplier")->values;
        flush_multipliers.resize(nozzle_nums, 1.f);
        set_flush_volumes_matrix(project_config->option<ConfigOptionFloats>("flush_volumes_matrix")->values, new_flush_volume_mtx, -1, new_nozzle_size);
        project_config->option<ConfigOptionFloats>("flush_multiplier")->values = flush_multipliers;
    }
}

void Plater::set_bed_shape() const
{
    std::string texture_filename;
    auto bundle = wxGetApp().preset_bundle;
    if (bundle != nullptr) {
        const Preset* curr = &bundle->printers.get_selected_preset();
        if (curr->is_system)
            texture_filename = PresetUtils::system_printer_bed_texture(*curr);
        else {
            auto *printer_model = curr->config.opt<ConfigOptionString>("printer_model");
            if (printer_model != nullptr && ! printer_model->value.empty()) {
                texture_filename = bundle->get_texture_for_printer_model(printer_model->value);
            }
        }
    }
    set_bed_shape(p->config->option<ConfigOptionPoints>("printable_area")->values,
        //BBS: add bed exclude areas
        p->config->option<ConfigOptionPoints>("bed_exclude_area")->values,
        p->config->option<ConfigOptionPoints>("wrapping_exclude_area")->values,
        p->config->option<ConfigOptionFloat>("printable_height")->value,
        p->config->option<ConfigOptionPointsGroups>("extruder_printable_area")->values,
        p->config->option<ConfigOptionFloatsNullable>("extruder_printable_height")->values,
        p->config->option<ConfigOptionString>("bed_custom_texture")->value.empty() ? texture_filename : p->config->option<ConfigOptionString>("bed_custom_texture")->value,
        p->config->option<ConfigOptionString>("bed_custom_model")->value);
}

//BBS: add bed exclude area
void Plater::set_bed_shape(const Pointfs& shape, const Pointfs& exclude_area, const Pointfs& wrapping_exclude_area, const double printable_height, std::vector<Pointfs> extruder_areas, std::vector<double> extruder_heights, const std::string& custom_texture, const std::string& custom_model, bool force_as_custom) const
{
    p->set_bed_shape(make_counter_clockwise(shape), exclude_area, wrapping_exclude_area, printable_height, extruder_areas, extruder_heights, custom_texture, custom_model, force_as_custom);
}

void Plater::force_filament_colors_update()
{
//BBS: filament_color logic has been moved out of filament setting
#if 0
    bool update_scheduled = false;
    DynamicPrintConfig* config = p->config;
    const std::vector<std::string> filament_presets = wxGetApp().preset_bundle->filament_presets;
    if (filament_presets.size() > 1 &&
        p->config->option<ConfigOptionStrings>("filament_colour")->values.size() == filament_presets.size())
    {
        const PresetCollection& filaments = wxGetApp().preset_bundle->filaments;
        std::vector<std::string> filament_colors;
        filament_colors.reserve(filament_presets.size());

        for (const std::string& filament_preset : filament_presets)
            filament_colors.push_back(filaments.find_preset(filament_preset, true)->config.opt_string("filament_colour", (unsigned)0));

        if (config->option<ConfigOptionStrings>("filament_colour")->values != filament_colors) {
            config->option<ConfigOptionStrings>("filament_colour")->values = filament_colors;
            update_scheduled = true;
        }
    }

    if (update_scheduled) {
        update();
        p->sidebar->obj_list()->update_filament_colors();
    }

    if (p->main_frame->is_loaded())
        this->p->schedule_background_process();
#endif
}

void Plater::force_print_bed_update()
{
    // Fill in the printer model key with something which cannot possibly be valid, so that Plater::on_config_change() will update the print bed
    // once a new Printer profile config is loaded.
    p->config->opt_string("printer_model", true) = "bbl_empty";
}

void Plater::on_activate()
{
    this->p->show_delayed_error_message();
}

// Get vector of extruder colors considering filament color, if extruder color is undefined.
std::vector<std::string> Plater::get_extruder_colors_from_plater_config(const GCodeProcessorResult* const result) const
{
    if (wxGetApp().is_gcode_viewer() && result != nullptr)
        return result->extruder_colors;
    else {
        const Slic3r::DynamicPrintConfig* config = &wxGetApp().preset_bundle->project_config;
        std::vector<std::string> filament_colors;
        if (!config->has("filament_colour")) // in case of a SLA print
            return filament_colors;

        filament_colors = (config->option<ConfigOptionStrings>("filament_colour"))->values;
        return filament_colors;
    }
}

std::vector<std::string> Plater::get_filament_colors_render_info() const
{
    const Slic3r::DynamicPrintConfig* config = &wxGetApp().preset_bundle->project_config;
    std::vector<std::string> color_packs;
    if (!config->has("filament_multi_colour")) return color_packs;

    color_packs = (config->option<ConfigOptionStrings>("filament_multi_colour"))->values;
    return color_packs;
}

std::vector<std::string> Plater::get_filament_color_render_type() const
{
    const Slic3r::DynamicPrintConfig *config = &wxGetApp().preset_bundle->project_config;
    std::vector<std::string>          ctype;
    if (!config->has("filament_colour_type")) return ctype;

    ctype = (config->option<ConfigOptionStrings>("filament_colour_type"))->values;
    return ctype;
}

/* Get vector of colors used for rendering of a Preview scene in "Color print" mode
 * It consists of extruder colors and colors, saved in model.custom_gcode_per_print_z
 */
std::vector<std::string> Plater::get_colors_for_color_print(const GCodeProcessorResult* const result) const
{
    std::vector<std::string> colors = get_extruder_colors_from_plater_config(result);

    if (wxGetApp().is_gcode_viewer() && result != nullptr) {
        for (const CustomGCode::Item& code : result->custom_gcode_per_print_z) {
            if (code.type == CustomGCode::ColorChange)
                colors.emplace_back(code.color);
        }
    }
    else {
        //BBS
        colors.reserve(colors.size() + p->model.get_curr_plate_custom_gcodes().gcodes.size());
        for (const CustomGCode::Item& code : p->model.get_curr_plate_custom_gcodes().gcodes) {
            if (code.type == CustomGCode::ColorChange)
                colors.emplace_back(code.color);
        }
    }

    return colors;
}

void Plater::set_global_filament_map_mode(FilamentMapMode mode)
{
    auto& project_config = wxGetApp().preset_bundle->project_config;
    auto mode_ptr = project_config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode");
    FilamentMapMode old_mode = mode_ptr->value;
    if(mode != old_mode)
        on_filament_map_mode_change();
    mode_ptr->value = mode;
}

void Plater::set_global_filament_map(const std::vector<int>& filament_map)
{
    auto& project_config = wxGetApp().preset_bundle->project_config;
    project_config.option<ConfigOptionInts>("filament_map")->values = filament_map;
}

std::vector<int> Plater::get_global_filament_map() const
{
    auto& project_config = wxGetApp().preset_bundle->project_config;
    return project_config.option<ConfigOptionInts>("filament_map")->values;
}


FilamentMapMode Plater::get_global_filament_map_mode() const
{
    auto& project_config = wxGetApp().preset_bundle->project_config;
    return project_config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode")->value;
}

void Plater::on_filament_map_mode_change()
{
    auto& plate_list = this->get_partplate_list();
    int plate_count = plate_list.get_plate_count();
    for (int idx = 0; idx < plate_count; ++idx) {
        auto plate=plate_list.get_plate(idx);
        auto plate_map_mode = plate->get_filament_map_mode();
        if (plate_map_mode == fmmDefault)
            plate->clear_filament_map();
    }
}

wxWindow* Plater::get_select_machine_dialog()
{
    return p->m_select_machine_dlg;
}

void Plater::update_print_error_info(int code, std::string msg, std::string extra)
{
    if (p->m_select_machine_dlg) {
        p->m_select_machine_dlg->update_print_error_info(code, msg, extra);
    }

    if (p->m_send_to_sdcard_dlg) {
        p->m_send_to_sdcard_dlg->update_print_error_info(code, msg, extra);
    }
    if (p->main_frame->m_calibration)
        p->main_frame->m_calibration->update_print_error_info(code, msg, extra);
}

wxString Plater::get_project_filename(const wxString& extension) const
{
    return p->get_project_filename(extension);
}

wxString Plater::get_export_gcode_filename(const wxString & extension, bool only_filename, bool export_all) const
{
    return p->get_export_gcode_filename(extension, only_filename, export_all);
}

void Plater::set_project_filename(const wxString& filename)
{
    p->set_project_filename(filename);
}

bool Plater::is_export_gcode_scheduled() const
{
    return p->background_process.is_export_scheduled();
}

const Selection &Plater::get_selection() const
{
    return p->get_selection();
}

int Plater::get_selected_object_idx()
{
    return p->get_selected_object_idx();
}

bool Plater::is_single_full_object_selection() const
{
    return p->get_selection().is_single_full_object();
}

GLCanvas3D* Plater::canvas3D()
{
    // BBS modify view3D->get_canvas3d() to current canvas
    return p->get_current_canvas3D();
}

const GLCanvas3D* Plater::canvas3D() const
{
    // BBS modify view3D->get_canvas3d() to current canvas
    return p->get_current_canvas3D();
}

GLCanvas3D* Plater::get_view3D_canvas3D()
{
    return p->view3D->get_canvas3d();
}

GLCanvas3D* Plater::get_preview_canvas3D()
{
    return p->preview->get_canvas3d();
}

GLCanvas3D* Plater::get_assmeble_canvas3D()
{
    if (p->assemble_view)
        return p->assemble_view->get_canvas3d();
    return nullptr;
}

GLCanvas3D* Plater::get_current_canvas3D(bool exclude_preview)
{
    return p->get_current_canvas3D(exclude_preview);
}

void Plater::arrange()
{
    auto &w = get_ui_job_worker();
    if (w.is_idle()) {
        p->take_snapshot(_u8L("Arrange"));
        replace_job(w, std::make_unique<ArrangeJob>());
    }
}

void Plater::set_current_canvas_as_dirty()
{
    p->set_current_canvas_as_dirty();
}

void Plater::unbind_canvas_event_handlers()
{
    p->unbind_canvas_event_handlers();
}

void Plater::reset_canvas_volumes()
{
    p->reset_canvas_volumes();
}

PrinterTechnology Plater::printer_technology() const
{
    return p->printer_technology;
}

const DynamicPrintConfig * Plater::config() const { return p->config; }

bool Plater::set_printer_technology(PrinterTechnology printer_technology)
{
    p->printer_technology = printer_technology;
    bool ret = p->background_process.select_technology(printer_technology);
    if (ret) {
        // Update the active presets.
    }
    //FIXME for SLA synchronize
    //p->background_process.apply(Model)!

    if (printer_technology == ptSLA) {
        for (ModelObject* model_object : p->model.objects) {
            model_object->ensure_on_bed();
        }
    }

    p->label_btn_export = printer_technology == ptFFF ? L("Export G-code") : L("Export");
    p->label_btn_send   = printer_technology == ptFFF ? L("Send G-code")   : L("Send to printer");

    if (wxGetApp().mainframe != nullptr)
        wxGetApp().mainframe->update_menubar();

    p->sidebar->get_searcher().set_printer_technology(printer_technology);

    p->notification_manager->set_fff(printer_technology == ptFFF);
    p->notification_manager->set_slicing_progress_hidden();

    return ret;
}

void Plater::clear_before_change_mesh(int obj_idx)
{
    ModelObject* mo = model().objects[obj_idx];

    // If there are custom supports/seams/mmu/fuzzy skin segmentation, remove them. Fixed mesh
    // may be different and they would make no sense.
    bool paint_removed = false;
    for (ModelVolume* mv : mo->volumes) {
        paint_removed |= ! mv->supported_facets.empty() || ! mv->seam_facets.empty() || ! mv->mmu_segmentation_facets.empty() || !mv->fuzzy_skin_facets.empty();
        mv->supported_facets.reset();
        mv->seam_facets.reset();
        mv->mmu_segmentation_facets.reset();
        mv->fuzzy_skin_facets.reset();
    }
    if (paint_removed) {
        // snapshot_time is captured by copy so the lambda knows where to undo/redo to.
        get_notification_manager()->push_notification(
                    NotificationType::CustomSupportsAndSeamRemovedAfterRepair,
                    NotificationManager::NotificationLevel::PrintInfoNotificationLevel,
                    _u8L("Custom supports and color painting were removed before repairing."));
    }
}

void Plater::changed_mesh(int obj_idx)
{
    ModelObject* mo = model().objects[obj_idx];
    sla::reproject_points_and_holes(mo);
    update();
    p->object_list_changed();
    p->schedule_background_process();
}

void Plater::changed_object(ModelObject &object){
    assert(object.get_model() == &p->model); // is object from same model?
    object.invalidate_bounding_box();

    // recenter and re - align to Z = 0
    object.ensure_on_bed(p->printer_technology != ptSLA);

    if (p->printer_technology == ptSLA) {
        // Update the SLAPrint from the current Model, so that the reload_scene()
        // pulls the correct data, update the 3D scene.
        p->update_restart_background_process(true, false);
    } else
        p->view3D->reload_scene(false);

    // update print
    p->schedule_background_process();
        
    // Check outside bed
    get_current_canvas3D()->requires_check_outside_state();
}

void Plater::changed_object(int obj_idx)
{
    if (obj_idx < 0)
        return;
    ModelObject *object = p->model.objects[obj_idx];
    if (object == nullptr)
        return;
    changed_object(*object);
}

void Plater::changed_objects(const std::vector<size_t>& object_idxs)
{
    if (object_idxs.empty())
        return;

    for (size_t obj_idx : object_idxs) {
        if (obj_idx < p->model.objects.size()) {
            if (p->model.objects[obj_idx]->min_z() >= SINKING_Z_THRESHOLD)
                // re - align to Z = 0
                p->model.objects[obj_idx]->ensure_on_bed();
        }
    }
    if (this->p->printer_technology == ptSLA) {
        // Update the SLAPrint from the current Model, so that the reload_scene()
        // pulls the correct data, update the 3D scene.
        this->p->update_restart_background_process(true, false);
    }
    else {
        p->view3D->reload_scene(false);
        p->view3D->get_canvas3d()->update_instance_printable_state_for_objects(object_idxs);
    }

    // update print
    this->p->schedule_background_process();
}

void Plater::schedule_background_process(bool schedule/* = true*/)
{
    if (schedule)
        this->p->schedule_background_process();

    this->p->suppressed_backround_processing_update = false;
}

bool Plater::is_background_process_update_scheduled() const
{
    return this->p->background_process_timer.IsRunning();
}

void Plater::suppress_background_process(const bool stop_background_process)
{
    if (stop_background_process)
        this->p->background_process_timer.Stop();

    this->p->suppressed_backround_processing_update = true;
}

void Plater::center_selection()     { p->center_selection(); }
void Plater::drop_selection()       { p->drop_selection(); }
void Plater::mirror(Axis axis)      { p->mirror(axis); }
void Plater::split_object()         { p->split_object(); }
void Plater::split_volume()         { p->split_volume(); }
void Plater::optimize_rotation()
{
    auto &w = get_ui_job_worker();
    if (w.is_idle()) {
        p->take_snapshot(_u8L("Optimize Rotation"));
        replace_job(w, std::make_unique<OrientJob>());
    }
}
void Plater::update_menus()         { p->menus.update(); }

wxString Plater::get_selected_printer_name_in_combox() {
    PresetBundle *        preset_bundle    = wxGetApp().preset_bundle;
    std::string           printer_model    = preset_bundle->printers.get_selected_preset().config.option<ConfigOptionString>("printer_model")->value;
    return printer_model;
}

void Plater::pop_warning_and_go_to_device_page(wxString printer_name, PrinterWarningType type, const wxString &title)
{
    printer_name.Replace("Bambu Lab", "", false);
    wxString content;
    if (type == PrinterWarningType::NOT_CONNECTED) {
        content = wxString::Format(_L("Printer not connected. Please go to the device page to connect %s before syncing."), printer_name);
    } else if (type == PrinterWarningType::INCONSISTENT) {
        content = wxString::Format(_L("The currently connected printer on the device page is not %s. Please switch to %s before syncing."), printer_name, printer_name);
    } else if (type == PrinterWarningType::UNINSTALL_FILAMENT) {
        content = _L("There are no filaments on the printer. Please load the filaments on the printer first.");
    } else if (type == PrinterWarningType::EMPTY_FILAMENT) {
        content = _L("The filaments on the printer are all unknown types. Please go to the printer screen or software device page to set the filament type.");
    }
    MessageDialog dlg(this, content, title, wxOK | wxFORWARD | wxICON_WARNING, _L("Device Page"));
    auto          result = dlg.ShowModal();
    if (result == wxFORWARD) {
        wxGetApp().mainframe->select_tab(size_t(MainFrame::tpMonitor));
    }
}

bool Plater::is_same_printer_for_connected_and_selected(bool popup_warning)
{
    if (!wxGetApp().getDeviceManager()) {
        return false;
    }
    MachineObject *obj = wxGetApp().getDeviceManager()->get_selected_machine();
    if (obj == nullptr) {
        return false;
    }
    if (!check_printer_initialized(obj, true, popup_warning))
        return false;
    Preset *      machine_preset     = get_printer_preset(obj);
    if (!machine_preset)
        return false;

    if (wxGetApp().is_blocking_printing()) {
        if (popup_warning) {
            auto printer_name = get_selected_printer_name_in_combox(); // wxString(obj->get_preset_printer_model_name(machine_print_name))
            pop_warning_and_go_to_device_page(printer_name, PrinterWarningType::INCONSISTENT, _L("Synchronize AMS Filament Information"));
        }
        return false;
    }
    return true;
}
// BBS
//void Plater::show_action_buttons(const bool ready_to_slice) const   { p->show_action_buttons(ready_to_slice); }

void Plater::fill_color(int extruder_id)
{
    if (can_fillcolor()) {
        p->assemble_view->get_canvas3d()->get_selection().fill_color(extruder_id);
    }
}

//BBS
void Plater::cut_selection_to_clipboard()
{
    Plater::TakeSnapshot snapshot(this, "Cut Selected Objects");
    if (can_cut_to_clipboard() && !p->sidebar->obj_list()->cut_to_clipboard()) {
        p->view3D->get_canvas3d()->get_selection().cut_to_clipboard();
    }
}

void Plater::copy_selection_to_clipboard()
{
    // At first try to copy selected values to the ObjectList's clipboard
    // to check if Settings or Layers are selected in the list
    // and then copy to 3DCanvas's clipboard if not
    if (can_copy_to_clipboard() && !p->sidebar->obj_list()->copy_to_clipboard())
        p->view3D->get_canvas3d()->get_selection().copy_to_clipboard();
}

void Plater::paste_from_clipboard()
{
    if (!can_paste_from_clipboard())
        return;

    Plater::TakeSnapshot snapshot(this, "Paste From Clipboard");

    // At first try to paste values from the ObjectList's clipboard
    // to check if Settings or Layers were copied
    // and then paste from the 3DCanvas's clipboard if not
    if (!p->sidebar->obj_list()->paste_from_clipboard())
        p->view3D->get_canvas3d()->get_selection().paste_from_clipboard();
}

//BBS: add clone
void Plater::clone_selection()
{
    if (is_selection_empty())
        return;
    CloneDialog dlg(this);
    dlg.ShowModal();
}

std::vector<Vec2f> Plater::get_empty_cells(const Vec2f step)
{
    PartPlate* plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
    BoundingBoxf3 build_volume = plate->get_build_volume(true);
    Vec2d vmin(build_volume.min.x(), build_volume.min.y()), vmax(build_volume.max.x(), build_volume.max.y());
    BoundingBoxf bbox(vmin, vmax);
    std::vector<Vec2f> cells;
    auto min_x = step(0)/2;// start_point.x() - step(0) * int((start_point.x() - bbox.min.x()) / step(0));
    auto min_y = step(1)/2;// start_point.y() - step(1) * int((start_point.y() - bbox.min.y()) / step(1));
    auto& exclude_box3s = plate->get_exclude_areas();
    std::vector<BoundingBoxf> exclude_boxs;
    for (auto& box : exclude_box3s) {
        Vec2d vmin(box.min.x(), box.min.y()), vmax(box.max.x(), box.max.y());
        exclude_boxs.emplace_back(vmin, vmax);
    }
    for (float x = min_x + bbox.min.x(); x < bbox.max.x() - step(0) / 2; x += step(0))
        for (float y = min_y + bbox.min.y(); y < bbox.max.y() - step(1) / 2; y += step(1)) {
            bool in_exclude = false;
            BoundingBoxf cell(Vec2d(x - step(0) / 2, y - step(1) / 2), Vec2d(x + step(0) / 2, y + step(1) / 2));
            for (auto& box : exclude_boxs) {
                if (box.overlap(cell)) {
                    in_exclude = true;
                    break;
                }
            }
            if(in_exclude)
                continue;
            cells.emplace_back(x, y);
        }
    return cells;
}

void Plater::search(bool plater_is_active, Preset::Type type, wxWindow *tag, TextInput *etag, wxWindow *stag)
{
    if (plater_is_active) {
        if (is_preview_shown())
            return;
        // plater should be focused for correct navigation inside search window
        this->SetFocus();

        wxKeyEvent evt;
#ifdef __APPLE__
        evt.m_keyCode = 'f';
#else /* __APPLE__ */
        evt.m_keyCode = WXK_CONTROL_F;
#endif /* __APPLE__ */
        evt.SetControlDown(true);
        canvas3D()->on_char(evt);
    }
    else
        p->sidebar->get_searcher().show_dialog(type, tag, etag, stag);
}

void Plater::msw_rescale()
{
    p->preview->msw_rescale();

    p->view3D->get_canvas3d()->msw_rescale();

    p->sidebar->msw_rescale();

    p->menus.msw_rescale();

    Layout();
    GetParent()->Layout();
}

void Plater::sys_color_changed()
{
    p->preview->sys_color_changed();
    p->sidebar->sys_color_changed();
    p->menus.sys_color_changed();
    if (p->m_select_machine_dlg) p->m_select_machine_dlg->sys_color_changed();

    Layout();
    GetParent()->Layout();
}

// BBS
#if 0
bool Plater::init_view_toolbar()
{
    return p->init_view_toolbar();
}

void Plater::enable_view_toolbar(bool enable)
{
    p->view_toolbar.set_enabled(enable);
}
#endif

bool Plater::init_collapse_toolbar()
{
    return p->init_collapse_toolbar();
}

const Camera& Plater::get_camera() const
{
    return p->camera;
}

Camera& Plater::get_camera()
{
    return p->camera;
}

//BBS: partplate list related functions
PartPlateList& Plater::get_partplate_list()
{
    return p->partplate_list;
}

void Plater::apply_background_progress()
{
    PartPlate* part_plate = p->partplate_list.get_curr_plate();
    int plate_index = p->partplate_list.get_curr_plate_index();
    bool result_valid = part_plate->is_slice_result_valid();
    const auto& preset_bundle = wxGetApp().preset_bundle;
    //always apply the current plate's print
    Print::ApplyStatus invalidated;
    if (preset_bundle->get_printer_extruder_count() > 1) {
        std::vector<int> f_maps = part_plate->get_real_filament_maps(preset_bundle->project_config);
        invalidated = p->background_process.apply(this->model(), preset_bundle->full_config(false, f_maps));
    }
    else
        invalidated = p->background_process.apply(this->model(), preset_bundle->full_config(false));

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" %1%: plate %2%, after apply, invalidated= %3%, previous result_valid %4% ") % __LINE__ % plate_index % invalidated % result_valid;
    if (invalidated & PrintBase::APPLY_STATUS_INVALIDATED)
    {
        part_plate->update_slice_result_valid_state(false);
        //p->ready_to_slice = true;
        p->main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, true);
    }
}

//BBS: select Plate
int Plater::select_plate(int plate_index, bool need_slice)
{
    int ret;
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" %1%: plate %2%, need_slice %3% ")%__LINE__ %plate_index  %need_slice;
    take_snapshot("select partplate!");
    ret = p->partplate_list.select_plate(plate_index);
    if (!ret) {
        if (is_view3D_shown())
            wxGetApp().plater()->canvas3D()->render();
    }
    const auto& preset_bundle = wxGetApp().preset_bundle;

    if ((!ret) && (p->background_process.can_switch_print()))
    {
        //select successfully
        p->partplate_list.update_slice_context_to_current_plate(p->background_process);
        p->preview->update_gcode_result(p->partplate_list.get_current_slice_result());
        p->update_print_volume_state();

        PartPlate* part_plate = p->partplate_list.get_curr_plate();
        bool result_valid = part_plate->is_slice_result_valid();
        PrintBase* print = nullptr;
        GCodeResult* gcode_result = nullptr;
        Print::ApplyStatus invalidated;

        part_plate->get_print(&print, &gcode_result, NULL);

        //always apply the current plate's print
        if (preset_bundle->get_printer_extruder_count() > 1) {
            std::vector<int> f_maps = part_plate->get_real_filament_maps(preset_bundle->project_config);
            invalidated = p->background_process.apply(this->model(), preset_bundle->full_config(false, f_maps));
        }
        else
            invalidated = p->background_process.apply(this->model(), preset_bundle->full_config(false));
        bool model_fits, validate_err;

        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" %1%: plate %2%, after apply, invalidated= %3%, previous result_valid %4% ")%__LINE__ %plate_index  %invalidated %result_valid;
        if (result_valid)
        {
            if (is_preview_shown())
            {
                if (need_slice) { //from preview's thumbnail
                    if ((invalidated & PrintBase::APPLY_STATUS_INVALIDATED) || (gcode_result->moves.empty())){
                        if (invalidated & PrintBase::APPLY_STATUS_INVALIDATED)
                            part_plate->update_slice_result_valid_state(false);
                        p->process_completed_with_error = -1;
                        p->m_slice_all = false;
                        reset_gcode_toolpaths();
                        reslice();
                    }
                    else {
                        validate_current_plate(model_fits, validate_err);
                        //just refresh_print
                        refresh_print();
                        p->main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, false, true);
                    }
                }
                else {// from multiple slice's next
                    //do nothing
                }
            }
            else
            {
                validate_current_plate(model_fits, validate_err);
                if (invalidated & PrintBase::APPLY_STATUS_INVALIDATED)
                {
                    part_plate->update_slice_result_valid_state(false);
                    // BBS
                    //p->show_action_buttons(true);
                    //p->ready_to_slice = true;
                    p->main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, true);
                }
                else
                {
                    // BBS
                    //p->show_action_buttons(false);
                    //p->ready_to_slice = false;
                    p->main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, false);

                    refresh_print();
                }
            }
        }
        else
        {
            //check inside status
            //model_fits = p->view3D->get_canvas3d()->check_volumes_outside_state() != ModelInstancePVS_Partly_Outside;
            //bool validate_err = false;
            validate_current_plate(model_fits, validate_err);
            if (model_fits && !validate_err) {
                p->process_completed_with_error = -1;
            }
            else {
                p->process_completed_with_error = p->partplate_list.get_curr_plate_index();
            }
            if (is_preview_shown())
            {
                if (need_slice)
                {
                    //p->process_completed_with_error = -1;
                    p->m_slice_all = false;
                    reset_gcode_toolpaths();
                    if (model_fits && !validate_err) {
                        if (!check_ams_status(false)){
                            return ret;
                        }
                        reslice();
                    }
                    else {
                        p->main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, false);
                        //sometimes the previous print's sliced result is still valid, but the newly added object is laid over the boundary
                        //then the print toolpath will be shown, so we should not refresh print here, only onload shell
                        //refresh_print();
                        p->update_fff_scene_only_shells();
                    }
                }
                else {
                    //p->ready_to_slice = false;
                    p->main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, false);
                    refresh_print();
                }
            }
            else
            {
                //validate_current_plate(model_fits, validate_err);
                //check inside status
                /*if (model_fits && !validate_err){
                    p->process_completed_with_error = -1;
                }
                else {
                    p->process_completed_with_error = p->partplate_list.get_curr_plate_index();
                }*/

                // BBS: don't show action buttons
                //p->show_action_buttons(true);
                //p->ready_to_slice = true;
                if (model_fits && part_plate->has_printable_instances())
                {
                    //p->view3D->get_canvas3d()->post_event(Event<bool>(EVT_GLCANVAS_ENABLE_ACTION_BUTTONS, true));
                    p->main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, true);
                }
                else
                {
                    //p->view3D->get_canvas3d()->post_event(Event<bool>(EVT_GLCANVAS_ENABLE_ACTION_BUTTONS, false));
                    p->main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, false);
                }
            }
        }
    }

    SimpleEvent event(EVT_GLCANVAS_PLATE_SELECT);
    p->on_plate_selected(event);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" %1%: plate %2%, return %3%")%__LINE__ %plate_index %ret;
    return ret;
}

int Plater::select_sliced_plate(int plate_index)
{
    int ret = 0;
    BOOST_LOG_TRIVIAL(info) << "select_sliced_plate plate_idx=" << plate_index;

    Freeze();
    ret = select_plate(plate_index, true);
    if (ret)
    {
        BOOST_LOG_TRIVIAL(error) << "select_plate error for plate_idx=" << plate_index;
        Thaw();
        return -1;
    }
    p->partplate_list.select_plate_view();
    Thaw();

    return ret;
}

extern std::string& get_object_limited_text();
extern std::string& get_object_clashed_text();
extern std::string& get_left_extruder_unprintable_text();
extern std::string& get_right_extruder_unprintable_text();

void Plater::validate_current_plate(bool& model_fits, bool& validate_error)
{
    ObjectFilamentResults object_results;
    ModelInstanceEPrintVolumeState state = p->view3D->get_canvas3d()->check_volumes_outside_state(&object_results);
    model_fits = (state != ModelInstancePVS_Partly_Outside);

    PartPlate *cur_plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
    if (model_fits) {  // TPU check
        bool  tpu_valid = cur_plate->check_tpu_printable_status(wxGetApp().preset_bundle->full_config(), wxGetApp().preset_bundle->get_used_tpu_filaments(cur_plate->get_extruders(true)));
        model_fits &= tpu_valid;
    }

    if (model_fits) { // Filament printable check
        wxString filament_printable_error_msg;
        bool filament_printable = cur_plate->check_filament_printable(wxGetApp().preset_bundle->full_config(), filament_printable_error_msg);
        model_fits &= filament_printable;
    }

    model_fits = model_fits && object_results.filaments.empty();
    validate_error = false;
    if (p->printer_technology == ptFFF) {
        //std::string plater_text = _u8L("An object is laid over the boundary of plate or exceeds the height limit.\n"
        //            "Please solve the problem by moving it totally on or off the plate, and confirming that the height is within the build volume.");;
        StringObjectException warning;
        Polygons polygons;
        std::vector<std::pair<Polygon, float>> height_polygons;
        p->background_process.fff_print()->set_check_multi_filaments_compatibility(wxGetApp().app_config->get("enable_high_low_temp_mixed_printing") == "false");
        StringObjectException err = p->background_process.validate(&warning, &polygons, &height_polygons);
        // update string by type
        post_process_string_object_exception(err);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": validate err=%1%, warning=%2%, model_fits %3%")%err.string%warning.string %model_fits;

        if (err.string.empty()) {
            p->partplate_list.get_curr_plate()->update_apply_result_invalid(false);
            p->notification_manager->set_all_slicing_errors_gray(true);
            p->notification_manager->close_notification_of_type(NotificationType::ValidateError);
            p->notification_manager->bbl_close_3mf_warn_notification();

            // Pass a warning from validation and either show a notification,
            // or hide the old one.
            p->process_validation_warning(warning);
            p->view3D->get_canvas3d()->reset_sequential_print_clearance();
            p->view3D->get_canvas3d()->set_as_dirty();
            p->view3D->get_canvas3d()->request_extra_frame();
        }
        else {
            // The print is not valid.
            p->partplate_list.get_curr_plate()->update_apply_result_invalid(true);
            // Show error as notification.
            p->notification_manager->push_validate_error_notification(err);
            p->process_validation_warning(warning);
            //model_fits = false;
            validate_error = true;
            p->view3D->get_canvas3d()->set_sequential_print_clearance_visible(true);
            p->view3D->get_canvas3d()->set_sequential_print_clearance_render_fill(true);
            p->view3D->get_canvas3d()->set_sequential_print_clearance_polygons(polygons, height_polygons);
        }

        std::string clashed_text = get_object_clashed_text();
        if (state == ModelInstancePVS_Partly_Outside) {
            p->notification_manager->push_plater_error_notification(clashed_text);
        }
        else {
            p->notification_manager->close_plater_error_notification(clashed_text);
        }
        std::string left_unprintable_text = get_left_extruder_unprintable_text(), right_unprintable_text = get_right_extruder_unprintable_text();
        if (!left_unprintable_text.empty())
        {
            p->notification_manager->bbl_show_filament_map_invalid_notification_before_slice(NotificationType::LeftExtruderUnprintableError, left_unprintable_text);
        }
        else {
            p->notification_manager->bbl_close_filament_map_invalid_notification_before_slice(NotificationType::LeftExtruderUnprintableError);
        }

        if (!right_unprintable_text.empty())
        {
            p->notification_manager->bbl_show_filament_map_invalid_notification_before_slice(NotificationType::RightExtruderUnprintableError,right_unprintable_text);
        }
        else {
            p->notification_manager->bbl_close_filament_map_invalid_notification_before_slice(NotificationType::RightExtruderUnprintableError);
        }

        /*if (state == ModelInstancePVS_Limited) {
            p->notification_manager->push_plater_warning_notification(get_object_limited_text());
        }
        else {
            p->notification_manager->close_plater_warning_notification(get_object_limited_text());
        }*/
    }

    PartPlate* part_plate = p->partplate_list.get_curr_plate();
    part_plate->update_slice_ready_status(model_fits);

    return;
}

void Plater::open_platesettings_dialog(wxCommandEvent& evt) {
    int plate_index = evt.GetInt();
    PlateSettingsDialog dlg(this, _L("Plate Settings"), evt.GetString() == "only_layer_sequence");
    PartPlate* curr_plate = p->partplate_list.get_curr_plate();
    dlg.sync_bed_type(curr_plate->get_bed_type());

    auto curr_print_seq = curr_plate->get_print_seq();
    if (curr_print_seq != PrintSequence::ByDefault) {
        dlg.sync_print_seq(int(curr_print_seq) + 1);
    }
    else
        dlg.sync_print_seq(0);

    auto first_layer_print_seq = curr_plate->get_first_layer_print_sequence();
    if (first_layer_print_seq.empty())
        dlg.sync_first_layer_print_seq(0);
    else
        dlg.sync_first_layer_print_seq(1, curr_plate->get_first_layer_print_sequence());

    auto other_layers_print_seq = curr_plate->get_other_layers_print_sequence();
    if (other_layers_print_seq.empty())
        dlg.sync_other_layers_print_seq(0, {});
    else {
        dlg.sync_other_layers_print_seq(1, curr_plate->get_other_layers_print_sequence());
    }

    dlg.sync_spiral_mode(curr_plate->get_spiral_vase_mode(), !curr_plate->has_spiral_mode_config());

    dlg.Bind(EVT_SET_BED_TYPE_CONFIRM, [this, plate_index, &dlg](wxCommandEvent& e) {
        PartPlate* curr_plate = p->partplate_list.get_curr_plate();
        BedType old_bed_type = curr_plate->get_bed_type();
        auto bt_sel = dlg.get_bed_type_choice();
        if (old_bed_type != bt_sel) {
            curr_plate->set_bed_type(bt_sel);
            update_project_dirty_from_presets();
            set_plater_dirty(true);
        }
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format("select bed type %1% for plate %2% at plate side") % bt_sel % plate_index;

        if (dlg.get_first_layer_print_seq_choice() != 0)
            curr_plate->set_first_layer_print_sequence(dlg.get_first_layer_print_seq());
        else
            curr_plate->set_first_layer_print_sequence({});

        if (dlg.get_other_layers_print_seq_choice() != 0)
            curr_plate->set_other_layers_print_sequence(dlg.get_other_layers_print_seq_infos());
        else
            curr_plate->set_other_layers_print_sequence({});

        int ps_sel = dlg.get_print_seq_choice();
        if (ps_sel != 0)
            curr_plate->set_print_seq(PrintSequence(ps_sel - 1));
        else
            curr_plate->set_print_seq(PrintSequence::ByDefault);

        int spiral_sel = dlg.get_spiral_mode_choice();
        if (spiral_sel == 1) {
            curr_plate->set_spiral_vase_mode(true, false);
        }
        else if (spiral_sel == 2) {
            curr_plate->set_spiral_vase_mode(false, false);
        }
        else {
            curr_plate->set_spiral_vase_mode(false, true);
        }

        update_project_dirty_from_presets();
        set_plater_dirty(true);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format("select print sequence %1% for plate %2% at plate side") % ps_sel % plate_index;
        auto plate_config = *(curr_plate->config());
        wxGetApp().plater()->config_change_notification(plate_config, std::string("print_sequence"));
        update();
        wxGetApp().obj_list()->update_selections();
        });
    dlg.set_plate_name(from_u8(curr_plate->get_plate_name()));

    dlg.ShowModal();
    curr_plate->set_plate_name(dlg.get_plate_name().ToUTF8().data());
}

void Plater::open_filament_map_setting_dialog(wxCommandEvent &evt)
{
    PartPlate* curr_plate = p->partplate_list.get_curr_plate();
    int value = evt.GetInt(); //1 means from gcode view
    bool need_slice = value ==1;  // If from gcode view, should slice

    const auto& project_config = wxGetApp().preset_bundle->project_config;
    auto filament_colors = config()->option<ConfigOptionStrings>("filament_colour")->values;
    auto filament_types = config()->option<ConfigOptionStrings>("filament_type")->values;

    auto plate_filament_maps = curr_plate->get_real_filament_maps(project_config);
    auto plate_filament_map_mode = curr_plate->get_filament_map_mode();
    if (plate_filament_maps.size() != filament_colors.size())  // refine it later, save filament map to app config
        plate_filament_maps.resize(filament_colors.size(), 1);

    FilamentMapDialog filament_dlg(this,
        filament_colors,
        filament_types,
        plate_filament_maps,
        curr_plate->get_extruders(true),
        plate_filament_map_mode,
        this->get_machine_sync_status(),
        false
    );

    if (filament_dlg.ShowModal() == wxID_OK) {
        std::vector<int> new_filament_maps = filament_dlg.get_filament_maps();
        std::vector<int> old_filament_maps = curr_plate->get_real_filament_maps(project_config);

        FilamentMapMode  old_map_mode = curr_plate->get_filament_map_mode();
        FilamentMapMode  new_map_mode = filament_dlg.get_mode();

        if (new_map_mode != old_map_mode) {
            curr_plate->set_filament_map_mode(new_map_mode);
        }

        if (new_map_mode == fmmManual){
            curr_plate->set_filament_maps(new_filament_maps);
        }

        bool need_invalidate = (old_map_mode != new_map_mode ||
                                old_filament_maps != new_filament_maps);

        if (need_invalidate) {
            if (need_slice) {
                wxPostEvent(this, SimpleEvent(EVT_GLTOOLBAR_SLICE_PLATE));
            }
            else {
                curr_plate->update_slice_result_valid_state(false);
                set_plater_dirty(true);
                update();
            }
        }
    }
    return;
}


//BBS: select Plate by hover_id
int Plater::select_plate_by_hover_id(int hover_id, bool right_click, bool isModidyPlateName)
{
    int ret;
    int action, plate_index;

    plate_index = hover_id / PartPlate::GRABBER_COUNT;
    action      = isModidyPlateName ? PartPlate::PLATE_NAME_HOVER_ID : hover_id % PartPlate::GRABBER_COUNT;

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": enter, hover_id %1%, plate_index %2%, action %3%")%hover_id % plate_index %action;
    if (action == 0)
    {
        //select plate
        ret = p->partplate_list.select_plate(plate_index);
        if (!ret) {
            SimpleEvent event(EVT_GLCANVAS_PLATE_SELECT);
            p->on_plate_selected(event);
        }
        if ((!ret)&&(p->background_process.can_switch_print()))
        {
            //select successfully
            p->partplate_list.update_slice_context_to_current_plate(p->background_process);
            p->preview->update_gcode_result(p->partplate_list.get_current_slice_result());
            p->update_print_volume_state();

            PartPlate* part_plate = p->partplate_list.get_curr_plate();
            bool result_valid = part_plate->is_slice_result_valid();
            PrintBase* print = nullptr;
            GCodeResult* gcode_result = nullptr;
            Print::ApplyStatus invalidated;

            const auto& preset_bundle = wxGetApp().preset_bundle;

            part_plate->get_print(&print, &gcode_result, NULL);
            //always apply the current plate's print
            if (preset_bundle->get_printer_extruder_count() > 1) {
                std::vector<int> f_maps = part_plate->get_real_filament_maps(preset_bundle->project_config);
                invalidated = p->background_process.apply(this->model(), preset_bundle->full_config(false, f_maps));
            }
            else
                invalidated = p->background_process.apply(this->model(), preset_bundle->full_config(false));
            bool model_fits, validate_err;
            validate_current_plate(model_fits, validate_err);

            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" %1%: after apply, invalidated= %2%, previous result_valid %3% ")%__LINE__ % invalidated %result_valid;
            if (result_valid)
            {
                if (invalidated & PrintBase::APPLY_STATUS_INVALIDATED)
                {
                    //bool model_fits, validate_err;
                    //validate_current_plate(model_fits, validate_err);
                    part_plate->update_slice_result_valid_state(false);

                    // BBS
                    //p->show_action_buttons(true);
                    //p->ready_to_slice = true;
                    p->main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, true);
                }
                else
                {
                    // BBS
                    //p->show_action_buttons(false);
                    //validate_current_plate(model_fits, validate_err);
                    //p->ready_to_slice = false;
                    p->main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, false);

                    refresh_print();
                }
            }
            else
            {
                //check inside status
                if (model_fits && !validate_err){
                    p->process_completed_with_error = -1;
                }
                else {
                    p->process_completed_with_error = p->partplate_list.get_curr_plate_index();
                }

                // BBS: don't show action buttons
                //p->show_action_buttons(true);
                //p->ready_to_slice = true;
                if (model_fits && part_plate->has_printable_instances())
                {
                    //p->view3D->get_canvas3d()->post_event(Event<bool>(EVT_GLCANVAS_ENABLE_ACTION_BUTTONS, true));
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": will set can_slice to true");
                    p->main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, true);
                }
                else
                {
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": will set can_slice to false, has_printable_instances %1%")%part_plate->has_printable_instances();
                    //p->view3D->get_canvas3d()->post_event(Event<bool>(EVT_GLCANVAS_ENABLE_ACTION_BUTTONS, false));
                    p->main_frame->update_slice_print_status(MainFrame::eEventPlateUpdate, false);
                }
            }
        }
    }
    else if ((action == 1)&&(!right_click))
    {
        //delete plate
        ret = delete_plate(plate_index);
    }
    else if ((action == 2)&&(!right_click))
    {
        //arrange the plate
        //take_snapshot("select_orient partplate");
        ret = select_plate(plate_index);
        if (!ret)
        {
            set_prepare_state(Job::PREPARE_STATE_MENU);
            orient();
        }
        else
        {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "can not select plate %1%" << plate_index;
            ret = -1;
        }
    }
    else if ((action == 3)&&(!right_click))
    {
        //arrange the plate
        //take_snapshot("select_arrange partplate");
        ret = select_plate(plate_index);
        if (!ret)
        {
            if (last_arrange_job_is_finished()) {
                set_prepare_state(Job::PREPARE_STATE_MENU);
                arrange();
            }
        }
        else
        {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "can not select plate %1%" << plate_index;
            ret = -1;
        }
    }
    else if ((action == 4)&&(!right_click))
    {
        //lock the plate
        take_snapshot("lock partplate");
        ret = p->partplate_list.lock_plate(plate_index, !p->partplate_list.is_locked(plate_index));
    }
    else if ((action == 5)&&(!right_click))
    {
        // set the plate type
        ret = select_plate(plate_index);
        if (!ret) {
            wxCommandEvent evt(EVT_OPEN_PLATESETTINGSDIALOG);
            evt.SetInt(plate_index);
            evt.SetEventObject(this);
            wxPostEvent(this, evt);

            this->schedule_background_process();
        } else {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "can not select plate %1%" << plate_index;
            ret = -1;
        }
    }
    else if ((action == PartPlate::PLATE_FILAMENT_MAP_ID) && (!right_click)) {
        ret = select_plate(plate_index);
        if (!ret) {
            PartPlate *         curr_plate = p->partplate_list.get_curr_plate();
            wxCommandEvent evt(EVT_OPEN_FILAMENT_MAP_SETTINGS_DIALOG);
            evt.SetInt(0); // 0 means not from gcodeviewer
            evt.SetEventObject(this);
            wxPostEvent(this, evt);
        } else {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "can not select plate %1%" << plate_index;
            ret = -1;
        }
    }
    else if ((action == 6) && (!right_click)) {
        // set the plate type
        ret = select_plate(plate_index);
        if (!ret) {
            PlateNameEditDialog dlg(this, wxID_ANY, _L("Edit Plate Name"));
            PartPlate *         curr_plate = p->partplate_list.get_curr_plate();

            wxString curr_plate_name = from_u8(curr_plate->get_plate_name());
            dlg.set_plate_name(curr_plate_name);

            int result=dlg.ShowModal();
            if (result == wxID_YES) {
                wxString dlg_plate_name = dlg.get_plate_name();
                curr_plate->set_plate_name(dlg_plate_name.ToUTF8().data());
            }
        } else {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "can not select plate %1%" << plate_index;
            ret = -1;
        }
    } else if ((action == 7) && (!right_click)) {
        // move plate to the front
        take_snapshot("move plate to the front");
        ret = p->partplate_list.move_plate_to_index(plate_index,0);
        p->partplate_list.update_slice_context_to_current_plate(p->background_process);
        p->preview->update_gcode_result(p->partplate_list.get_current_slice_result());
        p->sidebar->obj_list()->reload_all_plates();
        p->partplate_list.update_plates();
        update();
        p->partplate_list.select_plate(0);
    }

    else
    {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "invalid action %1%, with right_click=%2%" << action << right_click;
        ret = -1;
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" %1%: return %2%")%__LINE__ % ret;
    return ret;
}

int Plater::duplicate_plate(int plate_index)
{
    int index = plate_index, ret;
    if (plate_index == -1)
        index = p->partplate_list.get_curr_plate_index();

    ret = p->partplate_list.duplicate_plate(index);

    //need to call update
    update();
    return ret;
}

//BBS: delete the plate, index= -1 means the current plate
int Plater::delete_plate(int plate_index)
{
    int index = plate_index, ret;

    if (plate_index == -1)
        index = p->partplate_list.get_curr_plate_index();

    take_snapshot("delete partplate");
    ret = p->partplate_list.delete_plate(index);

    //BBS: update the current print to the current plate
    p->partplate_list.update_slice_context_to_current_plate(p->background_process);
    p->preview->update_gcode_result(p->partplate_list.get_current_slice_result());
    p->sidebar->obj_list()->reload_all_plates();

    // BBS update default view
    //get_camera().select_view("topfront");
    //get_camera().requires_zoom_to_plate = REQUIRES_ZOOM_TO_ALL_PLATE;

    //need to call update
    update();
    return ret;
}

//BBS: set bed positions
void Plater::set_bed_position(Vec2d& pos)
{
    p->bed.set_position(pos);
}

//BBS: is the background process slicing currently
bool Plater::is_background_process_slicing() const
{
    return p->m_is_slicing;
}

//BBS: update slicing context
void Plater::update_slicing_context_to_current_partplate()
{
    p->partplate_list.update_slice_context_to_current_plate(p->background_process);
    p->preview->update_gcode_result(p->partplate_list.get_current_slice_result());
}

//BBS: show object info
void Plater::show_object_info()
{
    NotificationManager *notify_manager = get_notification_manager();
    const Selection& selection = get_selection();
    int selCount = selection.get_volume_idxs().size();
    ModelObjectPtrs objects = model().objects;
    int obj_idx = selection.get_object_idx();
    std::string info_text;

    if (selCount > 1 && !selection.is_single_full_object()) {
        notify_manager->bbl_close_objectsinfo_notification();
        if (selection.get_mode() == Selection::EMode::Volume) {
            info_text += (boost::format(_utf8(L("Number of currently selected parts: %1%\n"))) % selCount).str();
        } else if (selection.get_mode() == Selection::EMode::Instance) {
            int content_count = selection.get_content().size();
            info_text += (boost::format(_utf8(L("Number of currently selected objects: %1%\n"))) % content_count).str();
        }
        notify_manager->bbl_show_objectsinfo_notification(info_text, false, !(p->current_panel == p->view3D));
        return;
    }
    else if (objects.empty() || (obj_idx < 0) || (obj_idx >= objects.size()) ||
        objects[obj_idx]->volumes.empty() ||// hack to avoid crash when deleting the last object on the bed
        (selection.is_single_full_object() && objects[obj_idx]->instances.size()> 1) ||
        !(selection.is_single_full_instance() || selection.is_single_volume()))
    {
        notify_manager->bbl_close_objectsinfo_notification();
        return;
    }

    const ModelObject* model_object = objects[obj_idx];
    int inst_idx = selection.get_instance_idx();
    if ((inst_idx < 0) || (inst_idx >= model_object->instances.size()))
    {
        notify_manager->bbl_close_objectsinfo_notification();
        return;
    }
    bool imperial_units = wxGetApp().app_config->get("use_inches") == "1";
    double koef = imperial_units ? GizmoObjectManipulation::mm_to_in : 1.0f;

    ModelVolume* vol = nullptr;
    Transform3d t;
    int face_count;
    Vec3d size;
    if (selection.is_single_volume()) {
        std::vector<int> obj_idxs, vol_idxs;
        wxGetApp().obj_list()->get_selection_indexes(obj_idxs, vol_idxs);
        if (vol_idxs.size() != 1)
        {
            //corner case when merge/split/remove
            return;
        }
        vol = model_object->volumes[vol_idxs[0]];
        t = model_object->instances[inst_idx]->get_matrix() * vol->get_matrix();
        info_text += (boost::format(_utf8(L("Part name: %1%\n"))) % vol->name).str();
        face_count = static_cast<int>(vol->mesh().facets_count());
        size = vol->get_convex_hull().transformed_bounding_box(t).size();
    }
    else {
        //int obj_idx, vol_idx;
        //wxGetApp().obj_list()->get_selected_item_indexes(obj_idx, vol_idx);
        //if (obj_idx < 0) {
        //    //corner case when merge/split/remove
        //    return;
        //}
        info_text += (boost::format(_utf8(L("Object name: %1%\n"))) % model_object->name).str();
        face_count = static_cast<int>(model_object->facets_count());
        size = model_object->instance_convex_hull_bounding_box(inst_idx).size();
    }

    //Vec3d size = vol ? vol->mesh().transformed_bounding_box(t).size() : model_object->instance_bounding_box(inst_idx).size();
    if (imperial_units)
        info_text += (boost::format(_utf8(L("Size: %1% x %2% x %3% in\n"))) %(size(0)*koef) %(size(1)*koef) %(size(2)*koef)).str();
    else
        info_text += (boost::format(_utf8(L("Size: %1% x %2% x %3% mm\n"))) %size(0) %size(1) %size(2)).str();

    const TriangleMeshStats& stats = vol ? vol->mesh().stats() : model_object->get_object_stl_stats();
    double volume_val = stats.volume;
    if (vol)
        volume_val *= std::fabs(t.matrix().block(0, 0, 3, 3).determinant());
    volume_val = volume_val * pow(koef,3);
    if (imperial_units)
        info_text += (boost::format(_utf8(L("Volume: %1% in³\n"))) %volume_val).str();
    else
        info_text += (boost::format(_utf8(L("Volume: %1% mm³\n"))) %volume_val).str();
    info_text += (boost::format(_utf8(L("Triangles: %1%\n"))) %face_count).str();

    wxString info_manifold;
    int non_manifold_edges = 0;
    auto mesh_errors = p->sidebar->obj_list()->get_mesh_errors_info(&info_manifold, &non_manifold_edges);

    #ifndef __WINDOWS__
    if (non_manifold_edges > 0) {
        info_manifold += into_u8("\n" + _L("Tips:") + "\n" +_L("\"Fix Model\" feature is currently only on Windows. Please repair the model on Orca Slicer(windows) or CAD softwares."));
    }
    #endif //APPLE & LINUX

    info_manifold = "<Error>" + info_manifold + "</Error>";
    info_text += into_u8(info_manifold);
    notify_manager->bbl_show_objectsinfo_notification(info_text, is_windows10()&&(non_manifold_edges > 0), !(p->current_panel == p->view3D));
}

bool Plater::show_publish_dialog(bool show)
{
    return p->show_publish_dlg(show);
}

void Plater::post_process_string_object_exception(StringObjectException &err)
{
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (err.type == StringExceptionType::STRING_EXCEPT_FILAMENT_NOT_MATCH_BED_TYPE) {
        try {
            int extruder_id = atoi(err.params[2].c_str()) - 1;
            if (extruder_id < preset_bundle->filament_presets.size()) {
                std::string filament_name = preset_bundle->filament_presets[extruder_id];
                for (auto filament_it = preset_bundle->filaments.begin(); filament_it != preset_bundle->filaments.end(); filament_it++) {
                    if (filament_it->name == filament_name) {
                        if (filament_it->is_system) {
                            filament_name = filament_it->alias;
                        } else {
                            auto preset = preset_bundle->filaments.get_preset_base(*filament_it);
                            if (preset && !preset->alias.empty()) {
                                filament_name = preset->alias;
                            } else {
                                char target = '@';
                                size_t pos    = filament_name.find(target);
                                if (pos != std::string::npos) {
                                    filament_name = filament_name.substr(0, pos - 1);
                                }
                            }
                        }
                        break;
                    }
                }
                err.string = format(_L("Plate %d: %s is not suggested to be used to print filament %s (%s). "
                                       "If you still want to do this print job, please set this filament's bed temperature to non-zero."),
                             err.params[0], err.params[1], err.params[2], filament_name);
                err.string += "\n";
            }
        } catch (...) {
            ;
        }
    }

    return;
}

void Plater::update_objects_position_when_select_preset(const std::function<void()> &select_prest)
{
    p->update_objects_position_when_select_preset(select_prest);
}

bool Plater::check_ams_status(bool is_slice_all)
{
    if (m_check_status == 0) {
        if (!p->check_ams_status_impl(is_slice_all)) {
            m_check_status = 0;
            return false;
        }
        else {
            m_check_status = 1;
        }
    }

    return true;
}

void Plater::update_machine_sync_status()
{
    DeviceManager *dev_maneger = wxGetApp().getDeviceManager();
    if (!dev_maneger) {
        GUI::wxGetApp().sidebar().update_sync_status(nullptr);
        return;
    }
    MachineObject *obj = wxGetApp().getDeviceManager()->get_selected_machine();
    GUI::wxGetApp().sidebar().update_sync_status(obj);
}

bool Plater::get_machine_sync_status()
{
    return p->get_machine_sync_status();
}

#if ENABLE_ENVIRONMENT_MAP
void Plater::init_environment_texture()
{
    if (p->environment_texture.get_id() == 0)
        p->environment_texture.load_from_file(resources_dir() + "/images/Pmetal_001.png", false, GLTexture::SingleThreaded, false);
}

unsigned int Plater::get_environment_texture_id() const
{
    return p->environment_texture.get_id();
}
#endif // ENABLE_ENVIRONMENT_MAP

const BuildVolume& Plater::build_volume() const
{
    return p->bed.build_volume();
}

// BBS
#if 0
const GLToolbar& Plater::get_view_toolbar() const
{
    return p->view_toolbar;
}

GLToolbar& Plater::get_view_toolbar()
{
    return p->view_toolbar;
}
#endif

const GLToolbar& Plater::get_collapse_toolbar() const
{
    return p->collapse_toolbar;
}

GLToolbar& Plater::get_collapse_toolbar()
{
    return p->collapse_toolbar;
}

void Plater::update_preview_bottom_toolbar()
{
    p->update_preview_bottom_toolbar();
}

void Plater::reset_gcode_toolpaths()
{
    //BBS: add some logs
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(": reset the gcode viewer's toolpaths");
    p->reset_gcode_toolpaths();
}

const Mouse3DController& Plater::get_mouse3d_controller() const
{
    return p->mouse3d_controller;
}

Mouse3DController& Plater::get_mouse3d_controller()
{
    return p->mouse3d_controller;
}

NotificationManager * Plater::get_notification_manager()
{
    return p->notification_manager.get();
}

DailyTipsWindow* Plater::get_dailytips() const
{
    static DailyTipsWindow* dailytips_win = new DailyTipsWindow();
    return dailytips_win;
}

const NotificationManager * Plater::get_notification_manager() const
{
    return p->notification_manager.get();
}

void Plater::init_notification_manager()
{
    p->init_notification_manager();
}

void Plater::show_status_message(std::string s)
{
    BOOST_LOG_TRIVIAL(trace) << "show_status_message:" << s;
}

bool Plater::can_delete() const { return p->can_delete(); }
bool Plater::can_delete_all() const { return p->can_delete_all(); }
bool Plater::can_add_model() const { return !is_background_process_slicing(); }
bool Plater::can_add_plate() const { return !is_background_process_slicing() && p->can_add_plate(); }
bool Plater::can_delete_plate() const { return p->can_delete_plate(); }
bool Plater::can_increase_instances() const { return p->can_increase_instances(); }
bool Plater::can_decrease_instances() const { return p->can_decrease_instances(); }
bool Plater::can_set_instance_to_object() const { return p->can_set_instance_to_object(); }
bool Plater::can_fix_through_netfabb() const { return p->can_fix_through_netfabb(); }
bool Plater::can_simplify() const { return p->can_simplify(); }
bool Plater::can_split_to_objects() const { return p->can_split_to_objects(); }
bool Plater::can_split_to_volumes() const { return p->can_split_to_volumes(); }
bool Plater::can_arrange() const { return p->can_arrange(); }
bool Plater::can_layers_editing() const { return p->can_layers_editing(); }
bool Plater::can_paste_from_clipboard() const
{
    if (!IsShown() || !p->is_view3D_shown()) return false;

    const Selection& selection = p->view3D->get_canvas3d()->get_selection();
    const Selection::Clipboard& clipboard = selection.get_clipboard();

    if (clipboard.is_empty() && p->sidebar->obj_list()->clipboard_is_empty())
        return false;

    if ((wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptSLA) && !clipboard.is_sla_compliant())
        return false;

    Selection::EMode mode = clipboard.get_mode();
    if ((mode == Selection::Volume) && !selection.is_from_single_instance())
        return false;

    if ((mode == Selection::Instance) && (selection.get_mode() != Selection::Instance))
        return false;

    return true;
}

//BBS support cut
bool Plater::can_cut_to_clipboard() const
{
    if (is_selection_empty())
        return false;
    return true;
}

bool Plater::can_copy_to_clipboard() const
{
    if (!IsShown() || !p->is_view3D_shown())
        return false;

    if (is_selection_empty())
        return false;

    const Selection& selection = p->view3D->get_canvas3d()->get_selection();
    if ((wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptSLA) && !selection.is_sla_compliant())
        return false;

    return true;
}

bool Plater::can_undo() const { return IsShown() && p->is_view3D_shown() && p->undo_redo_stack().has_undo_snapshot(); }
bool Plater::can_redo() const { return IsShown() && p->is_view3D_shown() && p->undo_redo_stack().has_redo_snapshot(); }
bool Plater::can_reload_from_disk() const { return p->can_reload_from_disk(); }
//BBS
bool Plater::can_fillcolor() const { return p->can_fillcolor(); }
bool Plater::has_assmeble_view() const { return p->has_assemble_view(); }
bool Plater::can_replace_with_stl() const { return p->can_replace_with_stl(); }
bool Plater::can_replace_all_with_stl() const { return p->can_replace_all_with_stl(); }
bool Plater::can_mirror() const { return p->can_mirror(); }
bool Plater::can_split(bool to_objects) const { return p->can_split(to_objects); }
#if ENABLE_ENHANCED_PRINT_VOLUME_FIT
bool Plater::can_scale_to_print_volume() const { return p->can_scale_to_print_volume(); }
#endif // ENABLE_ENHANCED_PRINT_VOLUME_FIT

const UndoRedo::Stack& Plater::undo_redo_stack_main() const { return p->undo_redo_stack_main(); }
void Plater::clear_undo_redo_stack_main() { p->undo_redo_stack_main().clear(); }
void Plater::enter_gizmos_stack() { p->enter_gizmos_stack(); }
bool Plater::leave_gizmos_stack() { return p->leave_gizmos_stack(); } // BBS: return false if not changed
bool Plater::inside_snapshot_capture() { return p->inside_snapshot_capture(); }

void Plater::toggle_render_statistic_dialog()
{
    p->show_render_statistic_dialog = !p->show_render_statistic_dialog;
}

bool Plater::is_render_statistic_dialog_visible() const
{
    return p->show_render_statistic_dialog;
}

void Plater::toggle_show_wireframe()
{
    p->show_wireframe = !p->show_wireframe;
}

bool Plater::is_show_wireframe() const
{
    return p->show_wireframe;
}

void Plater::enable_wireframe(bool status)
{
    p->wireframe_enabled = status;
}

bool Plater::is_wireframe_enabled() const
{
    return p->wireframe_enabled;
}


/*Plater::TakeSnapshot::TakeSnapshot(Plater *plater, const std::string &snapshot_name)
: TakeSnapshot(plater, from_u8(snapshot_name)) {}
Plater::TakeSnapshot::TakeSnapshot(Plater* plater, const std::string& snapshot_name, UndoRedo::SnapshotType snapshot_type)
: TakeSnapshot(plater, from_u8(snapshot_name), snapshot_type) {}*/


// Wrapper around wxWindow::PopupMenu to suppress error messages popping out while tracking the popup menu.
bool Plater::PopupMenu(wxMenu *menu, const wxPoint& pos)
{
    // Don't want to wake up and trigger reslicing while tracking the pop-up menu.
    SuppressBackgroundProcessingUpdate sbpu;
    // When tracking a pop-up menu, postpone error messages from the slicing result.
    m_tracking_popup_menu = true;
    bool out = wxGetApp().mainframe->PopupMenu(menu, pos);
    m_tracking_popup_menu = false;
    if (! m_tracking_popup_menu_error_message.empty()) {
        // Don't know whether the CallAfter is necessary, but it should not hurt.
        // The menus likely sends out some commands, so we may be safer if the dialog is shown after the menu command is processed.
        wxString message = std::move(m_tracking_popup_menu_error_message);
        wxTheApp->CallAfter([message, this]() { show_error(this, message); });
        m_tracking_popup_menu_error_message.clear();
    }
    return out;
}
void Plater::bring_instance_forward()
{
    p->bring_instance_forward();
}

bool Plater::need_update() const
{
    return p->need_update();
}

void Plater::set_need_update(bool need_update)
{
    p->set_need_update(need_update);
}

// BBS
//BBS: add popup logic for table object
bool Plater::PopupObjectTable(int object_id, int volume_id, const wxPoint& position)
{
    if (dynamic_cast<TabPrinter *>(wxGetApp().get_tab(Preset::TYPE_PRINTER))->m_extruders_count > 1) {
        MessageDialog dlg(this, _L("Currently, the object configuration form cannot be used with a multiple-extruder printer."), _L("Not available"), wxOK | wxICON_WARNING);
        dlg.ShowModal();
        return false;
    }
    return p->PopupObjectTable(object_id, volume_id, position);
}

bool Plater::PopupObjectTableBySelection()
{
    wxDataViewItem item;
    int obj_idx, vol_idx;
    const wxPoint pos = wxPoint(0, 0);  //Fake position
    wxGetApp().obj_list()->get_selected_item_indexes(obj_idx, vol_idx, item);
    return p->PopupObjectTable(obj_idx, vol_idx, pos);
}

void Plater::update_title_dirty_status()
{
    p->update_title_dirty_status();
}


wxMenu* Plater::plate_menu()            { return p->menus.plate_menu();             }
wxMenu* Plater::object_menu()           { return p->menus.object_menu();            }
wxMenu* Plater::part_menu()             { return p->menus.part_menu();              }
wxMenu* Plater::text_part_menu()        { return p->menus.text_part_menu();         }
wxMenu* Plater::svg_part_menu()         { return p->menus.svg_part_menu();          }
wxMenu* Plater::sla_object_menu()       { return p->menus.sla_object_menu();        }
wxMenu* Plater::default_menu()          { return p->menus.default_menu();           }
wxMenu* Plater::instance_menu()         { return p->menus.instance_menu();          }
wxMenu* Plater::layer_menu()            { return p->menus.layer_menu();             }
wxMenu* Plater::multi_selection_menu()  { return p->menus.multi_selection_menu();   }
wxMenu *Plater::filament_action_menu(int active_filament_menu_id) { return p->menus.filament_action_menu(active_filament_menu_id); }
int     Plater::GetPlateIndexByRightMenuInLeftUI() { return p->m_is_RightClickInLeftUI; }
void    Plater::SetPlateIndexByRightMenuInLeftUI(int index) { p->m_is_RightClickInLeftUI = index; }
SuppressBackgroundProcessingUpdate::SuppressBackgroundProcessingUpdate() :
    m_was_scheduled(wxGetApp().plater()->is_background_process_update_scheduled())
{
    wxGetApp().plater()->suppress_background_process(m_was_scheduled);
}

SuppressBackgroundProcessingUpdate::~SuppressBackgroundProcessingUpdate()
{
    wxGetApp().plater()->schedule_background_process(m_was_scheduled);
}
wxString get_view_type_string(Camera::ViewAngleType type) {
    switch (type) {
    case Slic3r::GUI::Camera::ViewAngleType::Iso: return _L("isometric");
    case Slic3r::GUI::Camera::ViewAngleType::Top_Front: return _L("top_front");
    case Slic3r::GUI::Camera::ViewAngleType::Left: return _L("left");
    case Slic3r::GUI::Camera::ViewAngleType::Right: return _L("right");
    case Slic3r::GUI::Camera::ViewAngleType::Top: return _L("top");
    case Slic3r::GUI::Camera::ViewAngleType::Bottom: return _L("bottom");
    case Slic3r::GUI::Camera::ViewAngleType::Front: return _L("front");
    case Slic3r::GUI::Camera::ViewAngleType::Rear: return _L("rear");
    default: return "";
    }
}
wxArrayString get_all_camera_view_type() {
    wxArrayString all_types;
    for (size_t i = 0; i < (int)Camera::ViewAngleType::Count_ViewAngleType; i++) {
        all_types.Add(get_view_type_string((Camera::ViewAngleType) i));
    }
    return all_types;
}
}}    // namespace Slic3r::GUI
