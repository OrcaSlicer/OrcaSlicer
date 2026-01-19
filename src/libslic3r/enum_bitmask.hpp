#ifndef slic3r_enum_bitmask_hpp_
#define slic3r_enum_bitmask_hpp_

// enum_bitmask for passing a set of attributes to a function in a type safe way.
// Adapted from https://gpfault.net/posts/typesafe-bitmasks.txt.html
// with hints from https://www.strikerx3.dev/cpp/2019/02/27/typesafe-enum-class-bitmasks-in-cpp.html

#include <type_traits>

namespace Slic3r {

// enum_bitmasks can only be used with enums.
template<class option_type, typename = typename std::enable_if<std::is_enum<option_type>::value>::type>
class enum_bitmask {
    // The type we'll use for storing the value of our bitmask should be the same as the enum's underlying type.
    using underlying_type = typename std::underlying_type<option_type>::type;

    // This method helps us avoid having to explicitly set enum values to powers of two.
    static constexpr underlying_type mask_value(option_type o) { return 1 << static_cast<underlying_type>(o); }

    // Private ctor to be used internally.
    explicit constexpr enum_bitmask(underlying_type o) : m_bits(o) {}

public:
    // Default ctor creates a bitmask with no options selected.
    constexpr enum_bitmask() : m_bits(0) {}

    // Creates a enum_bitmask with just one bit set.
    // This ctor is intentionally non-explicit, to allow passing an options to a function:
    // FunctionExpectingBitmask(Options::Opt1)
    constexpr enum_bitmask(option_type o) : m_bits(mask_value(o)) {}

    // Set the bit corresponding to the given option.
    constexpr enum_bitmask operator|(option_type t) const { return enum_bitmask(m_bits | mask_value(t)); }

    // Combine with another enum_bitmask of the same type.
    constexpr enum_bitmask operator|(enum_bitmask<option_type> t) const { return enum_bitmask(m_bits | t.m_bits); }
    
    // Set the bit corresponding to the given option.
    constexpr void operator|=(option_type t) { m_bits = enum_bitmask(m_bits | mask_value(t)); }

    // Combine with another enum_bitmask of the same type.
    constexpr void operator|=(enum_bitmask<option_type> t) { m_bits = enum_bitmask(m_bits | t.m_bits); }

    // Get the value of the bit corresponding to the given option.
    constexpr bool operator&(option_type t) const { return m_bits & mask_value(t); }
    constexpr bool has(option_type t) const { return m_bits & mask_value(t); }
    
    constexpr bool operator==(const enum_bitmask r) const { return m_bits == r.m_bits; }
    constexpr bool operator!=(const enum_bitmask r) const { return m_bits != r.m_bits; }
    // For sorting by the enum values.
    constexpr bool lower(const enum_bitmask r) const { return m_bits < r.m_bits; }

private:
    underlying_type m_bits = 0;
};

// For enabling free functions producing enum_bitmask<> type from bit operations on enums.
template<typename Enum> struct is_enum_bitmask_type { static const bool enable = false; };
#define ENABLE_ENUM_BITMASK_OPERATORS(x) template<> struct is_enum_bitmask_type<x> { static const bool enable = true; };
template<class Enum> inline constexpr bool is_enum_bitmask_type_v = is_enum_bitmask_type<Enum>::enable;

// Creates an enum_bitmask from two options, convenient for passing of options to a function:
// FunctionExpectingBitmask(Options::Opt1 | Options::Opt2 | Options::Opt3)
template <class option_type>
constexpr std::enable_if_t<is_enum_bitmask_type_v<option_type>, enum_bitmask<option_type>> operator|(option_type lhs, option_type rhs) {
    static_assert(std::is_enum_v<option_type>);
    return enum_bitmask<option_type>{lhs} | rhs;
}

template <class option_type>
constexpr std::enable_if_t<is_enum_bitmask_type_v<option_type>, enum_bitmask<option_type>> operator|(option_type lhs, enum_bitmask<option_type> rhs) {
    static_assert(std::is_enum_v<option_type>);
    return enum_bitmask<option_type>{lhs} | rhs;
}

template <class option_type>
constexpr std::enable_if_t<is_enum_bitmask_type_v<option_type>, enum_bitmask<option_type>> only_if(bool condition, option_type opt) {
    static_assert(std::is_enum_v<option_type>);
    return condition ? enum_bitmask<option_type>{opt} : enum_bitmask<option_type>{};
}

template <class option_type>
constexpr std::enable_if_t<is_enum_bitmask_type_v<option_type>, enum_bitmask<option_type>> only_if(bool condition, enum_bitmask<option_type> opt) {
    static_assert(std::is_enum_v<option_type>);
    return condition ? opt : enum_bitmask<option_type>{};
}

} // namespace Slic3r

#endif // slic3r_enum_bitmask_hpp_
