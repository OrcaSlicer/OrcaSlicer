// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <algorithm>
#include <optional>
#include <string>

#include "glaze/core/write_chars.hpp"
#include "glaze/util/dump.hpp"

namespace glz
{
   namespace detail
   {
      struct source_info final
      {
         size_t line{};
         size_t column{};
         std::string context{};
         size_t index{};
         size_t front_truncation{};
         size_t rear_truncation{};
      };

      // We convert to only single spaces for error messages in order to keep the source info
      // calculation more efficient and avoid needing to allocate more memory.
      inline void convert_tabs_to_single_spaces(std::string& input) noexcept
      {
         for (auto& c : input) {
            if (c == '\t') {
               c = ' ';
            }
         }
      }

      inline source_info get_source_info(const has_size auto& buffer, const size_t index)
      {
         using V = std::decay_t<decltype(buffer[0])>;

         if constexpr (std::same_as<V, std::byte>) {
            return {.context = "", .index = index};
         }
         else {
            if (index >= buffer.size()) {
               return {.context = "", .index = index};
            }

            const auto start = std::begin(buffer) + index;
            const auto line = size_t(std::count(std::begin(buffer), start, static_cast<V>('\n')) + 1);
            const auto rstart = std::rbegin(buffer) + buffer.size() - index - 1;
            const auto prev_new_line =
               std::find((std::min)(rstart + 1, std::rend(buffer)), std::rend(buffer), static_cast<V>('\n'));
            const auto column = size_t(std::distance(rstart, prev_new_line));
            const auto next_new_line =
               std::find((std::min)(start + 1, std::end(buffer)), std::end(buffer), static_cast<V>('\n'));

            const auto offset = (prev_new_line == std::rend(buffer) ? 0 : index - column + 1);
            auto context_begin = std::begin(buffer) + offset;
            auto context_end = next_new_line;

            size_t front_truncation = 0;
            size_t rear_truncation = 0;

            if (std::distance(context_begin, context_end) > 64) {
               // reduce the context length so that we can more easily see errors, especially for non-prettified buffers
               if (column <= 32) {
                  rear_truncation = 64;
                  context_end = context_begin + rear_truncation;
               }
               else {
                  front_truncation = column - 32;
                  context_begin += front_truncation;
                  if (std::distance(context_begin, context_end) > 64) {
                     rear_truncation = front_truncation + 64;
                     context_end = std::begin(buffer) + offset + rear_truncation;
                  }
               }
            }

            std::string context{context_begin, context_end};
            convert_tabs_to_single_spaces(context);
            return {line, column, context, index, front_truncation, rear_truncation};
         }
      }

      template <class B>
         requires(!has_size<B>)
      inline source_info get_source_info(const B* buffer, const size_t index)
      {
         return get_source_info(sv{buffer}, index);
      }

      inline std::string generate_error_string(const std::string_view error, const source_info& info,
                                               const std::string_view filename = "")
      {
         std::string b{};
         b.resize(error.size() + info.context.size() + filename.size() + 128);
         size_t ix{};

         if (not filename.empty()) {
            dump_not_empty(filename, b, ix);
            dump(':', b, ix);
         }

         glz::context ctx{};

         if (info.context.empty()) {
            dump("index ", b, ix);
            write_chars::op<opts{}>(info.index, ctx, b, ix);
            dump(": ", b, ix);
            dump_maybe_empty(error, b, ix);
         }
         else {
            write_chars::op<opts{}>(info.line, ctx, b, ix);
            dump(':', b, ix);
            write_chars::op<opts{}>(info.column, ctx, b, ix);
            dump(": ", b, ix);
            dump_maybe_empty(error, b, ix);
            dump('\n', b, ix);
            if (info.front_truncation) {
               if (info.rear_truncation) {
                  dump("...", b, ix);
                  dump_maybe_empty(info.context, b, ix);
                  dump("...\n   ", b, ix);
               }
               else {
                  dump("...", b, ix);
                  dump_maybe_empty(info.context, b, ix);
                  dump("\n   ", b, ix);
               }
            }
            else {
               dump("   ", b, ix);
               dump_maybe_empty(info.context, b, ix);
               dump("\n   ", b, ix);
            }
            dumpn(' ', info.column - 1 - info.front_truncation, b, ix);
            dump('^', b, ix);
         }

         b.resize(ix);
         return b;
      }
   }
}
