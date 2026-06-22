#pragma once

// orca/Config.hpp — adhoc-receiver helpers for the typed-config API.
//
// Phase 0.4d wave-1 catalog showed ~50% of GUI typed-config call sites are
// Adhoc: receiver is a local `DynamicPrintConfig` variable or function
// parameter, not derived from a known preset Scope. Those sites can't use
// `presets().get<K>(Scope)` because there's no Scope to pass.
//
// For those, this header provides receiver-passing free functions:
//   double v  = orca::config::get<orca::keys::layer_height>(cfg).value_or(0.2);
//   auto vec  = orca::config::get_vec<orca::keys::nozzle_diameter>(cfg);
//   orca::config::put<orca::keys::layer_height>(cfg, 0.3);
//
// The key tag carries the C++ primitive type, vector-ness, and the libslic3r
// CoType, so the compiler picks the right ConfigOption subclass via
// `if constexpr`. Wrong-type uses at the call site are a compile error rather
// than a runtime null deref.

#include "ConfigKeys.hpp"
#include "Presets.hpp"  // for ConfigScope + Presets::get/put declarations
#include "Result.hpp"

#include "libslic3r/PrintConfig.hpp"

#include <optional>
#include <string>
#include <vector>

namespace orca::config {

namespace detail {

// Map an orca::CoType to the libslic3r ConfigOption subclass that holds the
// value. Used by the if-constexpr dispatch in the templates below.
template <CoType C> struct co_traits;

template <> struct co_traits<CoType::Float>            { using opt = Slic3r::ConfigOptionFloat; };
template <> struct co_traits<CoType::Floats>           { using opt = Slic3r::ConfigOptionFloats; };
template <> struct co_traits<CoType::FloatsNullable>   { using opt = Slic3r::ConfigOptionFloatsNullable; };
template <> struct co_traits<CoType::Int>              { using opt = Slic3r::ConfigOptionInt; };
template <> struct co_traits<CoType::Ints>             { using opt = Slic3r::ConfigOptionInts; };
template <> struct co_traits<CoType::IntsNullable>     { using opt = Slic3r::ConfigOptionIntsNullable; };
template <> struct co_traits<CoType::String>           { using opt = Slic3r::ConfigOptionString; };
template <> struct co_traits<CoType::Strings>          { using opt = Slic3r::ConfigOptionStrings; };
template <> struct co_traits<CoType::Bool>             { using opt = Slic3r::ConfigOptionBool; };
template <> struct co_traits<CoType::Bools>            { using opt = Slic3r::ConfigOptionBools; };
template <> struct co_traits<CoType::BoolsNullable>    { using opt = Slic3r::ConfigOptionBoolsNullable; };
template <> struct co_traits<CoType::Percent>          { using opt = Slic3r::ConfigOptionPercent; };
template <> struct co_traits<CoType::Percents>         { using opt = Slic3r::ConfigOptionPercents; };
template <> struct co_traits<CoType::FloatOrPercent>   { using opt = Slic3r::ConfigOptionFloatOrPercent; };
template <> struct co_traits<CoType::FloatsOrPercents> { using opt = Slic3r::ConfigOptionFloatsOrPercents; };
template <> struct co_traits<CoType::Point>            { using opt = Slic3r::ConfigOptionPoint; };
template <> struct co_traits<CoType::Points>           { using opt = Slic3r::ConfigOptionPoints; };
template <> struct co_traits<CoType::PointsGroups>     { using opt = Slic3r::ConfigOptionPointsGroups; };
template <> struct co_traits<CoType::Enum>             { using opt = Slic3r::ConfigOptionInt; }; // see note below
template <> struct co_traits<CoType::Enums>            { using opt = Slic3r::ConfigOptionInts; };

// Note on Enum/Enums: the underlying libslic3r option type is templated
// (ConfigOptionEnum<E>) and can't be resolved without knowing E. For the
// untyped path we project to ConfigOptionInt (which the underlying type
// inherits from) and treat the value as int — callers that need the typed
// enum E should use the typed enum helpers below (get_enum<keys::K, E>).

} // namespace detail

// Scalar read. Returns std::nullopt if the key isn't present in cfg or has
// a different type than the key tag declares.
//
// Accepts `const ConfigBase&` so the receiver can be a DynamicPrintConfig,
// a plain DynamicConfig, or a StaticPrintConfig — anything inheriting
// libslic3r's ConfigBase. Writes (put/put_vec/put_at) still require
// DynamicConfig since static configs can't create options at runtime.
template <class K>
auto get(const Slic3r::ConfigBase& cfg) -> std::optional<typename K::type> {
    using T  = typename K::type;
    using CO = typename detail::co_traits<K::co_type>::opt;
    static_assert(!K::is_vector,
                  "orca::config::get<K> is for scalar keys; use get_vec<K> for vector keys");

    const Slic3r::ConfigOption* base = cfg.option(std::string{K::name});
    if (!base) return std::nullopt;
    const CO* typed = dynamic_cast<const CO*>(base);
    if (!typed) return std::nullopt;

    if constexpr (std::is_same_v<T, bool>) {
        return static_cast<bool>(typed->value);
    } else if constexpr (K::co_type == CoType::FloatOrPercent) {
        return T{typed->value, typed->percent};
    } else if constexpr (K::co_type == CoType::Point) {
        return typed->value;
    } else if constexpr (K::co_type == CoType::Enum) {
        return static_cast<T>(typed->value);
    } else {
        return typed->value;
    }
}

// Vector read. Returns the full vector by value (copy) — same semantics as
// `cfg.option<ConfigOptionFloats>("k")->values` but typed.
template <class K>
auto get_vec(const Slic3r::ConfigBase& cfg) -> std::optional<std::vector<typename K::type>> {
    using T  = typename K::type;
    using CO = typename detail::co_traits<K::co_type>::opt;
    static_assert(K::is_vector,
                  "orca::config::get_vec<K> is for vector keys; use get<K> for scalar keys");

    const Slic3r::ConfigOption* base = cfg.option(std::string{K::name});
    if (!base) return std::nullopt;
    const CO* typed = dynamic_cast<const CO*>(base);
    if (!typed) return std::nullopt;

    if constexpr (std::is_same_v<T, unsigned char>) {
        // Bools/BoolsNullable store as vector<unsigned char>; pass through.
        return typed->values;
    } else {
        return typed->values;
    }
}

// Vector element read.
template <class K>
auto get_at(const Slic3r::ConfigBase& cfg, std::size_t idx)
    -> std::optional<typename K::type>
{
    auto vec = get_vec<K>(cfg);
    if (!vec || idx >= vec->size()) return std::nullopt;
    return (*vec)[idx];
}

// Scalar write. Auto-creates the option if missing.
template <class K>
void put(Slic3r::DynamicPrintConfig& cfg, typename K::type value) {
    using CO = typename detail::co_traits<K::co_type>::opt;
    static_assert(!K::is_vector,
                  "orca::config::put<K> is for scalar keys; use put_vec<K> for vector keys");

    auto* opt = cfg.option<CO>(std::string{K::name}, /*create=*/true);
    if constexpr (std::is_same_v<typename K::type, bool>) {
        opt->value = value ? 1 : 0;
    } else if constexpr (K::co_type == CoType::Enum) {
        opt->value = static_cast<int>(value);
    } else {
        opt->value = std::move(value);
    }
}

// Vector write.
template <class K>
void put_vec(Slic3r::DynamicPrintConfig& cfg, std::vector<typename K::type> values) {
    using CO = typename detail::co_traits<K::co_type>::opt;
    static_assert(K::is_vector,
                  "orca::config::put_vec<K> is for vector keys; use put<K> for scalar keys");

    auto* opt = cfg.option<CO>(std::string{K::name}, /*create=*/true);
    opt->values = std::move(values);
}

// Vector element write.
template <class K>
void put_at(Slic3r::DynamicPrintConfig& cfg, std::size_t idx, typename K::type value) {
    using CO = typename detail::co_traits<K::co_type>::opt;
    static_assert(K::is_vector,
                  "orca::config::put_at<K> is for vector keys");

    auto* opt = cfg.option<CO>(std::string{K::name}, /*create=*/true);
    if (idx >= opt->values.size()) opt->values.resize(idx + 1);
    opt->values[idx] = std::move(value);
}

// ---------- Typed enum surface ----------
//
// ConfigKeys.hpp can't know the C++ enum type E for a coEnum key (it's
// generated from ConfigDef, which only records coEnum/coEnums). So the caller
// names the enum: `get_enum<keys::curr_bed_type, BedType>(cfg)`.
//
// Reads go through ConfigOption::getInt() — a virtual that works for BOTH
// ConfigOptionEnum<E> and ConfigOptionEnumGeneric (the latter is what gets
// stored when a key is registered with coEnum but no compile-time E). So a
// single int-based read handles every enum storage shape.

// Scalar enum read.
template <class K, class E>
auto get_enum(const Slic3r::ConfigBase& cfg) -> std::optional<E> {
    static_assert(K::co_type == CoType::Enum,
                  "orca::config::get_enum<K,E> is for scalar enum keys (coEnum)");
    const Slic3r::ConfigOption* base = cfg.option(std::string{K::name});
    if (!base) return std::nullopt;
    return static_cast<E>(base->getInt());
}

// Vector enum read (coEnums — e.g. ConfigOptionEnumsGeneric). Reads the int
// vector via the ConfigOptionInts base, casts each element to E.
template <class K, class E>
auto get_enums(const Slic3r::ConfigBase& cfg) -> std::optional<std::vector<E>> {
    static_assert(K::co_type == CoType::Enums,
                  "orca::config::get_enums<K,E> is for vector enum keys (coEnums)");
    auto ints = get_vec<K>(cfg);
    if (!ints) return std::nullopt;
    std::vector<E> out;
    out.reserve(ints->size());
    for (int v : *ints) out.push_back(static_cast<E>(v));
    return out;
}

// Vector enum element read.
template <class K, class E>
auto get_enum_at(const Slic3r::ConfigBase& cfg, std::size_t idx) -> std::optional<E> {
    static_assert(K::co_type == CoType::Enums,
                  "orca::config::get_enum_at<K,E> is for vector enum keys (coEnums)");
    auto v = get_at<K>(cfg, idx);
    if (!v) return std::nullopt;
    return static_cast<E>(*v);
}

// Scalar enum write. Stores a ConfigOptionEnum<E>; setInt is used so the value
// round-trips whether the stored option is ConfigOptionEnum<E> or generic.
template <class K, class E>
void put_enum(Slic3r::DynamicConfig& cfg, E value) {
    static_assert(K::co_type == CoType::Enum,
                  "orca::config::put_enum<K,E> is for scalar enum keys (coEnum)");
    auto* opt = cfg.option<Slic3r::ConfigOptionEnum<E>>(std::string{K::name}, /*create=*/true);
    opt->value = value;
}

} // namespace orca::config

// ---------- Scoped Presets template definitions ----------
//
// These live HERE (not in Presets.hpp) because they delegate to the
// orca::config:: free functions above. Putting them in Presets.hpp would
// require including Config.hpp from there, which would close a circular
// include loop (ConfigKeys.hpp → Presets.hpp). Consumers that want the
// typed surface include orca/Config.hpp; they get both Adhoc helpers and
// scoped Presets methods in one pull.

namespace orca {

template <class K>
auto Presets::get(ConfigScope scope) const -> std::optional<typename K::type> {
    const auto* cfg = config_for_scope(scope);
    if (!cfg) return std::nullopt;
    return ::orca::config::get<K>(*cfg);
}

template <class K>
auto Presets::get_at(ConfigScope scope, std::size_t idx) const -> std::optional<typename K::type> {
    const auto* cfg = config_for_scope(scope);
    if (!cfg) return std::nullopt;
    return ::orca::config::get_at<K>(*cfg, idx);
}

template <class K>
auto Presets::get_vec(ConfigScope scope) const -> std::optional<std::vector<typename K::type>> {
    const auto* cfg = config_for_scope(scope);
    if (!cfg) return std::nullopt;
    return ::orca::config::get_vec<K>(*cfg);
}

template <class K>
Result<void> Presets::put(ConfigScope scope, typename K::type value) {
    auto* cfg = config_for_scope(scope);
    if (!cfg) return err<void>(ErrorCode::InvalidState, "Presets::put — scope not resolvable");
    ::orca::config::put<K>(*cfg, std::move(value));
    return ok();
}

} // namespace orca
