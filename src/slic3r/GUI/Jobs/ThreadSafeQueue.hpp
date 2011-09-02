#ifndef THREADSAFEQUEUE_HPP
#define THREADSAFEQUEUE_HPP

#include <type_traits>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>

namespace Slic3r { namespace GUI {

// Helper structure for overloads of ThreadSafeQueueSPSC::consume_one()
// to block if the queue is empty.
struct BlockingWait
{
    // Timeout to wait for the arrival of new element into the queue.
    unsigned timeout_ms = 0;

    // An optional atomic flag to set true if an incoming element gets
    // consumed. The flag will be atomically set to true when popping the
    // front of the queue.
    std::atomic<bool> *pop_flag = nullptr;
};

// A thread safe queue for one producer and one consumer.
template<class T,
         template<class, class...> class Container = std::deque,
         class... ContainerArgs>
class ThreadSafeQueueSPSC
{
    std::queue<T, Container<T, ContainerArgs...>> m_queue;
    mutable std::mutex m_mutex;
    std::condition_variable m_cond_var;
public:

    // Consume one element, block if the queue is empty.
    template<class Fn> bool consume_one(const BlockingWait &blkw, Fn &&fn)
    {
        static_assert(!std::is_reference_v<T>, "");
        static_assert(std::is_default_constructible_v<T>, "");
        static_assert(std::is_move_assignable_v<T> || std::is_copy_assignable_v<T>, "");

        T el;
        {
            std::unique_lock lk{m_mutex};

            auto pred = [this]{ return !m_queue.empty(); };
            if (blkw.timeout_ms > 0) {
                auto timeout = std::chrono::milliseconds(blkw.timeout_ms);
                if (!m_cond_var.wait_for(lk, timeout, pred))
                    return false;
            }
            else
                m_cond_var.wait(lk, pred);

            if constexpr (std::is_move_assignable_v<T>)
                el = std::move(m_queue.front());
            else
                el = m_queue.front();

            m_queue.pop();

            if (blkw.pop_flag)
                // The optional flag is set before the lock us unlocked.
                blkw.pop_flag->store(true);
        }

        fn(el);
        return true;
    }

    // Consume one element, return true if consumed, false if queue was empty.
    template<class Fn> bool consume_one(Fn &&fn)
    {
        T el;
        {
            std::unique_lock lk{m_mutex};
            if (!m_queue.empty()) {
                if constexpr (std::is_move_assignable_v<T>)
                    el = std::move(m_queue.front());
                else
                    el = m_queue.front();

                m_queue.pop();
            } else
                return false;
        }

        fn(el);

        return true;
    }

    // Push element into the queue.
    template<class...TArgs> void push(TArgs&&...el)
    {
        std::lock_guard lk{m_mutex};
        m_queue.emplace(std::forward<TArgs>(el)...);
        m_cond_var.notify_one();
    }

    bool empty() const
    {
        std::lock_guard lk{m_mutex};
        return m_queue.empty();
    }

    size_t size() const
    {
        std::lock_guard lk{m_mutex};
        return m_queue.size();
    }

    void clear()
    {
        std::lock_guard lk{m_mutex};
        while (!m_queue.empty())
            m_queue.pop();
    }
};

}} // namespace Slic3r::GUI

#endif // THREADSAFEQUEUE_HPP
