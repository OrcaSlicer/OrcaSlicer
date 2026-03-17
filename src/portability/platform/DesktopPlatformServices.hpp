#pragma once

#include "DesktopInMemoryCredentialStore.hpp"
#include "IPlatformServices.hpp"

namespace Slic3r::portability::platform {

class DesktopPlatformServices final : public IPlatformServices
{
public:
    DesktopPlatformServices();

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

} // namespace Slic3r::portability::platform
