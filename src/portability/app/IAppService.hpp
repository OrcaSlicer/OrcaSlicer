#pragma once

#include <string>

namespace Slic3r::portability::app {

class IAppService
{
public:
    virtual ~IAppService() = default;

    virtual std::string executable_path() const = 0;
};

} // namespace Slic3r::portability::app
