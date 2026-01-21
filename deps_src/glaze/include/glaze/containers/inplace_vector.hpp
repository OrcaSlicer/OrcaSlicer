// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <new>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>

#ifndef GLZ_THROW_OR_ABORT
#if __cpp_exceptions
#define GLZ_THROW_OR_ABORT(EXC) (throw(EXC))
#define GLZ_NOEXCEPT noexcept(false)
#else
#include <cstdlib> // for std::abort
#define GLZ_THROW_OR_ABORT(EXC) (std::abort())
#define GLZ_NOEXCEPT noexcept(true)
#endif
#endif

namespace glz
{
   namespace detail::inplace_vector
   {
      template <class T, size_t N>
      struct byte_storage
      {
         alignas(T) unsigned char storage[sizeof(T) * N];
         size_t size_ = 0;
         constexpr T* data_ptr() noexcept { return std::launder(reinterpret_cast<T*>(storage)); }
         constexpr const T* data_ptr() const noexcept { return std::launder(reinterpret_cast<const T*>(storage)); }
         constexpr size_t storage_size() const noexcept { return size_; }
         constexpr void set_storage_size(size_t new_size) noexcept { size_ = new_size; }
      };

      template <class T>
      struct zero_storage
      {
         constexpr T* data_ptr() noexcept { return nullptr; }
         constexpr const T* data_ptr() const noexcept { return nullptr; }
         constexpr size_t storage_size() const noexcept { return 0; }
         constexpr void set_storage_size(size_t) noexcept {};
      };

      template <class T, size_t N>
      using storage_type = std::conditional_t<N == 0, zero_storage<T>, byte_storage<T, N>>;

      template <class T, size_t N>
      class inplace_vector_base : protected storage_type<T, N>
      {
        protected:
         using storage_t = storage_type<T, N>;

         using storage_t::data_ptr;
         using storage_t::set_storage_size;
         using storage_t::storage_size;

        public:
         using value_type = T;
         using pointer = T*;
         using const_pointer = const T*;
         using reference = value_type&;
         using const_reference = const value_type&;
         using size_type = size_t;
         using difference_type = std::ptrdiff_t;
         using iterator = T*;
         using const_iterator = const T*;
         using reverse_iterator = std::reverse_iterator<iterator>;
         using const_reverse_iterator = std::reverse_iterator<const_iterator>;

        protected:
         // Helper to destroy a range of elements
         constexpr void destroy_range(size_type start, size_type end) noexcept
         {
            if constexpr (!std::is_trivially_destructible_v<T>) {
               for (size_type i = start; i < end; ++i) {
                  std::destroy_at(this->data_ptr() + i);
               }
            }
         }

        public:
         // Constructors

         constexpr inplace_vector_base() noexcept = default;

         // Trivially copyable copy constructor
         constexpr inplace_vector_base(const inplace_vector_base& other)
            requires std::is_trivially_copyable_v<T>
         = default;

         // Non-trivially copyable copy constructor
         constexpr inplace_vector_base(const inplace_vector_base& other)
            requires(!std::is_trivially_copyable_v<T>)
         {
            assign(other.begin(), other.end());
         }

         // Trivially copyable move constructor
         constexpr inplace_vector_base(inplace_vector_base&& other) noexcept(N == 0 ||
                                                                             std::is_nothrow_move_constructible_v<T>)
            requires std::is_trivially_copyable_v<T>
         = default;

         // Non-trivially copyable move constructor
         constexpr inplace_vector_base(inplace_vector_base&& other) noexcept(N == 0 ||
                                                                             std::is_nothrow_move_constructible_v<T>)
            requires(!std::is_trivially_copyable_v<T>)
         {
            assign(std::make_move_iterator(other.begin()), std::make_move_iterator(other.end()));
            other.clear();
         }

         // Trivially destructible destructor
         constexpr ~inplace_vector_base()
            requires std::is_trivially_destructible_v<T>
         = default;

         // Non-trivially destructible destructor
         constexpr ~inplace_vector_base()
            requires(!std::is_trivially_destructible_v<T>)
         {
            destroy_range(0, storage_size());
         }

         // Trivially copyable copy assignment
         constexpr inplace_vector_base& operator=(const inplace_vector_base& other)
            requires std::is_trivially_copyable_v<T>
         = default;

         // Non-trivially copyable copy assignment
         constexpr inplace_vector_base& operator=(const inplace_vector_base& other)
            requires(!std::is_trivially_copyable_v<T>)
         {
            if (this != &other) {
               assign(other.begin(), other.end());
            }
            return *this;
         }

         // Trivially copyable move assignment
         constexpr inplace_vector_base& operator=(inplace_vector_base&& other) noexcept(
            N == 0 || (std::is_nothrow_move_assignable_v<T> && std::is_nothrow_move_constructible_v<T>))
            requires std::is_trivially_copyable_v<T>
         = default;

         // Non-trivially copyable move assignment
         constexpr inplace_vector_base& operator=(inplace_vector_base&& other) noexcept(
            N == 0 || (std::is_nothrow_move_assignable_v<T> && std::is_nothrow_move_constructible_v<T>))
            requires(!std::is_trivially_copyable_v<T>)
         {
            if (this != &other) {
               clear();
               assign(std::make_move_iterator(other.begin()), std::make_move_iterator(other.end()));
               other.clear();
            }
            return *this;
         }

         // Iterators
         constexpr iterator begin() noexcept { return this->data_ptr(); }

         constexpr const_iterator begin() const noexcept { return this->data_ptr(); }

         constexpr iterator end() noexcept { return this->data_ptr() + storage_size(); }

         constexpr const_iterator end() const noexcept { return this->data_ptr() + storage_size(); }

         constexpr reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }

         constexpr const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }

         constexpr reverse_iterator rend() noexcept { return reverse_iterator(begin()); }

         constexpr const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }

         constexpr const_iterator cbegin() const noexcept { return begin(); }

         constexpr const_iterator cend() const noexcept { return end(); }

         constexpr const_reverse_iterator crbegin() const noexcept { return rbegin(); }

         constexpr const_reverse_iterator crend() const noexcept { return rend(); }

         // Capacity
         constexpr bool empty() const noexcept { return storage_size() == 0; }

         constexpr size_type size() const noexcept { return storage_size(); }

         static constexpr size_type max_size() noexcept { return N; }

         static constexpr size_type capacity() noexcept { return N; }

         static constexpr void shrink_to_fit() noexcept
         {
            // No-op for inplace_vector since capacity is fixed
         }

         // Element access
         constexpr reference operator[](size_type n) { return this->data_ptr()[n]; }

         constexpr const_reference operator[](size_type n) const { return this->data_ptr()[n]; }

         constexpr reference front() { return this->data_ptr()[0]; }

         constexpr const_reference front() const { return this->data_ptr()[0]; }

         constexpr reference back() { return this->data_ptr()[storage_size() - 1]; }

         constexpr const_reference back() const { return this->data_ptr()[storage_size() - 1]; }

         constexpr T* data() noexcept { return this->data_ptr(); }

         constexpr const T* data() const noexcept { return this->data_ptr(); }

         // Standard modifiers
         constexpr void pop_back()
         {
            std::destroy_at(&back());
            set_storage_size(storage_size() - 1);
         }

         // Fallible APIs
         template <class... Args>
         constexpr T* try_emplace_back(Args&&... args)
            requires(std::constructible_from<T, Args...>)
         {
            if (storage_size() >= N) return nullptr;

            std::construct_at(this->data_ptr() + storage_size(), std::forward<Args>(args)...);
            set_storage_size(storage_size() + 1);
            return &back();
         }

         constexpr T* try_push_back(const T& x)
         {
            if (storage_size() >= N) return nullptr;

            if constexpr (std::is_trivially_copyable_v<T>) {
               std::memcpy(this->data_ptr() + storage_size(), &x, sizeof(T));
            }
            else {
               std::construct_at(this->data_ptr() + storage_size(), x);
            }
            set_storage_size(storage_size() + 1);
            return &back();
         }

         constexpr T* try_push_back(T&& x)
         {
            if (storage_size() >= N) return nullptr;

            std::construct_at(this->data_ptr() + storage_size(), std::move(x));
            set_storage_size(storage_size() + 1);
            return &back();
         }

         template <std::ranges::input_range R>
            requires std::convertible_to<std::ranges::range_reference_t<R>, T>
         constexpr auto try_append_range(R&& rg) -> std::ranges::borrowed_iterator_t<R>
         {
            auto it = std::ranges::begin(rg);
            const auto end = std::ranges::end(rg);
            for (; storage_size() != N && it != end; ++it) {
               try_emplace_back(*it);
            }
            return it;
         }

         // Unchecked APIs
         template <class... Args>
         constexpr reference unchecked_emplace_back(Args&&... args)
         {
            return *try_emplace_back(std::forward<Args>(args)...);
         }

         constexpr reference unchecked_push_back(const T& x) { return *try_push_back(x); }

         constexpr reference unchecked_push_back(T&& x) { return *try_push_back(std::move(x)); }

         // Erase operations
         constexpr iterator erase(const_iterator position)
         {
            size_type pos_idx = position - cbegin();

            // Destroy element at position
            std::destroy_at(this->data_ptr() + pos_idx);

            if constexpr (std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>) {
               if (pos_idx < storage_size() - 1) {
                  std::memmove(this->data_ptr() + pos_idx, this->data_ptr() + pos_idx + 1,
                               (storage_size() - pos_idx - 1) * sizeof(T));
               }
            }
            else {
               // Move elements after position one position to the left
               for (size_type i = pos_idx; i < storage_size() - 1; ++i)
                  this->data_ptr()[i] = std::move(this->data_ptr()[i + 1]);

               // Destroy the last element that was moved from
               std::destroy_at(this->data_ptr() + storage_size() - 1);
            }

            set_storage_size(storage_size() - 1);

            return begin() + pos_idx;
         }

         constexpr iterator erase(const_iterator first, const_iterator last)
         {
            size_type first_idx = first - cbegin();
            size_type last_idx = last - cbegin();
            size_type count = last_idx - first_idx;

            if (count == 0) return begin() + first_idx;

            // Destroy elements in range [first, last)
            for (size_type i = first_idx; i < last_idx; ++i) std::destroy_at(this->data_ptr() + i);

            if constexpr (std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>) {
               if (last_idx < storage_size()) {
                  std::memmove(this->data_ptr() + first_idx, this->data_ptr() + last_idx,
                               (storage_size() - last_idx) * sizeof(T));
               }
            }
            else {
               // Move elements after last count positions to the left
               for (size_type i = first_idx; i < storage_size() - count; ++i)
                  this->data_ptr()[i] = std::move(this->data_ptr()[i + count]);

               // Destroy the last elements that were moved from
               for (size_type i = storage_size() - count; i < storage_size(); ++i)
                  std::destroy_at(this->data_ptr() + i);
            }

            set_storage_size(storage_size() - count);

            return begin() + first_idx;
         }

         constexpr void clear() noexcept
         {
            destroy_range(0, storage_size());
            set_storage_size(0);
         }

         constexpr void swap(inplace_vector_base& x) noexcept(N == 0 || (std::is_nothrow_swappable_v<T> &&
                                                                         std::is_nothrow_move_constructible_v<T>))
         {
            if (this == &x) return;

            if constexpr (std::is_trivially_copyable_v<T>) {
               // For trivially copyable types, we can use a temporary buffer
               alignas(T) unsigned char temp[sizeof(T) * N];
               std::memcpy(temp, data_ptr(), storage_size() * sizeof(T));
               std::memcpy(data_ptr(), x.data_ptr(), x.storage_size() * sizeof(T));
               std::memcpy(x.data_ptr(), temp, storage_size() * sizeof(T));
               size_type temp_size = storage_size();
               set_storage_size(x.storage_size());
               x.set_storage_size(temp_size);
            }
            else {
               const auto min_size = std::min(storage_size(), x.storage_size());

               // Swap common elements
               for (size_type i = 0; i < min_size; ++i) std::swap(this->data_ptr()[i], x.data_ptr()[i]);

               // Handle the case where sizes are different
               if (storage_size() > x.storage_size()) {
                  // Move our excess elements to x
                  for (size_type i = min_size; i < storage_size(); ++i)
                     std::construct_at(x.data_ptr() + i, std::move(this->data_ptr()[i]));

                  // Destroy our excess elements
                  for (size_type i = min_size; i < storage_size(); ++i) std::destroy_at(this->data_ptr() + i);
               }
               else if (storage_size() < x.storage_size()) {
                  // Move x's excess elements to us
                  for (size_type i = min_size; i < x.storage_size(); ++i)
                     std::construct_at(this->data_ptr() + i, std::move(x.data_ptr()[i]));

                  // Destroy x's excess elements
                  for (size_type i = min_size; i < x.storage_size(); ++i) std::destroy_at(x.data_ptr() + i);
               }

               // Swap sizes
               std::swap(storage_size(), x.storage_size());
            }
         }

         // Comparison operators
         friend constexpr bool operator==(const inplace_vector_base& lhs, const inplace_vector_base& rhs)
         {
            if constexpr (N == 0) {
               return true; // Both are inplace_vector<T, 0>, always empty and thus equal.
            }
            else {
               if (lhs.storage_size() != rhs.storage_size()) return false;

               if constexpr (std::is_trivially_copyable_v<T>) {
                  return std::memcmp(lhs.data_ptr(), rhs.data_ptr(), lhs.storage_size() * sizeof(T)) == 0;
               }
               else {
                  return std::equal(lhs.begin(), lhs.end(), rhs.begin());
               }
            }
         }

         friend constexpr auto operator<=>(const inplace_vector_base& lhs, const inplace_vector_base& rhs)
         {
            return std::lexicographical_compare_three_way(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                                                          std::compare_three_way{});
         }
      };

   }

   // Non-member functions
   template <class T, size_t N>
   constexpr void swap(detail::inplace_vector::inplace_vector_base<T, N>& x,
                       detail::inplace_vector::inplace_vector_base<T, N>& y) noexcept(noexcept(x.swap(y)))
   {
      x.swap(y);
   }

   template <class T, size_t N, class U>
   constexpr typename detail::inplace_vector::inplace_vector_base<T, N>::size_type erase(
      detail::inplace_vector::inplace_vector_base<T, N>& c, const U& value)
   {
      auto it = std::remove(c.begin(), c.end(), value);
      auto r = c.end() - it;
      c.erase(it, c.end());
      return r;
   }

   template <class T, size_t N, class Predicate>
   constexpr typename detail::inplace_vector::inplace_vector_base<T, N>::size_type erase_if(
      detail::inplace_vector::inplace_vector_base<T, N>& c, Predicate pred)
   {
      auto it = std::remove_if(c.begin(), c.end(), pred);
      auto r = c.end() - it;
      c.erase(it, c.end());
      return r;
   }

   template <class T, size_t N>
   class inplace_vector : public detail::inplace_vector::inplace_vector_base<T, N>
   {
     public:
      // types
      using value_type = T;
      using pointer = T*;
      using const_pointer = const T*;
      using reference = value_type&;
      using const_reference = const value_type&;
      using size_type = size_t;
      using difference_type = std::ptrdiff_t;
      using iterator = T*;
      using const_iterator = const T*;
      using reverse_iterator = std::reverse_iterator<iterator>;
      using const_reverse_iterator = std::reverse_iterator<const_iterator>;

     public:
      // Constructors
      constexpr inplace_vector() noexcept = default;

      constexpr explicit inplace_vector(size_type n) // freestanding-deleted
      {
         if (n > N) GLZ_THROW_OR_ABORT(std::bad_alloc());

         // Use std::uninitialized_value_construct_n for proper value-initialization
         std::uninitialized_value_construct_n(this->data_ptr(), n);
         this->set_storage_size(n);
      }

      constexpr inplace_vector(size_type n, const T& value) // freestanding-deleted
      {
         if (n > N) GLZ_THROW_OR_ABORT(std::bad_alloc());

         if constexpr (std::is_trivially_copyable_v<T>) {
            // Construct first element then copy it
            if (n > 0) {
               std::construct_at(this->data_ptr(), value);
               for (size_type i = 1; i < n; ++i) {
                  std::memcpy(this->data_ptr() + i, this->data_ptr(), sizeof(T));
               }
            }
         }
         else {
            for (size_type i = 0; i < n; ++i) std::construct_at(this->data_ptr() + i, value);
         }

         this->set_storage_size(n);
      }

      template <class InputIterator>
      constexpr inplace_vector(InputIterator first, InputIterator last) // freestanding-deleted
      {
         assign(first, last);
      }

#ifdef __cpp_lib_containers_ranges
      template <std::ranges::input_range R>
         requires std::convertible_to<std::ranges::range_reference_t<R>, T>
      constexpr inplace_vector(std::from_range_t, R&& rg) // freestanding-deleted
      {
         assign_range(std::forward<R>(rg));
      }
#endif

      constexpr inplace_vector(std::initializer_list<T> il) // freestanding-deleted
      {
         assign(il.begin(), il.end());
      }
      constexpr inplace_vector& operator=(std::initializer_list<T> il) // freestanding-deleted
      {
         assign(il.begin(), il.end());
         return *this;
      }

      // Assign methods
      template <std::input_iterator InputIterator>
      constexpr void assign(InputIterator first, InputIterator last) // freestanding-deleted
      {
         this->clear();

         // Calculate distance if possible (for random access iterators)
         if constexpr (std::random_access_iterator<InputIterator>) {
            if (std::distance(first, last) > static_cast<difference_type>(N)) GLZ_THROW_OR_ABORT(std::bad_alloc());
         }

         if constexpr (std::is_trivially_copyable_v<T> && std::contiguous_iterator<InputIterator>) {
            // Fast path for contiguous trivially copyable types
            auto count = std::distance(first, last);
            if (count > 0) {
               std::memcpy(this->storage, std::to_address(first), count * sizeof(T));
               this->set_storage_size(static_cast<size_type>(count));
            }
         }
         else {
            // General path
            while (first != last) {
               if (this->storage_size() >= N) GLZ_THROW_OR_ABORT(std::bad_alloc());

               std::construct_at(this->data_ptr() + this->storage_size(), *first);
               this->set_storage_size(this->storage_size() + 1);
               ++first;
            }
         }
      }

      template <std::ranges::input_range R>
         requires std::convertible_to<std::ranges::range_reference_t<R>, T>
      constexpr void assign_range(R&& rg) // freestanding-deleted
      {
         this->clear();

         // Check size if possible
         if constexpr (std::ranges::sized_range<R>) {
            if (std::ranges::size(rg) > N) GLZ_THROW_OR_ABORT(std::bad_alloc());
         }

         // Fast path for contiguous ranges of trivially copyable types
         if constexpr (std::is_trivially_copyable_v<T> && std::ranges::contiguous_range<R> &&
                       std::same_as<std::remove_cvref_t<std::ranges::range_reference_t<R>>, T>) {
            auto count = std::ranges::size(rg);
            if (count > 0) {
               std::memcpy(this->storage, std::ranges::data(rg), count * sizeof(T));
               this->set_storage_size(static_cast<size_type>(count));
            }
         }
         else {
            // General path
            for (auto&& item : rg) {
               if (this->storage_size() >= N) GLZ_THROW_OR_ABORT(std::bad_alloc());

               std::construct_at(this->data_ptr() + this->storage_size(), std::forward<decltype(item)>(item));
               this->set_storage_size(this->storage_size() + 1);
            }
         }
      }

      constexpr void assign(size_type n, const T& value) // freestanding-deleted
      {
         if (n > N) GLZ_THROW_OR_ABORT(std::bad_alloc());

         this->clear();

         if constexpr (std::is_trivially_copyable_v<T>) {
            if (n > 0) {
               // Construct first element, then use memcpy for the rest
               std::construct_at(this->data_ptr(), value);
               for (size_type i = 1; i < n; ++i) {
                  std::memcpy(this->data_ptr() + i, this->data_ptr(), sizeof(T));
               }
               this->set_storage_size(n);
            }
         }
         else {
            for (size_type i = 0; i < n; ++i) {
               std::construct_at(this->data_ptr() + this->storage_size(), value);
               this->set_storage_size(this->storage_size() + 1);
            }
         }
      }

      // TODO add assign(initializer_list<T>)

      // Capacity

      constexpr void resize(size_type sz) // freestanding-deleted
      {
         if (sz > N) GLZ_THROW_OR_ABORT(std::bad_alloc());

         if (sz > this->storage_size()) {
            // Value-initialize the new elements
            std::uninitialized_value_construct_n(this->data_ptr() + this->storage_size(), sz - this->storage_size());
            this->set_storage_size(sz);
         }
         else if (sz < this->storage_size()) {
            // Destroy excess elements
            this->destroy_range(sz, this->storage_size());
            this->set_storage_size(sz);
         }
      }

      constexpr void resize(size_type sz, const T& c) // freestanding-deleted
      {
         if (sz > N) GLZ_THROW_OR_ABORT(std::bad_alloc());

         if (sz > this->storage_size()) {
            if constexpr (std::is_trivially_copyable_v<T>) {
               // For trivially copyable types, construct one and copy the rest
               if (this->storage_size() < sz) {
                  if (this->storage_size() == 0 && sz > 0) {
                     std::construct_at(this->data_ptr(), c);
                     for (size_type i = 1; i < sz; ++i) {
                        std::memcpy(this->data_ptr() + i, this->data_ptr(), sizeof(T));
                     }
                  }
                  else {
                     for (size_type i = this->storage_size(); i < sz; ++i) {
                        std::memcpy(this->data_ptr() + i, &c, sizeof(T));
                     }
                  }
               }
            }
            else {
               // Expand and copy-construct new elements
               for (size_type i = this->storage_size(); i < sz; ++i) std::construct_at(this->data_ptr() + i, c);
            }
            this->set_storage_size(sz);
         }
         else if (sz < this->storage_size()) {
            // Destroy excess elements
            this->destroy_range(sz, this->storage_size());
            this->set_storage_size(sz);
         }
      }

      static constexpr void reserve(size_type n) // freestanding-deleted
      {
         if (n > N) GLZ_THROW_OR_ABORT(std::bad_alloc());
         // No-op otherwise since capacity is fixed
      }

      // Element access

      constexpr reference at(size_type n) // freestanding-deleted
      {
         if (n >= this->storage_size()) GLZ_THROW_OR_ABORT(std::out_of_range("inplace_vector::at: index out of range"));
         return this->data_ptr()[n];
      }

      constexpr const_reference at(size_type n) const // freestanding-deleted
      {
         if (n >= this->storage_size()) GLZ_THROW_OR_ABORT(std::out_of_range("inplace_vector::at: index out of range"));
         return this->data_ptr()[n];
      }

      // Standard modifiers
      template <class... Args>
      constexpr reference emplace_back(Args&&... args) // freestanding-deleted
      {
         if (this->storage_size() >= N) GLZ_THROW_OR_ABORT(std::bad_alloc());
         return this->unchecked_emplace_back(std::forward<Args>(args)...);
      }

      constexpr reference push_back(const T& x) // freestanding-deleted
      {
         if (this->storage_size() >= N) GLZ_THROW_OR_ABORT(std::bad_alloc());
         return this->unchecked_emplace_back(x);
      }

      constexpr reference push_back(T&& x) // freestanding-deleted
      {
         return emplace_back(std::move(x));
      }

      template <std::ranges::input_range R>
         requires std::convertible_to<std::ranges::range_reference_t<R>, T>
      constexpr void append_range(R&& rg) // freestanding-deleted
      {
         // Check size if possible
         if constexpr (std::ranges::sized_range<R>) {
            if (this->storage_size() + std::ranges::size(rg) > N) GLZ_THROW_OR_ABORT(std::bad_alloc());
         }

         for (auto&& item : rg) {
            emplace_back(std::forward<decltype(item)>(item));
         }
      }

      // Insert operations
      template <class... Args>
      constexpr iterator emplace(const_iterator position, Args&&... args) // freestanding-deleted
      {
         if (this->storage_size() >= N) GLZ_THROW_OR_ABORT(std::bad_alloc());

         // Calculate position index
         size_type pos_idx = position - this->cbegin();

         if (pos_idx == this->storage_size()) {
            // Fast path: insert at end
            std::construct_at(this->data_ptr() + this->storage_size(), std::forward<Args>(args)...);
            this->set_storage_size(this->storage_size() + 1);
            return this->begin() + pos_idx;
         }

         // Move elements after position one position to the right
         if constexpr (std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>) {
            if (this->storage_size() > pos_idx) {
               std::memmove(this->data_ptr() + pos_idx + 1, this->data_ptr() + pos_idx,
                            (this->storage_size() - pos_idx) * sizeof(T));
            }
         }
         else {
            // Construct a new element at the end
            std::construct_at(this->data_ptr() + this->storage_size(),
                              std::move(this->data_ptr()[this->storage_size() - 1]));

            // Move each element one position to the right, starting from the end
            for (size_type i = this->storage_size() - 1; i > pos_idx; --i) {
               this->data_ptr()[i] = std::move(this->data_ptr()[i - 1]);
            }

            // Destroy the element at position
            std::destroy_at(this->data_ptr() + pos_idx);
         }

         // Construct new element
         std::construct_at(this->data_ptr() + pos_idx, std::forward<Args>(args)...);
         this->set_storage_size(this->storage_size() + 1);

         return this->begin() + pos_idx;
      }

      constexpr iterator insert(const_iterator position, const T& x) // freestanding-deleted
      {
         return emplace(position, x);
      }

      constexpr iterator insert(const_iterator position, T&& x) // freestanding-deleted
      {
         return emplace(position, std::move(x));
      }

      constexpr iterator insert(const_iterator position, size_type n, const T& x) // freestanding-deleted
      {
         if (this->storage_size() + n > N) GLZ_THROW_OR_ABORT(std::bad_alloc());

         if (n == 0) return this->begin() + (position - this->cbegin());

         // Calculate position index
         size_type pos_idx = position - this->cbegin();

         if constexpr (std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>) {
            // Move existing elements to make space
            if (pos_idx < this->storage_size()) {
               std::memmove(this->data_ptr() + pos_idx + n, this->data_ptr() + pos_idx,
                            (this->storage_size() - pos_idx) * sizeof(T));
            }

            // Copy the new elements
            for (size_type i = 0; i < n; ++i) {
               if constexpr (std::is_trivially_copyable_v<T>) {
                  std::memcpy(this->data_ptr() + pos_idx + i, &x, sizeof(T));
               }
               else {
                  std::construct_at(this->data_ptr() + pos_idx + i, x);
               }
            }
         }
         else {
            // General case for non-trivial types
            // First, move elements at the end to their new positions
            for (size_type i = 0; i < std::min(n, this->storage_size() - pos_idx); ++i) {
               std::construct_at(this->data_ptr() + this->storage_size() + n - 1 - i,
                                 std::move(this->data_ptr()[this->storage_size() - 1 - i]));
            }

            // Move the remaining elements that need to be shifted but not constructed
            for (size_type i = this->storage_size() - std::min(n, this->storage_size() - pos_idx) - 1;
                 i >= pos_idx && i < this->storage_size(); --i) {
               this->data_ptr()[i + n] = std::move(this->data_ptr()[i]);
            }

            // Destroy elements that will be overwritten
            for (size_type i = pos_idx; i < std::min(pos_idx + n, this->storage_size()); ++i) {
               std::destroy_at(this->data_ptr() + i);
            }

            // Construct new elements
            for (size_type i = 0; i < n; ++i) {
               std::construct_at(this->data_ptr() + pos_idx + i, x);
            }
         }

         this->set_storage_size(this->storage_size() + n);
         return this->begin() + pos_idx;
      }

      template <class InputIterator>
      constexpr iterator insert(const_iterator position, InputIterator first,
                                InputIterator last) // freestanding-deleted
      {
         // Calculate position index
         size_type pos_idx = position - this->cbegin();

         // For random access iterators, we can check size upfront
         if constexpr (std::random_access_iterator<InputIterator>) {
            auto count = std::distance(first, last);
            if (this->storage_size() + count > N) GLZ_THROW_OR_ABORT(std::bad_alloc());

            if (count == 0) return this->begin() + pos_idx;

            if constexpr (std::is_trivially_copyable_v<T> && std::contiguous_iterator<InputIterator> &&
                          std::is_trivially_destructible_v<T> &&
                          std::same_as<std::remove_cvref_t<std::iter_reference_t<InputIterator>>, T>) {
               // Move existing elements to make space
               if (pos_idx < this->storage_size()) {
                  std::memmove(this->data_ptr() + pos_idx + count, this->data_ptr() + pos_idx,
                               (this->storage_size() - pos_idx) * sizeof(T));
               }

               // Copy the new elements directly
               std::memcpy(this->data_ptr() + pos_idx, std::to_address(first), count * sizeof(T));
               this->set_storage_size(this->storage_size() + count);
            }
            else {
               // General implementation
               // Make space first
               if (pos_idx < this->storage_size()) {
                  // Move elements after position count positions to the right
                  for (size_type i = 0; i < std::min(count, this->storage_size() - pos_idx); ++i) {
                     std::construct_at(this->data_ptr() + this->storage_size() + count - 1 - i,
                                       std::move(this->data_ptr()[this->storage_size() - 1 - i]));
                  }

                  // Move the remaining elements
                  for (size_type i = this->storage_size() - std::min(count, this->storage_size() - pos_idx) - 1;
                       i >= pos_idx && i < this->storage_size(); --i) {
                     this->data_ptr()[i + count] = std::move(this->data_ptr()[i]);
                  }

                  // Destroy elements that will be overwritten
                  for (size_type i = pos_idx; i < std::min(pos_idx + count, this->storage_size()); ++i) {
                     std::destroy_at(this->data_ptr() + i);
                  }
               }

               // Insert new elements
               for (size_type i = 0; i < count; ++i, ++first) {
                  std::construct_at(this->data_ptr() + pos_idx + i, *first);
               }

               this->set_storage_size(this->storage_size() + count);
            }
         }
         else {
            // For non-random access iterators, copy to temporary buffer first
            inplace_vector<T, N> temp(first, last);

            if (this->storage_size() + temp.size() > N) GLZ_THROW_OR_ABORT(std::bad_alloc());

            insert(position, temp.begin(), temp.end());
         }

         return this->begin() + pos_idx;
      }

      template <std::ranges::input_range R>
         requires std::convertible_to<std::ranges::range_reference_t<R>, T>
      constexpr iterator insert_range(const_iterator position, R&& rg) // freestanding-deleted
      {
         // Calculate position index
         size_type pos_idx = position - this->cbegin();

         if constexpr (std::ranges::sized_range<R>) {
            auto count = std::ranges::size(rg);
            if (this->storage_size() + count > N) GLZ_THROW_OR_ABORT(std::bad_alloc());

            if (count == 0) return this->begin() + pos_idx;

            if constexpr (std::is_trivially_copyable_v<T> && std::ranges::contiguous_range<R> &&
                          std::is_trivially_destructible_v<T> &&
                          std::same_as<std::remove_cvref_t<std::ranges::range_reference_t<R>>, T>) {
               // Move existing elements to make space
               if (pos_idx < this->storage_size()) {
                  std::memmove(this->data_ptr() + pos_idx + count, this->data_ptr() + pos_idx,
                               (this->storage_size() - pos_idx) * sizeof(T));
               }

               // Copy the new elements directly
               std::memcpy(this->data_ptr() + pos_idx, std::ranges::data(rg), count * sizeof(T));
               this->set_storage_size(this->storage_size() + count);
            }
            else {
               // General implementation
               // Make space first
               if (pos_idx < this->storage_size()) {
                  // Move elements after position count positions to the right
                  for (size_type i = 0; i < std::min(count, this->storage_size() - pos_idx); ++i) {
                     std::construct_at(this->data_ptr() + this->storage_size() + count - 1 - i,
                                       std::move(this->data_ptr()[this->storage_size() - 1 - i]));
                  }

                  // Move the remaining elements
                  for (size_type i = this->storage_size() - std::min(count, this->storage_size() - pos_idx) - 1;
                       i >= pos_idx && i < this->storage_size(); --i) {
                     this->data_ptr()[i + count] = std::move(this->data_ptr()[i]);
                  }

                  // Destroy elements that will be overwritten
                  for (size_type i = pos_idx; i < std::min(pos_idx + count, this->storage_size()); ++i) {
                     std::destroy_at(this->data_ptr() + i);
                  }
               }

               // Insert new elements
               size_type i = 0;
               for (auto&& item : rg) {
                  std::construct_at(this->data_ptr() + pos_idx + i, std::forward<decltype(item)>(item));
                  ++i;
               }

               this->set_storage_size(this->storage_size() + count);
            }
         }
         else {
            // For non-sized ranges, convert to temporary buffer first
#ifdef __cpp_lib_containers_ranges
            inplace_vector<T, N> temp(std::from_range, std::forward<R>(rg));
#else
            inplace_vector<T, N> temp;
            for (auto&& item : rg) {
               temp.push_back(std::forward<decltype(item)>(item));
            }
#endif

            if (this->storage_size() + temp.size() > N) GLZ_THROW_OR_ABORT(std::bad_alloc());

            insert(position, temp.begin(), temp.end());
         }

         return this->begin() + pos_idx;
      }

      constexpr iterator insert(const_iterator position, std::initializer_list<T> il) // freestanding-deleted
      {
         return insert(position, il.begin(), il.end());
      }
   };

   namespace freestanding
   {
      template <class T, size_t N>
      class inplace_vector : public detail::inplace_vector::inplace_vector_base<T, N>
      {
        public:
         // types
         using value_type = T;
         using pointer = T*;
         using const_pointer = const T*;
         using reference = value_type&;
         using const_reference = const value_type&;
         using size_type = size_t;
         using difference_type = std::ptrdiff_t;
         using iterator = T*;
         using const_iterator = const T*;
         using reverse_iterator = std::reverse_iterator<iterator>;
         using const_reverse_iterator = std::reverse_iterator<const_iterator>;

        public:

        public:
         // Constructors

         constexpr inplace_vector() noexcept = default;

         constexpr explicit inplace_vector(size_type n) // freestanding-deleted
            = delete;

         constexpr inplace_vector(size_type n, const T& value) // freestanding-deleted
            = delete;

         template <class InputIterator>
         constexpr inplace_vector(InputIterator first, InputIterator last) // freestanding-deleted
            = delete;

#ifdef __cpp_lib_containers_ranges
         template <std::ranges::input_range R>
            requires std::convertible_to<std::ranges::range_reference_t<R>, T>
         constexpr inplace_vector(std::from_range_t, R&& rg) // freestanding-deleted
            = delete;
#endif

         constexpr inplace_vector(std::initializer_list<T> il) // freestanding-deleted
            = delete;
         constexpr inplace_vector& operator=(std::initializer_list<T> il) // freestanding-deleted
            = delete;

         // Assign methods
         template <std::input_iterator InputIterator>
         constexpr void assign(InputIterator first, InputIterator last) // freestanding-deleted
            = delete;

         template <std::ranges::input_range R>
            requires std::convertible_to<std::ranges::range_reference_t<R>, T>
         constexpr void assign_range(R&& rg) // freestanding-deleted
            = delete;

         constexpr void assign(size_type n, const T& value) // freestanding-deleted
            = delete;

         // TODO add assign(initializer_list<T>)

         // Capacity

         constexpr void resize(size_type sz) // freestanding-deleted
            = delete;

         constexpr void resize(size_type sz, const T& c) // freestanding-deleted
            = delete;

         static constexpr void reserve(size_type n) // freestanding-deleted
            = delete;

         // Element access

         constexpr reference at(size_type n) // freestanding-deleted
            = delete;

         constexpr const_reference at(size_type n) const // freestanding-deleted
            = delete;

         // Standard modifiers
         template <class... Args>
         constexpr reference emplace_back(Args&&... args) // freestanding-deleted
            = delete;

         constexpr reference push_back(const T& x) // freestanding-deleted
            = delete;

         constexpr reference push_back(T&& x) // freestanding-deleted
            = delete;

         template <std::ranges::input_range R>
            requires std::convertible_to<std::ranges::range_reference_t<R>, T>
         constexpr void append_range(R&& rg) // freestanding-deleted
            = delete;

         // Insert operations
         template <class... Args>
         constexpr iterator emplace(const_iterator position, Args&&... args) // freestanding-deleted
            = delete;

         constexpr iterator insert(const_iterator position, const T& x) // freestanding-deleted
            = delete;

         constexpr iterator insert(const_iterator position, T&& x) // freestanding-deleted
            = delete;

         constexpr iterator insert(const_iterator position, size_type n, const T& x) // freestanding-deleted
            = delete;

         template <class InputIterator>
         constexpr iterator insert(const_iterator position, InputIterator first,
                                   InputIterator last) // freestanding-deleted
            = delete;

         template <std::ranges::input_range R>
            requires std::convertible_to<std::ranges::range_reference_t<R>, T>
         constexpr iterator insert_range(const_iterator position, R&& rg) // freestanding-deleted
            = delete;

         constexpr iterator insert(const_iterator position, std::initializer_list<T> il) // freestanding-deleted
            = delete;
      };

   }
}
