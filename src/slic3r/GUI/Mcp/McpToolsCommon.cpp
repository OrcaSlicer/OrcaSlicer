#include "McpTools.hpp"

#include "libslic3r/Model.hpp"

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
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
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

// The words of `text` in lower case, split at anything but a letter or a digit. With `camel`, a word
// with a capital after a lower-case letter also counts as its parts: TriangleMesh as triangle and mesh.
std::vector<std::string> text_words(const std::string& text, bool camel)
{
    std::vector<std::string> words;
    std::string              word;
    std::vector<std::size_t> parts; // where each CamelCase part after the first starts in `word`
    const auto               flush = [&] {
        if (word.empty())
            return;
        std::size_t from = 0;
        for (const std::size_t at : parts) {
            words.push_back(word.substr(from, at - from));
            from = at;
        }
        if (!parts.empty())
            words.push_back(word.substr(from));
        words.push_back(std::exchange(word, {}));
        parts.clear();
    };
    unsigned char previous = 0;
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (!std::isalnum(u)) {
            flush();
            previous = 0;
            continue;
        }
        if (camel && std::isupper(u) && std::islower(previous))
            parts.push_back(word.size());
        word += char(std::tolower(u));
        previous = u;
    }
    flush();
    return words;
}

// Whether a word of `words` starts with `w`.
bool has_word(const std::vector<std::string>& words, const std::string& w)
{
    return std::any_of(words.begin(), words.end(), [&w](const std::string& word) { return word.compare(0, w.size(), w) == 0; });
}

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
        throw ToolFailure("OrcaSlicer is still starting; wait {for:\"ready\"} first");
    return *plater;
}

std::string arg_string(const json& args, const char* key, const std::string& fallback)
{
    auto it = args.find(key);
    return it != args.end() && it->is_string() ? it->get<std::string>() : fallback;
}

std::string no_such_plate(int plate, int count)
{
    return "Plate " + std::to_string(plate) + " does not exist (the project has " +
           (count == 1 ? "plate 0 only" : "plates 0.." + std::to_string(count - 1)) + ")";
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

std::vector<std::string> query_words(const std::string& query)
{
    std::vector<std::string> words = text_words(query, false);
    std::vector<std::string> longer;
    std::copy_if(words.begin(), words.end(), std::back_inserter(longer), [](const std::string& w) { return w.size() >= 3; });
    return longer.empty() ? words : longer;
}

std::pair<int, int> match_score(const std::vector<std::string>& words, const std::string& name, const std::string& text)
{
    const std::vector<std::string> in_name = text_words(name, true);
    const std::vector<std::string> in_text = text_words(text, true);
    std::pair<int, int>            score;
    for (const std::string& w : words)
        if (has_word(in_name, w)) {
            ++score.first;
            ++score.second;
        } else if (has_word(in_text, w))
            ++score.first;
    return score;
}

std::string first_sentence(const std::string& text)
{
    std::string line = text;
    std::replace(line.begin(), line.end(), '\n', ' ');
    for (std::size_t dot = line.find('.'); dot != std::string::npos; dot = line.find('.', dot + 1)) {
        const bool abbreviation = dot >= 3 && (line.compare(dot - 3, 3, "e.g") == 0 || line.compare(dot - 3, 3, "i.e") == 0);
        if (!abbreviation && (dot + 1 == line.size() || line[dot + 1] == ' '))
            return line.substr(0, dot + 1);
    }
    return line;
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
            {"description", std::string("The id list_objects reports for this ") + of +
                                ": the call is refused when the index names another one now."}};
}

void check_expected_ids(const ModelObject& object, const ModelVolume* volume, const json& args)
{
    if (args.contains("object_id") && args["object_id"].get<std::size_t>() != object.id().id)
        throw ToolFailure("The object at this index is now \"" + object.name + "\" (id " + std::to_string(object.id().id) +
                              "), not the expected id " + std::to_string(args["object_id"].get<std::size_t>()) +
                              ": list_objects again",
                          {{"id", object.id().id}});
    if (volume != nullptr && args.contains("part_id") && args["part_id"].get<std::size_t>() != volume->id().id)
        throw ToolFailure("The part at this index is now \"" + volume->name + "\" (id " + std::to_string(volume->id().id) +
                              "), not the expected id " + std::to_string(args["part_id"].get<std::size_t>()) +
                              ": list_objects again",
                          {{"id", volume->id().id}});
}

}}} // namespace Slic3r::GUI::Mcp
