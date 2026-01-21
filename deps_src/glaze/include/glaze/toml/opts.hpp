#pragma once

#include <glaze/core/opts.hpp>

namespace glz::toml
{

   enum struct opts_internal : uint32_t {
      none = 0,
      internal_struct = 1 << 0, // Currently in an inner struct
   };

   struct toml_opts
   {
      uint32_t format = TOML;
      bool error_on_unknown_keys{true};

      uint32_t internal{}; // default to 0
   };

   consteval bool check_is_internal(auto&& o) { return o.internal & uint32_t(opts_internal::internal_struct); }

   template <auto Opts>
   constexpr auto is_internal_on()
   {
      auto ret = Opts;
      ret.internal |= uint32_t(opts_internal::internal_struct);
      return ret;
   }

} // namespace glz::toml
