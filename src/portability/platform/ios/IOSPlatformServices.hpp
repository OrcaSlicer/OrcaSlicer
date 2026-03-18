#pragma once

#include "IOSKeychainCredentialStore.hpp"
#include "portability/platform/IPlatformServices.hpp"

#include <memory>

namespace Slic3r::portability::platform::ios {

class IOSPathProvider
{
public:
    virtual ~IOSPathProvider() = default;

    virtual std::string writable_app_data_path() const = 0;
    virtual std::string temporary_path() const         = 0;
};

class IOSDispatchQueue
{
public:
    virtual ~IOSDispatchQueue() = default;

    virtual void post_to_main_thread(std::function<void()> task) = 0;
    virtual void post_background(std::function<void()> task)     = 0;
};

std::shared_ptr<IOSPathProvider>  make_ios_path_provider();
std::shared_ptr<IOSDispatchQueue> make_ios_dispatch_queue();

class IOSPlatformServices final : public IPlatformServices
{
public:
    IOSPlatformServices();
    IOSPlatformServices(std::shared_ptr<IOSPathProvider>  path_provider,
                        std::shared_ptr<IOSDispatchQueue> dispatch_queue,
                        std::shared_ptr<ICredentialStore> credential_store);

    std::string_view platform_name() const override;
    std::string      writable_app_data_path() const override;
    std::string      temporary_path() const override;

    void post_to_main_thread(std::function<void()> task) override;
    void post_background(std::function<void()> task) override;

    ICredentialStore&       credential_store() override;
    const ICredentialStore& credential_store() const override;

private:
    std::shared_ptr<IOSPathProvider>  m_path_provider;
    std::shared_ptr<IOSDispatchQueue> m_dispatch_queue;
    std::shared_ptr<ICredentialStore> m_credential_store;
};

} // namespace Slic3r::portability::platform::ios
