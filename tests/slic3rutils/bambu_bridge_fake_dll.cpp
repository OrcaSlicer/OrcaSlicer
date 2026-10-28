// A Windows ABI fixture. It reads local upload fixtures but never contacts a printer.
#include "slic3r/Utils/BBLNetworkFunctions.hpp"
#include "slic3r/Utils/BambuNetworkTypes.hpp"
#include <ios>
#include <string>
#include <thread>
#include <filesystem>
#include <fstream>
#include <windows.h>
#include <cstddef>
#include <utility>

using namespace Slic3r;
struct FakeAgent {
    OnMsgArrivedFn discovery;
    OnMessageFn local_message;
    QueueOnMainFn queue;
};
#define EXPORT extern "C" __declspec(dllexport)
EXPORT std::string bambu_network_get_version()
{
    char version[64];
    const auto count = GetEnvironmentVariableA("ORCA_BAMBU_FIXTURE_VERSION", version, sizeof(version));
    return count && count < sizeof(version) ? std::string(version, count) : "02.08.04.99";
}
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
    if (params.task_name == "block-until-terminated") {
        if (update) update(PrintingStageUpload, 0, "entered");
        Sleep(INFINITE); // Model a DLL that ignores cancellation and never returns.
    }
    // DLL-owned threads also issue reverse callbacks while its exported function is blocked.
    std::thread worker([&] {
        if (fake->local_message) fake->local_message(params.dev_id, params.filename);
        if (update) update(PrintingStageUpload, 0, params.filename);
    });
    worker.join();
    return cancel && cancel() ? BAMBU_NETWORK_ERR_CANCELED : 0;
}

namespace {
bool verify_file(const std::string& filename, std::size_t expected_size)
{
    std::ifstream input(std::filesystem::u8path(filename), std::ios::binary);
    if (!input) return false;
    char buffer[4096];
    std::size_t offset = 0;
    while (input.read(buffer, sizeof(buffer)) || input.gcount()) {
        for (std::streamsize i = 0; i < input.gcount(); ++i, ++offset)
            if (static_cast<unsigned char>(buffer[i]) != static_cast<unsigned char>(offset * 37 + 11)) return false;
    }
    return input.eof() && offset == expected_size;
}
}

EXPORT int bambu_network_start_send_gcode_to_sdcard(void*, PrintParams params,
    OnUpdateStatusFn update, WasCancelledFn cancel, OnWaitFn wait)
{
    // Check transfer-relevant fields independently of the production JSON schema.
    if (params.dev_id != "fixture" || params.plate_index != 7 || params.ftp_folder != "folder" ||
        params.ftp_file != "upload.3mf" || params.ftp_file_md5 != "fixture-md5" ||
        !params.use_ssl_for_ftp || params.use_ssl_for_mqtt || params.task_use_ams ||
        params.try_emmc_print || params.auto_flow_cali != 2 || params.extruder_cali_manual_mode != -1)
        return BAMBU_NETWORK_ERR_INVALID_RESULT;
    if (!verify_file(params.filename, 16 * 1024 * 1024 + 257) || !verify_file(params.config_filename, 8193))
        return BAMBU_NETWORK_ERR_CHECK_MD5_FAILED;
    if (update) update(PrintingStageUpload, 100, params.filename);
    if (cancel && cancel()) return BAMBU_NETWORK_ERR_CANCELED;
    if (wait && !wait(PrintingStageWaitPrinter, "ready")) return BAMBU_NETWORK_ERR_INVALID_RESULT;
    return 0;
}
