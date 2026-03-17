#include "PlatformServices.hpp"

#include <wx/stdpaths.h>

namespace Slic3r {
namespace GUI {

wxString DesktopPlatformServices::executable_path() const
{
    return wxStandardPaths::Get().GetExecutablePath();
}

} // namespace GUI
} // namespace Slic3r
