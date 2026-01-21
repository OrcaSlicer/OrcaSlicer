// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <algorithm>
#include <charconv>
#include <optional>

#include "glaze/core/seek.hpp"
#include "glaze/json/read.hpp"
#include "glaze/json/skip.hpp"
#include "glaze/util/parse.hpp"
#include "glaze/util/string_literal.hpp"

namespace glz
{
   namespace jmespath
   {
      enum struct tokenization_error {
         none, // No error
         unbalanced_brackets, // Mismatched '[' and ']'
         unbalanced_parentheses, // Mismatched '(' and ')'
         unclosed_string, // String literal not properly closed
         invalid_escape_sequence, // Invalid escape sequence in string
         unexpected_delimiter, // Unexpected character encountered (e.g., consecutive delimiters)
      };

      struct tokenization_result
      {
         std::string_view first;
         std::string_view second;
         tokenization_error error;
      };

      /**
       * @brief Trims leading whitespace characters from a string_view.
       *
       * @param s The input string_view to trim.
       * @return A string_view with leading whitespace removed.
       */
      inline constexpr std::string_view trim_left(std::string_view s)
      {
         size_t start = 0;
         while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\n' || s[start] == '\r')) {
            start++;
         }
         return s.substr(start);
      }

      /**
       * @brief Splits a JMESPath expression into the first token and the remaining path with error handling.
       *
       * @param s The JMESPath expression to tokenize.
       * @return tokenization_result containing:
       *         - first: The first token of the expression.
       *         - second: The remaining expression after the first token.
       *         - error: tokenization_error indicating if an error occurred.
       */
      inline constexpr tokenization_result tokenize_jmes_path(std::string_view s)
      {
         if (s.empty()) {
            return {"", "", tokenization_error::none};
         }

         size_t pos = 0;
         size_t len = s.size();
         int bracket_level = 0;
         int parenthesis_level = 0;
         bool in_string = false;
         char string_delim = '\0';

         while (pos < len) {
            char current = s[pos];

            if (in_string) {
               if (current == string_delim) {
                  // Check for escaped delimiter
                  size_t backslashes = 0;
                  size_t temp = pos;
                  while (temp > 0 && s[--temp] == '\\') {
                     backslashes++;
                  }
                  if (backslashes % 2 == 0) {
                     in_string = false;
                  }
               }
               else if (current == '\\') {
                  // Validate escape sequence
                  if (pos + 1 >= len) {
                     return {"", "", tokenization_error::invalid_escape_sequence};
                  }
                  char next_char = s[pos + 1];
                  // Simple validation: allow known escape characters
                  if (next_char != '"' && next_char != '\'' && next_char != '\\' && next_char != '/' &&
                      next_char != 'b' && next_char != 'f' && next_char != 'n' && next_char != 'r' &&
                      next_char != 't' && next_char != 'u') {
                     return {"", "", tokenization_error::invalid_escape_sequence};
                  }
               }
               pos++;
               continue;
            }

            switch (current) {
            case '"':
            case '\'':
               in_string = true;
               string_delim = current;
               pos++;
               break;
            case '[':
               bracket_level++;
               pos++;
               break;
            case ']':
               if (bracket_level > 0) {
                  bracket_level--;
               }
               else {
                  return {"", "", tokenization_error::unbalanced_brackets};
               }
               pos++;
               break;
            case '(':
               parenthesis_level++;
               pos++;
               break;
            case ')':
               if (parenthesis_level > 0) {
                  parenthesis_level--;
               }
               else {
                  return {"", "", tokenization_error::unbalanced_parentheses};
               }
               pos++;
               break;
            case '.':
            case '|':
               if (bracket_level == 0 && parenthesis_level == 0) {
                  // Split here
                  return {s.substr(0, pos), s.substr(pos, len - pos), tokenization_error::none};
               }
               pos++;
               break;
            default:
               pos++;
               break;
            }
         }

         if (in_string) {
            return {"", "", tokenization_error::unclosed_string};
         }

         if (bracket_level != 0) {
            return {"", "", tokenization_error::unbalanced_brackets};
         }

         if (parenthesis_level != 0) {
            return {"", "", tokenization_error::unbalanced_parentheses};
         }

         // If no delimiter found, return the whole string as first token
         return {s, "", tokenization_error::none};
      }

      inline constexpr tokenization_error finalize_tokens(std::vector<std::string_view>& tokens)
      {
         std::vector<std::string_view> final_tokens;
         final_tokens.reserve(tokens.size()); // at least

         for (auto token : tokens) {
            size_t start = 0;
            while (start < token.size()) {
               // Find the next '['
               auto open = token.find('[', start);
               if (open == std::string_view::npos) {
                  // No more bracketed segments
                  if (start < token.size()) {
                     // Add remaining part (if not empty)
                     final_tokens.push_back(token.substr(start));
                  }
                  break; // move to next token
               }
               else {
                  // If there's a key part before the bracket, add it
                  if (open > start) {
                     final_tokens.push_back(token.substr(start, open - start));
                  }
                  // Now find the closing bracket ']'
                  auto close = token.find(']', open + 1);
                  if (close == std::string_view::npos) {
                     // Mismatched bracket
                     return tokenization_error::unbalanced_brackets;
                  }
                  // Extract the bracketed token: e.g. [0]
                  final_tokens.push_back(token.substr(open, close - open + 1));
                  start = close + 1; // continue after the ']'
               }
            }
         }

         tokens = std::move(final_tokens);
         return tokenization_error::none;
      }

      /**
       * @brief Recursively tokenizes a full JMESPath expression into all its tokens with error handling.
       *
       * @param expression The complete JMESPath expression to tokenize.
       * @param tokens A vector to store all tokens in the order they appear.
       * @param error An output parameter to capture any tokenization errors.
       * @return true if tokenization succeeded without errors, false otherwise.
       */
      inline constexpr jmespath::tokenization_error tokenize_full_jmespath(std::string_view expression,
                                                                           std::vector<std::string_view>& tokens)
      {
         tokens.clear();
         auto remaining = expression;

         while (!remaining.empty()) {
            tokenization_result result = tokenize_jmes_path(remaining);
            if (result.error != tokenization_error::none) {
               return result.error;
            }

            if (result.first.empty()) {
               return tokenization_error::unexpected_delimiter;
            }

            tokens.emplace_back(result.first);

            if (!result.second.empty()) {
               char delimiter = result.second.front();
               if (delimiter == '.' || delimiter == '|') {
                  remaining = result.second.substr(1);
                  remaining = trim_left(remaining);
                  if (!remaining.empty() && (remaining.front() == '.' || remaining.front() == '|')) {
                     return tokenization_error::unexpected_delimiter;
                  }
               }
               else {
                  return tokenization_error::unexpected_delimiter;
               }
            }
            else {
               break;
            }
         }

         // New step: finalize the tokens by splitting multiple bracket accesses
         auto err = finalize_tokens(tokens);
         if (err != jmespath::tokenization_error::none) {
            return err;
         }

         return tokenization_error::none;
      }

      template <const std::string_view& expression>
      consteval auto tokenize_as_array()
      {
         constexpr auto N = [] {
            std::vector<std::string_view> tokens;
            auto err = tokenize_full_jmespath(expression, tokens);
            if (err != tokenization_error::none) {
               std::abort();
            }
            return tokens.size();
         }();

         std::vector<std::string_view> tokens;
         auto err = tokenize_full_jmespath(expression, tokens);
         if (err != tokenization_error::none) {
            std::abort();
         }

         std::array<std::string_view, N> arr{};
         for (std::size_t i = 0; i < N; ++i) {
            arr[i] = tokens[i];
         }
         return arr; // Vector destroyed here, leaving only the array.
      }

      struct ArrayParseResult
      {
         bool is_array_access = false; // True if "key[...]"
         bool error = false; // True if parsing encountered an error
         std::string_view key; // The part before the first '['
         std::optional<int32_t> start; // For a single index or slice start
         std::optional<int32_t> end; // For slice end
         std::optional<int32_t> step; // For slice step
         size_t colon_count = 0; // Number of ':' characters found inside the brackets
      };

      inline constexpr std::optional<int> parse_int(std::string_view s)
      {
         if (s.empty()) {
            return std::nullopt;
         }
         int value;
         auto result = detail::from_chars(s.data(), s.data() + s.size(), value);
         if (result.ec == std::errc()) {
            return value;
         }
         return std::nullopt;
      }

      // Parse a token that may have array indexing or slicing.
      inline constexpr ArrayParseResult parse_jmespath_token(std::string_view token)
      {
         ArrayParseResult result;

         // Find the first '['
         auto open_pos = token.find('[');
         if (open_pos == std::string_view::npos) {
            // No array access, just a key.
            result.key = token;
            return result;
         }

         auto close_pos = token.rfind(']');
         if (close_pos == std::string_view::npos || close_pos < open_pos) {
            // Mismatched brackets -> error
            result.key = token.substr(0, open_pos);
            result.is_array_access = true;
            result.error = true;
            return result;
         }

         result.is_array_access = true;
         result.key = token.substr(0, open_pos);
         auto inside = token.substr(open_pos + 1, close_pos - (open_pos + 1));
         if (inside.empty()) {
            // Empty inside "[]" is invalid
            result.error = true;
            return result;
         }

         // Count colons to determine if it's a slice
         size_t colon_count = 0;
         for (char c : inside) {
            if (c == ':') {
               colon_count++;
            }
         }
         result.colon_count = colon_count;

         // Helper lambda to parse slice parts
         auto parse_slice = [&](std::string_view inside) {
            std::string_view parts[3];
            {
               size_t start_idx = 0;
               int idx = 0;
               for (size_t i = 0; i <= inside.size(); ++i) {
                  if (i == inside.size() || inside[i] == ':') {
                     if (idx < 3) {
                        parts[idx] = inside.substr(start_idx, i - start_idx);
                        idx++;
                     }
                     start_idx = i + 1;
                  }
               }
            }

            // Parse start
            if (!parts[0].empty()) {
               auto val = parse_int(parts[0]);
               if (!val.has_value()) {
                  result.error = true;
               }
               else {
                  result.start = val;
               }
            }

            // Parse end
            if (!parts[1].empty()) {
               auto val = parse_int(parts[1]);
               if (!val.has_value()) {
                  result.error = true;
               }
               else {
                  result.end = val;
               }
            }

            // Parse step
            if (colon_count == 2 && !parts[2].empty()) {
               auto val = parse_int(parts[2]);
               if (!val.has_value()) {
                  result.error = true;
               }
               else {
                  result.step = val;
               }
            }
         };

         if (colon_count == 0) {
            // single index
            auto val = parse_int(inside);
            if (!val.has_value()) {
               result.error = true;
            }
            else {
               result.start = val;
            }
         }
         else if (colon_count == 1 || colon_count == 2) {
            // slice
            parse_slice(inside);
         }
         else {
            // More than 2 colons is invalid
            result.error = true;
         }

         return result;
      }
   }

   namespace detail
   {
      template <auto Opts = opts{}, class T>
         requires(Opts.format == JSON && not readable_array_t<T> &&
                  not(tuple_t<std::decay_t<T>> || is_std_tuple<std::decay_t<T>>))
      inline void handle_slice(const jmespath::ArrayParseResult&, T&&, context& ctx, auto&&, auto&&)
      {
         ctx.error = error_code::syntax_error;
      }

      template <auto Opts = opts{}, class T>
         requires(Opts.format == JSON && (tuple_t<std::decay_t<T>> || is_std_tuple<std::decay_t<T>>))
      inline void handle_slice(const jmespath::ArrayParseResult& decomposed_key, T&& value, context& ctx, auto&& it,
                               auto end)
      {
         if (skip_ws<Opts>(ctx, it, end)) {
            return;
         }

         // Determine slice parameters
         int32_t step_idx = decomposed_key.step.value_or(1);
         bool has_negative_index = (decomposed_key.start.value_or(0) < 0) || (decomposed_key.end.value_or(0) < 0);

         // Only support step == 1 and positive indices for now for tuples
         if (step_idx != 1 || has_negative_index) {
            ctx.error = error_code::syntax_error;
            return;
         }

         const int32_t start_idx = decomposed_key.start.value_or(0);
         const int32_t end_idx = decomposed_key.end.value_or((std::numeric_limits<int32_t>::max)());

         if (*it == ']') {
            ++it;
            return;
         }

         int32_t current_index = 0;

         // Iterate tuple elements
         using TupleType = std::decay_t<T>;
         constexpr size_t N = glz::tuple_size_v<TupleType>;

         for_each<N>([&]<size_t I>() {
            if (bool(ctx.error)) return;

            int32_t target_idx = start_idx + I;

            // If target is beyond the slice request
            if (target_idx >= end_idx) {
               return;
            }

            // Skip until target_idx
            while (current_index < target_idx) {
               if (*it == ']') {
                  // Array ended before we reached target
                  ctx.error = error_code::array_element_not_found;
                  return;
               }
               skip_value<JSON>::op<Opts>(ctx, it, end);
               if (bool(ctx.error)) return;
               if (*it == ',') {
                  ++it;
                  skip_ws<Opts>(ctx, it, end);
               }
               else if (*it == ']') {
                  return;
               }
               current_index++;
            }

            // Now current_index == target_idx
            if (*it == ']') {
               ctx.error = error_code::array_element_not_found;
               return;
            }

            if constexpr (is_std_tuple<TupleType>) {
               parse<Opts.format>::template op<Opts>(std::get<I>(value), ctx, it, end);
            }
            else {
               parse<Opts.format>::template op<Opts>(glz::get<I>(value), ctx, it, end);
            }
            if (bool(ctx.error)) return;

            if (*it == ',') {
               ++it;
               skip_ws<Opts>(ctx, it, end);
            }
            current_index++;
         });

         if (bool(ctx.error)) return;

         // Consume remainder of array
         while (*it != ']') {
            skip_value<JSON>::op<Opts>(ctx, it, end);
            if (bool(ctx.error)) return;
            if (*it == ',') {
               ++it;
               skip_ws<Opts>(ctx, it, end);
            }
         }
         ++it; // consume ']'
      }

      template <auto Opts = opts{}, class T>
         requires(Opts.format == JSON && readable_array_t<T>)
      inline void handle_slice(const jmespath::ArrayParseResult& decomposed_key, T&& value, context& ctx, auto&& it,
                               auto end)
      {
         if (skip_ws<Opts>(ctx, it, end)) {
            return;
         }

         // Determine slice parameters
         int32_t step_idx = decomposed_key.step.value_or(1);
         bool has_negative_index = (decomposed_key.start.value_or(0) < 0) || (decomposed_key.end.value_or(0) < 0);

         // If we have negative indices or step != 1, fall back to the original method (read all then slice)
         if (step_idx != 1 || has_negative_index) {
            // Original fallback behavior:
            // Read entire array into value first
            value.clear();
            if (*it == ']') {
               // empty array
               ++it; // consume ']'
            }
            else {
               while (true) {
                  parse<Opts.format>::template op<Opts>(value.emplace_back(), ctx, it, end);
                  if (bool(ctx.error)) [[unlikely]]
                     return;

                  if (skip_ws<Opts>(ctx, it, end)) {
                     return;
                  }
                  if (*it == ']') {
                     ++it;
                     break;
                  }
                  if (*it != ',') {
                     ctx.error = error_code::parse_error;
                     return;
                  }
                  ++it;
                  if (skip_ws<Opts>(ctx, it, end)) {
                     return;
                  }
               }
            }

            // Now do the slicing
            const int32_t size = static_cast<int32_t>(value.size());
            auto wrap_index = [&](int32_t idx) {
               if (idx < 0) idx += size;
               return std::clamp(idx, int32_t{0}, size);
            };

            const int32_t start_idx = wrap_index(decomposed_key.start.value_or(0));
            const int32_t end_idx = wrap_index(decomposed_key.end.value_or(size));

            if (step_idx == 1) {
               if (start_idx < end_idx) {
                  if (start_idx > 0) {
                     value.erase(value.begin(), value.begin() + start_idx);
                  }
                  if (static_cast<size_t>(end_idx - start_idx) < value.size()) {
                     value.erase(value.begin() + (end_idx - start_idx), value.end());
                  }
               }
               else {
                  value.clear();
               }
            }
            else {
               // For steps != 1 (or negative steps), the fallback path was already chosen.
               // Just apply the same logic as before.
               std::size_t dest = 0;
               if (step_idx > 0) {
                  for (int32_t i = start_idx; i < end_idx; i += step_idx) {
                     value[dest++] = std::move(value[i]);
                  }
               }
               else {
                  for (int32_t i = start_idx; i > end_idx; i += step_idx) {
                     value[dest++] = std::move(value[i]);
                  }
               }
               value.resize(dest);
            }

            return;
         }

         // If we reach here, step == 1 and no negative indices, so we can do partial reading.
         value.clear();
         const int32_t start_idx = decomposed_key.start.value_or(0);
         const int32_t end_idx = decomposed_key.end.value_or((std::numeric_limits<int32_t>::max)());

         // If empty array
         if (*it == ']') {
            ++it; // consume ']'
            return;
         }

         // We'll read elements and track their index
         int32_t current_index = 0;
         while (true) {
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }

            // Decide whether we read or skip this element
            if (current_index < start_idx) {
               // Skip this element
               skip_value<JSON>::op<Opts>(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]]
                  return;
            }
            else if (current_index >= start_idx && current_index < end_idx) {
               // Read this element into value
               parse<Opts.format>::template op<Opts>(value.emplace_back(), ctx, it, end);
               if (bool(ctx.error)) [[unlikely]]
                  return;
            }
            else {
               // current_index >= end_idx, we can skip reading into value
               skip_value<JSON>::op<Opts>(ctx, it, end);
               if (bool(ctx.error)) [[unlikely]]
                  return;
            }

            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }
            if (*it == ']') {
               ++it; // finished reading array
               break;
            }
            if (*it != ',') {
               ctx.error = error_code::parse_error;
               return;
            }
            ++it; // consume ','
            if (skip_ws<Opts>(ctx, it, end)) {
               return;
            }

            ++current_index;
         }
      }
   }

   // Read into a C++ type given a path denoted by a JMESPath query
   template <string_literal Path, auto Options = opts{}, class T, contiguous Buffer>
      requires(Options.format == JSON)
   [[nodiscard]] inline error_ctx read_jmespath(T&& value, Buffer&& buffer)
   {
      static constexpr auto S = chars<Path>;
      static constexpr auto tokens = jmespath::tokenize_as_array<S>();
      static constexpr auto N = tokens.size();

      constexpr bool use_padded = resizable<Buffer> && non_const_buffer<Buffer> && !check_disable_padding(Options);

      static constexpr auto Opts = use_padded ? is_padded_on<Options>() : is_padded_off<Options>();

      if constexpr (use_padded) {
         // Pad the buffer for SWAR
         buffer.resize(buffer.size() + padding_bytes);
      }
      auto p = read_iterators<Opts>(buffer);
      auto it = p.first;
      auto end = p.second;
      auto start = it;

      context ctx{};

      if constexpr (N == 0) {
         parse<Opts.format>::template op<Opts>(value, ctx, it, end);
      }
      else {
         using namespace glz::detail;

         skip_ws<Opts>(ctx, it, end);

         for_each<N>([&]<auto I>() {
            if (bool(ctx.error)) [[unlikely]] {
               return;
            }

            static constexpr auto decomposed_key = jmespath::parse_jmespath_token(tokens[I]);
            static constexpr auto key = decomposed_key.key;

            if constexpr (decomposed_key.is_array_access) {
               // If we have a key, that means we're looking into an object like: key[0:5]
               if constexpr (key.empty()) {
                  if (skip_ws<Opts>(ctx, it, end)) {
                     return;
                  }
                  // We expect the JSON at this level to be an array
                  if (match_invalid_end<'[', Opts>(ctx, it, end)) {
                     return;
                  }

                  // If this is a slice (colon_count > 0)
                  if constexpr (decomposed_key.colon_count > 0) {
                     detail::handle_slice<Opts>(decomposed_key, value, ctx, it, end);
                  }
                  else {
                     // SINGLE INDEX SCENARIO (no slice, just an index)
                     if constexpr (decomposed_key.start.has_value()) {
                        constexpr auto n = decomposed_key.start.value();

                        if constexpr (I == (N - 1)) {
                           // Skip until we reach the target element n
                           for (int32_t i = 0; i < n; ++i) {
                              skip_value<JSON>::op<Opts>(ctx, it, end);
                              if (bool(ctx.error)) [[unlikely]]
                                 return;

                              if (skip_ws<Opts>(ctx, it, end)) {
                                 return;
                              }
                              if (*it != ',') {
                                 ctx.error = error_code::array_element_not_found;
                                 return;
                              }
                              ++it;
                              if (skip_ws<Opts>(ctx, it, end)) {
                                 return;
                              }
                           }

                           // Now read the element at index n
                           parse<Opts.format>::template op<Opts>(value, ctx, it, end);
                        }
                        else {
                           // Not the last token. We must still parse the element at index n so the next indexing can
                           // proceed.
                           for (int32_t i = 0; i < n; ++i) {
                              skip_value<JSON>::op<Opts>(ctx, it, end);
                              if (bool(ctx.error)) [[unlikely]]
                                 return;

                              if (skip_ws<Opts>(ctx, it, end)) {
                                 return;
                              }
                              if (*it != ',') {
                                 ctx.error = error_code::array_element_not_found;
                                 return;
                              }
                              ++it;
                              if (skip_ws<Opts>(ctx, it, end)) {
                                 return;
                              }
                           }
                        }
                     }
                     else {
                        ctx.error = error_code::array_element_not_found;
                        return;
                     }
                  }

                  // After handling the array access, we're done for this token
                  return;
               }
               else {
                  // Object scenario with a key, like: key[0:5]
                  if (match_invalid_end<'{', Opts>(ctx, it, end)) {
                     return;
                  }

                  while (true) {
                     if (skip_ws<Opts>(ctx, it, end)) {
                        return;
                     }
                     if (match<'"'>(ctx, it)) {
                        return;
                     }

                     auto* start = it;
                     skip_string_view(ctx, it, end);
                     if (bool(ctx.error)) [[unlikely]]
                        return;
                     const sv k = {start, size_t(it - start)};
                     ++it;

                     if (key.size() == k.size() && comparitor<key>(k.data())) {
                        if (skip_ws<Opts>(ctx, it, end)) {
                           return;
                        }
                        if (match_invalid_end<':', Opts>(ctx, it, end)) {
                           return;
                        }
                        if (skip_ws<Opts>(ctx, it, end)) {
                           return;
                        }
                        if (match_invalid_end<'[', Opts>(ctx, it, end)) {
                           return;
                        }

                        // Distinguish single index vs slice using colon_count
                        if constexpr (decomposed_key.colon_count > 0) {
                           detail::handle_slice<Opts, decomposed_key>(value, ctx, it, end);
                        }
                        else {
                           // SINGLE INDEX SCENARIO (colon_count == 0)
                           if constexpr (decomposed_key.start.has_value()) {
                              // Skip until we reach the target element
                              constexpr auto n = decomposed_key.start.value();
                              for (int32_t i = 0; i < n; ++i) {
                                 skip_value<JSON>::op<Opts>(ctx, it, end);
                                 if (bool(ctx.error)) [[unlikely]]
                                    return;

                                 if (skip_ws<Opts>(ctx, it, end)) {
                                    return;
                                 }
                                 if (*it != ',') {
                                    ctx.error = error_code::array_element_not_found;
                                    return;
                                 }
                                 ++it;
                                 if (skip_ws<Opts>(ctx, it, end)) {
                                    return;
                                 }
                              }

                              if (skip_ws<Opts>(ctx, it, end)) {
                                 return;
                              }

                              if constexpr (I == (N - 1)) {
                                 parse<Opts.format>::template op<Opts>(value, ctx, it, end);
                              }
                              return;
                           }
                           else {
                              ctx.error = error_code::array_element_not_found;
                              return;
                           }
                        }
                     }
                     else {
                        skip_value<JSON>::op<Opts>(ctx, it, end);
                        if (bool(ctx.error)) [[unlikely]] {
                           return;
                        }
                        if (skip_ws<Opts>(ctx, it, end)) {
                           return;
                        }
                        if (*it != ',') {
                           ctx.error = error_code::key_not_found;
                           return;
                        }
                        ++it;
                     }
                  }
               }
            }
            else {
               // If it's not array access, we are dealing with an object key
               if (match_invalid_end<'{', Opts>(ctx, it, end)) {
                  return;
               }

               while (it < end) {
                  if (skip_ws<Opts>(ctx, it, end)) {
                     return;
                  }
                  if (match<'"'>(ctx, it)) {
                     return;
                  }

                  auto* start = it;
                  skip_string_view(ctx, it, end);
                  if (bool(ctx.error)) [[unlikely]]
                     return;
                  const sv k = {start, size_t(it - start)};
                  ++it;

                  if (key.size() == k.size() && comparitor<key>(k.data())) {
                     if (skip_ws<Opts>(ctx, it, end)) {
                        return;
                     }
                     if (match_invalid_end<':', Opts>(ctx, it, end)) {
                        return;
                     }
                     if (skip_ws<Opts>(ctx, it, end)) {
                        return;
                     }

                     if constexpr (I == (N - 1)) {
                        parse<Opts.format>::template op<Opts>(value, ctx, it, end);
                     }
                     return;
                  }
                  else {
                     skip_value<JSON>::op<Opts>(ctx, it, end);
                     if (bool(ctx.error)) [[unlikely]] {
                        return;
                     }
                     if (skip_ws<Opts>(ctx, it, end)) {
                        return;
                     }
                     if (*it != ',') {
                        ctx.error = error_code::key_not_found;
                        return;
                     }
                     ++it;
                  }
               }
            }
         });
      }

      if constexpr (use_padded) {
         // Restore the original buffer state
         buffer.resize(buffer.size() - padding_bytes);
      }

      return {size_t(it - start), ctx.error, ctx.custom_error_message};
   }

   // A "compiled" jmespath expression, which can be pre-computed for efficient traversal
   struct jmespath_expression
   {
      std::string_view path{};
      jmespath::tokenization_error error{};
      std::vector<std::string_view> tokens{}; // evaluated tokens

      jmespath_expression(const std::string_view input_path) noexcept : path(input_path)
      {
         error = jmespath::tokenize_full_jmespath(path, tokens);
      }

      template <size_t N>
      jmespath_expression(const char (&input_path)[N]) noexcept : path(input_path)
      {
         error = jmespath::tokenize_full_jmespath(path, tokens);
      }
      jmespath_expression(const jmespath_expression&) noexcept = default;
      jmespath_expression(jmespath_expression&&) noexcept = default;
      jmespath_expression& operator=(const jmespath_expression&) noexcept = default;
      jmespath_expression& operator=(jmespath_expression&&) noexcept = default;
   };

   // Read into a C++ type given a path denoted by a JMESPath query
   // This version supports a runtime path
   template <auto Options = opts{}, class T, contiguous Buffer>
      requires(Options.format == JSON)
   [[nodiscard]] inline error_ctx read_jmespath(const jmespath_expression& expression, T&& value, Buffer&& buffer)
   {
      if (bool(expression.error)) {
         return {0, error_code::syntax_error, "JMESPath invalid expression"};
      }

      const auto& tokens = expression.tokens;
      const auto N = tokens.size();

      constexpr bool use_padded = resizable<Buffer> && non_const_buffer<Buffer> && !check_disable_padding(Options);
      static constexpr auto Opts = use_padded ? is_padded_on<Options>() : is_padded_off<Options>();

      if constexpr (use_padded) {
         // Pad the buffer for SWAR
         buffer.resize(buffer.size() + padding_bytes);
      }
      auto p = read_iterators<Opts>(buffer);
      auto it = p.first;
      auto end = p.second;
      auto start = it;

      context ctx{};

      if (N == 0) {
         parse<Opts.format>::template op<Opts>(value, ctx, it, end);
      }
      else {
         using namespace glz::detail;

         skip_ws<Opts>(ctx, it, end);

         for (size_t I = 0; I < N; ++I) {
            if (bool(ctx.error)) [[unlikely]] {
               break;
            }

            [&] {
               const auto decomposed_key = jmespath::parse_jmespath_token(tokens[I]);
               const auto& key = decomposed_key.key;

               if (decomposed_key.is_array_access) {
                  if (key.empty()) {
                     // Top-level array scenario
                     if (skip_ws<Opts>(ctx, it, end)) {
                        return;
                     }
                     if (match_invalid_end<'[', Opts>(ctx, it, end)) {
                        return;
                     }

                     if (decomposed_key.colon_count > 0) {
                        // Slice scenario
                        detail::handle_slice(decomposed_key, value, ctx, it, end);
                        return;
                     }
                     else {
                        // Single index scenario
                        if (decomposed_key.start.has_value()) {
                           const int32_t n = decomposed_key.start.value();

                           if (I == (N - 1)) {
                              // Skip until we reach the target element n
                              for (int32_t i = 0; i < n; ++i) {
                                 skip_value<JSON>::op<Opts>(ctx, it, end);
                                 if (bool(ctx.error)) [[unlikely]]
                                    return;

                                 if (skip_ws<Opts>(ctx, it, end)) {
                                    return;
                                 }
                                 if (*it != ',') {
                                    ctx.error = error_code::array_element_not_found;
                                    return;
                                 }
                                 ++it;
                                 if (skip_ws<Opts>(ctx, it, end)) {
                                    return;
                                 }
                              }

                              // Now read the element at index n
                              parse<Opts.format>::template op<Opts>(value, ctx, it, end);
                           }
                           else {
                              // Not the last token. We must still parse the element at index n so the next indexing can
                              // proceed.
                              for (int32_t i = 0; i < n; ++i) {
                                 skip_value<JSON>::op<Opts>(ctx, it, end);
                                 if (bool(ctx.error)) [[unlikely]]
                                    return;

                                 if (skip_ws<Opts>(ctx, it, end)) {
                                    return;
                                 }
                                 if (*it != ',') {
                                    ctx.error = error_code::array_element_not_found;
                                    return;
                                 }
                                 ++it;
                                 if (skip_ws<Opts>(ctx, it, end)) {
                                    return;
                                 }
                              }
                           }
                        }
                        else {
                           ctx.error = error_code::array_element_not_found;
                           return;
                        }
                        return;
                     }
                  }
                  else {
                     // Object scenario: key[...]
                     if (match_invalid_end<'{', Opts>(ctx, it, end)) {
                        return;
                     }

                     while (true) {
                        if (skip_ws<Opts>(ctx, it, end)) {
                           return;
                        }
                        if (match<'"'>(ctx, it)) {
                           return;
                        }

                        auto* start_pos = it;
                        skip_string_view(ctx, it, end);
                        if (bool(ctx.error)) [[unlikely]]
                           return;
                        const sv k = {start_pos, size_t(it - start_pos)};
                        ++it;

                        if (key.size() == k.size() && memcmp(key.data(), k.data(), key.size()) == 0) {
                           if (skip_ws<Opts>(ctx, it, end)) {
                              return;
                           }
                           if (match_invalid_end<':', Opts>(ctx, it, end)) {
                              return;
                           }
                           if (skip_ws<Opts>(ctx, it, end)) {
                              return;
                           }
                           if (match_invalid_end<'[', Opts>(ctx, it, end)) {
                              return;
                           }

                           if (decomposed_key.colon_count > 0) {
                              // Slice scenario
                              detail::handle_slice(decomposed_key, value, ctx, it, end);
                              return;
                           }
                           else {
                              // Single index scenario
                              if (decomposed_key.start.has_value()) {
                                 int32_t n = decomposed_key.start.value();
                                 for (int32_t i = 0; i < n; ++i) {
                                    skip_value<JSON>::op<Opts>(ctx, it, end);
                                    if (bool(ctx.error)) [[unlikely]]
                                       return;

                                    if (skip_ws<Opts>(ctx, it, end)) {
                                       return;
                                    }
                                    if (*it != ',') {
                                       ctx.error = error_code::array_element_not_found;
                                       return;
                                    }
                                    ++it;
                                    if (skip_ws<Opts>(ctx, it, end)) {
                                       return;
                                    }
                                 }

                                 if (skip_ws<Opts>(ctx, it, end)) {
                                    return;
                                 }

                                 if (I == (N - 1)) {
                                    parse<Opts.format>::template op<Opts>(value, ctx, it, end);
                                 }
                                 return;
                              }
                              else {
                                 ctx.error = error_code::array_element_not_found;
                                 return;
                              }
                           }
                        }
                        else {
                           skip_value<JSON>::op<Opts>(ctx, it, end);
                           if (bool(ctx.error)) [[unlikely]] {
                              return;
                           }
                           if (skip_ws<Opts>(ctx, it, end)) {
                              return;
                           }
                           if (*it != ',') {
                              ctx.error = error_code::key_not_found;
                              return;
                           }
                           ++it;
                        }
                     }
                  }
               }
               else {
                  // Non-array access: key-only navigation
                  if (match_invalid_end<'{', Opts>(ctx, it, end)) {
                     return;
                  }

                  while (it < end) {
                     if (skip_ws<Opts>(ctx, it, end)) {
                        return;
                     }
                     if (match<'"'>(ctx, it)) {
                        return;
                     }

                     auto* start_pos = it;
                     skip_string_view(ctx, it, end);
                     if (bool(ctx.error)) [[unlikely]]
                        return;
                     const sv k = {start_pos, size_t(it - start_pos)};
                     ++it;

                     if (key.size() == k.size() && memcmp(key.data(), k.data(), key.size()) == 0) {
                        if (skip_ws<Opts>(ctx, it, end)) {
                           return;
                        }
                        if (match_invalid_end<':', Opts>(ctx, it, end)) {
                           return;
                        }
                        if (skip_ws<Opts>(ctx, it, end)) {
                           return;
                        }

                        if (I == (N - 1)) {
                           parse<Opts.format>::template op<Opts>(value, ctx, it, end);
                        }
                        return;
                     }
                     else {
                        skip_value<JSON>::op<Opts>(ctx, it, end);
                        if (bool(ctx.error)) [[unlikely]] {
                           return;
                        }
                        if (skip_ws<Opts>(ctx, it, end)) {
                           return;
                        }
                        if (*it != ',') {
                           ctx.error = error_code::key_not_found;
                           return;
                        }
                        ++it;
                     }
                  }

                  if (!bool(ctx.error)) {
                     if (it == end) {
                        ctx.error = error_code::unexpected_end;
                     }
                     else {
                        ctx.error = error_code::key_not_found;
                     }
                  }
               }
            }();
         }
      }

      if constexpr (use_padded) {
         // Restore the original buffer state
         buffer.resize(buffer.size() - padding_bytes);
      }

      return {size_t(it - start), ctx.error, ctx.custom_error_message};
   }

}
