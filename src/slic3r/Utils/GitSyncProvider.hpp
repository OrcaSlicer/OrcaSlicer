#ifndef __GIT_SYNC_PROVIDER_HPP__
#define __GIT_SYNC_PROVIDER_HPP__

#include "BaseFileSyncProvider.hpp"
#include "GitSync.hpp"

namespace Slic3r {

/**
 * GitSyncProvider - IPresetSyncProvider backed by a Git remote.
 *
 * Layout in the repository:
 *   presets/<type>/<name>.json
 *
 * Uses the existing libgit2-backed GitSync transport for clone / pull /
 * commit / push. ETags map onto blob SHAs internally; see GitSync.
 */
class GitSyncProvider : public BaseFileSyncProvider
{
public:
    explicit GitSyncProvider(const GitSyncConfig& config)
        : BaseFileSyncProvider(std::make_unique<GitSync>(config))
    {}

    std::string provider_id() const override { return "git"; }
};

} // namespace Slic3r

#endif // __GIT_SYNC_PROVIDER_HPP__
