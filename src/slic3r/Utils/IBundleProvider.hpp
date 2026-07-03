#ifndef __I_BUNDLE_PROVIDER_HPP__
#define __I_BUNDLE_PROVIDER_HPP__

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

struct BundleMetadata; // libslic3r/PresetBundle.hpp

/**
 * IBundleProvider - optional capability for sync providers that can host
 * shareable preset bundles (the `_local/<id>/` and `_subscribed/<id>/`
 * collections introduced by the Orca Cloud feature).
 *
 * Implementations:
 *   - OrcaCloudServiceAgent (REST against cloud.orcaslicer.com)
 *   - WebDAVSyncProvider    (stores under bundles/_local/<id>/ on the server)
 *   - GitSyncProvider       (stores under bundles/_local/<id>/ in the repo)
 *
 * A sync provider that does not support bundles simply does not inherit from
 * this interface; NetworkAgent / GUI_App check via dynamic_cast.
 */
class IBundleProvider
{
public:
    virtual ~IBundleProvider() = default;

    // Discovery: list (bundle_id, version) pairs the user is subscribed to.
    virtual int list_subscribed_bundles(
        std::vector<std::pair<std::string, std::string>>* out_id_version,
        std::vector<std::string>&                         out_notfound,
        std::vector<std::string>&                         out_unauthorized) = 0;

    // Fetch a specific bundle (presets by type -> map<preset_name, json>).
    // If `version` is empty, the latest version is returned.
    virtual int fetch_bundle(const std::string& bundle_id,
                             const std::string& version,
                             std::map<std::string, std::map<std::string, std::string>>* out_presets,
                             BundleMetadata*    out_metadata) = 0;

    // Publish a locally-authored bundle. Provider may version it server-side
    // (or in the repo, for Git). On success out_published_version is set.
    virtual int publish_local_bundle(
        const BundleMetadata&                                            metadata,
        const std::map<std::string, std::map<std::string, std::string>>& presets,
        std::string&                                                     out_published_version) = 0;

    // Drop the subscription. Local cache cleanup is the GUI's responsibility.
    virtual int unsubscribe_bundle(const std::string& bundle_id) = 0;
};

} // namespace Slic3r

#endif // __I_BUNDLE_PROVIDER_HPP__
