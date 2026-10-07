#include "slic3r/Utils/BambuBridge/Client.hpp"
#include "slic3r/Utils/BambuBridge/WinPipe.hpp"
#include <filesystem>
#include <iostream>

using namespace Slic3r;
using namespace Slic3r::BambuBridge;

namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> F function(const char* name)
{
    auto pointer = Client::instance().function(name);
    require(pointer != nullptr, name);
    return reinterpret_cast<F>(pointer);
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 3) return 2;
    auto& client = Client::instance();
    try {
        require(SetEnvironmentVariableW(L"ORCA_BAMBU_HELPER", argv[1]), "SetEnvironmentVariable");
        std::string dll = std::filesystem::path(argv[2]).u8string();
        client.start(dll);
        require(client.version() == "02.08.04.99", "DLL version");
        require(!client.function("bambu_network_get_subtask"), "Unsupported exports stay unavailable");
        void* agent = function<func_create_agent>("bambu_network_create_agent")("logs");
        require(agent == agent_token(), "Opaque agent token");
        std::string discovered;
        function<func_set_on_ssdp_msg_fn>("bambu_network_set_on_ssdp_msg_fn")(agent,
            [&](std::string message) { discovered = std::move(message); });
        require(function<func_start_discovery>("bambu_network_start_discovery")(agent, true, true), "Discovery result");
        require(discovered.find("fixture") != std::string::npos, "Discovery callback");
        std::string ticket;
        require(function<func_request_bind_ticket>("bambu_network_request_bind_ticket")(agent, &ticket) == 0, "Output pointer result");
        require(ticket == "fixture-ticket", "Output pointer");
        detectResult detect{};
        require(function<func_bind_detect>("bambu_network_bind_detect")(agent, "ip", "code", detect) == 0, "Output reference result");
        require(detect.dev_id == "fixture", "Output reference");
        std::string selected, progress;
        function<func_set_on_local_message_fn>("bambu_network_set_on_local_message_fn")(agent,
            [&](std::string, std::string) {
                // The callback runs while the helper waits for the DLL's own worker thread.
                selected = function<func_get_user_selected_machine>("bambu_network_get_user_selected_machine")(agent);
            });
        PrintParams params{};
        params.dev_id = "fixture";
        params.filename = "C:/prints/\xe2\x98\x83.3mf";
        const int result = function<func_start_local_print>("bambu_network_start_local_print")(agent, params,
            [&](int, int, std::string file) { progress = std::move(file); }, [] { return true; });
        require(result == BAMBU_NETWORK_ERR_CANCELED, "Cancellation callback result");
        require(progress == params.filename, "Unicode PrintParams and progress");
        require(selected == "fixture", "DLL-thread callback with nested RPC");
        function<func_set_on_ssdp_msg_fn>("bambu_network_set_on_ssdp_msg_fn")(agent, nullptr);
        require(function<func_destroy_agent>("bambu_network_destroy_agent")(agent) == 0, "Agent destruction");
        client.stop();
        require(!client.running(), "Helper shutdown");
        // New process/session must not inherit old callbacks or handles.
        client.start(dll);
        agent = function<func_create_agent>("bambu_network_create_agent")("logs");
        require(agent != nullptr, "Agent after restart");
        require(function<func_destroy_agent>("bambu_network_destroy_agent")(agent) == 0, "Destroy after restart");
        client.stop();
        std::cout << "Bambu helper process smoke test passed\n";
        return 0;
    } catch (const std::exception& e) {
        client.stop();
        std::cerr << e.what() << '\n';
        return 1;
    }
}
