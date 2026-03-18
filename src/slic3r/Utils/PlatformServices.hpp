#ifndef slic3r_GUI_PlatformServices_hpp_
#define slic3r_GUI_PlatformServices_hpp_

#include <memory>
#include <wx/string.h>

namespace Slic3r::portability::app {
class IAppService;
} // namespace Slic3r::portability::app

namespace Slic3r { namespace GUI {

class IPlatformServices
{
public:
    virtual ~IPlatformServices()             = default;
    virtual wxString executable_path() const = 0;
};

class DesktopPlatformServices final : public IPlatformServices
{
public:
    DesktopPlatformServices();
    ~DesktopPlatformServices() override;

    wxString executable_path() const override;

private:
    std::unique_ptr<portability::app::IAppService> m_app_service;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_PlatformServices_hpp_
