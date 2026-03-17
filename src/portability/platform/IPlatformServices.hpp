#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "ICredentialStore.hpp"

namespace Slic3r::portability::platform {

class IPlatformServices
{
public:
    virtual ~IPlatformServices() = default;

    virtual std::string_view platform_name() const = 0;
    virtual std::string      writable_app_data_path() const = 0;
    virtual std::string      temporary_path() const = 0;

    virtual void post_to_main_thread(std::function<void()> task) = 0;
    virtual void post_background(std::function<void()> task) = 0;

    virtual ICredentialStore&       credential_store() = 0;
    virtual const ICredentialStore& credential_store() const = 0;
};

} // namespace Slic3r::portability::platform
