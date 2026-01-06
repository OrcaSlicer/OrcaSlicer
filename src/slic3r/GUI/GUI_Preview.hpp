#ifndef slic3r_GUI_Preview_hpp_
#define slic3r_GUI_Preview_hpp_

#include <wx/panel.h>

#include "libslic3r/Point.hpp"
#include "libslic3r/CustomGCode.hpp"

//BBS: add print base
#include "libslic3r/PrintBase.hpp"

#include <string>
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include <slic3r/GUI/GCodeViewer.hpp>

class wxGLCanvas;
class wxBoxSizer;
class wxStaticText;
class wxComboBox;
class wxComboCtrl;
class wxCheckBox;

namespace Slic3r {

class DynamicPrintConfig;
class Print;
class BackgroundSlicingProcess;
class Model;

namespace GUI {

class GLCanvas3D;
class GLToolbar;
class Bed3D;
struct Camera;
class Plater;
#ifdef _WIN32
class BitmapComboBox;
#endif

class View3D : public wxPanel
{
    wxGLCanvas* m_canvas_widget;
    GLCanvas3D* m_canvas;

public:
    View3D(wxWindow* parent, Bed3D& bed, Model* model, DynamicPrintConfig* config, BackgroundSlicingProcess* process);
    virtual ~View3D();

    wxGLCanvas* get_wxglcanvas() { return m_canvas_widget; }
    GLCanvas3D* get_canvas3d() { return m_canvas; }

    void set_as_dirty();
    void bed_shape_changed();
    void plates_count_changed();

    void select_view(const std::string& direction);

    //BBS
    void select_curr_plate_all();
    void select_object_from_idx(std::vector<int> &object_idxs);
    void remove_curr_plate_all();

    void select_all();
    void deselect_all();
    void exit_gizmo();
    void delete_selected();
    void center_selected();
    void drop_selected();
    void center_selected_plate(const int plate_idx);
    void mirror_selection(Axis axis);

    bool is_layers_editing_enabled() const;
    bool is_layers_editing_allowed() const;
    void enable_layers_editing(bool enable);

    bool is_dragging() const;
    bool is_reload_delayed() const;

    void reload_scene(bool refresh_immediately, bool force_full_scene_refresh = false);
    void render();

private:
    bool init(wxWindow* parent, Bed3D& bed, Model* model, DynamicPrintConfig* config, BackgroundSlicingProcess* process);
};

class Preview : public wxPanel
{
    wxGLCanvas* m_canvas_widget { nullptr };
    GLCanvas3D* m_canvas { nullptr };
    DynamicPrintConfig* m_config;
    BackgroundSlicingProcess* m_process;
    GCodeProcessorResult* m_gcode_result;

#ifdef __linux__
    // We are getting mysterious crashes on Linux in gtk due to OpenGL context activation GH #1874 #1955.
    // So we are applying a workaround here.
    bool m_volumes_cleanup_required { false };
#endif /* __linux__ */

    // Calling this function object forces Plater::schedule_background_process.
    std::function<void()> m_schedule_background_process;

    unsigned int m_number_extruders { 1 };
    bool m_keep_current_preview_type{ false };

    //bool m_loaded { false };
    //BBS: add logic for preview print
    const Slic3r::PrintBase* m_loaded_print { nullptr };
    //BBS: add only gcode mode
    bool m_only_gcode { false };
    bool m_reload_paint_after_background_process_apply{false};

public:
    enum class OptionType : unsigned int
    {
        Travel,
        Wipe,
        Retractions,
        Unretractions,
        Seams,
        ToolChanges,
        ColorChanges,
        PausePrints,
        CustomGCodes,
        Shells,
        ToolMarker,
        Legend
    };

    Preview(wxWindow* parent, Bed3D& bed, Model* model, DynamicPrintConfig* config, BackgroundSlicingProcess* process, 
        GCodeProcessorResult* gcode_result, std::function<void()> schedule_background_process = []() {});
    virtual ~Preview();

    //BBS: update gcode_result
    void update_gcode_result(GCodeProcessorResult* gcode_result);

    wxGLCanvas* get_wxglcanvas() { return m_canvas_widget; }
    GLCanvas3D* get_canvas3d() { return m_canvas; }

    void set_as_dirty();

    void bed_shape_changed();
    void select_view(const std::string& direction);
    void set_drop_target(wxDropTarget* target);

    //BBS: add only gcode mode
    void load_print(bool keep_z_range = false, bool only_gcode = false);
    void reload_print(bool keep_volumes = false, bool only_gcode = false);
    void refresh_print();
    //BBS: always load shell at preview
    void load_shells(const Print& print, bool force_previewing = false);
    void reset_shells();

    void msw_rescale();
    void sys_color_changed();

    //BBS: add m_loaded_print logic
    bool is_loaded() const { return (m_loaded_print != nullptr); }
    //BBS
    void on_tick_changed(Type type);

    void show_sliders(bool show = true);
    void show_moves_sliders(bool show = true);
    void show_layers_sliders(bool show = true);
    void set_reload_paint_after_background_process_apply(bool flag) { m_reload_paint_after_background_process_apply = flag; }
    bool get_reload_paint_after_background_process_apply() { return m_reload_paint_after_background_process_apply; }

private:
    bool init(wxWindow* parent, Bed3D& bed, Model* model);

    void bind_event_handlers();
    void unbind_event_handlers();
    void on_size(wxSizeEvent& evt);
    // Create/Update/Reset double slider on 3dPreview
    void check_layers_slider_values(std::vector<CustomGCode::Item>& ticks_from_model,
        const std::vector<double>& layers_z);

    void update_layers_slider(const std::vector<double>& layers_z, bool keep_z_range = false);    
    void update_layers_slider_mode();
    void update_layers_slider_from_canvas(wxKeyEvent &event);
    //BBS: add only gcode mode
    void load_print_as_fff(bool keep_z_range = false, bool only_gcode = false);
};


class AssembleView : public wxPanel
{
    wxGLCanvas* m_canvas_widget{ nullptr };
    GLCanvas3D* m_canvas{ nullptr };
public:
    AssembleView(wxWindow* parent, Bed3D& bed, Model* model, DynamicPrintConfig* config, BackgroundSlicingProcess* process);
    ~AssembleView();

    wxGLCanvas* get_wxglcanvas() { return m_canvas_widget; }
    GLCanvas3D* get_canvas3d() { return m_canvas; }

    void set_as_dirty();
    void render();

    bool is_reload_delayed() const;
    void reload_scene(bool refresh_immediately, bool force_full_scene_refresh = false);
    void select_view(const std::string& direction);

private:
    bool init(wxWindow* parent, Bed3D& bed, Model* model, DynamicPrintConfig* config, BackgroundSlicingProcess* process);
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_Preview_hpp_
