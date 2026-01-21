// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <algorithm>
#include <concepts>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <vector>

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

// A flat_map. This version uses a single container for key/value pairs for the sake of cache locality. The
// std::flat_map uses separate key/value arrays.

namespace glz
{
   template <typename K1, typename K2>
   concept KeyEqualComparable = requires(const K1& k1, const K2& k2) {
      { k1 == k2 } -> std::convertible_to<bool>;
      { k2 == k1 } -> std::convertible_to<bool>;
   };

   template <class Key, class T, class Compare = std::less<>, class Container = std::vector<std::pair<Key, T>>>
   struct flat_map
   {
      // Member types
      using key_type = Key;
      using mapped_type = T;
      using value_type = std::pair<Key, T>;
      using key_compare = Compare;
      using size_type = typename Container::size_type;
      using difference_type = typename Container::difference_type;
      using reference = value_type&;
      using const_reference = const value_type&;
      using iterator = typename Container::iterator;
      using const_iterator = typename Container::const_iterator;
      using reverse_iterator = typename Container::reverse_iterator;
      using const_reverse_iterator = typename Container::const_reverse_iterator;

     private:
      Container data_;
      key_compare comp_;

      template <typename Comp>
      static constexpr bool IsDefaultComparator = std::same_as<Comp, std::less<>> || std::same_as<Comp, std::less<Key>>;

     public:
      // Constructors
      flat_map() : data_(), comp_() {}

      explicit flat_map(const Compare& comp) : data_(), comp_(comp) {}

      template <class InputIt>
      flat_map(InputIt first, InputIt last, const Compare& comp = Compare()) : data_(), comp_(comp)
      {
         insert(first, last);
      }

      flat_map(std::initializer_list<value_type> init, const Compare& comp = Compare()) : data_(), comp_(comp)
      {
         insert(init.begin(), init.end());
      }

      // Copy and Move Constructors
      flat_map(const flat_map& other) = default;
      flat_map(flat_map&& other) noexcept = default;

      // Assignment Operators
      flat_map& operator=(const flat_map& other) = default;
      flat_map& operator=(flat_map&& other) noexcept = default;

      // Iterators
      iterator begin() noexcept { return data_.begin(); }
      const_iterator begin() const noexcept { return data_.begin(); }
      const_iterator cbegin() const noexcept { return data_.cbegin(); }

      iterator end() noexcept { return data_.end(); }
      const_iterator end() const noexcept { return data_.end(); }
      const_iterator cend() const noexcept { return data_.cend(); }

      // Capacity
      [[nodiscard]] bool empty() const noexcept { return data_.empty(); }
      size_type size() const noexcept { return data_.size(); }
      void reserve(size_type new_cap) { data_.reserve(new_cap); }
      size_type capacity() const noexcept { return data_.capacity(); }
      void shrink_to_fit() { data_.shrink_to_fit(); }

      // Modifiers
      void clear() noexcept { data_.clear(); }

      std::pair<iterator, bool> insert(const value_type& value)
      {
         auto it = lower_bound(value.first);
         if (it != data_.end()) {
            if constexpr (IsDefaultComparator<Compare> &&
                          KeyEqualComparable<decltype(value.first), decltype(it->first)>) {
               if (value.first == it->first) {
                  return {it, false};
               }
            }
            else {
               if (!comp_(value.first, it->first) && !comp_(it->first, value.first)) {
                  return {it, false};
               }
            }
         }
         return {data_.insert(it, value), true};
      }

      std::pair<iterator, bool> insert(value_type&& value)
      {
         auto it = lower_bound(value.first);
         if (it != data_.end()) {
            if constexpr (IsDefaultComparator<Compare> &&
                          KeyEqualComparable<decltype(value.first), decltype(it->first)>) {
               if (value.first == it->first) {
                  return {it, false};
               }
            }
            else {
               if (!comp_(value.first, it->first) && !comp_(it->first, value.first)) {
                  return {it, false};
               }
            }
         }
         return {data_.emplace(it, std::move(value)), true};
      }

      template <class InputIt>
      void insert(InputIt first, InputIt last)
      {
         if (first == last) return;
         data_.reserve(data_.size() + std::distance(first, last));
         for (; first != last; ++first) {
            insert(*first);
         }
      }

      void insert(std::initializer_list<value_type> ilist) { insert(ilist.begin(), ilist.end()); }

      template <class... Args>
      std::pair<iterator, bool> emplace(Args&&... args)
      {
         value_type value(std::forward<Args>(args)...);
         return insert(std::move(value));
      }

      iterator erase(const_iterator pos) { return data_.erase(pos); }

      iterator erase(const_iterator first, const_iterator last) { return data_.erase(first, last); }

      template <typename K>
      size_type erase(const K& key)
      {
         auto it = find(key);
         if (it != end()) {
            data_.erase(it);
            return 1;
         }
         return 0;
      }

      void swap(flat_map& other) noexcept(
         std::allocator_traits<typename Container::allocator_type>::is_always_equal::value &&
         std::is_nothrow_swappable_v<Compare>)
      {
         data_.swap(other.data_);
         std::swap(comp_, other.comp_);
      }

      // Lookup
      template <typename K>
      size_type count(const K& key) const
      {
         return find(key) != end() ? 1 : 0;
      }

      template <typename K>
      iterator find(const K& key)
      {
         auto it = lower_bound(key);
         if (it != data_.end()) {
            if constexpr (IsDefaultComparator<Compare> && KeyEqualComparable<K, decltype(it->first)>) {
               if (key == it->first) {
                  return it;
               }
            }
            else {
               if (!comp_(key, it->first) && !comp_(it->first, key)) {
                  return it;
               }
            }
         }
         return data_.end();
      }

      template <typename K>
      const_iterator find(const K& key) const
      {
         auto it = lower_bound(key);
         if (it != data_.end()) {
            if constexpr (IsDefaultComparator<Compare> && KeyEqualComparable<K, decltype(it->first)>) {
               if (key == it->first) {
                  return it;
               }
            }
            else {
               if (!comp_(key, it->first) && !comp_(it->first, key)) {
                  return it;
               }
            }
         }
         return data_.end();
      }

      template <typename K>
      bool contains(const K& key) const
      {
         return find(key) != end();
      }

      template <typename K>
      iterator lower_bound(const K& key)
      {
         return std::lower_bound(data_.begin(), data_.end(), key,
                                 [this](const value_type& lhs, const K& rhs) { return comp_(lhs.first, rhs); });
      }

      template <typename K>
      const_iterator lower_bound(const K& key) const
      {
         return std::lower_bound(data_.begin(), data_.end(), key,
                                 [this](const value_type& lhs, const K& rhs) { return comp_(lhs.first, rhs); });
      }

      template <typename K>
      iterator upper_bound(const K& key)
      {
         return std::upper_bound(data_.begin(), data_.end(), key,
                                 [this](const K& lhs, const value_type& rhs) { return comp_(lhs, rhs.first); });
      }

      template <typename K>
      const_iterator upper_bound(const K& key) const
      {
         return std::upper_bound(data_.begin(), data_.end(), key,
                                 [this](const K& lhs, const value_type& rhs) { return comp_(lhs, rhs.first); });
      }

      // Element Access
      mapped_type& operator[](const key_type& key)
      {
         auto it = lower_bound(key);
         if (it == data_.end() || comp_(key, it->first)) {
            it = data_.emplace(it, key, mapped_type());
         }
         return it->second;
      }

      mapped_type& operator[](key_type&& key)
      {
         auto it = lower_bound(key);
         if (it == data_.end() || comp_(key, it->first)) {
            it = data_.emplace(it, std::move(key), mapped_type());
         }
         return it->second;
      }

      template <typename K>
      mapped_type& at(const K& key)
      {
         auto it = find(key);
         if (it == data_.end()) {
            GLZ_THROW_OR_ABORT(std::out_of_range("flat_map::at: key not found"));
         }
         return it->second;
      }

      template <typename K>
      const mapped_type& at(const K& key) const
      {
         auto it = find(key);
         if (it == data_.end()) {
            GLZ_THROW_OR_ABORT(std::out_of_range("flat_map::at: key not found"));
         }
         return it->second;
      }

      // Observers
      key_compare key_comp() const { return comp_; }
   };

   // Non-member functions
   template <class Key, class T, class Compare, class Container>
   bool operator==(const flat_map<Key, T, Compare, Container>& lhs, const flat_map<Key, T, Compare, Container>& rhs)
   {
      return lhs.data_ == rhs.data_;
   }

   template <class Key, class T, class Compare, class Container>
   bool operator!=(const flat_map<Key, T, Compare, Container>& lhs, const flat_map<Key, T, Compare, Container>& rhs)
   {
      return !(lhs == rhs);
   }

   template <class Key, class T, class Compare, class Container>
   void swap(flat_map<Key, T, Compare, Container>& lhs,
             flat_map<Key, T, Compare, Container>& rhs) noexcept(noexcept(lhs.swap(rhs)))
   {
      lhs.swap(rhs);
   }
}
