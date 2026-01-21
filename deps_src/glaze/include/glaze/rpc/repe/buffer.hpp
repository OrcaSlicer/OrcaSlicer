// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <cstddef>
#include <cstring>
#include <optional>
#include <span>

#include "glaze/core/reflect.hpp"
#include "glaze/json/read.hpp"
#include "glaze/rpc/repe/header.hpp"

namespace glz::repe
{
   // ============================================================
   // Header utilities
   // ============================================================

   /// Finalize message header lengths after modifying query/body
   /// @param msg The message to finalize
   inline void finalize_header(message& msg)
   {
      msg.header.query_length = msg.query.size();
      msg.header.body_length = msg.body.size();
      msg.header.length = sizeof(header) + msg.header.query_length + msg.header.body_length;
   }

   // ============================================================
   // Error encoding/decoding
   // ============================================================

   inline void encode_error(const error_code ec, message& msg)
   {
      msg.header.ec = ec;
      msg.body.clear();
   }

   template <class ErrorMessage>
      requires(requires(ErrorMessage m) { m.size(); })
   inline void encode_error(const error_code ec, message& msg, ErrorMessage&& error_message)
   {
      msg.header.ec = ec;
      if (error_message.size() > (std::numeric_limits<uint32_t>::max)()) {
         return;
      }
      if (error_message.empty()) {
         return;
      }
      const auto error_size = error_message.size();
      msg.header.body_length = error_size;
      msg.body = error_message;
   }

   template <class ErrorMessage>
      requires(not requires(ErrorMessage m) { m.size(); })
   inline void encode_error(const error_code ec, message& msg, ErrorMessage&& error_message)
   {
      encode_error(ec, msg, std::string_view{error_message});
   }

   /// Decodes a repe::message when an error has been encountered
   inline std::string decode_error(message& msg)
   {
      if (bool(msg.error())) {
         const auto ec = msg.header.ec;
         if (msg.header.body_length >= 4) {
            const std::string_view error_message = msg.body;
            std::string ret = "REPE error: ";
            ret.append(glz::format_error(ec));
            ret.append(" | ");
            ret.append(error_message);
            return ret;
         }
         else {
            return std::string{"REPE error: "} + glz::format_error(ec);
         }
      }
      return {"no error"};
   }

   /// Decodes a repe::message into a structure
   /// Returns a std::string with a formatted error on error
   template <auto Opts = opts{}, class T>
   inline std::optional<std::string> decode_message(T&& value, message& msg)
   {
      if (bool(msg.header.ec)) {
         const auto ec = msg.header.ec;
         if (msg.header.body_length >= 4) {
            const std::string_view error_message = msg.body;
            std::string ret = "REPE error: ";
            ret.append(glz::format_error(ec));
            ret.append(" | ");
            ret.append(error_message);
            return {ret};
         }
         else {
            return std::string{"REPE error: "} + glz::format_error(ec);
         }
      }

      // we don't const qualify `msg` for performance
      auto ec = glz::read<Opts>(std::forward<T>(value), msg.body);
      if (ec) {
         return {glz::format_error(ec, msg.body)};
      }

      return {};
   }

   // ============================================================
   // Serialization: repe::message → raw bytes
   // ============================================================

   /// Serialize a repe::message to a string buffer
   /// @param msg The message to serialize
   /// @return String containing the wire-format bytes (header + query + body)
   inline std::string to_buffer(const message& msg)
   {
      std::string buffer;
      buffer.resize(sizeof(header) + msg.query.size() + msg.body.size());
      std::memcpy(buffer.data(), &msg.header, sizeof(header));
      std::memcpy(buffer.data() + sizeof(header), msg.query.data(), msg.query.size());
      std::memcpy(buffer.data() + sizeof(header) + msg.query.size(), msg.body.data(), msg.body.size());
      return buffer;
   }

   /// Serialize a repe::message to an existing buffer
   /// @param msg The message to serialize
   /// @param buffer Output buffer (will be resized)
   inline void to_buffer(const message& msg, std::string& buffer)
   {
      buffer.resize(sizeof(header) + msg.query.size() + msg.body.size());
      std::memcpy(buffer.data(), &msg.header, sizeof(header));
      std::memcpy(buffer.data() + sizeof(header), msg.query.data(), msg.query.size());
      std::memcpy(buffer.data() + sizeof(header) + msg.query.size(), msg.body.data(), msg.body.size());
   }

   // ============================================================
   // Deserialization: raw bytes → repe::message
   // ============================================================

   /// Deserialize a repe::message from raw bytes
   /// @param data Pointer to wire-format data
   /// @param size Size of data in bytes
   /// @param msg Output message
   /// @return error_code::none on success, appropriate error on failure
   inline error_code from_buffer(const char* data, size_t size, message& msg)
   {
      if (size < sizeof(header)) {
         return error_code::invalid_header;
      }

      std::memcpy(&msg.header, data, sizeof(header));

      // Validate REPE magic
      if (msg.header.spec != repe_magic) {
         return error_code::invalid_header;
      }

      // Validate version
      if (msg.header.version != 1) {
         return error_code::version_mismatch;
      }

      // Validate sizes
      const size_t expected_size = sizeof(header) + msg.header.query_length + msg.header.body_length;
      if (size < expected_size) {
         return error_code::invalid_body;
      }

      // Extract query and body
      const char* query_start = data + sizeof(header);
      const char* body_start = query_start + msg.header.query_length;

      msg.query.assign(query_start, msg.header.query_length);
      msg.body.assign(body_start, msg.header.body_length);

      return error_code::none;
   }

   /// Deserialize from string_view
   inline error_code from_buffer(std::string_view data, message& msg)
   {
      return from_buffer(data.data(), data.size(), msg);
   }

   // ============================================================
   // Header-only parsing (for routing without full deserialization)
   // ============================================================

   /// Parse only the header from raw bytes (useful for routing)
   /// @param data Pointer to wire-format data
   /// @param size Size of data in bytes
   /// @param hdr Output header
   /// @return error_code::none on success
   inline error_code parse_header(const char* data, size_t size, header& hdr)
   {
      if (size < sizeof(header)) {
         return error_code::invalid_header;
      }

      std::memcpy(&hdr, data, sizeof(header));

      if (hdr.spec != repe_magic) {
         return error_code::invalid_header;
      }

      return error_code::none;
   }

   /// Parse only the header from string_view
   inline error_code parse_header(std::string_view data, header& hdr)
   {
      return parse_header(data.data(), data.size(), hdr);
   }

   /// Extract just the query string without full deserialization
   /// @param data Pointer to wire-format data
   /// @param size Size of data in bytes
   /// @return The query string, or empty string on error
   inline std::string_view extract_query(const char* data, size_t size)
   {
      if (size < sizeof(header)) {
         return {};
      }

      header hdr;
      std::memcpy(&hdr, data, sizeof(header));

      if (hdr.spec != repe_magic) {
         return {};
      }

      if (size < sizeof(header) + hdr.query_length) {
         return {};
      }

      return {data + sizeof(header), static_cast<size_t>(hdr.query_length)};
   }

   /// Extract just the query string from string_view
   inline std::string_view extract_query(std::string_view data) { return extract_query(data.data(), data.size()); }

   // ============================================================
   // Zero-copy helper functions (for raw buffer call handlers)
   // ============================================================

   /// Check if request is a notification without full deserialization
   /// Uses offsetof for robustness against header layout changes
   /// @param data Span of raw message bytes
   /// @return true if this is a notification (no response expected)
   inline bool is_notify(std::span<const char> data) noexcept
   {
      if (data.size() < sizeof(header)) return false;
      uint8_t notify{};
      std::memcpy(&notify, data.data() + offsetof(header, notify), sizeof(notify));
      return notify != 0;
   }

   /// Extract message ID without full deserialization
   /// @param data Span of raw message bytes
   /// @return The message ID, or 0 if data is too small
   inline uint64_t extract_id(std::span<const char> data) noexcept
   {
      if (data.size() < sizeof(header)) return 0;
      uint64_t id{};
      std::memcpy(&id, data.data() + offsetof(header, id), sizeof(id));
      return id;
   }

   /// Quick validation without full parse
   /// @param data Span of raw message bytes
   /// @return error_code::none if valid, appropriate error otherwise
   inline error_code validate_header_only(std::span<const char> data) noexcept
   {
      if (data.size() < sizeof(header)) return error_code::invalid_header;

      uint16_t spec{};
      uint8_t version{};
      std::memcpy(&spec, data.data() + offsetof(header, spec), sizeof(spec));
      std::memcpy(&version, data.data() + offsetof(header, version), sizeof(version));

      if (spec != repe_magic) return error_code::invalid_header;
      if (version != 1) return error_code::version_mismatch;
      return error_code::none;
   }

   /// Encode an error directly into a buffer (avoids message intermediary)
   /// @param ec The error code
   /// @param buffer Output buffer (will be resized)
   /// @param error_message The error message text
   /// @param id Optional message ID to include in response
   template <class ErrorMessage>
   inline void encode_error_buffer(error_code ec, std::string& buffer, ErrorMessage&& error_message, uint64_t id = 0)
   {
      header hdr{};
      hdr.spec = repe_magic;
      hdr.version = 1;
      hdr.id = id;
      hdr.ec = ec;
      hdr.body_format = body_format::UTF8;

      std::string_view msg{error_message};
      hdr.query_length = 0;
      hdr.body_length = msg.size();
      hdr.length = sizeof(header) + hdr.body_length;

      buffer.resize(sizeof(header) + msg.size());
      std::memcpy(buffer.data(), &hdr, sizeof(header));
      if (!msg.empty()) {
         std::memcpy(buffer.data() + sizeof(header), msg.data(), msg.size());
      }
   }

   /// Create a complete error response buffer (convenience function)
   /// @param ec The error code
   /// @param message The error message text
   /// @param id Optional message ID
   /// @return String containing the complete error response
   inline std::string make_error_response(error_code ec, std::string_view message, uint64_t id = 0)
   {
      std::string buffer;
      encode_error_buffer(ec, buffer, message, id);
      return buffer;
   }
}
