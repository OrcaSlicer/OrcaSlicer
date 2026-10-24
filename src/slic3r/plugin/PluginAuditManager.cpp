#include "PluginAuditManager.hpp"

#include "../Utils/OrcaCloudServiceAgent.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h" // GCODEVIEWER_APP_KEY, and SLIC3R_APP_KEY via libslic3r_version.h
#include "libslic3r_version.h"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem/file_status.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <mutex>
#include <memory>
#include <slic3r/GUI/BindDialog.hpp>
#include <slic3r/GUI/GUI_App.hpp>
#include <slic3r/plugin/PluginFsUtils.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <wx/app.h>
#include <wx/event.h>
#include <wx/msgdlg.h>
#include <wx/string.h>
#include <wx/thread.h>
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/plugin/PluginDescriptor.hpp"
#include <Python.h>

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

    // threading
    {"_thread.start_new_thread", AuditEventCategory::Threading},
    // processreplace: exec* replaces the current process image rather than spawning a child
    {"os.exec", AuditEventCategory::ProcessReplace},
};

// Returns the category event_name belongs to, or AuditEventCategory::None when it isn't audited.
static AuditEventCategory event_category(const std::string& event_name)
{
    const auto it = audit_event_categories.find(event_name);
    return it == audit_event_categories.end() ? AuditEventCategory::None : it->second;
}

// Events the 3.12 stdlib raises with sys.audit() inside the function that performs the whole
// operation, so the raising frame bounds everything the operation does. Every other mapped event
// is a primitive raised from C, whose current frame is whatever Python code called it -- possibly
// a library loop over many targets -- so it never marks an operation. glob.glob and glob.glob/2
// are raised in iglob(), which returns its lazy generator before any directory is listed.
static const std::unordered_set<std::string> operation_root_events{
    "urllib.Request",      "http.client.connect",   "os.walk",          "os.fwalk",
    "glob.glob",           "glob.glob/2",           "pathlib.Path.glob", "pathlib.Path.rglob",
    "subprocess.Popen",    "pty.spawn",             "tempfile.mkstemp", "tempfile.mkdtemp",
    "shutil.copyfile",     "shutil.copymode",       "shutil.copystat",  "shutil.copytree",
    "shutil.chown",        "shutil.move",           "shutil.rmtree",    "shutil.make_archive",
    "shutil.unpack_archive",
};

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

// The category's name in a saved event grant, e.g. "FsRead" in "FsRead:open".
static const char* category_name(AuditEventCategory category)
{
    switch (category) {
    case AuditEventCategory::FsRead:         return "FsRead";
    case AuditEventCategory::FsReadWrite:    return "FsReadWrite";
    case AuditEventCategory::FsCreate:       return "FsCreate";
    case AuditEventCategory::FsDelete:       return "FsDelete";
    case AuditEventCategory::Http:           return "Http";
    case AuditEventCategory::Socket:         return "Socket";
    case AuditEventCategory::ProcessCreate:  return "ProcessCreate";
    case AuditEventCategory::ProcessReplace: return "ProcessReplace";
    case AuditEventCategory::Threading:      return "Threading";
    case AuditEventCategory::None:           break;
    }
    return "None";
}

// ---------------------------------------------------------------------------
// Path safety
// ---------------------------------------------------------------------------

namespace {

// Canonicalize a path for containment comparison.  weakly_canonical resolves symlinks but does
// NOT require the path to exist — it canonicalizes the prefix that exists and appends the
// non-existing tail lexically.
boost::filesystem::path canonical_for_comparison(const boost::filesystem::path& path)
{
    namespace fs = boost::filesystem;

    boost::system::error_code ec;
    fs::path canon = fs::weakly_canonical(path, ec);
    if (ec) {
        // Fall back to lexically_normal + absolute
        canon = fs::absolute(path, ec).lexically_normal();
        if (ec)
            canon = path;
    }
    return canon;
}

// True when the final component of `path` is a symlink whose target does not exist.
//
// weakly_canonical() leaves such a link in its lexical tail: it stats the prefix and stops at the
// first non-existing component, and exists() follows the link and reports false for a dangling
// one. A target such as <tree>/link would therefore compare as if it lived inside the tree even
// though a write through it lands at the link's target, which the grant does not describe.
//
// A link whose target exists is not dangling: symlink_status() reports the link itself (it does
// not follow it) while exists() follows it and succeeds. The canonical form of such a candidate is
// already resolved to the target, so this check does not fire for it. An intermediate dangling
// component is not reported here and needs no special case: the open cannot follow it either, so
// nothing is created.
bool ends_in_dangling_symlink(const boost::filesystem::path& path)
{
    namespace fs = boost::filesystem;

    boost::system::error_code ec;
    const fs::file_status status = fs::symlink_status(path, ec);
    // symlink_status does not follow the link, so a missing target still reports symlink; a path
    // that does not exist at all reports an error instead.
    if (ec || !fs::is_symlink(status))
        return false;

    // exists() follows the link: false means the target is missing.
    return !fs::exists(path, ec);
}

bool same_path_component(const boost::filesystem::path& lhs, const boost::filesystem::path& rhs)
{
#ifdef _WIN32
    return boost::algorithm::iequals(lhs.native(), rhs.native());
#else
    return lhs == rhs;
#endif
}

// Component-wise containment: the whole of canonical_root must be a prefix of
// canonical_candidate, and the remaining components must not traverse upward. Both arguments are
// expected to come from canonical_for_comparison(). On a match, suffix receives the candidate's
// components below the root (empty when the two paths are equal).
bool suffix_inside_root(const boost::filesystem::path& canonical_candidate,
                        const boost::filesystem::path& canonical_root,
                        boost::filesystem::path&       suffix)
{
    suffix.clear();

    auto cand_it  = canonical_candidate.begin();
    auto cand_end = canonical_candidate.end();
    auto root_it  = canonical_root.begin();
    auto root_end = canonical_root.end();

    // Consume matching components
    while (root_it != root_end && cand_it != cand_end && same_path_component(*root_it, *cand_it)) {
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

    for (; cand_it != cand_end; ++cand_it)
        suffix /= *cand_it;
    return true;
}

} // namespace

bool is_inside_allowed_root(const boost::filesystem::path& candidate, const boost::filesystem::path& allowed_root)
{
    const boost::filesystem::path canonical_candidate = canonical_for_comparison(candidate);

    // Fail closed on a dangling final symlink instead of comparing the path it is spelled as: the
    // grant describes the link, not the destination a write through it would create. The test is
    // applied to the canonical form, which leaves a dangling link unresolved as its final
    // component, so a trailing separator does not hide it either.
    if (ends_in_dangling_symlink(canonical_candidate))
        return false;

    boost::filesystem::path suffix;
    return suffix_inside_root(canonical_candidate, canonical_for_comparison(allowed_root), suffix);
}

// ---------------------------------------------------------------------------
// ScopedPluginAuditContext
// ---------------------------------------------------------------------------

thread_local std::string PluginAuditManager::m_current_plugin_key = "";
thread_local std::string PluginAuditManager::m_current_capability_name = "";
thread_local std::vector<AllowedRoot> PluginAuditManager::m_scoped_allowed_roots;
thread_local std::vector<PyObject*> PluginAuditManager::m_root_frames;
thread_local int PluginAuditManager::m_releasing_frames = 0;
thread_local bool PluginAuditManager::m_audit_denial_pending = false;
thread_local bool PluginAuditManager::m_has_last_violation = false;
thread_local AuditViolation PluginAuditManager::m_last_violation;

ScopedPluginAuditContext::ScopedPluginAuditContext(const std::string& plugin_key,
                                                   const std::string& capability_name)
    : m_plugin_key(plugin_key)
    , m_capability(capability_name)
    , m_previous_id(PluginAuditManager::instance().current_plugin())
    , m_previous_capability(PluginAuditManager::instance().current_capability())
    , m_previous_scoped_roots(PluginAuditManager::m_scoped_allowed_roots)
    , m_previous_root_frames(std::move(PluginAuditManager::m_root_frames))
{
    PluginAuditManager::instance().set_current_plugin(plugin_key);
    PluginAuditManager::instance().set_current_capability(capability_name);
    PluginAuditManager::m_scoped_allowed_roots.clear();
    PluginAuditManager::m_root_frames.clear();
}

ScopedPluginAuditContext::~ScopedPluginAuditContext()
{
    PluginAuditManager& mgr = PluginAuditManager::instance();

    // Let this scope's root frames go while it is still the current context, and as its own
    // plugin: the trampoline's denial path clears the key before the scope ends, and a finalizer
    // must never be judged as the outer plugin, or bypass the audit when there is none. A
    // finalizer may record a new root frame, so repeat until none is held.
    if (!PluginAuditManager::m_root_frames.empty()) {
        assert(PyGILState_Check());
        mgr.set_current_plugin(m_plugin_key);
        mgr.set_current_capability(m_capability);
        while (!PluginAuditManager::m_root_frames.empty()) {
            std::vector<PyObject*> frames = std::move(PluginAuditManager::m_root_frames);
            PluginAuditManager::m_root_frames.clear();
            mgr.release_frames(frames);
        }
    }

    mgr.set_current_plugin(m_previous_id);
    mgr.set_current_capability(m_previous_capability);
    PluginAuditManager::m_scoped_allowed_roots = std::move(m_previous_scoped_roots);
    PluginAuditManager::m_root_frames          = std::move(m_previous_root_frames);
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
    // on its mode, and is a non-static member we cannot call without an instance. Denying all
    // four names is cheaper and more robust than replicating that; the two unused names cost one
    // string comparison each. Keep the plugin install-state sidecar here too: it controls which
    // packages load on startup and must remain host-managed. Single-sourced so install_hook() and
    // the tests seed from the same list and cannot drift apart.
    return {
        SLIC3R_APP_KEY ".conf",
        GCODEVIEWER_APP_KEY ".conf",
        SLIC3R_APP_KEY ".ini",
        GCODEVIEWER_APP_KEY ".ini",
        secret_constants::USER_SECRET_FILENAME,
        ".install_state.json",
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
    // granted as a read-only allowed root). Match as whole path components, not substrings, so
    // imports such as numpy/__config__.py and stdlib configparser.py remain usable. Inside
    // data_dir() they match below that root only; see is_denied_path_keyword().
    return {"secret", "secrets", "cert", "certs", "certificate", "certificates", "conf", "config"};
}

static bool has_denied_config_extension(std::string name)
{
    const size_t stream_pos = name.find(':');
    if (stream_pos != std::string::npos)
        name.erase(stream_pos);

    const boost::filesystem::path path(name);
    const std::string extension = path.extension().string();
    return extension == ".conf" || extension == ".ini";
}

bool PluginAuditManager::is_denied_path_keyword(const boost::filesystem::path& candidate) const
{
    namespace fs = boost::filesystem;

    const fs::path canon = canonical_for_comparison(candidate);

    // The app's data folder can itself sit below a component named like one of the keywords
    // without being one: Flatpak keeps it under <home>/.var/app/<app-id>/config/OrcaSlicer, and
    // the sandbox's home layout is not ours to change.  A keyword above data_dir() therefore says
    // nothing about what a plugin may reach inside it, so inside data_dir() the keywords apply to
    // the suffix below data_dir() only.  Everywhere else -- resources_dir() with the bundled
    // certificates, a plugin's own folders, arbitrary files -- the whole path is still checked,
    // and so is a keyword under data_dir() itself.
    fs::path canonical_data_dir;
    if (!data_dir().empty())
        canonical_data_dir = canonical_for_comparison(data_dir());

    fs::path scope = canon;
    fs::path suffix;
    if (!canonical_data_dir.empty() && suffix_inside_root(canon, canonical_data_dir, suffix))
        scope = suffix;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_denied_path_keywords.empty())
        return false;

    for (const auto& component : scope) {
        std::string name = component.string();
        if (name.empty())
            continue;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
        for (const auto& keyword : m_denied_path_keywords) {
            if (name == keyword || (keyword == "conf" && has_denied_config_extension(name)))
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

    // Importing Python modules reads their source and may inspect package directories. These
    // reads are normal interpreter work, not plugin data access; keep the exemption read-only
    // and exclude third-party packages from the stdlib roots.
    if (!is_write && is_stdlib_path(candidate))
        return {true, ""};

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

// The interpreter's stdlib folders, and the site-packages folders inside them that hold
// third-party code, both resolved. Set by install_hook(); like the verdict cache below, only
// touched while the GIL is held.
static std::vector<boost::filesystem::path>   s_stdlib_dirs;
static std::vector<boost::filesystem::path>   s_site_package_dirs;
static std::unordered_map<std::string, bool>  s_stdlib_file_verdicts; // co_filename -> is stdlib

void collect_stdlib_dirs()
{
    s_stdlib_dirs.clear();
    s_site_package_dirs.clear();
    s_stdlib_file_verdicts.clear();

    PyObject* sysconfig = PyImport_ImportModule("sysconfig");
    PyObject* paths     = sysconfig ? PyObject_CallMethod(sysconfig, "get_paths", nullptr) : nullptr;
    if (paths && PyDict_Check(paths)) {
        const auto collect = [paths](const char* key, std::vector<boost::filesystem::path>& out) {
            const std::string dir = python_unicode(PyDict_GetItemString(paths, key)); // borrowed
            if (dir.empty())
                return;
            boost::system::error_code ec;
            const boost::filesystem::path resolved = boost::filesystem::weakly_canonical(dir, ec);
            out.push_back(ec ? boost::filesystem::path(dir) : resolved);
        };
        collect("stdlib", s_stdlib_dirs);
        collect("platstdlib", s_stdlib_dirs);
        collect("purelib", s_site_package_dirs);
        collect("platlib", s_site_package_dirs);
    }
    Py_XDECREF(paths);
    Py_XDECREF(sysconfig);
    if (PyErr_Occurred()) {
        PyErr_Clear();
        BOOST_LOG_TRIVIAL(error) << "[AUDIT] Failed to read the stdlib folders from sysconfig";
    }
}

bool is_stdlib_frame(PyObject* frame)
{
    PyCodeObject* code = PyFrame_GetCode(reinterpret_cast<PyFrameObject*>(frame)); // new reference
    const std::string filename = python_unicode(code->co_filename);
    Py_DECREF(code);
    return PluginAuditManager::is_stdlib_file(filename);
}

// The frames on this thread's Python stack, innermost first. The pointers stay valid while the
// caller runs, because a running activation keeps its frame object alive.
std::vector<PyObject*> stack_frames()
{
    std::vector<PyObject*> frames;
    PyFrameObject* frame = PyEval_GetFrame(); // borrowed reference
    Py_XINCREF(frame);                        // normalize to an owned reference for the loop below
    while (frame) {
        frames.push_back(reinterpret_cast<PyObject*>(frame));
        PyFrameObject* back = PyFrame_GetBack(frame); // new reference, or nullptr at the top of the stack
        Py_DECREF(frame);
        frame = back;
    }
    return frames;
}

// Moves the held root frames whose operation has returned from `held` to `dropped`. A suspended
// generator's frame (os.walk between two items) is off the stack, but its operation resumes, so
// it stays held until the scope ends.
void take_returned_frames(std::vector<PyObject*>& held, std::vector<PyObject*>& dropped)
{
    if (held.empty())
        return;

    const std::vector<PyObject*> stack = stack_frames();
    const auto running = [&stack](PyObject* frame) {
        if (std::find(stack.begin(), stack.end(), frame) != stack.end())
            return true;
        // NULL once the generator has finished or been deleted: its frame then owns its own data.
        PyObject* generator = PyFrame_GetGenerator(reinterpret_cast<PyFrameObject*>(frame)); // new reference
        const bool suspended = generator != nullptr;
        Py_XDECREF(generator);
        return suspended;
    };
    const auto returned = std::stable_partition(held.begin(), held.end(), running);
    dropped.insert(dropped.end(), returned, held.end());
    held.erase(returned, held.end());
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
    {"socket.sendto", {1}},
    {"socket.sendmsg", {1}},

    {"os.system", {0}},
    {"subprocess.Popen", {1, 0}},
    {"os.posix_spawn", {1, 0}},
    {"os.spawn", {2, 1}},
    {"os.startfile", {0}},
    {"os.startfile/2", {0}},
    {"pty.spawn", {0}},
    {"_winapi.CreateProcess", {1, 0}},
    {"_posixsubprocess.fork_exec", {0}},
    {"os.exec", {0}},
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
    case AuditEventCategory::Threading:
        // Thread creation exposes no user-supplied target. Use a fixed sentinel so the grant
        // persists per plugin: the permission list matches targets by exact string, and the
        // started function's repr embeds an address that changes every run.
        targets.emplace_back(kThreadPermissionTarget);
        return targets;
    default:
        break;
    }

    const auto it = audit_target_arg_indices.find(event_name);
    if (it != audit_target_arg_indices.end()) {
        for (const Py_ssize_t index : it->second) {
            // None is an argument left out (sendmsg without an address), not a target named "None".
            PyObject* const item = tuple_item(args, index);
            if (item == Py_None)
                continue;
            std::string value = python_str(item);
            if (!value.empty()) {
                targets.push_back(std::move(value));
                break;
            }
        }
    }

    return targets;
}

// The saved list that grants an event: its category's target list, or the event-grant list for
// an event with no target. Kept apart, so that no grant doubles as a target of the same name (an
// os.fork grant is not an os.exec of a command named "os.fork").
std::vector<std::string>* permission_list_for(AuditEventCategory category, bool by_event, PluginPermissions& permissions)
{
    if (by_event)
        return &permissions.events;

    switch (category) {
    case AuditEventCategory::FsRead:        return &permissions.fs_read;
    case AuditEventCategory::FsReadWrite:   return &permissions.fs_readwrite;
    case AuditEventCategory::Http:          return &permissions.network_http;
    case AuditEventCategory::Socket:        return &permissions.network_socket;
    case AuditEventCategory::ProcessCreate: return &permissions.process;
    case AuditEventCategory::Threading:     return &permissions.threading;
    case AuditEventCategory::ProcessReplace: return &permissions.process;
    default:                                return nullptr;
    }
}

// An event with no target is granted by its effective category and name, e.g. "FsRead:open"
// for a read of a file descriptor, which does not grant a write of one ("FsReadWrite:open").
std::string event_grant_key(AuditEventCategory category, const std::string& event_name)
{
    return std::string(category_name(category)) + ":" + event_name;
}

// The directory named by a directory-tree grant, i.e. "<dir>/*". "*" is the only wildcard, and
// only as the grant's final path component; any other "*" is an ordinary character, so a grant
// can never widen into a pattern the plugin did not spell out. Returns false for a grant that is
// not a tree grant.
bool tree_grant_root(const std::string& grant, boost::filesystem::path& root)
{
    if (grant.size() < 2 || grant.back() != '*' || grant.find('*') != grant.size() - 1)
        return false;

    const boost::filesystem::path path(grant);
    if (path.filename() != "*")
        return false;

    const boost::filesystem::path parent = path.parent_path();
    if (parent.empty())
        return false;

    // Never resolve ".." here: a grant shown to the user as "/a/../*" must not silently become
    // the root or another ancestor. Without a wildcard the string still matches verbatim.
    for (const boost::filesystem::path& component : parent) {
        if (component == "..")
            return false;
    }

    // Rebuild from the components so "." and duplicate separators do not affect the canonical
    // comparison below.
    boost::filesystem::path normalized;
    for (const boost::filesystem::path& component : parent.lexically_normal()) {
        if (!component.empty())
            normalized /= component;
    }
    root = std::move(normalized);
    return !root.empty();
}

// The grant to display and store for a permission request: a recognized tree grant is resolved
// against the working directory when the request is made, so a relative "<dir>/*" names the
// directory it actually covers. Grants that are already absolute, exact (non-tree) grants, and
// the non-filesystem buckets are returned unchanged: they are matched verbatim, and re-rendering
// them would change the string the user approved.
std::string absolute_tree_grant(const std::string& grant)
{
    boost::filesystem::path root;
    if (!tree_grant_root(grant, root) || root.is_absolute())
        return grant;

    boost::system::error_code ec;
    const boost::filesystem::path absolute_root = boost::filesystem::absolute(root, ec);
    if (ec || !absolute_root.is_absolute())
        return grant;

    return (absolute_root.lexically_normal() / "*").string();
}

// True when a filesystem grant covers a target: the exact string, or -- for a tree grant -- the
// granted folder itself and every path beneath it, at any depth. Components are compared one by
// one, so "/root/*" covers "/root/a/b" but not "/root-other/file".
//
// A tree grant and its target are both canonicalized before the comparison, so a symlink inside
// the granted folder cannot launder access to a path outside it, and a grant rooted at a symlink
// applies to the symlink's target. weakly_canonical keeps a write target whose tail does not
// exist yet matchable: it canonicalizes the existing prefix and appends the rest lexically. A
// dangling final symlink is not covered at all: containment fails closed, because a write through
// it would land at its target rather than at the granted path.
bool fs_grant_covers(const std::string& grant, const std::string& target)
{
    if (grant == target)
        return true;
    if (target.empty())
        return false;

    boost::filesystem::path root;
    if (!tree_grant_root(grant, root))
        return false;

    return is_inside_allowed_root(boost::filesystem::path(target), root);
}

// True when `granted` already covers `target`. Only the filesystem buckets accept a tree grant:
// an event grant ("FsRead:open") and the non-filesystem buckets (URLs, socket addresses,
// commands) are matched verbatim, as before.
bool has_permission(AuditEventCategory category, bool by_event, const std::vector<std::string>& granted,
                    const std::string& target)
{
    const bool fs_bucket =
        !by_event && (category == AuditEventCategory::FsRead || category == AuditEventCategory::FsReadWrite);
    for (const auto& grant : granted) {
        if (grant == target)
            return true;
        if (fs_bucket && fs_grant_covers(grant, target))
            return true;
    }
    return false;
}

// A sandbox plugin may not have a sidecar yet. Seed the small amount of install metadata the
// writer needs so clicking Yes still creates the JSON file.
void seed_install_metadata(const PluginDescriptor& descriptor, PluginInstallState& state)
{
    if (!state.plugin_name.empty())
        return;

    state.installed_from    = descriptor.is_cloud_plugin() ? "cloud" : "local";
    state.installed_version = !descriptor.installed_version.empty() ? descriptor.installed_version : descriptor.version;
    state.plugin_name       = descriptor.name;
    state.cloud_uuid        = descriptor.cloud_uuid();
    state.enabled           = true;
}

bool persist_permission(const std::string&        plugin_key,
                        PluginInstallState&       state,
                        AuditEventCategory        category,
                        bool                      by_event,
                        std::vector<std::string>& permission_list,
                        const std::string&        target)
{
    PluginDescriptor descriptor;
    if (!PluginManager::instance().try_get_plugin_descriptor(plugin_key, descriptor) || descriptor.plugin_root.empty())
        return false;

    seed_install_metadata(descriptor, state);

    if (!has_permission(category, by_event, permission_list, target))
        permission_list.push_back(target);
    return write_install_state(boost::filesystem::path(descriptor.plugin_root), state);
}

int report_denied(PluginAuditManager&            mgr,
                  const std::string&             event_name,
                  const AuditDecision&           decision,
                  const std::string&             target = {})
{
    AuditViolation violation;
    violation.plugin_key = mgr.current_plugin();
    violation.event_name = event_name;
    violation.reason     = decision.reason;
    mgr.report_violation(violation);

    std::string message = "Plugin attempted audited operation \"" + event_name + "\" without permission";
    if (!decision.reason.empty())
        message += ": " + decision.reason;
    if (!target.empty())
        message += ": " + target;

    PyErr_SetString(PyExc_PermissionError, message.c_str());
    return -1;
}

wxString audit_message(AuditEventCategory category, const wxString& plugin_name, const wxString& event_name,
                       const wxString& target_list, size_t target_count)
{
    if (target_list.IsEmpty())
        return wxString::Format(
            _L("Plugin \"%s\" is requesting permission for the Python audit event \"%s\".\n\n"
                "This operation does not expose a target the audit hook can display."),
            plugin_name, event_name);

    switch (category) {
    case AuditEventCategory::FsRead:
        return wxString::Format(_L_PLURAL("Plugin \"%s\" is requesting to read the following file:\n%s",
                                          "Plugin \"%s\" is requesting to read the following files:\n%s", target_count),
                                plugin_name, target_list);
    case AuditEventCategory::FsReadWrite:
        return wxString::Format(_L_PLURAL("Plugin \"%s\" is requesting to read/write the following file:\n%s",
                                          "Plugin \"%s\" is requesting to read/write the following files:\n%s", target_count),
                                plugin_name, target_list);
    case AuditEventCategory::FsCreate:
        return wxString::Format(_L_PLURAL("Plugin \"%s\" is requesting to create the following file:\n%s",
                                          "Plugin \"%s\" is requesting to create the following files:\n%s", target_count),
                                plugin_name, target_list);
    case AuditEventCategory::FsDelete:
        return wxString::Format(_L_PLURAL("Plugin \"%s\" is requesting to delete the following file:\n%s",
                                          "Plugin \"%s\" is requesting to delete the following files:\n%s", target_count),
                                plugin_name, target_list);
    case AuditEventCategory::Http:
        return wxString::Format(_L("Plugin \"%s\" is requesting to make an HTTP request to:\n%s"), plugin_name, target_list);
    case AuditEventCategory::Socket:
        return wxString::Format(_L("Plugin \"%s\" is requesting to open a network connection to:\n%s"), plugin_name, target_list);
    case AuditEventCategory::ProcessCreate:
        return wxString::Format(_L_PLURAL("Plugin \"%s\" is requesting to run the following command:\n%s",
                                          "Plugin \"%s\" is requesting to run the following commands:\n%s", target_count),
                                plugin_name, target_list);
    case AuditEventCategory::Threading:
        return wxString::Format(_L("Plugin \"%s\" is requesting permission to create a thread."), plugin_name);
    case AuditEventCategory::ProcessReplace:
        return wxString::Format(_L("Plugin \"%s\" is requesting to replace the running application with:\n%s"), plugin_name, target_list);
    default:
        return wxString::Format(_L("Plugin \"%s\" is requesting permission for the Python audit event \"%s\"."), plugin_name, event_name);
    }
}

// Builds and shows the modal permission prompt. Must run on the GUI thread.
bool prompt_for_targets(AuditEventCategory category, const std::string& plugin_name, const std::string& event_name,
                        const std::vector<std::string>& unresolved)
{
    wxString target_list;
    for (const auto& target : unresolved)
        target_list += wxString::FromUTF8(target.c_str()) + "\n";

    wxMessageDialog dialog(nullptr,
                           audit_message(category, wxString::FromUTF8(plugin_name.c_str()),
                                        wxString::FromUTF8(event_name.c_str()), target_list, unresolved.size()),
                           _L("Plugin permission request"), wxYES_NO | wxICON_WARNING);
    return dialog.ShowModal() == wxID_YES;
}

// Shows a prompt whose body may cover several permission buckets. Must run on the GUI thread.
bool prompt_for_permission_message(const wxString& message)
{
    wxMessageDialog dialog(nullptr, message, _L("Plugin permissions"), wxYES_NO | wxICON_WARNING);
    return dialog.ShowModal() == wxID_YES;
}

// Records a grant in the plugin's sidecar so it is not asked again. Reads the install state
// freshly because the async prompt outlives the caller's stack copy of it.
void persist_grant(const std::string& plugin_key, AuditEventCategory category, bool by_event, const std::vector<std::string>& keys)
{
    PluginInstallState state;
    if (!PluginManager::instance().get_install_state(plugin_key, state))
        return;

    std::vector<std::string>* permission_list = permission_list_for(category, by_event, state.permissions);
    if (!permission_list)
        return;

    for (const auto& key : keys)
        persist_permission(plugin_key, state, category, by_event, *permission_list, key);
}

static PluginAuditManager::PermissionPrompt s_permission_prompt_for_testing;
static PluginAuditManager::PermissionMessagePrompt s_permission_message_prompt_for_testing;

static const char* audit_category_name(AuditEventCategory category)
{
    switch (category) {
    case AuditEventCategory::FsRead: return "FsRead";
    case AuditEventCategory::FsReadWrite: return "FsReadWrite";
    case AuditEventCategory::FsCreate: return "FsCreate";
    case AuditEventCategory::FsDelete: return "FsDelete";
    case AuditEventCategory::Http: return "Http";
    case AuditEventCategory::Socket: return "Socket";
    case AuditEventCategory::ProcessCreate: return "ProcessCreate";
    case AuditEventCategory::ProcessReplace: return "ProcessReplace";
    case AuditEventCategory::Threading: return "Threading";
    default: return "None";
    }
}

// Asks the user to grant an audited event, returning true when they approve an inline prompt.
// `keys` are what a Yes saves: the unresolved targets, or the event-grant key when the event has
// no target (by_event), which the prompt does not show as a target.
//
// An audited event can fire on a thread the UI thread may itself be blocked waiting on: the
// SlicingPipeline hook runs on the slicing worker thread (see PluginHooks.cpp), and
// BackgroundSlicingProcess::stop()/stop_internal() park the UI thread until that worker stops.
// Blocking the worker on a marshaled modal -- which is safe for the plugin-load worker that
// request_permissions runs on -- would therefore deadlock the application (the
// invariant PluginHostUi.cpp documents for slicing-hook UI calls). Off the main thread the prompt
// is therefore posted asynchronously and the current event denied (fail closed, like an unanswered
// prompt); the grant is persisted once the user accepts, so a later attempt succeeds without
// re-prompting.
bool request_permission(AuditEventCategory category, const std::string& plugin_key, const std::string& plugin_name,
                        const std::string& event_name, bool by_event, const std::vector<std::string>& keys)
{
    const std::vector<std::string> shown = by_event ? std::vector<std::string>{} : keys;
    std::string targets_text = by_event ? "<none>" : "";
    for (const auto& target : shown) {
        if (!targets_text.empty())
            targets_text += ", ";
        targets_text += target;
    }

    BOOST_LOG_TRIVIAL(info) << "[AUDIT PROMPT] plugin=" << plugin_key << " event=" << event_name
                            << " category=" << audit_category_name(category) << " targets=[" << targets_text << "]"
                            << (by_event && !keys.empty() ? " grant=" + keys.front() : std::string{});

    if (s_permission_prompt_for_testing)
        return s_permission_prompt_for_testing(event_name, shown);

    if (wxTheApp == nullptr || GUI::wxGetApp().is_closing())
        return false;

    if (wxIsMainThread())
        return prompt_for_targets(category, plugin_name, event_name, shown);

    GUI::wxGetApp().CallAfter([category, plugin_key, plugin_name, event_name, by_event, keys, shown]() {
        if (wxTheApp == nullptr || GUI::wxGetApp().is_closing())
            return;
        if (prompt_for_targets(category, plugin_name, event_name, shown))
            persist_grant(plugin_key, category, by_event, keys);
    });
    return false;
}

// Decides an event that no allowed root or running operation covers: a saved grant allows it,
// otherwise the user is asked and a Yes is saved.
int decide_audited_event(PluginAuditManager&             mgr,
                         PluginInstallState&              state,
                         const std::string&                plugin_key,
                         const std::string&                plugin_name,
                         const std::string&                event_name,
                         AuditEventCategory                category,
                         const std::vector<std::string>&   targets)
{
    const bool                     by_event = targets.empty();
    const std::vector<std::string> keys     = by_event ? std::vector<std::string>{event_grant_key(category, event_name)} : targets;
    std::vector<std::string>*      permission_list = permission_list_for(category, by_event, state.permissions);

    std::vector<std::string> unresolved = keys;
    if (permission_list) {
        unresolved.clear();
        for (const auto& key : keys)
            if (!has_permission(category, by_event, *permission_list, key))
                unresolved.push_back(key);
        if (unresolved.empty())
            return 0;
    }

    // Letting a root frame go runs finalizers inside a scope destructor or another event's hook,
    // where no dialog may open: what they do must already be granted.
    if (mgr.releasing_frames())
        return report_denied(mgr, event_name, {false, "audit permission required"});

    if (!request_permission(category, plugin_key, plugin_name, event_name, by_event, unresolved))
        return report_denied(mgr, event_name, {false, "audit permission required"});

    if (permission_list)
        for (const auto& key : unresolved)
            persist_permission(plugin_key, state, category, by_event, *permission_list, key);
    return 0;
}

// Judges an audited event raised while a plugin context is active.
int audit_plugin_event(PluginAuditManager& mgr, const std::string& event_name, AuditEventCategory event_type, PyObject* args)
{
    const bool                      fs_category = is_fs_category(event_type);
    const std::vector<std::string>  targets      = audit_targets(event_name, event_type, args);

    // A denied path (secrets, certificates, config files -- see is_denied_path) is an
    // unconditional block: checked before the running-operation check below, so no granted
    // operation can launder access to one, and before the allowed-root shortcut further down,
    // so an allowed root (e.g. the read-only resources folder) cannot make a denied path
    // underneath it reachable.
    if (fs_category) {
        for (const auto& target : targets) {
            if (mgr.is_denied_path(boost::filesystem::path(target)))
                return report_denied(mgr, event_name, {false, "denied path"}, target);
        }
    }

    // A nested event of an operation already granted, e.g. socket.connect inside an approved
    // urlopen(), is not asked again. urllib.Request is always judged on its own URL, so a
    // redirect or a nested urlopen() inside a running request is not covered by it.
    if (event_name != "urllib.Request" && mgr.covered_by_root_frame())
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
                return mgr.check_path_access(boost::filesystem::path(target), is_write).allowed;
            });
        if (all_inside_allowed_root)
            return 0;
    }

    PluginInstallState state;
    const bool have_install_state = PluginManager::instance().get_install_state(mgr.current_plugin(), state);
    if (!have_install_state) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << " Failed to get install state for " << mgr.current_plugin();
    }

    const std::string plugin_name = state.plugin_name.empty() ? mgr.current_plugin() : state.plugin_name;

    const int result = decide_audited_event(mgr, state, mgr.current_plugin(), plugin_name, event_name, event_type, targets);

    // A granted operation covers its nested events while it runs. A targetless grant names no
    // operation, so it covers nothing else.
    if (result == 0 && !targets.empty() && operation_root_events.count(event_name))
        mgr.record_root_frame();
    return result;
}

} // namespace PluginAuditDetail

bool PluginAuditManager::request_permissions(const std::string& plugin_key, const PluginPermissions& requested)
{
    if (plugin_key.empty())
        return true;

    PluginDescriptor descriptor;
    if (!PluginManager::instance().try_get_plugin_descriptor(plugin_key, descriptor) || descriptor.plugin_root.empty())
        return false;

    PluginInstallState state;
    read_install_state(boost::filesystem::path(descriptor.plugin_root), state);

    // Per bucket, keep only the targets that no earlier grant already covers. Dropping these
    // before prompting keeps a repeated declaration from asking again for what is on disk.
    struct MissingBucket {
        AuditEventCategory       category;
        std::vector<std::string> targets;
    };
    std::vector<MissingBucket> missing_buckets;

    const auto collect_missing = [&missing_buckets](AuditEventCategory category,
                                                    const std::vector<std::string>& requested_targets,
                                                    const std::vector<std::string>& granted_targets) {
        // Only the filesystem buckets accept a tree grant; the others match verbatim.
        const bool tree_grant_bucket =
            category == AuditEventCategory::FsRead || category == AuditEventCategory::FsReadWrite;
        MissingBucket bucket{category, {}};
        for (const std::string& requested_target : requested_targets) {
            if (requested_target.empty())
                continue;
            // A relative wildcard names a directory relative to the working directory the request
            // is made in, while the audit hook reports absolute targets. Resolve it once, here, so
            // the prompt shows and the sidecar stores the scope the grant actually covers.
            const std::string target = tree_grant_bucket
                                           ? PluginAuditDetail::absolute_tree_grant(requested_target)
                                           : requested_target;
            // A saved wildcard already covers a more specific request. Deduplicate inside the
            // request verbatim only: two requested entries may cover different sets.
            if (!PluginAuditDetail::has_permission(category, /*by_event=*/false, granted_targets, target) &&
                std::find(bucket.targets.begin(), bucket.targets.end(), target) == bucket.targets.end())
                bucket.targets.push_back(target);
        }
        if (!bucket.targets.empty())
            missing_buckets.push_back(std::move(bucket));
    };

    collect_missing(AuditEventCategory::FsRead, requested.fs_read, state.permissions.fs_read);
    collect_missing(AuditEventCategory::FsReadWrite, requested.fs_readwrite, state.permissions.fs_readwrite);
    collect_missing(AuditEventCategory::Http, requested.network_http, state.permissions.network_http);
    collect_missing(AuditEventCategory::Socket, requested.network_socket, state.permissions.network_socket);
    collect_missing(AuditEventCategory::ProcessCreate, requested.process, state.permissions.process);
    collect_missing(AuditEventCategory::Threading, requested.threading, state.permissions.threading);

    if (missing_buckets.empty())
        return true;

    const auto build_message = [](const std::string& plugin_name, const std::vector<MissingBucket>& buckets) {
        wxString message;
        for (const MissingBucket& bucket : buckets) {
            wxString targets;
            for (const std::string& target : bucket.targets)
                targets += wxString::FromUTF8(target.c_str()) + "\n";

            if (!message.IsEmpty())
                message += "\n\n";
            message += PluginAuditDetail::audit_message(bucket.category, wxString::FromUTF8(plugin_name.c_str()),
                                                        wxEmptyString, targets, bucket.targets.size());
        }
        return message;
    };

    bool granted = false;
    if (PluginAuditDetail::s_permission_message_prompt_for_testing) {
        // Tests run without a wxApp to host the dialog: hand the seam the message it would show.
        granted = PluginAuditDetail::s_permission_message_prompt_for_testing(
            build_message(descriptor.name, missing_buckets).utf8_string());
    } else if (wxTheApp == nullptr || GUI::wxGetApp().is_closing()) {
        return false;
    } else if (wxIsMainThread()) {
        granted = PluginAuditDetail::prompt_for_permission_message(build_message(descriptor.name, missing_buckets));
    } else {
        auto result = std::make_shared<std::promise<bool>>();
        auto future = result->get_future();
        GUI::wxGetApp().CallAfter([result, plugin_name = descriptor.name, missing_buckets, build_message]() {
            result->set_value(PluginAuditDetail::prompt_for_permission_message(build_message(plugin_name, missing_buckets)));
        });
        granted = future.get();
    }

    if (!granted)
        return false;

    // The dialog can stay open while another grant is persisted; re-read before saving so this
    // request adds to the current state instead of replacing it.
    PluginInstallState latest_state;
    if (read_install_state(boost::filesystem::path(descriptor.plugin_root), latest_state))
        state = std::move(latest_state);

    PluginAuditDetail::seed_install_metadata(descriptor, state);

    for (const MissingBucket& bucket : missing_buckets) {
        std::vector<std::string>* permission_list =
            PluginAuditDetail::permission_list_for(bucket.category, /*by_event=*/false, state.permissions);
        if (!permission_list)
            continue;
        for (const std::string& target : bucket.targets)
            if (std::find(permission_list->begin(), permission_list->end(), target) == permission_list->end())
                permission_list->push_back(target);
    }

    return write_install_state(boost::filesystem::path(descriptor.plugin_root), state);
}

void PluginAuditManager::record_root_frame()
{
    PyObject* frame = reinterpret_cast<PyObject*>(PyEval_GetFrame()); // borrowed reference
    if (!frame || !PluginAuditDetail::is_stdlib_frame(frame))
        return;
    if (std::find(m_root_frames.begin(), m_root_frames.end(), frame) != m_root_frames.end())
        return; // glob.glob and glob.glob/2 share a frame
    Py_INCREF(frame);
    m_root_frames.push_back(frame);
}

bool PluginAuditManager::covered_by_root_frame() const
{
    if (m_root_frames.empty())
        return false;

    // Only stdlib frames may sit between the event and the operation: plugin or library code
    // the operation calls back into (an os.walk onerror, a urllib handler) acts on its own.
    for (PyObject* frame : PluginAuditDetail::stack_frames()) {
        if (std::find(m_root_frames.begin(), m_root_frames.end(), frame) != m_root_frames.end())
            return true;
        if (!PluginAuditDetail::is_stdlib_frame(frame))
            return false;
    }
    return false;
}

bool PluginAuditManager::releasing_frames() const { return m_releasing_frames > 0; }

bool PluginAuditManager::is_stdlib_file(const std::string& filename)
{
    // Frozen stdlib modules (os, posixpath, codecs, ...) carry a pseudo filename.
    if (boost::algorithm::starts_with(filename, "<frozen "))
        return true;
    if (filename.empty() || filename.front() == '<')
        return false;

    // Both sides are resolved: co_filename follows the Python home as it was given, which may
    // be a symlink.
    const auto [it, inserted] = PluginAuditDetail::s_stdlib_file_verdicts.try_emplace(filename, false);
    if (inserted) {
        const boost::filesystem::path file(filename);
        const auto inside_any = [&file](const std::vector<boost::filesystem::path>& dirs) {
            return std::any_of(dirs.begin(), dirs.end(),
                               [&file](const boost::filesystem::path& dir) { return is_inside_allowed_root(file, dir); });
        };
        it->second = inside_any(PluginAuditDetail::s_stdlib_dirs) && !inside_any(PluginAuditDetail::s_site_package_dirs);
    }
    return it->second;
}

bool PluginAuditManager::is_stdlib_path(const boost::filesystem::path& path)
{
    if (path.empty())
        return false;

    const auto inside_any = [&path](const std::vector<boost::filesystem::path>& dirs) {
        return std::any_of(dirs.begin(), dirs.end(),
                           [&path](const boost::filesystem::path& dir) { return is_inside_allowed_root(path, dir); });
    };
    return inside_any(PluginAuditDetail::s_stdlib_dirs) && !inside_any(PluginAuditDetail::s_site_package_dirs);
}

void PluginAuditManager::release_frames(std::vector<PyObject*>& frames)
{
    if (frames.empty())
        return;

    // A finalizer's own denial must neither replace the PermissionError of the event being
    // decided nor leave a stale denial behind for the next call.
    PyObject* const      error          = PyErr_GetRaisedException();
    const bool           denial_pending = m_audit_denial_pending;
    const bool           has_violation  = m_has_last_violation;
    const AuditViolation violation      = m_last_violation;

    std::vector<PyObject*> released;
    released.swap(frames);
    ++m_releasing_frames;
    for (PyObject* frame : released)
        Py_DECREF(frame);
    --m_releasing_frames;

    m_audit_denial_pending = denial_pending;
    m_has_last_violation   = has_violation;
    m_last_violation       = violation;
    PyErr_SetRaisedException(error);
}

void PluginAuditManager::set_permission_prompt_for_testing(PermissionPrompt prompt)
{
    PluginAuditDetail::s_permission_prompt_for_testing = std::move(prompt);
}

void PluginAuditManager::set_permission_message_prompt_for_testing(PermissionMessagePrompt prompt)
{
    PluginAuditDetail::s_permission_message_prompt_for_testing = std::move(prompt);
}

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

    // Root frames whose operation has returned are let go once this event is decided, so that a
    // finalizer they run re-enters a hook whose held frames are already consistent.
    std::vector<PyObject*> returned_frames;
    PluginAuditDetail::take_returned_frames(m_root_frames, returned_frames);
    const int result = PluginAuditDetail::audit_plugin_event(*mgr, event_name, event_type, args);
    mgr->release_frames(returned_frames);
    return result;
}

void PluginAuditManager::install_hook()
{
    // Identifies stdlib frames for operation roots and stdlib paths for read-only access.
    PluginAuditDetail::collect_stdlib_dirs();

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

    // The user's app config and cloud credentials live directly inside data_dir(), and plugin
    // install state lives beneath it, so the root just granted would otherwise expose them to
    // plugins. Deny them by name.
    //
    // Seeded here rather than by each secret's owner because install_hook() runs during lazy
    // interpreter init and therefore provably precedes any plugin bytecode, whereas
    // OrcaCloudServiceAgent::set_config_dir runs during networking init — neither strictly
    // precedes the other, and if Orca cloud never initializes an owner-registered token deny
    // would never exist at all.  default_denied_filenames() is the single source of that list
    // (see its comment for why all four config names are denied); the tests seed from it too.
    for (const auto& name : default_denied_filenames())
        add_denied_filename(name);

    // Categorical denies on top of the exact-name list above: a path a plugin reaches must not
    // have a "secret", "cert"(ificate), or "conf"(ig) component below data_dir() -- or anywhere
    // in the path outside data_dir(), whose own location may legitimately contain one (Flatpak
    // keeps it under .../config/OrcaSlicer). This holds regardless of which allowed root the
    // path happens to sit inside. default_denied_path_keywords() is the single source of this
    // list; the tests seed from it too.
    for (const auto& keyword : default_denied_path_keywords())
        add_denied_path_keyword(keyword);

    if (PySys_AddAuditHook(audit_hook, this) < 0) {
        BOOST_LOG_TRIVIAL(error) << "[AUDIT] Failed to install CPython audit hook";
        return;
    }
    BOOST_LOG_TRIVIAL(info) << "[AUDIT] CPython audit hook installed successfully";
}

} // namespace Slic3r
