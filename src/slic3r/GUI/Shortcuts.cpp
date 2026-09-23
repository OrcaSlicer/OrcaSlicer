#include "Shortcuts.hpp"

#include "I18N.hpp"
#include "libslic3r/AppConfig.hpp"

#include <algorithm>
#include <map>
#include <utility>

namespace Slic3r { namespace GUI {

namespace {

constexpr uint8_t GLOBAL      = context_bit(ShortcutContext::Global);
constexpr uint8_t PLATER      = context_bit(ShortcutContext::Plater);
constexpr uint8_t PREVIEW     = context_bit(ShortcutContext::Preview);
constexpr uint8_t OBJECT_LIST = context_bit(ShortcutContext::ObjectList);
constexpr uint8_t PAINTING    = context_bit(ShortcutContext::Painting);
constexpr uint8_t CANVAS      = PLATER | PREVIEW;

constexpr int CTRL       = wxMOD_CONTROL;
constexpr int SHIFT      = wxMOD_SHIFT;
constexpr int CTRL_SHIFT = wxMOD_CONTROL | wxMOD_SHIFT;

#ifdef __APPLE__
constexpr KeyChord PREFERENCES_CHORD{ ',', CTRL };
constexpr KeyChord DELETE_CHORD{ WXK_BACK };
constexpr KeyChord MOUSE3D_CHORD{ 'M', CTRL_SHIFT };
#else
constexpr KeyChord PREFERENCES_CHORD{ 'P', CTRL };
constexpr KeyChord DELETE_CHORD{ WXK_DELETE };
constexpr KeyChord MOUSE3D_CHORD{ 'M', CTRL };
#endif

// Each row: enum value, AppConfig key, description shown in the dialog, contexts the key is
// looked up in, and the default chord. Rows are in the order the dialog lists them, within
// the sections of the Shortcut enum.
//   SHORTCUT  runs once per key press.
//   REPEATING runs again on every auto-repeat of a held key.
//   STEPPING  repeats, and Shift or Ctrl held with its key select a step variant of it instead
//             of another shortcut (ShortcutInfo::modifier_variants).
#define SHORTCUT(id, key, name, contexts, ...)  ShortcutInfo{ Shortcut::id, key, name, contexts, __VA_ARGS__, false, false }
#define REPEATING(id, key, name, contexts, ...) ShortcutInfo{ Shortcut::id, key, name, contexts, __VA_ARGS__, true,  false }
#define STEPPING(id, key, name, contexts, ...)  ShortcutInfo{ Shortcut::id, key, name, contexts, __VA_ARGS__, true,  true }

constexpr std::array<ShortcutInfo, size_t(Shortcut::Count)> shortcut_table = {{
    // Project
    SHORTCUT(NewProject,        "new_project",        L("New Project"),                                          GLOBAL, { 'N', CTRL }),
    SHORTCUT(OpenProject,       "open_project",       L("Open Project"),                                         GLOBAL, { 'O', CTRL }),
    SHORTCUT(SaveProject,       "save_project",       L("Save Project"),                                         GLOBAL, { 'S', CTRL }),
    SHORTCUT(SaveProjectAs,     "save_project_as",    L("Save Project as"),                                      GLOBAL, { 'S', CTRL_SHIFT }),
    SHORTCUT(ImportModel,       "import_model",       L("Import geometry data from STL/STEP/3MF/OBJ/AMF files"), GLOBAL, { 'I', CTRL }),
    SHORTCUT(Publish3mf,        "publish_3mf",        L("Publish 3MF"),                                          GLOBAL, { 'E', CTRL_SHIFT }),

    // Slicing and printing
    SHORTCUT(SlicePlate,        "slice_plate",        L("Slice plate"),                                          GLOBAL, { 'R', CTRL }),
    SHORTCUT(ExportSlicedFile,  "export_sliced_file", L("Export plate sliced file"),                             GLOBAL, { 'G', CTRL }),
    SHORTCUT(PrintPlate,        "print_plate",        L("Print plate"),                                          GLOBAL, { 'G', CTRL_SHIFT }),
    SHORTCUT(PrintHostQueue,    "print_host_queue",   L("Print host upload queue"),                              GLOBAL, { 'J', CTRL }),

    // Selection
    SHORTCUT(SelectAll,         "select_all",         L("Select all objects"),                                   PLATER | OBJECT_LIST, { 'A', CTRL }),
    SHORTCUT(SelectAllPlates,   "select_all_plates",  L("Select all objects on all plates"),                     PLATER,               { 'A', CTRL_SHIFT }),

    // Editing
    REPEATING(Undo,             "undo",               L("Undo"),                                                 PLATER | OBJECT_LIST, { 'Z', CTRL }),
    REPEATING(Redo,             "redo",               L("Redo"),                                                 PLATER | OBJECT_LIST, { 'Y', CTRL }),
    SHORTCUT(Cut,               "cut",                L("Cut"),                                                  PLATER | OBJECT_LIST, { 'X', CTRL }),
    SHORTCUT(Copy,              "copy",               L("Copy to clipboard"),                                    PLATER | OBJECT_LIST, { 'C', CTRL }),
    SHORTCUT(Paste,             "paste",              L("Paste from clipboard"),                                 PLATER | OBJECT_LIST, { 'V', CTRL }),
    SHORTCUT(DeleteSelected,    "delete_selected",    L("Delete Selected"),                                      PLATER | OBJECT_LIST, DELETE_CHORD),
    SHORTCUT(DeleteAll,         "delete_all",         L("Delete All"),                                           PLATER,               { 'D', CTRL }),
    SHORTCUT(CloneSelected,     "clone_selected",     L("Clone Selected"),                                       PLATER | OBJECT_LIST, { 'K', CTRL }),

    // Objects
    REPEATING(AddInstance,      "add_instance",       L("Add instance"),                                         PLATER | OBJECT_LIST, { '+' }),
    REPEATING(RemoveInstance,   "remove_instance",    L("Remove instance"),                                      PLATER | OBJECT_LIST, { '-' }),
    SHORTCUT(TogglePrintable,   "toggle_printable",   L("Toggle printable for object/part"),                     PLATER | OBJECT_LIST, { 'V' }),
    SHORTCUT(ToggleAutoDrop,    "toggle_auto_drop",   L("Auto Drop"),                                            OBJECT_LIST,          { 'D' }),

    // Placement
    SHORTCUT(Arrange,           "arrange",            L("Arrange all objects"),                                  PLATER, { 'A' }),
    SHORTCUT(ArrangePlate,      "arrange_plate",      L("Arrange objects on selected plates"),                   PLATER, { 'A', SHIFT }),
    SHORTCUT(Orient,            "orient",             L("Auto orient all/selected objects"),                     PLATER, { 'Q' }),
    SHORTCUT(OrientPlate,       "orient_plate",       L("Auto orient all objects on current plate"),             PLATER, { 'Q', SHIFT }),
    STEPPING(MoveSelectionLeft,   "move_selection_left",    L("Move selection 10mm in negative X direction"),    PLATER, { WXK_LEFT }),
    STEPPING(MoveSelectionRight,  "move_selection_right",   L("Move selection 10mm in positive X direction"),    PLATER, { WXK_RIGHT }),
    STEPPING(MoveSelectionUp,     "move_selection_up",      L("Move selection 10mm in positive Y direction"),    PLATER, { WXK_UP }),
    STEPPING(MoveSelectionDown,   "move_selection_down",    L("Move selection 10mm in negative Y direction"),    PLATER, { WXK_DOWN }),
    REPEATING(RotateSelectionLeft,  "rotate_selection_left",  L("Rotate selection 45 degrees counterclockwise"), PLATER, { WXK_PAGEUP }),
    REPEATING(RotateSelectionRight, "rotate_selection_right", L("Rotate selection 45 degrees clockwise"),        PLATER, { WXK_PAGEDOWN }),

    // Gizmos
    SHORTCUT(GizmoMove,         "gizmo_move",         L("Gizmo move"),                                           PLATER, { 'M' }),
    SHORTCUT(GizmoRotate,       "gizmo_rotate",       L("Gizmo rotate"),                                         PLATER, { 'R' }),
    SHORTCUT(GizmoScale,        "gizmo_scale",        L("Gizmo scale"),                                          PLATER, { 'S' }),
    SHORTCUT(GizmoFlatten,      "gizmo_flatten",      L("Gizmo place face on bed"),                              PLATER, { 'F' }),
    SHORTCUT(GizmoCut,          "gizmo_cut",          L("Gizmo cut"),                                            PLATER, { 'C' }),
    SHORTCUT(GizmoMeshBoolean,  "gizmo_mesh_boolean", L("Gizmo mesh boolean"),                                   PLATER, { 'B' }),
    SHORTCUT(GizmoFdmSupports,  "gizmo_fdm_supports", L("Gizmo FDM paint-on supports"),                          PLATER, { 'L' }),
    SHORTCUT(GizmoSeam,         "gizmo_seam",         L("Gizmo FDM paint-on seam"),                              PLATER, { 'P' }),
    SHORTCUT(GizmoFuzzySkin,    "gizmo_fuzzy_skin",   L("Gizmo FDM paint-on fuzzy skin"),                        PLATER, { 'H' }),
    SHORTCUT(GizmoMmuSegmentation, "gizmo_mmu_segmentation", L("Gizmo multi-material painting"),                 PLATER, { 'N' }),
    SHORTCUT(GizmoEmboss,       "gizmo_emboss",       L("Gizmo text emboss/engrave"),                            PLATER, { 'T' }),
    SHORTCUT(GizmoMeasure,      "gizmo_measure",      L("Gizmo measure"),                                        PLATER, { 'U' }),
    SHORTCUT(GizmoAssembly,     "gizmo_assembly",     L("Gizmo assemble"),                                       PLATER, { 'Y' }),
    SHORTCUT(GizmoBrimEars,     "gizmo_brim_ears",    L("Gizmo brim ears"),                                      PLATER, { 'E' }),

    // Sliders
    SHORTCUT(GoToLayer,           "go_to_layer",           L("Jump to layer"),                                   PREVIEW, { 'G', SHIFT }),
    STEPPING(LayerSliderUp,       "layer_slider_up",       L("Vertical slider - Move active thumb Up"),          PREVIEW, { WXK_UP }),
    STEPPING(LayerSliderDown,     "layer_slider_down",     L("Vertical slider - Move active thumb Down"),        PREVIEW, { WXK_DOWN }),
    STEPPING(MovesSliderLeft,     "moves_slider_left",     L("Horizontal slider - Move active thumb Left"),      PREVIEW, { WXK_LEFT }),
    STEPPING(MovesSliderRight,    "moves_slider_right",    L("Horizontal slider - Move active thumb Right"),     PREVIEW, { WXK_RIGHT }),
    REPEATING(MovesSliderStart,   "moves_slider_start",    L("Horizontal slider - Move to start position"),      PREVIEW, { WXK_HOME }),
    REPEATING(MovesSliderEnd,     "moves_slider_end",      L("Horizontal slider - Move to last position"),       PREVIEW, { WXK_END }),

    // Painting tools
    SHORTCUT(PaintToolCircle,      "paint_tool_circle",       L("Circle"),                                       PAINTING, { 'C' }),
    SHORTCUT(PaintToolSphere,      "paint_tool_sphere",       L("Sphere"),                                       PAINTING, { 'S' }),
    SHORTCUT(PaintToolFill,        "paint_tool_fill",         L("Fill"),                                         PAINTING, { 'F' }),
    SHORTCUT(PaintToolGapFill,     "paint_tool_gap_fill",     L("Gap Fill"),                                     PAINTING, { 'G' }),
    SHORTCUT(PaintToolTriangle,    "paint_tool_triangle",     L("Triangle"),                                     PAINTING, { 'T' }),
    SHORTCUT(PaintToolHeightRange, "paint_tool_height_range", L("Height Range"),                                 PAINTING, { 'H' }),

    // Camera
    SHORTCUT(ViewDefault,       "view_default",       L("Camera view - Default"),                                GLOBAL, { '0', CTRL }),
    SHORTCUT(ViewTop,           "view_top",           L("Camera view - Top"),                                    GLOBAL, { '1', CTRL }),
    SHORTCUT(ViewBottom,        "view_bottom",        L("Camera view - Bottom"),                                 GLOBAL, { '2', CTRL }),
    SHORTCUT(ViewFront,         "view_front",         L("Camera view - Front"),                                  GLOBAL, { '3', CTRL }),
    SHORTCUT(ViewRear,          "view_rear",          L("Camera view - Behind"),                                 GLOBAL, { '4', CTRL }),
    SHORTCUT(ViewLeft,          "view_left",          L("Camera Angle - Left side"),                             GLOBAL, { '5', CTRL }),
    SHORTCUT(ViewRight,         "view_right",         L("Camera Angle - Right side"),                            GLOBAL, { '6', CTRL }),
    SHORTCUT(ViewPlate,         "view_plate",         L("Camera view - Current plate"),                          GLOBAL, { '7', CTRL }),
    REPEATING(ZoomIn,           "zoom_in",            L("Zoom in"),                                              CANVAS, { 'I' }),
    REPEATING(ZoomOut,          "zoom_out",           L("Zoom out"),                                             CANVAS, { 'O' }),
    SHORTCUT(Mouse3DSettings,   "mouse3d_settings",   L("Show/Hide 3Dconnexion devices settings dialog"),        CANVAS, MOUSE3D_CHORD),

    // Display
    SHORTCUT(ShowLabels,        "show_labels",        L("Show object labels in 3D scene."),                      GLOBAL,  { 'E', CTRL }),
    SHORTCUT(ShowWireframe,     "show_wireframe",     L("Show/Hide wireframe"),                                  CANVAS,  { WXK_RETURN, CTRL_SHIFT }),
    SHORTCUT(ToggleGcodeWindow,   "toggle_gcode_window",   L("On/Off G-code window"),                            PREVIEW, { 'C' }),
    SHORTCUT(ToggleOneLayerMode,  "toggle_one_layer_mode", L("On/Off one layer mode of the vertical slider"),    PREVIEW, { 'L' }),

    // Application
    SHORTCUT(Preferences,       "preferences",        L("Preferences"),                                          GLOBAL, PREFERENCES_CHORD),
    SHORTCUT(Search,            "search",             L("Search"),                                               GLOBAL, { 'F', CTRL }),
    SHORTCUT(SwitchView,        "switch_view",        L("Switch between Prepare/Preview"),                       CANVAS, { WXK_TAB }),
    SHORTCUT(CollapseSidebar,   "collapse_sidebar",   L("Collapse/Expand the sidebar"),                          CANVAS, { WXK_TAB, SHIFT }),
    SHORTCUT(ReloadDevicePage,  "reload_device_page", L("Reload the device page"),                               CANVAS, { WXK_F5 }),
    SHORTCUT(KeyboardShortcuts, "keyboard_shortcuts", L("Show keyboard shortcuts list"),                         CANVAS, { '?' }),

    // Speed Dial
    SHORTCUT(SpeedDial,         "speed_dial",         L("Open the Speed Dial"),                                  GLOBAL, { WXK_SPACE }),
}};

#undef SHORTCUT
#undef REPEATING
#undef STEPPING

// The first shortcut of each section's run in shortcut_table and the section's heading, indexed
// by ShortcutSection.
struct SectionInfo
{
    Shortcut    first;
    const char* name;
};

constexpr std::array<SectionInfo, size_t(ShortcutSection::Count)> section_table = {{
    { Shortcut::NewProject,      L("Project") },
    { Shortcut::SlicePlate,      L("Slicing and printing") },
    { Shortcut::SelectAll,       L("Selection") },
    { Shortcut::Undo,            L("Editing") },
    { Shortcut::AddInstance,     L("Objects") },
    { Shortcut::Arrange,         L("Placement") },
    { Shortcut::GizmoMove,       L("Gizmos") },
    { Shortcut::GoToLayer,       L("Sliders") },
    { Shortcut::PaintToolCircle, L("Painting tools") },
    { Shortcut::ViewDefault,     L("Camera") },
    { Shortcut::ShowLabels,      L("Display") },
    { Shortcut::Preferences,     L("Application") },
    { Shortcut::SpeedDial,       L("Speed Dial") },
}};

constexpr bool sections_follow_table_order()
{
    if (section_table.front().first != Shortcut(0))
        return false;
    for (size_t i = 1; i < section_table.size(); ++i)
        if (section_table[i].first <= section_table[i - 1].first)
            return false;
    return true;
}
static_assert(sections_follow_table_order(), "section_table must begin at the first shortcut and ascend");

constexpr bool table_is_in_enum_order()
{
    for (size_t i = 0; i < shortcut_table.size(); ++i)
        if (shortcut_table[i].id != Shortcut(i))
            return false;
    return true;
}
static_assert(table_is_in_enum_order(), "shortcut_table must list every Shortcut in declaration order");

const char* CONFIG_SECTION = "shortcuts";
const char* UNBOUND        = "none";

bool share_context(uint8_t a, uint8_t b) { return (a & b) != 0 || (a & GLOBAL) != 0 || (b & GLOBAL) != 0; }

// The modifiers a modifier_variants shortcut accepts on top of its binding.
constexpr int STEP_MODIFIERS = wxMOD_SHIFT | wxMOD_CONTROL;

// The step modifiers chord adds to binding, 0 when chord is not a step of it; a binding with
// Shift or Ctrl of its own has no steps, so no two bindings share one.
int step_modifiers(const KeyChord& chord, const KeyChord& binding)
{
    if (!binding.valid() || (binding.modifiers & STEP_MODIFIERS) != 0 || chord.key != binding.key ||
        (chord.modifiers & ~STEP_MODIFIERS) != binding.modifiers)
        return 0;
    return chord.modifiers & STEP_MODIFIERS;
}

} // namespace

const ShortcutInfo& shortcut_info(Shortcut shortcut) { return shortcut_table[size_t(shortcut)]; }

ShortcutSection shortcut_section(Shortcut shortcut)
{
    size_t section = 0;
    for (size_t i = 1; i < section_table.size(); ++i)
        if (section_table[i].first <= shortcut)
            section = i;
    return ShortcutSection(section);
}

const char* section_name(ShortcutSection section) { return section_table[size_t(section)].name; }

std::vector<Shortcut> shortcuts_in(ShortcutContext context)
{
    std::vector<Shortcut> out;
    for (const ShortcutInfo& info : shortcut_table)
        if (info.contexts & context_bit(context))
            out.push_back(info.id);
    return out;
}

ShortcutRegistry::ShortcutRegistry() { rebuild_index(); }

KeyChord ShortcutRegistry::binding(Shortcut shortcut) const
{
    const std::optional<KeyChord>& override = m_overrides[size_t(shortcut)];
    return override.has_value() ? *override : shortcut_info(shortcut).default_chord;
}

bool ShortcutRegistry::is_customized(Shortcut shortcut) const { return m_overrides[size_t(shortcut)].has_value(); }

std::string ShortcutRegistry::display(Shortcut shortcut) const { return binding(shortcut).display(); }

std::string ShortcutRegistry::with_key(const std::string& text, Shortcut shortcut) const
{
    const std::string key = display(shortcut);
    return key.empty() ? text : text + " [" + key + "]";
}

std::string ShortcutRegistry::accelerator(Shortcut shortcut) const
{
    const KeyChord chord = binding(shortcut);
    return chord.is_menu_accelerator() ? chord.to_string() : std::string();
}

std::optional<Shortcut> ShortcutRegistry::lookup(ShortcutContext context, const KeyChord& chord) const
{
    if (!chord.valid())
        return std::nullopt;
    auto it = m_index.find(chord);
    if (it == m_index.end())
        return std::nullopt;
    for (Shortcut shortcut : it->second)
        if (shortcut_info(shortcut).contexts & context_bit(context))
            return shortcut;
    return std::nullopt;
}

std::optional<ShortcutRegistry::Match> ShortcutRegistry::match(ShortcutContext context, const KeyChord& chord) const
{
    if (const std::optional<Shortcut> exact = lookup(context, chord); exact.has_value())
        return Match{ *exact, 0 };
    if ((chord.modifiers & STEP_MODIFIERS) == 0)
        return std::nullopt;
    for (const ShortcutInfo& info : shortcut_table)
        if (info.modifier_variants && (info.contexts & context_bit(context)))
            if (const int step = step_modifiers(chord, binding(info.id)); step != 0)
                return Match{ info.id, step };
    return std::nullopt;
}

std::vector<Shortcut> ShortcutRegistry::conflicts(Shortcut shortcut, const KeyChord& chord) const
{
    std::vector<Shortcut> out;
    if (!chord.valid())
        return out;
    const uint8_t contexts = shortcut_info(shortcut).contexts;
    for (const ShortcutInfo& other : shortcut_table)
        if (other.id != shortcut && share_context(contexts, other.contexts) && binding(other.id) == chord)
            out.push_back(other.id);
    return out;
}

std::optional<Shortcut> ShortcutRegistry::step_owner(Shortcut shortcut, const KeyChord& chord) const
{
    const uint8_t contexts = shortcut_info(shortcut).contexts;
    for (const ShortcutInfo& other : shortcut_table)
        if (other.modifier_variants && other.id != shortcut && share_context(contexts, other.contexts))
            if (const int step = step_modifiers(chord, binding(other.id)); step == wxMOD_SHIFT || step == wxMOD_CONTROL)   // the combined step stays assignable
                return other.id;
    return std::nullopt;
}

void ShortcutRegistry::bind(Shortcut shortcut, const KeyChord& chord)
{
    const ShortcutInfo& info = shortcut_info(shortcut);
    if (chord == info.default_chord)
        m_overrides[size_t(shortcut)].reset();
    else
        m_overrides[size_t(shortcut)] = chord;
    rebuild_index();
}

void ShortcutRegistry::reset(Shortcut shortcut)
{
    m_overrides[size_t(shortcut)].reset();
    rebuild_index();
}

void ShortcutRegistry::reset_all()
{
    m_overrides.fill(std::nullopt);
    rebuild_index();
}

void ShortcutRegistry::load(const AppConfig& config)
{
    m_overrides.fill(std::nullopt);
    if (config.has_section(CONFIG_SECTION)) {
        const std::map<std::string, std::string>& section = config.get_section(CONFIG_SECTION);
        for (const ShortcutInfo& info : shortcut_table) {
            auto it = section.find(info.key);
            if (it == section.end())
                continue;
            if (it->second == UNBOUND)
                m_overrides[size_t(info.id)] = KeyChord{};
            else if (std::optional<KeyChord> chord = KeyChord::parse(it->second); chord.has_value())
                // A Global chord is seen before any text field, so it needs a modifier or a non-typing key.
                if ((info.contexts & GLOBAL) == 0 || chord->is_menu_accelerator())
                    m_overrides[size_t(info.id)] = *chord;
        }
    }
    rebuild_index();
}

void ShortcutRegistry::save(AppConfig& config) const
{
    for (const ShortcutInfo& info : shortcut_table) {
        const std::optional<KeyChord>& override = m_overrides[size_t(info.id)];
        if (override.has_value())
            config.set(CONFIG_SECTION, info.key, override->valid() ? override->to_string() : UNBOUND);
        else
            config.erase(CONFIG_SECTION, info.key);
    }
}

void ShortcutRegistry::rebuild_index()
{
    m_index.clear();
    for (const ShortcutInfo& info : shortcut_table)
        if (const KeyChord chord = binding(info.id); chord.valid())
            m_index[chord].push_back(info.id);
}

}} // namespace Slic3r::GUI
