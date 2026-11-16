#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

class OrcaCloudServiceAgent;

// Cloud preset deletes waiting to be sent. Deleting a synced preset in the UI and the scan for orphaned
// .info files add them; the preset sync thread sends them.
//
// Every entry belongs to an account, and the queue belongs to one session: the time one account stays
// signed in with its presets loaded. Starting another account's session drops every entry and changes the
// generation, so an entry is never sent with another account's token, an add that began in an earlier
// session is refused, and a response that arrives after the switch is ignored. Nothing is lost by the
// drop: each pending delete keeps its .info file, and the scan queues it again when its account next
// signs in.
//
// A failed delete stays queued and waits before its next attempt, one minute after the first failure and
// twice as long after each further one, up to an hour.
class CloudDeleteQueue
{
public:
    using Clock = std::chrono::steady_clock;

    struct Entry
    {
        std::string setting_id;
        std::string info_path; // the deleted preset's .info file, removed once the cloud has no such profile
        std::string owner;     // user id of the account the delete belongs to
    };

    // What one sync pass sends: the due entries of the session's account, and the session they belong to.
    struct Batch
    {
        uint64_t           generation{0};
        std::vector<Entry> entries;
    };

    // Starts the session of `user_id`, empty when signed out. Another account drops every entry and
    // changes the generation; the account already signed in keeps both.
    void start_session(const std::string& user_id);

    uint64_t generation() const;
    bool     is_current(uint64_t generation) const;

    // Queues a delete for `owner` unless the session changed since `generation` was read. An empty owner
    // means the session's own account, the one whose presets are loaded. The check and the add are one
    // step under the queue's lock. A setting_id already queued keeps its entry and its wait. Returns false
    // when the delete was refused: an empty setting_id, or a session that has moved on.
    bool add(uint64_t generation, const std::string& setting_id, const std::string& info_path, const std::string& owner);

    // The entries of the session's account whose wait is over. Entries of other accounts stay queued unsent.
    Batch due(Clock::time_point now) const;

    // Records the result of sending `setting_id` in session `generation`. `gone` means the cloud holds no
    // such profile for this account any more: the entry leaves the queue and its .info path is returned for
    // removal. Otherwise the entry waits longer, counted from `now`. A response from an earlier session
    // changes nothing and returns nothing.
    std::optional<std::string> finish(uint64_t generation, const std::string& setting_id, bool gone, Clock::time_point now);

    // One sync pass: sends the due deletes one at a time through `send`, which returns true when the cloud
    // no longer has the profile, and hands each confirmed entry to `on_gone` once it has left the queue.
    // `current_user` names the account whose token `send` uses. That token can change to another account
    // just before the queue starts the new account's session, so before each request the pass checks that
    // the session is unchanged and the token belongs to the entry's owner, and after it that the token
    // still does; it stops at the first change. `now` times each result, so a long pass doesn't eat into
    // the waits of the failures it records.
    void send_due(const std::function<Clock::time_point()>& now,
                  const std::function<std::string()>&       current_user,
                  const std::function<bool(const Entry&)>&  send,
                  const std::function<void(const Entry&)>&  on_gone);

    size_t size() const;

private:
    struct Pending
    {
        Entry             entry;
        int               failures{0};
        Clock::time_point not_before{};
    };

    mutable std::mutex             m_mutex;
    std::string                    m_user_id;
    uint64_t                       m_generation{0};
    std::map<std::string, Pending> m_pending; // by setting_id
};

// Queues the delete recorded by each orphaned .info file under `user_dir`, the signed-in account's preset
// folder: a .info whose preset .json is gone marks a delete the cloud hasn't confirmed. The owner is the
// .info's user_id, or the session's account when it has none. Returns false when the session changed
// since `generation`, which ends the scan.
bool queue_orphaned_info_files(CloudDeleteQueue& queue, uint64_t generation, const std::string& user_dir);

// Removes a confirmed delete's .info file, under the lock other instances take to edit user presets.
void remove_info_file(const std::string& info_path);

// One pass of `queue` through `agent`: sends the signed-in account's due deletes, each only with its
// owner's token, and removes the .info file of every delete the cloud confirms before handing the entry
// to `on_gone`.
void send_cloud_deletes(CloudDeleteQueue&                                          queue,
                        OrcaCloudServiceAgent&                                     agent,
                        const std::function<void(const CloudDeleteQueue::Entry&)>& on_gone = {});

} // namespace Slic3r
