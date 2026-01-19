#ifndef NLOPT_BOILERPLATE_HPP
#define NLOPT_BOILERPLATE_HPP

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4244)
#pragma warning(disable: 4267)
#endif
#include <nlopt.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <libnest2d/optimizer.hpp>
#include <cassert>
#include <libnest2d/utils/metaloop.hpp>

#include <utility>

namespace libnest2d { namespace opt {

inline nlopt::algorithm method2nloptAlg(Method m) {

    switch(m) {
    case Method::L_SIMPLEX: return nlopt::LN_NELDERMEAD;
    case Method::L_SUBPLEX: return nlopt::LN_SBPLX;
    case Method::G_GENETIC: return nlopt::GN_ESCH;
    default: assert(false); throw(m);
    }
}

/**
 * Optimizer based on NLopt.
 *
 * All the optimized types have to be convertible to double.
 */
class NloptOptimizer: public Optimizer<NloptOptimizer> {
protected:
    nlopt::opt opt_;
    std::vector<double> lower_bounds_;
    std::vector<double> upper_bounds_;
    std::vector<double> initvals_;
    nlopt::algorithm alg_;
    Method localmethod_;

    using Base = Optimizer<NloptOptimizer>;

    friend Base;

    // ********************************************************************** */

    // TODO: CHANGE FOR LAMBDAS WHEN WE WILL MOVE TO C++14

    struct BoundsFunc {
        NloptOptimizer& self;
        inline explicit BoundsFunc(NloptOptimizer& o): self(o) {}

        template<class T> void operator()(int N, T& bounds)
        {
            self.lower_bounds_[N] = bounds.min();
            self.upper_bounds_[N] = bounds.max();
        }
    };

    struct InitValFunc {
        NloptOptimizer& self;
        inline explicit InitValFunc(NloptOptimizer& o): self(o) {}

        template<class T> void operator()(int N, T& initval)
        {
            self.initvals_[N] = initval;
        }
    };

    struct ResultCopyFunc {
        NloptOptimizer& self;
        inline explicit ResultCopyFunc(NloptOptimizer& o): self(o) {}

        template<class T> void operator()(int N, T& resultval)
        {
            resultval = self.initvals_[N];
        }
    };

    struct FunvalCopyFunc {
        using D = const std::vector<double>;
        D& params;
        inline explicit FunvalCopyFunc(D& p): params(p) {}

        template<class T> void operator()(int N, T& resultval)
        {
            resultval = params[N];
        }
    };

    /* ********************************************************************** */

    template<class Fn, class...Args>
    static double optfunc(const std::vector<double>& params,
                          std::vector<double>& /*grad*/,
                          void *data)
    {
        using TData = std::pair<remove_ref_t<Fn>*, NloptOptimizer*>;
        auto typeddata = static_cast<TData*>(data);

        if(typeddata->second->stopcr_.stop_condition())
            typeddata->second->opt_.force_stop();

        auto fnptr = typeddata->first;
        auto funval = std::tuple<Args...>();

        // copy the obtained objectfunction arguments to the funval tuple.
        metaloop::apply(FunvalCopyFunc(params), funval);

        auto ret = metaloop::callFunWithTuple(*fnptr, funval,
                                              index_sequence_for<Args...>());

        return ret;
    }

    template<class Func, class...Args>
    Result<Args...> optimize(Func&& func,
                             std::tuple<Args...> initvals,
                             Bound<Args>... args)
    {
        lower_bounds_.resize(sizeof...(Args));
        upper_bounds_.resize(sizeof...(Args));
        initvals_.resize(sizeof...(Args));

        opt_ = nlopt::opt(alg_, sizeof...(Args) );

        // Copy the bounds which is obtained as a parameter pack in args into
        // lower_bounds_ and upper_bounds_
        metaloop::apply(BoundsFunc(*this), args...);

        opt_.set_lower_bounds(lower_bounds_);
        opt_.set_upper_bounds(upper_bounds_);

        nlopt::opt localopt;
        switch(opt_.get_algorithm()) {
        case nlopt::GN_MLSL:
        case nlopt::GN_MLSL_LDS:
            localopt = nlopt::opt(method2nloptAlg(localmethod_),
                                  sizeof...(Args));
            localopt.set_lower_bounds(lower_bounds_);
            localopt.set_upper_bounds(upper_bounds_);
            opt_.set_local_optimizer(localopt);
        default: ;
        }

        double abs_diff = stopcr_.absolute_score_difference;
        double rel_diff = stopcr_.relative_score_difference;
        double stopval = stopcr_.stop_score;
        if(!std::isnan(abs_diff)) opt_.set_ftol_abs(abs_diff);
        if(!std::isnan(rel_diff)) opt_.set_ftol_rel(rel_diff);
        if(!std::isnan(stopval))  opt_.set_stopval(stopval);

        if(this->stopcr_.max_iterations > 0)
            opt_.set_maxeval(this->stopcr_.max_iterations );

        // Take care of the initial values, copy them to initvals_
        metaloop::apply(InitValFunc(*this), initvals);

        std::pair<remove_ref_t<Func>*, NloptOptimizer*> data =
                std::make_pair(&func, this);

        switch(dir_) {
        case OptDir::MIN:
            opt_.set_min_objective(optfunc<Func, Args...>, &data); break;
        case OptDir::MAX:
            opt_.set_max_objective(optfunc<Func, Args...>, &data); break;
        }

        Result<Args...> result;
        nlopt::result rescode;

        try {
            rescode = opt_.optimize(initvals_, result.score);
            result.resultcode = static_cast<ResultCodes>(rescode);
        } catch( nlopt::forced_stop& ) {
            result.resultcode = ResultCodes::FORCED_STOP;
        }

        metaloop::apply(ResultCopyFunc(*this), result.optimum);

        return result;
    }

public:
    inline explicit NloptOptimizer(nlopt::algorithm alg,
                                   StopCriteria stopcr = {}):
        Base(stopcr), alg_(alg), localmethod_(Method::L_SIMPLEX) {}

};

}
}


#endif // NLOPT_BOILERPLATE_HPP
