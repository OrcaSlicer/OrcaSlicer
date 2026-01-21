// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <type_traits>

#include "glaze/core/opts.hpp"
#include "glaze/json/read.hpp"
#include "glaze/json/write.hpp"

namespace glz
{
   // set the maximum precision for writing floats
   template <class T>
   struct write_float32_t
   {
      static constexpr bool glaze_wrapper = true;
      using value_type = T;
      T& val;
   };

   template <class T>
   struct meta<write_float32_t<T>>
   {
      static constexpr bool custom_write = true;
      static constexpr auto value{[](auto&& s) -> auto& { return s.val; }}; // reading uses the value directly
   };

   template <class T>
   struct write_float64_t
   {
      static constexpr bool glaze_wrapper = true;
      using value_type = T;
      T& val;
   };

   template <class T>
   struct meta<write_float64_t<T>>
   {
      static constexpr bool custom_write = true;
      static constexpr auto value{[](auto&& s) -> auto& { return s.val; }}; // reading uses the value directly
   };

   template <class T>
   struct write_float_full_t
   {
      static constexpr bool glaze_wrapper = true;
      using value_type = T;
      T& val;
   };

   template <class T>
   struct meta<write_float_full_t<T>>
   {
      static constexpr bool custom_write = true;
      static constexpr auto value{[](auto&& s) -> auto& { return s.val; }}; // reading uses the value directly
   };

   template <class T>
   struct to<JSON, write_float32_t<T>>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&&... args) noexcept
      {
         static constexpr auto O = set_opt<Opts, &decltype(Opts)::float_max_write_precision>(float_precision::float32);
         using Value = core_t<decltype(value.val)>;
         to<JSON, Value>::template op<O>(value.val, ctx, args...);
      }
   };

   template <class T>
   struct to<JSON, write_float64_t<T>>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&&... args) noexcept
      {
         static constexpr auto O = set_opt<Opts, &decltype(Opts)::float_max_write_precision>(float_precision::float64);
         using Value = core_t<decltype(value.val)>;
         to<JSON, Value>::template op<O>(value.val, ctx, args...);
      }
   };

   template <class T>
   struct to<JSON, write_float_full_t<T>>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, auto&&... args) noexcept
      {
         static constexpr auto O = set_opt<Opts, &decltype(Opts)::float_max_write_precision>(float_precision::full);
         using Value = core_t<decltype(value.val)>;
         to<JSON, Value>::template op<O>(value.val, ctx, args...);
      }
   };

   template <auto MemPtr>
   inline constexpr decltype(auto) write_float32_t_impl() noexcept
   {
      return [](auto&& val) { return write_float32_t<std::remove_reference_t<decltype(val.*MemPtr)>>{val.*MemPtr}; };
   }

   template <auto MemPtr>
   inline constexpr decltype(auto) write_float64_impl() noexcept
   {
      return [](auto&& val) { return write_float64_t<std::remove_reference_t<decltype(val.*MemPtr)>>{val.*MemPtr}; };
   }

   template <auto MemPtr>
   inline constexpr decltype(auto) write_float_full_impl() noexcept
   {
      return [](auto&& val) { return write_float_full_t<std::remove_reference_t<decltype(val.*MemPtr)>>{val.*MemPtr}; };
   }

   template <auto MemPtr>
   constexpr auto write_float32 = write_float32_t_impl<MemPtr>();

   template <auto MemPtr>
   constexpr auto write_float64 = write_float64_impl<MemPtr>();

   template <auto MemPtr>
   constexpr auto write_float_full = write_float_full_impl<MemPtr>();
}
