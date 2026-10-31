#include "McpTools.hpp"

#include "McpTypes.hpp"
#include "UiInspector.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Plater.hpp"

#include <wx/app.h>
#include <wx/glcanvas.h>
#include <wx/window.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
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

ToolDef inspect_tool(std::string name, std::string title, std::string description, json schema, ToolHandler handler)
{
    ToolDef d;
    d.name         = std::move(name);
    d.title        = std::move(title);
    d.description  = std::move(description);
    d.input_schema = std::move(schema);
    d.read_only    = true;
    d.idempotent   = true;
    d.guard        = GuardClass::Inspect;
    d.inline_call  = true;
    d.journal      = false;
    d.handler      = std::move(handler);
    return d;
}

ToolDef mutate_tool(std::string name, std::string title, std::string description, json schema, ToolHandler handler, bool destructive)
{
    ToolDef d;
    d.name         = std::move(name);
    d.title        = std::move(title);
    d.description  = std::move(description);
    d.input_schema = std::move(schema);
    d.destructive  = destructive;
    d.guard        = GuardClass::Mutate;
    d.handler      = std::move(handler);
    return d;
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

// Polls `poll` on the UI thread every `interval` until it returns a non-null value, which it returns,
// or until `timeout` passes or the call is cancelled, when it returns null.
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
        throw ToolFailure("OrcaSlicer is still starting; wait {for:\"ready\"} first");
    return *plater;
}

std::string arg_string(const json& args, const char* key, const std::string& fallback)
{
    auto it = args.find(key);
    return it != args.end() && it->is_string() ? it->get<std::string>() : fallback;
}

int arg_int(const json& args, const char* key, int fallback)
{
    auto it = args.find(key);
    return it != args.end() && it->is_number() ? it->get<int>() : fallback;
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
        throw ToolFailure("Cancelled: OrcaSlicer is closing or the script was stopped");
    const std::string undrawn = undrawn_window(ctx);
    throw ToolFailure("Timed out waiting for " + what + (undrawn.empty() ? "" : ": " + undrawn));
}

}}} // namespace Slic3r::GUI::Mcp
