#pragma once

#include <string_view>

namespace OrcaSlicer::Portability {

class IPlatformServices
{
public:
    virtual ~IPlatformServices() = default;

    virtual std::string_view platform_name() const = 0;
};

} // namespace OrcaSlicer::Portability
