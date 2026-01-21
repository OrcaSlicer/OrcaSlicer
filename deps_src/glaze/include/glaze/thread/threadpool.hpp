// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace glz
{
   // A thread pool
   struct pool final
   {
      pool() : pool(concurrency()) {}

      pool(const size_t n) { n_threads(n); }

      void n_threads(const size_t n)
      {
         finish_work(); // finish any active work
         std::lock_guard lock(mtx);
         closed = false;

         threads.clear();
         threads.reserve(n);
         for (size_t i = 0; i < n; ++i) {
            threads.emplace_back([this, thread_number = i] {
               while (true) {
                  std::unique_lock lock(mtx);
                  work_cv.wait(lock, [this] { return closed || !queue.empty(); }); // Wait for work
                  if (queue.empty()) {
                     if (closed) {
                        return;
                     }
                  }
                  else {
                     // Grab work
                     ++working;
                     auto work = queue.front();
                     queue.pop_front();
                     lock.unlock();

                     (*work)(thread_number);

                     // Notify that work is finished
                     --working;
                     lock.lock();
                     if (queue.empty() && (working == 0)) {
                        done_cv.notify_one();
                     }
                  }
               }
            });
         }
      }

      size_t concurrency() const noexcept { return std::thread::hardware_concurrency(); }

      using callable_t = std::function<void(const size_t)>;

      template <class F>
         requires(not std::invocable<F, size_t>)
      auto emplace_back(F&& func)
      {
         using result_t = std::invoke_result_t<F>;

         std::lock_guard lock(mtx);

         auto promise = std::make_shared<std::promise<result_t>>();

         queue.emplace_back() =
            std::make_shared<callable_t>([promise, f = std::forward<F>(func)](const size_t /*thread_number*/) {
#if __cpp_exceptions
               try {
                  if constexpr (std::is_void_v<result_t>) {
                     f();
                     promise->set_value();
                  }
                  else {
                     promise->set_value(f());
                  }
               }
               catch (...) {
                  promise->set_exception(std::current_exception());
               }
#else
               if constexpr (std::is_void_v<result_t>) {
                  f();
                  promise->set_value();
               }
               else {
                  promise->set_value(f());
               }
#endif
            });

         work_cv.notify_one();

         return promise->get_future();
      }

      // Takes a function whose input is the thread number (size_t)
      template <class F>
         requires std::invocable<F, size_t>
      auto emplace_back(F&& func)
      {
         using result_t = std::invoke_result_t<F, size_t>;

         std::lock_guard lock(mtx);

         auto promise = std::make_shared<std::promise<result_t>>();

         queue.emplace_back() =
            std::make_shared<callable_t>([promise, f = std::forward<F>(func)](const size_t thread_number) {
#if __cpp_exceptions
               try {
                  if constexpr (std::is_void_v<result_t>) {
                     f(thread_number);
                     promise->set_value();
                  }
                  else {
                     promise->set_value(f(thread_number));
                  }
               }
               catch (...) {
                  promise->set_exception(std::current_exception());
               }
#else
               if constexpr (std::is_void_v<result_t>) {
                  f(thread_number);
                  promise->set_value();
               }
               else {
                  promise->set_value(f(thread_number));
               }
#endif
            });

         work_cv.notify_one();

         return promise->get_future();
      }

      bool computing() const noexcept { return (working != 0); }

      uint32_t number_working() const noexcept { return working; }

      void wait()
      {
         std::unique_lock lock(mtx);
         if (queue.empty() && (working == 0)) return;
         done_cv.wait(lock);
      }

      size_t size() const { return threads.size(); }

      ~pool() { finish_work(); }

     private:
      std::vector<std::thread> threads{};
      std::deque<std::shared_ptr<callable_t>> queue{};
      std::atomic<uint32_t> working = 0;
      std::atomic<bool> closed = false;
      std::mutex mtx{};
      std::condition_variable work_cv{};
      std::condition_variable done_cv{};

      void finish_work()
      {
         // Close the queue and finish all the remaining work
         {
            std::lock_guard lock(mtx);
            closed = true;
            work_cv.notify_all();
         }

         wait(); // wait for work to finish

         for (auto& t : threads) {
            if (t.joinable()) t.join();
         }
      }
   };
}
