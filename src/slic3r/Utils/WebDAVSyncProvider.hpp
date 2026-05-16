#ifndef __WEBDAV_SYNC_PROVIDER_HPP__
#define __WEBDAV_SYNC_PROVIDER_HPP__

#include "BaseFileSyncProvider.hpp"
#include "WebDAVSync.hpp"

namespace Slic3r {

/**
 * WebDAVSyncProvider - IPresetSyncProvider backed by a WebDAV server.
 * Layout on the server:
 *   <prefix>presets/<type>/<name>.json
 * where <prefix> is the WebDAV backend's remote_prefix() (defaults to
 * "orcaslicer-sync/").
 */
class WebDAVSyncProvider : public BaseFileSyncProvider
{
public:
    explicit WebDAVSyncProvider(const WebDAVConfig& config)
        : BaseFileSyncProvider(std::make_unique<WebDAVSync>(config))
    {}

    std::string provider_id() const override { return "webdav"; }
};

} // namespace Slic3r

#endif // __WEBDAV_SYNC_PROVIDER_HPP__
