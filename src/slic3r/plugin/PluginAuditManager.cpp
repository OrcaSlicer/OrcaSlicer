#include "PluginAuditManager.hpp"

#include "libslic3r/Utils.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/trivial.hpp>

#include <cstdlib>
#include <utility>

namespace Slic3r {

// ---------------------------------------------------------------------------
// Path safety
// ---------------------------------------------------------------------------

bool is_inside_allowed_root(const std::filesystem::path& candidate, const std::filesystem::path& allowed_root)
{
    namespace fs = std::filesystem;

    std::error_code ec;

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
thread_local PluginAuditManager::AuditMode PluginAuditManager::m_audit_mode = PluginAuditManager::AuditMode::Loading;
thread_local std::vector<std::filesystem::path> PluginAuditManager::m_scoped_allowed_roots;
thread_local bool PluginAuditManager::m_has_last_violation = false;
thread_local AuditViolation PluginAuditManager::m_last_violation;

ScopedPluginAuditContext::ScopedPluginAuditContext(const std::string& plugin_key, PluginAuditManager::AuditMode mode)
    : m_previous_id(PluginAuditManager::instance().current_plugin())
    , m_previous_mode(PluginAuditManager::instance().audit_mode())
    , m_previous_scoped_roots(PluginAuditManager::m_scoped_allowed_roots)
{
    PluginAuditManager::instance().set_current_plugin(plugin_key);
    PluginAuditManager::instance().set_audit_mode(mode);
    PluginAuditManager::m_scoped_allowed_roots.clear();
}

ScopedPluginAuditContext::~ScopedPluginAuditContext()
{
    PluginAuditManager::instance().set_current_plugin(m_previous_id);
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

void PluginAuditManager::add_global_allowed_root(const std::filesystem::path& root)
{
    if (root.empty())
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_global_allowed_roots.push_back(root);
    BOOST_LOG_TRIVIAL(info) << "[AUDIT] Global allowed root: " << root.string();
}

void PluginAuditManager::add_scoped_allowed_root(const std::filesystem::path& root)
{
    if (root.empty())
        return;

    m_scoped_allowed_roots.push_back(root);
    BOOST_LOG_TRIVIAL(info) << "[AUDIT] Scoped allowed root for plugin " << current_plugin() << ": " << root.string();
}

// ---------------------------------------------------------------------------
// Audit mode
// ---------------------------------------------------------------------------

void PluginAuditManager::set_audit_mode(AuditMode mode) { m_audit_mode = mode; }

PluginAuditManager::AuditMode PluginAuditManager::audit_mode() const { return m_audit_mode; }

// ---------------------------------------------------------------------------
// Policy checks
// ---------------------------------------------------------------------------

AuditDecision PluginAuditManager::check_open(const std::string& path_str, const std::string& mode)
{
    if (path_str.empty())
        return {true, ""};

    std::string plugin_key = current_plugin();
    if (plugin_key.empty())
        return {true, ""}; // not running inside a plugin context

    // During import/loading, only block writes.  Python must be able to read
    // stdlib modules and the plugin file itself during import.
    if (m_audit_mode == AuditMode::Loading) {
        bool is_write = (mode.find('w') != std::string::npos || mode.find('a') != std::string::npos || mode.find('+') != std::string::npos);
        if (!is_write)
            return {true, ""};
    }

    namespace fs = std::filesystem;
    fs::path candidate(path_str);

    // Resolve relative paths against the current working directory
    if (candidate.is_relative()) {
        std::error_code ec;
        candidate = fs::absolute(candidate, ec);
        if (ec)
            candidate = fs::path(path_str);
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

    BOOST_LOG_TRIVIAL(warning) << "[AUDIT] block path=" << candidate.string() << " open_mode=" << mode
                               << " audit_mode=" << (m_audit_mode == AuditMode::Loading ? "Loading" : "Enforcing")
                               << " plugin=" << plugin_key;
    return {false, "outside allowed root"};
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
        if (!decision.allowed) {
            AuditViolation violation;
            violation.plugin_key = mgr->current_plugin();
            violation.event_name = event_name;
            violation.path       = path_str;
            violation.reason     = decision.reason;
            mgr->report_violation(violation);

            PyErr_SetString(PyExc_PermissionError, "Plugin attempted to access a blocked file path");
            return -1;
        }
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
}

} // namespace Slic3r
