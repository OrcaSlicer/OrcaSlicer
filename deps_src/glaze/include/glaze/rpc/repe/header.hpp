// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "glaze/core/context.hpp"

namespace glz::repe
{
   // REPE protocol magic bytes (0x1507 = 5383)
   inline constexpr uint16_t repe_magic = 0x1507;

   // REPE Reserved Query Formats (0-4095 are reserved)
   enum class query_format : uint16_t { RAW_BINARY = 0, JSON_POINTER = 1 };

   // REPE Reserved Body Formats (0-4095 are reserved)
   enum class body_format : uint16_t { RAW_BINARY = 0, BEVE = 1, JSON = 2, UTF8 = 3 };

   struct header
   {
      uint64_t length{}; // Total length of [header, query, body]
      //
      uint16_t spec{repe_magic}; // Magic two bytes to denote the REPE specification
      uint8_t version = 1; // REPE version
      uint8_t notify{}; // 1 (true) for no response from server
      uint32_t reserved{}; // Must be zero, receivers must ignore this field
      //
      uint64_t id{}; // Identifier
      //
      uint64_t query_length{}; // The total length of the query
      //
      uint64_t body_length{}; // The total length of the body
      //
      repe::query_format query_format{};
      repe::body_format body_format{};
      error_code ec{};
   };

   static_assert(sizeof(header) == 48);

   // query and body are heap allocated and we want to be able to reuse memory
   struct message final
   {
      repe::header header{};
      std::string query{};
      std::string body{};

      operator bool() const { return bool(header.ec); }

      error_code error() const { return header.ec; }
   };

   // User interface that will be encoded into a REPE header
   struct user_header final
   {
      std::string_view query = ""; // The JSON pointer path to call or member to access/assign
      uint64_t id{}; // Identifier
      error_code ec{};
      bool notify{};
   };

   inline repe::header encode(const user_header& h) noexcept
   {
      repe::header ret{
         .notify = h.notify, //
         .id = h.id, //
         .query_length = h.query.size(), //
         .ec = h.ec //
      };
      return ret;
   }
}
