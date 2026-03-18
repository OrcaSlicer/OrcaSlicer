#include "IOSKeychainCredentialStore.hpp"

#import <Foundation/Foundation.h>
#import <Security/Security.h>

namespace Slic3r::portability::platform::ios {

namespace {

NSMutableDictionary* base_query(const std::string& service, const std::string& account)
{
    NSMutableDictionary* query = [NSMutableDictionary dictionary];
    query[(__bridge id) kSecClass]       = (__bridge id) kSecClassGenericPassword;
    query[(__bridge id) kSecAttrService] = [NSString stringWithUTF8String:service.c_str()];
    query[(__bridge id) kSecAttrAccount] = [NSString stringWithUTF8String:account.c_str()];
    return query;
}

IOSKeychainStatus keychain_status_from_os_status(const OSStatus status)
{
    if (status == errSecSuccess)
        return IOSKeychainStatus::success;
    if (status == errSecItemNotFound)
        return IOSKeychainStatus::item_not_found;
    return IOSKeychainStatus::error;
}

class IOSSecurityFrameworkCredentialBackend final : public IOSKeychainCredentialBackend
{
public:
    IOSKeychainReadResult read(const std::string& service, const std::string& account) const override
    {
        NSMutableDictionary* query = base_query(service, account);
        query[(__bridge id) kSecReturnData]  = @YES;
        query[(__bridge id) kSecMatchLimit]  = (__bridge id) kSecMatchLimitOne;

        CFTypeRef result = nullptr;
        const OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef) query, &result);
        if (status == errSecItemNotFound)
            return {IOSKeychainStatus::item_not_found, std::nullopt};
        if (status != errSecSuccess)
            return {IOSKeychainStatus::error, std::nullopt};

        NSData* data = (__bridge_transfer NSData*) result;
        if (data == nil)
            return {IOSKeychainStatus::error, std::nullopt};

        return {IOSKeychainStatus::success, std::string(static_cast<const char*>(data.bytes), data.length)};
    }

    IOSKeychainStatus update(const std::string& service, const std::string& account, const std::string& secret) override
    {
        NSMutableDictionary* query = base_query(service, account);
        NSData* secret_data = [NSData dataWithBytes:secret.data() length:secret.size()];
        NSDictionary* attributes_to_update = @{(__bridge id) kSecValueData : secret_data};
        const OSStatus status = SecItemUpdate((__bridge CFDictionaryRef) query, (__bridge CFDictionaryRef) attributes_to_update);
        return keychain_status_from_os_status(status);
    }

    IOSKeychainStatus add(const std::string& service, const std::string& account, const std::string& secret) override
    {
        NSMutableDictionary* query = base_query(service, account);
        query[(__bridge id) kSecValueData] = [NSData dataWithBytes:secret.data() length:secret.size()];
        const OSStatus status = SecItemAdd((__bridge CFDictionaryRef) query, nullptr);
        return keychain_status_from_os_status(status);
    }

    IOSKeychainStatus remove(const std::string& service, const std::string& account) override
    {
        NSMutableDictionary* query = base_query(service, account);
        const OSStatus status = SecItemDelete((__bridge CFDictionaryRef) query);
        return keychain_status_from_os_status(status);
    }
};

} // namespace

std::shared_ptr<IOSKeychainCredentialBackend> make_ios_keychain_credential_backend()
{
    return std::make_shared<IOSSecurityFrameworkCredentialBackend>();
}

IOSKeychainCredentialStore::IOSKeychainCredentialStore()
    : IOSKeychainCredentialStore(make_ios_keychain_credential_backend())
{
}

IOSKeychainCredentialStore::IOSKeychainCredentialStore(std::shared_ptr<IOSKeychainCredentialBackend> backend)
    : m_backend(backend ? std::move(backend) : make_ios_keychain_credential_backend())
{
}

std::optional<std::string> IOSKeychainCredentialStore::read(const std::string& service, const std::string& account) const
{
    const IOSKeychainReadResult result = m_backend->read(service, account);
    if (result.status != IOSKeychainStatus::success)
        return std::nullopt;

    return result.secret;
}

bool IOSKeychainCredentialStore::write(const std::string& service, const std::string& account, const std::string& secret)
{
    const IOSKeychainStatus update_status = m_backend->update(service, account, secret);
    if (update_status == IOSKeychainStatus::success)
        return true;
    if (update_status != IOSKeychainStatus::item_not_found)
        return false;

    return m_backend->add(service, account, secret) == IOSKeychainStatus::success;
}

bool IOSKeychainCredentialStore::remove(const std::string& service, const std::string& account)
{
    const IOSKeychainStatus status = m_backend->remove(service, account);
    return status == IOSKeychainStatus::success || status == IOSKeychainStatus::item_not_found;
}

} // namespace Slic3r::portability::platform::ios
