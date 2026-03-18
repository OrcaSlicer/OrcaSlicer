#include "DesktopPlatformServices.hpp"

#include <cstdlib>
#include <filesystem>
#include <thread>

namespace Slic3r::portability::platform {

DesktopPlatformServices::DesktopPlatformServices() = default;

std::string_view DesktopPlatformServices::platform_name() const
{
    return "desktop";
}

std::string DesktopPlatformServices::writable_app_data_path() const
{
#ifdef _WIN32
    if (const char *value = std::getenv("APPDATA"); value != nullptr)
        return value;
#elif defined(__APPLE__)
    if (const char *value = std::getenv("HOME"); value != nullptr)
        return (std::filesystem::path(value) / "Library" / "Application Support").string();
#else
    if (const char *value = std::getenv("XDG_DATA_HOME"); value != nullptr)
        return value;

    if (const char *value = std::getenv("HOME"); value != nullptr)
        return (std::filesystem::path(value) / ".local" / "share").string();
#endif

    return std::filesystem::current_path().string();
}

std::string DesktopPlatformServices::temporary_path() const
{
    std::error_code ec;
    const std::filesystem::path path = std::filesystem::temp_directory_path(ec);
    if (ec)
        return std::filesystem::current_path().string();

    return path.string();
}

void DesktopPlatformServices::post_to_main_thread(std::function<void()> task)
{
    if (task)
        task();
}

void DesktopPlatformServices::post_background(std::function<void()> task)
{
    std::thread([task = std::move(task)] {
        if (task)
            task();
    }).detach();
}

ICredentialStore& DesktopPlatformServices::credential_store()
{
    return m_credential_store;
}

const ICredentialStore& DesktopPlatformServices::credential_store() const
{
    return m_credential_store;
}

} // namespace Slic3r::portability::platform
