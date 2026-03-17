#include "portability/platform/DesktopPlatformServices.hpp"

namespace OrcaSlicer::Portability {

std::string_view DesktopPlatformServices::platform_name() const
{
    return "desktop";
}

} // namespace OrcaSlicer::Portability
#include "DesktopPlatformServices.hpp"

#include <cstdlib>
#include <filesystem>
#include <future>

namespace Slic3r::Portability::Platform {

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
    std::async(std::launch::async, [task = std::move(task)] {
        if (task)
            task();
    });
}

bool DesktopPlatformServices::read_secure_value(const std::string &key, std::string &value) const
{
    const auto it = m_fake_secure_store.find(key);
    if (it == m_fake_secure_store.end())
        return false;

    value = it->second;
    return true;
}

bool DesktopPlatformServices::write_secure_value(const std::string &key, const std::string &value)
{
    m_fake_secure_store[key] = value;
    return true;
}

} // namespace Slic3r::Portability::Platform
