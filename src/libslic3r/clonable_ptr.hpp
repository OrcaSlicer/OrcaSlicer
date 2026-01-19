// clonable_ptr: a smart pointer with a usage similar to unique_ptr, with the exception, that
// the copy constructor / copy assignment operator work by calling the ->clone() method.

// derived from https://github.com/SRombauts/shared_ptr/blob/master/include/unique_ptr.hpp
/**
 * @file  clonable_ptr.hpp
 * @brief clonable_ptr is a fake implementation to use in place of a C++11 std::clonable_ptr when compiling on an older compiler.
 *
 * @see http://www.cplusplus.com/reference/memory/clonable_ptr/
 *
 * Copyright (c) 2014-2019 Sebastien Rombauts (sebastien.rombauts@gmail.com)
 *
 * Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
 * or copy at http://opensource.org/licenses/MIT)
 */

#include "assert.h"

namespace Slic3r {

// Detect whether the compiler supports C++11 noexcept exception specifications.
#if defined(_MSC_VER) && _MSC_VER < 1900 && ! defined(noexcept)
    #define noexcept throw()
#endif

template<class T>
class clonable_ptr
{
public:
    /// The type of the managed object, aliased as member type
    typedef T element_type;

    /// @brief Default constructor
    clonable_ptr() noexcept :
        px(nullptr)
    {
    }
    /// @brief Constructor with the provided pointer to manage
    explicit clonable_ptr(T* p) noexcept :
        px(p)
    {
    }
    /// @brief Copy constructor, clones by calling the rhs.clone() method
    clonable_ptr(const clonable_ptr& rhs) :
		px(rhs ? rhs.px->clone() : nullptr)
    {
    }
    /// @brief Move constructor, never throws
    clonable_ptr(clonable_ptr&& rhs) noexcept :
        px(rhs.px)
    {
        rhs.px = nullptr;
    }
    /// @brief Assignment operator
    clonable_ptr& operator=(const clonable_ptr& rhs)
    {
		delete px;
		px = rhs ? rhs.px->clone() : nullptr;
        return *this;
    }
    /// @brief Move operator, never throws
    clonable_ptr& operator=(clonable_ptr&& rhs)
    {
		delete px;
        px = rhs.px;
        rhs.px = nullptr;
        return *this;
    }
    /// @brief the destructor releases its ownership and destroy the object
    inline ~clonable_ptr() noexcept
    {
        destroy();
    }
    /// @brief this reset releases its ownership and destroy the object
    inline void reset() noexcept
    {
        destroy();
    }
    /// @brief this reset release its ownership and re-acquire another one
    void reset(T* p) noexcept
    {
        assert((nullptr == p) || (px != p)); // auto-reset not allowed
        destroy();
        px = p;
    }

    /// @brief Swap method for the copy-and-swap idiom (copy constructor and swap method)
    void swap(clonable_ptr& rhs) noexcept
    {
        T *tmp = px;
        px = rhs.px;
        rhs.px = tmp;
    }

    /// @brief release the ownership of the px pointer without destroying the object!
    inline void release() noexcept
    {
        px = nullptr;
    }

    // reference counter operations :
    inline operator bool() const noexcept
    {
        return (nullptr != px); // TODO nullptrptr
    }

    // underlying pointer operations :
    inline T& operator*()  const noexcept
    {
        assert(nullptr != px);
        return *px;
    }
    inline T* operator->() const noexcept
    {
        assert(nullptr != px);
        return px;
    }
    inline T* get()  const noexcept
    {
        // no assert, can return nullptr
        return px;
    }

private:
    /// @brief release the ownership of the px pointer and destroy the object
    inline void destroy() noexcept
    {
        delete px;
        px = nullptr;
    }

    /// @brief hack: const-cast release the ownership of the px pointer without destroying the object!
    inline void release() const noexcept
    {
        px = nullptr;
    }

private:
    T* px; //!< Native pointer
};

// comparison operators
template<class T, class U> inline bool operator==(const clonable_ptr<T>& l, const clonable_ptr<U>& r) noexcept
{
    return (l.get() == r.get());
}
template<class T, class U> inline bool operator!=(const clonable_ptr<T>& l, const clonable_ptr<U>& r) noexcept
{
    return (l.get() != r.get());
}
template<class T, class U> inline bool operator<=(const clonable_ptr<T>& l, const clonable_ptr<U>& r) noexcept
{
    return (l.get() <= r.get());
}
template<class T, class U> inline bool operator<(const clonable_ptr<T>& l, const clonable_ptr<U>& r) noexcept
{
    return (l.get() < r.get());
}
template<class T, class U> inline bool operator>=(const clonable_ptr<T>& l, const clonable_ptr<U>& r) noexcept
{
    return (l.get() >= r.get());
}
template<class T, class U> inline bool operator>(const clonable_ptr<T>& l, const clonable_ptr<U>& r) noexcept
{
    return (l.get() > r.get());
}

} // namespace Slic3r
