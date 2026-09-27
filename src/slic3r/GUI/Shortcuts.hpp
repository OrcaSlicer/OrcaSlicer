#pragma once

#include "KeyChord.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Slic3r {

class AppConfig;

namespace GUI {

// Where a key press is looked up. A shortcut may belong to several contexts; Global ones are
// dispatched by the main frame before any child window sees the key.
enum class ShortcutContext : uint8_t { Global, Plater, Preview, ObjectList, Painting, Count };

constexpr uint8_t context_bit(ShortcutContext context) { return uint8_t(1u << unsigned(context)); }

enum class Shortcut : uint8_t {
    // Project
    NewProject, OpenProject, SaveProject, SaveProjectAs, ImportModel, Publish3mf,
    // Slicing and printing
    SlicePlate, ExportSlicedFile, PrintPlate, PrintHostQueue,
    // Selection
    SelectAll, SelectAllPlates,
    // Editing
    Undo, Redo, Cut, Copy, Paste, DeleteSelected, DeleteAll, CloneSelected,
    // Objects
    AddInstance, RemoveInstance, TogglePrintable, ToggleAutoDrop,
    // Placement
    Arrange, ArrangePlate, Orient, OrientPlate,
    MoveSelectionLeft, MoveSelectionRight, MoveSelectionUp, MoveSelectionDown, RotateSelectionLeft, RotateSelectionRight,
    // Gizmos
    GizmoMove, GizmoRotate, GizmoScale, GizmoFlatten, GizmoCut, GizmoMeshBoolean, GizmoFdmSupports, GizmoSeam, GizmoFuzzySkin,
    GizmoMmuSegmentation, GizmoEmboss, GizmoMeasure, GizmoAssembly, GizmoBrimEars,
    // Sliders
    GoToLayer, LayerSliderUp, LayerSliderDown, MovesSliderLeft, MovesSliderRight, MovesSliderStart, MovesSliderEnd,
    // Painting tools
    PaintToolCircle, PaintToolSphere, PaintToolFill, PaintToolGapFill, PaintToolTriangle, PaintToolHeightRange,
    // Camera
    ViewDefault, ViewTop, ViewBottom, ViewFront, ViewRear, ViewLeft, ViewRight, ViewPlate, ZoomIn, ZoomOut, Mouse3DSettings,
    // Display
    ShowLabels, ShowWireframe, ToggleGcodeWindow, ToggleOneLayerMode,
    // Application
    Preferences, Search, SwitchView, CollapseSidebar, ReloadDevicePage, KeyboardShortcuts,
    // Speed Dial
    SpeedDial,
    Count
};

struct ShortcutInfo
{
    Shortcut    id;
    const char* key;          // AppConfig key
    const char* name;         // untranslated description
    uint8_t     contexts;     // context_bit() mask
    KeyChord    default_chord;
    bool        repeatable;   // runs on key auto-repeat as well
    // Shift or Ctrl held with the bound key select a variant of this action, such as the finer
    // move step or the faster slider step.
    bool        modifier_variants;
};

// Headings of the shortcuts dialog, in listing order.
enum class ShortcutSection : uint8_t {
    Project, SlicingAndPrinting, Selection, Editing, Objects, Placement, Gizmos, Sliders, PaintingTools, Camera, Display, Application, SpeedDial,
    Count
};

const ShortcutInfo&   shortcut_info(Shortcut shortcut);
ShortcutSection       shortcut_section(Shortcut shortcut);
const char*           section_name(ShortcutSection section);   // untranslated heading
std::vector<Shortcut> shortcuts_in(ShortcutContext context);   // in table order

// Effective key bindings: the built-in defaults overlaid with the user's own. Owns the
// chord -> shortcut index every dispatcher queries.
class ShortcutRegistry
{
public:
    ShortcutRegistry();

    KeyChord binding(Shortcut shortcut) const;   // invalid when unbound
    bool     is_customized(Shortcut shortcut) const;

    std::string display(Shortcut shortcut) const;      // "Ctrl+N" for tooltips and the shortcuts dialog
    std::string with_key(const std::string& text, Shortcut shortcut) const;   // "text [Ctrl+N]", or text alone when unbound
    std::string accelerator(Shortcut shortcut) const;  // wx accelerator text for menu labels; empty when unbound or not menu-safe

    // Matches a key event in one context. Global shortcuts are only found through the Global context.
    std::optional<Shortcut> lookup(ShortcutContext context, const KeyChord& chord) const;

    // lookup(), then the modifier_variants shortcuts, which also match with Shift or Ctrl added
    // to their binding; step_modifiers holds whichever of the two were added.
    struct Match
    {
        Shortcut shortcut;
        int      step_modifiers;
    };
    std::optional<Match> match(ShortcutContext context, const KeyChord& chord) const;

    // Shortcuts other than shortcut bound to chord in a context it shares; Global shortcuts
    // share every context.
    std::vector<Shortcut> conflicts(Shortcut shortcut, const KeyChord& chord) const;

    // The modifier_variants shortcut in a shared context whose Shift or Ctrl step is chord;
    // such a chord is reserved for that step.
    std::optional<Shortcut> step_owner(Shortcut shortcut, const KeyChord& chord) const;

    void bind(Shortcut shortcut, const KeyChord& chord);  // an invalid chord unbinds
    void reset(Shortcut shortcut);
    void reset_all();

    void load(const AppConfig& config);
    void save(AppConfig& config) const;

private:
    void rebuild_index();

    std::array<std::optional<KeyChord>, size_t(Shortcut::Count)>         m_overrides;
    std::unordered_map<KeyChord, std::vector<Shortcut>, KeyChordHash> m_index;
};

} // namespace GUI
} // namespace Slic3r
