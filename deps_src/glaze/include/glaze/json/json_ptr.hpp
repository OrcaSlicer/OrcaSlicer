// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <algorithm>
#include <any>
#include <charconv>

#include "glaze/core/seek.hpp"
#include "glaze/json/read.hpp"
#include "glaze/json/skip.hpp"
#include "glaze/util/parse.hpp"
#include "glaze/util/string_literal.hpp"

namespace glz
{
   [[nodiscard]] inline constexpr bool maybe_numeric_key(const sv key)
   {
      return key.find_first_not_of("0123456789") == std::string_view::npos;
   }

   template <string_literal JsonPointer, auto Opts = opts{}>
   [[nodiscard]] inline auto get_view_json(contiguous auto&& buffer)
   {
      static constexpr auto S = chars<JsonPointer>;
      static constexpr auto tokens = split_json_ptr<S>();
      static constexpr auto N = tokens.size();

      context ctx{};
      auto p = read_iterators<Opts>(buffer);

      auto it = p.first;
      auto end = p.second;

      // Don't automatically const qualify the buffer so we can write to the view,
      // which allows us to write to a JSON Pointer location
      using span_t =
         std::span<std::conditional_t<std::is_const_v<std::remove_pointer_t<decltype(it)>>, const char, char>>;
      using result_t = expected<span_t, error_ctx>;

      auto start = it;

      if (buffer.empty()) [[unlikely]] {
         ctx.error = error_code::no_read_input;
      }

      if (bool(ctx.error)) [[unlikely]] {
         return result_t{unexpected(error_ctx{0, ctx.error})};
      }

      if constexpr (N == 0) {
         return result_t{span_t{it, end}};
      }
      else {
         using namespace glz::detail;

         skip_ws<Opts>(ctx, it, end);

         result_t ret;

         for_each<N>([&]<size_t I>() {
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }

            static constexpr auto key = tokens[I];
            if constexpr (maybe_numeric_key(key)) {
               switch (*it) {
               case '{': {
                  ++it;
                  while (true) {
                     if (skip_ws<Opts>(ctx, it, end)) {
                        return;
                     }
                     if (match<'"'>(ctx, it)) {
                        return;
                     }

                     auto* start = it;
                     skip_string_view(ctx, it, end);
                     if (bool(ctx.error)) [[unlikely]]
                        return;
                     const sv k = {start, size_t(it - start)};
                     ++it;

                     if (key.size() == k.size() && comparitor<key>(k.data())) {
                        if (skip_ws<Opts>(ctx, it, end)) {
                           return;
                        }
                        if (match_invalid_end<':', Opts>(ctx, it, end)) {
                           return;
                        }
                        if (skip_ws<Opts>(ctx, it, end)) {
                           return;
                        }

                        if constexpr (I == (N - 1)) {
                           ret = parse_value<Opts>(ctx, it, end);
                        }
                        return;
                     }
                     else {
                        skip_value<JSON>::op<Opts>(ctx, it, end);
                        if (bool(ctx.error)) [[unlikely]] {
                           return;
                        }
                        if (*it != ',') {
                           ctx.error = error_code::key_not_found;
                           return;
                        }
                        ++it;
                     }
                  }
               }
               case '[': {
                  ++it;
                  // Could optimize by counting commas
                  static constexpr auto n = stoui(key);
                  if constexpr (n) {
                     for_each<n.value()>([&]<size_t>() {
                        skip_value<JSON>::op<Opts>(ctx, it, end);
                        if (bool(ctx.error)) [[unlikely]] {
                           return;
                        }
                        if (*it != ',') {
                           ctx.error = error_code::array_element_not_found;
                           return;
                        }
                        ++it;
                     });

                     if (skip_ws<Opts>(ctx, it, end)) {
                        return;
                     }

                     if constexpr (I == (N - 1)) {
                        ret = parse_value<Opts>(ctx, it, end);
                     }
                     return;
                  }
                  else {
                     ctx.error = error_code::array_element_not_found;
                     return;
                  }
               }
               }
            }
            else {
               if (match_invalid_end<'{', Opts>(ctx, it, end)) {
                  return;
               }

               while (it < end) {
                  if (skip_ws<Opts>(ctx, it, end)) {
                     return;
                  }
                  if (match<'"'>(ctx, it)) {
                     return;
                  }

                  auto* start = it;
                  skip_string_view(ctx, it, end);
                  if (bool(ctx.error)) [[unlikely]]
                     return;
                  const sv k = {start, size_t(it - start)};
                  ++it;

                  if (key.size() == k.size() && comparitor<key>(k.data())) {
                     if (skip_ws<Opts>(ctx, it, end)) {
                        return;
                     }
                     if (match_invalid_end<':', Opts>(ctx, it, end)) {
                        return;
                     }
                     if (skip_ws<Opts>(ctx, it, end)) {
                        return;
                     }

                     if constexpr (I == (N - 1)) {
                        ret = parse_value<Opts>(ctx, it, end);
                     }
                     return;
                  }
                  else {
                     skip_value<JSON>::op<Opts>(ctx, it, end);
                     if (bool(ctx.error)) [[unlikely]] {
                        return;
                     }
                     if (*it != ',') {
                        ctx.error = error_code::key_not_found;
                        return;
                     }
                     ++it;
                  }
               }
            }
         });

         if (bool(ctx.error)) [[unlikely]] {
            return result_t{unexpected(error_ctx{size_t(it - start), ctx.error})};
         }

         return ret;
      }
   }

   template <class T, string_literal Str, auto Opts = opts{}>
   [[nodiscard]] inline expected<T, error_ctx> get_as_json(contiguous auto&& buffer)
   {
      const auto str = glz::get_view_json<Str>(buffer);
      if (str) {
         return glz::read_json<T>(*str);
      }
      return unexpected(str.error());
   }

   template <string_literal Str, auto Opts = opts{}>
   [[nodiscard]] inline expected<sv, error_ctx> get_sv_json(contiguous auto&& buffer)
   {
      const auto s = glz::get_view_json<Str>(buffer);
      if (s) {
         return sv{reinterpret_cast<const char*>(s->data()), s->size()};
      }
      return unexpected(s.error());
   }

   // Write raw text to a JSON value denoted by a JSON Pointer
   template <string_literal Path, auto Opts = opts{}>
   [[nodiscard]] inline error_ctx write_at(const std::string_view value, contiguous auto&& buffer)
   {
      auto view = glz::get_view_json<Path, Opts>(buffer);
      if (view) {
         // erase the current value
         const size_t location = size_t(view->data() - buffer.data());
         buffer.erase(location, view->size());
         // insert the new value
         buffer.insert(location, value);
         return {};
      }
      else {
         return view.error();
      }
   }

   namespace detail
   {
      // Check if a string could be a numeric array index
      inline bool runtime_maybe_numeric(const std::string& s)
      {
         return !s.empty() && s.find_first_not_of("0123456789") == std::string::npos;
      }
   } // namespace detail

   // Runtime version of get_view_json - navigate to a JSON value using a runtime JSON pointer
   template <auto Opts = opts{}>
   [[nodiscard]] inline auto get_view_json(const sv json_ptr, contiguous auto&& buffer)
   {
      using namespace glz::detail;

      context ctx{};
      auto [it, end] = read_iterators<Opts>(buffer);

      using span_t =
         std::span<std::conditional_t<std::is_const_v<std::remove_pointer_t<decltype(it)>>, const char, char>>;
      using result_t = expected<span_t, error_ctx>;

      auto start = it;

      if (buffer.empty()) [[unlikely]] {
         return result_t{unexpected(error_ctx{0, error_code::no_read_input})};
      }

      // Empty pointer means return the whole document
      if (json_ptr.empty()) {
         return result_t{span_t{it, end}};
      }

      sv remaining_ptr = json_ptr;

      while (!remaining_ptr.empty()) {
         auto [token, next_remaining] = parse_json_ptr_token(remaining_ptr);
         if (token.empty() && remaining_ptr.size() > 0 && remaining_ptr[0] == '/') {
            // Empty token after '/' is valid (represents empty string key)
            // But if we got here with no token and remaining_ptr doesn't start with '/',
            // it's an error
         }
         remaining_ptr = next_remaining;
         const bool is_last = remaining_ptr.empty();

         if (skip_ws<Opts>(ctx, it, end)) {
            return result_t{unexpected(error_ctx{size_t(it - start), ctx.error})};
         }

         if (it >= end) {
            return result_t{unexpected(error_ctx{size_t(it - start), error_code::unexpected_end})};
         }

         const bool is_numeric = runtime_maybe_numeric(token);

         if (*it == '{') {
            ++it;
            bool found = false;

            while (true) {
               if (skip_ws<Opts>(ctx, it, end)) {
                  return result_t{unexpected(error_ctx{size_t(it - start), ctx.error})};
               }

               if (it >= end) {
                  return result_t{unexpected(error_ctx{size_t(it - start), error_code::unexpected_end})};
               }

               if (*it == '}') {
                  break; // key not found
               }

               if (*it != '"') {
                  return result_t{unexpected(error_ctx{size_t(it - start), error_code::expected_quote})};
               }
               ++it;

               auto key_start = it;
               skip_string_view(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]] {
                  return result_t{unexpected(error_ctx{size_t(it - start), ctx.error})};
               }
               const sv key_content{key_start, size_t(it - key_start)};
               ++it; // skip closing quote

               if (skip_ws<Opts>(ctx, it, end)) {
                  return result_t{unexpected(error_ctx{size_t(it - start), ctx.error})};
               }

               if (it >= end || *it != ':') {
                  return result_t{unexpected(error_ctx{
                     size_t(it - start), it >= end ? error_code::unexpected_end : error_code::expected_colon})};
               }
               ++it;

               if (skip_ws<Opts>(ctx, it, end)) {
                  return result_t{unexpected(error_ctx{size_t(it - start), ctx.error})};
               }

               if (token.size() == key_content.size() && std::equal(token.begin(), token.end(), key_content.begin())) {
                  found = true;
                  if (is_last) {
                     return result_t{parse_value<Opts>(ctx, it, end)};
                  }
                  break; // continue to next token
               }

               // Skip this value
               skip_value<JSON>::op<Opts>(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]] {
                  return result_t{unexpected(error_ctx{size_t(it - start), ctx.error})};
               }

               if (skip_ws<Opts>(ctx, it, end)) {
                  return result_t{unexpected(error_ctx{size_t(it - start), ctx.error})};
               }

               if (it < end && *it == ',') {
                  ++it;
               }
            }

            if (!found) {
               return result_t{unexpected(error_ctx{size_t(it - start), error_code::key_not_found})};
            }
         }
         else if (*it == '[') {
            if (!is_numeric) {
               return result_t{unexpected(error_ctx{size_t(it - start), error_code::array_element_not_found})};
            }

            size_t index{};
            auto [p, ec] = std::from_chars(token.data(), token.data() + token.size(), index);
            if (ec != std::errc{}) {
               return result_t{unexpected(error_ctx{size_t(it - start), error_code::array_element_not_found})};
            }

            ++it; // skip '['

            for (size_t i = 0; i < index; ++i) {
               if (skip_ws<Opts>(ctx, it, end)) {
                  return result_t{unexpected(error_ctx{size_t(it - start), ctx.error})};
               }
               if (it >= end || *it == ']') {
                  return result_t{unexpected(error_ctx{size_t(it - start), error_code::array_element_not_found})};
               }
               skip_value<JSON>::op<Opts>(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]] {
                  return result_t{unexpected(error_ctx{size_t(it - start), ctx.error})};
               }
               if (skip_ws<Opts>(ctx, it, end)) {
                  return result_t{unexpected(error_ctx{size_t(it - start), ctx.error})};
               }
               if (it < end && *it == ',') {
                  ++it;
               }
            }

            if (skip_ws<Opts>(ctx, it, end)) {
               return result_t{unexpected(error_ctx{size_t(it - start), ctx.error})};
            }
            if (it >= end || *it == ']') {
               return result_t{unexpected(error_ctx{size_t(it - start), error_code::array_element_not_found})};
            }

            if (is_last) {
               return result_t{parse_value<Opts>(ctx, it, end)};
            }
         }
         else {
            return result_t{unexpected(error_ctx{size_t(it - start), error_code::syntax_error})};
         }
      }

      return result_t{unexpected(error_ctx{size_t(it - start), error_code::syntax_error})};
   }

   // Runtime version of write_at - write a JSON value at a runtime JSON pointer location
   template <auto Opts = opts{}>
   [[nodiscard]] inline error_ctx write_at(const sv json_ptr, const sv value, contiguous auto&& buffer)
   {
      auto view = glz::get_view_json<Opts>(json_ptr, buffer);
      if (view) {
         const size_t location = size_t(view->data() - buffer.data());
         buffer.erase(location, view->size());
         buffer.insert(location, value);
         return {};
      }
      return view.error();
   }
}
