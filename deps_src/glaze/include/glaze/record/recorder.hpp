#pragma once

#include <deque>
#include <string>
#include <variant>

#include "glaze/core/common.hpp"
#include "glaze/csv.hpp" // TODO: split out recorder specializations so this isn't necessary
#include "glaze/json.hpp" // TODO: split out recorder specializations so this isn't necessary
#include "glaze/util/string_literal.hpp"
#include "glaze/util/type_traits.hpp"
#include "glaze/util/variant.hpp"

namespace glz
{
   namespace detail
   {
      template <class Data>
      struct recorder_assigner
      {
         Data& data;
         sv name{};

         template <class T>
         void operator=(T& ref)
         {
            using container_type = std::decay_t<decltype(data[0].second.first)>;
            data.emplace_back(
               std::make_pair(name, std::make_pair(container_type{std::deque<std::decay_t<T>>{}}, &ref)));
         }
      };
   }

   /// <summary>
   /// recorder for saving state over the course of a run
   /// deques are used to avoid reallocation for large amounts of data as the recording length is typically unknown
   /// </summary>
   template <class... Ts>
   struct recorder
   {
      static constexpr auto glaze_reflect = false;

      using container_type = std::variant<std::deque<Ts>...>;

      std::deque<std::pair<std::string, std::pair<container_type, const void*>>> data;

      auto operator[](const sv name) { return detail::recorder_assigner<decltype(data)>{data, name}; }

      void update()
      {
         for (auto& [name, value] : data) {
            auto* ptr = value.second;
            std::visit(
               [&](auto&& container) {
                  using ContainerType = std::decay_t<decltype(container)>;
                  using T = typename ContainerType::value_type;

                  container.emplace_back(*static_cast<const T*>(ptr));
               },
               value.first);
         }
      }
   };

   template <class T>
   concept is_recorder = is_specialization_v<T, recorder>;

   template <is_recorder T>
   struct to<JSON, T>
   {
      template <auto Opts, class... Args>
      static void op(auto&& value, is_context auto&& ctx, Args&&... args)
      {
         dump('{', std::forward<Args>(args)...);

         if constexpr (Opts.prettify) {
            ctx.depth += check_indentation_width(Opts);
            dump('\n', args...);
            dumpn(check_indentation_char(Opts), ctx.depth, args...);
         }

         const size_t n = value.data.size();
         for (size_t i = 0; i < n; ++i) {
            auto& [name, v] = value.data[i];
            serialize<JSON>::op<Opts>(name, ctx, args...); // write name as key

            dump(':', args...);
            if constexpr (Opts.prettify) {
               dump(' ', args...);
            }

            serialize<JSON>::op<Opts>(v.first, ctx, args...); // write deque
            if (i < n - 1) {
               dump(',', std::forward<Args>(args)...);
            }

            if constexpr (Opts.prettify) {
               dump('\n', args...);
               dumpn(check_indentation_char(Opts), ctx.depth, args...);
            }
         }

         if constexpr (Opts.prettify) {
            ctx.depth -= check_indentation_width(Opts);
            dump('\n', args...);
            dumpn(check_indentation_char(Opts), ctx.depth, args...);
         }
         dump('}', args...);
      }
   };

   template <is_recorder T>
   struct from<JSON, T>
   {
      template <auto Options>
      static void op(auto&& value, is_context auto&& ctx, auto&& it, auto end)
      {
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         constexpr auto Opts = opening_handled_off<ws_handled_off<Options>()>();

         if constexpr (!check_opening_handled(Options)) {
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
            if (match_invalid_end<'{', Opts>(ctx, it, end)) {
               return;
            }
         }

         if (skip_ws<Opts>(ctx, it, end)) {
            return;
         }

         // we read into available containers, we do not intialize here
         const size_t n = value.data.size();
         for (size_t i = 0; i < n; ++i) {
            if (*it == '}') [[unlikely]] {
               ctx.error = error_code::expected_brace;
            }

            // find the string, escape characters are not supported for recorders
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
            const auto name = parse_key(ctx, it, end);

            auto& [str, v] = value.data[i];
            if (name != str) {
               ctx.error = error_code::name_mismatch; // Recorder read of name does not match initialized state
               return;
            }

            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
            if (match_invalid_end<':', Opts>(ctx, it, end)) {
               return;
            }
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }

            std::visit([&](auto&& deq) { parse<JSON>::op<Opts>(deq, ctx, it, end); }, v.first);

            if (i < n - 1) {
               if (skip_ws<Opts>(ctx, it, end)) {
                  return;
               }
               if (match_invalid_end<',', Opts>(ctx, it, end)) {
                  return;
               }
               if (skip_ws<Opts>(ctx, it, end)) {
                  return;
               }
            }
         }

         if (skip_ws<Opts>(ctx, it, end)) {
            return;
         }
         match<'}'>(ctx, it);
      }
   };

   template <is_recorder T>
   struct to<CSV, T>
   {
      template <auto Opts>
      static void op(auto&& value, is_context auto&& ctx, auto&&... args)
      {
         if constexpr (Opts.layout == rowwise) {
            const size_t n = value.data.size();
            for (size_t i = 0; i < n; ++i) {
               auto& [name, v] = value.data[i];
               dump_maybe_empty(name, args...);

               dump(',', args...);

               std::visit(
                  [&](auto& x) {
                     serialize<CSV>::op<Opts>(x, ctx, args...); // write deque
                  },
                  v.first);

               if (i < n - 1) {
                  dump('\n', args...);
               }
            }
         }
         else {
            // dump titles
            const auto n = value.data.size();
            size_t i = 0;
            for (auto& [name, data] : value.data) {
               dump_maybe_empty(name, args...);
               ++i;
               if (i < n) {
                  dump(',', args...);
               }
            }

            dump('\n', args...);

            size_t row = 0;
            bool end = false;
            while (true) {
               i = 0;
               for (auto& [name, data] : value.data) {
                  bool breakout = false;
                  std::visit(
                     [&](auto& v) {
                        if (row >= v.size()) {
                           end = true;
                           breakout = true;
                           return;
                        }
                        serialize<CSV>::op<Opts>(v[row], ctx, args...); // write deque
                     },
                     data.first);

                  if (breakout) {
                     break;
                  }

                  ++i;
                  if (i < n) {
                     dump(',', args...);
                  }
               }

               if (end) {
                  break;
               }

               dump('\n', args...);

               ++row;
            }
         }
      }
   };
}
