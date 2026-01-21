// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include "glaze/core/context.hpp"
#include "glaze/core/meta.hpp"

template <>
struct glz::meta<glz::error_code>
{
   static constexpr sv name = "glz::error_code";
   using enum glz::error_code;
   static constexpr std::array keys{"none",
                                    "version_mismatch",
                                    "invalid_header",
                                    "invalid_query",
                                    "invalid_body",
                                    "parse_error",
                                    "method_not_found",
                                    "timeout",
                                    "send_error",
                                    "connection_failure",
                                    "end_reached",
                                    "partial_read_complete",
                                    "no_read_input",
                                    "data_must_be_null_terminated",
                                    "parse_number_failure",
                                    "expected_brace",
                                    "expected_bracket",
                                    "expected_quote",
                                    "expected_comma",
                                    "expected_colon",
                                    "exceeded_static_array_size",
                                    "exceeded_max_recursive_depth",
                                    "unexpected_end",
                                    "expected_end_comment",
                                    "syntax_error",
                                    "unexpected_enum",
                                    "attempt_const_read",
                                    "attempt_member_func_read",
                                    "attempt_read_hidden",
                                    "invalid_nullable_read",
                                    "invalid_variant_object",
                                    "invalid_variant_array",
                                    "invalid_variant_string",
                                    "no_matching_variant_type",
                                    "expected_true_or_false",
                                    "constraint_violated",
                                    "key_not_found",
                                    "unknown_key",
                                    "missing_key",
                                    "invalid_flag_input",
                                    "invalid_escape",
                                    "u_requires_hex_digits",
                                    "unicode_escape_conversion_failure",
                                    "dump_int_error",
                                    "file_open_failure",
                                    "file_close_failure",
                                    "file_include_error",
                                    "file_extension_not_supported",
                                    "could_not_determine_extension",
                                    "nonexistent_json_ptr",
                                    "get_wrong_type",
                                    "seek_failure",
                                    "cannot_be_referenced",
                                    "invalid_get",
                                    "invalid_get_fn",
                                    "invalid_call",
                                    "invalid_partial_key",
                                    "name_mismatch",
                                    "array_element_not_found",
                                    "elements_not_convertible_to_design",
                                    "unknown_distribution",
                                    "invalid_distribution_elements",
                                    "hostname_failure",
                                    "includer_error",
                                    "feature_not_supported",
                                    "invalid_json_pointer",
                                    "patch_test_failed",
                                    "buffer_overflow",
                                    "invalid_length"};
   static constexpr std::array value{none, //
                                     version_mismatch, //
                                     invalid_header, //
                                     invalid_query, //
                                     invalid_body, //
                                     parse_error, //
                                     method_not_found, //
                                     timeout, //
                                     send_error, //
                                     connection_failure, //
                                     end_reached, // A non-error code for non-null terminated input buffers
                                     partial_read_complete,
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
                                     feature_not_supported, //
                                     // JSON Pointer errors (RFC 6901)
                                     invalid_json_pointer, //
                                     // JSON Patch errors (RFC 6902)
                                     patch_test_failed, //
                                     // Buffer errors
                                     buffer_overflow, //
                                     invalid_length};
};
