#ifndef LIBNEST2D_PARALLEL_HPP
#define LIBNEST2D_PARALLEL_HPP

#include <iterator>
#include <functional>
#include <future>

#ifdef LIBNEST2D_THREADING_tbb
#include <tbb/parallel_for.h>
#endif

#ifdef LIBNEST2D_THREADING_omp
#include <omp.h>
#endif

namespace libnest2d { namespace __parallel {
    
template<class It> 
using TIteratorValue = typename std::iterator_traits<It>::value_type;

template<class Iterator>
inline void enumerate(
        Iterator from, Iterator to,
        std::function<void(TIteratorValue<Iterator>, size_t)> fn,
        std::launch policy = std::launch::deferred | std::launch::async)
{
    using TN = size_t;
    auto iN = to-from;
    TN N = iN < 0? 0 : TN(iN);

#ifdef LIBNEST2D_THREADING_tbb
    if((policy & std::launch::async) == std::launch::async) {
        tbb::parallel_for<TN>(0, N, [from, fn] (TN n) { fn(*(from + n), n); } );
    } else {
        for(TN n = 0; n < N; n++) fn(*(from + n), n);
    }
#endif

#ifdef LIBNEST2D_THREADING_omp
    if((policy & std::launch::async) == std::launch::async) {
        #pragma omp parallel for
        for(int n = 0; n < int(N); n++) fn(*(from + n), TN(n));
    }
    else {
        for(TN n = 0; n < N; n++) fn(*(from + n), n);
    }
#endif

#ifdef LIBNEST2D_THREADING_std
    std::vector<std::future<void>> rets(N);

    auto it = from;
    for(TN b = 0; b < N; b++) {
        rets[b] = std::async(policy, fn, *it++, unsigned(b));
    }

    for(TN fi = 0; fi < N; ++fi) rets[fi].wait();
#endif
}

}}

#endif //LIBNEST2D_PARALLEL_HPP
