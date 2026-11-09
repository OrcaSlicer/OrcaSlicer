#include "slic3r/Utils/LanMonitorSdk.hpp"

#include <cstddef>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

void probe(const std::filesystem::path& module, const std::filesystem::path& log_directory)
{
    const auto module_path = std::filesystem::absolute(module);
    const auto logs_path = std::filesystem::absolute(log_directory);
    std::filesystem::create_directories(logs_path);
    std::filesystem::current_path(logs_path);
    LanMonitor::SdkLibrary library(module_path);
    auto create = library.get<void* (*)(std::string)>("bambu_network_create_agent");
    auto version = library.get<std::string (*)()>("bambu_network_get_version");
    std::cout << "SDK version: " << version() << std::endl;

    std::vector<void*> agents;
    for (std::size_t index = 0; index != 3; ++index) {
        const auto directory = logs_path / std::to_string(index);
        std::filesystem::create_directories(directory);
        void* agent = create(directory.u8string());
        if (!agent) {
            std::cout << "Agent " << index << ": allocation failed" << std::endl;
            LanMonitor::finish(4);
        }
        for (void* previous : agents) {
            if (previous == agent) {
                std::cout << "Agent " << index << ": factory returned an existing handle" << std::endl;
                LanMonitor::finish(3);
            }
        }
        agents.push_back(agent);
        std::cout << "Agent " << index << ": distinct handle" << std::endl;
    }
    std::cout << "Allocation check passed; live-printer validation is still required" << std::endl;
    LanMonitor::finish(0);
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
    if (argc != 3) {
        std::cerr << "Usage: lan_monitor_probe <networking-library> <log-directory>" << std::endl;
        LanMonitor::finish(2);
    }
    try {
        probe(argv[1], argv[2]);
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        LanMonitor::finish(2);
    }
}
