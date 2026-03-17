#pragma once

#include <string_view>

namespace OrcaSlicer::Portability {

class IRenderBackend
{
public:
    virtual ~IRenderBackend() = default;

    virtual std::string_view backend_name() const = 0;
};

} // namespace OrcaSlicer::Portability
