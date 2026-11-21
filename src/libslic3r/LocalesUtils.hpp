#ifndef slic3r_LocalesUtils_hpp_
#define slic3r_LocalesUtils_hpp_

#include <cstddef>
#include <string>
#include <clocale>
#include <iomanip>
#include <cassert>
#include <string_view>

#ifdef __APPLE__
#include <xlocale.h>
#endif

namespace Slic3r {

// RAII wrapper that sets LC_NUMERIC to "C" on construction
// and restores the old value on destruction.
class CNumericLocalesSetter {
public:
    CNumericLocalesSetter();
    ~CNumericLocalesSetter();
    // A copy would restore the locale twice, and count down once more than up.
    CNumericLocalesSetter(const CNumericLocalesSetter&) = delete;
    CNumericLocalesSetter& operator=(const CNumericLocalesSetter&) = delete;

private:
    // Inside another setter on this thread, which does the setting and restoring.
    bool m_nested { false };
#ifdef _WIN32
    std::string m_orig_numeric_locale;
#else
    locale_t m_original_locale;
    locale_t m_new_locale;
#endif

};

// A function to check that current C locale uses decimal point as a separator.
// Intended mostly for asserts.
bool is_decimal_separator_point();


// A substitute for std::to_string that works according to
// C++ locales, not C locale. Meant to be used when we need
// to be sure that decimal point is used as a separator.
// (We use user C locales and "C" C++ locales in most of the code.)
std::string float_to_string_decimal_point(double value, int precision = -1);
// With shortest=true and no fixed precision, preserve the double using its shortest round-trip spelling.
std::string float_to_string_decimal_point(double value, int precision, bool shortest);
// The shortest spelling of a finite value built on a stream, where the floating-point std::to_chars is missing.
// Prints what the general format of std::to_chars prints: the fewest digits that read back as the same double,
// in fixed notation for decimal exponents from -4 to 5 and in scientific notation otherwise.
std::string shortest_float_to_string_decimal_point(double value);
//std::string float_to_string_decimal_point(float value,  int precision = -1);
double string_to_double_decimal_point(const std::string_view str, size_t* pos = nullptr);
// Parse a complete finite number, including an optional leading '+', without a locale lookup.
bool try_parse_double_decimal_point(std::string_view str, double& value);
// Parses like atof in the C locale, skipping leading whitespace and a '+',
// without the C runtime's per-call locale lookup.
double atof_decimal_point(std::string_view str);

} // namespace Slic3r

#endif // slic3r_LocalesUtils_hpp_
