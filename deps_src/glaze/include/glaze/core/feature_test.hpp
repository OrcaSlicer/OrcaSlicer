// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

// Detect constexpr std::string support
// The old GCC ABI (_GLIBCXX_USE_CXX11_ABI=0) does not have constexpr std::string::size()
// This affects features like rename_key returning std::string
#if defined(__GLIBCXX__) && defined(_GLIBCXX_USE_CXX11_ABI) && _GLIBCXX_USE_CXX11_ABI == 0
#define GLZ_HAS_CONSTEXPR_STRING 0
#else
#define GLZ_HAS_CONSTEXPR_STRING 1
#endif

namespace glz
{
   // Constexpr bool for use in if constexpr or other compile-time contexts
   // Use GLZ_HAS_CONSTEXPR_STRING macro for #if preprocessor guards
   inline constexpr bool has_constexpr_string = GLZ_HAS_CONSTEXPR_STRING;
}

// Glaze Feature Test Macros for breaking changes

// v7.0.1 moves std::error_code integration to separate optional header
//
// The glaze_error_category struct, error_category global, and make_error_code() function
// are now in <glaze/core/std_error_code.hpp> instead of being included by default.
//
// This reduces binary size by ~34KB for users who don't need std::error_code integration.
// The overhead comes from the global error_category variable with std::error_category vtable
// which forces a DATA segment with page alignment overhead.
//
// To restore std::error_code integration, include:
//   #include <glaze/core/std_error_code.hpp>
#define glaze_v7_0_1_std_error_code_header

// v7.0.0 renames write_member_functions to write_function_pointers
//
// Options:
// - 'write_member_functions' renamed to 'write_function_pointers'
// - This option now controls serialization of both member and non-member function pointers
// - check_write_member_functions() renamed to check_write_function_pointers()
//
// context struct:
// - 'indentation_level' renamed to 'depth'
// - This field tracks nesting depth of structures (objects/arrays)
// - Used for indentation when writing and stack overflow protection when reading
//
// is_context concept:
// - Now checks for 'depth' member instead of 'indentation_level'
#define glaze_v7_0_0_write_function_pointers
#define glaze_v7_0_0_depth

// v7.0.0 moves additional options out of glz::opts to inheritable options pattern
//
// Options moved out of glz::opts (use custom opts struct or opt tags in glz::meta):
// - 'quoted_num' - treat numbers as quoted strings
// - 'raw_string' - skip escape sequence processing for strings
// - 'structs_as_arrays' - serialize structs as arrays without field keys
//
// Options renamed AND moved out of glz::opts:
// - 'raw' renamed to 'unquoted' - write string values without surrounding quotes
// - 'number' renamed to 'string_as_number' - treat string types as numbers
//
// Wrapper aliases glz::raw and glz::number are deprecated (use glz::unquoted and glz::string_as_number)
//
// Migration for custom opts structs:
//   // OLD:
//   struct my_opts : glz::opts { bool raw = true; bool number = true; };
//   // NEW:
//   struct my_opts : glz::opts { bool unquoted = true; bool string_as_number = true; };
//
// New opt tags for glz::meta:
// - glz::quoted_num_opt_tag
// - glz::string_as_number_opt_tag (replaces number semantics)
// - glz::unquoted_opt_tag (replaces raw semantics)
// - glz::raw_string_opt_tag
// - glz::structs_as_arrays_opt_tag
#define glaze_v7_0_0_opts_quoted_num
#define glaze_v7_0_0_opts_string_as_number
#define glaze_v7_0_0_opts_unquoted
#define glaze_v7_0_0_opts_raw_string
#define glaze_v7_0_0_opts_structs_as_arrays

// v6.5.0 unified error_ctx and streaming I/O support
//
// error_ctx struct:
// - Field order changed: {count, ec, custom_error_message}
//   (was {ec, custom_error_message, location, includer_error})
// - 'location' field renamed to 'count' for consistency across read/write
// - 'includer_error' field removed (use custom_error_message)
//
// context struct:
// - No longer 'final' (now inheritable for streaming_context)
// - 'includer_error' field removed (use custom_error_message)
//
// is_context concept:
// - Changed from exact type match to structural concept
// - Old: std::same_as<std::decay_t<T>, context>
// - New: Checks for 'error' and 'indentation_level' members
//
// Return types:
// - Raw buffer writes (write_json(value, char*)) now return error_ctx
//   instead of expected<size_t, error_ctx>. Byte count is in error_ctx::count.
//
// New error code:
// - error_code::buffer_overflow: Returned when writing to fixed-size buffers
//   that are too small
#define glaze_v6_5_0_error_ctx

// v6.2.0 moves append_arrays and error_on_const_read out of glz::opts
// Use a custom opts struct inheriting from glz::opts to enable these options
#define glaze_v6_2_0_opts_append_arrays
#define glaze_v6_2_0_opts_error_on_const_read

// v6.2.0 changes std::array<char, N> serialization to respect array bounds instead of scanning for null terminator
// This fixes a potential buffer overflow when arrays lack null terminators
// Use const char* for null-terminated C-style string semantics
#define glaze_v6_2_0_array_char_bounds

// v6.1.0 removes bools_as_numbers from default glz::opts
#define glaze_v6_1_0_bools_as_numbers_opt

// v6.0.0 renames json/json_t.hpp to json/generic.hpp and deprecates glz::json_t alias
#define glaze_v6_0_0_generic_header

// v5.2.0 Moves `layout` CSV option into glz::opts_csv
#define glaze_v5_2_0_opts_csv

// v5.1.0 swaps the template parameters of read_supported/write_supported to enable cleaner concepts and usage
#define glaze_v5_1_0_supported_swap

// v5.0.0 removes many internal functions and concepts out of the detail namespace to enable cleaner customization
#define glaze_v5_0_0
// v5.0.0 moves to more generic read_supported and write_supported concepts
// removes concepts like `read_json_supported` and uses `read_supported<JSON, T>`
#define glaze_v5_0_0_generic_supported
// v5.0.0 makes glz::opts the default options and moves some of the compile time options out of the struct
#define glaze_v5_0_0_customized_opts

// v4.3.0 removed global glz::trace
#define glaze_v4_3_0_trace

// v4.2.3 renamed glz::tuplet::tuple to glz::tuple
#define glaze_v4_2_3_tuple

// v3.6.0 renames glz::refl to glz::reflect and does not instantiate the struct as an
// inline constexpr variable
#define glaze_v3_6_0_reflect

// v3.5.0 change glz::detail::to_json and glz::detail::from_json specializations
// to glz::to<JSON and glz::from<JSON
// The template specialization takes a uint32_t Format as the first template parameter
#define glaze_v3_5_0_to_from
