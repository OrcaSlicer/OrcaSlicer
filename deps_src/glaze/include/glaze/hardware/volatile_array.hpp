// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

// A matching std::array API for volatile data

#include <cstddef>
#include <initializer_list>
#include <type_traits>

namespace glz
{
   template <class T>
   concept is_volatile_array = requires { std::decay_t<T>::glaze_volatile_array; };

   template <typename T, std::size_t N>
   class volatile_array
   {
     public:
      static constexpr auto glaze_volatile_array = true;
      static constexpr auto length = N;
      using value_type = T;
      using size_type = std::size_t;
      using difference_type = std::ptrdiff_t;
      using reference = volatile T&;
      using const_reference = const volatile T&;
      using pointer = volatile T*;
      using const_pointer = const volatile T*;
      using iterator = volatile T*;
      using const_iterator = const volatile T*;
      using reverse_iterator = std::reverse_iterator<iterator>;
      using const_reverse_iterator = std::reverse_iterator<const_iterator>;

      volatile_array() noexcept = default;
      volatile_array(std::initializer_list<T> init_list) noexcept
      {
         auto first = init_list.begin();
         auto last = init_list.end();
         auto* it = data_;
         for (; first != last; (void)++first, (void)++it) {
            *it = *first;
         }
      }
      volatile_array(const volatile_array&) noexcept = default;
      volatile_array(volatile_array&&) noexcept = default;

      template <is_volatile_array Other>
      constexpr volatile volatile_array& operator=(const Other& other) volatile
      {
         for (size_type i = 0; i < N; ++i) {
            data_[i] = other.data_[i];
         }
         return *this;
      }

      constexpr reference operator[](size_type pos) volatile noexcept { return data_[pos]; }

      constexpr const_reference operator[](size_type pos) const volatile noexcept { return data_[pos]; }

      constexpr reference front() volatile noexcept { return data_[0]; }

      constexpr const_reference front() const volatile noexcept { return data_[0]; }

      constexpr reference back() volatile noexcept { return data_[N - 1]; }

      constexpr const_reference back() const volatile noexcept { return data_[N - 1]; }

      constexpr pointer data() volatile noexcept { return data_; }

      constexpr const_pointer data() const volatile noexcept { return data_; }

      constexpr iterator begin() volatile noexcept { return data_; }

      constexpr const_iterator begin() const volatile noexcept { return data_; }

      constexpr const_iterator cbegin() const volatile noexcept { return data_; }

      constexpr iterator end() volatile noexcept { return data_ + N; }

      constexpr const_iterator end() const volatile noexcept { return data_ + N; }

      constexpr const_iterator cend() const volatile noexcept { return data_ + N; }

      constexpr reverse_iterator rbegin() volatile noexcept { return reverse_iterator(end()); }

      constexpr const_reverse_iterator rbegin() const volatile noexcept { return const_reverse_iterator(end()); }

      constexpr const_reverse_iterator crbegin() const volatile noexcept { return const_reverse_iterator(cend()); }

      constexpr reverse_iterator rend() volatile noexcept { return reverse_iterator(begin()); }

      constexpr const_reverse_iterator rend() const volatile noexcept { return const_reverse_iterator(begin()); }

      constexpr const_reverse_iterator crend() const volatile noexcept { return const_reverse_iterator(cbegin()); }

      constexpr bool empty() const volatile noexcept { return N == 0; }

      constexpr size_type size() const volatile noexcept { return N; }

      constexpr size_type max_size() const volatile noexcept { return N; }

      constexpr void fill(const T& value) volatile
      {
         for (size_type i = 0; i < N; ++i) {
            data_[i] = value;
         }
      }

      template <is_volatile_array Other>
      void swap(Other& other) volatile noexcept(std::is_nothrow_swappable_v<Other>)
      {
         for (size_type i = 0; i < N; ++i) {
            std::swap(data_[i], other.data_[i]);
         }
      }

      volatile T data_[N];
   };

   // Comparison operators
   template <is_volatile_array L, is_volatile_array R>
      requires(L::length == R::length)
   constexpr bool operator==(const L& lhs, const R& rhs) noexcept
   {
      for (std::size_t i = 0; i < L::length; ++i) {
         if (lhs[i] != rhs[i]) return false;
      }
      return true;
   }

   template <is_volatile_array L, is_volatile_array R>
      requires(L::length == R::length)
   constexpr bool operator!=(const L& lhs, const R& rhs) noexcept
   {
      return !(lhs == rhs);
   }

   template <is_volatile_array L, is_volatile_array R>
      requires(L::length == R::length)
   constexpr bool operator<(const L& lhs, const R& rhs) noexcept
   {
      for (std::size_t i = 0; i < L::length; ++i) {
         if (lhs[i] < rhs[i]) return true;
         if (rhs[i] < lhs[i]) return false;
      }
      return false;
   }

   template <is_volatile_array L, is_volatile_array R>
      requires(L::length == R::length)
   constexpr bool operator<=(const L& lhs, const R& rhs) noexcept
   {
      return !(rhs < lhs);
   }

   template <is_volatile_array L, is_volatile_array R>
      requires(L::length == R::length)
   constexpr bool operator>(const L& lhs, const R& rhs) noexcept
   {
      return rhs < lhs;
   }

   template <is_volatile_array L, is_volatile_array R>
      requires(L::length == R::length)
   constexpr bool operator>=(const L& lhs, const R& rhs) noexcept
   {
      return !(lhs < rhs);
   }
}
