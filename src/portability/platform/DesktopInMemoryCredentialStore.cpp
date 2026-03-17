#include "DesktopInMemoryCredentialStore.hpp"

#include <utility>

namespace Slic3r::portability::platform {

std::optional<std::string> DesktopInMemoryCredentialStore::read(const std::string& service, const std::string& account) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto                  it = m_credentials.find({service, account});
    if (it == m_credentials.end())
        return std::nullopt;
    return it->second;
}

bool DesktopInMemoryCredentialStore::write(const std::string& service, const std::string& account, const std::string& secret)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_credentials[{service, account}] = secret;
    return true;
}

bool DesktopInMemoryCredentialStore::remove(const std::string& service, const std::string& account)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_credentials.erase({service, account}) > 0;
}

} // namespace Slic3r::portability::platform
