#ifndef EXECUTIONSEQ_HPP
#define EXECUTIONSEQ_HPP

#ifdef PRUSASLICER_USE_EXECUTION_STD // Conflicts with our version of TBB
#include <execution>
#endif

#include "Execution.hpp"

namespace Slic3r {

// Execution policy implementing dummy sequential algorithms
struct ExecutionSeq {};

template<> struct IsExecutionPolicy_<ExecutionSeq> : public std::true_type {};

static constexpr ExecutionSeq ex_seq = {};

template<class EP> struct IsSequentialEP_ { static constexpr bool value = false; };

template<> struct IsSequentialEP_<ExecutionSeq>: public std::true_type {};
#ifdef PRUSASLICER_USE_EXECUTION_STD
template<> struct IsExecutionPolicy_<std::execution::sequenced_policy>: public std::true_type {};
template<> struct IsSequentialEP_<std::execution::sequenced_policy>: public std::true_type {};
#endif

template<class EP>
constexpr bool IsSequentialEP = IsSequentialEP_<remove_cvref_t<EP>>::value;

template<class EP, class R = EP>
using SequentialEPOnly = std::enable_if_t<IsSequentialEP<EP>, R>;

template<class EP>
struct execution::Traits<EP, SequentialEPOnly<EP, void>> {
private:
    struct _Mtx { inline void lock() {} inline void unlock() {} };

    template<class Fn, class It>
    static IteratorOnly<It, void> loop_(It from, It to, Fn &&fn)
    {
        for (auto it = from; it != to; ++it) fn(*it);
    }

    template<class Fn, class I>
    static IntegerOnly<I, void> loop_(I from, I to, Fn &&fn)
    {
        for (I i = from; i < to; ++i) fn(i);
    }

public:
    using SpinningMutex = _Mtx;
    using BlockingMutex = _Mtx;

    template<class It, class Fn>
    static void for_each(const EP &,
                         It   from,
                         It   to,
                         Fn &&fn,
                         size_t /* ignore granularity */ = 1)
    {
        loop_(from, to, std::forward<Fn>(fn));
    }

    template<class I, class MergeFn, class T, class AccessFn>
    static T reduce(const EP &,
                    I         from,
                    I         to,
                    const T & init,
                    MergeFn  &&mergefn,
                    AccessFn &&access,
                    size_t   /*granularity*/ = 1
                    )
    {
        T acc = init;
        loop_(from, to, [&](auto &i) { acc = mergefn(acc, access(i)); });
        return acc;
    }

    static size_t max_concurrency(const EP &) { return 1; }
};

} // namespace Slic3r

#endif // EXECUTIONSEQ_HPP
