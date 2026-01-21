// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>

namespace glz
{
   inline constexpr size_t max_recursive_depth_limit = 256;

   enum struct error_code : uint32_t {
      // REPE compliant error codes
      none, //
      version_mismatch,
      invalid_header,
      invalid_query,
      invalid_body,
      parse_error,
      method_not_found,
      timeout,
      send_error,
      connection_failure,
      // Other errors
      end_reached, // A non-error code for non-null terminated input buffers
      partial_read_complete, // A non-error code for short circuiting partial reads
      no_read_input, //
      data_must_be_null_terminated, //
      parse_number_failure, //
      expected_brace, //
      expected_bracket, //
      expected_quote, //
      expected_comma, //
      expected_colon, //
      exceeded_static_array_size, //
      exceeded_max_recursive_depth, //
      unexpected_end, //
      expected_end_comment, //
      syntax_error, //
      unexpected_enum, //
      attempt_const_read, //
      attempt_member_func_read, //
      attempt_read_hidden, //
      invalid_nullable_read, //
      invalid_variant_object, //
      invalid_variant_array, //
      invalid_variant_string, //
      no_matching_variant_type, //
      expected_true_or_false, //
      constraint_violated, //
      // Key errors
      key_not_found, //
      unknown_key, //
      missing_key, //
      // Other errors
      invalid_flag_input, //
      invalid_escape, //
      u_requires_hex_digits, //
      unicode_escape_conversion_failure, //
      dump_int_error, //
      // File errors
      file_open_failure, //
      file_close_failure, //
      file_include_error, //
      file_extension_not_supported, //
      could_not_determine_extension, //
      // JSON pointer access errors
      nonexistent_json_ptr, //
      get_wrong_type, //
      seek_failure, //
      // Other errors
      cannot_be_referenced, //
      invalid_get, //
      invalid_get_fn, //
      invalid_call, //
      invalid_partial_key, //
      name_mismatch, //
      array_element_not_found, //
      elements_not_convertible_to_design, //
      unknown_distribution, //
      invalid_distribution_elements, //
      hostname_failure, //
      includer_error, //
      // Feature support
      feature_not_supported, //
      // JSON Pointer errors (RFC 6901)
      invalid_json_pointer, // Malformed JSON pointer syntax (e.g., "~" at end, "~2")
      // JSON Patch errors (RFC 6902)
      patch_test_failed, // Test operation value mismatch (unique to RFC 6902 test op)
      // Buffer errors
      buffer_overflow, // Write would exceed fixed buffer capacity
      invalid_length // Length exceeds allowed limit (buffer size or user-configured max)
   };

   // Unified error context for all read/write operations
   // Provides error information and byte count processed
   struct error_ctx final
   {
      size_t count{}; // Bytes processed (read or written)
      error_code ec{}; // Error code (none on success)
      std::string_view custom_error_message{}; // Human-readable error context

      // Returns true when there IS an error (matches std::error_code semantics)
      operator bool() const noexcept { return ec != error_code::none; }

      bool operator==(const error_code e) const noexcept { return ec == e; }
   };

   // Runtime context for configuration
   // We do not template the context on iterators so that it can be easily shared across buffer implementations
   // Not final: streaming_context inherits from this to add streaming-specific state
   struct context
   {
      error_code error{};
      std::string_view custom_error_message;
      // INTERNAL USE:
      uint32_t depth{}; // Nesting depth of structures (objects/arrays)
      // Used for indentation when writing and for stack overflow protection when reading
      std::string current_file; // top level file path
      // NOTE: The default constructor is valid for std::string_view, so we use this rather than {}
      // because debuggers like jumping to std::string_view initialization calls
   };

   // Concept for any context type (base or streaming)
   template <class T>
   concept is_context = requires(T& ctx) {
      { ctx.error } -> std::same_as<error_code&>;
      { ctx.depth } -> std::same_as<uint32_t&>;
   };

   // Runtime constraint concepts
   // These detect if a user-defined context has runtime constraint fields.
   // Users can inherit from glz::context and add these fields for runtime limits:
   //   struct my_context : glz::context {
   //      size_t max_string_length = 1024;
   //      size_t max_array_size = 100;
   //      size_t max_map_size = 50;
   //      bool allocate_raw_pointers = false;
   //   };
   // Use with if constexpr to ensure zero binary overhead when not used.

   template <class Ctx>
   concept has_runtime_max_string_length = requires(Ctx& ctx) {
      { ctx.max_string_length } -> std::convertible_to<size_t>;
   };

   template <class Ctx>
   concept has_runtime_max_array_size = requires(Ctx& ctx) {
      { ctx.max_array_size } -> std::convertible_to<size_t>;
   };

   template <class Ctx>
   concept has_runtime_max_map_size = requires(Ctx& ctx) {
      { ctx.max_map_size } -> std::convertible_to<size_t>;
   };

   template <class Ctx>
   concept has_runtime_allocate_raw_pointers = requires(Ctx& ctx) {
      { ctx.allocate_raw_pointers } -> std::convertible_to<bool>;
   };
}
