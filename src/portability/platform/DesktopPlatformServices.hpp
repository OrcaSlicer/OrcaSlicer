#pragma once

#include "portability/platform/IPlatformServices.hpp"

namespace OrcaSlicer::Portability {

class DesktopPlatformServices final : public IPlatformServices
{
public:
    std::string_view platform_name() const override;
};

} // namespace OrcaSlicer::Portability
