#pragma once

#include <nlohmann/json.hpp>

#include <libslic3r/Config.hpp>

#include <wx/string.h>
#include <wx/thread.h>

#include <cassert>
#include <cstddef>
#include <functional>
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
namespace Ops {
struct Refusal;
}

struct AppActionRunResult
{
    enum class Level { Success, Info, Error, Busy };

    Level level = Level::Info;
    wxString message; // empty = "nothing worth showing"
    // False when the command did nothing (no selection, no objects): the palette shows the message
    // at its level all the same, and automation reports the run as failed.
    bool done = true;
    // For automation only: a JSON object saying what the command did that its message does not
    // (the new object order, the object an Assemble made). An automated call's reply has its fields
    // (NativeCommands::from_run_result()); the palette never shows it.
    std::string reply;
    // The operation's refusal that `message` words in the UI's language, when the command ran an
    // operation that refused: automation replies with its English text and its code.
    std::shared_ptr<const Ops::Refusal> refusal;
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
    // Search-only alias when the title differs from the descriptive ConfigOptionDef name (e.g. title
    // "Reverse on even", full_label "Overhang reversal"). Empty when the two agree.
    std::string full_label;
    // Search-only English counterparts of title(), group, source_name() and full_label, so a
    // localized UI can still be searched in English. Each is derived exactly like the localized
    // text it mirrors, minus the translation, so in an English UI it equals that text. Empty when
    // there is no translated counterpart.
    std::string title_en;
    std::string group_en;
    std::string source_en;
    std::string full_label_en;
    // Full wiki URL, when the action has one (SettingActions whose row declared a label_path).
    std::string help_url;
    // For automation (MCP, scripts): what the action does in English, the arguments it takes (a JSON
    // Schema; none for most actions) and whether its effect finishes after run() returns (an export that
    // runs in the background). Not shown in the palette.
    std::string    automation_description;
    nlohmann::json input_schema;
    bool           completes_in_background = false;
    // The catalog key of a command's tile (NativeCommands), whose own function run_action runs; empty for
    // every other action.
    std::string command;

    virtual ~AppAction() = default;
    // Re-resolves + runs (UI thread). `args` are a command's arguments (the layer percentage of Go to
    // layer); the other actions take none.
    virtual AppActionRunResult run(const nlohmann::json& args = nlohmann::json::object()) const = 0;

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
// The prefix of a catalog command's tile id, "orca_command:<key>:orca" (NativeCommands::make_action()).
inline constexpr const char* kCommandPrefix = "orca_command";

// True when a setting at `setting_mode` cannot be edited in `current_mode` and the UI must switch
// first. Developer settings are handled as a separate prompt by the Speed Dial.
inline bool requires_mode_switch(ConfigOptionMode setting_mode, ConfigOptionMode current_mode)
{
    return setting_mode > current_mode;
}

// Cap + dedupe a persisted favourite-id list, preserving first-occurrence order. A stale or
// hand-edited config must never grow the stored list past `limit`, and a duplicated id must collapse to its first pin.
std::vector<std::string> cap_favourites(const std::vector<std::string>& ids, size_t limit);

// Whether `id` is the id of a Design tab tile, the tile of a Design command whose entry has an action. Such a
// tile is live only while that tab is shown, in its entry's mode. The Design table tells it apart, so it does
// even when the catalog holds no Design command (the CAD feature off at startup). A build without CAD has no
// table, and there it is never one.
bool is_design_tile_id(const std::string& id);

// The pins snapshot() keeps, in their order: `stored` gets those with a live action (`live`) and the Design
// tab's whose tile is not live now (is_design_tile_id()), which the person pinned in that tab and keeps when
// the Speed Dial opens elsewhere; `shown`, the bar, gets the first ActionRegistry::kFavLimit live ones. A live
// pin past them stays stored and comes onto the bar once a pin before it is unpinned. Any other pin with no live
// action is dropped.
void prune_favourites(const std::vector<std::string>& favs, const std::function<bool(const std::string&)>& live,
                      std::vector<std::string>& stored, std::vector<std::string>& shown);

// Pins `id` at the end of `favs`, the stored pins, unless the bar is full: it shows kFavLimit pins, counted as
// prune_favourites() shows them, so a pin that is not live takes no place on it. False, leaving `favs` as it is,
// when it is full or `favs` holds ActionRegistry::kFavStoredLimit pins; a pin already on the bar stays where it is.
bool add_favourite(std::vector<std::string>& favs, const std::string& id, const std::function<bool(const std::string&)>& live);

// `stored` in the order the Speed Dial page asks for, which sends the pins its bar shows: the places those pins
// hold in `stored` take them in the order of `ids`, and every other pin (one the bar does not show) keeps its
// place. An id that is not stored is ignored, and one given twice counts once. `stored` holds no id twice.
std::vector<std::string> reordered_favourites(std::vector<std::string> stored, const std::vector<std::string>& ids);

// One action as the Speed Dial page receives it. Each English alias (title_en, group_en, source_en,
// full_label_en) is sent only when it is non-empty and differs from its localized counterpart, so an
// English UI receives none and searches exactly the displayed text.
nlohmann::json action_json(const AppAction& a);

// Self-contained sink and single owner of runnable actions for the app session.
//
// Workflow:
// 1. init() (once, UI thread) subscribes to the plugin loader and enumerates the current script
//    capabilities into actions, then materialises the static built-ins from the NativeCommands catalog.
// 2. Loader load/unload callbacks route through refresh_source()/refresh_capability(),
//    which upsert()/remove() actions. The registry keeps the only action list and
//    restores persisted user state as actions arrive.
// 3. Dynamic built-in families (settings, plates, recent projects, the Design tab's tiles) are
//    re-materialised at the top of snapshot(), because their membership follows live state (the current
//    configs, plate list, recents, the tab shown and its mode); all but settings also at the top of
//    describe() and of find_live(), an automated lookup by id.
// 4. Consumers use by_id(), find_live(), snapshot(), and run() without knowing the source.
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

    // Rebuilds the built-in command actions in the current UI locale. The command catalog copies
    // translated titles/groups at construction, so after a live language switch the stored titles
    // are stale until this runs. Ids are key-based and upsert re-seeds persisted state, so
    // favourites/run history survive. UI thread only. No-op before init().
    void relocalize_builtins();

    // Takes ownership, seeds persisted state, then inserts the action or replaces
    // the action with the same id. A null action is ignored.
    void upsert(std::unique_ptr<AppAction> action);

    // Removes the action with this id. Missing ids are a harmless no-op.
    void remove(const std::string& id);

    // Always-clean read surface. UI thread only.
    const AppAction* by_id(const std::string& id) const;
    // The action `id` as the app is now: by_id() after the families whose membership follows live state
    // (plates, recent projects, the Design tab's tiles) are re-materialised, for a caller that may have
    // changed that state since the last snapshot() or describe(), such as an automated run_action after a
    // sketch was opened or the tab switched. Settings are not, being thousands: snapshot() and describe()
    // with include_settings materialise them. UI thread only.
    const AppAction* find_live(const std::string& id);

    // Hard cap on the favourites bar: the numbered quick-launch slots (Alt/Option+1..9, 0).
    static constexpr size_t kFavLimit = 10;
    // The stored pins at most. A pin is added only while the bar shows fewer than kFavLimit, so ordinary use
    // stores at most kFavLimit for each set of live actions the bar is shown with (the Design tab's model, a
    // sketch, and every other tab), thirty; reading the stored list cuts it to this, so a corrupt or
    // hand-edited configuration cannot grow it.
    static constexpr size_t kFavStoredLimit = 100;

    // Dispatch + write-through (registry is the only thing that touches AppConfig). Automation passes
    // record_stats = false so agent runs don't reorder the user's palette.
    AppActionRunResult run(const std::string& id, const nlohmann::json& args = nlohmann::json::object(), bool record_stats = true);

    // The catalog for agents: actions whose id, title, group, source or description contains `text`
    // (case-insensitive, English aliases included), sorted by group then title, one page of them as
    // {items, offset, total} with next_offset while more remain. Settings are listed only when asked
    // for, since there are thousands.
    struct Query
    {
        std::string text;
        std::string group;
        bool        include_settings = false;
        std::size_t offset           = 0;
        std::size_t limit            = 50;
    };
    nlohmann::json describe(const Query& query);
    // Pin/unpin. Returns false when `on` finds the bar full (add_favourite()) so the
    // caller can surface a "favourites are full" hint instead of silently dropping the pin.
    bool set_favourite(const std::string& id, bool on);
    void reorder_favourites(const std::vector<std::string>& ids); // persist a new bar order (reordered_favourites())

    // Ordered pinned list (the source of truth), capped at kFavStoredLimit and deduped: the bar's pins and
    // those it does not show (prune_favourites()).
    std::vector<std::string> favourite_ids() const;
    // The bar as it is now: the first kFavLimit live pins in stored order (prune_favourites()), the live actions
    // being the ones the open Speed Dial shows, as snapshot() materialised them. UI thread.
    std::vector<std::string> favourites_bar() const;

    // Run-confirm gate, keyed by action id (per-action "don't ask again").
    bool should_ask(const std::string& id) const;
    void suppress_ask(const std::string& id);

    // Footer expand/collapse preference. Global (applies to every action) and persisted; absent
    // means expanded, so a fresh config picks the richer default with no migration.
    bool tooltip_expanded() const;
    void set_tooltip_expanded(bool expanded);

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
    // Called at the top of snapshot(), so the palette always reflects the current configs, and by
    // describe() when it is asked for settings.
    void materialize_setting_actions();

    // (Re)materialise one "Go to Plate N" action per live plate, so the palette lists every plate
    // directly on each spawn (no second-phase picker). FFF-editor only; SLA/gcode modes have no
    // plate UI, so nothing is materialised and stale ids are dropped. Run by materialize_live().
    void materialize_plate_actions();

    // (Re)materialise one "Open recent project <name>" action per recent project file, so the
    // palette lists every recent project and can load it by clicking. Keyed by file path (stable);
    // files that no longer exist are skipped and their stale ids dropped. Run by materialize_live().
    void materialize_recent_project_actions();

    // (Re)materialise the Design tab's tiles: while the tab is shown with the CAD feature on, one per catalog
    // command of group kDesignGroup with in_palette whose entry is offered in the tab's mode
    // (Mcp::design_tiles()), titled as the right-click menu names its entry (the "Design" context) with the
    // entry's icon; otherwise none. A build without CAD has none.
    void materialize_design_actions();

    // The three families above whose membership follows live state and is cheap to rebuild: plates, recent
    // projects and the Design tab's tiles. snapshot(), describe() and find_live() run it first.
    void materialize_live();

    // Loader callbacks (marshalled to the UI thread) land here. refresh_source rebuilds
    // one plugin's whole action set; refresh_capability touches a single capability.
    void refresh_source(const std::string& plugin_key, ActionChange change);
    void refresh_capability(const std::string& plugin_key, const std::string& capability, ActionChange change);
    void refresh_page_capability(const std::string& plugin_key, const std::string& capability, ActionChange change);

    bool m_started = false;                                                // init() runs exactly once; guards double-subscription
    std::unordered_map<std::string, std::shared_ptr<AppAction>> m_actions; // UI-thread confined; no lock
};

}} // namespace Slic3r::GUI
