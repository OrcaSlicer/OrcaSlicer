#include "IOSPlatformServices.hpp"

#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>

namespace Slic3r::portability::platform::ios {

IOSPlatformServices::IOSPlatformServices() = default;

std::string_view IOSPlatformServices::platform_name() const
{
    return "ios";
}

std::string IOSPlatformServices::writable_app_data_path() const
{
    NSError* error = nil;
    NSURL*   appSupportURL = [[NSFileManager defaultManager] URLForDirectory:NSApplicationSupportDirectory
                                                                     inDomain:NSUserDomainMask
                                                            appropriateForURL:nil
                                                                       create:YES
                                                                        error:&error];
    if (appSupportURL == nil)
        return {};

    return std::string(appSupportURL.path.UTF8String);
}

std::string IOSPlatformServices::temporary_path() const
{
    NSString* temporaryDirectory = NSTemporaryDirectory();
    if (temporaryDirectory == nil)
        return {};

    return std::string(temporaryDirectory.UTF8String);
}

void IOSPlatformServices::post_to_main_thread(std::function<void()> task)
{
    if (!task)
        return;

    dispatch_async(dispatch_get_main_queue(), [task = std::move(task)]() mutable { task(); });
}

void IOSPlatformServices::post_background(std::function<void()> task)
{
    if (!task)
        return;

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), [task = std::move(task)]() mutable { task(); });
}

ICredentialStore& IOSPlatformServices::credential_store()
{
    return m_credential_store;
}

const ICredentialStore& IOSPlatformServices::credential_store() const
{
    return m_credential_store;
}

} // namespace Slic3r::portability::platform::ios
