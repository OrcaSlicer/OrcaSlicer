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

} // namespace

std::optional<std::string> IOSKeychainCredentialStore::read(const std::string& service, const std::string& account) const
{
    NSMutableDictionary* query = base_query(service, account);
    query[(__bridge id) kSecReturnData]  = @YES;
    query[(__bridge id) kSecMatchLimit]  = (__bridge id) kSecMatchLimitOne;

    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef) query, &result);
    if (status == errSecItemNotFound)
        return std::nullopt;
    if (status != errSecSuccess)
        return std::nullopt;

    NSData* data = (__bridge_transfer NSData*) result;
    if (data == nil)
        return std::nullopt;

    return std::string(static_cast<const char*>(data.bytes), data.length);
}

bool IOSKeychainCredentialStore::write(const std::string& service, const std::string& account, const std::string& secret)
{
    NSMutableDictionary* query      = base_query(service, account);
    NSData*              secretData = [NSData dataWithBytes:secret.data() length:secret.size()];

    NSDictionary* attributesToUpdate = @{(__bridge id) kSecValueData : secretData};
    const OSStatus  updateStatus      = SecItemUpdate((__bridge CFDictionaryRef) query, (__bridge CFDictionaryRef) attributesToUpdate);
    if (updateStatus == errSecSuccess)
        return true;

    if (updateStatus != errSecItemNotFound)
        return false;

    query[(__bridge id) kSecValueData] = secretData;
    const OSStatus addStatus = SecItemAdd((__bridge CFDictionaryRef) query, nullptr);
    return addStatus == errSecSuccess;
}

bool IOSKeychainCredentialStore::remove(const std::string& service, const std::string& account)
{
    NSMutableDictionary* query = base_query(service, account);
    const OSStatus       status = SecItemDelete((__bridge CFDictionaryRef) query);
    return status == errSecSuccess || status == errSecItemNotFound;
}

} // namespace Slic3r::portability::platform::ios
