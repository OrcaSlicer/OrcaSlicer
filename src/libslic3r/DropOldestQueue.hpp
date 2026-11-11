#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

namespace Slic3r {

// A thread-safe FIFO that never holds more than `capacity` items: pushing into a full queue
// drops the oldest item. A live stream uses it between the thread that receives frames and the
// thread that shows them, so a consumer that falls behind loses frames instead of falling ever
// further behind.
template<class T> class DropOldestQueue
{
public:
    explicit DropOldestQueue(size_t capacity) : m_capacity(std::max<size_t>(capacity, 1)) {}

    void push(T value)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_items.size() >= m_capacity)
                m_items.pop_front();
            m_items.push_back(std::move(value));
        }
        m_cond.notify_one();
    }

    // Waits up to `timeout` for an item. Returns false when none arrived.
    bool pop(T &out, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_cond.wait_for(lock, timeout, [this] { return !m_items.empty(); }))
            return false;
        out = std::move(m_items.front());
        m_items.pop_front();
        return true;
    }

    void set_capacity(size_t capacity)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_capacity = std::max<size_t>(capacity, 1);
        while (m_items.size() > m_capacity)
            m_items.pop_front();
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_items.clear();
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_items.size();
    }

private:
    mutable std::mutex      m_mutex;
    std::condition_variable m_cond;
    std::deque<T>           m_items;
    size_t                  m_capacity;
};

} // namespace Slic3r
