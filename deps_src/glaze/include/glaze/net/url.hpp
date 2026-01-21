// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

namespace glz
{
   // URL encoding/decoding utilities
   // Implements percent-encoding per RFC 3986 and application/x-www-form-urlencoded parsing per WHATWG URL Standard

   /**
    * @brief Convert a hex character to its integer value
    * @param c The hex character ('0'-'9', 'a'-'f', 'A'-'F')
    * @return The integer value (0-15), or -1 if invalid
    */
   constexpr int hex_char_to_int(char c) noexcept
   {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
   }

   /**
    * @brief Decode a URL-encoded (percent-encoded) string into a provided buffer
    *
    * Handles:
    * - %XX hex escapes (e.g., %20 -> space, %2F -> /)
    * - + as space (per application/x-www-form-urlencoded)
    *
    * @param input The URL-encoded string
    * @param output The buffer to write the decoded string to (cleared before writing)
    */
   inline void url_decode(std::string_view input, std::string& output)
   {
      output.clear();
      output.reserve(input.size());

      for (size_t i = 0; i < input.size(); ++i) {
         if (input[i] == '%' && i + 2 < input.size()) {
            const int high = hex_char_to_int(input[i + 1]);
            const int low = hex_char_to_int(input[i + 2]);
            if (high >= 0 && low >= 0) {
               output += static_cast<char>((high << 4) | low);
               i += 2;
               continue;
            }
         }
         else if (input[i] == '+') {
            output += ' ';
            continue;
         }
         output += input[i];
      }
   }

   /**
    * @brief Decode a URL-encoded (percent-encoded) string
    *
    * Convenience overload that allocates and returns a new string.
    *
    * @param input The URL-encoded string
    * @return The decoded string
    */
   [[nodiscard]] inline std::string url_decode(std::string_view input)
   {
      std::string result;
      url_decode(input, result);
      return result;
   }

   /**
    * @brief URL-encode (percent-encode) a string into a provided buffer
    *
    * Encodes characters for safe use in URLs:
    * - Unreserved characters (A-Z, a-z, 0-9, -, ., _, ~) pass through unchanged
    * - Space is encoded as + (per application/x-www-form-urlencoded)
    * - All other characters are percent-encoded (%XX)
    *
    * @param input The string to encode
    * @param output The buffer to write the encoded string to (cleared before writing)
    */
   inline void url_encode(std::string_view input, std::string& output)
   {
      output.clear();
      output.reserve(input.size() * 3); // worst case: every char encoded

      constexpr char hex_chars[] = "0123456789ABCDEF";

      for (unsigned char c : input) {
         if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
             c == '_' || c == '~') {
            // Unreserved characters pass through
            output += static_cast<char>(c);
         }
         else if (c == ' ') {
            // Space encoded as + for form encoding
            output += '+';
         }
         else {
            // Everything else percent-encoded
            output += '%';
            output += hex_chars[c >> 4];
            output += hex_chars[c & 0x0F];
         }
      }
   }

   /**
    * @brief URL-encode (percent-encode) a string
    *
    * Convenience overload that allocates and returns a new string.
    *
    * @param input The string to encode
    * @return The encoded string
    */
   [[nodiscard]] inline std::string url_encode(std::string_view input)
   {
      std::string result;
      url_encode(input, result);
      return result;
   }

   /**
    * @brief Parse a URL-encoded query string or form body into a provided map
    *
    * Parses key=value pairs separated by '&'.
    * Both keys and values are URL-decoded.
    *
    * @param query_string The query string (without leading '?')
    * @param output The map to write key-value pairs to (cleared before writing)
    * @param key_buffer Optional reusable buffer for key decoding (avoids allocations)
    * @param value_buffer Optional reusable buffer for value decoding (avoids allocations)
    */
   inline void parse_urlencoded(std::string_view query_string, std::unordered_map<std::string, std::string>& output,
                                std::string& key_buffer, std::string& value_buffer)
   {
      output.clear();

      if (query_string.empty()) {
         return;
      }

      size_t pos = 0;
      while (pos < query_string.size()) {
         // Find the end of this key=value pair
         size_t amp_pos = query_string.find('&', pos);
         if (amp_pos == std::string_view::npos) {
            amp_pos = query_string.size();
         }

         std::string_view pair = query_string.substr(pos, amp_pos - pos);
         if (!pair.empty()) {
            // Find the '=' separator
            size_t eq_pos = pair.find('=');
            if (eq_pos != std::string_view::npos) {
               std::string_view key = pair.substr(0, eq_pos);
               std::string_view value = pair.substr(eq_pos + 1);
               if (!key.empty()) {
                  url_decode(key, key_buffer);
                  url_decode(value, value_buffer);
                  output[key_buffer] = value_buffer;
               }
            }
            else {
               // Key without value (e.g., "?flag")
               url_decode(pair, key_buffer);
               output[key_buffer] = "";
            }
         }

         pos = amp_pos + 1;
      }
   }

   /**
    * @brief Parse a URL-encoded query string or form body into a provided map
    *
    * Parses key=value pairs separated by '&'.
    * Both keys and values are URL-decoded.
    *
    * @param query_string The query string (without leading '?')
    * @param output The map to write key-value pairs to (cleared before writing)
    */
   inline void parse_urlencoded(std::string_view query_string, std::unordered_map<std::string, std::string>& output)
   {
      std::string key_buffer;
      std::string value_buffer;
      parse_urlencoded(query_string, output, key_buffer, value_buffer);
   }

   /**
    * @brief Parse a URL-encoded query string or form body
    *
    * Convenience overload that allocates and returns a new map.
    *
    * Examples:
    * - "limit=10&offset=20" -> {{"limit", "10"}, {"offset", "20"}}
    * - "name=John%20Doe" -> {{"name", "John Doe"}}
    * - "flag" -> {{"flag", ""}}
    * - "a=1&a=2" -> {{"a", "2"}} (last value wins)
    *
    * @param query_string The query string (without leading '?')
    * @return Map of decoded key-value pairs
    */
   [[nodiscard]] inline std::unordered_map<std::string, std::string> parse_urlencoded(std::string_view query_string)
   {
      std::unordered_map<std::string, std::string> result;
      parse_urlencoded(query_string, result);
      return result;
   }

   /**
    * @brief Result of splitting a target into path and query string
    */
   struct target_components
   {
      std::string_view path{};
      std::string_view query_string{};
   };

   /**
    * @brief Split a request target into path and query string components
    *
    * Examples:
    * - "/api/users" -> {"/api/users", ""}
    * - "/api/users?limit=10" -> {"/api/users", "limit=10"}
    * - "/search?q=hello%20world&page=1" -> {"/search", "q=hello%20world&page=1"}
    *
    * @param target The full request target (path + optional query string)
    * @return The path and query string components (query string without '?')
    */
   constexpr target_components split_target(std::string_view target) noexcept
   {
      const size_t query_pos = target.find('?');
      if (query_pos == std::string_view::npos) {
         return {target, {}};
      }
      return {target.substr(0, query_pos), target.substr(query_pos + 1)};
   }
}
