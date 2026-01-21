// Glaze Library
// For the license information refer to glaze.hpp
#pragma once

// We include naming metas for standard types for consistency across compilers
#include "glaze/api/std/array.hpp"
#include "glaze/api/std/deque.hpp"
#include "glaze/api/std/functional.hpp"
#include "glaze/api/std/list.hpp"
#include "glaze/api/std/map.hpp"
#include "glaze/api/std/optional.hpp"
#include "glaze/api/std/shared_ptr.hpp"
#include "glaze/api/std/string.hpp"
#include "glaze/api/std/tuple.hpp"
#include "glaze/api/std/unique_ptr.hpp"
#include "glaze/api/std/unordered_map.hpp"
#include "glaze/api/std/variant.hpp"
#include "glaze/api/std/vector.hpp"
#include "glaze/api/tuplet.hpp"
#include "glaze/api/type_support.hpp"
#include "glaze/core/custom_meta.hpp"
#include "glaze/json/wrappers.hpp"
#include "glaze/json/write.hpp"

namespace glz
{
   namespace detail
   {
      enum struct defined_formats : uint32_t;
      struct ExtUnits final
      {
         std::optional<std::string_view> unitAscii{}; // ascii representation of the unit, e.g. "m^2" for square meters
         std::optional<std::string_view>
            unitUnicode{}; // unicode representation of the unit, e.g. "m²" for square meters
         constexpr bool operator==(const ExtUnits&) const noexcept = default;
      };
   }
   struct schema final
   {
      bool reflection_helper{}; // needed to support automatic reflection, because ref is a std::optional
      std::optional<std::string_view> ref{};
      using schema_number = std::optional<std::variant<int64_t, uint64_t, double>>;
      using schema_any = std::variant<std::monostate, bool, int64_t, uint64_t, double, std::string_view>;
      // meta data keywords, ref: https://www.learnjsonschema.com/2020-12/meta-data/
      std::optional<std::string_view> title{};
      std::optional<std::string_view> description{};
      std::optional<schema_any> defaultValue{};
      std::optional<bool> deprecated{};
      std::optional<std::vector<std::string_view>> examples{};
      std::optional<bool> readOnly{};
      std::optional<bool> writeOnly{};
      // hereafter validation keywords, ref: https://www.learnjsonschema.com/2020-12/validation/
      std::optional<schema_any> constant{};
      // string only keywords
      std::optional<uint64_t> minLength{};
      std::optional<uint64_t> maxLength{};
      std::optional<std::string_view> pattern{};
      // https://www.learnjsonschema.com/2020-12/format-annotation/format/
      std::optional<detail::defined_formats> format{};
      // number only keywords
      schema_number minimum{};
      schema_number maximum{};
      schema_number exclusiveMinimum{};
      schema_number exclusiveMaximum{};
      schema_number multipleOf{};
      // object only keywords
      std::optional<uint64_t> minProperties{};
      std::optional<uint64_t> maxProperties{};
      // std::optional<std::map<std::string_view, std::vector<std::string_view>>> dependent_required{};
      std::optional<std::vector<std::string_view>> required{};
      // array only keywords
      std::optional<uint64_t> minItems{};
      std::optional<uint64_t> maxItems{};
      std::optional<uint64_t> minContains{};
      std::optional<uint64_t> maxContains{};
      std::optional<bool> uniqueItems{};
      // properties
      std::optional<std::vector<std::string_view>> enumeration{}; // enum

      // out of json schema specification
      std::optional<detail::ExtUnits> ExtUnits{};
      std::optional<bool>
         ExtAdvanced{}; // flag to indicate that the parameter is advanced and can be hidden in default views

      static constexpr auto schema_attributes{true}; // allowance flag to indicate metadata within glz::object(...)

      // TODO switch to using variants when we have write support to get rid of nulls
      // TODO We should be able to generate the json schema compiletime
      struct glaze
      {
         using T = schema;
         static constexpr std::array keys{"$ref", //
                                          "title", //
                                          "description", //
                                          "default", //
                                          "deprecated", //
                                          "examples", //
                                          "readOnly", //
                                          "writeOnly", //
                                          "const", //
                                          "minLength", //
                                          "maxLength", //
                                          "pattern", //
                                          "format", //
                                          "minimum", //
                                          "maximum", //
                                          "exclusiveMinimum", //
                                          "exclusiveMaximum", //
                                          "multipleOf", //
                                          "minProperties", //
                                          "maxProperties", //
                                          //"dependentRequired", //
                                          "required", //
                                          "minItems", //
                                          "maxItems", //
                                          "minContains", //
                                          "maxContains", //
                                          "uniqueItems", //
                                          "enum", //
                                          "ExtUnits", //
                                          "ExtAdvanced"};

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-braces"
#endif
         static constexpr glz::tuple value = {&T::ref, //
                                              &T::title, //
                                              &T::description, //
                                              &T::defaultValue, //
                                              &T::deprecated, //
                                              unquoted<&T::examples>, //
                                              &T::readOnly, //
                                              &T::writeOnly, //
                                              &T::constant, //
                                              &T::minLength, //
                                              &T::maxLength, //
                                              &T::pattern, //
                                              &T::format, //
                                              &T::minimum, //
                                              &T::maximum, //
                                              &T::exclusiveMinimum, //
                                              &T::exclusiveMaximum, //
                                              &T::multipleOf, //
                                              &T::minProperties, //
                                              &T::maxProperties, //
                                              // &T::dependent_required, //
                                              &T::required, //
                                              &T::minItems, //
                                              &T::maxItems, //
                                              &T::minContains, //
                                              &T::maxContains, //
                                              &T::uniqueItems, //
                                              &T::enumeration, //
                                              &T::ExtUnits, //
                                              &T::ExtAdvanced};
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
      };
   };

   namespace detail
   {
      struct schematic final
      {
         std::optional<std::vector<std::string_view>> type{};
         std::optional<std::map<std::string_view, schema, std::less<>>> properties{}; // glaze_object
         std::optional<schema> items{}; // array
         std::optional<std::variant<bool, schema>> additionalProperties{}; // map
         std::optional<std::map<std::string_view, schematic, std::less<>>> defs{};
         std::optional<std::vector<schematic>> oneOf{};
         std::optional<std::vector<std::string_view>> required{};
         std::optional<std::vector<std::string_view>> examples{};
         schema attributes{};
      };

      enum struct defined_formats : uint32_t {
         datetime, //
         date, //
         time, //
         duration, //
         email, //
         idn_email, //
         hostname, //
         idn_hostname, //
         ipv4, //
         ipv6, //
         uri, //
         uri_reference, //
         iri, //
         iri_reference, //
         uuid, //
         uri_template, //
         json_pointer, //
         relative_json_pointer, //
         regex
      };
   }
}

template <>
struct glz::meta<glz::detail::defined_formats>
{
   static constexpr std::string_view name = "defined_formats";
   using enum glz::detail::defined_formats;
   static constexpr std::array keys{
      "date-time", "date",          "time", "duration",     "email",        "idn-email",
      "hostname",  "idn-hostname",  "ipv4", "ipv6",         "uri",          "uri-reference",
      "iri",       "iri-reference", "uuid", "uri-template", "json-pointer", "relative-json-pointer",
      "regex"};
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-braces"
#endif
   static constexpr std::array value{datetime, //
                                     date, //
                                     time, //
                                     duration, //
                                     email, //
                                     idn_email, //
                                     hostname, //
                                     idn_hostname, //
                                     ipv4, //
                                     ipv6, //
                                     uri, //
                                     uri_reference, //
                                     iri, //
                                     iri_reference, //
                                     uuid, //
                                     uri_template, //
                                     json_pointer, //
                                     relative_json_pointer, //
                                     regex};
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
};

template <>
struct glz::meta<glz::detail::schematic>
{
   static constexpr std::string_view name = "glz::detail::schema";
   using T = detail::schematic;
   static constexpr std::array keys{"type", //
                                    "properties", //
                                    "items", //
                                    "additionalProperties", //
                                    "$defs", //
                                    "oneOf", //
                                    "examples", //
                                    "required", //
                                    "title", //
                                    "description", //
                                    "default", //
                                    "deprecated", //
                                    "readOnly", //
                                    "writeOnly", //
                                    "const", //
                                    "minLength", //
                                    "maxLength", //
                                    "pattern", //
                                    "format", //
                                    "minimum", //
                                    "maximum", //
                                    "exclusiveMinimum", //
                                    "exclusiveMaximum", //
                                    "multipleOf", //
                                    "minProperties", //
                                    "maxProperties", //
                                    // "dependentRequired", //
                                    "minItems", //
                                    "maxItems", //
                                    "minContains", //
                                    "maxContains", //
                                    "uniqueItems", //
                                    "enum", //
                                    "ExtUnits", //
                                    "ExtAdvanced"};

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-braces"
#endif
   [[maybe_unused]] static constexpr glz::tuple value{
      &T::type, //
      &T::properties, //
      &T::items, //
      &T::additionalProperties, //
      &T::defs, //
      &T::oneOf, //
      unquoted<&T::examples>, //
      &T::required, //
      [](auto&& s) -> auto& { return s.attributes.title; }, //
      [](auto&& s) -> auto& { return s.attributes.description; }, //
      [](auto&& s) -> auto& { return s.attributes.defaultValue; }, //
      [](auto&& s) -> auto& { return s.attributes.deprecated; }, //
      [](auto&& s) -> auto& { return s.attributes.readOnly; }, //
      [](auto&& s) -> auto& { return s.attributes.writeOnly; }, //
      [](auto&& s) -> auto& { return s.attributes.constant; }, //
      [](auto&& s) -> auto& { return s.attributes.minLength; }, //
      [](auto&& s) -> auto& { return s.attributes.maxLength; }, //
      [](auto&& s) -> auto& { return s.attributes.pattern; }, //
      [](auto&& s) -> auto& { return s.attributes.format; }, //
      [](auto&& s) -> auto& { return s.attributes.minimum; }, //
      [](auto&& s) -> auto& { return s.attributes.maximum; }, //
      [](auto&& s) -> auto& { return s.attributes.exclusiveMinimum; }, //
      [](auto&& s) -> auto& { return s.attributes.exclusiveMaximum; }, //
      [](auto&& s) -> auto& { return s.attributes.multipleOf; }, //
      [](auto&& s) -> auto& { return s.attributes.minProperties; }, //
      [](auto&& s) -> auto& { return s.attributes.maxProperties; }, //
      // [](auto&& s) -> auto& { return s.attributes.dependent_required; }, //
      [](auto&& s) -> auto& { return s.attributes.minItems; }, //
      [](auto&& s) -> auto& { return s.attributes.maxItems; }, //
      [](auto&& s) -> auto& { return s.attributes.minContains; }, //
      [](auto&& s) -> auto& { return s.attributes.maxContains; }, //
      [](auto&& s) -> auto& { return s.attributes.uniqueItems; }, //
      [](auto&& s) -> auto& { return s.attributes.enumeration; }, //
      [](auto&& s) -> auto& { return s.attributes.ExtUnits; }, //
      [](auto&& s) -> auto& { return s.attributes.ExtAdvanced; }};
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
};

namespace glz
{
   // Note: has_custom_meta_v and detail::custom_read_input_type are defined in common.hpp

   namespace detail
   {
      template <class T = void>
      struct to_json_schema
      {
         template <auto Opts>
         static void op(auto& s, auto& defs)
         {
            // &T::member
            if constexpr (glaze_t<T> && std::is_member_object_pointer_v<meta_wrapper_t<T>>) {
               using val_t = member_t<T, meta_wrapper_t<T>>;
               to_json_schema<val_t>::template op<Opts>(s, defs);
            }
            else if constexpr (glaze_const_value_t<T>) { // &T::constexpr_member
               using constexpr_val_t = member_t<T, meta_wrapper_t<T>>;
               static constexpr auto val_v{*glz::meta_wrapper_v<T>};
               if constexpr (glz::glaze_enum_t<constexpr_val_t>) {
                  s.attributes.constant = glz::enum_name_v<val_v>;
               }
               else {
                  // General case, needs to be convertible to schema_any
                  s.attributes.constant = val_v;
               }
               to_json_schema<constexpr_val_t>::template op<Opts>(s, defs);
            }
            else if constexpr (has_mimic<T>) {
               // Type with mimic declaration: use the mimic type's schema
               to_json_schema<mimic_type<T>>::template op<Opts>(s, defs);
            }
            else if constexpr (detail::custom_read_input_type<T>::has_custom) {
               // Type with custom read/write: infer schema from input type
               using InputType = typename detail::custom_read_input_type<T>::type;
               to_json_schema<InputType>::template op<Opts>(s, defs);
            }
            else {
               s.type = {"number", "string", "boolean", "object", "array", "null"};
            }
         }
      };

      template <class T>
         requires(std::same_as<T, bool> || std::same_as<T, std::vector<bool>::reference> ||
                  std::same_as<T, std::vector<bool>::const_reference>)
      struct to_json_schema<T>
      {
         template <auto Opts>
         static void op(auto& s, auto&)
         {
            s.type = {"boolean"};
         }
      };

      template <num_t T>
      struct to_json_schema<T>
      {
         template <auto Opts>
         static void op(auto& s, auto&)
         {
            using V = std::decay_t<T>;
            if constexpr (std::integral<V>) {
               s.type = {"integer"};
               s.attributes.minimum = static_cast<std::int64_t>(std::numeric_limits<V>::lowest());
               s.attributes.maximum = static_cast<std::uint64_t>((std::numeric_limits<V>::max)());
            }
            else {
               s.type = {"number"};
               s.attributes.minimum = std::numeric_limits<V>::lowest();
               s.attributes.maximum = (std::numeric_limits<V>::max)();
            }
         }
      };

      template <class T>
         requires str_t<T> || char_t<T>
      struct to_json_schema<T>
      {
         template <auto Opts>
         static void op(auto& s, auto&)
         {
            s.type = {"string"};
         }
      };

      template <always_null_t T>
      struct to_json_schema<T>
      {
         template <auto Opts>
         static void op(auto& s, auto&)
         {
            s.type = {"null"};
            s.attributes.constant = std::monostate{};
         }
      };

      template <glaze_enum_t T>
      struct to_json_schema<T>
      {
         template <auto Opts>
         static void op(auto& s, auto&)
         {
            s.type = {"string"};

            // TODO use oneOf instead of enum to handle doc comments
            static constexpr auto N = reflect<T>::size;
            // s.enumeration = std::vector<std::string_view>(N);
            // for_each<N>([&]<auto I>() {
            //    static constexpr auto item = std::get<I>(meta_v<V>);
            //    (*s.enumeration)[I] = std::get<0>(item);
            // });
            s.oneOf = std::vector<schematic>(N);
            for_each<N>([&]<auto I>() {
               auto& enumeration = (*s.oneOf)[I];
               // Do not override if already set
               if (!enumeration.attributes.constant.has_value()) {
                  enumeration.attributes.constant = reflect<T>::keys[I];
               }
               if (!enumeration.attributes.title.has_value()) {
                  enumeration.attributes.title = reflect<T>::keys[I];
               }
            });
         }
      };

      template <class T>
      struct to_json_schema<basic_raw_json<T>>
      {
         template <auto Opts>
         static void op(auto& s, auto&)
         {
            s.type = {"number", "string", "boolean", "object", "array", "null"};
         }
      };

      template <array_t T>
      struct to_json_schema<T>
      {
         template <auto Opts>
         static void op(auto& s, auto& defs)
         {
            using V = std::decay_t<range_value_t<std::decay_t<T>>>;
            s.type = {"array"};
            if constexpr (has_fixed_size_container<std::decay_t<T>>) {
               s.attributes.minItems = get_size<std::decay_t<T>>();
               s.attributes.maxItems = get_size<std::decay_t<T>>();
            }
            auto& def = defs[name_v<V>];
            if (!def.type) {
               to_json_schema<V>::template op<Opts>(def, defs);
            }
            s.items = schema{true, join_v<chars<"#/$defs/">, name_v<V>>};
         }
      };

      template <writable_map_t T>
      struct to_json_schema<T>
      {
         template <auto Opts>
         static void op(auto& s, auto& defs)
         {
            using V = std::decay_t<glz::tuple_element_t<1, range_value_t<std::decay_t<T>>>>;
            s.type = {"object"};
            auto& def = defs[name_v<V>];
            if (!def.type) {
               to_json_schema<V>::template op<Opts>(def, defs);
            }
            s.additionalProperties = schema{true, join_v<chars<"#/$defs/">, name_v<V>>};
         }
      };

      template <nullable_t T>
      struct to_json_schema<T>
      {
         template <auto Opts>
         static void op(auto& s, auto& defs)
         {
            using V = std::decay_t<decltype(*std::declval<std::decay_t<T>>())>;
            to_json_schema<V>::template op<Opts>(s, defs);
            // to_json_schema above should populate the correct type, let's throw if it wasn't set
            auto& type = s.type.value();
            auto it = std::find_if(type.begin(), type.end(), [&](const auto& str) { return str == "null"; });
            if (it == type.end()) {
               type.emplace_back("null");
            }
         }
      };

      template <is_variant T>
      struct to_json_schema<T>
      {
         template <auto Opts>
         static void op(auto& s, auto& defs)
         {
            static constexpr auto N = std::variant_size_v<T>;
            using type_counts = variant_type_count<T>;
            s.type = std::vector<sv>{};
            if constexpr (type_counts::n_number) {
               (*s.type).emplace_back("number");
            }
            if constexpr (type_counts::n_string) {
               (*s.type).emplace_back("string");
            }
            if constexpr (type_counts::n_bool) {
               (*s.type).emplace_back("boolean");
            }
            if constexpr (type_counts::n_object) {
               (*s.type).emplace_back("object");
            }
            if constexpr (type_counts::n_array) {
               (*s.type).emplace_back("array");
            }
            if constexpr (type_counts::n_null) {
               (*s.type).emplace_back("null");
            }
            s.oneOf = std::vector<schematic>(N);

            const auto& ids = ids_v<T>;

            for_each<N>([&]<auto I>() {
               using V = std::decay_t<std::variant_alternative_t<I, T>>;
               auto& schema_val = (*s.oneOf)[I];
               to_json_schema<V>::template op<Opts>(schema_val, defs);

               if (not schema_val.attributes.title) {
                  schema_val.attributes.title = ids[I];
               }

               if constexpr ((glaze_object_t<V> || reflectable<V>) && not tag_v<T>.empty()) {
                  if (not schema_val.required) {
                     schema_val.required = std::vector<sv>{}; // allocate
                  }
                  schema_val.required->emplace_back(tag_v<T>);
                  auto& tag = (*schema_val.properties)[tag_v<T>];
                  tag.constant = ids[I];
               }
            });
         }
      };

      template <class T>
         requires glaze_array_t<std::decay_t<T>> || tuple_t<std::decay_t<T>>
      struct to_json_schema<T>
      {
         template <auto Opts>
         static void op(auto& s, auto&)
         {
            // TODO: Actually handle this. We can specify a schema per item in items
            //      We can also do size restrictions on static arrays
            s.type = {"array"};
         }
      };

      template <class T>
      consteval bool json_schema_matches_object_keys()
      {
         if constexpr (json_schema_t<T> && (count_members<json_schema_type<T>> > 0)) {
            constexpr auto& json_schema_names = member_names<json_schema_type<T>>;
            auto fields = reflect<T>::keys;
            std::sort(fields.begin(), fields.end());

            for (const auto& key : json_schema_names) {
               if (!std::binary_search(fields.begin(), fields.end(), key)) {
                  return false;
               }
            }
            return true;
         }
         else {
            return true;
         }
      }

      auto consteval has_slash(std::string_view str) noexcept -> bool
      {
         return std::ranges::any_of(str, [](const auto character) { return character == '/'; });
      }
      template <const std::string_view& ref>
      auto consteval validate_ref() noexcept -> void
      {
#if (__cpp_static_assert >= 202306L) && (__cplusplus > 202302L)
         static_assert(!has_slash(ref),
                       join_v<chars<"Slash in name: \"">, ref, chars<"\" in json schema references is not allowed">>);
#else
         static_assert(!has_slash(ref), "Slashes in json schema references are not allowed");
#endif
      }

      template <class T>
         requires((glaze_object_t<T> || reflectable<T>))
      struct to_json_schema<T>
      {
         template <auto Opts>
         static void op(auto& s, auto& defs)
         {
            static_assert(json_schema_matches_object_keys<T>());

            s.type = {"object"};

            using V = std::decay_t<T>;

            if constexpr (requires { meta<V>::required; }) {
               if (!s.required) {
                  s.required = std::vector<sv>{};
               }
               for (auto& v : meta<V>::required) {
                  s.required->emplace_back(v);
               }
            }

            if constexpr (requires { meta<V>::examples; }) {
               if (!s.examples) {
                  s.examples = std::vector<sv>{};
               }
               for (auto& v : meta<V>::examples) {
                  s.examples->emplace_back(v);
               }
            }

            static constexpr auto N = reflect<T>::size;
            static constexpr auto json_schema_size = reflect<json_schema_type<T>>::size;
            auto req = s.required.value_or(std::vector<std::string_view>{});

            s.properties = std::map<sv, schema, std::less<>>();
            for_each<N>([&]<auto I>() {
               using val_t = std::decay_t<refl_t<T, I>>;

               auto& def = defs[name_v<val_t>];

               static constexpr sv key = reflect<T>::keys[I];
               if constexpr (requires_key<T, val_t, Opts>(key)) {
                  req.emplace_back(key);
               }

               schema ref_val{};
               if constexpr (N > 0 && json_schema_size > 0) {
                  // We need schema_index to be the index within json_schema_type<T>,
                  // but this struct may have fewer keys that don't match the full set of keys in the struct T
                  // we therefore can't use `decode_hash_with_size` for either structure.
                  // Instead we just loop over the keys, looking for a match:

                  constexpr auto schema_index = [] {
                     size_t i{};
                     const auto& schema_keys = reflect<json_schema_type<T>>::keys;
                     for (; i < json_schema_size; ++i) {
                        if (schema_keys[i] == key) {
                           return i;
                        }
                     }
                     return json_schema_size;
                  }();

                  if constexpr (schema_index < json_schema_size) {
                     // Experimented with a to_array approach, but the compilation times were significantly higher
                     // even when converting this access to a run-time access
                     // Tested with both creating a std::array and a heap allocated C-style array and storing in a
                     // unique_ptr
                     static const auto schema_v = json_schema_type<T>{};
                     ref_val = get<schema_index>(to_tie(schema_v));
                  }
               }
               if (!ref_val.ref) {
                  validate_ref<name_v<val_t>>();
                  ref_val.ref = join_v<chars<"#/$defs/">, name_v<val_t>>;
               }

               if (!def.type) {
                  to_json_schema<val_t>::template op<Opts>(def, defs);
               }

               (*s.properties)[key] = ref_val;
            });
            if (!req.empty()) {
               s.required = std::move(req);
            }
            s.additionalProperties = false;
         }
      };
   }

   // Moved definition outside of write_json_schema to fix MSVC bug
   template <class Opts>
   struct opts_write_type_info_off : std::decay_t<Opts>
   {
      bool write_type_info = false;
   };

   template <class T, auto Opts = opts{}, class Buffer>
   [[nodiscard]] error_ctx write_json_schema(Buffer&& buffer)
   {
      detail::schematic s{};
      s.defs.emplace();
      detail::to_json_schema<std::decay_t<T>>::template op<Opts>(s, *s.defs);
      s.attributes.title = name_v<T>;
      // Making this static constexpr options to fix MSVC bug
      static constexpr opts options = opts_write_type_info_off<decltype(Opts)>{{Opts}};
      return write<options>(std::move(s), std::forward<Buffer>(buffer));
   }

   template <class T, auto Opts = opts{}>
   [[nodiscard]] glz::expected<std::string, error_ctx> write_json_schema()
   {
      std::string buffer{};
      const error_ctx ec = write_json_schema<T, Opts>(buffer);
      if (bool(ec)) [[unlikely]] {
         return glz::unexpected(ec);
      }
      return {buffer};
   }
}
