// This file is part of Eigen, a lightweight C++ template library
// for linear algebra.
//
// Copyright (C) 2025 Weiwei Kong <weiweikong@google.com>
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef EIGEN_THREADPOOL_FORKJOIN_H
#define EIGEN_THREADPOOL_FORKJOIN_H

// IWYU pragma: private
#include "./InternalHeaderCheck.h"

namespace Eigen {

// ForkJoinScheduler provides implementations of various non-blocking ParallelFor algorithms for unary
// and binary parallel tasks. More specifically, the implementations follow the binary tree-based
// algorithm from the following paper:
//
//   Lea, D. (2000, June). A java fork/join framework. *In Proceedings of the
//   ACM 2000 conference on Java Grande* (pp. 36-43).
//
// For a given binary task function `f(i,j)` and integers `num_threads`, `granularity`, `start`, and `end`,
// the implemented parallel for algorithm schedules and executes at most `num_threads` of the functions
// from the following set in parallel (either synchronously or asynchronously):
//
//   f(start,start+s_1), f(start+s_1,start+s_2), ..., f(start+s_n,end)
//
// where `s_{j+1} - s_{j}` and `end - s_n` are roughly within a factor of two of `granularity`. For a unary
// task function `g(k)`, the same operation is applied with
//
//   f(i,j) = [&](){ for(Index k = i; k < j; ++k) g(k); };
//
// Note that the parameter `granularity` should be tuned by the user based on the trade-off of running the
// given task function sequentially vs. scheduling individual tasks in parallel. An example of a partially
// tuned `granularity` is in `Eigen::CoreThreadPoolDevice::parallelFor(...)` where the template
// parameter `PacketSize` and float input `cost` are used to indirectly compute a granularity level for a
// given task function.
//
// Example usage #1 (synchronous):
// ```
// ThreadPool thread_pool(num_threads);
// ForkJoinScheduler::ParallelFor(0, num_tasks, granularity, std::move(parallel_task), &thread_pool);
// ```
//
// Example usage #2 (executing multiple tasks asynchronously, each one parallelized with ParallelFor):
// ```
// ThreadPool thread_pool(num_threads);
// Barrier barrier(num_async_calls);
// auto done = [&](){ barrier.Notify(); };
// for (Index k=0; k<num_async_calls; ++k) {
//   ForkJoinScheduler::ParallelForAsync(task_start[k], task_end[k], granularity[k], parallel_task[k], done,
//   &thread_pool);
// }
// barrier.Wait();
// ```
class ForkJoinScheduler {
 public:
  // Runs `do_func` asynchronously for the range [start, end) with a specified
  // granularity. `do_func` should be of type `std::function<void(Index,
  // Index)`. `done()` is called exactly once after all tasks have been executed.
  template <typename DoFnType, typename DoneFnType, typename ThreadPoolEnv>
  static void ParallelForAsync(Index start, Index end, Index granularity, DoFnType&& do_func, DoneFnType&& done,
                               ThreadPoolTempl<ThreadPoolEnv>* thread_pool) {
    if (start >= end) {
      done();
      return;
    }
    thread_pool->Schedule([start, end, granularity, thread_pool, do_func = std::forward<DoFnType>(do_func),
                           done = std::forward<DoneFnType>(done)]() {
      RunParallelFor(start, end, granularity, do_func, thread_pool);
      done();
    });
  }

  // Synchronous variant of ParallelForAsync.
  // WARNING: Making nested calls to `ParallelFor`, e.g., calling `ParallelFor` inside a task passed into another
  // `ParallelFor` call, may lead to deadlocks due to how task stealing is implemented.
  template <typename DoFnType, typename ThreadPoolEnv>
  static void ParallelFor(Index start, Index end, Index granularity, DoFnType&& do_func,
                          ThreadPoolTempl<ThreadPoolEnv>* thread_pool) {
    if (start >= end) return;
    Barrier barrier(1);
    auto done = [&barrier]() { barrier.Notify(); };
    ParallelForAsync(start, end, granularity, do_func, done, thread_pool);
    barrier.Wait();
  }

 private:
  // Schedules `right_thunk`, runs `left_thunk`, and runs other tasks until `right_thunk` has finished.
  template <typename LeftType, typename RightType, typename ThreadPoolEnv>
  static void ForkJoin(LeftType&& left_thunk, RightType&& right_thunk, ThreadPoolTempl<ThreadPoolEnv>* thread_pool) {
    typedef typename ThreadPoolTempl<ThreadPoolEnv>::Task Task;
    std::atomic<bool> right_done(false);
    auto execute_right = [&right_thunk, &right_done]() {
      std::forward<RightType>(right_thunk)();
      right_done.store(true, std::memory_order_release);
    };
    thread_pool->Schedule(execute_right);
    std::forward<LeftType>(left_thunk)();
    Task task;
    while (!right_done.load(std::memory_order_acquire)) {
      thread_pool->MaybeGetTask(&task);
      if (task.f) task.f();
    }
  }

  static Index ComputeMidpoint(Index start, Index end, Index granularity) {
    // Typical workloads choose initial values of `{start, end, granularity}` such that `start - end` and
    // `granularity` are powers of two. Since modern processors usually implement (2^x)-way
    // set-associative caches, we minimize the number of cache misses by choosing midpoints that are not
    // powers of two (to avoid having two addresses in the main memory pointing to the same point in the
    // cache). More specifically, we choose the midpoint at (roughly) the 9/16 mark.
    const Index size = end - start;
    const Index offset = numext::round_down(9 * (size + 1) / 16, granularity);
    return start + offset;
  }

  template <typename DoFnType, typename ThreadPoolEnv>
  static void RunParallelFor(Index start, Index end, Index granularity, DoFnType&& do_func,
                             ThreadPoolTempl<ThreadPoolEnv>* thread_pool) {
    Index mid = ComputeMidpoint(start, end, granularity);
    if ((end - start) < granularity || mid == start || mid == end) {
      do_func(start, end);
      return;
    }
    ForkJoin([start, mid, granularity, &do_func,
              thread_pool]() { RunParallelFor(start, mid, granularity, do_func, thread_pool); },
             [mid, end, granularity, &do_func, thread_pool]() {
               RunParallelFor(mid, end, granularity, do_func, thread_pool);
             },
             thread_pool);
  }
};

}  // namespace Eigen

#endif  // EIGEN_THREADPOOL_FORKJOIN_H
