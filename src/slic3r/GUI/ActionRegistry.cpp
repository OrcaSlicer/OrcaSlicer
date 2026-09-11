#include "ActionRegistry.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "slic3r/plugin/PluginManager.hpp"

#include <libslic3r/AppConfig.hpp>
#include <slic3r/plugin/PythonPluginInterface.hpp>

#include <wx/thread.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <unordered_map>

namespace Slic3r { namespace GUI {

namespace {

constexpr const char* kConfigSection = "speed_dial";

nlohmann::json parse_config_json(const std::string& value, nlohmann::json fallback)
{
    nlohmann::json parsed = nlohmann::json::parse(value, nullptr, false);
    return parsed.is_discarded() ? std::move(fallback) : parsed;
}

nlohmann::json read_section(const char* key, nlohmann::json fallback)
{
    return parse_config_json(wxGetApp().app_config->get(kConfigSection, key), std::move(fallback));
}

void write_section(const char* key, const nlohmann::json& j)
{
    wxGetApp().app_config->set(kConfigSection, key, j.dump());
}

std::vector<std::string> read_string_array(const char* key)
{
    auto j = read_section(key, nlohmann::json::array());
    std::vector<std::string> v;
    for (auto& e : j)
        if (e.is_string())
            v.push_back(e.get<std::string>());
    return v;
}

// frecency = frequency + recency; score halves every 30 idle days.
double frecency_score(int count, long long last, long long now)
{
    if (count <= 0)
        return 0.0;
    constexpr double HALF_LIFE_DAYS = 30.0;
    double age = std::max(0.0, double(now - last) / 86400.0);
    return count * std::pow(2.0, -age / HALF_LIFE_DAYS);
}

// ---- script-plugin action source (the one and only source) ------------------

std::string find_loaded_source_name(PluginManager& manager, const std::string& plugin_key)
{
    PluginDescriptor descriptor;
    if (manager.try_get_plugin_descriptor(plugin_key, descriptor) && !descriptor.name.empty())
        return descriptor.name;
    return plugin_key;
}

// A runnable script capability exposed as a speed-dial action. source_key = plugin_key
// (identity), so a plugin display-name change does not re-key the action.
struct PluginScriptAction : AppAction
{
    static constexpr const char* kIdPrefix = "plugin_script_action";

    std::string plugin_key;
    std::string capability;

    // The id an action for (plugin_key, capability) would have - lets refresh_capability
    // remove a gone capability without materialising the action.
    static std::string id_for(const std::string& plugin_key, const std::string& capability)
    {
        return AppAction::compose_id(kIdPrefix, capability.empty() ? plugin_key : capability, plugin_key);
    }

    PluginScriptAction(std::string plugin_key_in, std::string capability_in, std::string source_name)
        : AppAction(kIdPrefix,
                    capability_in.empty() ? plugin_key_in : capability_in,  // title
                    plugin_key_in,                                          // source_key
                    std::move(source_name)),
          plugin_key(std::move(plugin_key_in)), capability(std::move(capability_in))
    {}

    AppActionRunResult run() const override
    {
        std::string error;
        const ExecutionResult result = PluginManager::instance().run_script_capability(plugin_key, capability, error);
        if (!error.empty())
            return {AppActionRunResult::Level::Error, from_u8(error)};

        const bool skipped = result.status == PluginResult::Skipped;
        const wxString fallback = skipped ? _L("Script plugin skipped.") : _L("Script plugin finished.");
        return {skipped ? AppActionRunResult::Level::Info : AppActionRunResult::Level::Success,
                result.message.empty() ? fallback : from_u8(result.message)};
    }
};

// Builds an action for a capability, or nullptr if it is not a currently-loaded,
// enabled script capability.
std::unique_ptr<AppAction> make_action(const std::string& plugin_key, const std::string& capability,
                                       const std::string& source_name)
{
    PluginManager& manager = PluginManager::instance();
    if (!manager.is_plugin_loaded(plugin_key))
        return nullptr;
    // only_enabled defaults true, so a disabled capability resolves to nullptr here.
    if (!manager.get_plugin_capability({PluginCapabilityType::Script, capability, plugin_key}))
        return nullptr;
    return std::make_unique<PluginScriptAction>(plugin_key, capability, source_name);
}

} // namespace

void ActionRegistry::init()
{
    assert(wxThread::IsMain());
    assert(!m_started);
    m_started = true;

    PluginManager& manager = PluginManager::instance();

    auto on_source = [this](const std::string& plugin_key, ActionChange change) {
        if (!wxTheApp || wxGetApp().is_closing())
            return;
        wxGetApp().CallAfter([this, plugin_key, change] {
            if (!wxGetApp().is_closing())
                this->refresh_source(plugin_key, change);
        });
    };
    auto on_capability = [this](const PluginCapabilityId& capability, ActionChange change) {
        if (capability.type != PluginCapabilityType::Script || !wxTheApp || wxGetApp().is_closing())
            return;
        const std::string plugin_key = capability.plugin_key;
        const std::string name       = capability.name;
        wxGetApp().CallAfter([this, plugin_key, name, change] {
            if (!wxGetApp().is_closing())
                this->refresh_capability(plugin_key, name, change);
        });
    };

    // Subscribe before enumerating so a concurrent load cannot land between the initial
    // snapshot and callback registration. Duplicate notifications are safe: upsert is by
    // id and the m_actions scan in refresh_source is idempotent.
    manager.subscribe_on_load_callback(
        [on_source](const std::string& key) { on_source(key, ActionChange::Added); });
    manager.subscribe_on_unload_callback(
        [on_source](const std::string& key) { on_source(key, ActionChange::Removed); });
    manager.subscribe_on_capability_load_callback(
        [on_capability](const PluginCapabilityId& capability) {
            on_capability(capability, ActionChange::Added);
        });
    manager.subscribe_on_capability_unload_callback(
        [on_capability](const PluginCapabilityId& capability) {
            on_capability(capability, ActionChange::Removed);
        });

    // enumerate current script capabilities
    std::unordered_map<std::string, std::string> source_names;
    for (const PluginDescriptor& desc : manager.get_plugin_descriptors())
        if (!desc.name.empty())
            source_names.emplace(desc.plugin_key, desc.name);

    for (const auto& capability : manager.get_plugin_capabilities("", PluginCapabilityType::Script)) {
        if (!capability)
            continue;
        const std::string& key = capability->audit_plugin_key();
        auto it = source_names.find(key);
        const std::string& source_name = it == source_names.end() ? key : it->second;
        if (auto action = make_action(key, capability->name(), source_name))
            upsert(std::move(action));
    }
}

void ActionRegistry::refresh_source(const std::string& plugin_key, ActionChange change)
{
    assert(wxThread::IsMain());

    // Remove this source's current actions. Collect first - erasing from m_actions while
    // iterating invalidates the iterator. why: m_actions (not the loader) is the source of
    // truth, so this is correct even after the plugin has already unloaded.
    std::vector<std::string> stale;
    for (const auto& [id, action] : m_actions)
        if (action->source_key() == plugin_key)
            stale.push_back(id);
    for (const std::string& id : stale)
        remove(id);

    if (change == ActionChange::Removed)
        return;

    PluginManager& manager = PluginManager::instance();
    const std::string source_name = find_loaded_source_name(manager, plugin_key);
    for (const auto& capability : manager.get_plugin_capabilities(plugin_key, PluginCapabilityType::Script)) {
        if (!capability)
            continue;
        if (auto action = make_action(plugin_key, capability->name(), source_name))
            upsert(std::move(action));
    }
}

void ActionRegistry::refresh_capability(const std::string& plugin_key, const std::string& capability,
                                        ActionChange change)
{
    assert(wxThread::IsMain());

    const std::string id = PluginScriptAction::id_for(plugin_key, capability);
    if (change == ActionChange::Removed) {
        remove(id);
        return;
    }

    PluginManager& manager = PluginManager::instance();
    if (auto action = make_action(plugin_key, capability, find_loaded_source_name(manager, plugin_key)))
        upsert(std::move(action));
    else
        remove(id);
}

void ActionRegistry::upsert(std::unique_ptr<AppAction> action)
{
    assert(wxThread::IsMain());
    if (!action)
        return;

    seed_state(*action);
    std::string id = action->id();
    std::shared_ptr<AppAction> stored = std::move(action);
    m_actions.insert_or_assign(std::move(id), std::move(stored));
}

void ActionRegistry::remove(const std::string& id)
{
    assert(wxThread::IsMain());
    m_actions.erase(id);
}

void ActionRegistry::seed_state(AppAction& a) const
{
    auto favs = read_string_array("favourite_actions");
    a.favourite = std::find(favs.begin(), favs.end(), a.id()) != favs.end();

    nlohmann::json stats = read_section("stats", nlohmann::json::object());
    auto it = stats.find(a.id());
    if (it != stats.end() && it->is_object()) {
        a.count = it->value("count", 0);
        a.last  = it->value("last", 0LL);
    } else {
        a.count = 0;
        a.last  = 0;
    }
}

// ---- read surface -----------------------------------------------------------

const AppAction* ActionRegistry::by_id(const std::string& id) const
{
    assert(wxThread::IsMain());
    auto it = m_actions.find(id);
    return it == m_actions.end() ? nullptr : it->second.get();
}

AppAction* ActionRegistry::find(const std::string& id)
{
    return const_cast<AppAction*>(by_id(id));
}

// ---- dispatch + write-through ----------------------------------------------

AppActionRunResult ActionRegistry::run(const std::string& id)
{
    assert(wxThread::IsMain());
    auto it = m_actions.find(id);
    if (it == m_actions.end())
        return {}; // default Info, empty message
    // why: hold a shared_ptr keep-alive, never a bare map entry. A runner may pump a
    // nested event loop; a queued source refresh can erase the entry while the
    // keep-alive preserves the action until run returns.
    std::shared_ptr<AppAction> keep = it->second;
    AppActionRunResult o = keep->run();
    if (o.level == AppActionRunResult::Level::Busy)
        return o;

    // Bump stats (write-through). Re-read to avoid clobbering a concurrent field.
    nlohmann::json stats = read_section("stats", nlohmann::json::object());
    if (!stats.is_object())  // corrupt (valid-JSON, non-object) value degrades to empty
        stats = nlohmann::json::object();
    nlohmann::json& e = stats[id];
    if (!e.is_object())
        e = nlohmann::json::object();
    e["count"] = e.value("count", 0) + 1;
    e["last"]  = (long long) std::time(nullptr);
    write_section("stats", stats);
    if (AppAction* live = find(id)) { live->count = e["count"]; live->last = e["last"]; }
    return o;
}

void ActionRegistry::set_favourite(const std::string& id, bool on)
{
    assert(wxThread::IsMain());
    auto favs = read_string_array("favourite_actions");
    auto it   = std::find(favs.begin(), favs.end(), id);
    if (on && it == favs.end())
        favs.push_back(id);
    if (!on && it != favs.end())
        favs.erase(it);
    write_section("favourite_actions", nlohmann::json(favs));
    if (AppAction* live = find(id))
        live->favourite = on;
}

void ActionRegistry::reorder_favourites(const std::vector<std::string>& ids)
{
    assert(wxThread::IsMain());
    auto cur = read_string_array("favourite_actions");
    std::vector<std::string> next;
    // keep the requested order, but only ids that are actually favourites (guard a bad payload)
    for (const auto& id : ids)
        if (std::find(cur.begin(), cur.end(), id) != cur.end() &&
            std::find(next.begin(), next.end(), id) == next.end())
            next.push_back(id);
    // why: don't drop favourites the page omitted (e.g. pins with no live action hidden from the bar)
    for (const auto& id : cur)
        if (std::find(next.begin(), next.end(), id) == next.end())
            next.push_back(id);
    write_section("favourite_actions", nlohmann::json(next));
}

bool ActionRegistry::should_ask(const std::string& id) const
{
    assert(wxThread::IsMain());
    auto arr = read_string_array("ask_suppressed");
    return std::find(arr.begin(), arr.end(), id) == arr.end();
}

void ActionRegistry::suppress_ask(const std::string& id)
{
    assert(wxThread::IsMain());
    auto arr = read_string_array("ask_suppressed");
    if (std::find(arr.begin(), arr.end(), id) == arr.end())
        arr.push_back(id);
    write_section("ask_suppressed", nlohmann::json(arr));
}

// ---- snapshot ---------------------------------------------------------------

nlohmann::json ActionRegistry::snapshot() const
{
    assert(wxThread::IsMain());
    std::vector<const AppAction*> sorted;
    sorted.reserve(m_actions.size());
    for (const auto& entry : m_actions)
        sorted.push_back(entry.second.get());

    const long long now = (long long) std::time(nullptr);
    std::sort(sorted.begin(), sorted.end(), [&](const AppAction* a, const AppAction* b) {
        double sa = frecency_score(a->count, a->last, now);
        double sb = frecency_score(b->count, b->last, now);
        if (sa != sb)
            return sa > sb;
        if (a->title() != b->title())
            return a->title() < b->title();
        if (a->source_name() != b->source_name())
            return a->source_name() < b->source_name();
        return a->id() < b->id();
    });

    nlohmann::json actions = nlohmann::json::array();
    for (const AppAction* a : sorted)
        actions.push_back({{"id", a->id()},
                           {"title", a->title()},
                           {"source", a->source_name()},
                           {"shortcut", ""}});
    // why: favourites is the ORDERED pin list - it must come from favourite_actions
    // as stored, not be re-derived from the frecency-sorted actions (that would
    // reorder the favourites bar). The page (js) filters out ids with no live action itself.
    nlohmann::json favourites(read_string_array("favourite_actions"));
    return {{"actions", std::move(actions)}, {"favourites", std::move(favourites)}};
}

}} // namespace Slic3r::GUI
