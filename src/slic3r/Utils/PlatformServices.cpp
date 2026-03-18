#include "PlatformServices.hpp"

#include "DesktopAppServiceAdapter.hpp"

#include <string>

namespace Slic3r { namespace GUI {

DesktopPlatformServices::DesktopPlatformServices() : m_app_service(std::make_unique<DesktopAppServiceAdapter>()) {}

DesktopPlatformServices::~DesktopPlatformServices() = default;

wxString DesktopPlatformServices::executable_path() const
{
    const std::string path = m_app_service->executable_path();
    return wxString::FromUTF8(path.c_str());
}

}} // namespace Slic3r::GUI
