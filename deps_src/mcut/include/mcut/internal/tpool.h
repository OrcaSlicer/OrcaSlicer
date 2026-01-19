/**
 * Copyright (c) 2021-2022 Floyd M. Chitalu.
 * All rights reserved.
 *
 * NOTE: This file is licensed under GPL-3.0-or-later (default).
 * A commercial license can be purchased from Floyd M. Chitalu.
 *
 * License details:
 *
 * (A)  GNU General Public License ("GPL"); a copy of which you should have
 *      recieved with this file.
 * 	    - see also: <http://www.gnu.org/licenses/>
 * (B)  Commercial license.
 *      - email: floyd.m.chitalu@gmail.com
 *
 * The commercial license options is for users that wish to use MCUT in
 * their products for comercial purposes but do not wish to release their
 * software products under the GPL license.
 *
 * Author(s)     : Floyd M. Chitalu
 */

#ifndef MCUT_SCHEDULER_H_
#define MCUT_SCHEDULER_H_

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <stack>
#include <functional>
#include <list>
#include <utility>

#include "mcut/internal/utils.h"

class function_wrapper {
private:
    struct impl_base {
        virtual void call() = 0;
        virtual ~impl_base() { }
    };

    std::unique_ptr<impl_base> impl;

    template <typename F>
    struct impl_type : impl_base {
        F f;
        impl_type(F&& f_)
            : f(std::move(f_))
        {
        }
        void call() { return f(); }
    };

public:
    template <typename F>
    function_wrapper(F&& f)
        : impl(new impl_type<F>(std::move(f)))
    {
    }

    void operator()() { impl->call(); }

    function_wrapper() = default;

    function_wrapper(function_wrapper&& other) noexcept
        : impl(std::move(other.impl))
    {
    }

    function_wrapper& operator=(function_wrapper&& other) noexcept
    {
        impl = std::move(other.impl);
        return *this;
    }

    function_wrapper(const function_wrapper&) = delete;
    function_wrapper(function_wrapper&) = delete;
    function_wrapper& operator=(const function_wrapper&) = delete;
};

template <typename T>
class thread_safe_queue {
private:
    std::atomic<bool>* m_done;
    struct node {
        std::shared_ptr<T> data;
        std::unique_ptr<node> next;
    };

    std::mutex head_mutex;
    std::unique_ptr<node> head;
    std::mutex tail_mutex;
    node* tail;
    std::condition_variable data_cond;

    std::unique_ptr<node> try_pop_head(T& value)
    {
        std::lock_guard<std::mutex> head_lock(head_mutex);
        if (head.get() == get_tail()) {
            return std::unique_ptr<node>(nullptr);
        }
        value = std::move(*head->data);
        return pop_head();
    }

    node* get_tail()
    {
        std::lock_guard<std::mutex> tail_lock(tail_mutex);
        return tail;
    }

    // dont call directly
    std::unique_ptr<node> pop_head()
    {
        std::unique_ptr<node> old_head = std::move(head);
        head = std::move(old_head->next);
        return (old_head);
    }

#if _WIN32 // on windows
    // there is a C26115 warning here, which is by design. The warning is caused by
    // the fact that our function here has the side effect of locking the mutex "head_mutex"
    // via the construction of the unique_lock.
    // The warning is removed by annotating the function with "_Acquires_lock_(...)".
    // See here: https://developercommunity.visualstudio.com/t/unexpected-warning-c26115-for-returning-a-unique-l/1077322
    _Acquires_lock_(return)
#endif
        std::unique_lock<std::mutex> wait_for_data()
    {
        std::unique_lock<std::mutex> head_lock(head_mutex);
        auto until = [&]() { return m_done->load() || head.get() != get_tail(); };
        data_cond.wait(head_lock, until);
        return head_lock;
    }

    std::unique_ptr<node> wait_pop_head(T& value)
    {
        std::unique_lock<std::mutex> head_lock(wait_for_data());
        if (m_done->load() == false) {
            value = std::move(*head->data);
            return pop_head();
        } else {
            return std::unique_ptr<node>(nullptr);
        }
    }

public:
    thread_safe_queue()
        :  head(new node)
        , tail(head.get()) /*, can_wait_for_data(true)*/
    {
    }
    thread_safe_queue(const thread_safe_queue& other) = delete;
    thread_safe_queue& operator=(const thread_safe_queue& other) = delete;

    void set_done_ptr(std::atomic<bool>* done)
    {
        m_done = done;
    }

    void disrupt_wait_for_data()
    {
        // https://stackoverflow.com/questions/60658842/condition-variable-doesnt-get-notified-to-wake-up-even-with-a-predicate
        //
        // Need to this to prevent data race, which can happen when user-app thread
        // is too slow to reach this function i.e. that an API/context/device thread
        // waits on the condition variable AFTER the client thread calls notify_one()
        std::unique_lock<std::mutex> lock(head_mutex);
        data_cond.notify_one();
    }

    void notify_one()
    {
        data_cond.notify_one();
    }

    void push(T new_value)
    {
        std::shared_ptr<T> new_data(std::make_shared<T>(std::move(new_value)));
        std::unique_ptr<node> p(new node);
        {
            std::lock_guard<std::mutex> tail_lock(tail_mutex);
            tail->data = new_data;
            node* const new_tail = p.get();
            tail->next = std::move(p);
            tail = new_tail;
        }
        data_cond.notify_one();
    }

    void wait_and_pop(T& value)
    {
        std::unique_ptr<node> const old_head = wait_pop_head(value);
    }

    bool try_pop(T& value)
    {
        std::unique_ptr<node> const old_head = try_pop_head(value);
        return !(!(old_head)); // https://stackoverflow.com/questions/30521849/error-on-implicit-cast-from-stdunique-ptr-to-bool
    }

    bool empty()
    {
        std::lock_guard<std::mutex> head_lock(head_mutex);
        return (head.get() == get_tail());
    }
};

class join_threads {
    std::vector<std::thread>& threads;

public:
    explicit join_threads(std::vector<std::thread>& threads_)
        : threads(threads_)
    {
    }
    ~join_threads()
    {
        for (unsigned long i = 0; i < threads.size(); ++i) {
            if (threads[i].joinable())
                threads[i].join();
        }
    }
};

class thread_pool {
    std::atomic<bool> m_done;
    std::vector<thread_safe_queue<function_wrapper>> work_queues;

    std::vector<std::thread> threads; // NOTE: must be declared after "thread_pool_terminate" and "work_queues"
    join_threads joiner;

    void worker_thread(int thread_id)
    {
        log_msg("[MCUT] Launch helper thread " << std::this_thread::get_id() << " (" << thread_id << ")");

        do {
            function_wrapper task;

            if (!(work_queues[thread_id].try_pop(task))) {
                work_queues[thread_id].wait_and_pop(task);
            }

            if (m_done) {
                break; // finished (i.e. MCUT context was destroyed)
            }

            task(); // run the task

        } while (true);

        log_msg("[MCUT] Shutdown helper thread " << std::this_thread::get_id() << " (" << thread_id << ")");
    }

    uint32_t machine_thread_count;

public:
    thread_pool(uint32_t nthreads, uint32_t used_cores)
        :  m_done(false),

        joiner(threads)
        , machine_thread_count(0)
    {
        log_msg("[MCUT] Create threadpool " << this);
        machine_thread_count = (uint32_t)std::thread::hardware_concurrency();
        uint32_t const pool_thread_count = std::min(
            nthreads, 
            // prevents over-subscription given that "N=used_cores" API threads are already in use
            machine_thread_count - used_cores);

        try {

            work_queues = std::vector<thread_safe_queue<function_wrapper>>(pool_thread_count);
            
            for (unsigned i = 0; i < pool_thread_count; ++i) {
                work_queues[i].set_done_ptr(&m_done);
                threads.push_back(std::thread(&thread_pool::worker_thread, this, i));
            }
        } catch (...) {
            m_done.store(true);
            wakeup_and_shutdown();
            throw;
        }
    }

    ~thread_pool()
    {
        log_msg("[MCUT] Destroy threadpool " << this);
        
        m_done.store(true);
        wakeup_and_shutdown();
    }

    // submit empty task so that worker threads can wake up
    // with a valid (but redundant) task to then exit
    void wakeup_and_shutdown()
    {
        for (unsigned i = 0; i < get_num_threads(); ++i) {
            work_queues[i].disrupt_wait_for_data();
        }
    }

public:
    /*
    The thread pool takes care of the exception safety too. Any exception thrown by the
    task gets propagated through the std::future returned from submit() , and if the function
    exits with an exception, the thread pool destructor abandons any not-yet-completed
    tasks and waits for the pool threads to finish.
*/
    template <typename FunctionType>
    std::future<typename std::result_of<FunctionType()>::type> submit(uint32_t worker_thread_id, FunctionType f)
    {
        typedef typename std::result_of<FunctionType()>::type result_type;

        std::packaged_task<result_type()> task(std::move(f));
        std::future<result_type> res(task.get_future());

        work_queues[worker_thread_id].push(std::move(task));

        return res;
    }

    size_t get_num_threads() const
    {
        return threads.size();
    }

    uint32_t get_num_hardware_threads()
    {
        return machine_thread_count;
    }
};

static void get_scheduling_parameters(
    // the number of thread that will actually do some computation (including master)
    uint32_t& num_threads,
    // maximum possible number of threads that can be scheduled to perform the task.
    uint32_t& max_threads,
    // the size of each block of elements assigned to a thread (note: last thread,
    // which is the master thread might have less)
    uint32_t& block_size,
    // number of data elements to process
    const uint32_t length,
    const uint32_t available_threads, // number of worker threads and master master thread
    // minimum number of element assigned to a thread (below this threshold and we
    // run just one thread)
    const uint32_t min_per_thread = (1 << 10))
{
    // maximum possible number of threads that can be scheduled to perform the task.
    max_threads = (length + min_per_thread - 1) / min_per_thread;

    // the number of thread that will actually do some computation (including master)
    num_threads = std::min(available_threads != 0 ? available_threads : 2, max_threads);

    // the size of each block of elements assigned to a thread (note: last thread,
    // which is the master thread might have less)
    block_size = length / num_threads;

    // printf("max_threads=%u num_threads=%u block_size=%u\n", max_threads, num_threads, block_size);

    if (num_threads == 0 || num_threads > available_threads) {
        throw std::runtime_error("invalid number of threads (" + std::to_string(num_threads) + ")");
    }

    if (max_threads == 0) {
        throw std::runtime_error("invalid maximum posible number of threads (" + std::to_string(max_threads) + ")");
    }

    if (block_size == 0) {
        throw std::runtime_error("invalid work block-size per thread (" + std::to_string(block_size) + ")");
    }
}

class barrier_t {
    unsigned const count;
    std::atomic<unsigned> spaces;
    std::atomic<unsigned> generation;

public:
    explicit barrier_t(unsigned count_)
        : count(count_)
        , spaces(count)
        , generation(0)
    {
    }
    void wait()
    {
        unsigned const my_generation = generation;
        if (!--spaces) {
            spaces = count;
            ++generation;
        } else {
            while (generation == my_generation)
                std::this_thread::yield();
        }
    }
};

template <typename InputStorageIteratorType, typename OutputStorageType, typename FunctionType>
void parallel_for(
    thread_pool& pool,
    // start of data elements to be processed in parallel
    const InputStorageIteratorType& first,
    // end of of data elements to be processed in parallel (e.g. std::map::end())
    const InputStorageIteratorType& last,
    // the function that is executed on a sub-block of element within the range [first, last)
    FunctionType& task_func,
    // the part of the result/output that is computed by the master thread (i.e. the one that is scheduling)
    // NOTE: this result must be merged which the output computed for the other
    // sub-block in the input ranges. This other data is accessed from the std::futures
    OutputStorageType& master_thread_output,
    // Future promises of data (to be merged) that is computed by worker threads
    std::vector<std::future<OutputStorageType>>& futures,
    // minimum number of element assigned to a thread (below this threshold and we
    // run just one thread)
    const uint32_t min_per_thread = (1 << 10))
{
    uint32_t const length_ = std::distance(first, last);

    MCUT_ASSERT(length_ != 0);
    uint32_t block_size = 0;

    uint32_t max_threads = 0;
    const uint32_t available_threads = pool.get_num_threads() + 1; // workers and master (+1)
    uint32_t num_threads = 0;

    get_scheduling_parameters(
        num_threads,
        max_threads,
        block_size,
        length_,
        available_threads,
        min_per_thread);

    futures.resize(num_threads - 1);
    InputStorageIteratorType block_start = first;

    for (uint32_t i = 0; i < (num_threads - 1); ++i) {
        InputStorageIteratorType block_end = block_start;

        std::advance(block_end, block_size);

        futures[i] = pool.submit(i,
            [&, block_start, block_end]() -> OutputStorageType {
                return task_func(block_start, block_end);
            });

        block_start = block_end;
    }

    master_thread_output = task_func(block_start, last);
}

template <typename InputStorageIteratorType, typename FunctionType>
void parallel_for(
    thread_pool& pool,
    // start of data elements to be processed in parallel
    const InputStorageIteratorType& first,
    // end of of data elements to be processed in parallel (e.g. std::map::end())
    const InputStorageIteratorType& last,
    // the function that is executed on a sub-block of element within the range [first, last)
    // return void
    FunctionType& task_func,
    // minimum number of element assigned to a thread (below this threshold and we
    // run just one thread)
    const uint32_t min_per_thread = (1 << 10))
{
    uint32_t const length_ = std::distance(first, last);

    MCUT_ASSERT(length_ != 0);

    uint32_t block_size;

    uint32_t max_threads = 0;
    const uint32_t available_threads = pool.get_num_threads() + 1; // workers and master (+1)
    uint32_t num_threads = 0;

    get_scheduling_parameters(
        num_threads,
        max_threads,
        block_size,
        length_,
        available_threads,
        min_per_thread);

    std::vector<std::future<void>> futures;
    futures.resize(num_threads - 1);
    InputStorageIteratorType block_start = first;

    for (uint32_t i = 0; i < (num_threads - 1); ++i) {
        InputStorageIteratorType block_end = block_start;

        std::advance(block_end, block_size);

        futures[i] = pool.submit(i,
            [&, block_start, block_end]() {
                task_func(block_start, block_end);
            });

        block_start = block_end;
    }

    task_func(block_start, last); // master thread work

    for (uint32_t i = 0; i < (uint32_t)futures.size(); ++i) {
        futures[i].wait();
    }
}

template <typename Iterator>
void partial_sum(Iterator first, Iterator last, Iterator d_first)
{
    // std::vector<uint32_t>::iterator first = cc_uptr->face_sizes_cache.begin();
    // std::vector<uint32_t>::iterator last = cc_uptr->face_sizes_cache.end();
    // Iterator d_first = first;

    if (first != last) {

        uint32_t sum = *first;
        *d_first = sum;

        while (++first != last) {
            sum = sum + *first;
            *++d_first = sum;
        }

        ++d_first;
    }
}

template <typename Iterator>
void parallel_partial_sum(thread_pool& pool, Iterator first, Iterator last)
{
    typedef typename Iterator::value_type value_type;
    struct process_chunk {
        void operator()(Iterator begin, Iterator last,
            std::future<value_type>* previous_end_value,
            std::promise<value_type>* end_value)
        {
            try {
                Iterator end = last;
                ++end;
                partial_sum(begin, end, begin);
                if (previous_end_value) {
                    const value_type addend = previous_end_value->get();
                    *last += addend;
                    if (end_value) {
                        end_value->set_value(*last);
                    }
                    std::for_each(begin, last, [addend](value_type& item) {
                        item += addend;
                    });
                } else if (end_value) {
                    end_value->set_value(*last);
                }
            } catch (...) {
                if (end_value) {
                    end_value->set_exception(std::current_exception());
                } else {
                    throw;
                }
            }
        }
    };

    // number of elements in range
    unsigned long const length = std::distance(first, last);

    if (!length)
        return;

    uint32_t max_threads = 0;
    const uint32_t available_threads = pool.get_num_threads() + 1; // workers and master (+1)
    uint32_t num_threads = 0;
    uint32_t block_size = 0;

    get_scheduling_parameters(
        num_threads,
        max_threads,
        block_size,
        length,
        available_threads);

    typedef typename Iterator::value_type value_type;

    // promises holding the value of the last element of a block. Only the
    // first N-1 blocks (i.e. threads) will have to update this value.
    std::vector<std::promise<value_type>> end_values(num_threads - 1);
    // futures that are used to wait for the end value (the last element) of
    // previous block. Only the last N-1 blocks (i.e. threads) will have to wait
    // on this value. That is, the first thread has no "previous" block to wait
    // on,
    std::vector<std::future<value_type>> previous_end_values;
    previous_end_values.reserve(num_threads - 1);
    std::vector<std::future<void>> futures;
    futures.resize(num_threads - 1);

    Iterator block_start = first;

    // for each pool-thread
    for (unsigned long i = 0; i < (num_threads - 1); ++i) {
        Iterator block_last = block_start;
        std::advance(block_last, block_size - 1);

        // process_chunk()" handles all synchronisation
        futures[i] = pool.submit(i,
            [i /*NOTE: capture by-value*/, &previous_end_values, &end_values, block_start, block_last]() {
                process_chunk()(block_start, block_last, (i != 0) ? &previous_end_values[i - 1] : 0, &end_values[i]);
            });

        block_start = block_last;
        ++block_start;
        previous_end_values.push_back(end_values[i].get_future()); // for next thread to wait on
    }

    Iterator final_element = block_start;
    std::advance(final_element, std::distance(block_start, last) - 1);

    // NOTE: this is a blocking call since the master thread has to wait
    // for worker-threads assigned to preceeding chunks to finish before it can
    // process its own chunk.
    process_chunk()(block_start, final_element,
        (num_threads > 1) ? &previous_end_values.back() : 0,
        0);
}

template <typename Iterator, typename MatchType>
void find_element(Iterator begin, Iterator end,
    MatchType match,
    std::promise<Iterator>* result,
    std::atomic<bool>* done_flag)
{
    try {
        for (; (begin != end) && !done_flag->load(); ++begin) {
            if (*begin == match) {
                result->set_value(begin);
                done_flag->store(true);
                return;
            }
        }
    } catch (...) {
        try {
            result->set_exception(std::current_exception());
            done_flag->store(true);
        } catch (...) {
        }
    }
}

template <typename Iterator, typename MatchType>
Iterator parallel_find(thread_pool& pool, Iterator first, Iterator last, MatchType match)
{
    unsigned long const length = std::distance(first, last);

    if (!length)
        return last;

    uint32_t max_threads = 0;
    const uint32_t available_threads = pool.get_num_threads() + 1; // workers and master (+1)
    uint32_t num_threads = 0;
    uint32_t block_size = 0;

    get_scheduling_parameters(
        num_threads,
        max_threads,
        block_size,
        length,
        available_threads);

    std::promise<Iterator> result;
    std::atomic<bool> done_flag(false);
    std::vector<std::future<void>> futures;
    futures.resize(num_threads - 1);

    {
        Iterator block_start = first;
        for (unsigned long i = 0; i < (num_threads - 1); ++i) {
            Iterator block_end = block_start;
            std::advance(block_end, block_size);

            // find_element()" handles all synchronisation
            futures[i] = pool.submit(i,
                [&result, &done_flag, block_start, block_end, &match]() {
                    find_element<Iterator, MatchType>,
                        block_start, block_end, match,
                        &result, &done_flag;
                });
            block_start = block_end;
        }
        find_element<Iterator, MatchType>(block_start, last, match, &result, &done_flag);
    }
    if (!done_flag.load()) {
        return last; // if nothing found (by any thread), return "end"
    }
    return result.get_future().get();
}

template <typename Iterator, typename KeyType>
void find_map_element_by_key(Iterator begin, Iterator end,
    KeyType match,
    std::promise<Iterator>* result,
    std::atomic<bool>* done_flag,
    barrier_t* barrier_ptr)

{
    try {
        for (; (begin != end) && !done_flag->load(); ++begin) {
            if (begin->first == match) {
                result->set_value(begin);
                done_flag->store(true);
                break; // return;
            }
        }
    } catch (...) {
        try {
            result->set_exception(std::current_exception());
            done_flag->store(true);
        } catch (...) {
        }
    }
    barrier_ptr->wait();
}

template <typename Iterator, typename KeyType>
Iterator parallel_find_in_map_by_key(thread_pool& pool, Iterator first, Iterator last, KeyType match)
{
    unsigned long const length = std::distance(first, last);

    if (!length)
        return last;

    uint32_t max_threads = 0;
    const uint32_t available_threads = pool.get_num_threads() + 1; // workers and master (+1)
    uint32_t num_threads = 0;
    uint32_t block_size = 0;

    get_scheduling_parameters(
        num_threads,
        max_threads,
        block_size,
        length,
        available_threads);

    barrier_t barrier(num_threads);

    std::promise<Iterator> result;
    std::atomic<bool> done_flag(false);

    {
        std::vector<std::future<void>> futures;
        futures.resize(num_threads - 1);

        Iterator block_start = first;
        for (unsigned long i = 0; i < (num_threads - 1); ++i) {
            Iterator block_end = block_start;
            std::advance(block_end, block_size);

            futures[i] = pool.submit(i,
                [&result, &done_flag, block_start, block_end, &match, &barrier]() {
                    find_map_element_by_key<Iterator, KeyType>(
                        block_start, block_end, match,
                        &result, &done_flag, &barrier);
                });
            block_start = block_end;
        }
        find_map_element_by_key<Iterator, KeyType>(block_start, last, match, &result, &done_flag, &barrier);

        for (uint32_t i = 0; i < (uint32_t)futures.size(); ++i) {
            futures[i].wait();
        }
    }

    if (!done_flag.load()) {
        return last; // if nothing found (by any thread), return "end"
    }
    return result.get_future().get();
}

template <typename Iterator, typename UnaryPredicate>
void find_element_with_pred(Iterator begin, Iterator end,
    UnaryPredicate pred,
    std::promise<Iterator>* result,
    std::atomic<bool>* done_flag,
    barrier_t* barrier_ptr)
{
    try {
        for (; (begin != end) && !done_flag->load(); ++begin) {
            const bool found = pred(*begin);
            if (found) {
                result->set_value(begin);
                done_flag->store(true);
                break; // return;
            }
        }
    } catch (...) {
        try {
            result->set_exception(std::current_exception());
            done_flag->store(true);
        } catch (...) {
        }
    }
    barrier_ptr->wait();
}

template <typename Iterator, class UnaryPredicate>
Iterator parallel_find_if(thread_pool& pool, Iterator first, Iterator last, UnaryPredicate predicate)
{
    unsigned long const length = std::distance(first, last);

    if (!length)
        return last;

    uint32_t max_threads = 0;
    const uint32_t available_threads = pool.get_num_threads() + 1; // workers and master (+1)
    uint32_t num_threads = 0;
    uint32_t block_size = 0;

    get_scheduling_parameters(
        num_threads,
        max_threads,
        block_size,
        length,
        available_threads);

    barrier_t barrier(num_threads);

    std::promise<Iterator> result;
    std::atomic<bool> done_flag(false);
    std::vector<std::future<void>> futures;
    futures.resize(num_threads - 1);

    Iterator block_start = first;
    for (uint32_t i = 0; i < (num_threads - 1); ++i) {
        Iterator block_end = block_start;
        std::advance(block_end, block_size);

        // barrier handles all synchronisation
        futures[i] = pool.submit(i,
            [&result, &done_flag, block_start, block_end, &predicate, &barrier]() {
                find_element_with_pred<Iterator, UnaryPredicate>(
                    block_start, block_end, predicate,
                    &result, &done_flag, &barrier);
            });
        block_start = block_end;
    }
    find_element_with_pred<Iterator, UnaryPredicate>(block_start, last, predicate, &result, &done_flag, &barrier);

    if (!done_flag.load()) {
        return last; // if nothing found (by any thread), return "end"
    }
    return result.get_future().get();
}

template<typename T>
class threadsafe_list
{
    struct node
    {
        std::mutex m;
        T data;
        std::unique_ptr<node> next;

        node():
            next()
        {}
        
        node(T const& value):
            data(value)
        {}
    };
    
    node head;

public:
    threadsafe_list()
    {}

    ~threadsafe_list()
    {
        remove_if([](T const&){return true;});
    }

    threadsafe_list(threadsafe_list const& other)=delete;
    threadsafe_list& operator=(threadsafe_list const& other)=delete;
    
    void push_front(T /*const&*/ value)
    {
        std::unique_ptr<node> new_node(new node(value));
        std::lock_guard<std::mutex> lk(head.m);
        new_node->next=std::move(head.next);
        head.next=std::move(new_node);
    }

    template<typename Function>
    void for_each(Function f)
    {
        node* current=&head;
        std::unique_lock<std::mutex> lk(head.m);
        while(node* const next=current->next.get())
        {
            std::unique_lock<std::mutex> next_lk(next->m);
            lk.unlock();
            f(next->data);
            current=next;
            lk=std::move(next_lk);
        }
    }

    template<typename Predicate>
    T find_first_if(Predicate p)
    {
        node* current=&head;
        std::unique_lock<std::mutex> lk(head.m);
        while(node* const next=current->next.get())
        {
            std::unique_lock<std::mutex> next_lk(next->m);
            lk.unlock();
            if(p(next->data))
            {
                return next->data;
            }
            current=next;
            lk=std::move(next_lk);
        }
        return T();
    }

    template<typename Predicate>
    void remove_if(Predicate p)
    {
        node* current=&head;
        std::unique_lock<std::mutex> lk(head.m);
        while(node* const next=current->next.get())
        {
            std::unique_lock<std::mutex> next_lk(next->m);
            if(p(next->data))
            {
                std::unique_ptr<node> old_next=std::move(current->next);
                current->next=std::move(next->next);
                next_lk.unlock();
            }
            else
            {
                lk.unlock();
                current=next;
                lk=std::move(next_lk);
            }
        }
    }
};


struct empty_stack: std::exception
{
    const char* what() const throw()
    {
        return "empty stack";
    }
};

template<typename T>
class threadsafe_stack
{
private:
    std::stack<T> data;
    mutable std::mutex m;
public:
    threadsafe_stack(){}
    threadsafe_stack(const threadsafe_stack& other)
    {
        std::lock_guard<std::mutex> lock(other.m);
        data=other.data;
    }
    threadsafe_stack& operator=(const threadsafe_stack&) = delete;

    void push(T new_value)
    {
        std::lock_guard<std::mutex> lock(m);
        data.push(std::move(new_value));
    }
    std::shared_ptr<T> pop()
    {
        std::lock_guard<std::mutex> lock(m);
        if(data.empty()) throw empty_stack();
        std::shared_ptr<T> const res(
            std::make_shared<T>(std::move(data.top())));
        data.pop();
        return res;
    }
    void pop(T& value)
    {
        std::lock_guard<std::mutex> lock(m);
        if(data.empty()) throw empty_stack();
        value=std::move(data.top());
        data.pop();
    }
    bool empty() const
    {
        std::lock_guard<std::mutex> lock(m);
        return data.empty();
    }
};

#endif // MCUT_SCHEDULER_H_