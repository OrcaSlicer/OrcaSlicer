#include "DesktopAppServiceAdapter.hpp"

#include <wx/stdpaths.h>

namespace Slic3r { namespace GUI {

std::string DesktopAppServiceAdapter::executable_path() const { return wxStandardPaths::Get().GetExecutablePath().ToStdString(); }

}} // namespace Slic3r::GUI
