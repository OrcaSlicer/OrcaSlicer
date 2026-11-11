#include "PluginAuditManager.hpp"

#include "../Utils/OrcaCloudServiceAgent.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/libslic3r.h" // GCODEVIEWER_APP_KEY, and SLIC3R_APP_KEY via libslic3r_version.h
#include "libslic3r_version.h"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem/file_status.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <future>
#include <mutex>
#include <memory>
#ifdef __APPLE__
#include <fcntl.h>
#include <limits.h>
#endif
#include <slic3r/GUI/BindDialog.hpp>
#include <slic3r/GUI/GUI_App.hpp>
#include <slic3r/GUI/MsgDialog.hpp>
#include <slic3r/plugin/PluginFsUtils.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <wx/app.h>
#include <wx/colour.h>
#include <wx/event.h>
#include <wx/dcclient.h>
#include <wx/gdicmn.h>
#include <wx/html/htmlwin.h>
#include <wx/msgdlg.h>
#include <wx/settings.h>
#include <wx/sizer.h>
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

// HTTP and raw-socket events are governed by the same network permission: a declared host may be
// reached through either layer, and the layers nest for one plugin action (urllib -> http.client
// -> socket).
static bool is_network_category(AuditEventCategory category)
{
    return category == AuditEventCategory::Http || category == AuditEventCategory::Socket;
}

// Create and delete grants name the folder that directly contains the target: an approval stores
// that parent folder rather than the target, and a saved exact grant allows later operations in
// the same folder. A tree grant "<folder>/*" also allows operations in every folder below it.
// Empty means no grant and a sole "*" means any location; see create_delete_grant_covers.
static bool is_create_delete_category(AuditEventCategory category)
{
    return category == AuditEventCategory::FsCreate || category == AuditEventCategory::FsDelete;
}

// ---------------------------------------------------------------------------
// Network permission identity
// ---------------------------------------------------------------------------

// Lowercases A-Z only. A locale-aware lowercase can break the UTF-8 of non-Latin names.
static std::string ascii_lower(std::string value)
{
    for (char& c : value)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return value;
}

static std::string trim_ascii_space(std::string value)
{
    boost::algorithm::trim(value);
    return value;
}

// Canonical storage keeps IPv6 literals bracketed (https://[::1]), while a socket address
// exposes the bare literal (::1). Compare them without the brackets.
static std::string without_ipv6_brackets(std::string host)
{
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);
    return host;
}

// Returns the host part of a URL authority, dropping userinfo and port. IPv6 literals keep their
// brackets so that a URL and a bare address compare consistently.
static std::string host_from_authority(std::string authority)
{
    const size_t at = authority.rfind('@');
    if (at != std::string::npos)
        authority.erase(0, at + 1);

    if (authority.empty())
        return {};

    if (authority.front() == '[') {
        const size_t close = authority.find(']');
        if (close == std::string::npos)
            return {};
        authority.erase(close + 1);
        return ascii_lower(authority);
    }

    const size_t colon = authority.find(':');
    if (colon != std::string::npos)
        authority.erase(colon);

    if (authority.find_first_of(" \t\r\n/?#@") != std::string::npos)
        return {};

    return ascii_lower(authority);
}

// Extracts a host from a target that is not a URL: "host", "host:443", "[::1]:443", "('host',
// 443)" (the form older releases persisted for socket address events), or "'host'".
static std::string network_host_from_target(const std::string& target)
{
    std::string value = trim_ascii_space(target);
    if (value.empty())
        return {};

    if (value.front() == '(') {
        // Legacy persisted address tuple: ("host", 443) or ("host", 443, 0, 0).
        const size_t quote = value.find_first_of("'\"");
        if (quote != std::string::npos) {
            const char   delimiter = value[quote];
            const size_t end       = value.find(delimiter, quote + 1);
            if (end == std::string::npos)
                return {};
            value = value.substr(quote + 1, end - quote - 1);
        } else {
            value = value.substr(1);
            const size_t comma = value.find(',');
            if (comma != std::string::npos)
                value.erase(comma);
        }
    } else if (value.front() == '[') {
        const size_t close = value.find(']');
        if (close == std::string::npos)
            return {};
        value.erase(close + 1);
    } else {
        // Strip a ":port" suffix, but leave a bare IPv6 literal ("::1") intact.
        const size_t colon = value.find(':');
        if (colon != std::string::npos && value.find(':', colon + 1) == std::string::npos)
            value.erase(colon);
    }

    value = trim_ascii_space(value);
    if (value.empty() || value.find_first_of(" \t\r\n/?#@") != std::string::npos)
        return {};

    return ascii_lower(value);
}

std::string PluginAuditManager::normalize_network_url(const std::string& url)
{
    const size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos || scheme_end == 0)
        return {};

    const std::string scheme = ascii_lower(url.substr(0, scheme_end));
    if (scheme != "http" && scheme != "https")
        return {};

    const size_t authority_start = scheme_end + 3;
    const size_t authority_end   = url.find_first_of("/?#", authority_start);
    const std::string authority  = url.substr(authority_start, authority_end == std::string::npos
                                                                    ? std::string::npos
                                                                    : authority_end - authority_start);

    const std::string host = host_from_authority(authority);
    if (host.empty())
        return {};

    return scheme + "://" + host;
}

bool PluginAuditManager::network_permission_matches(const std::vector<std::string>& granted,
                                                    const std::string&              target)
{
    if (target.empty())
        return false;

    const bool        target_is_url = target.find("://") != std::string::npos;
    const std::string target_url    = normalize_network_url(target);
    if (target_is_url && target_url.empty()) {
        // A non-http(s) URL has no network identity we can generalize; only an exact grant
        // recorded by an earlier approval stays valid, so an unrelated scheme never matches.
        return std::find(granted.begin(), granted.end(), target) != granted.end();
    }

    const std::string target_host =
        target_is_url ? std::string{} : without_ipv6_brackets(network_host_from_target(target));
    if (!target_is_url && target_host.empty())
        return false;

    for (const std::string& entry : granted) {
        // Preserve an exact grant recorded by an older release, e.g. a full URL with a query.
        if (entry == target)
            return true;

        const std::string entry_url = normalize_network_url(entry);
        if (target_is_url) {
            // Canonical scheme://host grants are host-wide. Older grants that persisted a full
            // URL remain scoped to that exact URL, preserving the operation-root URL check.
            if (!entry_url.empty() && entry_url == entry && entry_url == target_url)
                return true;
            continue;
        }

        if (!entry_url.empty()) {
            // A bare host carries no scheme: only a canonical scheme://host grant is host-wide.
            // A grant that persisted a full URL stays scoped to that URL, so it must not
            // authorize raw socket access to its whole host. Normalization lowercases the scheme
            // and host, so a canonical grant compares equal to its lowercased form.
            if (ascii_lower(entry) == entry_url) {
                const std::string entry_host = entry_url.substr(entry_url.find("://") + 3);
                if (without_ipv6_brackets(entry_host) == target_host)
                    return true;
            }
        } else if (without_ipv6_brackets(network_host_from_target(entry)) == target_host) {
            // Legacy persisted address tuples, e.g. "('host', 443)".
            return true;
        }
    }

    return false;
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

struct PermissionMessage {
    wxString text;
    std::vector<std::pair<size_t, size_t>> targets;
};

class PermissionDialog : public GUI::MsgDialog
{
public:
    PermissionDialog(const PermissionMessage& message, const wxString& title, bool show_tip)
        : MsgDialog(nullptr, title, wxEmptyString, wxYES_NO | wxICON_WARNING)
    {
        auto* html = new wxHtmlWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxHW_SCROLLBAR_AUTO);
        const wxFont prose = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
        const wxFont code = GUI::wxGetApp().code_font();
        const int point_size = prose.GetPointSize();
        int sizes[] = {point_size, code.GetPointSize(), point_size, point_size, point_size, point_size, point_size};
        html->SetFonts(prose.GetFaceName(), code.GetFaceName(), sizes);
        html->SetBorders(2);

        auto escape = [](const wxString& text, bool fixed) {
            std::string result = xml_escape(text.ToUTF8().data());
            boost::replace_all(result, "\r\n", "\n");
            boost::replace_all(result, "\n", "<br>");
            if (fixed)
                boost::replace_all(result, " ", "&nbsp;");
            return wxString::FromUTF8(result.c_str());
        };
        wxString body;
        size_t pos = 0;
        for (const auto& range : message.targets) {
            body += escape(message.text.Mid(pos, range.first - pos), false);
            // Add visual separation without changing the translated message or target text.
            // The target list already ends in a newline; surrounding messages may provide
            // their own blank lines, so only add spacing where there isn't one already.
            if (range.first > 0) {
                if (message.text[range.first - 1] != '\n')
                    body += "<br>";
                if (range.first < 2 || message.text[range.first - 2] != '\n')
                    body += "<br>";
            }
            body += "<font size=2><tt>" + escape(message.text.Mid(range.first, range.second), true) + "</tt></font>";
            pos = range.first + range.second;
            if (pos < message.text.length() && message.text[pos] != '\n')
                body += "<br>";
        }
        body += escape(message.text.Mid(pos), false);
        const wxColour foreground = GUI::wxGetApp().get_label_clr_default();
        const wxColour background = GetBackgroundColour();

        wxClientDC dc(this);
        dc.SetFont(prose);
        const int em = std::max(dc.GetTextExtent("m").x, FromDIP(10));
        const wxSize extent = dc.GetMultiLineTextExtent(message.text);
        const int width = std::min(std::max(extent.x + FromDIP(20), 35 * em), 68 * em);
        // Lay out at the intended width before measuring. wxHtmlWindow computes its virtual
        // size from the rendered cells, including wrapped prose and unbreakable target lines.
        html->SetSize(wxSize(width, 4 * em));
        html->SetPage(wxString::Format("<html><body bgcolor=\"#%02x%02x%02x\"><font color=\"#%02x%02x%02x\">",
                                       background.Red(), background.Green(), background.Blue(),
                                       foreground.Red(), foreground.Green(), foreground.Blue()) + body + "</font></body></html>");
        const wxSize virtual_size = html->GetVirtualSize();
        const int scrollbar = virtual_size.x > html->GetClientSize().x
                                  ? wxSystemSettings::GetMetric(wxSYS_HSCROLL_Y, html) : 0;
        const int height = std::min(std::max(virtual_size.y + scrollbar + FromDIP(4), 4 * em), 48 * em);
        html->SetMinSize(wxSize(width, height));
        m_html = html;
        m_content_dip = ToDIP(wxSize(width, height));
        content_sizer->Add(html, 1, wxEXPAND | wxRIGHT, 8);
        GUI::wxGetApp().UpdateDarkUIWin(html);
        if (show_tip)
            show_dsa_button(_L("Don't show this tip again"));
        finalize();
    }

    void on_dpi_changed(const wxRect& suggested_rect) override
    {
        GUI::MsgDialog::on_dpi_changed(suggested_rect);
        m_html->SetMinSize(FromDIP(m_content_dip));
        GetSizer()->SetSizeHints(this);
        Layout();
        Fit();
    }

private:
    wxHtmlWindow* m_html = nullptr;
    wxSize m_content_dip;
};

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

// True when two canonical paths name the same location: every component must match, using the
// platform's case rule. Unlike suffix_inside_root() this is full equality, so a lookalike that
// merely shares a prefix -- or a file beside the granted one -- can never match.
bool same_canonical_path(const boost::filesystem::path& lhs, const boost::filesystem::path& rhs)
{
    auto lhs_it = lhs.begin(), lhs_end = lhs.end();
    auto rhs_it = rhs.begin(), rhs_end = rhs.end();
    for (; lhs_it != lhs_end && rhs_it != rhs_end; ++lhs_it, ++rhs_it) {
        if (!same_path_component(*lhs_it, *rhs_it))
            return false;
    }
    return lhs_it == lhs_end && rhs_it == rhs_end;
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
thread_local std::vector<PyObject*> PluginAuditManager::m_delete_root_frames;
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
    , m_previous_delete_root_frames(std::move(PluginAuditManager::m_delete_root_frames))
{
    PluginAuditManager::instance().set_current_plugin(plugin_key);
    PluginAuditManager::instance().set_current_capability(capability_name);
    PluginAuditManager::m_scoped_allowed_roots.clear();
    PluginAuditManager::m_root_frames.clear();
    PluginAuditManager::m_delete_root_frames.clear();
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
        PluginAuditManager::m_delete_root_frames.clear();
    }

    mgr.set_current_plugin(m_previous_id);
    mgr.set_current_capability(m_previous_capability);
    PluginAuditManager::m_scoped_allowed_roots = std::move(m_previous_scoped_roots);
    PluginAuditManager::m_root_frames          = std::move(m_previous_root_frames);
    PluginAuditManager::m_delete_root_frames   = std::move(m_previous_delete_root_frames);
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

    const std::string lower = ascii_lower(keyword);

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
        const std::string name = ascii_lower(component.string());
        if (name.empty())
            continue;
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

namespace {

// A complete sentence of its own, so a translator can place it independently of the request
// body. Informational only: a prompt never navigates to, or opens, the Plugins dialog.
void append_permission_dialog_tip(wxString& message)
{
    message += "\n\n";
    message += _L("Tip: You can review and change this plugin's saved permissions in the Plugins dialog, on the Permissions tab.");
}

// The preference affects only the explanatory tip, never the permission decision.
// Must run on the GUI thread.
bool show_permission_dialog(PermissionMessage message, const wxString& title)
{
    auto* config = GUI::wxGetApp().app_config;
    const bool show_tip = config == nullptr || config->get("hide_plugin_permission_tip") != "1";
    if (show_tip)
        append_permission_dialog_tip(message.text);

    PermissionDialog dialog(message, title, show_tip);
    const int result = dialog.ShowModal();
    if (show_tip && config != nullptr && (result == wxID_YES || result == wxID_NO) && dialog.get_checkbox_state()) {
        config->set("hide_plugin_permission_tip", "1");
        config->save();
    }
    return result == wxID_YES;
}

} // namespace

bool PluginAuditManager::request_network_permissions(const std::string&              plugin_key,
                                                      const std::vector<std::string>& urls)
{
    if (plugin_key.empty() || urls.empty())
        return true;

    // Reduce every request to scheme://host before comparing or persisting it, so a URL with a
    // path or query cannot create a second grant for the same host.
    PluginPermissions requested;
    for (const std::string& url : urls) {
        const std::string normalized = normalize_network_url(url);
        if (normalized.empty()) {
            BOOST_LOG_TRIVIAL(warning) << "[AUDIT] Invalid network permission request for plugin " << plugin_key << ": " << url;
            return false;
        }
        if (std::find(requested.network_http.begin(), requested.network_http.end(), normalized) == requested.network_http.end())
            requested.network_http.push_back(normalized);
    }

    // network_permission_matches() also applies the recorded host grant to raw socket events, so
    // one HTTP grant covers both event layers without prompting for each list separately.
    return request_permissions(plugin_key, requested);
}

// A grant stored by an audit prompt can change what an existing Plugins dialog shows. Post a
// refresh to the GUI thread: the grant may be persisted by a plugin-load or slicing worker, and
// neither the notification manager nor the webview is thread safe. Never creates, shows or raises
// the dialog; GUI_App checks the live pointer on the GUI thread, and the dialog clears that
// pointer from its destroy handler.
void post_permission_changed_notification(const std::string& plugin_key)
{
    if (wxTheApp == nullptr)
        return;

    GUI::GUI_App* app = &GUI::wxGetApp();
    if (app->is_closing())
        return;

    app->CallAfter([app, plugin_key]() {
        if (wxTheApp == nullptr || app->is_closing())
            return;
        app->notify_plugin_permissions_changed(plugin_key);
    });
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

// Converts a str/bytes leaf to text, clearing a failed conversion. Returns false when `object` is
// neither str nor bytes, so callers can apply their own fallback.
bool python_text(PyObject* object, std::string& out)
{
    if (PyUnicode_Check(object)) {
        out = python_unicode(object);
        return true;
    }
    if (PyBytes_Check(object)) {
        const char* value = PyBytes_AsString(object);
        if (!value) {
            PyErr_Clear();
            out.clear();
            return true;
        }
        out = value;
        return true;
    }
    return false;
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

    std::string result;
    python_text(path_object, result);
    Py_DECREF(path_object);
    return result;
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

// Audit events whose path arguments are resolved against a directory file descriptor. CPython's
// tuple layouts (verified against 3.12):
//   os.mkdir(path, mode, dir_fd)
//   os.remove(path, dir_fd) / os.rmdir(path, dir_fd)
//   os.symlink(src, dst, dir_fd)            -- dir_fd applies to the destination only
//   os.link(src, dst, src_dir_fd, dst_dir_fd)
//   shutil.rmtree(path, dir_fd)             -- newer Python releases expose dir_fd here too
//
// A relative path must be resolved against the descriptor's folder, not the process working
// directory. Returns the dir_fd argument index for the given path argument, or -1 when the path
// argument is not resolved through a descriptor.
static int create_delete_dir_fd_index(const std::string& event_name, Py_ssize_t path_index)
{
    if (event_name == "os.mkdir")
        return path_index == 0 ? 2 : -1;
    if (event_name == "os.remove" || event_name == "os.rmdir")
        return path_index == 0 ? 1 : -1;
    if (event_name == "os.symlink")
        return path_index == 1 ? 2 : -1;
    if (event_name == "os.link")
        return path_index == 0 ? 2 : (path_index == 1 ? 3 : -1);
    if (event_name == "shutil.rmtree")
        return path_index == 0 ? 1 : -1;
    return -1;
}

// Resolves an open directory descriptor to its folder. Linux uses the /proc/self/fd link and
// macOS uses F_GETPATH. Returns false when the platform or the running environment cannot name
// the folder; the caller then fails closed instead of checking a path inside the working
// directory that the operation does not use.
static bool resolve_dir_fd_folder(int fd, boost::filesystem::path& out)
{
    namespace fs = boost::filesystem;

#if defined(__linux__)
    boost::system::error_code ec;
    const fs::path target = fs::read_symlink(fs::path("/proc/self/fd") / std::to_string(fd), ec);
    if (ec || target.empty() || !target.is_absolute())
        return false;
    out = target;
    return true;
#elif defined(__APPLE__)
    char buffer[PATH_MAX] = {};
    if (fcntl(fd, F_GETPATH, buffer) == -1)
        return false;
    const fs::path target(buffer);
    if (target.empty() || !target.is_absolute())
        return false;
    out = target;
    return true;
#else
    (void) fd;
    (void) out;
    return false;
#endif
}

// Replaces create/delete target paths that were resolved against a non-default dir_fd with paths
// under the folder that descriptor names. An absolute path ignores dir_fd (POSIX semantics), so it
// keeps its spelling. Returns false when a descriptor cannot be resolved or is not an integer;
// the caller then denies the operation.
static bool resolve_create_delete_targets(const std::string& event_name, PyObject* args,
                                          std::vector<std::string>& targets)
{
    const Py_ssize_t path_count = two_path_fs_events.count(event_name) ? 2 : 1;
    std::vector<std::string> resolved;
    resolved.reserve(targets.size());

    for (Py_ssize_t path_index = 0; path_index < path_count; ++path_index) {
        const std::string path = python_path(tuple_item(args, path_index));
        if (path.empty())
            continue;

        const int fd_index = create_delete_dir_fd_index(event_name, path_index);
        PyObject* const fd_item = fd_index >= 0 ? tuple_item(args, fd_index) : nullptr;
        if (fd_item != nullptr && fd_item != Py_None) {
            if (!PyLong_Check(fd_item))
                return false;
            const long fd_value = PyLong_AsLong(fd_item);
            if (PyErr_Occurred()) {
                PyErr_Clear();
                return false;
            }
            // CPython passes AT_FDCWD (negative) for an omitted dir_fd.
            if (fd_value >= 0 && !boost::filesystem::path(path).is_absolute()) {
                boost::filesystem::path base;
                if (!resolve_dir_fd_folder(static_cast<int>(fd_value), base))
                    return false;
                resolved.push_back((base / boost::filesystem::path(path)).string());
                continue;
            }
        }

        resolved.push_back(path);
    }

    targets = std::move(resolved);
    return true;
}

// A link creation reports its existing source and its new destination, but only the destination
// is created. The create grant covers the destination's parent folder; the source is not treated
// as created (a hardlink source is separately required to be writable). Both paths stay subject
// to the deny registry, and an unreadable destination fails closed rather than falling back to
// the source.
static std::vector<std::string> create_permission_targets(const std::string& event_name,
                                                          const std::vector<std::string>& targets)
{
    if (event_name == "os.link" || event_name == "os.symlink" || event_name == "_winapi.CreateJunction")
        return targets.size() == 2 ? std::vector<std::string>{targets.back()} : std::vector<std::string>{};
    return targets;
}

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
    case AuditEventCategory::FsCreate:      return &permissions.fs_create;
    case AuditEventCategory::FsDelete:      return &permissions.fs_delete;
    case AuditEventCategory::Http:          return &permissions.network_http;
    case AuditEventCategory::Socket:        return &permissions.network_socket;
    case AuditEventCategory::ProcessCreate: return &permissions.process;
    case AuditEventCategory::ProcessReplace: return &permissions.process;
    // Threading is a boolean rather than a target list; decide_audited_event and persist_grant
    // handle it directly.
    case AuditEventCategory::Threading:     return nullptr;
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

// True when `root` is a filesystem root: POSIX "/", a Windows drive root such as "C:\", or a
// UNC/network share root such as "\\server\share\" (including the extended-length forms
// "\\?\C:\" and "\\?\UNC\server\share\"). A tree grant rooted here covers the whole filesystem,
// volume, or network share, not one folder, so the declaration prompt must say so instead of
// presenting it as an ordinary nested folder.
bool is_filesystem_root(const boost::filesystem::path& root)
{
    if (!root.is_absolute())
        return false;

#ifdef _WIN32
    const auto is_separator = [](char c) { return c == '/' || c == '\\'; };
    const std::string path = root.string();

    std::vector<std::string> components;
    for (size_t start = 0; start < path.size();) {
        while (start < path.size() && is_separator(path[start]))
            ++start;
        size_t end = start;
        while (end < path.size() && !is_separator(path[end]))
            ++end;
        if (end > start && path.compare(start, end - start, ".") != 0)
            components.emplace_back(path.substr(start, end - start));
        start = end;
    }

    if (path.size() >= 2 && is_separator(path[0]) && is_separator(path[1])) {
        if (components.size() == 4 && components[0] == "?" &&
            boost::algorithm::iequals(components[1], std::string("UNC")))
            return true; // \\?\UNC\server\share
        return components.size() == 2; // \\server\share or an extended drive root \\?\C:
    }
    return components.size() == 1 && components[0].size() == 2 && components[0][1] == ':';
#else
    return root == root.root_path();
#endif
}

// The grant to display and store for a permission request: a recognized tree grant is resolved
// against the working directory when the request is made, so a relative "<dir>/*" names the
// directory it actually covers. Grants that are already absolute, exact (non-tree) grants, and
// the non-filesystem buckets are returned unchanged: re-rendering them would change the string
// the user approved, and an exact grant is matched by path equivalence (see exact_grant_covers)
// rather than by that spelling alone.
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

// True when an exact (non-tree) grant names the same file as `target`. A verbatim match is
// accepted first, unchanged. Otherwise an absolute grant is compared with the target's canonical
// path: "." / ".." and repeated separators are resolved, a relative target is resolved against
// the current working directory, and symlinks are followed on both sides. The grant is therefore
// treated as naming the file rather than the exact spelling the user approved, so another path or
// a symlink alias reaching that file matches too; that is a deliberate slight broadening of an
// exact grant. A link to a different file, a sibling, or a lookalike name still does not match:
// the comparison is full path equality, not containment, so an exact grant never becomes a tree
// grant.
//
// The grant must be absolute. A relative grant only matches verbatim: which directory it names
// depends on the working directory at check time, which a plugin can steer with os.chdir().
//
// Canonicalization errors fail closed instead of falling back to lexical normalization, which
// could equate different files across unresolved symlinks and "..". A target whose final
// component is a dangling symlink is not matched by equivalence, and neither is a grant ending in
// one. Windows compares case-insensitively and accepts either separator; POSIX stays
// case-sensitive and treats "\" as an ordinary name character. Hardlinks, 8.3 short names and
// extended-length paths remain distinct paths.
bool exact_grant_covers(const std::string& grant, const std::string& target)
{
    if (grant == target)
        return true;

    namespace fs = boost::filesystem;

    const fs::path grant_path(grant);
    if (!grant_path.is_absolute())
        return false;

    boost::system::error_code ec;
    const fs::path target_path(target);
    const fs::path absolute_target = target_path.is_absolute() ? target_path : fs::absolute(target_path, ec);
    if (ec || !absolute_target.is_absolute())
        return false;

    // Check the spelled path as well as its canonical form: a trailing separator can hide a
    // dangling final link from the spelled form, and weakly_canonical() may either leave such a
    // link as the path's tail or resolve it to the missing name.
    if (ends_in_dangling_symlink(grant_path) || ends_in_dangling_symlink(absolute_target))
        return false;

    const fs::path canonical_grant = fs::weakly_canonical(grant_path, ec);
    if (ec || !canonical_grant.is_absolute() || ends_in_dangling_symlink(canonical_grant))
        return false;

    const fs::path canonical_target = fs::weakly_canonical(absolute_target, ec);
    if (ec || !canonical_target.is_absolute() || ends_in_dangling_symlink(canonical_target))
        return false;

    return same_canonical_path(canonical_grant, canonical_target);
}

// True when a filesystem grant covers a target: the exact string, an equivalent spelling of the
// same file for an exact grant, or -- for a tree grant -- the granted folder itself and every path
// beneath it, at any depth. Components are compared one by one, so "/root/*" covers "/root/a/b" but
// not "/root-other/file". A tree grant's root must be absolute; a relative root never authorizes,
// because the directory it names depends on the process working directory. That applies to an
// exact spelling of the entry too: "foo/*" must not match the relative target "foo/*", which is
// still a plugin-chosen path.
//
// A tree grant and its target are both canonicalized before the comparison, so a symlink inside
// the granted folder cannot launder access to a path outside it, and a grant rooted at a symlink
// applies to the symlink's target. weakly_canonical keeps a write target whose tail does not
// exist yet matchable: it canonicalizes the existing prefix and appends the rest lexically. A
// dangling final symlink is not covered at all: containment fails closed, because a write through
// it would land at its target rather than at the granted path.
//
// An exact grant keeps its verbatim match -- legacy entries behave as before -- and additionally
// matches an equivalent spelling of the one file it names; see exact_grant_covers().
bool fs_grant_covers(const std::string& grant, const std::string& target)
{
    if (target.empty())
        return false;

    boost::filesystem::path root;
    if (!tree_grant_root(grant, root))
        return exact_grant_covers(grant, target);

    // A relative root names whatever directory the process happens to be in, which differs
    // between runs and which a plugin could otherwise steer. Fail closed for the whole entry,
    // exact match included.
    if (!root.is_absolute())
        return false;

    return grant == target || is_inside_allowed_root(boost::filesystem::path(target), root);
}

// ---------------------------------------------------------------------------
// Create and delete permission identity
// ---------------------------------------------------------------------------

// A create or delete grant names a folder whose direct entries may be created or deleted, or a
// tree grant "<root>/*" that additionally covers every folder below that root, at any depth.
// An empty list grants nothing, and a sole "*" grants any location. Any other entry must be an
// absolute folder path or an absolute tree grant; a relative or unreadable entry never matches.
// A "*" beside another entry is therefore an ordinary, non-matching entry: full access is only
// ever the explicit one-element form.
bool create_delete_grant_full(const std::vector<std::string>& granted)
{
    return granted.size() == 1 && granted.front() == "*";
}

// Strictly resolves an absolute folder for a grant comparison. Unlike canonical_for_comparison it
// does not fall back to a lexical path: a path that cannot be canonicalized, or whose final
// component is a dangling symlink, never names a folder the grant covers. Relative paths never
// match -- which folder they name depends on the working directory at check time.
bool canonical_folder(const boost::filesystem::path& folder, boost::filesystem::path& out)
{
    namespace fs = boost::filesystem;

    if (folder.empty() || !folder.is_absolute())
        return false;
    if (ends_in_dangling_symlink(folder))
        return false;

    boost::system::error_code ec;
    const fs::path canonical = fs::weakly_canonical(folder, ec);
    if (ec || !canonical.is_absolute() || ends_in_dangling_symlink(canonical))
        return false;

    out = canonical;
    return true;
}

// The folder that directly contains the entry a create/delete event targets. The target itself may
// not exist yet (a create), so its parent is canonicalized with weakly_canonical: the existing
// prefix is resolved and the non-existing tail is appended lexically. A final ".." or "." names a
// directory rather than an entry, so that target is resolved first and its parent is used instead.
// The final component itself is deliberately not canonicalized: deleting or creating a symlink
// acts on the link inside its parent folder, not on the link's destination. A trailing separator
// on a symlink is rejected instead of resolving through the link: the path is ambiguous between
// the link and its destination, and must not grant the destination's parent folder. Returns false
// when the folder cannot be resolved, and the operation then fails closed.
bool create_delete_target_folder(const std::string& target, boost::filesystem::path& folder)
{
    namespace fs = boost::filesystem;

    if (target.empty())
        return false;

    const fs::path path(target);
    boost::system::error_code ec;
    const fs::path absolute = path.is_absolute() ? path : fs::absolute(path, ec);
    if (ec || !absolute.is_absolute())
        return false;

    const fs::path filename = path.filename();
    if (filename == "." || filename == "..") {
        if (filename == ".") {
            // A trailing separator (or a trailing ".") names the directory itself. Strip those
            // components to find the entry the path ends at; if it is a symlink, resolve-through
            // would name the link destination's folder, so fail closed.
            fs::path entry = path;
            while (entry.filename() == "." && !entry.parent_path().empty())
                entry = entry.parent_path();
            if (!entry.empty() && entry.filename() != ".") {
                boost::system::error_code status_ec;
                const fs::file_status status = fs::symlink_status(entry, status_ec);
                if (!status_ec && fs::is_symlink(status))
                    return false;
            }
        }

        // The target names a directory: resolve it, then use the directory that contains it.
        const fs::path resolved = fs::weakly_canonical(absolute, ec);
        if (ec || !resolved.is_absolute())
            return false;
        const fs::path parent = resolved.parent_path();
        if (parent.empty())
            return false;
        folder = parent;
        return true;
    }

    const fs::path parent = fs::weakly_canonical(absolute.parent_path(), ec);
    if (ec || !parent.is_absolute())
        return false;

    folder = parent;
    return true;
}

// True when a runtime folder path contains '*'. A folder literally named with a wildcard cannot
// be stored as a literal create/delete grant: the sidecar would read the marker as a tree grant
// and cover a broader scope than the approved folder. Runtime callers fail closed before
// prompting for or persisting such a grant.
bool create_delete_folder_has_wildcard(const boost::filesystem::path& folder)
{
    return folder.string().find('*') != std::string::npos;
}

// True when one saved create/delete grant covers the canonical folder `wanted`. A tree grant
// covers its canonical root and every folder below it, at any depth. An exact grant names exactly
// one folder, so it is consulted only when `allow_exact_grant` is true: a tree request is never
// covered by an exact grant, because that grant authorizes only the entries directly inside its
// folder, not the subtree.
bool create_delete_grant_covers_canonical_folder(const std::vector<std::string>& granted,
                                                 const boost::filesystem::path&  wanted,
                                                 bool                            allow_exact_grant)
{
    for (const std::string& grant : granted) {
        boost::filesystem::path root;
        if (tree_grant_root(grant, root)) {
            boost::filesystem::path canonical_root;
            if (!canonical_folder(root, canonical_root))
                continue; // relative or unresolvable roots never authorize
            boost::filesystem::path suffix;
            if (suffix_inside_root(wanted, canonical_root, suffix))
                return true;
            continue;
        }
        if (!allow_exact_grant)
            continue;
        if (!canonical_folder(boost::filesystem::path(grant), root))
            continue; // relative or unresolvable entries never authorize
        if (same_canonical_path(root, wanted))
            return true;
    }
    return false;
}

// True when a saved create/delete grant already covers `folder`, the directory directly
// containing a create or delete target. An exact grant names that folder; a tree grant names
// its root and every folder below it. Folders are compared in canonical form so "." / "..",
// repeated separators and symlinked folders do not create a second grant for the same location,
// and a symlink inside a tree cannot escape it. A full grant covers every folder.
bool create_delete_grant_covers_folder(const std::vector<std::string>& granted, const std::string& folder)
{
    if (create_delete_grant_full(granted))
        return true;

    boost::filesystem::path wanted;
    if (!canonical_folder(boost::filesystem::path(folder), wanted))
        return false;

    return create_delete_grant_covers_canonical_folder(granted, wanted, /*allow_exact_grant=*/true);
}

// True when the saved create/delete grants cover creating or deleting `target`: the folder
// directly containing it is named by an exact grant, or lies inside a tree grant's root.
bool create_delete_grant_covers(const std::vector<std::string>& granted, const std::string& target)
{
    if (create_delete_grant_full(granted))
        return true;

    boost::filesystem::path folder;
    return create_delete_target_folder(target, folder) && create_delete_grant_covers_folder(granted, folder.string());
}

// Resolves a folder path to the absolute, canonical location stored in the sidecar. A relative
// path is resolved against the working directory the request is made in, like a relative tree
// grant. Returns false for an empty or unresolvable path; the caller fails closed instead of
// storing a grant with a different scope. The final component must not be a dangling symlink
// (see canonical_folder).
bool resolve_create_delete_folder(const boost::filesystem::path& path, boost::filesystem::path& folder)
{
    if (path.empty())
        return false;

    boost::system::error_code ec;
    const boost::filesystem::path absolute = path.is_absolute() ? path : boost::filesystem::absolute(path, ec);
    if (ec || !absolute.is_absolute())
        return false;

    return canonical_folder(absolute, folder);
}

// Resolves a folder requested by a plugin declaration to the absolute, canonical path stored in
// the sidecar. A relative folder is resolved against the working directory the request is made
// in, like a relative tree grant. Returns false for an empty, wildcard-containing, unresolvable or
// dangling entry; the declaration then fails closed instead of storing a grant with a different
// scope. The sole "*" full form is handled before this helper.
bool absolute_create_delete_folder(const std::string& requested, std::string& folder)
{
    if (requested.empty() || requested.find('*') != std::string::npos)
        return false;

    boost::filesystem::path canonical;
    if (!resolve_create_delete_folder(boost::filesystem::path(requested), canonical))
        return false;

    folder = canonical.string();
    return true;
}

// Splits a requested create/delete tree grant into its root. Unlike tree_grant_root, this is
// applied to a declaration, not to a stored grant: "." and ".." are allowed because the root is
// canonicalized and shown to the user before it is stored. "*" must be the only wildcard and the
// final path component. Returns false for every other shape.
bool requested_tree_grant_root(const std::string& requested, boost::filesystem::path& root)
{
    if (requested.size() < 2 || requested.back() != '*' || requested.find('*') != requested.size() - 1)
        return false;

    const boost::filesystem::path path(requested);
    if (path.filename() != "*")
        return false;

    const boost::filesystem::path parent = path.parent_path();
    if (parent.empty())
        return false;

    root = parent;
    return true;
}

// Resolves one entry from a create/delete declaration to the string stored for it. An exact
// folder becomes its canonical absolute path. A tree grant "<dir>/*" resolves its root the same
// way and is stored as "<canonical-root>/*", so the sidecar records the absolute scope the prompt
// showed. Returns false for an empty entry, a wildcard other than a final tree marker, or an
// entry that cannot be resolved; the declaration then fails closed. The sole "*" full form is
// handled before this helper.
bool absolute_create_delete_grant(const std::string& requested, std::string& grant)
{
    boost::filesystem::path root;
    if (!requested_tree_grant_root(requested, root))
        return absolute_create_delete_folder(requested, grant);

    boost::filesystem::path canonical;
    if (!resolve_create_delete_folder(root, canonical))
        return false;

    grant = (canonical / "*").string();
    return true;
}

// True when a saved create/delete grant already covers the whole scope of another grant entry.
// An exact requested folder is covered by an exact grant naming it, or by a tree grant whose root
// contains it. A requested tree grant is covered only by a tree grant whose root contains its
// root (or by full access): an exact grant authorizes direct entries in one folder, not the
// subtree the request asks for.
bool create_delete_grant_covers_grant(const std::vector<std::string>& granted, const std::string& requested)
{
    if (create_delete_grant_full(granted))
        return true;

    boost::filesystem::path requested_root;
    if (!tree_grant_root(requested, requested_root))
        return create_delete_grant_covers_folder(granted, requested);

    boost::filesystem::path wanted_root;
    if (!canonical_folder(requested_root, wanted_root))
        return false;

    return create_delete_grant_covers_canonical_folder(granted, wanted_root, /*allow_exact_grant=*/false);
}

// True when `granted` already covers `target`. Only the filesystem buckets accept a tree grant,
// and their exact entries may name the target file by an equivalent path spelling: an event
// grant ("FsRead:open") and the non-filesystem buckets (URLs, socket addresses, commands) are
// matched verbatim, as before. The create/delete buckets are folder grants: they match the
// folder directly containing the target (an exact folder grant) or any folder inside a tree
// grant's root, not the target's spelling.
bool has_permission(AuditEventCategory category, bool by_event, const std::vector<std::string>& granted,
                    const std::string& target)
{
    const bool fs_bucket =
        !by_event && (category == AuditEventCategory::FsRead || category == AuditEventCategory::FsReadWrite);
    const bool create_delete_bucket =
        !by_event && is_create_delete_category(category);

    if (create_delete_bucket)
        return create_delete_grant_covers(granted, target);

    for (const auto& grant : granted) {
        // fs_grant_covers also owns the exact match, so that a relative tree entry cannot match
        // by spelling alone.
        if (fs_bucket ? fs_grant_covers(grant, target) : grant == target)
            return true;
    }
    return false;
}

// A declared or previously approved network grant may live in either network list: an HTTP grant
// covers the sockets the same request opens, and a raw socket grant covers a later HTTP call to
// the same host.
bool has_network_permission(const PluginPermissions& permissions, const std::string& target)
{
    return PluginAuditManager::network_permission_matches(permissions.network_http, target) ||
           PluginAuditManager::network_permission_matches(permissions.network_socket, target);
}

// A targetless network event names no host the grant could be matched against: http.client.send
// writes to a connection that was already opened. Any recorded network grant covers it.
bool has_any_network_permission(const PluginPermissions& permissions)
{
    return !permissions.network_http.empty() || !permissions.network_socket.empty();
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

// Serialize the sidecar read-modify-write against every other writer. The lock is taken before
// the descriptor lookup, keeping the shared install-state -> PluginManager::m_mutex order.
bool persist_permission(const std::string& plugin_key, AuditEventCategory category, bool by_event,
                        const std::string& target)
{
    std::lock_guard<std::mutex> state_lock(install_state_mutex());

    PluginDescriptor descriptor;
    if (!PluginManager::instance().try_get_plugin_descriptor(plugin_key, descriptor) || descriptor.plugin_root.empty())
        return false;

    const boost::filesystem::path root(descriptor.plugin_root);
    PluginInstallState          state;
    // A missing sidecar is a discovered plugin that has not been given one yet: seed it below. An
    // existing sidecar that cannot be read must not be replaced, or its grants would be lost.
    if (read_install_state_status(root, state) == InstallStateReadStatus::Unreadable)
        return false;
    seed_install_metadata(descriptor, state);

    std::vector<std::string>* permission_list = permission_list_for(category, by_event, state.permissions);
    if (!permission_list)
        return false;

    if (is_create_delete_category(category)) {
        // A runtime folder whose name contains '*' cannot be stored as a literal create/delete
        // grant: the sidecar would read it as a tree grant. Fail closed rather than persist a
        // grant whose scope differs from the one the prompt showed.
        if (target.find('*') != std::string::npos)
            return false;
        // The caller passes the target's parent folder for these buckets, so compare it as a
        // folder rather than as a target path.
        if (!create_delete_grant_covers_folder(*permission_list, target))
            permission_list->push_back(target);
    } else if (!has_permission(category, by_event, *permission_list, target)) {
        permission_list->push_back(target);
    }
    return write_install_state(root, state);
}

// Threading is a boolean grant, so it has no target list to append to.
bool persist_threading_grant(const std::string& plugin_key)
{
    std::lock_guard<std::mutex> state_lock(install_state_mutex());

    PluginDescriptor descriptor;
    if (!PluginManager::instance().try_get_plugin_descriptor(plugin_key, descriptor) || descriptor.plugin_root.empty())
        return false;

    const boost::filesystem::path root(descriptor.plugin_root);
    PluginInstallState          state;
    // Same rule as persist_permission: seed a missing sidecar, never replace an unreadable one.
    if (read_install_state_status(root, state) == InstallStateReadStatus::Unreadable)
        return false;
    seed_install_metadata(descriptor, state);
    state.permissions.threading = true;
    return write_install_state(root, state);
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
                       const wxString& target_list, size_t target_count, bool full_fs_scope = false,
                       bool tree_scope = false, bool root_tree_scope = false)
{
    // Create and delete grants name folders, and a declaration may ask for the whole filesystem
    // with a sole "*". Describe that scope instead of printing the marker as a folder.
    if (full_fs_scope) {
        switch (category) {
        case AuditEventCategory::FsCreate:
            return wxString::Format(
                _L("Plugin \"%s\" is requesting to create files anywhere on this computer.\n\n"
                   "Paths the host denies stay blocked."),
                plugin_name);
        case AuditEventCategory::FsDelete:
            return wxString::Format(
                _L("Plugin \"%s\" is requesting to delete files anywhere on this computer.\n\n"
                   "Paths the host denies stay blocked."),
                plugin_name);
        default:
            break;
        }
    }

    if (target_list.IsEmpty())
        return wxString::Format(
            _L("Plugin \"%s\" is requesting permission for the Python audit event \"%s\".\n\n"
                "This operation does not expose a target the audit hook can display."),
            plugin_name, event_name);

    if (tree_scope) {
        if (root_tree_scope) {
            switch (category) {
            // A tree grant rooted at a filesystem, drive, or network share root covers everything
            // on it. Say so explicitly instead of describing it like an ordinary nested folder.
            case AuditEventCategory::FsCreate:
                return wxString::Format(_L_PLURAL("Plugin \"%s\" is requesting to create files in the following folder:\n%s\n\n"
                                                  "Approval covers creating entries directly inside the listed folder and every "
                                                  "folder below it. A target that is a filesystem, drive, or network share root, such as /*, "
                                                  "C:\\*, or \\\\server\\share\\*, covers that entire filesystem, drive, or network share.",
                                                  "Plugin \"%s\" is requesting to create files in the following folders:\n%s\n\n"
                                                  "Approval covers creating entries directly inside each listed folder and every "
                                                  "folder below it. A target that is a filesystem, drive, or network share root, such as /*, "
                                                  "C:\\*, or \\\\server\\share\\*, covers that entire filesystem, drive, or network share.",
                                                  target_count),
                                        plugin_name, target_list);
            case AuditEventCategory::FsDelete:
                return wxString::Format(_L_PLURAL("Plugin \"%s\" is requesting to delete files in the following folder:\n%s\n\n"
                                                  "Approval covers deleting entries directly inside the listed folder and every "
                                                  "folder below it. A target that is a filesystem, drive, or network share root, such as /*, "
                                                  "C:\\*, or \\\\server\\share\\*, covers that entire filesystem, drive, or network share.",
                                                  "Plugin \"%s\" is requesting to delete files in the following folders:\n%s\n\n"
                                                  "Approval covers deleting entries directly inside each listed folder and every "
                                                  "folder below it. A target that is a filesystem, drive, or network share root, such as /*, "
                                                  "C:\\*, or \\\\server\\share\\*, covers that entire filesystem, drive, or network share.",
                                                  target_count),
                                        plugin_name, target_list);
            default:
                break;
            }
        }
        switch (category) {
        // The target list shows a tree grant as "<folder>/*". Its approval covers subfolders,
        // unlike a literal folder entry, so the prompt says so. The scope sentence is part of
        // each msgid rather than appended, so a translator can reorder it.
        case AuditEventCategory::FsCreate:
            return wxString::Format(_L_PLURAL("Plugin \"%s\" is requesting to create files in the following folder:\n%s\n\n"
                                              "Approval covers creating entries directly inside the listed folder. "
                                              "A target ending in /* also covers that folder's subfolders, at any depth.",
                                              "Plugin \"%s\" is requesting to create files in the following folders:\n%s\n\n"
                                              "Approval covers creating entries directly inside each listed folder. "
                                              "A target ending in /* also covers that folder's subfolders, at any depth.",
                                              target_count),
                                    plugin_name, target_list);
        case AuditEventCategory::FsDelete:
            return wxString::Format(_L_PLURAL("Plugin \"%s\" is requesting to delete files in the following folder:\n%s\n\n"
                                              "Approval covers deleting entries directly inside the listed folder. "
                                              "A target ending in /* also covers that folder's subfolders, at any depth.",
                                              "Plugin \"%s\" is requesting to delete files in the following folders:\n%s\n\n"
                                              "Approval covers deleting entries directly inside each listed folder. "
                                              "A target ending in /* also covers that folder's subfolders, at any depth.",
                                              target_count),
                                    plugin_name, target_list);
        default:
            break;
        }
    }

    switch (category) {
    case AuditEventCategory::FsRead:
        return wxString::Format(_L_PLURAL("Plugin \"%s\" is requesting to read the following file:\n%s",
                                          "Plugin \"%s\" is requesting to read the following files:\n%s", target_count),
                                plugin_name, target_list);
    case AuditEventCategory::FsReadWrite:
        return wxString::Format(_L_PLURAL("Plugin \"%s\" is requesting to read/write the following file:\n%s",
                                          "Plugin \"%s\" is requesting to read/write the following files:\n%s", target_count),
                                plugin_name, target_list);
    // These buckets persist a folder grant: a Yes stores the folder shown in the prompt, and
    // later operations whose target is directly inside it are allowed. The scope sentence is part
    // of each msgid rather than appended, so a translator can reorder it.
    case AuditEventCategory::FsCreate:
        return wxString::Format(_L_PLURAL("Plugin \"%s\" is requesting to create files in the following folder:\n%s\n\n"
                                          "Approval covers creating entries directly inside the listed folder. "
                                          "It does not cover its subfolders.",
                                          "Plugin \"%s\" is requesting to create files in the following folders:\n%s\n\n"
                                          "Approval covers creating entries directly inside each listed folder. "
                                          "It does not cover their subfolders.", target_count),
                                plugin_name, target_list);
    case AuditEventCategory::FsDelete:
        return wxString::Format(_L_PLURAL("Plugin \"%s\" is requesting to delete files in the following folder:\n%s\n\n"
                                          "Approval covers deleting entries directly inside the listed folder. "
                                          "It does not cover its subfolders.",
                                          "Plugin \"%s\" is requesting to delete files in the following folders:\n%s\n\n"
                                          "Approval covers deleting entries directly inside each listed folder. "
                                          "It does not cover their subfolders.", target_count),
                                plugin_name, target_list);
    case AuditEventCategory::Http:
        return wxString::Format(
            _L("Plugin \"%s\" is requesting network access to:\n%s\n\n"
               "Approval covers HTTP or HTTPS access and raw network connections to the listed hosts. "
               "It does not restrict ports. A URL with a path, query or port is covered only for that "
               "exact URL."),
            plugin_name, target_list);
    case AuditEventCategory::Socket:
        return wxString::Format(
            _L("Plugin \"%s\" is requesting network access to:\n%s\n\n"
               "Approval covers raw network connections to the listed hosts. It does not restrict ports."),
            plugin_name, target_list);
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

// Format with a placeholder first so translated messages can put the target list anywhere.
PermissionMessage formatted_audit_message(AuditEventCategory category, const wxString& plugin_name,
                                          const wxString& event_name, const wxString& targets, size_t count,
                                          bool full_fs_scope = false, bool tree_scope = false,
                                          bool root_tree_scope = false)
{
    if (targets.empty())
        return {audit_message(category, plugin_name, event_name, targets, count, full_fs_scope, tree_scope,
                              root_tree_scope), {}};

    wxString marker = "__ORCA_PERMISSION_TARGETS__";
    while (plugin_name.Contains(marker) || event_name.Contains(marker) || targets.Contains(marker))
        marker += "_";
    PermissionMessage message{
        audit_message(category, plugin_name, event_name, marker, count, full_fs_scope, tree_scope, root_tree_scope), {}};
    const size_t pos = message.text.find(marker);
    if (pos != wxString::npos) {
        message.text.replace(pos, marker.length(), targets);
        message.targets.emplace_back(pos, targets.length());
    }
    return message;
}

// Builds and shows the modal permission prompt. Must run on the GUI thread.
bool prompt_for_targets(AuditEventCategory category, const std::string& plugin_name, const std::string& event_name,
                        const std::vector<std::string>& unresolved)
{
    wxString target_list;
    for (const auto& target : unresolved)
        target_list += wxString::FromUTF8(target.c_str()) + "\n";

    const bool tree_scope = std::any_of(unresolved.begin(), unresolved.end(), [](const std::string& target) {
        boost::filesystem::path root;
        return tree_grant_root(target, root);
    });

    PermissionMessage message = formatted_audit_message(category, wxString::FromUTF8(plugin_name.c_str()),
                                                         wxString::FromUTF8(event_name.c_str()), target_list,
                                                         unresolved.size(), /*full_fs_scope=*/false, tree_scope,
                                                         /*root_tree_scope=*/false);
    return show_permission_dialog(message, _L("Plugin permission request"));
}

// Shows a prompt whose body may cover several permission buckets. Must run on the GUI thread.
bool prompt_for_permission_message(const PermissionMessage& message)
{
    return show_permission_dialog(message, _L("Plugin permissions"));
}

// Records a grant in the plugin's sidecar so it is not asked again. Each persist helper re-reads
// the sidecar under the shared lock, because the async prompt outlives the caller's copy of it.
// Returns true when at least one write succeeded, and only then notifies an existing Plugins
// dialog: a failed write leaves its Permissions tab accurate as it stands.
bool persist_grant(const std::string& plugin_key, AuditEventCategory category, bool by_event, const std::vector<std::string>& keys)
{
    bool persisted = false;
    if (category == AuditEventCategory::Threading) {
        persisted = persist_threading_grant(plugin_key);
    } else {
        for (const auto& key : keys) {
            if (persist_permission(plugin_key, category, by_event, key))
                persisted = true;
        }
    }

    if (persisted)
        post_permission_changed_notification(plugin_key);
    return persisted;
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
// `keys` are what a Yes saves and, for a targeted event, what the prompt shows. For a targetless
// event (by_event) the prompt shows no target and `keys` holds the event-grant key. Create/delete
// pass the canonical parent folder of each target, so the prompt shows exactly what is stored.
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
//
// Create and delete now persist a folder grant. On the GUI thread the inline prompt is answered
// before the operation returns, so approval lets it proceed immediately. Off the main thread the
// prompt is posted and the current operation is denied, so the plugin has to retry it after the
// approval has stored the folder.
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
// otherwise the user is asked. A Yes is saved for every category that has a persistent grant
// list, including create/delete, whose grant is the folder containing each approved target.
int decide_audited_event(PluginAuditManager&             mgr,
                         PluginInstallState&              state,
                         const std::string&                plugin_key,
                         const std::string&                plugin_name,
                         const std::string&                event_name,
                         AuditEventCategory                category,
                         const std::vector<std::string>&   targets)
{
    // Full access turns permission auditing off for this plugin, so no grant or prompt applies.
    // This is reached only after audit_plugin_event's unconditional denied-path loop: the
    // install-state sidecar, the app config and the cloud refresh token stay blocked even here.
    // It is a Python audit-hook policy only; the plugin still runs in the host process, so other
    // processes are not restricted.
    if (state.permissions.full_access)
        return 0;

    const bool                     by_event = targets.empty();
    const std::vector<std::string> keys     = by_event ? std::vector<std::string>{event_grant_key(category, event_name)} : targets;
    const bool                     threading_category = category == AuditEventCategory::Threading;
    const bool                     create_delete_category = is_create_delete_category(category);
    std::vector<std::string>*      permission_list = permission_list_for(category, by_event, state.permissions);

    // Create/delete grants name folders: an approval saves the folder that directly contains
    // each unresolved target, and the prompt shows those canonical folders -- including the
    // absolute form of relative targets and the resolved form of symlinked parents -- so the user
    // approves exactly the scope that will be stored. Every target has to be covered, and a
    // target whose folder cannot be resolved fails closed.
    std::vector<std::string> unresolved;
    if (threading_category) {
        // Threading is a boolean grant rather than a target list. The event still reports the
        // fixed sentinel target so it is not treated as a targetless event grant.
        if (!state.permissions.threading)
            unresolved = keys;
    } else if (create_delete_category && !by_event) {
        for (const std::string& target : targets) {
            if (permission_list && has_permission(category, /*by_event=*/false, *permission_list, target))
                continue;
            boost::filesystem::path folder;
            if (!create_delete_target_folder(target, folder))
                return report_denied(mgr, event_name, {false, "target folder cannot be resolved"}, target);
            const std::string grant = folder.string();
            // A folder whose own name contains '*' cannot be shown or stored as a literal grant:
            // the sidecar would read it as a tree grant over a broader scope. Fail closed.
            if (create_delete_folder_has_wildcard(folder))
                return report_denied(mgr, event_name, {false, "target folder contains a wildcard"}, target);
            if (std::find(unresolved.begin(), unresolved.end(), grant) != unresolved.end())
                continue; // another target in this folder already requires the same approval
            unresolved.push_back(grant);
        }
    } else {
        for (const auto& key : keys) {
            // A network grant authorizes a specific host, except for a targetless event such as
            // http.client.send, which names no host: any network grant covers that.
            const bool granted_network =
                is_network_category(category) &&
                (by_event ? has_any_network_permission(state.permissions)
                          : has_network_permission(state.permissions, key));
            if (!(permission_list && has_permission(category, by_event, *permission_list, key)) && !granted_network)
                unresolved.push_back(key);
        }
    }

    if (unresolved.empty())
        return 0;

    // Letting a root frame go runs finalizers inside a scope destructor or another event's hook,
    // where no dialog may open: what they do must already be granted.
    if (mgr.releasing_frames())
        return report_denied(mgr, event_name, {false, "audit permission required"});

    if (!request_permission(category, plugin_key, plugin_name, event_name, by_event, unresolved))
        return report_denied(mgr, event_name, {false, "audit permission required"});

    if (threading_category || permission_list) {
        persist_grant(plugin_key, category, by_event, unresolved);
    }
    return 0;
}

// A hardlink makes an existing source writable through its new name, so the source needs write
// authorization independently of the destination's create permission, not merely read access:
// a writable allowed root, a saved fs_readwrite grant, full access, or a read/write approval.
// A read grant is deliberately insufficient because the link turns a read-only path into a
// writable one. A create grant is never stored for the source. Returns false when the source is
// not writable or the approval is missing; the caller then denies the event. Called only after
// root-frame coverage, so a link performed by an approved read/write operation is covered as
// part of that operation.
bool authorize_hardlink_source(PluginAuditManager& mgr, const std::string& event_name, const std::string& source)
{
    if (source.empty())
        return false;

    // check_path_access also consults the deny registry; a denied source already denied the event
    // earlier, and a false result here still fails closed. is_write=true admits only writable
    // allowed roots, not read-only ones such as the bundled resources folder.
    if (mgr.check_path_access(boost::filesystem::path(source), /*is_write=*/true).allowed)
        return true;

    PluginInstallState state;
    const bool have_state = PluginManager::instance().get_install_state(mgr.current_plugin(), state);
    if (have_state && state.permissions.full_access)
        return true;
    if (have_state &&
        has_permission(AuditEventCategory::FsReadWrite, /*by_event=*/false, state.permissions.fs_readwrite, source))
        return true;

    // Letting a root frame go runs finalizers where no dialog may open: what they do must already
    // be granted, so the source check fails closed instead of prompting.
    if (mgr.releasing_frames())
        return false;

    const std::string plugin_name = state.plugin_name.empty() ? mgr.current_plugin() : state.plugin_name;
    if (!request_permission(AuditEventCategory::FsReadWrite, mgr.current_plugin(), plugin_name, event_name,
                            /*by_event=*/false, {source}))
        return false;

    persist_grant(mgr.current_plugin(), AuditEventCategory::FsReadWrite, /*by_event=*/false, {source});
    return true;
}

// Judges an audited event raised while a plugin context is active.
int audit_plugin_event(PluginAuditManager& mgr, const std::string& event_name, AuditEventCategory event_type, PyObject* args)
{
    const bool                      fs_category = is_fs_category(event_type);
    std::vector<std::string>        targets      = audit_targets(event_name, event_type, args);

    // A dir_fd makes a relative path name an entry in the descriptor's folder, not in the working
    // directory. Resolve it before any check so the deny registry, allowed roots and folder grants
    // all see the folder the operation actually uses. A descriptor that cannot be resolved denies
    // the operation; every later shortcut would otherwise judge the wrong path.
    if (is_create_delete_category(event_type) && !resolve_create_delete_targets(event_name, args, targets))
        return report_denied(mgr, event_name, {false, "directory file descriptor cannot be resolved"});

    // A link creation writes only its destination; its existing source is not a create target.
    const std::vector<std::string> permission_targets =
        event_type == AuditEventCategory::FsCreate ? create_permission_targets(event_name, targets) : targets;

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

    // A create/delete event with no readable destination cannot be matched against a folder grant.
    if (is_create_delete_category(event_type) && permission_targets.empty())
        return report_denied(mgr, event_name, {false, "target path cannot be resolved"});

    // A nested event of an operation already granted, e.g. socket.connect inside an approved
    // urlopen(), is not asked again. urllib.Request is always judged on its own URL, so a
    // redirect or a nested urlopen() inside a running request is not covered by it. A read/write
    // root (copytree, unpack_archive, move) covers the create/delete events it performs, but a
    // delete root (rmtree) does not cover nested deletes: an exact folder grant covers direct
    // entries only, so each nested delete is judged on its own (a tree grant can still allow it).
    if (event_name != "urllib.Request" &&
        mgr.covered_by_root_frame(/*allow_delete_roots=*/event_type != AuditEventCategory::FsDelete))
        return 0;

    // A hardlink's source becomes writable through the new name, so the source needs write
    // authorization on its own. This runs after root-frame coverage, so a link performed by an
    // approved read/write operation is covered as part of it, and before implicit allowed roots,
    // so a destination inside data_dir cannot authorize a source outside it. The destination's
    // create permission does not cover the source.
    if (event_name == "os.link" && is_create_delete_category(event_type)) {
        if (targets.size() < 2)
            return report_denied(mgr, event_name, {false, "hardlink source cannot be resolved"});
        if (!authorize_hardlink_source(mgr, event_name, targets.front()))
            return report_denied(mgr, event_name, {false, "hardlink source requires write permission"}, targets.front());
    }

    // A filesystem target that resolves entirely inside a pre-determined allowed root -- the
    // plugin system's own data_dir() tree (which holds each plugin's storage folder and the
    // installed/system profile cache), the read-only bundled resources folder, or a per-call
    // scoped root such as the current G-code folder -- is part of the plugin system's normal
    // workflow and does not need a prompt.
    if (fs_category && !permission_targets.empty()) {
        const bool is_write = event_type != AuditEventCategory::FsRead;
        const bool all_inside_allowed_root =
            std::all_of(permission_targets.begin(), permission_targets.end(), [&](const std::string& target) {
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

    const int result = decide_audited_event(mgr, state, mgr.current_plugin(), plugin_name, event_name, event_type, permission_targets);

    // A granted operation covers its nested events while it runs, except that a delete root does
    // not cover a nested delete (see the coverage check above). A targetless grant names no
    // operation, so it covers nothing else.
    if (result == 0 && !permission_targets.empty() && operation_root_events.count(event_name))
        mgr.record_root_frame(event_type);
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

    PluginInstallState     state;
    InstallStateReadStatus state_status;
    {
        std::lock_guard<std::mutex> state_lock(install_state_mutex());
        state_status = read_install_state_status(boost::filesystem::path(descriptor.plugin_root), state);
    }

    // An existing sidecar that cannot be read is never replaced: its grants and capability choices
    // would be lost, and the flag below cannot be trusted either. A missing sidecar is a plugin
    // that has not been given one yet and is seeded when the grant is saved.
    if (state_status == InstallStateReadStatus::Unreadable)
        return false;

    // Full access is an explicit opt-out of auditing: a declaration neither prompts nor adds a
    // grant. Only a readable sidecar that stores the flag counts, so a missing file is never
    // mistaken for an opt-out.
    if (state.permissions.full_access)
        return true;

    // Per bucket, keep only the targets that no earlier grant already covers. Dropping these
    // before prompting keeps a repeated declaration from asking again for what is on disk.
    struct MissingBucket {
        AuditEventCategory       category;
        std::vector<std::string> targets;
    };
    std::vector<MissingBucket> missing_buckets;

    // A network target counts as granted when either network list covers it, matched the same
    // way the audit hook matches it (canonical scheme://host, bare host, legacy address tuple),
    // so a declaration that is already reachable is not asked for again.
    const auto collect_missing = [&missing_buckets, &state](AuditEventCategory category,
                                                            const std::vector<std::string>& requested_targets,
                                                            const std::vector<std::string>& granted_targets) {
        // Only the filesystem buckets accept a tree grant. Network targets match by host and the
        // remaining buckets verbatim.
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
            const bool already_granted =
                is_network_category(category)
                    ? PluginAuditDetail::has_network_permission(state.permissions, target)
                    : PluginAuditDetail::has_permission(category, /*by_event=*/false, granted_targets, target);

            if (!already_granted && std::find(bucket.targets.begin(), bucket.targets.end(), target) == bucket.targets.end())
                bucket.targets.push_back(target);
        }
        if (!bucket.targets.empty())
            missing_buckets.push_back(std::move(bucket));
    };

    collect_missing(AuditEventCategory::FsRead, requested.fs_read, state.permissions.fs_read);
    collect_missing(AuditEventCategory::FsReadWrite, requested.fs_readwrite, state.permissions.fs_readwrite);

    // Create/delete requests name folders and are stored as canonical absolute paths. A trailing
    // "/*" requests the folder and every folder below it at any depth; any other wildcard is
    // rejected. A sole "*" requests unrestricted access. An entry that cannot be resolved fails
    // the whole request: storing a grant with a different scope than the one approved is not
    // acceptable.
    const auto collect_missing_create_delete =
        [&missing_buckets](AuditEventCategory category, const std::vector<std::string>& requested_folders,
                           const std::vector<std::string>& granted_folders) {
            const size_t requested_count = static_cast<size_t>(
                std::count_if(requested_folders.begin(), requested_folders.end(),
                              [](const std::string& folder) { return !folder.empty(); }));
            MissingBucket bucket{category, {}};
            for (const std::string& requested_folder : requested_folders) {
                if (requested_folder.empty())
                    continue;
                if (requested_folder == "*") {
                    // Unrestricted access is the one-element form, matching what the sidecar
                    // stores. A "*" beside other entries is ambiguous, so the request fails closed
                    // instead of widening or silently ignoring it.
                    if (requested_count != 1)
                        return false;
                    if (!PluginAuditDetail::create_delete_grant_full(granted_folders))
                        bucket.targets.assign(1, "*");
                    break;
                }
                // A trailing "/*" is a tree grant. Declarations accept literal folders, tree
                // grants and the sole "*" full form; any other wildcard makes the request fail
                // closed instead of being stored as a literal name or widened into a folder grant.
                std::string grant;
                if (!PluginAuditDetail::absolute_create_delete_grant(requested_folder, grant))
                    return false;
                if (!PluginAuditDetail::create_delete_grant_covers_grant(granted_folders, grant) &&
                    std::find(bucket.targets.begin(), bucket.targets.end(), grant) == bucket.targets.end())
                    bucket.targets.push_back(grant);
            }
            if (!bucket.targets.empty())
                missing_buckets.push_back(std::move(bucket));
            return true;
        };

    if (!collect_missing_create_delete(AuditEventCategory::FsCreate, requested.fs_create, state.permissions.fs_create) ||
        !collect_missing_create_delete(AuditEventCategory::FsDelete, requested.fs_delete, state.permissions.fs_delete))
        return false;

    collect_missing(AuditEventCategory::Http, requested.network_http, state.permissions.network_http);
    collect_missing(AuditEventCategory::Socket, requested.network_socket, state.permissions.network_socket);
    collect_missing(AuditEventCategory::ProcessCreate, requested.process, state.permissions.process);
    // Threading is a single boolean grant: ask once when requested and not yet granted. The fixed
    // sentinel gives the prompt a target even though thread creation exposes none.
    if (requested.threading && !state.permissions.threading)
        missing_buckets.push_back({AuditEventCategory::Threading, {kThreadPermissionTarget}});

    if (missing_buckets.empty())
        return true;

    const auto build_message = [](const std::string& plugin_name, const std::vector<MissingBucket>& buckets) {
        PermissionMessage message;
        for (const MissingBucket& bucket : buckets) {
            // A sole "*" is the unrestricted form: describe the scope instead of listing it as a
            // folder.
            const bool full_fs_scope = is_create_delete_category(bucket.category) &&
                                       bucket.targets.size() == 1 && bucket.targets.front() == "*";
            // A create/delete bucket with a tree grant shows "<folder>/*" and must disclose that
            // its approval also covers every subfolder.
            const bool tree_scope =
                is_create_delete_category(bucket.category) &&
                std::any_of(bucket.targets.begin(), bucket.targets.end(), [](const std::string& target) {
                    boost::filesystem::path root;
                    return PluginAuditDetail::tree_grant_root(target, root);
                });
            // A tree grant at a filesystem, drive, or network share root covers everything on it;
            // its prompt must say so explicitly rather than describing it as a routine nested folder.
            const bool root_tree_scope =
                tree_scope && std::any_of(bucket.targets.begin(), bucket.targets.end(), [](const std::string& target) {
                    boost::filesystem::path root;
                    return PluginAuditDetail::tree_grant_root(target, root) &&
                           PluginAuditDetail::is_filesystem_root(root);
                });
            wxString targets;
            if (!full_fs_scope)
                for (const std::string& target : bucket.targets)
                    targets += wxString::FromUTF8(target.c_str()) + "\n";

            if (!message.text.IsEmpty())
                message.text += "\n\n";
            PermissionMessage part = PluginAuditDetail::formatted_audit_message(
                bucket.category, wxString::FromUTF8(plugin_name.c_str()), wxEmptyString, targets, bucket.targets.size(),
                full_fs_scope, tree_scope, root_tree_scope);
            for (const auto& range : part.targets)
                message.targets.emplace_back(message.text.length() + range.first, range.second);
            message.text += part.text;
        }
        return message;
    };

    bool granted = false;
    if (PluginAuditDetail::s_permission_message_prompt_for_testing) {
        // Tests run without a wxApp to host the dialog: hand the seam the message it would show.
        wxString message = build_message(descriptor.name, missing_buckets).text;
        append_permission_dialog_tip(message);
        granted = PluginAuditDetail::s_permission_message_prompt_for_testing(message.utf8_string());
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

    // The dialog can stay open while another grant is persisted; take the shared lock and re-read
    // before saving so this request adds to the current state instead of replacing it.
    bool persisted = false;
    {
        std::lock_guard<std::mutex> state_lock(install_state_mutex());
        PluginInstallState     latest_state;
        InstallStateReadStatus latest_status =
            read_install_state_status(boost::filesystem::path(descriptor.plugin_root), latest_state);
        // A malformed file is never replaced. A sidecar that was readable when prompting started
        // must not be recreated if it disappears meanwhile: that is lost state, not a new install.
        // Only a sidecar absent at both reads is seeded.
        if (latest_status == InstallStateReadStatus::Unreadable)
            return false;
        if (latest_status == InstallStateReadStatus::Absent && state_status == InstallStateReadStatus::Readable)
            return false;
        state = std::move(latest_state);

        PluginAuditDetail::seed_install_metadata(descriptor, state);

        for (const MissingBucket& bucket : missing_buckets) {
            if (bucket.category == AuditEventCategory::Threading) {
                state.permissions.threading = true;
                continue;
            }
            std::vector<std::string>* permission_list =
                PluginAuditDetail::permission_list_for(bucket.category, /*by_event=*/false, state.permissions);
            if (!permission_list)
                continue;
            if (is_create_delete_category(bucket.category)) {
                // A full grant added by another writer while the dialog was open already covers
                // every folder; adding a folder beside it would stop it reading as the full form.
                if (PluginAuditDetail::create_delete_grant_full(*permission_list))
                    continue;
                if (std::find(bucket.targets.begin(), bucket.targets.end(), "*") != bucket.targets.end()) {
                    // Unrestricted create/delete is the one-element form; drop folder grants it covers.
                    permission_list->assign(1, "*");
                    continue;
                }
            }
            for (const std::string& target : bucket.targets) {
                // Another writer may have added an exact grant or a tree grant that already covers
                // this entry while the dialog was open; do not store a redundant duplicate.
                if (is_create_delete_category(bucket.category) &&
                    PluginAuditDetail::create_delete_grant_covers_grant(*permission_list, target))
                    continue;
                if (std::find(permission_list->begin(), permission_list->end(), target) == permission_list->end())
                    permission_list->push_back(target);
            }
        }

        persisted = write_install_state(boost::filesystem::path(descriptor.plugin_root), state);
    }

    // Only a stored grant changes what the dialog would read back; a failed write changed nothing.
    if (persisted)
        post_permission_changed_notification(plugin_key);
    return persisted;
}

bool PluginAuditManager::is_create_delete_tree_grant(const std::string& target)
{
    boost::filesystem::path root;
    return PluginAuditDetail::requested_tree_grant_root(target, root);
}

void PluginAuditManager::record_root_frame(AuditEventCategory category)
{
    PyObject* frame = reinterpret_cast<PyObject*>(PyEval_GetFrame()); // borrowed reference
    if (!frame || !PluginAuditDetail::is_stdlib_frame(frame))
        return;
    if (std::find(m_root_frames.begin(), m_root_frames.end(), frame) == m_root_frames.end()) {
        Py_INCREF(frame);
        m_root_frames.push_back(frame);
    }
    // Remember a frame recorded for a delete root. The list is non-owning; m_root_frames owns the
    // reference, and both lists are purged together when the frame is released.
    if (category == AuditEventCategory::FsDelete &&
        std::find(m_delete_root_frames.begin(), m_delete_root_frames.end(), frame) == m_delete_root_frames.end())
        m_delete_root_frames.push_back(frame);
}

bool PluginAuditManager::covered_by_root_frame(bool allow_delete_roots) const
{
    if (m_root_frames.empty())
        return false;

    // Only stdlib frames may sit between the event and the operation: plugin or library code
    // the operation calls back into (an os.walk onerror, a urllib handler) acts on its own.
    for (PyObject* frame : PluginAuditDetail::stack_frames()) {
        if (std::find(m_root_frames.begin(), m_root_frames.end(), frame) != m_root_frames.end()) {
            // A delete root does not cover a nested delete: an exact folder grant covers direct
            // entries only, while an outer read/write root may still cover the same operation,
            // and a tree grant may allow it when the event is judged.
            const bool delete_root =
                std::find(m_delete_root_frames.begin(), m_delete_root_frames.end(), frame) != m_delete_root_frames.end();
            if (!delete_root || allow_delete_roots)
                return true;
            continue;
        }
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
    for (PyObject* frame : released)
        m_delete_root_frames.erase(std::remove(m_delete_root_frames.begin(), m_delete_root_frames.end(), frame),
                                   m_delete_root_frames.end());
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
    int result = -1;
    // A C++ exception must not unwind through the interpreter that raised the event, so a check
    // that throws denies the event, like an unanswered prompt.
    try {
        result = PluginAuditDetail::audit_plugin_event(*mgr, event_name, event_type, args);
    } catch (const std::exception& error) {
        BOOST_LOG_TRIVIAL(error) << "[AUDIT] event=" << event_name << " denied, its check failed: " << error.what();
        result = PluginAuditDetail::report_denied(*mgr, event_name, {false, "audit check failed"});
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "[AUDIT] event=" << event_name << " denied, its check failed";
        result = PluginAuditDetail::report_denied(*mgr, event_name, {false, "audit check failed"});
    }
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
