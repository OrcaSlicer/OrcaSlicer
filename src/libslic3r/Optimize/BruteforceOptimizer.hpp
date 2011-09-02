#ifndef BRUTEFORCEOPTIMIZER_HPP
#define BRUTEFORCEOPTIMIZER_HPP

#include <libslic3r/Optimize/Optimizer.hpp>

namespace Slic3r { namespace opt {

namespace detail {
// Implementing a bruteforce optimizer

// Return the number of iterations needed to reach a specific grid position (idx)
template<size_t N>
long num_iter(const std::array<size_t, N> &idx, size_t gridsz)
{
    long ret = 0;
    for (size_t i = 0; i < N; ++i) ret += idx[i] * std::pow(gridsz, i);
    return ret;
}

// Implementation of a grid search where the search interval is sampled in
// equidistant points for each dimension. Grid size determines the number of
// samples for one dimension so the number of function calls is gridsize ^ dimension.
struct AlgBurteForce {
    bool to_min;
    StopCriteria stc;
    size_t gridsz;

    AlgBurteForce(const StopCriteria &cr, size_t gs): stc{cr}, gridsz{gs} {}

    // This function is called recursively for each dimension and generates
    // the grid values for the particular dimension. If D is less than zero,
    // the object function input values are generated for each dimension and it
    // can be evaluated. The current best score is compared with the newly
    // returned score and changed appropriately.
    template<int D, size_t N, class Fn, class Cmp>
    bool run(std::array<size_t, N> &idx,
             Result<N> &result,
             const Bounds<N> &bounds,
             Fn &&fn,
             Cmp &&cmp)
    {
        if (stc.stop_condition()) return false;

        if constexpr (D < 0) { // Let's evaluate fn
            Input<N> inp;

            auto max_iter = stc.max_iterations();
            if (max_iter && num_iter(idx, gridsz) >= max_iter)
                return false;

            for (size_t d = 0; d < N; ++d) {
                const Bound &b = bounds[d];
                double step = (b.max() - b.min()) / (gridsz - 1);
                inp[d] = b.min() + idx[d] * step;
            }

            auto score = fn(inp);
            if (cmp(score, result.score)) { // Change current score to the new
                double absdiff = std::abs(score - result.score);

                result.score = score;
                result.optimum = inp;

                // Check if the required precision is reached.
                if (absdiff < stc.abs_score_diff() ||
                    absdiff < stc.rel_score_diff() * std::abs(score))
                    return false;
            }

        } else {
            for (size_t i = 0; i < gridsz; ++i) {
                idx[D] = i; // Mark the current grid position and dig down
                if (!run<D - 1>(idx, result, bounds, std::forward<Fn>(fn),
                                std::forward<Cmp>(cmp)))
                    return false;
            }
        }

        return true;
    }

    template<class Fn, size_t N>
    Result<N> optimize(Fn&& fn,
                       const Input<N> &/*initvals*/,
                       const Bounds<N>& bounds)
    {
        std::array<size_t, N> idx = {};
        Result<N> result;

        if (to_min) {
            result.score = std::numeric_limits<double>::max();
            run<int(N) - 1>(idx, result, bounds, std::forward<Fn>(fn),
                            std::less<double>{});
        }
        else {
            result.score = std::numeric_limits<double>::lowest();
            run<int(N) - 1>(idx, result, bounds, std::forward<Fn>(fn),
                            std::greater<double>{});
        }

        return result;
    }
};

} // namespace detail

using AlgBruteForce = detail::AlgBurteForce;

template<>
class Optimizer<AlgBruteForce> {
    AlgBruteForce m_alg;

public:

    Optimizer(const StopCriteria &cr = {}, size_t gridsz = 100)
        : m_alg{cr, gridsz}
    {}

    Optimizer& to_max() { m_alg.to_min = false; return *this; }
    Optimizer& to_min() { m_alg.to_min = true;  return *this; }

    template<class Func, size_t N>
    Result<N> optimize(Func&& func,
                       const Input<N> &initvals,
                       const Bounds<N>& bounds)
    {
        return m_alg.optimize(std::forward<Func>(func), initvals, bounds);
    }

    Optimizer &set_criteria(const StopCriteria &cr)
    {
        m_alg.stc = cr; return *this;
    }

    const StopCriteria &get_criteria() const { return m_alg.stc; }
};

}} // namespace Slic3r::opt

#endif // BRUTEFORCEOPTIMIZER_HPP
