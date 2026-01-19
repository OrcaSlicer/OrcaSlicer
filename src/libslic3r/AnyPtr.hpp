#ifndef ANYPTR_HPP
#define ANYPTR_HPP

#include <memory>
#include <type_traits>
#include <boost/variant.hpp>

namespace Slic3r {

// A general purpose pointer holder that can hold any type of smart pointer
// or raw pointer which can own or not own any object they point to.
// In case a raw pointer is stored, it is not destructed so ownership is
// assumed to be foreign.
//
// The stored pointer is not checked for being null when dereferenced.
//
// This is a movable only object due to the fact that it can possibly hold
// a unique_ptr which a non-copy.
template<class T>
class AnyPtr {
    enum { RawPtr, UPtr, ShPtr, WkPtr };

    boost::variant<T*, std::unique_ptr<T>, std::shared_ptr<T>, std::weak_ptr<T>> ptr;

    template<class Self> static T *get_ptr(Self &&s)
    {
        switch (s.ptr.which()) {
        case RawPtr: return boost::get<T *>(s.ptr);
        case UPtr: return boost::get<std::unique_ptr<T>>(s.ptr).get();
        case ShPtr: return boost::get<std::shared_ptr<T>>(s.ptr).get();
        case WkPtr: {
            auto shptr = boost::get<std::weak_ptr<T>>(s.ptr).lock();
            return shptr.get();
        }
        }

        return nullptr;
    }

public:
    template<class TT = T, class = std::enable_if_t<std::is_convertible_v<TT, T>>>
    AnyPtr(TT *p = nullptr) : ptr{p}
    {}
    template<class TT, class = std::enable_if_t<std::is_convertible_v<TT, T>>>
    AnyPtr(std::unique_ptr<TT> p) : ptr{std::unique_ptr<T>(std::move(p))}
    {}
    template<class TT, class = std::enable_if_t<std::is_convertible_v<TT, T>>>
    AnyPtr(std::shared_ptr<TT> p) : ptr{std::shared_ptr<T>(std::move(p))}
    {}
    template<class TT, class = std::enable_if_t<std::is_convertible_v<TT, T>>>
    AnyPtr(std::weak_ptr<TT> p) : ptr{std::weak_ptr<T>(std::move(p))}
    {}

    ~AnyPtr() = default;

    AnyPtr(AnyPtr &&other) noexcept : ptr{std::move(other.ptr)} {}
    AnyPtr(const AnyPtr &other) = delete;

    AnyPtr &operator=(AnyPtr &&other) noexcept { ptr = std::move(other.ptr); return *this; }
    AnyPtr &operator=(const AnyPtr &other) = delete;

    template<class TT, class = std::enable_if_t<std::is_convertible_v<TT, T>>>
    AnyPtr &operator=(TT *p) { ptr = p; return *this; }

    template<class TT, class = std::enable_if_t<std::is_convertible_v<TT, T>>>
    AnyPtr &operator=(std::unique_ptr<TT> p) { ptr = std::move(p); return *this; }

    template<class TT, class = std::enable_if_t<std::is_convertible_v<TT, T>>>
    AnyPtr &operator=(std::shared_ptr<TT> p) { ptr = p; return *this; }

    template<class TT, class = std::enable_if_t<std::is_convertible_v<TT, T>>>
    AnyPtr &operator=(std::weak_ptr<TT> p) { ptr = std::move(p); return *this; }

    const T &operator*() const { return *get_ptr(*this); }
    T &operator*() { return *get_ptr(*this); }

    T *operator->() { return get_ptr(*this); }
    const T *operator->() const { return get_ptr(*this); }

    T *get() { return get_ptr(*this); }
    const T *get() const { return get_ptr(*this); }

    operator bool() const
    {
        switch (ptr.which()) {
        case RawPtr: return bool(boost::get<T *>(ptr));
        case UPtr: return bool(boost::get<std::unique_ptr<T>>(ptr));
        case ShPtr: return bool(boost::get<std::shared_ptr<T>>(ptr));
        case WkPtr: {
            auto shptr = boost::get<std::weak_ptr<T>>(ptr).lock();
            return bool(shptr);
        }
        }

        return false;
    }

    // If the stored pointer is a shared or weak pointer, returns a reference
    // counted copy. Empty shared pointer is returned otherwise.
    std::shared_ptr<T> get_shared_cpy() const
    {
        std::shared_ptr<T> ret;

        switch (ptr.which()) {
        case ShPtr: ret = boost::get<std::shared_ptr<T>>(ptr); break;
        case WkPtr: ret = boost::get<std::weak_ptr<T>>(ptr).lock(); break;
        default:
            ;
        }

        return ret;
    }

    // If the underlying pointer is unique, convert to shared pointer
    void convert_unique_to_shared()
    {
        if (ptr.which() == UPtr)
            ptr = std::shared_ptr<T>{std::move(boost::get<std::unique_ptr<T>>(ptr))};
    }

    // Returns true if the data is owned by this AnyPtr instance
    bool is_owned() const noexcept
    {
        return ptr.which() == UPtr || ptr.which() == ShPtr;
    }
};

} // namespace Slic3r

#endif // ANYPTR_HPP
