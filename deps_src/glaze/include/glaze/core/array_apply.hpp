// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <cstdint>

namespace glz
{
   template <class Adapter>
   struct array_apply_t
   {
      Adapter adapter{};
      size_t m_size{};

      struct dummy_iterator
      {
         using difference_type = std::ptrdiff_t;
         using value_type = Adapter;

         Adapter& operator*() const { return *adapter; }

         bool operator!=(const dummy_iterator& rhs) const { return position != rhs.position; }

         dummy_iterator& operator++()
         {
            ++position;
            return *this;
         }
         dummy_iterator operator++(int)
         {
            ++position;
            return dummy_iterator{adapter, position - 1};
         }
         Adapter* adapter{};
         size_t position{};
      };

      using reference = Adapter&;

      void clear() {};
      void resize(size_t newSize) { m_size = newSize; }
      reference emplace_back() { return adapter; }
      size_t size() const { return m_size; }

      dummy_iterator begin() { return dummy_iterator{&adapter, 0}; }
      dummy_iterator end() { return dummy_iterator{&adapter, m_size}; }
   };

   template <auto Adapter>
   constexpr auto array_apply = [] { return [](auto&& v) { return array_apply_t{Adapter(v)}; }; }();
}
