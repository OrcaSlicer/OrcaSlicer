#pragma once

#include <glaze/core/wrappers.hpp>

#include "opts.hpp"
#include "read.hpp"
#include "types.hpp"
#include "write.hpp"

namespace glz
{
   template <class T>
   struct atom_as_string_t
   {
      static constexpr bool glaze_wrapper = true;
      using value_type = T;
      T& val;
   };

   template <class T>
   struct from<EETF, atom_as_string_t<T>>
   {
      template <auto Opts>
      static void op(auto&& value, auto&&... args)
      {
         static thread_local eetf::atom a{};
         parse<EETF>::op<Opts>(a, args...);
         value.val = a;
      }
   };

   template <class T>
   struct to<EETF, atom_as_string_t<T>>
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&& b, auto& ix)
      {
         static thread_local eetf::atom s(value.val);
         using S = core_t<decltype(s)>;
         to<EETF, S>::template op<Opts>(s, ctx, b, ix);
      }
   };

   template <auto MemPtr>
   inline constexpr decltype(auto) aas_impl() noexcept
   {
      return [](auto&& val) { return atom_as_string_t<std::remove_reference_t<decltype(val.*MemPtr)>>{val.*MemPtr}; };
   }

   // Read and write atoms as string
   template <auto MemPtr>
   constexpr auto atom_as_string = aas_impl<MemPtr>();
}
