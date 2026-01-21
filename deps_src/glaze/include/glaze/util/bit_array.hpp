// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <array>
#include <bit>
#include <limits>

namespace glz
{
   // Basicly std::bitset but exposes things normally not availible like the bitscan functions
   template <size_t N, std::unsigned_integral Chunk = uint64_t>
   struct bit_array
   {
      static constexpr size_t n_chunk_bits = std::numeric_limits<Chunk>::digits;
      static constexpr size_t n_chunks = (N == 0) ? 0 : (N - 1) / n_chunk_bits + 1;

      struct reference
      {
         Chunk* data{};
         Chunk maskbit{};

         constexpr reference& operator=(bool other) noexcept
         {
            if (other)
               *data |= maskbit;
            else
               *data &= ~maskbit;
            return *this;
         }

         constexpr operator bool() const noexcept { return (*data & maskbit) != 0; }

         constexpr bool operator~() const noexcept { return (*data & maskbit) == 0; }
      };

      std::array<Chunk, n_chunks> data{};

      constexpr reference operator[](size_t pos)
      {
         const auto chunk = pos / n_chunk_bits;
         const auto offset = pos % n_chunk_bits;
         const auto maskbit = Chunk{1} << offset;
         return reference{&data[chunk], maskbit};
      }

      constexpr bool operator[](size_t pos) const
      {
         const auto chunk = pos / n_chunk_bits;
         const auto offset = pos % n_chunk_bits;
         const auto maskbit = Chunk{1} << offset;
         return data[chunk] & maskbit;
      }

      constexpr int popcount() noexcept
      {
         if constexpr (n_chunks == 1) {
            return std::popcount(data[0]);
         }
         else {
            int res{};
            for (auto&& item : data) {
               res += std::popcount(item);
            }
            return res;
         }
      }

      constexpr int countl_zero() noexcept
      {
         if constexpr (n_chunks == 1) {
            return std::countl_zero(data[0]);
         }
         else {
            int res{};
            for (auto&& item : data) {
               const auto leading_zeros = std::countl_zero(item);
               res += leading_zeros;
               if (leading_zeros < n_chunk_bits) {
                  return res;
               }
            }
            return res;
         }
      }

      constexpr int countr_zero() noexcept
      {
         if constexpr (n_chunks == 1) {
            return std::countr_zero(data[0]);
         }
         else {
            int res{};
            for (int i = static_cast<int>(data.size()) - 1; i > -1; --i) {
               const auto trailing_zeros = std::countr_zero(data[i]);
               res += trailing_zeros;
               if (trailing_zeros < n_chunk_bits) {
                  return res;
               }
            }
            return res;
         }
      }

      constexpr int has_single_bit() noexcept
      {
         if constexpr (n_chunks == 1) {
            return std::has_single_bit(data[0]);
         }
         else {
            return popcount() == 1;
         }
      }

      constexpr bit_array& flip() noexcept
      {
         for (auto& item : data) {
            item = ~item;
         }
         return *this;
      }

      constexpr bit_array& operator&=(const bit_array& rhs) noexcept
      {
         for (size_t i{}; i < n_chunks; ++i) {
            data[i] &= rhs.data[i];
         }
         return *this;
      }

      constexpr bit_array operator&(const bit_array& rhs) const noexcept
      {
         auto ret = *this;
         return ret &= rhs;
      }

      constexpr bool operator==(const bit_array& rhs) const noexcept { return data == rhs.data; }
   };
}
