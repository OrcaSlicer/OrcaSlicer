#ifndef LIBNEST2D_CONFIG_HPP
#define LIBNEST2D_CONFIG_HPP

#ifndef NDEBUG
#include <iostream>
#endif

#include <stdexcept>
#include <string>
#include <cmath>
#include <type_traits>
#include <limits>

#define MAX_NUM_PLATES 36

#if defined(_MSC_VER) &&  _MSC_VER <= 1800 || __cplusplus < 201103L
    #define BP2D_NOEXCEPT
    #define BP2D_CONSTEXPR
    #define BP2D_COMPILER_MSVC12
#elif __cplusplus >= 201103L
    #define BP2D_NOEXCEPT noexcept
    #define BP2D_CONSTEXPR constexpr
#endif

/*
 * Debugging output dout and derr definition
 */
//#ifndef NDEBUG
//#   define dout std::cout
//#   define derr std::cerr
//#else
//#   define dout 0 && std::cout
//#   define derr 0 && std::cerr
//#endif

namespace libnest2d {

struct DOut {
#ifndef NDEBUG
    std::ostream& out = std::cout;
#endif
};

struct DErr {
#ifndef NDEBUG
    std::ostream& out = std::cerr;
#endif
};

template<class T>
inline DOut&& operator<<( DOut&& out, T&& d) {
#ifndef NDEBUG
    out.out << d;
#endif
    return std::move(out);
}

template<class T>
inline DErr&& operator<<( DErr&& out, T&& d) {
#ifndef NDEBUG
    out.out << d;
#endif
    return std::move(out);
}
inline DOut dout() { return DOut(); }
inline DErr derr() { return DErr(); }

template< class T >
struct remove_cvref {
    using type = typename std::remove_cv<
        typename std::remove_reference<T>::type>::type;
};

template< class T >
using remove_cvref_t = typename remove_cvref<T>::type;

template< class T >
using remove_ref_t = typename std::remove_reference<T>::type;

template<bool B, class T>
using enable_if_t = typename std::enable_if<B, T>::type;

template<class F, class...Args>
struct invoke_result {
    using type = typename std::result_of<F(Args...)>::type;
};

template<class F, class...Args>
using invoke_result_t = typename invoke_result<F, Args...>::type;

/**
 * A useful little tool for triggering static_assert error messages e.g. when
 * a mandatory template specialization (implementation) is missing.
 *
 * \tparam T A template argument that may come from and outer template method.
 */
template<class T> struct always_false { enum { value = false }; };

const double BP2D_CONSTEXPR Pi = 3.141592653589793238463; // 2*std::acos(0);
const double BP2D_CONSTEXPR Pi_2 = 2*Pi;

/**
 * @brief Only for the Radian and Degrees classes to behave as doubles.
 */
class Double {
protected:
  double val_;
public:
  Double(): val_(double{}) { }
  Double(double d) : val_(d) { }

  operator double() const BP2D_NOEXCEPT { return val_; }
  operator double&() BP2D_NOEXCEPT { return val_; }
};

class Degrees;

/**
 * @brief Data type representing radians. It supports conversion to degrees.
 */
class Radians: public Double {
    mutable double sin_ = std::nan(""), cos_ = std::nan("");
public:
    Radians(double rads = Double() ): Double(rads) {}
    inline Radians(const Degrees& degs);

    inline operator Degrees();
    inline double toDegrees();

    inline double sin() const {
        if(std::isnan(sin_)) {
            cos_ = std::cos(val_);
            sin_ = std::sin(val_);
        }
        return sin_;
    }

    inline double cos() const {
        if(std::isnan(cos_)) {
            cos_ = std::cos(val_);
            sin_ = std::sin(val_);
        }
        return cos_;
    }
};

/**
 * @brief Data type representing degrees. It supports conversion to radians.
 */
class Degrees: public Double {
public:
    Degrees(double deg = Double()): Double(deg) {}
    Degrees(const Radians& rads): Double( rads * 180/Pi ) {}
    inline double toRadians() { return Radians(*this);}
};

inline bool operator==(const Degrees& deg, const Radians& rads) {
    Degrees deg2 = rads;
    auto diff = std::abs(deg - deg2);
    return diff < 0.0001;
}

inline bool operator==(const Radians& rads, const Degrees& deg) {
    return deg == rads;
}

inline Radians::operator Degrees() { return *this * 180/Pi; }

inline Radians::Radians(const Degrees &degs): Double( degs * Pi/180) {}

inline double Radians::toDegrees() { return operator Degrees(); }

enum class GeomErr : std::size_t {
    OFFSET,
    MERGE,
    NFP
};

const std::string ERROR_STR[] = {
    "Offsetting could not be done! An invalid geometry may have been added.",
    "Error while merging geometries!",
    "No fit polygon cannot be calculated."
};

class GeometryException: public std::exception {

    virtual const std::string& errorstr(GeomErr errcode) const BP2D_NOEXCEPT {
        return ERROR_STR[static_cast<std::size_t>(errcode)];
    }

    GeomErr errcode_;
public:

    GeometryException(GeomErr code): errcode_(code) {}

    GeomErr errcode() const { return errcode_; }

    const char * what() const BP2D_NOEXCEPT override {
        return errorstr(errcode_).c_str();
    }
};

struct ScalarTag {};
struct BigIntTag {};
struct RationalTag {};

template<class T> struct _NumTag { 
    using Type = 
        enable_if_t<std::is_arithmetic<T>::value, ScalarTag>; 
};

template<class T> using NumTag = typename _NumTag<remove_cvref_t<T>>::Type;

/// A local version for abs that is garanteed to work with libnest2d types
template <class T> inline T abs(const T& v, ScalarTag) 
{ 
    return std::abs(v); 
}

template<class T> inline T abs(const T& v) { return abs(v, NumTag<T>()); }

template<class T2, class T1> inline T2 cast(const T1& v, ScalarTag, ScalarTag) 
{
    return static_cast<T2>(v);    
}

template<class T2, class T1> inline T2 cast(const T1& v) { 
    return cast<T2, T1>(v, NumTag<T1>(), NumTag<T2>());
}

}
#endif // LIBNEST2D_CONFIG_HPP
