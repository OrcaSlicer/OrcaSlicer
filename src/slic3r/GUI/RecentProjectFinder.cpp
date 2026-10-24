#include "RecentProjectFinder.hpp"

#include <algorithm>
#include <exception>
#include <set>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <wx/filename.h>

#include "libslic3r/Thread.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI.hpp"

namespace Slic3r { namespace GUI {

namespace fs = boost::filesystem;

namespace {

// Hidden files are skipped: on external drives macOS leaves a "._name.3mf" next to each file.
bool is_project_file(const fs::path& path)
{
    boost::system::error_code ec;
    const std::string         name = path.filename().string();
    return fs::is_regular_file(path, ec) && !boost::starts_with(name, ".") && boost::iends_with(name, ".3mf");
}

std::vector<wxString> find_project_files(const wxString& folder, const std::atomic<bool>& cancel)
{
    std::vector<wxString>               files;
    boost::system::error_code           ec;
    fs::recursive_directory_iterator    it(into_path(folder), fs::directory_options::skip_permission_denied, ec);
    for (; !ec && !cancel && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (is_project_file(it->path()))
            files.push_back(from_path(it->path()));
    if (ec)
        BOOST_LOG_TRIVIAL(warning) << "Scan for 3MFs stopped in " << into_u8(folder) << ": " << ec.message();
    return files;
}

bool is_same_project(const RecentProjectEntry& entry, const FoundProject& file)
{
    if (entry.hash.empty())
        return wxFileName(entry.path).GetFullName() == wxFileName(file.path).GetFullName();
    return entry.hash == file.hash;
}

FoundProject with_preview(FoundProject file)
{
    file.thumbnail = bbs_3mf_get_thumbnail(into_u8(file.path).c_str());
    file.listing   = bbs_3mf_get_listing_info(into_u8(file.path));
    return file;
}

// The history files that were never hashed, now hashed.
std::vector<std::pair<wxString, std::string>> hash_unhashed(const std::vector<RecentProjectEntry>& history,
                                                            const std::atomic<bool>&               cancel)
{
    std::vector<std::pair<wxString, std::string>> hashes;
    for (const RecentProjectEntry& entry : history) {
        if (cancel)
            break;
        if (entry.missing || !entry.hash.empty())
            continue;
        std::string hash = project_file_hash(entry.path);
        if (!hash.empty())
            hashes.emplace_back(entry.path, hash);
    }
    return hashes;
}

std::vector<RecentProjectEntry> with_hashes(std::vector<RecentProjectEntry>                      history,
                                            const std::vector<std::pair<wxString, std::string>>& hashes)
{
    // Not a structured binding: C++17 lambdas can't capture one.
    for (const std::pair<wxString, std::string>& file_hash : hashes) {
        const wxString& path  = file_hash.first;
        auto            entry = std::find_if(history.begin(), history.end(), [&path](const RecentProjectEntry& e) { return e.path == path; });
        if (entry != history.end())
            entry->hash = file_hash.second;
    }
    return history;
}

// The files of the folder that are not in the history yet, hashed. Hashing is the slow part of a scan.
std::vector<FoundProject> find_new_projects(const wxString&                        folder,
                                            const std::vector<RecentProjectEntry>& history,
                                            const std::atomic<bool>&               cancel)
{
    std::set<wxString> known_paths;
    for (const RecentProjectEntry& entry : history)
        known_paths.insert(entry.path);

    std::vector<FoundProject> found;
    for (const wxString& path : find_project_files(folder, cancel)) {
        if (cancel)
            break;
        if (known_paths.count(path) > 0)
            continue;
        std::string hash = project_file_hash(path);
        if (!hash.empty())
            found.push_back({path, hash, {}, {}});
    }
    return found;
}

RecentProjectMoves find_recent_project_moves(const wxString&                 folder,
                                             std::vector<RecentProjectEntry> history,
                                             bool                            add_new,
                                             const std::atomic<bool>&        cancel)
{
    auto hashes = hash_unhashed(history, cancel);
    history     = with_hashes(std::move(history), hashes);

    RecentProjectMoves moves = plan_recent_project_moves(history, find_new_projects(folder, history, cancel), add_new);
    moves.hashes = std::move(hashes);
    for (auto& relink : moves.relinks)
        relink.second = with_preview(std::move(relink.second));
    for (FoundProject& addition : moves.additions)
        addition = with_preview(std::move(addition));
    return moves;
}

} // namespace

std::string project_file_hash(const wxString& path)
{
    std::string file = into_u8(path);
    std::string hash;
    try {
        bbl_calc_md5(file, hash);
    } catch (const std::exception& err) {
        BOOST_LOG_TRIVIAL(warning) << "Can't hash " << file << ": " << err.what();
    }
    return hash;
}

RecentProjectMoves plan_recent_project_moves(const std::vector<RecentProjectEntry>& history,
                                             const std::vector<FoundProject>&       found,
                                             bool                                   add_new)
{
    std::set<wxString>                     known_paths;
    std::set<std::string>                  present_hashes;
    std::vector<const RecentProjectEntry*> missing;
    for (const RecentProjectEntry& entry : history) {
        known_paths.insert(entry.path);
        if (entry.missing)
            missing.push_back(&entry);
        else if (!entry.hash.empty())
            present_hashes.insert(entry.hash);
    }

    RecentProjectMoves moves;
    for (const FoundProject& file : found) {
        if (known_paths.count(file.path) > 0 || present_hashes.count(file.hash) > 0)
            continue;
        auto match = std::find_if(missing.begin(), missing.end(), [&file](const RecentProjectEntry* e) { return is_same_project(*e, file); });
        if (match != missing.end()) {
            moves.relinks.emplace_back((*match)->path, file);
            missing.erase(match);
        } else if (add_new) {
            moves.additions.push_back(file);
        } else {
            continue;
        }
        present_hashes.insert(file.hash);
    }
    return moves;
}

RecentProjectFinder::~RecentProjectFinder()
{
    m_cancel = true;
    if (m_thread.joinable())
        m_thread.join();
}

void RecentProjectFinder::start(const wxString& folder, std::vector<RecentProjectEntry> history, bool add_new, Done done)
{
    if (m_running)
        return;
    if (m_thread.joinable())
        m_thread.join();
    m_running = true;
    m_thread  = create_thread([this, folder, history = std::move(history), add_new, done = std::move(done)]() mutable {
        RecentProjectMoves moves = find_recent_project_moves(folder, std::move(history), add_new, m_cancel);
        BOOST_LOG_TRIVIAL(info) << "Find 3MFs: scan of " << into_u8(folder) << " done, " << moves.relinks.size() << " relinked, "
                                << moves.additions.size() << " new, " << moves.hashes.size() << " hashed";
        if (!m_cancel)
            done(std::move(moves));
        m_running = false;
    });
}

}} // namespace Slic3r::GUI
