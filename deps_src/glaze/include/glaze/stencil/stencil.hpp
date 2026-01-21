// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/core/read.hpp"
#include "glaze/core/reflect.hpp"
#include "glaze/core/write.hpp"

namespace glz
{
   // HTML escape function
   inline std::string html_escape(const std::string& input)
   {
      std::string result;
      result.reserve(static_cast<size_t>(input.size() * 1.1)); // Reserve some extra space

      for (char c : input) {
         switch (c) {
         case '<':
            result += "&lt;";
            break;
         case '>':
            result += "&gt;";
            break;
         case '&':
            result += "&amp;";
            break;
         case '"':
            result += "&quot;";
            break;
         case '\'':
            result += "&#x27;";
            break;
         default:
            result += c;
            break;
         }
      }
      return result;
   }

   template <auto Opts = opts{.format = STENCIL}, class Template, class T, resizable Buffer>
   [[nodiscard]] error_ctx stencil(Template&& layout, T&& value, Buffer& buffer)
   {
      context ctx{};

      if (layout.empty()) [[unlikely]] {
         ctx.error = error_code::no_read_input;
         return {0, ctx.error, ctx.custom_error_message};
      }

      auto p = read_iterators<Opts, false>(layout);
      auto it = p.first;
      auto end = p.second;
      auto outer_start = it;

      if (not bool(ctx.error)) [[likely]] {
         auto skip_whitespace = [&] {
            while (it < end && whitespace_table[uint8_t(*it)]) {
               ++it;
            }
         };

         while (it < end) {
            if (*it == '{') {
               ++it;
               if (it != end && *it == '{') {
                  ++it;

                  // Check for triple braces (unescaped HTML)
                  bool is_triple_brace = false;
                  if (it != end && *it == '{') {
                     ++it;
                     is_triple_brace = true;
                  }

                  bool is_section = false;
                  bool is_inverted_section = false;
                  bool is_comment = false;

                  if (it != end && !is_triple_brace) {
                     if (*it == '!') {
                        ++it;
                        is_comment = true;
                     }
                     else if (*it == '#') {
                        ++it;
                        is_section = true;
                     }
                     else if (*it == '^') {
                        ++it;
                        is_inverted_section = true;
                     }
                  }

                  skip_whitespace();

                  auto start = it;
                  while (it != end && *it != '}' && *it != ' ' && *it != '\t') {
                     ++it;
                  }

                  if (it == end) {
                     ctx.error = error_code::unexpected_end;
                     return {size_t(it - outer_start), ctx.error, ctx.custom_error_message};
                  }

                  const sv key{start, size_t(it - start)};

                  skip_whitespace();

                  if (is_comment) {
                     while (it < end && !(it + 1 < end && *it == '}' && *(it + 1) == '}')) {
                        ++it;
                     }
                     if (it + 1 < end) {
                        it += 2; // Skip '}}'
                     }
                     continue;
                  }

                  if (is_section || is_inverted_section) {
                     // Find the closing tag '{{/key}}'
                     std::string closing_tag = "{{/" + std::string(key) + "}}";
                     auto closing_pos = std::search(it, end, closing_tag.begin(), closing_tag.end());

                     if (closing_pos == end) {
                        ctx.error = error_code::unexpected_end;
                        return {size_t(it - outer_start), ctx.error, "Closing tag not found for section"};
                     }

                     if (it + 1 < end) {
                        it += 2; // Skip '}}'
                     }

                     // Extract inner template between current position and closing tag
                     std::string_view inner_template(it, closing_pos);
                     it = closing_pos + closing_tag.size();

                     // Retrieve the value associated with 'key'
                     bool condition = false;
                     bool is_container = false;

                     {
                        static constexpr auto N = reflect<T>::size;
                        static constexpr auto HashInfo = hash_info<T>;

                        const auto index =
                           decode_hash_with_size<STENCIL, T, HashInfo, HashInfo.type>::op(start, end, key.size());

                        if (index >= N) {
                           ctx.error = error_code::unknown_key;
                           return {size_t(it - outer_start), ctx.error, ctx.custom_error_message};
                        }
                        else {
                           visit<N>(
                              [&]<size_t I>() {
                                 static constexpr auto TargetKey = get<I>(reflect<T>::keys);
                                 if (TargetKey == key) [[likely]] {
                                    using field_type = refl_t<T, I>;

                                    if constexpr (bool_t<field_type>) {
                                       // Boolean field
                                       if constexpr (reflectable<T>) {
                                          condition = bool(get_member(value, get<I>(to_tie(value))));
                                       }
                                       else if constexpr (glaze_object_t<T>) {
                                          condition = bool(get_member(value, get<I>(reflect<T>::values)));
                                       }
                                    }
                                    else if constexpr (writable_array_t<field_type>) {
                                       // Container field - check if empty for condition
                                       is_container = true;

                                       if constexpr (reflectable<T>) {
                                          auto& container = get_member(value, get<I>(to_tie(value)));
                                          condition = !empty_range(container);

                                          // Process container iteration for regular sections
                                          if (is_section && condition) {
                                             using element_type = std::decay_t<decltype(*std::begin(container))>;
                                             if constexpr (reflectable<element_type> || glaze_object_t<element_type>) {
                                                for (const auto& item : container) {
                                                   std::string inner_buffer;
                                                   auto inner_ec = stencil<Opts>(inner_template, item, inner_buffer);
                                                   if (inner_ec) {
                                                      ctx.error = inner_ec.ec;
                                                      return;
                                                   }
                                                   buffer.append(inner_buffer);
                                                }
                                             }
                                             else {
                                                // For primitive containers, we can't do recursive stencil
                                                // This would require special handling for {{.}} syntax
                                                ctx.error = error_code::syntax_error;
                                                return;
                                             }
                                          }
                                       }
                                       else if constexpr (glaze_object_t<T>) {
                                          auto& container = get_member(value, get<I>(reflect<T>::values));
                                          condition = !empty_range(container);

                                          // Process container iteration for regular sections
                                          if (is_section && condition) {
                                             using element_type = std::decay_t<decltype(*std::begin(container))>;
                                             if constexpr (reflectable<element_type> || glaze_object_t<element_type>) {
                                                for (const auto& item : container) {
                                                   std::string inner_buffer;
                                                   auto inner_ec = stencil<Opts>(inner_template, item, inner_buffer);
                                                   if (inner_ec) {
                                                      ctx.error = inner_ec.ec;
                                                      return;
                                                   }
                                                   buffer.append(inner_buffer);
                                                }
                                             }
                                             else {
                                                // For primitive containers, we can't do recursive stencil
                                                // This would require special handling for {{.}} syntax
                                                ctx.error = error_code::syntax_error;
                                                return;
                                             }
                                          }
                                       }
                                    }
                                    else {
                                       // For other types, default to false for sections
                                       condition = false;
                                    }
                                 }
                                 else {
                                    ctx.error = error_code::unknown_key;
                                 }
                              },
                              index);
                        }
                     }

                     if (bool(ctx.error)) [[unlikely]] {
                        return {size_t(it - outer_start), ctx.error, ctx.custom_error_message};
                     }

                     // Handle inverted sections and boolean sections
                     if (is_inverted_section) {
                        // For inverted sections, show content if condition is false
                        if (!condition) {
                           std::string inner_buffer;
                           auto inner_ec = stencil<Opts>(inner_template, value, inner_buffer);
                           if (inner_ec) {
                              return inner_ec;
                           }
                           buffer.append(inner_buffer);
                        }
                     }
                     else if (is_section && !is_container) {
                        // For boolean sections (non-containers), show content if condition is true
                        if (condition) {
                           std::string inner_buffer;
                           auto inner_ec = stencil<Opts>(inner_template, value, inner_buffer);
                           if (inner_ec) {
                              return inner_ec;
                           }
                           buffer.append(inner_buffer);
                        }
                     }
                     // Container iteration for regular sections was already handled above

                     while (it < end && whitespace_table[uint8_t(*it)]) {
                        buffer.push_back(*it);
                        ++it;
                     }
                     continue;
                  }

                  // Handle regular placeholder (double braces) or unescaped (triple braces)
                  static constexpr auto N = reflect<T>::size;
                  static constexpr auto HashInfo = hash_info<T>;

                  const auto index =
                     decode_hash_with_size<STENCIL, T, HashInfo, HashInfo.type>::op(start, end, key.size());

                  if (index >= N) [[unlikely]] {
                     ctx.error = error_code::unknown_key;
                     return {size_t(it - outer_start), ctx.error, ctx.custom_error_message};
                  }
                  else [[likely]] {
                     // For triple braces, we need to expect three closing braces
                     size_t expected_closing_braces = is_triple_brace ? 3 : 2;

                     // Check for correct closing braces
                     size_t closing_brace_count = 0;
                     auto temp_it = it;
                     while (temp_it < end && *temp_it == '}' && closing_brace_count < 3) {
                        ++temp_it;
                        ++closing_brace_count;
                     }

                     if (closing_brace_count < expected_closing_braces) {
                        ctx.error = error_code::syntax_error;
                        return {size_t(it - outer_start), ctx.error, ctx.custom_error_message};
                     }

                     // Serialize the value
                     std::string temp_buffer;
                     static constexpr auto RawOpts =
                        set_json<opt_true<Opts, unquoted_opt_tag{}>>(); // write out string like values without quotes

                     visit<N>(
                        [&]<size_t I>() {
                           static constexpr auto TargetKey = get<I>(reflect<T>::keys);
                           if ((TargetKey.size() == key.size()) && comparitor<TargetKey>(start)) [[likely]] {
                              size_t ix = 0;
                              temp_buffer.resize(2 * write_padding_bytes);

                              if constexpr (reflectable<T>) {
                                 serialize<JSON>::template op<RawOpts>(get_member(value, get<I>(to_tie(value))), ctx,
                                                                       temp_buffer, ix);
                              }
                              else if constexpr (glaze_object_t<T>) {
                                 serialize<JSON>::template op<RawOpts>(get_member(value, get<I>(reflect<T>::values)),
                                                                       ctx, temp_buffer, ix);
                              }

                              temp_buffer.resize(ix);
                           }
                           else {
                              ctx.error = error_code::unknown_key;
                           }
                        },
                        index);

                     if (bool(ctx.error)) [[unlikely]] {
                        return {size_t(it - outer_start), ctx.error, ctx.custom_error_message};
                     }

                     // Apply HTML escaping for double braces, leave unescaped for triple braces
                     if (is_triple_brace) {
                        buffer.append(temp_buffer);
                     }
                     else {
                        if constexpr (Opts.format == MUSTACHE) {
                           buffer.append(html_escape(temp_buffer));
                        }
                        else {
                           buffer.append(temp_buffer);
                        }
                     }

                     // Skip the closing braces
                     it += expected_closing_braces;
                     continue;
                  }
               }
               else {
                  buffer.append("{");
                  // 'it' is already incremented past the first '{'
               }
            }
            else {
               buffer.push_back(*it);
               ++it;
            }
         }
      }

      if (bool(ctx.error)) [[unlikely]] {
         return {size_t(it - outer_start), ctx.error, ctx.custom_error_message};
      }

      return {};
   }

   template <auto Opts = opts{.format = STENCIL}, class Template, class T>
   [[nodiscard]] expected<std::string, error_ctx> stencil(Template&& layout, T&& value)
   {
      std::string buffer{};
      auto ec = stencil<Opts>(std::forward<Template>(layout), std::forward<T>(value), buffer);
      if (ec) {
         return unexpected<error_ctx>(ec);
      }
      return {buffer};
   }

   template <auto Opts = opts{.format = MUSTACHE}, class Template, class T, resizable Buffer>
      requires(Opts.format == MUSTACHE)
   [[nodiscard]] error_ctx mustache(Template&& layout, T&& value, Buffer& buffer)
   {
      return stencil<Opts>(std::forward<Template>(layout), std::forward<T>(value), buffer);
   }

   template <auto Opts = opts{.format = MUSTACHE}, class Template, class T>
      requires(Opts.format == MUSTACHE)
   [[nodiscard]] expected<std::string, error_ctx> mustache(Template&& layout, T&& value)
   {
      std::string buffer{};
      auto ec = stencil<Opts>(std::forward<Template>(layout), std::forward<T>(value), buffer);
      if (ec) {
         return unexpected<error_ctx>(ec);
      }
      return {buffer};
   }
}
