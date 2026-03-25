#include "GitSync.hpp"

#include <git2.h>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/algorithm/string.hpp>

#include <sstream>
#include <fstream>
#include <cstring>
#include <mutex>

namespace fs = boost::filesystem;

namespace Slic3r {

// ---------------------------------------------------------------------------
// LibGit2Init — singleton RAII for git_libgit2_init / git_libgit2_shutdown
// ---------------------------------------------------------------------------

LibGit2Init& LibGit2Init::instance()
{
    static LibGit2Init s;
    return s;
}

LibGit2Init::LibGit2Init()  { git_libgit2_init(); }
LibGit2Init::~LibGit2Init() { git_libgit2_shutdown(); }

// ---------------------------------------------------------------------------
// Helper: format the last libgit2 error into a string
// ---------------------------------------------------------------------------
static std::string last_git_error(const char* prefix = nullptr)
{
    const git_error* e = git_error_last();
    std::string msg;
    if (prefix) {
        msg += prefix;
        msg += ": ";
    }
    if (e && e->message)
        msg += e->message;
    else
        msg += "unknown libgit2 error";
    return msg;
}

// ---------------------------------------------------------------------------
// GitSync
// ---------------------------------------------------------------------------

GitSync::GitSync(const GitSyncConfig& config)
    : m_config(config)
{
    LibGit2Init::instance(); // ensure libgit2 is initialized
}

GitSync::~GitSync()
{
    disconnect();
}

std::string GitSync::info_message() const
{
    if (m_branch_created)
        return "Created new branch '" + m_config.branch + "' on the remote repository.";
    return {};
}

std::string GitSync::local_file_path(const std::string& relative_path) const
{
    return (fs::path(m_config.local_clone_path) / relative_path).string();
}

// Credential callback used by libgit2 for HTTPS authentication
int GitSync::credential_cb(git_credential** out, const char* /*url*/,
                           const char* username_from_url,
                           unsigned int allowed_types, void* payload)
{
    auto* config = static_cast<const GitSyncConfig*>(payload);
    if (allowed_types & GIT_CREDENTIAL_USERPASS_PLAINTEXT) {
        const char* user = config->username.empty()
            ? (username_from_url ? username_from_url : "git")
            : config->username.c_str();
        const char* pass = config->token.c_str();
        return git_credential_userpass_plaintext_new(out, user, pass);
    }
    return GIT_PASSTHROUGH;
}

// ---------------------------------------------------------------------------
// connect / disconnect / is_connected
// ---------------------------------------------------------------------------

bool GitSync::connect(std::string& error_out)
{
    fs::path clone_dir(m_config.local_clone_path);

    if (fs::exists(clone_dir / ".git")) {
        if (!open_repo(error_out))
            return false;

        // Ensure HEAD is on the configured branch
        std::string branch_ref = "refs/heads/" + m_config.branch;
        git_reference* head_ref = nullptr;
        bool need_switch = true;
        if (git_repository_head(&head_ref, m_repo) == 0) {
            const char* name = git_reference_name(head_ref);
            if (name && branch_ref == name)
                need_switch = false;
            git_reference_free(head_ref);
        }
        if (need_switch) {
            BOOST_LOG_TRIVIAL(info) << "GitSync: switching HEAD to " << branch_ref;
            git_repository_set_head(m_repo, branch_ref.c_str());

            // Check if the target branch exists locally
            git_reference* br = nullptr;
            bool branch_exists = (git_reference_lookup(&br, m_repo, branch_ref.c_str()) == 0);
            if (br) git_reference_free(br);

            if (branch_exists) {
                // Checkout the branch content
                git_checkout_options co = GIT_CHECKOUT_OPTIONS_INIT;
                co.checkout_strategy = GIT_CHECKOUT_FORCE;
                git_checkout_head(m_repo, &co);
            } else {
                // Branch doesn't exist yet — clear index and working tree
                // so the first commit starts clean (no leftover files from old branch)
                BOOST_LOG_TRIVIAL(info) << "GitSync: branch '" << m_config.branch
                                        << "' does not exist locally, clearing working tree";
                git_index* index = nullptr;
                if (git_repository_index(&index, m_repo) == 0) {
                    git_index_clear(index);
                    git_index_write(index);
                    git_index_free(index);
                }
                // Remove working tree files (except .git)
                for (auto& entry : fs::directory_iterator(clone_dir)) {
                    if (entry.path().filename() == ".git") continue;
                    boost::system::error_code ec;
                    fs::remove_all(entry.path(), ec);
                }
            }
        }

        return pull(error_out);
    }

    return clone_repo(error_out);
}

void GitSync::disconnect()
{
    if (m_repo) {
        git_repository_free(m_repo);
        m_repo = nullptr;
    }
}

bool GitSync::is_connected() const
{
    return m_repo != nullptr;
}

// ---------------------------------------------------------------------------
// clone_repo — initial clone via libgit2
// ---------------------------------------------------------------------------

bool GitSync::clone_repo(std::string& error_out)
{
    fs::create_directories(m_config.local_clone_path);

    git_clone_options opts = GIT_CLONE_OPTIONS_INIT;
    opts.checkout_branch = m_config.branch.c_str();

    // Set up credentials
    if (!m_config.token.empty()) {
        opts.fetch_opts.callbacks.credentials = &GitSync::credential_cb;
        opts.fetch_opts.callbacks.payload     = const_cast<GitSyncConfig*>(&m_config);
    }

    int rc = git_clone(&m_repo, m_config.repo_url.c_str(),
                       m_config.local_clone_path.c_str(), &opts);

    if (rc < 0) {
        // Branch might not exist on remote — retry with the default branch
        m_repo = nullptr;
        boost::system::error_code ec;
        fs::remove_all(m_config.local_clone_path, ec);
        fs::create_directories(m_config.local_clone_path);

        git_clone_options opts2 = GIT_CLONE_OPTIONS_INIT;
        if (!m_config.token.empty()) {
            opts2.fetch_opts.callbacks.credentials = &GitSync::credential_cb;
            opts2.fetch_opts.callbacks.payload     = const_cast<GitSyncConfig*>(&m_config);
        }

        rc = git_clone(&m_repo, m_config.repo_url.c_str(),
                       m_config.local_clone_path.c_str(), &opts2);
        if (rc < 0) {
            error_out = last_git_error("git clone failed");
            m_repo = nullptr;
            return false;
        }

        // Create orphan branch with empty tree (no content from default branch)
        git_index* index = nullptr;
        if (git_repository_index(&index, m_repo) == 0) {
            git_index_clear(index);

            git_oid tree_oid;
            if (git_index_write_tree(&tree_oid, index) == 0) {
                git_tree* tree = nullptr;
                if (git_tree_lookup(&tree, m_repo, &tree_oid) == 0) {
                    git_signature* sig = nullptr;
                    if (git_signature_now(&sig, m_config.author_name.c_str(),
                                          m_config.author_email.c_str()) == 0) {
                        git_oid commit_oid;
                        std::string branch_ref = "refs/heads/" + m_config.branch;
                        // Orphan commit: 0 parents
                        rc = git_commit_create_v(&commit_oid, m_repo,
                            branch_ref.c_str(), sig, sig, "UTF-8",
                            "Initialize OrcaSlicer sync", tree, 0);

                        if (rc == 0) {
                            if (git_repository_set_head(m_repo, branch_ref.c_str()) < 0)
                                BOOST_LOG_TRIVIAL(warning) << "GitSync: set_head failed after orphan commit: " << last_git_error();

                            // Checkout empty tree — removes working dir files from default branch
                            git_checkout_options co_opts = GIT_CHECKOUT_OPTIONS_INIT;
                            co_opts.checkout_strategy = GIT_CHECKOUT_FORCE;
                            if (git_checkout_head(m_repo, &co_opts) < 0)
                                BOOST_LOG_TRIVIAL(warning) << "GitSync: checkout_head failed after orphan commit: " << last_git_error();
                        }
                        git_signature_free(sig);
                    }
                    git_tree_free(tree);
                }
            }
            git_index_free(index);
        }

        m_branch_created = true;
        BOOST_LOG_TRIVIAL(info) << "GitSync: created orphan branch '" << m_config.branch << "'";
    }

    // Configure user info for commits
    git_config* cfg = nullptr;
    if (git_repository_config(&cfg, m_repo) == 0) {
        git_config_set_string(cfg, "user.name", m_config.author_name.c_str());
        git_config_set_string(cfg, "user.email", m_config.author_email.c_str());
        git_config_free(cfg);
    }

    return true;
}

// ---------------------------------------------------------------------------
// open_repo — open an existing local clone
// ---------------------------------------------------------------------------

bool GitSync::open_repo(std::string& error_out)
{
    if (m_repo)
        return true;

    int rc = git_repository_open(&m_repo, m_config.local_clone_path.c_str());
    if (rc < 0) {
        error_out = last_git_error("Failed to open repository");
        m_repo = nullptr;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// test_connection — lightweight ls-remote equivalent
// ---------------------------------------------------------------------------

bool GitSync::test_connection(std::string& error_out)
{
    LibGit2Init::instance();

    // Create a temporary in-memory remote to test connectivity
    git_remote* remote = nullptr;
    int rc;

    if (m_repo) {
        rc = git_remote_lookup(&remote, m_repo, "origin");
        if (rc < 0) {
            rc = git_remote_create_anonymous(&remote, m_repo, m_config.repo_url.c_str());
        }
    } else {
        // Without a repo, create a temporary one in-memory is not possible,
        // so we try to init a bare temp repo just for the test
        git_repository* tmp_repo = nullptr;
        fs::path tmp_path = fs::temp_directory_path() / "orcaslicer_git_test";
        fs::create_directories(tmp_path);

        rc = git_repository_init(&tmp_repo, tmp_path.string().c_str(), 1 /* bare */);
        if (rc < 0) {
            error_out = last_git_error("Failed to create temp repo for connection test");
            fs::remove_all(tmp_path);
            return false;
        }

        rc = git_remote_create_anonymous(&remote, tmp_repo, m_config.repo_url.c_str());
        if (rc < 0) {
            error_out = last_git_error("Failed to create remote");
            git_repository_free(tmp_repo);
            fs::remove_all(tmp_path);
            return false;
        }

        git_remote_callbacks callbacks = GIT_REMOTE_CALLBACKS_INIT;
        if (!m_config.token.empty()) {
            callbacks.credentials = &GitSync::credential_cb;
            callbacks.payload     = const_cast<GitSyncConfig*>(&m_config);
        }

        rc = git_remote_connect(remote, GIT_DIRECTION_FETCH, &callbacks, nullptr, nullptr);
        bool ok = (rc == 0);
        if (!ok)
            error_out = last_git_error("Connection test failed");

        if (ok)
            git_remote_disconnect(remote);
        git_remote_free(remote);
        git_repository_free(tmp_repo);
        fs::remove_all(tmp_path);
        return ok;
    }

    if (rc < 0) {
        error_out = last_git_error("Failed to get remote");
        if (remote) git_remote_free(remote);
        return false;
    }

    git_remote_callbacks callbacks = GIT_REMOTE_CALLBACKS_INIT;
    if (!m_config.token.empty()) {
        callbacks.credentials = &GitSync::credential_cb;
        callbacks.payload     = const_cast<GitSyncConfig*>(&m_config);
    }

    rc = git_remote_connect(remote, GIT_DIRECTION_FETCH, &callbacks, nullptr, nullptr);
    bool ok = (rc == 0);
    if (!ok)
        error_out = last_git_error("Connection test failed");

    if (ok)
        git_remote_disconnect(remote);
    git_remote_free(remote);
    return ok;
}

// ---------------------------------------------------------------------------
// fetch_remote — fetch from origin
// ---------------------------------------------------------------------------

bool GitSync::fetch_remote(std::string& error_out)
{
    git_remote* remote = nullptr;
    int rc = git_remote_lookup(&remote, m_repo, "origin");
    if (rc < 0) {
        error_out = last_git_error("Failed to lookup remote 'origin'");
        return false;
    }

    git_fetch_options fetch_opts = GIT_FETCH_OPTIONS_INIT;
    if (!m_config.token.empty()) {
        fetch_opts.callbacks.credentials = &GitSync::credential_cb;
        fetch_opts.callbacks.payload     = const_cast<GitSyncConfig*>(&m_config);
    }

    rc = git_remote_fetch(remote, nullptr, &fetch_opts, "sync fetch");
    git_remote_free(remote);

    if (rc < 0) {
        error_out = last_git_error("Fetch failed");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// merge_fetched — fast-forward merge of FETCH_HEAD
// ---------------------------------------------------------------------------

bool GitSync::merge_fetched(std::string& error_out)
{
    // Resolve remote branch ref
    git_oid fetch_head_oid;
    std::string refname = "refs/remotes/origin/" + m_config.branch;
    int rc = git_reference_name_to_id(&fetch_head_oid, m_repo, refname.c_str());
    if (rc < 0) {
        // Remote branch doesn't exist yet (new branch not pushed).
        // Nothing to merge — the first commit_and_push will create it.
        BOOST_LOG_TRIVIAL(info) << "GitSync: remote branch ref '" << refname
                                << "' not found, nothing to merge";
        return true;
    }

    // Get the annotated commit for the fetched head
    git_annotated_commit* fetchhead_commit = nullptr;
    rc = git_annotated_commit_lookup(&fetchhead_commit, m_repo, &fetch_head_oid);
    if (rc < 0) {
        error_out = last_git_error("Failed to lookup fetched commit");
        return false;
    }

    // Perform merge analysis
    git_merge_analysis_t analysis;
    git_merge_preference_t preference;
    const git_annotated_commit* their_heads[] = { fetchhead_commit };
    rc = git_merge_analysis(&analysis, &preference, m_repo, their_heads, 1);
    if (rc < 0) {
        error_out = last_git_error("Merge analysis failed");
        git_annotated_commit_free(fetchhead_commit);
        return false;
    }

    if (analysis & GIT_MERGE_ANALYSIS_UP_TO_DATE) {
        // Already up to date
        git_annotated_commit_free(fetchhead_commit);
        return true;
    }

    if (analysis & GIT_MERGE_ANALYSIS_FASTFORWARD) {
        // Fast-forward: update HEAD to the fetched commit
        git_reference* ref = nullptr;
        git_reference* new_ref = nullptr;
        std::string local_ref = "refs/heads/" + m_config.branch;

        rc = git_reference_lookup(&ref, m_repo, local_ref.c_str());
        if (rc == 0) {
            rc = git_reference_set_target(&new_ref, ref, &fetch_head_oid, "fast-forward");
            git_reference_free(ref);
            if (new_ref) git_reference_free(new_ref);
        } else {
            // Branch doesn't exist locally yet, create it
            rc = git_reference_create(&new_ref, m_repo, local_ref.c_str(),
                                      &fetch_head_oid, 1, "fast-forward");
            if (new_ref) git_reference_free(new_ref);
        }

        if (rc < 0) {
            error_out = last_git_error("Fast-forward failed");
            git_annotated_commit_free(fetchhead_commit);
            return false;
        }

        // Checkout the updated HEAD
        git_checkout_options checkout_opts = GIT_CHECKOUT_OPTIONS_INIT;
        checkout_opts.checkout_strategy = GIT_CHECKOUT_FORCE;

        git_object* target = nullptr;
        git_object_lookup(&target, m_repo, &fetch_head_oid, GIT_OBJECT_COMMIT);
        if (target) {
            if (git_checkout_tree(m_repo, target, &checkout_opts) < 0)
                BOOST_LOG_TRIVIAL(warning) << "GitSync: git_checkout_tree failed: " << last_git_error();
            git_object_free(target);
        }

        // Update HEAD
        if (git_repository_set_head(m_repo, local_ref.c_str()) < 0)
            BOOST_LOG_TRIVIAL(warning) << "GitSync: git_repository_set_head failed: " << last_git_error();

        git_annotated_commit_free(fetchhead_commit);
        return true;
    }

    // Non-fast-forward — reset to remote since sync_git is a cache, not user workspace.
    // Local preset files are the source of truth; sync_single_file() handles real conflicts.
    BOOST_LOG_TRIVIAL(warning) << "GitSync: non-fast-forward detected, resetting to remote";
    git_annotated_commit_free(fetchhead_commit);

    git_object* target = nullptr;
    rc = git_object_lookup(&target, m_repo, &fetch_head_oid, GIT_OBJECT_COMMIT);
    if (rc < 0 || !target) {
        error_out = last_git_error("Failed to lookup remote commit for reset");
        return false;
    }

    git_checkout_options co = GIT_CHECKOUT_OPTIONS_INIT;
    co.checkout_strategy = GIT_CHECKOUT_FORCE;
    rc = git_reset(m_repo, target, GIT_RESET_HARD, &co);
    git_object_free(target);
    if (rc < 0) {
        error_out = last_git_error("Hard reset to remote failed");
        return false;
    }

    BOOST_LOG_TRIVIAL(info) << "GitSync: successfully reset to remote HEAD";
    return true;
}

// ---------------------------------------------------------------------------
// refresh — pull latest changes before a sync cycle
// ---------------------------------------------------------------------------

bool GitSync::refresh(std::string& error_out)
{
    return pull(error_out);
}

// ---------------------------------------------------------------------------
// pull — fetch + merge
// ---------------------------------------------------------------------------

bool GitSync::pull(std::string& error_out)
{
    if (!m_repo && !open_repo(error_out))
        return false;

    if (!fetch_remote(error_out))
        return false;

    return merge_fetched(error_out);
}

// ---------------------------------------------------------------------------
// ensure_directory
// ---------------------------------------------------------------------------

bool GitSync::ensure_directory(const std::string& remote_path, std::string& error_out)
{
    fs::path dir = fs::path(m_config.local_clone_path) / remote_path;
    try {
        fs::create_directories(dir);
        // Git doesn't track empty directories, add .gitkeep
        fs::path gitkeep = dir / ".gitkeep";
        bool created = false;
        if (!fs::exists(gitkeep)) {
            std::ofstream f(gitkeep.string());
            f.close();
            created = true;
        }
        // Stage the .gitkeep so it gets committed
        if (created && m_repo) {
            git_index* index = nullptr;
            if (git_repository_index(&index, m_repo) == 0) {
                std::string rel = remote_path + "/.gitkeep";
                git_index_add_bypath(index, rel.c_str());
                git_index_write(index);
                git_index_free(index);
            }
        }
        return true;
    } catch (const std::exception& e) {
        error_out = std::string("Failed to create directory: ") + e.what();
        return false;
    }
}

// ---------------------------------------------------------------------------
// blob_hash_for_file — compute blob OID for a working-tree file
// ---------------------------------------------------------------------------

std::string GitSync::blob_hash_for_file(const std::string& relative_path)
{
    std::string full = local_file_path(relative_path);
    if (!fs::exists(full)) return {};

    git_oid oid;
    int rc = git_odb_hashfile(&oid, full.c_str(), GIT_OBJECT_BLOB);
    if (rc < 0) return {};

    char hex[GIT_OID_SHA1_HEXSIZE + 1];
    git_oid_tostr(hex, sizeof(hex), &oid);
    return std::string(hex);
}

// ---------------------------------------------------------------------------
// list_files
// ---------------------------------------------------------------------------

bool GitSync::list_files(const std::string& remote_dir,
                          std::vector<RemoteFileInfo>& out,
                          std::string& error_out)
{
    if (!m_repo) {
        error_out = "Not connected";
        return false;
    }

    fs::path dir = fs::path(m_config.local_clone_path) / remote_dir;
    if (!fs::exists(dir))
        return true; // empty — not an error

    try {
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (entry.path().filename() == ".git" || entry.path().filename() == ".gitkeep")
                continue;

            RemoteFileInfo info;
            info.path         = (fs::path(remote_dir) / entry.path().filename()).string();
            info.is_directory = fs::is_directory(entry);

            if (!info.is_directory) {
                info.size = fs::file_size(entry);
                info.modified_time = fs::last_write_time(entry);

                // Use blob OID as etag
                info.etag = blob_hash_for_file(info.path);
            }

            out.push_back(std::move(info));
        }
    } catch (const std::exception& e) {
        error_out = std::string("Failed to list files: ") + e.what();
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// download_file
// ---------------------------------------------------------------------------

bool GitSync::download_file(const std::string& remote_path,
                             std::string& content_out,
                             RemoteFileInfo& info_out,
                             std::string& error_out,
                             SyncError* error_code_out)
{
    std::string full = local_file_path(remote_path);

    if (!fs::exists(full)) {
        error_out = "File not found: " + remote_path;
        if (error_code_out) *error_code_out = SyncError::NotFound;
        return false;
    }

    try {
        std::ifstream f(full, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        content_out = ss.str();
    } catch (const std::exception& e) {
        error_out = std::string("Failed to read file: ") + e.what();
        return false;
    }

    info_out.path = remote_path;
    info_out.size = content_out.size();
    info_out.etag = blob_hash_for_file(remote_path);
    info_out.modified_time = static_cast<long long>(fs::last_write_time(full));

    return true;
}

// ---------------------------------------------------------------------------
// upload_file
// ---------------------------------------------------------------------------

bool GitSync::upload_file(const std::string& remote_path,
                           const std::string& content,
                           const std::string& expected_etag,
                           std::string& new_etag_out,
                           std::string& error_out,
                           SyncError* error_code_out)
{
    std::string full = local_file_path(remote_path);

    // Check for conflict using blob hash
    if (!expected_etag.empty() && fs::exists(full)) {
        std::string current_hash = blob_hash_for_file(remote_path);
        if (!current_hash.empty() && current_hash != expected_etag) {
            error_out = "CONFLICT";
            if (error_code_out) *error_code_out = SyncError::Conflict;
            return false;
        }
    }

    // Ensure parent directory exists
    fs::path parent = fs::path(full).parent_path();
    fs::create_directories(parent);

    // Write file
    try {
        std::ofstream f(full, std::ios::binary | std::ios::trunc);
        f.write(content.data(), content.size());
        f.close();
    } catch (const std::exception& e) {
        error_out = std::string("Failed to write file: ") + e.what();
        return false;
    }

    // Stage the file via libgit2 index
    if (m_repo) {
        git_index* index = nullptr;
        int rc = git_repository_index(&index, m_repo);
        if (rc == 0) {
            rc = git_index_add_bypath(index, remote_path.c_str());
            if (rc < 0) {
                error_out = last_git_error("git index add failed");
                git_index_free(index);
                return false;
            }
            if (git_index_write(index) < 0) {
                error_out = last_git_error("git_index_write failed");
                git_index_free(index);
                return false;
            }
            git_index_free(index);
        } else {
            error_out = last_git_error("Failed to get index");
            return false;
        }
    }

    new_etag_out = blob_hash_for_file(remote_path);
    return true;
}

// ---------------------------------------------------------------------------
// delete_file
// ---------------------------------------------------------------------------

bool GitSync::delete_file(const std::string& remote_path, std::string& error_out)
{
    std::string full = local_file_path(remote_path);

    if (!fs::exists(full))
        return true; // already gone

    // Remove from index
    if (m_repo) {
        git_index* index = nullptr;
        int rc = git_repository_index(&index, m_repo);
        if (rc == 0) {
            if (git_index_remove_bypath(index, remote_path.c_str()) < 0)
                BOOST_LOG_TRIVIAL(warning) << "GitSync: git_index_remove_bypath failed: " << last_git_error();
            if (git_index_write(index) < 0)
                BOOST_LOG_TRIVIAL(warning) << "GitSync: git_index_write failed: " << last_git_error();
            git_index_free(index);
        }
    }

    // Remove from working tree
    try {
        fs::remove(full);
    } catch (const std::exception& e) {
        error_out = std::string("Failed to delete file: ") + e.what();
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// commit_and_push
// ---------------------------------------------------------------------------

bool GitSync::commit_and_push(const std::string& message, std::string& error_out)
{
    if (!m_repo) {
        error_out = "Not connected";
        return false;
    }

    std::string branch_ref = "refs/heads/" + m_config.branch;

    // Get index and check if there are staged changes
    git_index* index = nullptr;
    int rc = git_repository_index(&index, m_repo);
    if (rc < 0) {
        error_out = last_git_error("Failed to get index");
        return false;
    }

    // Resolve the branch tip tree (not HEAD — HEAD may point to a different branch)
    git_tree* tip_tree = nullptr;
    git_reference* branch_ref_obj = nullptr;
    if (git_reference_lookup(&branch_ref_obj, m_repo, branch_ref.c_str()) == 0) {
        const git_oid* tip_oid = git_reference_target(branch_ref_obj);
        if (tip_oid) {
            git_commit* tip_commit = nullptr;
            if (git_commit_lookup(&tip_commit, m_repo, tip_oid) == 0) {
                git_commit_tree(&tip_tree, tip_commit);
                git_commit_free(tip_commit);
            }
        }
        git_reference_free(branch_ref_obj);
    }

    // Check for changes by comparing index to branch tip tree
    bool has_changes = false;
    if (tip_tree) {
        git_diff* diff = nullptr;
        rc = git_diff_tree_to_index(&diff, m_repo, tip_tree, index, nullptr);
        if (rc == 0 && diff)
            has_changes = git_diff_num_deltas(diff) > 0;
        if (diff)
            git_diff_free(diff);
        git_tree_free(tip_tree);
    } else {
        // Branch doesn't exist yet (first commit) — index has content means changes
        has_changes = git_index_entrycount(index) > 0;
    }

    if (!has_changes) {
        git_index_free(index);
        return true; // Nothing to commit
    }

    // Write the index as a tree
    git_oid tree_oid;
    rc = git_index_write_tree(&tree_oid, index);
    git_index_free(index);
    if (rc < 0) {
        error_out = last_git_error("Failed to write tree");
        return false;
    }

    git_tree* tree = nullptr;
    rc = git_tree_lookup(&tree, m_repo, &tree_oid);
    if (rc < 0) {
        error_out = last_git_error("Failed to lookup tree");
        return false;
    }

    // Build the signature
    git_signature* sig = nullptr;
    rc = git_signature_now(&sig, m_config.author_name.c_str(), m_config.author_email.c_str());
    if (rc < 0) {
        error_out = last_git_error("Failed to create signature");
        git_tree_free(tree);
        return false;
    }

    // Get parent commit from the branch tip (not HEAD)
    git_commit* parent = nullptr;
    git_reference* br_ref = nullptr;
    if (git_reference_lookup(&br_ref, m_repo, branch_ref.c_str()) == 0) {
        const git_oid* tip_oid = git_reference_target(br_ref);
        if (tip_oid) {
            if (git_commit_lookup(&parent, m_repo, tip_oid) < 0)
                BOOST_LOG_TRIVIAL(warning) << "GitSync: git_commit_lookup failed: " << last_git_error();
        }
        git_reference_free(br_ref);
    }

    // Create the commit — update the branch ref directly (not "HEAD")
    git_oid commit_oid;
    const git_commit* parents[] = { parent };
    int nparents = parent ? 1 : 0;
    rc = git_commit_create(&commit_oid, m_repo, branch_ref.c_str(),
                           sig, sig, "UTF-8",
                           message.c_str(), tree,
                           nparents, parents);

    git_signature_free(sig);
    git_tree_free(tree);
    if (parent) git_commit_free(parent);

    if (rc < 0) {
        error_out = last_git_error("Commit failed");
        return false;
    }

    // Ensure HEAD points to our branch
    git_repository_set_head(m_repo, branch_ref.c_str());

    // Push to remote
    git_remote* remote = nullptr;
    rc = git_remote_lookup(&remote, m_repo, "origin");
    if (rc < 0) {
        error_out = last_git_error("Failed to lookup remote 'origin'");
        return false;
    }

    git_push_options push_opts = GIT_PUSH_OPTIONS_INIT;
    if (!m_config.token.empty()) {
        push_opts.callbacks.credentials = &GitSync::credential_cb;
        push_opts.callbacks.payload     = const_cast<GitSyncConfig*>(&m_config);
    }

    std::string refspec = branch_ref + ":" + branch_ref;
    const char* refspec_strs[] = { refspec.c_str() };
    git_strarray refspecs = { const_cast<char**>(refspec_strs), 1 };

    rc = git_remote_push(remote, &refspecs, &push_opts);
    git_remote_free(remote);

    if (rc < 0) {
        error_out = last_git_error("Push failed");
        return false;
    }

    return true;
}

} // namespace Slic3r
