#include "NativeCommands.hpp"

#include "calib_dlg.hpp"
#include "Camera.hpp"
#include "DailyTips.hpp"
#include "GCodeViewer.hpp"
#include "GLCanvas3D.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Factories.hpp"
#include "GUI_ObjectList.hpp"
#include "I18N.hpp"
#include "IMSlider.hpp"
#include "MainFrame.hpp"
#include "NetworkTestDialog.hpp"
#include "Plater.hpp"
#include "PluginsDialog.hpp"
#include "PlateSettingsDialog.hpp"
#include "DeviceCore/DevManager.h"

#include <libslic3r/Model.hpp>
#include <libslic3r/Utils.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include <wx/utils.h>

namespace Slic3r { namespace GUI {

namespace {

// Plate ops are an FFF feature: SLA has a single plate and no plate UI, gcode-only mode has no
// editable project - so gate every plate op on FFF + the normal editor.
bool is_fff_plater(Plater* plater) { return plater && plater->printer_technology() == ptFFF && !plater->only_gcode_mode(); }

AppActionRunResult plate_unavailable() { return {AppActionRunResult::Level::Info, _L("Plates are a filament (FFF) feature.")}; }

// Switch to the Prepare (3D) panel so object/calibration ops have a live canvas + selection, and
// the notebook page label matches. A no-op when the 3D panel is already shown.
void ensure_3d_view(Plater* plater)
{
    if (plater && !plater->is_view3D_shown()) {
        plater->select_view_3D("3D");
        if (MainFrame* mf = wxGetApp().mainframe; mf)
            mf->select_tab(TAB_ID_PREPARE);
    }
}

// Object op guard + run: object ops read the Prepare canvas selection, so ensure that view first so
// a launch from another tab doesn't report a spuriously empty selection.
AppActionRunResult object_op(Plater* plater, bool (*ok)(Plater*), void (*op)(Plater*))
{
    if (!plater)
        return {AppActionRunResult::Level::Info, _L("Open the 3D view first.")};
    ensure_3d_view(plater);
    if (!ok(plater))
        return {AppActionRunResult::Level::Info, _L("Select an object first.")};
    op(plater);
    return {AppActionRunResult::Level::Success};
}

// Jump the preview to a layer selected by a 0-100 percent of the layer range. The caller has already
// switched to Preview (which may request a slice); if a slicer result is present the slider is
// repositioned immediately, otherwise the jump is a no-op until the user re-slices.
void go_to_layer(Plater* plater, const std::string& param)
{
    if (!plater)
        return;
    double pct = 50.0;
    try {
        pct = std::stod(param);
    } catch (const std::exception&) {}
    pct = std::clamp(pct, 0.0, 100.0);

    GLCanvas3D* canvas = plater->get_current_canvas3D();
    if (!canvas)
        return;
    GCodeViewer& viewer = canvas->get_gcode_viewer();
    IMSlider* layers    = viewer.get_layers_slider();
    IMSlider* moves     = viewer.get_moves_slider();
    if (!layers || layers->GetMaxValue() <= 0)
        return;

    const double max = double(layers->GetMaxValue());
    const int target = int(std::lround(pct / 100.0 * max));
    layers->SetHigherValue(target);
    if (layers->is_one_layer())
        layers->SetLowerValue(target);
    layers->set_as_dirty();
    if (moves) {
        moves->SetHigherValue(moves->GetMaxValue());
        moves->set_as_dirty();
    }
}

// Select a named camera view. Plater::select_view dispatches to the current panel.
AppActionRunResult view_command(Plater* plater, const std::string& dir)
{
    if (plater)
        plater->select_view(dir);
    return {AppActionRunResult::Level::Success};
}

// Calibration wizards. Routes through MainFrame::run_calibration, the same entry point as the
// Calibration menu (which caches most of the wizard dialogs).
AppActionRunResult calib_command(CalibKind kind)
{
    MainFrame* mf = wxGetApp().mainframe;
    if (!mf)
        return {AppActionRunResult::Level::Info, _L("Open the 3D view first.")};
    ensure_3d_view(wxGetApp().plater());
    mf->run_calibration(kind);
    return {AppActionRunResult::Level::Success};
}

constexpr const char* kCommandPrefix = "orca_command";

// Thin AppAction wrapper for one catalog entry: identity and presentation come from the catalog,
// run() routes back to it. The id is keyed by the stable catalog key (not the display title), so a
// rename or a UI-language switch never re-keys the action.
struct CommandAction : AppAction
{
    std::string command_key;

    AppActionRunResult run(const std::string& param) const override { return NativeCommands::run(command_key, param); }

    explicit CommandAction(const NativeCommand& c)
        : AppAction(AppActionId{AppAction::compose_id(kCommandPrefix, c.key, kOrcaSourceKey)}, c.title, kOrcaSourceKey, kOrcaSourceName)
        , command_key(c.key)
    {
        this->kind  = AppActionKind::Command;
        this->group = c.group;
        this->input = c.input;
        this->icon  = c.icon;
    }
};

std::vector<NativeCommand> build_command_catalog()
{
    std::vector<NativeCommand> out;
    auto add = [&](std::string key, std::string title, std::string group, std::function<AppActionRunResult(const std::string&)> runner,
                   std::string input = {}, std::string icon = {}) {
        out.push_back({std::move(key), std::move(title), std::move(group), std::move(input), std::move(icon), std::move(runner)});
    };
    // Presentation-first overload: keeps the tile icon next to the title/group it belongs to.
    auto add_with_icon = [&](std::string key, std::string title, std::string group, std::string icon,
                             std::function<AppActionRunResult(const std::string&)> runner, std::string input = {}) {
        add(std::move(key), std::move(title), std::move(group), std::move(runner), std::move(input), std::move(icon));
    };

    // ---- Slice & Export ----
    add_with_icon("slice_and_preview", _u8L("Slice and Preview"), _u8L("Slice & Export"), "media_play", [](const std::string&) {
        Plater* plater = wxGetApp().plater();
        if (plater) {
            plater->reslice();
            plater->select_view_3D("Preview", false);
            if (MainFrame* mf = wxGetApp().mainframe; mf)
                mf->select_tab(TAB_ID_PREVIEW);
        }
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    add_with_icon(
        "go_to_layer", _u8L("Go to layer (percent)"), _u8L("Commands"), "height_range_layer",
        [](const std::string& param) {
            Plater* plater = wxGetApp().plater();
            if (plater) {
                plater->select_view_3D("Preview", false);
                if (MainFrame* mf = wxGetApp().mainframe; mf)
                    mf->select_tab(TAB_ID_PREVIEW);
                go_to_layer(plater, param);
            }
            return AppActionRunResult{AppActionRunResult::Level::Success};
        },
        "percent");

    // "go_to_tab" is two-phase: the palette collects the tab after activating it, then hands the tab
    // id back as `param` (same contract as go_to_layer's percent).
    add(
        "go_to_tab", _u8L("Go to tab..."), _u8L("Commands"),
        [](const std::string& param) {
            if (MainFrame* mf = wxGetApp().mainframe; mf && !param.empty())
                mf->select_tab(from_u8(param));
            return AppActionRunResult{AppActionRunResult::Level::Success};
        },
        "tab");

    add_with_icon("load_project", _u8L("Load Project"), _u8L("Commands"), "menu_open", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->load_project();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("save_project", _u8L("Save Project"), _u8L("Commands"), "menu_save", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->save_project(false);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("save_project_as", _u8L("Save Project As"), _u8L("Commands"), "menu_save", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->save_project(true);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("open_preferences", _u8L("Preferences"), _u8L("Commands"), "cog", [](const std::string&) {
        wxGetApp().open_preferences();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    // ---- Mode ----
    add_with_icon("mode_simple", _u8L("Mode: Simple"), _u8L("Mode"), "advanced", [](const std::string&) {
        wxGetApp().set_mode(comSimple);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("mode_advanced", _u8L("Mode: Advanced"), _u8L("Mode"), "advanced", [](const std::string&) {
        wxGetApp().set_mode(comAdvanced);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("mode_expert", _u8L("Mode: Expert"), _u8L("Mode"), "advanced", [](const std::string&) {
        wxGetApp().set_mode(comExpert);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    // Mirrors Preferences > Developer > Developer mode: flip the flag, persist, refresh the UI.
    add_with_icon("toggle_developer_mode", _u8L("Toggle Developer Mode"), _u8L("Mode"), "advanced", [](const std::string&) {
        GUI_App& app = wxGetApp();
        const bool on = !app.app_config->get_bool("developer_mode");
        app.app_config->set_bool("developer_mode", on);
        app.app_config->save();
        app.update_mode();
        return AppActionRunResult{AppActionRunResult::Level::Success, on ? _L("Developer mode enabled.") : _L("Developer mode disabled.")};
    });

    // ---- Export pipeline ----
    add_with_icon("export_gcode", _u8L("Export G-code"), _u8L("Slice & Export"), "menu_export_gcode", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->export_gcode(false);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("export_stl", _u8L("Export STL"), _u8L("Slice & Export"), "menu_export_stl", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->export_stl();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("export_3mf", _u8L("Export 3MF"), _u8L("Slice & Export"), "menu_save", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->export_core_3mf();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("export_sliced_file", _u8L("Export Sliced File"), _u8L("Slice & Export"), "menu_export_sliced_file", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->export_gcode_3mf(false);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("export_all_sliced_file", _u8L("Export All Sliced Files"), _u8L("Slice & Export"), "menu_export_sliced_file", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->export_gcode_3mf(true);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    // ---- Calibration ----
    add_with_icon("calib_temperature", _u8L("Temperature Calibration"), _u8L("Calibration"), "calib_sf",
        [](const std::string&) { return calib_command(CalibKind::Temperature); });
    add_with_icon("calib_max_volumetric", _u8L("Max Volumetric Speed Calibration"), _u8L("Calibration"), "calib_sf",
        [](const std::string&) { return calib_command(CalibKind::MaxVolumetric); });
    add_with_icon("calib_pressure_advance", _u8L("Pressure Advance Calibration"), _u8L("Calibration"), "calib_sf",
        [](const std::string&) { return calib_command(CalibKind::PressureAdvance); });
    add_with_icon("calib_flow_ratio", _u8L("Flow Ratio Calibration"), _u8L("Calibration"), "calib_sf",
        [](const std::string&) { return calib_command(CalibKind::FlowRatio); });
    add_with_icon("calib_retraction", _u8L("Retraction Calibration"), _u8L("Calibration"), "calib_sf",
        [](const std::string&) { return calib_command(CalibKind::Retraction); });
    add_with_icon("calib_cornering", _u8L("Cornering Calibration"), _u8L("Calibration"), "calib_sf",
        [](const std::string&) { return calib_command(CalibKind::Cornering); });
    add_with_icon("calib_input_shaping_freq", _u8L("Input Shaping Frequency Calibration"), _u8L("Calibration"), "calib_sf",
        [](const std::string&) { return calib_command(CalibKind::InputShapingFreq); });
    add_with_icon("calib_input_shaping_damp", _u8L("Input Shaping Damping Calibration"), _u8L("Calibration"), "calib_sf",
        [](const std::string&) { return calib_command(CalibKind::InputShapingDamp); });
    add_with_icon("calib_vfa", _u8L("VFA Calibration"), _u8L("Calibration"), "calib_sf", [](const std::string&) { return calib_command(CalibKind::VFA); });

    // ---- View ----
    for (auto [key, dir, title] :
         std::initializer_list<std::tuple<const char*, const char*, const char*>>{{"view_top", "top", "View: Top"},
                                                                                  {"view_bottom", "bottom", "View: Bottom"},
                                                                                  {"view_front", "front", "View: Front"},
                                                                                  {"view_rear", "rear", "View: Rear"},
                                                                                  {"view_left", "left", "View: Left"},
                                                                                  {"view_right", "right", "View: Right"},
                                                                                  {"view_iso", "iso", "View: Isometric"}}) {
        std::string k = key, d = dir;
        add(k, Slic3r::GUI::I18N::translate_utf8(title), _u8L("View"),
            [d](const std::string&) { return view_command(wxGetApp().plater(), d); });
    }
    add("view_default", _u8L("View: Default"), _u8L("View"), [](const std::string&) {
        Plater* plater = wxGetApp().plater();
        if (plater) {
            plater->select_view("plate");
            if (GLCanvas3D* canvas = plater->get_current_canvas3D())
                canvas->zoom_to_bed();
        }
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("view_fit_bed", _u8L("Fit Bed to View"), _u8L("View"), [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            if (GLCanvas3D* canvas = plater->get_current_canvas3D())
                canvas->zoom_to_bed();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("view_toggle_perspective", _u8L("Toggle Perspective"), _u8L("View"), [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->get_camera().select_next_type();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("reset_window_layout", _u8L("Reset Window Layout"), _u8L("View"), "toolbar_reset", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->reset_window_layout();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    // ---- Object ----
    add_with_icon("obj_delete", _u8L("Delete Selected"), _u8L("Object"), "menu_delete", [](const std::string&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return !p->is_selection_empty(); }, [](Plater* p) { p->remove_selected(); });
    });
    add_with_icon("obj_delete_all", _u8L("Delete All Objects"), _u8L("Object"), "menu_remove", [](const std::string&) {
        return object_op(
            wxGetApp().plater(), [](Plater* p) { return p->can_delete_all(); }, [](Plater* p) { p->delete_all_objects_from_model(); });
    });
    add_with_icon("obj_mirror_x", _u8L("Mirror X"), _u8L("Object"), "menu_mirror_x", [](const std::string&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return p->can_mirror(); }, [](Plater* p) { p->mirror(Axis::X); });
    });
    add_with_icon("obj_mirror_y", _u8L("Mirror Y"), _u8L("Object"), "menu_mirror_y", [](const std::string&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return p->can_mirror(); }, [](Plater* p) { p->mirror(Axis::Y); });
    });
    add_with_icon("obj_mirror_z", _u8L("Mirror Z"), _u8L("Object"), "menu_mirror_z", [](const std::string&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return p->can_mirror(); }, [](Plater* p) { p->mirror(Axis::Z); });
    });
    add_with_icon("obj_split_objects", _u8L("Split to Objects"), _u8L("Object"), "menu_split_objects", [](const std::string&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return p->can_split_to_objects(); }, [](Plater* p) { p->split_object(true); });
    });
    add_with_icon("obj_split_parts", _u8L("Split to Parts"), _u8L("Object"), "menu_split_parts", [](const std::string&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return p->can_split_to_volumes(); }, [](Plater* p) { p->split_volume(); });
    });
    add("obj_center", _u8L("Center Selected on Plate"), _u8L("Object"), [](const std::string&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return !p->is_selection_empty(); }, [](Plater* p) { p->center_selection(); });
    });
    add_with_icon("obj_drop", _u8L("Drop to Bed"), _u8L("Object"), "toolbar_flatten", [](const std::string&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return !p->is_selection_empty(); }, [](Plater* p) { p->drop_selection(); });
    });
    add("obj_fit_volume", _u8L("Scale to Fit Print Volume"), _u8L("Object"), [](const std::string&) {
        return object_op(
            wxGetApp().plater(), [](Plater* p) { return p->can_scale_to_print_volume(); },
            [](Plater* p) { p->scale_selection_to_fit_print_volume(); });
    });
    add_with_icon("obj_instances_up", _u8L("Increase Instances"), _u8L("Object"), "instance_add", [](const std::string&) {
        return object_op(
            wxGetApp().plater(), [](Plater* p) { return p->can_increase_instances(); }, [](Plater* p) { p->increase_instances(); });
    });
    add_with_icon("obj_instances_down", _u8L("Decrease Instances"), _u8L("Object"), "instance_remove", [](const std::string&) {
        return object_op(
            wxGetApp().plater(), [](Plater* p) { return p->can_decrease_instances(); }, [](Plater* p) { p->decrease_instances(); });
    });
    add_with_icon("obj_arrange", _u8L("Auto-Arrange"), _u8L("Object"), "toolbar_arrange", [](const std::string&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return p->can_arrange(); }, [](Plater* p) { p->arrange(); });
    });
    add_with_icon("obj_orient", _u8L("Auto-Orient"), _u8L("Object"), "toolbar_orient", [](const std::string&) {
        return object_op(wxGetApp().plater(), [](Plater* p) { return p->can_arrange(); }, [](Plater* p) { p->orient(); });
    });

    // ---- Add Primitive ---- (the Add > Add Primitive submenu; creates a new object)
    auto add_primitive = [&](std::string key, std::string title, std::string icon, const char* type_name) {
        add_with_icon(std::move(key), std::move(title), _u8L("Add Primitive"), std::move(icon), [type_name](const std::string&) {
            Plater* plater = wxGetApp().plater();
            if (plater) {
                ensure_3d_view(plater);
                if (ObjectList* list = wxGetApp().obj_list())
                    list->load_generic_subobject(type_name, ModelVolumeType::INVALID);
            }
            return AppActionRunResult{AppActionRunResult::Level::Success};
        });
    };
    add_primitive("add_primitive_cube", _u8L("Cube"), "menu_obj_cube", "Cube");
    add_primitive("add_primitive_cylinder", _u8L("Cylinder"), "menu_obj_cylinder", "Cylinder");
    add_primitive("add_primitive_sphere", _u8L("Sphere"), "menu_obj_sphere", "Sphere");
    add_primitive("add_primitive_cone", _u8L("Cone"), "menu_obj_cone", "Cone");
    add_primitive("add_primitive_disc", _u8L("Disc"), "menu_obj_disc", "Disc");
    add_primitive("add_primitive_torus", _u8L("Torus"), "menu_obj_torus", "Torus");
    add_with_icon("add_primitive_text", _u8L("Text"), _u8L("Add Primitive"), "menu_obj_text", [](const std::string&) {
        Plater* plater = wxGetApp().plater();
        if (plater) {
            ensure_3d_view(plater);
            if (GLCanvas3D* canvas = plater->canvas3D())
                canvas->clear_popup_menu_position();
            MenuFactory::add_text_volume(ModelVolumeType::INVALID);
        }
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("add_primitive_svg", _u8L("SVG"), _u8L("Add Primitive"), "menu_obj_svg", [](const std::string&) {
        Plater* plater = wxGetApp().plater();
        if (plater) {
            ensure_3d_view(plater);
            if (GLCanvas3D* canvas = plater->canvas3D())
                canvas->clear_popup_menu_position();
            MenuFactory::add_svg_volume(ModelVolumeType::INVALID);
        }
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    // ---- Add Handy models ---- (the Add > Add Handy models submenu)
    const std::vector<MenuFactory::HandyModel>& handy = MenuFactory::handy_models();
    for (std::size_t i = 0; i < handy.size(); ++i) {
        add("add_handy_" + std::string(handy[i].key), Slic3r::GUI::I18N::translate_utf8(handy[i].label), _u8L("Add Handy models"),
            [i](const std::string&) {
                if (Plater* plater = wxGetApp().plater())
                    ensure_3d_view(plater);
                MenuFactory::load_handy_model(i);
                return AppActionRunResult{AppActionRunResult::Level::Success};
            });
    }

    // ---- Plate ----
    add_with_icon("plate_add", _u8L("Add Plate"), _u8L("Plate"), "toolbar_add_plate", [](const std::string&) {
        Plater* plater = wxGetApp().plater();
        if (!is_fff_plater(plater))
            return plate_unavailable();
        if (!plater->can_add_plate())
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("Cannot add another plate (maximum reached).")};
        plater->add_plate();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("plate_duplicate", _u8L("Duplicate Plate"), _u8L("Plate"), "menu_copy", [](const std::string&) {
        Plater* plater = wxGetApp().plater();
        if (!is_fff_plater(plater))
            return plate_unavailable();
        if (!plater->can_add_plate())
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("Cannot duplicate a plate (maximum reached).")};
        plater->duplicate_plate();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("plate_delete", _u8L("Delete Plate"), _u8L("Plate"), "menu_delete", [](const std::string&) {
        Plater* plater = wxGetApp().plater();
        if (!is_fff_plater(plater))
            return plate_unavailable();
        if (!plater->can_delete_plate())
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("Cannot delete the only plate.")};
        plater->delete_plate();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("plate_rename", _u8L("Rename Plate"), _u8L("Plate"), "plate_name_edit", [](const std::string&) {
        Plater* plater = wxGetApp().plater();
        if (!is_fff_plater(plater))
            return plate_unavailable();
        PartPlate* curr = plater->get_partplate_list().get_curr_plate();
        PlateNameEditDialog dlg((wxWindow*) wxGetApp().mainframe, wxID_ANY, _L("Edit Plate Name"));
        dlg.set_plate_name(from_u8(curr->get_plate_name()));
        if (dlg.ShowModal() == wxID_YES)
            curr->set_plate_name(dlg.get_plate_name().ToUTF8().data());
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("plate_toggle_lock", _u8L("Toggle Plate Lock"), _u8L("Plate"), "lock_normal", [](const std::string&) {
        Plater* plater = wxGetApp().plater();
        if (!is_fff_plater(plater))
            return plate_unavailable();
        PartPlateList& plates = plater->get_partplate_list();
        const int index       = plates.get_curr_plate_index();
        plater->take_snapshot("lock partplate");
        plates.lock_plate(index, !plates.is_locked(index));
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("plate_goto", _u8L("Go to Plate"), _u8L("Plate"), "go_next_plate", [](const std::string& param) {
        Plater* plater = wxGetApp().plater();
        if (!is_fff_plater(plater))
            return plate_unavailable();
        PartPlateList& plates = plater->get_partplate_list();
        const int count       = plates.get_plate_count();
        if (count <= 0)
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("No plates available.")};
        int index = 0;
        try {
            index = std::stoi(param);
        } catch (const std::exception&) {}
        index = std::clamp(index, 0, count - 1);
        plater->select_plate(index, false);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    // ---- Printer / device connection ----
    add_with_icon("sync_ams", _u8L("Synchronize Filament List from AMS"), _u8L("Printer"), "ams_fila_sync", [](const std::string&) {
        Plater* plater     = wxGetApp().plater();
        DeviceManager* dev = wxGetApp().getDeviceManager();
        if (dev && dev->get_selected_machine() && plater) {
            plater->sidebar().sync_ams_list();
            return AppActionRunResult{AppActionRunResult::Level::Success};
        }
        return AppActionRunResult{AppActionRunResult::Level::Info, _L("Connect a printer to synchronize the AMS filament list.")};
    });

    // ---- Presets / cloud ----
    add_with_icon("preset_bundle", _u8L("Open Preset Bundle"), _u8L("Presets"), "menu_edit_preset", [](const std::string&) {
        wxGetApp().open_presetbundledialog();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("sync_presets", _u8L("Sync Presets"), _u8L("Presets"), "printer_sync_ok", [](const std::string&) {
        if (!wxGetApp().is_user_login())
            return AppActionRunResult{AppActionRunResult::Level::Info, _L("Sign in to sync presets.")};
        wxGetApp().restart_sync_user_preset();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    // ---- Import ----
    add_with_icon("import_file", _u8L("Import 3MF/STL/STEP/SVG/OBJ/AMF"), _u8L("Import"), "menu_import", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater()) {
#ifdef __APPLE__
            plater->add_model();
#else
                plater->add_file();
#endif
        }
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("import_zip_archive", _u8L("Import ZIP Archive"), _u8L("Import"), "menu_import", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->import_zip_archive();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("import_configs", _u8L("Import Configs"), _u8L("Import"), "menu_import", [](const std::string&) {
        if (MainFrame* mf = wxGetApp().mainframe)
            mf->load_config_file();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    // ---- Export extras ----
    add_with_icon("export_stl_multi", _u8L("Export All Objects as STLs"), _u8L("Export"), "menu_export_stl", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->export_stl(false, false, true);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("export_drc_single", _u8L("Export All Objects as DRC (one file)"), _u8L("Export"), "menu_export_stl", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->export_stl(false, false, false, FT_DRC);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("export_drc_multi", _u8L("Export All Objects as DRCs"), _u8L("Export"), "menu_export_stl", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->export_stl(false, false, true, FT_DRC);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("export_toolpaths_obj", _u8L("Export Toolpaths as OBJ"), _u8L("Export"), "menu_export_toolpaths", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater())
            plater->export_toolpaths_to_obj();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("export_config", _u8L("Export Preset Bundle"), _u8L("Export"), "menu_export_config", [](const std::string&) {
        if (MainFrame* mf = wxGetApp().mainframe)
            mf->export_config();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    // ---- Help ---- (mirrors the top-bar Help menu, plus the wiki/YouTube links)
    add("help_keyboard_shortcuts", _u8L("Keyboard Shortcuts"), _u8L("Help"), [](const std::string&) {
        wxGetApp().keyboard_shortcuts();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("help_setup_wizard", _u8L("Setup Wizard"), _u8L("Help"), [](const std::string&) {
        wxGetApp().ShowUserGuide();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("help_open_config_folder", _u8L("Show Configuration Folder"), _u8L("Help"), "folder-closed", [](const std::string&) {
        Slic3r::GUI::desktop_open_datadir_folder();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("help_troubleshoot", _u8L("Troubleshoot Center"), _u8L("Help"), [](const std::string&) {
        wxGetApp().troubleshoot();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("help_network_test", _u8L("Open Network Test"), _u8L("Help"), [](const std::string&) {
        NetworkTestDialog dlg(wxGetApp().mainframe);
        dlg.ShowModal();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("help_tip_of_the_day", _u8L("Show Tip of the Day"), _u8L("Help"), "info", [](const std::string&) {
        if (Plater* plater = wxGetApp().plater()) {
            plater->get_dailytips()->open();
            if (GLCanvas3D* canvas = plater->get_current_canvas3D())
                canvas->set_as_dirty();
        }
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("help_check_updates", _u8L("Check for Updates"), _u8L("Help"), "ams_refresh_normal", [](const std::string&) {
        wxGetApp().check_new_version_sf(true, 1);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("help_about", _u8L("About OrcaSlicer"), _u8L("Help"), "OrcaSlicer_about", [](const std::string&) {
        Slic3r::GUI::about();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add_with_icon("open_wiki", _u8L("Open Wiki"), _u8L("Help"), "link_wiki_img", [](const std::string&) {
        wxLaunchDefaultBrowser("https://www.orcaslicer.com/wiki/", wxBROWSER_NEW_WINDOW);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("open_youtube", _u8L("Open YouTube Channel"), _u8L("Help"), [](const std::string&) {
        wxLaunchDefaultBrowser("https://www.youtube.com/@OfficialOrcaSlicer/videos", wxBROWSER_NEW_WINDOW);
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    // ---- Plugins ----
    add("open_plugins", _u8L("Open Plugins"), _u8L("Plugins"), [](const std::string&) {
        wxGetApp().open_plugins_dialog();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("refresh_plugins", _u8L("Refresh Plugins"), _u8L("Plugins"), [](const std::string&) {
        wxGetApp().refresh_plugins();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("install_plugin", _u8L("Install Plugin"), _u8L("Plugins"), [](const std::string&) {
        open_plugin_hub();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });
    add("install_local_plugin", _u8L("Install Local Plugin"), _u8L("Plugins"), [](const std::string&) {
        wxGetApp().install_local_plugin();
        return AppActionRunResult{AppActionRunResult::Level::Success};
    });

    return out;
}

} // namespace

const std::vector<NativeCommand>& NativeCommands::catalog()
{
    static const std::vector<NativeCommand> commands = build_command_catalog();
    return commands;
}

std::unique_ptr<AppAction> NativeCommands::make_action(const NativeCommand& command)
{
    return std::make_unique<CommandAction>(command);
}

AppActionRunResult NativeCommands::run(const std::string& key, const std::string& param)
{
    GUI_App& app = wxGetApp();
    if (app.is_closing())
        return {};
    for (const NativeCommand& c : catalog())
        if (c.key == key)
            return c.runner(param);
    return {AppActionRunResult::Level::Info, _L("Unknown command.")};
}

}} // namespace Slic3r::GUI
