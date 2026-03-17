#pragma once

#include "portability/platform/IPlatformServices.hpp"

namespace OrcaSlicer::Portability {
#ifndef orcaslicer_DesktopPlatformServices_hpp_
#define orcaslicer_DesktopPlatformServices_hpp_

#include "IPlatformServices.hpp"

#include <unordered_map>

namespace Slic3r::Portability::Platform {

class DesktopPlatformServices final : public IPlatformServices
{
public:
    std::string_view platform_name() const override;
};

} // namespace OrcaSlicer::Portability
    std::string writable_app_data_path() const override;
    std::string temporary_path() const override;

    void post_to_main_thread(std::function<void()> task) override;
    void post_background(std::function<void()> task) override;

    bool read_secure_value(const std::string &key, std::string &value) const override;
    bool write_secure_value(const std::string &key, const std::string &value) override;

private:
    std::unordered_map<std::string, std::string> m_fake_secure_store;
};

} // namespace Slic3r::Portability::Platform

#endif // orcaslicer_DesktopPlatformServices_hpp_
