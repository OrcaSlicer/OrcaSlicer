#ifndef GENETIC_HPP
#define GENETIC_HPP

#include "nlopt_boilerplate.hpp"

namespace libnest2d { namespace opt {

class GeneticOptimizer: public NloptOptimizer {
public:
    inline explicit GeneticOptimizer(const StopCriteria& scr = {}):
        NloptOptimizer(method2nloptAlg(Method::G_GENETIC), scr) {}

    inline GeneticOptimizer& localMethod(Method m) {
        localmethod_ = m;
        return *this;
    }

    inline void seed(unsigned long val) { nlopt::srand(val); }
};

template<>
struct OptimizerSubclass<Method::G_GENETIC> { using Type = GeneticOptimizer; };

template<>
inline TOptimizer<Method::G_GENETIC> GlobalOptimizer<Method::G_GENETIC>(
        Method localm, const StopCriteria& scr )
{
    return GeneticOptimizer (scr).localMethod(localm);
}

}
}

#endif // GENETIC_HPP
