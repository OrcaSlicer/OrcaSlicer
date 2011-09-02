#include "LocalesUtils.hpp"

#ifdef _WIN32
    #include <charconv>
#endif
#include <stdexcept>

#include <fast_float/fast_float.h>


namespace Slic3r {


CNumericLocalesSetter::CNumericLocalesSetter()
{
#ifdef _WIN32
    _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
    m_orig_numeric_locale = std::setlocale(LC_NUMERIC, nullptr);
    std::setlocale(LC_NUMERIC, "C");
#elif __APPLE__
    m_original_locale = uselocale((locale_t)0);
    m_new_locale = newlocale(LC_NUMERIC_MASK, "C", m_original_locale);
    uselocale(m_new_locale);
#else // linux / BSD
    m_original_locale = uselocale((locale_t)0);
    m_new_locale = duplocale(m_original_locale);
    m_new_locale = newlocale(LC_NUMERIC_MASK, "C", m_new_locale);
    uselocale(m_new_locale);
#endif
}



CNumericLocalesSetter::~CNumericLocalesSetter()
{
#ifdef _WIN32
    std::setlocale(LC_NUMERIC, m_orig_numeric_locale.data());
#else
    uselocale(m_original_locale);
    freelocale(m_new_locale);
#endif
}



bool is_decimal_separator_point()
{
    char str[5] = "";
    sprintf(str, "%.1f", 0.5f);
    return str[1] == '.';
}


double string_to_double_decimal_point(const std::string_view str, size_t* pos /* = nullptr*/)
{
    double out;
    size_t p = fast_float::from_chars(str.data(), str.data() + str.size(), out).ptr - str.data();
    if (pos)
        *pos = p;
    return out;
}

std::string float_to_string_decimal_point(double value, int precision/* = -1*/)
{
    // Our Windows build server fully supports C++17 std::to_chars. Let's use it.
    // Other platforms are behind, fall back to slow stringstreams for now.
#ifdef _WIN32
    constexpr size_t SIZE = 20;
    char out[SIZE] = "";
    std::to_chars_result res;
    if (precision >=0)
        res = std::to_chars(out, out+SIZE, value, std::chars_format::fixed, precision);
    else
        res = std::to_chars(out, out+SIZE, value, std::chars_format::general, 6);
    if (res.ec == std::errc::value_too_large)
        throw std::invalid_argument("float_to_string_decimal_point conversion failed.");
    return std::string(out, res.ptr - out);
#else
    std::stringstream buf;
    if (precision >= 0)
        buf << std::fixed << std::setprecision(precision);
    buf << value;
    return buf.str();
#endif
}


} // namespace Slic3r

