#include "IOSPlatformServices.hpp"

#include <filesystem>
#include <thread>

namespace OrcaSlicer::Portability {

std::string IOSPlatformServices::app_data_path()
{
    // Placeholder path until NSFileManager integration is added.
    return (std::filesystem::temp_directory_path() / "orcaslicer-ios-appdata").string();
}

std::string IOSPlatformServices::temp_path()
{
    // Placeholder path until NSTemporaryDirectory integration is added.
    return std::filesystem::temp_directory_path().string();
}

void IOSPlatformServices::dispatch_to_main_thread(std::function<void()> task)
{
    // Placeholder: execute immediately until a main-queue dispatch implementation is introduced.
    if (task) {
        task();
    }
}

void IOSPlatformServices::dispatch_to_background(std::function<void()> task)
{
    // Placeholder: detach a background thread until Grand Central Dispatch is wired in.
    if (task) {
        std::thread(std::move(task)).detach();
    }
}

} // namespace OrcaSlicer::Portability
