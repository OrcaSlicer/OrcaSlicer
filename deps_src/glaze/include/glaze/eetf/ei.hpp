#pragma once

#include <ei.h>

#include <glaze/concepts/container_concepts.hpp>
#include <glaze/core/common.hpp>
#include <glaze/core/context.hpp>

#include "defs.hpp"
#include "types.hpp"

namespace glz
{

#define CHECK_OFFSET(off)                     \
   if ((it + (off)) > end) [[unlikely]] {     \
      ctx.error = error_code::unexpected_end; \
      return;                                 \
   }

   using header_pair = std::pair<std::size_t, std::size_t>;

   namespace detail
   {
      template <class F, is_context Ctx, class It0, class It1>
      GLZ_ALWAYS_INLINE void decode_impl(F&& func, Ctx&& ctx, It0&& it, It1 end)
      {
         int index{};
         if (func(it, &index) < 0) [[unlikely]] {
            ctx.error = error_code::parse_number_failure;
            return;
         }

         CHECK_OFFSET(index);
         std::advance(it, index);
      }

      template <output_buffer B, class IX>
      [[nodiscard]] GLZ_ALWAYS_INLINE int resize_buffer(B&& b, IX&& ix, int index)
      {
         if (b.size() < static_cast<std::size_t>(index)) {
            b.resize((std::max)(b.size() * 2, static_cast<std::size_t>(index)));
         }

         return static_cast<int>(ix);
      }

      template <class F, is_context Ctx, class B, class IX>
      GLZ_ALWAYS_INLINE void encode_impl(F&& func, Ctx&& ctx, B&& b, IX&& ix)
      {
         int index{static_cast<int>(ix)};
         if (func(nullptr, &index) < 0) {
            ctx.error = error_code::seek_failure;
            return;
         }

         index = resize_buffer(b, ix, index);
         if (func(reinterpret_cast<char*>(b.data()), &index) < 0) {
            ctx.error = error_code::seek_failure;
            return;
         }

         ix = index;
      }

   } // namespace detail

   template <class It>
   [[nodiscard]] GLZ_ALWAYS_INLINE int decode_version(is_context auto&& ctx, It&& it)
   {
      int index{};
      int version{};
      if (ei_decode_version(it, &index, &version) < 0) [[unlikely]] {
         ctx.error = error_code::syntax_error;
         return -1;
      }

      std::advance(it, index);
      return version;
   }

   template <class It>
   GLZ_ALWAYS_INLINE int get_type(is_context auto&& ctx, It&& it)
   {
      int type{};
      int size{};
      int index{};
      if (ei_get_type(it, &index, &type, &size) < 0) {
         ctx.error = error_code::syntax_error;
         return -1;
      }

      return type;
   }

   template <class It0, class It1>
   auto skip_term(is_context auto&& ctx, It0&& it, It1 end)
   {
      int index{};
      if (ei_skip_term(it, &index) < 0) {
         ctx.error = error_code::syntax_error;
         index = 0;
      }

      CHECK_OFFSET(index);
      std::advance(it, index);
   }

   template <num_t T, class... Args>
   GLZ_ALWAYS_INLINE void decode_number(T&& value, Args&&... args)
   {
      using namespace std::placeholders;
      using V = std::remove_cvref_t<T>;
      if constexpr (std::floating_point<std::remove_cvref_t<T>>) {
         double v;
         detail::decode_impl(std::bind(ei_decode_double, _1, _2, &v), std::forward<Args>(args)...);
         value = static_cast<std::remove_cvref_t<T>>(v);
      }
      else {
         if constexpr (sizeof(V) > sizeof(long)) {
            if constexpr (std::is_signed_v<V>) {
               long long v;
               detail::decode_impl(std::bind(ei_decode_longlong, _1, _2, &v), std::forward<Args>(args)...);
               value = static_cast<T>(v);
            }
            else {
               unsigned long long v;
               detail::decode_impl(std::bind(ei_decode_ulonglong, _1, _2, &v), std::forward<Args>(args)...);
               value = static_cast<T>(v);
            }
         }
         else {
            if constexpr (std::is_signed_v<V>) {
               long v;
               detail::decode_impl(std::bind(ei_decode_long, _1, _2, &v), std::forward<Args>(args)...);
               value = static_cast<T>(v);
            }
            else {
               unsigned long v;
               detail::decode_impl(std::bind(ei_decode_ulong, _1, _2, &v), std::forward<Args>(args)...);
               value = static_cast<T>(v);
            }
         }
      }
   }

   template <class It0, class It1>
   GLZ_ALWAYS_INLINE void decode_token(auto&& value, is_context auto&& ctx, It0&& it, It1 end)
   {
      using namespace std::placeholders;

      int index{};
      int type{};
      int sz{};
      if (ei_get_type(it, &index, &type, &sz) < 0) [[unlikely]] {
         ctx.error = error_code::syntax_error;
         return;
      }

      CHECK_OFFSET(sz);

      value.resize(sz);
      if (eetf::is_atom(type)) {
         detail::decode_impl(std::bind(ei_decode_atom, _1, _2, value.data()), ctx, it, end);
      }
      else {
         detail::decode_impl(std::bind(ei_decode_string, _1, _2, value.data()), ctx, it, end);
      }
   }

   template <class... Args>
   GLZ_ALWAYS_INLINE void decode_boolean(auto&& value, Args&&... args)
   {
      using namespace std::placeholders;

      int v{};
      detail::decode_impl(std::bind(ei_decode_boolean, _1, _2, &v), std::forward<Args>(args)...);
      value = v != 0;
   }

   template <auto Opts, class T, class It0>
   void decode_binary(T&& value, std::size_t sz, is_context auto&& ctx, It0&& it, auto end)
   {
      using namespace std::placeholders;

      CHECK_OFFSET(sz * sizeof(std::uint8_t));

      using V = range_value_t<std::decay_t<T>>;

      if constexpr (resizable<T>) {
         value.resize(sz);
         if constexpr (check_shrink_to_fit(Opts)) {
            value.shrink_to_fit();
         }
      }
      else {
         if (sz > value.size()) {
            ctx.error = error_code::syntax_error;
            return;
         }
      }

      [[maybe_unused]] long szl{};
      if constexpr (sizeof(V) == sizeof(std::uint8_t)) {
         detail::decode_impl(std::bind(ei_decode_binary, _1, _2, value.data(), &szl), ctx, it, end);
      }
      else {
         std::vector<std::uint8_t> buff(sz);
         detail::decode_impl(std::bind(ei_decode_binary, _1, _2, buff.data(), &szl), ctx, it, end);
         std::copy(buff.begin(), buff.end(), value.begin());
      }
   }

   template <is_context Ctx, class It>
   GLZ_ALWAYS_INLINE auto decode_list_header(Ctx&& ctx, It&& it)
   {
      int arity{};
      int index{};
      if (ei_decode_list_header(it, &index, &arity) < 0) [[unlikely]] {
         ctx.error = error_code::syntax_error;
         return header_pair(-1ull, -1ull);
      }

      return header_pair(static_cast<std::size_t>(arity), static_cast<std::size_t>(index));
   }

   template <auto Opts, class T>
   GLZ_ALWAYS_INLINE void decode_list(T&& value, is_context auto&& ctx, auto&& it, auto end)
   {
      using V = range_value_t<std::decay_t<T>>;

      auto [arity, index] = decode_list_header(ctx, it);
      if (bool(ctx.error)) {
         return;
      }

      if constexpr (resizable<T>) {
         value.resize(arity);
         if constexpr (check_shrink_to_fit(Opts)) {
            value.shrink_to_fit();
         }
      }
      else {
         if (static_cast<std::size_t>(arity) > value.size()) {
            ctx.error = error_code::syntax_error;
            return;
         }
      }

      CHECK_OFFSET(index);
      std::advance(it, index);

      for (std::size_t idx = 0; idx < arity; idx++) {
         V v;
         from<EETF, V>::template op<Opts>(v, ctx, it, end);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         value[idx] = std::move(v);
      }

      // TODO handle elang list endings
   }

   template <auto Opts, class T, is_context Ctx, class It0, class It1>
   GLZ_ALWAYS_INLINE void decode_sequence(T&& value, Ctx&& ctx, It0&& it, It1 end)
   {
      int index{};
      int type{};
      int sz{};
      if (ei_get_type(it, &index, &type, &sz) < 0) [[unlikely]] {
         ctx.error = error_code::syntax_error;
         return;
      }

      if (eetf::is_binary(type)) {
         decode_binary<Opts>(std::forward<T>(value), static_cast<std::size_t>(sz), std::forward<Ctx>(ctx),
                             std::forward<It0>(it), end);
      }
      else if (eetf::is_list(type)) {
         if (eetf::is_string(type)) {
            std::string buff;
            decode_token(buff, std::forward<Ctx>(ctx), std::forward<It0>(it), end);
            if constexpr (resizable<T>) {
               value.resize(sz);
               if constexpr (check_shrink_to_fit(Opts)) {
                  value.shrink_to_fit();
               }
            }
            else {
               if (static_cast<std::size_t>(sz) > value.size()) {
                  ctx.error = error_code::syntax_error;
                  return;
               }
            }
            std::copy(buff.begin(), buff.end(), value.begin());
         }
         else {
            decode_list<Opts>(std::forward<T>(value), std::forward<Ctx>(ctx), std::forward<It0>(it), end);
         }
      }
      // else if tuple?
      else {
         ctx.error = error_code::elements_not_convertible_to_design;
      }
   }

   template <is_context Ctx, class It>
   GLZ_ALWAYS_INLINE auto decode_map_header(Ctx&& ctx, It&& it)
   {
      int arity{};
      int index{};
      if (ei_decode_map_header(it, &index, &arity) < 0) [[unlikely]] {
         ctx.error = error_code::syntax_error;
         return header_pair(-1ull, -1ull);
      }

      return header_pair(static_cast<std::size_t>(arity), static_cast<std::size_t>(index));
   }

   template <is_context Ctx, class It>
   GLZ_ALWAYS_INLINE auto decode_tuple_header(Ctx&& ctx, It&& it)
   {
      int arity{};
      int index{};
      if (ei_decode_tuple_header(it, &index, &arity) < 0) [[unlikely]] {
         ctx.error = error_code::syntax_error;
         return header_pair(-1ull, -1ull);
      }

      return header_pair(static_cast<std::size_t>(arity), static_cast<std::size_t>(index));
   }

   template <class B, class IX>
   GLZ_ALWAYS_INLINE void encode_version(is_context auto&& ctx, B&& b, IX&& ix)
   {
      int index{static_cast<int>(ix)};
      if (ei_encode_version(reinterpret_cast<char*>(b.data()), &index) < 0) [[unlikely]] {
         ctx.error = error_code::unexpected_end;
         return;
      }

      ix = index;
   }

   template <class... Args>
   GLZ_ALWAYS_INLINE void encode_boolean(const bool value, Args&&... args)
   {
      using namespace std::placeholders;
      detail::encode_impl(std::bind(ei_encode_boolean, _1, _2, value), std::forward<Args>(args)...);
   }

   template <class T, class... Args>
   GLZ_ALWAYS_INLINE void encode_number(T&& value, Args&&... args)
   {
      using namespace std::placeholders;

      using V = std::remove_cvref_t<T>;
      if constexpr (std::floating_point<std::remove_cvref_t<T>>) {
         detail::encode_impl(std::bind(ei_encode_double, _1, _2, value), std::forward<Args>(args)...);
      }
      else if constexpr (sizeof(T) > sizeof(long)) {
         if constexpr (std::is_signed_v<V>) {
            detail::encode_impl(std::bind(ei_encode_longlong, _1, _2, value), std::forward<Args>(args)...);
         }
         else {
            detail::encode_impl(std::bind(ei_encode_ulonglong, _1, _2, value), std::forward<Args>(args)...);
         }
      }
      else {
         if constexpr (std::is_signed_v<V>) {
            detail::encode_impl(std::bind(ei_encode_long, _1, _2, value), std::forward<Args>(args)...);
         }
         else {
            detail::encode_impl(std::bind(ei_encode_ulong, _1, _2, value), std::forward<Args>(args)...);
         }
      }
   }

   template <class... Args>
   GLZ_ALWAYS_INLINE void encode_atom(auto&& value, Args&&... args)
   {
      using namespace std::placeholders;
      detail::encode_impl(std::bind(ei_encode_atom, _1, _2, value.data()), std::forward<Args>(args)...);
   }

   template <class... Args>
   GLZ_ALWAYS_INLINE void encode_atom_len(auto&& value, std::size_t sz, Args&&... args)
   {
      using namespace std::placeholders;
      detail::encode_impl(std::bind(ei_encode_atom_len, _1, _2, value.data(), static_cast<int>(sz)),
                          std::forward<Args>(args)...);
   }

   template <class... Args>
   GLZ_ALWAYS_INLINE void encode_string(auto&& value, Args&&... args)
   {
      using namespace std::placeholders;
      detail::encode_impl(std::bind(ei_encode_string, _1, _2, value.data()), std::forward<Args>(args)...);
   }

   template <class... Args>
   GLZ_ALWAYS_INLINE void encode_tuple_header(std::size_t arity, Args&&... args)
   {
      using namespace std::placeholders;
      detail::encode_impl(std::bind(ei_encode_tuple_header, _1, _2, static_cast<int>(arity)),
                          std::forward<Args>(args)...);
   }

   template <class... Args>
   GLZ_ALWAYS_INLINE void encode_list_header(std::size_t arity, Args&&... args)
   {
      using namespace std::placeholders;
      detail::encode_impl(std::bind(ei_encode_list_header, _1, _2, static_cast<int>(arity)),
                          std::forward<Args>(args)...);
   }

   template <class... Args>
   GLZ_ALWAYS_INLINE void encode_list_tail(Args&&... args)
   {
      using namespace std::placeholders;
      detail::encode_impl(std::bind(ei_encode_list_header, _1, _2, 0), std::forward<Args>(args)...);
   }

   template <class... Args>
   GLZ_ALWAYS_INLINE void encode_map_header(std::size_t arity, Args&&... args)
   {
      using namespace std::placeholders;
      detail::encode_impl(std::bind(ei_encode_map_header, _1, _2, static_cast<int>(arity)),
                          std::forward<Args>(args)...);
   }

} // namespace glz
