#ifndef slic3r_PluginAuditManager_hpp_
#define slic3r_PluginAuditManager_hpp_

#include <Python.h>
#include <mutex>
#include <string>
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
    boost::filesystem::path  path;
    std::string            reason;
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
    void add_global_allowed_root(const boost::filesystem::path& root);
    void add_scoped_allowed_root(const boost::filesystem::path& root);

    // --- denied-filenames registry ---
    // Filenames a plugin may never touch, in any directory, regardless of audit mode or
    // enclosing allowed root.  A candidate is denied when its filename starts with a
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

    // --- enforcement mode ---
    enum class AuditMode {
        // Import/loading phase: allow reads anywhere, only block writes
        // outside allowed roots.  Python needs to read stdlib modules
        // during import and those are not inside plugin directories.
        Loading,

        // Execution phase: block both reads and writes outside allowed
        // roots, plus subprocess/socket/ctypes.
        Enforcing,
    };

    void set_audit_mode(AuditMode mode);
    AuditMode audit_mode() const;

    // --- policy checks ---
    // Shared core for every audited filesystem event.  The deny list is consulted above the
    // Loading-mode read exemption and above the allowed roots, so a denied filename is
    // blocked even though every scope currently runs in Loading and the files in question
    // sit inside data_dir(), which is itself a global allowed root.
    AuditDecision check_path_access(const boost::filesystem::path& candidate, bool is_write);
    AuditDecision check_open(const std::string& path, const std::string& mode);

    void report_violation(const AuditViolation& violation);
    void clear_last_violation();
    bool last_violation(AuditViolation& violation) const;

    bool verbose_events = true;

private:
    friend class ScopedPluginAuditContext;

    PluginAuditManager() = default;

    static int audit_hook(const char* event, PyObject* args, void* user_data);

    static thread_local std::string  m_current_plugin_key;
    static thread_local std::string  m_current_capability_name;
    static thread_local AuditMode    m_audit_mode;
    static thread_local std::vector<boost::filesystem::path> m_scoped_allowed_roots;
    static thread_local bool         m_has_last_violation;
    static thread_local AuditViolation m_last_violation;

    // mutable: is_denied_filename() is a const query that must lock.
    mutable std::mutex m_mutex;
    std::vector<boost::filesystem::path> m_global_allowed_roots;
    std::vector<std::string>             m_denied_filenames;
};

// RAII guard that sets the current plugin key and capability name, restoring the previous
// pair on scope exit. `capability_name` may be empty for calls that are not scoped to a
// single capability.
class ScopedPluginAuditContext
{
public:
    explicit ScopedPluginAuditContext(
        const std::string&                   plugin_key,
        const std::string&                   capability_name = {},
        PluginAuditManager::AuditMode        mode = PluginAuditManager::AuditMode::Loading);

    ~ScopedPluginAuditContext();

    ScopedPluginAuditContext(const ScopedPluginAuditContext&)            = delete;
    ScopedPluginAuditContext& operator=(const ScopedPluginAuditContext&) = delete;

private:
    std::string                   m_previous_id;
    std::string                   m_previous_capability;
    PluginAuditManager::AuditMode m_previous_mode;
    std::vector<boost::filesystem::path> m_previous_scoped_roots;
};

} // namespace Slic3r

#endif // slic3r_PluginAuditManager_hpp_
