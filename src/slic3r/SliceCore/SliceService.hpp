#pragma once

#include <string>

#include "SliceTypes.hpp"

namespace Slic3r {
namespace SliceCore {

class SliceService {
public:
    explicit SliceService(std::string resources_dir);

    SliceResult run(const SliceRequest &req);

private:
    std::string m_resources_dir;
};

} // namespace SliceCore
} // namespace Slic3r
