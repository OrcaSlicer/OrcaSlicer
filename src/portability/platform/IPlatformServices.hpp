#pragma once

namespace Slic3r::portability::platform {

class ICredentialStore;

class IPlatformServices
{
public:
    virtual ~IPlatformServices() = default;

    virtual ICredentialStore&       credential_store()       = 0;
    virtual const ICredentialStore& credential_store() const = 0;
};

} // namespace Slic3r::portability::platform
