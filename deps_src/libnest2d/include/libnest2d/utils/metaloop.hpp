#ifndef METALOOP_HPP
#define METALOOP_HPP

#include <libnest2d/common.hpp>
#include <tuple>
#include <functional>

namespace libnest2d {

/* ************************************************************************** */
/* C++14 std::index_sequence implementation:                                  */
/* ************************************************************************** */

/**
 * \brief C++11 compatible implementation of the index_sequence type from C++14
 */
template<size_t...Ints> struct index_sequence {
    using value_type = size_t;
    BP2D_CONSTEXPR value_type size() const { return sizeof...(Ints); }
};

// A Help structure to generate the integer list
template<size_t...Nseq> struct genSeq;

// Recursive template to generate the list
template<size_t I, size_t...Nseq> struct genSeq<I, Nseq...> {
    // Type will contain a genSeq with Nseq appended by one element
    using Type = typename genSeq< I - 1, I - 1, Nseq...>::Type;
};

// Terminating recursion
template <size_t ... Nseq> struct genSeq<0, Nseq...> {
    // If I is zero, Type will contain index_sequence with the fuly generated
    // integer list.
    using Type = index_sequence<Nseq...>;
};

/// Helper alias to make an index sequence from 0 to N
template<size_t N> using make_index_sequence = typename genSeq<N>::Type;

/// Helper alias to make an index sequence for a parameter pack
template<class...Args>
using index_sequence_for = make_index_sequence<sizeof...(Args)>;


/* ************************************************************************** */

namespace opt {

using std::forward;
using std::tuple;
using std::get;
using std::tuple_element;

/**
 * @brief Helper class to be able to loop over a parameter pack's elements.
 */
class metaloop {

// The implementation is based on partial struct template specializations.
// Basically we need a template type that is callable and takes an integer
// non-type template parameter which can be used to implement recursive calls.
//
// C++11 will not allow the usage of a plain template function that is why we
// use struct with overloaded call operator. At the same time C++11 prohibits
// partial template specialization with a non type parameter such as int. We
// need to wrap that in a type (see metaloop::Int).

/*
 * A helper alias to create integer values wrapped as a type. It is necessary
 * because a non type template parameter (such as int) would be prohibited in
 * a partial specialization. Also for the same reason we have to use a class
 * _Metaloop instead of a simple function as a functor. A function cannot be
 * partially specialized in a way that is necessary for this trick.
 */
template<int N> using Int = std::integral_constant<int, N>;

/*
 * Helper class to implement in-place functors.
 *
 * We want to be able to use inline functors like a lambda to keep the code
 * as clear as possible.
 */
template<int N, class Fn> class MapFn {
    Fn&& fn_;
public:

    // It takes the real functor that can be specified in-place but only
    // with C++14 because the second parameter's type will depend on the
    // type of the parameter pack element that is processed. In C++14 we can
    // specify this second parameter type as auto in the lambda parameter list.
    inline MapFn(Fn&& fn): fn_(forward<Fn>(fn)) {}

    template<class T> void operator ()(T&& pack_element) {
        // We provide the index as the first parameter and the pack (or tuple)
        // element as the second parameter to the functor.
        fn_(N, forward<T>(pack_element));
    }
};

/*
 * Implementation of the template loop trick.
 * We create a mechanism for looping over a parameter pack in compile time.
 * \tparam Idx is the loop index which will be decremented at each recursion.
 * \tparam Args The parameter pack that will be processed.
 *
 */
template <typename Idx, class...Args>
class _MetaLoop {};

// Implementation for the first element of Args...
template <class...Args>
class _MetaLoop<Int<0>, Args...> {
public:

    const static BP2D_CONSTEXPR int N = 0;
    const static BP2D_CONSTEXPR int ARGNUM = sizeof...(Args)-1;

    template<class Tup, class Fn>
    void run( Tup&& valtup, Fn&& fn) {
        MapFn<ARGNUM-N, Fn> {forward<Fn>(fn)} (get<ARGNUM-N>(valtup));
    }
};

// Implementation for the N-th element of Args...
template <int N, class...Args>
class _MetaLoop<Int<N>, Args...> {
public:

    const static BP2D_CONSTEXPR int ARGNUM = sizeof...(Args)-1;

    template<class Tup, class Fn>
    void run(Tup&& valtup, Fn&& fn) {
        MapFn<ARGNUM-N, Fn> {forward<Fn>(fn)} (std::get<ARGNUM-N>(valtup));

        // Recursive call to process the next element of Args
        _MetaLoop<Int<N-1>, Args...> ().run(forward<Tup>(valtup),
                                            forward<Fn>(fn));
    }
};

/*
 * Instantiation: We must instantiate the template with the last index because
 * the generalized version calls the decremented instantiations recursively.
 * Once the instantiation with the first index is called, the terminating
 * version of run is called which does not call itself anymore.
 *
 * If you are utterly annoyed, at least you have learned a super crazy
 * functional meta-programming pattern.
 */
template<class...Args>
using MetaLoop = _MetaLoop<Int<sizeof...(Args)-1>, Args...>;

public:

/**
 * \brief The final usable function template.
 *
 * This is similar to what varags was on C but in compile time C++11.
 * You can call:
 * apply(<the mapping function>, <arbitrary number of arguments of any type>);
 * For example:
 *
 *      struct mapfunc {
 *          template<class T> void operator()(int N, T&& element) {
 *              std::cout << "The value of the parameter "<< N <<": "
 *                        << element << std::endl;
 *          }
 *      };
 *
 *      apply(mapfunc(), 'a', 10, 151.545);
 *
 * C++14:
 *      apply([](int N, auto&& element){
 *          std::cout << "The value of the parameter "<< N <<": "
 *                        << element << std::endl;
 *      }, 'a', 10, 151.545);
 *
 * This yields the output:
 * The value of the parameter 0: a
 * The value of the parameter 1: 10
 * The value of the parameter 2: 151.545
 *
 * As an addition, the function can be called with a tuple as the second
 * parameter holding the arguments instead of a parameter pack.
 *
 */
template<class...Args, class Fn>
inline static void apply(Fn&& fn, Args&&...args) {
    MetaLoop<Args...>().run(tuple<Args&&...>(forward<Args>(args)...),
                            forward<Fn>(fn));
}

/// The version of apply with a tuple rvalue reference.
template<class...Args, class Fn>
inline static void apply(Fn&& fn, tuple<Args...>&& tup) {
    MetaLoop<Args...>().run(std::move(tup), forward<Fn>(fn));
}

/// The version of apply with a tuple lvalue reference.
template<class...Args, class Fn>
inline static void apply(Fn&& fn, tuple<Args...>& tup) {
    MetaLoop<Args...>().run(tup, forward<Fn>(fn));
}

/// The version of apply with a tuple const reference.
template<class...Args, class Fn>
inline static void apply(Fn&& fn, const tuple<Args...>& tup) {
    MetaLoop<Args...>().run(tup, forward<Fn>(fn));
}

/**
 * Call a function with its arguments encapsualted in a tuple.
 */
template<class Fn, class Tup, std::size_t...Is>
inline static auto
callFunWithTuple(Fn&& fn, Tup&& tup, index_sequence<Is...>) ->
    decltype(fn(std::get<Is>(tup)...))
{
    return fn(std::get<Is>(tup)...);
}

};
}
}

#endif // METALOOP_HPP
