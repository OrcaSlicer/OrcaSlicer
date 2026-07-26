#pragma once

#include <nlohmann/json.hpp>

#include <wx/string.h>
#include <wx/thread.h>

#include <cassert>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {

// How a source's action set changed. Drives the registry's refresh handlers.
enum class ActionChange { Added, Removed };

// Result of running an AppAction, in the action layer's own vocabulary. Concrete
// actions translate their runner-specific result into this generic shape.
struct AppActionRunResult
{
    enum class Level { Success, Info, Error, Busy };

    Level    level = Level::Info;
    wxString message;   // empty = "nothing worth showing"
};

// A speed-dial action: identity + user-state seeded from config + how to run itself.
// Abstract base - the only virtual is run(); concrete subclasses know how to run
// and what their source is.
// note: named AppAction, not Action - Slic3r::GUI::Action is already taken by
// UnsavedChangesDialog's exit-action enum, and this header reaches most GUI TUs.
struct AppAction
{
    const std::string& id() const { return m_id; }
    const std::string& title() const { return m_title; }
    const std::string& source_key() const { return m_source_key; }   // stable source identity
    const std::string& source_name() const { return m_source_name; } // source display name

    // Builds the stable id "<prefix>:<title>:<source_key>". why: one place owns the
    // format - both the base ctor and the one raw-key lookup (removing a capability
    // without an action object) go through this; ids are never parsed back apart.
    static std::string compose_id(std::string_view prefix, std::string_view title, std::string_view source_key)
    {
        std::string out;
        out.reserve(prefix.size() + title.size() + source_key.size() + 2);
        out.append(prefix).append(1, ':').append(title).append(1, ':').append(source_key);
        return out;
    }

    // seeded from AppConfig for the snapshot / sort:
    bool        favourite = false;
    int         count = 0;
    long long   last = 0;     // epoch seconds

    virtual ~AppAction() = default;
    virtual AppActionRunResult run() const = 0; // re-resolves + runs (UI thread)

protected:
    // The definition is constructor-set and immutable. Refreshes replace an action
    // instead of mutating identity after the registry has indexed it by id.
    // why: source_key (not the display name) carries identity, so renaming the source's
    // display name leaves the id - and its persisted stats/favourite - intact.
    AppAction(std::string_view prefix, std::string title, std::string source_key, std::string source_name)
        : m_id(compose_id(prefix, title, source_key)),
          m_title(std::move(title)),
          m_source_key(std::move(source_key)),
          m_source_name(std::move(source_name)) {}

private:
    std::string m_id;          // <prefix>:<title>:<source_key> - stable identity + AppConfig key
    std::string m_title;       // display name
    std::string m_source_key;  // stable identity of the action's source (e.g. plugin_key)
    std::string m_source_name; // display name of the action's source
};

// Self-contained sink and single owner of runnable actions for the app session.
//
// Workflow:
// 1. init() (once, UI thread) subscribes to the plugin loader and enumerates the
//    current script capabilities into actions.
// 2. Loader load/unload callbacks route through refresh_source()/refresh_capability(),
//    which upsert()/remove() actions. The registry keeps the only action list and
//    restores persisted user state as actions arrive.
// 3. Consumers use by_id(), snapshot(), and run() without knowing the source.
//
// note: there is exactly one source (script plugins), so it lives inline here rather
// than behind a polymorphic source interface.
class ActionRegistry
{
public:
    // Subscribes to the plugin loader and enumerates its current actions. Call once
    // on the UI thread after the plugin system is up; wires the initial list and live
    // updates together.
    void init();

    // Takes ownership, seeds persisted state, then inserts the action or replaces
    // the action with the same id. A null action is ignored.
    void upsert(std::unique_ptr<AppAction> action);

    // Removes the action with this id. Missing ids are a harmless no-op.
    void remove(const std::string& id);

    // Always-clean read surface. UI thread only.
    const AppAction*                               by_id(const std::string& id) const;

    // Dispatch + write-through (registry is the only thing that touches AppConfig).
    AppActionRunResult run(const std::string& id);          // runs + bumps stats
    void             set_favourite(const std::string& id, bool on);
    void             reorder_favourites(const std::vector<std::string>& ids);   // persist a new bar order

    // Run-confirm gate, keyed by action id (per-action "don't ask again").
    bool should_ask(const std::string& id) const;
    void suppress_ask(const std::string& id);

    // Flat, frecency-sorted snapshot for the webview: {actions:[...], favourites:[...]}.
    nlohmann::json snapshot() const;

private:
    void         seed_state(AppAction& a) const;               // favourite/stats from config
    AppAction*      find(const std::string& id);

    // Loader callbacks (marshalled to the UI thread) land here. refresh_source rebuilds
    // one plugin's whole action set; refresh_capability touches a single capability.
    void refresh_source(const std::string& plugin_key, ActionChange change);
    void refresh_capability(const std::string& plugin_key, const std::string& capability, ActionChange change);

    bool m_started = false;  // init() runs exactly once; guards double-subscription
    std::unordered_map<std::string, std::shared_ptr<AppAction>> m_actions;  // UI-thread confined; no lock
};

}} // namespace Slic3r::GUI
