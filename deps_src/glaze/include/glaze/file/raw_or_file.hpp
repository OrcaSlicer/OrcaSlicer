// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <filesystem>

#include "glaze/core/common.hpp"

namespace glz
{
   // Register this with an object to allow loading a file when a valid file path is given as a string
   // If the file does not exist, the string is handled as a glz::raw_json
   // This enables file including of unknown structures that will be decoded in the future when state is known
   struct raw_or_file final
   {
      std::string str = R"("")";
   };

   template <>
   struct from<JSON, raw_or_file>
   {
      template <auto Options>
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end)
      {
         constexpr auto Opts = ws_handled_off<Options>();
         auto& v = value;
         // check if we are decoding a string, which could be a file path
         if (*it == '"') {
            parse<JSON>::op<Opts>(v.str, ctx, it, end);
            if (bool(ctx.error)) [[unlikely]]
               return;

            namespace fs = std::filesystem;
            const auto path = relativize_if_not_absolute(fs::path(ctx.current_file).parent_path(), fs::path{v.str});

            if (fs::exists(path) && fs::is_regular_file(path)) {
               const auto string_path = path.string();
               const auto ec = file_to_buffer(v.str, string_path);

               if (bool(ec)) [[unlikely]] {
                  ctx.error = error_code::includer_error;
                  auto& error_msg = error_buffer();
                  error_msg = "file failed to open: " + string_path;
                  ctx.custom_error_message = error_msg;
                  return;
               }

               const auto ecode = validate_jsonc(v.str);
               if (ecode) [[unlikely]] {
                  ctx.error = error_code::includer_error;
                  auto& error_msg = error_buffer();
                  error_msg = glz::format_error(ecode, v.str);
                  ctx.custom_error_message = error_msg;
                  return;
               }
            }
            else {
               // The file path doesn't exist, so we want a string with quotes
               // But, we skipped the quotes when first reading
               // So, now we add back the quotes
               v.str = "\"" + v.str + "\"";
            }
         }
         else {
            auto it_start = it;
            skip_value<JSON>::op<Opts>(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]]
               return;
            value.str = {it_start, static_cast<size_t>(it - it_start)};
         }
      }
   };

   template <>
   struct to<JSON, raw_or_file>
   {
      template <auto Opts>
      GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&&, auto&& b, auto& ix)
      {
         dump_maybe_empty(value.str, b, ix);
      }
   };
}
