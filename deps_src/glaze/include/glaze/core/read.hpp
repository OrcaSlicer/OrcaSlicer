// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <span>

#include "glaze/api/std/span.hpp"
#include "glaze/core/buffer_traits.hpp"
#include "glaze/core/common.hpp"
#include "glaze/core/streaming_state.hpp"
#include "glaze/util/parse.hpp"

namespace glz
{
   template <auto Opts, bool Padded = false>
   auto read_iterators(contiguous auto&& buffer) noexcept
   {
      static_assert(sizeof(decltype(*buffer.data())) == 1);

      auto it = reinterpret_cast<const char*>(buffer.data());
      auto end = reinterpret_cast<const char*>(buffer.data()); // to be incremented

      if constexpr (Padded) {
         end += buffer.size() - padding_bytes;
      }
      else {
         end += buffer.size();
      }

      return std::pair{it, end};
   }

   template <auto Opts, class T>
      requires read_supported<T, Opts.format>
   [[nodiscard]] error_ctx read(T& value, contiguous auto&& buffer, is_context auto&& ctx)
   {
      static_assert(sizeof(decltype(*buffer.data())) == 1);
      using Buffer = std::remove_reference_t<decltype(buffer)>;

      if constexpr (Opts.format != NDJSON) {
         if (buffer.empty()) [[unlikely]] {
            ctx.error = error_code::no_read_input;
            return {0, ctx.error, ctx.custom_error_message};
         }
      }

      constexpr bool use_padded = resizable<Buffer> && non_const_buffer<Buffer> && !check_disable_padding(Opts);

      [[maybe_unused]] size_t original_size{};
      if constexpr (use_padded) {
         // Pad the buffer for SWAR
         original_size = buffer.size();
         buffer.resize(original_size + padding_bytes);
      }

      auto [it, end] = read_iterators<Opts, use_padded>(buffer);
      auto start = it;
      if (bool(ctx.error)) [[unlikely]] {
         goto finish;
      }

      if constexpr (use_padded) {
         parse<Opts.format>::template op<is_padded_on<Opts>()>(value, ctx, it, end);
      }
      else {
         parse<Opts.format>::template op<is_padded_off<Opts>()>(value, ctx, it, end);
      }

      if (bool(ctx.error)) [[unlikely]] {
         goto finish;
      }

      // The JSON RFC 8259 defines: JSON-text = ws value ws
      // So, trailing whitespace is permitted and sometimes we want to
      // validate this, even though this memory will not affect Glaze.
      if constexpr (check_validate_trailing_whitespace(Opts)) {
         if (it < end) {
            skip_ws<Opts>(ctx, it, end);
            if (bool(ctx.error)) [[unlikely]] {
               goto finish;
            }
            if (it != end) [[unlikely]] {
               ctx.error = error_code::syntax_error;
            }
         }
      }

   finish:
      // We don't do depth validation for partial reading
      if constexpr (check_partial_read(Opts)) {
         if (ctx.error == error_code::partial_read_complete) [[likely]] {
            ctx.error = error_code::none;
         }
         else if (ctx.error == error_code::end_reached && ctx.depth == 0) {
            ctx.error = error_code::none;
         }
      }
      else {
         if (ctx.error == error_code::end_reached && ctx.depth == 0) {
            ctx.error = error_code::none;
         }
      }

      if constexpr (use_padded) {
         // Restore the original buffer state
         buffer.resize(original_size);
      }

      return {size_t(it - start), ctx.error, ctx.custom_error_message};
   }

   template <auto Opts, class T>
      requires read_supported<T, Opts.format>
   [[nodiscard]] error_ctx read(T& value, contiguous auto&& buffer)
   {
      context ctx{};
      return read<Opts>(value, buffer, ctx);
   }

   template <class T>
   concept c_style_char_buffer = std::convertible_to<std::remove_cvref_t<T>, std::string_view> && !has_data<T>;

   template <class T>
   concept is_buffer = c_style_char_buffer<T> || contiguous<T>;

   // for char array input
   template <auto Opts, class T, c_style_char_buffer Buffer>
      requires read_supported<T, Opts.format>
   [[nodiscard]] error_ctx read(T& value, Buffer&& buffer, auto&& ctx)
   {
      const auto str = std::string_view{std::forward<Buffer>(buffer)};
      if (str.empty()) {
         return {0, error_code::no_read_input};
      }
      return read<Opts>(value, str, ctx);
   }

   template <auto Opts, class T, c_style_char_buffer Buffer>
      requires read_supported<T, Opts.format>
   [[nodiscard]] error_ctx read(T& value, Buffer&& buffer)
   {
      context ctx{};
      return read<Opts>(value, std::forward<Buffer>(buffer), ctx);
   }

   // Streaming read for input streaming buffers
   // Uses incremental parsing with internal refill points for arrays/objects
   // Returns error_ctx with total bytes consumed across all refills
   template <auto Opts, class T, class Buffer, has_streaming_state Ctx>
      requires read_supported<T, Opts.format> && is_input_streaming<std::remove_reference_t<Buffer>>
   [[nodiscard]] error_ctx read_streaming(T& value, Buffer&& buffer, Ctx&& ctx)
   {
      // For streaming, we need null_terminated = false to track depth
      static constexpr auto StreamingOpts = [] {
         auto o = is_padded_off<Opts>();
         o.null_terminated = false;
         return o;
      }();

      // Initial fill if buffer is empty
      if (buffer.empty()) {
         if (!refill_buffer(buffer)) {
            if constexpr (Opts.format != NDJSON) {
               ctx.error = error_code::no_read_input;
               return {0, ctx.error, ctx.custom_error_message};
            }
         }
      }

      // Set up streaming state so parsers can trigger internal refills
      ctx.stream = make_streaming_state(buffer);

      auto [it, end] = read_iterators<Opts>(buffer);

      // Parse with streaming-aware context
      // The parser will internally refill as needed at safe points (between array elements, etc.)
      parse<Opts.format>::template op<StreamingOpts>(value, ctx, it, end);

      // Calculate final consumed bytes
      // Note: During streaming, the buffer may have been refilled multiple times,
      // so we use bytes_consumed() which tracks total consumption
      const size_t final_consumed = static_cast<size_t>(it - ctx.stream.data());
      consume_buffer(buffer, final_consumed);

      // Handle end_reached as success when parsing completed at depth 0
      // (same logic as non-streaming read)
      if (ctx.error == error_code::end_reached && ctx.depth == 0) {
         ctx.error = error_code::none;
      }

      if (bool(ctx.error)) {
         return {buffer.bytes_consumed(), ctx.error, ctx.custom_error_message};
      }

      return {buffer.bytes_consumed(), error_code::none, ctx.custom_error_message};
   }

   template <auto Opts, class T, class Buffer>
      requires read_supported<T, Opts.format> && is_input_streaming<std::remove_reference_t<Buffer>>
   [[nodiscard]] error_ctx read_streaming(T& value, Buffer&& buffer)
   {
      streaming_context ctx{};
      return read_streaming<Opts>(value, std::forward<Buffer>(buffer), ctx);
   }
}
