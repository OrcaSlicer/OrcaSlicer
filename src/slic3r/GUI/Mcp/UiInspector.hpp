#pragma once

#include "UiRefs.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"

#include <nlohmann/json.hpp>

#include <wx/gdicmn.h>
#include <wx/weakref.h>
#include <wx/window.h>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class wxBookCtrlBase;
class wxMenu;
class wxTopLevelWindow;

namespace Slic3r { namespace GUI {

class GLCanvas3D;

namespace Mcp {

class McpService;

// What a ref or locator points at.
struct UiTarget
{
    enum class Kind { Window, MenuItem, ToolbarItem, Gizmo, ImGuiItem };
    Kind                kind = Kind::Window;
    wxWeakRef<wxWindow> window;        // Window: the window; others: the frame or GL canvas window
    GLCanvas3D*         canvas = nullptr;
    int                 menu_id = -1;  // MenuItem
    std::string         menu_path;     // "File>Export>Export G-code"
    std::string         name;          // ToolbarItem name, Gizmo name, notebook page text
    int                 page = -1;     // Window: a page of the notebook `window`
    int                 gizmo = -1;
    unsigned            imgui_id = 0;  // ImGuiItem
    std::string         imgui_label, imgui_window; // the label it is listed by (see imgui_display_labels()), its window's full name
    std::string         imgui_name;                // see imgui_item_name()
    int                 imgui_twins = 0;           // items with its name in its window when found (see imgui_find_item())
    float               x = 0.f, y = 0.f, w = 0.f, h = 0.f; // ImGuiItem rectangle in ImGui coordinates
    nlohmann::json      locator;
};

struct SnapshotOptions
{
    std::string root = "all";   // all | main | dialog | <ref of a window>
    int         max_nodes = 400;
    bool        include_imgui = true;
    bool        include_menus = true;
    bool        visible_only  = true;
    std::string query;
};

struct ClickOptions
{
    std::string button = "left"; // left | right | middle
    bool        double_click = false;
    std::vector<std::string> modifiers; // ctrl, shift, alt, cmd
    // Canvas coordinates (logical pixels) when clicking a point of a 3D canvas.
    bool        at_point = false;
    int         x = 0, y = 0;
};

// Lists and drives the GUI for the ui_* tools: wx widgets of every shown top-level window, the
// menus, the 3D canvases' toolbar items, gizmos and ImGui items. Input is synthesized as wx
// events delivered to the widgets' own handlers (no OS input, focus or pointer needed), with
// semantic activation for controls whose handlers listen to command events. UI thread only.
class UiInspector
{
public:
    explicit UiInspector(McpService& service) : m_service(service) {}

    // {tree, nodes, omitted}
    nlohmann::json snapshot(const SnapshotOptions& options);
    // A ref ("w12") or a locator object. Throws ToolFailure.
    UiTarget resolve(const nlohmann::json& target);
    // The locator that finds `target` again in another session, with "index" when it alone would
    // match several elements. What the journal records.
    nlohmann::json locator_for(const UiTarget& target);

    nlohmann::json click(const UiTarget& target, const ClickOptions& options);
    nlohmann::json input(const UiTarget& target, const nlohmann::json& value, bool submit);
    nlohmann::json key(const UiTarget* target, const std::string& chord, const std::string& text);
    // Where keys without a target go: the focused window inside the topmost modal dialog or that
    // dialog itself while one is open, else the focused window, else the current 3D canvas.
    wxWindow* key_target(const UiTarget* target) const;
    // Mouse gestures on a 3D canvas: move, down, up, click, drag, wheel, at canvas pixels or at
    // points of the model. `owner` started the call: "" for a client, else the job of the script
    // running it, which a held press belongs to (see release_held()). It throws before the first
    // event when a point does not resolve; the caller ends a press held from before then.
    nlohmann::json mouse(GLCanvas3D& canvas, const nlohmann::json& gesture, const std::string& owner);
    // Ends a press a `down` left held, with its button-up and the key-ups of its modifiers: the one
    // `owner` started, or any when `owner` is null.
    void release_held(const std::string* owner);
    // Scrolls a widget into view, as clicks and typing do before they act. An ImGui item outside
    // the visible part of its panel is scrolled into it, and the target's rectangle updated.
    void reveal(UiTarget& target);
    // The 3D canvas a target is in (or the current one when `target` is null).
    GLCanvas3D* canvas_of(const UiTarget* target) const;

private:
    void walk(wxWindow* window, int depth, std::vector<UiNode>& nodes, std::size_t& omitted, const SnapshotOptions& options);
    void add_menus(wxMenu* menu, const std::string& path, wxWindow* frame, int depth, std::vector<UiNode>& nodes, std::size_t& omitted,
                   const SnapshotOptions& options);
    void add_canvas_items(GLCanvas3D& canvas, int depth, std::vector<UiNode>& nodes, std::size_t& omitted, const SnapshotOptions& options);
    bool push(UiNode node, UiTarget target, std::vector<UiNode>& nodes, std::size_t& omitted, const SnapshotOptions& options);
    // What a locator finds, in snapshot order; resolve() and locator_for() share them.
    std::vector<wxWindow*>                                          match_windows(const nlohmann::json& locator);
    std::vector<std::pair<GLCanvas3D*, ImGuiWrapper::RecordedItem>> match_imgui(const nlohmann::json& locator);
    void add_imgui_items(GLCanvas3D& canvas, int depth, std::vector<UiNode>& nodes, std::size_t& omitted, const SnapshotOptions& options);
    // The item's record in the canvas's last frame; throws when it is gone.
    ImGuiWrapper::RecordedItem imgui_record(const UiTarget& target) const;
    nlohmann::json             imgui_input(UiTarget target, const nlohmann::json& value, bool submit);
    std::vector<std::pair<wxBookCtrlBase*, int>>                    match_tabs(const nlohmann::json& locator);

    // A press a `down` left held, with the modifiers it holds.
    struct HeldGesture
    {
        GLCanvas3D* canvas = nullptr;
        wxPoint     pos;
        int         mods = 0;
        bool        left = false, right = false, middle = false;
        std::string owner;
    };

    McpService&                m_service;
    RefTable<UiTarget>         m_refs;
    std::optional<HeldGesture> m_held;
};

// The canvases of the plater that are alive now: the 3D view, the preview and the assembly view.
std::vector<GLCanvas3D*> live_canvases();
// "3d", "preview" or "assemble".
const char* canvas_name(GLCanvas3D* canvas);
// The stable English name automation uses for a gizmo (GLGizmosManager::EType).
std::string gizmo_name(int type);

// What each recorded ImGui item is listed and found by: its shown label, or for a field with none
// (a slider, drag, input or combo) the text left of it on its row, which only the first field
// after that text takes. Empty when neither exists. Parallel to `items`.
std::vector<std::string> imgui_display_labels(const std::vector<ImGuiWrapper::RecordedItem>& items,
                                              const std::vector<ImGuiWrapper::RecordedText>& texts);
// The name of a recorded item: the one record_named_item() gave it, else its label's id part.
std::string imgui_item_name(const ImGuiWrapper::RecordedItem& item);
// How many of `items` in `window` have `name`; 0 for no name.
int imgui_twins(const std::vector<ImGuiWrapper::RecordedItem>& items, const std::string& name, const std::string& window);
// Where an item drawn earlier is among `items`: by its id, or, when its label shows its value and so
// changes its id ("1 selected##select_tool_volume", the Text gizmo's font button), by its name if
// one item in `window` has it. `twins` is imgui_twins() when it was found, 0 if unknown: controls
// drawn without an ImGui item take their ids by order among the same-named ones in their ID scope
// (ImGuiWrapper::record_named_item()), so once that number changes an id may be another's, and no
// item is found.
std::optional<std::size_t> imgui_find_item(const std::vector<ImGuiWrapper::RecordedItem>& items,
                                           unsigned                                        id,
                                           const std::string&                              name,
                                           const std::string&                              window,
                                           int                                             twins = 0);
// Whether a field showing `shown` holds `wanted`: numbers equal at the precision the field prints
// (units after the number ignored), anything else exactly.
bool imgui_value_matches(const std::string& shown, const nlohmann::json& wanted);

}}} // namespace Slic3r::GUI::Mcp
