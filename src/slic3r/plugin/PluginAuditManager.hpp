#ifndef slic3r_PluginAuditManager_hpp_
#define slic3r_PluginAuditManager_hpp_

#include <Python.h>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/filesystem.hpp>

namespace Slic3r {

struct AuditDecision {
    bool        allowed = true;
    std::string reason;
};

struct AuditViolation {
    std::string            plugin_key;
    std::string            event_name;
    std::string            reason;
};

// A filesystem root a plugin may access while an audit context is active. allow_write is
// false for a root that only grants reads (e.g. the bundled, shared resources folder) --
// a write-shaped event never matches such a root, even though a read-shaped one does.
struct AllowedRoot {
    boost::filesystem::path path;
    bool                    allow_write = true;
};

// The set of CPython audit events PluginAuditManager recognizes, grouped by the kind of
// operation they represent. None means the event isn't one audit_hook() acts on at all.
enum class AuditEventCategory {
    None,
    FsRead,
    FsReadWrite,
    FsCreate,
    FsDelete,
    Http,
    Socket,
    ProcessCreate,
    Threading,
};

// Returns true if candidate resolves to a path inside allowed_root.
// Uses weakly_canonical and component-wise comparison to reject traversal attacks.
bool is_inside_allowed_root(const boost::filesystem::path& candidate,
                            const boost::filesystem::path& allowed_root);

class PluginAuditManager
{
public:
    static PluginAuditManager& instance();

    // Call once after Py_Initialize to install the global audit hook.
    void install_hook();

    // --- current-plugin context (thread_local) ---
    void        set_current_plugin(const std::string& plugin_key);
    std::string current_plugin() const;
    void        clear_current_plugin();

    // --- current-capability context (thread_local) ---
    // The capability whose method is currently executing, within the current plugin. Empty
    // while a plugin-wide call runs, and during capture (get_name/get_type), where the
    // capability has no cached name yet.
    void        set_current_capability(const std::string& capability_name);
    std::string current_capability() const;
    void        clear_current_capability();

    // --- allowed-roots registry ---
    // allow_write = false registers a read-only root: a read-shaped event inside it is allowed,
    // but a write/create/delete-shaped event is not, so it falls through to the normal
    // prompt-or-deny path instead.
    void add_global_allowed_root(const boost::filesystem::path& root, bool allow_write = true);
    void add_scoped_allowed_root(const boost::filesystem::path& root, bool allow_write = true);

    // --- denied-filenames registry ---
    // Filenames a plugin may never touch, in any directory, regardless of the enclosing allowed
    // root.  A candidate is denied when its filename starts with a
    // registered name, so .bak/.tmp companions are covered by the same entry.
    //
    // The comparison is case-insensitive on every platform, unlike the _WIN32-only iequals
    // in is_inside_allowed_root: the default macOS APFS configuration is case-insensitive
    // too, so `orcaslicer.conf` reaches the real file there.  Over-blocking a genuinely
    // distinct name on Linux is the fail-safe direction and costs nothing real.
    void add_denied_filename(const std::string& filename);

    // The list install_hook() seeds into the deny registry: the app config (both app keys and
    // both extensions) and the cloud refresh token. Exposed so tests seed the exact same set
    // without a live interpreter, so the test and production seeding cannot drift apart.
    static std::vector<std::string> default_denied_filenames();

    // True when candidate's base name starts with a denied name (case-insensitive). No path
    // resolution: laundering a denied file through a symlink, hardlink, subprocess, or Windows
    // 8.3 short name is out of scope (see the design doc). This blocks direct access only.
    bool is_denied_filename(const boost::filesystem::path& candidate) const;

    // --- denied-path-keyword registry ---
    // Keywords that categorically deny a path if ANY of its components (directory or file
    // name), not just the base name, contains one case-insensitively -- e.g. a "secrets"
    // subfolder, a "certificates" folder, or a "conf"/"config" file anywhere the plugin can
    // otherwise reach, including inside an allowed root. This is intentionally broader and
    // fuzzier than the exact-name is_denied_filename registry: it exists to categorically rule
    // out whole classes of sensitive paths (secrets, certificates, config) rather than name
    // specific known files, at the cost of over-blocking an unrelated name that happens to
    // contain the keyword -- the fail-safe direction, same rationale as is_denied_filename.
    void add_denied_path_keyword(const std::string& keyword);

    // The list install_hook() seeds into the keyword registry. Exposed so tests seed the exact
    // same set without a live interpreter.
    static std::vector<std::string> default_denied_path_keywords();

    // True when any component of candidate's (canonicalized) path contains a registered
    // keyword, case-insensitively.
    bool is_denied_path_keyword(const boost::filesystem::path& candidate) const;

    // is_denied_filename(candidate) || is_denied_path_keyword(candidate). Convenience for
    // call sites that only need to know whether a path is categorically off-limits, not which
    // specific rule fired.
    bool is_denied_path(const boost::filesystem::path& candidate) const;

    // --- policy checks ---
    // Shared core for every audited filesystem event.  The deny checks are consulted above the
    // allowed roots, so a denied path is blocked even when it sits inside an allowed root (e.g.
    // data_dir(), which is a global allowed root).
    AuditDecision check_path_access(const boost::filesystem::path& candidate, bool is_write);
    AuditDecision check_open(const std::string& path, const std::string& mode);

    // Ask the user to grant the requested filesystem-read paths. The request may originate on a
    // plugin load worker, so the implementation marshals the modal dialog to the wx main thread.
    // Returns true only when every missing path was granted and persisted; denial aborts the plugin
    // load without adding a permission.
    bool request_filesystem_read_permissions(const std::string& plugin_key,
                                             const std::vector<std::string>& paths);

    void report_violation(const AuditViolation& violation);
    bool audit_denial_pending() const;
    void clear_audit_denial();
    void clear_last_violation();
    bool last_violation(AuditViolation& violation) const;

    // --- call-site cascade cache ---
    // A single plugin action often fires several nested CPython audit events as it passes
    // through stdlib layers (urllib.request calling http.client calling socket, for example).
    // Once the user approves one event, every stdlib frame still on the stack for that call
    // is recorded here by (filename, function, first line) identity. A later event whose own
    // ancestor chain still contains one of those frames is the same logical action seen from
    // a deeper layer, so it is auto-approved instead of prompting again.
    bool has_approved_ancestor(const std::string& plugin_key, const std::vector<std::string>& call_site_ids) const;
    void record_approved_call_sites(const std::string& plugin_key, const std::vector<std::string>& call_site_ids);

    bool verbose_events = true;

private:
    friend class ScopedPluginAuditContext;

    PluginAuditManager() = default;

    static int audit_hook(const char* event, PyObject* args, void* user_data);

    static thread_local std::string                   m_current_plugin_key;
    static thread_local std::string                   m_current_capability_name;
    static thread_local std::vector<AllowedRoot>       m_scoped_allowed_roots;
    static thread_local bool                           m_audit_denial_pending;
    static thread_local bool                           m_has_last_violation;
    static thread_local AuditViolation                 m_last_violation;

    // mutable: is_denied_filename() and has_approved_ancestor() are const queries that must lock.
    mutable std::mutex m_mutex;
    std::vector<AllowedRoot>             m_global_allowed_roots;
    std::vector<std::string>             m_denied_filenames;
    std::vector<std::string>             m_denied_path_keywords;
    std::unordered_map<std::string, std::unordered_set<std::string>> m_approved_call_sites; // plugin_key -> call-site ids
};

// RAII guard that sets the current plugin key and capability name, restoring the previous
// pair on scope exit. `capability_name` may be empty for calls that are not scoped to a
// single capability.
class ScopedPluginAuditContext
{
public:
    explicit ScopedPluginAuditContext(
        const std::string&                   plugin_key,
        const std::string&                   capability_name = {});

    ~ScopedPluginAuditContext();

    ScopedPluginAuditContext(const ScopedPluginAuditContext&)            = delete;
    ScopedPluginAuditContext& operator=(const ScopedPluginAuditContext&) = delete;

private:
    std::string                   m_previous_id;
    std::string                   m_previous_capability;
    std::vector<AllowedRoot>      m_previous_scoped_roots;
};

} // namespace Slic3r

#endif // slic3r_PluginAuditManager_hpp_
