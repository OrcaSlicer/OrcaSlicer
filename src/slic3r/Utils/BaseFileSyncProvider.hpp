#ifndef __BASE_FILE_SYNC_PROVIDER_HPP__
#define __BASE_FILE_SYNC_PROVIDER_HPP__

#include "IPresetSyncProvider.hpp"
#include "IBundleProvider.hpp"
#include "SyncBackend.hpp"
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Slic3r {

/**
 * BaseFileSyncProvider - common implementation of IPresetSyncProvider for
 * file-storage backends (WebDAV, Git, S3, Nextcloud, ...).
 *
 * Subclasses construct a SyncBackend (the transport) and hand it to the
 * base; this class then maps the per-preset IPresetSyncProvider contract
 * onto file-level upload/download/list/delete operations.
 *
 * Remote layout per subclass remote_prefix():
 *   <prefix>presets/<type>/<sanitized_name>.json
 *
 * State JSON sits in data_dir as "<provider_id>_sync_state.json" and is
 * keyed by remote_id ("<type>/<name>") -> {etag, updated_time, base_hash}.
 */
class BaseFileSyncProvider : public IPresetSyncProvider, public IBundleProvider
{
public:
    explicit BaseFileSyncProvider(std::unique_ptr<SyncBackend> backend);
    ~BaseFileSyncProvider() override;

    // IPresetSyncProvider --------------------------------------------------
    std::string  provider_id()   const override = 0;
    std::string  display_name()  const override;
    std::string  fingerprint()   const override;
    bool         is_configured() const override;
    int          connect(std::string& error_out)        override;
    bool         is_connected()                         override;

    PresetSyncResult push_preset(const std::string& preset_type,
                                 const std::string& preset_name,
                                 const std::string& json_content,
                                 const std::string& remote_id,
                                 const std::string& expected_etag) override;
    PresetSyncResult pull_preset(const std::string& preset_type,
                                 const std::string& remote_id,
                                 std::string&       out_json) override;
    int delete_preset(const std::string& preset_type,
                      const std::string& remote_id) override;
    int list_presets(const PresetListCallback& cb) override;

    std::vector<PresetSyncConflict> take_pending_conflicts() override;
    int apply_conflict_resolution(const PresetSyncConflict&           conflict,
                                  const PresetSyncConflictResolution& resolution) override;

    void load_state() override;
    void save_state() override;

    // IBundleProvider ------------------------------------------------------
    int list_subscribed_bundles(
        std::vector<std::pair<std::string, std::string>>* out_id_version,
        std::vector<std::string>&                         out_notfound,
        std::vector<std::string>&                         out_unauthorized) override;
    int fetch_bundle(const std::string& bundle_id,
                     const std::string& version,
                     std::map<std::string, std::map<std::string, std::string>>* out_presets,
                     BundleMetadata*    out_metadata) override;
    int publish_local_bundle(
        const BundleMetadata&                                            metadata,
        const std::map<std::string, std::map<std::string, std::string>>& presets,
        std::string&                                                     out_published_version) override;
    int unsubscribe_bundle(const std::string& bundle_id) override;

protected:
    // Joins "<prefix>presets/<type>/<name>.json".
    std::string remote_path_for(const std::string& preset_type,
                                const std::string& preset_name) const;
    // Inverse of remote_path_for: parses "presets/<type>/<file>.json" out of
    // the listed path. Returns false on malformed input.
    bool        parse_remote_path(const std::string& remote_path,
                                  std::string&       out_type,
                                  std::string&       out_name) const;

    // <data_dir>/<provider_id>_sync_state.json
    std::string state_file_path() const;

    // Cached per-remote-id metadata.
    struct CachedEntry {
        std::string etag;
        long long   updated_time{0};
        std::string base_hash; // sha256 of the last successfully synced content
    };

    std::unique_ptr<SyncBackend>       m_backend;
    mutable std::mutex                 m_state_mutex;
    std::string                        m_state_fingerprint;
    std::map<std::string, CachedEntry> m_state;             // key = remote_id
    // Bundles the user has explicitly hidden on this machine. file backends
    // treat every bundle present in <prefix>bundles/ as available to all
    // configured clients, so unsubscribe is local-only (the bundle stays on
    // the server for other users).
    std::vector<std::string>           m_hidden_bundles;

    mutable std::mutex                 m_conflicts_mutex;
    std::vector<PresetSyncConflict>    m_pending_conflicts;

private:
    static std::string preset_types_iter[3];
};

} // namespace Slic3r

#endif // __BASE_FILE_SYNC_PROVIDER_HPP__
