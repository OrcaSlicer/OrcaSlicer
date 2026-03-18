#pragma once

#include "portability/platform/ICredentialStore.hpp"

#include <memory>

namespace Slic3r::portability::platform::ios {

enum class IOSKeychainStatus {
    success,
    item_not_found,
    error,
};

struct IOSKeychainReadResult
{
    IOSKeychainStatus          status{IOSKeychainStatus::error};
    std::optional<std::string> secret;
};

class IOSKeychainCredentialBackend
{
public:
    virtual ~IOSKeychainCredentialBackend() = default;

    virtual IOSKeychainReadResult read(const std::string& service, const std::string& account) const                        = 0;
    virtual IOSKeychainStatus     update(const std::string& service, const std::string& account, const std::string& secret) = 0;
    virtual IOSKeychainStatus     add(const std::string& service, const std::string& account, const std::string& secret)    = 0;
    virtual IOSKeychainStatus     remove(const std::string& service, const std::string& account)                            = 0;
};

std::shared_ptr<IOSKeychainCredentialBackend> make_ios_keychain_credential_backend();

class IOSKeychainCredentialStore final : public ICredentialStore
{
public:
    IOSKeychainCredentialStore();
    explicit IOSKeychainCredentialStore(std::shared_ptr<IOSKeychainCredentialBackend> backend);

    std::optional<std::string> read(const std::string& service, const std::string& account) const override;
    bool                       write(const std::string& service, const std::string& account, const std::string& secret) override;
    bool                       remove(const std::string& service, const std::string& account) override;

private:
    std::shared_ptr<IOSKeychainCredentialBackend> m_backend;
};

} // namespace Slic3r::portability::platform::ios
