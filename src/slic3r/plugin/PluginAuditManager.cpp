#include "PluginAuditManager.hpp"

#include "../Utils/OrcaCloudServiceAgent.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h" // GCODEVIEWER_APP_KEY, and SLIC3R_APP_KEY via libslic3r_version.h

#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/trivial.hpp>

#include <cstdlib>
#include <utility>

namespace Slic3r {

// ---------------------------------------------------------------------------
// Path safety
// ---------------------------------------------------------------------------

bool is_inside_allowed_root(const boost::filesystem::path& candidate, const boost::filesystem::path& allowed_root)
{
    namespace fs = boost::filesystem;

    boost::system::error_code ec;

    // Canonicalize both paths.  weakly_canonical resolves symlinks but does
    // NOT require the path to exist — it canonicalizes the prefix that exists
    // and appends the non-existing tail lexically.
    fs::path canon_candidate = fs::weakly_canonical(candidate, ec);
    if (ec) {
        // Fall back to lexically_normal + absolute
        canon_candidate = fs::absolute(candidate, ec).lexically_normal();
        if (ec)
            canon_candidate = candidate;
    }

    fs::path canon_root = fs::weakly_canonical(allowed_root, ec);
    if (ec) {
        canon_root = fs::absolute(allowed_root, ec).lexically_normal();
        if (ec)
            canon_root = allowed_root;
    }

    // Component-wise comparison: the root must be a prefix of candidate,
    // and the next component must not be ".." or missing.
    auto cand_it  = canon_candidate.begin();
    auto cand_end = canon_candidate.end();
    auto root_it  = canon_root.begin();
    auto root_end = canon_root.end();

    const auto same_component = [](const fs::path& lhs, const fs::path& rhs) {
#ifdef _WIN32
        return boost::algorithm::iequals(lhs.native(), rhs.native());
#else
        return lhs == rhs;
#endif
    };

    // Consume matching components
    while (root_it != root_end && cand_it != cand_end && same_component(*root_it, *cand_it)) {
        ++root_it;
        ++cand_it;
    }

    // If we didn't consume the entire root, candidate is not inside it.
    if (root_it != root_end)
        return false;

    // The remaining path components must not traverse upward.
    for (auto it = cand_it; it != cand_end; ++it) {
        if (*it == "..")
            return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// ScopedPluginAuditContext
// ---------------------------------------------------------------------------

thread_local std::string PluginAuditManager::m_current_plugin_key           = "";
thread_local std::string PluginAuditManager::m_current_capability_name     = "";
thread_local PluginAuditManager::AuditMode PluginAuditManager::m_audit_mode = PluginAuditManager::AuditMode::Loading;
thread_local std::vector<boost::filesystem::path> PluginAuditManager::m_scoped_allowed_roots;
thread_local bool PluginAuditManager::m_has_last_violation = false;
thread_local AuditViolation PluginAuditManager::m_last_violation;

ScopedPluginAuditContext::ScopedPluginAuditContext(const std::string& plugin_key,
                                                   const std::string& capability_name,
                                                   PluginAuditManager::AuditMode mode)
    : m_previous_id(PluginAuditManager::instance().current_plugin())
    , m_previous_capability(PluginAuditManager::instance().current_capability())
    , m_previous_mode(PluginAuditManager::instance().audit_mode())
    , m_previous_scoped_roots(PluginAuditManager::m_scoped_allowed_roots)
{
    PluginAuditManager::instance().set_current_plugin(plugin_key);
    PluginAuditManager::instance().set_current_capability(capability_name);
    PluginAuditManager::instance().set_audit_mode(mode);
    PluginAuditManager::m_scoped_allowed_roots.clear();
}

ScopedPluginAuditContext::~ScopedPluginAuditContext()
{
    PluginAuditManager::instance().set_current_plugin(m_previous_id);
    PluginAuditManager::instance().set_current_capability(m_previous_capability);
    PluginAuditManager::instance().set_audit_mode(m_previous_mode);
    PluginAuditManager::m_scoped_allowed_roots = std::move(m_previous_scoped_roots);
}

// ---------------------------------------------------------------------------
// PluginAuditManager
// ---------------------------------------------------------------------------

PluginAuditManager& PluginAuditManager::instance()
{
    static PluginAuditManager mgr;
    return mgr;
}

void PluginAuditManager::set_current_plugin(const std::string& plugin_key) { m_current_plugin_key = plugin_key; }

std::string PluginAuditManager::current_plugin() const { return m_current_plugin_key; }

void PluginAuditManager::clear_current_plugin() { m_current_plugin_key.clear(); }

void PluginAuditManager::set_current_capability(const std::string& capability_name) { m_current_capability_name = capability_name; }

std::string PluginAuditManager::current_capability() const { return m_current_capability_name; }

void PluginAuditManager::clear_current_capability() { m_current_capability_name.clear(); }

void PluginAuditManager::add_global_allowed_root(const boost::filesystem::path& root)
{
    if (root.empty())
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_global_allowed_roots.push_back(root);
    BOOST_LOG_TRIVIAL(info) << "[AUDIT] Global allowed root: " << root.string();
}

void PluginAuditManager::add_scoped_allowed_root(const boost::filesystem::path& root)
{
    if (root.empty())
        return;

    m_scoped_allowed_roots.push_back(root);
    BOOST_LOG_TRIVIAL(info) << "[AUDIT] Scoped allowed root for plugin " << current_plugin() << ": " << root.string();
}

// ---------------------------------------------------------------------------
// Denied filenames
// ---------------------------------------------------------------------------

void PluginAuditManager::add_denied_filename(const std::string& filename)
{
    if (filename.empty())
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_denied_filenames.push_back(filename);
    BOOST_LOG_TRIVIAL(info) << "[AUDIT] Denied filename: " << filename;
}

std::vector<std::string> PluginAuditManager::default_denied_filenames()
{
    // AppConfig::config_path() picks .conf vs .ini on the USE_JSON_CONFIG ifdef and the app key
    // on its mode, and is a non-static member we cannot call without an instance.  Denying all
    // four names is cheaper and more robust than replicating that; the two unused names cost one
    // string comparison each.  Single-sourced here so install_hook() and the tests seed from the
    // same list and cannot drift apart.
    return {
        SLIC3R_APP_KEY ".conf",
        GCODEVIEWER_APP_KEY ".conf",
        SLIC3R_APP_KEY ".ini",
        GCODEVIEWER_APP_KEY ".ini",
        secret_constants::USER_SECRET_FILENAME,
    };
}

bool PluginAuditManager::is_denied_filename(const boost::filesystem::path& candidate) const
{
    // Match on the base name alone, with no path resolution.  Traversal is handled for free,
    // because filename() of data_dir()/plugins/../OrcaSlicer.conf is already "OrcaSlicer.conf",
    // and the prefix rule covers the .bak/.tmp companions that hold the same secrets plus Windows
    // alternate data streams ("OrcaSlicer.conf:stream").  A plugin that launders a denied file
    // through a symlink, a hardlink, a subprocess, or a Windows 8.3 short name is out of scope
    // (see the design doc): this blocks direct access, not an actively evasive plugin.
    const std::string filename = candidate.filename().string();
    if (filename.empty())
        return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& denied : m_denied_filenames) {
        if (boost::algorithm::istarts_with(filename, denied))
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Audit mode
// ---------------------------------------------------------------------------

void PluginAuditManager::set_audit_mode(AuditMode mode) { m_audit_mode = mode; }

PluginAuditManager::AuditMode PluginAuditManager::audit_mode() const { return m_audit_mode; }

// ---------------------------------------------------------------------------
// Policy checks
// ---------------------------------------------------------------------------

AuditDecision PluginAuditManager::check_path_access(const boost::filesystem::path& path, bool is_write)
{
    if (path.empty())
        return {true, ""};

    std::string plugin_key = current_plugin();
    if (plugin_key.empty())
        return {true, ""}; // not running inside a plugin context

    // Denied filenames are checked first, above both the Loading exemption below and the
    // allowed roots.  The app config and the cloud refresh token live directly inside
    // data_dir(), which is a global allowed root, and no scope ever sets Enforcing — so a
    // deny placed any lower would be unreachable for reads.
    if (is_denied_filename(path)) {
        BOOST_LOG_TRIVIAL(warning) << "[AUDIT] block path=" << path.string() << " is_write=" << is_write
                                   << " plugin=" << plugin_key << " reason=denied filename";
        return {false, "denied filename"};
    }

    // During import/loading, only block writes.  Python must be able to read
    // stdlib modules and the plugin file itself during import.
    if (m_audit_mode == AuditMode::Loading && !is_write)
        return {true, ""};

    namespace fs = boost::filesystem;
    fs::path candidate = path;

    // Resolve relative paths against the current working directory
    if (candidate.is_relative()) {
        boost::system::error_code ec;
        fs::path absolute_candidate = fs::absolute(candidate, ec);
        if (!ec)
            candidate = absolute_candidate;
    }

    for (const auto& root : m_scoped_allowed_roots) {
        if (is_inside_allowed_root(candidate, root)) {
            return {true, ""};
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& root : m_global_allowed_roots) {
            if (is_inside_allowed_root(candidate, root)) {
                return {true, ""};
            }
        }
    }

    BOOST_LOG_TRIVIAL(warning) << "[AUDIT] block path=" << candidate.string() << " is_write=" << is_write
                               << " audit_mode=" << (m_audit_mode == AuditMode::Loading ? "Loading" : "Enforcing")
                               << " plugin=" << plugin_key;
    return {false, "outside allowed root"};
}

AuditDecision PluginAuditManager::check_open(const std::string& path_str, const std::string& mode)
{
    const bool is_write = mode.find('w') != std::string::npos || mode.find('a') != std::string::npos ||
                          mode.find('+') != std::string::npos;
    return check_path_access(boost::filesystem::path(path_str), is_write);
}

void PluginAuditManager::report_violation(const AuditViolation& violation)
{
    m_last_violation     = violation;
    m_has_last_violation = true;

    BOOST_LOG_TRIVIAL(warning) << "[AUDIT BLOCKED] plugin=" << violation.plugin_key << " event=" << violation.event_name
                               << " path=" << violation.path.string() << " reason=" << violation.reason;
}

void PluginAuditManager::clear_last_violation()
{
    m_has_last_violation = false;
    m_last_violation     = AuditViolation{};
}

bool PluginAuditManager::last_violation(AuditViolation& violation) const
{
    if (!m_has_last_violation)
        return false;

    violation = m_last_violation;
    return true;
}

// ---------------------------------------------------------------------------
// The C-level audit hook
// ---------------------------------------------------------------------------

namespace {

// Records a blocked event and raises PermissionError in the calling interpreter.  Returns -1
// so an event branch can `return report_denied(...)` directly.
int report_denied(PluginAuditManager&            mgr,
                  const std::string&             event_name,
                  const boost::filesystem::path& path,
                  const AuditDecision&           decision)
{
    AuditViolation violation;
    violation.plugin_key = mgr.current_plugin();
    violation.event_name = event_name;
    violation.path       = path;
    violation.reason     = decision.reason;
    mgr.report_violation(violation);

    PyErr_SetString(PyExc_PermissionError, "Plugin attempted to access a blocked file path");
    return -1;
}

} // namespace

int PluginAuditManager::audit_hook(const char* event, PyObject* args, void* user_data)
{
    auto* mgr = static_cast<PluginAuditManager*>(user_data);
    if (!mgr)
        return 0;

    std::string event_name(event ? event : "");

    // Verbose logging of every audit event (can be noisy)
    if (mgr->verbose_events) {
        BOOST_LOG_TRIVIAL(debug) << "[AUDIT EVENT] " << event_name;
    }

    // extensive list of audit events can be found at https://docs.python.org/3/library/audit_events.html

    // --- open event ---
    if (event_name == "open") {
        const char* path_cstr = nullptr;
        const char* mode_cstr = nullptr;
        int flags             = 0;

        // open(path, mode, flags) — path may be str, bytes, or int fd
        if (!PyArg_ParseTuple(args, "s|si", &path_cstr, &mode_cstr, &flags)) {
            PyErr_Clear(); // couldn't parse; allow
            return 0;
        }

        std::string path_str(path_cstr ? path_cstr : "");
        std::string mode_str(mode_cstr ? mode_cstr : "r");

        AuditDecision decision = mgr->check_open(path_str, mode_str);
        if (!decision.allowed)
            return report_denied(*mgr, event_name, path_str, decision);
        return 0;
    }

    // --- os.rename event (raised by os.rename and os.replace) ---
    if (event_name == "os.rename") {
        const char* src_cstr   = nullptr;
        const char* dst_cstr   = nullptr;
        PyObject*   src_dir_fd = nullptr;
        PyObject*   dst_dir_fd = nullptr;

        // os.rename(src, dst, src_dir_fd, dst_dir_fd) — paths may be str, bytes, or int fd.
        // The dir_fd arguments are unused, but must be accepted for the tuple to parse.
        if (!PyArg_ParseTuple(args, "ss|OO", &src_cstr, &dst_cstr, &src_dir_fd, &dst_dir_fd)) {
            PyErr_Clear(); // couldn't parse; allow
            return 0;
        }

        // A rename writes at both ends, so either end being denied blocks the call.
        for (const char* path_cstr : {src_cstr, dst_cstr}) {
            std::string   path_str(path_cstr ? path_cstr : "");
            AuditDecision decision = mgr->check_path_access(path_str, /* is_write */ true);
            if (!decision.allowed)
                return report_denied(*mgr, event_name, path_str, decision);
        }
        return 0;
    }

    // --- os.remove event (raised by os.remove and os.unlink) ---
    if (event_name == "os.remove") {
        const char* path_cstr = nullptr;
        PyObject*   dir_fd    = nullptr;

        // os.remove(path, dir_fd) — path may be str, bytes, or int fd
        if (!PyArg_ParseTuple(args, "s|O", &path_cstr, &dir_fd)) {
            PyErr_Clear(); // couldn't parse; allow
            return 0;
        }

        std::string   path_str(path_cstr ? path_cstr : "");
        AuditDecision decision = mgr->check_path_access(path_str, /* is_write */ true);
        if (!decision.allowed)
            return report_denied(*mgr, event_name, path_str, decision);
        return 0;
    }

    // Unknown event — allow by default
    return 0;
}

void PluginAuditManager::install_hook()
{
    if (PySys_AddAuditHook(audit_hook, this) < 0) {
        BOOST_LOG_TRIVIAL(error) << "[AUDIT] Failed to install CPython audit hook";
        return;
    }
    BOOST_LOG_TRIVIAL(info) << "[AUDIT] CPython audit hook installed successfully";

    // data_dir() is the only globally-allowed root during enforced plugin execution.
    // The executable directory and resources directory are intentionally NOT allowed
    // here: plugins must not write outside data_dir() (G-code plugins additionally get
    // the temp G-code folder via a scoped root). Reads remain permissive in Loading mode.
    add_global_allowed_root(data_dir());

    // The user's app config and cloud credentials live directly inside data_dir(), so the
    // root just granted would otherwise expose them to any plugin.  Deny them by name.
    //
    // Seeded here rather than by each secret's owner because install_hook() runs during lazy
    // interpreter init and therefore provably precedes any plugin bytecode, whereas
    // OrcaCloudServiceAgent::set_config_dir runs during networking init — neither strictly
    // precedes the other, and if Orca cloud never initializes an owner-registered token deny
    // would never exist at all.  default_denied_filenames() is the single source of that list
    // (see its comment for why all four config names are denied); the tests seed from it too.
    for (const auto& name : default_denied_filenames())
        add_denied_filename(name);
}

} // namespace Slic3r
