#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "ICredentialStore.hpp"

namespace Slic3r::portability::platform {

class DesktopInMemoryCredentialStore final : public ICredentialStore
{
public:
    std::optional<std::string> read(const std::string& service, const std::string& account) const override;
    bool                       write(const std::string& service, const std::string& account, const std::string& secret) override;
    bool                       remove(const std::string& service, const std::string& account) override;

private:
    using CredentialKey = std::pair<std::string, std::string>;

    mutable std::mutex                   m_mutex;
    std::map<CredentialKey, std::string> m_credentials;
};

} // namespace Slic3r::portability::platform
