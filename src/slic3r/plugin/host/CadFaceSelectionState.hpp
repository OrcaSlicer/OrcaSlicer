#pragma once

#include "slic3r/GUI/CAD/CadFaceSelection.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

using CadFaceSnapshot = std::optional<GUI::CadFaceSelection>;

class CadFaceSubscription
{
public:
    using Callback = std::function<void(const CadFaceSnapshot&, const CadFaceSubscription&)>;

    CadFaceSubscription(std::string owner, Callback callback)
        : m_owner(std::move(owner)), m_callback(std::move(callback)) {}
    ~CadFaceSubscription() { close(); }

    void close()
    {
        Callback released;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            released.swap(m_callback);
        }
        // Destroy a Python callback outside the lock: its holder may need the GIL.
    }

    bool is_active() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return bool(m_callback);
    }

    const std::string& owner() const { return m_owner; }

    void deliver(const CadFaceSnapshot& face) const
    {
        Callback callback;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            callback = m_callback;
        }
        // A callback may close itself, subscribe again, or query the current selection.
        if (callback)
            callback(face, *this);
    }

private:
    const std::string m_owner;
    mutable std::mutex m_mutex;
    Callback m_callback;
};

// Native selection changes update this copy on the UI thread. Python can read/subscribe from
// a Script worker without a blocking UI round-trip. The returned notification is queued by the
// GUI, so no Python callback runs inside the native mouse handler.
class CadFaceSelectionState
{
public:
    CadFaceSnapshot selected_face() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_face;
    }

    std::shared_ptr<CadFaceSubscription> subscribe(std::string owner, CadFaceSubscription::Callback callback)
    {
        auto subscription = std::make_shared<CadFaceSubscription>(std::move(owner), std::move(callback));
        std::lock_guard<std::mutex> lock(m_mutex);
        m_subscriptions.erase(std::remove_if(m_subscriptions.begin(), m_subscriptions.end(),
                                            [](const auto& weak) { return weak.expired(); }), m_subscriptions.end());
        m_subscriptions.push_back(subscription);
        return subscription;
    }

    std::function<void()> update(CadFaceSnapshot face)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if ((!face && !m_face) || (face && m_face && face->same_geometry(*m_face)))
            return {};
        ++m_revision;
        if (face)
            face->selection_revision = m_revision;
        m_face = face;
        // Weak handles: queued events neither keep a forgotten subscription alive nor deliver
        // to one that has been closed. A later subscriber does not receive earlier events.
        return [subscriptions = m_subscriptions, face = std::move(face)] {
            for (const auto& weak : subscriptions)
                if (auto subscription = weak.lock())
                    subscription->deliver(face);
        };
    }

    void close_for_plugin(const std::string& owner)
    {
        std::vector<std::shared_ptr<CadFaceSubscription>> closing;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto& weak : m_subscriptions)
                if (auto subscription = weak.lock(); subscription && subscription->owner() == owner)
                    closing.push_back(std::move(subscription));
        }
        for (const auto& subscription : closing)
            subscription->close();
    }

private:
    mutable std::mutex m_mutex;
    CadFaceSnapshot m_face;
    std::uint64_t m_revision{0};
    std::vector<std::weak_ptr<CadFaceSubscription>> m_subscriptions;
};

} // namespace Slic3r
