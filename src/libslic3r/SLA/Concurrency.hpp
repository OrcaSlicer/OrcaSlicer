#ifndef SLA_CONCURRENCY_H
#define SLA_CONCURRENCY_H

// FIXME: Deprecated

#include <libslic3r/Execution/ExecutionSeq.hpp>
#include <libslic3r/Execution/ExecutionTBB.hpp>

namespace Slic3r {
namespace sla {

// Set this to true to enable full parallelism in this module.
// Only the well tested parts will be concurrent if this is set to false.
const constexpr bool USE_FULL_CONCURRENCY = true;

template<bool> struct _ccr {};

template<> struct _ccr<true>
{
    using SpinningMutex = execution::SpinningMutex<ExecutionTBB>;
    using BlockingMutex = execution::BlockingMutex<ExecutionTBB>;

    template<class It, class Fn>
    static void for_each(It from, It to, Fn &&fn, size_t granularity = 1)
    {
        execution::for_each(ex_tbb, from, to, std::forward<Fn>(fn), granularity);
    }

    template<class...Args>
    static auto reduce(Args&&...args)
    {
        return execution::reduce(ex_tbb, std::forward<Args>(args)...);
    }

    static size_t max_concurreny()
    {
        return execution::max_concurrency(ex_tbb);
    }
};

template<> struct _ccr<false>
{
    using SpinningMutex = execution::SpinningMutex<ExecutionSeq>;
    using BlockingMutex = execution::BlockingMutex<ExecutionSeq>;

    template<class It, class Fn>
    static void for_each(It from, It to, Fn &&fn, size_t granularity = 1)
    {
        execution::for_each(ex_seq, from, to, std::forward<Fn>(fn), granularity);
    }

    template<class...Args>
    static auto reduce(Args&&...args)
    {
        return execution::reduce(ex_seq, std::forward<Args>(args)...);
    }

    static size_t max_concurreny()
    {
        return execution::max_concurrency(ex_seq);
    }
};

using ccr = _ccr<USE_FULL_CONCURRENCY>;
using ccr_seq = _ccr<false>;
using ccr_par = _ccr<true>;

}} // namespace Slic3r::sla

#endif // SLACONCURRENCY_H
