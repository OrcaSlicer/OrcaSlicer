#ifndef slic3r_PluginAuditPromptQueue_hpp_
#define slic3r_PluginAuditPromptQueue_hpp_

#include "PluginAuditManager.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

// What a queued prompt asks the user. A runtime event and a permission declaration can both grant
// what they show; an informational denial cannot, so it has no Yes/No choice.
enum class AuditPromptKind {
    RuntimeEvent,
    DeclaredPermissions,
    DeniedOperation,
};

// One permission bucket of a load-time declaration. Kept plain (no wx types) so the queue can be
// owned by production code that outlives static destruction.
struct AuditPromptBucket
{
    AuditEventCategory       category = AuditEventCategory::None;
    std::vector<std::string> targets;

    bool operator==(const AuditPromptBucket& other) const
    {
        return category == other.category && targets == other.targets;
    }
};

// Resolves a blocked load worker exactly once. The queue may cancel a request from another thread
// (plugin unload, shutdown) while the GUI thread is still showing its dialog; the second resolve
// must be a no-op. Held by shared_ptr so a request copy and the queue's active slot share it.
struct AuditPromptCompletion
{
    void resolve(bool value)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_resolved)
            return;
        m_resolved = true;
        m_promise.set_value(value);
    }

    std::future<bool> get_future() { return m_promise.get_future(); }

private:
    std::mutex         m_mutex;
    std::promise<bool> m_promise;
    bool               m_resolved = false;
};

// One audit decision awaiting a user answer. `keys` is what a Yes persists; `shown` is what the
// dialog lists (empty for an event that is granted by name, with no target to show).
struct AuditPromptRequest
{
    AuditPromptKind          kind = AuditPromptKind::RuntimeEvent;
    AuditEventCategory       category = AuditEventCategory::None;
    std::string              plugin_key;
    std::string              plugin_name;
    std::string              event_name;
    bool                     by_event = false;
    std::vector<std::string> keys;
    std::vector<std::string> shown;
    // DeclaredPermissions only: the missing buckets the combined dialog lists.
    std::vector<AuditPromptBucket> buckets;
    // Set only when a worker is blocked waiting for the answer.
    std::shared_ptr<AuditPromptCompletion> completion;
};

// Serializes audit prompts on the GUI thread. At most one request is active: the next one is
// handed out by take_next() only after the previous dialog has returned from ShowModal(), so a
// modal dialog's nested event loop cannot open another audit prompt.
//
// Thread-safe: audit events enqueue from plugin worker threads, the GUI thread drains. Requests
// are never merged unless they are exactly equivalent, so coalescing cannot widen a grant.
class AuditPromptQueue
{
public:
    enum class Admission {
        ShowNow,   // idle and allowed to prompt here: present this request, then complete()
        Queued,    // a prompt is active or waiting: the current audit event stays denied
        Duplicate, // an equivalent request is already active or waiting: this one is dropped
        Rejected   // a synchronous-only caller found the queue busy: nothing was stored
    };

    // Presents a request synchronously only when the queue is idle. Busy callers are rejected
    // atomically instead of leaving a deferred prompt behind: used by GUI-thread callers whose
    // operation is already being denied and cannot wait for a later Yes.
    Admission admit_synchronous(AuditPromptRequest request)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_active || !m_pending.empty())
            return Admission::Rejected;

        m_active = std::move(request);
        m_active_cancelled  = false;
        m_active_committing = false;
        return Admission::ShowNow;
    }

    // `may_show_now` is true only on the GUI thread: it lets an idle queue hand the request back
    // for a synchronous prompt instead of queueing it for a later GUI callback.
    Admission admit(AuditPromptRequest request, bool may_show_now)
    {
        AuditPromptRequest queued = request;
        return admit_with_fallback(std::move(request), std::move(queued), may_show_now);
    }

    // For a create/delete request: show the immediate Yes/No prompt when idle on the GUI thread,
    // but queue `queued_request` (an OK-only informational denial) when that is not possible. The
    // kind cannot be switched after admission, so the fallback is chosen under the queue lock.
    Admission admit_with_fallback(AuditPromptRequest immediate_request, AuditPromptRequest queued_request,
                                  bool may_show_now)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (may_show_now && !m_active && m_pending.empty()) {
            m_active = std::move(immediate_request);
            m_active_cancelled  = false;
            m_active_committing = false;
            return Admission::ShowNow;
        }

        if (contains_locked(queued_request))
            return Admission::Duplicate;

        m_pending.push_back(std::move(queued_request));
        return Admission::Queued;
    }

    // Claims the oldest queued request when no prompt is active, and marks it active.
    std::optional<AuditPromptRequest> take_next()
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

    // Claims the active answer for persistence. Cancellation and commit are mutually exclusive:
    // once this returns true, discard_all()/discard_plugin() leave the request alone so a user's
    // accepted answer cannot be dropped midway through the sidecar write. A request already
    // cancelled (shutdown or unload won the race) returns false and must not be persisted.
    bool begin_commit()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_active || m_active_cancelled)
            return false;
        m_active_committing = true;
        return true;
    }

    // Marks the active request done. Returns true when it was cancelled while its dialog was up,
    // so the caller must not persist an accepted grant. Idempotent.
    bool complete()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const bool cancelled = m_active_cancelled;
        m_active.reset();
        m_active_cancelled  = false;
        m_active_committing = false;
        return cancelled;
    }

    bool has_pending() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !m_pending.empty();
    }

    bool idle() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !m_active && m_pending.empty();
    }

    std::size_t pending_count() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_pending.size();
    }

    // Drops every queued request and cancels any blocked worker. An active prompt stays on screen
    // (it may be mid-modal), but its completion is released now and its late answer must not be
    // persisted. A commit already begun is left to finish. Used on application shutdown.
    void discard_all()
    {
        std::vector<std::shared_ptr<AuditPromptCompletion>> completions;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const AuditPromptRequest& request : m_pending)
                if (request.completion)
                    completions.push_back(request.completion);
            m_pending.clear();

            if (m_active && !m_active_committing) {
                m_active_cancelled = true;
                if (m_active->completion)
                    completions.push_back(m_active->completion);
            }
        }

        // Never resolve under the queue lock: the wake-up is another thread that may re-enter.
        for (const auto& completion : completions)
            completion->resolve(false);
    }

    // Drops queued requests for one plugin and cancels a blocked worker of that plugin. An active
    // prompt is cancelled but stays on screen until its dialog returns, unless its commit already
    // started. Used when a plugin is unloaded or removed.
    void discard_plugin(const std::string& plugin_key)
    {
        std::vector<std::shared_ptr<AuditPromptCompletion>> completions;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const AuditPromptRequest& request : m_pending)
                if (request.plugin_key == plugin_key && request.completion)
                    completions.push_back(request.completion);

            m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
                                           [&plugin_key](const AuditPromptRequest& request) {
                                               return request.plugin_key == plugin_key;
                                           }),
                            m_pending.end());

            if (m_active && m_active->plugin_key == plugin_key && !m_active_committing) {
                m_active_cancelled = true;
                if (m_active->completion)
                    completions.push_back(m_active->completion);
            }
        }

        for (const auto& completion : completions)
            completion->resolve(false);
    }

private:
    // Equality only over what a Yes would grant, plus the plugin it is granted to. Distinct
    // categories, plugins, targets, or event grants are never considered equivalent.
    static bool same_request(const AuditPromptRequest& a, const AuditPromptRequest& b)
    {
        if (a.kind != b.kind || a.plugin_key != b.plugin_key)
            return false;

        switch (a.kind) {
        case AuditPromptKind::RuntimeEvent:
            return a.category == b.category && a.by_event == b.by_event && a.keys == b.keys;
        case AuditPromptKind::DeclaredPermissions:
            return a.buckets == b.buckets;
        case AuditPromptKind::DeniedOperation:
            return a.category == b.category && a.shown == b.shown;
        }
        return false;
    }

    bool contains_locked(const AuditPromptRequest& request) const
    {
        if (m_active && same_request(*m_active, request))
            return true;
        return std::any_of(m_pending.begin(), m_pending.end(),
                           [&request](const AuditPromptRequest& pending) { return same_request(pending, request); });
    }

    mutable std::mutex                m_mutex;
    std::deque<AuditPromptRequest>    m_pending;
    std::optional<AuditPromptRequest> m_active;
    bool                              m_active_cancelled = false;
    bool                              m_active_committing = false;
};

} // namespace Slic3r

#endif // slic3r_PluginAuditPromptQueue_hpp_
