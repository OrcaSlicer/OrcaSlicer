#pragma once

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

#ifndef GLZ_THROW_OR_ABORT
#if __cpp_exceptions
#define GLZ_THROW_OR_ABORT(EXC) (throw(EXC))
#define GLZ_NOEXCEPT noexcept(false)
#else
#define GLZ_THROW_OR_ABORT(EXC) (std::abort())
#define GLZ_NOEXCEPT noexcept(true)
#endif
#endif

#include "glaze/thread/value_proxy.hpp"

// Provides a thread-safe vector
// Uses simple proxy objects with appropriate locks for read/write operations

namespace glz
{
   template <class T>
   struct async_vector
   {
     private:
      std::vector<T> items;
      mutable std::shared_mutex mutex;

     public:
      using value_type = T;
      using size_type = std::size_t;
      using difference_type = std::ptrdiff_t;
      using reference = T&;
      using const_reference = const T&;

      // Constructors
      async_vector() = default;

      // Copy constructor
      async_vector(const async_vector& other)
      {
         std::shared_lock lock(other.mutex);
         items = other.items;
      }

      // Move constructor
      async_vector(async_vector&& other) noexcept
      {
         std::unique_lock lock(other.mutex);
         items = std::move(other.items);
      }

      // Copy assignment
      async_vector& operator=(const async_vector& other)
      {
         if (this != &other) {
            std::unique_lock lock1(mutex, std::defer_lock);
            std::shared_lock lock2(other.mutex, std::defer_lock);
            std::lock(lock1, lock2);
            items = other.items;
         }
         return *this;
      }

      // Move assignment
      async_vector& operator=(async_vector&& other) noexcept
      {
         if (this != &other) {
            std::unique_lock lock1(mutex, std::defer_lock);
            std::unique_lock lock2(other.mutex, std::defer_lock);
            std::lock(lock1, lock2);
            items = std::move(other.items);
         }
         return *this;
      }

      // Proxy for write access
      struct proxy
      {
         using value_type = typename std::vector<T>::value_type;
         using size_type = typename std::vector<T>::size_type;
         using difference_type = typename std::vector<T>::difference_type;
         using reference = typename std::vector<T>::reference;
         using const_reference = typename std::vector<T>::const_reference;
         using pointer = typename std::vector<T>::pointer;
         using const_pointer = typename std::vector<T>::const_pointer;
         using iterator = typename std::vector<T>::iterator;
         using const_iterator = typename std::vector<T>::const_iterator;
         using reverse_iterator = typename std::vector<T>::reverse_iterator;
         using const_reverse_iterator = typename std::vector<T>::const_reverse_iterator;

         std::vector<T>* ptr{};
         std::unique_lock<std::shared_mutex> lock{};

        public:
         proxy(std::vector<T>& p, std::unique_lock<std::shared_mutex>&& lock) noexcept : ptr{&p}, lock(std::move(lock))
         {}

         // Size and capacity
         bool empty() const noexcept { return ptr->empty(); }
         size_type size() const noexcept { return ptr->size(); }
         size_type max_size() const noexcept { return ptr->max_size(); }
         size_type capacity() const noexcept { return ptr->capacity(); }
         void reserve(size_type new_cap) { ptr->reserve(new_cap); }
         void shrink_to_fit() { ptr->shrink_to_fit(); }
         void resize(size_type count) { ptr->resize(count); }
         void resize(size_type count, const value_type& value) { ptr->resize(count, value); }

         // Element access
         reference operator[](size_type pos) { return (*ptr)[pos]; }
         const_reference operator[](size_type pos) const { return (*ptr)[pos]; }
         reference at(size_type pos) { return ptr->at(pos); }
         const_reference at(size_type pos) const { return ptr->at(pos); }
         reference front() { return ptr->front(); }
         const_reference front() const { return ptr->front(); }
         reference back() { return ptr->back(); }
         const_reference back() const { return ptr->back(); }
         T* data() noexcept { return ptr->data(); }
         const T* data() const noexcept { return ptr->data(); }

         // Iterators
         iterator begin() noexcept { return ptr->begin(); }
         const_iterator begin() const noexcept { return ptr->begin(); }
         const_iterator cbegin() const noexcept { return ptr->cbegin(); }

         iterator end() noexcept { return ptr->end(); }
         const_iterator end() const noexcept { return ptr->end(); }
         const_iterator cend() const noexcept { return ptr->cend(); }

         reverse_iterator rbegin() noexcept { return ptr->rbegin(); }
         const_reverse_iterator rbegin() const noexcept { return ptr->rbegin(); }
         const_reverse_iterator crbegin() const noexcept { return ptr->crbegin(); }

         reverse_iterator rend() noexcept { return ptr->rend(); }
         const_reverse_iterator rend() const noexcept { return ptr->rend(); }
         const_reverse_iterator crend() const noexcept { return ptr->crend(); }

         // Modifiers
         void clear() { ptr->clear(); }

         iterator insert(const_iterator pos, const T& value) { return ptr->insert(pos, value); }

         iterator insert(const_iterator pos, T&& value) { return ptr->insert(pos, std::move(value)); }

         iterator insert(const_iterator pos, size_type count, const T& value) { return ptr->insert(pos, count, value); }

         template <class InputIt>
         iterator insert(const_iterator pos, InputIt first, InputIt last)
         {
            return ptr->insert(pos, first, last);
         }

         iterator insert(const_iterator pos, std::initializer_list<T> ilist) { return ptr->insert(pos, ilist); }

         template <class... Args>
         iterator emplace(const_iterator pos, Args&&... args)
         {
            return ptr->emplace(pos, std::forward<Args>(args)...);
         }

         iterator erase(const_iterator pos) { return ptr->erase(pos); }

         iterator erase(const_iterator first, const_iterator last) { return ptr->erase(first, last); }

         void push_back(const T& value) { ptr->push_back(value); }

         void push_back(T&& value) { ptr->push_back(std::move(value)); }

         template <class... Args>
         reference emplace_back(Args&&... args)
         {
            return ptr->emplace_back(std::forward<Args>(args)...);
         }

         void pop_back() { ptr->pop_back(); }

         void swap(proxy& other) noexcept(noexcept(ptr->swap(*other.ptr))) { ptr->swap(*other.ptr); }

         void swap(std::vector<T>& other) noexcept(noexcept(ptr->swap(other))) { ptr->swap(other); }

         // Comparison operators
         bool operator==(const proxy& other) const { return *ptr == *other.ptr; }

         bool operator!=(const proxy& other) const { return *ptr != *other.ptr; }

         bool operator<(const proxy& other) const { return *ptr < *other.ptr; }

         bool operator<=(const proxy& other) const { return *ptr <= *other.ptr; }

         bool operator>(const proxy& other) const { return *ptr > *other.ptr; }

         bool operator>=(const proxy& other) const { return *ptr >= *other.ptr; }

         // Allow access to the underlying vector for advanced operations
         std::vector<T>* operator->() noexcept { return ptr; }
         const std::vector<T>* operator->() const noexcept { return ptr; }

         std::vector<T>& operator*() noexcept { return *ptr; }
         const std::vector<T>& operator*() const noexcept { return *ptr; }

         std::vector<T>& value() noexcept { return *ptr; }
         const std::vector<T>& value() const noexcept { return *ptr; }
      };

      // Proxy for read access
      struct const_proxy
      {
         using value_type = typename std::vector<T>::value_type;
         using size_type = typename std::vector<T>::size_type;
         using difference_type = typename std::vector<T>::difference_type;
         using reference = typename std::vector<T>::reference;
         using const_reference = typename std::vector<T>::const_reference;
         using pointer = typename std::vector<T>::pointer;
         using const_pointer = typename std::vector<T>::const_pointer;
         using iterator = typename std::vector<T>::iterator;
         using const_iterator = typename std::vector<T>::const_iterator;
         using reverse_iterator = typename std::vector<T>::reverse_iterator;
         using const_reverse_iterator = typename std::vector<T>::const_reverse_iterator;

         const std::vector<T>* ptr{};
         std::shared_lock<std::shared_mutex> lock{};

        public:
         const_proxy(const std::vector<T>& p, std::shared_lock<std::shared_mutex>&& lock) noexcept
            : ptr{&p}, lock(std::move(lock))
         {}

         // Size and capacity
         bool empty() const noexcept { return ptr->empty(); }
         size_type size() const noexcept { return ptr->size(); }
         size_type max_size() const noexcept { return ptr->max_size(); }
         size_type capacity() const noexcept { return ptr->capacity(); }

         // Iterators
         const_iterator begin() const noexcept { return ptr->begin(); }
         const_iterator cbegin() const noexcept { return ptr->cbegin(); }

         const_iterator end() const noexcept { return ptr->end(); }
         const_iterator cend() const noexcept { return ptr->cend(); }

         const_reverse_iterator rbegin() const noexcept { return ptr->rbegin(); }
         const_reverse_iterator crbegin() const noexcept { return ptr->crbegin(); }

         const_reverse_iterator rend() const noexcept { return ptr->rend(); }
         const_reverse_iterator crend() const noexcept { return ptr->crend(); }

         // Element access
         const T& operator[](size_type pos) const { return (*ptr)[pos]; }
         const T& at(size_type pos) const { return ptr->at(pos); }
         const T& front() const { return ptr->front(); }
         const T& back() const { return ptr->back(); }

         // Allow access to the underlying vector for advanced operations
         const std::vector<T>* operator->() const noexcept { return ptr; }
         const std::vector<T>& operator*() const noexcept { return *ptr; }
         const std::vector<T>& value() const noexcept { return *ptr; }
      };

      // Core access methods
      proxy write() { return {items, std::unique_lock{mutex}}; }
      const_proxy read() const { return {items, std::shared_lock{mutex}}; }

      // Common operations with their own locks (for convenience)
      bool empty() const noexcept
      {
         std::shared_lock lock(mutex);
         return items.empty();
      }

      size_type size() const noexcept
      {
         std::shared_lock lock(mutex);
         return items.size();
      }

      void resize(size_type count)
      {
         std::unique_lock lock(mutex);
         items.resize(count);
      }

      void resize(size_type count, const value_type& value)
      {
         std::unique_lock lock(mutex);
         items.resize(count, value);
      }

      void clear() noexcept
      {
         std::unique_lock lock(mutex);
         items.clear();
      }

      void push_back(const T& value)
      {
         std::unique_lock lock(mutex);
         items.push_back(value);
      }

      void push_back(T&& value)
      {
         std::unique_lock lock(mutex);
         items.push_back(std::move(value));
      }

      template <class... Args>
      void emplace_back(Args&&... args)
      {
         std::unique_lock lock(mutex);
         items.emplace_back(std::forward<Args>(args)...);
      }

      void pop_back()
      {
         std::unique_lock lock(mutex);
         items.pop_back();
      }

      // Capacity methods
      size_type max_size() const noexcept
      {
         std::shared_lock lock(mutex);
         return items.max_size();
      }

      void reserve(size_type new_cap)
      {
         std::unique_lock lock(mutex);
         items.reserve(new_cap);
      }

      size_type capacity() const noexcept
      {
         std::shared_lock lock(mutex);
         return items.capacity();
      }

      void shrink_to_fit()
      {
         std::unique_lock lock(mutex);
         items.shrink_to_fit();
      }

      // Swap operations
      void swap(async_vector& other) noexcept
      {
         if (this == &other) return;
         std::scoped_lock lock(mutex, other.mutex);
         items.swap(other.items);
      }

      friend void swap(async_vector& a, async_vector& b) noexcept { a.swap(b); }

      friend bool operator==(const async_vector& lhs, const async_vector& rhs)
      {
         if (&lhs == &rhs) return true;
         std::shared_lock lock(lhs.mutex, std::defer_lock);
         std::shared_lock lock2(rhs.mutex, std::defer_lock);
         std::lock(lock, lock2);
         return lhs.items == rhs.items;
      }

      // async_vector != async_vector
      friend bool operator!=(const async_vector& lhs, const async_vector& rhs) { return !(lhs == rhs); }

      // async_vector == const std::vector<T>&
      friend bool operator==(const async_vector& lhs, const std::vector<T>& rhs)
      {
         std::shared_lock lock(lhs.mutex);
         return lhs.items == rhs;
      }

      // const std::vector<T>& == async_vector
      friend bool operator==(const std::vector<T>& lhs, const async_vector& rhs)
      {
         std::shared_lock lock(rhs.mutex);
         return lhs == rhs.items;
      }

      // async_vector != const std::vector<T>&
      friend bool operator!=(const async_vector& lhs, const std::vector<T>& rhs) { return !(lhs == rhs); }

      // const std::vector<T>& != async_vector
      friend bool operator!=(const std::vector<T>& lhs, const async_vector& rhs) { return !(lhs == rhs); }
   };
}

namespace glz
{
   template <uint32_t Format, class T>
      requires(is_specialization_v<T, glz::async_vector>)
   struct from<Format, T>
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end) noexcept
      {
         auto proxy = value.write();
         parse<Format>::template op<Opts>(*proxy, ctx, it, end);
      }
   };

   template <uint32_t Format, class T>
      requires(is_specialization_v<T, glz::async_vector>)
   struct to<Format, T>
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&&... args) noexcept
      {
         auto proxy = value.read();
         serialize<Format>::template op<Opts>(*proxy, ctx, args...);
      }
   };
}
