#ifndef slic3r_GUI_PlatformServices_hpp_
#define slic3r_GUI_PlatformServices_hpp_

#include <wx/string.h>

namespace Slic3r {
namespace GUI {

class IPlatformServices
{
public:
    virtual ~IPlatformServices() = default;
    virtual wxString executable_path() const = 0;
};

class DesktopPlatformServices final : public IPlatformServices
{
public:
    wxString executable_path() const override;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_PlatformServices_hpp_
