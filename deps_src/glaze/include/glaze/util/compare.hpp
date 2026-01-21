// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace glz
{
   template <class Char>
   inline bool compare(const Char* lhs, const Char* rhs, uint64_t count) noexcept
   {
      if (count > 7) {
         uint64_t v[2];
         while (count > 8) {
            std::memcpy(v, lhs, 8);
            std::memcpy(v + 1, rhs, 8);
            if (v[0] != v[1]) {
               return false;
            }
            count -= 8;
            lhs += 8;
            rhs += 8;
         }

         const auto shift = 8 - count;
         lhs -= shift;
         rhs -= shift;

         std::memcpy(v, lhs, 8);
         std::memcpy(v + 1, rhs, 8);
         return v[0] == v[1];
      }

      {
         constexpr uint64_t n{sizeof(uint32_t)};
         if (count >= n) {
            uint32_t v[2];
            std::memcpy(v, lhs, n);
            std::memcpy(v + 1, rhs, n);
            if (v[0] != v[1]) {
               return false;
            }
            count -= n;
            lhs += n;
            rhs += n;
         }
      }
      {
         constexpr uint64_t n{sizeof(uint16_t)};
         if (count >= n) {
            uint16_t v[2];
            std::memcpy(v, lhs, n);
            std::memcpy(v + 1, rhs, n);
            if (v[0] != v[1]) {
               return false;
            }
            count -= n;
            lhs += n;
            rhs += n;
         }
      }
      if (count && *lhs != *rhs) {
         return false;
      }
      return true;
   }

   // IMPORTANT:
   // This comparison function produces less binary than `compare` above and is very fast.
   // However, if our count is less than 8, we must be able to access the previous [8 - count] bytes.
   template <class Char>
   inline bool internal_compare(const Char* lhs, const Char* rhs, uint64_t count) noexcept
   {
      uint64_t v[2];
      while (count > 8) {
         std::memcpy(v, lhs, 8);
         std::memcpy(v + 1, rhs, 8);
         if (v[0] != v[1]) {
            return false;
         }
         count -= 8;
         lhs += 8;
         rhs += 8;
      }

      const auto shift = 8 - count;
      lhs -= shift;
      rhs -= shift;

      std::memcpy(v, lhs, 8);
      std::memcpy(v + 1, rhs, 8);
      return v[0] == v[1];
   }

   template <uint64_t Count, class Char>
   GLZ_ALWAYS_INLINE bool compare(const Char* lhs, const Char* rhs) noexcept
   {
      if constexpr (Count > 8) {
         return 0 == std::memcmp(lhs, rhs, Count);
         // return internal_compare(lhs, rhs, Count);
      }
      else if constexpr (Count == 8) {
         uint64_t l, r;
         std::memcpy(&l, lhs, 8);
         std::memcpy(&r, rhs, 8);
         return l == r;
      }
      else if constexpr (Count == 7) {
         uint32_t l, r;
         std::memcpy(&l, lhs, 4);
         std::memcpy(&r, rhs, 4);
         uint32_t l2, r2;
         std::memcpy(&l2, lhs + 3, 4);
         std::memcpy(&r2, rhs + 3, 4);
         return (l == r) & (l2 == r2);
      }
      else if constexpr (Count == 6) {
         uint32_t l, r;
         std::memcpy(&l, lhs, 4);
         std::memcpy(&r, rhs, 4);
         uint16_t l2, r2;
         std::memcpy(&l2, lhs + 4, 2);
         std::memcpy(&r2, rhs + 4, 2);
         return (l == r) & (l2 == r2);
      }
      else if constexpr (Count == 5) {
         uint32_t l, r;
         std::memcpy(&l, lhs, 4);
         std::memcpy(&r, rhs, 4);
         return (l == r) & (lhs[4] == rhs[4]);
      }
      else if constexpr (Count == 4) {
         uint32_t l, r;
         std::memcpy(&l, lhs, 4);
         std::memcpy(&r, rhs, 4);
         return l == r;
      }
      else if constexpr (Count == 3) {
         uint16_t l, r;
         std::memcpy(&l, lhs, 2);
         std::memcpy(&r, rhs, 2);
         return (l == r) & (lhs[2] == rhs[2]);
      }
      else if constexpr (Count == 2) {
         uint16_t l, r;
         std::memcpy(&l, lhs, 2);
         std::memcpy(&r, rhs, 2);
         return l == r;
      }
      else if constexpr (Count == 1) {
         return *lhs == *rhs;
      }
      else if constexpr (Count == 0) {
         return true;
      }
   }

   template <size_t N>
   consteval auto bytes_to_unsigned_type() noexcept
   {
      if constexpr (N == 1) {
         return uint8_t{};
      }
      else if constexpr (N == 2) {
         return uint16_t{};
      }
      else if constexpr (N == 4) {
         return uint32_t{};
      }
      else if constexpr (N == 8) {
         return uint64_t{};
      }
      else {
         return;
      }
   }

   template <size_t N>
   using unsigned_bytes_t = std::decay_t<decltype(bytes_to_unsigned_type<N>())>;

   template <const std::string_view& Str, size_t N>
      requires(N <= 8)
   consteval auto pack()
   {
      using T = unsigned_bytes_t<N>;
      T v{};
      for (size_t i = 0; i < N; ++i) {
         v |= (static_cast<T>(uint8_t(Str[i])) << ((i % 8) * 8));
      }
      return v;
   }

   template <const std::string_view& Str, size_t N>
      requires(N > 8)
   consteval auto pack()
   {
      constexpr auto chunks = N / 8;
      std::array<uint64_t, ((chunks > 0) ? chunks + 1 : 1)> v{};
      for (size_t i = 0; i < N; ++i) {
         const auto chunk = i / 8;
         v[chunk] |= (static_cast<uint64_t>(uint8_t(Str[i])) << ((i % 8) * 8));
      }
      return v;
   }

   template <const std::string_view& Str, size_t N>
      requires(N <= 8)
   consteval auto pack_buffered()
   {
      using T = unsigned_bytes_t<N>;
      T v{};
      for (size_t i = 0; i < Str.size(); ++i) {
         v |= (static_cast<T>(uint8_t(Str[i])) << ((i % 8) * 8));
      }
      return v;
   }

   template <const std::string_view& Str, size_t N = Str.size()>
   GLZ_ALWAYS_INLINE bool comparitor(const auto* other) noexcept
   {
      // pack() builds values in little-endian order (byte 0 in LSB position).
      // On big-endian systems, memcpy produces native (big-endian) values,
      // so we need to byteswap to match the packed representation.
      if constexpr (N == 8) {
         static constexpr auto packed = pack<Str, 8>();
         uint64_t in;
         std::memcpy(&in, other, 8);
         if constexpr (std::endian::native == std::endian::big) {
            in = std::byteswap(in);
         }
         return (in == packed);
      }
      else if constexpr (N == 7) {
         static constexpr auto packed = pack_buffered<Str, 8>();
         uint64_t in{};
         std::memcpy(&in, other, 7);
         if constexpr (std::endian::native == std::endian::big) {
            in = std::byteswap(in);
         }
         return (in == packed);
      }
      else if constexpr (N == 6) {
         static constexpr auto packed = pack_buffered<Str, 8>();
         uint64_t in{};
         std::memcpy(&in, other, 6);
         if constexpr (std::endian::native == std::endian::big) {
            in = std::byteswap(in);
         }
         return (in == packed);
      }
      else if constexpr (N == 5) {
         static constexpr auto packed = pack<Str, 4>();
         uint32_t in;
         std::memcpy(&in, other, 4);
         if constexpr (std::endian::native == std::endian::big) {
            in = std::byteswap(in);
         }
         return (in == packed) & (Str[4] == other[4]);
      }
      else if constexpr (N == 4) {
         static constexpr auto packed = pack<Str, 4>();
         uint32_t in;
         std::memcpy(&in, other, 4);
         if constexpr (std::endian::native == std::endian::big) {
            in = std::byteswap(in);
         }
         return (in == packed);
      }
      else if constexpr (N == 3) {
         static constexpr auto packed = pack<Str, 2>();
         uint16_t in;
         std::memcpy(&in, other, 2);
         if constexpr (std::endian::native == std::endian::big) {
            in = std::byteswap(in);
         }
         return (in == packed) & (Str[2] == other[2]);
      }
      else if constexpr (N == 2) {
         static constexpr auto packed = pack<Str, 2>();
         uint16_t in;
         std::memcpy(&in, other, 2);
         if constexpr (std::endian::native == std::endian::big) {
            in = std::byteswap(in);
         }
         return (in == packed);
      }
      else if constexpr (N == 1) {
         return Str[0] == other[0];
      }
      else if constexpr (N == 0) {
         return true;
      }
      else {
         // Clang and GCC optimize this extremely well for constexpr std::string_view
         // Packing data can create more binary on GCC
         // The other cases probably aren't needed as compiler explorer shows them optimized equally well as memcmp
         return 0 == std::memcmp(Str.data(), other, N);
      }
   }

   // compare_sv checks sizes
   inline constexpr bool compare_sv(const std::string_view lhs, const std::string_view rhs) noexcept
   {
      if consteval {
         return lhs == rhs;
      }
      else {
         return (lhs.size() == rhs.size()) && compare(lhs.data(), rhs.data(), lhs.size());
      }
   }

   template <const std::string_view& lhs>
   inline constexpr bool compare_sv(const std::string_view rhs) noexcept
   {
      if consteval {
         return lhs == rhs;
      }
      else {
         constexpr auto N = lhs.size();
         return (N == rhs.size()) && compare<N>(lhs.data(), rhs.data());
      }
   }
}
