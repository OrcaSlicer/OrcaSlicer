// A Windows ABI fixture only. It never opens a network connection or touches a printer.
#include "slic3r/Utils/BBLNetworkFunctions.hpp"
#include <thread>

using namespace Slic3r;
struct FakeAgent {
    OnMsgArrivedFn discovery;
    OnMessageFn local_message;
    QueueOnMainFn queue;
};
#define EXPORT extern "C" __declspec(dllexport)
EXPORT std::string bambu_network_get_version() { return "02.08.04.99"; }
EXPORT bool bambu_network_check_debug_consistent(bool debug)
{
#ifdef NDEBUG
    return !debug;
#else
    return debug;
#endif
}
EXPORT void* bambu_network_create_agent(std::string) { return new FakeAgent; }
EXPORT int bambu_network_destroy_agent(void* agent) { delete static_cast<FakeAgent*>(agent); return 0; }
EXPORT int bambu_network_set_queue_on_main_fn(void* agent, QueueOnMainFn fn)
{ static_cast<FakeAgent*>(agent)->queue = std::move(fn); return 0; }
EXPORT int bambu_network_set_on_ssdp_msg_fn(void* agent, OnMsgArrivedFn fn)
{ static_cast<FakeAgent*>(agent)->discovery = std::move(fn); return 0; }
EXPORT int bambu_network_set_on_local_message_fn(void* agent, OnMessageFn fn)
{ static_cast<FakeAgent*>(agent)->local_message = std::move(fn); return 0; }
EXPORT bool bambu_network_start_discovery(void* agent, bool start, bool)
{
    auto* fake = static_cast<FakeAgent*>(agent);
    if (start && fake->discovery) fake->discovery(R"({"dev_id":"fixture"})");
    return true;
}
EXPORT std::string bambu_network_get_user_selected_machine(void*) { return "fixture"; }
EXPORT int bambu_network_request_bind_ticket(void*, std::string* ticket)
{ if (ticket) *ticket = "fixture-ticket"; return 0; }
EXPORT int bambu_network_bind_detect(void*, std::string, std::string, detectResult& result)
{ result.dev_id = "fixture"; result.result_msg = "found"; return 0; }
EXPORT int bambu_network_start_local_print(void* agent, PrintParams params, OnUpdateStatusFn update, WasCancelledFn cancel)
{
    auto* fake = static_cast<FakeAgent*>(agent);
    // DLL-owned threads also issue reverse callbacks while its exported function is blocked.
    std::thread worker([&] {
        if (fake->local_message) fake->local_message(params.dev_id, params.filename);
        if (update) update(PrintingStageUpload, 0, params.filename);
    });
    worker.join();
    return cancel && cancel() ? BAMBU_NETWORK_ERR_CANCELED : 0;
}
