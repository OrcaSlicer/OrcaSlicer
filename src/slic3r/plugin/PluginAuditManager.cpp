#include "PluginAuditManager.hpp"

#include "../Utils/OrcaCloudServiceAgent.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h" // GCODEVIEWER_APP_KEY, and SLIC3R_APP_KEY via libslic3r_version.h

#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <future>
#include <slic3r/GUI/BindDialog.hpp>
#include <slic3r/GUI/GUI_App.hpp>
#include <slic3r/plugin/PluginFsUtils.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <wx/event.h>
#include <wx/msgdlg.h>

namespace Slic3r {

// extensive list of audit events can be found at https://docs.python.org/3/library/audit_events.html
static const std::unordered_map<std::string, AuditEventCategory> audit_event_categories{
    // fsread
    {"glob.glob", AuditEventCategory::FsRead},
    {"glob.glob/2", AuditEventCategory::FsRead},
    {"os.fwalk", AuditEventCategory::FsRead},
    {"os.getxattr", AuditEventCategory::FsRead},
    {"os.listdir", AuditEventCategory::FsRead},
    {"os.listdrives", AuditEventCategory::FsRead},
    {"os.listmounts", AuditEventCategory::FsRead},
    {"os.listvolumes", AuditEventCategory::FsRead},
    {"os.listxattr", AuditEventCategory::FsRead},
    {"os.scandir", AuditEventCategory::FsRead},
    {"os.walk", AuditEventCategory::FsRead},
    {"pathlib.Path.glob", AuditEventCategory::FsRead},
    {"pathlib.Path.rglob", AuditEventCategory::FsRead},

    // fsreadwrite
    {"os.chflags", AuditEventCategory::FsReadWrite},
    {"os.chmod", AuditEventCategory::FsReadWrite},
    {"os.chown", AuditEventCategory::FsReadWrite},
    {"os.removexattr", AuditEventCategory::FsReadWrite},
    {"os.rename", AuditEventCategory::FsReadWrite},
    {"os.setxattr", AuditEventCategory::FsReadWrite},
    {"os.truncate", AuditEventCategory::FsReadWrite},
    {"os.utime", AuditEventCategory::FsReadWrite},
    {"shutil.chown", AuditEventCategory::FsReadWrite},
    {"shutil.copymode", AuditEventCategory::FsReadWrite},
    {"shutil.copystat", AuditEventCategory::FsReadWrite},
    {"shutil.copyfile", AuditEventCategory::FsReadWrite},
    {"shutil.copytree", AuditEventCategory::FsReadWrite},
    {"shutil.make_archive", AuditEventCategory::FsReadWrite},
    {"shutil.move", AuditEventCategory::FsReadWrite},
    {"shutil.unpack_archive", AuditEventCategory::FsReadWrite},

    // fscreate
    {"os.link", AuditEventCategory::FsCreate},
    {"os.mkdir", AuditEventCategory::FsCreate},
    {"os.symlink", AuditEventCategory::FsCreate},
    {"tempfile.mkdtemp", AuditEventCategory::FsCreate},
    {"tempfile.mkstemp", AuditEventCategory::FsCreate},
    {"_winapi.CreateJunction", AuditEventCategory::FsCreate},

    // fsdelete
    {"os.remove", AuditEventCategory::FsDelete},
    {"os.rmdir", AuditEventCategory::FsDelete},
    {"shutil.rmtree", AuditEventCategory::FsDelete},

    // http
    {"http.client.connect", AuditEventCategory::Http},
    {"http.client.send", AuditEventCategory::Http},
    {"urllib.Request", AuditEventCategory::Http},

    // socket
    {"socket.__new__", AuditEventCategory::Socket},
    {"socket.bind", AuditEventCategory::Socket},
    {"socket.connect", AuditEventCategory::Socket},
    {"socket.getaddrinfo", AuditEventCategory::Socket},
    {"socket.gethostbyaddr", AuditEventCategory::Socket},
    {"socket.gethostbyname", AuditEventCategory::Socket},
    {"socket.gethostname", AuditEventCategory::Socket},
    {"socket.getnameinfo", AuditEventCategory::Socket},
    {"socket.getservbyname", AuditEventCategory::Socket},
    {"socket.getservbyport", AuditEventCategory::Socket},
    {"socket.sendmsg", AuditEventCategory::Socket},
    {"socket.sendto", AuditEventCategory::Socket},

    // processcreate
    {"os.fork", AuditEventCategory::ProcessCreate},
    {"os.forkpty", AuditEventCategory::ProcessCreate},
    {"os.posix_spawn", AuditEventCategory::ProcessCreate},
    {"os.spawn", AuditEventCategory::ProcessCreate},
    {"os.system", AuditEventCategory::ProcessCreate},
    {"os.startfile", AuditEventCategory::ProcessCreate},
    {"os.startfile/2", AuditEventCategory::ProcessCreate},
    {"pty.spawn", AuditEventCategory::ProcessCreate},
    {"subprocess.Popen", AuditEventCategory::ProcessCreate},
    {"_winapi.CreateProcess", AuditEventCategory::ProcessCreate},
    {"_posixsubprocess.fork_exec", AuditEventCategory::ProcessCreate},
};

// Returns the category event_name belongs to, or AuditEventCategory::None when it isn't audited.
static AuditEventCategory event_category(const std::string& event_name)
{
    const auto it = audit_event_categories.find(event_name);
    return it == audit_event_categories.end() ? AuditEventCategory::None : it->second;
}

// True for the categories whose targets are filesystem paths, as opposed to a network
// address or a process command line -- the deny-path and allowed-root checks only make sense
// against a path.
static bool is_fs_category(AuditEventCategory category)
{
    switch (category) {
    case AuditEventCategory::FsRead:
    case AuditEventCategory::FsReadWrite:
    case AuditEventCategory::FsCreate:
    case AuditEventCategory::FsDelete:
        return true;
    default:
        return false;
    }
}

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

thread_local std::string PluginAuditManager::m_current_plugin_key = "";
thread_local std::string PluginAuditManager::m_current_capability_name = "";
thread_local std::vector<AllowedRoot> PluginAuditManager::m_scoped_allowed_roots;
thread_local bool PluginAuditManager::m_audit_denial_pending = false;
thread_local bool PluginAuditManager::m_has_last_violation = false;
thread_local AuditViolation PluginAuditManager::m_last_violation;

ScopedPluginAuditContext::ScopedPluginAuditContext(const std::string& plugin_key,
                                                   const std::string& capability_name)
    : m_previous_id(PluginAuditManager::instance().current_plugin())
    , m_previous_capability(PluginAuditManager::instance().current_capability())
    , m_previous_scoped_roots(PluginAuditManager::m_scoped_allowed_roots)
{
    PluginAuditManager::instance().set_current_plugin(plugin_key);
    PluginAuditManager::instance().set_current_capability(capability_name);
    PluginAuditManager::m_scoped_allowed_roots.clear();
}

ScopedPluginAuditContext::~ScopedPluginAuditContext()
{
    PluginAuditManager::instance().set_current_plugin(m_previous_id);
    PluginAuditManager::instance().set_current_capability(m_previous_capability);
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

void PluginAuditManager::add_global_allowed_root(const boost::filesystem::path& root, bool allow_write)
{
    if (root.empty())
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_global_allowed_roots.push_back({root, allow_write});
    BOOST_LOG_TRIVIAL(info) << "[AUDIT] Global allowed root: " << root.string() << " allow_write=" << allow_write;
}

void PluginAuditManager::add_scoped_allowed_root(const boost::filesystem::path& root, bool allow_write)
{
    if (root.empty())
        return;

    m_scoped_allowed_roots.push_back({root, allow_write});
    BOOST_LOG_TRIVIAL(info) << "[AUDIT] Scoped allowed root for plugin " << current_plugin() << ": " << root.string()
                            << " allow_write=" << allow_write;
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
// Denied path keywords
// ---------------------------------------------------------------------------

void PluginAuditManager::add_denied_path_keyword(const std::string& keyword)
{
    if (keyword.empty())
        return;

    std::string lower = keyword;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });

    std::lock_guard<std::mutex> lock(m_mutex);
    m_denied_path_keywords.push_back(lower);
    BOOST_LOG_TRIVIAL(info) << "[AUDIT] Denied path keyword: " << lower;
}

std::vector<std::string> PluginAuditManager::default_denied_path_keywords()
{
    // Broad, categorical rules on top of the exact-name is_denied_filename registry: a plugin
    // must never be able to reach a secret, a certificate, or a configuration file just because
    // it happens to live inside an otherwise-allowed root (e.g. the bundled TLS client cert at
    // resources_dir()/cert/..., which would become reachable the moment resources_dir() is
    // granted as a read-only allowed root).
    return {"secret", "cert", "conf"};
}

bool PluginAuditManager::is_denied_path_keyword(const boost::filesystem::path& candidate) const
{
    namespace fs = boost::filesystem;

    boost::system::error_code ec;
    fs::path canon = fs::weakly_canonical(candidate, ec);
    if (ec) {
        canon = fs::absolute(candidate, ec).lexically_normal();
        if (ec)
            canon = candidate;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_denied_path_keywords.empty())
        return false;

    for (const auto& component : canon) {
        std::string name = component.string();
        if (name.empty())
            continue;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
        for (const auto& keyword : m_denied_path_keywords) {
            if (name.find(keyword) != std::string::npos)
                return true;
        }
    }
    return false;
}

bool PluginAuditManager::is_denied_path(const boost::filesystem::path& candidate) const
{
    return is_denied_filename(candidate) || is_denied_path_keyword(candidate);
}

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

    // Denied filenames/keywords are checked before the allowed roots.  The app config and the
    // cloud refresh token live directly inside data_dir(), which is a global allowed root, and
    // the bundled TLS client cert lives inside resources_dir(), a read-only global allowed
    // root, so a deny placed any lower would be unreachable.
    if (is_denied_filename(path)) {
        BOOST_LOG_TRIVIAL(warning) << "[AUDIT] block path=" << path.string() << " is_write=" << is_write
                                   << " plugin=" << plugin_key << " reason=denied filename";
        return {false, "denied filename"};
    }
    if (is_denied_path_keyword(path)) {
        BOOST_LOG_TRIVIAL(warning) << "[AUDIT] block path=" << path.string() << " is_write=" << is_write
                                   << " plugin=" << plugin_key << " reason=denied path keyword";
        return {false, "denied path keyword"};
    }

    namespace fs = boost::filesystem;
    fs::path candidate = path;

    // Resolve relative paths against the current working directory
    if (candidate.is_relative()) {
        boost::system::error_code ec;
        fs::path absolute_candidate = fs::absolute(candidate, ec);
        if (!ec)
            candidate = absolute_candidate;
    }

    // A root that doesn't allow writes only matches a read-shaped request; a write/create/
    // delete-shaped one falls through to "outside allowed root" for that root even though the
    // path is physically inside it.
    for (const auto& root : m_scoped_allowed_roots) {
        if ((!is_write || root.allow_write) && is_inside_allowed_root(candidate, root.path)) {
            return {true, ""};
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& root : m_global_allowed_roots) {
            if ((!is_write || root.allow_write) && is_inside_allowed_root(candidate, root.path)) {
                return {true, ""};
            }
        }
    }

    BOOST_LOG_TRIVIAL(warning) << "[AUDIT] block path=" << candidate.string() << " is_write=" << is_write
                               << " plugin=" << plugin_key;
    return {false, "outside allowed root"};
}

AuditDecision PluginAuditManager::check_open(const std::string& path_str, const std::string& mode)
{
    const bool is_write = mode.find('w') != std::string::npos || mode.find('a') != std::string::npos ||
                          mode.find('+') != std::string::npos || mode.find('x') != std::string::npos;
    return check_path_access(boost::filesystem::path(path_str), is_write);
}

bool PluginAuditManager::request_filesystem_read_permissions(const std::string&              plugin_key,
                                                              const std::vector<std::string>& paths)
{
    if (plugin_key.empty() || paths.empty())
        return true;

    PluginDescriptor descriptor;
    if (!PluginManager::instance().try_get_plugin_descriptor(plugin_key, descriptor) || descriptor.plugin_root.empty())
        return false;

    PluginInstallState state;
    read_install_state(boost::filesystem::path(descriptor.plugin_root), state);

    std::vector<std::string> missing;
    for (const std::string& path : paths) {
        if (std::find(state.permissions.fs_read.begin(), state.permissions.fs_read.end(), path) == state.permissions.fs_read.end())
            missing.push_back(path);
    }
    if (missing.empty())
        return true;

    if (wxTheApp == nullptr || GUI::wxGetApp().is_closing())
        return false;

    auto show_dialog = [&descriptor, &missing]() {
        wxString requested_paths;
        for (const std::string& path : missing)
            requested_paths += wxString::FromUTF8(path.c_str()) + "\n";

        wxMessageDialog dialog(
            nullptr,
            wxString::Format("Plugin \"%s\" requests filesystem read access to:\n%s",
                             wxString::FromUTF8(descriptor.name.c_str()), requested_paths),
            "Plugin permissions",
            wxYES_NO | wxICON_WARNING);
        return dialog.ShowModal() == wxID_YES;
    };

    bool granted = false;
    if (wxIsMainThread()) {
        granted = show_dialog();
    } else {
        auto result = std::make_shared<std::promise<bool>>();
        auto future = result->get_future();
        GUI::wxGetApp().CallAfter([result, descriptor_name = descriptor.name, missing]() {
            wxString requested_paths;
            for (const std::string& path : missing)
                requested_paths += wxString::FromUTF8(path.c_str()) + "\n";

            wxMessageDialog dialog(
                nullptr,
                wxString::Format("Plugin \"%s\" requests filesystem read access to:\n%s",
                                 wxString::FromUTF8(descriptor_name.c_str()), requested_paths),
                "Plugin permissions",
                wxYES_NO | wxICON_WARNING);
            result->set_value(dialog.ShowModal() == wxID_YES);
        });
        granted = future.get();
    }

    if (!granted)
        return false;

    if (state.plugin_name.empty()) {
        state.installed_from    = descriptor.is_cloud_plugin() ? "cloud" : "local";
        state.installed_version = !descriptor.installed_version.empty() ? descriptor.installed_version : descriptor.version;
        state.plugin_name       = descriptor.name;
        state.cloud_uuid        = descriptor.cloud_uuid();
        state.enabled            = true;
    }

    state.permissions.fs_read.insert(state.permissions.fs_read.end(), missing.begin(), missing.end());
    return write_install_state(boost::filesystem::path(descriptor.plugin_root), state);
}

void PluginAuditManager::report_violation(const AuditViolation& violation)
{
    m_last_violation     = violation;
    m_has_last_violation = true;
    m_audit_denial_pending = true;

    BOOST_LOG_TRIVIAL(warning) << "[AUDIT BLOCKED] plugin=" << violation.plugin_key << " event=" << violation.event_name
                               << " reason=" << violation.reason;
}

bool PluginAuditManager::audit_denial_pending() const { return m_audit_denial_pending; }

void PluginAuditManager::clear_audit_denial() { m_audit_denial_pending = false; }

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

bool PluginAuditManager::has_approved_ancestor(const std::string&              plugin_key,
                                               const std::vector<std::string>& call_site_ids) const
{
    if (plugin_key.empty() || call_site_ids.empty())
        return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_approved_call_sites.find(plugin_key);
    if (it == m_approved_call_sites.end())
        return false;

    for (const auto& id : call_site_ids)
        if (it->second.count(id))
            return true;
    return false;
}

void PluginAuditManager::record_approved_call_sites(const std::string&              plugin_key,
                                                     const std::vector<std::string>& call_site_ids)
{
    if (plugin_key.empty() || call_site_ids.empty())
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    auto& approved = m_approved_call_sites[plugin_key];
    approved.insert(call_site_ids.begin(), call_site_ids.end());
}

// ---------------------------------------------------------------------------
// The C-level audit hook
// ---------------------------------------------------------------------------

namespace PluginAuditDetail {

PyObject* tuple_item(PyObject* args, Py_ssize_t index)
{
    if (!args || !PyTuple_Check(args) || index < 0 || index >= PyTuple_GET_SIZE(args))
        return nullptr;
    return PyTuple_GET_ITEM(args, index);
}

std::string python_path(PyObject* object)
{
    if (!object)
        return {};

    PyObject* path_object = PyOS_FSPath(object);
    if (!path_object) {
        PyErr_Clear();
        return {};
    }

    const char* path = PyUnicode_Check(path_object) ? PyUnicode_AsUTF8(path_object) : PyBytes_AsString(path_object);
    std::string result = path ? path : "";
    Py_DECREF(path_object);
    if (!path)
        PyErr_Clear();
    return result;
}

std::string python_unicode(PyObject* object)
{
    if (!object || !PyUnicode_Check(object))
        return {};
    const char* text = PyUnicode_AsUTF8(object);
    if (!text) {
        PyErr_Clear();
        return {};
    }
    return text;
}

std::string python_str(PyObject* object)
{
    if (!object)
        return {};

    PyObject* str_object = PyObject_Str(object);
    if (!str_object) {
        PyErr_Clear();
        return {};
    }

    const std::string result = python_unicode(str_object);
    Py_DECREF(str_object);
    return result;
}

std::vector<std::string> call_site_identities(const std::string& plugin_root)
{
    std::vector<std::string> ids;
    if (plugin_root.empty())
        return ids;

    PyFrameObject* frame = PyEval_GetFrame(); // borrowed reference
    Py_XINCREF(frame);                        // normalize to an owned reference for the loop below

    while (frame) {
        PyCodeObject* code     = PyFrame_GetCode(frame); // new reference
        const std::string filename = python_unicode(reinterpret_cast<PyObject*>(code->co_filename));
        const bool is_plugin_frame = !filename.empty() && boost::algorithm::starts_with(filename, plugin_root);

        std::string id;
        if (!is_plugin_frame && !filename.empty()) {
            const std::string funcname = python_unicode(reinterpret_cast<PyObject*>(code->co_name));
            id = filename + ":" + funcname + ":" + std::to_string(code->co_firstlineno);
        }
        Py_DECREF(code);

        PyFrameObject* back = PyFrame_GetBack(frame); // new reference, or nullptr at the top of the stack
        Py_DECREF(frame);
        frame = back;

        if (is_plugin_frame)
            break;
        if (!id.empty())
            ids.push_back(std::move(id));
    }

    Py_XDECREF(frame); // only holds a reference here if the loop exited via break

    return ids;
}

static const std::unordered_set<std::string> two_path_fs_events{
    "os.rename",       "os.link",          "os.symlink",         "shutil.copyfile",
    "shutil.copytree", "shutil.copymode",  "shutil.copystat",    "shutil.move",
    "shutil.unpack_archive", "_winapi.CreateJunction",
};

static const std::unordered_map<std::string, std::vector<Py_ssize_t>> audit_target_arg_indices{
    {"http.client.connect", {1}},
    {"urllib.Request", {0}},

    {"socket.connect", {1}},
    {"socket.bind", {1}},
    {"socket.getaddrinfo", {0}},
    {"socket.gethostbyname", {0}},
    {"socket.gethostbyaddr", {0}},
    {"socket.getnameinfo", {0}},
    {"socket.getservbyname", {0}},
    {"socket.getservbyport", {0}},

    {"os.system", {0}},
    {"subprocess.Popen", {1, 0}},
    {"os.posix_spawn", {1, 0}},
    {"os.spawn", {2, 1}},
    {"os.startfile", {0}},
    {"pty.spawn", {0}},
    {"_winapi.CreateProcess", {1, 0}},
    {"_posixsubprocess.fork_exec", {0}},
};

AuditEventCategory open_category(PyObject* args)
{
    const std::string mode = python_unicode(tuple_item(args, 1));
    if (!mode.empty() && mode.find_first_of("wax+") == std::string::npos)
        return AuditEventCategory::FsRead;
    return AuditEventCategory::FsReadWrite;
}

std::vector<std::string> audit_targets(const std::string& event_name, AuditEventCategory category, PyObject* args)
{
    std::vector<std::string> targets;

    switch (category) {
    case AuditEventCategory::FsRead:
    case AuditEventCategory::FsReadWrite:
    case AuditEventCategory::FsCreate:
    case AuditEventCategory::FsDelete: {
        const Py_ssize_t path_count = two_path_fs_events.count(event_name) ? 2 : 1;
        for (Py_ssize_t index = 0; index < path_count; ++index) {
            const std::string path = python_path(tuple_item(args, index));
            if (!path.empty())
                targets.push_back(path);
        }
        return targets;
    }
    default:
        break;
    }

    const auto it = audit_target_arg_indices.find(event_name);
    if (it != audit_target_arg_indices.end()) {
        for (const Py_ssize_t index : it->second) {
            std::string value = python_str(tuple_item(args, index));
            if (!value.empty()) {
                targets.push_back(std::move(value));
                break;
            }
        }
    }

    return targets;
}

std::vector<std::string>* permission_list_for(AuditEventCategory category, PluginPermissions& permissions)
{
    switch (category) {
    case AuditEventCategory::FsRead:        return &permissions.fs_read;
    case AuditEventCategory::FsReadWrite:   return &permissions.fs_readwrite;
    case AuditEventCategory::Http:          return &permissions.network_http;
    case AuditEventCategory::Socket:        return &permissions.network_socket;
    case AuditEventCategory::ProcessCreate: return &permissions.process;
    default:                                return nullptr;
    }
}

bool has_permission(const std::vector<std::string>& granted, const std::string& target)
{
    return std::find(granted.begin(), granted.end(), target) != granted.end();
}

bool persist_permission(const std::string&        plugin_key,
                        PluginInstallState&       state,
                        std::vector<std::string>& permission_list,
                        const std::string&        target)
{
    PluginDescriptor descriptor;
    if (!PluginManager::instance().try_get_plugin_descriptor(plugin_key, descriptor) || descriptor.plugin_root.empty())
        return false;

    // A sandbox plugin may not have a sidecar yet.  Seed the small amount of install metadata
    // needed by the writer so clicking Yes still creates the JSON file.
    if (state.plugin_name.empty()) {
        state.installed_from    = descriptor.is_cloud_plugin() ? "cloud" : "local";
        state.installed_version = !descriptor.installed_version.empty() ? descriptor.installed_version : descriptor.version;
        state.plugin_name       = descriptor.name;
        state.cloud_uuid        = descriptor.cloud_uuid();
        state.enabled            = true;
    }

    if (!has_permission(permission_list, target))
        permission_list.push_back(target);
    return write_install_state(boost::filesystem::path(descriptor.plugin_root), state);
}

int report_denied(PluginAuditManager&            mgr,
                  const std::string&             event_name,
                  const AuditDecision&           decision)
{
    AuditViolation violation;
    violation.plugin_key = mgr.current_plugin();
    violation.event_name = event_name;
    violation.reason     = decision.reason;
    mgr.report_violation(violation);

    PyErr_SetString(PyExc_PermissionError, "Plugin attempted an audited operation without permission");
    return -1;
}

wxString audit_message(AuditEventCategory category, const wxString& plugin_name, const wxString& event_name,
                       const wxString& target_list)
{
    if (target_list.IsEmpty())
        return wxString::Format(
            _L("Plugin \"%s\" is requesting permission for the Python audit event \"%s\".\n\n"
                "This operation does not expose a target the audit hook can display."),
            plugin_name, event_name);

    switch (category) {
    case AuditEventCategory::FsRead:
        return wxString::Format(_L("Plugin \"%s\" is requesting to read the following file(s):\n%s"), plugin_name, target_list);
    case AuditEventCategory::FsReadWrite:
        return wxString::Format(_L("Plugin \"%s\" is requesting to read/write the following file(s):\n%s"), plugin_name, target_list);
    case AuditEventCategory::FsCreate:
        return wxString::Format(_L("Plugin \"%s\" is requesting to create the following file(s):\n%s"), plugin_name, target_list);
    case AuditEventCategory::FsDelete:
        return wxString::Format(_L("Plugin \"%s\" is requesting to delete the following file(s):\n%s"), plugin_name, target_list);
    case AuditEventCategory::Http:
        return wxString::Format(_L("Plugin \"%s\" is requesting to make an HTTP request to:\n%s"), plugin_name, target_list);
    case AuditEventCategory::Socket:
        return wxString::Format(_L("Plugin \"%s\" is requesting to open a network connection to:\n%s"), plugin_name, target_list);
    case AuditEventCategory::ProcessCreate:
        return wxString::Format(_L("Plugin \"%s\" is requesting to run the following command(s):\n%s"), plugin_name, target_list);
    default:
        return wxString::Format(_L("Plugin \"%s\" is requesting permission for the Python audit event \"%s\"."), plugin_name, event_name);
    }
}

int decide_audited_event(PluginAuditManager&             mgr,
                         PluginInstallState&              state,
                         const std::string&                plugin_key,
                         const std::string&                plugin_name,
                         const std::string&                event_name,
                         AuditEventCategory                category,
                         const std::vector<std::string>&   targets,
                         std::vector<std::string>*         permission_list,
                         const std::vector<std::string>&   call_site_ids)
{
    std::vector<std::string> unresolved = targets;
    if (permission_list) {
        unresolved.clear();
        for (const auto& target : targets)
            if (!has_permission(*permission_list, target))
                unresolved.push_back(target);
        if (!targets.empty() && unresolved.empty())
            return 0;
    }

    wxString target_list;
    for (const auto& target : unresolved)
        target_list += wxString::FromUTF8(target.c_str()) + "\n";

    wxMessageDialog dialog(nullptr,
                           audit_message(category, wxString::FromUTF8(plugin_name.c_str()),
                                        wxString::FromUTF8(event_name.c_str()), target_list),
                           _L("Plugin permission request"), wxYES_NO | wxICON_WARNING);
    if (dialog.ShowModal() != wxID_YES)
        return report_denied(mgr, event_name, {false, "audit permission required"});

    if (permission_list)
        for (const auto& target : unresolved)
            persist_permission(plugin_key, state, *permission_list, target);

    mgr.record_approved_call_sites(plugin_key, call_site_ids);
    return 0;
}

} // namespace PluginAuditDetail

int PluginAuditManager::audit_hook(const char* event, PyObject* args, void* user_data)
{
    auto* mgr = static_cast<PluginAuditManager*>(user_data);
    if (!mgr)
        return 0;

    std::string event_name(event ? event : "");

    if (event_name.empty())
        return 0;

    // Verbose logging of every audit event (can be noisy)
    if (mgr->verbose_events) {
        BOOST_LOG_TRIVIAL(debug) << "[AUDIT EVENT] " << event_name;
    }

    if (mgr->current_plugin().empty()) {
        BOOST_LOG_TRIVIAL(trace) << "[AUDIT] event=" << event_name << " bypassed (no plugin context)";
        return 0;
    }

    // the open function can take in different flags that determine if it is a read or readwrite.
    const AuditEventCategory event_type =
        event_name == "open" ? PluginAuditDetail::open_category(args) : event_category(event_name);
    if (event_type == AuditEventCategory::None)
        return 0;

    const bool                      fs_category = is_fs_category(event_type);
    const std::vector<std::string>  targets      = PluginAuditDetail::audit_targets(event_name, event_type, args);

    // A denied path (secrets, certificates, config files -- see is_denied_path) is an
    // unconditional block: checked before the ancestor-cascade check below, so a cascade
    // approval recorded for an unrelated action can never launder access to one, and before
    // the allowed-root shortcut further down, so an allowed root (e.g. the read-only resources
    // folder) cannot make a denied path underneath it reachable.
    if (fs_category) {
        for (const auto& target : targets) {
            if (mgr->is_denied_path(boost::filesystem::path(target)))
                return PluginAuditDetail::report_denied(*mgr, event_name, {false, "denied path"});
        }
    }

    PluginDescriptor plugin_descriptor;
    PluginManager::instance().try_get_plugin_descriptor(mgr->current_plugin(), plugin_descriptor);

    // Some plugins call other audited events, e.g. urlib.request will call socket.connect. So this is so that if urlib.request
    // was already approved, socket.connect won't trigger another permission dialog.
    const std::vector<std::string> call_site_ids =
        PluginAuditDetail::call_site_identities(plugin_descriptor.plugin_root);
    if (mgr->has_approved_ancestor(mgr->current_plugin(), call_site_ids))
        return 0;

    // A filesystem target that resolves entirely inside a pre-determined allowed root -- the
    // plugin system's own data_dir() tree (which holds each plugin's storage folder and the
    // installed/system profile cache), the read-only bundled resources folder, or a per-call
    // scoped root such as the current G-code folder -- is part of the plugin system's normal
    // workflow and does not need a prompt.
    if (fs_category && !targets.empty()) {
        const bool is_write = event_type != AuditEventCategory::FsRead;
        const bool all_inside_allowed_root =
            std::all_of(targets.begin(), targets.end(), [&](const std::string& target) {
                return mgr->check_path_access(boost::filesystem::path(target), is_write).allowed;
            });
        if (all_inside_allowed_root)
            return 0;
    }

    PluginInstallState state;
    const bool have_install_state = PluginManager::instance().get_install_state(mgr->current_plugin(), state);
    if (!have_install_state) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << " Failed to get install state for " << mgr->current_plugin();
    }

    const std::string plugin_name = state.plugin_name.empty() ? mgr->current_plugin() : state.plugin_name;

    std::vector<std::string>* permission_list = PluginAuditDetail::permission_list_for(event_type, state.permissions);

    return PluginAuditDetail::decide_audited_event(*mgr, state, mgr->current_plugin(), plugin_name, event_name,
                                                    event_type, targets, permission_list, call_site_ids);
}

void PluginAuditManager::install_hook()
{
    // data_dir() is the primary globally-allowed root during plugin execution: read+write. It
    // covers the plugin system's own workflow needs -- each plugin's storage folder
    // (data_dir()/orca_plugins) and the installed/system profile cache (data_dir()/system) --
    // without a separate, narrower grant for either (G-code plugins additionally get the temp
    // G-code folder via a scoped root, see SlicingPipelinePluginCapabilityTrampoline).
    add_global_allowed_root(data_dir());

    // resources_dir() holds the app's bundled, shared assets (installed system profiles, the
    // bundled TLS client cert, web assets). Plugins may read from it -- e.g. inspecting bundled
    // profiles -- but must never write into the shared, potentially multi-user app install, so
    // it is granted read-only. The bundled cert itself stays unreachable regardless, via the
    // "cert" denied-path keyword seeded below.
    add_global_allowed_root(resources_dir(), /*allow_write=*/false);

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

    // Categorical denies on top of the exact-name list above: no path a plugin can reach may
    // contain a "secret", "cert"(ificate), or "conf"(ig) path component, regardless of which
    // allowed root it happens to sit inside. default_denied_path_keywords() is the single
    // source of this list; the tests seed from it too.
    for (const auto& keyword : default_denied_path_keywords())
        add_denied_path_keyword(keyword);

    if (PySys_AddAuditHook(audit_hook, this) < 0) {
        BOOST_LOG_TRIVIAL(error) << "[AUDIT] Failed to install CPython audit hook";
        return;
    }
    BOOST_LOG_TRIVIAL(info) << "[AUDIT] CPython audit hook installed successfully";
}

} // namespace Slic3r
