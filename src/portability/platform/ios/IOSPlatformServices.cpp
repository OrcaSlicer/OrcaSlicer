#include "IOSPlatformServices.hpp"

#include <filesystem>
#include <future>

namespace Slic3r::portability::platform::ios {

IOSPlatformServices::IOSPlatformServices() = default;

std::string_view IOSPlatformServices::platform_name() const
{
    return "ios";
}

std::string IOSPlatformServices::writable_app_data_path() const
{
    // Placeholder path while Objective-C++ bridge to NSFileManager is still pending.
    return (std::filesystem::temp_directory_path() / "orcaslicer-ios-appdata").string();
}

std::string IOSPlatformServices::temporary_path() const
{
    // Placeholder path while Objective-C++ bridge to NSTemporaryDirectory is still pending.
    return std::filesystem::temp_directory_path().string();
}

void IOSPlatformServices::post_to_main_thread(std::function<void()> task)
{
    // Placeholder behavior until dispatch_async to main queue is integrated.
    if (task)
        task();
}

void IOSPlatformServices::post_background(std::function<void()> task)
{
    if (!task)
        return;

    std::async(std::launch::async, [task = std::move(task)] { task(); });
}

ICredentialStore& IOSPlatformServices::credential_store()
{
    return m_credential_store;
}

const ICredentialStore& IOSPlatformServices::credential_store() const
{
    return m_credential_store;
}

} // namespace Slic3r::portability::platform::ios
