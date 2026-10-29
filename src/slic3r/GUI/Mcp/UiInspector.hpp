#pragma once

#include "CanvasTarget.hpp"
#include "UiRefs.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"

#include <imgui/imgui.h>
#include <nlohmann/json.hpp>

#include <wx/dataview.h>
#include <wx/gdicmn.h>
#include <wx/weakref.h>
#include <wx/window.h>

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class wxBookCtrlBase;
class wxEventLoopBase;
class wxIdleEvent;
class wxMenu;
class wxTopLevelWindow;
struct ImGuiWindow;

namespace Slic3r {

class GLVolume;

namespace GUI {

class GLCanvas3D;
class GLToolbar;
class GLToolbarItem;

namespace Mcp {

class McpService;

// What a ref or locator points at.
struct UiTarget
{
    enum class Kind { Window, MenuItem, ToolbarItem, Gizmo, ImGuiItem, Row, PlateIcon };
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
    // Row of the list `window`, by its labels from the top and how many earlier rows of the list have
    // the same labels; found again for each use, since lists rebuild their rows. `column` names the
    // cell to act on, empty for the row's first.
    std::vector<std::string> row_path;
    int                      row_twin = 0;
    std::string              column;
    // PlateIcon of the Prepare view's `canvas`: the plate and the icon's picking action (plate_icon_action()).
    int                      plate = -1, plate_action = -1;
    nlohmann::json      locator;
};

// A row of a list, tree, table or drop-down list, as ui_snapshot lists it and row locators find it.
struct ListRow
{
    std::vector<std::string> path;        // labels from the top: the row's own label last
    std::string              value;       // its other cells' text, joined with " | "
    std::string              note;        // what the row stands for (object list rows, by model indices)
    int                      checked = -1; // its checkbox cell: 0/1; -1 for none
    bool                     enabled = true, selected = false;
    bool                     shown   = true; // every tree branch above it is expanded
    int                      depth   = 0;    // nesting below the list
    int                      twin    = 0;    // earlier rows of the list with the same path
    wxDataViewItem           item;           // a data view's row
    int                      index   = -1;   // the position of a row of any other list
};

struct SnapshotOptions
{
    std::string root = "all";   // all | main | dialog | popup | <ref of a window>
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
    // Logical pixels from the target's top left: a point of a 3D canvas or of a custom-drawn widget.
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
    ~UiInspector() { capture_menus(false); }
    UiInspector(const UiInspector&)            = delete;
    UiInspector& operator=(const UiInspector&) = delete;

    // {tree, nodes, omitted}
    nlohmann::json snapshot(const SnapshotOptions& options);
    // A ref ("w12") or a locator object. Throws ToolFailure.
    UiTarget resolve(const nlohmann::json& target);
    // The locator that finds `target` again in another session, with "index" when it alone would
    // match several elements. What the journal records.
    nlohmann::json locator_for(const UiTarget& target);

    nlohmann::json click(const UiTarget& target, const ClickOptions& options);
    // Scrolls a data view row to view and says whether it has its place there yet: GTK lays a list's
    // rows out a frame after they change. Any other target has its place.
    bool place_row(const UiTarget& target);
    nlohmann::json input(const UiTarget& target, const nlohmann::json& value, bool submit);
    nlohmann::json key(const UiTarget* target, const std::string& chord, const std::string& text);
    // Where keys without a target go: the focused window inside the topmost modal dialog or that
    // dialog itself while one is open, else the focused window, else the 3D view shown.
    wxWindow* key_target(const UiTarget* target) const;
    // Mouse gestures on a 3D canvas: move, down, up, click, drag, wheel, at canvas pixels or at
    // points of the model (not on the Design view, whose model is not Prepare's). `owner` started the call: "" for a client, else the job of the script
    // running it, which a held press belongs to (see release_held()). It throws before the first
    // event when a point does not resolve; the caller ends a press held from before then.
    nlohmann::json mouse(GLCanvas3D& canvas, const nlohmann::json& gesture, const std::string& owner);
    // Ends a press a `down` left held, with its button-up and the key-ups of its modifiers: the one
    // `owner` started, or any when `owner` is null.
    void release_held(const std::string* owner);
    // Scrolls a widget into view, as clicks and typing do before they act. An ImGui item outside
    // the visible part of its panel is scrolled into it, and the target's rectangle updated.
    void reveal(UiTarget& target);
    // The 3D canvas a target is in (or the one shown when `target` is null).
    GLCanvas3D* canvas_of(const UiTarget* target) const;

    // Popup and context menus while MCP acts on the UI. A menu that opens through popup_menu() is
    // taken instead of shown, since a native menu loop would hold the UI thread until a person
    // closed it. A call's window for this runs from the start of each of its UI closures until the
    // first idle event with nothing queued after the closure ends, so menus that its queued events
    // open are taken too; between calls a person's menu shows. Only menus of the event loop a
    // closure ran in are taken: a person's right click in a dialog that a call opened shows its menu.
    // An input call arms a capture (arm_menu()), which the first menu it opens goes to: listed, or
    // the item it names run. Any other menu is closed unseen and kept for closed_menus().

    // Installs the capture in SyntheticInput, or removes it.
    void capture_menus(bool on);
    // UiInvoker's activity hook: a closure of `tool` starts or ends.
    void ui_activity(const std::string& tool, bool running);
    // Arms a capture in the active event loop, for the item `path` names ("Add Modifier>Cylinder"),
    // or for a listing when it is empty. Returns the token for disarm_menu().
    int arm_menu(const std::string& path);
    // Ends a capture: {popup_menu: listing} for a menu it listed, {ran: path} for the item it ran,
    // either with menus_closed when more menus opened after it; {} when no menu opened. Throws
    // ToolFailure when no menu opened for a `path`, or the menu had no item it could run.
    nlohmann::json disarm_menu(int token);
    // Ends a capture without reading it, when the input failed.
    void drop_menu(int token);
    // The last menus closed unseen, oldest first: {tool, ago_s, items, disabled?, checked?}.
    nlohmann::json closed_menus() const;
    // SyntheticInput's capture: true when it took `menu`, which `owner` was about to show.
    bool on_menu(wxMenu& menu, wxWindow& owner);

private:
    void walk(wxWindow* window, int depth, std::vector<UiNode>& nodes, std::size_t& omitted, const SnapshotOptions& options);
    void add_menus(wxMenu* menu, const std::string& path, wxWindow* frame, int depth, std::vector<UiNode>& nodes, std::size_t& omitted,
                   const SnapshotOptions& options);
    void add_canvas_items(GLCanvas3D& canvas, int depth, std::vector<UiNode>& nodes, std::size_t& omitted, const SnapshotOptions& options);
    bool push(UiNode node, UiTarget target, std::vector<UiNode>& nodes, std::size_t& omitted, const SnapshotOptions& options);
    // What a locator finds, in snapshot order; resolve() and locator_for() share them.
    std::vector<wxWindow*>                                          match_windows(const nlohmann::json& locator);
    std::vector<std::pair<GLCanvas3D*, ImGuiWrapper::RecordedItem>> match_imgui(const nlohmann::json& locator);
    std::vector<std::pair<wxWindow*, ListRow>>                      match_rows(const nlohmann::json& locator);
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

    // A call's window for taking menus: a closure running, or one that ended before the next idle and
    // at most kMenuWindowGrace ago.
    static constexpr std::chrono::seconds kMenuWindowGrace{1};
    struct MenuWindow
    {
        wxEventLoopBase*                      loop = nullptr;
        std::string                           tool;
        bool                                  running = true;
        std::chrono::steady_clock::time_point ended;
    };
    struct MenuCapture
    {
        int              token = 0;
        std::string      path;
        wxEventLoopBase* loop  = nullptr;
        bool             fired = false;
        int              closed = 0;
        nlohmann::json   result;
        std::string      error;
        nlohmann::json   details;
    };
    struct ClosedMenu
    {
        std::string                           tool;
        std::chrono::steady_clock::time_point at;
        nlohmann::json                        listing;
    };
    void         on_idle(wxIdleEvent& event);
    MenuCapture* menu_capture(int token);

    bool                     m_capturing_menus = false;
    std::vector<MenuWindow>  m_menu_windows;
    std::vector<MenuCapture> m_menu_captures;
    std::deque<ClosedMenu>   m_closed_menus;
    int                      m_menu_tokens = 0;
};

// The top-level window `w` belongs to: its own, or for a widget in a popup (a drop-down list, the print
// button's menu), the one the popup opened from, which on every port is the popup's parent. Guards and
// `within` judge popup widgets by it.
wxWindow* owning_window(wxWindow* w);
// `w` is, or is inside, a top-level window named kHiddenFromAutomation.
bool hidden_from_automation(const wxWindow* w);
// The shown popups that opened from `top` or from its widgets, which ui_snapshot lists after it.
std::vector<wxWindow*> popups_of(wxWindow* top);

// Rows of the lists ui_snapshot reads: wxDataViewCtrl (every row, collapsed branches too), wxListBox and
// wxCheckListBox, Orca's DropDown, wxGrid (read-only) and the Design tab's DesignRowList. Empty for
// other windows.
std::vector<ListRow> list_rows(wxWindow* list);
bool                 is_row_list(wxWindow* w);
// The names a row's cell is picked by (`column`): header text, "#n" for a column without one, and for
// the object list, which shows no header, fixed English names. Empty for lists without columns.
std::vector<std::string> list_columns(wxWindow* list);
// The rows under `roots` (in snapshot order, popups after the window they belong to) that the labels
// `wanted` name, by row_matches().
std::vector<std::pair<wxWindow*, ListRow>> find_rows(const std::vector<wxWindow*>& roots, const std::vector<std::string>& wanted);
// Acts on a row as a person's click on its cell under `column` does: a data view row is selected as
// the click would select it, with one wxEVT_DATAVIEW_SELECTION_CHANGED for it, while the pointer is
// held over the cell for code that reads where it is; a double click activates it and a right click
// sends its context menu event. A list box row is selected, a drop-down row picked, a table row's cell
// made current, a Design list row clicked at its cell. Throws ToolFailure.
nlohmann::json click_row(wxWindow& list, const ListRow& row, const std::string& column, const ClickOptions& options);
// Ticks or clears a row's checkbox cell as a person's click on it does. Throws ToolFailure.
nlohmann::json input_row(wxWindow& list, const ListRow& row, const std::string& column, const nlohmann::json& value);

// The canvases of the plater that are alive now: the 3D view, the preview and the assembly view.
std::vector<GLCanvas3D*> live_canvases();
// The Design tab's view once the tab is built, else null.
GLCanvas3D* design_canvas();
// live_canvases() and the Design view: the canvases the ui_* tools list, find items in and send
// input to. The gizmo state and the idle checks read live_canvases(), since the Design view has no
// gizmos.
std::vector<GLCanvas3D*> ui_canvases();
// The 3D view on screen: the Design view while its tab is shown, else the plater's current canvas.
GLCanvas3D* shown_canvas();
// "3d", "preview", "assemble" or "design".
const char* canvas_name(GLCanvas3D* canvas);
// The canvas of a name canvas_name() gives; "" is the Prepare view. Null for an unknown name, and for
// "design" while the Design tab is not built.
GLCanvas3D* canvas_by_name(const std::string& name);
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

// The ImGui window a click at `pos` (ImGui coordinates) goes to instead of the scene, found as ImGui
// finds the hovered window: the frontmost one the last frame drew that is shown, takes mouse input
// and holds the point, with ImGui's hover padding and hit-test hole. Null when the click reaches the
// scene.
ImGuiWindow* imgui_window_at(const ImVec2& pos);
// What a reason calls an ImGui window: `the notification "<its first text>"` for a notification,
// `the "<title>" window` for another, "an ImGui window" for one without a title. `texts` are the
// recorded texts of its canvas.
std::string imgui_window_description(const std::string& window, const std::vector<ImGuiWrapper::RecordedText>& texts);

// The meshes a click on the model meets, as a person sees them (see check_surface()): shown parts
// that are active or of the instance the open gizmo works on (`own`). Modifiers and negative parts
// are see-through, except `target`, the part the click is for.
std::vector<SurfaceMesh> surface_meshes(const std::vector<GLVolume*>&             volumes,
                                        const GLVolume*                           target,
                                        const std::function<bool(const GLVolume*)>& own);

// A GL toolbar's items, with the visibility and enabled state the canvas's idle handler gives them
// (GLToolbar::update_items_state()) brought up to date first: until that handler runs they keep the
// state of the last idle event, so an item a `select` just enabled would still read disabled.
// `changed` tells whether a state changed, which the canvas has yet to draw.
const std::vector<GLToolbarItem*>& toolbar_items(GLToolbar& toolbar, bool* changed = nullptr);

}}} // namespace Slic3r::GUI::Mcp
