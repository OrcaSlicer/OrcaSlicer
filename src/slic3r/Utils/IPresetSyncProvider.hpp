#ifndef __I_PRESET_SYNC_PROVIDER_HPP__
#define __I_PRESET_SYNC_PROVIDER_HPP__

#include <functional>
#include <string>
#include <vector>

namespace Slic3r {

// Generic outcome of a single preset operation against a sync provider.
struct PresetSyncResult
{
    // HTTP-style status: 200 ok, 404 not found, 409 conflict, 412 etag mismatch,
    // 401 unauthorized, 500/other transport errors. Providers without HTTP map
    // their native error states into these codes.
    int         http_code{0};
    std::string error_message;
    std::string remote_id;       // server-issued or provider-synthesized
    std::string etag;            // for next OCC round
    long long   updated_time{0}; // epoch ms, as reported by the remote
};

// A detected concurrent modification surfaced to the GUI for merge resolution.
struct PresetSyncConflict
{
    std::string preset_type;     // "print" | "filament" | "printer"
    std::string preset_name;     // canonical preset name
    std::string local_json;
    std::string remote_json;
    std::string base_json;       // last-synced version (empty -> 2-way merge)
    std::string remote_id;       // for re-pushing after merge
};

// How the user / policy resolved a PresetSyncConflict.
enum class PresetSyncConflictChoice {
    KeepLocal,
    KeepRemote,
    Skip,
    Merge,                       // merged_json carries the result
};

struct PresetSyncConflictResolution
{
    PresetSyncConflictChoice choice{PresetSyncConflictChoice::Skip};
    std::string              merged_json; // populated only when choice == Merge
};

// Listing callback for IPresetSyncProvider::list_presets.
// Called once per remote preset entry. `etag` carries opaque provider data the
// orchestrator should hand back on subsequent push/pull calls.
using PresetListCallback = std::function<void(const std::string& preset_type,
                                              const std::string& remote_id,
                                              const std::string& etag,
                                              long long          updated_time)>;

/**
 * IPresetSyncProvider - narrow interface for synchronizing user presets with
 * a remote store. Implementations include:
 *   - OrcaCloudServiceAgent (REST per-preset against cloud.orcaslicer.com)
 *   - WebDAVSyncProvider    (PROPFIND/PUT against any WebDAV server)
 *   - GitSyncProvider       (clone/commit/push against any Git remote)
 *
 * Lifetime is owned by NetworkAgent. The active provider is configured from
 * Preferences and may be swapped at runtime via NetworkAgent::set_sync_provider.
 *
 * Conflict handling: concurrent modifications are detected at push_preset time
 * via an etag/precondition mismatch (HTTP 412/409). On detection the provider
 * fetches the current remote version and enqueues a PresetSyncConflict for the
 * GUI to surface via take_pending_conflicts(). list_presets does NOT diff
 * against local state -- a pull applies the remote content last-write-wins, so
 * local edits that were never pushed can be overwritten. Push first if in
 * doubt.
 *
 * Deletion propagation: file backends carry no tombstones. delete_preset
 * removes the remote file, but a preset deleted on one client is not actively
 * deleted on another -- it simply stops appearing in list_presets. This is the
 * accepted last-write-wins semantics for the self-hosted (WebDAV/Git) targets;
 * Orca Cloud tracks deletions server-side instead.
 */
class IPresetSyncProvider
{
public:
    virtual ~IPresetSyncProvider() = default;

    // Identity & configuration
    virtual std::string provider_id()   const = 0; // "orca" | "webdav" | "git"
    virtual std::string display_name()  const = 0; // human-readable, for UI
    virtual std::string fingerprint()   const = 0; // changes -> invalidate state

    virtual bool        is_configured() const = 0;
    virtual int         connect(std::string& error_out)        = 0;
    virtual bool        is_connected() = 0; // not const: may touch state_mutex / refresh tokens

    // Per-preset operations.
    // expected_etag = last known etag for OCC. Empty string means "create new".
    virtual PresetSyncResult push_preset(const std::string& preset_type,
                                         const std::string& preset_name,
                                         const std::string& json_content,
                                         const std::string& expected_etag) = 0;

    virtual PresetSyncResult pull_preset(const std::string& preset_type,
                                         const std::string& remote_id,
                                         std::string&       out_json) = 0;

    virtual int delete_preset(const std::string& preset_type,
                              const std::string& remote_id) = 0;

    virtual int list_presets(const PresetListCallback& cb) = 0;

    // Conflict queue. Drained by the GUI on a timer; survives stop/start cycles
    // through save_state/load_state.
    virtual std::vector<PresetSyncConflict> take_pending_conflicts() = 0;

    // After the GUI resolves a conflict, the orchestrator hands the choice
    // back to the provider so it can apply (e.g. push the merged content).
    virtual int apply_conflict_resolution(const PresetSyncConflict&             conflict,
                                          const PresetSyncConflictResolution&   resolution) = 0;

    // State persistence (etag/updated_time per remote_id, fingerprint snapshot).
    // The orchestrator does NOT pick the file path -- the provider chooses one
    // under data_dir, typically "<provider_id>_sync_state.json".
    virtual void load_state() = 0;
    virtual void save_state() = 0;
};

} // namespace Slic3r

#endif // __I_PRESET_SYNC_PROVIDER_HPP__
