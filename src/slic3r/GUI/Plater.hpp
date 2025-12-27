#ifndef slic3r_Plater_hpp_
#define slic3r_Plater_hpp_

#include <memory>
#include <vector>
#include <boost/filesystem/path.hpp>

#include <wx/panel.h>
// BBS
#include <wx/notebook.h>

#include "Selection.hpp"

#include "libslic3r/enum_bitmask.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "Jobs/Job.hpp"
#include "Jobs/Worker.hpp"
#include "Search.hpp"
#include "PartPlate.hpp"
#include "GUI_App.hpp"
#include "Jobs/PrintJob.hpp"
#include "Jobs/SendJob.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintBase.hpp"

#include "libslic3r/calib.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/FlushVolCalc.hpp"

#define FILAMENT_SYSTEM_COLORS_NUM      16

class wxButton;
class ScalableButton;
class wxScrolledWindow;
class wxString;
class ComboBox;
class Button;

namespace Slic3r {

class BuildVolume;
enum class BuildVolume_Type : char;
class Model;
class ModelObject;
class ModelInstance;
class Print;
class SLAPrint;
//BBS: add partplatelist and SlicingStatusEvent
class PartPlateList;
class SlicingStatusEvent;
enum SLAPrintObjectStep : unsigned int;
enum class ConversionType : int;
class DevAms;

using ModelInstancePtrs = std::vector<ModelInstance*>;


namespace UndoRedo {
    class Stack;
    enum class SnapshotType : unsigned char;
    struct Snapshot;
}

namespace GUI {
class SyncAmsInfoDialog;
class MainFrame;
class ConfigOptionsGroup;
class ObjectSettings;
class ObjectLayers;
class ObjectList;
class GLCanvas3D;
class Mouse3DController;
class NotificationManager;
class DailyTipsWindow;
struct Camera;
class GLToolbar;
class PlaterPresetComboBox;
class PartPlateList;
class SyncNozzleAndAmsDialog;
class FinishSyncAmsDialog;
using t_optgroups = std::vector <std::shared_ptr<ConfigOptionsGroup>>;

class Plater;
enum class ActionButtonType : int;

#define EVT_PUBLISHING_START        1
#define EVT_PUBLISHING_STOP         2

//BBS: add EVT_SLICING_UPDATE declare here
wxDECLARE_EVENT(EVT_SLICING_UPDATE, Slic3r::SlicingStatusEvent);
wxDECLARE_EVENT(EVT_PUBLISH,        wxCommandEvent);
wxDECLARE_EVENT(EVT_OPEN_PLATESETTINGSDIALOG,        wxCommandEvent);

// Explanation of int param
// Bit 0: 1 = automatic mode, 0 = manual mode.
// Bit 1: 1 = need auto slicing(from preview page), 0 = not need auto slicing.
wxDECLARE_EVENT(EVT_OPEN_FILAMENT_MAP_SETTINGS_DIALOG, wxCommandEvent);
wxDECLARE_EVENT(EVT_REPAIR_MODEL,        wxCommandEvent);
wxDECLARE_EVENT(EVT_FILAMENT_COLOR_CHANGED,        wxCommandEvent);
wxDECLARE_EVENT(EVT_INSTALL_PLUGIN_NETWORKING,        wxCommandEvent);
wxDECLARE_EVENT(EVT_INSTALL_PLUGIN_HINT,        wxCommandEvent);
wxDECLARE_EVENT(EVT_UPDATE_PLUGINS_WHEN_LAUNCH,        wxCommandEvent);
wxDECLARE_EVENT(EVT_PREVIEW_ONLY_MODE_HINT,        wxCommandEvent);
wxDECLARE_EVENT(EVT_GLCANVAS_COLOR_MODE_CHANGED,   SimpleEvent);
wxDECLARE_EVENT(EVT_PRINT_FROM_SDCARD_VIEW,   SimpleEvent);
wxDECLARE_EVENT(EVT_CREATE_FILAMENT, SimpleEvent);
wxDECLARE_EVENT(EVT_MODIFY_FILAMENT, SimpleEvent);
wxDECLARE_EVENT(EVT_ADD_FILAMENT, SimpleEvent);
wxDECLARE_EVENT(EVT_DEL_FILAMENT, SimpleEvent);
wxDECLARE_EVENT(EVT_NOTICE_CHILDE_SIZE_CHANGED, SimpleEvent);
wxDECLARE_EVENT(EVT_NOTICE_FULL_SCREEN_CHANGED, IntEvent);
using ColorEvent = Event<wxColour>;
wxDECLARE_EVENT(EVT_ADD_CUSTOM_FILAMENT, ColorEvent);
const wxString DEFAULT_PROJECT_NAME = "Untitled";

class SidebarProps
{
public:
    static int TitlebarMargin(){ return 8 ;} // Use as side margins on titlebar. Has less margin on sides to create separation with its content
    static int ContentMargin() { return 12;} // Use as side margins contents of title
    static int ContentMarginV(){ return 9 ;} // Use as vertical margins contents of title
    static int IconSpacing()   { return 10;} // Use on main elements in same group of controls
    static int WideSpacing()   { return 18;} // Use between main elements / control groups for separation or preventing accidental clicks important
    static int ElementSpacing(){ return 5 ;} // Use if elements has relation between them like edit button for combo box etc.
};

class Sidebar : public wxPanel
{
    ConfigOptionMode    m_mode;
    //Button *         btn_sync{nullptr};
    ScalableButton *  ams_btn{nullptr};
    bool                                    m_last_slice_state = false;
    SyncNozzleAndAmsDialog*                 m_sna_dialog{nullptr};
    FinishSyncAmsDialog*                    m_fna_dialog{nullptr};
    std::vector<BedType>                    m_cur_combox_bed_types;
    std::string                             m_cur_image_bed_type;
    int                                     m_last_combo_bedtype_count{0};
    bool                                    m_begin_sync_printer_status{false};
    SyncAmsInfoDialog*                      m_sync_dlg{nullptr};

    void update_sync_ams_btn_enable(wxUpdateUIEvent &e);

public:
    enum DockingState
    {
        None, Left, Right
    };

    Sidebar(Plater *parent);
    Sidebar(Sidebar &&) = delete;
    Sidebar(const Sidebar &) = delete;
    Sidebar &operator=(Sidebar &&) = delete;
    Sidebar &operator=(const Sidebar &) = delete;
    ~Sidebar();

    void on_enter_image_printer_bed(wxMouseEvent &evt);
    void on_leave_image_printer_bed(wxMouseEvent &evt);
    void on_change_color_mode(bool is_dark);
    void create_printer_preset();
    void init_filament_combo(PlaterPresetComboBox **combo, const int filament_idx);
    void remove_unused_filament_combos(const size_t current_extruder_count);
    void update_all_preset_comboboxes();
    //void update_partplate(PartPlateList& list);
    void update_presets(Slic3r::Preset::Type preset_type);
    //BBS
    const std::vector<BedType>& get_cur_combox_bed_types() { return m_cur_combox_bed_types; }
    void update_presets_from_to(Slic3r::Preset::Type preset_type, std::string from, std::string to);
    BedType get_cur_select_bed_type();
    std::string get_cur_select_bed_image();
    void set_bed_type_accord_combox(BedType bed_type);
    bool reset_bed_type_combox_choices(bool is_sidebar_init = false);
    void change_top_border_for_mode_sizer(bool increase_border);
    void msw_rescale();
    void sys_color_changed();
    void search();
    void jump_to_option(size_t selected);
    void jump_to_option(const std::string& opt_key, Preset::Type type, const std::wstring& category);
    // BBS. Add filament_added() method.
    void on_filament_count_change(size_t num_filaments);
    void on_filaments_delete(size_t filament_id);

    void add_filament();
    void delete_filament(size_t filament_id = size_t(-1), int replace_filament_id = -1);  // 0 base, -1 means default
    void change_filament(size_t from_id, size_t to_id);  // 0 base
    void edit_filament();
    void add_custom_filament(wxColour new_col);
    bool is_new_project_in_gcode3mf();
    // BBS
    void on_bed_type_change(BedType bed_type);
    void load_ams_list(MachineObject* obj);
    std::map<int, DynamicPrintConfig> build_filament_ams_list(MachineObject* obj);
    void sync_ams_list(bool is_from_big_sync_btn = false);
    bool sync_extruder_list();
    bool need_auto_sync_extruder_list_after_connect_priner(const MachineObject* obj);
    void update_sync_status(const MachineObject* obj);
    int get_sidebar_pos_right_x();
    void on_size(SimpleEvent &e);
    void on_full_screen(IntEvent &);
    void get_big_btn_sync_pos_size(wxPoint &pt, wxSize &size);
    void get_small_btn_sync_pos_size(wxPoint &pt, wxSize &size);
    // Orca
    static bool should_show_SEMM_buttons();
    void show_SEMM_buttons(bool bshow);
    void update_dynamic_filament_list();

    PlaterPresetComboBox *  printer_combox();
    ObjectList*             obj_list();
    ObjectSettings*         obj_settings();
    ObjectLayers*           obj_layers();
    wxPanel*                scrolled_panel();
    wxPanel* print_panel();
    wxPanel* filament_panel();

    ConfigOptionsGroup*     og_freq_chng_params(const bool is_fff);
    wxButton*               get_wiping_dialog_button();
    void                    set_flushing_volume_warning(const bool flushing_volume_modify);

    // BBS
    void                    enable_buttons(bool enable);
    void                    set_btn_label(const ActionButtonType btn_type, const wxString& label) const;
    bool                    show_reslice(bool show) const;
	bool                    show_export(bool show) const;
	bool                    show_send(bool show) const;
    bool                    show_eject(bool show)const;
	bool                    show_export_removable(bool show) const;
	bool                    get_eject_shown() const;
    bool                    is_multifilament();
    void                    deal_btn_sync();
    void                    pop_sync_nozzle_and_ams_dialog();
    void                    pop_finsish_sync_ams_dialog();
    void                    update_mode();
    bool                    is_collapsed();
    void                    collapse(bool collapse);
    void                    update_searcher();
    void                    update_ui_from_settings();
	bool                    show_object_list(bool show) const;
    void                    finish_param_edit();

    /**
     * @brief Automatically calculates flushing volumes
     * @param filament_idx Specifies the filament index to calculate. -1 indicates all filament indices.
     * @param extruder_id Specifies the extruder id to calculate. -1 indicates all extruders indices.
     */
    void                    auto_calc_flushing_volumes(const int filament_idx = -1, const int extruder_id = -1);
    void                    jump_to_object(ObjectDataViewModelNode* item);
    void                    can_search();
#ifdef _MSW_DARK_MODE
    void                    show_mode_sizer(bool show);
#endif

    std::vector<PlaterPresetComboBox*>&   combos_filament();
    void                                 clear_combos_filament_badge();
    void                                 udpate_combos_filament_badge();
    Search::OptionsSearcher&        get_searcher();
    std::string&                    get_search_line();
    void                            update_printer_thumbnail();

    bool need_auto_sync_after_connect_printer() const { return m_need_auto_sync_after_connect_printer; }
    void set_need_auto_sync_after_connect_printer(bool need_auto_sync) { m_need_auto_sync_after_connect_printer = need_auto_sync; }

private:
    void  auto_calc_flushing_volumes_internal(const int filament_id, const int extruder_id);

private:
    struct priv;
    std::unique_ptr<priv> p;

    wxBoxSizer* m_scrolled_sizer = nullptr;
    bool            m_need_auto_sync_after_connect_printer{false};
};

class Plater: public wxPanel
{
public:
    using fs_path = boost::filesystem::path;

    Plater(wxWindow *parent, MainFrame *main_frame);
    Plater(Plater &&) = delete;
    Plater(const Plater &) = delete;
    Plater &operator=(Plater &&) = delete;
    Plater &operator=(const Plater &) = delete;
    ~Plater() = default;

    bool Show(bool show = true);

    bool is_project_dirty() const;
    bool is_presets_dirty() const;
    void set_plater_dirty(bool is_dirty);
    void update_project_dirty_from_presets();
    int  save_project_if_dirty(const wxString& reason);
    void reset_project_dirty_after_save();
    void reset_project_dirty_initial_presets();
#if ENABLE_PROJECT_DIRTY_STATE_DEBUG_WINDOW
    void render_project_state_debug_window() const;
#endif // ENABLE_PROJECT_DIRTY_STATE_DEBUG_WINDOW

    Sidebar& sidebar();
    const Model& model() const;
    Model& model();
    const Print& fff_print() const;
    Print& fff_print();
    const SLAPrint& sla_print() const;
    SLAPrint& sla_print();

    int new_project(bool skip_confirm = false, bool silent = false, const wxString& project_name = wxString());
    // BBS: save & backup
    void load_project(wxString const & filename = "", wxString const & originfile = "-");
    int save_project(bool saveAs = false);
    //BBS download project by project id
    void import_model_id(wxString download_info);
    void download_project(const wxString& project_id);
    void request_model_download(wxString url);
    void request_download_project(std::string project_id);
    // BBS: check snapshot
    bool up_to_date(bool saved, bool backup);

    bool open_3mf_file(const fs::path &file_path);
    int  get_3mf_file_count(std::vector<fs::path> paths);
    void add_file();
    void add_model(bool imperial_units = false, std::string fname = "");
    void import_zip_archive();
    void import_sl1_archive();
    void extract_config_from_project();
    void load_gcode();
    void load_gcode(const wxString& filename);
    void reload_gcode_from_disk();
    void refresh_print();

    // SoftFever
    void calib_pa(const Calib_Params& params);
    void calib_flowrate(bool is_linear, int pass);
    void calib_temp(const Calib_Params& params);
    void calib_max_vol_speed(const Calib_Params& params);
    void calib_retraction(const Calib_Params& params);
    void calib_VFA(const Calib_Params& params);
    void calib_input_shaping_freq(const Calib_Params& params);
    void calib_input_shaping_damp(const Calib_Params& params);
    void Calib_Cornering(const Calib_Params& params);

    BuildVolume_Type get_build_volume_type() const;

    //BBS: add only gcode mode
    bool is_gcode_3mf() { return m_exported_file; }
    bool only_gcode_mode() { return m_only_gcode; }
    void set_only_gcode(bool only_gcode) { m_only_gcode = only_gcode; }

    //BBS: add only gcode mode
    bool using_exported_file() { return m_exported_file; }
    void set_using_exported_file(bool exported_file) {
        m_exported_file = exported_file;
    }

    bool is_empty_project();
    bool is_multi_extruder_ams_empty();
    // BBS
    bool is_new_project_and_check_state() { return m_new_project_and_check_state; }
    wxString get_project_name();
    void update_all_plate_thumbnails(bool force_update = false);
    void update_obj_preview_thumbnail(ModelObject *, int obj_idx, int vol_idx, std::vector<Slic3r::ColorRGBA> colors, int camera_view_angle_type);
    void invalid_all_plate_thumbnails();
    void force_update_all_plate_thumbnails();

    const VendorProfile::PrinterModel * get_curr_printer_model();
    std::map<std::string, std::string> get_bed_texture_maps();
    bool                               get_enable_wrapping_detection();

    static wxColour get_next_color_for_filament();
    static wxString get_slice_warning_string(GCodeProcessorResult::SliceWarning& warning);

    bool preview_zip_archive(const boost::filesystem::path& archive_path);

    // BBS: restore
    std::vector<size_t> load_files(const std::vector<boost::filesystem::path>& input_files, LoadStrategy strategy = LoadStrategy::LoadModel | LoadStrategy::LoadConfig,  bool ask_multi = false);
    // To be called when providing a list of files to the GUI slic3r on command line.
    std::vector<size_t> load_files(const std::vector<std::string>& input_files, LoadStrategy strategy = LoadStrategy::LoadModel | LoadStrategy::LoadConfig,  bool ask_multi = false);
    // to be called on drag and drop
    bool load_files(const wxArrayString& filenames);

    const wxString& get_last_loaded_gcode() const { return m_last_loaded_gcode; }

    void update(bool conside_update_flag = false, bool force_background_processing_update = false);
    //BBS
    void object_list_changed();

    // Get the worker handling the UI jobs (arrange, fill bed, etc...)
    // Here is an example of starting up an ad-hoc job:
    //    queue_job(
    //        get_ui_job_worker(),
    //        [](Job::Ctl &ctl) {
    //            // Executed in the worker thread
    //
    //            CursorSetterRAII cursor_setter{ctl};
    //            std::string msg = "Running";
    //
    //            ctl.update_status(0, msg);
    //            for (int i = 0; i < 100; i++) {
    //                usleep(100000);
    //                if (ctl.was_canceled()) break;
    //                ctl.update_status(i + 1, msg);
    //            }
    //            ctl.update_status(100, msg);
    //        },
    //        [](bool, std::exception_ptr &e) {
    //            // Executed in UI thread after the work is done
    //
    //            try {
    //                if (e) std::rethrow_exception(e);
    //            } catch (std::exception &e) {
    //                BOOST_LOG_TRIVIAL(error) << e.what();
    //            }
    //            e = nullptr;
    //        });
    // This would result in quick run of the progress indicator notification
    // from 0 to 100. Use replace_job() instead of queue_job() to cancel all
    // pending jobs.
    Worker& get_ui_job_worker();
    const Worker & get_ui_job_worker() const;

    void select_view(const std::string& direction);
    //BBS: add no_slice logic
    void select_view_3D(const std::string& name, bool no_slice = true);

    void reload_paint_after_background_process_apply();
    bool is_preview_shown() const;
    bool is_preview_loaded() const;
    bool is_view3D_shown() const;

    bool are_view3D_labels_shown() const;
    void show_view3D_labels(bool show);

    bool is_view3D_overhang_shown() const;
    void show_view3D_overhang(bool show);

    bool is_sidebar_enabled() const;
    void enable_sidebar(bool enabled);
    bool is_sidebar_collapsed() const;
    void collapse_sidebar(bool collapse);
    Sidebar::DockingState get_sidebar_docking_state() const;

    void reset_window_layout();

    // Called after the Preferences dialog is closed and the program settings are saved.
    // Update the UI based on the current preferences.
    void update_ui_from_settings();

    //BBS
    void select_curr_plate_all();
    void remove_curr_plate_all();

    void select_all();
    void deselect_all();
    void exit_gizmo();
    void remove(size_t obj_idx);
    void reset(bool apply_presets_change = false);
    void reset_with_confirm();
    //BBS: return int for various result
    int close_with_confirm(std::function<bool(bool yes_or_no)> second_check = nullptr); // BBS close project
    //BBS: trigger a restore project event
    void trigger_restore_project(int skip_confirm = 0);
    bool delete_object_from_model(size_t obj_idx, bool refresh_immediately = true); // BBS support refresh immediately
    void delete_all_objects_from_model(); //BBS delete all objects from model
    void set_selected_visible(bool visible);
    void remove_selected();
    void increase_instances(size_t num = 1);
    void decrease_instances(size_t num = 1);
    void set_number_of_copies(/*size_t num*/);
    void fill_bed_with_copies();
    void fill_bed_with_instances();
    bool is_selection_empty() const;
    void scale_selection_to_fit_print_volume();
    void convert_unit(ConversionType conv_type);
    // BBS: replace z with plane_points
    void cut(size_t obj_idx, size_t instance_idx, std::array<Vec3d, 4> plane_points, ModelObjectCutAttributes attributes);

    // BBS: segment model with CGAL
    void segment(size_t obj_idx, size_t instance_idx, double smoothing_alpha=0.5, int segment_number=5);
    void apply_cut_object_to_model(size_t init_obj_idx, const ModelObjectPtrs& cut_objects);
    void merge(size_t obj_idx, std::vector<int> &vol_indeces);

    void send_to_printer(bool isall = false);
    void export_gcode(bool prefer_removable);
    void export_gcode_3mf(bool export_all = false);
    void send_gcode_finish(wxString name);
    void export_core_3mf();
    static TriangleMesh combine_mesh_fff(const ModelObject& mo, int instance_id, std::function<void(const std::string&)> notify_func = {});
    void export_stl(bool extended = false, bool selection_only = false, bool multi_stls = false);
    //BBS: remove amf
    //void export_amf();
    //BBS add extra param for exporting 3mf silence
    // BBS: backup
    int export_3mf(const boost::filesystem::path& output_path = boost::filesystem::path(), SaveStrategy strategy = SaveStrategy::Default, int export_plate_idx = -1, Export3mfProgressFn proFn = nullptr);

    //BBS
    void publish_project();

    void reload_from_disk();
    void replace_with_stl();
    void replace_all_with_stl();
    void reload_all_from_disk();
    bool has_toolpaths_to_export() const;
    void export_toolpaths_to_obj() const;
    void reslice();
    void record_slice_preset(std::string action);
    void reslice_SLA_supports(const ModelObject &object, bool postpone_error_messages = false);
    void reslice_SLA_hollowing(const ModelObject &object, bool postpone_error_messages = false);
    void reslice_SLA_until_step(SLAPrintObjectStep step, const ModelObject &object, bool postpone_error_messages = false);

    void clear_before_change_mesh(int obj_idx);
    void changed_mesh(int obj_idx);

    void changed_object(ModelObject &object);
    void changed_object(int obj_idx);
    void changed_objects(const std::vector<size_t>& object_idxs);
    void schedule_background_process(bool schedule = true);
    bool is_background_process_update_scheduled() const;
    void suppress_background_process(const bool stop_background_process) ;
    /* -1: send current gcode if not specified
     * -2: send all gcode to target machine */
    int send_gcode(int plate_idx = -1, Export3mfProgressFn proFn = nullptr);
    void send_gcode_legacy(int plate_idx = -1, Export3mfProgressFn proFn = nullptr, bool use_3mf = false);
    int export_config_3mf(int plate_idx = -1, Export3mfProgressFn proFn = nullptr);
    //BBS jump to nonitor after print job finished
    void send_calibration_job_finished(wxCommandEvent &evt);
    void print_job_finished(wxCommandEvent &evt);
    void send_job_finished(wxCommandEvent& evt);
    void publish_job_finished(wxCommandEvent& evt);
    void open_platesettings_dialog(wxCommandEvent& evt);
    void open_filament_map_setting_dialog(wxCommandEvent &evt);
    void on_change_color_mode(SimpleEvent& evt);
	void eject_drive();

    void take_snapshot(const std::string &snapshot_name);
    //void take_snapshot(const wxString &snapshot_name);
    void take_snapshot(const std::string &snapshot_name, UndoRedo::SnapshotType snapshot_type);
    //void take_snapshot(const wxString &snapshot_name, UndoRedo::SnapshotType snapshot_type);

    void undo();
    void redo();
    void undo_to(int selection);
    void redo_to(int selection);
    bool undo_redo_string_getter(const bool is_undo, int idx, const char** out_text);
    void undo_redo_topmost_string_getter(const bool is_undo, std::string& out_text);
    int update_print_required_data(Slic3r::DynamicPrintConfig config, Slic3r::Model model, Slic3r::PlateDataPtrs plate_data_list, std::string file_name, std::string file_path);
    bool search_string_getter(int idx, const char** label, const char** tooltip);
    // For the memory statistics.
    const Slic3r::UndoRedo::Stack& undo_redo_stack_main() const;
    void clear_undo_redo_stack_main();
    // Enter / leave the Gizmos specific Undo / Redo stack. To be used by the SLA support point editing gizmo.
    void enter_gizmos_stack();
    // BBS: return false if not changed
    bool leave_gizmos_stack();

    void on_filament_change(size_t filament_idx);
    void on_filament_count_change(size_t extruders_count);
    void on_filaments_delete(size_t extruders_count, size_t filament_id, int replace_filament_id = -1);
    std::vector<Slic3r::ColorRGBA> get_extruders_colors();
    // BBS
    void on_bed_type_change(BedType bed_type);

    bool update_filament_colors_in_full_config();
    void config_change_notification(const DynamicPrintConfig &config, const std::string& key);
    void on_config_change(const DynamicPrintConfig &config);
    void force_filament_colors_update();
    void force_print_bed_update();
    // On activating the parent window.
    void on_activate();
    std::vector<std::string> get_extruder_colors_from_plater_config(const GCodeProcessorResult* const result = nullptr) const;
    std::vector<std::string> get_filament_colors_render_info() const;
    std::vector<std::string> get_filament_color_render_type() const;
    std::vector<std::string> get_colors_for_color_print(const GCodeProcessorResult* const result = nullptr) const;

    void set_global_filament_map_mode(FilamentMapMode mode);
    void set_global_filament_map(const std::vector<int>& filament_map);
    std::vector<int> get_global_filament_map() const;
    FilamentMapMode get_global_filament_map_mode() const;

    void update_menus();
    wxString get_selected_printer_name_in_combox();
    enum class PrinterWarningType {
        NOT_CONNECTED,
        INCONSISTENT,
        UNINSTALL_FILAMENT,
        EMPTY_FILAMENT
    };
    void pop_warning_and_go_to_device_page(wxString printer_name, PrinterWarningType type, const wxString &title);
    bool check_printer_initialized(MachineObject *obj, bool only_warning = false,bool popup_warning = true);
    bool is_same_printer_for_connected_and_selected(bool popup_warning = true);
    // BBS
    //void show_action_buttons(const bool is_ready_to_slice) const;

    wxString get_project_filename(const wxString& extension = wxEmptyString) const;
    wxString get_export_gcode_filename(const wxString& extension = wxEmptyString, bool only_filename = false, bool export_all = false) const;
    void set_project_filename(const wxString& filename);
    void update_print_error_info(int code, std::string msg, std::string extra);

    bool is_export_gcode_scheduled() const;

    const Selection& get_selection() const;
    int get_selected_object_idx();
    bool is_single_full_object_selection() const;
    GLCanvas3D* canvas3D();
    const GLCanvas3D * canvas3D() const;
    GLCanvas3D* get_current_canvas3D(bool exclude_preview = false);
    GLCanvas3D* get_view3D_canvas3D();
    GLCanvas3D* get_preview_canvas3D();
    GLCanvas3D* get_assmeble_canvas3D();
    wxWindow* get_select_machine_dialog();

    void arrange();
    void orient();
    void find_new_position(const ModelInstancePtrs  &instances);
    //BBS: add job state related functions
    void set_prepare_state(int state);
    int get_prepare_state();
    //BBS: add print job releated functions
    void get_print_job_data(PrintPrepareData* data);
    void set_print_job_plate_idx(int plate_idx);

    int get_send_calibration_finished_event();
    int get_print_finished_event();
    int get_send_finished_event();
    int get_publish_finished_event();

    void set_current_canvas_as_dirty();
    void unbind_canvas_event_handlers();
    void reset_canvas_volumes();

    PrinterTechnology   printer_technology() const;
    const DynamicPrintConfig * config() const;
    bool                set_printer_technology(PrinterTechnology printer_technology);

    //BBS
    void cut_selection_to_clipboard();

    void copy_selection_to_clipboard();
    void paste_from_clipboard();
    //BBS: add clone logic
    void clone_selection();
    void center_selection();
    void drop_selection();
    void search(bool plater_is_active, Preset::Type  type, wxWindow *tag, TextInput *etag, wxWindow *stag);
    void mirror(Axis axis);
    void split_object();
    void split_volume();
    void optimize_rotation();
    // find all empty cells on the plate and won't overlap with exclusion areas
    static std::vector<Vec2f> get_empty_cells(const Vec2f step);

    //BBS:
    void fill_color(int extruder_id);

    std::string get_3mf_filename() { return m_3mf_path; };
    bool can_delete() const;
    bool can_delete_all() const;
    bool can_add_model() const;
    bool can_add_plate() const;
    bool can_delete_plate() const;
    bool can_increase_instances() const;
    bool can_decrease_instances() const;
    bool can_set_instance_to_object() const;
    bool can_fix_through_netfabb() const;
    bool can_simplify() const;
    bool can_split_to_objects() const;
    bool can_split_to_volumes() const;
    bool can_arrange() const;
    //BBS
    bool can_cut_to_clipboard() const;
    bool can_layers_editing() const;
    bool can_paste_from_clipboard() const;
    bool can_copy_to_clipboard() const;
    bool can_undo() const;
    bool can_redo() const;
    bool can_reload_from_disk() const;
    bool can_replace_with_stl() const;
    bool can_replace_all_with_stl() const;
    bool can_mirror() const;
    bool can_split(bool to_objects) const;
#if ENABLE_ENHANCED_PRINT_VOLUME_FIT
    bool can_scale_to_print_volume() const;
#endif // ENABLE_ENHANCED_PRINT_VOLUME_FIT

    //BBS:
    bool can_fillcolor() const;
    bool has_assmeble_view() const;

    void msw_rescale();
    void sys_color_changed();

    // BBS
#if 0
    bool init_view_toolbar();
    void enable_view_toolbar(bool enable);
#endif

    bool init_collapse_toolbar();

    const Camera& get_camera() const;
    Camera& get_camera();

    //BBS: partplate list related functions
    PartPlateList& get_partplate_list();
    void validate_current_plate(bool& model_fits, bool& validate_error);
    //BBS: select the plate by index
    int select_plate(int plate_index, bool need_slice = false);
    //BBS: update progress result
    void apply_background_progress();
    //BBS: select the plate by hover_id
    int select_plate_by_hover_id(int hover_id, bool right_click = false, bool isModidyPlateName = false);
    //BBS: delete the plate, index= -1 means the current plate
    int delete_plate(int plate_index = -1);
    int duplicate_plate(int plate_index = -1);
    //BBS: select the sliced plate by index
    int select_sliced_plate(int plate_index);
    //BBS: set bed positions
    void set_bed_position(Vec2d& pos);
    //BBS: is the background process slicing currently
    bool is_background_process_slicing() const;
    //BBS: update slicing context
    void update_slicing_context_to_current_partplate();
    //BBS: show object info
    void show_object_info();
    //BBS
    bool show_publish_dialog(bool show = true);
    //BBS: post process string object exception strings by warning types
    void post_process_string_object_exception(StringObjectException &err);
    void update_objects_position_when_select_preset(const std::function<void()> &select_prest);

    bool check_ams_status(bool is_slice_all);
    // only check sync status and printer model id
    bool get_machine_sync_status();

    void update_machine_sync_status();

#if ENABLE_ENVIRONMENT_MAP
    void init_environment_texture();
    unsigned int get_environment_texture_id() const;
#endif // ENABLE_ENVIRONMENT_MAP

    const BuildVolume& build_volume() const;

    // BBS
    //const GLToolbar& get_view_toolbar() const;
    //GLToolbar& get_view_toolbar();

    const GLToolbar& get_collapse_toolbar() const;
    GLToolbar& get_collapse_toolbar();

    void update_preview_bottom_toolbar();
    void update_preview_moves_slider();
    void enable_preview_moves_slider(bool enable);

#if 0
    void update_partplate();
#endif

    void reset_gcode_toolpaths();
    void reset_last_loaded_gcode() { m_last_loaded_gcode = ""; }

    const Mouse3DController& get_mouse3d_controller() const;
    Mouse3DController& get_mouse3d_controller();

    //BBS: update when switch muilti_extruder printer
    void update_flush_volume_matrix(size_t old_nozzle_size, size_t new_nozzle_size);
    //BBS: add bed exclude area
	void set_bed_shape() const;
    void set_bed_shape(const Pointfs       &shape,
                       const Pointfs       &exclude_area,
                       const Pointfs       &wrapping_exclude_area,
                       const double         printable_height,
                       std::vector<Pointfs> extruder_areas,
                       std::vector<double>  extruder_heights,
                       const std::string   &custom_texture,
                       const std::string   &custom_model,
                       bool                 force_as_custom = false) const;

	const NotificationManager* get_notification_manager() const;
	NotificationManager* get_notification_manager();
    DailyTipsWindow* get_dailytips() const;
    //BBS: show message in status bar
    void show_status_message(std::string s);

    void init_notification_manager();

    void bring_instance_forward();

    bool need_update() const;
    void set_need_update(bool need_update);
    void update_title_dirty_status();

    // ROII wrapper for suppressing the Undo / Redo snapshot to be taken.
	class SuppressSnapshots
	{
	public:
		SuppressSnapshots(Plater *plater) : m_plater(plater)
		{
			m_plater->suppress_snapshots();
		}
		~SuppressSnapshots()
		{
			m_plater->allow_snapshots();
		}
	private:
		Plater *m_plater;
	};

    // RAII wrapper for taking an Undo / Redo snapshot while disabling the snapshot taking by the methods called from inside this snapshot.
	class TakeSnapshot
	{
	public:
        TakeSnapshot(Plater *plater, const std::string &snapshot_name) : m_plater(plater)
        {
			m_plater->take_snapshot(snapshot_name);
			m_plater->suppress_snapshots();
		}
		/*TakeSnapshot(Plater *plater, const wxString &snapshot_name) : m_plater(plater)
		{
			m_plater->take_snapshot(snapshot_name);
			m_plater->suppress_snapshots();
		}*/
        TakeSnapshot(Plater* plater, const std::string& snapshot_name, UndoRedo::SnapshotType snapshot_type) : m_plater(plater)
        {
            m_plater->take_snapshot(snapshot_name, snapshot_type);
            m_plater->suppress_snapshots();
        }
        /*TakeSnapshot(Plater *plater, const wxString &snapshot_name, UndoRedo::SnapshotType snapshot_type) : m_plater(plater)
        {
            m_plater->take_snapshot(snapshot_name, snapshot_type);
            m_plater->suppress_snapshots();
        }*/

		~TakeSnapshot()
		{
			m_plater->allow_snapshots();
		}
	private:
		Plater *m_plater;
	};

    // BBS: limit to single snapshot taking by the methods called from inside
    // this snapshot.
    class SingleSnapshot
    {
    public:
        SingleSnapshot(Plater *plater) : m_plater(plater)
        {
            m_plater->single_snapshots_enter(this);
        }

        ~SingleSnapshot() { m_plater->single_snapshots_leave(this); }

        bool check(bool modify)
        {
            if (token && (this->modify || !modify)) return false;
            token = true;
            this->modify = modify;
            return true;
        }

    private:
        Plater *m_plater;
        bool    token = false;
        bool    modify = false;
    };

    bool inside_snapshot_capture();

    void toggle_render_statistic_dialog();
    bool is_render_statistic_dialog_visible() const;

    void toggle_show_wireframe();
    bool is_show_wireframe() const;
    void enable_wireframe(bool status);
    bool is_wireframe_enabled() const;

	// Wrapper around wxWindow::PopupMenu to suppress error messages popping out while tracking the popup menu.
	bool PopupMenu(wxMenu *menu, const wxPoint& pos = wxDefaultPosition);
    bool PopupMenu(wxMenu *menu, int x, int y) { return this->PopupMenu(menu, wxPoint(x, y)); }

    //BBS: add popup logic for table object
    bool PopupObjectTable(int object_id, int volume_id, const wxPoint& position);
    //BBS: popup selection at default position
    bool PopupObjectTableBySelection();

    // get same Plater/ObjectList menus
    wxMenu* plate_menu();
    wxMenu* object_menu();
    wxMenu* part_menu();
    wxMenu* text_part_menu();
    wxMenu* svg_part_menu();
    wxMenu* sla_object_menu();
    wxMenu* default_menu();
    wxMenu* instance_menu();
    wxMenu* layer_menu();
    wxMenu* multi_selection_menu();
    wxMenu* filament_action_menu(int active_filament_menu_id);
    int     GetPlateIndexByRightMenuInLeftUI();
    void    SetPlateIndexByRightMenuInLeftUI(int);
    static bool has_illegal_filename_characters(const wxString& name);
    static bool has_illegal_filename_characters(const std::string& name);
    static void show_illegal_characters_warning(wxWindow* parent);


    std::string get_preview_only_filename() { return m_preview_only_filename; };

    bool last_arrange_job_is_finished()
    {
        bool prevRunning = false;
        return m_arrange_running.compare_exchange_strong(prevRunning, true);
    };
    std::atomic<bool> m_arrange_running{false};
    void              reset_check_status() { m_check_status = 0; }

    bool is_loading_project() const { return m_loading_project; }

private:
    struct priv;
    std::unique_ptr<priv> p;
    std::string           m_3mf_path;
    // Set true during PopupMenu() tracking to suppress immediate error message boxes.
    // The error messages are collected to m_tracking_popup_menu_error_message instead and these error messages
    // are shown after the pop-up dialog closes.
    bool 	 m_tracking_popup_menu = false;
    wxString m_tracking_popup_menu_error_message;

    wxString m_last_loaded_gcode;
    //BBS: add only gcode mode
    bool m_only_gcode { false };//just for .gcode file not for .gcode.3mf
    bool m_exported_file { false };
    bool skip_thumbnail_invalid { false };
    bool m_loading_project { false };
    bool m_new_project_and_check_state{false};
    std::string m_preview_only_filename;
    int m_valid_plates_count { 0 };
    int m_check_status = 0; // 0 not check, 1 check success, 2 check failed

    void suppress_snapshots();
    void allow_snapshots();
    // BBS: single snapshot
    void single_snapshots_enter(SingleSnapshot *single);
    void single_snapshots_leave(SingleSnapshot *single);
    // BBS: add project slice related functions
    int start_next_slice();

    void _calib_pa_pattern(const Calib_Params& params);
    void _calib_pa_pattern_gen_gcode();
    void _calib_pa_tower(const Calib_Params& params);
    void _calib_pa_select_added_objects();

    void cut_horizontal(size_t obj_idx, size_t instance_idx, double z, ModelObjectCutAttributes attributes);

    void on_filament_map_mode_change();
    friend class SuppressBackgroundProcessingUpdate;
    friend class PlaterDropTarget;
};

class SuppressBackgroundProcessingUpdate
{
public:
    SuppressBackgroundProcessingUpdate();
    ~SuppressBackgroundProcessingUpdate();
private:
    bool m_was_scheduled;
};

std::vector<int> get_min_flush_volumes(const DynamicPrintConfig &full_config, size_t nozzle_id);

Preset *get_printer_preset(const MachineObject *obj);
wxArrayString get_all_camera_view_type();


} // namespace GUI
} // namespace Slic3r

#endif