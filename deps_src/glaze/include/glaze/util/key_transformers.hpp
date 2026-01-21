// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <cctype>
#include <string>
#include <string_view>

#include "glaze/core/feature_test.hpp"

namespace glz
{
   inline constexpr char ascii_toupper(char c) noexcept
   {
      return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
   }

   inline constexpr char ascii_tolower(char c) noexcept
   {
      return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
   }

   inline constexpr bool is_upper(char c) noexcept { return c >= 'A' && c <= 'Z'; }

   inline constexpr bool is_lower(char c) noexcept { return c >= 'a' && c <= 'z'; }

   inline constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

   inline constexpr bool is_alpha(char c) noexcept { return is_upper(c) || is_lower(c); }

   inline constexpr bool is_alnum(char c) noexcept { return is_alpha(c) || is_digit(c); }

   // Convert snake_case to camelCase
   inline constexpr std::string to_camel_case(std::string_view sv)
   {
      std::string out;
      out.reserve(sv.size());
      bool upper_next = false;
      for (size_t i = 0; i < sv.size(); ++i) {
         char c = sv[i];
         if (c == '_') {
            // Skip underscore and set flag to uppercase next letter
            if (i + 1 < sv.size() && sv[i + 1] != '_') {
               upper_next = true;
            }
         }
         else {
            if (upper_next) {
               out.push_back(ascii_toupper(c));
               upper_next = false;
            }
            else {
               out.push_back(c);
            }
         }
      }
      return out;
   }

   // Convert snake_case to PascalCase (UpperCamelCase)
   inline constexpr std::string to_pascal_case(std::string_view sv)
   {
      std::string out;
      out.reserve(sv.size());
      bool upper_next = true; // Start with uppercase
      for (size_t i = 0; i < sv.size(); ++i) {
         char c = sv[i];
         if (c == '_' && i + 1 < sv.size()) {
            upper_next = true;
         }
         else {
            if (upper_next) {
               out.push_back(ascii_toupper(c));
               upper_next = false;
            }
            else {
               out.push_back(c);
            }
         }
      }
      return out;
   }

   // Convert camelCase/PascalCase to snake_case
   inline constexpr std::string to_snake_case(std::string_view sv)
   {
      std::string out;
      out.reserve(sv.size() * 2); // Reserve extra space for underscores

      for (size_t i = 0; i < sv.size(); ++i) {
         char c = sv[i];

         if (is_upper(c)) {
            // Don't add underscore at the beginning
            if (i > 0) {
               // Check if we should add underscore
               bool prev_is_lower_or_digit = is_lower(sv[i - 1]) || is_digit(sv[i - 1]);
               bool next_is_lower = i + 1 < sv.size() && is_lower(sv[i + 1]);
               bool prev_is_upper = i > 0 && is_upper(sv[i - 1]);

               // Add underscore if:
               // 1. Previous char is lowercase or digit
               // 2. This is the start of a new word in an acronym (prev is upper, next is lower)
               if (prev_is_lower_or_digit || (prev_is_upper && next_is_lower)) {
                  out.push_back('_');
               }
            }
            out.push_back(ascii_tolower(c));
         }
         else {
            out.push_back(c);
         }
      }

      return out;
   }

   // Convert camelCase/PascalCase/snake_case to SCREAMING_SNAKE_CASE
   inline constexpr std::string to_screaming_snake_case(std::string_view sv)
   {
      std::string out;
      out.reserve(sv.size() * 2);

      for (size_t i = 0; i < sv.size(); ++i) {
         char c = sv[i];

         if (c == '_') {
            out.push_back('_');
         }
         else if (is_upper(c)) {
            // Don't add underscore at the beginning
            if (i > 0 && sv[i - 1] != '_') {
               // Check if we should add underscore
               bool prev_is_lower_or_digit = is_lower(sv[i - 1]) || is_digit(sv[i - 1]);
               bool next_is_lower = i + 1 < sv.size() && is_lower(sv[i + 1]);
               bool prev_is_upper = i > 0 && is_upper(sv[i - 1]);

               // Add underscore if:
               // 1. Previous char is lowercase or digit
               // 2. This is the start of a new word in an acronym (prev is upper, next is lower)
               if (prev_is_lower_or_digit || (prev_is_upper && next_is_lower)) {
                  out.push_back('_');
               }
            }
            out.push_back(c);
         }
         else {
            out.push_back(ascii_toupper(c));
         }
      }

      return out;
   }

   // Convert any case to kebab-case
   // Simplified version to work around Clang constexpr limitations
   inline constexpr std::string to_kebab_case(std::string_view sv)
   {
      std::string out;
      out.reserve(sv.size() * 2);

      char prev = '\0';
      for (size_t i = 0; i < sv.size(); ++i) {
         char c = sv[i];

         if (c == '_') {
            out.push_back('-');
         }
         else if (is_upper(c)) {
            // Add dash before uppercase if previous was lowercase/digit
            if (i > 0 && prev != '_' && prev != '-' && (is_lower(prev) || is_digit(prev))) {
               out.push_back('-');
            }
            // Also add dash if this starts a new word in acronym
            else if (i > 0 && i + 1 < sv.size() && is_upper(prev) && is_lower(sv[i + 1])) {
               out.push_back('-');
            }
            out.push_back(ascii_tolower(c));
         }
         else {
            out.push_back(c);
         }
         prev = c;
      }

      return out;
   }

   // Convert any case to SCREAMING-KEBAB-CASE
   inline constexpr std::string to_screaming_kebab_case(std::string_view sv)
   {
      std::string out;
      out.reserve(sv.size() * 2);

      for (size_t i = 0; i < sv.size(); ++i) {
         char c = sv[i];

         if (c == '_') {
            out.push_back('-');
         }
         else if (is_upper(c)) {
            // Don't add dash at the beginning
            if (i > 0 && sv[i - 1] != '_') {
               // Check if we should add dash
               bool prev_is_lower_or_digit = is_lower(sv[i - 1]) || is_digit(sv[i - 1]);
               bool next_is_lower = i + 1 < sv.size() && is_lower(sv[i + 1]);
               bool prev_is_upper = i > 0 && is_upper(sv[i - 1]);

               // Add dash if:
               // 1. Previous char is lowercase or digit
               // 2. This is the start of a new word in an acronym (prev is upper, next is lower)
               if (prev_is_lower_or_digit || (prev_is_upper && next_is_lower)) {
                  out.push_back('-');
               }
            }
            out.push_back(c);
         }
         else {
            out.push_back(ascii_toupper(c));
         }
      }

      return out;
   }

   // Convert to lowercase (simple case conversion)
   inline constexpr std::string to_lower_case(std::string_view sv)
   {
      std::string out;
      out.reserve(sv.size());
      for (char c : sv) {
         out.push_back(ascii_tolower(c));
      }
      return out;
   }

   // Convert to UPPERCASE (simple case conversion)
   inline constexpr std::string to_upper_case(std::string_view sv)
   {
      std::string out;
      out.reserve(sv.size());
      for (char c : sv) {
         out.push_back(ascii_toupper(c));
      }
      return out;
   }

   // Struct wrappers for inheritance-based usage
   // These require constexpr std::string support (not available with _GLIBCXX_USE_CXX11_ABI=0)
#if GLZ_HAS_CONSTEXPR_STRING
   struct camel_case
   {
      static constexpr std::string rename_key(const auto key) { return to_camel_case(key); }
   };

   struct pascal_case
   {
      static constexpr std::string rename_key(const auto key) { return to_pascal_case(key); }
   };

   struct snake_case
   {
      static constexpr std::string rename_key(const auto key) { return to_snake_case(key); }
   };

   struct screaming_snake_case
   {
      static constexpr std::string rename_key(const auto key) { return to_screaming_snake_case(key); }
   };

   struct kebab_case
   {
      static constexpr std::string rename_key(const auto key) { return to_kebab_case(key); }
   };

   struct screaming_kebab_case
   {
      static constexpr std::string rename_key(const auto key) { return to_screaming_kebab_case(key); }
   };

   struct lower_case
   {
      static constexpr std::string rename_key(const auto key) { return to_lower_case(key); }
   };

   struct upper_case
   {
      static constexpr std::string rename_key(const auto key) { return to_upper_case(key); }
   };
#endif
}