// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <charconv>

#include "glaze/core/opts.hpp"
#include "glaze/core/read.hpp"
#include "glaze/core/reflect.hpp"
#include "glaze/csv/skip.hpp"
#include "glaze/file/file_ops.hpp"
#include "glaze/util/glaze_fast_float.hpp"
#include "glaze/util/parse.hpp"

namespace glz
{
   template <>
   struct parse<CSV>
   {
      template <auto Opts, class T, is_context Ctx, class It0, class It1>
      static void op(T&& value, Ctx&& ctx, It0&& it, It1 end)
      {
         from<CSV, std::decay_t<T>>::template op<Opts>(std::forward<T>(value), std::forward<Ctx>(ctx),
                                                       std::forward<It0>(it), std::forward<It1>(end));
      }
   };

   GLZ_ALWAYS_INLINE bool csv_new_line(is_context auto& ctx, auto&& it, auto end) noexcept
   {
      if (it == end) [[unlikely]] {
         ctx.error = error_code::unexpected_end;
         return true;
      }

      if (*it == '\n') {
         ++it;
      }
      else if (*it == '\r') {
         ++it;
         if (it == end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return true;
         }
         if (*it == '\n') [[likely]] {
            ++it;
         }
         else [[unlikely]] {
            ctx.error = error_code::syntax_error;
            return true;
         }
      }
      else [[unlikely]] {
         ctx.error = error_code::syntax_error;
         return true;
      }
      return false;
   }

   template <glaze_value_t T>
   struct from<CSV, T>
   {
      template <auto Opts, is_context Ctx, class It0, class It1>
      static void op(auto&& value, Ctx&& ctx, It0&& it, It1 end)
      {
         using V = decltype(get_member(std::declval<T>(), meta_wrapper_v<T>));
         from<CSV, V>::template op<Opts>(get_member(value, meta_wrapper_v<T>), std::forward<Ctx>(ctx),
                                         std::forward<It0>(it), end);
      }
   };

   template <num_t T>
   struct from<CSV, T>
   {
      template <auto Opts, class It>
      static void op(auto&& value, is_context auto&& ctx, It&& it, auto end) noexcept
      {
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         if (it == end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         using V = decay_keep_volatile_t<decltype(value)>;
         if constexpr (int_t<V>) {
            if constexpr (std::is_unsigned_v<V>) {
               uint64_t i{};
               if (*it == '-') [[unlikely]] {
                  ctx.error = error_code::parse_number_failure;
                  return;
               }

               if (not glz::atoi(i, it, end)) [[unlikely]] {
                  ctx.error = error_code::parse_number_failure;
                  return;
               }

               if (i > (std::numeric_limits<V>::max)()) [[unlikely]] {
                  ctx.error = error_code::parse_number_failure;
                  return;
               }
               value = static_cast<V>(i);
            }
            else {
               if (not glz::atoi(value, it, end)) [[unlikely]] {
                  ctx.error = error_code::parse_number_failure;
                  return;
               }
            }
         }
         else {
            auto [ptr, ec] = glz::from_chars<false>(it, end, value); // Always treat as non-null-terminated
            if (ec != std::errc()) [[unlikely]] {
               ctx.error = error_code::parse_number_failure;
               return;
            }
            it = ptr;
         }
      }
   };

   // CSV spec: https://www.ietf.org/rfc/rfc4180.txt
   // Quotes are escaped via double quotes

   template <string_t T>
   struct from<CSV, T>
   {
      template <auto Opts, class It>
      static void op(auto&& value, is_context auto&& ctx, It&& it, auto end)
      {
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         value.clear();

         if (it == end) {
            return;
         }

         if (*it == '"') {
            // Quoted field
            ++it; // Skip the opening quote

            if constexpr (check_raw_string(Opts)) {
               // Raw string mode: don't process escape sequences
               while (it != end) {
                  if (*it == '"') {
                     ++it; // Skip the quote
                     if (it == end || *it != '"') {
                        // Single quote - end of field
                        break;
                     }
                     // Double quote - add one quote and continue
                     value.push_back('"');
                     ++it;
                  }
                  else {
                     value.push_back(*it);
                     ++it;
                  }
               }
            }
            else {
               // Normal mode: process escape sequences properly
               while (it != end) {
                  if (*it == '"') {
                     ++it; // Skip the quote
                     if (it == end) {
                        // End of input after closing quote
                        break;
                     }
                     if (*it == '"') {
                        // Escaped quote
                        value.push_back('"');
                        ++it;
                     }
                     else {
                        // Closing quote
                        break;
                     }
                  }
                  else {
                     value.push_back(*it);
                     ++it;
                  }
               }
            }

            // After closing quote, expect comma, newline, or end of input
            if (it != end && *it != ',' && *it != '\n' && *it != '\r') {
               // Invalid character after closing quote
               ctx.error = error_code::syntax_error;
               return;
            }
         }
         else {
            // Unquoted field
            while (it != end && *it != ',' && *it != '\n' && *it != '\r') {
               value.push_back(*it);
               ++it;
            }
         }
      }
   };

   template <char_t T>
   struct from<CSV, T>
   {
      template <auto Opts, class It>
      static void op(auto&& value, is_context auto&& ctx, It&& it, auto end)
      {
         using storage_t = std::remove_cvref_t<decltype(value)>;

         if (it == end) {
            ctx.error = error_code::unexpected_end;
            return;
         }

         std::string_view field{};
         bool quoted = false;

         if (*it == '"') {
            quoted = true;
            ++it;

            auto content_begin = it;
            bool closed = false;

            while (it != end) {
               if (*it == '"') {
                  ++it;
                  if (it == end) {
                     closed = true;
                     break;
                  }
                  if (*it == '"') {
                     ++it;
                  }
                  else {
                     closed = true;
                     break;
                  }
               }
               else {
                  ++it;
               }
            }

            if (!closed) {
               ctx.error = error_code::syntax_error;
               return;
            }

            auto closing = it;
            --closing;
            field = std::string_view(content_begin, static_cast<std::size_t>(closing - content_begin));

            if (it != end && *it != ',' && *it != '\n' && *it != '\r') {
               ctx.error = error_code::syntax_error;
               return;
            }
         }
         else {
            auto content_begin = it;
            while (it != end && *it != ',' && *it != '\n' && *it != '\r') {
               ++it;
            }
            field = std::string_view(content_begin, static_cast<std::size_t>(it - content_begin));
         }

         if (field.empty()) {
            value = storage_t{};
            return;
         }

         storage_t parsed{};
         bool has_char = false;

         if (quoted) {
            std::size_t idx = 0;
            while (idx < field.size()) {
               const char c = field[idx];
               if (c == '"') {
                  if (idx + 1 >= field.size() || field[idx + 1] != '"') {
                     ctx.error = error_code::syntax_error;
                     return;
                  }
                  idx += 2;
                  if (has_char) {
                     ctx.error = error_code::syntax_error;
                     return;
                  }
                  parsed = static_cast<storage_t>('"');
                  has_char = true;
               }
               else {
                  ++idx;
                  if (has_char) {
                     ctx.error = error_code::syntax_error;
                     return;
                  }
                  parsed = static_cast<storage_t>(c);
                  has_char = true;
               }
            }
         }
         else {
            if (field.size() != 1) {
               ctx.error = error_code::syntax_error;
               return;
            }
            parsed = static_cast<storage_t>(field.front());
            has_char = true;
         }

         if (!has_char) {
            value = storage_t{};
            return;
         }

         value = parsed;
      }
   };

   template <class T>
      requires(is_named_enum<T>)
   struct from<CSV, T>
   {
      template <auto Opts, class It>
      static void op(auto&& value, is_context auto&& ctx, It&& it, auto end)
      {
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         if (it == end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         std::string field{};
         parse<CSV>::op<Opts>(field, ctx, it, end);

         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         sv key{field.data(), field.size()};

         constexpr auto N = reflect<T>::size;

         if constexpr (N == 0) {
            ctx.error = error_code::unexpected_enum;
            return;
         }
         else if constexpr (N == 1) {
            if (key == get<0>(reflect<T>::keys)) {
               value = get<0>(reflect<T>::values);
            }
            else {
               ctx.error = error_code::unexpected_enum;
            }
         }
         else {
            static constexpr auto HashInfo = hash_info<T>;
            const auto index = decode_hash_with_size<CSV, T, HashInfo, HashInfo.type>::op(
               key.data(), key.data() + key.size(), key.size());

            if (index >= N || reflect<T>::keys[index] != key) [[unlikely]] {
               ctx.error = error_code::unexpected_enum;
               return;
            }

            visit<N>([&]<size_t I>() { value = get<I>(reflect<T>::values); }, index);
         }
      }
   };

   template <bool_t T>
   struct from<CSV, T>
   {
      template <auto Opts, class It>
      static void op(auto&& value, is_context auto&& ctx, It&& it, auto end) noexcept
      {
         if (bool(ctx.error)) [[unlikely]] {
            return;
         }

         if (it == end) [[unlikely]] {
            ctx.error = error_code::unexpected_end;
            return;
         }

         auto start = it;

         // Skip to end of field (comma, newline, or end)
         while (it != end && *it != ',' && *it != '\n' && *it != '\r') {
            ++it;
         }

         const auto field_size = static_cast<size_t>(it - start);

         if (field_size == 0) {
            // Empty field defaults to false
            value = false;
            return;
         }

         // Try to parse as string boolean first
         if (field_size == 4) {
            if ((start[0] == 't' || start[0] == 'T') && (start[1] == 'r' || start[1] == 'R') &&
                (start[2] == 'u' || start[2] == 'U') && (start[3] == 'e' || start[3] == 'E')) {
               value = true;
               return;
            }
         }
         else if (field_size == 5) {
            if ((start[0] == 'f' || start[0] == 'F') && (start[1] == 'a' || start[1] == 'A') &&
                (start[2] == 'l' || start[2] == 'L') && (start[3] == 's' || start[3] == 'S') &&
                (start[4] == 'e' || start[4] == 'E')) {
               value = false;
               return;
            }
         }

         // Fall back to numeric parsing (0/1)
         it = start; // Reset iterator for numeric parsing
         uint64_t temp;
         if (not glz::atoi(temp, it, end)) [[unlikely]] {
            ctx.error = error_code::expected_true_or_false;
            return;
         }
         value = static_cast<bool>(temp);
      }
   };

   template <>
   struct from<CSV, skip>
   {
      template <auto Opts, class It0, class It1>
      GLZ_ALWAYS_INLINE static void op(auto&&, is_context auto&& ctx, It0&& it, It1 end) noexcept
      {
         skip_value<CSV>::template op<Opts>(ctx, std::forward<It0>(it), end);
      }
   };

   template <readable_array_t T>
   struct from<CSV, T>
   {
      template <auto Opts, class It>
      static void op(auto&& value, is_context auto&& ctx, It&& it, auto end)
      {
         parse<CSV>::op<Opts>(value.emplace_back(), ctx, it, end);
      }
   };

   // Utility to quickly count cells in a row for pre-allocation
   template <class It>
   inline size_t count_csv_cells(It start, It end) noexcept
   {
      if (start == end) {
         return 0;
      }

      size_t count = 1; // At least one cell if non-empty
      bool in_quotes = false;

      while (start != end) {
         if (*start == '"') {
            in_quotes = !in_quotes;
         }
         else if (*start == ',' && !in_quotes) {
            ++count;
         }
         else if ((*start == '\n' || *start == '\r') && !in_quotes) {
            break;
         }
         ++start;
      }

      return count;
   }

   // Specialization for 2D arrays (e.g., std::vector<std::vector<T>>)
   template <readable_array_t T>
      requires(readable_array_t<typename T::value_type>)
   struct from<CSV, T>
   {
      using Row = typename T::value_type;
      using Value = typename Row::value_type;

      template <auto Opts, class It>
      static void op(auto&& value, is_context auto&& ctx, It&& it, auto end)
      {
         // Clear existing data if not appending (only for resizable containers)
         if constexpr (!check_append_arrays(Opts) && resizable<T>) {
            value.clear();
         }

         // Handle column-wise layout
         if constexpr (check_layout(Opts) == colwise) {
            // Column-wise reading: transpose the data as we read
            // First, read all data into a temporary structure
            std::vector<std::vector<Value>> temp_cols;

            // Skip header row if configured
            if constexpr (requires { Opts.skip_header_row; }) {
               if constexpr (Opts.skip_header_row) {
                  while (it != end && *it != '\n' && *it != '\r') {
                     ++it;
                  }
                  if (it != end) {
                     if (*it == '\r') {
                        ++it;
                        if (it != end && *it == '\n') {
                           ++it;
                        }
                     }
                     else if (*it == '\n') {
                        ++it;
                     }
                  }
               }
            }

            // Read the CSV data row by row, but store column by column
            while (it != end) {
               size_t col_index = 0;

               while (it != end) {
                  // Ensure we have enough columns
                  if (col_index >= temp_cols.size()) {
                     temp_cols.resize(col_index + 1);
                  }

                  // Parse the value
                  Value cell_value{};
                  parse<CSV>::op<Opts>(cell_value, ctx, it, end);

                  if (bool(ctx.error)) [[unlikely]] {
                     return;
                  }

                  // Add to the appropriate column
                  temp_cols[col_index].push_back(std::move(cell_value));
                  ++col_index;

                  // Check for field separator or end of row
                  if (it != end) {
                     if (*it == ',') {
                        ++it;
                        // Handle trailing comma
                        if (it == end || *it == '\n' || *it == '\r') {
                           // Add empty value for trailing comma
                           if (col_index >= temp_cols.size()) {
                              temp_cols.resize(col_index + 1);
                           }
                           Value empty_value{};
                           temp_cols[col_index].push_back(std::move(empty_value));
                           break;
                        }
                     }
                     else if (*it == '\n' || *it == '\r') {
                        break;
                     }
                  }
                  else {
                     break;
                  }
               }

               // Handle line endings
               if (it != end) {
                  if (*it == '\r') {
                     ++it;
                     if (it != end && *it == '\n') {
                        ++it;
                     }
                  }
                  else if (*it == '\n') {
                     ++it;
                  }
               }
            }

            // Now transpose temp_cols into value
            // Each column becomes a row in the output
            for (auto& col : temp_cols) {
               Row row{};

               // Resize if it's a fixed-size container
               if constexpr (requires { row.resize(col.size()); }) {
                  row.resize(col.size());
                  for (size_t i = 0; i < col.size(); ++i) {
                     row[i] = std::move(col[i]);
                  }
               }
               else if constexpr (emplace_backable<Row>) {
                  for (auto& val : col) {
                     row.emplace_back(std::move(val));
                  }
               }
               else if constexpr (requires { row.push_back(Value{}); }) {
                  for (auto& val : col) {
                     row.push_back(std::move(val));
                  }
               }
               else {
                  static_assert(false_v<Row>, "Row type must support resizing or pushing");
               }

               // Add the row to the output
               if constexpr (emplace_backable<T>) {
                  value.emplace_back(std::move(row));
               }
               else if constexpr (requires { value.push_back(row); }) {
                  value.push_back(std::move(row));
               }
               else if constexpr (resizable<T>) {
                  // For fixed-size outer containers
                  if (value.size() <= temp_cols.size()) {
                     value.resize(temp_cols.size());
                  }
                  value[temp_cols.size() - 1] = std::move(row);
               }
               else {
                  static_assert(false_v<T>, "Container must support emplace_back, push_back, or resizing");
               }
            }

            return; // Exit early for column-wise
         }

         // Handle header row if skip_header_row is enabled
         if constexpr (requires { Opts.skip_header_row; }) {
            if constexpr (Opts.skip_header_row) {
               // Skip the first row
               while (it != end && *it != '\n' && *it != '\r') {
                  ++it;
               }
               if (it != end) {
                  if (*it == '\r') {
                     ++it;
                     if (it != end && *it == '\n') {
                        ++it;
                     }
                  }
                  else if (*it == '\n') {
                     ++it;
                  }
               }
            }
         }

         // Parse data rows
         while (it != end) {
            Row row{};

            // Pre-allocate row capacity for better performance
            if constexpr (resizable<Row>) {
               auto row_start = it;
               // Find end of current row to count cells
               auto row_end = it;
               while (row_end != end && *row_end != '\n' && *row_end != '\r') {
                  ++row_end;
               }
               const auto estimated_cells = count_csv_cells(row_start, row_end);
               if (estimated_cells > 0) {
                  row.reserve(estimated_cells);
               }
            }

            // Parse cells in current row
            size_t cell_index = 0;
            bool row_has_data = false;

            while (it != end && *it != '\n' && *it != '\r') {
               Value cell_value{};
               parse<CSV>::op<Opts>(cell_value, ctx, it, end);

               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }

               // Handle different row container types
               if constexpr (emplace_backable<Row>) {
                  // Resizable containers like std::vector
                  row.emplace_back(std::move(cell_value));
               }
               else if constexpr (requires { row.push_back(cell_value); }) {
                  // Containers with push_back but not emplace_back
                  row.push_back(std::move(cell_value));
               }
               else if constexpr (requires { row[cell_index] = cell_value; }) {
                  // Fixed-size containers like std::array
                  if (cell_index < row.size()) {
                     row[cell_index] = std::move(cell_value);
                  }
                  else {
                     // Index out of bounds for fixed-size container
                     ctx.error = error_code::syntax_error;
                     return;
                  }
               }
               else {
                  // Unsupported row container type
                  static_assert(
                     emplace_backable<Row> || requires { row.push_back(cell_value); } ||
                        requires { row[cell_index] = cell_value; },
                     "Row container must support emplace_back, push_back, or indexed assignment");
               }

               ++cell_index;
               row_has_data = true;

               // Check for end of row or more cells
               if (it == end) {
                  break;
               }

               if (*it == ',') {
                  ++it;
                  // Handle trailing comma by adding empty value
                  if (it == end || *it == '\n' || *it == '\r') {
                     Value empty_value{};
                     if constexpr (emplace_backable<Row>) {
                        row.emplace_back(std::move(empty_value));
                     }
                     else if constexpr (requires { row.push_back(empty_value); }) {
                        row.push_back(std::move(empty_value));
                     }
                     else if constexpr (requires { row[cell_index] = empty_value; }) {
                        if (cell_index < row.size()) {
                           row[cell_index] = std::move(empty_value);
                        }
                        else {
                           ctx.error = error_code::syntax_error;
                           return;
                        }
                     }
                     ++cell_index;
                  }
                  continue;
               }
               else if (*it == '\r' || *it == '\n') {
                  break;
               }
               else [[unlikely]] {
                  ctx.error = error_code::syntax_error;
                  return;
               }
            }

            // Only add row if it has data
            if (row_has_data) {
               if constexpr (emplace_backable<T>) {
                  value.emplace_back(std::move(row));
               }
               else if constexpr (requires { value.push_back(row); }) {
                  value.push_back(std::move(row));
               }
               else if constexpr (requires { value[value.size()] = row; }) {
                  // This shouldn't happen for typical 2D containers, but handle it
                  static_assert(resizable<T>, "Outer container must be resizable for CSV parsing");
               }
               else {
                  static_assert(
                     emplace_backable<T> || requires { value.push_back(row); },
                     "Outer container must support emplace_back or push_back");
               }
            }

            // Handle line endings
            if (it != end) {
               if (csv_new_line(ctx, it, end)) [[unlikely]] {
                  break;
               }
            }
         }

         // Validate rectangular shape if required
         if constexpr (requires { Opts.validate_rectangular; } && Opts.validate_rectangular) {
            if (!value.empty()) {
               const auto expected_cols = value[0].size();
               for (size_t i = 1; i < value.size(); ++i) {
                  if (value[i].size() != expected_cols) {
                     ctx.error = error_code::constraint_violated;
                     ctx.custom_error_message = "non-rectangular CSV rows";
                     return;
                  }
               }
            }
         }
      }
   };

   template <char delim>
   inline void goto_delim(auto&& it, auto end) noexcept
   {
      while (it != end && *it != delim) {
         ++it;
      }
   }

   inline auto read_column_wise_keys(auto&& ctx, auto&& it, auto end)
   {
      std::vector<std::pair<sv, size_t>> keys;

      auto read_key = [&](auto&& start, auto&& it) {
         sv key{start, size_t(it - start)};

         size_t csv_index{};

         const auto brace_pos = key.find('[');
         if (brace_pos != sv::npos) {
            const auto close_brace = key.find(']');
            const auto index = key.substr(brace_pos + 1, close_brace - (brace_pos + 1));
            key = key.substr(0, brace_pos);
            const auto [ptr, ec] = std::from_chars(index.data(), index.data() + index.size(), csv_index);
            if (ec != std::errc()) {
               ctx.error = error_code::syntax_error;
               return;
            }
         }

         keys.emplace_back(std::pair{key, csv_index});
      };

      auto start = it;
      while (it != end) {
         if (*it == ',') {
            read_key(start, it);
            ++it;
            start = it;
         }
         else if (*it == '\r' || *it == '\n') {
            auto line_end = it; // Position before incrementing
            if (*it == '\r') {
               ++it;
               if (it != end && *it != '\n') [[unlikely]] {
                  ctx.error = error_code::syntax_error;
                  return keys;
               }
            }

            if (start == line_end) {
               // trailing comma or empty
            }
            else {
               read_key(start, line_end); // Use original line ending position
            }
            break;
         }
         else {
            ++it;
         }
      }

      return keys;
   }

   template <readable_map_t T>
   struct from<CSV, T>
   {
      template <auto Opts, class It>
      static void op(auto&& value, is_context auto&& ctx, It&& it, auto end)
      {
         if constexpr (check_layout(Opts) == rowwise) {
            while (it != end) {
               auto start = it;
               goto_delim<','>(it, end);
               sv key{start, static_cast<size_t>(it - start)};

               size_t csv_index{};

               const auto brace_pos = key.find('[');
               if (brace_pos != sv::npos) {
                  const auto close_brace = key.find(']');
                  const auto index = key.substr(brace_pos + 1, close_brace - (brace_pos + 1));
                  key = key.substr(0, brace_pos);
                  const auto [ptr, ec] = std::from_chars(index.data(), index.data() + index.size(), csv_index);
                  if (ec != std::errc()) {
                     ctx.error = error_code::syntax_error;
                     return;
                  }
               }

               if (it == end || *it != ',') [[unlikely]] {
                  ctx.error = error_code::syntax_error;
                  return;
               }
               ++it;

               using key_type = typename std::decay_t<decltype(value)>::key_type;
               auto& member = value[key_type(key)];
               using M = std::decay_t<decltype(member)>;
               if constexpr (fixed_array_value_t<M> && emplace_backable<M>) {
                  size_t col = 0;
                  while (it != end) {
                     if (col < member.size()) [[likely]] {
                        auto& element = member[col];
                        if (csv_index < element.size()) [[likely]] {
                           parse<CSV>::op<Opts>(element[csv_index], ctx, it, end);
                        }
                        else [[unlikely]] {
                           ctx.error = error_code::syntax_error;
                           return;
                        }
                     }
                     else [[unlikely]] {
                        auto& element = member.emplace_back();
                        if (csv_index < element.size()) [[likely]] {
                           parse<CSV>::op<Opts>(element[csv_index], ctx, it, end);
                        }
                        else [[unlikely]] {
                           ctx.error = error_code::syntax_error;
                           return;
                        }
                     }

                     if (it == end) break;

                     if (*it == '\r') {
                        ++it;
                        if (it != end && *it == '\n') {
                           ++it;
                        }
                        break;
                     }
                     else if (*it == '\n') {
                        ++it;
                        break;
                     }

                     if (*it == ',') {
                        ++it;
                     }
                     else {
                        ctx.error = error_code::syntax_error;
                        return;
                     }

                     ++col;
                  }
               }
               else {
                  while (it != end) {
                     parse<CSV>::op<Opts>(member, ctx, it, end);

                     if (it == end) break;

                     if (*it == '\r') {
                        ++it;
                        if (it != end && *it == '\n') {
                           ++it;
                        }
                        break;
                     }
                     else if (*it == '\n') {
                        ++it;
                        break;
                     }

                     if (*it == ',') {
                        ++it;
                     }
                     else {
                        ctx.error = error_code::syntax_error;
                        return;
                     }
                  }
               }
            }
         }
         else // column wise
         {
            const auto keys = read_column_wise_keys(ctx, it, end);

            if (bool(ctx.error)) {
               return;
            }

            if (csv_new_line(ctx, it, end)) {
               return;
            }

            const auto n_keys = keys.size();

            size_t row = 0;

            while (it != end) {
               for (size_t i = 0; i < n_keys; ++i) {
                  using key_type = typename std::decay_t<decltype(value)>::key_type;
                  auto& member = value[key_type(keys[i].first)];
                  using M = std::decay_t<decltype(member)>;
                  if constexpr (fixed_array_value_t<M> && emplace_backable<M>) {
                     const auto index = keys[i].second;
                     if (row < member.size()) [[likely]] {
                        auto& element = member[row];
                        if (index < element.size()) [[likely]] {
                           parse<CSV>::op<Opts>(element[index], ctx, it, end);
                        }
                        else [[unlikely]] {
                           ctx.error = error_code::syntax_error;
                           return;
                        }
                     }
                     else [[unlikely]] {
                        auto& element = member.emplace_back();
                        if (index < element.size()) [[likely]] {
                           parse<CSV>::op<Opts>(element[index], ctx, it, end);
                        }
                        else [[unlikely]] {
                           ctx.error = error_code::syntax_error;
                           return;
                        }
                     }
                  }
                  else {
                     parse<CSV>::op<Opts>(member, ctx, it, end);
                  }

                  if (it != end && *it == ',') {
                     ++it;
                  }
               }

               if (it == end) break;

               if (*it == '\r') {
                  ++it;
                  if (it != end && *it == '\n') {
                     ++it;
                     ++row;
                  }
                  else [[unlikely]] {
                     ctx.error = error_code::syntax_error;
                     return;
                  }
               }
               else if (*it == '\n') {
                  ++it;
                  ++row;
               }
            }
         }
      }
   };

   // For types like std::vector<T> where T is a struct/object
   template <readable_array_t T>
      requires(glaze_object_t<typename T::value_type> || reflectable<typename T::value_type>)
   struct from<CSV, T>
   {
      using U = typename T::value_type;

      template <auto Opts, class It>
      static void op(auto&& value, is_context auto&& ctx, It&& it, auto end)
      {
         static constexpr auto N = reflect<U>::size;
         static constexpr auto HashInfo = hash_info<U>;

         // Clear existing data if not appending
         if constexpr (!check_append_arrays(Opts)) {
            value.clear();
         }

         if constexpr (check_layout(Opts) == colwise) {
            // Read column headers
            std::vector<size_t> member_indices;

            if constexpr (check_use_headers(Opts)) {
               auto headers = read_column_wise_keys(ctx, it, end);

               if (bool(ctx.error)) [[unlikely]] {
                  return;
               }

               if (csv_new_line(ctx, it, end)) {
                  return;
               }

               // Map header names to member indices
               for (const auto& [key, idx] : headers) {
                  const auto member_idx = decode_hash_with_size<CSV, U, HashInfo, HashInfo.type>::op(
                     key.data(), key.data() + key.size(), key.size());

                  if (member_idx >= N) [[unlikely]] {
                     ctx.error = error_code::unknown_key;
                     return;
                  }

                  member_indices.push_back(member_idx);
               }
            }
            else {
               // Use default order of members
               for (size_t i = 0; i < N; ++i) {
                  member_indices.push_back(i);
               }
            }

            const auto n_cols = member_indices.size();

            // Read rows
            while (it != end) {
               U struct_value{};

               for (size_t i = 0; i < n_cols; ++i) {
                  const auto member_idx = member_indices[i];

                  visit<N>(
                     [&]<size_t I>() {
                        if (I == member_idx) {
                           decltype(auto) member = [&]() -> decltype(auto) {
                              if constexpr (reflectable<U>) {
                                 return get_member(struct_value, get<I>(to_tie(struct_value)));
                              }
                              else {
                                 return get_member(struct_value, get<I>(reflect<U>::values));
                              }
                           }();

                           parse<CSV>::op<Opts>(member, ctx, it, end);
                        }
                     },
                     member_idx);

                  if (bool(ctx.error)) [[unlikely]] {
                     return;
                  }

                  if (i < n_cols - 1) {
                     if (it == end || *it != ',') [[unlikely]] {
                        ctx.error = error_code::syntax_error;
                        return;
                     }
                     ++it;
                  }
               }

               value.push_back(std::move(struct_value));

               if (it == end) {
                  break;
               }

               // Handle newlines
               if (*it == '\r') {
                  ++it;
                  if (it != end && *it == '\n') {
                     ++it;
                  }
                  else [[unlikely]] {
                     ctx.error = error_code::syntax_error;
                     return;
                  }
               }
               else if (*it == '\n') {
                  ++it;
               }
               else [[unlikely]] {
                  ctx.error = error_code::syntax_error;
                  return;
               }

               if (it == end) {
                  break;
               }
            }
         }
         else // rowwise layout
         {
            // Row-wise: each row is a complete struct
            // Skip header row if configured
            if constexpr (Opts.skip_header_row) {
               while (it != end && *it != '\n' && *it != '\r') {
                  ++it;
               }
               if (it != end) {
                  if (*it == '\r') {
                     ++it;
                     if (it != end && *it == '\n') {
                        ++it;
                     }
                  }
                  else if (*it == '\n') {
                     ++it;
                  }
               }
            }

            // When not using headers, we assume fields are in declaration order
            if constexpr (!check_use_headers(Opts)) {
               // Skip leading whitespace and empty lines
               while (it != end && (*it == ' ' || *it == '\t' || *it == '\n' || *it == '\r')) {
                  ++it;
               }

               // If we've consumed all input (empty CSV), return successfully
               if (it == end) {
                  return;
               }

               while (it != end) {
                  U struct_value{};

                  // Parse each field in declaration order
                  for_each<N>([&]<size_t I>() {
                     if (bool(ctx.error)) [[unlikely]] {
                        return;
                     }

                     decltype(auto) member = [&]() -> decltype(auto) {
                        if constexpr (reflectable<U>) {
                           return get_member(struct_value, get<I>(to_tie(struct_value)));
                        }
                        else {
                           return get_member(struct_value, get<I>(reflect<U>::values));
                        }
                     }();

                     parse<CSV>::op<Opts>(member, ctx, it, end);

                     if (bool(ctx.error)) [[unlikely]] {
                        return;
                     }

                     // Handle field separator
                     if constexpr (I < N - 1) {
                        if (it != end && *it == ',') {
                           ++it;
                        }
                        else if (it == end || *it == '\n' || *it == '\r') {
                           // Row ended early - not enough fields
                           ctx.error = error_code::syntax_error;
                           return;
                        }
                     }
                  });

                  if (bool(ctx.error)) [[unlikely]] {
                     return;
                  }

                  value.push_back(std::move(struct_value));

                  // Handle row terminator
                  if (it != end) {
                     if (*it == '\r') {
                        ++it;
                        if (it != end && *it == '\n') {
                           ++it;
                        }
                     }
                     else if (*it == '\n') {
                        ++it;
                     }
                     else if (*it == ',') {
                        // Extra fields in row - error
                        ctx.error = error_code::syntax_error;
                        return;
                     }
                  }
               }
            }
            else {
               // With headers - not yet implemented for rowwise
               ctx.error = error_code::feature_not_supported;
               return;
            }
         }
      }
   };

   template <class T>
      requires((glaze_object_t<T> || reflectable<T>) && not custom_read<T>)
   struct from<CSV, T>
   {
      template <auto Opts, class It>
      static void op(auto&& value, is_context auto&& ctx, It&& it, auto end)
      {
         static constexpr auto N = reflect<T>::size;
         static constexpr auto HashInfo = hash_info<T>;

         if constexpr (check_layout(Opts) == rowwise) {
            while (it != end) {
               auto start = it;
               goto_delim<','>(it, end);
               sv key{start, static_cast<size_t>(it - start)};

               size_t csv_index{};

               const auto brace_pos = key.find('[');
               if (brace_pos != sv::npos) {
                  const auto close_brace = key.find(']');
                  const auto index = key.substr(brace_pos + 1, close_brace - (brace_pos + 1));
                  key = key.substr(0, brace_pos);
                  const auto [ptr, ec] = std::from_chars(index.data(), index.data() + index.size(), csv_index);
                  if (ec != std::errc()) [[unlikely]] {
                     ctx.error = error_code::syntax_error;
                     return;
                  }
               }

               if (it == end || *it != ',') [[unlikely]] {
                  ctx.error = error_code::syntax_error;
                  return;
               }
               ++it;

               const auto index = decode_hash_with_size<CSV, T, HashInfo, HashInfo.type>::op(
                  key.data(), key.data() + key.size(), key.size());

               // Verify that the decoded index actually matches the key string to avoid
               // accidental matches from non-member inputs (e.g., fuzzed data).
               if (index < N && reflect<T>::keys[index] == key) [[likely]] {
                  visit<N>(
                     [&]<size_t I>() {
                        decltype(auto) member = [&]() -> decltype(auto) {
                           if constexpr (reflectable<T>) {
                              return get_member(value, get<I>(to_tie(value)));
                           }
                           else {
                              return get_member(value, get<I>(reflect<T>::values));
                           }
                        }();

                        using M = std::decay_t<decltype(member)>;
                        if constexpr (fixed_array_value_t<M> && emplace_backable<M>) {
                           size_t col = 0;
                           while (it != end) {
                              if (col < member.size()) [[likely]] {
                                 auto& element = member[col];
                                 if (csv_index < element.size()) [[likely]] {
                                    parse<CSV>::op<Opts>(element[csv_index], ctx, it, end);
                                 }
                                 else [[unlikely]] {
                                    ctx.error = error_code::syntax_error;
                                    return;
                                 }
                              }
                              else [[unlikely]] {
                                 auto& element = member.emplace_back();
                                 if (csv_index < element.size()) [[likely]] {
                                    parse<CSV>::op<Opts>(element[csv_index], ctx, it, end);
                                 }
                                 else [[unlikely]] {
                                    ctx.error = error_code::syntax_error;
                                    return;
                                 }
                              }

                              if (it == end) break;

                              if (*it == '\r') {
                                 ++it;
                                 if (it != end && *it == '\n') {
                                    ++it;
                                    break;
                                 }
                                 else [[unlikely]] {
                                    ctx.error = error_code::syntax_error;
                                    return;
                                 }
                              }
                              else if (*it == '\n') {
                                 ++it;
                                 break;
                              }

                              if (*it == ',') [[likely]] {
                                 ++it;
                              }
                              else [[unlikely]] {
                                 ctx.error = error_code::syntax_error;
                                 return;
                              }

                              ++col;
                           }
                        }
                        else {
                           while (it != end) {
                              parse<CSV>::op<Opts>(member, ctx, it, end);

                              if (it == end) break;

                              if (*it == '\r') {
                                 ++it;
                                 if (it != end && *it == '\n') {
                                    ++it;
                                    break;
                                 }
                                 else [[unlikely]] {
                                    ctx.error = error_code::syntax_error;
                                    return;
                                 }
                              }
                              else if (*it == '\n') {
                                 ++it;
                                 break;
                              }

                              if (*it == ',') [[likely]] {
                                 ++it;
                              }
                              else [[unlikely]] {
                                 ctx.error = error_code::syntax_error;
                                 return;
                              }
                           }
                        }
                     },
                     index);

                  if (bool(ctx.error)) [[unlikely]] {
                     return;
                  }
               }
               else [[unlikely]] {
                  ctx.error = error_code::unknown_key;
                  return;
               }
            }
         }
         else // column wise
         {
            const auto keys = read_column_wise_keys(ctx, it, end);

            if (bool(ctx.error)) [[unlikely]] {
               return;
            }

            if (csv_new_line(ctx, it, end)) {
               return;
            }

            const auto n_keys = keys.size();

            size_t row = 0;

            bool at_end{it == end};
            if (!at_end) {
               while (true) {
                  for (size_t i = 0; i < n_keys; ++i) {
                     const auto key = keys[i].first;
                     const auto index = decode_hash_with_size<CSV, T, HashInfo, HashInfo.type>::op(
                        key.data(), key.data() + key.size(), key.size());

                     if (index < N) [[likely]] {
                        visit<N>(
                           [&]<size_t I>() {
                              decltype(auto) member = [&]() -> decltype(auto) {
                                 if constexpr (reflectable<T>) {
                                    return get_member(value, get<I>(to_tie(value)));
                                 }
                                 else {
                                    return get_member(value, get<I>(reflect<T>::values));
                                 }
                              }();

                              using M = std::decay_t<decltype(member)>;
                              if constexpr (fixed_array_value_t<M> && emplace_backable<M>) {
                                 const auto index = keys[i].second;
                                 if (row < member.size()) [[likely]] {
                                    auto& element = member[row];
                                    if (index < element.size()) {
                                       parse<CSV>::op<Opts>(element[index], ctx, it, end);
                                    }
                                    else {
                                       ctx.error = error_code::syntax_error;
                                       return;
                                    }
                                 }
                                 else [[unlikely]] {
                                    auto& element = member.emplace_back();
                                    if (index < element.size()) {
                                       parse<CSV>::op<Opts>(element[index], ctx, it, end);
                                    }
                                    else {
                                       ctx.error = error_code::syntax_error;
                                       return;
                                    }
                                 }
                              }
                              else {
                                 parse<CSV>::op<Opts>(member, ctx, it, end);
                              }
                           },
                           index);

                        if (bool(ctx.error)) [[unlikely]] {
                           return;
                        }
                     }
                     else [[unlikely]] {
                        ctx.error = error_code::unknown_key;
                        return;
                     }

                     at_end = it == end;
                     if (!at_end && *it == ',') {
                        ++it;
                        at_end = it == end;
                     }
                  }
                  if (!at_end) [[likely]] {
                     if (csv_new_line(ctx, it, end)) {
                        return;
                     }

                     ++row;
                     at_end = it == end;
                     if (at_end) break;
                  }
                  else {
                     break;
                  }
               }
            }
         }
      }
   };

   template <uint32_t layout = rowwise, read_supported<CSV> T, class Buffer>
   [[nodiscard]] inline auto read_csv(T&& value, Buffer&& buffer)
   {
      return read<opts_csv{.layout = layout}>(value, std::forward<Buffer>(buffer));
   }

   template <uint32_t layout = rowwise, read_supported<CSV> T, class Buffer>
   [[nodiscard]] inline auto read_csv(Buffer&& buffer)
   {
      T value{};
      read<opts_csv{.layout = layout}>(value, std::forward<Buffer>(buffer));
      return value;
   }

   template <uint32_t layout = rowwise, read_supported<CSV> T, is_buffer Buffer>
   [[nodiscard]] inline error_ctx read_file_csv(T& value, const sv file_name, Buffer&& buffer)
   {
      context ctx{};
      ctx.current_file = file_name;

      const auto ec = file_to_buffer(buffer, ctx.current_file);

      if (bool(ec)) {
         return {0, ec};
      }

      return read<opts_csv{.layout = layout}>(value, buffer, ctx);
   }
}
