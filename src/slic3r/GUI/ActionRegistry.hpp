#pragma once

#include <nlohmann/json.hpp>

#include <libslic3r/Config.hpp>

#include <wx/string.h>
#include <wx/thread.h>

#include <cassert>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI {

// How a source's action set changed. Drives the registry's refresh handlers.
enum class ActionChange { Added, Removed };

// What kind of runnable thing an action is. Drives the run-confirm gate (plugins ask, commands don't).
enum class AppActionKind { Plugin, Command };

// Result of running an AppAction, in the action layer's own vocabulary. Concrete
// actions translate their runner-specific result into this generic shape.
struct AppActionRunResult
{
    enum class Level { Success, Info, Error, Busy };

    Level level = Level::Info;
    wxString message; // empty = "nothing worth showing"
};

// Tag carrying a precomputed action id, used by the explicit-id ctor below. It exists so the
// id ctor and the compose-from-prefix ctor are NOT both reachable from a `const char*` first
// argument (which would make calls like AppAction("orca_command", ...) ambiguous).
struct AppActionId
{
    std::string id;
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
    bool favourite = false;
    int count      = 0;
    long long last = 0; // epoch seconds

    // Speed Dial presentation: Plugin keeps group empty (the UI falls back to the
    // source name); Command sets a section label (e.g. "Commands", "Mode").
    AppActionKind kind = AppActionKind::Plugin;
    std::string group;
    // Second-phase input descriptor for the palette: "percent" (jump to layer by a 0-100
    // value) or "tab" (pick a notebook tab). Empty = run immediately on activation.
    std::string input;
    // Tile pictogram: SVG base name under resources/images; empty renders a blank tile (commands
    // without a GUI icon, plugins). Set from NativeCommands / the setting's category icon.
    std::string icon;
    // Settings mode required to edit this action (SettingActions only). The palette prompts before
    // running an action whose mode is above the user's current mode. comSimple for everything else.
    ConfigOptionMode required_mode = comSimple;
    // Description shown in the Speed Dial's footer strip (SettingActions: the localized tooltip).
    std::string tooltip;
    // Full wiki URL, when the action has one (SettingActions whose row declared a label_path).
    std::string help_url;

    virtual ~AppAction() = default;
    // Re-resolves + runs (UI thread). `param` carries an optional per-run argument for
    // commands (e.g. a layer percentage); plugins ignore it.
    virtual AppActionRunResult run(const std::string& param = {}) const = 0;

protected:
    // The definition is constructor-set and immutable. Refreshes replace an action
    // instead of mutating identity after the registry has indexed it by id.
    // why: source_key (not the display name) carries identity, so renaming the source's
    // display name leaves the id - and its persisted stats/favourite - intact.
    AppAction(std::string_view prefix, std::string title, std::string source_key, std::string source_name)
        : m_id(compose_id(prefix, title, source_key))
        , m_title(std::move(title))
        , m_source_key(std::move(source_key))
        , m_source_name(std::move(source_name))
    {}

    // Explicit-id ctor: for actions whose id must NOT be derived from the display title
    // (e.g. a setting action keyed by opt_key+type, so a rename/localization never re-keys it).
    AppAction(AppActionId id, std::string title, std::string source_key, std::string source_name)
        : m_id(std::move(id.id)), m_title(std::move(title)), m_source_key(std::move(source_key)), m_source_name(std::move(source_name))
    {}

private:
    std::string m_id;          // <prefix>:<title>:<source_key> - stable identity + AppConfig key
    std::string m_title;       // display name
    std::string m_source_key;  // stable identity of the action's source (e.g. plugin_key)
    std::string m_source_name; // display name of the action's source
};

// Stable identity/display name of the built-in ("OrcaSlicer") action source. Shared by the native
// command catalog and the dynamically materialised setting/plate/recent actions so every built-in
// action re-keys together.
inline constexpr const char* kOrcaSourceKey  = "orca";
inline constexpr const char* kOrcaSourceName = "OrcaSlicer";

// True when a setting at `setting_mode` cannot be edited in `current_mode` and the UI must switch
// first. Developer settings are handled as a separate prompt by the Speed Dial.
inline bool requires_mode_switch(ConfigOptionMode setting_mode, ConfigOptionMode current_mode)
{
    return setting_mode > current_mode;
}

// Cap + dedupe a persisted favourite-id list, preserving first-occurrence order. A stale or
// hand-edited config must never grow the quick-launch bar past `limit`, and a duplicated id must collapse to its first pin.
std::vector<std::string> cap_favourites(const std::vector<std::string>& ids, size_t limit);

// Self-contained sink and single owner of runnable actions for the app session.
//
// Workflow:
// 1. init() (once, UI thread) subscribes to the plugin loader and enumerates the current script
//    capabilities into actions, then materialises the static built-ins from the NativeCommands catalog.
// 2. Loader load/unload callbacks route through refresh_source()/refresh_capability(),
//    which upsert()/remove() actions. The registry keeps the only action list and
//    restores persisted user state as actions arrive.
// 3. Dynamic built-in families (settings, plates, recent projects) are re-materialised at the top of
//    snapshot(), because their membership follows live state (the current configs, plate list, recents).
// 4. Consumers use by_id(), snapshot(), and run() without knowing the source.
//
// note: the static catalog lives in NativeCommands; the registry owns the pool, persistence and
// dispatch, and materialises the dynamic families inline rather than behind a source interface.
class ActionRegistry
{
public:
    ~ActionRegistry();

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
    const AppAction* by_id(const std::string& id) const;

    // Hard cap on the favourites bar: the numbered quick-launch slots (Alt/Option+1..9, 0).
    static constexpr size_t kFavLimit = 10;

    // Dispatch + write-through (registry is the only thing that touches AppConfig).
    AppActionRunResult run(const std::string& id, const std::string& param = {}); // runs + bumps stats
    // Pin/unpin. Returns false when `on` would exceed kFavLimit (the bar is full) so the
    // caller can surface a "favourites are full" hint instead of silently dropping the pin.
    bool set_favourite(const std::string& id, bool on);
    void reorder_favourites(const std::vector<std::string>& ids); // persist a new bar order

    // Ordered pinned list (the source of truth), capped at kFavLimit and deduped, matching the
    // visible bar the palette renders.
    std::vector<std::string> favourite_ids() const;

    // Run-confirm gate, keyed by action id (per-action "don't ask again").
    bool should_ask(const std::string& id) const;
    void suppress_ask(const std::string& id);

    // Flat, frecency-sorted snapshot for the webview:
    // {actions:[...], favourites:[...], recent:[...]} (recent = last-N launched by recency).
    nlohmann::json snapshot();

    // "Go to tab..." Speed Dial helper: enumerate the MainFrame notebook's current pages
    // as [{id,title,icon},...], using the page's real label (not the compact-blanked button text).
    // Live by construction - built-in tabs (Home/Prepare/Preview/Device/Project/Calibration) and
    // plugin tabs (plugin.<key>.<name>) are all Notebook pages, so a page appears/disappears with
    // the notebook. Plugin tabs hidden in the overflow menu (many plugins) aren't separate pages and
    // are not listed. Call on the UI thread; null-safe.
    nlohmann::json tab_options() const;

private:
    void seed_state(AppAction& a) const; // favourite/stats from config
    AppAction* find(const std::string& id);

    // Read the persisted stats blob + capped favourite list once for a materialisation pass.
    void load_persisted(nlohmann::json& stats, std::vector<std::string>& favs) const;

    // (Re)materialise the current visible config settings as SettingActions from the live
    // searcher (respecting printer-tech + user-mode + visibility filtering), removing stale ones.
    // Called at the top of snapshot() so the palette always reflects the current configs.
    void materialize_setting_actions();

    // (Re)materialise one "Go to Plate N" action per live plate, so the palette lists every plate
    // directly on each spawn (no second-phase picker). FFF-editor only; SLA/gcode modes have no
    // plate UI, so nothing is materialised and stale ids are dropped. Called at the top of snapshot().
    void materialize_plate_actions();

    // (Re)materialise one "Open recent project <name>" action per recent project file, so the
    // palette lists every recent project and can load it by clicking. Keyed by file path (stable);
    // files that no longer exist are skipped and their stale ids dropped. Called at the top of snapshot().
    void materialize_recent_project_actions();

    // Loader callbacks (marshalled to the UI thread) land here. refresh_source rebuilds
    // one plugin's whole action set; refresh_capability touches a single capability.
    void refresh_source(const std::string& plugin_key, ActionChange change);
    void refresh_capability(const std::string& plugin_key, const std::string& capability, ActionChange change);

    bool m_started = false;                                                // init() runs exactly once; guards double-subscription
    std::unordered_map<std::string, std::shared_ptr<AppAction>> m_actions; // UI-thread confined; no lock
};

}} // namespace Slic3r::GUI
