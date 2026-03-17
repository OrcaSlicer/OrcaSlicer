#pragma once

#include <functional>
#include <string>

namespace OrcaSlicer::Portability {

class IOSPlatformServices
{
public:
    static std::string app_data_path();
    static std::string temp_path();

    static void dispatch_to_main_thread(std::function<void()> task);
    static void dispatch_to_background(std::function<void()> task);
};

} // namespace OrcaSlicer::Portability
