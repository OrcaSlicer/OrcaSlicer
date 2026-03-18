#ifndef slic3r_GUI_DesktopAppServiceAdapter_hpp_
#define slic3r_GUI_DesktopAppServiceAdapter_hpp_

#include "portability/app/IAppService.hpp"

namespace Slic3r { namespace GUI {

class DesktopAppServiceAdapter final : public portability::app::IAppService
{
public:
    std::string executable_path() const override;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_DesktopAppServiceAdapter_hpp_
