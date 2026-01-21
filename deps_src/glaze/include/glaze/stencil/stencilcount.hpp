// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/core/read.hpp"
#include "glaze/core/reflect.hpp"
#include "glaze/core/write.hpp"
#include "glaze/format/format_to.hpp"

namespace glz
{
   template <auto Opts = opts{}, class Template, class T, resizable Buffer>
   [[nodiscard]] error_ctx stencilcount(Template&& layout, T&& value, Buffer& buffer)
   {
      context ctx{};

      if (layout.empty()) [[unlikely]] {
         ctx.error = error_code::no_read_input;
         return {0, ctx.error, ctx.custom_error_message};
      }

      auto [it, end] = read_iterators<Opts, false>(layout);
      auto outer_start = it;
      if (layout.empty()) [[unlikely]] {
         ctx.error = error_code::no_read_input;
      }
      if (not bool(ctx.error)) [[likely]] {
         auto skip_whitespace = [&] {
            while (whitespace_table[uint8_t(*it)]) {
               ++it;
            }
         };

         std::unordered_map<uint64_t, uint64_t> numbering{};
         uint64_t major_count{};
         uint64_t prev_count{};

         while (it < end) {
            switch (*it) {
            case '{': {
               ++it;
               if (*it == '{') {
                  ++it;
                  skip_whitespace();

                  uint64_t count{};
                  while (*it == '+') {
                     ++it;
                     ++count;
                  }

                  if (count < prev_count) {
                     numbering.clear();
                  }

                  if (count == 1) {
                     ++major_count;
                     format_to(buffer, major_count);
                     buffer.append(".");
                  }
                  else if (count > 1) {
                     format_to(buffer, major_count);

                     for (size_t i = 1; i < count; ++i) {
                        buffer.append(".");
                        auto& x = numbering[i];
                        if (i == (count - 1)) {
                           ++x;
                        }
                        format_to(buffer, x);
                     }
                  }

                  if (count > 0) {
                     prev_count = count;
                  }

                  if (*it == '}') {
                     ++it;
                     if (*it == '}') {
                        ++it;
                        break;
                     }
                     else {
                        buffer.append("}");
                     }
                     break;
                  }

                  auto start = it;
                  while (it != end && *it != '}' && *it != ' ' && *it != '\t') {
                     ++it;
                  }

                  const sv key{start, size_t(it - start)};

                  skip_whitespace();

                  static constexpr auto N = reflect<T>::size;
                  static constexpr auto HashInfo = hash_info<T>;

                  const auto index =
                     decode_hash_with_size<STENCIL, T, HashInfo, HashInfo.type>::op(start, end, key.size());

                  if (index < N) [[likely]] {
                     static thread_local std::string temp{};
                     visit<N>(
                        [&]<size_t I>() {
                           static constexpr auto TargetKey = get<I>(reflect<T>::keys);
                           static constexpr auto Length = TargetKey.size();
                           if ((Length == key.size()) && comparitor<TargetKey>(start)) [[likely]] {
                              if constexpr (reflectable<T> && N > 0) {
                                 std::ignore = write<opt_true<Opts, unquoted_opt_tag{}>>(
                                    get_member(value, get<I>(to_tie(value))), temp, ctx);
                              }
                              else if constexpr (glaze_object_t<T> && N > 0) {
                                 std::ignore = write<opt_true<Opts, unquoted_opt_tag{}>>(
                                    get_member(value, get<I>(reflect<T>::values)), temp, ctx);
                              }
                           }
                           else {
                              ctx.error = error_code::unknown_key;
                           }
                        },
                        index);

                     if (bool(ctx.error)) [[unlikely]] {
                        return {size_t(it - start), ctx.error, ctx.custom_error_message};
                     }

                     buffer.append(temp);
                  }
                  else {
                     // TODO: Is this an error?
                  }

                  skip_whitespace();

                  if (*it == '}') {
                     ++it;
                     if (*it == '}') {
                        ++it;
                        break;
                     }
                     else {
                        buffer.append("}");
                     }
                     break;
                  }
               }
               else {
                  buffer.push_back('{');
               }

               break;
            }
            default: {
               buffer.push_back(*it);
               ++it;
            }
            }
         }
      }

      if (bool(ctx.error)) [[unlikely]] {
         return {size_t(it - outer_start), ctx.error, ctx.custom_error_message};
      }

      return {};
   }

   template <auto Opts = opts{}, class Template, class T>
   [[nodiscard]] expected<std::string, error_ctx> stencilcount(Template&& layout, T&& value)
   {
      std::string buffer{};
      auto ec = stencilcount(std::forward<Template>(layout), std::forward<T>(value), buffer);
      if (ec) {
         return unexpected<error_ctx>(ec);
      }
      return {buffer};
   }
}
