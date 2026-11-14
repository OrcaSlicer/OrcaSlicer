#include "UpdateHandoff.hpp"

#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/directory.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <exception>
#include <ios>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <boost/nowide/convert.hpp>
#include <boost/nowide/fstream.hpp>

#ifdef _WIN32
#include <boost/nowide/cstdlib.hpp>
#endif

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

#include "Utils.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace Slic3r {

namespace {

namespace fs = boost::filesystem;
using nlohmann::json;

std::string text_field(const json& object, const char* key)
{
    auto it = object.find(key);
    if (it == object.end() || !it->is_string())
        return std::string();
    return it->get<std::string>();
}

int64_t integer_field(const json& object, const char* key)
{
    auto it = object.find(key);
    if (it == object.end() || !it->is_number())
        return 0;
    return it->get<int64_t>();
}

std::string& cache_dir_override()
{
    static std::string dir;
    return dir;
}

// The OS cache directory, resolved without wx: the application resolves the
// cache with wx and passes it in the handoff, so this is only reached by tests and
// by a helper started without one. Nothing here may fall back to the data
// directory, which the updater is not supposed to write to.
std::string os_cache_root()
{
#ifdef _WIN32
    // nowide: a profile path such as C:\Users\José\AppData is not representable in the
    // ANSI bytes std::getenv returns, and every path the updater handles is UTF-8.
    if (const char* local = boost::nowide::getenv("LOCALAPPDATA"); local != nullptr && local[0] != '\0')
        return (fs::path(local) / "OrcaSlicer" / "updater").string();
#elif defined __APPLE__
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
        return (fs::path(home) / "Library" / "Caches" / "OrcaSlicer" / "updater").string();
#else
    if (const char* cache = std::getenv("XDG_CACHE_HOME"); cache != nullptr && cache[0] != '\0')
        return (fs::path(cache) / "OrcaSlicer" / "updater").string();
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
        return (fs::path(home) / ".cache" / "OrcaSlicer" / "updater").string();
#endif
    boost::system::error_code ec;
    const fs::path          temp = fs::temp_directory_path(ec);
    if (!ec)
        return (temp / "OrcaSlicer" / "updater").string();
    return "OrcaSlicer-updater";
}

std::string cache_root()
{
    const std::string& override_dir = cache_dir_override();
    if (!override_dir.empty())
        return override_dir;
    return os_cache_root();
}

void ensure_parent_dir(const std::string& path)
{
    const fs::path parent = fs::path(path).parent_path();
    if (parent.empty())
        return;
    boost::system::error_code ec;
    fs::create_directories(parent, ec);
}

bool read_file(const std::string& path, std::string& out)
{
    boost::nowide::ifstream input(path, std::ios::binary);
    if (!input)
        return false;
    out.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return true;
}

bool write_file_atomic(const std::string& path, const std::string& payload)
{
    ensure_parent_dir(path);
    return !write_file_atomically(path, payload, /*binary=*/true);
}

void remove_file(const std::string& path)
{
    boost::system::error_code ec;
    fs::remove(path, ec);
}

bool is_plain_path_segment(const std::string& segment)
{
    if (segment.empty() || segment == "." || segment == "..")
        return false;
    return std::all_of(segment.begin(), segment.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
               c == '+' || c == '-';
    });
}

#ifdef _WIN32
using LockHandle = HANDLE;
#else
using LockHandle = int;
#endif

// The install locks this process holds, by path. A lock lives as long as its open
// handle, so the OS drops it when the holder exits, even if it crashed.
std::map<std::string, LockHandle>& held_install_locks()
{
    static std::map<std::string, LockHandle> locks;
    return locks;
}

std::mutex& held_install_locks_mutex()
{
    static std::mutex mutex;
    return mutex;
}

void close_lock_file(LockHandle handle)
{
#ifdef _WIN32
    ::CloseHandle(handle);
#else
    ::close(handle);
#endif
}

// Takes the exclusive lock without waiting. False when another handle holds the lock.
// The file stays empty: a held range lock denies reads through any other handle on
// Windows, so a pid written there could not be read while the lock is held anyway.
bool open_lock_file(const std::string& path, LockHandle& out)
{
#ifdef _WIN32
    const HANDLE handle = ::CreateFileW(boost::nowide::widen(path).c_str(), GENERIC_READ | GENERIC_WRITE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    // A range beyond the end of the file is a valid lock.
    OVERLAPPED overlapped = {};
    if (!::LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped)) {
        ::CloseHandle(handle);
        return false;
    }
    // A file left behind by an older build still holds its owner pid; empty it.
    ::SetFilePointer(handle, 0, nullptr, FILE_BEGIN);
    ::SetEndOfFile(handle);
    out = handle;
    return true;
#else
    const int fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0)
        return false;
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return false;
    }
    // The file may have been removed between open and flock. A lock on that file excludes nobody.
    struct stat opened  = {};
    struct stat current = {};
    if (::fstat(fd, &opened) != 0 || ::stat(path.c_str(), &current) != 0 || opened.st_dev != current.st_dev ||
        opened.st_ino != current.st_ino) {
        ::close(fd);
        return false;
    }
    if (::ftruncate(fd, 0) != 0) {
        ::close(fd);
        return false;
    }
    out = fd;
    return true;
#endif
}

bool read_install_record_file(InstallRecord& out)
{
    std::string text;
    if (!read_file(updater_install_record_path(), text))
        return false;
    return install_record_from_json(text, out);
}

} // namespace

std::string updater_cache_dir()
{
    return cache_root();
}

void set_updater_cache_dir(const std::string& dir)
{
    cache_dir_override() = dir;
}

void clear_updater_cache_dir()
{
    cache_dir_override().clear();
}

std::string updater_download_dir()
{
    return (fs::path(cache_root()) / "downloads").string();
}

std::string updater_helper_dir()
{
    return (fs::path(cache_root()) / "helper").string();
}

std::string updater_handoff_path()
{
    return (fs::path(cache_root()) / "handoff.json").string();
}

std::string updater_install_record_path()
{
    return (fs::path(cache_root()) / "installed.json").string();
}

std::string updater_pending_path()
{
    return (fs::path(cache_root()) / "pending.json").string();
}

std::string updater_lock_path()
{
    return (fs::path(cache_root()) / "install.lock").string();
}

std::string updater_log_path()
{
    return (fs::path(cache_root()) / "updater.log").string();
}

std::string updater_failure_path()
{
    return (fs::path(cache_root()) / "update_failed.txt").string();
}

bool write_update_failure(const std::string& message)
{
    return write_file_atomic(updater_failure_path(), message);
}

bool read_update_failure(std::string& message)
{
    return read_file(updater_failure_path(), message) && !message.empty();
}

void remove_update_failure()
{
    remove_file(updater_failure_path());
}

std::string install_identity_key(const std::string& install_root)
{
    std::string normalised = install_root;
    while (normalised.size() > 1 && (normalised.back() == '/' || normalised.back() == '\\'))
        normalised.pop_back();
#ifdef _WIN32
    for (char& c : normalised)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
#endif
    uint64_t hash = 14695981039346656037ull;
    for (const char c : normalised) {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
        hash *= 1099511628211ull;
    }
    static const char* const hex = "0123456789abcdef";
    std::string              key = "installed_artifact:";
    for (int shift = 60; shift >= 0; shift -= 4)
        key.push_back(hex[(hash >> shift) & 0xfu]);
    return key;
}

DownloadPrep prepare_download_dir(const std::string& dir,
                                  const std::string& asset_name,
                                  const std::string& digest,
                                  int64_t            expected_size)
{
    DownloadPrep      prep;
    const fs::path    directory(dir);
    const std::string name   = fs::path(asset_name).filename().string();
    const std::string keep   = name.empty() ? std::string("update.part") : name;
    const fs::path    part   = directory / (keep + ".part");
    const fs::path    marker = directory / (keep + ".part.digest");

    boost::system::error_code ec;
    if (!fs::is_directory(directory, ec))
        fs::create_directories(directory, ec);

    // A .part is resumed only for the build it was started for, and only while it is
    // smaller than that build: anything else is a leftover.
    std::string recorded;
    const bool  have_marker = read_file(marker.string(), recorded);
    while (!recorded.empty() && (recorded.back() == '\n' || recorded.back() == '\r' || recorded.back() == ' '))
        recorded.pop_back();
    if (!digest.empty() && fs::is_regular_file(part, ec) && have_marker) {
        prep.resumable = recorded == digest;
        if (prep.resumable && expected_size > 0) {
            const auto size = fs::file_size(part, ec);
            if (!ec) {
                prep.complete  = static_cast<int64_t>(size) == expected_size;
                prep.resumable = static_cast<int64_t>(size) < expected_size;
            }
        }
    }

    // The helper renames a verified download to its asset name. It is kept while it is this
    // build's, so a prefetch that is never installed does not download the artifact again.
    const fs::path artifact = directory / keep;
    if (!prep.complete && !digest.empty() && expected_size > 0 && recorded == digest) {
        const auto size = fs::file_size(artifact, ec);
        prep.complete   = !ec && static_cast<int64_t>(size) == expected_size;
    }

    // Checked before this, so the marker of a verified artifact is not taken for a leftover.
    if (!prep.resumable && !prep.complete) {
        for (const fs::path& stale : {part, marker})
            if (fs::is_regular_file(stale, ec) && fs::remove(stale, ec))
                prep.dropped.push_back(stale.string());
    }

    // Whatever else sits here belongs to another release.
    for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path entry = it->path();
        if (!fs::is_regular_file(entry, ec))
            continue;
        if (entry == part || entry == marker || (prep.complete && entry == artifact))
            continue;
        if (fs::remove(entry, ec))
            prep.dropped.push_back(entry.string());
    }

    // Recorded before the download, so a later run knows which build the .part belongs to.
    // Not while an old .part survives: the download would resume on top of it.
    if (!prep.complete && !fs::exists(part, ec))
        (void) write_file_atomic(marker.string(), digest);
    return prep;
}

bool is_release_download_url(const std::string& url, const std::string& asset_name)
{
    static const std::string prefix = "https://github.com/OrcaSlicer/OrcaSlicer/releases/download/";
    if (asset_name.empty() || url.compare(0, prefix.size(), prefix) != 0)
        return false;

    // Exactly <tag>/<asset_name>: no other path segment, query, fragment or escape can
    // reach a different repository path once curl normalizes the URL.
    const std::string rest  = url.substr(prefix.size());
    const std::string::size_type slash = rest.find('/');
    if (slash == std::string::npos || rest.find('/', slash + 1) != std::string::npos)
        return false;
    const std::string tag  = rest.substr(0, slash);
    const std::string name = rest.substr(slash + 1);
    return name == asset_name && is_plain_path_segment(tag) && is_plain_path_segment(name);
}

std::string handoff_to_json(const UpdateHandoff& handoff)
{
    json j;
    j["channel"]       = handoff.channel;
    j["version"]       = handoff.version;
    j["version_label"] = handoff.version_label;
    j["asset_name"]    = handoff.asset_name;
    j["url"]           = handoff.url;
    j["digest"]        = handoff.digest;
    j["size"]          = handoff.size;
    j["flavour"]       = handoff.flavour;
    j["install_root"]  = handoff.install_root;
    j["exe_path"]      = handoff.exe_path;
    j["cache_dir"]     = handoff.cache_dir;
    j["parent_pid"]    = handoff.parent_pid;
    j["log_path"]      = handoff.log_path;
    j["lock_path"]     = handoff.lock_path;
    return j.dump();
}

bool handoff_from_json(const std::string& text, UpdateHandoff& out)
{
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception&) {
        return false;
    }
    if (!j.is_object())
        return false;

    out.channel       = text_field(j, "channel");
    out.version       = text_field(j, "version");
    out.version_label = text_field(j, "version_label");
    out.asset_name    = text_field(j, "asset_name");
    out.url           = text_field(j, "url");
    out.digest        = text_field(j, "digest");
    out.size          = integer_field(j, "size");
    out.flavour       = text_field(j, "flavour");
    out.install_root  = text_field(j, "install_root");
    out.exe_path      = text_field(j, "exe_path");
    out.cache_dir     = text_field(j, "cache_dir");
    out.parent_pid    = integer_field(j, "parent_pid");
    out.log_path      = text_field(j, "log_path");
    out.lock_path     = text_field(j, "lock_path");
    return true;
}

bool write_handoff(const UpdateHandoff& handoff)
{
    return write_file_atomic(updater_handoff_path(), handoff_to_json(handoff));
}

bool read_handoff(UpdateHandoff& out)
{
    std::string text;
    if (!read_file(updater_handoff_path(), text))
        return false;
    return handoff_from_json(text, out);
}

void remove_handoff()
{
    remove_file(updater_handoff_path());
}

std::string install_record_to_json(const InstallRecord& record)
{
    json j;
    j["digest"]  = record.digest;
    j["channel"] = record.channel;
    j["version"] = record.version;
    j["target"]  = record.target;
    return j.dump();
}

bool install_record_from_json(const std::string& text, InstallRecord& out)
{
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception&) {
        return false;
    }
    if (!j.is_object())
        return false;

    out.digest  = text_field(j, "digest");
    out.channel = text_field(j, "channel");
    out.version = text_field(j, "version");
    out.target  = text_field(j, "target");
    return true;
}

bool write_install_record(const InstallRecord& record)
{
    return write_file_atomic(updater_install_record_path(), install_record_to_json(record));
}

bool read_install_record(InstallRecord& out)
{
    return read_install_record_file(out);
}

void remove_install_record()
{
    remove_file(updater_install_record_path());
}

std::string pending_install_to_json(const PendingInstall& pending)
{
    json j;
    j["backup"]  = pending.backup;
    j["target"]  = pending.target;
    j["digest"]  = pending.digest;
    j["channel"] = pending.channel;
    j["version"] = pending.version;
    return j.dump();
}

bool pending_install_from_json(const std::string& text, PendingInstall& out)
{
    json j;
    try {
        j = json::parse(text);
    } catch (const std::exception&) {
        return false;
    }
    if (!j.is_object())
        return false;

    out.backup  = text_field(j, "backup");
    out.target  = text_field(j, "target");
    out.digest  = text_field(j, "digest");
    out.channel = text_field(j, "channel");
    out.version = text_field(j, "version");
    return true;
}

bool write_pending_install(const PendingInstall& pending)
{
    return write_file_atomic(updater_pending_path(), pending_install_to_json(pending));
}

bool read_pending_install(PendingInstall& out)
{
    std::string text;
    if (!read_file(updater_pending_path(), text))
        return false;
    return pending_install_from_json(text, out);
}

void remove_pending_install()
{
    remove_file(updater_pending_path());
}

bool acquire_install_lock(const std::string& path)
{
    std::lock_guard<std::mutex> guard(held_install_locks_mutex());
    if (held_install_locks().count(path) != 0)
        return false;
    ensure_parent_dir(path);
    LockHandle handle{};
    if (!open_lock_file(path, handle))
        return false;
    held_install_locks().emplace(path, handle);
    return true;
}

void release_install_lock(const std::string& path)
{
    std::lock_guard<std::mutex> guard(held_install_locks_mutex());
    const auto it = held_install_locks().find(path);
    if (it == held_install_locks().end())
        return;
    // Removed while still locked, so a waiter that gets the lock next finds the path gone.
    remove_file(path);
    close_lock_file(it->second);
    held_install_locks().erase(it);
}

} // namespace Slic3r
