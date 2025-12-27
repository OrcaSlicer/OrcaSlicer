#ifndef SIMPLEX_HPP
#define SIMPLEX_HPP

#include "nlopt_boilerplate.hpp"

namespace libnest2d { namespace opt {

class SimplexOptimizer: public NloptOptimizer {
public:
    inline explicit SimplexOptimizer(const StopCriteria& scr = {}):
        NloptOptimizer(method2nloptAlg(Method::L_SIMPLEX), scr) {}
};

template<>
struct OptimizerSubclass<Method::L_SIMPLEX> { using Type = SimplexOptimizer; };

}
}

#endif // SIMPLEX_HPP
