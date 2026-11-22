#include "McpTools.hpp"

#include "libslic3r/Model.hpp"

#include "UiInspector.hpp"
#include "slic3r/GUI/ActionRegistry.hpp"
#include "slic3r/GUI/CommandCatalog.hpp"
#include "slic3r/GUI/CommandTypes.hpp"
#include "slic3r/GUI/NativeCommands.hpp"
#include "slic3r/GUI/Operations/Outcome.hpp"
#include "slic3r/GUI/Operations/PlateOps.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <nlohmann/json.hpp>
#include <wx/app.h>
#include <wx/glcanvas.h>
#include <wx/window.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef __WXGTK__
#include <gtk/gtk.h>
#endif

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

json object_schema(std::initializer_list<Property> properties, std::vector<std::string> required)
{
    json props = json::object();
    for (const auto& [name, schema] : properties)
        props[name] = schema;
    json out{{"type", "object"}, {"properties", std::move(props)}, {"additionalProperties", false}};
    if (!required.empty())
        out["required"] = std::move(required);
    return out;
}

json prop(const char* type, const std::string& description) { return {{"type", type}, {"description", description}}; }

json enum_prop(std::vector<std::string> values, const std::string& description)
{
    return {{"type", "string"}, {"enum", std::move(values)}, {"description", description}};
}

json array_prop(json items, const std::string& description) { return {{"type", "array"}, {"items", std::move(items)}, {"description", description}}; }

NativeCommand inspect_tool(std::string name, std::string title, std::string description, json schema, CommandFn handler)
{
    NativeCommand d = NativeCommands::long_command(std::move(name), std::move(title), {}, std::move(schema), std::move(handler),
                                                   /*inline_call=*/true);
    d.description   = std::move(description);
    d.listed        = true;
    d.read_only     = true;
    d.idempotent    = true;
    d.guard         = GuardClass::Inspect;
    d.journal       = false;
    return d;
}

NativeCommand reaching(std::vector<std::string> operations, NativeCommand command)
{
    command.operations = std::move(operations);
    return command;
}

NativeCommand mutate_tool(std::string name, std::string title, std::string description, json schema, CommandFn handler, bool destructive)
{
    NativeCommand d = NativeCommands::long_command(std::move(name), std::move(title), {}, std::move(schema), std::move(handler));
    d.description   = std::move(description);
    d.listed        = true;
    d.destructive   = destructive;
    d.guard         = GuardClass::Mutate;
    return d;
}

NativeCommand menu_command(std::string name, std::string title, std::string description, json schema, json actions,
                          std::function<AppActionRunResult(const json& args)> fn)
{
    NativeCommand d = NativeCommands::ui_command(std::move(name), std::move(title), {}, std::move(schema), std::move(fn));
    d.description   = std::move(description);
    d.actions       = std::move(actions);
    d.listed        = true;
    return d;
}

AppActionRunResult menu_reply(const json& reply)
{
    AppActionRunResult result{AppActionRunResult::Level::Succeeded};
    result.reply = reply.dump();
    return result;
}

AppActionRunResult quiet_refusal(const Ops::Refusal& refusal)
{
    AppActionRunResult kept{AppActionRunResult::Level::Info, {}, false};
    kept.refusal = std::make_shared<const Ops::Refusal>(refusal);
    return kept;
}

namespace {

// Whether a 3D view is shown but can't draw yet. On Wayland a canvas gets its GL surface from the
// map event GTK handles once the main loop runs again, after the closures queued with the change
// that showed it, such as a tab switch. wxGLCanvas's IsShownOnScreen() needs that surface; the
// base class's only needs the windows shown. UI thread.
bool canvas_being_mapped()
{
    for (GLCanvas3D* canvas : ui_canvases())
        if (const wxGLCanvas* glc = canvas->get_wxglcanvas();
            glc != nullptr && glc->wxWindowBase::IsShownOnScreen() && !glc->IsShownOnScreen())
            return true;
    return false;
}

#ifdef __WXGTK__
// The main window's frame clock, or null while the window is hidden or minimized, so not meant to be
// drawn. UI thread.
GdkFrameClock* main_frame_clock()
{
    MainFrame* frame = wxTheApp != nullptr ? wxGetApp().mainframe : nullptr;
    if (frame == nullptr || !frame->IsShownOnScreen() || frame->IsIconized())
        return nullptr;
    return gtk_widget_get_frame_clock(frame->GetHandle());
}
#endif

} // namespace

json poll_until(ToolContext& ctx, const std::function<json()>& poll, std::chrono::milliseconds timeout, std::chrono::milliseconds interval)
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        json v = ctx.observe_ui(poll);
        if (!v.is_null() || ctx.cancelled() || std::chrono::steady_clock::now() >= end)
            return v;
        std::this_thread::sleep_for(interval);
    }
}

void settle(ToolContext& ctx)
{
    // Only a sync point: the change is made already, so a refusal here (a dialog it opened, a UI
    // rebuild it caused) neither undoes it nor fails the call. Nor does a 3D view that never gets its
    // surface: it only fails the calls that draw in it, so the wait for it is short. The map event
    // comes within a main-loop pass or two, hence the short interval.
    try {
        poll_until(ctx, [] { return canvas_being_mapped() ? json() : json(true); }, std::chrono::seconds(1), std::chrono::milliseconds(10));
    } catch (const ToolFailure&) {
    }
}

std::string undrawn_window([[maybe_unused]] ToolContext& ctx)
{
#ifdef __WXGTK__
    // GTK waits for the compositor's go-ahead after each frame it shows, so a frame asked for runs
    // within a refresh or two while the window is drawn and never while it isn't.
    try {
        const json before = ctx.observe_ui([] {
            GdkFrameClock* clock = main_frame_clock();
            if (clock == nullptr)
                return json();
            gdk_frame_clock_request_phase(clock, GDK_FRAME_CLOCK_PHASE_PAINT);
            return json(gdk_frame_clock_get_frame_counter(clock));
        });
        if (before.is_null())
            return {};
        const json ran = poll_until(
            ctx,
            [counter = before.get<std::int64_t>()] {
                GdkFrameClock* clock = main_frame_clock();
                return clock == nullptr || gdk_frame_clock_get_frame_counter(clock) != counter ? json(true) : json();
            },
            std::chrono::milliseconds(500), std::chrono::milliseconds(20));
        if (!ran.is_null() || ctx.cancelled())
            return {};
    } catch (const ToolFailure&) {
        return {};
    }
    return "OrcaSlicer's window is not being drawn, as while the display is off; turn the display on and try again";
#else
    return {};
#endif
}

Plater& ui_plater()
{
    if (wxTheApp == nullptr || wxGetApp().is_closing())
        throw ToolFailure("OrcaSlicer is shutting down");
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr || wxGetApp().mainframe == nullptr)
        throw ToolFailure("OrcaSlicer is still starting; app_wait {for:\"ready\"} first");
    return *plater;
}

std::string arg_string(const json& args, const char* key, const std::string& fallback)
{
    auto it = args.find(key);
    return it != args.end() && it->is_string() ? it->get<std::string>() : fallback;
}

std::string no_such_plate(int plate, int count) { return Ops::no_such_plate(plate, count).english(); }

ToolFailure refused(const Ops::Refusal& refusal, const std::string& hint, const char* names_key)
{
    json details{{"code", refusal.code}};
    if (names_key != nullptr)
        details[names_key] = refusal.names;
    if (std::string agent = refusal_hint(refusal); !agent.empty())
        details["hint"] = std::move(agent);
    return ToolFailure(refusal.english() + (hint.empty() ? "" : " " + hint), std::move(details));
}

std::string refusal_hint(const Ops::Refusal& refusal)
{
    const std::string code = refusal.code;
    // Two refusals share gizmo_open: Select All's, for the gizmo of the view shown (select_everything()), and the
    // operations', for the Prepare view's (ObjectOps' gizmo_open()), so the hint tells them apart by the call.
    if (code == "gizmo_open" || code == "gizmo_editing")
        return "object_select {none: true} closes the open gizmo of the view shown, as Deselect All does. Select All is "
               "refused for that view's gizmo, any other operation for the Prepare view's, so for those in the assemble view, "
               "view_set {canvas: \"3d\"} comes first. Deselecting clears the selection: a call that acted on it names its "
               "targets when it runs again.";
    // Only Add Primitive's person's form ({shape} alone) asks for the selection to be clear. The Speed Dial's
    // add_primitive actions take no arguments, so the call with a size or silent is the command's.
    if (code == "object_selected")
        return "object_select {none: true} first; or call object_add_primitive with size or silent: true, which adds the "
               "shape whatever is selected; part_add adds it to the selected object";
    // The refusals that ask for a selection, where the call can name what it acts on.
    if (code == "nothing_selected" || code == "one_object_only" || code == "wrong_selection" || code == "too_few_objects" ||
        code == "several_instances")
        return "name the targets in the call (its objects, instances or parts, or object and part, as its schema takes them), "
               "or select them with object_select";
    return {};
}

int arg_int(const json& args, const char* key, int fallback)
{
    auto it = args.find(key);
    if (it == args.end() || !it->is_number())
        return fallback;
    // Checked before it is narrowed: get<int>() would turn 4294967296 into 0, another index or count the command then
    // accepts, and a float past int's range into undefined behavior.
    constexpr auto lo = std::numeric_limits<int>::min(), hi = std::numeric_limits<int>::max();
    const bool     fits = it->is_number_unsigned() ? it->get<std::uint64_t>() <= std::uint64_t(hi) :
                          it->is_number_integer()  ? it->get<std::int64_t>() >= lo && it->get<std::int64_t>() <= hi :
                                                     std::isfinite(it->get<double>()) && it->get<double>() >= lo && it->get<double>() <= hi;
    if (!fits)
        throw ToolFailure(std::string(key) + " " + it->dump() + " is out of range", {{"code", "invalid_argument"}});
    return it->get<int>();
}

double arg_double(const json& args, const char* key, double fallback)
{
    auto it = args.find(key);
    return it != args.end() && it->is_number() ? it->get<double>() : fallback;
}

bool arg_bool(const json& args, const char* key, bool fallback)
{
    auto it = args.find(key);
    return it != args.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

json page(const json& items, const json& args, std::size_t default_limit)
{
    const std::size_t total  = items.size();
    const std::size_t offset = std::min<std::size_t>(std::size_t(std::max(0, arg_int(args, "offset", 0))), total);
    const std::size_t limit  = std::size_t(std::max(1, arg_int(args, "limit", int(default_limit))));
    json              out    = json::array();
    for (std::size_t i = offset; i < total && out.size() < limit; ++i)
        out.push_back(items[i]);
    json result{{"total", total}, {"offset", offset}, {"items", std::move(out)}};
    if (offset + limit < total)
        result["next_offset"] = offset + limit;
    return result;
}

json poll_ui(ToolContext& ctx, const std::function<json()>& poll, std::chrono::milliseconds timeout, const std::string& what)
{
    json v = poll_until(ctx, poll, timeout, std::chrono::milliseconds(200));
    if (!v.is_null())
        return v;
    if (ctx.cancelled())
        throw ToolFailure("Cancelled: OrcaSlicer is closing or the replay or script was stopped");
    const std::string undrawn = undrawn_window(ctx);
    throw ToolFailure("Timed out waiting for " + what + (undrawn.empty() ? "" : ": " + undrawn));
}

json expected_id_prop(const char* of)
{
    return {{"type", "integer"},
            {"minimum", 0},
            {"description", std::string("Optional guard: the ") + of + "'s id in `object_list`."}};
}

NativeCommand with_expected_ids(NativeCommand command)
{
    const json properties = command.input_schema.value("properties", json::object());
    for (const char* name : {"object_id", "part_id", "object_ids", "part_ids", "instance_object_ids"})
        if (properties.contains(name))
            command.id_args.emplace_back(name);
    return command;
}

void check_expected_ids(const ModelObject& object, const ModelVolume* volume, const json& args)
{
    if (args.contains("object_id") && args["object_id"].get<std::size_t>() != object.id().id)
        throw ToolFailure("The object at this index is now \"" + object.name + "\" (id " + std::to_string(object.id().id) +
                              "), not the expected id " + std::to_string(args["object_id"].get<std::size_t>()) +
                              ": object_list again",
                          {{"id", object.id().id}});
    if (volume != nullptr && args.contains("part_id") && args["part_id"].get<std::size_t>() != volume->id().id)
        throw ToolFailure("The part at this index is now \"" + volume->name + "\" (id " + std::to_string(volume->id().id) +
                              "), not the expected id " + std::to_string(args["part_id"].get<std::size_t>()) +
                              ": object_list again",
                          {{"id", volume->id().id}});
}

}}} // namespace Slic3r::GUI::Mcp
