#ifndef slic3r_PluginAuditPromptQueue_hpp_
#define slic3r_PluginAuditPromptQueue_hpp_

#include "PluginAuditManager.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

// One runtime audit decision awaiting a user answer. `keys` is what a Yes persists; `shown` is
// what the dialog lists (empty for an event that is granted by name, with no target to show).
struct AuditPromptRequest
{
    AuditEventCategory       category = AuditEventCategory::None;
    std::string              plugin_key;
    std::string              plugin_name;
    std::string              event_name;
    bool                     by_event = false;
    std::vector<std::string> keys;
    std::vector<std::string> shown;
};

// Serializes runtime audit prompts on the GUI thread. At most one request is active: the next one
// is handed out by take_next() only after the previous dialog has returned from ShowModal(), so a
// modal dialog's nested event loop cannot open another audit dialog.
//
// Thread-safe: audit events enqueue from plugin worker threads, the GUI thread drains. Requests
// are never merged unless they are exactly equivalent, so coalescing cannot widen a grant.
class AuditPromptQueue
{
public:
    enum class Admission {
        ShowNow,  // idle and allowed to prompt here: present this request, then complete()
        Queued,   // a prompt is active or waiting: the current audit event stays denied
        Duplicate // an equivalent request is already active or waiting: this one is dropped
    };

    // `may_show_now` is true only on the GUI thread: it lets an idle queue hand the request back
    // for a synchronous prompt instead of queueing it for a later GUI callback.
    Admission admit(AuditPromptRequest request, bool may_show_now)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (contains_locked(request))
            return Admission::Duplicate;

        if (may_show_now && !m_active && m_pending.empty()) {
            m_active = request;
            return Admission::ShowNow;
        }

        m_pending.push_back(std::move(request));
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
        return m_active;
    }

    // Marks the active request done. Idempotent.
    void complete()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_active.reset();
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

    // Drops every queued request. An active prompt is unaffected, but complete() then hands out
    // nothing. Used on application shutdown.
    void discard_all()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending.clear();
    }

    // Drops queued requests for one plugin; an active prompt is unaffected. Used when a plugin is
    // unloaded or removed.
    void discard_plugin(const std::string& plugin_key)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
                                       [&plugin_key](const AuditPromptRequest& request) {
                                           return request.plugin_key == plugin_key;
                                       }),
                        m_pending.end());
    }

private:
    // Equality only over what a Yes would grant, plus the plugin it is granted to. Distinct
    // categories, plugins, targets, or event grants are never considered equivalent.
    static bool same_request(const AuditPromptRequest& a, const AuditPromptRequest& b)
    {
        return a.category == b.category && a.plugin_key == b.plugin_key && a.by_event == b.by_event &&
               a.keys == b.keys;
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
};

} // namespace Slic3r

#endif // slic3r_PluginAuditPromptQueue_hpp_
