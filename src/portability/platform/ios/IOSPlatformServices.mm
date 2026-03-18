#include "IOSPlatformServices.hpp"

#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>

namespace Slic3r::portability::platform::ios {

namespace {

class FoundationIOSPathProvider final : public IOSPathProvider
{
public:
    std::string writable_app_data_path() const override
    {
        NSError* error = nil;
        NSURL* app_support_url = [[NSFileManager defaultManager] URLForDirectory:NSApplicationSupportDirectory
                                                                         inDomain:NSUserDomainMask
                                                                appropriateForURL:nil
                                                                           create:YES
                                                                            error:&error];
        if (app_support_url == nil)
            return {};

        return std::string(app_support_url.path.UTF8String);
    }

    std::string temporary_path() const override
    {
        NSString* temporary_directory = NSTemporaryDirectory();
        if (temporary_directory == nil)
            return {};

        return std::string(temporary_directory.UTF8String);
    }
};

class IOSGCDDispatchQueue final : public IOSDispatchQueue
{
public:
    void post_to_main_thread(std::function<void()> task) override
    {
        if (!task)
            return;

        dispatch_async(dispatch_get_main_queue(), [task = std::move(task)]() mutable { task(); });
    }

    void post_background(std::function<void()> task) override
    {
        if (!task)
            return;

        dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), [task = std::move(task)]() mutable { task(); });
    }
};

} // namespace

std::shared_ptr<IOSPathProvider> make_ios_path_provider()
{
    return std::make_shared<FoundationIOSPathProvider>();
}

std::shared_ptr<IOSDispatchQueue> make_ios_dispatch_queue()
{
    return std::make_shared<IOSGCDDispatchQueue>();
}

IOSPlatformServices::IOSPlatformServices()
    : IOSPlatformServices(make_ios_path_provider(), make_ios_dispatch_queue(), std::make_shared<IOSKeychainCredentialStore>())
{
}

IOSPlatformServices::IOSPlatformServices(std::shared_ptr<IOSPathProvider> path_provider,
                                         std::shared_ptr<IOSDispatchQueue> dispatch_queue,
                                         std::shared_ptr<ICredentialStore> credential_store)
    : m_path_provider(path_provider ? std::move(path_provider) : make_ios_path_provider()),
      m_dispatch_queue(dispatch_queue ? std::move(dispatch_queue) : make_ios_dispatch_queue()),
      m_credential_store(credential_store ? std::move(credential_store) : std::make_shared<IOSKeychainCredentialStore>())
{
}

std::string_view IOSPlatformServices::platform_name() const
{
    return "ios";
}

std::string IOSPlatformServices::writable_app_data_path() const
{
    return m_path_provider->writable_app_data_path();
}

std::string IOSPlatformServices::temporary_path() const
{
    return m_path_provider->temporary_path();
}

void IOSPlatformServices::post_to_main_thread(std::function<void()> task)
{
    m_dispatch_queue->post_to_main_thread(std::move(task));
}

void IOSPlatformServices::post_background(std::function<void()> task)
{
    m_dispatch_queue->post_background(std::move(task));
}

ICredentialStore& IOSPlatformServices::credential_store()
{
    return *m_credential_store;
}

const ICredentialStore& IOSPlatformServices::credential_store() const
{
    return *m_credential_store;
}

} // namespace Slic3r::portability::platform::ios
