#pragma once

#include "slic3r/GUI/CAD/CadFaceSelection.hpp"
#include "slic3r/GUI/CAD/CadShapeSelection.hpp"

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

template<class Value>
class CadSelectionSubscription
{
public:
    using Callback = std::function<void(const std::optional<Value>&, const CadSelectionSubscription&)>;

    CadSelectionSubscription(std::string owner, Callback callback)
        : m_owner(std::move(owner)), m_callback(std::move(callback)) {}
    ~CadSelectionSubscription() { close(); }

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

    void deliver(const std::optional<Value>& selection) const
    {
        Callback callback;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            callback = m_callback;
        }
        // A callback may close itself, subscribe again, or query the current selection.
        if (callback)
            callback(selection, *this);
    }

private:
    const std::string m_owner;
    mutable std::mutex m_mutex;
    Callback m_callback;
};

// Native selection changes update this copy on the UI thread. Python can read/subscribe from
// a Script worker without a blocking UI round-trip. The returned notification is queued by the
// GUI, so no Python callback runs inside the native mouse handler.
template<class Value>
class CadSelectionState
{
public:
    using Subscription = CadSelectionSubscription<Value>;
    using Snapshot = std::optional<Value>;
    Snapshot selected() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_selection;
    }

    std::shared_ptr<Subscription> subscribe(std::string owner, typename Subscription::Callback callback)
    {
        auto subscription = std::make_shared<Subscription>(std::move(owner), std::move(callback));
        std::lock_guard<std::mutex> lock(m_mutex);
        m_subscriptions.erase(std::remove_if(m_subscriptions.begin(), m_subscriptions.end(),
                                            [](const auto& weak) { return weak.expired(); }), m_subscriptions.end());
        m_subscriptions.push_back(subscription);
        return subscription;
    }

    std::function<void()> update(Snapshot selection)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if ((!selection && !m_selection) || (selection && m_selection && selection->same_geometry(*m_selection)))
            return {};
        ++m_revision;
        if (selection)
            selection->selection_revision = m_revision;
        m_selection = selection;
        // Weak handles: queued events neither keep a forgotten subscription alive nor deliver
        // to one that has been closed. A later subscriber does not receive earlier events.
        return [subscriptions = m_subscriptions, selection = std::move(selection)] {
            for (const auto& weak : subscriptions)
                if (auto subscription = weak.lock())
                    subscription->deliver(selection);
        };
    }

    void close_for_plugin(const std::string& owner)
    {
        std::vector<std::shared_ptr<Subscription>> closing;
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
    Snapshot m_selection;
    std::uint64_t m_revision{0};
    std::vector<std::weak_ptr<Subscription>> m_subscriptions;
};

using CadFaceSubscription = CadSelectionSubscription<GUI::CadFaceSelection>;
using CadFaceSelectionState = CadSelectionState<GUI::CadFaceSelection>;
using CadShapeSubscription = CadSelectionSubscription<GUI::CadShapeSelection>;
using CadShapeSelectionState = CadSelectionState<GUI::CadShapeSelection>;

} // namespace Slic3r
