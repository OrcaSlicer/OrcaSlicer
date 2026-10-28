// GUI automation: look at the widget tree, click, type, press keys, drag on the 3D canvas, answer
// dialogs and run JavaScript in web views.

#include "JobRegistry.hpp"
#include "McpService.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"
#include "ModalTracker.hpp"
#include "UiInspector.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <wx/app.h>
#include <wx/button.h>
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
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;
using namespace std::chrono_literals;

namespace {

const json kTargetSchema = {{"type", json::array({"string", "object"})},
                            {"description",
                             "A ref from the latest ui_snapshot (\"w12\") or a locator: {label, class, name, within, index} for "
                             "widgets, {option:\"layer_height\"} for a setting field, {menu:\"File>Export>Export G-code\"}, {gltoolbar:\"arrange\"}, {gizmo:\"Move\"}, or "
                             "{imgui:\"label\", window?, canvas?}."}};

const json kModifiersSchema = {{"type", "array"},
                               {"items", {{"type", "string"}, {"enum", {"ctrl", "shift", "alt", "cmd"}}}},
                               {"description", "Modifier keys held during the click."}};

bool in_top_dialog(McpService& service, wxWindow* window)
{
    wxDialog* top = service.modals().top();
    return top != nullptr && window != nullptr && wxGetTopLevelParent(window) == top;
}

// GUI input tools run under the recover guard so they can work in the topmost modal dialog; a
// target anywhere else gets the checks a mutate call would: no other call's nested loop, no open
// modal, and no MCP slice or export holding the project.
void guard_target(McpService& service, wxWindow* window)
{
    if (in_top_dialog(service, window))
        return;
    service.ui().require_mutate();
    if (const auto ops = service.jobs().running_operations(); !ops.empty())
        throw ToolFailure("Busy: job " + ops.front() + " (a slice or export) is running; wait for it or cancel it first",
                          {{"job", ops.front()}});
}

wxWindow* target_window(const UiTarget& t) { return t.kind == UiTarget::Kind::Window ? t.window.get() : nullptr; }

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
        for (wxWindow* child : w->GetChildren())
            visit(child);
    };
    if (wxGetApp().mainframe != nullptr)
        visit(wxGetApp().mainframe);
    for (wxWindow* tlw : wxTopLevelWindows)
        if (tlw != wxGetApp().mainframe)
            visit(tlw);
    return out;
}

// Receives the result of one RunScriptAsync call into a State the job thread waits on. Created,
// used and deleted on the UI thread only, like the weak reference it keeps to its web view.
class ScriptSink : public wxEvtHandler
{
public:
    struct State
    {
        std::mutex              mutex;
        std::condition_variable cv;
        bool                    done  = false;
        bool                    error = false;
        std::string             text;
    };

    ScriptSink(wxWebView* web, std::shared_ptr<State> state) : m_web(web), m_state(std::move(state))
    {
        web->Bind(wxEVT_WEBVIEW_SCRIPT_RESULT, &ScriptSink::on_result, this);
    }
    ~ScriptSink() override
    {
        if (wxWebView* web = m_web.get())
            web->Unbind(wxEVT_WEBVIEW_SCRIPT_RESULT, &ScriptSink::on_result, this);
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

// Presses the button with `id` in `dialog` so the dialog's own handler runs, or ends the dialog with
// that id when no such button exists. Returns whether a button was pressed. UI thread only.
bool press_dialog_button(wxDialog* dialog, int id)
{
    wxWindow* pressed = nullptr;
    std::function<void(wxWindow*)> find = [&](wxWindow* w) {
        for (wxWindow* child : w->GetChildren()) {
            if (pressed != nullptr || dynamic_cast<wxTopLevelWindow*>(child) != nullptr)
                continue;
            if (child->GetId() == id && child->IsShown() && child->IsEnabled() &&
                (dynamic_cast<wxButton*>(child) != nullptr || class_name(child).find("Button") != std::string::npos))
                pressed = child;
            else
                find(child);
        }
    };
    find(dialog);
    if (pressed == nullptr) {
        dialog->EndModal(id);
        return false;
    }
    wxCommandEvent evt(wxEVT_BUTTON, id);
    evt.SetEventObject(pressed);
    pressed->GetEventHandler()->ProcessEvent(evt);
    return true;
}

} // namespace

void register_ui_tools(McpService& service)
{
    ToolRegistry& reg = service.registry();

    {
        ToolDef def = inspect_tool(
            "ui_snapshot", "List the GUI",
            "Describe what is on screen as an indented tree, one element per line with a ref: wx widgets of every shown window "
            "(buttons, fields, checkboxes, combos, tabs, labels), the main menus, the 3D canvas's toolbar items and gizmos, and "
            "the ImGui items drawn in the last frame. Pass the refs to ui_click / ui_input / ui_key. Refs go stale when the "
            "element goes away; take a new snapshot then. root: \"all\" (default), \"main\", \"dialog\" (topmost modal) or a "
            "window's ref. query filters by label, value, name or class. Prefer run_action and the typed tools when one fits.",
            object_schema({{"root", prop("string", "all | main | dialog | <ref of a window>.")},
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
        "ui_click", "Click an element",
        "Activate a GUI element: menu items, buttons, checkboxes, toggles, radio buttons, toolbar items and gizmos are activated "
        "the way their owners expect; other widgets and ImGui items get a synthesized mouse click. With x/y on a 3D canvas "
        "target, clicks that point (logical pixels from the canvas's top left). Modifiers are seen by the 3D canvas, the "
        "object list and handlers that read the event. A click that opens a dialog returns blocked_by_dialog with the job.",
        object_schema({{"target", kTargetSchema},
                       {"button", enum_prop({"left", "right", "middle"}, "Default left.")},
                       {"double", prop("boolean", "Double click.")},
                       {"modifiers", kModifiersSchema},
                       {"x", prop("integer", "Canvas x for a click on a 3D canvas.")},
                       {"y", prop("integer", "Canvas y for a click on a 3D canvas.")}},
                      {"target"}),
        [&service](const json& args, ToolContext& ctx) {
            ClickOptions o;
            o.button       = arg_string(args, "button", "left");
            o.double_click = arg_bool(args, "double", false);
            o.modifiers    = args.value("modifiers", std::vector<std::string>{});
            o.at_point     = args.contains("x") || args.contains("y");
            o.x            = arg_int(args, "x", 0);
            o.y            = arg_int(args, "y", 0);
            json locator;
            json out = ctx.run_ui([&] {
                UiTarget t = service.inspector().resolve(args["target"]);
                locator    = service.inspector().locator_for(t);
                guard_target(service, target_window(t));
                return service.inspector().click(t, o);
            });
            journal_with_locator(ctx, args, locator);
            settle(ctx);
            return ToolResult::ok(out);
        }));

    reg.add(recover_input_tool(
        "ui_input", "Set a control's value",
        "Set the text, number, selection or checked state of a control and send the event a person's edit would: text fields "
        "(submit:true also presses Enter and leaves the field, which is when Orca's setting fields commit), spin controls, "
        "sliders, choice / combo / list boxes (by option text or index), checkboxes and toggles (true/false). On an ImGui "
        "field it clicks, selects all and types. For settings, set_settings is more reliable.",
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
        "ui_key", "Press keys",
        "Send a key chord (\"Ctrl+S\", \"Escape\", \"Return\", \"Delete\", \"Shift+Tab\", \"Left\", \"F5\") and/or type text to "
        "a target or, without one, to the focused window (the topmost dialog while one is open, the 3D canvas when nothing "
        "has focus). The window's shortcut handling "
        "sees it first, as with a real key press. Typing reaches ImGui fields and key handlers; to fill a native text field "
        "use ui_input.",
        object_schema({{"target", kTargetSchema}, {"keys", prop("string", "Key chord.")}, {"text", prop("string", "Characters to type.")}}),
        [&service](const json& args, ToolContext& ctx) {
            const std::string keys = arg_string(args, "keys"), text = arg_string(args, "text");
            if (keys.empty() && text.empty())
                throw ToolFailure("Give keys, text or both");
            json locator;
            json out = ctx.run_ui([&] {
                std::optional<UiTarget> t;
                if (args.contains("target")) {
                    t       = service.inspector().resolve(args["target"]);
                    locator = service.inspector().locator_for(*t);
                }
                guard_target(service, service.inspector().key_target(t ? &*t : nullptr));
                return service.inspector().key(t ? &*t : nullptr, keys, text);
            });
            journal_with_locator(ctx, args, locator);
            settle(ctx);
            return ToolResult::ok(out);
        }));

    reg.add(mutate_tool(
        "ui_mouse", "Mouse gesture on the 3D view",
        "Synthesize a mouse gesture on a 3D canvas at logical pixel coordinates from its top left: move, down, up, click, "
        "drag (from x,y to `to`, in `steps` moves; the left button rotates the camera, right or middle pans, on an object it "
        "moves it) or wheel (delta in notches, positive zooms in). Coordinates come from a canvas screenshot divided by the "
        "screen scale, or from ui_snapshot's ImGui item rectangles.",
        object_schema({{"action", enum_prop({"move", "down", "up", "click", "drag", "wheel"}, "Default click.")},
                       {"canvas", enum_prop({"3d", "preview", "assemble", "current"}, "Default current.")},
                       {"x", prop("integer", "Start x.")},
                       {"y", prop("integer", "Start y.")},
                       {"to", array_prop({{"type", "integer"}}, "Drag end [x, y].")},
                       {"steps", {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}, {"description", "Drag moves (default 10)."}}},
                       {"button", enum_prop({"left", "right", "middle"}, "Default left.")},
                       {"double", prop("boolean", "Double click.")},
                       {"delta", prop("integer", "Wheel notches.")},
                       {"modifiers", kModifiersSchema}},
                      {"x", "y"}),
        [&service](const json& args, ToolContext& ctx) {
            json out = ctx.run_ui([&] {
                const std::string name   = arg_string(args, "canvas", "current");
                Plater&           plater = ui_plater();
                GLCanvas3D*       canvas = name == "3d"       ? plater.get_view3D_canvas3D() :
                                           name == "preview"  ? plater.get_preview_canvas3D() :
                                           name == "assemble" ? plater.get_assmeble_canvas3D() :
                                                                plater.get_current_canvas3D();
                if (canvas == nullptr || !canvas->get_wxglcanvas()->IsShownOnScreen())
                    throw ToolFailure("That 3D view is not shown; switch to it with set_view first");
                return service.inspector().mouse(*canvas, args);
            });
            settle(ctx);
            return ToolResult::ok(out);
        }));

    reg.add(recover_input_tool(
        "dialog_respond", "Answer a dialog",
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
            "dialog_auto_answer", "Pre-answer dialogs",
            "Answer modal dialogs automatically, native ones included: a dialog whose title or message matches a rule's regex "
            "(case-insensitive) returns that button at once and never shows. A matching wx dialog that is already open is "
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

    reg.add(mutate_tool(
        "webview_eval", "Run JavaScript in a web view",
        "Evaluate JavaScript in one of OrcaSlicer's web views (Home, the device page, login and others) and return the value of "
        "its last expression (JSON values come back parsed). Without a target the only shown web view is used. Waits up to "
        "timeout_s (default 10).",
        object_schema({{"script", prop("string", "JavaScript to run.")},
                       {"target", kTargetSchema},
                       {"timeout_s", {{"type", "integer"}, {"minimum", 1}, {"maximum", 120}, {"description", "Default 10."}}}},
                      {"script"}),
        [&service](const json& args, ToolContext& ctx) {
            const std::string script  = arg_string(args, "script");
            const auto        timeout = std::chrono::seconds(arg_int(args, "timeout_s", 10));
            auto              state   = std::make_shared<ScriptSink::State>();
            ScriptSink*       sink    = nullptr; // the UI thread's to create and delete
            json started = ctx.run_ui([&]() -> json {
                wxWebView* web = nullptr;
                if (args.contains("target")) {
                    UiTarget t = service.inspector().resolve(args["target"]);
                    web        = dynamic_cast<wxWebView*>(target_window(t));
                    if (web == nullptr)
                        throw ToolFailure("The target is not a web view (ui_snapshot lists them as kind webview)");
                } else {
                    const auto all = visible_webviews();
                    if (all.empty())
                        throw ToolFailure("No web view is shown (the Home tab has one: set_view tab=home)");
                    if (all.size() > 1) {
                        json urls = json::array();
                        for (wxWebView* w : all)
                            urls.push_back(into_u8(w->GetCurrentURL()));
                        throw ToolFailure("Several web views are shown; pass target (ui_snapshot query=webview)", {{"web_views", urls}});
                    }
                    web = all.front();
                }
                sink = new ScriptSink(web, state);
                try {
                    web->RunScriptAsync(from_u8(script), sink);
                } catch (...) {
                    delete sink;
                    sink = nullptr;
                    throw;
                }
                return {{"url", into_u8(web->GetCurrentURL())}};
            });
            // The result arrives as an event on the UI thread; wait for it here, without the UI.
            bool        done = false, error = false;
            std::string text;
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                const auto                   end = std::chrono::steady_clock::now() + timeout;
                while (!state->done && !ctx.cancelled() && std::chrono::steady_clock::now() < end)
                    state->cv.wait_until(lock, std::min(end, std::chrono::steady_clock::now() + std::chrono::milliseconds(200)));
                done  = state->done;
                error = state->error;
                text  = state->text;
            }
            delete_on_ui(sink);
            if (!done)
                throw ToolFailure(ctx.cancelled() ? "OrcaSlicer is shutting down" : "Timed out waiting for the script result");
            if (error)
                throw ToolFailure("The script failed: " + text);
            json value = json::parse(text, nullptr, false);
            if (value.is_discarded())
                value = text;
            return ToolResult::ok({{"result", value}, {"url", started["url"]}});
        }));
}

}}} // namespace Slic3r::GUI::Mcp
