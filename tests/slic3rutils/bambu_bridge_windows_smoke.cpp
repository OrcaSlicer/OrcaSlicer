#include "slic3r/Utils/BBLNetworkFunctions.hpp"
#include "slic3r/Utils/BambuBridge/Bindings.hpp"
#include "slic3r/Utils/BambuBridge/Client.hpp"
#include "slic3r/Utils/BambuBridge/WinPipe.hpp"
#include "slic3r/Utils/BambuNetworkTypes.hpp"
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <fstream>
#include <future>
#include <chrono>
#include <cstddef>
#include <system_error>
#include <utility>

using namespace Slic3r;
using namespace Slic3r::BambuBridge;

namespace {
using namespace std::chrono_literals;
struct UploadFiles {
    std::filesystem::path directory;
    UploadFiles()
    {
        directory = std::filesystem::temp_directory_path() /
            (L"Orca bridge \u2603 " + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directory(directory);
        write(directory / L"upload \u2603.3mf", 16 * 1024 * 1024 + 257);
        write(directory / L"config \u2603.3mf", 8193);
    }
    static void write(const std::filesystem::path& path, std::size_t size)
    {
        std::ofstream output(path, std::ios::binary);
        std::string bytes(size, '\0');
        for (std::size_t i = 0; i < size; ++i) bytes[i] = static_cast<char>(i * 37 + 11);
        output.write(bytes.data(), bytes.size());
        output.close();
        if (!output) throw std::runtime_error("Cannot write upload fixture");
    }
    ~UploadFiles() { std::error_code error; std::filesystem::remove_all(directory, error); }
};
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
        require(SetEnvironmentVariableW(L"ORCA_BAMBU_FIXTURE_VERSION", L"02.03.00.00"), "Fixture ABI override");
        bool rejected = false;
        try { client.start(dll); } catch (const std::exception&) { rejected = true; }
        require(rejected, "Unsupported DLL ABI is rejected");
        require(!client.running(), "Failed handshake leaves no active session");
        require(SetEnvironmentVariableW(L"ORCA_BAMBU_FIXTURE_VERSION", nullptr), "Clear fixture ABI override");
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
        UploadFiles files;
        PrintParams upload;
        upload.dev_id = "fixture";
        upload.filename = (files.directory / L"upload \u2603.3mf").u8string();
        upload.config_filename = (files.directory / L"config \u2603.3mf").u8string();
        upload.plate_index = 7;
        upload.ftp_folder = "folder";
        upload.ftp_file = "upload.3mf";
        upload.ftp_file_md5 = "fixture-md5";
        upload.use_ssl_for_ftp = true;
        upload.auto_flow_cali = 2;
        bool waited = false;
        require(function<func_start_send_gcode_to_sdcard>("bambu_network_start_send_gcode_to_sdcard")(agent, upload,
            [&](int, int, std::string file) { progress = std::move(file); }, [] { return false; },
            [&](int, std::string) { waited = true; return true; }) == 0, "DLL reads upload and config bytes intact");
        require(progress == upload.filename, "Upload path survives Unicode and spaces");
        require(waited, "Upload wait callback");
        // A missing file must produce the DLL's failure rather than fabricated success.
        upload.filename += ".missing";
        require(function<func_start_send_gcode_to_sdcard>("bambu_network_start_send_gcode_to_sdcard")(
            agent, upload, nullptr, nullptr, nullptr) == BAMBU_NETWORK_ERR_CHECK_MD5_FAILED, "Missing upload fails");
        function<func_set_on_ssdp_msg_fn>("bambu_network_set_on_ssdp_msg_fn")(agent, nullptr);
        require(function<func_destroy_agent>("bambu_network_destroy_agent")(agent) == 0, "Agent destruction");
        client.stop();
        require(!client.running(), "Helper shutdown");
        // New process/session must not inherit old callbacks or handles.
        require(SetEnvironmentVariableW(L"ORCA_BAMBU_HELPER", L"1"), "Packaged helper opt-in");
        client.start(dll);
        agent = function<func_create_agent>("bambu_network_create_agent")("logs");
        require(agent != nullptr, "Agent after restart");
        require(function<func_destroy_agent>("bambu_network_destroy_agent")(agent) == 0, "Destroy after restart");
        client.stop();
        client.start(dll);
        agent = function<func_create_agent>("bambu_network_create_agent")("logs");
        std::promise<void> entered;
        auto entry = entered.get_future();
        params.task_name = "block-until-terminated";
        auto blocked = std::async(std::launch::async, [&] {
            return function<func_start_local_print>("bambu_network_start_local_print")(agent, params,
                [&](int, int, std::string) { entered.set_value(); }, [] { return false; });
        });
        const auto entry_status = entry.wait_for(5s);
        client.stop(); // Must terminate the DLL even though its upload export never returns.
        require(entry_status == std::future_status::ready, "Blocking upload entered DLL");
        require(blocked.wait_for(5s) == std::future_status::ready, "Disconnect releases upload caller");
        require(blocked.get() == BAMBU_NETWORK_ERR_INVALID_RESULT, "Interrupted upload fails without retry");
        require(!client.running(), "Stopped during upload");
        std::cout << "Bambu helper process smoke test passed\n";
        return 0;
    } catch (const std::exception& e) {
        client.stop();
        std::cerr << e.what() << '\n';
        return 1;
    }
}
