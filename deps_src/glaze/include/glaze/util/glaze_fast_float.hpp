// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

// This is an adapter to speed up the parsing portion of the fast_float conversion

#include "glaze/util/fast_float.hpp"
#include "glaze/util/inline.hpp"

namespace glz
{
   // Assuming that you use no more than 19 digits, this will
   // parse an ASCII string.
   template <bool null_terminated, class UC>
   GLZ_ALWAYS_INLINE constexpr glz::fast_float::parsed_number_string_t<UC> parse_number_string(UC const* p,
                                                                                               UC const* pend) noexcept
   {
      using namespace glz::fast_float;
      static constexpr UC decimal_point = '.';

      parsed_number_string_t<UC> answer;
      answer.valid = false;
      answer.too_many_digits = false;
      answer.negative = (*p == UC('-'));
      if (*p == UC('-')) { // C++17 20.19.3.(7.1) explicitly forbids '+' sign here
         ++p;

         if constexpr (not null_terminated) {
            if (p == pend) [[unlikely]] {
               return answer;
            }
         }

         if (!glz::fast_float::is_integer(*p)) [[unlikely]] { // a sign must be followed by an integer
            return answer;
         }
      }
      UC const* const start_digits = p;

      uint64_t i = 0; // an unsigned int avoids signed overflows (which are bad)

      uint64_t digit;
      if constexpr (null_terminated) {
         while ((digit = glz::fast_float::digit_value(*p)) <= 9) {
            // a multiplication by 10 is cheaper than an arbitrary integer
            // multiplication
            i = 10 * i + digit; // might overflow, we will handle the overflow later
            ++p;
         }
      }
      else {
         while ((p != pend) && (digit = glz::fast_float::digit_value(*p)) <= 9) {
            // a multiplication by 10 is cheaper than an arbitrary integer
            // multiplication
            i = 10 * i + digit; // might overflow, we will handle the overflow later
            ++p;
         }
      }

      UC const* const end_of_integer_part = p;
      int64_t digit_count = int64_t(end_of_integer_part - start_digits);
      answer.integer = glz::fast_float::span<const UC>(start_digits, size_t(digit_count));

      // at least 1 digit in integer part, without leading zeros
      if (digit_count == 0 || (start_digits[0] == UC('0') && digit_count > 1)) {
         return answer;
      }

      int64_t exponent = 0;
      const bool has_decimal_point = [&] {
         if constexpr (null_terminated) {
            return (*p == decimal_point);
         }
         else {
            return (p != pend) && (*p == decimal_point);
         }
      }();
      if (has_decimal_point) {
         ++p;
         UC const* before = p;
         // can occur at most twice without overflowing, but let it occur more, since
         // for integers with many digits, digit parsing is the primary bottleneck.
         loop_parse_if_eight_digits(p, pend, i);

         if constexpr (null_terminated) {
            while ((digit = glz::fast_float::digit_value(*p)) <= 9) {
               ++p;
               i = i * 10 + digit; // in rare cases, this will overflow, but that's ok
            }
         }
         else {
            while ((p != pend) && (digit = glz::fast_float::digit_value(*p)) <= 9) {
               ++p;
               i = i * 10 + digit; // in rare cases, this will overflow, but that's ok
            }
         }
         exponent = before - p;
         answer.fraction = glz::fast_float::span<const UC>(before, size_t(p - before));
         digit_count -= exponent;
      }
      // at least 1 digit in fractional part
      if (has_decimal_point && exponent == 0) {
         return answer;
      }

      int64_t exp_number = 0; // explicit exponential part

      if constexpr (null_terminated) {
         if ((UC('e') == *p) || (UC('E') == *p)) {
            UC const* location_of_e = p;
            ++p;
            bool neg_exp = false;
            if (UC('-') == *p) {
               neg_exp = true;
               ++p;
            }
            else if (UC('+') == *p) { // '+' on exponent is allowed by C++17 20.19.3.(7.1)
               ++p;
            }
            if ((digit = glz::fast_float::digit_value(*p)) > 9) {
               // Otherwise, we will be ignoring the 'e'.
               p = location_of_e;
            }
            else {
               do {
                  if (exp_number < 0x10000000) {
                     exp_number = 10 * exp_number + digit;
                  }
                  ++p;
               } while ((digit = glz::fast_float::digit_value(*p)) <= 9);
               if (neg_exp) {
                  exp_number = -exp_number;
               }
               exponent += exp_number;
            }
         }
      }
      else {
         if ((p != pend) && ((UC('e') == *p) || (UC('E') == *p))) {
            UC const* location_of_e = p;
            ++p;
            bool neg_exp = false;
            if ((p != pend) && (UC('-') == *p)) {
               neg_exp = true;
               ++p;
            }
            else if ((p != pend) && (UC('+') == *p)) { // '+' on exponent is allowed by C++17 20.19.3.(7.1)
               ++p;
            }
            if ((p == pend) || (digit = glz::fast_float::digit_value(*p)) > 9) {
               // Otherwise, we will be ignoring the 'e'.
               p = location_of_e;
            }
            else {
               do {
                  if (exp_number < 0x10000000) {
                     exp_number = 10 * exp_number + digit;
                  }
                  ++p;
               } while ((p != pend) && (digit = glz::fast_float::digit_value(*p)) <= 9);
               if (neg_exp) {
                  exp_number = -exp_number;
               }
               exponent += exp_number;
            }
         }
      }

      answer.lastmatch = p;
      answer.valid = true;

      // If we frequently had to deal with long strings of digits,
      // we could extend our code by using a 128-bit integer instead
      // of a 64-bit integer. However, this is uncommon.
      //
      // We can deal with up to 19 digits.
      if (digit_count > 19) { // this is uncommon
         // It is possible that the integer had an overflow.
         // We have to handle the case where we have 0.0000somenumber.
         // We need to be mindful of the case where we only have zeroes...
         // E.g., 0.000000000...000.
         UC const* start = start_digits;
         if constexpr (null_terminated) {
            while ((*start == UC('0') || *start == decimal_point)) {
               if (*start == UC('0')) {
                  --digit_count;
               }
               ++start;
            }
         }
         else {
            while ((start != pend) && (*start == UC('0') || *start == decimal_point)) {
               if (*start == UC('0')) {
                  --digit_count;
               }
               ++start;
            }
         }

         if (digit_count > 19) {
            answer.too_many_digits = true;
            // Let us start again, this time, avoiding overflows.
            // We don't need to check if is_integer, since we use the
            // pre-tokenized spans from above.
            i = 0;
            p = answer.integer.ptr;
            UC const* int_end = p + answer.integer.len();
            const uint64_t minimal_nineteen_digit_integer{1000000000000000000};
            while ((i < minimal_nineteen_digit_integer) && (p != int_end)) {
               i = i * 10 + uint64_t(*p - UC('0'));
               ++p;
            }
            if (i >= minimal_nineteen_digit_integer) { // We have a big integers
               exponent = end_of_integer_part - p + exp_number;
            }
            else { // We have a value with a fractional component.
               p = answer.fraction.ptr;
               UC const* frac_end = p + answer.fraction.len();
               while ((i < minimal_nineteen_digit_integer) && (p != frac_end)) {
                  i = i * 10 + uint64_t(*p - UC('0'));
                  ++p;
               }
               exponent = answer.fraction.ptr - p + exp_number;
            }
            // We have now corrected both exponent and i, to a truncated value
         }
      }
      answer.exponent = exponent;
      answer.mantissa = i;
      return answer;
   }

   template <bool null_terminated, class T, class UC>
   constexpr glz::fast_float::from_chars_result_t<UC> from_chars(UC const* first, UC const* last, T& value) noexcept
   {
      using namespace glz::fast_float;
      static_assert(is_supported_float_type<T>(), "only some floating-point types are supported");
      static_assert(is_supported_char_type<UC>(), "only char, wchar_t, char16_t and char32_t are supported");

      parsed_number_string_t<UC> pns = glz::parse_number_string<null_terminated, UC>(first, last);
      if (!pns.valid) [[unlikely]] {
         return {.ptr = first, .ec = std::errc::invalid_argument};
      }

      return from_chars_advanced(pns, value);
   }
}
