#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#endif
#include "slic3r/Utils/LanMonitorSdk.hpp"
#include "slic3r/Utils/BambuCameraSource.hpp"
#define BAMBU_DYNAMIC
#include "slic3r/GUI/Printer/BambuTunnel.h"
#ifdef LAN_CAMERA_DECODE
#include "slic3r/GUI/AVVideoDecoder.hpp"
#include <wx/gdicmn.h>
#include <wx/image.h>
#include <wx/init.h>
#include <memory>
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

namespace {
struct Camera
{
    Bambu_Tunnel tunnel = nullptr;
    int error = 0;
    unsigned frames = 0;
    std::chrono::steady_clock::time_point last_frame{};
    std::int64_t longest_gap_ms = 0;
#ifdef LAN_CAMERA_DECODE
    std::unique_ptr<AVVideoDecoder> decoder;
    unsigned decoded_frames = 0;
    unsigned decode_failures = 0;
    unsigned near_black_frames = 0;
#endif
    std::mutex mutex;
    std::vector<int> diagnostic_codes;
};

void camera_log(void* context, int level, const tchar* text)
{
    if (level != 1 || !text) return;
    try {
        const std::basic_string<tchar> message(text);
        if (message.empty() || message.back() != ']') return;
        const auto begin = message.find_last_of('[');
        if (begin == message.npos) return;
        const auto value = message.substr(begin + 1, message.size() - begin - 2);
        std::size_t end = 0;
        const int code = std::stoi(value, &end);
        if (code == 0 || end != value.size()) return;
        auto& camera = *static_cast<Camera*>(context);
        std::lock_guard<std::mutex> lock(camera.mutex);
        if (camera.diagnostic_codes.size() < 32) camera.diagnostic_codes.push_back(code);
    } catch (...) {}
}
} // namespace

int main()
{
    using json = nlohmann::json;
    using Clock = std::chrono::steady_clock;
    try {
#ifdef LAN_CAMERA_DECODE
        wxInitializer wx;
        if (!wx.IsOk()) return 2;
#endif
#ifdef _WIN32
        WSADATA sockets{};
        if (WSAStartup(MAKEWORD(2, 2), &sockets) != 0) return 2;
#endif
        std::string input;
        std::getline(std::cin, input);
        const auto config = json::parse(input);
        const int seconds = std::clamp(config.value("seconds", 30), 1, 180);
        LanMonitor::SdkLibrary library(std::filesystem::u8path(config.at("module").get<std::string>()));
        auto create = library.get<decltype(BambuLib::Bambu_Create)>("Bambu_Create");
        auto logger = library.get<decltype(BambuLib::Bambu_SetLogger)>("Bambu_SetLogger");
        auto open = library.get<decltype(BambuLib::Bambu_Open)>("Bambu_Open");
        auto start = library.get<decltype(BambuLib::Bambu_StartStream)>("Bambu_StartStream");
        auto read = library.get<decltype(BambuLib::Bambu_ReadSample)>("Bambu_ReadSample");
#ifdef LAN_CAMERA_DECODE
        auto info = library.get<decltype(BambuLib::Bambu_GetStreamInfo)>("Bambu_GetStreamInfo");
#endif
        auto close = library.get<decltype(BambuLib::Bambu_Close)>("Bambu_Close");
        auto destroy = library.get<decltype(BambuLib::Bambu_Destroy)>("Bambu_Destroy");
        std::vector<Camera> cameras(config.at("printers").size());
        const auto began = Clock::now();
        auto connect_camera = [&](std::size_t i) {
            Slic3r::BambuCameraSource source;
            const auto& printer = config["printers"][i];
            source.host = printer.at("host");
            source.password = printer.at("code");
            source.device = printer.at("id");
            cameras[i].error = create(&cameras[i].tunnel, source.url().c_str());
            if (cameras[i].error == 0) logger(cameras[i].tunnel, camera_log, &cameras[i]);
            if (cameras[i].error == 0) cameras[i].error = open(cameras[i].tunnel);
            if (cameras[i].error == 0) cameras[i].error = Bambu_would_block;
        };
        auto read_camera = [&](std::size_t i) {
            auto& camera = cameras[i];
            if (camera.error == Bambu_would_block) {
                camera.error = start(camera.tunnel, true);
#ifdef LAN_CAMERA_DECODE
                if (camera.error == 0) {
                    Bambu_StreamInfo stream{};
                    camera.error = info(camera.tunnel, 0, &stream);
                    if (camera.error == 0) {
                        camera.decoder = std::make_unique<AVVideoDecoder>();
                        camera.error = camera.decoder->open(stream);
                    }
                }
#endif
            }
            if (camera.error != 0) return;
            Bambu_Sample sample{};
            const auto result = read(camera.tunnel, &sample);
            if (result == 0) {
                const auto now = Clock::now();
                if (camera.frames)
                    camera.longest_gap_ms = std::max(camera.longest_gap_ms,
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - camera.last_frame).count());
                camera.last_frame = now;
#ifdef LAN_CAMERA_DECODE
                wxImage frame;
                if (camera.decoder->decode(sample) < 0 || !camera.decoder->toWxImage(frame, wxSize(64, 36))) {
                    ++camera.decode_failures;
                } else {
                    ++camera.decoded_frames;
                    const std::size_t bytes = static_cast<std::size_t>(frame.GetWidth()) * frame.GetHeight() * 3;
                    if (*std::max_element(frame.GetData(), frame.GetData() + bytes) < 4) ++camera.near_black_frames;
                }
#endif
                if (++camera.frames == 1) {
                    const auto file = std::filesystem::u8path(config.at("output").get<std::string>()) / ("camera-" + std::to_string(i) + ".jpg");
                    std::ofstream image(file, std::ios::binary);
                    image.write(reinterpret_cast<const char*>(sample.buffer), sample.size);
                }
            } else if (result != Bambu_would_block) camera.error = result;
        };
        std::vector<std::thread> threads;
        if (config.value("parallel", true)) {
            for (std::size_t i = 0; i < cameras.size(); ++i)
                threads.emplace_back([&, i] {
                    try {
                        connect_camera(i);
                        while (Clock::now() - began < std::chrono::seconds(seconds)) {
                            read_camera(i);
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        }
                    } catch (...) { cameras[i].error = -1; }
                });
            for (auto& thread : threads) thread.join();
        } else {
            for (std::size_t i = 0; i < cameras.size(); ++i) connect_camera(i);
            while (Clock::now() - began < std::chrono::seconds(seconds)) {
                for (std::size_t i = 0; i < cameras.size(); ++i) read_camera(i);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        json result = json::array();
        bool success = true;
        for (auto& camera : cameras) {
            success = success && camera.frames > 1;
            if (camera.tunnel) { close(camera.tunnel); destroy(camera.tunnel); }
            std::lock_guard<std::mutex> lock(camera.mutex);
            json record = {{"frames", camera.frames}, {"error", camera.error}, {"diagnostic_codes", camera.diagnostic_codes},
                           {"longest_gap_ms", camera.longest_gap_ms}};
#ifdef LAN_CAMERA_DECODE
            record["decoded_frames"] = camera.decoded_frames;
            record["decode_failures"] = camera.decode_failures;
            record["near_black_frames"] = camera.near_black_frames;
            success = success && camera.decode_failures == 0 && camera.decoded_frames == camera.frames;
#endif
            result.push_back(std::move(record));
        }
        std::cout << "ORCA_CAMERA_PROBE " << result.dump() << std::endl;
        LanMonitor::finish(success ? 0 : 1);
    } catch (const std::exception&) {
        std::cout << "ORCA_CAMERA_PROBE configuration or SDK failure" << std::endl;
        LanMonitor::finish(2);
    }
}
