#include "PrinterAgentOperationConfig.hpp"

#include <boost/log/trivial.hpp>

#include <set>
#include <type_traits>
#include <utility>
#include <vector>

namespace Slic3r {
namespace {

template<typename T> bool read_option(const nlohmann::json& options, const char* key, T& value)
{
    auto it = options.find(key);
    if (it == options.end())
        return false;
    try {
        if constexpr (std::is_same_v<T, std::string>) {
            if (!it->is_string())
                return false;
        } else if constexpr (std::is_same_v<T, bool>) {
            if (!it->is_boolean())
                return false;
        } else if constexpr (std::is_same_v<T, int>) {
            if (!it->is_number_integer() && !it->is_number_unsigned())
                return false;
        }
        value = it->get<T>();
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

void merge_extra_options(nlohmann::json& extra, const nlohmann::json& options, std::string& opaque)
{
    auto it = options.find("extra_options");
    if (it == options.end())
        return;
    if (it->is_object()) {
        extra.update(*it);
    } else if (it->is_string()) {
        const std::string value = it->get<std::string>();
        // A string extra_options is the agent's own opaque payload. Keep it unchanged unless it is
        // itself a JSON object, in which case the unknown keys can be merged into it.
        nlohmann::json parsed = nlohmann::json::parse(value, nullptr, false);
        if (!parsed.is_discarded() && parsed.is_object())
            extra.update(parsed);
        else
            opaque = value;
    }
}

} // namespace

void apply_printer_agent_operation_options(const nlohmann::json& options, PrintParams& params)
{
    if (!options.is_object() || options.empty())
        return;

    read_option(options, "project_name", params.project_name);
    read_option(options, "preset_name", params.preset_name);
    read_option(options, "task_name", params.task_name);
    read_option(options, "ftp_folder", params.ftp_folder);
    read_option(options, "comments", params.comments);

    // The target is carried explicitly so a plugin can act on a printer that is not the
    // Orca-selected MachineObject. target_id wins over the PrintParams field name.
    read_option(options, "dev_id", params.dev_id);
    read_option(options, "target_id", params.dev_id);
    read_option(options, "connection_type", params.connection_type);
    read_option(options, "dev_ip", params.dev_ip);
    read_option(options, "username", params.username);
    read_option(options, "password", params.password);
    read_option(options, "use_ssl_for_ftp", params.use_ssl_for_ftp);
    read_option(options, "use_ssl_for_mqtt", params.use_ssl_for_mqtt);

    read_option(options, "ams_mapping", params.ams_mapping);
    read_option(options, "ams_mapping2", params.ams_mapping2);
    read_option(options, "ams_mapping_info", params.ams_mapping_info);
    read_option(options, "nozzle_mapping", params.nozzle_mapping);
    read_option(options, "nozzles_info", params.nozzles_info);

    read_option(options, "task_bed_type", params.task_bed_type);
    read_option(options, "task_bed_leveling", params.task_bed_leveling);
    read_option(options, "task_flow_cali", params.task_flow_cali);
    read_option(options, "task_vibration_cali", params.task_vibration_cali);
    read_option(options, "task_layer_inspect", params.task_layer_inspect);
    read_option(options, "task_record_timelapse", params.task_record_timelapse);
    read_option(options, "task_timelapse_use_internal", params.task_timelapse_use_internal);
    read_option(options, "task_use_ams", params.task_use_ams);
    read_option(options, "task_ext_change_assist", params.task_ext_change_assist);
    read_option(options, "try_emmc_print", params.try_emmc_print);

    read_option(options, "auto_bed_leveling", params.auto_bed_leveling);
    read_option(options, "auto_flow_cali", params.auto_flow_cali);
    read_option(options, "auto_offset_cali", params.auto_offset_cali);
    read_option(options, "extruder_cali_manual_mode", params.extruder_cali_manual_mode);

    // Everything the host does not map is a printer-specific option for the agent. Keep it in
    // extra_options as JSON, the field the agent already receives verbatim.
    static const std::set<std::string> host_keys = {
        "project_name", "preset_name", "task_name", "ftp_folder", "comments",
        "dev_id", "target_id", "connection_type", "dev_ip", "username", "password",
        "use_ssl_for_ftp", "use_ssl_for_mqtt",
        "ams_mapping", "ams_mapping2", "ams_mapping_info", "nozzle_mapping", "nozzles_info",
        "task_bed_type", "task_bed_leveling", "task_flow_cali", "task_vibration_cali",
        "task_layer_inspect", "task_record_timelapse", "task_timelapse_use_internal",
        "task_use_ams", "task_ext_change_assist", "try_emmc_print",
        "auto_bed_leveling", "auto_flow_cali", "auto_offset_cali", "extruder_cali_manual_mode"};

    nlohmann::json extra = nlohmann::json::object();
    std::string    opaque_extra;
    merge_extra_options(extra, options, opaque_extra);
    for (auto it = options.begin(); it != options.end(); ++it) {
        if (it.key() != "extra_options" && host_keys.find(it.key()) == host_keys.end())
            extra[it.key()] = it.value();
    }
    if (!opaque_extra.empty())
        params.extra_options = opaque_extra; // an opaque payload wins over unmerged unknown keys
    else if (!extra.empty())
        params.extra_options = extra.dump();
}

PrinterAgentOperationConfigStore& PrinterAgentOperationConfigStore::instance()
{
    static PrinterAgentOperationConfigStore store;
    return store;
}

namespace {

// The target a plugin supplies when it completes the operation. target_id is the explicit
// operation target; dev_id is accepted as the PrintParams spelling.
bool submitted_target(const nlohmann::json& options, std::string& target_id)
{
    for (const char* key : {"target_id", "dev_id"}) {
        auto it = options.find(key);
        if (it == options.end() || !it->is_string())
            continue;
        const std::string value = it->get<std::string>();
        if (!value.empty()) {
            target_id = value;
            return true;
        }
    }
    return false;
}

} // namespace

std::string PrinterAgentOperationConfigStore::begin(PrinterAgentOperationKind kind,
                                                    std::weak_ptr<IPrinterAgent> agent,
                                                    std::string              agent_id,
                                                    std::string              host_dev_id,
                                                    int                      plate_index,
                                                    bool                     all_plates,
                                                    std::string              plugin_key,
                                                    Completion               completion)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::string token = "printer-agent-operation-config-" + std::to_string(m_next_token++);
    Pending pending;
    pending.kind        = kind;
    pending.agent       = std::move(agent);
    pending.agent_id    = std::move(agent_id);
    pending.host_dev_id = std::move(host_dev_id);
    pending.plate_index = plate_index;
    pending.all_plates  = all_plates;
    pending.plugin_key  = std::move(plugin_key);
    pending.completion  = std::move(completion);
    m_pending.emplace(token, std::move(pending));
    return token;
}

bool PrinterAgentOperationConfigStore::set_target(const std::string& token, std::string target_id,
                                                  PrinterAgentOperationTargetSource source)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_pending.find(token);
    if (it == m_pending.end())
        return false;
    it->second.target_id     = std::move(target_id);
    it->second.target_source = source;
    return true;
}

bool PrinterAgentOperationConfigStore::take_pending_for_plugin(const std::string& token,
                                                               const std::string& plugin_key,
                                                               Pending& out)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_pending.find(token);
    if (it == m_pending.end())
        return false;
    if (plugin_key.empty() || it->second.plugin_key != plugin_key)
        return false;
    out = std::move(it->second);
    m_pending.erase(it);
    return true;
}

bool PrinterAgentOperationConfigStore::submit(const std::string& token, const std::string& plugin_key,
                                              nlohmann::json options, std::string* error)
{
    Pending pending;
    {
        // Removing the token from m_pending and recording it in m_submitted is one atomic state
        // transition. A concurrent cancel_for_plugin() either runs before it (the token is gone,
        // so submit fails) or after it (it removes the submitted handoff, so consume_submitted()
        // fails). It can never observe the gap between the two maps.
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_pending.find(token);
        if (it == m_pending.end()) {
            if (error)
                *error = "Unknown, already completed, or not owned by this plugin: " + token;
            return false;
        }
        if (plugin_key.empty() || it->second.plugin_key != plugin_key) {
            if (error)
                *error = "Unknown, already completed, or not owned by this plugin: " + token;
            return false;
        }
        if (!it->second.completion) {
            if (error)
                *error = "The pre-operation configuration has no completion handler: " + token;
            return false;
        }

        Pending& entry = it->second;
        std::string submitted_target_id;
        const bool  has_submitted_target = submitted_target(options, submitted_target_id);
        if (has_submitted_target &&
            (entry.target_source == PrinterAgentOperationTargetSource::HostSelected ||
             entry.target_source == PrinterAgentOperationTargetSource::AgentSelected)) {
            // A plugin-owned UI may deliberately take over a default target (the host selection or
            // the agent's reported selection).
            entry.target_id     = submitted_target_id;
            entry.target_source = PrinterAgentOperationTargetSource::Plugin;
        } else if (!entry.target_id.empty()) {
            // A target fixed by the agent or an earlier plugin decision cannot be switched later.
            if (has_submitted_target && submitted_target_id != entry.target_id) {
                if (error)
                    *error = "The operation was started for target '" + entry.target_id +
                             "' and cannot be completed for a different target";
                return false;
            }
        } else if (has_submitted_target) {
            // A plugin-owned UI may choose the target after the hook returned.
            entry.target_id     = submitted_target_id;
            entry.target_source = PrinterAgentOperationTargetSource::Plugin;
        } else {
            if (error)
                *error = "No target printer was selected for the operation";
            return false;
        }

        pending = std::move(entry);
        m_pending.erase(it);
        m_submitted[token] = Submitted{pending.plugin_key};
    }

    PrinterAgentOperationCompletion completion;
    completion.accepted      = true;
    completion.token         = token;
    completion.kind          = pending.kind;
    completion.plate_index   = pending.plate_index;
    completion.all_plates    = pending.all_plates;
    completion.agent_id      = pending.agent_id;
    completion.agent         = pending.agent;
    completion.plugin_key    = pending.plugin_key;
    completion.target_id     = pending.target_id;
    completion.target_source = pending.target_source;
    completion.options       = std::move(options);

    pending.completion(completion);
    return true;
}

bool PrinterAgentOperationConfigStore::cancel(const std::string& token, const std::string& plugin_key,
                                              std::string* error)
{
    Pending pending;
    if (!take_pending_for_plugin(token, plugin_key, pending)) {
        if (error)
            *error = "Unknown, already completed, or not owned by this plugin: " + token;
        return false;
    }
    if (pending.completion) {
        PrinterAgentOperationCompletion completion;
        completion.accepted      = false;
        completion.token         = token;
        completion.kind          = pending.kind;
        completion.plate_index   = pending.plate_index;
        completion.all_plates    = pending.all_plates;
        completion.agent_id      = pending.agent_id;
        completion.agent         = pending.agent;
        completion.plugin_key    = pending.plugin_key;
        completion.target_id     = pending.target_id;
        completion.target_source = pending.target_source;
        pending.completion(completion);
    }
    return true;
}

void PrinterAgentOperationConfigStore::discard(const std::string& token)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_submitted.erase(token);
    m_pending.erase(token);
}

void PrinterAgentOperationConfigStore::cancel_for_plugin(const std::string& plugin_key)
{
    std::vector<Pending> cancelled;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_pending.begin(); it != m_pending.end();) {
            if (it->second.plugin_key == plugin_key) {
                cancelled.push_back(std::move(it->second));
                it = m_pending.erase(it);
            } else {
                ++it;
            }
        }
        // A submit already handed its result to a queued UI callback. Removing the submitted entry
        // under this same mutex closes the race with submit(): a submit that committed before this
        // point is invalidated here, and a submit that arrives after it finds no pending token.
        for (auto it = m_submitted.begin(); it != m_submitted.end();) {
            if (it->second.plugin_key == plugin_key)
                it = m_submitted.erase(it);
            else
                ++it;
        }
    }
    for (Pending& pending : cancelled) {
        BOOST_LOG_TRIVIAL(info) << "printer_agent: cancelling pending "
                                << (pending.kind == PrinterAgentOperationKind::Print ? "print" : "send")
                                << " configuration after plugin unload: " << plugin_key
                                << ", target=" << pending.target_id
                                << (pending.host_dev_id.empty() ? "" : " (host " + pending.host_dev_id + ")")
                                << ", plate=" << pending.plate_index
                                << (pending.all_plates ? " (all)" : "");
        if (pending.completion) {
            PrinterAgentOperationCompletion completion;
            completion.accepted      = false;
            completion.token         = {};
            completion.kind          = pending.kind;
            completion.plate_index   = pending.plate_index;
            completion.all_plates    = pending.all_plates;
            completion.agent_id      = pending.agent_id;
            completion.agent         = pending.agent;
            completion.plugin_key    = pending.plugin_key;
            completion.target_id     = pending.target_id;
            completion.target_source = pending.target_source;
            pending.completion(completion);
        }
    }
}

bool PrinterAgentOperationConfigStore::consume_submitted(const std::string& token)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_submitted.erase(token) > 0;
}

bool PrinterAgentOperationConfigStore::is_pending(const std::string& token) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pending.find(token) != m_pending.end();
}

bool PrinterAgentOperationConfigStore::is_pending_for(const std::string& token, const std::string& plugin_key) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_pending.find(token);
    return it != m_pending.end() && !plugin_key.empty() && it->second.plugin_key == plugin_key;
}

std::size_t PrinterAgentOperationConfigStore::pending_count() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pending.size();
}

void PrinterAgentOperationConfigStore::reset_for_testing()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending.clear();
    m_submitted.clear();
}

} // namespace Slic3r
