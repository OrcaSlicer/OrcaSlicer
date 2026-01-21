#pragma once

#include <glaze/core/buffer_traits.hpp>
#include <glaze/core/reflect.hpp>
#include <glaze/core/write.hpp>

#include "defs.hpp"
#include "ei.hpp"
#include "opts.hpp"

namespace glz
{

   template <>
   struct serialize<EETF>
   {
      template <auto Opts, class T, class... Args>
         requires(check_no_header(Opts))
      GLZ_ALWAYS_INLINE static void op(T&& value, Args&&... args) noexcept
      {
         to<EETF, std::remove_cvref_t<T>>::template op<Opts>(std::forward<T>(value), std::forward<Args>(args)...);
      }
   };

   template <boolean_like T>
   struct to<EETF, T>
   {
      template <auto Opts, class V, class... Args>
         requires(not check_no_header(Opts))
      GLZ_ALWAYS_INLINE static void op(V&& v, Args... args) noexcept
      {
         encode_version(std::forward<Args>(args)...);
         op<no_header_on<Opts>()>(std::forward<V>(v), std::forward<Args>(args)...);
      }

      template <auto Opts, class... Args>
         requires(check_no_header(Opts))
      GLZ_ALWAYS_INLINE static void op(const bool value, Args&&... args) noexcept
      {
         encode_boolean(value, std::forward<Args>(args)...);
      }
   };

   template <num_t T>
   struct to<EETF, T> final
   {
      template <auto Opts, class V, class... Args>
         requires(not check_no_header(Opts))
      GLZ_ALWAYS_INLINE static void op(V&& v, Args... args) noexcept
      {
         encode_version(std::forward<Args>(args)...);
         op<no_header_on<Opts>()>(std::forward<V>(v), std::forward<Args>(args)...);
      }

      template <auto Opts, class... Args>
         requires(check_no_header(Opts))
      GLZ_ALWAYS_INLINE static void op(auto&& value, Args&&... args) noexcept
      {
         encode_number(value, std::forward<Args>(args)...);
      }
   };

   template <atom_t T>
   struct to<EETF, T> final
   {
      template <auto Opts, class V, class... Args>
         requires(not check_no_header(Opts))
      GLZ_ALWAYS_INLINE static void op(V&& v, Args... args) noexcept
      {
         encode_version(std::forward<Args>(args)...);
         op<no_header_on<Opts>()>(std::forward<V>(v), std::forward<Args>(args)...);
      }

      template <auto Opts, class... Args>
         requires(check_no_header(Opts))
      GLZ_ALWAYS_INLINE static void op(auto&& value, Args&&... args) noexcept
      {
         encode_atom(value, std::forward<Args>(args)...);
      }
   };

   // using for write reflectable map keys
   template <str_t T>
   struct to<EETF, T> final
   {
      template <auto Opts, class V, class... Args>
         requires(not check_no_header(Opts))
      GLZ_ALWAYS_INLINE static void op(V&& v, Args... args) noexcept
      {
         encode_version(std::forward<Args>(args)...);
         op<no_header_on<Opts>()>(std::forward<V>(v), std::forward<Args>(args)...);
      }

      template <auto Opts, class... Args>
         requires(check_no_header(Opts))
      GLZ_ALWAYS_INLINE static void op(auto&& value, Args&&... args) noexcept
      {
         encode_atom_len(value, value.size(), std::forward<Args>(args)...);
      }
   };

   template <string_t T>
   struct to<EETF, T> final
   {
      template <auto Opts, class V, class... Args>
         requires(not check_no_header(Opts))
      GLZ_ALWAYS_INLINE static void op(V&& v, Args... args) noexcept
      {
         encode_version(std::forward<Args>(args)...);
         op<no_header_on<Opts>()>(std::forward<V>(v), std::forward<Args>(args)...);
      }

      template <auto Opts, class... Args>
         requires(check_no_header(Opts))
      GLZ_ALWAYS_INLINE static void op(auto&& value, Args&&... args) noexcept
      {
         encode_string(value, std::forward<Args>(args)...);
      }
   };

   template <class T>
      requires(tuple_t<T> || is_std_tuple<T>)
   struct to<EETF, T> final
   {
      template <auto Opts, class V, class... Args>
         requires(not check_no_header(Opts))
      GLZ_ALWAYS_INLINE static void op(V&& v, Args... args) noexcept
      {
         encode_version(std::forward<Args>(args)...);
         op<no_header_on<Opts>()>(std::forward<V>(v), std::forward<Args>(args)...);
      }

      template <auto Opts, is_context Ctx, class... Args>
         requires(check_no_header(Opts))
      GLZ_ALWAYS_INLINE static void op(auto&& value, Ctx&& ctx, Args&&... args) noexcept
      {
         static constexpr auto N = glz::tuple_size_v<T>;

         encode_tuple_header(N, ctx, std::forward<Args>(args)...);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         if constexpr (is_std_tuple<T>) {
            [&]<size_t... I>(std::index_sequence<I...>) {
               (serialize<EETF>::op<Opts>(std::get<I>(value), ctx, args...), ...);
            }(std::make_index_sequence<N>{});
         }
         else {
            [&]<size_t... I>(std::index_sequence<I...>) {
               (serialize<EETF>::op<Opts>(glz::get<I>(value), ctx, args...), ...);
            }(std::make_index_sequence<N>{});
         }
      }
   };

   template <writable_array_t T>
   struct to<EETF, T> final
   {
      template <auto Opts, class V, class... Args>
         requires(not check_no_header(Opts))
      GLZ_ALWAYS_INLINE static void op(V&& v, Args... args) noexcept
      {
         encode_version(std::forward<Args>(args)...);
         op<no_header_on<Opts>()>(std::forward<V>(v), std::forward<Args>(args)...);
      }

      template <auto Opts, is_context Ctx, class B>
         requires(check_no_header(Opts))
      GLZ_ALWAYS_INLINE static void op(auto&& value, Ctx&& ctx, B&& b, size_t& ix) noexcept
      {
         const auto n = value.size();
         encode_list_header(n, ctx, b, ix);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         for (auto& i : value) {
            serialize<EETF>::op<Opts>(i, ctx, b, ix);
            if constexpr (is_output_streaming<B>) {
               flush_buffer(b, ix);
            }
         }

         encode_list_tail(ctx, b, ix);
      }
   };

   template <writable_map_t T>
   struct to<EETF, T> final
   {
      template <auto Opts, class V, class... Args>
         requires(not check_no_header(Opts))
      GLZ_ALWAYS_INLINE static void op(V&& v, Args... args) noexcept
      {
         encode_version(std::forward<Args>(args)...);
         op<no_header_on<Opts>()>(std::forward<V>(v), std::forward<Args>(args)...);
      }

      template <auto Opts, is_context Ctx, class B>
         requires(check_no_header(Opts))
      GLZ_ALWAYS_INLINE static void op(T&& value, Ctx&& ctx, B&& b, size_t& ix) noexcept
      {
         const auto n = value.size();
         encode_map_header(n, ctx, b, ix);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         for (auto&& [k, v] : value) {
            serialize<EETF>::op<Opts>(k, ctx, b, ix);
            serialize<EETF>::op<Opts>(v, ctx, b, ix);
            if constexpr (is_output_streaming<B>) {
               flush_buffer(b, ix);
            }
         }
      }
   };

   template <class T>
      requires glaze_object_t<T> || reflectable<T>
   struct to<EETF, T> final
   {
      template <auto Opts, class V, class... Args>
         requires(not check_no_header(Opts))
      GLZ_ALWAYS_INLINE static void op(V&& v, Args&&... args) noexcept
      {
         encode_version(std::forward<Args>(args)...);
         op<no_header_on<Opts>()>(std::forward<V>(v), std::forward<Args>(args)...);
      }

      template <auto Opts, is_context Ctx, class B>
         requires(check_no_header(Opts))
      GLZ_ALWAYS_INLINE static void op(auto&& value, Ctx&& ctx, B&& b, size_t& ix) noexcept
      {
         static constexpr auto N = reflect<T>::size;

         if constexpr (Opts.layout == eetf::map_layout) {
            encode_map_header(N, ctx, b, ix);
         }
         else {
            encode_list_header(N, ctx, b, ix);
         }

         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         [[maybe_unused]] decltype(auto) t = [&]() -> decltype(auto) {
            if constexpr (reflectable<T>) {
               return to_tie(value);
            }
            else {
               return nullptr;
            }
         }();

         for_each<N>([&]<size_t I>() {
            if constexpr (Opts.layout == eetf::proplist_layout) {
               encode_tuple_header(2, ctx, b, ix);
            }

            static constexpr sv key = reflect<T>::keys[I];
            serialize<EETF>::op<Opts>(key, ctx, b, ix);

            decltype(auto) member = [&]() -> decltype(auto) {
               if constexpr (reflectable<T>) {
                  return get<I>(t);
               }
               else {
                  return get<I>(reflect<T>::values);
               }
            }();

            serialize<EETF>::op<Opts>(get_member(value, member), ctx, b, ix);
            if constexpr (is_output_streaming<B>) {
               flush_buffer(b, ix);
            }
         });
      }
   };

   template <class T>
   struct to<EETF, T> final
   {
      // empty for compilation error if use unsupported value type
   };

   template <uint8_t layout = glz::eetf::map_layout, write_supported<EETF> T, output_buffer Buffer>
   [[nodiscard]] error_ctx write_term(T&& value, Buffer&& buffer) noexcept
   {
      return write<eetf::eetf_opts{.format = EETF, .layout = layout}>(std::forward<T>(value),
                                                                      std::forward<Buffer>(buffer));
   }

   template <uint8_t layout = glz::eetf::map_layout, write_supported<EETF> T, raw_buffer Buffer>
   [[nodiscard]] expected<size_t, error_ctx> write_term(T&& value, Buffer&& buffer) noexcept
   {
      return write<eetf::eetf_opts{.format = EETF, .layout = layout}>(std::forward<T>(value),
                                                                      std::forward<Buffer>(buffer));
   }

   template <write_supported<EETF> T>
   [[nodiscard]] expected<std::string, error_ctx> write_term(T&& value) noexcept
   {
      return write<eetf::eetf_opts{.format = EETF}>(std::forward<T>(value));
   }

} // namespace glz
