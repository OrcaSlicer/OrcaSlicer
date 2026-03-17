#pragma once

#include "portability/platform/ICredentialStore.hpp"

namespace Slic3r::portability::platform::ios {

class IOSKeychainCredentialStore final : public ICredentialStore
{
public:
    std::optional<std::string> read(const std::string& service, const std::string& account) const override;
    bool                       write(const std::string& service, const std::string& account, const std::string& secret) override;
    bool                       remove(const std::string& service, const std::string& account) override;
};

} // namespace Slic3r::portability::platform::ios
