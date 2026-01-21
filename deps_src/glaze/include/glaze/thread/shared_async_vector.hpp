#pragma once

#include <algorithm>
#include <cassert>
#include <iterator>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

#include "glaze/thread/value_proxy.hpp"
#include "glaze/util/expected.hpp"

#ifndef GLZ_THROW_OR_ABORT
#if __cpp_exceptions
#define GLZ_THROW_OR_ABORT(EXC) (throw(EXC))
#define GLZ_NOEXCEPT noexcept(false)
#else
#define GLZ_THROW_OR_ABORT(EXC) (std::abort())
#define GLZ_NOEXCEPT noexcept(true)
#endif
#endif

// Provides a thread-safe vector with shared ownership
// This shared_async_vector allows multiple instances to share the same data and mutex
// It ensures thread safety during read and write operations

namespace glz
{

   template <class V>
   struct shared_async_vector
   {
     private:
      struct shared_state
      {
         std::vector<std::unique_ptr<V>> items;
         mutable std::shared_mutex mutex;
      };

      std::shared_ptr<shared_state> state = std::make_shared<shared_state>();

     public:
      using value_type = V;
      using size_type = std::size_t;
      using difference_type = std::ptrdiff_t;
      using reference = V&;
      using const_reference = const V&;
      using pointer = V*;
      using const_pointer = const V*;

      // Forward declaration of iterator classes
      class iterator;
      class const_iterator;

      // Iterator Class Definition
      class iterator
      {
        public:
         using iterator_category = std::random_access_iterator_tag;
         using value_type = V;
         using difference_type = std::ptrdiff_t;
         using pointer = V*;
         using reference = V&;

        private:
         typename std::vector<std::unique_ptr<V>>::iterator item_it;
         std::shared_ptr<shared_state> state;
         std::shared_ptr<std::shared_lock<std::shared_mutex>> shared_lock_ptr;
         std::shared_ptr<std::unique_lock<std::shared_mutex>> unique_lock_ptr;

        public:
         iterator(typename std::vector<std::unique_ptr<V>>::iterator item_it, std::shared_ptr<shared_state> state,
                  std::shared_ptr<std::shared_lock<std::shared_mutex>> existing_shared_lock = nullptr,
                  std::shared_ptr<std::unique_lock<std::shared_mutex>> existing_unique_lock = nullptr)
            : item_it(item_it),
              state(state),
              shared_lock_ptr(existing_shared_lock),
              unique_lock_ptr(existing_unique_lock)
         {
            // Acquire a shared lock only if no lock is provided
            if (!shared_lock_ptr && !unique_lock_ptr) {
               shared_lock_ptr = std::make_shared<std::shared_lock<std::shared_mutex>>(state->mutex);
            }
         }

         // Copy Constructor
         iterator(const iterator& other)
            : item_it(other.item_it),
              state(other.state),
              shared_lock_ptr(other.shared_lock_ptr),
              unique_lock_ptr(other.unique_lock_ptr)
         {}

         // Move Constructor
         iterator(iterator&& other) noexcept
            : item_it(std::move(other.item_it)),
              state(std::move(other.state)),
              shared_lock_ptr(std::move(other.shared_lock_ptr)),
              unique_lock_ptr(std::move(other.unique_lock_ptr))
         {}

         // Copy Assignment
         iterator& operator=(const iterator& other)
         {
            if (this != &other) {
               item_it = other.item_it;
               state = other.state;
               shared_lock_ptr = other.shared_lock_ptr;
               unique_lock_ptr = other.unique_lock_ptr;
            }
            return *this;
         }

         friend struct shared_async_vector<V>;

         // Dereference operators
         reference operator*() const { return *(*item_it); }
         pointer operator->() const { return (*item_it).get(); }

         // Increment and Decrement operators
         iterator& operator++()
         {
            ++item_it;
            return *this;
         }

         iterator operator++(int)
         {
            iterator tmp(*this);
            ++(*this);
            return tmp;
         }

         iterator& operator--()
         {
            --item_it;
            return *this;
         }

         iterator operator--(int)
         {
            iterator tmp(*this);
            --(*this);
            return tmp;
         }

         // Arithmetic operators
         iterator operator+(difference_type n) const
         {
            return iterator(item_it + n, state, shared_lock_ptr, unique_lock_ptr);
         }

         iterator& operator+=(difference_type n)
         {
            item_it += n;
            return *this;
         }

         iterator operator-(difference_type n) const
         {
            return iterator(item_it - n, state, shared_lock_ptr, unique_lock_ptr);
         }

         iterator& operator-=(difference_type n)
         {
            item_it -= n;
            return *this;
         }

         difference_type operator-(const iterator& other) const { return item_it - other.item_it; }

         // Comparison operators
         bool operator==(const iterator& other) const noexcept { return item_it == other.item_it; }
         bool operator!=(const iterator& other) const noexcept { return item_it != other.item_it; }
         bool operator<(const iterator& other) const noexcept { return item_it < other.item_it; }
         bool operator>(const iterator& other) const noexcept { return item_it > other.item_it; }
         bool operator<=(const iterator& other) const noexcept { return item_it <= other.item_it; }
         bool operator>=(const iterator& other) const noexcept { return item_it >= other.item_it; }

         // Access operator
         reference operator[](difference_type n) const { return *(*(item_it + n)); }
      };

      // Const Iterator Class Definition
      class const_iterator
      {
        public:
         using iterator_category = std::random_access_iterator_tag;
         using value_type = V;
         using difference_type = std::ptrdiff_t;
         using pointer = const V*;
         using reference = const V&;

        private:
         typename std::vector<std::unique_ptr<V>>::const_iterator item_it;
         std::shared_ptr<shared_state> state;
         std::shared_ptr<std::shared_lock<std::shared_mutex>> shared_lock_ptr;

        public:
         const_iterator(typename std::vector<std::unique_ptr<V>>::const_iterator item_it,
                        std::shared_ptr<shared_state> state,
                        std::shared_ptr<std::shared_lock<std::shared_mutex>> existing_shared_lock = nullptr)
            : item_it(item_it), state(state), shared_lock_ptr(existing_shared_lock)
         {
            // Acquire a shared lock only if no lock is provided
            if (!shared_lock_ptr) {
               shared_lock_ptr = std::make_shared<std::shared_lock<std::shared_mutex>>(state->mutex);
            }
         }

         // Copy Constructor
         const_iterator(const const_iterator& other)
            : item_it(other.item_it), state(other.state), shared_lock_ptr(other.shared_lock_ptr)
         {}

         // Move Constructor
         const_iterator(const_iterator&& other) noexcept
            : item_it(std::move(other.item_it)),
              state(std::move(other.state)),
              shared_lock_ptr(std::move(other.shared_lock_ptr))
         {}

         // Copy Assignment
         const_iterator& operator=(const const_iterator& other)
         {
            if (this != &other) {
               item_it = other.item_it;
               state = other.state;
               shared_lock_ptr = other.shared_lock_ptr;
            }
            return *this;
         }

         friend struct shared_async_vector<V>;

         // Dereference operators
         reference operator*() const { return *(*item_it); }
         pointer operator->() const { return (*item_it).get(); }

         // Increment and Decrement operators
         const_iterator& operator++()
         {
            ++item_it;
            return *this;
         }

         const_iterator operator++(int)
         {
            const_iterator tmp(*this);
            ++(*this);
            return tmp;
         }

         const_iterator& operator--()
         {
            --item_it;
            return *this;
         }

         const_iterator operator--(int)
         {
            const_iterator tmp(*this);
            --(*this);
            return tmp;
         }

         // Arithmetic operators
         const_iterator operator+(difference_type n) const
         {
            return const_iterator(item_it + n, state, shared_lock_ptr);
         }

         const_iterator& operator+=(difference_type n)
         {
            item_it += n;
            return *this;
         }

         const_iterator operator-(difference_type n) const
         {
            return const_iterator(item_it - n, state, shared_lock_ptr);
         }

         const_iterator& operator-=(difference_type n)
         {
            item_it -= n;
            return *this;
         }

         difference_type operator-(const const_iterator& other) const { return item_it - other.item_it; }

         // Comparison operators
         bool operator==(const const_iterator& other) const noexcept { return item_it == other.item_it; }
         bool operator!=(const const_iterator& other) const noexcept { return item_it != other.item_it; }
         bool operator<(const const_iterator& other) const noexcept { return item_it < other.item_it; }
         bool operator>(const const_iterator& other) const noexcept { return item_it > other.item_it; }
         bool operator<=(const const_iterator& other) const noexcept { return item_it <= other.item_it; }
         bool operator>=(const const_iterator& other) const noexcept { return item_it >= other.item_it; }

         // Access operator
         reference operator[](difference_type n) const { return *(*(item_it + n)); }
      };

      // Value Proxy Class Definition
      class value_proxy
      {
        private:
         V& value_ref;
         std::shared_ptr<std::shared_lock<std::shared_mutex>> shared_lock_ptr;
         std::shared_ptr<std::unique_lock<std::shared_mutex>> unique_lock_ptr;

        public:
         value_proxy(V& value_ref, std::shared_ptr<std::shared_lock<std::shared_mutex>> existing_shared_lock = nullptr,
                     std::shared_ptr<std::unique_lock<std::shared_mutex>> existing_unique_lock = nullptr)
            : value_ref(value_ref), shared_lock_ptr(existing_shared_lock), unique_lock_ptr(existing_unique_lock)
         {
            // Ensure that a lock is provided
            assert(shared_lock_ptr || unique_lock_ptr);
         }

         static constexpr bool glaze_value_proxy = true;

         // Disable Copy and Move
         value_proxy(const value_proxy&) = delete;
         value_proxy& operator=(const value_proxy&) = delete;
         value_proxy(value_proxy&&) = delete;
         value_proxy& operator=(value_proxy&&) = delete;

         // Access the value
         V& value() { return value_ref; }

         const V& value() const { return value_ref; }

         // Arrow Operator
         V* operator->() { return &value_ref; }

         const V* operator->() const { return &value_ref; }

         V& operator*() { return value_ref; }

         const V& operator*() const { return value_ref; }

         // Implicit Conversion to V&
         operator V&() { return value_ref; }

         operator const V&() const { return value_ref; }

         template <class T>
         value_proxy& operator=(const T& other)
         {
            value_ref = other;
            return *this;
         }

         bool operator==(const V& other) const { return value() == other; }
      };

      // Const Value Proxy Class Definition
      class const_value_proxy
      {
        private:
         const V& value_ref;
         std::shared_ptr<std::shared_lock<std::shared_mutex>> shared_lock_ptr;

        public:
         const_value_proxy(const V& value_ref,
                           std::shared_ptr<std::shared_lock<std::shared_mutex>> existing_shared_lock)
            : value_ref(value_ref), shared_lock_ptr(existing_shared_lock)
         {
            // Ensure that a lock is provided
            assert(shared_lock_ptr);
         }

         // Disable Copy and Move
         const_value_proxy(const const_value_proxy&) = delete;
         const_value_proxy& operator=(const const_value_proxy&) = delete;
         const_value_proxy(const_value_proxy&&) = delete;
         const_value_proxy& operator=(const const_value_proxy&&) = delete;

         // Access the value
         const V& value() const { return value_ref; }

         // Arrow Operator
         const V* operator->() const { return &value_ref; }

         const V& operator*() const { return value_ref; }

         // Implicit Conversion to const V&
         operator const V&() const { return value_ref; }

         bool operator==(const V& other) const { return value() == other; }
      };

      // Element Access Methods
      value_proxy operator[](size_type pos)
      {
         auto shared_lock_ptr = std::make_shared<std::shared_lock<std::shared_mutex>>(state->mutex);
         return value_proxy(*state->items[pos], shared_lock_ptr);
      }

      const_value_proxy operator[](size_type pos) const
      {
         auto shared_lock_ptr = std::make_shared<std::shared_lock<std::shared_mutex>>(state->mutex);
         return const_value_proxy(*state->items[pos], shared_lock_ptr);
      }

      value_proxy at(size_type pos)
      {
         auto shared_lock_ptr = std::make_shared<std::shared_lock<std::shared_mutex>>(state->mutex);
         if (pos >= state->items.size()) {
            GLZ_THROW_OR_ABORT(std::out_of_range("Index out of range"));
         }
         return value_proxy(*state->items[pos], shared_lock_ptr);
      }

      const_value_proxy at(size_type pos) const
      {
         auto shared_lock_ptr = std::make_shared<std::shared_lock<std::shared_mutex>>(state->mutex);
         if (pos >= state->items.size()) {
            GLZ_THROW_OR_ABORT(std::out_of_range("Index out of range"));
         }
         return const_value_proxy(*state->items[pos], shared_lock_ptr);
      }

      value_proxy front()
      {
         auto shared_lock_ptr = std::make_shared<std::shared_lock<std::shared_mutex>>(state->mutex);
         return value_proxy(*state->items.front(), shared_lock_ptr);
      }

      const_value_proxy front() const
      {
         auto shared_lock_ptr = std::make_shared<std::shared_lock<std::shared_mutex>>(state->mutex);
         return const_value_proxy(*state->items.front(), shared_lock_ptr);
      }

      value_proxy back()
      {
         auto shared_lock_ptr = std::make_shared<std::shared_lock<std::shared_mutex>>(state->mutex);
         return value_proxy(*state->items.back(), shared_lock_ptr);
      }

      const_value_proxy back() const
      {
         auto shared_lock_ptr = std::make_shared<std::shared_lock<std::shared_mutex>>(state->mutex);
         return const_value_proxy(*state->items.back(), shared_lock_ptr);
      }

      // Capacity Methods
      bool empty() const
      {
         std::shared_lock lock(state->mutex);
         return state->items.empty();
      }

      size_type size() const
      {
         std::shared_lock lock(state->mutex);
         return state->items.size();
      }

      size_type max_size() const
      {
         std::shared_lock lock(state->mutex);
         return state->items.max_size();
      }

      void reserve(size_type new_cap)
      {
         std::unique_lock lock(state->mutex);
         state->items.reserve(new_cap);
      }

      size_type capacity() const
      {
         std::shared_lock lock(state->mutex);
         return state->items.capacity();
      }

      void shrink_to_fit()
      {
         std::unique_lock lock(state->mutex);
         state->items.shrink_to_fit();
      }

      // Modifiers
      void clear()
      {
         std::unique_lock lock(state->mutex);
         state->items.clear();
      }

      void push_back(const V& value)
      {
         std::unique_lock lock(state->mutex);
         state->items.push_back(std::make_unique<V>(value));
      }

      template <class... Args>
      void emplace_back(Args&&... args)
      {
         std::unique_lock lock(state->mutex);
         state->items.emplace_back(std::make_unique<V>(std::forward<Args>(args)...));
      }

      void pop_back()
      {
         std::unique_lock lock(state->mutex);
         state->items.pop_back();
      }

      iterator insert(const_iterator pos, const V& value)
      {
         auto unique_lock_ptr = std::make_shared<std::unique_lock<std::shared_mutex>>(state->mutex);
         auto it = state->items.insert(pos.item_it, std::make_unique<V>(value));
         return iterator(it, state, nullptr, unique_lock_ptr);
      }

      iterator insert(const_iterator pos, V&& value)
      {
         auto unique_lock_ptr = std::make_shared<std::unique_lock<std::shared_mutex>>(state->mutex);
         auto it = state->items.insert(pos.item_it, std::make_unique<V>(std::move(value)));
         return iterator(it, state, nullptr, unique_lock_ptr);
      }

      template <class... Args>
      iterator emplace(const_iterator pos, Args&&... args)
      {
         auto unique_lock_ptr = std::make_shared<std::unique_lock<std::shared_mutex>>(state->mutex);
         auto it = state->items.emplace(pos.item_it, std::make_unique<V>(std::forward<Args>(args)...));
         return iterator(it, state, nullptr, unique_lock_ptr);
      }

      iterator erase(iterator pos)
      {
         auto unique_lock_ptr = std::make_shared<std::unique_lock<std::shared_mutex>>(state->mutex);
         auto it = state->items.erase(pos.item_it);
         return iterator(it, state, nullptr, unique_lock_ptr);
      }

      iterator erase(iterator first, iterator last)
      {
         auto unique_lock_ptr = std::make_shared<std::unique_lock<std::shared_mutex>>(state->mutex);
         auto it = state->items.erase(first.item_it, last.item_it);
         return iterator(it, state, nullptr, unique_lock_ptr);
      }

      iterator erase(const_iterator pos)
      {
         auto unique_lock_ptr = std::make_shared<std::unique_lock<std::shared_mutex>>(state->mutex);
         auto it = state->items.erase(pos.item_it);
         return iterator(it, state, nullptr, unique_lock_ptr);
      }

      iterator erase(const_iterator first, const_iterator last)
      {
         auto unique_lock_ptr = std::make_shared<std::unique_lock<std::shared_mutex>>(state->mutex);
         auto it = state->items.erase(first.item_it, last.item_it);
         return iterator(it, state, nullptr, unique_lock_ptr);
      }

      void resize(size_type count)
      {
         std::unique_lock lock(state->mutex);
         if (count < state->items.size()) {
            state->items.resize(count);
         }
         else {
            while (state->items.size() < count) {
               state->items.emplace_back(std::make_unique<V>());
            }
         }
      }

      void resize(size_type count, const V& value)
      {
         std::unique_lock lock(state->mutex);
         if (count < state->items.size()) {
            state->items.resize(count);
         }
         else {
            while (state->items.size() < count) {
               state->items.emplace_back(std::make_unique<V>(value));
            }
         }
      }

      void swap(shared_async_vector& other)
      {
         if (state == other.state) return;

         std::unique_lock<std::shared_mutex> lock1(state->mutex, std::defer_lock);
         std::unique_lock<std::shared_mutex> lock2(other.state->mutex, std::defer_lock);
         std::lock(lock1, lock2);
         state->items.swap(other.state->items);
      }

      // Iterators
      iterator begin()
      {
         auto shared_lock_ptr = std::make_shared<std::shared_lock<std::shared_mutex>>(state->mutex);
         return iterator(state->items.begin(), state, shared_lock_ptr);
      }

      const_iterator begin() const
      {
         auto shared_lock_ptr = std::make_shared<std::shared_lock<std::shared_mutex>>(state->mutex);
         return const_iterator(state->items.cbegin(), state, shared_lock_ptr);
      }

      const_iterator cbegin() const
      {
         auto shared_lock_ptr = std::make_shared<std::shared_lock<std::shared_mutex>>(state->mutex);
         return const_iterator(state->items.cbegin(), state, shared_lock_ptr);
      }

      iterator end()
      {
         auto shared_lock_ptr = std::make_shared<std::shared_lock<std::shared_mutex>>(state->mutex);
         return iterator(state->items.end(), state, shared_lock_ptr);
      }

      const_iterator end() const
      {
         auto shared_lock_ptr = std::make_shared<std::shared_lock<std::shared_mutex>>(state->mutex);
         return const_iterator(state->items.cend(), state, shared_lock_ptr);
      }

      const_iterator cend() const
      {
         auto shared_lock_ptr = std::make_shared<std::shared_lock<std::shared_mutex>>(state->mutex);
         return const_iterator(state->items.cend(), state, shared_lock_ptr);
      }
   };
}
