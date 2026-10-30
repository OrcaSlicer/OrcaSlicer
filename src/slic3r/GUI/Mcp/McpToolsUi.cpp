// GUI automation: look at the widget tree, click, type, press keys, drag on the 3D canvas, answer
// dialogs and run JavaScript in web views.

#include "JobRegistry.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "McpCore.hpp"
#include "McpPaths.hpp"
#include "McpService.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"
#include "ModalTracker.hpp"
#include "UiInspector.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/ExportPresetBundleDialog.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoSVG.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoTextureDisplacement.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PresetBundleDialog.hpp"
#include "slic3r/GUI/WebGuideDialog.hpp"

#include "libslic3r/Point.hpp"

#include <wx/app.h>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/glcanvas.h>
#include <wx/toplevel.h>
#include <wx/vector.h>
#include <wx/webview.h>
#include <wx/weakref.h>
#include <wx/window.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <optional>
#include <regex>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;
using namespace std::chrono_literals;

namespace {

const json kTargetSchema = {{"type", json::array({"string", "object"})},
                            {"description",
                             "A ref from the latest ui_snapshot (\"w12\") or a locator: {label, class, name, within, index} for "
                             "widgets, {option:\"layer_height\"} for a setting field, {menu:\"File>Export>Export G-code\"}, {gltoolbar:\"arrange\"}, {gizmo:\"Move\"}, "
                             "{imgui:\"label\", imgui_name?:\"name\", window?, canvas?, index?} for an ImGui item, by its label, its name or both, or "
                             "{row:\"Plate 1>Cube>Cylinder\", column?, within?, index?} for a row of a list, tree, table or drop-down list, by its "
                             "last labels and the column of the cell to act on, or {plate_icon:\"lock\", plate:0} for a plate or its icon in the "
                             "Prepare view: plate, delete, orient, arrange, lock, settings, name, move_to_front, filament_map, imex_mode."}};

const json kModifiersSchema = {{"type", "array"},
                               {"items", {{"type", "string"}, {"enum", {"ctrl", "cmd", "rawctrl", "shift", "alt"}}}},
                               {"description", "Modifier keys held during the click, as in ui_key: ctrl and cmd are both Command on macOS "
                                               "and Ctrl elsewhere; rawctrl is the macOS Control key."}};

bool in_top_dialog(McpService& service, wxWindow* window)
{
    wxDialog* top = service.modals().top();
    return top != nullptr && window != nullptr && owning_window(window) == top;
}

// GUI input tools run under the recover guard so they can work in the topmost modal dialog; a
// target anywhere else gets the checks a mutate call would: no other call's nested loop, no open
// modal, and operation admission (no MCP operation holding the project, no busy UI job worker).
void guard_target(McpService& service, wxWindow* window)
{
    if (in_top_dialog(service, window))
        return;
    service.ui().require_mutate();
    service.core().admit({});
}

// The window a target is in: a widget's own, a row's list.
wxWindow* target_window(const UiTarget& t)
{
    return t.kind == UiTarget::Kind::Window || t.kind == UiTarget::Kind::Row ? t.window.get() : nullptr;
}

// The journal replays locators, never refs: refs mean nothing in another session.
void journal_with_locator(ToolContext& ctx, const json& args, const json& locator)
{
    json recorded = args;
    if (recorded.contains("target") && recorded["target"].is_string() && !locator.is_null())
        recorded["target"] = locator;
    ctx.set_journal_args(std::move(recorded));
}

ToolDef recover_input_tool(std::string name, std::string title, std::string description, json schema, ToolHandler handler)
{
    ToolDef d = mutate_tool(std::move(name), std::move(title), std::move(description), std::move(schema), std::move(handler));
    d.guard   = GuardClass::Recover;
    return d;
}

const json kPopupItemSchema = prop("string", "Run this item of the menu the input opens (a right click's context menu, a button's menu), "
                                             "by its label path, \"Add Modifier>Cylinder\"; \"#2\" picks a repeated label's second item. "
                                             "Without it, such a menu is listed in the reply (popup_menu), not shown.");

// The menu an input call may open, which the inspector takes instead of showing it
// (UiInspector::arm_menu()): armed in the closure that delivers the input, read once the events the
// input queued have run. A call that fails before reading it drops it.
struct PopupCapture
{
    McpService&  service;
    ToolContext& ctx;
    std::string  item;
    int          token = 0;
    // Also reply the dialogs open once the input's events ran: an input whose work opens a dialog
    // from a posted event (a plate icon) returns before the dialog shows.
    bool         dialogs = false;
    // Set by a call queued on the 3D view behind the click, which the view posts to itself and opens
    // its menu from: once it has run, so has the click (or the click's command is in a dialog's loop,
    // which runs it too).
    std::shared_ptr<bool> marked;

    PopupCapture(McpService& service, ToolContext& ctx, std::string item) : service(service), ctx(ctx), item(std::move(item)) {}
    PopupCapture(const PopupCapture&)            = delete;
    PopupCapture& operator=(const PopupCapture&) = delete;
    ~PopupCapture()
    {
        if (token == 0)
            return;
        try {
            ctx.observe_ui([this] {
                service.inspector().drop_menu(token);
                return json();
            });
        } catch (const ToolFailure&) {
        }
    }

    // UI thread, in the input's closure, before the input.
    void arm() { token = service.inspector().arm_menu(item); }
    // UI thread, after an input to a 3D view, on the handler the input posted its work to.
    void mark(wxEvtHandler* queue)
    {
        if (queue == nullptr)
            return;
        marked = std::make_shared<bool>(false);
        queue->CallAfter([m = marked] { *m = true; });
    }
    // Job thread: settles, then {popup_menu} or {ran} (with the dialogs a command it ran left open),
    // or {} when no menu opened.
    json read()
    {
        for (int i = 0; marked && i < 100 && !ctx.observe_ui([this] { return json(*marked); }).get<bool>(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        settle(ctx);
        const int t = std::exchange(token, 0);
        return ctx.observe_ui([&] {
            json out = service.inspector().disarm_menu(t);
            if ((dialogs || out.contains("ran")) && service.modals().any_open())
                out["dialogs"] = service.modals().snapshot();
            return out;
        });
    }
};

// Every shown web view, main window first.
std::vector<wxWebView*> visible_webviews()
{
    std::vector<wxWebView*> out;
    std::function<void(wxWindow*)> visit = [&](wxWindow* w) {
        if (!w->IsShownOnScreen())
            return;
        if (auto* web = dynamic_cast<wxWebView*>(w)) {
            out.push_back(web);
            return;
        }
        // A dialog is its parent's child too; it is visited once, as a top-level window.
        for (wxWindow* child : w->GetChildren())
            if (dynamic_cast<wxTopLevelWindow*>(child) == nullptr)
                visit(child);
    };
    if (wxGetApp().mainframe != nullptr)
        visit(wxGetApp().mainframe);
    for (wxWindow* tlw : wxTopLevelWindows)
        if (tlw != wxGetApp().mainframe && tlw->GetName() != kHiddenFromAutomation)
            visit(tlw);
    return out;
}

// The modal dialogs whose web views take scripts: the setup wizard and the preset bundle dialogs,
// whose pages have no typed route. Sign-in dialogs are never among them, and the flushing volumes
// and filament colours are written through set_settings's project scope instead.
bool takes_scripts(const wxDialog* dialog)
{
    return dynamic_cast<const GuideFrame*>(dialog) != nullptr || dynamic_cast<const PresetBundleDialog*>(dialog) != nullptr ||
           dynamic_cast<const ExportPresetBundleDialog*>(dialog) != nullptr;
}

// The web view a script without a target runs in: the one in the topmost modal dialog while one is
// open, else the only one shown.
wxWebView* default_webview(McpService& service)
{
    std::vector<wxWebView*> webs = visible_webviews();
    std::string             where;
    if (wxDialog* top = service.modals().top()) {
        std::vector<wxWebView*> in_top;
        for (wxWebView* w : webs)
            if (owning_window(w) == top)
                in_top.push_back(w);
        if (!in_top.empty()) {
            webs  = std::move(in_top);
            where = " in \"" + into_u8(top->GetTitle()) + "\"";
        }
    }
    if (webs.empty())
        throw ToolFailure("No web view is shown (the Home tab has one: set_view tab=home)");
    if (webs.size() > 1) {
        json urls = json::array();
        for (wxWebView* w : webs)
            urls.push_back(into_u8(w->GetCurrentURL()));
        throw ToolFailure("Several web views are shown" + where + "; pass target (ui_snapshot query=webview)", {{"web_views", urls}});
    }
    return webs.front();
}

// A web view in the topmost modal dialog takes scripts when that dialog is one of takes_scripts(),
// one in a dialog under it is refused, and any other gets the checks a mutate call would.
void guard_script(McpService& service, wxWebView* web)
{
    const std::vector<wxDialog*>& stack = service.modals().stack();
    const auto                    in    = std::find(stack.begin(), stack.end(), owning_window(web));
    if (in != stack.end()) {
        const std::string title = into_u8((*in)->GetTitle());
        if (*in != stack.back())
            throw ToolFailure("\"" + title + "\" is under \"" + into_u8(stack.back()->GetTitle()) + "\"; answer the topmost dialog first");
        if (!takes_scripts(*in))
            throw ToolFailure("Scripts run in a dialog only in the setup wizard and the preset bundle dialogs, not in \"" + title + "\"");
    }
    guard_target(service, web);
}

// The script as webview_eval runs it. Messages the page posts to its host (window.wx.postMessage) while
// the script runs are held until it has returned: a host that ends its dialog on one (a page's Close or
// OK button) would otherwise destroy the web view before the result comes back, and wx's script
// callback, which keeps a plain pointer to the web view, would then reach a destroyed one. The result
// is JSON: the value as wx gives it (an object as JSON, undefined as "undefined") and how many messages
// were held.
std::string with_held_messages(const std::string& script)
{
    return "(function () { var host = window.wx, held = [], swapped = !!(host && host.postMessage);"
           " if (swapped) window.wx = {postMessage: function (m) { held.push(m); }};"
           " try { var res = eval(" + json(script).dump(-1, ' ', true) + ");"
           " return JSON.stringify({held: held.length, value: typeof res == 'object' ? JSON.stringify(res) :"
           " typeof res == 'undefined' ? 'undefined' : String(res)}); }"
           " finally { if (swapped) { window.wx = host; if (held.length) setTimeout(function () {"
           " held.forEach(function (m) { host.postMessage(m); }); }, 0); } } })()";
}

// Receives the result of one RunScriptAsync call into a State the job thread waits on, or the web
// view's destruction when its dialog went away while the script ran. Created, used and deleted on the
// UI thread only, like the weak reference it keeps to its web view.
class ScriptSink : public wxEvtHandler
{
public:
    struct State
    {
        std::mutex              mutex;
        std::condition_variable cv;
        bool                    done   = false;
        bool                    error  = false;
        bool                    destroyed = false; // the web view went away first
        std::string             text;
    };

    ScriptSink(wxWebView* web, std::shared_ptr<State> state) : m_web(web), m_state(std::move(state))
    {
        web->Bind(wxEVT_WEBVIEW_SCRIPT_RESULT, &ScriptSink::on_result, this);
        web->Bind(wxEVT_DESTROY, &ScriptSink::on_destroy, this);
    }
    ~ScriptSink() override
    {
        if (wxWebView* web = m_web.get()) {
            web->Unbind(wxEVT_WEBVIEW_SCRIPT_RESULT, &ScriptSink::on_result, this);
            web->Unbind(wxEVT_DESTROY, &ScriptSink::on_destroy, this);
        }
    }

    void on_destroy(wxWindowDestroyEvent& evt)
    {
        evt.Skip();
        if (evt.GetEventObject() != m_web.get())
            return;
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            if (m_state->done)
                return;
            m_state->done      = true;
            m_state->destroyed = true;
        }
        m_state->cv.notify_all();
    }

    void on_result(wxWebViewEvent& evt)
    {
        if (evt.GetClientData() != this) {
            evt.Skip();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_state->mutex);
            m_state->done  = true;
            m_state->error = evt.IsError();
            m_state->text  = into_u8(evt.GetString());
        }
        m_state->cv.notify_all();
    }

private:
    wxWeakRef<wxWebView>   m_web;
    std::shared_ptr<State> m_state;
};

// Deletes a sink on the UI thread whatever the guards say, so a dialog or a busy UI can't leak it.
void delete_on_ui(ScriptSink* sink)
{
    if (sink != nullptr && wxTheApp != nullptr)
        wxTheApp->CallAfter([sink] { delete sink; });
}

} // namespace

void register_ui_tools(McpService& service)
{
    ToolRegistry& reg = service.registry();

    {
        ToolDef def = inspect_tool(
            "ui_snapshot", L("List the GUI"),
            "Describe what is on screen as an indented tree, one element per line with a ref: wx widgets of every shown window "
            "(buttons, fields, checkboxes, combos, tabs, labels), the rows of lists, trees and tables (label, other cells, "
            "checkbox, what an object list row is), each shown popup after the window it opened from, the main menus, the 3D "
            "canvas's toolbar items and gizmos, and "
            "each ImGui panel drawn in the last frame with its items (kind, value, name) and text. Pass the refs to ui_click / "
            "ui_input / ui_key. Refs go stale when the "
            "element goes away; take a new snapshot then. root: \"all\" (default), \"main\", \"dialog\" (topmost modal), "
            "\"popup\" or a window's ref. query filters by label, value, name or class. Prefer run_action and the typed tools "
            "when one fits.",
            object_schema({{"root", prop("string", "all | main | dialog | popup | <ref of a window>.")},
                           {"query", prop("string", "Case-insensitive text to look for.")},
                           {"max_nodes", {{"type", "integer"}, {"minimum", 1}, {"maximum", 5000}, {"description", "Default 400."}}},
                           {"include_imgui", prop("boolean", "List ImGui items (default true).")},
                           {"include_menus", prop("boolean", "List menu items (default true).")}}),
            [&service](const json& args, ToolContext& ctx) {
                SnapshotOptions o;
                o.root          = arg_string(args, "root", "all");
                o.query         = arg_string(args, "query");
                o.max_nodes     = arg_int(args, "max_nodes", 400);
                o.include_imgui = arg_bool(args, "include_imgui", true);
                o.include_menus = arg_bool(args, "include_menus", true);
                json snap = ctx.run_ui([&] { return service.inspector().snapshot(o); });
                ToolResult r = ToolResult::ok();
                const std::string tree = snap["tree"].get<std::string>();
                snap.erase("tree");
                r.data = std::move(snap);
                r.content.push_back({{"type", "text"}, {"text", tree.empty() ? std::string("(nothing shown)") : tree}});
                return r;
            });
        reg.add(def);
    }

    reg.add(recover_input_tool(
        "ui_click", L("Click an element"),
        "Activate a GUI element: menu items, buttons, checkboxes, toggles, radio buttons, toolbar items and gizmos are activated "
        "the way their owners expect (a gizmo as its toolbar icon: it opens, a second click closes it, and Text on an object "
        "with no text creates a text part and replies created); other widgets and ImGui items get a synthesized mouse click, "
        "after the item is scrolled into view. A row is selected as a click on its cell selects it (ctrl adds, shift extends; "
        "the object list acts on the column named, as Printable toggles), a double click activates it, a right click opens "
        "its menu; a drop-down row is picked. A plate icon does what clicking it in the Prepare view does: the reply says the "
        "plate's lock state (locked), arrange and orient wait for their job (done), settings and filament_map list the dialog "
        "they open (dialogs), and a right click opens the plate menu. With x/y, clicks that point of a 3D canvas or a "
        "custom-drawn widget (logical pixels from its top left). Modifiers are seen by the 3D canvas, the "
        "object list and handlers that read the event. A menu the click opens is never shown: it is listed as popup_menu, "
        "or popup_item runs one of its items (ran). A click that opens a dialog returns blocked_by_dialog with the job.",
        object_schema({{"target", kTargetSchema},
                       {"button", enum_prop({"left", "right", "middle"}, "Default left.")},
                       {"double", prop("boolean", "Double click.")},
                       {"modifiers", kModifiersSchema},
                       {"x", prop("integer", "x of the point to click, in a 3D canvas or a custom-drawn widget.")},
                       {"y", prop("integer", "y of the point to click.")},
                       {"popup_item", kPopupItemSchema}},
                      {"target"}),
        [&service](const json& args, ToolContext& ctx) {
            ClickOptions o;
            o.button       = arg_string(args, "button", "left");
            o.double_click = arg_bool(args, "double", false);
            o.modifiers    = args.value("modifiers", std::vector<std::string>{});
            o.at_point     = args.contains("x") || args.contains("y");
            o.x            = arg_int(args, "x", 0);
            o.y            = arg_int(args, "y", 0);
            json         locator;
            GLCanvas3D*  canvas = nullptr;
            int          gizmo  = -1;
            PopupCapture menu(service, ctx, arg_string(args, "popup_item"));
            // A list's rows get their places a frame after they change (on GTK; a dialog's tree rebuilt
            // by a preset choice), so a row is scrolled to first and given up to 1 s to take its place.
            for (int i = 0; i < 20; ++i) {
                const bool placed = ctx.run_ui([&] {
                                         UiTarget t = service.inspector().resolve(args["target"]);
                                         guard_target(service, target_window(t));
                                         return json(service.inspector().place_row(t));
                                     }).get<bool>();
                if (placed)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            json out = ctx.run_ui([&] {
                UiTarget t = service.inspector().resolve(args["target"]);
                locator    = service.inspector().locator_for(t);
                guard_target(service, target_window(t));
                canvas = t.canvas;
                gizmo  = t.gizmo;
                menu.arm();
                json clicked = service.inspector().click(t, o);
                // A 3D view posts a click's work to itself; a plate icon posts its dialogs to the plater.
                const bool  plate_icon = t.kind == UiTarget::Kind::PlateIcon;
                GLCanvas3D* view       = service.inspector().canvas_of(&t);
                if (plate_icon && o.button == "left")
                    menu.mark(&ui_plater());
                else if (view != nullptr)
                    menu.mark(view->get_wxglcanvas());
                menu.dialogs = plate_icon;
                return clicked;
            });
            journal_with_locator(ctx, args, locator);
            if (out.value("started", false)) {
                // A plate's arrange or orient icon started a job on the UI job worker: the reply
                // follows its end.
                out.update(poll_ui(
                    ctx, [] { return ui_plater().get_ui_job_worker().is_idle() ? json{{"done", true}} : json(); },
                    std::chrono::minutes(30), "the plate's " + out.value("clicked", std::string()) + " job"));
            }
            if (out.value("creating", false)) {
                // The Text gizmo's icon with no text selected queued a job that creates a text part
                // and opens the gizmo when it ends.
                out = poll_ui(
                    ctx,
                    [&]() -> json {
                        if (!ui_plater().get_ui_job_worker().is_idle())
                            return json();
                        const auto canvases = live_canvases();
                        const bool active   = std::find(canvases.begin(), canvases.end(), canvas) != canvases.end() &&
                                            int(canvas->get_gizmos_manager().get_current_type()) == gizmo;
                        return json{{"gizmo", out["gizmo"]}, {"created", true}, {"active", active}};
                    },
                    std::chrono::seconds(30), "the new text part");
            }
            out.update(menu.read());
            return ToolResult::ok(out);
        }));

    reg.add(recover_input_tool(
        "ui_input", L("Set a control's value"),
        "Set the text, number, selection or checked state of a control and send the event a person's edit would: text fields "
        "(submit:true also presses Enter and leaves the field, which is when Orca's setting fields commit), spin controls, "
        "sliders, choice / combo / list boxes (by option text or index), checkboxes and toggles (true/false), a row's "
        "checkbox (true/false). An ImGui item "
        "is set by its kind and its value read back: a field is typed into (a slider through the number field beside it, "
        "multiline text with \\n), a combo's option is picked by its text or 0-based index, a checkbox, radio or toggle "
        "clicked when it differs. For "
        "settings, set_settings is more reliable.",
        object_schema({{"target", kTargetSchema},
                       {"value", {{"type", json::array({"string", "number", "boolean"})}, {"description", "The new value."}}},
                       {"submit", prop("boolean", "Press Enter / commit after setting (default true).")}},
                      {"target", "value"}),
        [&service](const json& args, ToolContext& ctx) {
            json locator;
            json out = ctx.run_ui([&] {
                UiTarget t = service.inspector().resolve(args["target"]);
                locator    = service.inspector().locator_for(t);
                guard_target(service, target_window(t));
                return service.inspector().input(t, args["value"], arg_bool(args, "submit", true));
            });
            journal_with_locator(ctx, args, locator);
            settle(ctx);
            return ToolResult::ok(out);
        }));

    reg.add(recover_input_tool(
        "ui_key", L("Press keys"),
        "Send a key chord (\"Ctrl+S\", \"Escape\", \"Return\", \"Delete\", \"Shift+Tab\", \"Left\", \"F5\") and/or type text to "
        "a target or, without one, to the focused window (the topmost dialog while one is open, the 3D view shown when nothing "
        "has focus). As with a real key press, the shortcut handling of the target's panels and window sees it first. "
        "Typing reaches ImGui fields and key handlers; to fill a native text field "
        "use ui_input. A menu the keys open is listed as popup_menu, or popup_item runs one of its items.",
        object_schema({{"target", kTargetSchema},
                       {"keys", prop("string", "Key chord.")},
                       {"text", prop("string", "Characters to type.")},
                       {"popup_item", kPopupItemSchema}}),
        [&service](const json& args, ToolContext& ctx) {
            const std::string keys = arg_string(args, "keys"), text = arg_string(args, "text");
            if (keys.empty() && text.empty())
                throw ToolFailure("Give keys, text or both");
            json         locator;
            PopupCapture menu(service, ctx, arg_string(args, "popup_item"));
            json out = ctx.run_ui([&] {
                std::optional<UiTarget> t;
                if (args.contains("target")) {
                    t       = service.inspector().resolve(args["target"]);
                    locator = service.inspector().locator_for(*t);
                }
                guard_target(service, service.inspector().key_target(t ? &*t : nullptr));
                menu.arm();
                return service.inspector().key(t ? &*t : nullptr, keys, text);
            });
            journal_with_locator(ctx, args, locator);
            out.update(menu.read());
            return ToolResult::ok(out);
        }));

    const json target_schema = {
        {"type", "object"},
        {"description", "A point of the model: {object, instance (default 0), volume (part, default 0), and point [x, y, z] in "
                        "the part's own mesh coordinates (frame \"volume\", the default) or the scene's (frame \"world\"), or "
                        "spot: top | bottom | front | back | left | right, where a ray along that axis first meets the part}."},
        {"properties",
         {{"object", {{"type", "integer"}, {"minimum", 0}}},
          {"instance", {{"type", "integer"}, {"minimum", 0}}},
          {"volume", {{"type", "integer"}, {"minimum", 0}}},
          {"point", array_prop({{"type", "number"}}, "[x, y, z] mm")},
          {"frame", enum_prop({"volume", "world"}, "Default volume.")},
          {"spot", enum_prop({"top", "bottom", "front", "back", "left", "right"}, "A side of the part.")}}},
        {"required", {"object"}},
        {"additionalProperties", false}};
    const json pixel_or_target = {{"type", json::array({"array", "object"})},
                                  {"description", "A canvas pixel [x, y] or a point of the model, as for at."}};
    std::vector<std::string> mouse_canvases{"3d", "preview", "assemble", "current"};
#ifdef SLIC3R_CAD
    mouse_canvases.insert(mouse_canvases.end() - 1, "design");
#endif
    reg.add(mutate_tool(
        "ui_mouse", L("Mouse gesture on the 3D view"),
        "Synthesize a mouse gesture on a 3D canvas, as a person's would: move, down, up, click, drag (from the start through "
        "`path` to `to`, `steps` moves per leg: on an object it moves it, in a painting gizmo it paints from a press on "
        "the object being painted (one drag whose path zigzags over an area paints all of it), and elsewhere it turns or pans the camera as this install's mouse settings say, so "
        "turn the camera with set_view camera or orbit instead) or wheel (delta in notches, positive zooms in or scrolls a panel up). Give "
        "the start as `at`, a point of the model, which is projected through the camera and by default checked to land on "
        "that part there (check \"point\" only projects it), or as x/y, logical pixels from the canvas's top left. "
        "Modifiers are pressed as keys for the gesture (shift for Measure's point picking, alt for erasing). A down keeps "
        "its button and modifiers held, so moves drag with it, until an up or another gesture. Grabbers are set through "
        "the gizmo's panel fields or transform, not dragged. A menu a right click opens is listed as popup_menu, or "
        "popup_item (with click or up) runs one of its items. camera_moved in the reply says the gesture moved the camera, "
        "so earlier screenshots and pixels no longer match the view.",
        object_schema({{"action", enum_prop({"move", "down", "up", "click", "drag", "wheel"}, "Default click.")},
                       {"canvas", enum_prop(mouse_canvases, "Default current, the view shown. design takes x/y only.")},
                       {"at", target_schema},
                       {"x", prop("integer", "Start x, instead of at.")},
                       {"y", prop("integer", "Start y, instead of at.")},
                       {"to", pixel_or_target},
                       {"path", array_prop(pixel_or_target, "Drag waypoints after the start, before to.")},
                       {"check", enum_prop({"surface", "point"}, "For model points: surface (default) or projection only.")},
                       {"tolerance", {{"type", "number"}, {"minimum", 0}, {"description", "Surface check distance in mm (default 1% of the part's size, at least 0.1)."}}},
                       {"steps", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}, {"description", "Moves per drag leg (default 10)."}}},
                       {"button", enum_prop({"left", "right", "middle"}, "Default left.")},
                       {"double", prop("boolean", "Double click.")},
                       {"delta", prop("integer", "Wheel notches.")},
                       {"modifiers", kModifiersSchema},
                       {"popup_item", kPopupItemSchema}}),
        [&service](const json& args, ToolContext& ctx) {
            const std::string action = arg_string(args, "action", "click");
            if (args.contains("popup_item") && action != "click" && action != "up")
                throw ToolFailure("popup_item goes with action click or up, which open a right click's menu");
            const std::string owner = service.script_owner(service.jobs().ancestry(ctx.job_id()));
            PopupCapture      menu(service, ctx, arg_string(args, "popup_item"));
            json out = ctx.run_ui([&] {
                // A client's press that runs after the endpoint closed would stay held, with no one left
                // to release it.
                if (owner.empty() && !service.endpoint_open())
                    throw ToolFailure("The MCP server was turned off before this gesture ran; it was not performed");
                try {
                    ui_plater(); // refuses while OrcaSlicer starts or closes
                    const std::string name   = arg_string(args, "canvas", "current");
                    GLCanvas3D*       canvas = name == "current" ? shown_canvas() : canvas_by_name(name);
                    if (canvas == nullptr || !canvas->get_wxglcanvas()->IsShownOnScreen())
                        throw ToolFailure(name == "design" ? "The Design view is not shown; set_view {\"tab\": \"design\"} first" :
                                                             "That 3D view is not shown; switch to it with set_view first");
                    // The Design view sets up its GL at its first frame, as DesignCanvas guards its own renders.
                    if (canvas == design_canvas() && !canvas->is_initialized())
                        throw ToolFailure("The Design view has not drawn yet");
                    // A gesture off the model turns, pans or zooms the camera as the user's mouse settings
                    // say, which leaves earlier screenshots and pixels showing another view.
                    const Camera&     camera = ui_plater().get_camera();
                    const Transform3d view   = camera.get_view_matrix();
                    const double      zoom   = camera.get_zoom();
                    menu.arm();
                    json done = service.inspector().mouse(*canvas, args, owner);
                    menu.mark(canvas->get_wxglcanvas());
                    if (canvas != design_canvas() && (!camera.get_view_matrix().isApprox(view) || camera.get_zoom() != zoom))
                        done["camera_moved"] = true;
                    return done;
                } catch (...) {
                    // A gesture that fails ends a press held from before it, as any other gesture would.
                    service.inspector().release_held(nullptr);
                    throw;
                }
            });
            out.update(menu.read());
            return ToolResult::ok(out);
        }));

    reg.add(mutate_tool(
        "gizmo_file", L("Use a file in the open gizmo"),
        "Do what a file picker inside the open gizmo does, with a path instead of its dialog: svg_change loads another SVG "
        "file into the SVG part being edited, svg_save_as writes that part's SVG to a new file (an existing one only with "
        "overwrite), texture_import imports a height-map image into the texture library and puts it on Texture "
        "Displacement's active layer. To add an SVG part, use run_action add_primitive_svg with a path. Gizmos open with "
        "ui_click {target: {gizmo: name}}, and their panels take ui_input / ui_click on the ImGui items ui_snapshot lists.",
        object_schema({{"action", enum_prop({"svg_change", "svg_save_as", "texture_import"}, "What to do.")},
                       {"path", prop("string", "Absolute path of the file to read or write.")},
                       {"overwrite", prop("boolean", "svg_save_as: replace an existing file.")}},
                      {"action", "path"}),
        [](const json& args, ToolContext& ctx) {
            const std::string action = arg_string(args, "action");
            json out = ctx.run_ui([&]() -> json {
                GLCanvas3D* canvas = ui_plater().get_current_canvas3D();
                if (canvas == nullptr)
                    throw ToolFailure("No 3D view is shown");
                GLGizmosManager&                 gizmos = canvas->get_gizmos_manager();
                const GLGizmosManager::EType     needed = action == "texture_import" ? GLGizmosManager::TextureDisplacement : GLGizmosManager::Svg;
                if (gizmos.get_current_type() != needed)
                    throw ToolFailure(needed == GLGizmosManager::Svg ? "Open the SVG gizmo on an SVG part first" :
                                                                       "Open the Texture displacement gizmo first");
                std::string err;
                const auto  path = action == "svg_save_as" ? resolve_output_path(arg_string(args, "path"), arg_bool(args, "overwrite", false), err) :
                                                             resolve_input_path(arg_string(args, "path"), err);
                if (!path)
                    throw ToolFailure(err);
                const std::string file = path_to_utf8(*path);
                if (action == "texture_import") {
                    if (!dynamic_cast<GLGizmoTextureDisplacement*>(gizmos.get_current())->import_texture(file, err))
                        throw ToolFailure(err);
                    return {{"imported", file}};
                }
                auto* svg = dynamic_cast<GLGizmoSVG*>(gizmos.get_current());
                if (action == "svg_change") {
                    if (!svg->change_file(file))
                        throw ToolFailure("The file has no shapes the SVG gizmo can use");
                    return {{"changed_to", file}};
                }
                if (!svg->save_svg_as(file))
                    throw ToolFailure("Could not write " + file);
                return {{"saved", file}};
            });
            settle(ctx);
            return ToolResult::ok(out);
        }));

    reg.add(recover_input_tool(
        "dialog_respond", L("Answer a dialog"),
        "Press a button of the topmost modal dialog (or of the one whose title contains dialog.title, which must be the "
        "topmost): yes, no, ok, cancel, close, apply, a button's label, or a numeric id. Native dialogs (system message "
        "boxes, file pickers) can't be answered here; register dialog_auto_answer before opening them.",
        object_schema({{"button", prop("string", "Button to press.")},
                       {"dialog", {{"type", "object"},
                                   {"properties", {{"title", prop("string", "Text the dialog's title contains (case-insensitive).")}}},
                                   {"additionalProperties", false},
                                   {"description", "Which dialog; default the topmost."}}}},
                      {"button"}),
        [&service](const json& args, ToolContext& ctx) {
            const std::string button = arg_string(args, "button");
            const std::string title  = args.contains("dialog") ? args["dialog"].value("title", "") : std::string();
            json out = ctx.run_ui([&]() -> json {
                const auto& stack = service.modals().stack();
                if (stack.empty())
                    throw ToolFailure("No modal dialog is open");
                wxDialog* dialog = stack.back();
                if (!title.empty()) {
                    auto contains = [&](wxDialog* d) {
                        std::string t = into_u8(d->GetTitle()), want = title;
                        std::transform(t.begin(), t.end(), t.begin(), ::tolower);
                        std::transform(want.begin(), want.end(), want.begin(), ::tolower);
                        return t.find(want) != std::string::npos;
                    };
                    if (!contains(dialog)) {
                        for (wxDialog* d : stack)
                            if (contains(d))
                                throw ToolFailure("\"" + into_u8(d->GetTitle()) + "\" is under \"" + into_u8(dialog->GetTitle()) +
                                                  "\"; answer the topmost dialog first");
                        throw ToolFailure("No open dialog's title contains \"" + title + "\"", {{"dialogs", service.modals().snapshot()}});
                    }
                }
                if (is_native_dialog(dialog))
                    throw ToolFailure("This is a native dialog, which can't be clicked; cancel the job's dialog by registering "
                                      "dialog_auto_answer before the action that opens it",
                                      {{"dialog", describe_dialog(dialog)}});
                const int id = resolve_dialog_button(dialog, button);
                if (id == wxID_NONE)
                    throw ToolFailure("The dialog has no button \"" + button + "\"", {{"dialog", describe_dialog(dialog)}});
                const std::string dialog_title = into_u8(dialog->GetTitle());
                const bool        pressed      = press_dialog_button(dialog, id);
                return {{"dialog", dialog_title}, {"button", button}, {"id", id}, {"pressed", pressed}};
            });
            settle(ctx);
            return ToolResult::ok(out);
        }));

    {
        ToolDef def = recover_input_tool(
            "dialog_auto_answer", L("Pre-answer dialogs"),
            "Answer modal dialogs automatically, native ones included, when their title or message matches a rule's regex "
            "(case-insensitive): a native dialog returns that button at once and never shows; a wx dialog has the button "
            "pressed as dialog_respond does, so its handler runs. A matching wx dialog that is already open is "
            "answered too, so a prompt that appears around the same time (the restore prompt after startup) is handled "
            "either way. Native file pickers can only be cancelled. Called from a script, the rules last until the script "
            "ends; otherwise they replace the rules this session set before (an empty list clears them).",
            object_schema({{"rules", array_prop({{"type", "object"},
                                                 {"properties", {{"match", prop("string", "Regex searched in \"title\\nmessage\".")},
                                                                 {"button", prop("string", "yes | no | ok | cancel | close | apply | label | id")}}},
                                                 {"required", {"match", "button"}},
                                                 {"additionalProperties", false}},
                                                "Rules, first match wins.")}},
                          {"rules"}),
            [&service](const json& args, ToolContext& ctx) {
                std::vector<AutoAnswerRule> rules;
                for (const json& r : args["rules"])
                    rules.push_back(make_rule(r["match"].get<std::string>(), r["button"].get<std::string>()));
                const std::vector<AutoAnswerRule> registered = rules;
                const std::string scope = service.set_auto_answer(std::move(rules), service.jobs().ancestry(ctx.job_id()));
                json out{{"scope", scope}, {"rules", service.modals().rules_json()}};
                json answered = ctx.run_ui([&]() -> json {
                    wxDialog* top = service.modals().top();
                    if (top == nullptr || is_native_dialog(top))
                        return json();
                    const json        info = describe_dialog(top);
                    const std::string text = info["title"].get<std::string>() + "\n" + info["message"].get<std::string>();
                    for (const AutoAnswerRule& rule : registered)
                        if (std::regex_search(text, rule.compiled))
                            if (const int id = resolve_dialog_button(top, rule.button); id != wxID_NONE) {
                                press_dialog_button(top, id);
                                return {{"dialog", info["title"]}, {"button", rule.button}};
                            }
                    return json();
                });
                if (!answered.is_null()) {
                    out["answered_open_dialog"] = std::move(answered);
                    settle(ctx);
                }
                return ToolResult::ok(std::move(out));
            });
        def.idempotent = true;
        reg.add(def);
    }

    reg.add(recover_input_tool(
        "webview_eval", L("Run JavaScript in a web view"),
        "Evaluate JavaScript in one of OrcaSlicer's web views (Home, the device page and others) and return the value of its "
        "last expression (JSON values come back parsed). While a modal dialog is open, scripts run only in the setup "
        "wizard's and the preset bundle dialogs' pages, never in a sign-in dialog; without a target, the web view of the "
        "topmost dialog is used, else the only shown one. Commit such a page through its own button, since dialog_respond "
        "only closes the dialog; closed: true says the script closed its dialog. Messages the page posts to OrcaSlicer "
        "reach it once the script has returned. Waits up to timeout_s (default 10).",
        object_schema({{"script", prop("string", "JavaScript to run.")},
                       {"target", kTargetSchema},
                       {"timeout_s", {{"type", "integer"}, {"minimum", 1}, {"maximum", 120}, {"description", "Default 10."}}}},
                      {"script"}),
        [&service](const json& args, ToolContext& ctx) {
            const std::string script  = arg_string(args, "script");
            const auto        timeout = std::chrono::seconds(arg_int(args, "timeout_s", 10));
            auto              state   = std::make_shared<ScriptSink::State>();
            ScriptSink*       sink    = nullptr; // the UI thread's to create and delete
            json              locator;
            const wxDialog*   modal   = nullptr; // the dialog the web view is in, only compared
            json started = ctx.run_ui([&]() -> json {
                wxWebView* web = nullptr;
                if (args.contains("target")) {
                    UiTarget t = service.inspector().resolve(args["target"]);
                    locator    = service.inspector().locator_for(t);
                    web        = dynamic_cast<wxWebView*>(target_window(t));
                    if (web == nullptr)
                        throw ToolFailure("The target is not a web view (ui_snapshot lists them as kind webview)");
                } else
                    web = default_webview(service);
                guard_script(service, web);
                if (in_top_dialog(service, web))
                    modal = service.modals().top();
                sink = new ScriptSink(web, state);
                try {
                    web->RunScriptAsync(from_u8(with_held_messages(script)), sink);
                } catch (...) {
                    delete sink;
                    sink = nullptr;
                    throw;
                }
                return {{"url", into_u8(web->GetCurrentURL())}};
            });
            journal_with_locator(ctx, args, locator);
            // The result arrives as an event on the UI thread; wait for it here, without the UI.
            bool        done = false, error = false, destroyed = false;
            std::string text;
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                const auto                   end = std::chrono::steady_clock::now() + timeout;
                while (!state->done && !ctx.cancelled() && std::chrono::steady_clock::now() < end)
                    state->cv.wait_until(lock, std::min(end, std::chrono::steady_clock::now() + std::chrono::milliseconds(200)));
                done      = state->done;
                error     = state->error;
                destroyed = state->destroyed;
                text      = state->text;
            }
            delete_on_ui(sink);
            if (!done)
                throw ToolFailure(ctx.cancelled() ? "Cancelled: OrcaSlicer is closing or the script was stopped" :
                                                    "Timed out waiting for the script result");
            if (error)
                throw ToolFailure("The script failed: " + text);
            int held = 0;
            if (const json wrapped = json::parse(text, nullptr, false); wrapped.is_object() && wrapped.contains("value") && wrapped["value"].is_string()) {
                held = wrapped.value("held", 0);
                text = wrapped["value"].get<std::string>();
            }
            // The page's held messages reach its host just after the result; a dialog one of them ends
            // leaves the modal stack when its loop returns.
            bool closed = destroyed;
            for (int i = 0; !closed && modal != nullptr && held > 0 && i < 10; ++i) {
                try {
                    closed = ctx.observe_ui([&] {
                                    const auto& stack = service.modals().stack();
                                    return json(std::find(stack.begin(), stack.end(), modal) == stack.end());
                                }).get<bool>();
                } catch (const ToolFailure&) {
                    break;
                }
                if (!closed)
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
            json value = destroyed ? json() : json::parse(text, nullptr, false);
            if (value.is_discarded())
                value = text;
            json out{{"result", value}, {"url", started["url"]}};
            if (closed)
                out["closed"] = true;
            return ToolResult::ok(out);
        }));
}

}}} // namespace Slic3r::GUI::Mcp
