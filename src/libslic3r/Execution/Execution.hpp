#ifndef EXECUTION_HPP
#define EXECUTION_HPP

#include <type_traits>
#include <utility>
#include <cstddef>
#include <iterator>

#include "libslic3r/libslic3r.h"

namespace Slic3r {

// Override for valid execution policies
template<class EP> struct IsExecutionPolicy_ : public std::false_type {};

template<class EP> constexpr bool IsExecutionPolicy =
    IsExecutionPolicy_<remove_cvref_t<EP>>::value;

template<class EP, class T = void>
using ExecutionPolicyOnly = std::enable_if_t<IsExecutionPolicy<EP>, T>;

namespace execution {

// This struct needs to be specialized for each execution policy.
// See ExecutionSeq.hpp and ExecutionTBB.hpp for example.
template<class EP, class En = void> struct Traits {};

template<class EP> using AsTraits = Traits<remove_cvref_t<EP>>;

// Each execution policy should declare two types of mutexes. A a spin lock and
// a blocking mutex. These types should satisfy the BasicLockable concept.
template<class EP> using SpinningMutex = typename Traits<EP>::SpinningMutex;
template<class EP> using BlockingMutex = typename Traits<EP>::BlockingMutex;

// Query the available threads for concurrency.
template<class EP, class = ExecutionPolicyOnly<EP> >
size_t max_concurrency(const EP &ep)
{
    return AsTraits<EP>::max_concurrency(ep);
}

// foreach loop with the execution policy passed as argument. Granularity can
// be specified explicitly. max_concurrency() can be used for optimal results.
template<class EP, class It, class Fn, class = ExecutionPolicyOnly<EP>>
void for_each(const EP &ep, It from, It to, Fn &&fn, size_t granularity = 1)
{
    AsTraits<EP>::for_each(ep, from, to, std::forward<Fn>(fn), granularity);
}

// A reduce operation with the execution policy passed as argument.
// mergefn has T(const T&, const T&) signature
// accessfn has T(I) signature if I is an integral type and
// T(const I::value_type &) if I is an iterator type.
template<class EP,
         class I,
         class MergeFn,
         class T,
         class AccessFn,
         class = ExecutionPolicyOnly<EP> >
T reduce(const EP & ep,
         I          from,
         I          to,
         const T &  init,
         MergeFn && mergefn,
         AccessFn &&accessfn,
         size_t     granularity = 1)
{
    return AsTraits<EP>::reduce(ep, from, to, init,
                                std::forward<MergeFn>(mergefn),
                                std::forward<AccessFn>(accessfn),
                                granularity);
}

// An overload of reduce method to be used with iterators as 'from' and 'to'
// arguments. Access functor is omitted here.
template<class EP,
         class I,
         class MergeFn,
         class T,
         class = ExecutionPolicyOnly<EP> >
T reduce(const EP &ep,
         I         from,
         I         to,
         const T & init,
         MergeFn &&mergefn,
         size_t    granularity = 1)
{
    return reduce(
        ep, from, to, init, std::forward<MergeFn>(mergefn),
        [](const auto &i) { return i; }, granularity);
}

template<class EP,
         class I,
         class T,
         class AccessFn,
         class = ExecutionPolicyOnly<EP>>
T accumulate(const EP & ep,
             I          from,
             I          to,
             const T &  init,
             AccessFn &&accessfn,
             size_t     granularity = 1)
{
    return reduce(ep, from, to, init, std::plus<T>{},
                  std::forward<AccessFn>(accessfn), granularity);
}


template<class EP,
         class I,
         class T,
         class = ExecutionPolicyOnly<EP> >
T accumulate(const EP &ep,
             I         from,
             I         to,
             const T & init,
             size_t    granularity = 1)
{
    return reduce(
        ep, from, to, init, std::plus<T>{}, [](const auto &i) { return i; },
        granularity);
}

} // namespace execution_policy
} // namespace Slic3r

#endif // EXECUTION_HPP
