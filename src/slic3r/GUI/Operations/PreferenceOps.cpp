#include "PreferenceOps.hpp"

#include "Outcome.hpp"

#include "slic3r/GUI/Event.hpp"
#include "slic3r/GUI/GCodeViewer.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_Init.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Mcp/McpEndpoint.hpp"
#include "slic3r/GUI/Mcp/McpOptions.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Preferences.hpp"
#include "slic3r/GUI/WebViewDialog.hpp"
#include "slic3r/plugin/host/PluginPages.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Format/DRC.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/lexical_cast.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <ios>
#include <locale>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <wx/event.h>
#include <wx/filefn.h>
#include <wx/filename.h>
#include <wx/string.h>
#include <wx/toplevel.h>
#include <wx/window.h>

namespace Slic3r { namespace GUI { namespace Ops {

using nlohmann::json;

namespace {

constexpr const char* kGeneral   = "general";
constexpr const char* kFiles     = "files";
constexpr const char* kBehaviour = "behaviour";
constexpr const char* kControl   = "control";
constexpr const char* kGraphics  = "graphics";
constexpr const char* kOnline    = "online";
constexpr const char* kDeveloper = "developer";

using Lines   = std::vector<std::string>;
using Choices = std::vector<std::pair<std::string, std::string>>;

// Adds the table's rows, each under the tab and section named last.
class TableWriter
{
public:
    explicit TableWriter(std::vector<Preference>& table) : m_table(table) {}

    void section(const char* tab, const char* title)
    {
        m_tab     = tab;
        m_section = title;
    }
    Preference& checkbox(const char* key, const char* label, Lines tooltip = {})
    {
        Preference& p = add(key, label, PreferenceKind::Checkbox, std::move(tooltip));
        p.fallback    = "false"; // AppConfig::get_bool() of nothing stored
        return p;
    }
    // The dialog shows the first entry where nothing is stored.
    Preference& choice(const char* key, const char* label, Choices choices, Lines tooltip = {})
    {
        Preference& p = add(key, label, PreferenceKind::Choice, std::move(tooltip));
        p.fallback    = choices.empty() ? std::string() : choices.front().first;
        p.choices     = std::move(choices);
        return p;
    }
    Preference& number(const char* key, const char* label, std::optional<double> min, std::optional<double> max, Lines tooltip = {})
    {
        Preference& p = add(key, label, PreferenceKind::Number, std::move(tooltip));
        p.min         = min;
        p.max         = max;
        return p;
    }
    Preference& text(const char* key, const char* label, Lines tooltip = {})
    {
        return add(key, label, PreferenceKind::Text, std::move(tooltip));
    }

private:
    Preference& add(const char* key, const char* label, PreferenceKind kind, Lines tooltip)
    {
        Preference p;
        p.key     = key;
        p.tab     = m_tab;
        p.section = m_section;
        p.label   = label;
        p.kind    = kind;
        p.tooltip = std::move(tooltip);
        m_table.push_back(std::move(p));
        return m_table.back();
    }

    std::vector<Preference>& m_table;
    std::string              m_tab, m_section;
};

bool cad_feature_on() { return wxGetApp().is_enable_cad_feature(); }

#ifdef _WIN32
// The Microsoft Store package declares its file associations in its manifest, so the dialog shows a link to Windows'
// settings in place of the checkboxes.
bool not_in_store_package() { return !is_running_in_msix(); }
#endif

std::vector<Preference> build_table()
{
    std::vector<Preference> table;
    TableWriter             t(table);

    //// GENERAL > Settings
    t.section(kGeneral, L("Settings"));
    Preference& language = t.choice("language", L("Language"), {});
    // TRN %1% is the key of a setting of the Preferences dialog; the sentence says why a script may not change it.
    language.refused     = L("%1%: changing the language restarts OrcaSlicer.");
    language.own_row     = true; // the languages are the translations installed
    // The dialog's labels add the units to the translated names.
    t.choice("use_inches", L("Units"), {{"0", "Metric (mm, g)"}, {"1", "Imperial (in, oz)"}}).own_row = true;
    t.choice("default_page", L("Default page"), {{"0", L("Home")}, {"1", L("Prepare")}}, {L("Set the page opened on startup.")});
#ifdef _WIN32
    Preference& dark_mode = t.checkbox("dark_color_mode", L("Enable dark Mode"));
    dark_mode.fallback    = "0"; // stored as 1 or 0
    dark_mode.own_row     = true;
#endif
    t.checkbox("single_instance", L("Allow only one OrcaSlicer instance"),
#if __APPLE__
               {L("On OSX there is always only one instance of app running by default. However it is allowed to run multiple instances "
                  "of same app from the command line. In such case this settings will allow only one instance.")});
#else
               {L("If this is enabled, when starting OrcaSlicer and another instance of the same OrcaSlicer is already running, that "
                  "instance will be reactivated instead.")});
#endif
    t.checkbox("show_splash_screen", L("Show splash screen"), {L("Show the splash screen during startup.")});
#ifdef __linux__
    t.checkbox("window_buttons_on_left", L("Use window buttons on left side")).note = L("(Requires restart)");
    table.back().restart_needed = true;
    t.checkbox("prefer_discrete_gpu",
               // TRN "GPU" is the graphics card; the option only matters on computers that have two of them.
               L("Render on the discrete GPU"),
               // TRN DRI_PRIME is an environment variable name and must stay as it is.
               {L("On a computer with two GPUs, run OrcaSlicer on the discrete GPU instead of the one that drives the display. "
                  "Some Wayland systems cannot show frames rendered on the other GPU, or fail to open the window at all. "
                  "Turn this off if the 3D view stays empty; if OrcaSlicer no longer starts, start it once with DRI_PRIME=0 "
                  "in the environment to get back here.")})
        .note = L("(Requires restart)");
    table.back().restart_needed = true;
#endif

    //// GENERAL > Features
    t.section(kGeneral, L("Features"));
    t.checkbox("enable_multi_machine", L("Multi device management"),
               {L("With this option enabled, you can send a task to multiple devices at the same time and manage multiple devices.")})
        .note = L("(Requires restart)");
    table.back().restart_needed = true;
#ifdef SLIC3R_CAD
    t.checkbox("enable_cad_feature", L("CAD feature (experimental)"),
               {L("With this option enabled, the Design tab is shown, where models can be built and edited "
                  "parametrically. This feature is experimental and still under development.")})
        .note = L("(Requires restart)");
    table.back().restart_needed = true;
#endif

    //// GENERAL > AI features
    t.section(kGeneral, L("AI features"));
    Preference& mcp = t.checkbox("mcp_enabled", L("MCP server"),
                                 {L("Run a local server that AI agents connect to through the Model Context Protocol (MCP).")});
    mcp.own_row     = true; // checked while the server runs, and it turns the server on and off
    // TRN %1% is the key of a setting of the Preferences dialog; the sentence says why a script may not change it.
    mcp.refused     = L("%1%: turning the MCP server off ends the connection of the agents that use it.");
    Preference& port = t.number("mcp_port", L("Server port"), 1024, 65535, {L("Local port of the MCP server.")});
    port.own_row     = true; // it shows the port the server is on, and moves the server
    // TRN %1% is the key of a setting of the Preferences dialog; the sentence says why a script may not change it.
    port.refused     = L("%1%: changing the port moves the MCP server the agents connect to.");
    // The switch that lets Python scripts run unrestricted is changed only in its own window, hidden from automation, so
    // its row has no control named after a key and is not in this table.

    //// FILES > Project
    t.section(kFiles, L("Project"));
    t.choice(SETTING_PROJECT_LOAD_BEHAVIOUR, L("Load behavior"),
             {{OPTION_PROJECT_LOAD_BEHAVIOUR_LOAD_ALL, L("Load All")},
              {OPTION_PROJECT_LOAD_BEHAVIOUR_ASK_WHEN_RELEVANT, L("Ask When Relevant")},
              {OPTION_PROJECT_LOAD_BEHAVIOUR_ALWAYS_ASK, L("Always Ask")},
              {OPTION_PROJECT_LOAD_BEHAVIOUR_LOAD_GEOMETRY, L("Load Geometry Only")}},
             {L("Should printer/filament/process settings be loaded when opening a 3MF file?")});
    // The checkbox and the period's field share a row.
    t.checkbox("backup_switch", L("Auto backup"), {L("Backup your project periodically to help with restoring from an occasional crash.")})
        .own_row = true;
    Preference& backup_interval = t.number("backup_interval", L("Auto backup"), 0, std::nullopt, {L("The period of backup in seconds.")});
    backup_interval.unit        = L("sec");
    backup_interval.own_row     = true;
    t.checkbox("export_sources_full_pathnames", L("Store full source file paths in projects"),
               {L("If enabled, saved projects store the absolute path to imported source files (STEP/STL/...), so "
                  "\"Reload from disk\" still works when the source file is kept in a different folder than the project. "
                  "If disabled, only the filename is stored, which keeps projects portable and avoids embedding absolute paths.")});

    //// FILES > Recent files
    t.section(kFiles, L("Recent files"));
    t.number("max_recent_count", L("Maximum recent files"), 0, std::nullopt, {L("Maximum count of recent files")}).own_row = true;
    t.checkbox("recent_models", L("Add STL/STEP files to recent files list"));

    //// FILES > Folder paths
    t.section(kFiles, L("Folder paths"));
    Preference& downloads = t.text("download_path", L("Downloads folder"), {L("Target folder for downloaded items")});
    downloads.own_row     = true; // a folder picker
    downloads.folder      = true;

    //// FILES > File import
    t.section(kFiles, L("File import"));
    t.checkbox("import_names_cleanup_show_dialog", L("Ask how to handle unsupported characters in imported names"),
               {L("Show the removal or replacement choices when original names contain unsupported characters.")});

    //// FILES > 3MF import
    t.section(kFiles, L("3MF import"));
    t.checkbox("import_names_3mf_show_dialog", L("Show options when importing"),
               {L("Show original-name and sorting options when importing a 3MF without project settings into the current project.")});
    t.checkbox("import_names_3mf_keep", L("Keep original names"),
               {L("Use the body names stored in the imported file when it contains them.")});
    t.checkbox("no_warn_when_modified_gcodes", L("Don't warn when loading 3MF with modified G-code"));

    //// FILES > STEP import
    t.section(kFiles, L("STEP import"));
    t.checkbox("enable_step_mesh_setting", L("Show options when importing"),
               {L("If enabled, a parameter settings dialog will appear during STEP file import.")})
        .wiki = "import_export#dont-show-again";
    Preference& linear = t.number("linear_deflection", L("Linear deflection"), 0.001, 0.1,
                                  {L("Linear deflection used when meshing imported STEP files.\nSmaller values produce higher-quality "
                                     "meshes but increase processing time.\nUsed as the default in the import dialog, or directly when "
                                     "the import dialog is disabled.\nDefault: 0.003 mm.")});
    linear.unit     = "mm";
    linear.decimals = 3;
    t.number("angle_deflection", L("Angle deflection"), 0.01, 1.0,
             {L("Angle deflection used when meshing imported STEP files.\nSmaller values produce higher-quality meshes but increase "
                "processing time.\nUsed as the default in the import dialog, or directly when the import dialog is disabled.\nDefault: "
                "0.5.")})
        .decimals = 2;
    t.checkbox("is_split_compound", L("Split into multiple objects"),
               {L("If enabled, compound and compsolid shapes in imported STEP files are split into multiple objects.\nUsed as the default "
                  "in the import dialog, or directly when the import dialog is disabled.\nDefault: enabled.")});
    t.checkbox("import_names_step_keep", L("Keep original names"),
               {L("Use the body names stored in the imported file when it contains them.")});

    //// FILES > AMF import
    t.section(kFiles, L("AMF import"));
    t.checkbox("import_names_amf_show_dialog", L("Show options when importing"),
               {L("Show body grouping, original-name and sorting options when importing geometry into the current project.")});
    t.checkbox("import_names_amf_keep", L("Keep original names"),
               {L("Use the body names stored in the imported file when it contains them.")});

    //// FILES > DRC export
    t.section(kFiles, L("DRC export"));
    Preference& drc = t.number("drc_bits", L("Quality level for Draco export"), DRC_BITS_MIN, DRC_BITS_MAX,
                               {L("Controls the quantization bit depth used when compressing the mesh to Draco format.\n"
                                  "0 = lossless compression (geometry is preserved at full precision). "
                                  "Valid lossy values range from 8 to 30.\n"
                                  "Lower values produce smaller files but lose more geometric detail; "
                                  "higher values preserve more detail at the cost of larger files.")});
    drc.unit = L("bits");
    drc.wiki = "import_export#drc";

#ifdef _WIN32
    //// FILES > Extensions
    t.section(kFiles, L("Associate files to OrcaSlicer"));
    const auto association = [&t](const char* key, const char* label, const char* tooltip) {
        Preference& p = t.checkbox(key, label, {tooltip});
        // TRN %1% is the key of a setting of the Preferences dialog; the sentence says why a script may not change it.
        p.refused     = L("%1%: a file association is a setting of Windows.");
        p.shown       = not_in_store_package;
    };
    association("associate_3mf", L("Associate 3MF files to OrcaSlicer"),
                L("If enabled, this sets OrcaSlicer as the default application to open 3MF files."));
    association("associate_drc", L("Associate DRC files to OrcaSlicer"),
                L("If enabled, sets OrcaSlicer as default application to open DRC files."));
    association("associate_stl", L("Associate STL files to OrcaSlicer"),
                L("If enabled, this sets OrcaSlicer as the default application to open STL files."));
    association("associate_step", L("Associate STEP files to OrcaSlicer"),
                L("If enabled, this sets OrcaSlicer as the default application to open STEP files."));
#endif

    //// BEHAVIOUR > Prepare
    t.section(kBehaviour, L("Prepare"));
    t.choice("auto_calculate_flush", L("Auto flush after changing..."),
             {{"all", L("All")}, {"color change", L("Color")}, {"disabled", L("None")}},
             {L("Auto calculate flushing volumes when selected values changed")});
    t.checkbox("auto_arrange", L("Auto arrange plate after cloning"));
    // The checkbox and the delay's field share a row.
    t.checkbox("auto_slice_after_change", L("Auto slice after changes"),
               {L("If enabled, OrcaSlicer will re-slice automatically whenever slicing-related settings change.")})
        .own_row = true;
    Preference& delay = t.number("auto_slice_change_delay_seconds", L("Auto slice after changes"), 0, std::nullopt,
                                 {L("Delay in seconds before auto slicing starts, allowing multiple edits to be grouped. Use 0 to slice "
                                    "immediately.")});
    delay.unit    = L("sec");
    delay.own_row = true;
    t.checkbox("enable_primitive_size_dialog", L("Show options when adding a primitive"),
               {L("If enabled, a dialog asks for the dimensions of a primitive shape before it is added.")});
    t.checkbox("enable_high_low_temp_mixed_printing", L("Remove mixed temperature restriction"),
               {L("With this option enabled, you can print materials with a large temperature difference together.")})
        // TRN %1% is the key of a setting of the Preferences dialog; the sentence says why a script may not change it.
        .refused = L("%1%: turning it on asks to confirm a warning first.");
    t.checkbox("remember_print_action", L("Remember last print action"),
               {L("If enabled, OrcaSlicer will remember the last selected option in the print button's dropdown (e.g. Print, Export plate "
                  "sliced file, Export G-code file) and use it as the default on next startup.")});

    //// BEHAVIOUR > Preview
    t.section(kBehaviour, L("Preview"));
    // ORCA: view type the preview opens with
    t.choice("preview_default_view_type", L("Default view type"), GCodeViewer::default_view_type_choices(),
             {L("The color scheme the sliced preview opens with.\n"
                "Automatic: Filament for multi material prints, Line Type for single material ones.\n"
                "Last used: the view type you selected last.\n"
                "Any other value always opens that view type.\n"
                "You can still switch the view type in the preview afterwards.")});
    t.choice("preview_reduced_detail_mode", L("Simplify preview while dragging"),
             {{"off", L("Off")}, {"solid", L("Solid model")}, {"shell", L("Shell only")}},
             {L("What the sliced preview draws while you drag the camera or a preview slider, or zoom with the mouse wheel, so that large "
                "prints stay responsive. "
                "The full toolpaths are restored as soon as you let go.\n"
                "Off: the full toolpaths.\n"
                "Solid model: the sliced objects and the prime tower as solid shapes in the colors of the current view, cut to the visible "
                "layer range, "
                "with its bottom and top layers and the supports drawn as toolpaths. Negative volumes are not cut out. "
                "A print made object by object, or one on a belt printer, keeps its full toolpaths.\n"
                "Shell only: every layer without its sparse infill, internal solid infill, internal bridges and gap fill, which lie under "
                "the walls and skins. "
                "Walls, top and bottom surfaces, bridges, supports and the prime tower are drawn whole, so the print looks the same from "
                "outside.\n"
                "The bottom and top layers of the visible range are always drawn whole; a layer with no infill of its own, as with infill "
                "combination, takes the layers down to the nearest one with infill along.")});
    t.checkbox("preview_dim_previous_layers", L("Dim lower layers"),
               {L("When scrubbing the layer slider in the sliced preview, render the layers below the current one darkened so that only "
                  "the layer being viewed is shown at full brightness.")});
    t.number("preview_dim_previous_layers_brightness", L("Dimmed layer brightness"), 0, 99,
             {L("How brightly the dimmed layers are rendered when \"Dim lower layers\" is enabled.\n"
                "99% is barely darkened, 0% renders them black. Capped at 99% because 100% would be the same as disabling the option.")})
        .unit = L("%");

    //// BEHAVIOUR > Preset
    t.section(kBehaviour, L("Preset"));
    t.checkbox("remember_printer_config", L("Remember printer configuration"),
               {L("If enabled, Orca will remember and switch filament/process configuration for each printer automatically.")});
    t.choice("group_filament_presets", L("Group user filament presets"),
             {{"0", L("All")}, {"1", L("None")}, {"2", L("By type")}, {"3", L("By vendor")}},
             {L("Group user filament presets based on selection")});
    t.number("filaments_area_preferred_count", L("Optimize filaments area height for..."), 8, 99,
             {L("Optimizes filament area maximum height by chosen filament count.")})
        .unit = L("filaments");
    t.checkbox("show_shared_profiles_notification", L("Show shared profiles notification"),
               {L("Show a notification with a link to browse shared profiles when the selected printer is changed.")});

    //// BEHAVIOUR > Plugins
    t.section(kBehaviour, L("Plugins"));
    t.number(SETTING_PLUGIN_PAGES_VISIBLE_COUNT, L("Visible plugin pages"), PLUGIN_PAGES_VISIBLE_COUNT_MIN, PLUGIN_PAGES_VISIBLE_COUNT_MAX,
             {L("Number of plugin pages shown as fixed tabs before the remaining pages collapse into a dropdown on the last tab.")})
        .unit = L("pages");

#ifdef SLIC3R_CAD
    //// BEHAVIOUR > Design
    // Design-tab only, so it stays out of the way while the CAD feature is switched off.
    t.section(kBehaviour, L("Design"));
    t.checkbox("design_connector_face_glyph", L("Draw mate connectors as a face"),
               {L("In the Design tab, draw a mate connector as a small face instead of the conventional "
                  "disc with a roll quadrant. A face shows its orientation at a glance, without learning the disc convention. "
                  "Turn this off for the conventional CAD representation.")})
        .shown = cad_feature_on;
    // Saved WITH each design (it decides which loops are closed, i.e. what solid a project
    // rebuilds into), so it applies to designs started from now on; an open design keeps
    // the rule it was made with.
    t.checkbox("auto_close_sketch_loops", L("Auto-close sketch loops"),
               {L("Treat sketch endpoints within 0.001 mm as one joint and weld the loop shut. "
                  "Off: only exactly coincident endpoints join, so a loop with a tiny gap is "
                  "shown as open instead of being closed for you. Saved with each design; "
                  "applies to designs started after the change.")})
        .shown = cad_feature_on;
#endif

    //// BEHAVIOUR > Speed dial
    t.section(kBehaviour, L("Speed dial"));
    t.checkbox("enable_speed_dial", L("Open the Speed Dial from the keyboard"),
               {L("When enabled, the Speed Dial keyboard shortcut (Space by default) opens the action search from any page.")});
    t.number(SETTING_SPEED_DIAL_RECENT_COUNT, L("Recent actions"), SPEED_DIAL_RECENT_COUNT_MIN, SPEED_DIAL_RECENT_COUNT_MAX,
             {L("How many recently launched actions to show at the top of the Speed Dial. Set to 0 to hide recent actions.")})
        .unit =
        // TRN Unit shown after the number of recent actions, as in "5 actions".
        L("actions");

    //// CONTROL > Camera
    t.section(kControl, L("Camera"));
    t.choice("camera_navigation_style", L("Camera style"), {{"0", L("Default")}, {"1", L("Touchpad")}},
             {L("Select camera navigation style.\nDefault: LMB+move for rotation, RMB/MMB+move for panning.\nTouchpad: Alt+move for "
                "rotation, Shift+move for panning.")});
    Preference& orbit = t.number("camera_orbit_mult", L("Orbit speed multiplier"), 0.05, 2.0,
                                 {L("Multiplies the orbit speed for finer or coarser camera movement.")});
    orbit.decimals    = 2;
    orbit.own_row     = true;
    t.checkbox("zoom_to_mouse", L("Zoom to mouse position"),
               {L("Zoom in towards the mouse pointer's position in the 3D view, rather than the 2D window center.")});
    // TRN: "Dolly" is a film and camera term: the camera itself travels towards or away from the scene.
    t.checkbox("perspective_camera_dolly", L("Dolly the perspective camera"),
               {L("In perspective view, zooming moves the camera towards the scene and keeps the field of view fixed. If not enabled, "
                  "the camera stays at about the same distance and zooming changes the field of view.\nThe camera can enter the scene "
                  "when this is enabled, and nearby objects are then cut off.")});
    t.checkbox("use_free_camera", L("Use free camera"), {L("If enabled, use free camera. If not enabled, use constrained camera.")});
    t.checkbox("reverse_mouse_wheel_zoom", L("Reverse mouse zoom"), {L("If enabled, reverses the direction of zoom with mouse wheel.")});
    // These values are saved in the config, so new actions go at the end.
    const Choices drag_actions = {{"0", L("None")}, {"1", L("Pan")}, {"2", L("Rotate")},
                                  // TRN: Mouse drag action. It moves the camera in dolly mode ("Dolly" is a film and camera term) and zooms otherwise.
                                  {"3", L("Dolly/Zoom")}};
    t.choice("left_mouse_drag_action", L("Left Mouse Drag"), drag_actions,
             {L("Set the action that dragging the left mouse button should perform.")});
    t.choice("middle_mouse_drag_action", L("Middle Mouse Drag"), drag_actions,
             {L("Set the action that dragging the middle mouse button should perform.")});
    t.choice("right_mouse_drag_action", L("Right Mouse Drag"), drag_actions,
             {L("Set the action that dragging the right mouse button should perform.")});

    //// GRAPHICS > General
    t.section(kGraphics, L("General"));
    t.checkbox(SETTING_OPENGL_PHONG_SMOOTH_NORMALS, L("Smooth normals"),
               {L("Applies smooth normals to the model.\n\nRequires manual scene reload to take effect "
                  "(right-click on 3D view → \"Reload All\").")});
    t.checkbox("preview_translucent_filaments", L("Translucent filaments"),
               {L("Draw the toolpaths of filaments whose color is partly transparent as see-through, as the 3D view draws their objects. "
                  "Turn it off to draw every filament opaque.")});

    //// GRAPHICS > Realistic view
    t.section(kGraphics, L("Realistic View"));
    t.checkbox(SETTING_OPENGL_REALISTIC_PHONG, L("Phong shading"), {L("Uses Phong shading inside realistic view.")});
    t.checkbox(SETTING_OPENGL_REALISTIC_PREVIEW, L("Enable in Preview"),
               {L("Also applies realistic view to the Preview canvas, not just Prepare.\n"
                  "Preview draws the full toolpath geometry, so shadows and SSAO cost considerably"
                  " more there than on a plain model.")});
    t.checkbox(SETTING_OPENGL_PHONG_SSAO, L("SSAO ambient occlusion"), {L("Applies SSAO in realistic view.")});
    t.choice(SETTING_OPENGL_REALISTIC_SHADOWS, L("Shadows"),
             {{"off", L("Off")},
              // TRN Realistic-view shadow mode: the light stays fixed in the scene.
              {"static", L("Static")},
              // TRN Realistic-view shadow mode: the light turns with the camera.
              {"orbit", L("Orbit")}},
             {L("Renders cast shadows on the plate, other objects, and each object onto itself in realistic view.\n"
                "Static: the light stays fixed in the world, so the shadows are only recomputed when the scene changes.\n"
                "Orbit: the light turns with the camera, recomputing the shadows every frame the camera moves.")});
    t.checkbox(SETTING_OPENGL_REALISTIC_GLOSS, L("Glossy toolpaths"),
               {L("Highlights the toolpaths in Preview as glossy plastic in realistic view.\n"
                  "Off, a translucent filament is matte and an opaque one keeps the highlight of the plain view.")});

    //// GRAPHICS > Outline
    t.section(kGraphics, L("Outline"));
    t.checkbox(SETTING_OPENGL_OUTLINE_PERSISTENT, L("Persistent"),
               {L("When Outline is enabled in the 3D canvas menu, outlines visible objects even when they are not selected.")});

    //// GRAPHICS > Anti-aliasing
    t.section(kGraphics, L("Anti-aliasing"));
    // Its tooltip ends saying that it requires a restart.
    t.choice(SETTING_OPENGL_AA_SAMPLES, L("MSAA Multiplier"), {{"0", L("Disabled")}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}, {"16", "16x"}},
             {L("Set the Multi-Sample Anti-Aliasing level.\n"
                "Higher values result in smoother edges, but the impact on performance is exponential.\n"
                "Lower values improve performance, at the cost of jagged edges.\n"
                "If disabled, its recommended to enable FXAA to reduce jagged edges with minimal performance impact.\n\n"
                "Requires application restart.")})
        .restart_needed = true;
    t.checkbox(SETTING_OPENGL_FXAA_ENABLED, L("FXAA post-processing"),
               {L("Applies Fast Approximate Anti-Aliasing as a screen-space pass.\n"
                  "Useful for disabling or reducing the MSAA setting to improve performance.\n\n"
                  "Takes effect immediately.")});

    //// GRAPHICS > FPS
    t.section(kGraphics, L("FPS"));
    Preference& fps_cap = t.number(SETTING_OPENGL_FPS_CAP, L("FPS cap"), 0, 240,
                                   {L("Limits viewport frame rate to reduce GPU load and power usage.\n"
                                      "Set to 0 for unlimited frame rate.")});
    fps_cap.note        = L("(0 = unlimited)");
    fps_cap.unit        = L("FPS");
    t.checkbox(SETTING_OPENGL_SCENE_CACHE, L("Reuse the 3D scene while idle"),
               {L("Skips redrawing the 3D scene when only the mouse cursor moves over the viewport, "
                  "and reuses the previous frame's scene instead. Reduces GPU load.\n"
                  "Disable it if the viewport shows stale or missing contents.\n\n"
                  "Takes effect immediately.")});
    t.checkbox(SETTING_OPENGL_SKIP_IDENTICAL_FRAMES, L("Skip unchanged frames"),
               {L("Skips drawing a frame altogether when it would be identical to the one already on screen.\n"
                  "Only applies to frames that reuse the 3D scene, so it needs Reuse the 3D scene while idle.\n"
                  "Disable it if a hover highlight, tooltip or animation stops updating.\n\n"
                  "Takes effect immediately.")});
    t.checkbox(SETTING_OPENGL_SHOW_FPS_OVERLAY, L("Show FPS overlay"),
               {L("Displays rendering counts in the top-right corner of the viewport."),
                L("FPS: frames presented to the screen per second."),
                L("3D: frames per second that redrew the 3D scene.")});
    t.checkbox(SETTING_OPENGL_SHOW_RENDER_TIMINGS, L("Show render timings"),
               {L("Displays how many milliseconds each part of a frame that redraws the 3D scene takes, in the top-right corner of the "
                  "viewport."),
                L("CPU: time spent issuing the drawing commands."), L("GPU: time the graphics card spent running them."),
                L("Adds a small overhead to each frame while enabled.")});

    //// ONLINE > Connection
    t.section(kOnline, L("Connection"));
    Preference& region = t.choice("region", L("Login region"),
                                  {{"Asia-Pacific", "Asia-Pacific"},
                                   {"China", "China"},
                                   {"Europe", "Europe"},
                                   {"North America", "North America"},
                                   {"Others", "Others"}});
    region.fallback    = ""; // no region is shown where none is stored
    region.own_row     = true;
    // TRN %1% is the key of a setting of the Preferences dialog; the sentence says why a script may not change it.
    region.refused     = L("%1%: changing the login region signs out of the cloud account.");
    t.checkbox("stealth_mode", L("Stealth mode"),
               {L("This disables all cloud features, including Orca Cloud profile syncing. Users who prefer to work entirely offline can "
                  "enable this option.\n\nNote: When Stealth Mode is enabled, your user profiles will not be backed up to Orca Cloud.")});
    t.checkbox("hide_login_side_panel", L("Hide login side panel"), {L("Hide the login side panel on the home page.")});

    //// ONLINE > Update & sync
    t.section(kOnline, L("Update & sync"));
    t.checkbox("check_stable_update_only", L("Check for stable updates only"));
    t.checkbox("sync_user_preset", L("Auto sync user presets (Printer/Filament/Process)"));
    t.choice("sync_ams_filament_mode", L("Filament sync mode"), {{"0", L("Filament & Color")}, {"1", L("Color only")}},
             {L("Choose whether sync updates both filament preset and color, or only color.")});
    t.checkbox("sync_system_preset", L("Update built-in presets automatically."));
    t.checkbox(SETTING_USE_ENCRYPTED_TOKEN_FILE, L("Use encrypted file for token storage"),
               {L("Store authentication tokens in an encrypted file instead of the system keychain. (Requires restart)")})
        .restart_needed = true;

    //// ONLINE > Network plugin
    t.section(kOnline, L("Bambu network plug-in"));
    t.checkbox("installed_networking", L("Enable Bambu network plug-in")).refused =
        // TRN %1% is the key of a setting of the Preferences dialog; the sentence says why a script may not change it.
        L("%1%: enabling the Bambu network plug-in downloads and installs it.");
    Preference& plugin_version = t.choice(SETTING_NETWORK_PLUGIN_VERSION, L("Network plug-in version"), {},
                                          {L("Select the network plug-in version to use")});
    plugin_version.own_row     = true; // the versions are those the app can load
    // TRN %1% is the key of a setting of the Preferences dialog; the sentence says why a script may not change it.
    plugin_version.refused     = L("%1%: switching the version downloads or reloads the network plug-in.");

    //// DEVELOPER > Settings
    t.section(kDeveloper, L("Settings"));
    t.checkbox("developer_mode", L("Developer mode")).wiki = "option_mode#developer+mode";
    t.checkbox("skip_ams_blacklist_check", L("Skip AMS blacklist check"));
    t.checkbox("show_unsupported_presets", L("Show unsupported presets"),
               {L("Show incompatible/unsupported presets in the printer and filament dropdown lists. These presets cannot be selected.")});
    t.checkbox("export_statistics", L("Export statistics with G-code"),
               {L("If enabled, exporting a G-code file also writes its statistics as JSON next to it, as <name>.statistics.json, "
                  "and an exported sliced 3MF file holds them inside: print time, filament and feature totals, "
                  "and the same per layer and per object.")});

    //// DEVELOPER > Experimental Features
    t.section(kDeveloper, L("Experimental Features"));
    t.checkbox("keep_painting", L("Keep painted feature after mesh change"),
               {L("Attempt to keep painted features (color/seam/support/fuzzy etc.) after changing the object mesh (such as cut/reload "
                  "from disk/simplify/fix etc.)\nHighly experimental! Slow and may create artifact.")});
    t.checkbox("use_printer_agents", L("Use printer agents instead of print hosts"),
               {L("Route print jobs for non-Bambu printers through printer plug-in agents instead of the classic print-host upload "
                  "flow.\nWhen disabled, OrcaSlicer uses the legacy print-host behavior.")});

    //// DEVELOPER > Storage
    t.section(kDeveloper, L("Storage"));
    t.checkbox("allow_abnormal_storage", L("Allow Abnormal Storage"),
               {L("This allows the use of Storage that is marked as abnormal by the Printer.\nUse at your own risk, can cause issues!")});

    //// DEVELOPER > Log Level
    t.section(kDeveloper, L("Log Level"));
    // The values are get_string_logging_level()'s, in the order of their levels.
    Preference& log_level = t.choice("log_severity_level", L("Log Level"),
                                     {{"fatal", L("fatal")},
                                      {"error", L("error")},
                                      {"warning", L("warning")},
                                      {"info", L("info")},
                                      {"debug", L("debug")},
                                      {"trace", L("trace")}},
                                     {L("Log Level")});
    log_level.fallback    = ""; // the row shows no level where none is stored
    log_level.own_row     = true;

    return table;
}

// A number as a person writes it: no trailing zeros, no exponent for the ranges of the table.
std::string number_text(double value)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << value;
    return out.str();
}

Refusal refusal(const char* code, const char* msgid, std::vector<std::string> args, std::vector<std::string> names = {})
{
    return {RefusalKind::Precondition, code, msgid, std::move(args), std::move(names)};
}

// Why `value` is not one `p` takes, or none.
std::optional<Refusal> value_refusal(const Preference& p, const json& value)
{
    switch (p.kind) {
    case PreferenceKind::Checkbox:
        if (!value.is_boolean())
            // TRN %1% is a preference's key.
            return refusal("not_a_boolean", L("%1% is a checkbox: give true or false."), {p.key});
        return std::nullopt;
    case PreferenceKind::Choice: {
        const std::string given = value.is_string() ? value.get<std::string>() : value.dump();
        const bool        known = (value.is_string() || value.is_number_integer()) &&
                           std::any_of(p.choices.begin(), p.choices.end(), [&given](const auto& c) { return c.first == given; });
        if (known)
            return std::nullopt;
        std::vector<std::string> values;
        for (const auto& [stored, label] : p.choices)
            values.push_back(stored);
        // TRN %1% is a preference's key, %2% the value a call gave it.
        return refusal("no_such_choice", L("%1% has no choice \"%2%\"."), {p.key, given}, std::move(values));
    }
    case PreferenceKind::Number: {
        if (!value.is_number())
            // TRN %1% is a preference's key.
            return refusal("not_a_number", L("%1% takes a number."), {p.key});
        const double number = value.get<double>();
        if (p.decimals == 0 && std::floor(number) != number)
            // TRN %1% is a preference's key.
            return refusal("not_a_whole_number", L("%1% takes a whole number."), {p.key});
        if ((p.min && number < *p.min) || (p.max && number > *p.max)) {
            const std::string low = p.min ? number_text(*p.min) : std::string(), high = p.max ? number_text(*p.max) : std::string();
            if (p.min && p.max)
                // TRN %1% is a preference's key, %2% and %3% the lowest and highest numbers it takes.
                return refusal("out_of_range", L("%1% takes a number from %2% to %3%."), {p.key, low, high}, {low, high});
            if (p.min)
                // TRN %1% is a preference's key, %2% the lowest number it takes.
                return refusal("out_of_range", L("%1% takes a number of at least %2%."), {p.key, low}, {low});
            // TRN %1% is a preference's key, %2% the highest number it takes.
            return refusal("out_of_range", L("%1% takes a number of at most %2%."), {p.key, high}, {high});
        }
        return std::nullopt;
    }
    case PreferenceKind::Text: {
        if (!value.is_string())
            // TRN %1% is a preference's key.
            return refusal("not_a_text", L("%1% takes a text."), {p.key});
        const wxString path = wxString::FromUTF8(value.get<std::string>());
        if (p.folder && (path.empty() || !wxFileName(path).IsAbsolute() || !wxDirExists(path)))
            // TRN %1% is a preference's key, %2% the text a call gave it.
            return refusal("not_a_folder", L("%1% takes the absolute path of an existing folder, which \"%2%\" is not."),
                           {p.key, value.get<std::string>()});
        return std::nullopt;
    }
    }
    return std::nullopt;
}

// The Preview tab's canvas, where the main window is up.
GLCanvas3D* preview_canvas()
{
    Plater* plater = wxGetApp().plater();
    return plater != nullptr ? plater->get_preview_canvas3D() : nullptr;
}

// Draws the loaded preview again with the change just made to its viewer.
void redraw_preview(GLCanvas3D& canvas)
{
    canvas.set_as_dirty();
    canvas.request_extra_frame();
}

} // namespace

const std::vector<Preference>& preferences()
{
    static const std::vector<Preference> table = build_table();
    return table;
}

std::vector<Preference> shown_preferences()
{
    std::vector<Preference> shown;
    for (const Preference& p : preferences())
        if (p.shown == nullptr || p.shown())
            shown.push_back(p);
    return shown;
}

const Preference* find_preference(const std::vector<Preference>& table, const std::string& key)
{
    const auto it = std::find_if(table.begin(), table.end(), [&key](const Preference& p) { return p.key == key; });
    return it != table.end() ? &*it : nullptr;
}

std::string preference_text(const Preference& p, const json& value)
{
    switch (p.kind) {
    case PreferenceKind::Checkbox: {
        const bool on = value.get<bool>();
        if (p.fallback == "0")
            return on ? "1" : "0";
        return on ? "true" : "false";
    }
    case PreferenceKind::Choice: return value.is_string() ? value.get<std::string>() : value.dump();
    case PreferenceKind::Number: {
        const double number = value.get<double>();
        if (p.decimals == 0)
            return std::to_string(std::llround(number));
        // As the dialog's field writes it: wxString::FromCDouble() with the row's decimals.
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::fixed << std::setprecision(p.decimals) << number;
        return out.str();
    }
    case PreferenceKind::Text: return value.get<std::string>();
    }
    return {};
}

json preference_value(const Preference& p, const std::string& stored)
{
    switch (p.kind) {
    case PreferenceKind::Checkbox: return AppConfig::is_true(stored);
    case PreferenceKind::Number: {
        std::istringstream in(stored);
        in.imbue(std::locale::classic());
        double number = 0;
        if (stored.empty() || !(in >> number) || !in.eof())
            return stored;
        if (p.decimals == 0 && std::floor(number) == number)
            return std::int64_t(number);
        return number;
    }
    case PreferenceKind::Choice:
    case PreferenceKind::Text: return stored;
    }
    return stored;
}

std::optional<Refusal> check_preferences(const std::vector<Preference>& table, const std::map<std::string, json>& values)
{
    for (const auto& [key, value] : values) {
        const Preference* p = find_preference(table, key);
        if (p == nullptr) {
            std::vector<std::string> keys;
            for (const Preference& known : table)
                keys.push_back(known.key);
            // TRN %1% is the name of a setting a call asked for.
            return refusal("not_a_preference", L("%1% is not a setting of the Preferences dialog."), {key},
                           closest_names(std::move(keys), key));
        }
        if (p->refused != nullptr)
            return refusal("preference_refused", p->refused, {key}, {key});
        if (std::optional<Refusal> why = value_refusal(*p, value))
            return why;
    }
    return std::nullopt;
}

Outcome<PreferenceChanges> set_preferences(const std::map<std::string, json>& values)
{
    const std::vector<Preference> table = shown_preferences();
    for (const auto& [key, value] : values)
        if (find_preference(table, key) == nullptr && find_preference(preferences(), key) != nullptr)
            // TRN %1% is the name of a setting that the Preferences dialog shows only in some cases.
            return refusal("preference_not_shown", L("The Preferences dialog does not show %1% now."), {key});
    if (std::optional<Refusal> why = check_preferences(table, values))
        return *why;

    AppConfig&        config = *wxGetApp().app_config;
    PreferenceChanges changes;
    for (const auto& [key, value] : values) {
        const Preference& p     = *find_preference(table, key);
        const std::string after = preference_text(p, value);
        std::string       before;
        const bool        stored = config.get("app", key, before);
        if (stored && preference_value(p, before) == preference_value(p, after)) {
            changes.unchanged.push_back(key);
            continue;
        }
        config.set(key, after);
        changes.changed.push_back({key, stored ? before : p.fallback, after, stored});
    }
    if (changes.changed.empty())
        return changes;
    config.save();

    bool redraw = false;
    for (const PreferenceChange& change : changes.changed) {
        apply_preference(change.key);
        const std::vector<std::string>& drawn = canvas_preferences();
        redraw |= std::find(drawn.begin(), drawn.end(), change.key) != drawn.end();
    }
    if (Plater* plater = wxGetApp().plater(); redraw && plater != nullptr)
        plater->set_current_canvas_as_dirty();
    return changes;
}

void apply_preference(const std::string& key)
{
    GUI_App&   app    = wxGetApp();
    AppConfig& config = *app.app_config;
    if (key == "sync_user_preset") {
        const bool sync = config.get("sync_user_preset") == "true";
        if (sync)
            app.start_sync_user_preset();
        else
            app.stop_sync_user_preset();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " sync_user_preset: " << (sync ? "true" : "false");
    } else if (key == "stealth_mode") {
        if (config.get_stealth_mode())
            app.on_stealth_mode_enter();
    } else if (key == "hide_login_side_panel") {
        if (WebViewPanel* home = WebViewPanel::if_built())
            home->SendCloudProvidersInfo();
    } else if (key == "perspective_camera_dolly") {
        // ORCA: apply the camera change immediately to the current view
        if (Plater* plater = app.plater()) {
            plater->apply_camera_dolly_setting();
            plater->set_current_canvas_as_dirty();
        }
    } else if (key == "preview_translucent_filaments") {
        // ORCA: apply the filament translucency change immediately to the currently loaded preview
        if (GLCanvas3D* canvas = preview_canvas()) {
            canvas->get_gcode_viewer().set_translucent_filaments(config.get_bool(key));
            redraw_preview(*canvas);
        }
    } else if (key == "preview_dim_previous_layers") {
        // ORCA: apply the preview dimming change immediately to the currently loaded preview
        if (GLCanvas3D* canvas = preview_canvas()) {
            canvas->get_gcode_viewer().set_dim_previous_layers(config.get_bool(key));
            redraw_preview(*canvas);
        }
    } else if (key == "preview_dim_previous_layers_brightness") {
        // ORCA: apply the new brightness immediately to the currently loaded preview
        if (GLCanvas3D* canvas = preview_canvas()) {
            canvas->get_gcode_viewer().set_dim_previous_layers_brightness(0.01f * std::atoi(config.get(key).c_str()));
            redraw_preview(*canvas);
        }
    } else if (key == "preview_reduced_detail_mode") {
        // apply the new mode immediately to the currently loaded preview
        if (GLCanvas3D* canvas = preview_canvas()) {
            canvas->get_gcode_viewer().set_reduced_detail_mode(config.get(key));
            redraw_preview(*canvas);
        }
#ifdef __WXMSW__
    } else if (key == "associate_3mf" || key == "associate_drc" || key == "associate_stl" || key == "associate_step") {
        const std::wstring extension = wxString::FromUTF8(key.substr(std::string("associate_").size())).ToStdWstring();
        if (config.get(key) == "true")
            app.associate_files(extension);
        else
            app.disassociate_files(extension);
    } else if (key == "installed_networking") {
        if (config.get_bool("installed_networking"))
            app.CallAfter([] { wxGetApp().ShowDownNetPluginDlg(); });
#endif // __WXMSW__
    } else if (key == "developer_mode") {
        app.update_mode();
    } else if (key == "internal_developer_mode") {
        // webview  dump_vedio
        app.update_internal_development();
        if (config.get("internal_developer_mode") == "true")
            app.mainframe->show_log_window();
    } else if (key == "show_unsupported_presets" || key == "group_filament_presets") {
        app.plater()->sidebar().update_presets(Preset::TYPE_FILAMENT);
    } else if (key == "use_printer_agents") {
        // Rebuild the Device tab so the native/web-UI choice reflects the new flag
        // immediately, instead of only on the next printer-preset change or restart.
        if (app.plater())
            app.plater()->sidebar().update_all_preset_comboboxes();
    } else if (key == "filaments_area_preferred_count") {
        if (Plater* plater = app.plater()) {
            Sidebar& sidebar = plater->sidebar();
            sidebar.update_filaments_area_height();
            sidebar.Freeze();
            sidebar.Layout();
            sidebar.Thaw();
            plater->PostSizeEvent();
        }
    } else if (key == SETTING_PLUGIN_PAGES_VISIBLE_COUNT) {
        app.mainframe->plugin_pages().set_visible_page_count(std::atoi(config.get(key).c_str()));
    } else if (key == "max_recent_count") {
        long max = 0;
        if (wxString::FromUTF8(config.get(key)).ToLong(&max))
            app.mainframe->set_max_recent_count(max);
    } else if (key == "log_severity_level") {
        Slic3r::set_logging_level(Slic3r::level_string_to_boost(config.get(key)));
    } else if (key == "backup_switch") {
        const bool  on              = config.get("backup_switch") == "true";
        std::string backup_interval = "10";
        config.get("backup_interval", backup_interval);
        Slic3r::set_backup_interval(on ? boost::lexical_cast<long>(backup_interval) : 0);
    } else if (key == "backup_interval") {
        // The period runs the timer only while Auto backup is on, as the app's start sets it; the dialog's field is
        // disabled while it is off.
        long interval = 0;
        wxString::FromUTF8(config.get(key)).ToLong(&interval);
        Slic3r::set_backup_interval(config.get_bool("backup_switch") ? interval : 0);
    } else if (key == "download_path") {
        // The open Preferences dialog shows the folder in its row. Its Browse button is what reaches it: a call that
        // writes the folder cannot run while the dialog, which is modal, is open.
        for (wxWindow* window : wxTopLevelWindows)
            if (auto* dialog = dynamic_cast<PreferencesDialog*>(window))
                dialog->show_download_path();
    } else if (key == "dark_color_mode") {
        app.Update_dark_mode_flag();
#ifdef _MSW_DARK_MODE
        app.force_colors_update();
        app.update_ui_from_settings();
#endif
        SimpleEvent evt = SimpleEvent(EVT_GLCANVAS_COLOR_MODE_CHANGED);
        wxPostEvent(app.plater(), evt);
    }
}

const std::vector<std::string>& canvas_preferences()
{
    static const std::vector<std::string> keys = {
        SETTING_OPENGL_FXAA_ENABLED,       SETTING_OPENGL_FPS_CAP,           SETTING_OPENGL_SHOW_FPS_OVERLAY,
        SETTING_OPENGL_SHOW_RENDER_TIMINGS, SETTING_OPENGL_SCENE_CACHE,       SETTING_OPENGL_SKIP_IDENTICAL_FRAMES,
        SETTING_OPENGL_REALISTIC_SHADOWS,   SETTING_OPENGL_REALISTIC_GLOSS,   SETTING_OPENGL_OUTLINE_PERSISTENT,
        SETTING_OPENGL_OUTLINE_COLOR,       SETTING_OPENGL_SELECTION_OUTLINE_COLOR};
    return keys;
}

bool mcp_switch_shown()
{
    const Mcp::McpEndpoint* endpoint = wxGetApp().mcp_endpoint();
    return wxGetApp().app_config->get_bool("mcp_enabled") || (endpoint != nullptr && endpoint->serving());
}

int mcp_port_shown()
{
    GUI_App&                app      = wxGetApp();
    const Mcp::McpEndpoint* endpoint = app.mcp_endpoint();
    if (endpoint != nullptr && endpoint->serving())
        return endpoint->port();
    const GUI_InitParams* params = app.init_params;
    return Mcp::endpoint_port(params != nullptr ? params->mcp.port : 0, app.app_config->get("mcp_port"));
}

std::map<std::string, std::string> shown_texts()
{
    return {{"mcp_enabled", mcp_switch_shown() ? "true" : "false"}, {"mcp_port", std::to_string(mcp_port_shown())}};
}

}}} // namespace Slic3r::GUI::Ops
