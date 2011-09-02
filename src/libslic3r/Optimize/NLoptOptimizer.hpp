#ifndef NLOPTOPTIMIZER_HPP
#define NLOPTOPTIMIZER_HPP

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4244)
#pragma warning(disable: 4267)
#endif
#include <nlopt.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <utility>

#include <libslic3r/Optimize/Optimizer.hpp>

namespace Slic3r { namespace opt {

namespace detail {

// Helper types for NLopt algorithm selection in template contexts
template<nlopt_algorithm alg> struct NLoptAlg {};

// NLopt can combine multiple algorithms if one is global an other is a local
// method. This is how template specializations can be informed about this fact.
template<nlopt_algorithm gl_alg, nlopt_algorithm lc_alg = NLOPT_LN_NELDERMEAD>
struct NLoptAlgComb {};

template<class M> struct IsNLoptAlg {
    static const constexpr bool value = false;
};

template<nlopt_algorithm a> struct IsNLoptAlg<NLoptAlg<a>> {
    static const constexpr bool value = true;
};

template<nlopt_algorithm a1, nlopt_algorithm a2>
struct IsNLoptAlg<NLoptAlgComb<a1, a2>> {
    static const constexpr bool value = true;
};

template<class M, class T = void>
using NLoptOnly = std::enable_if_t<IsNLoptAlg<M>::value, T>;


enum class OptDir { MIN, MAX }; // Where to optimize

struct NLopt { // Helper RAII class for nlopt_opt
    nlopt_opt ptr = nullptr;

    template<class...A> explicit NLopt(A&&...a)
    {
        ptr = nlopt_create(std::forward<A>(a)...);
    }

    NLopt(const NLopt&) = delete;
    NLopt(NLopt&&) = delete;
    NLopt& operator=(const NLopt&) = delete;
    NLopt& operator=(NLopt&&) = delete;

    ~NLopt() { nlopt_destroy(ptr); }
};

template<class Method> class NLoptOpt {};

// Optimizers based on NLopt.
template<nlopt_algorithm alg> class NLoptOpt<NLoptAlg<alg>> {
protected:
    StopCriteria m_stopcr;
    OptDir m_dir;

    template<class Fn> using TOptData =
        std::tuple<std::remove_reference_t<Fn>*, NLoptOpt*, nlopt_opt>;

    template<class Fn, size_t N>
    static double optfunc(unsigned n, const double *params,
                          double *gradient,
                          void *data)
    {
        assert(n >= N);

        auto tdata = static_cast<TOptData<Fn>*>(data);

        if (std::get<1>(*tdata)->m_stopcr.stop_condition())
            nlopt_force_stop(std::get<2>(*tdata));

        auto fnptr  = std::get<0>(*tdata);
        auto funval = to_arr<N>(params);

        double scoreval = 0.;
        using RetT = decltype((*fnptr)(funval));
        if constexpr (std::is_convertible_v<RetT, ScoreGradient<N>>) {
            ScoreGradient<N> score = (*fnptr)(funval);
            for (size_t i = 0; i < n; ++i) gradient[i] = (*score.gradient)[i];
            scoreval = score.score;
        } else {
            scoreval = (*fnptr)(funval);
        }

        return scoreval;
    }

    template<size_t N>
    void set_up(NLopt &nl, const Bounds<N>& bounds)
    {
        std::array<double, N> lb, ub;

        for (size_t i = 0; i < N; ++i) {
            lb[i] = bounds[i].min();
            ub[i] = bounds[i].max();
        }

        nlopt_set_lower_bounds(nl.ptr, lb.data());
        nlopt_set_upper_bounds(nl.ptr, ub.data());

        double abs_diff = m_stopcr.abs_score_diff();
        double rel_diff = m_stopcr.rel_score_diff();
        double stopval = m_stopcr.stop_score();
        if(!std::isnan(abs_diff)) nlopt_set_ftol_abs(nl.ptr, abs_diff);
        if(!std::isnan(rel_diff)) nlopt_set_ftol_rel(nl.ptr, rel_diff);
        if(!std::isnan(stopval))  nlopt_set_stopval(nl.ptr, stopval);

        if(m_stopcr.max_iterations() > 0)
            nlopt_set_maxeval(nl.ptr, m_stopcr.max_iterations());
    }

    template<class Fn, size_t N>
    Result<N> optimize(NLopt &nl, Fn &&fn, const Input<N> &initvals)
    {
        Result<N> r;

        TOptData<Fn> data = std::make_tuple(&fn, this, nl.ptr);

        switch(m_dir) {
        case OptDir::MIN:
            nlopt_set_min_objective(nl.ptr, optfunc<Fn, N>, &data); break;
        case OptDir::MAX:
            nlopt_set_max_objective(nl.ptr, optfunc<Fn, N>, &data); break;
        }

        r.optimum = initvals;
        r.resultcode = nlopt_optimize(nl.ptr, r.optimum.data(), &r.score);

        return r;
    }

public:

    template<class Func, size_t N>
    Result<N> optimize(Func&& func,
                       const Input<N> &initvals,
                       const Bounds<N>& bounds)
    {
        NLopt nl{alg, N};
        set_up(nl, bounds);

        return optimize(nl, std::forward<Func>(func), initvals);
    }

    explicit NLoptOpt(StopCriteria stopcr = {}) : m_stopcr(stopcr) {}

    void set_criteria(const StopCriteria &cr) { m_stopcr = cr; }
    const StopCriteria &get_criteria() const noexcept { return m_stopcr; }
    void set_dir(OptDir dir) noexcept { m_dir = dir; }

    void seed(long s) { nlopt_srand(s); }
};

template<nlopt_algorithm glob, nlopt_algorithm loc>
class NLoptOpt<NLoptAlgComb<glob, loc>>: public NLoptOpt<NLoptAlg<glob>>
{
    using Base = NLoptOpt<NLoptAlg<glob>>;
public:

    template<class Fn, size_t N>
    Result<N> optimize(Fn&& f,
                       const Input<N> &initvals,
                       const Bounds<N>& bounds)
    {
        NLopt nl_glob{glob, N}, nl_loc{loc, N};

        Base::set_up(nl_glob, bounds);
        Base::set_up(nl_loc, bounds);
        nlopt_set_local_optimizer(nl_glob.ptr, nl_loc.ptr);

        return Base::optimize(nl_glob, std::forward<Fn>(f), initvals);
    }

    explicit NLoptOpt(StopCriteria stopcr = {}) : Base{stopcr} {}
};

} // namespace detail;

// Optimizers based on NLopt.
template<class M> class Optimizer<M, detail::NLoptOnly<M>> {
    detail::NLoptOpt<M> m_opt;

public:

    Optimizer& to_max() { m_opt.set_dir(detail::OptDir::MAX); return *this; }
    Optimizer& to_min() { m_opt.set_dir(detail::OptDir::MIN); return *this; }

    template<class Func, size_t N>
    Result<N> optimize(Func&& func,
                       const Input<N> &initvals,
                       const Bounds<N>& bounds)
    {
        return m_opt.optimize(std::forward<Func>(func), initvals, bounds);
    }

    explicit Optimizer(StopCriteria stopcr = {}) : m_opt(stopcr) {}

    Optimizer &set_criteria(const StopCriteria &cr)
    {
        m_opt.set_criteria(cr); return *this;
    }

    const StopCriteria &get_criteria() const { return m_opt.get_criteria(); }

    void seed(long s) { m_opt.seed(s); }
};

// Predefinded NLopt algorithms
using AlgNLoptGenetic = detail::NLoptAlgComb<NLOPT_GN_ESCH>;
using AlgNLoptSubplex = detail::NLoptAlg<NLOPT_LN_SBPLX>;
using AlgNLoptSimplex = detail::NLoptAlg<NLOPT_LN_NELDERMEAD>;
using AlgNLoptDIRECT  = detail::NLoptAlg<NLOPT_GN_DIRECT>;
using AlgNLoptMLSL    = detail::NLoptAlg<NLOPT_GN_MLSL>;

}} // namespace Slic3r::opt

#endif // NLOPTOPTIMIZER_HPP
