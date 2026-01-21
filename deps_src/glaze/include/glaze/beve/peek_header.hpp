// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/beve/header.hpp"
#include "glaze/core/context.hpp"
#include "glaze/util/expected.hpp"

namespace glz
{
   // Extension subtypes for beve_header
   namespace extension
   {
      constexpr uint8_t delimiter = 0; // Data delimiter (tag = 0x06)
      constexpr uint8_t variant = 1; // Variant type (tag = 0x0E), count = variant index
      constexpr uint8_t complex = 3; // Complex number/array (tag = 0x1E)
      constexpr uint8_t complex_number = 0; // Single complex (count = 2)
      constexpr uint8_t complex_array = 1; // Array of complex (count = element count)
   }

   // Information extracted from a BEVE header without full deserialization
   struct beve_header
   {
      uint8_t tag{}; // The raw tag byte
      uint8_t type{}; // Base type: null(0), number(1), string(2), object(3), typed_array(4), generic_array(5),
                      // extensions(6)
      uint8_t ext_type{}; // For extensions: subtype (extension::variant, extension::complex, etc.)
      size_t count{}; // Element/member count for containers, string length for strings, 1 for scalars
                      // For variants: the variant index; for complex_number: 2; for complex_array: element count
      size_t header_size{}; // Total bytes consumed by tag + count encoding (for seeking past header)
   };

   // Internal helper to peek at compressed integer without modifying iterator
   namespace detail
   {
      [[nodiscard]] GLZ_ALWAYS_INLINE constexpr size_t peek_compressed_int_size(const uint8_t* data,
                                                                                size_t available) noexcept
      {
         if (available == 0) return 0;
         const uint8_t config = data[0] & 0b000000'11;
         return byte_count_lookup[config];
      }

      [[nodiscard]] GLZ_ALWAYS_INLINE constexpr size_t peek_compressed_int_value(const uint8_t* data,
                                                                                 size_t available) noexcept
      {
         if (available == 0) return 0;

         const uint8_t header = data[0];
         const uint8_t config = header & 0b000000'11;
         const size_t required = byte_count_lookup[config];

         if (available < required) return 0;

         switch (config) {
         case 0:
            return header >> 2;
         case 1: {
            uint16_t h;
            std::memcpy(&h, data, 2);
            if constexpr (std::endian::native == std::endian::big) {
               h = std::byteswap(h);
            }
            return h >> 2;
         }
         case 2: {
            uint32_t h;
            std::memcpy(&h, data, 4);
            if constexpr (std::endian::native == std::endian::big) {
               h = std::byteswap(h);
            }
            return h >> 2;
         }
         case 3: {
            if constexpr (sizeof(size_t) > sizeof(uint32_t)) {
               uint64_t h;
               std::memcpy(&h, data, 8);
               if constexpr (std::endian::native == std::endian::big) {
                  h = std::byteswap(h);
               }
               return static_cast<size_t>(h >> 2);
            }
            else {
               return 0; // 8-byte length encoding not supported on 32-bit systems
            }
         }
         default:
            return 0;
         }
      }
   }

   // Peek at a BEVE buffer's header to extract type and count information
   // without performing full deserialization.
   //
   // This is useful for:
   // - Pre-allocating containers before reading
   // - Validating buffer structure before committing to deserialization
   // - Making decisions about how to process incoming data
   //
   // Returns the header information on success, or an error_ctx on failure.
   template <class Buffer>
   [[nodiscard]] expected<beve_header, error_ctx> beve_peek_header(const Buffer& buffer) noexcept
   {
      const auto* data = reinterpret_cast<const uint8_t*>(buffer.data());
      const size_t size = buffer.size();

      if (size == 0) [[unlikely]] {
         return unexpected(error_ctx{0, error_code::unexpected_end});
      }

      beve_header info{};
      info.tag = data[0];
      info.type = info.tag & 0b00000'111;

      switch (info.type) {
      case tag::null: {
         // null or boolean - check if it's actually a boolean
         if ((info.tag & 0b00001'000) != 0) {
            // It's a boolean
            info.count = 1;
         }
         else {
            info.count = 0;
         }
         info.header_size = 1;
         break;
      }
      case tag::number: {
         // Single number - count is always 1
         info.count = 1;
         info.header_size = 1;
         break;
      }
      case tag::string: {
         // String: tag + compressed_int(length)
         if (size < 2) [[unlikely]] {
            return unexpected(error_ctx{1, error_code::unexpected_end});
         }
         const size_t int_size = detail::peek_compressed_int_size(data + 1, size - 1);
         if (int_size == 0 || size < 1 + int_size) [[unlikely]] {
            return unexpected(error_ctx{1, error_code::unexpected_end});
         }
         info.count = detail::peek_compressed_int_value(data + 1, size - 1);
         info.header_size = 1 + int_size;
         break;
      }
      case tag::object: {
         // Object: tag + compressed_int(member_count)
         if (size < 2) [[unlikely]] {
            return unexpected(error_ctx{1, error_code::unexpected_end});
         }
         const size_t int_size = detail::peek_compressed_int_size(data + 1, size - 1);
         if (int_size == 0 || size < 1 + int_size) [[unlikely]] {
            return unexpected(error_ctx{1, error_code::unexpected_end});
         }
         info.count = detail::peek_compressed_int_value(data + 1, size - 1);
         info.header_size = 1 + int_size;
         break;
      }
      case tag::typed_array: {
         // Typed array: tag + compressed_int(element_count)
         if (size < 2) [[unlikely]] {
            return unexpected(error_ctx{1, error_code::unexpected_end});
         }
         const size_t int_size = detail::peek_compressed_int_size(data + 1, size - 1);
         if (int_size == 0 || size < 1 + int_size) [[unlikely]] {
            return unexpected(error_ctx{1, error_code::unexpected_end});
         }
         info.count = detail::peek_compressed_int_value(data + 1, size - 1);
         info.header_size = 1 + int_size;
         break;
      }
      case tag::generic_array: {
         // Generic array: tag + compressed_int(element_count)
         if (size < 2) [[unlikely]] {
            return unexpected(error_ctx{1, error_code::unexpected_end});
         }
         const size_t int_size = detail::peek_compressed_int_size(data + 1, size - 1);
         if (int_size == 0 || size < 1 + int_size) [[unlikely]] {
            return unexpected(error_ctx{1, error_code::unexpected_end});
         }
         info.count = detail::peek_compressed_int_value(data + 1, size - 1);
         info.header_size = 1 + int_size;
         break;
      }
      case tag::extensions: {
         // Extensions: delimiter, variant, or complex
         // Subtype is encoded in bits 3-4 of the tag
         const uint8_t subtype = (info.tag >> 3) & 0b11;
         info.ext_type = subtype;

         if (subtype == extension::delimiter) {
            // Delimiter (tag = 0x06): just a separator marker
            info.count = 0;
            info.header_size = 1;
         }
         else if (subtype == extension::variant) {
            // Variant (tag = 0x0E): tag + compressed_int(index) + value
            if (size < 2) [[unlikely]] {
               return unexpected(error_ctx{1, error_code::unexpected_end});
            }
            const size_t int_size = detail::peek_compressed_int_size(data + 1, size - 1);
            if (int_size == 0 || size < 1 + int_size) [[unlikely]] {
               return unexpected(error_ctx{1, error_code::unexpected_end});
            }
            info.count = detail::peek_compressed_int_value(data + 1, size - 1); // variant index
            info.header_size = 1 + int_size;
         }
         else if (subtype == extension::complex) {
            // Complex (tag = 0x1E): tag + complex_header + ...
            if (size < 2) [[unlikely]] {
               return unexpected(error_ctx{1, error_code::unexpected_end});
            }
            const uint8_t complex_header = data[1];
            const bool is_array = (complex_header & 1) == extension::complex_array;

            if (is_array) {
               // Complex array: tag + complex_header + compressed_int(count) + data
               if (size < 3) [[unlikely]] {
                  return unexpected(error_ctx{2, error_code::unexpected_end});
               }
               const size_t int_size = detail::peek_compressed_int_size(data + 2, size - 2);
               if (int_size == 0 || size < 2 + int_size) [[unlikely]] {
                  return unexpected(error_ctx{2, error_code::unexpected_end});
               }
               info.count = detail::peek_compressed_int_value(data + 2, size - 2);
               info.header_size = 2 + int_size;
            }
            else {
               // Single complex number: tag + complex_header + real + imag
               info.count = 2; // real + imag parts
               info.header_size = 2;
            }
         }
         else {
            // Unknown extension subtype
            return unexpected(error_ctx{0, error_code::syntax_error});
         }
         break;
      }
      default: {
         return unexpected(error_ctx{0, error_code::syntax_error});
      }
      }

      return info;
   }

   // Convenience overload for C-style arrays and raw pointers with size
   [[nodiscard]] inline expected<beve_header, error_ctx> beve_peek_header(const void* data, size_t size) noexcept
   {
      return beve_peek_header(std::string_view{reinterpret_cast<const char*>(data), size});
   }

   // Peek at a BEVE buffer's header at a specific byte offset.
   //
   // This avoids creating substrings or slicing the buffer when you need to
   // inspect headers at arbitrary positions. Useful for:
   // - Buffers with custom headers/prefixes before the BEVE data
   // - Memory-mapped files where you seek to specific positions
   // - Resuming parsing after partial reads
   // - Concatenated/delimited BEVE streams
   // - BEVE data embedded within larger binary structures
   //
   // Parameters:
   //   buffer - The buffer containing BEVE data
   //   offset - The byte offset at which to start peeking
   //
   // Returns the header information on success, or an error_ctx on failure.
   // The header_size in the result is relative to the offset position.
   template <class Buffer>
   [[nodiscard]] expected<beve_header, error_ctx> beve_peek_header_at(const Buffer& buffer, size_t offset) noexcept
   {
      const size_t size = buffer.size();
      if (offset >= size) [[unlikely]] {
         return unexpected(error_ctx{offset, error_code::unexpected_end});
      }

      const auto* data = reinterpret_cast<const uint8_t*>(buffer.data()) + offset;
      const size_t remaining = size - offset;

      return beve_peek_header(std::string_view{reinterpret_cast<const char*>(data), remaining});
   }

   // Convenience overload for raw pointers with size and offset
   [[nodiscard]] inline expected<beve_header, error_ctx> beve_peek_header_at(const void* data, size_t size,
                                                                             size_t offset) noexcept
   {
      if (offset >= size) [[unlikely]] {
         return unexpected(error_ctx{offset, error_code::unexpected_end});
      }
      return beve_peek_header(std::string_view{reinterpret_cast<const char*>(data) + offset, size - offset});
   }
}
