#ifndef MTUTILS_HPP
#define MTUTILS_HPP

#include <atomic>       // for std::atomic_flag and memory orders
#include <mutex>        // for std::lock_guard
#include <functional>   // for std::function
#include <utility>      // for std::forward
#include <vector>
#include <algorithm>
#include <cmath>

#include "libslic3r.h"

namespace Slic3r {

/// Handy little spin mutex for the cached meshes.
/// Implements the "Lockable" concept
class SpinMutex
{
    std::atomic_flag                m_flg;
    static const /*constexpr*/ auto MO_ACQ = std::memory_order_acquire;
    static const /*constexpr*/ auto MO_REL = std::memory_order_release;

public:
    inline SpinMutex() { m_flg.clear(MO_REL); }
    inline void lock() { while (m_flg.test_and_set(MO_ACQ)) ; }
    inline bool try_lock() { return !m_flg.test_and_set(MO_ACQ); }
    inline void unlock() { m_flg.clear(MO_REL); }
};

/// A wrapper class around arbitrary object that needs thread safe caching.
template<class T> class CachedObject
{
public:
    // Method type which refreshes the object when it has been invalidated
    using Setter = std::function<void(T &)>;

private:
    T         m_obj;   // the object itself
    bool      m_valid; // invalidation flag
    SpinMutex m_lck;   // to make the caching thread safe

    // the setter will be called just before the object's const value is
    // about to be retrieved.
    std::function<void(T &)> m_setter;

public:
    // Forwarded constructor
    template<class... Args>
    inline CachedObject(Setter &&fn, Args &&... args)
        : m_obj(std::forward<Args>(args)...), m_valid(false), m_setter(fn)
    {}

    // invalidate the value of the object. The object will be refreshed at
    // the next retrieval (Setter will be called). The data that is used in
    // the setter function should be guarded as well during modification so
    // the modification has to take place in fn.
    template<class Fn> void invalidate(Fn &&fn)
    {
        std::lock_guard<SpinMutex> lck(m_lck);
        fn();
        m_valid = false;
    }

    // Get the const object properly updated.
    inline const T &get()
    {
        std::lock_guard<SpinMutex> lck(m_lck);
        if (!m_valid) {
            m_setter(m_obj);
            m_valid = true;
        }
        return m_obj;
    }
};

template<class C> bool all_of(const C &container)
{
    return std::all_of(container.begin(),
                       container.end(),
                       [](const typename C::value_type &v) {
                           return static_cast<bool>(v);
                       });
}

//template<class T>
//using remove_cvref_t = std::remove_reference_t<std::remove_cv_t<T>>;

/// Exactly like Matlab https://www.mathworks.com/help/matlab/ref/linspace.html
template<class T, class I, class = IntegerOnly<I>>
inline std::vector<T> linspace_vector(const ArithmeticOnly<T> &start, 
                                      const T &stop, 
                                      const I &n)
{
    std::vector<T> vals(n, T());

    T      stride = (stop - start) / n;
    size_t i      = 0;
    std::generate(vals.begin(), vals.end(), [&i, start, stride] {
        return start + i++ * stride;
    });

    return vals;
}

template<size_t N, class T>
inline std::array<ArithmeticOnly<T>, N> linspace_array(const T &start, const T &stop)
{
    std::array<T, N> vals = {T()};

    T      stride = (stop - start) / N;
    size_t i      = 0;
    std::generate(vals.begin(), vals.end(), [&i, start, stride] {
        return start + i++ * stride;
    });

    return vals;
}

/// A set of equidistant values starting from 'start' (inclusive), ending
/// in the closest multiple of 'stride' less than or equal to 'end' and
/// leaving 'stride' space between each value. 
/// Very similar to Matlab [start:stride:end] notation.
template<class T>
inline std::vector<ArithmeticOnly<T>> grid(const T &start, 
                                           const T &stop, 
                                           const T &stride)
{
    std::vector<T> vals(size_t(std::ceil((stop - start) / stride)), T());
    
    int i = 0;
    std::generate(vals.begin(), vals.end(), [&i, start, stride] {
        return start + i++ * stride; 
    });
     
    return vals;
}

} // namespace Slic3r

#endif // MTUTILS_HPP
