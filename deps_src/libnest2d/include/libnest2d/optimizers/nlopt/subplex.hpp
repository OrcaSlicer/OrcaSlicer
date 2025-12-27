#ifndef SUBPLEX_HPP
#define SUBPLEX_HPP

#include "nlopt_boilerplate.hpp"

namespace libnest2d { namespace opt {

class SubplexOptimizer: public NloptOptimizer {
public:
    inline explicit SubplexOptimizer(const StopCriteria& scr = {}):
        NloptOptimizer(method2nloptAlg(Method::L_SUBPLEX), scr) {}
};

template<>
struct OptimizerSubclass<Method::L_SUBPLEX> { using Type = SubplexOptimizer; };

}
}

#endif // SUBPLEX_HPP
