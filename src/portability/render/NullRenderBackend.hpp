#pragma once

#include "portability/render/IRenderBackend.hpp"

namespace OrcaSlicer::Portability {

class NullRenderBackend final : public IRenderBackend
{
public:
    std::string_view backend_name() const override { return "null"; }
};

} // namespace OrcaSlicer::Portability
