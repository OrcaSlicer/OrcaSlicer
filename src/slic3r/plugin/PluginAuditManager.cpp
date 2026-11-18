#include "PluginAuditManager.hpp"

#include "../Utils/OrcaCloudServiceAgent.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
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

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <mutex>
#include <memory>
#ifdef __APPLE__
#include <fcntl.h>
#include <limits.h>
#endif
#include <optional>
#include <slic3r/GUI/BindDialog.hpp>
#include <slic3r/GUI/GUI_App.hpp>
#include <slic3r/GUI/MsgDialog.hpp>
#include <slic3r/GUI/format.hpp>
#include <slic3r/plugin/PluginFsUtils.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <wx/app.h>
#include <wx/colour.h>
#include <wx/dcclient.h>
#include <wx/event.h>
#include <wx/font.h>
#include <wx/gdicmn.h>
#include <wx/html/htmlwin.h>
#include <wx/msgdlg.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/thread.h>
#include <wx/window.h>
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
// address or a process executable -- the deny-path and allowed-root checks only make sense
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
    PermissionDialog(wxWindow* parent, const PermissionMessage& message, const wxString& title, bool show_tip)
        : MsgDialog(parent, title, wxEmptyString, wxYES_NO | wxICON_WARNING)
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

#ifdef _WIN32
// True when a status error only says the path does not exist. Boost clears `ec` for that case on
// POSIX, but its Windows status path (process_status_failure()) always stores the Win32 error, so
// ERROR_FILE_NOT_FOUND and ERROR_PATH_NOT_FOUND mean "just missing" there too. Anything else
// (access denied, sharing violation, invalid name) is a real failure.
bool missing_path_error(const boost::system::error_code& ec)
{
    return !ec || ec.value() == ERROR_FILE_NOT_FOUND || ec.value() == ERROR_PATH_NOT_FOUND;
}

// Windows can spell one directory with either its long name or its 8.3 short name, and both name
// the same location. Python reports long names (os.path.realpath() and os.scandir()), while an
// allowed root is registered with whatever spelling the process was given, so the two spellings
// must compare equal. weakly_canonical() does not unify them: boost resolves symlinks and "."/".."
// but keeps every component as written, so link resolution stays exactly Boost's and this helper
// only re-spells the path it produced.
//
// GetLongPathNameW is a name-conversion API: it requires an existing path, can fail (for example
// when a parent cannot be read), and its documentation makes no promise about symbolic links. A
// caller that must keep a final link entry therefore must not hand a link to it (see
// delete_entry_names_subtree_root). Components below the longest existing prefix do not exist yet
// and have no short name, so they are appended as written. On failure `out` keeps the unexpanded
// canonical path and the caller decides whether to fail closed.
bool expand_short_name_prefix(const boost::filesystem::path& canonical, boost::filesystem::path& out)
{
    namespace fs = boost::filesystem;

    out = canonical;
    if (canonical.empty() || !canonical.is_absolute())
        return false;

    // No cheap test says whether a spelling is short: Windows does not guarantee that an 8.3 alias
    // contains '~' (or matches any other shape), so every comparison has to ask the file system.
    // Split off the components that do not exist yet; only an existing prefix has a long form.
    fs::path existing = canonical;
    fs::path tail;
    boost::system::error_code ec;
    for (;;) {
        if (fs::exists(existing, ec) && !ec)
            break;
        // A missing component is exactly the tail this function appends as written; any other
        // status error (access denied, sharing violation, invalid name) fails closed.
        if (!missing_path_error(ec) || !existing.has_relative_path())
            return false;
        const fs::path component = existing.filename();
        if (component.empty())
            return false;
        tail = tail.empty() ? component : component / tail;
        existing = existing.parent_path();
    }

    std::wstring buffer(static_cast<std::size_t>(MAX_PATH) + 1, L'\0');
    for (;;) {
        const DWORD length = ::GetLongPathNameW(existing.c_str(), buffer.data(),
                                                static_cast<DWORD>(buffer.size()));
        if (length == 0)
            return false;
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        buffer.resize(static_cast<std::size_t>(length) + 1);
    }

    const fs::path expanded(buffer);
    if (!expanded.is_absolute())
        return false;

    out = tail.empty() ? expanded : expanded / tail;
    return true;
}
#else
// 8.3 short names are a Windows-only alias; elsewhere the canonical path is already comparable.
bool expand_short_name_prefix(const boost::filesystem::path& canonical, boost::filesystem::path& out)
{
    out = canonical;
    return true;
}
#endif

// Canonicalize a path for containment comparison.  weakly_canonical resolves symlinks but does
// NOT require the path to exist — it canonicalizes the prefix that exists and appends the
// non-existing tail lexically. The result then has its Windows 8.3 short names expanded, so both
// sides of a comparison are spelled the same way.
// Returns false when the short-name expansion failed; `out` then holds the unexpanded canonical
// path, and a caller that must fail closed instead of comparing two spellings has to check it.
bool canonicalize_for_comparison(const boost::filesystem::path& path, boost::filesystem::path& out)
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
    return expand_short_name_prefix(canon, out);
}

// Best-effort form for checks that need one canonical path only, e.g. the denied-keyword scan:
// when the expansion fails the plain canonical spelling is used, as it was before it existed.
boost::filesystem::path canonical_for_comparison(const boost::filesystem::path& path)
{
    boost::filesystem::path out;
    canonicalize_for_comparison(path, out);
    return out;
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
// expected to come from canonicalize_for_comparison()/canonical_folder(), i.e. expanded to long
// names where the platform has 8.3 aliases. On a match, suffix receives the candidate's
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
    // Both sides are canonicalized identically, including the expansion of Windows 8.3 short
    // names, so a root registered under one spelling still contains a target reported under the
    // other. A side that cannot be normalized fails closed: comparing differently spelled paths
    // could otherwise report containment that does not hold.
    boost::filesystem::path canonical_candidate;
    if (!canonicalize_for_comparison(candidate, canonical_candidate))
        return false;

    // Fail closed on a dangling final symlink instead of comparing the path it is spelled as: the
    // grant describes the link, not the destination a write through it would create. The test is
    // applied to the canonical form, which leaves a dangling link unresolved as its final
    // component, so a trailing separator does not hide it either.
    if (ends_in_dangling_symlink(canonical_candidate))
        return false;

    boost::filesystem::path canonical_root;
    if (!canonicalize_for_comparison(allowed_root, canonical_root))
        return false;

    boost::filesystem::path suffix;
    return suffix_inside_root(canonical_candidate, canonical_root, suffix);
}

// ---------------------------------------------------------------------------
// ScopedPluginAuditContext
// ---------------------------------------------------------------------------

thread_local std::string PluginAuditManager::m_current_plugin_key = "";
thread_local std::string PluginAuditManager::m_current_capability_name = "";
thread_local std::vector<AllowedRoot> PluginAuditManager::m_scoped_allowed_roots;
thread_local std::vector<PluginAuditManager::RootFrame> PluginAuditManager::m_root_frames;
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
            std::vector<PluginAuditManager::RootFrame> frames = std::move(PluginAuditManager::m_root_frames);
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

// A live top-level window keeps the audit dialog ordered with the app and lets wx tear it down
// with its parent on shutdown. Null only before the main window exists or after it is gone; the
// callers check is_closing() first.
wxWindow* audit_prompt_parent()
{
    return wxTheApp != nullptr ? wxTheApp->GetTopWindow() : nullptr;
}

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

    PermissionDialog dialog(audit_prompt_parent(), message, title, show_tip);
    const int result = dialog.ShowModal();
    if (show_tip && config != nullptr && (result == wxID_YES || result == wxID_NO) && dialog.get_checkbox_state()) {
        try {
            config->set("hide_plugin_permission_tip", "1");
            config->save();
        } catch (const std::exception& ex) {
            BOOST_LOG_TRIVIAL(warning) << "[AUDIT PROMPT] Failed to save the permission-tip preference: " << ex.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(warning) << "[AUDIT PROMPT] Failed to save the permission-tip preference: unknown exception";
        }
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

// Defined below: the process-wide serialized prompt queue. Declared here because invalidating a
// plugin's recursive-delete state also drops its queued consents.
AuditPromptQueue& runtime_audit_prompts();

// A one-time consent for a recursive delete whose prompt could not run inline. It is deliberately
// in-memory only: no sidecar grant is written, so the approval cannot widen future delete access.
// The plugin key and canonical subtree root bind it to exactly one retry, and the short lifetime
// plus bounded count keep abandoned prompts from accumulating.
struct RecursiveDeleteApproval
{
    std::string                             plugin_key;
    std::string                             canonical_root;
    std::chrono::steady_clock::time_point   expires_at;
};

using RecursiveDeleteClock = std::chrono::steady_clock;

constexpr int                            kRecursiveDeleteApprovalLifetimeSeconds = 60;
std::mutex                               s_recursive_delete_approval_mutex;
std::vector<RecursiveDeleteApproval>     s_recursive_delete_approvals;
std::chrono::milliseconds                s_recursive_delete_approval_lifetime{kRecursiveDeleteApprovalLifetimeSeconds * 1000};
constexpr std::size_t                    kMaxRecursiveDeleteApprovals = 16;

// Prompts that have been posted but not yet answered. The key is the same plugin/target pair an
// approval would use, so a plugin retry loop cannot queue a second modal while the first is still
// open. The entry is cleared when the dialog is answered, or when posting is abandoned.
struct RecursiveDeletePromptKey
{
    std::string   plugin_key;
    std::string   canonical_root;
    std::uint64_t generation;
};

std::vector<RecursiveDeletePromptKey>    s_pending_recursive_delete_prompts;
// Bounds the recursive-delete consents that may be waiting for an answer at once, for both the
// test seam's pending set and the serialized production queue. A plugin retry loop can name many
// distinct targets; at capacity the new consent is rejected rather than queued.
constexpr std::size_t                    kMaxRecursiveDeletePrompts = 16;

// Incremented whenever a plugin's permission state is edited, revoked, or reinstalled. Posted
// dialog callbacks capture this value and must not store an approval after it changes.
std::unordered_map<std::string, std::uint64_t> s_recursive_delete_generations;

PluginAuditManager::RecursiveDeletePrompt s_recursive_delete_prompt_for_testing;
bool                                      s_recursive_delete_prompt_background_for_testing = false;
bool                                      s_recursive_delete_prompt_deferred_for_testing = false;

std::uint64_t recursive_delete_generation(const std::string& plugin_key)
{
    std::lock_guard<std::mutex> lock(s_recursive_delete_approval_mutex);
    const auto it = s_recursive_delete_generations.find(plugin_key);
    return it == s_recursive_delete_generations.end() ? 0 : it->second;
}

void invalidate_recursive_delete_state(const std::string& plugin_key)
{
    if (plugin_key.empty())
        return;

    {
        std::lock_guard<std::mutex> lock(s_recursive_delete_approval_mutex);
        ++s_recursive_delete_generations[plugin_key];

        // Any approval or dedup key was granted for the previous permission state. Erase both so
        // a later retry starts from the current state. A dialog already posted cannot be
        // withdrawn; its callback becomes a no-op through the generation token it captured.
        s_recursive_delete_approvals.erase(
            std::remove_if(s_recursive_delete_approvals.begin(), s_recursive_delete_approvals.end(),
                           [&plugin_key](const RecursiveDeleteApproval& approval) {
                               return approval.plugin_key == plugin_key;
                           }),
            s_recursive_delete_approvals.end());
        s_pending_recursive_delete_prompts.erase(
            std::remove_if(s_pending_recursive_delete_prompts.begin(), s_pending_recursive_delete_prompts.end(),
                           [&plugin_key](const RecursiveDeletePromptKey& prompt) {
                               return prompt.plugin_key == plugin_key;
                           }),
            s_pending_recursive_delete_prompts.end());
    }

    // Queued production consents for the old permission state are dropped too. An active dialog
    // cannot be withdrawn; the generation it captured makes its late answer a no-op.
    runtime_audit_prompts().discard_recursive_delete_consents(plugin_key);
}

// Caller holds s_recursive_delete_approval_mutex.
void prune_recursive_delete_approvals(const RecursiveDeleteClock::time_point& now)
{
    s_recursive_delete_approvals.erase(
        std::remove_if(s_recursive_delete_approvals.begin(), s_recursive_delete_approvals.end(),
                       [now](const RecursiveDeleteApproval& approval) { return approval.expires_at <= now; }),
        s_recursive_delete_approvals.end());
}

// True when no prompt for this plugin and target is already posted. The caller must pair a true
// result with finish_recursive_delete_prompt() after the dialog is answered.
bool begin_recursive_delete_prompt(const std::string& plugin_key, const std::string& canonical_root,
                                   std::uint64_t generation)
{
    std::lock_guard<std::mutex> lock(s_recursive_delete_approval_mutex);
    const auto same = [&plugin_key, &canonical_root](const RecursiveDeletePromptKey& prompt) {
        return prompt.plugin_key == plugin_key && prompt.canonical_root == canonical_root;
    };
    if (std::find_if(s_pending_recursive_delete_prompts.begin(), s_pending_recursive_delete_prompts.end(), same) !=
        s_pending_recursive_delete_prompts.end())
        return false;

    // At capacity, reject the new prompt instead of evicting an existing key: the evicted
    // dialog is already queued and cannot be withdrawn, so eviction would let repeated distinct
    // targets queue unlimited dialogs while losing the ability to deduplicate them.
    if (s_pending_recursive_delete_prompts.size() >= kMaxRecursiveDeletePrompts)
        return false;

    s_pending_recursive_delete_prompts.push_back({plugin_key, canonical_root, generation});
    return true;
}

void finish_recursive_delete_prompt(const std::string& plugin_key, const std::string& canonical_root,
                                    std::uint64_t generation)
{
    std::lock_guard<std::mutex> lock(s_recursive_delete_approval_mutex);
    s_pending_recursive_delete_prompts.erase(
        std::remove_if(s_pending_recursive_delete_prompts.begin(), s_pending_recursive_delete_prompts.end(),
                       [&plugin_key, &canonical_root, generation](const RecursiveDeletePromptKey& prompt) {
                           return prompt.plugin_key == plugin_key && prompt.canonical_root == canonical_root &&
                                  prompt.generation == generation;
                       }),
        s_pending_recursive_delete_prompts.end());
}

std::size_t pending_recursive_delete_prompts()
{
    std::lock_guard<std::mutex> lock(s_recursive_delete_approval_mutex);
    return s_pending_recursive_delete_prompts.size();
}

// Releases a pending-prompt dedup key on every exit path, including an exception thrown by the
// dialog or the test seam. A deferred test answer deliberately keeps the key and calls
// dismiss(); production callbacks just let the guard run.
class RecursiveDeletePromptGuard
{
public:
    RecursiveDeletePromptGuard(std::string plugin_key, std::string canonical_root, std::uint64_t generation)
        : m_plugin_key(std::move(plugin_key))
        , m_canonical_root(std::move(canonical_root))
        , m_generation(generation)
    {}
    ~RecursiveDeletePromptGuard()
    {
        if (m_active)
            finish_recursive_delete_prompt(m_plugin_key, m_canonical_root, m_generation);
    }

    void dismiss() { m_active = false; }

    RecursiveDeletePromptGuard(const RecursiveDeletePromptGuard&)            = delete;
    RecursiveDeletePromptGuard& operator=(const RecursiveDeletePromptGuard&) = delete;

private:
    std::string   m_plugin_key;
    std::string   m_canonical_root;
    std::uint64_t m_generation;
    bool          m_active = true;
};

void store_recursive_delete_approval(const std::string& plugin_key, const std::string& canonical_root)
{
    if (plugin_key.empty() || canonical_root.empty())
        return;

    std::lock_guard<std::mutex> lock(s_recursive_delete_approval_mutex);
    const RecursiveDeleteClock::time_point now = RecursiveDeleteClock::now();
    prune_recursive_delete_approvals(now);

    // Bound the store even when a user answers many prompts: the oldest pending consent is no
    // more useful than an expired one.
    if (s_recursive_delete_approvals.size() >= kMaxRecursiveDeleteApprovals)
        s_recursive_delete_approvals.erase(s_recursive_delete_approvals.begin());

    s_recursive_delete_approvals.push_back({plugin_key, canonical_root, now + s_recursive_delete_approval_lifetime});
}

bool take_recursive_delete_approval(const std::string& plugin_key, const std::string& canonical_root)
{
    if (plugin_key.empty() || canonical_root.empty())
        return false;

    std::lock_guard<std::mutex> lock(s_recursive_delete_approval_mutex);
    prune_recursive_delete_approvals(RecursiveDeleteClock::now());

    const auto found = std::find_if(s_recursive_delete_approvals.begin(), s_recursive_delete_approvals.end(),
                                    [&plugin_key, &canonical_root](const RecursiveDeleteApproval& approval) {
                                        return approval.plugin_key == plugin_key &&
                                               approval.canonical_root == canonical_root;
                                    });
    if (found == s_recursive_delete_approvals.end())
        return false;

    // Single use: the retry this consent authorised is the only one it can start.
    s_recursive_delete_approvals.erase(found);
    return true;
}

std::size_t pending_recursive_delete_approvals()
{
    std::lock_guard<std::mutex> lock(s_recursive_delete_approval_mutex);
    prune_recursive_delete_approvals(RecursiveDeleteClock::now());
    return s_recursive_delete_approvals.size();
}

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
template<typename Record>
void take_returned_frames(std::vector<Record>& held, std::vector<Record>& dropped)
{
    if (held.empty())
        return;

    const std::vector<PyObject*> stack = stack_frames();
    const auto running = [&stack](const Record& record) {
        PyObject* frame = record.frame;
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

// Network events name their target directly: a host, an address tuple or a URL.
static const std::unordered_map<std::string, Py_ssize_t> network_target_arg_indices{
    {"http.client.connect", 1},
    {"urllib.Request", 0},

    {"socket.connect", 1},
    {"socket.bind", 1},
    {"socket.getaddrinfo", 0},
    {"socket.gethostbyname", 0},
    {"socket.gethostbyaddr", 0},
    {"socket.getnameinfo", 0},
    {"socket.getservbyname", 0},
    {"socket.getservbyport", 0},
    {"socket.sendto", 1},
    {"socket.sendmsg", 1},
};

// Process events each expose the executable in a different argument, and some pass the whole
// command line or argv. A grant normally names the tool only, so the arguments are dropped; the
// exception is a Windows command line whose executable is unquoted and followed by arguments
// (see windows_command_line_target).
enum class ProcessTargetFormat {
    Path,               // the argument is the executable path or name
    FirstToken,         // a command string or argv sequence: its first token or element
    WindowsCommandLine, // a Windows command line: its first argument, possibly a quoted path
};

struct ProcessTargetSource {
    Py_ssize_t          index;
    ProcessTargetFormat format;
};

// An exact-command entry shares the process list with executable entries. This reserved prefix
// keeps the two namespaces apart: an exact command line can never be mistaken for an executable
// name, and an existing executable grant (which does not carry the prefix) keeps its
// argument-insensitive meaning. It is stored in the sidecar and listed by the Plugins dialog; the
// permission prompt shows the command line without it.
static const std::string kWindowsCommandLineGrantPrefix = "CommandLine:";

static bool is_exact_command_grant(const std::string& target)
{
    return boost::algorithm::starts_with(target, kWindowsCommandLineGrantPrefix);
}

static std::string exact_command_grant(const std::string& command_line)
{
    return kWindowsCommandLineGrantPrefix + command_line;
}

// The prompt shows the command line the user is asked about, not the storage marker.
static std::string process_target_for_display(const std::string& target)
{
    return is_exact_command_grant(target) ? target.substr(kWindowsCommandLineGrantPrefix.size()) : target;
}

// A process prompt can list executable grants and exact command lines together (a declaration may
// do so; a runtime event has one target). The wording must not claim argument-insensitive scope
// for an exact command line.
struct ProcessPromptScope {
    bool exact_command = false;
    bool executable    = false;
};

static ProcessPromptScope process_prompt_scope(const std::vector<std::string>& targets)
{
    ProcessPromptScope scope;
    for (const std::string& target : targets) {
        if (is_exact_command_grant(target))
            scope.exact_command = true;
        else
            scope.executable = true;
    }
    return scope;
}

// os.system does not run a program named by its first token: it hands the whole string to the
// platform shell. POSIX passes it to /bin/sh. The Windows C runtime runs the command interpreter
// named by COMSPEC, or cmd.exe when COMSPEC is unset or empty, and also falls back to cmd.exe
// when it cannot open the configured file (ENOENT or EACCES). Either interpreter may therefore
// run the command, so every one of them is a required target: a narrow grant for a harmless
// COMSPEC target must not admit the fallback shell. Approving the shells covers every command
// string the plugin passes to os.system, because a shell parses and executes all of the string,
// arguments included.
//
// The runtime chooses the interpreter when it runs the command, so the audit names the possible
// interpreters instead of checking whether the COMSPEC file exists: a file that is present while
// the prompt is answered can be removed before the call. A COMSPEC that already is the bare name
// cmd.exe, in any casing, names the fallback itself and is listed once.
#ifdef _WIN32
static std::vector<std::string> system_shell_targets()
{
    constexpr const char* kFallbackShell = "cmd.exe";

    // _wsystem reads COMSPEC through the CRT environment, which is the copy it consults; the
    // process environment block can differ. Read the same value and copy it at once, because a
    // concurrent _wputenv may replace the buffer the pointer names.
    const wchar_t* const comspec_value = ::_wgetenv(L"COMSPEC");
    // An unset or empty COMSPEC leaves only the runtime's default interpreter.
    if (comspec_value == nullptr || *comspec_value == L'\0')
        return {kFallbackShell};

    std::string comspec = wxString(comspec_value).utf8_string();
    // A bare cmd.exe is the fallback interpreter itself, whatever the casing, so one target stays.
    if (comspec.find_first_of("/\\") == std::string::npos &&
        wxString::FromUTF8(comspec).CmpNoCase(wxString::FromUTF8(kFallbackShell)) == 0)
        return {kFallbackShell};

    return {std::move(comspec), kFallbackShell};
}
#else
static std::vector<std::string> system_shell_targets()
{
    return {"/bin/sh"};
}
#endif

static const std::unordered_map<std::string, std::vector<ProcessTargetSource>> process_target_sources{
    {"os.exec",                    {{0, ProcessTargetFormat::Path}}},
    {"os.posix_spawn",             {{0, ProcessTargetFormat::Path}}},
    {"os.spawn",                   {{1, ProcessTargetFormat::Path}}},
    {"os.startfile",               {{0, ProcessTargetFormat::Path}}},
    {"os.startfile/2",             {{0, ProcessTargetFormat::Path}}},
    // executable is unresolved when the caller did not pass one, as on Windows, where args is
    // then a single command line; on POSIX args is an argv sequence and executable is always set.
    {"subprocess.Popen",           {{0, ProcessTargetFormat::Path}, {1, ProcessTargetFormat::WindowsCommandLine}}},
    // application_name wins when it is given, so its source comes first; when it is omitted
    // command_line is a Windows command line whose first argument names the executable.
    {"_winapi.CreateProcess",      {{0, ProcessTargetFormat::Path}, {1, ProcessTargetFormat::WindowsCommandLine}}},
    {"pty.spawn",                  {{0, ProcessTargetFormat::FirstToken}}},
    // The first argument is argv; its first element names the program.
    {"_posixsubprocess.fork_exec", {{0, ProcessTargetFormat::FirstToken}}},
};

AuditEventCategory open_category(PyObject* args)
{
    const std::string mode = python_unicode(tuple_item(args, 1));
    if (!mode.empty() && mode.find_first_of("wax+") == std::string::npos)
        return AuditEventCategory::FsRead;
    return AuditEventCategory::FsReadWrite;
}

// The first token of a command string, or the first element of an argv sequence. A sequence
// element is kept whole, so an executable path may contain spaces.
std::string first_process_token(PyObject* object)
{
    if (!object)
        return {};

    std::string text;
    if (python_text(object, text)) {
        const size_t start = text.find_first_not_of(" \t\r\n");
        if (start == std::string::npos)
            return {};
        const size_t end = text.find_first_of(" \t\r\n", start);
        return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }

    if (!PySequence_Check(object))
        return python_path(object);

    PyObject* first = PySequence_GetItem(object, 0); // new reference
    if (!first) {
        PyErr_Clear();
        return {};
    }
    std::string value = python_path(first);
    Py_DECREF(first);
    return value;
}

// The grant target for a Windows command line. Windows does not split it like a POSIX shell: a
// path that contains spaces is enclosed in double quotes, and backslashes before a quote follow
// the CommandLineToArgvW rules (2n backslashes and a quote give n backslashes and end the
// argument, 2n+1 give n backslashes and a literal quote).
//
// A quoted first argument names the executable unambiguously and is returned alone, as is a
// command line that is only the executable: both keep the argument-insensitive executable grant.
// An unquoted first argument followed by more text is different. CreateProcess resolves it by
// trying successively longer prefixes of the name, so the first whitespace-delimited token need
// not be the executable it starts (the command line "C:\Program Files\app.exe --x" can start
// "C:\Program Files\app.exe", not "C:\Program"). Such a command line is returned as one exact
// grant instead, because granting the first token would describe a different program and would
// cover every later command line with that token.
std::string windows_command_line_target(const std::string& command_line)
{
    size_t index = command_line.find_first_not_of(" \t");
    if (index == std::string::npos)
        return {};

    if (command_line[index] != '"') {
        const size_t end       = command_line.find_first_of(" \t", index);
        const size_t token_end = end == std::string::npos ? command_line.size() : end;
        if (command_line.find_first_not_of(" \t", token_end) != std::string::npos)
            return exact_command_grant(command_line.substr(index));
        return command_line.substr(index, token_end - index);
    }

    std::string token;
    ++index; // the opening quote is not part of the argument
    while (index < command_line.size()) {
        if (command_line[index] == '\\') {
            size_t backslashes = 0;
            while (index < command_line.size() && command_line[index] == '\\') {
                ++backslashes;
                ++index;
            }
            if (index < command_line.size() && command_line[index] == '"') {
                token.append(backslashes / 2, '\\');
                if (backslashes % 2) {
                    token.push_back('"');
                    ++index;
                } else {
                    ++index; // the closing quote
                    break;
                }
            } else {
                token.append(backslashes, '\\');
            }
            continue;
        }
        if (command_line[index] == '"') {
            ++index; // the closing quote
            break;
        }
        token.push_back(command_line[index]);
        ++index;
    }
    return token;
}

std::string process_target(const ProcessTargetSource& source, PyObject* args)
{
    PyObject* const item = tuple_item(args, source.index);
    if (!item || item == Py_None)
        return {};
    switch (source.format) {
    case ProcessTargetFormat::Path:
        return python_path(item);
    case ProcessTargetFormat::FirstToken:
        return first_process_token(item);
    case ProcessTargetFormat::WindowsCommandLine: {
        std::string command_line;
        if (python_text(item, command_line))
            return windows_command_line_target(command_line);
        return first_process_token(item);
    }
    }
    return {};
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
    case AuditEventCategory::ProcessCreate:
    case AuditEventCategory::ProcessReplace: {
        // The shells a command string may run in, not the command itself. Every possible
        // interpreter has to be granted before the event is allowed.
        if (event_name == "os.system")
            return system_shell_targets();
        const auto it = process_target_sources.find(event_name);
        if (it == process_target_sources.end())
            return targets;
        for (const ProcessTargetSource& source : it->second) {
            std::string value = process_target(source, args);
            if (!value.empty()) {
                targets.push_back(std::move(value));
                break;
            }
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

    const auto it = network_target_arg_indices.find(event_name);
    if (it != network_target_arg_indices.end()) {
        // None is an argument left out (sendmsg without an address), not a target named "None".
        PyObject* const item = tuple_item(args, it->second);
        if (item != Py_None) {
            std::string value = python_str(item);
            if (!value.empty())
                targets.push_back(std::move(value));
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

// The folder named by a tree grant, i.e. "<dir>/*", exactly as spelled. "*" is the only wildcard,
// and only as the grant's final path component; any other "*" is an ordinary character, so a
// grant can never widen into a pattern the plugin did not spell out. Returns false for a grant
// that is not a tree grant.
bool tree_grant_parent(const std::string& grant, boost::filesystem::path& parent)
{
    if (grant.size() < 2 || grant.back() != '*' || grant.find('*') != grant.size() - 1)
        return false;

    const boost::filesystem::path path(grant);
    if (path.filename() != "*")
        return false;

    const boost::filesystem::path root = path.parent_path();
    if (root.empty())
        return false;

    parent = root;
    return true;
}

// The directory named by a directory-tree grant, i.e. "<dir>/*". Unlike tree_grant_parent this is
// applied to a stored grant, so ".." is rejected rather than resolved: a grant shown to the user
// as "/a/../*" must not silently become the root or another ancestor. Without a wildcard the
// string still matches verbatim. Returns false for a grant that is not a tree grant.
bool tree_grant_root(const std::string& grant, boost::filesystem::path& root)
{
    boost::filesystem::path parent;
    if (!tree_grant_parent(grant, parent))
        return false;

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
// case-sensitive and treats "\" as an ordinary name character. 8.3 short names of the same file are
// unified on Windows; hardlinks and extended-length spellings remain distinct paths.
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

    // Expand 8.3 short names on both sides exactly like the create/delete comparisons, or a grant
    // saved under one spelling would not cover the same file named the other way and the user
    // would be asked again. Any expansion failure fails closed, as the canonicalization above does.
    fs::path expanded_grant;
    fs::path expanded_target;
    if (!expand_short_name_prefix(canonical_grant, expanded_grant) ||
        !expand_short_name_prefix(canonical_target, expanded_target))
        return false;

    return same_canonical_path(expanded_grant, expanded_target);
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

    // Both sides of every grant comparison come through here or through
    // is_inside_allowed_root(), so expanding Windows 8.3 short names keeps a stored grant
    // spelled one way comparable to a target spelled the other. Failing closed on an expansion
    // error keeps an unresolvable spelling from authorizing anything.
    if (!expand_short_name_prefix(canonical, out))
        return false;
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

// True when `target` names the approved subtree root itself, without resolving the final path
// component through a symlink. Deleting a link acts on the link entry, not on its destination, so
// an outside link pointing into the subtree must not be treated as the subtree root. The parent is
// canonicalized, which resolves intermediate symlinks and ".." with the operating system's
// semantics; the path is never lexically folded first, because "link/.." must follow the link
// before stepping up. The final component is compared by name -- with an 8.3 short name of a
// non-link entry expanded to its long spelling -- and a final ".." fails closed.
bool delete_entry_names_subtree_root(const std::string& target, const boost::filesystem::path& canonical_subtree)
{
    namespace fs = boost::filesystem;

    if (target.empty() || canonical_subtree.empty() || !canonical_subtree.is_absolute())
        return false;

    boost::system::error_code ec;
    const fs::path path(target);
    const fs::path absolute = path.is_absolute() ? path : fs::absolute(path, ec);
    if (ec || !absolute.is_absolute())
        return false;

    // Strip only trailing separators / "." components so a trailing slash still names the entry.
    // Do not lexically fold the remaining path: that would turn "<link>/.." into the link's
    // lexical parent instead of the parent of the link's destination.
    fs::path entry = absolute;
    while (!entry.empty() && (entry.filename() == "." || entry.filename().empty()) &&
           !entry.parent_path().empty())
        entry = entry.parent_path();

    if (entry.empty() || entry.filename() == "." || entry.filename() == "..")
        return false;

    const fs::path parent = fs::weakly_canonical(entry.parent_path(), ec);
    if (ec || !parent.is_absolute() || !fs::is_directory(parent, ec) || ec)
        return false;

    fs::path expanded_parent;
    if (!expand_short_name_prefix(parent, expanded_parent))
        return false;

    // The final component stays compared by name, so the expanded parent alone is enough for a link
    // entry: its destination is never followed. Spelling the component long is only safe when it is
    // not a link, because GetLongPathNameW's documentation does not promise to leave a link
    // unresolved; it is needed when a target names the root through the root's own 8.3 alias. A
    // missing entry has no alias and stays as written.
    const fs::path entry_path = expanded_parent / entry.filename();
    boost::system::error_code status_ec;
    const fs::file_status status = fs::symlink_status(entry_path, status_ec);
    if (status_ec || fs::is_symlink(status))
        return same_canonical_path(entry_path, canonical_subtree);

    fs::path expanded_entry;
    if (!expand_short_name_prefix(entry_path, expanded_entry))
        return false;

    return same_canonical_path(expanded_entry, canonical_subtree);
}

// True when deleting `target` acts on an entry inside `canonical_subtree`. The containing folder
// decides scope, so a symlink entry is judged by where it lives, not by what it points at. The one
// exception is the subtree root itself, recognised without following its final component.
bool delete_target_within_subtree(const std::string& target, const boost::filesystem::path& canonical_subtree)
{
    boost::filesystem::path folder;
    if (create_delete_target_folder(target, folder) && is_inside_allowed_root(folder, canonical_subtree))
        return true;

    return delete_entry_names_subtree_root(target, canonical_subtree);
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
// canonicalized and shown to the user before it is stored. Returns false for every shape
// tree_grant_parent rejects.
bool requested_tree_grant_root(const std::string& requested, boost::filesystem::path& root)
{
    return tree_grant_parent(requested, root);
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
                       bool tree_scope = false, bool root_tree_scope = false, bool exact_process_command = false,
                       bool executable_process_target = false)
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
            // A mixed target list may also hold exact folders: only the entries ending in /* or \*
            // cover subfolders, so the message cannot say that every listed folder does.
            case AuditEventCategory::FsCreate:
                return wxString::Format(_L_PLURAL("Plugin \"%s\" is requesting to create files in the following folder:\n%s\n\n"
                                                  "Approval covers creating entries directly inside the listed folder. "
                                                  "Only a target ending in /* or \\* also includes that folder's subfolders, at any depth. "
                                                  "An exact folder target covers the entries directly inside that folder only. "
                                                  "A target that is a filesystem, drive, or network share root, such as /*, "
                                                  "C:\\*, or \\\\server\\share\\*, covers that entire filesystem, drive, or network share.",
                                                  "Plugin \"%s\" is requesting to create files in the following folders:\n%s\n\n"
                                                  "Approval covers creating entries directly inside each listed folder. "
                                                  "Only targets ending in /* or \\* also include their folder's subfolders, at any depth. "
                                                  "Exact folder targets cover the entries directly inside that folder only. "
                                                  "A target that is a filesystem, drive, or network share root, such as /*, "
                                                  "C:\\*, or \\\\server\\share\\*, covers that entire filesystem, drive, or network share.",
                                                  target_count),
                                        plugin_name, target_list);
            case AuditEventCategory::FsDelete:
                return wxString::Format(_L_PLURAL("Plugin \"%s\" is requesting to delete files in the following folder:\n%s\n\n"
                                                  "Approval covers deleting entries directly inside the listed folder. "
                                                  "Only a target ending in /* or \\* also includes that folder's subfolders, at any depth. "
                                                  "An exact folder target covers the entries directly inside that folder only. "
                                                  "A target that is a filesystem, drive, or network share root, such as /*, "
                                                  "C:\\*, or \\\\server\\share\\*, covers that entire filesystem, drive, or network share.",
                                                  "Plugin \"%s\" is requesting to delete files in the following folders:\n%s\n\n"
                                                  "Approval covers deleting entries directly inside each listed folder. "
                                                  "Only targets ending in /* or \\* also include their folder's subfolders, at any depth. "
                                                  "Exact folder targets cover the entries directly inside that folder only. "
                                                  "A target that is a filesystem, drive, or network share root, such as /*, "
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
                                              "Only a target ending in /* or \\* also includes that folder's subfolders, at any depth. "
                                              "An exact folder target covers the entries directly inside that folder only.",
                                              "Plugin \"%s\" is requesting to create files in the following folders:\n%s\n\n"
                                              "Approval covers creating entries directly inside each listed folder. "
                                              "Only targets ending in /* or \\* also include their folder's subfolders, at any depth. "
                                              "Exact folder targets cover the entries directly inside that folder only.",
                                              target_count),
                                    plugin_name, target_list);
        case AuditEventCategory::FsDelete:
            return wxString::Format(_L_PLURAL("Plugin \"%s\" is requesting to delete files in the following folder:\n%s\n\n"
                                              "Approval covers deleting entries directly inside the listed folder. "
                                              "Only a target ending in /* or \\* also includes that folder's subfolders, at any depth. "
                                              "An exact folder target covers the entries directly inside that folder only.",
                                              "Plugin \"%s\" is requesting to delete files in the following folders:\n%s\n\n"
                                              "Approval covers deleting entries directly inside each listed folder. "
                                              "Only targets ending in /* or \\* also include their folder's subfolders, at any depth. "
                                              "Exact folder targets cover the entries directly inside that folder only.",
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
        // An exact command line is saved with its arguments rather than as a tool name, so the
        // prompt must not promise that approval covers any arguments. The full line is stored,
        // which can include credentials or tokens in its arguments.
        if (exact_process_command && !executable_process_target)
            return wxString::Format(_L_PLURAL("Plugin \"%s\" is requesting to run the following exact command line:\n%s\n\n"
                                              "Approval is saved for this exact command line, including its arguments, and covers "
                                              "only it. A different command line, including different arguments, asks again. The "
                                              "full command line is shown and saved, so it may contain sensitive values.",
                                              "Plugin \"%s\" is requesting to run the following exact command lines:\n%s\n\n"
                                              "Approval is saved for each exact command line, including its arguments, and covers "
                                              "only that line. A different command line, including different arguments, asks again. "
                                              "The full command lines are shown and saved, so they may contain sensitive values.",
                                              target_count),
                                    plugin_name, target_list);
        if (exact_process_command)
            return wxString::Format(_L_PLURAL("Plugin \"%s\" is requesting to run the following commands:\n%s\n\n"
                                              "Approval covers the listed executables with any arguments. A listed exact command "
                                              "line is saved with its arguments and covers only that command line. Exact command "
                                              "lines are shown and saved in full, so they may contain sensitive values. Approving a "
                                              "shell allows it to run any command.",
                                              "Plugin \"%s\" is requesting to run the following commands:\n%s\n\n"
                                              "Approval covers the listed executables with any arguments. Each listed exact command "
                                              "line is saved with its arguments and covers only that command line. Exact command "
                                              "lines are shown and saved in full, so they may contain sensitive values. Approving a "
                                              "shell allows it to run any command.",
                                              target_count),
                                    plugin_name, target_list);
        return wxString::Format(_L_PLURAL("Plugin \"%s\" is requesting to run the following command:\n%s\n\n"
                                          "Approval covers any arguments. Approving a shell allows it to run any command.",
                                          "Plugin \"%s\" is requesting to run the following commands:\n%s\n\n"
                                          "Approval covers any arguments. Approving a shell allows it to run any command.", target_count),
                                plugin_name, target_list);
    case AuditEventCategory::Threading:
        return wxString::Format(_L("Plugin \"%s\" is requesting permission to create a thread."), plugin_name);
    case AuditEventCategory::ProcessReplace:
        return wxString::Format(_L("Plugin \"%s\" is requesting to replace the running application with:\n%s\n\n"
                                   "Approval covers any arguments. Approving a shell allows it to run any command."), plugin_name, target_list);
    default:
        return wxString::Format(_L("Plugin \"%s\" is requesting permission for the Python audit event \"%s\"."), plugin_name, event_name);
    }
}

// Format with a placeholder first so translated messages can put the target list anywhere.
PermissionMessage formatted_audit_message(AuditEventCategory category, const wxString& plugin_name,
                                          const wxString& event_name, const wxString& targets, size_t count,
                                          bool full_fs_scope = false, bool tree_scope = false,
                                          bool root_tree_scope = false, bool exact_process_command = false,
                                          bool executable_process_target = false)
{
    if (targets.empty())
        return {audit_message(category, plugin_name, event_name, targets, count, full_fs_scope, tree_scope,
                              root_tree_scope, exact_process_command, executable_process_target), {}};

    wxString marker = "__ORCA_PERMISSION_TARGETS__";
    while (plugin_name.Contains(marker) || event_name.Contains(marker) || targets.Contains(marker))
        marker += "_";
    PermissionMessage message{
        audit_message(category, plugin_name, event_name, marker, count, full_fs_scope, tree_scope, root_tree_scope,
                      exact_process_command, executable_process_target), {}};
    const size_t pos = message.text.find(marker);
    if (pos != wxString::npos) {
        message.text.replace(pos, marker.length(), targets);
        message.targets.emplace_back(pos, targets.length());
    }
    return message;
}

// A runtime create/delete prompt shows both the path that triggered it and the folder a Yes
// saves, so the user can see why the plugin was stopped and what the persistent grant covers.
// The declaration prompt keeps showing only the requested folder scope.
PermissionMessage formatted_create_delete_message(AuditEventCategory category, const wxString& plugin_name,
                                                 const std::vector<PluginAuditManager::PermissionPromptEntry>& entries)
{
    // TRN %1% is the path the plugin tried to create or delete; %2% is the folder saved as a
    // grant if the user approves.
    const wxString entry_format = _L("Target: %1%\nSaved folder: %2%");
    wxString       target_list;
    for (const auto& entry : entries) {
        if (!target_list.IsEmpty())
            target_list += "\n";
        target_list += GUI::format_wxstr(entry_format, entry.target, entry.grant_folder);
    }

    wxString marker = "__ORCA_PERMISSION_TARGETS__";
    while (plugin_name.Contains(marker) || target_list.Contains(marker))
        marker += "_";

    wxString text;
    switch (category) {
    case AuditEventCategory::FsCreate:
        // TRN %1% is the plugin name; %2% is the list of targets, each with the folder saved as a
        // grant.
        text = GUI::format_wxstr(
            _L_PLURAL("Plugin \"%1%\" is requesting to create the following entry:\n%2%\n\n"
                      "If you approve, the folder shown with this target is saved as a grant for future "
                      "operations. The grant covers creating entries directly inside that folder. It does not "
                      "cover the subfolders inside that folder.",
                      "Plugin \"%1%\" is requesting to create the following entries:\n%2%\n\n"
                      "If you approve, the folder shown with each target is saved as a grant for future "
                      "operations. Each grant covers creating entries directly inside its folder. It does not "
                      "cover the subfolders inside that folder.",
                      entries.size()),
            plugin_name, marker);
        break;
    case AuditEventCategory::FsDelete:
        // TRN %1% is the plugin name; %2% is the list of targets, each with the folder saved as a
        // grant.
        text = GUI::format_wxstr(
            _L_PLURAL("Plugin \"%1%\" is requesting to delete the following entry:\n%2%\n\n"
                      "If you approve, the folder shown with this target is saved as a grant for future "
                      "operations. The grant covers deleting entries directly inside that folder. It does not "
                      "cover the subfolders inside that folder.",
                      "Plugin \"%1%\" is requesting to delete the following entries:\n%2%\n\n"
                      "If you approve, the folder shown with each target is saved as a grant for future "
                      "operations. Each grant covers deleting entries directly inside its folder. It does not "
                      "cover the subfolders inside that folder.",
                      entries.size()),
            plugin_name, marker);
        break;
    default:
        return {};
    }

    PermissionMessage message{text, {}};
    const size_t pos = message.text.find(marker);
    if (pos != wxString::npos) {
        message.text.replace(pos, marker.length(), target_list);
        message.targets.emplace_back(pos, target_list.length());
    }
    return message;
}

// Builds and shows the modal permission prompt. Must run on the GUI thread.
bool prompt_for_targets(AuditEventCategory category, const std::string& plugin_name, const std::string& event_name,
                        const std::vector<std::string>& unresolved,
                        const std::vector<PluginAuditManager::PermissionPromptEntry>& entries)
{
    if (is_create_delete_category(category) && !entries.empty()) {
        PermissionMessage message =
            formatted_create_delete_message(category, wxString::FromUTF8(plugin_name.c_str()), entries);
        return show_permission_dialog(message, _L("Plugin permission request"));
    }

    const bool process_category =
        category == AuditEventCategory::ProcessCreate || category == AuditEventCategory::ProcessReplace;
    const ProcessPromptScope process_scope =
        process_category ? process_prompt_scope(unresolved) : ProcessPromptScope{};

    wxString target_list;
    for (const auto& target : unresolved) {
        const std::string shown = process_category ? process_target_for_display(target) : target;
        target_list += wxString::FromUTF8(shown.c_str()) + "\n";
    }

    const bool tree_scope = std::any_of(unresolved.begin(), unresolved.end(), [](const std::string& target) {
        boost::filesystem::path root;
        return tree_grant_root(target, root);
    });

    PermissionMessage message = formatted_audit_message(category, wxString::FromUTF8(plugin_name.c_str()),
                                                         wxString::FromUTF8(event_name.c_str()), target_list,
                                                         unresolved.size(), /*full_fs_scope=*/false, tree_scope,
                                                         /*root_tree_scope=*/false, process_scope.exact_command,
                                                         process_scope.executable);
    return show_permission_dialog(message, _L("Plugin permission request"));
}

// Shows a prompt whose body may cover several permission buckets. Must run on the GUI thread.
bool prompt_for_permission_message(const PermissionMessage& message)
{
    return show_permission_dialog(message, _L("Plugin permissions"));
}

// Builds the combined body for a load-time declaration. Each bucket keeps its own formatted target
// block, so a translated sentence can place its target list independently. A create/delete
// bucket keeps the scope disclosure the declaration prompt has always shown: a sole "*" is
// unrestricted scope, and a tree grant ("<folder>/*") also covers every subfolder.
PermissionMessage build_permission_message(const std::string& plugin_name, const std::vector<AuditPromptBucket>& buckets)
{
    PermissionMessage message;
    for (const AuditPromptBucket& bucket : buckets) {
        const bool full_fs_scope = is_create_delete_category(bucket.category) && bucket.targets.size() == 1 &&
                                   bucket.targets.front() == "*";
        const bool tree_scope =
            is_create_delete_category(bucket.category) &&
            std::any_of(bucket.targets.begin(), bucket.targets.end(), [](const std::string& target) {
                boost::filesystem::path root;
                return tree_grant_root(target, root);
            });
        const bool root_tree_scope =
            tree_scope && std::any_of(bucket.targets.begin(), bucket.targets.end(), [](const std::string& target) {
                boost::filesystem::path root;
                return tree_grant_root(target, root) && is_filesystem_root(root);
            });

        const bool process_category =
            bucket.category == AuditEventCategory::ProcessCreate || bucket.category == AuditEventCategory::ProcessReplace;
        const ProcessPromptScope process_scope =
            process_category ? process_prompt_scope(bucket.targets) : ProcessPromptScope{};

        wxString targets;
        if (!full_fs_scope)
            for (const std::string& target : bucket.targets) {
                const std::string shown = process_category ? process_target_for_display(target) : target;
                targets += wxString::FromUTF8(shown.c_str()) + "\n";
            }

        if (!message.text.IsEmpty())
            message.text += "\n\n";
        PermissionMessage part = formatted_audit_message(bucket.category, wxString::FromUTF8(plugin_name.c_str()),
                                                          wxEmptyString, targets, bucket.targets.size(),
                                                          full_fs_scope, tree_scope, root_tree_scope,
                                                          process_scope.exact_command, process_scope.executable);
        for (const auto& range : part.targets)
            message.targets.emplace_back(message.text.length() + range.first, range.second);
        message.text += part.text;
    }
    return message;
}

// A recursive delete consent shows the canonical subtree root and states that the approval lasts
// only for this operation (or, when posted asynchronously, for one next matching attempt). That
// scope cannot be expressed by the persistent create/delete grant prompt, so it has its own
// complete message rather than a fragment appended to the folder prompt. A filesystem, drive, or
// network share root gets an additional sentence: deleting it covers everything on that root.
PermissionMessage formatted_recursive_delete_message(const wxString& plugin_name, const wxString& target,
                                                     bool root_scope, bool background)
{
    wxString marker = "__ORCA_RECURSIVE_DELETE_TARGET__";
    while (plugin_name.Contains(marker) || target.Contains(marker))
        marker += "_";

    // TRN %1% is the plugin name; %2% is the canonical folder whose subtree may be deleted.
    wxString format;
    if (root_scope) {
        format = background
                     ? _L("Plugin \"%1%\" is requesting to recursively delete the following folder:\n%2%\n\n"
                          "This target is a filesystem, drive, or network share root. Approval covers that entire "
                          "filesystem, drive, or network share, and applies to the next matching attempt within one "
                          "minute after you approve. The current attempt has already been denied. It is not saved as "
                          "a permission and cannot be reused for another operation. Protected paths stay blocked.")
                     : _L("Plugin \"%1%\" is requesting to recursively delete the following folder:\n%2%\n\n"
                          "This target is a filesystem, drive, or network share root. Approval covers that entire "
                          "filesystem, drive, or network share while this operation runs. It is not saved as a "
                          "permission and cannot be reused for another operation. Protected paths stay blocked.");
    } else {
        format = background
                     ? _L("Plugin \"%1%\" is requesting to recursively delete the following folder:\n%2%\n\n"
                          "Approval covers deleting the entries in this folder and everything inside it, and applies "
                          "to the next matching attempt within one minute after you approve. The current attempt has "
                          "already been denied. It is not saved as a permission and cannot be reused for another "
                          "operation. Protected paths stay blocked.")
                     : _L("Plugin \"%1%\" is requesting to recursively delete the following folder:\n%2%\n\n"
                          "Approval covers deleting the entries in this folder and everything inside it while this "
                          "operation runs. It is not saved as a permission and cannot be reused for another operation. "
                          "Protected paths stay blocked.");
    }
    wxString text = GUI::format_wxstr(format, plugin_name, marker);

    PermissionMessage message{text, {}};
    const size_t pos = message.text.find(marker);
    if (pos != wxString::npos) {
        message.text.replace(pos, marker.length(), target);
        message.targets.emplace_back(pos, target.length());
    }
    return message;
}

// Builds and shows the modal recursive-delete prompt. Must run on the GUI thread.
bool prompt_for_recursive_delete(const std::string& plugin_name, const std::string& target, bool root_scope,
                                 bool background)
{
    return show_permission_dialog(
        formatted_recursive_delete_message(wxString::FromUTF8(plugin_name.c_str()), wxString::FromUTF8(target.c_str()),
                                           root_scope, background),
        _L("Plugin permission request"));
}

// Defined below: the drain scheduler and the completion helper. A recursive-delete consent joins
// the same serialization as every other audit prompt.
bool schedule_runtime_prompt_drain_checked();
bool finish_audit_prompt(const std::shared_ptr<AuditPromptCompletion>& completion, bool satisfied);

// Asks for one-time consent to delete the canonical `target` subtree. On the wx main thread the
// answer is available before the operation returns, so a Yes lets this attempt proceed. Off that
// thread, or while another audit prompt is active, the modal is serialized through
// AuditPromptQueue and the current attempt is denied; a Yes records a short-lived single-use
// approval that only the same plugin and target can consume on a retry. A pending prompt for the
// same plugin/target is not queued again, so a plugin retry loop cannot flood the user with
// duplicate modals.
bool request_recursive_delete_consent(const std::string& plugin_key, const std::string& plugin_name,
                                      const std::string& target, bool root_scope)
{
    BOOST_LOG_TRIVIAL(info) << "[AUDIT PROMPT] plugin=" << plugin_key << " event=shutil.rmtree scope=subtree target=["
                            << target << "]";

    const std::uint64_t generation = recursive_delete_generation(plugin_key);

    if (s_recursive_delete_prompt_for_testing) {
        if (s_recursive_delete_prompt_background_for_testing) {
            if (!begin_recursive_delete_prompt(plugin_key, target, generation))
                return false;
            RecursiveDeletePromptGuard guard(plugin_key, target, generation);
            const bool granted = s_recursive_delete_prompt_for_testing(plugin_name, target);
            if (s_recursive_delete_prompt_deferred_for_testing) {
                guard.dismiss(); // answered later through answer_recursive_delete_prompt_for_testing
                return false;
            }
            if (granted && recursive_delete_generation(plugin_key) == generation)
                PluginAuditManager::instance().add_recursive_delete_approval(plugin_key, target);
            return false;
        }
        return s_recursive_delete_prompt_for_testing(plugin_name, target);
    }

    if (wxTheApp == nullptr || GUI::wxGetApp().is_closing() || PluginManager::instance().is_shutting_down() ||
        PluginManager::instance().is_plugin_load_cancelled(plugin_key))
        return false;

    // A recursive-delete consent joins the same serialized prompt queue as every other audit
    // prompt, so it cannot open a second dialog inside another permission dialog's modal loop.
    // On the GUI thread an idle queue shows it inline and its answer can authorize this attempt;
    // any queued request was raised off the GUI thread (or behind another prompt), so the current
    // attempt is denied and a Yes becomes a one-time retry approval.
    AuditPromptRequest request;
    request.kind                        = AuditPromptKind::RecursiveDeleteConsent;
    request.plugin_key                  = plugin_key;
    request.plugin_name                 = plugin_name;
    request.event_name                  = "shutil.rmtree";
    request.shown                       = {target};
    request.root_scope                  = root_scope;
    request.recursive_delete_generation = generation;

    const AuditPromptQueue::Admission admission = runtime_audit_prompts().admit(request, wxIsMainThread() != 0);
    if (admission == AuditPromptQueue::Admission::Duplicate || admission == AuditPromptQueue::Admission::Rejected)
        return false;
    if (admission == AuditPromptQueue::Admission::Queued) {
        schedule_runtime_prompt_drain_checked();
        return false;
    }

    // A permission edit may have landed between reading the generation and claiming the queue
    // slot. Do not show consent for a stale state; the retry will ask again with the new one.
    if (recursive_delete_generation(plugin_key) != generation)
        return finish_audit_prompt(nullptr, false);

    // ShowNow: no other audit prompt is active, so present this consent synchronously.
    bool granted = false;
    try {
        const bool alive = !PluginManager::instance().is_shutting_down() &&
                           !PluginManager::instance().is_plugin_load_cancelled(plugin_key);
        granted = alive && prompt_for_recursive_delete(plugin_name, target, root_scope, /*background=*/false);
    } catch (const std::exception& ex) {
        BOOST_LOG_TRIVIAL(error) << "[AUDIT PROMPT] Failed to show recursive-delete prompt for plugin=" << plugin_key
                                 << ": " << ex.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "[AUDIT PROMPT] Failed to show recursive-delete prompt for plugin=" << plugin_key
                                 << ": unknown exception";
    }
    return finish_audit_prompt(nullptr, granted && recursive_delete_generation(plugin_key) == generation);
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

// Persists every missing target of a declaration in one sidecar write. Takes the shared lock and
// re-reads the sidecar so a grant saved while the queued dialog was open is kept. The
// sidecar_was_readable flag records the status seen when the declaration was read: a sidecar that
// was readable then and is absent now must not be recreated, since that is lost state rather than
// a new install. Only a sidecar absent at both reads is seeded. Notifies an existing Plugins
// dialog when a write succeeded; a failed write leaves its Permissions tab accurate.
bool persist_declared_permissions(const std::string& plugin_key, const std::vector<AuditPromptBucket>& buckets,
                                  bool sidecar_was_readable)
{
    PluginDescriptor descriptor;
    if (!PluginManager::instance().try_get_plugin_descriptor(plugin_key, descriptor) || descriptor.plugin_root.empty())
        return false;

    bool persisted = false;
    {
        std::lock_guard<std::mutex> state_lock(install_state_mutex());

        PluginInstallState state;
        const boost::filesystem::path root(descriptor.plugin_root);
        const InstallStateReadStatus status = read_install_state_status(root, state);
        // A malformed file is never replaced, and a sidecar that disappeared after a readable read
        // is treated as lost state, not as a new install.
        if (status == InstallStateReadStatus::Unreadable)
            return false;
        if (status == InstallStateReadStatus::Absent && sidecar_was_readable)
            return false;

        seed_install_metadata(descriptor, state);
        for (const AuditPromptBucket& bucket : buckets) {
            if (bucket.category == AuditEventCategory::Threading) {
                state.permissions.threading = true;
                continue;
            }
            std::vector<std::string>* permission_list =
                permission_list_for(bucket.category, /*by_event=*/false, state.permissions);
            if (!permission_list)
                continue;
            if (is_create_delete_category(bucket.category)) {
                // A full grant added by another writer while the dialog was open already covers
                // every folder; adding a folder beside it would stop it reading as the full form.
                if (create_delete_grant_full(*permission_list))
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
                    create_delete_grant_covers_grant(*permission_list, target))
                    continue;
                if (std::find(permission_list->begin(), permission_list->end(), target) == permission_list->end())
                    permission_list->push_back(target);
            }
        }

        persisted = write_install_state(root, state);
    }

    // Notify outside the lock: the refresh is posted to the GUI thread, and a dialog must not wait
    // on a sidecar writer. A failed write changed nothing, so only a stored grant notifies.
    if (persisted)
        post_permission_changed_notification(plugin_key);
    return persisted;
}

static PluginAuditManager::PermissionPrompt s_permission_prompt_for_testing;
static PluginAuditManager::PermissionMessagePrompt s_permission_message_prompt_for_testing;

AuditPromptQueue& runtime_audit_prompts()
{
    // Deliberately leaked. A worker can be waiting on a queued prompt while static destructors
    // run; the queue itself must stay valid until process exit, and its destructor has no work
    // that is safe to order against wx teardown.
    static AuditPromptQueue* queue = new AuditPromptQueue();
    return *queue;
}

bool show_queued_prompt(const AuditPromptRequest& request)
{
    switch (request.kind) {
    case AuditPromptKind::RuntimeEvent:
        return prompt_for_targets(request.category, request.plugin_name, request.event_name, request.shown,
                                  request.entries);
    case AuditPromptKind::DeclaredPermissions:
        return prompt_for_permission_message(build_permission_message(request.plugin_name, request.buckets));
    case AuditPromptKind::RecursiveDeleteConsent:
        // The request is shown only after the attempt that raised it was denied, so the consent
        // must describe itself as applying to the next matching attempt.
        return !request.shown.empty() &&
               prompt_for_recursive_delete(request.plugin_name, request.shown.front(), request.root_scope,
                                           /*background=*/true);
    }
    return false;
}

void drain_runtime_prompts();

void schedule_runtime_prompt_drain()
{
    if (wxTheApp == nullptr || GUI::wxGetApp().is_closing()) {
        runtime_audit_prompts().discard_all();
        return;
    }
    GUI::wxGetApp().CallAfter([]() { drain_runtime_prompts(); });
}

// Schedules the next prompt. A failure here cannot be allowed to strand queued workers: discard
// everything still waiting and log why, so every completion is released with a denial.
bool schedule_runtime_prompt_drain_checked()
{
    try {
        schedule_runtime_prompt_drain();
        return true;
    } catch (const std::exception& ex) {
        BOOST_LOG_TRIVIAL(error) << "[AUDIT PROMPT] Failed to schedule the next prompt: " << ex.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "[AUDIT PROMPT] Failed to schedule the next prompt: unknown exception";
    }
    runtime_audit_prompts().discard_all();
    return false;
}

// Ends the active prompt exactly once, in queue order: complete first, then release the waiting
// worker, then schedule the next prompt. Used from normal and exceptional paths alike.
bool finish_audit_prompt(const std::shared_ptr<AuditPromptCompletion>& completion, bool satisfied)
{
    if (runtime_audit_prompts().complete())
        satisfied = false;
    if (completion)
        completion->resolve(satisfied);
    if (runtime_audit_prompts().has_pending())
        schedule_runtime_prompt_drain_checked();
    return satisfied;
}

// Shows the oldest queued prompt. The next one is scheduled only after this dialog has returned,
// so its modal event loop cannot deliver another prompt that opens a second dialog.
void drain_runtime_prompts()
{
    if (wxTheApp == nullptr || GUI::wxGetApp().is_closing()) {
        runtime_audit_prompts().discard_all();
        return;
    }

    std::optional<AuditPromptRequest> request = runtime_audit_prompts().take_next();
    if (!request)
        return;

    // A request may have been enqueued concurrently with plugin unload, after its queued
    // requests were discarded. Keep requests raised during plugin loading valid as well: plugin
    // code can run before the manager commits it as loaded. A cancelled load keeps its in-progress
    // slot until its worker unwinds, but an observed cancellation makes it ineligible for a new
    // prompt or a grant that has not begun committing; the queue cancels a dialog already up.
    // An ordinary load failure is different: its queued runtime prompt survives so the user can
    // approve the event whose denial aborted the load and then retry with the saved grant.
    PluginManager& manager = PluginManager::instance();
    const auto plugin_available = [&manager](const AuditPromptRequest& prompt) {
        AuditPromptPluginState state;
        state.loaded           = manager.is_plugin_loaded(prompt.plugin_key);
        state.load_in_progress = manager.is_plugin_load_in_progress(prompt.plugin_key);
        state.load_cancelled   = manager.is_plugin_load_cancelled(prompt.plugin_key);
        state.load_failed      = manager.is_plugin_load_failed(prompt.plugin_key);
        state.shutting_down    = manager.is_shutting_down();
        return prompt_plugin_is_available(state, prompt.kind);
    };

    bool satisfied = false;
    try {
        const bool alive = plugin_available(*request);
        // A permission edit racing the enqueue can leave a consent queued with the previous
        // generation. Do not show it: its Yes could not store anything, and a fresh retry should
        // be the prompt the user sees.
        const bool stale_consent =
            request->kind == AuditPromptKind::RecursiveDeleteConsent &&
            recursive_delete_generation(request->plugin_key) != request->recursive_delete_generation;
        const bool granted = alive && !stale_consent && show_queued_prompt(*request);

        // Decide under the still-active queue entry so a shutdown/unload that arrives before the
        // sidecar write cancels this request. begin_commit() makes an accepted answer atomic
        // against cancellation: if cancel wins, nothing is persisted; if commit wins, the answer is
        // valid and complete() cannot report it as cancelled afterwards.
        const bool alive_after_prompt = plugin_available(*request);
        satisfied = granted && alive_after_prompt && !PluginManager::instance().is_shutting_down() &&
                    runtime_audit_prompts().begin_commit();

        if (satisfied) {
            switch (request->kind) {
            case AuditPromptKind::RuntimeEvent:
                persist_grant(request->plugin_key, request->category, request->by_event, request->keys);
                break;
            case AuditPromptKind::DeclaredPermissions:
                satisfied = persist_declared_permissions(request->plugin_key, request->buckets,
                                                         request->sidecar_was_readable);
                if (!satisfied)
                    BOOST_LOG_TRIVIAL(warning) << "[AUDIT PROMPT] Failed to persist declared permissions for "
                                               << request->plugin_key;
                break;
            case AuditPromptKind::RecursiveDeleteConsent:
                // One-time consent is never persisted. A permission edit or reinstall while the
                // dialog was open invalidated the generation, so a stale Yes stores nothing.
                if (!request->shown.empty() &&
                    recursive_delete_generation(request->plugin_key) == request->recursive_delete_generation)
                    PluginAuditManager::instance().add_recursive_delete_approval(request->plugin_key,
                                                                                request->shown.front());
                else
                    satisfied = false;
                break;
            }
        }
    } catch (const std::exception& ex) {
        BOOST_LOG_TRIVIAL(error) << "[AUDIT PROMPT] Permission prompt failed for plugin=" << request->plugin_key
                                 << ": " << ex.what();
        satisfied = false;
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "[AUDIT PROMPT] Permission prompt failed for plugin=" << request->plugin_key
                                 << ": unknown exception";
        satisfied = false;
    }

    // While a commit is active, discards above leave it alone; finish_audit_prompt() completes
    // first, then resolves the worker, then schedules the next prompt.
    finish_audit_prompt(request->completion, satisfied);
}

void discard_runtime_prompts()
{
    runtime_audit_prompts().discard_all();
}

void discard_runtime_prompts_for(const std::string& plugin_key)
{
    runtime_audit_prompts().discard_plugin(plugin_key);
}

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
// pass the canonical parent folder of each target in `keys`, and `entries` additionally pairs
// every triggering target with that folder, so the prompt shows both the reason for the prompt
// and exactly what it saves.
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
// Runtime prompts are serialized by AuditPromptQueue: at most one is modal at a time, and the next
// is scheduled only after the active dialog has closed. An event delivered inside that dialog's
// nested event loop is queued and denied instead of opening a second dialog. Create and delete
// keep the request's target entries and persist the approved folder, so a queued approval still
// grants what a retry needs; only the inline GUI-thread prompt lets the current operation proceed.
bool request_permission(AuditEventCategory category, const std::string& plugin_key, const std::string& plugin_name,
                        const std::string& event_name, bool by_event, const std::vector<std::string>& keys,
                        const std::vector<PluginAuditManager::PermissionPromptEntry>& entries = {})
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
        return s_permission_prompt_for_testing(event_name, shown, entries);

    if (wxTheApp == nullptr || GUI::wxGetApp().is_closing() || PluginManager::instance().is_shutting_down() ||
        PluginManager::instance().is_plugin_load_cancelled(plugin_key))
        return false;

    const bool on_gui_thread = wxIsMainThread() != 0;

    AuditPromptRequest request;
    request.kind        = AuditPromptKind::RuntimeEvent;
    request.category    = category;
    request.plugin_key  = plugin_key;
    request.plugin_name = plugin_name;
    request.event_name  = event_name;
    request.by_event    = by_event;
    request.keys        = keys;
    request.shown       = shown;
    request.entries     = entries;

    const AuditPromptQueue::Admission admission = runtime_audit_prompts().admit(request, on_gui_thread);
    if (admission == AuditPromptQueue::Admission::Duplicate || admission == AuditPromptQueue::Admission::Rejected)
        return false;

    if (admission == AuditPromptQueue::Admission::Queued) {
        // A prompt is already active (this event arrived inside its modal loop) or an earlier one
        // is waiting. The current operation stays denied, as for an unanswered prompt; the
        // queued prompt is shown once the active dialog has fully closed, and a Yes persists the
        // same grant an inline answer would.
        schedule_runtime_prompt_drain_checked();
        return false;
    }

    // ShowNow: no other audit prompt is active, so present this one synchronously. The event is
    // approved only when the user says Yes to it and no unload cancelled it while it was up.
    bool granted = false;
    try {
        const bool alive = !PluginManager::instance().is_shutting_down() &&
                           !PluginManager::instance().is_plugin_load_cancelled(plugin_key);
        granted = alive && show_queued_prompt(request);
    } catch (const std::exception& ex) {
        BOOST_LOG_TRIVIAL(error) << "[AUDIT PROMPT] Failed to show permission prompt for plugin=" << plugin_key
                                 << ": " << ex.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "[AUDIT PROMPT] Failed to show permission prompt for plugin=" << plugin_key
                                 << ": unknown exception";
    }
    return finish_audit_prompt(request.completion, granted);
}

// Decides an event that no allowed root or running operation covers: a saved grant allows it,
// otherwise the user is asked. A Yes is saved for every category that has a persistent grant
// list, including create/delete, whose grant is the folder containing each approved target.
// Recursive deletes are the exception: their consent covers one subtree for one operation and
// is never written to the sidecar. `approved_delete_subtree` is set to that canonical subtree so
// the recorded root frame can cover its nested deletes.
int decide_audited_event(PluginAuditManager&             mgr,
                         PluginInstallState&              state,
                         const std::string&                plugin_key,
                         const std::string&                plugin_name,
                         const std::string&                event_name,
                         AuditEventCategory                category,
                         const std::vector<std::string>&   targets,
                         boost::filesystem::path&          approved_delete_subtree)
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

    // A recursive delete walks and deletes a whole subtree, so it is judged as a subtree
    // operation before the ordinary folder-grant check. A plain folder grant names direct
    // entries only: even one naming the target folder or its parent is insufficient. Saved tree
    // grants must cover both the entries inside the target and removal of the target folder
    // itself. Otherwise the user is asked for one-time consent for this canonical subtree before
    // the walk starts; that consent is never persisted.
    if (category == AuditEventCategory::FsDelete && event_name == "shutil.rmtree" && !by_event) {
        if (targets.size() != 1)
            return report_denied(mgr, event_name, {false, "recursive delete target cannot be resolved"});

        boost::filesystem::path root;
        if (!resolve_create_delete_folder(boost::filesystem::path(targets.front()), root))
            return report_denied(mgr, event_name, {false, "recursive delete target cannot be resolved"}, targets.front());

        const std::string canonical_root = root.string();
        const std::string subtree_grant  = (root / "*").string();
        const bool subtree_covered =
            permission_list &&
            (create_delete_grant_full(*permission_list) ||
             create_delete_grant_covers_grant(*permission_list, subtree_grant));
        if (subtree_covered) {
            // A tree grant over the target covers the entries inside it, not necessarily deleting
            // the target folder itself: a target-rooted "<target>/*" grant leaves the final
            // os.rmdir(target) to the containing folder of target. Only return here when that
            // removal is separately covered too; otherwise the one-time consent below has to
            // authorise the root removal explicitly before the walk starts.
            if (has_permission(AuditEventCategory::FsDelete, /*by_event=*/false, *permission_list, targets.front())) {
                // A matching consent is obsolete once saved grants cover the whole operation.
                mgr.consume_recursive_delete_approval(plugin_key, canonical_root);
                return 0;
            }
        }

        // A Yes posted by an earlier attempt is bound to this plugin and canonical target. Take
        // it before asking again; consuming removes it, so it cannot authorise a second run.
        if (mgr.consume_recursive_delete_approval(plugin_key, canonical_root)) {
            approved_delete_subtree = root;
            return 0;
        }

        // Letting a root frame go runs finalizers where no dialog may open: what they do must
        // already be granted.
        if (mgr.releasing_frames())
            return report_denied(mgr, event_name, {false, "recursive delete requires subtree permission"}, canonical_root);

        const bool root_scope = is_filesystem_root(root);
        if (!request_recursive_delete_consent(plugin_key, plugin_name, canonical_root, root_scope))
            return report_denied(mgr, event_name, {false, "recursive delete requires subtree permission"}, canonical_root);

        approved_delete_subtree = root;
        return 0;
    }

    // Create/delete grants name folders: an approval saves the folder that directly contains
    // each unresolved target, and the prompt shows both the triggering target and that canonical
    // folder -- including the absolute form of relative targets and the resolved form of
    // symlinked parents -- so the user approves exactly the scope that will be stored. Every
    // target has to be covered, and a target whose folder cannot be resolved fails closed.
    std::vector<std::string> unresolved;
    std::vector<PluginAuditManager::PermissionPromptEntry> prompt_entries;
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
            // Show every triggering target, even when several targets resolve to the same folder;
            // the folder list stays deduplicated because a Yes saves each folder once.
            prompt_entries.push_back({target, grant});
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

    if (!request_permission(category, plugin_key, plugin_name, event_name, by_event, unresolved, prompt_entries))
        return report_denied(mgr, event_name, {false, "audit permission required"});

    // persist_grant re-reads the sidecar under the shared lock and handles the threading
    // boolean; it notifies an existing Plugins dialog only when a write succeeded.
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
    // delete root (rmtree) does not cover nested deletes unless this operation was started with a
    // subtree-scoped delete (one-time consent or an implicit writable root). In that case a
    // nested delete is covered only while its target stays inside the approved subtree and the
    // stack between it and the root holds stdlib frames only; a tree grant can still allow a
    // nested delete on its own. Protected paths were already denied above, so they remain blocked
    // even inside an approved subtree.
    const bool approved_subtree_covers_delete =
        event_type == AuditEventCategory::FsDelete &&
        mgr.delete_target_inside_approved_subtree(permission_targets);
    if (event_name != "urllib.Request" &&
        (mgr.covered_by_root_frame(/*allow_delete_roots=*/event_type != AuditEventCategory::FsDelete) ||
         approved_subtree_covers_delete))
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
                if (event_type == AuditEventCategory::FsDelete) {
                    // Delete scope is the entry's containing folder (or the root itself), not the
                    // final symlink's destination. This both keeps an outside link entry outside
                    // its allowed root and lets a recursive delete remove a link that lives inside
                    // a writable root but points outside.
                    return mgr.delete_target_inside_writable_allowed_root(target);
                }
                return mgr.check_path_access(boost::filesystem::path(target), is_write).allowed;
            });
        if (all_inside_allowed_root) {
            // A recursive delete allowed by an implicit root still records its operation frame,
            // scoped to the target subtree. That covers the fd-based reads and os.open calls
            // rmtree performs (os.scandir(fd) exposes no path) and lets the walk remove symlink
            // entries without prompting, while nested deletes stay inside the target subtree.
            if (event_type == AuditEventCategory::FsDelete && event_name == "shutil.rmtree" &&
                permission_targets.size() == 1) {
                boost::filesystem::path implicit_subtree;
                if (resolve_create_delete_folder(boost::filesystem::path(permission_targets.front()), implicit_subtree))
                    mgr.record_root_frame(event_type, implicit_subtree);
            }
            return 0;
        }
    }

    PluginInstallState state;
    const bool have_install_state = PluginManager::instance().get_install_state(mgr.current_plugin(), state);
    if (!have_install_state) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << " Failed to get install state for " << mgr.current_plugin();
    }

    const std::string plugin_name = state.plugin_name.empty() ? mgr.current_plugin() : state.plugin_name;

    boost::filesystem::path approved_delete_subtree;
    const int result = decide_audited_event(mgr, state, mgr.current_plugin(), plugin_name, event_name, event_type,
                                            permission_targets, approved_delete_subtree);

    // A granted operation covers its nested events while it runs, except that a delete root does
    // not cover a nested delete unless a subtree-scoped delete was recorded on the frame (see the
    // coverage check above). A targetless grant names no operation, so it covers nothing else.
    if (result == 0 && !permission_targets.empty() && operation_root_events.count(event_name))
        mgr.record_root_frame(event_type, approved_delete_subtree);
    return result;
}

} // namespace PluginAuditDetail

bool PluginAuditManager::delete_target_inside_writable_allowed_root(const std::string& target) const
{
    if (target.empty())
        return false;

    // Deleting `target` acts on an entry in its containing folder, or on the root itself. Never
    // decide from the final component's symlink destination: an outside link pointing into a
    // writable root must not become an implicit delete of the outside entry.
    const auto inside_root = [&target](const AllowedRoot& root) {
        if (!root.allow_write)
            return false;
        boost::filesystem::path canonical_root;
        return PluginAuditDetail::canonical_folder(root.path, canonical_root) &&
               PluginAuditDetail::delete_target_within_subtree(target, canonical_root);
    };

    if (std::any_of(m_scoped_allowed_roots.begin(), m_scoped_allowed_roots.end(), inside_root))
        return true;

    std::lock_guard<std::mutex> lock(m_mutex);
    return std::any_of(m_global_allowed_roots.begin(), m_global_allowed_roots.end(), inside_root);
}

bool PluginAuditManager::consume_recursive_delete_approval(const std::string& plugin_key, const std::string& canonical_root)
{
    return PluginAuditDetail::take_recursive_delete_approval(plugin_key, canonical_root);
}

void PluginAuditManager::add_recursive_delete_approval(const std::string& plugin_key, const std::string& canonical_root)
{
    PluginAuditDetail::store_recursive_delete_approval(plugin_key, canonical_root);
}

AuditPromptQueue& runtime_prompt_queue_for_testing() { return PluginAuditDetail::runtime_audit_prompts(); }

bool prompt_plugin_is_available(const AuditPromptPluginState& state, AuditPromptKind kind)
{
    if (state.shutting_down || state.load_cancelled)
        return false;

    if (state.loaded || state.load_in_progress)
        return true;

    // No worker is left to answer a load-time declaration after its load failed. A runtime event
    // or a one-time recursive-delete consent has no worker: its denial may be what aborted the
    // load, so an ordinary failure keeps it actionable.
    return (kind == AuditPromptKind::RuntimeEvent || kind == AuditPromptKind::RecursiveDeleteConsent) &&
           state.load_failed;
}

bool AuditPromptBucket::operator==(const AuditPromptBucket& other) const
{
    return category == other.category && targets == other.targets;
}

void AuditPromptCompletion::resolve(bool value)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_resolved)
        return;
    m_resolved = true;
    m_promise.set_value(value);
}

std::future<bool> AuditPromptCompletion::get_future() { return m_promise.get_future(); }

AuditPromptQueue::Admission AuditPromptQueue::admit_synchronous(AuditPromptRequest request)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_active || !m_pending.empty())
        return Admission::Rejected;

    m_active = std::move(request);
    m_active_cancelled  = false;
    m_active_committing = false;
    return Admission::ShowNow;
}

AuditPromptQueue::Admission AuditPromptQueue::admit(AuditPromptRequest request, bool may_show_now)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (may_show_now && !m_active && m_pending.empty()) {
        m_active = std::move(request);
        m_active_cancelled  = false;
        m_active_committing = false;
        return Admission::ShowNow;
    }

    if (contains_locked(request))
        return Admission::Duplicate;

    // A recursive-delete consent is a modal per target, and a plugin retry loop can name many
    // distinct targets. Keep that set bounded, as the direct-prompt path did: at capacity, reject
    // the new consent instead of letting pending dialogs grow without limit. The count includes
    // the active prompt because its dialog is still on screen.
    if (request.kind == AuditPromptKind::RecursiveDeleteConsent) {
        const auto is_consent = [](const AuditPromptRequest& queued) {
            return queued.kind == AuditPromptKind::RecursiveDeleteConsent;
        };
        std::size_t consent_count = std::count_if(m_pending.begin(), m_pending.end(), is_consent);
        if (m_active && is_consent(*m_active))
            ++consent_count;
        if (consent_count >= PluginAuditDetail::kMaxRecursiveDeletePrompts)
            return Admission::Rejected;
    }

    m_pending.push_back(std::move(request));
    return Admission::Queued;
}

std::optional<AuditPromptRequest> AuditPromptQueue::take_next()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_active || m_pending.empty())
        return std::nullopt;

    m_active = std::move(m_pending.front());
    m_pending.pop_front();
    m_active_cancelled  = false;
    m_active_committing = false;
    return m_active;
}

bool AuditPromptQueue::begin_commit()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_active || m_active_cancelled)
        return false;
    m_active_committing = true;
    return true;
}

bool AuditPromptQueue::complete()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const bool cancelled = m_active_cancelled;
    m_active.reset();
    m_active_cancelled  = false;
    m_active_committing = false;
    return cancelled;
}

bool AuditPromptQueue::has_pending() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_pending.empty();
}

bool AuditPromptQueue::idle() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_active && m_pending.empty();
}

std::size_t AuditPromptQueue::pending_count() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pending.size();
}

void AuditPromptQueue::discard_all()
{
    const std::vector<std::shared_ptr<AuditPromptCompletion>> completions =
        discard_matching([](const AuditPromptRequest&) { return true; });

    // Never resolve under the queue lock: the wake-up is another thread that may re-enter.
    for (const auto& completion : completions)
        completion->resolve(false);
}

void AuditPromptQueue::discard_plugin(const std::string& plugin_key)
{
    const std::vector<std::shared_ptr<AuditPromptCompletion>> completions =
        discard_matching([&plugin_key](const AuditPromptRequest& request) { return request.plugin_key == plugin_key; });

    for (const auto& completion : completions)
        completion->resolve(false);
}

void AuditPromptQueue::discard_recursive_delete_consents(const std::string& plugin_key)
{
    // No worker ever waits on a consent, so the returned completions are always empty; ignore
    // them. The active consent, if any, is marked cancelled so its answer stores nothing.
    discard_matching([&plugin_key](const AuditPromptRequest& request) {
        return request.kind == AuditPromptKind::RecursiveDeleteConsent && request.plugin_key == plugin_key;
    });
}

std::vector<std::shared_ptr<AuditPromptCompletion>>
AuditPromptQueue::discard_matching(const std::function<bool(const AuditPromptRequest&)>& matches)
{
    std::vector<std::shared_ptr<AuditPromptCompletion>> completions;
    std::lock_guard<std::mutex>                         lock(m_mutex);

    // Collect before erasing so the workers are released oldest first.
    for (const AuditPromptRequest& request : m_pending)
        if (matches(request) && request.completion)
            completions.push_back(request.completion);
    m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(), matches), m_pending.end());

    // A commit already begun is left to finish, matching the active prompt's contract.
    if (m_active && matches(*m_active) && !m_active_committing) {
        m_active_cancelled = true;
        if (m_active->completion)
            completions.push_back(m_active->completion);
    }
    return completions;
}

bool AuditPromptQueue::same_request(const AuditPromptRequest& a, const AuditPromptRequest& b)
{
    if (a.kind != b.kind || a.plugin_key != b.plugin_key)
        return false;

    switch (a.kind) {
    case AuditPromptKind::RuntimeEvent:
        // The entries are what the dialog shows for a create/delete target, so two requests that
        // trigger on different paths are not equivalent even when they save the same folder.
        return a.category == b.category && a.by_event == b.by_event && a.keys == b.keys && a.shown == b.shown &&
               a.entries == b.entries;
    case AuditPromptKind::DeclaredPermissions:
        return a.buckets == b.buckets;
    case AuditPromptKind::RecursiveDeleteConsent:
        // One pending consent per plugin and canonical subtree within one permission generation.
        // A permission edit or revocation starts a new generation: the stale prompt's Yes is
        // ignored, and a retry after the edit must not be coalesced into that dead request.
        return a.shown == b.shown && a.recursive_delete_generation == b.recursive_delete_generation;
    }
    return false;
}

bool AuditPromptQueue::contains_locked(const AuditPromptRequest& request) const
{
    if (m_active && same_request(*m_active, request))
        return true;
    return std::any_of(m_pending.begin(), m_pending.end(),
                       [&request](const AuditPromptRequest& pending) { return same_request(pending, request); });
}

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
    std::vector<AuditPromptBucket> missing_buckets;

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
        AuditPromptBucket bucket{category, {}};
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
            AuditPromptBucket bucket{category, {}};
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

    // A cancelled load no longer represents a live plugin: do not ask, and do not save a grant
    // that the discarded load would otherwise leave behind. Re-checked after enqueue below.
    if (PluginManager::instance().is_plugin_load_cancelled(plugin_key))
        return false;

    if (PluginAuditDetail::s_permission_message_prompt_for_testing) {
        // Tests run without a wxApp to host the dialog: hand the seam the message it would show.
        try {
            PermissionMessage message = PluginAuditDetail::build_permission_message(descriptor.name, missing_buckets);
            append_permission_dialog_tip(message.text);
            if (!PluginAuditDetail::s_permission_message_prompt_for_testing(message.text.utf8_string()))
                return false;
            return PluginAuditDetail::persist_declared_permissions(plugin_key, missing_buckets,
                                                                  state_status == InstallStateReadStatus::Readable);
        } catch (const std::exception& ex) {
            BOOST_LOG_TRIVIAL(error) << "[AUDIT PROMPT] Failed to handle permission test seam for plugin=" << plugin_key
                                     << ": " << ex.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "[AUDIT PROMPT] Failed to handle permission test seam for plugin=" << plugin_key
                                     << ": unknown exception";
        }
        return false;
    }

    if (wxTheApp == nullptr || GUI::wxGetApp().is_closing() || PluginManager::instance().is_shutting_down() ||
        PluginManager::instance().is_plugin_load_cancelled(plugin_key))
        return false;

    const bool on_gui_thread = wxIsMainThread() != 0;

    const bool sidecar_was_readable = state_status == InstallStateReadStatus::Readable;

    AuditPromptRequest request;
    request.kind                 = AuditPromptKind::DeclaredPermissions;
    request.plugin_key           = plugin_key;
    request.plugin_name          = descriptor.name;
    request.buckets              = missing_buckets;
    request.sidecar_was_readable = sidecar_was_readable;

    // A load worker blocks on this completion. A GUI-thread caller cannot wait inside another
    // modal dialog, so it is admitted synchronously or rejected without leaving a deferred prompt
    // that could later grant a permission for a load that already failed.
    std::shared_ptr<AuditPromptCompletion> completion;
    std::future<bool>                      future;
    AuditPromptQueue::Admission            admission;
    if (on_gui_thread) {
        admission = PluginAuditDetail::runtime_audit_prompts().admit_synchronous(std::move(request));
    } else {
        completion = std::make_shared<AuditPromptCompletion>();
        future     = completion->get_future();
        request.completion = completion;
        admission = PluginAuditDetail::runtime_audit_prompts().admit(std::move(request), /*may_show_now=*/false);
    }

    if (admission == AuditPromptQueue::Admission::Duplicate || admission == AuditPromptQueue::Admission::Rejected)
        return false;

    if (admission == AuditPromptQueue::Admission::ShowNow) {
        bool satisfied = false;
        try {
            const bool alive = !PluginManager::instance().is_shutting_down() &&
                               !PluginManager::instance().is_plugin_load_cancelled(plugin_key);
            const bool granted =
                alive && PluginAuditDetail::prompt_for_permission_message(
                             PluginAuditDetail::build_permission_message(descriptor.name, missing_buckets));
            satisfied = granted && !PluginManager::instance().is_shutting_down() &&
                        !PluginManager::instance().is_plugin_load_cancelled(plugin_key) &&
                        PluginAuditDetail::runtime_audit_prompts().begin_commit();
            if (satisfied) {
                satisfied = PluginAuditDetail::persist_declared_permissions(
                    plugin_key, missing_buckets, sidecar_was_readable);
                if (!satisfied)
                    BOOST_LOG_TRIVIAL(warning) << "[AUDIT PROMPT] Failed to store declared permissions for plugin="
                                               << plugin_key;
            }
        } catch (const std::exception& ex) {
            BOOST_LOG_TRIVIAL(error) << "[AUDIT PROMPT] Failed to handle declared permissions for plugin=" << plugin_key
                                     << ": " << ex.what();
            satisfied = false;
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "[AUDIT PROMPT] Failed to handle declared permissions for plugin=" << plugin_key
                                     << ": unknown exception";
            satisfied = false;
        }
        return PluginAuditDetail::finish_audit_prompt(nullptr, satisfied);
    }

    // Queued: only a load worker reaches this branch. The GUI callback will answer, persist, and
    // release the worker.
    if (!PluginAuditDetail::schedule_runtime_prompt_drain_checked())
        return false;

    // A load that began before shutdown or was cancelled by unload must not wait forever on a
    // prompt that will not open. Re-check after enqueue so a discard racing this admission cannot
    // strand the worker, and a request admitted after unload's discard still learns it is dead.
    if (PluginManager::instance().is_shutting_down() ||
        PluginManager::instance().is_plugin_load_cancelled(plugin_key)) {
        PluginAuditDetail::discard_runtime_prompts_for(plugin_key);
        return false;
    }

    try {
        return future.get();
    } catch (const std::exception& ex) {
        BOOST_LOG_TRIVIAL(error) << "[AUDIT PROMPT] Permission wait failed for plugin=" << plugin_key << ": " << ex.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "[AUDIT PROMPT] Permission wait failed for plugin=" << plugin_key
                                 << ": unknown exception";
    }
    PluginAuditDetail::discard_runtime_prompts_for(plugin_key);
    return false;
}

bool PluginAuditManager::is_create_delete_tree_grant(const std::string& target)
{
    boost::filesystem::path root;
    return PluginAuditDetail::requested_tree_grant_root(target, root);
}

void PluginAuditManager::record_root_frame(AuditEventCategory category,
                                           const boost::filesystem::path& approved_delete_subtree)
{
    PyObject* frame = reinterpret_cast<PyObject*>(PyEval_GetFrame()); // borrowed reference
    if (!frame || !PluginAuditDetail::is_stdlib_frame(frame))
        return;

    const bool delete_root = category == AuditEventCategory::FsDelete;
    auto       it          = std::find_if(m_root_frames.begin(), m_root_frames.end(),
                                          [frame](const RootFrame& record) { return record.frame == frame; });
    if (it == m_root_frames.end()) {
        Py_INCREF(frame);
        m_root_frames.push_back({frame, delete_root, approved_delete_subtree});
        return;
    }
    // The record owns the one reference; the mark is sticky, so a frame first recorded as a
    // read/write root still stops covering nested deletes once it is also a delete root.
    if (delete_root)
        it->delete_root = true;
    // Keep a subtree consent already recorded on this frame; a later grant does not widen it.
    if (!approved_delete_subtree.empty())
        it->approved_delete_subtree = approved_delete_subtree;
}

bool PluginAuditManager::covered_by_root_frame(bool allow_delete_roots) const
{
    if (m_root_frames.empty())
        return false;

    // Only stdlib frames may sit between the event and the operation: plugin or library code
    // the operation calls back into (an os.walk onerror, a urllib handler) acts on its own.
    for (PyObject* frame : PluginAuditDetail::stack_frames()) {
        const auto root = std::find_if(m_root_frames.begin(), m_root_frames.end(),
                                       [frame](const RootFrame& record) { return record.frame == frame; });
        if (root != m_root_frames.end()) {
            // A delete root does not cover a nested delete by itself: an exact folder grant
            // covers direct entries only, while an outer read/write root may still cover the
            // same operation, and a tree grant may allow it when the event is judged. A delete
            // root started with one-time subtree consent is handled by
            // delete_target_inside_approved_subtree(), which also checks the target's scope.
            if (!root->delete_root || allow_delete_roots)
                return true;
            continue;
        }
        if (!PluginAuditDetail::is_stdlib_frame(frame))
            return false;
    }
    return false;
}

bool PluginAuditManager::delete_target_inside_approved_subtree(const std::vector<std::string>& targets) const
{
    if (m_root_frames.empty() || targets.empty())
        return false;

    // Same frame walk as covered_by_root_frame: stop at plugin or third-party code, so a callback
    // the operation invokes is judged on its own. The first delete root carrying a one-time
    // consent is the operation boundary; only its canonical subtree covers the nested deletes.
    for (PyObject* frame : PluginAuditDetail::stack_frames()) {
        const auto root = std::find_if(m_root_frames.begin(), m_root_frames.end(),
                                       [frame](const RootFrame& record) { return record.frame == frame; });
        if (root == m_root_frames.end()) {
            if (!PluginAuditDetail::is_stdlib_frame(frame))
                break;
            continue;
        }

        if (!root->delete_root || root->approved_delete_subtree.empty())
            continue; // keep looking for an outer operation with subtree consent

        const boost::filesystem::path& approved_subtree = root->approved_delete_subtree;
        for (const std::string& target : targets) {
            if (!PluginAuditDetail::delete_target_within_subtree(target, approved_subtree))
                return false;
        }
        return true;
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

void PluginAuditManager::release_frames(std::vector<RootFrame>& frames)
{
    if (frames.empty())
        return;

    // A finalizer's own denial must neither replace the PermissionError of the event being
    // decided nor leave a stale denial behind for the next call.
    PyObject* const      error          = PyErr_GetRaisedException();
    const bool           denial_pending = m_audit_denial_pending;
    const bool           has_violation  = m_has_last_violation;
    const AuditViolation violation      = m_last_violation;

    std::vector<RootFrame> released;
    released.swap(frames);
    ++m_releasing_frames;
    for (const RootFrame& record : released)
        Py_DECREF(record.frame);
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

void PluginAuditManager::set_recursive_delete_prompt_for_testing(RecursiveDeletePrompt prompt)
{
    PluginAuditDetail::s_recursive_delete_prompt_for_testing = std::move(prompt);
}

void PluginAuditManager::set_recursive_delete_prompt_background_for_testing(bool background)
{
    PluginAuditDetail::s_recursive_delete_prompt_background_for_testing = background;
}

std::string PluginAuditManager::format_recursive_delete_permission_prompt_for_testing(const std::string& plugin_name,
                                                                                       const std::string& target,
                                                                                       bool root_scope,
                                                                                       bool background)
{
    return PluginAuditDetail::formatted_recursive_delete_message(wxString::FromUTF8(plugin_name.c_str()),
                                                                wxString::FromUTF8(target.c_str()), root_scope,
                                                                background)
        .text.utf8_string();
}

void PluginAuditManager::set_recursive_delete_prompt_deferred_for_testing(bool deferred)
{
    PluginAuditDetail::s_recursive_delete_prompt_deferred_for_testing = deferred;
}

std::uint64_t PluginAuditManager::recursive_delete_generation_for_testing(const std::string& plugin_key)
{
    return PluginAuditDetail::recursive_delete_generation(plugin_key);
}

void PluginAuditManager::answer_recursive_delete_prompt_for_testing(const std::string& plugin_key,
                                                                     const std::string& canonical_root, bool granted,
                                                                     std::uint64_t generation)
{
    PluginAuditDetail::finish_recursive_delete_prompt(plugin_key, canonical_root, generation);
    if (granted && PluginAuditDetail::recursive_delete_generation(plugin_key) == generation)
        PluginAuditDetail::store_recursive_delete_approval(plugin_key, canonical_root);
}

void PluginAuditManager::invalidate_recursive_delete_state(const std::string& plugin_key)
{
    PluginAuditDetail::invalidate_recursive_delete_state(plugin_key);
}

std::size_t PluginAuditManager::pending_recursive_delete_prompts_for_testing()
{
    return PluginAuditDetail::pending_recursive_delete_prompts();
}

bool PluginAuditManager::delete_target_inside_subtree_for_testing(const std::string& target,
                                                                  const std::string& canonical_subtree)
{
    boost::filesystem::path subtree;
    if (!PluginAuditDetail::canonical_folder(boost::filesystem::path(canonical_subtree), subtree))
        return false;
    return PluginAuditDetail::delete_target_within_subtree(target, subtree);
}

void PluginAuditManager::clear_recursive_delete_approvals_for_testing()
{
    std::lock_guard<std::mutex> lock(PluginAuditDetail::s_recursive_delete_approval_mutex);
    PluginAuditDetail::s_recursive_delete_approvals.clear();
    PluginAuditDetail::s_pending_recursive_delete_prompts.clear();
}

void PluginAuditManager::set_recursive_delete_approval_lifetime_for_testing(std::chrono::milliseconds lifetime)
{
    std::lock_guard<std::mutex> lock(PluginAuditDetail::s_recursive_delete_approval_mutex);
    PluginAuditDetail::s_recursive_delete_approval_lifetime = lifetime;
}

std::size_t PluginAuditManager::pending_recursive_delete_approvals_for_testing()
{
    return PluginAuditDetail::pending_recursive_delete_approvals();
}

std::string PluginAuditManager::format_create_delete_permission_prompt_for_testing(
    AuditEventCategory category, const std::string& plugin_name, const std::vector<PermissionPromptEntry>& entries)
{
    return PluginAuditDetail::formatted_create_delete_message(category, wxString::FromUTF8(plugin_name.c_str()), entries)
        .text.utf8_string();
}

void PluginAuditManager::set_permission_message_prompt_for_testing(PermissionMessagePrompt prompt)
{
    PluginAuditDetail::s_permission_message_prompt_for_testing = std::move(prompt);
}

void PluginAuditManager::discard_pending_permission_prompts()
{
    PluginAuditDetail::discard_runtime_prompts();
}

void PluginAuditManager::discard_pending_permission_prompts_for(const std::string& plugin_key)
{
    PluginAuditDetail::discard_runtime_prompts_for(plugin_key);
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
    std::vector<RootFrame> returned_frames;
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
