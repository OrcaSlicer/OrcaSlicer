// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <cstdint>
#include <type_traits>

#include "glaze/core/context.hpp"
#include "glaze/core/optimization_level.hpp"
#include "glaze/util/inline.hpp"
#include "glaze/util/type_traits.hpp"

namespace glz
{
   // Formats
   // Built in formats must be less than 65536
   // User defined formats can be 65536 to 4294967296
   inline constexpr uint32_t INVALID = 0;
   inline constexpr uint32_t BEVE = 1;
   inline constexpr uint32_t CBOR = 2; // RFC 8949 - Concise Binary Object Representation
   inline constexpr uint32_t JSON = 10;
   inline constexpr uint32_t JSON_PTR = 20;
   inline constexpr uint32_t MSGPACK = 30;
   inline constexpr uint32_t NDJSON = 100; // new line delimited JSON
   inline constexpr uint32_t TOML = 400;
   inline constexpr uint32_t STENCIL = 500;
   inline constexpr uint32_t MUSTACHE = 501;
   inline constexpr uint32_t CSV = 10000;
   inline constexpr uint32_t EETF = 20000;

   // Protocol formats
   inline constexpr uint32_t REPE = 30000;
   inline constexpr uint32_t REST = 30100;
   inline constexpr uint32_t JSONRPC = 30200;

   // layout
   inline constexpr uint8_t rowwise = 0;
   inline constexpr uint8_t colwise = 1;

   enum struct float_precision : uint8_t { //
      full, //
      float32 = 4, //
      float64 = 8, //
      float128 = 16 //
   };

   // We use 16 padding bytes because surrogate unicode pairs require 12 bytes
   // and we want a power of 2 buffer
   inline constexpr uint32_t padding_bytes = 16;

   // Write padding bytes simplifies our dump calculations by making sure we have significant excess
   inline constexpr size_t write_padding_bytes = 256;

   // This macro exists so that Glaze tests can change the default behavior
   // to easily run tests as if strings were not null terminated
#ifndef GLZ_NULL_TERMINATED
#define GLZ_NULL_TERMINATED true
#endif

   enum struct opts_internal : uint32_t {
      none = 0,
      opening_handled = 1 << 0, // the opening character has been handled
      closing_handled = 1 << 1, // the closing character has been handled
      ws_handled = 1 << 2, // whitespace has already been parsed
      no_header = 1 << 3, // whether or not a binary header is needed
      disable_write_unknown =
         1 << 4, // whether to turn off writing unknown fields for a glz::meta specialized for unknown writing
      is_padded = 1 << 5, // whether or not the read buffer is padded
      disable_padding = 1 << 6, // to explicitly disable padding for contexts like includers
      write_unchecked = 1 << 7 // the write buffer has sufficient space and does not need to be checked
   };

   // NOTE TO USER:
   // glz::opts are the default options for using Glaze
   // You can create your own options struct with more or less fields as long as your struct has:
   // - opts_internal internal{};
   // - uint32_t format
   // The recommended approach is to inherit:
   // struct custom_opts : glz::opts {
   //   bool validate_trailing_whitespace = true;
   // };

   struct opts
   {
      // USER CONFIGURABLE
      uint32_t format = JSON;
      bool null_terminated = GLZ_NULL_TERMINATED; // Whether the input buffer is null terminated
      bool comments = false; // Support reading in JSONC style comments
      bool error_on_unknown_keys = true; // Error when an unknown key is encountered
      bool skip_null_members = true; // Skip writing out params in an object if the value is null
      bool prettify = false; // Write out prettified JSON
      bool minified = false; // Require minified input for JSON, which results in faster read performance
      bool error_on_missing_keys = false; // Require all non nullable keys to be present in the object. Use
                                          // skip_null_members = false to require nullable members

      bool partial_read =
         false; // Reads into the deepest structural object and then exits without parsing the rest of the input

      // INTERNAL OPTIONS
      uint32_t internal{}; // default should be 0

      [[nodiscard]] constexpr bool operator==(const opts&) const noexcept = default;
   };

   // CSV Format Options
   // Note: You can always create your own options struct if you want to share it between formats
   struct opts_csv
   {
      uint32_t format = CSV;
      static constexpr bool null_terminated = true; // Whether the input buffer is null terminated
      uint8_t layout = rowwise; // CSV row wise output/input
      bool use_headers = true; // Whether to write column/row headers in CSV format
      bool raw_string = false; // do not decode/encode escaped characters for strings (improves read/write performance)

      // New options for headerless CSV support
      bool skip_header_row =
         false; // Skip first row when reading (useful for CSV with headers when reading into 2D arrays)
      bool validate_rectangular = false; // Ensure all rows have the same column count when reading 2D arrays

      // INTERNAL OPTIONS
      uint32_t internal{}; // default should be 0

      [[nodiscard]] constexpr bool operator==(const opts_csv&) const noexcept = default;
   };

   // Add these fields to a custom options struct if you want to use them
   // OTHER AVAILABLE OPTIONS (and default values):

   // ---
   // bool bools_as_numbers = false;
   // Read and write booleans with 1's and 0's

   // ---
   // bool validate_skipped = false;
   // If full validation should be performed on skipped values

   // ---
   // bool write_function_pointers = false;
   // If function pointers (member and non-member) should be serialized when provided in glz::meta

   // ---
   // bool validate_trailing_whitespace = false;
   // If, after parsing a value, we want to validate the trailing whitespace

   // ---
   // bool concatenate = true;
   // Concatenates ranges of std::pair into single objects when writing

   // ---
   // bool allow_conversions = true;
   // Whether conversions between convertible types are allowed in BEVE, e.g. double -> float

   // ---
   // bool write_type_info = true;
   // Write type info for meta objects in variants

   // ---
   // bool append_arrays = false;
   // When reading into an array the data will be appended if the type supports it

   // ---
   // bool shrink_to_fit = false;
   // Shrinks dynamic containers to new size to save memory

   // ---
   // bool error_on_const_read = false;
   // Error if attempt is made to read into a const value, by default the value is skipped without error

   // ---
   // bool hide_non_invocable = true;
   // Hides non-invocable members from the cli_menu (may be applied elsewhere in the future)

   // ---
   // bool escape_control_characters = false;
   // Escapes control characters like 0x01 or null characters with proper unicode escape sequences.
   // The default behavior does not escape these characters for performance and safety
   // (embedding nulls can cause issues, especially with C APIs)
   // Glaze will error when parsing non-escaped control character (per the JSON spec)
   // This option allows escaping control characters to avoid such errors.

   // ---
   // float_precision float_max_write_precision{};
   // The maximum precision type used for writing floats, higher precision floats will be cast down to this precision

   // ---
   // static constexpr std::string_view float_format = "{:.2f}";
   // Format string for floating-point output using std::format (C++23)
   // Must be a static constexpr member to be usable as a template parameter
   // Example: "{:.2f}" for 2 decimal places, "{:.6g}" for 6 significant digits, "{:.0f}" for integers
   // When specified, uses std::format_to instead of Dragonbox for float/double serialization

   // ---
   // bool skip_self_constraint = false;
   // Skip self_constraint validation during reading. Useful for performance when constraints are known to be valid
   // or when validation should be deferred.

   // ---
   // bool linear_search = false;
   // Uses linear key search instead of hash-based lookup for JSON object fields.
   // This eliminates 256-byte hash tables per struct type, significantly reducing binary size.
   // Trades O(1) hash lookup for O(N) linear search - faster for small structs (< ~8 fields)
   // due to cache effects, and much smaller binaries for embedded systems.

   // ---
   // optimization_level optimization_level = optimization_level::normal;
   // Controls speed vs binary size tradeoff. See glaze/core/optimization_level.hpp for details.
   // Levels: size, normal (default)
   // Use preset struct: glz::opts_size

   // ---
   // size_t max_string_length = 0;
   // Maximum length for string allocations when reading. 0 means no limit (default).
   // When set, strings exceeding this length will fail with error_code::invalid_length.
   // Useful for preventing memory exhaustion attacks from malicious input.
   // Currently applies to BEVE format.

   // ---
   // size_t max_array_size = 0;
   // Maximum size for array/vector allocations when reading. 0 means no limit (default).
   // When set, arrays exceeding this size will fail with error_code::invalid_length.
   // Useful for preventing memory exhaustion attacks from malicious input.
   // Currently applies to BEVE format.

   // ---
   // bool allocate_raw_pointers = false;
   // When true, allows Glaze to allocate memory for null raw pointers during deserialization using `new`.
   // By default, Glaze refuses to read into null raw pointers because it would have to call `new`
   // without any known way to delete the memory, making memory leaks easy.
   // Enable this option only when you are prepared to manually manage the allocated memory.
   // Works with JSON, BEVE, CBOR, and MSGPACK formats.

   // ---
   // char indentation_char = ' ';
   // Prettified JSON indentation character. Use '\t' for tabs.

   // ---
   // uint8_t indentation_width = 3;
   // Prettified JSON indentation size (number of indentation_char per level).

   // ---
   // bool new_lines_in_arrays = true;
   // Whether prettified arrays should have new lines for each element.
   // Set to false for more compact array output.

   // ---
   // bool quoted_num = false;
   // Treat numbers as quoted strings. When writing, numbers are wrapped in quotes: 123 -> "123"
   // When reading, quoted numbers are parsed: "123" -> 123
   // Useful for APIs that require numeric values as strings.

   // ---
   // bool string_as_number = false;
   // Treat string types (like std::string) as numbers during serialization.
   // When writing, strings are output without quotes. When reading, unquoted values are read into strings.
   // Useful for reading/writing numeric strings without quote overhead.

   // ---
   // bool unquoted = false;
   // Write string-like values without surrounding quotes.
   // The string content is still escape-processed unless raw_string is also true.

   // ---
   // bool raw_string = false;
   // Skip escape sequence encoding/decoding for strings.
   // Improves read/write performance when strings are known to not contain escape characters.
   // Can be combined with 'raw' to write completely unprocessed string content.

   // ---
   // bool structs_as_arrays = false;
   // Serialize/deserialize structs as arrays without field keys.
   // Useful for binary formats or when field names add unnecessary overhead.
   // Order of fields is determined by the glz::meta definition.

   struct append_arrays_opt_tag
   {};

   struct bools_as_numbers_opt_tag
   {};

   struct escape_control_characters_opt_tag
   {};

   struct quoted_num_opt_tag
   {};

   struct string_as_number_opt_tag
   {};

   struct unquoted_opt_tag
   {};

   struct raw_string_opt_tag
   {};

   struct structs_as_arrays_opt_tag
   {};

   // Helper for deprecated option static_asserts (dependent false pattern)
   template <class>
   inline constexpr bool deprecated_opts_raw = false;
   template <class>
   inline constexpr bool deprecated_opts_number = false;

   template <auto member_ptr>
   concept is_append_arrays_tag = std::same_as<std::decay_t<decltype(member_ptr)>, append_arrays_opt_tag>;

   template <auto member_ptr>
   concept is_bools_as_numbers_tag = std::same_as<std::decay_t<decltype(member_ptr)>, bools_as_numbers_opt_tag>;

   template <auto member_ptr>
   concept is_escape_control_characters_tag =
      std::same_as<std::decay_t<decltype(member_ptr)>, escape_control_characters_opt_tag>;

   template <auto member_ptr>
   concept is_quoted_num_tag = std::same_as<std::decay_t<decltype(member_ptr)>, quoted_num_opt_tag>;

   template <auto member_ptr>
   concept is_string_as_number_tag = std::same_as<std::decay_t<decltype(member_ptr)>, string_as_number_opt_tag>;

   template <auto member_ptr>
   concept is_unquoted_tag = std::same_as<std::decay_t<decltype(member_ptr)>, unquoted_opt_tag>;

   template <auto member_ptr>
   concept is_raw_string_tag = std::same_as<std::decay_t<decltype(member_ptr)>, raw_string_opt_tag>;

   template <auto member_ptr>
   concept is_structs_as_arrays_tag = std::same_as<std::decay_t<decltype(member_ptr)>, structs_as_arrays_opt_tag>;

   consteval bool check_validate_skipped(auto&& Opts)
   {
      if constexpr (requires { Opts.validate_skipped; }) {
         return Opts.validate_skipped;
      }
      else {
         return false;
      }
   }

   consteval bool check_append_arrays(auto&& Opts)
   {
      if constexpr (requires { Opts.append_arrays; }) {
         return Opts.append_arrays;
      }
      else {
         return false;
      }
   }

   consteval bool check_error_on_const_read(auto&& Opts)
   {
      if constexpr (requires { Opts.error_on_const_read; }) {
         return Opts.error_on_const_read;
      }
      else {
         return false;
      }
   }

   consteval bool check_write_function_pointers(auto&& Opts)
   {
      if constexpr (requires { Opts.write_function_pointers; }) {
         return Opts.write_function_pointers;
      }
      else {
         return false;
      }
   }

   consteval bool check_bools_as_numbers(auto&& Opts)
   {
      if constexpr (requires { Opts.bools_as_numbers; }) {
         return Opts.bools_as_numbers;
      }
      else {
         return false;
      }
   }

   consteval bool check_validate_trailing_whitespace(auto&& Opts)
   {
      if constexpr (requires { Opts.validate_trailing_whitespace; }) {
         return Opts.validate_trailing_whitespace;
      }
      else {
         return false;
      }
   }

   consteval bool check_partial_read(auto&& Opts)
   {
      if constexpr (requires { Opts.partial_read; }) {
         return Opts.partial_read;
      }
      else {
         return false;
      }
   }

   consteval bool check_concatenate(auto&& Opts)
   {
      if constexpr (requires { Opts.concatenate; }) {
         return Opts.concatenate;
      }
      else {
         return true;
      }
   }

   consteval bool check_allow_conversions(auto&& Opts)
   {
      if constexpr (requires { Opts.allow_conversions; }) {
         return Opts.allow_conversions;
      }
      else {
         return true;
      }
   }

   consteval bool check_write_type_info(auto&& Opts)
   {
      if constexpr (requires { Opts.write_type_info; }) {
         return Opts.write_type_info;
      }
      else {
         return true;
      }
   }

   consteval bool check_shrink_to_fit(auto&& Opts)
   {
      if constexpr (requires { Opts.shrink_to_fit; }) {
         return Opts.shrink_to_fit;
      }
      else {
         return false;
      }
   }

   consteval bool check_hide_non_invocable(auto&& Opts)
   {
      if constexpr (requires { Opts.hide_non_invocable; }) {
         return Opts.hide_non_invocable;
      }
      else {
         return true;
      }
   }

   consteval bool check_escape_control_characters(auto&& Opts)
   {
      if constexpr (requires { Opts.escape_control_characters; }) {
         return Opts.escape_control_characters;
      }
      else {
         return false;
      }
   }

   consteval bool check_skip_null_members_on_read(auto&& Opts)
   {
      if constexpr (requires { Opts.skip_null_members_on_read; }) {
         return Opts.skip_null_members_on_read;
      }
      else {
         return false;
      }
   }

   consteval bool check_use_headers(auto&& Opts)
   {
      if constexpr (requires { Opts.use_headers; }) {
         return Opts.use_headers;
      }
      else {
         return true;
      }
   }

   consteval bool check_raw_string(auto&& Opts)
   {
      if constexpr (requires { Opts.raw_string; }) {
         return Opts.raw_string;
      }
      else {
         return false;
      }
   }

   consteval bool check_quoted_num(auto&& Opts)
   {
      if constexpr (requires { Opts.quoted_num; }) {
         return Opts.quoted_num;
      }
      else {
         return false;
      }
   }

   consteval bool check_string_as_number(auto&& Opts)
   {
      if constexpr (requires { Opts.string_as_number; }) {
         return Opts.string_as_number;
      }
      else if constexpr (requires { Opts.number; }) {
         static_assert(deprecated_opts_number<std::decay_t<decltype(Opts)>>,
                       "\n\n"
                       "  [glaze] Deprecated: 'number' option has been renamed to 'string_as_number'\n"
                       "  \n"
                       "  Please update your custom opts struct:\n"
                       "  \n"
                       "    // OLD (deprecated):\n"
                       "    struct my_opts : glz::opts {\n"
                       "       bool number = true;\n"
                       "    };\n"
                       "  \n"
                       "    // NEW:\n"
                       "    struct my_opts : glz::opts {\n"
                       "       bool string_as_number = true;\n"
                       "    };\n"
                       "\n");
         return Opts.number;
      }
      else {
         return false;
      }
   }

   consteval bool check_unquoted(auto&& Opts)
   {
      if constexpr (requires { Opts.unquoted; }) {
         return Opts.unquoted;
      }
      else if constexpr (requires { Opts.raw; }) {
         static_assert(deprecated_opts_raw<std::decay_t<decltype(Opts)>>,
                       "\n\n"
                       "  [glaze] Deprecated: 'raw' option has been renamed to 'unquoted'\n"
                       "  \n"
                       "  Please update your custom opts struct:\n"
                       "  \n"
                       "    // OLD (deprecated):\n"
                       "    struct my_opts : glz::opts {\n"
                       "       bool raw = true;\n"
                       "    };\n"
                       "  \n"
                       "    // NEW:\n"
                       "    struct my_opts : glz::opts {\n"
                       "       bool unquoted = true;\n"
                       "    };\n"
                       "\n");
         return Opts.raw;
      }
      else {
         return false;
      }
   }

   consteval bool check_structs_as_arrays(auto&& Opts)
   {
      if constexpr (requires { Opts.structs_as_arrays; }) {
         return Opts.structs_as_arrays;
      }
      else {
         return false;
      }
   }

   consteval uint8_t check_layout(auto&& Opts)
   {
      if constexpr (requires { Opts.layout; }) {
         return Opts.layout;
      }
      else {
         return rowwise;
      }
   }

   consteval char check_indentation_char(auto&& Opts)
   {
      if constexpr (requires { Opts.indentation_char; }) {
         return Opts.indentation_char;
      }
      else {
         return ' ';
      }
   }

   consteval uint8_t check_indentation_width(auto&& Opts)
   {
      if constexpr (requires { Opts.indentation_width; }) {
         return Opts.indentation_width;
      }
      else {
         return 3;
      }
   }

   consteval bool check_new_lines_in_arrays(auto&& Opts)
   {
      if constexpr (requires { Opts.new_lines_in_arrays; }) {
         return Opts.new_lines_in_arrays;
      }
      else {
         return true;
      }
   }

   consteval float_precision check_float_max_write_precision(auto&& Opts)
   {
      if constexpr (requires { Opts.float_max_write_precision; }) {
         return Opts.float_max_write_precision;
      }
      else {
         return {};
      }
   }

   consteval bool check_skip_self_constraint(auto&& Opts)
   {
      if constexpr (requires { Opts.skip_self_constraint; }) {
         return Opts.skip_self_constraint;
      }
      else {
         return false;
      }
   }

   consteval bool check_assume_sufficient_buffer(auto&& Opts)
   {
      if constexpr (requires { Opts.assume_sufficient_buffer; }) {
         return Opts.assume_sufficient_buffer;
      }
      else {
         return false;
      }
   }

   consteval bool check_linear_search(auto&& Opts)
   {
      if constexpr (requires { Opts.linear_search; }) {
         return Opts.linear_search;
      }
      else {
         // In size mode, default to linear search (no hash tables)
         if constexpr (requires { Opts.optimization_level; }) {
            return Opts.optimization_level == optimization_level::size;
         }
         return false;
      }
   }

   consteval size_t check_max_string_length(auto&& Opts)
   {
      if constexpr (requires { Opts.max_string_length; }) {
         return Opts.max_string_length;
      }
      else {
         return 0; // 0 means no limit
      }
   }

   consteval size_t check_max_array_size(auto&& Opts)
   {
      if constexpr (requires { Opts.max_array_size; }) {
         return Opts.max_array_size;
      }
      else {
         return 0; // 0 means no limit
      }
   }

   consteval size_t check_max_map_size(auto&& Opts)
   {
      if constexpr (requires { Opts.max_map_size; }) {
         return Opts.max_map_size;
      }
      else {
         return 0; // 0 means no limit
      }
   }

   consteval bool check_allocate_raw_pointers(auto&& Opts)
   {
      if constexpr (requires { Opts.allocate_raw_pointers; }) {
         return Opts.allocate_raw_pointers;
      }
      else {
         return false;
      }
   }

   consteval optimization_level check_optimization_level(auto&& Opts)
   {
      if constexpr (requires { Opts.optimization_level; }) {
         return Opts.optimization_level;
      }
      else {
         return optimization_level::normal;
      }
   }

   consteval bool is_size_optimized(auto&& Opts) { return check_optimization_level(Opts) == optimization_level::size; }

   // Check if raw pointer allocation is possible (either compile-time or runtime option available)
   template <auto Opts, class Ctx>
   concept can_allocate_raw_pointer = check_allocate_raw_pointers(Opts) || has_runtime_allocate_raw_pointers<Ctx>;

   // Helper to attempt allocation of a null raw pointer during deserialization.
   // Compile-time allocate_raw_pointers option takes precedence over runtime context.
   // Returns true on success, false on failure (with ctx.error set to invalid_nullable_read).
   // Only call this when can_allocate_raw_pointer<Opts, Ctx> is satisfied.
   template <auto Opts, class Ptr, class Ctx>
      requires(std::is_pointer_v<std::remove_cvref_t<Ptr>> && can_allocate_raw_pointer<Opts, std::decay_t<Ctx>>)
   GLZ_ALWAYS_INLINE constexpr bool try_allocate_raw_pointer(Ptr& value, Ctx& ctx) noexcept
   {
      using PtrType = std::remove_cvref_t<Ptr>;
      using PointedType = std::remove_pointer_t<PtrType>;

      if constexpr (check_allocate_raw_pointers(Opts)) {
         value = new PointedType{};
         return true;
      }
      else if constexpr (has_runtime_allocate_raw_pointers<std::decay_t<Ctx>>) {
         if (ctx.allocate_raw_pointers) {
            value = new PointedType{};
            return true;
         }
         ctx.error = error_code::invalid_nullable_read;
         return false;
      }
   }

   consteval bool check_opening_handled(auto&& o) { return o.internal & uint32_t(opts_internal::opening_handled); }

   consteval bool check_closing_handled(auto&& o) { return o.internal & uint32_t(opts_internal::closing_handled); }

   consteval bool check_ws_handled(auto&& o) { return o.internal & uint32_t(opts_internal::ws_handled); }

   consteval bool check_no_header(auto&& o) { return o.internal & uint32_t(opts_internal::no_header); }

   consteval bool check_disable_write_unknown(auto&& o)
   {
      return o.internal & uint32_t(opts_internal::disable_write_unknown);
   }

   consteval bool check_is_padded(auto&& o) { return o.internal & uint32_t(opts_internal::is_padded); }

   consteval bool check_disable_padding(auto&& o) { return o.internal & uint32_t(opts_internal::disable_padding); }

   consteval bool check_write_unchecked(auto&& o) { return o.internal & uint32_t(opts_internal::write_unchecked); }

   template <auto Opts>
   constexpr auto opening_handled()
   {
      auto ret = Opts;
      ret.internal |= uint32_t(opts_internal::opening_handled);
      return ret;
   }

   template <auto Opts>
   constexpr auto opening_and_closing_handled()
   {
      auto ret = Opts;
      ret.internal |= (uint32_t(opts_internal::opening_handled) | uint32_t(opts_internal::closing_handled));
      return ret;
   }

   template <auto Opts>
   constexpr auto opening_handled_off()
   {
      auto ret = Opts;
      ret.internal &= ~uint32_t(opts_internal::opening_handled);
      return ret;
   }

   template <auto Opts>
   constexpr auto opening_and_closing_handled_off()
   {
      auto ret = Opts;
      ret.internal &= ~(uint32_t(opts_internal::opening_handled) | uint32_t(opts_internal::closing_handled));
      return ret;
   }

   // Skip ws_handled optimization when minified (skip_ws is a no-op, so the flag is pointless)
   template <auto Opts>
   constexpr auto ws_handled()
   {
      if constexpr (Opts.minified) {
         return Opts;
      }
      else {
         auto ret = Opts;
         ret.internal |= uint32_t(opts_internal::ws_handled);
         return ret;
      }
   }

   template <auto Opts>
   constexpr auto ws_handled_off()
   {
      auto ret = Opts;
      ret.internal &= ~uint32_t(opts_internal::ws_handled);
      return ret;
   }

   template <auto Opts>
   constexpr auto no_header_on()
   {
      auto ret = Opts;
      ret.internal |= uint32_t(opts_internal::no_header);
      return ret;
   }

   template <auto Opts>
   constexpr auto no_header_off()
   {
      auto ret = Opts;
      ret.internal &= ~uint32_t(opts_internal::no_header);
      return ret;
   }

   template <auto Opts>
   constexpr auto is_padded_on()
   {
      auto ret = Opts;
      ret.internal |= uint32_t(opts_internal::is_padded);
      return ret;
   }

   template <auto Opts>
   constexpr auto is_padded_off()
   {
      auto ret = Opts;
      ret.internal &= ~uint32_t(opts_internal::is_padded);
      return ret;
   }

   template <auto Opts>
   constexpr auto disable_padding_on()
   {
      auto ret = Opts;
      ret.internal |= uint32_t(opts_internal::disable_padding);
      return ret;
   }

   template <auto Opts>
   constexpr auto disable_padding_off()
   {
      auto ret = Opts;
      ret.internal &= ~uint32_t(opts_internal::disable_padding);
      return ret;
   }

   template <auto Opts>
   constexpr auto write_unchecked_on()
   {
      auto ret = Opts;
      ret.internal |= uint32_t(opts_internal::write_unchecked);
      return ret;
   }

   template <auto Opts>
   constexpr auto write_unchecked_off()
   {
      auto ret = Opts;
      ret.internal &= ~uint32_t(opts_internal::write_unchecked);
      return ret;
   }

   template <auto Opts, auto member_ptr>
   constexpr auto set_opt(auto&& value)
   {
      if constexpr (is_append_arrays_tag<member_ptr>) {
         if constexpr (requires { Opts.append_arrays; }) {
            auto ret = Opts;
            ret.append_arrays = static_cast<bool>(value);
            return ret;
         }
         else {
            struct opts_append_arrays : std::decay_t<decltype(Opts)>
            {
               bool append_arrays{};
            };
            return opts_append_arrays{{Opts}, static_cast<bool>(value)};
         }
      }
      else if constexpr (is_bools_as_numbers_tag<member_ptr>) {
         if constexpr (requires { Opts.bools_as_numbers; }) {
            auto ret = Opts;
            ret.bools_as_numbers = static_cast<bool>(value);
            return ret;
         }
         else {
            struct opts_bools_as_numbers : std::decay_t<decltype(Opts)>
            {
               bool bools_as_numbers{};
            };
            return opts_bools_as_numbers{{Opts}, static_cast<bool>(value)};
         }
      }
      else if constexpr (is_escape_control_characters_tag<member_ptr>) {
         if constexpr (requires { Opts.escape_control_characters; }) {
            auto ret = Opts;
            ret.escape_control_characters = static_cast<bool>(value);
            return ret;
         }
         else {
            struct opts_escape_control_characters : std::decay_t<decltype(Opts)>
            {
               bool escape_control_characters{};
            };
            return opts_escape_control_characters{{Opts}, static_cast<bool>(value)};
         }
      }
      else if constexpr (is_quoted_num_tag<member_ptr>) {
         if constexpr (requires { Opts.quoted_num; }) {
            auto ret = Opts;
            ret.quoted_num = static_cast<bool>(value);
            return ret;
         }
         else {
            struct opts_quoted_num : std::decay_t<decltype(Opts)>
            {
               bool quoted_num{};
            };
            return opts_quoted_num{{Opts}, static_cast<bool>(value)};
         }
      }
      else if constexpr (is_string_as_number_tag<member_ptr>) {
         if constexpr (requires { Opts.string_as_number; }) {
            auto ret = Opts;
            ret.string_as_number = static_cast<bool>(value);
            return ret;
         }
         else if constexpr (requires { Opts.number; }) {
            static_assert(deprecated_opts_number<std::decay_t<decltype(Opts)>>,
                          "\n\n"
                          "  [glaze] Deprecated: 'number' option has been renamed to 'string_as_number'\n"
                          "  Please rename 'bool number' to 'bool string_as_number' in your custom opts struct.\n"
                          "\n");
            auto ret = Opts;
            ret.number = static_cast<bool>(value);
            return ret;
         }
         else {
            struct opts_string_as_number : std::decay_t<decltype(Opts)>
            {
               bool string_as_number{};
            };
            return opts_string_as_number{{Opts}, static_cast<bool>(value)};
         }
      }
      else if constexpr (is_unquoted_tag<member_ptr>) {
         if constexpr (requires { Opts.unquoted; }) {
            auto ret = Opts;
            ret.unquoted = static_cast<bool>(value);
            return ret;
         }
         else if constexpr (requires { Opts.raw; }) {
            static_assert(deprecated_opts_raw<std::decay_t<decltype(Opts)>>,
                          "\n\n"
                          "  [glaze] Deprecated: 'raw' option has been renamed to 'unquoted'\n"
                          "  Please rename 'bool raw' to 'bool unquoted' in your custom opts struct.\n"
                          "\n");
            auto ret = Opts;
            ret.raw = static_cast<bool>(value);
            return ret;
         }
         else {
            struct opts_unquoted : std::decay_t<decltype(Opts)>
            {
               bool unquoted{};
            };
            return opts_unquoted{{Opts}, static_cast<bool>(value)};
         }
      }
      else if constexpr (is_raw_string_tag<member_ptr>) {
         if constexpr (requires { Opts.raw_string; }) {
            auto ret = Opts;
            ret.raw_string = static_cast<bool>(value);
            return ret;
         }
         else {
            struct opts_raw_string : std::decay_t<decltype(Opts)>
            {
               bool raw_string{};
            };
            return opts_raw_string{{Opts}, static_cast<bool>(value)};
         }
      }
      else if constexpr (is_structs_as_arrays_tag<member_ptr>) {
         if constexpr (requires { Opts.structs_as_arrays; }) {
            auto ret = Opts;
            ret.structs_as_arrays = static_cast<bool>(value);
            return ret;
         }
         else {
            struct opts_structs_as_arrays : std::decay_t<decltype(Opts)>
            {
               bool structs_as_arrays{};
            };
            return opts_structs_as_arrays{{Opts}, static_cast<bool>(value)};
         }
      }
      else {
         auto ret = Opts;
         ret.*member_ptr = value;
         return ret;
      }
   }

   template <auto Opts, auto member_ptr>
   constexpr auto opt_on()
   {
      if constexpr (is_append_arrays_tag<member_ptr>) {
         if constexpr (requires { Opts.append_arrays; }) {
            auto ret = Opts;
            ret.append_arrays = true;
            return ret;
         }
         else {
            struct opts_append_arrays : std::decay_t<decltype(Opts)>
            {
               bool append_arrays = true;
            };
            return opts_append_arrays{{Opts}};
         }
      }
      else if constexpr (is_bools_as_numbers_tag<member_ptr>) {
         if constexpr (requires { Opts.bools_as_numbers; }) {
            auto ret = Opts;
            ret.bools_as_numbers = true;
            return ret;
         }
         else {
            struct opts_bools_as_numbers : std::decay_t<decltype(Opts)>
            {
               bool bools_as_numbers = true;
            };
            return opts_bools_as_numbers{{Opts}};
         }
      }
      else if constexpr (is_escape_control_characters_tag<member_ptr>) {
         if constexpr (requires { Opts.escape_control_characters; }) {
            auto ret = Opts;
            ret.escape_control_characters = true;
            return ret;
         }
         else {
            struct opts_escape_control_characters : std::decay_t<decltype(Opts)>
            {
               bool escape_control_characters = true;
            };
            return opts_escape_control_characters{{Opts}};
         }
      }
      else if constexpr (is_quoted_num_tag<member_ptr>) {
         if constexpr (requires { Opts.quoted_num; }) {
            auto ret = Opts;
            ret.quoted_num = true;
            return ret;
         }
         else {
            struct opts_quoted_num : std::decay_t<decltype(Opts)>
            {
               bool quoted_num = true;
            };
            return opts_quoted_num{{Opts}};
         }
      }
      else if constexpr (is_string_as_number_tag<member_ptr>) {
         if constexpr (requires { Opts.string_as_number; }) {
            auto ret = Opts;
            ret.string_as_number = true;
            return ret;
         }
         else if constexpr (requires { Opts.number; }) {
            static_assert(deprecated_opts_number<std::decay_t<decltype(Opts)>>,
                          "\n\n"
                          "  [glaze] Deprecated: 'number' option has been renamed to 'string_as_number'\n"
                          "  Please rename 'bool number' to 'bool string_as_number' in your custom opts struct.\n"
                          "\n");
            auto ret = Opts;
            ret.number = true;
            return ret;
         }
         else {
            struct opts_string_as_number : std::decay_t<decltype(Opts)>
            {
               bool string_as_number = true;
            };
            return opts_string_as_number{{Opts}};
         }
      }
      else if constexpr (is_unquoted_tag<member_ptr>) {
         if constexpr (requires { Opts.unquoted; }) {
            auto ret = Opts;
            ret.unquoted = true;
            return ret;
         }
         else if constexpr (requires { Opts.raw; }) {
            static_assert(deprecated_opts_raw<std::decay_t<decltype(Opts)>>,
                          "\n\n"
                          "  [glaze] Deprecated: 'raw' option has been renamed to 'unquoted'\n"
                          "  Please rename 'bool raw' to 'bool unquoted' in your custom opts struct.\n"
                          "\n");
            auto ret = Opts;
            ret.raw = true;
            return ret;
         }
         else {
            struct opts_unquoted : std::decay_t<decltype(Opts)>
            {
               bool unquoted = true;
            };
            return opts_unquoted{{Opts}};
         }
      }
      else if constexpr (is_raw_string_tag<member_ptr>) {
         if constexpr (requires { Opts.raw_string; }) {
            auto ret = Opts;
            ret.raw_string = true;
            return ret;
         }
         else {
            struct opts_raw_string : std::decay_t<decltype(Opts)>
            {
               bool raw_string = true;
            };
            return opts_raw_string{{Opts}};
         }
      }
      else if constexpr (is_structs_as_arrays_tag<member_ptr>) {
         if constexpr (requires { Opts.structs_as_arrays; }) {
            auto ret = Opts;
            ret.structs_as_arrays = true;
            return ret;
         }
         else {
            struct opts_structs_as_arrays : std::decay_t<decltype(Opts)>
            {
               bool structs_as_arrays = true;
            };
            return opts_structs_as_arrays{{Opts}};
         }
      }
      else {
         auto ret = Opts;
         ret.*member_ptr = true;
         return ret;
      }
   }

   template <auto Opts, auto member_ptr>
   inline constexpr auto opt_true = opt_on<Opts, member_ptr>();

   template <auto Opts, auto member_ptr>
   constexpr auto opt_off()
   {
      if constexpr (is_append_arrays_tag<member_ptr>) {
         if constexpr (requires { Opts.append_arrays; }) {
            auto ret = Opts;
            ret.append_arrays = false;
            return ret;
         }
         else {
            return Opts;
         }
      }
      else if constexpr (is_bools_as_numbers_tag<member_ptr>) {
         if constexpr (requires { Opts.bools_as_numbers; }) {
            auto ret = Opts;
            ret.bools_as_numbers = false;
            return ret;
         }
         else {
            return Opts;
         }
      }
      else if constexpr (is_escape_control_characters_tag<member_ptr>) {
         if constexpr (requires { Opts.escape_control_characters; }) {
            auto ret = Opts;
            ret.escape_control_characters = false;
            return ret;
         }
         else {
            return Opts;
         }
      }
      else if constexpr (is_quoted_num_tag<member_ptr>) {
         if constexpr (requires { Opts.quoted_num; }) {
            auto ret = Opts;
            ret.quoted_num = false;
            return ret;
         }
         else {
            return Opts;
         }
      }
      else if constexpr (is_string_as_number_tag<member_ptr>) {
         if constexpr (requires { Opts.string_as_number; }) {
            auto ret = Opts;
            ret.string_as_number = false;
            return ret;
         }
         else if constexpr (requires { Opts.number; }) {
            static_assert(deprecated_opts_number<std::decay_t<decltype(Opts)>>,
                          "\n\n"
                          "  [glaze] Deprecated: 'number' option has been renamed to 'string_as_number'\n"
                          "  Please rename 'bool number' to 'bool string_as_number' in your custom opts struct.\n"
                          "\n");
            auto ret = Opts;
            ret.number = false;
            return ret;
         }
         else {
            return Opts;
         }
      }
      else if constexpr (is_unquoted_tag<member_ptr>) {
         if constexpr (requires { Opts.unquoted; }) {
            auto ret = Opts;
            ret.unquoted = false;
            return ret;
         }
         else if constexpr (requires { Opts.raw; }) {
            static_assert(deprecated_opts_raw<std::decay_t<decltype(Opts)>>,
                          "\n\n"
                          "  [glaze] Deprecated: 'raw' option has been renamed to 'unquoted'\n"
                          "  Please rename 'bool raw' to 'bool unquoted' in your custom opts struct.\n"
                          "\n");
            auto ret = Opts;
            ret.raw = false;
            return ret;
         }
         else {
            return Opts;
         }
      }
      else if constexpr (is_raw_string_tag<member_ptr>) {
         if constexpr (requires { Opts.raw_string; }) {
            auto ret = Opts;
            ret.raw_string = false;
            return ret;
         }
         else {
            return Opts;
         }
      }
      else if constexpr (is_structs_as_arrays_tag<member_ptr>) {
         if constexpr (requires { Opts.structs_as_arrays; }) {
            auto ret = Opts;
            ret.structs_as_arrays = false;
            return ret;
         }
         else {
            return Opts;
         }
      }
      else {
         auto ret = Opts;
         ret.*member_ptr = false;
         return ret;
      }
   }

   template <auto Opts, auto member_ptr>
   inline constexpr auto opt_false = opt_off<Opts, member_ptr>();

   template <auto Opts>
   constexpr auto disable_write_unknown_off()
   {
      auto ret = Opts;
      ret.internal &= ~uint32_t(opts_internal::disable_write_unknown);
      return ret;
   }

   template <auto Opts>
   constexpr auto disable_write_unknown_on()
   {
      auto ret = Opts;
      ret.internal |= uint32_t(opts_internal::disable_write_unknown);
      return ret;
   }

   template <auto Opts>
   constexpr auto set_beve()
   {
      auto ret = Opts;
      ret.format = BEVE;
      return ret;
   }

   template <auto Opts>
   constexpr auto set_cbor()
   {
      auto ret = Opts;
      ret.format = CBOR;
      return ret;
   }

   template <auto Opts>
   constexpr auto set_json()
   {
      auto ret = Opts;
      ret.format = JSON;
      return ret;
   }

   template <auto Opts>
   constexpr auto set_msgpack()
   {
      auto ret = Opts;
      ret.format = MSGPACK;
      return ret;
   }

   template <auto Opts>
   constexpr auto set_toml()
   {
      auto ret = Opts;
      ret.format = TOML;
      return ret;
   }
}

namespace glz
{
   template <uint32_t Format = INVALID, class T = void>
   struct to;

   template <uint32_t Format = INVALID, class T = void>
   struct from;

   template <uint32_t Format = INVALID, class T = void>
   struct to_partial;

   template <uint32_t Format = INVALID>
   struct skip_value;

   template <class T, uint32_t Format>
   concept write_supported = requires { to<Format, std::remove_cvref_t<T>>{}; };

   template <class T, uint32_t Format>
   concept read_supported = requires { from<Format, std::remove_cvref_t<T>>{}; };

   // These templates save typing by determining the core type used to select the proper to/from specialization
   // Long term I would like to remove these detail indirections.

   template <uint32_t Format>
   struct parse
   {};

   template <uint32_t Format>
   struct serialize
   {};

   template <uint32_t Format>
   struct serialize_partial
   {};

   // Preset options for size-optimized builds (embedded systems)
   struct opts_size : opts
   {
      glz::optimization_level optimization_level = glz::optimization_level::size;
   };
}
