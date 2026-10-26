#include <catch2/catch_all.hpp>

#include "slic3r/GUI/Jobs/PrintJob.hpp"
#include "slic3r/Utils/IPrinterAgent.hpp"
#include "slic3r/Utils/PrinterAgentOperationConfig.hpp"

#include <nlohmann/json.hpp>

#include <string>

using namespace Slic3r;

namespace {

struct Completion
{
    int            calls = 0;
    bool           accepted = false;
    std::string    token;
    std::string    target_id;
    nlohmann::json options;

    PrinterAgentOperationConfigStore::Completion fn()
    {
        return [this](const PrinterAgentOperationCompletion& completion) {
            ++calls;
            accepted  = completion.accepted;
            token     = completion.token;
            target_id = completion.target_id;
            options   = completion.options;
        };
    }
};

std::string begin_operation(PrinterAgentOperationConfigStore& store,
                            const std::string& plugin_key,
                            const std::string& host_dev_id,
                            Completion&        completion,
                            PrinterAgentOperationKind kind = PrinterAgentOperationKind::Print)
{
    return store.begin(kind, std::weak_ptr<IPrinterAgent>{}, "agent-id", host_dev_id, 2, false,
                       plugin_key, completion.fn());
}

} // namespace

TEST_CASE("A pre-operation config token completes once for its owning plugin", "[PrinterAgent][unit]")
{
    auto& store = PrinterAgentOperationConfigStore::instance();
    store.reset_for_testing();

    Completion completion;
    const std::string token = begin_operation(store, "plugin-a", "dev-1", completion);
    CHECK(store.is_pending(token));
    CHECK(store.is_pending_for(token, "plugin-a"));
    CHECK_FALSE(store.is_pending_for(token, "plugin-b"));

    std::string error;
    CHECK_FALSE(store.submit(token, "plugin-b", nlohmann::json::object(), &error));
    CHECK_FALSE(error.empty());
    CHECK(store.is_pending(token));
    CHECK(completion.calls == 0);

    nlohmann::json options = {{"task_use_ams", false}};
    CHECK(store.submit(token, "plugin-a", options, &error));
    CHECK(completion.calls == 1);
    CHECK(completion.accepted);
    CHECK(completion.target_id == "dev-1");
    CHECK(completion.options == options);
    CHECK_FALSE(store.is_pending(token));

    // The submitted handoff is valid exactly once.
    CHECK(store.consume_submitted(token));
    CHECK_FALSE(store.consume_submitted(token));

    // A second completion of the same token is rejected: the job must not start twice.
    CHECK_FALSE(store.submit(token, "plugin-a", nlohmann::json::object(), &error));
    CHECK(completion.calls == 1);

    store.reset_for_testing();
}

TEST_CASE("A plugin cancelling a pre-operation config starts no job", "[PrinterAgent][unit]")
{
    auto& store = PrinterAgentOperationConfigStore::instance();
    store.reset_for_testing();

    Completion completion;
    const std::string token = begin_operation(store, "plugin-a", "dev-2", completion,
                                              PrinterAgentOperationKind::Send);

    std::string error;
    CHECK_FALSE(store.cancel(token, "plugin-b", &error));
    CHECK(store.is_pending(token));

    CHECK(store.cancel(token, "plugin-a", &error));
    CHECK(completion.calls == 1);
    CHECK_FALSE(completion.accepted);
    CHECK_FALSE(store.is_pending(token));
    CHECK_FALSE(store.consume_submitted(token));

    store.reset_for_testing();
}

TEST_CASE("Discarding a pre-operation config token does not run its completion", "[PrinterAgent][unit]")
{
    auto& store = PrinterAgentOperationConfigStore::instance();
    store.reset_for_testing();

    Completion completion;
    const std::string token = begin_operation(store, "plugin-a", "dev-3", completion);
    store.discard(token);
    CHECK_FALSE(store.is_pending(token));
    CHECK_FALSE(store.consume_submitted(token));
    CHECK(completion.calls == 0);

    store.reset_for_testing();
}

TEST_CASE("Unloading a plugin cancels only that plugin's pending pre-operation configs", "[PrinterAgent][unit]")
{
    auto& store = PrinterAgentOperationConfigStore::instance();
    store.reset_for_testing();

    Completion first;
    Completion second;
    const std::string first_token  = begin_operation(store, "plugin-a", "dev-4", first);
    const std::string second_token = begin_operation(store, "plugin-b", "dev-5", second);

    store.cancel_for_plugin("plugin-a");
    CHECK(first.calls == 1);
    CHECK_FALSE(first.accepted);
    CHECK_FALSE(store.is_pending(first_token));
    CHECK(store.is_pending(second_token));
    CHECK(second.calls == 0);

    // A late submit from the unloaded plugin is rejected rather than starting its job.
    std::string error;
    CHECK_FALSE(store.submit(first_token, "plugin-a", nlohmann::json::object(), &error));
    CHECK(second.calls == 0);

    store.cancel_for_plugin("plugin-b");
    CHECK(second.calls == 1);
    CHECK_FALSE(second.accepted);
    store.reset_for_testing();
}

TEST_CASE("A plugin-owned target can be chosen at submit time", "[PrinterAgent][unit]")
{
    auto& store = PrinterAgentOperationConfigStore::instance();
    store.reset_for_testing();

    Completion completion;
    // No host-selected machine and no target in the hook answer.
    const std::string token = begin_operation(store, "plugin-a", "", completion);
    CHECK(store.is_pending(token));

    std::string error;
    // A plugin-owned UI must choose a target; submit without one is rejected and can be retried.
    CHECK_FALSE(store.submit(token, "plugin-a", nlohmann::json::object(), &error));
    CHECK_FALSE(error.empty());
    CHECK(store.is_pending(token));

    nlohmann::json options = {{"target_id", "plugin-target"}};
    CHECK(store.submit(token, "plugin-a", options, &error));
    CHECK(completion.calls == 1);
    CHECK(completion.accepted);
    CHECK(completion.target_id == "plugin-target");
    CHECK(store.consume_submitted(token));

    store.reset_for_testing();
}

TEST_CASE("A plugin-owned UI may replace a default agent-selected target", "[PrinterAgent][unit]")
{
    auto& store = PrinterAgentOperationConfigStore::instance();
    store.reset_for_testing();

    Completion completion;
    const std::string token = begin_operation(store, "plugin-a", "", completion);
    REQUIRE(store.set_target(token, "agent-target", PrinterAgentOperationTargetSource::AgentSelected));

    nlohmann::json options = {{"target_id", "plugin-target"}};
    CHECK(store.submit(token, "plugin-a", options));
    CHECK(completion.target_id == "plugin-target");
    CHECK(completion.calls == 1);

    store.reset_for_testing();
}

TEST_CASE("A submitted pre-operation config cannot switch a fixed target", "[PrinterAgent][unit]")
{
    auto& store = PrinterAgentOperationConfigStore::instance();
    store.reset_for_testing();

    Completion completion;
    const std::string token = begin_operation(store, "plugin-a", "host-target", completion);
    REQUIRE(store.set_target(token, "fixed-target", PrinterAgentOperationTargetSource::Plugin));

    std::string error;
    nlohmann::json conflicting = {{"target_id", "other-target"}};
    CHECK_FALSE(store.submit(token, "plugin-a", conflicting, &error));
    CHECK_FALSE(error.empty());
    CHECK(store.is_pending(token));
    CHECK(completion.calls == 0);

    nlohmann::json same = {{"target_id", "fixed-target"}};
    CHECK(store.submit(token, "plugin-a", same, &error));
    CHECK(completion.calls == 1);
    CHECK(completion.target_id == "fixed-target");

    store.reset_for_testing();
}

TEST_CASE("An unload after submit invalidates the queued start", "[PrinterAgent][unit]")
{
    auto& store = PrinterAgentOperationConfigStore::instance();
    store.reset_for_testing();

    Completion completion;
    const std::string token = begin_operation(store, "plugin-a", "dev-6", completion);

    REQUIRE(store.submit(token, "plugin-a", nlohmann::json::object()));
    REQUIRE(completion.calls == 1);

    // The UI callback is queued but the plugin unloads before it runs.
    store.cancel_for_plugin("plugin-a");
    CHECK_FALSE(store.consume_submitted(token));

    store.reset_for_testing();
}

TEST_CASE("An unload from the completion callback invalidates the queued start", "[PrinterAgent][unit]")
{
    auto& store = PrinterAgentOperationConfigStore::instance();
    store.reset_for_testing();

    int calls = 0;
    const std::string token = store.begin(
        PrinterAgentOperationKind::Print, std::weak_ptr<IPrinterAgent>{}, "agent-id", "dev-7", 2, false,
        "plugin-a", [&](const PrinterAgentOperationCompletion& completion) {
            ++calls;
            if (completion.accepted) {
                // The submit has not returned to its caller yet; an unload here must still
                // invalidate the submitted handoff.
                store.cancel_for_plugin("plugin-a");
            }
        });

    REQUIRE(store.submit(token, "plugin-a", nlohmann::json::object()));
    CHECK(calls == 1);
    CHECK_FALSE(store.consume_submitted(token));

    store.reset_for_testing();
}

TEST_CASE("Agent-routed print selects the LAN start from the resolved transport", "[PrinterAgent][unit]")
{
    CHECK(Slic3r::GUI::PrintJob::agent_routed_uses_local_start("lan"));
    CHECK_FALSE(Slic3r::GUI::PrintJob::agent_routed_uses_local_start("cloud"));
    CHECK_FALSE(Slic3r::GUI::PrintJob::agent_routed_uses_local_start(""));
}

TEST_CASE("Pre-operation options map onto PrintParams and keep agent-specific keys", "[PrinterAgent][unit]")
{
    PrintParams params{};
    params.task_use_ams = true;
    nlohmann::json options = {
        {"project_name", "bench"},
        {"preset_name", "0.2mm"},
        {"target_id", "printer-42"},
        {"connection_type", "lan"},
        {"ams_mapping", "[1,2]"},
        {"task_use_ams", false},
        {"auto_flow_cali", 2},
        {"printer_specific", "value"},
    };

    apply_printer_agent_operation_options(options, params);

    CHECK(params.project_name == "bench");
    CHECK(params.preset_name == "0.2mm");
    CHECK(params.dev_id == "printer-42");
    CHECK(params.connection_type == "lan");
    CHECK(params.ams_mapping == "[1,2]");
    CHECK_FALSE(params.task_use_ams);
    CHECK(params.auto_flow_cali == 2);

    // The host does not interpret printer-specific options; the agent receives them verbatim.
    const nlohmann::json extra = nlohmann::json::parse(params.extra_options);
    CHECK(extra.at("printer_specific") == "value");
    CHECK_FALSE(extra.contains("target_id"));
    CHECK_FALSE(extra.contains("project_name"));
    CHECK_FALSE(extra.contains("ams_mapping"));
}

TEST_CASE("An empty pre-operation config leaves PrintParams untouched", "[PrinterAgent][unit]")
{
    PrintParams params{};
    params.project_name = "unchanged";
    apply_printer_agent_operation_options(nlohmann::json(), params);
    apply_printer_agent_operation_options(nlohmann::json::object(), params);
    CHECK(params.project_name == "unchanged");
    CHECK(params.extra_options.empty());
}
