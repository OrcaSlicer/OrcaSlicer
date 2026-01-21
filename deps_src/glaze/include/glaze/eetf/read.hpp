#pragma once

#include <glaze/core/read.hpp>
#include <glaze/core/reflect.hpp>

#include "defs.hpp"
#include "ei.hpp"
#include "opts.hpp"

namespace glz
{

   template <>
   struct skip_value<EETF>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(is_context auto&& ctx, auto&& it, auto end) noexcept
      {
         skip_term(ctx, it, end);
      }
   };

   template <>
   struct parse<EETF>
   {
      template <auto Opts, class T, is_context Ctx, class It0, class It1>
         requires(not check_no_header(Opts))
      GLZ_ALWAYS_INLINE static void op(T&& value, Ctx&& ctx, It0&& it, It1 end) noexcept
      {
         // TODO Check version
         const auto version = decode_version(ctx, it);
         if (version != 131) { // TODO find in erlang files
            ctx.error = error_code::version_mismatch;
            return;
         }

         if (it == end) {
            ctx.error = error_code::unexpected_end;
            return;
         }

         parse<Opts.format>::template op<no_header_on<Opts>()>(std::forward<T>(value), std::forward<Ctx>(ctx),
                                                               std::forward<It0>(it), end);
      }

      template <auto Opts, class T, is_context Ctx, class It0, class It1>
         requires(check_no_header(Opts))
      GLZ_ALWAYS_INLINE static void op(T&& value, Ctx&& ctx, It0&& it, It1 end) noexcept
      {
         if (bool(ctx.error)) {
            return;
         }

         if constexpr (std::is_const_v<std::remove_reference_t<T>>) {
            if constexpr (check_error_on_const_read(Opts)) {
               ctx.error = error_code::attempt_const_read;
            }
            else {
               // do not read anything into the const value
               skip_value<EETF>::op<Opts>(std::forward<Ctx>(ctx), std::forward<It0>(it), end);
            }
         }
         else {
            from<EETF, std::remove_cvref_t<T>>::template op<Opts>(std::forward<T>(value), std::forward<Ctx>(ctx),
                                                                  std::forward<It0>(it), end);
         }
      }
   };

   template <readable_array_t T>
   struct from<EETF, T> final
   {
      template <auto Opts, is_context Ctx, class It0, class It1>
      GLZ_ALWAYS_INLINE static void op(auto&& value, Ctx&& ctx, It0&& it, It1 end) noexcept
      {
         decode_sequence<Opts>(std::forward<T>(value), std::forward<Ctx>(ctx), std::forward<It0>(it),
                               std::forward<It1>(end));
      }
   };

   template <boolean_like T>
   struct from<EETF, T>
   {
      template <auto Opts, is_context Ctx, class It0, class It1>
      GLZ_ALWAYS_INLINE static void op(auto&& value, Ctx&& ctx, It0&& it, It1 end) noexcept
      {
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         if (it == end) {
            return;
         }

         decode_boolean(std::forward<T>(value), std::forward<Ctx>(ctx), std::forward<It0>(it), std::forward<It1>(end));
      }
   };

   template <num_t T>
   struct from<EETF, T> final
   {
      template <auto Opts, is_context Ctx, class It0, class It1>
      GLZ_ALWAYS_INLINE static void op(auto&& value, Ctx&& ctx, It0&& it, It1 end) noexcept
      {
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         if (it == end) {
            return;
         }

         decode_number(std::forward<T>(value), std::forward<Ctx>(ctx), std::forward<It0>(it), std::forward<It1>(end));
      }
   };

   template <atom_t T>
   struct from<EETF, T> final
   {
      template <auto Opts, is_context Ctx, class It0, class It1>
      GLZ_ALWAYS_INLINE static void op(auto&& value, Ctx&& ctx, It0&& it, It1 end) noexcept
      {
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         value.clear();

         if (it == end) {
            return;
         }

         decode_token(std::forward<T>(value), std::forward<Ctx>(ctx), std::forward<It0>(it), std::forward<It1>(end));
      }
   };

   template <str_t T>
   struct from<EETF, T> final
   {
      template <auto Opts, is_context Ctx, class It0, class It1>
      GLZ_ALWAYS_INLINE static void op(auto&& value, Ctx&& ctx, It0&& it, It1 end) noexcept
      {
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         value.clear();

         if (it == end) {
            return;
         }

         decode_token(std::forward<T>(value), std::forward<Ctx>(ctx), std::forward<It0>(it), std::forward<It1>(end));
      }
   };

   template <class T>
      requires(tuple_t<T> || is_std_tuple<T>)
   struct from<EETF, T> final
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end) noexcept
      {
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         if (it == end) {
            return;
         }

         auto [fields_count, index] = decode_tuple_header(ctx, it);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         if (it + index > end) {
            ctx.error = error_code::unexpected_end;
            return;
         }

         it += index;

         using V = std::decay_t<T>;
         constexpr auto N = glz::tuple_size_v<V>;

         if (fields_count != N) {
            ctx.error = error_code::syntax_error;
            return;
         }

         if constexpr (is_std_tuple<T>) {
            for_each<N>([&]<size_t I>() { parse<EETF>::op<Opts>(std::get<I>(value), ctx, it, end); });
         }
         else {
            for_each<N>([&]<size_t I>() { parse<EETF>::op<Opts>(glz::get<I>(value), ctx, it, end); });
         }
      }
   };

   template <class T>
      requires glaze_object_t<T> || reflectable<T>
   struct from<EETF, T> final
   {
      template <is_context Ctx, class It0, class It1>
      class field_iterator
      {
        public:
         template <typename F>
         field_iterator(F&& f, Ctx&& ctx, It0&& it, It1 end)
         {
            term_header = f(ctx, it);
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }

            CHECK_OFFSET(term_header.second);
            std::advance(it, term_header.second);
         }

         field_iterator(error_code ec, Ctx&& ctx) : term_header{-1ull, -1ull} { ctx.error = ec; }

         template <auto Opts>
         bool next(Ctx&& ctx, It0&& it, It1 end)
         {
            if (term_header.first == 0) {
               return false;
            }

            if constexpr (Opts.layout == glz::eetf::proplist_layout) {
               const auto header = decode_tuple_header(ctx, it);
               if (bool(ctx.error)) [[unlikely]] {
                  return false;
               }

               if (header.first != 2) [[unlikely]] {
                  ctx.error = error_code::syntax_error;
                  return false;
               }

               if ((it + header.second) > end) [[unlikely]] {
                  ctx.error = error_code::unexpected_end;
                  return false;
               }

               std::advance(it, header.second);
            }

            term_header.first -= 1;
            return true;
         }

         bool empty() const { return term_header.first == 0; }

        private:
         header_pair term_header;
      };

      template <auto Opts, is_context Ctx, class It0, class It1>
      static auto make_term_iterator(Ctx&& ctx, It0&& it, It1 end)
      {
         using fi = field_iterator<Ctx, It0, It1>;
         const auto tag = get_type(ctx, it);
         if (bool(ctx.error)) [[unlikely]] {
            return fi(ctx.error, ctx);
         }

         if (eetf::is_map(tag) && Opts.layout == glz::eetf::map_layout) {
            return fi(decode_map_header<Ctx, It0>, ctx, it, end);
         }
         else if (eetf::is_list(tag) && Opts.layout == glz::eetf::proplist_layout) {
            return fi(decode_list_header<Ctx, It0>, ctx, it, end);
         }

         return fi(error_code::invalid_header, ctx);
      }

      template <auto Opts, is_context Ctx, class It0, class It1>
      static void op(auto&& value, Ctx&& ctx, It0&& it, It1 end) noexcept
      {
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         if (it == end) {
            return;
         }

         auto term_it = make_term_iterator<Opts>(ctx, it, end);
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         // empty term
         if (term_it.empty()) [[unlikely]] {
            return;
         }

         static constexpr auto N = reflect<T>::size;
         while (term_it.template next<Opts>(ctx, it, end)) {
            if constexpr (N > 0) {
               static constexpr auto HashInfo = hash_info<T>;

               eetf::atom mkey;
               from<EETF, eetf::atom>::op<Opts>(mkey, ctx, it, end);
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }

               const auto n = mkey.size();
               const auto index =
                  decode_hash_with_size<EETF, T, HashInfo, HashInfo.type>::op(mkey.data(), mkey.data() + n, n);
               if (index < N) [[likely]] {
                  const sv key{mkey.data(), n};

                  visit<N>(
                     [&]<size_t I>() {
                        static constexpr auto TargetKey = get<I>(reflect<T>::keys);
                        static constexpr auto Length = TargetKey.size();
                        if ((Length == n) && compare<Length>(TargetKey.data(), key.data())) [[likely]] {
                           if constexpr (reflectable<T>) {
                              parse<EETF>::op<Opts>(get_member(value, get<I>(to_tie(value))), ctx, it, end);
                           }
                           else {
                              parse<EETF>::op<Opts>(get_member(value, get<I>(reflect<T>::values)), ctx, it, end);
                           }
                        }
                        else {
                           if constexpr (Opts.error_on_unknown_keys) {
                              ctx.error = error_code::unknown_key;
                              return;
                           }
                           else {
                              skip_value<EETF>::op<Opts>(ctx, it, end);
                              if (bool(ctx.error)) [[unlikely]]
                                 return;
                           }
                        }
                     },
                     index);

                  if (bool(ctx.error)) [[unlikely]] {
                     return;
                  }
               }
               else [[unlikely]] {
                  if constexpr (Opts.error_on_unknown_keys) {
                     ctx.error = error_code::unknown_key;
                     return;
                  }
                  else {
                     skip_value<EETF>::op<Opts>(ctx, it, end);
                     if (bool(ctx.error)) [[unlikely]]
                        return;
                  }
               }
            }
            else if constexpr (Opts.error_on_unknown_keys) {
               ctx.error = error_code::unknown_key;
               return;
            }
            else {
               skip_value<EETF>::op<Opts>(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }
            }
         }
      }
   };

   template <uint8_t layout = glz::eetf::map_layout, read_supported<EETF> T, class Buffer>
   [[nodiscard]] inline error_ctx read_term(T&& value, Buffer&& buffer) noexcept
   {
      return read<eetf::eetf_opts{.format = EETF, .layout = layout}>(value, std::forward<Buffer>(buffer));
   }

   template <uint8_t layout = glz::eetf::map_layout, read_supported<EETF> T, is_buffer Buffer>
   [[nodiscard]] expected<T, error_ctx> read_term(Buffer&& buffer) noexcept
   {
      T value{};
      context ctx{};
      const error_ctx ec =
         read<eetf::eetf_opts{.format = EETF, .layout = layout}>(value, std::forward<Buffer>(buffer), ctx);
      if (ec) {
         return unexpected<error_ctx>(ec);
      }
      return value;
   }

} // namespace glz
