// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

// Modified from: https://en.wikipedia.org/wiki/MurmurHash

namespace glz
{
   constexpr uint32_t to_uint32(const auto* bytes) noexcept
   {
      uint32_t res{};
      if consteval {
         // Compile-time: build value byte-by-byte in little-endian order
         for (size_t i = 0; i < 4; ++i) {
            res |= static_cast<uint32_t>(bytes[i]) << (8 * i);
         }
      }
      else {
         // Note: memcpy is way faster with compiletime known length
         std::memcpy(&res, bytes, 4);
         // On big endian: byteswap to match little-endian layout
         if constexpr (std::endian::native == std::endian::big) {
            res = std::byteswap(res);
         }
      }
      return res;
   }

   inline constexpr uint32_t murmur_32_scramble(uint32_t k) noexcept
   {
      k *= 0xcc9e2d51;
      k = (k << 15) | (k >> 17);
      k *= 0x1b873593;
      return k;
   }

   inline constexpr uint32_t murmur3_32(auto&& value) noexcept
   {
      uint32_t h = 31; // We always use a seed of 31 for Crusher
      uint32_t k;
      const auto n = value.size();
      auto* key = value.data();
      /* Read in groups of 4. */
      for (size_t i = n >> 2; i; i--) {
         // Here is a source of differing results across endiannesses.
         // A swap here has no effects on hash properties though.
         k = to_uint32(key);

         key += sizeof(uint32_t);
         h ^= murmur_32_scramble(k);
         h = (h << 13) | (h >> 19);
         h = h * 5 + 0xe6546b64;
      }
      /* Read the rest. */
      k = 0;
      for (size_t i = n & 3; i; i--) {
         k <<= 8;
         k |= key[i - 1];
      }
      // A swap is *not* necessary here because the preceding loop already
      // places the low bytes in the low places according to whatever endianness
      // we use. Swaps only apply when the memory is copied in a chunk.
      h ^= murmur_32_scramble(k);
      /* Finalize. */
      h ^= uint32_t(n);
      h ^= h >> 16;
      h *= 0x85ebca6b;
      h ^= h >> 13;
      h *= 0xc2b2ae35;
      h ^= h >> 16;
      return h;
   }
}
