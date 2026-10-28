#pragma once

#include "UiRefs.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"

#include <nlohmann/json.hpp>

#include <wx/weakref.h>
#include <wx/window.h>

#include <cstddef>
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
    std::string         imgui_label, imgui_window;
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
    // Mouse gestures on a 3D canvas: move, down, up, click, drag, wheel.
    nlohmann::json mouse(GLCanvas3D& canvas, const nlohmann::json& gesture);
    // Scrolls a widget into view, as clicks and typing do before they act.
    void reveal(const UiTarget& target);
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
    std::vector<std::pair<wxBookCtrlBase*, int>>                    match_tabs(const nlohmann::json& locator);

    McpService&         m_service;
    RefTable<UiTarget>  m_refs;
};

// The canvases of the plater that are alive now: the 3D view, the preview and the assembly view.
std::vector<GLCanvas3D*> live_canvases();

}}} // namespace Slic3r::GUI::Mcp
