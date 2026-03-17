#pragma once

#include "portability/platform/DesktopInMemoryCredentialStore.hpp"
#include "portability/platform/IPlatformServices.hpp"

namespace Slic3r::portability::platform::ios {

class IOSPlatformServices final : public IPlatformServices
{
public:
    IOSPlatformServices();

    std::string_view platform_name() const override;
    std::string      writable_app_data_path() const override;
    std::string      temporary_path() const override;

    void post_to_main_thread(std::function<void()> task) override;
    void post_background(std::function<void()> task) override;

    ICredentialStore&       credential_store() override;
    const ICredentialStore& credential_store() const override;

private:
    DesktopInMemoryCredentialStore m_credential_store;
};

} // namespace Slic3r::portability::platform::ios
