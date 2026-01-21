// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/json/read.hpp"
#include "glaze/json/write.hpp"

namespace glz
{
   template <>
   struct parse<NDJSON>
   {
      template <auto Opts, class T, is_context Ctx, class It0, class It1>
      static void op(T&& value, Ctx&& ctx, It0&& it, It1&& end)
      {
         from<NDJSON, std::remove_reference_t<T>>::template op<Opts>(std::forward<T>(value), std::forward<Ctx>(ctx),
                                                                     std::forward<It0>(it), std::forward<It1>(end));
      }
   };

   template <>
   struct serialize<NDJSON>
   {
      template <auto Opts, class T, is_context Ctx, class B, class IX>
      static void op(T&& value, Ctx&& ctx, B&& b, IX&& ix)
      {
         to<NDJSON, std::decay_t<T>>::template op<Opts>(std::forward<T>(value), std::forward<Ctx>(ctx),
                                                        std::forward<B>(b), std::forward<IX>(ix));
      }
   };

   template <class T>
      requires readable_array_t<T> && (emplace_backable<T> || !resizable<T>)
   struct from<NDJSON, T>
   {
      template <auto Opts>
      static void op(auto& value, is_context auto&& ctx, auto&& it, auto end)
      {
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         if (it == end) {
            if constexpr (resizable<T>) {
               value.clear();

               if constexpr (check_shrink_to_fit(Opts)) {
                  value.shrink_to_fit();
               }
            }
         }

         const auto n = value.size();

         auto value_it = value.begin();

         auto read_new_lines = [&] {
            while (*it == '\r') {
               ++it;
               if (*it == '\n') {
                  ++it;
               }
               else {
                  ctx.error = error_code::syntax_error; // Expected '\n' after '\r'
                  return;
               }
            }
            while (*it == '\n') {
               ++it;
            }
         };

         for (size_t i = 0; i < n; ++i) {
            parse<JSON>::op<Opts>(*value_it++, ctx, it, end);
            if (it == end) {
               if constexpr (erasable<T>) {
                  value.erase(value_it,
                              value.end()); // use erase rather than resize for non-default constructible elements

                  if constexpr (check_shrink_to_fit(Opts)) {
                     value.shrink_to_fit();
                  }
               }
               return;
            }

            read_new_lines();
         }

         // growing
         if constexpr (emplace_backable<T>) {
            while (it < end) {
               parse<JSON>::op<Opts>(value.emplace_back(), ctx, it, end);
               if (bool(ctx.error)) {
                  return;
               }

               read_new_lines();
            }
         }
         else {
            ctx.error = error_code::exceeded_static_array_size;
         }
      }
   };

   template <class T>
      requires glaze_array_t<T> || tuple_t<T> || is_std_tuple<T>
   struct from<NDJSON, T>
   {
      template <auto Opts>
      static void op(auto& value, is_context auto&& ctx, auto&& it, auto end)
      {
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         static constexpr auto N = []() constexpr {
            if constexpr (glaze_array_t<T>) {
               return reflect<T>::size;
            }
            else {
               return glz::tuple_size_v<T>;
            }
         }();

         auto read_new_lines = [&] {
            while (*it == '\r') {
               ++it;
               if (*it == '\n') {
                  ++it;
               }
               else {
                  ctx.error = error_code::syntax_error; // Expected '\n' after '\r'
                  return;
               }
            }
            while (*it == '\n') {
               ++it;
            }
         };

         for_each<N>([&]<auto I>() {
            if (it == end) {
               return;
            }
            if constexpr (I != 0) {
               read_new_lines();
            }
            if constexpr (is_std_tuple<T>) {
               parse<JSON>::op<Opts>(std::get<I>(value), ctx, it, end);
            }
            else if constexpr (glaze_array_t<T>) {
               parse<JSON>::op<Opts>(get_member(value, glz::get<I>(meta_v<T>)), ctx, it, end);
            }
            else {
               parse<JSON>::op<Opts>(glz::get<I>(value), ctx, it, end);
            }
         });
      }
   };

   template <writable_array_t T>
   struct to<NDJSON, T>
   {
      template <auto Opts, class... Args>
      static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         const auto is_empty = [&]() -> bool {
            if constexpr (has_size<T>) {
               return value.size() ? false : true;
            }
            else {
               return value.empty();
            }
         }();

         if (!is_empty) {
            auto it = value.begin();
            using Value = core_t<decltype(*it)>;
            to<JSON, Value>::template op<Opts>(*it, ctx, b, ix);
            ++it;
            const auto end = value.end();
            for (; it != end; ++it) {
               dump('\n', b, ix);
               to<JSON, Value>::template op<Opts>(*it, ctx, b, ix);
            }
         }
      }
   };

   template <class T>
      requires glaze_array_t<T> || tuple_t<T>
   struct to<NDJSON, T>
   {
      template <auto Opts, class... Args>
      static void op(auto&& value, is_context auto&& ctx, Args&&... args)
      {
         static constexpr auto N = []() constexpr {
            if constexpr (glaze_array_t<std::decay_t<T>>) {
               return glz::tuple_size_v<meta_t<std::decay_t<T>>>;
            }
            else {
               return glz::tuple_size_v<std::decay_t<T>>;
            }
         }();

         using V = std::decay_t<T>;
         for_each<N>([&]<auto I>() {
            if constexpr (glaze_array_t<V>) {
               serialize<JSON>::op<Opts>(get_member(value, glz::get<I>(meta_v<T>)), ctx, args...);
            }
            else {
               serialize<JSON>::op<Opts>(glz::get<I>(value), ctx, args...);
            }
            constexpr bool needs_new_line = I < N - 1;
            if constexpr (needs_new_line) {
               dump('\n', args...);
            }
         });
      }
   };

   template <class T>
      requires is_std_tuple<std::decay_t<T>>
   struct to<NDJSON, T>
   {
      template <auto Opts, class... Args>
      static void op(auto&& value, is_context auto&& ctx, Args&&... args)
      {
         static constexpr auto N = []() constexpr {
            if constexpr (glaze_array_t<std::decay_t<T>>) {
               return glz::tuple_size_v<meta_t<std::decay_t<T>>>;
            }
            else {
               return glz::tuple_size_v<std::decay_t<T>>;
            }
         }();

         using V = std::decay_t<T>;
         for_each<N>([&]<auto I>() {
            if constexpr (glaze_array_t<V>) {
               serialize<JSON>::op<Opts>(value.*std::get<I>(meta_v<V>), ctx, std::forward<Args>(args)...);
            }
            else {
               serialize<JSON>::op<Opts>(std::get<I>(value), ctx, std::forward<Args>(args)...);
            }
            constexpr bool needs_new_line = I < N - 1;
            if constexpr (needs_new_line) {
               dump('\n', std::forward<Args>(args)...);
            }
         });
      }
   };

   template <read_supported<NDJSON> T, class Buffer>
   [[nodiscard]] auto read_ndjson(T& value, Buffer&& buffer)
   {
      context ctx{};
      return read<opts{.format = NDJSON}>(value, std::forward<Buffer>(buffer), ctx);
   }

   template <read_supported<NDJSON> T, class Buffer>
   [[nodiscard]] expected<T, error_ctx> read_ndjson(Buffer&& buffer)
   {
      T value{};
      context ctx{};
      const auto ec = read<opts{.format = NDJSON}>(value, std::forward<Buffer>(buffer), ctx);
      if (ec == error_code::none) {
         return value;
      }
      return unexpected(ec);
   }

   template <auto Opts = opts{.format = NDJSON}, read_supported<NDJSON> T>
   [[nodiscard]] error_ctx read_file_ndjson(T& value, const sv file_name)
   {
      context ctx{};
      ctx.current_file = file_name;

      std::string buffer;

      const auto ec = file_to_buffer(buffer, ctx.current_file);

      if (bool(ec)) {
         return {0, ec};
      }

      return read<Opts>(value, buffer, ctx);
   }

   template <write_supported<NDJSON> T, class Buffer>
   [[nodiscard]] error_ctx write_ndjson(T&& value, Buffer&& buffer)
   {
      return write<opts{.format = NDJSON}>(std::forward<T>(value), std::forward<Buffer>(buffer));
   }

   template <write_supported<NDJSON> T>
   [[nodiscard]] expected<std::string, error_ctx> write_ndjson(T&& value)
   {
      return write<opts{.format = NDJSON}>(std::forward<T>(value));
   }

   template <write_supported<NDJSON> T>
   [[nodiscard]] error_ctx write_file_ndjson(T&& value, const std::string& file_name, auto&& buffer)
   {
      const auto ec = write<opts{.format = NDJSON}>(std::forward<T>(value), buffer);
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
