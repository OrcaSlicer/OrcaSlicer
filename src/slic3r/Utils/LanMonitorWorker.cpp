#include "slic3r/Utils/LanMonitorSdk.hpp"
#include "slic3r/Utils/LanMonitorSnapshot.hpp"
#include "slic3r/Utils/JsonReport.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using MessageFn = std::function<void(std::string, std::string)>;
using ConnectedFn = std::function<void(std::string)>;
using LocalConnectFn = std::function<void(int, std::string, std::string)>;
using QueueFn = std::function<void(std::function<void()>)>;
using SendFn = int (*)(void*, std::string, std::string, int, int);

void emit(const json& value) { std::cout << "ORCA_LAN_MONITOR " << value.dump() << std::endl; }

struct Target
{
    std::string id;
    std::string host;
    std::string code;
    std::uint64_t reports = 0;
    int connection = -1;
    Clock::time_point last_report{};
    Slic3r::LanMonitorSnapshot state;
    json report;
};

void run(const json& settings)
{
    const auto module = std::filesystem::absolute(std::filesystem::u8path(settings.at("module").get<std::string>()));
    const auto directory = std::filesystem::absolute(std::filesystem::u8path(settings.at("directory").get<std::string>()));
    const auto certificate = settings.at("certificate").get<std::string>();
    const auto country = settings.value("country", std::string("US"));
    const int duration = std::clamp(settings.value("seconds", 60), 0, 3600);
    const int second_delay = settings.value("second_delay", 20);
    std::filesystem::create_directories(directory);
    std::filesystem::current_path(directory);
    std::vector<Target> targets;
    for (const auto& item : settings.at("printers"))
        targets.push_back({item.at("id"), item.at("host"), item.at("code")});
    if (targets.empty() || targets.size() > 2)
        throw std::runtime_error("One or two printers are required");

    LanMonitor::SdkLibrary library(module);
    auto create = library.get<void* (*)(std::string)>("bambu_network_create_agent");
    auto version = library.get<std::string (*)()>("bambu_network_get_version");
    if (version().rfind("02.", 0) != 0) {
        emit({{"kind", "error"}, {"reason", "unsupported_sdk"}});
        LanMonitor::finish(2);
    }
    void* agent = create(directory.u8string());
    if (!agent) throw std::runtime_error("SDK agent allocation failed");
    auto set_config = library.get<int (*)(void*, std::string)>("bambu_network_set_config_dir");
    auto init_log = library.get<int (*)(void*)>("bambu_network_init_log");
    auto set_cert = library.get<int (*)(void*, std::string, std::string)>("bambu_network_set_cert_file");
    auto set_country = library.get<int (*)(void*, std::string)>("bambu_network_set_country_code");
    auto track = library.get<int (*)(void*, bool)>("bambu_network_track_enable");
    auto start = library.get<int (*)(void*)>("bambu_network_start");
    auto connect = library.get<int (*)(void*, std::string, std::string, std::string, std::string, bool)>("bambu_network_connect_printer");
    auto disconnect = library.get<int (*)(void*)>("bambu_network_disconnect_printer");
    auto send = library.get<SendFn>("bambu_network_send_message_to_printer");

    auto stop = std::make_shared<std::atomic<bool>>(false);
    // An application worker runs until its owner closes stdin or sends a stop command.
    if (duration == 0) {
        std::thread([stop] {
            std::string command;
            while (std::getline(std::cin, command)) {
                const auto value = json::parse(command, nullptr, false);
                if (value.is_object() && value.contains("command") && value["command"].is_string() && value["command"] == "stop") break;
            }
            stop->store(true);
        }).detach();
    }
    std::mutex queue_mutex;
    std::deque<std::function<void()>> pending;
    auto queue = [&](std::function<void()> fn) {
        std::lock_guard<std::mutex> lock(queue_mutex);
        pending.push_back(std::move(fn));
    };
    auto request_status = [&](const std::string& id) {
        const json request = {{"pushing", {{"sequence_id", "10001"}, {"command", "pushall"}, {"version", 1}, {"push_target", 1}}}};
        send(agent, id, request.dump(), 0, 0);
    };
    library.get<int (*)(void*, QueueFn)>("bambu_network_set_queue_on_main_fn")(agent, queue);
    library.get<int (*)(void*, std::function<std::string()>)>("bambu_network_set_get_country_code_fn")(agent, [country] { return country; });
    library.get<int (*)(void*, LocalConnectFn)>("bambu_network_set_on_local_connect_fn")(
        agent, [&](int state, std::string id, std::string reason) {
            queue([&, state, id = std::move(id), reason = std::move(reason)] {
                for (std::size_t i = 0; i < targets.size(); ++i) {
                    if (targets[i].id != id) continue;
                    targets[i].connection = state;
                    emit({{"kind", "connection"}, {"target", i}, {"state", state},
                          {"reason", reason == "5" ? "authentication" : reason == "-1" ? "connection" : "other"}});
                    if (state == 0) request_status(id);
                }
            });
        });
    library.get<int (*)(void*, ConnectedFn)>("bambu_network_set_on_printer_connected_fn")(
        agent, [&](std::string id) { queue([&, id = std::move(id)] { request_status(id); }); });
    library.get<int (*)(void*, MessageFn)>("bambu_network_set_on_local_message_fn")(
        agent, [&](std::string id, std::string payload) {
            queue([&, id = std::move(id), payload = std::move(payload)] {
                const auto message = json::parse(payload, nullptr, false);
                if (!message.is_object() || !message.contains("print") || !message["print"].is_object()) return;
                for (std::size_t i = 0; i < targets.size(); ++i) {
                    auto& target = targets[i];
                    if (target.id != id) continue;
                    const auto mode = message["print"].find("msg");
                    if (mode != message["print"].end() && *mode == 1) {
                        if (target.report.empty()) { request_status(id); continue; }
                        Slic3r::merge_json_report(message, target.report);
                    } else {
                        target.report = message;
                    }
                    target.reports++;
                    target.last_report = Clock::now();
                    // Decode supplied fields: omitted values persist in the snapshot, while a new
                    // job must not inherit estimates restored from the previous job's baseline.
                    target.state.update(message);
                }
            });
        });
    set_config(agent, directory.u8string());
    init_log(agent);
    set_cert(agent, certificate, "slicer_base64.cer");
    set_country(agent, country);
    track(agent, false);
    emit({{"kind", "start"}, {"sdk", version()}, {"start_result", start(agent)}});

    const auto began = Clock::now();
    std::size_t connected_count = 0;
    int last_sample = -1;
    while (true) {
        std::deque<std::function<void()>> work;
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            pending.swap(work);
        }
        for (auto& fn : work) fn();
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - began).count();
        if (stop->load() || (duration > 0 && elapsed >= duration)) break;
        if (connected_count < targets.size() && (connected_count == 0 || elapsed >= second_delay)) {
            auto& target = targets[connected_count];
            const int result = connect(agent, target.id, target.host, "bblp", target.code, true);
            emit({{"kind", "connect_request"}, {"target", connected_count}, {"result", result}});
            connected_count++;
        }
        if (elapsed != last_sample) {
            last_sample = static_cast<int>(elapsed);
            for (std::size_t i = 0; i < targets.size(); ++i) {
                const auto& target = targets[i];
                json sample = {{"kind", "sample"}, {"target", i}, {"elapsed", elapsed},
                               {"reports", target.reports}, {"connection", target.connection}, {"status", target.state.to_json()}};
                if (target.reports)
                    sample["age_seconds"] = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - target.last_report).count();
                emit(sample);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const int disconnected = disconnect(agent);
    emit({{"kind", "finished"}, {"disconnect_result", disconnected}});
    LanMonitor::finish(0);
}

} // namespace

int main()
{
    try {
        std::string input;
        if (!std::getline(std::cin, input)) throw std::runtime_error("Missing worker configuration");
        run(json::parse(input));
    } catch (const std::exception&) {
        // Configuration and SDK diagnostics may contain credentials or printer identifiers.
        emit({{"kind", "error"}, {"reason", "Worker configuration or SDK call failed"}});
        LanMonitor::finish(2);
    }
}
