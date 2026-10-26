#ifndef slic3r_PrinterAgentOperationConfig_hpp_
#define slic3r_PrinterAgentOperationConfig_hpp_

#include "IPrinterAgent.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Slic3r {

// Where the operation's target printer came from. It decides which value must still match when
// an asynchronous plugin UI completes: a host-selected target must still be the selected machine,
// and an agent-selected target must still be the agent's selected machine. A target explicitly
// supplied by the plugin is authoritative and is not invalidated by host selection changes.
enum class PrinterAgentOperationTargetSource {
    None = 0,
    Plugin,
    HostSelected,
    AgentSelected
};

// What the asynchronous completion hands back to the action that started it.
struct PrinterAgentOperationCompletion
{
    // False when the plugin cancelled or its plugin unloaded before completion.
    bool                            accepted = false;
    // Token of the pending operation. The GUI consumes it (consume_submitted) immediately before
    // starting the job, so a plugin unload between submit and the queued start still wins.
    std::string                     token;
    PrinterAgentOperationKind       kind = PrinterAgentOperationKind::Print;
    int                             plate_index = -1;
    bool                            all_plates  = false;
    std::string                     agent_id;
    std::weak_ptr<IPrinterAgent>    agent;
    std::string                     plugin_key;
    std::string                     target_id;
    PrinterAgentOperationTargetSource target_source = PrinterAgentOperationTargetSource::None;
    nlohmann::json                  options = nlohmann::json::object();
};

// Host-side registry for asynchronous, plugin-owned pre-operation UI.
//
// A printer agent whose configure_operation() answers PrinterAgentUiMode::Plugin keeps the UI
// open after the Plater action handler has returned. The host records the pending action under
// the token handed to the agent; the plugin later completes it from its HTML UI (or any other
// asynchronous callback) through orca.host.printer_agent.submit_operation_config() or
// cancel_operation_config(). Tokens are scoped to the plugin that owns the active printer agent:
// another plugin cannot complete or cancel them, and an unload cancels every token the unloaded
// plugin still owns so a late submit cannot start a job for a plugin that is gone.
//
// submit() keeps the completed token in a second map until the GUI acknowledges it with
// consume_submitted(). An unload that happens after submit but before the queued start removes
// that entry, so the start is ignored.
//
// The store is thread-safe and does not touch wx. Completion runs on the thread that submits or
// cancels; GUI callers marshal to the main thread themselves.
class PrinterAgentOperationConfigStore
{
public:
    using Completion = std::function<void(const PrinterAgentOperationCompletion&)>;

    static PrinterAgentOperationConfigStore& instance();

    // Reserve a token for one operation. `agent`/`agent_id` identify the initiating agent, and
    // `host_dev_id` is the machine the Orca UI had selected at that moment (empty if none).
    std::string begin(PrinterAgentOperationKind kind,
                      std::weak_ptr<IPrinterAgent> agent,
                      std::string              agent_id,
                      std::string              host_dev_id,
                      int                      plate_index,
                      bool                     all_plates,
                      std::string              plugin_key,
                      Completion               completion);

    // Record the target resolved after configure_operation() answered.
    bool set_target(const std::string& token, std::string target_id,
                    PrinterAgentOperationTargetSource source);

    // Complete a token owned by `plugin_key`. Returns false and sets *error when the token is
    // unknown, already completed, owned by another plugin, or when the submitted target conflicts
    // with the target fixed at initiation. The completion is invoked with accepted == true.
    // Moving the token to the submitted state is atomic with cancel_for_plugin(): an unload either
    // sees the pending token or invalidates the submitted handoff, never a gap between them.
    bool submit(const std::string& token, const std::string& plugin_key, nlohmann::json options,
                std::string* error = nullptr);

    // Cancel a token owned by `plugin_key`. The completion is invoked with accepted == false.
    bool cancel(const std::string& token, const std::string& plugin_key, std::string* error = nullptr);

    // Host-side withdrawal for a token whose answer was DefaultDialog or None_: the caller has
    // already chosen that branch, so the completion must not run.
    void discard(const std::string& token);

    // Cancel every pending token owned by the plugin and invalidate every submitted-but-not-yet-
    // started token, so a plugin that disappears mid-UI cannot strand the action or start a late
    // job.
    void cancel_for_plugin(const std::string& plugin_key);

    // Acknowledge a submitted token immediately before starting its job. Returns false if the
    // token was cancelled (for example by an unload) while the start was queued.
    bool consume_submitted(const std::string& token);

    bool is_pending(const std::string& token) const;

    // True only while `token` is pending and owned by `plugin_key`.
    bool is_pending_for(const std::string& token, const std::string& plugin_key) const;

    std::size_t pending_count() const;

    // Test-only: forget every token without invoking completions.
    void reset_for_testing();

private:
    struct Pending
    {
        PrinterAgentOperationKind kind = PrinterAgentOperationKind::Print;
        std::weak_ptr<IPrinterAgent> agent;
        std::string               agent_id;
        std::string               host_dev_id;
        std::string               target_id;
        PrinterAgentOperationTargetSource target_source = PrinterAgentOperationTargetSource::None;
        int                       plate_index = -1;
        bool                      all_plates  = false;
        std::string               plugin_key;
        Completion                completion;
    };

    struct Submitted
    {
        std::string plugin_key;
    };

    PrinterAgentOperationConfigStore() = default;

    bool take_pending_for_plugin(const std::string& token, const std::string& plugin_key, Pending& out);

    mutable std::mutex                          m_mutex;
    std::unordered_map<std::string, Pending>    m_pending;
    std::unordered_map<std::string, Submitted>  m_submitted;
    std::uint64_t                               m_next_token = 1;
};

} // namespace Slic3r

#endif // slic3r_PrinterAgentOperationConfig_hpp_
