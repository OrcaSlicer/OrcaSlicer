#ifndef OPTIMIZER_HPP
#define OPTIMIZER_HPP

#include <tuple>
#include <functional>
#include <limits>

#include <libnest2d/common.hpp>

namespace libnest2d { namespace opt {

using std::forward;
using std::tuple;
using std::make_tuple;

/// A Type trait for upper and lower limit of a numeric type.
template<class T, class B = void >
struct limits {
    inline static T min() { return std::numeric_limits<T>::min(); }
    inline static T max() { return std::numeric_limits<T>::max(); }
};

template<class T>
struct limits<T, enable_if_t<std::numeric_limits<T>::has_infinity, void>> {
    inline static T min() { return -std::numeric_limits<T>::infinity(); }
    inline static T max() { return std::numeric_limits<T>::infinity(); }
};

/// An interval of possible input values for optimization
template<class T>
class Bound {
    T min_;
    T max_;
public:
    Bound(const T& min = limits<T>::min(),
          const T& max = limits<T>::max()): min_(min), max_(max) {}
    inline const T min() const BP2D_NOEXCEPT { return min_; }
    inline const T max() const BP2D_NOEXCEPT { return max_; }
};

/**
 * Helper function to make a Bound object with its type deduced automatically.
 */
template<class T>
inline Bound<T> bound(const T& min, const T& max) { return Bound<T>(min, max); }

/**
 * This is the type of an input tuple for the object function. It holds the
 * values and their type in each dimension.
 */
template<class...Args> using Input = tuple<Args...>;

template<class...Args>
inline tuple<Args...> initvals(Args...args) { return make_tuple(args...); }

/**
 * @brief Specific optimization methods for which a default optimizer
 * implementation can be instantiated.
 */
enum class Method {
    L_SIMPLEX,
    L_SUBPLEX,
    G_GENETIC,
    G_PARTICLE_SWARM
    //...
};

/**
 * @brief Info about result of an optimization. These codes are exactly the same
 * as the nlopt codes for convinience.
 */
enum ResultCodes {
    FAILURE = -1, /* generic failure code */
    INVALID_ARGS = -2,
    OUT_OF_MEMORY = -3,
    ROUNDOFF_LIMITED = -4,
    FORCED_STOP = -5,
    SUCCESS = 1, /* generic success code */
    STOPVAL_REACHED = 2,
    FTOL_REACHED = 3,
    XTOL_REACHED = 4,
    MAXEVAL_REACHED = 5,
    MAXTIME_REACHED = 6
};

/**
 * \brief A type to hold the complete result of the optimization.
 */
template<class...Args>
struct Result {
    ResultCodes resultcode;
    tuple<Args...> optimum;
    double score;
};

/**
 * @brief A type for specifying the stop criteria.
 */
struct StopCriteria {

    /// If the absolute value difference between two scores.
    double absolute_score_difference = std::nan("");

    /// If the relative value difference between two scores.
    double relative_score_difference = std::nan("");

    /// Stop if this value or better is found.
    double stop_score = std::nan("");

    /// A predicate that if evaluates to true, the optimization should terminate
    /// and the best result found prior to termination should be returned.
    std::function<bool()> stop_condition = [] { return false; };

    /// The max allowed number of iterations.
    unsigned max_iterations = 0;
};

/**
 * \brief The Optimizer base class with CRTP pattern.
 */
template<class Subclass>
class Optimizer {
protected:
    enum class OptDir{
        MIN,
        MAX
    } dir_;

    StopCriteria stopcr_;

public:

    inline explicit Optimizer(const StopCriteria& scr = {}): stopcr_(scr) {}

    /**
     * \brief Optimize for minimum value of the provided objectfunction.
     * \param objectfunction The function that will be searched for the minimum
     * return value.
     * \param initvals A tuple with the initial values for the search
     * \param bounds A parameter pack with the bounds for each dimension.
     * \return Returns a Result<Args...> structure.
     * An example call would be:
     *     auto result = opt.optimize_min(
     *           [](tuple<double> x) // object function
     *           {
     *               return std::pow(std::get<0>(x), 2);
     *           },
     *           make_tuple(-0.5),  // initial value
     *           {-1.0, 1.0}           // search space bounds
     *       );
     */
    template<class Func, class...Args>
    inline Result<Args...> optimize_min(Func&& objectfunction,
                                        Input<Args...> initvals,
                                        Bound<Args>... bounds)
    {
        dir_ = OptDir::MIN;
        return static_cast<Subclass*>(this)->template optimize<Func, Args...>(
                    forward<Func>(objectfunction), initvals, bounds... );
    }

    template<class Func, class...Args>
    inline Result<Args...> optimize_min(Func&& objectfunction,
                                        Input<Args...> initvals)
    {
        dir_ = OptDir::MIN;
        return static_cast<Subclass*>(this)->template optimize<Func, Args...>(
                    forward<Func>(objectfunction), initvals, Bound<Args>()... );
    }

    template<class...Args, class Func>
    inline Result<Args...> optimize_min(Func&& objectfunction)
    {
        dir_ = OptDir::MIN;
        return static_cast<Subclass*>(this)->template optimize<Func, Args...>(
                    forward<Func>(objectfunction),
                    Input<Args...>(),
                    Bound<Args>()... );
    }

    /// Same as optimize_min but optimizes for maximum function value.
    template<class Func, class...Args>
    inline Result<Args...> optimize_max(Func&& objectfunction,
                                        Input<Args...> initvals,
                                        Bound<Args>... bounds)
    {
        dir_ = OptDir::MAX;
        return static_cast<Subclass*>(this)->template optimize<Func, Args...>(
                    forward<Func>(objectfunction), initvals, bounds... );
    }

    template<class Func, class...Args>
    inline Result<Args...> optimize_max(Func&& objectfunction,
                                        Input<Args...> initvals)
    {
        dir_ = OptDir::MAX;
        return static_cast<Subclass*>(this)->template optimize<Func, Args...>(
                    forward<Func>(objectfunction), initvals, Bound<Args>()... );
    }

    template<class...Args, class Func>
    inline Result<Args...> optimize_max(Func&& objectfunction)
    {
        dir_ = OptDir::MAX;
        return static_cast<Subclass*>(this)->template optimize<Func, Args...>(
                    forward<Func>(objectfunction),
                    Input<Args...>(),
                    Bound<Args>()... );
    }

};

// Just to be able to instantiate an unimplemented method and generate compile
// error.
template<class T = void>
class DummyOptimizer : public Optimizer<DummyOptimizer<T>> {
    friend class Optimizer<DummyOptimizer<T>>;

public:
    DummyOptimizer() {
        static_assert(always_false<T>::value, "Optimizer unimplemented!");
    }

    DummyOptimizer(const StopCriteria&) {
        static_assert(always_false<T>::value, "Optimizer unimplemented!");
    }

    template<class Func, class...Args>
    Result<Args...> optimize(Func&& /*func*/,
                             tuple<Args...> /*initvals*/,
                             Bound<Args>...  /*args*/)
    {
        return Result<Args...>();
    }
};

// Specializing this struct will tell what kind of optimizer to generate for
// a given method
template<Method m> struct OptimizerSubclass { using Type = DummyOptimizer<>; };

/// Optimizer type based on the method provided in parameter m.
template<Method m> using TOptimizer = typename OptimizerSubclass<m>::Type;

/// Global optimizer with an explicitly specified local method.
template<Method m>
inline TOptimizer<m> GlobalOptimizer(Method, const StopCriteria& scr = {})
{ // Need to be specialized in order to do anything useful.
    return TOptimizer<m>(scr);
}

}
}

#endif // OPTIMIZER_HPP
