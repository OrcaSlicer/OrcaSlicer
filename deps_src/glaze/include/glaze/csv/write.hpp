// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/core/buffer_traits.hpp"
#include "glaze/core/opts.hpp"
#include "glaze/core/write.hpp"
#include "glaze/core/write_chars.hpp"
#include "glaze/util/dump.hpp"
#include "glaze/util/for_each.hpp"

namespace glz
{
   template <>
   struct serialize<CSV>
   {
      template <auto Opts, class T, is_context Ctx, class B, class IX>
      static void op(T&& value, Ctx&& ctx, B&& b, IX&& ix)
      {
         to<CSV, std::decay_t<T>>::template op<Opts>(std::forward<T>(value), std::forward<Ctx>(ctx), std::forward<B>(b),
                                                     std::forward<IX>(ix));
      }
   };

   template <glaze_value_t T>
   struct to<CSV, T>
   {
      template <auto Opts, is_context Ctx, class B, class IX>
      static void op(auto&& value, Ctx&& ctx, B&& b, IX&& ix)
      {
         using V = decltype(get_member(std::declval<T>(), meta_wrapper_v<T>));
         to<CSV, V>::template op<Opts>(get_member(value, meta_wrapper_v<T>), std::forward<Ctx>(ctx), std::forward<B>(b),
                                       std::forward<IX>(ix));
      }
   };

   template <num_t T>
   struct to<CSV, T>
   {
      template <auto Opts, class B>
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         write_chars::op<Opts>(value, ctx, b, ix);
      }
   };

   template <bool_t T>
   struct to<CSV, T>
   {
      template <auto Opts, class B>
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
            return;
         }
         if (value) {
            dump('1', b, ix);
         }
         else {
            dump('0', b, ix);
         }
      }
   };

   template <writable_array_t T>
   struct to<CSV, T>
   {
      template <auto Opts, class B>
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         if constexpr (resizable<T>) {
            if constexpr (check_layout(Opts) == rowwise) {
               const auto n = value.size();
               for (size_t i = 0; i < n; ++i) {
                  serialize<CSV>::op<Opts>(value[i], ctx, b, ix);
                  if (bool(ctx.error)) [[unlikely]] {
                     return;
                  }

                  if (i != (n - 1)) {
                     if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
                        return;
                     }
                     dump(',', b, ix);
                     if constexpr (is_output_streaming<B>) {
                        flush_buffer(b, ix);
                     }
                  }
               }
            }
            else {
               static_assert(false_v<T>, "Dynamic arrays within dynamic arrays are unsupported");
            }
         }
         else {
            const auto n = value.size();
            for (size_t i = 0; i < n; ++i) {
               serialize<CSV>::op<Opts>(value[i], ctx, b, ix);
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }

               if (i != (n - 1)) {
                  if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
                     return;
                  }
                  dump(',', b, ix);
                  if constexpr (is_output_streaming<B>) {
                     flush_buffer(b, ix);
                  }
               }
            }
         }
      }
   };

   // Specialization for 2D arrays (e.g., std::vector<std::vector<T>>)
   template <writable_array_t T>
      requires(writable_array_t<typename T::value_type>)
   struct to<CSV, T>
   {
      using Row = typename T::value_type;

      template <auto Opts, class B>
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         if constexpr (check_layout(Opts) == rowwise) {
            // Write row by row
            const auto n_rows = value.size();
            for (size_t r = 0; r < n_rows; ++r) {
               const auto& row = value[r];
               const auto n_cols = row.size();

               for (size_t c = 0; c < n_cols; ++c) {
                  serialize<CSV>::op<Opts>(row[c], ctx, b, ix);

                  if (bool(ctx.error)) [[unlikely]] {
                     return;
                  }

                  if (c < n_cols - 1) {
                     if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
                        return;
                     }
                     dump(',', b, ix);
                     if constexpr (is_output_streaming<B>) {
                        flush_buffer(b, ix);
                     }
                  }
               }

               if (r < n_rows - 1) {
                  if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
                     return;
                  }
                  dump('\n', b, ix);
                  if constexpr (is_output_streaming<B>) {
                     flush_buffer(b, ix);
                  }
               }
            }

            // Always add a trailing newline for consistency
            if (n_rows > 0) {
               if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
                  return;
               }
               dump('\n', b, ix);
            }
         }
         else {
            // Write column by column (transpose)
            if (value.empty()) {
               return;
            }

            // Find maximum column count
            size_t max_cols = 0;
            for (const auto& row : value) {
               max_cols = std::max(max_cols, row.size());
            }

            // Write transposed data
            for (size_t c = 0; c < max_cols; ++c) {
               for (size_t r = 0; r < value.size(); ++r) {
                  if (c < value[r].size()) {
                     serialize<CSV>::op<Opts>(value[r][c], ctx, b, ix);

                     if (bool(ctx.error)) [[unlikely]] {
                        return;
                     }
                  }
                  // else write empty cell (nothing to write)

                  if (r < value.size() - 1) {
                     if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
                        return;
                     }
                     dump(',', b, ix);
                     if constexpr (is_output_streaming<B>) {
                        flush_buffer(b, ix);
                     }
                  }
               }

               if (c < max_cols - 1) {
                  if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
                     return;
                  }
                  dump('\n', b, ix);
                  if constexpr (is_output_streaming<B>) {
                     flush_buffer(b, ix);
                  }
               }
            }

            // Add trailing newline for consistency
            if (max_cols > 0) {
               if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
                  return;
               }
               dump('\n', b, ix);
            }
         }
      }
   };

   // Helper function to check if a string needs CSV quoting
   template <class Str>
   inline bool needs_csv_quoting(const Str& str)
   {
      for (const auto c : str) {
         if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            return true;
         }
      }
      return false;
   }

   // Dump a CSV string with proper quoting and escaping
   template <class B>
   inline void dump_csv_string(is_context auto& ctx, const sv str, B& b, size_t& ix)
   {
      if (needs_csv_quoting(str)) {
         // Need to quote this string - worst case: every char is a quote (doubled) plus surrounding quotes
         if (!ensure_space(ctx, b, ix + str.size() * 2 + 2 + write_padding_bytes)) [[unlikely]] {
            return;
         }

         dump('"', b, ix);

         // Write the string, escaping quotes by doubling them
         for (const auto c : str) {
            if (c == '"') {
               dump('"', b, ix);
               dump('"', b, ix);
            }
            else {
               b[ix] = c;
               ++ix;
            }
         }

         dump('"', b, ix);
      }
      else {
         // No special characters, write as-is
         if (!ensure_space(ctx, b, ix + str.size() + write_padding_bytes)) [[unlikely]] {
            return;
         }
         dump_maybe_empty(str, b, ix);
      }
   }

   // Dump a single character with CSV quoting if needed
   template <class B>
   inline void dump_csv_char(is_context auto& ctx, const char c, B& b, size_t& ix)
   {
      // Worst case: quoted double-quote = 4 chars (""")
      if (!ensure_space(ctx, b, ix + 4 + write_padding_bytes)) [[unlikely]] {
         return;
      }

      if (c == ',' || c == '"' || c == '\n' || c == '\r') {
         dump('"', b, ix);
         if (c == '"') {
            dump('"', b, ix);
            dump('"', b, ix);
         }
         else {
            b[ix] = c;
            ++ix;
         }
         dump('"', b, ix);
      }
      else {
         b[ix] = c;
         ++ix;
      }
   }

   template <class T>
      requires str_t<T> || char_t<T>
   struct to<CSV, T>
   {
      template <auto Opts, class B>
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         if constexpr (char_t<T>) {
            dump_csv_char(ctx, value, b, ix);
         }
         else {
            dump_csv_string(ctx, value, b, ix);
         }
      }
   };

   template <writable_map_t T>
   struct to<CSV, T>
   {
      template <auto Opts, class B>
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         if constexpr (check_layout(Opts) == rowwise) {
            for (auto& [name, data] : value) {
               if constexpr (check_use_headers(Opts)) {
                  if (!ensure_space(ctx, b, ix + name.size() + 2 + write_padding_bytes)) [[unlikely]] {
                     return;
                  }
                  dump_maybe_empty(name, b, ix);
                  dump(',', b, ix);
               }
               const auto n = data.size();
               for (size_t i = 0; i < n; ++i) {
                  serialize<CSV>::op<Opts>(data[i], ctx, b, ix);
                  if (bool(ctx.error)) [[unlikely]] {
                     return;
                  }
                  if (i < n - 1) {
                     if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
                        return;
                     }
                     dump(',', b, ix);
                     if constexpr (is_output_streaming<B>) {
                        flush_buffer(b, ix);
                     }
                  }
               }
               if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
                  return;
               }
               dump('\n', b, ix);
               if constexpr (is_output_streaming<B>) {
                  flush_buffer(b, ix);
               }
            }
         }
         else {
            // dump titles
            const auto n = value.size();
            if constexpr (check_use_headers(Opts)) {
               size_t i = 0;
               for (auto& [name, data] : value) {
                  if (!ensure_space(ctx, b, ix + name.size() + 2 + write_padding_bytes)) [[unlikely]] {
                     return;
                  }
                  dump_maybe_empty(name, b, ix);
                  ++i;
                  if (i < n) {
                     dump(',', b, ix);
                     if constexpr (is_output_streaming<B>) {
                        flush_buffer(b, ix);
                     }
                  }
               }
               if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
                  return;
               }
               dump('\n', b, ix);
               if constexpr (is_output_streaming<B>) {
                  flush_buffer(b, ix);
               }
            }

            size_t row = 0;
            bool end = false;
            while (true) {
               size_t i = 0;
               for (auto& [name, data] : value) {
                  if (row >= data.size()) {
                     end = true;
                     break;
                  }

                  serialize<CSV>::op<Opts>(data[row], ctx, b, ix);
                  if (bool(ctx.error)) [[unlikely]] {
                     return;
                  }
                  ++i;
                  if (i < n) {
                     if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
                        return;
                     }
                     dump(',', b, ix);
                     if constexpr (is_output_streaming<B>) {
                        flush_buffer(b, ix);
                     }
                  }
               }

               if (end) {
                  break;
               }

               if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
                  return;
               }
               dump('\n', b, ix);
               if constexpr (is_output_streaming<B>) {
                  flush_buffer(b, ix);
               }

               ++row;
            }
         }
      }
   };

   template <class T>
      requires((glaze_object_t<T> || reflectable<T>) && not custom_write<T>)
   struct to<CSV, T>
   {
      template <auto Opts, class B>
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         static constexpr auto N = reflect<T>::size;

         [[maybe_unused]] decltype(auto) t = [&] {
            if constexpr (reflectable<T>) {
               return to_tie(value);
            }
            else {
               return nullptr;
            }
         }();

         if constexpr (check_layout(Opts) == rowwise) {
            for_each<N>([&]<auto I>() {
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }
               using value_type = typename std::decay_t<refl_t<T, I>>::value_type;

               static constexpr sv key = reflect<T>::keys[I];

               decltype(auto) mem = [&]() -> decltype(auto) {
                  if constexpr (reflectable<T>) {
                     return get<I>(t);
                  }
                  else {
                     return get<I>(reflect<T>::values);
                  }
               }();

               if constexpr (writable_array_t<value_type>) {
                  decltype(auto) member = get_member(value, mem);
                  const auto count = member.size();
                  const auto size = member[0].size();
                  for (size_t i = 0; i < size; ++i) {
                     if constexpr (check_use_headers(Opts)) {
                        if (!ensure_space(ctx, b, ix + key.size() + 32 + write_padding_bytes)) [[unlikely]] {
                           return;
                        }
                        dump<key>(b, ix);
                        dump('[', b, ix);
                        write_chars::op<Opts>(i, ctx, b, ix);
                        dump(']', b, ix);
                        dump(',', b, ix);
                     }

                     for (size_t j = 0; j < count; ++j) {
                        serialize<CSV>::op<Opts>(member[j][i], ctx, b, ix);
                        if (bool(ctx.error)) [[unlikely]] {
                           return;
                        }
                        if (j != count - 1) {
                           if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
                              return;
                           }
                           dump(',', b, ix);
                        }
                     }

                     if (i != size - 1) {
                        if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
                           return;
                        }
                        dump('\n', b, ix);
                     }
                  }
               }
               else {
                  if constexpr (check_use_headers(Opts)) {
                     if (!ensure_space(ctx, b, ix + key.size() + 2 + write_padding_bytes)) [[unlikely]] {
                        return;
                     }
                     dump<key>(b, ix);
                     dump(',', b, ix);
                  }
                  serialize<CSV>::op<Opts>(get_member(value, mem), ctx, b, ix);
                  if (bool(ctx.error)) [[unlikely]] {
                     return;
                  }
                  if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
                     return;
                  }
                  dump('\n', b, ix);
               }
            });
         }
         else {
            // write titles
            if constexpr (check_use_headers(Opts)) {
               for_each<N>([&]<auto I>() {
                  if (bool(ctx.error)) [[unlikely]] {
                     return;
                  }
                  using X = refl_t<T, I>;

                  static constexpr sv key = reflect<T>::keys[I];

                  decltype(auto) member = [&]() -> decltype(auto) {
                     if constexpr (reflectable<T>) {
                        return get<I>(t);
                     }
                     else {
                        return get<I>(reflect<T>::values);
                     }
                  }();

                  if constexpr (fixed_array_value_t<X>) {
                     const auto size = get_member(value, member)[0].size();
                     for (size_t i = 0; i < size; ++i) {
                        if (!ensure_space(ctx, b, ix + key.size() + 32 + write_padding_bytes)) [[unlikely]] {
                           return;
                        }
                        dump<key>(b, ix);
                        dump('[', b, ix);
                        write_chars::op<Opts>(i, ctx, b, ix);
                        dump(']', b, ix);
                        if (i != size - 1) {
                           dump(',', b, ix);
                        }
                     }
                  }
                  else {
                     serialize<CSV>::op<Opts>(key, ctx, b, ix);
                  }

                  if constexpr (I != N - 1) {
                     if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
                        return;
                     }
                     dump(',', b, ix);
                  }
               });

               if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
                  return;
               }
               dump('\n', b, ix);
            }

            size_t row = 0;
            bool end = false;

            while (true) {
               for_each<N>([&]<auto I>() {
                  if (bool(ctx.error)) [[unlikely]] {
                     return;
                  }
                  using X = std::decay_t<refl_t<T, I>>;

                  decltype(auto) mem = [&]() -> decltype(auto) {
                     if constexpr (reflectable<T>) {
                        return get<I>(t);
                     }
                     else {
                        return get<I>(reflect<T>::values);
                     }
                  }();

                  if constexpr (fixed_array_value_t<X>) {
                     decltype(auto) member = get_member(value, mem);
                     if (row >= member.size()) {
                        end = true;
                        return;
                     }

                     const auto n = member[0].size();
                     for (size_t i = 0; i < n; ++i) {
                        serialize<CSV>::op<Opts>(member[row][i], ctx, b, ix);
                        if (bool(ctx.error)) [[unlikely]] {
                           return;
                        }
                        if (i != n - 1) {
                           if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
                              return;
                           }
                           dump(',', b, ix);
                        }
                     }
                  }
                  else {
                     decltype(auto) member = get_member(value, mem);
                     if (row >= member.size()) {
                        end = true;
                        return;
                     }

                     serialize<CSV>::op<Opts>(member[row], ctx, b, ix);
                     if (bool(ctx.error)) [[unlikely]] {
                        return;
                     }

                     if constexpr (I != N - 1) {
                        if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
                           return;
                        }
                        dump(',', b, ix);
                     }
                  }
               });

               if (end || bool(ctx.error)) {
                  break;
               }

               ++row;

               if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
                  return;
               }
               dump('\n', b, ix);
            }
         }
      }
   };

   // For types like std::vector<T> where T is a struct/object
   template <writable_array_t T>
      requires(glaze_object_t<typename T::value_type> || reflectable<typename T::value_type>)
   struct to<CSV, T>
   {
      using U = typename T::value_type;

      template <auto Opts, class B>
      static void op(auto&& value, is_context auto&& ctx, B&& b, auto& ix)
      {
         static constexpr auto N = reflect<U>::size;

         // Write headers (field names) if enabled
         if constexpr (check_use_headers(Opts)) {
            for_each<N>([&]<auto I>() {
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }
               static constexpr sv key = reflect<U>::keys[I];
               serialize<CSV>::op<Opts>(key, ctx, b, ix);

               if constexpr (I < N - 1) {
                  if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
                     return;
                  }
                  dump(',', b, ix);
               }
            });

            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
            if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
               return;
            }
            dump('\n', b, ix);
         }

         // Write each struct as a row
         for (const auto& item : value) {
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
            for_each<N>([&]<auto I>() {
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }
               decltype(auto) mem = [&]() -> decltype(auto) {
                  if constexpr (reflectable<U>) {
                     return get<I>(to_tie(item));
                  }
                  else {
                     return get<I>(reflect<U>::values);
                  }
               }();

               serialize<CSV>::op<Opts>(get_member(item, mem), ctx, b, ix);

               if constexpr (I < N - 1) {
                  if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
                     return;
                  }
                  dump(',', b, ix);
               }
            });

            if (bool(ctx.error)) [[unlikely]] {
               return;
            }
            if (!ensure_space(ctx, b, ix + 1 + write_padding_bytes)) [[unlikely]] {
               return;
            }
            dump('\n', b, ix);
         }
      }
   };

   template <uint32_t layout = rowwise, write_supported<CSV> T, class Buffer>
   [[nodiscard]] auto write_csv(T&& value, Buffer&& buffer)
   {
      return write<opts_csv{.layout = layout}>(std::forward<T>(value), std::forward<Buffer>(buffer));
   }

   template <uint32_t layout = rowwise, write_supported<CSV> T>
   [[nodiscard]] expected<std::string, error_ctx> write_csv(T&& value)
   {
      return write<opts_csv{.layout = layout}>(std::forward<T>(value));
   }

   template <uint32_t layout = rowwise, write_supported<CSV> T>
   [[nodiscard]] error_ctx write_file_csv(T&& value, const std::string& file_name, auto&& buffer)
   {
      const auto ec = write<opts_csv{.layout = layout}>(std::forward<T>(value), buffer);
      if (bool(ec)) [[unlikely]] {
         return ec;
      }
      const auto file_ec = buffer_to_file(buffer, file_name);
      if (bool(file_ec)) [[unlikely]] {
         return {0, file_ec};
      }
      return {buffer.size(), error_code::none};
   }
}
