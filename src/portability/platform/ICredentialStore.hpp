#pragma once

#include <optional>
#include <string>

namespace Slic3r::portability::platform {

class ICredentialStore
{
public:
    virtual ~ICredentialStore() = default;

    virtual std::optional<std::string> read(const std::string& service, const std::string& account) const                       = 0;
    virtual bool                       write(const std::string& service, const std::string& account, const std::string& secret) = 0;
    virtual bool                       remove(const std::string& service, const std::string& account)                           = 0;
};

} // namespace Slic3r::portability::platform
