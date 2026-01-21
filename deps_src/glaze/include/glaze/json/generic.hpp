// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#ifndef GLZ_THROW_OR_ABORT
#if __cpp_exceptions
#define GLZ_THROW_OR_ABORT(EXC) (throw(EXC))
#define GLZ_NOEXCEPT noexcept(false)
#else
#define GLZ_THROW_OR_ABORT(EXC) (std::abort())
#define GLZ_NOEXCEPT noexcept(true)
#endif
#endif

#if __cpp_exceptions
#include <stdexcept>
#endif

namespace glz
{
   inline void glaze_error([[maybe_unused]] const char* msg) GLZ_NOEXCEPT
   {
      GLZ_THROW_OR_ABORT(std::runtime_error(msg));
   }
}

#include <cstddef>
#include <map>
#include <variant>
#include <vector>

#include "glaze/api/std/string.hpp"
#include "glaze/api/std/variant.hpp"
#include "glaze/json/read.hpp"
#include "glaze/json/write.hpp"
#include "glaze/util/expected.hpp"

#if defined(_MSC_VER) && !defined(__clang__)
// Turn off broken MSVC warning for "declaration of 'v' hides previous local declaration"
#pragma warning(push)
#pragma warning(disable : 4456)
#endif

namespace glz
{
   // Number storage mode for generic JSON types
   enum class num_mode {
      f64, // double only - fast, JavaScript-compatible (default)
      i64, // int64_t → double - signed integer precision
      u64 // uint64_t → int64_t → double - full integer range
   };

   // Helper to determine variant type based on mode
   template <num_mode Mode, class null_t, class array_t, class object_t>
   struct generic_val_type;

   template <class null_t, class array_t, class object_t>
   struct generic_val_type<num_mode::f64, null_t, array_t, object_t>
   {
      using type = std::variant<null_t, double, std::string, bool, array_t, object_t>;
   };

   template <class null_t, class array_t, class object_t>
   struct generic_val_type<num_mode::i64, null_t, array_t, object_t>
   {
      using type = std::variant<null_t, int64_t, double, std::string, bool, array_t, object_t>;
   };

   template <class null_t, class array_t, class object_t>
   struct generic_val_type<num_mode::u64, null_t, array_t, object_t>
   {
      using type = std::variant<null_t, uint64_t, int64_t, double, std::string, bool, array_t, object_t>;
   };

   // Generic json type.
   // Template parameter controls number storage mode for precise integer handling
   template <num_mode Mode = num_mode::f64>
   struct generic_json
   {
      virtual ~generic_json() {}

      using array_t = std::vector<generic_json<Mode>>;
      using object_t = std::map<std::string, generic_json<Mode>, std::less<>>;
      using null_t = std::nullptr_t;

      // Variant type depends on Mode:
      // - f64: double only (fast, JS-compatible)
      // - i64: int64_t, then double (signed integer precision)
      // - u64: uint64_t, then int64_t, then double (full integer range)
      using val_t = typename generic_val_type<Mode, null_t, array_t, object_t>::type;

      val_t data;

      /**
       * @brief Converts the JSON data to a string representation.
       * @return An `expected` containing a JSON string if successful, or an error context.
       */
      expected<std::string, error_ctx> dump() const { return write_json(data); }

      /**
       * @brief Gets the value as the specified type.
       * @tparam T The type to get the value as.
       * @return Reference to the value of the specified type.
       */
      template <class T>
      [[nodiscard]] T& get()
      {
         return std::get<T>(data);
      }

      template <class T>
      [[nodiscard]] const T& get() const
      {
         return std::get<T>(data);
      }

      template <class T>
      [[nodiscard]] T* get_if() noexcept
      {
         return std::get_if<T>(&data);
      }

      template <class T>
      [[nodiscard]] const T* get_if() const noexcept
      {
         return std::get_if<T>(&data);
      }

      template <class T>
      [[nodiscard]] T as() const
      {
         // Prefer get becuase it returns a reference
         return get<T>();
      }

      template <class T>
         requires(requires { static_cast<T>(std::declval<double>()); })
      [[nodiscard]] T as() const
      {
         // Can be used for int and the like
         if constexpr (Mode == num_mode::u64) {
            if (holds<uint64_t>()) {
               return static_cast<T>(get<uint64_t>());
            }
            if (holds<int64_t>()) {
               return static_cast<T>(get<int64_t>());
            }
         }
         else if constexpr (Mode == num_mode::i64) {
            if (holds<int64_t>()) {
               return static_cast<T>(get<int64_t>());
            }
         }
         return static_cast<T>(get<double>());
      }

      template <class T>
         requires std::convertible_to<std::string, T>
      [[nodiscard]] T as() const
      {
         // Can be used for string_view and the like
         return get<std::string>();
      }

      template <class T>
      [[nodiscard]] bool holds() const noexcept
      {
         return std::holds_alternative<T>(data);
      }

      generic_json& operator[](std::integral auto&& index) { return std::get<array_t>(data)[index]; }

      const generic_json& operator[](std::integral auto&& index) const { return std::get<array_t>(data)[index]; }

      generic_json& operator[](std::convertible_to<std::string_view> auto&& key)
      {
         //[] operator for maps does not support heterogeneous lookups yet
         if (holds<null_t>()) data = object_t{};
         auto& object = std::get<object_t>(data);
         auto iter = object.find(key);
         if (iter == object.end()) {
            iter = object.insert(std::make_pair(std::string(key), generic_json{})).first;
         }
         return iter->second;
      }

      const generic_json& operator[](std::convertible_to<std::string_view> auto&& key) const
      {
         //[] operator for maps does not support heterogeneous lookups yet
         auto& object = std::get<object_t>(data);
         auto iter = object.find(key);
         if (iter == object.end()) {
            glaze_error("Key not found.");
         }
         return iter->second;
      }

      generic_json& operator=(const std::nullptr_t value)
      {
         data = value;
         return *this;
      }

      generic_json& operator=(const double value)
      {
         data = value;
         return *this;
      }

      // for unsigned integers
      template <class T>
         requires(std::unsigned_integral<T> && !std::same_as<T, bool>)
      generic_json& operator=(const T value)
      {
         if constexpr (Mode == num_mode::u64) {
            data = static_cast<uint64_t>(value);
         }
         else if constexpr (Mode == num_mode::i64) {
            data = static_cast<int64_t>(value);
         }
         else {
            data = static_cast<double>(value);
         }
         return *this;
      }

      // for signed integers
      template <class T>
         requires(std::signed_integral<T>)
      generic_json& operator=(const T value)
      {
         if constexpr (Mode == num_mode::u64 || Mode == num_mode::i64) {
            data = static_cast<int64_t>(value);
         }
         else {
            data = static_cast<double>(value);
         }
         return *this;
      }

      generic_json& operator=(const std::string& value)
      {
         data = value;
         return *this;
      }

      generic_json& operator=(const std::string_view value)
      {
         data = std::string(value);
         return *this;
      }

      generic_json& operator=(const char* value)
      {
         data = std::string(value);
         return *this;
      }

      generic_json& operator=(const bool value)
      {
         data = value;
         return *this;
      }

      generic_json& operator=(const array_t& value)
      {
         data = value;
         return *this;
      }

      generic_json& operator=(const object_t& value)
      {
         data = value;
         return *this;
      }

      template <class T>
         requires(!std::is_same_v<std::decay_t<T>, generic_json> && !std::is_same_v<std::decay_t<T>, std::nullptr_t> &&
                  !std::is_same_v<std::decay_t<T>, double> && !std::is_same_v<std::decay_t<T>, bool> &&
                  !std::is_same_v<std::decay_t<T>, std::string> && !std::is_same_v<std::decay_t<T>, std::string_view> &&
                  !std::is_same_v<std::decay_t<T>, const char*> && !std::is_same_v<std::decay_t<T>, array_t> &&
                  !std::is_same_v<std::decay_t<T>, object_t> && !std::integral<std::decay_t<T>> &&
                  !std::floating_point<std::decay_t<T>> && requires { write_json(std::declval<T>()); })
      generic_json& operator=(T&& value)
      {
         auto json_str = write_json(std::forward<T>(value));
         if (json_str) {
            auto result = read_json<generic_json>(*json_str);
            if (result) {
               *this = std::move(*result);
            }
         }
         return *this;
      }

      [[nodiscard]] generic_json& at(std::convertible_to<std::string_view> auto&& key) { return operator[](key); }

      [[nodiscard]] const generic_json& at(std::convertible_to<std::string_view> auto&& key) const
      {
         return operator[](key);
      }

      [[nodiscard]] bool contains(std::convertible_to<std::string_view> auto&& key) const
      {
         if (!holds<object_t>()) return false;
         auto& object = std::get<object_t>(data);
         auto iter = object.find(key);
         return iter != object.end();
      }

      explicit operator bool() const { return !std::holds_alternative<null_t>(data); }

      val_t* operator->() noexcept { return &data; }

      val_t& operator*() noexcept { return data; }

      const val_t& operator*() const noexcept { return data; }

      void reset() noexcept { data = null_t{}; }

      generic_json() = default;
      generic_json(const generic_json& other) : data(other.data) {}
      generic_json& operator=(const generic_json&) = default;
      generic_json(generic_json&& other) noexcept : data(std::move(other.data)) {}
      generic_json& operator=(generic_json&&) = default;

      template <class T>
         requires std::convertible_to<T, val_t> && (!std::derived_from<std::decay_t<T>, generic_json>)
      generic_json(T&& val) : data(std::forward<T>(val))
      {}

      template <class T>
         requires std::convertible_to<T, double> && (!std::derived_from<std::decay_t<T>, generic_json>) &&
                  (!std::convertible_to<T, val_t>)
      generic_json(T&& val)
      {
         if constexpr (Mode == num_mode::u64) {
            if constexpr (std::unsigned_integral<T>) {
               data = static_cast<uint64_t>(val);
            }
            else if constexpr (std::signed_integral<T>) {
               data = static_cast<int64_t>(val);
            }
            else {
               data = static_cast<double>(val);
            }
         }
         else if constexpr (Mode == num_mode::i64) {
            if constexpr (std::integral<T>) {
               data = static_cast<int64_t>(val);
            }
            else {
               data = static_cast<double>(val);
            }
         }
         else {
            data = static_cast<double>(val);
         }
      }

      generic_json(const std::string_view value) : data(std::string(value)) {}

      generic_json(std::initializer_list<std::pair<const char*, generic_json>>&& obj)
      {
         data.template emplace<object_t>();
         auto& data_obj = std::get<object_t>(data);
         for (auto&& pair : obj) {
            // std::initializer_list holds const values that cannot be moved
            data_obj.emplace(pair.first, pair.second);
         }
      }

      // Prevent conflict with object initializer list
      template <bool deprioritize = true>
      generic_json(std::initializer_list<generic_json>&& arr)
      {
         data.template emplace<array_t>(std::move(arr));
      }

      [[nodiscard]] bool is_array() const noexcept { return holds<array_t>(); }

      [[nodiscard]] bool is_object() const noexcept { return holds<object_t>(); }

      [[nodiscard]] bool is_number() const noexcept
      {
         if constexpr (Mode == num_mode::u64) {
            return holds<uint64_t>() || holds<int64_t>() || holds<double>();
         }
         else if constexpr (Mode == num_mode::i64) {
            return holds<int64_t>() || holds<double>();
         }
         else {
            return holds<double>();
         }
      }

      [[nodiscard]] bool is_string() const noexcept { return holds<std::string>(); }

      [[nodiscard]] bool is_boolean() const noexcept { return holds<bool>(); }

      [[nodiscard]] bool is_null() const noexcept { return holds<std::nullptr_t>(); }

      [[nodiscard]] array_t& get_array() { return get<array_t>(); }
      [[nodiscard]] const array_t& get_array() const { return get<array_t>(); }

      [[nodiscard]] object_t& get_object() { return get<object_t>(); }
      [[nodiscard]] const object_t& get_object() const { return get<object_t>(); }

      [[nodiscard]] double& get_number()
      {
         if constexpr (Mode == num_mode::u64) {
            if (holds<uint64_t>()) {
               glaze_error(
                  "Cannot get reference to double when variant holds uint64_t. Use as<double>() for conversion.");
            }
            if (holds<int64_t>()) {
               glaze_error(
                  "Cannot get reference to double when variant holds int64_t. Use as<double>() for conversion.");
            }
         }
         else if constexpr (Mode == num_mode::i64) {
            if (holds<int64_t>()) {
               glaze_error(
                  "Cannot get reference to double when variant holds int64_t. Use as<double>() for conversion.");
            }
         }
         return get<double>();
      }
      [[nodiscard]] const double& get_number() const
      {
         if constexpr (Mode == num_mode::u64) {
            if (holds<uint64_t>()) {
               glaze_error(
                  "Cannot get reference to double when variant holds uint64_t. Use as<double>() for conversion.");
            }
            if (holds<int64_t>()) {
               glaze_error(
                  "Cannot get reference to double when variant holds int64_t. Use as<double>() for conversion.");
            }
         }
         else if constexpr (Mode == num_mode::i64) {
            if (holds<int64_t>()) {
               glaze_error(
                  "Cannot get reference to double when variant holds int64_t. Use as<double>() for conversion.");
            }
         }
         return get<double>();
      }

      // Get number as double, converting from integer types if needed (only available in i64/u64 modes)
      [[nodiscard]] double as_number() const
         requires(Mode != num_mode::f64)
      {
         if constexpr (Mode == num_mode::u64) {
            if (holds<uint64_t>()) {
               return static_cast<double>(get<uint64_t>());
            }
         }
         if constexpr (Mode == num_mode::u64 || Mode == num_mode::i64) {
            if (holds<int64_t>()) {
               return static_cast<double>(get<int64_t>());
            }
         }
         return get<double>();
      }

      // Check if the number is stored as uint64_t (only available in u64 mode)
      [[nodiscard]] bool is_uint64() const noexcept
         requires(Mode == num_mode::u64)
      {
         return holds<uint64_t>();
      }

      // Check if the number is stored as int64_t (only available in i64/u64 modes)
      [[nodiscard]] bool is_int64() const noexcept
         requires(Mode != num_mode::f64)
      {
         return holds<int64_t>();
      }

      // Check if the number is stored as double (only available in i64/u64 modes)
      [[nodiscard]] bool is_double() const noexcept
         requires(Mode != num_mode::f64)
      {
         return holds<double>();
      }

      [[nodiscard]] std::string& get_string() { return get<std::string>(); }
      [[nodiscard]] const std::string& get_string() const { return get<std::string>(); }

      [[nodiscard]] bool& get_boolean() { return get<bool>(); }
      [[nodiscard]] const bool& get_boolean() const { return get<bool>(); }

      // empty() returns true if the value is an empty JSON object, array, or string, or a null value
      // otherwise returns false
      [[nodiscard]] bool empty() const noexcept
      {
         if (auto* v = get_if<object_t>()) {
            return v->empty();
         }
         else if (auto* v = get_if<array_t>()) {
            return v->empty();
         }
         else if (auto* v = get_if<std::string>()) {
            return v->empty();
         }
         else if (is_null()) {
            return true;
         }
         else {
            return false;
         }
      }

      // returns the count of items in an object or an array, or the size of a string, otherwise returns zero
      [[nodiscard]] size_t size() const noexcept
      {
         if (auto* v = get_if<object_t>()) {
            return v->size();
         }
         else if (auto* v = get_if<array_t>()) {
            return v->size();
         }
         else if (auto* v = get_if<std::string>()) {
            return v->size();
         }
         else {
            return 0;
         }
      }
   };

   // Type aliases for different number handling modes
   using generic = generic_json<num_mode::f64>; // double only - fast, JavaScript-compatible
   using generic_i64 = generic_json<num_mode::i64>; // int64_t → double - signed integer precision
   using generic_u64 = generic_json<num_mode::u64>; // uint64_t → int64_t → double - full integer range

   // Backwards compatibility alias
   using json_t [[deprecated("glz::json_t is deprecated, use glz::generic instead")]] = generic;

   template <num_mode Mode>
   [[nodiscard]] inline bool is_array(const generic_json<Mode>& value) noexcept
   {
      return value.is_array();
   }

   template <num_mode Mode>
   [[nodiscard]] inline bool is_object(const generic_json<Mode>& value) noexcept
   {
      return value.is_object();
   }

   template <num_mode Mode>
   [[nodiscard]] inline bool is_number(const generic_json<Mode>& value) noexcept
   {
      return value.is_number();
   }

   template <num_mode Mode>
   [[nodiscard]] inline bool is_string(const generic_json<Mode>& value) noexcept
   {
      return value.is_string();
   }

   template <num_mode Mode>
   [[nodiscard]] inline bool is_boolean(const generic_json<Mode>& value) noexcept
   {
      return value.is_boolean();
   }

   template <num_mode Mode>
   [[nodiscard]] inline bool is_null(const generic_json<Mode>& value) noexcept
   {
      return value.is_null();
   }
}

template <glz::num_mode Mode>
struct glz::meta<glz::generic_json<Mode>>
{
   static constexpr std::string_view name = "glz::generic";
   using T = glz::generic_json<Mode>;
   static constexpr auto value = &T::data;
};

namespace glz
{
   // Concept for types that can be directly converted from generic without JSON round-trip
   template <class T>
   concept directly_convertible_from_generic =
      std::same_as<T, bool> || std::same_as<T, double> || std::same_as<T, std::string> ||
      std::same_as<T, std::string_view> || std::integral<T> || (readable_array_t<T> && !std::same_as<T, std::string>) ||
      readable_map_t<T>;

   // These functions allow a generic value to be read/written to a C++ struct

   // Optimized overload for types that support direct conversion
   template <auto Opts, class T>
      requires read_supported<T, Opts.format> && directly_convertible_from_generic<T>
   [[nodiscard]] error_ctx read(T& value, const generic& source)
   {
      return convert_from_generic(value, source);
   }

   // General overload for complex types (structs, etc.) that need JSON round-trip
   template <auto Opts, class T>
      requires read_supported<T, Opts.format> && (!directly_convertible_from_generic<T>)
   [[nodiscard]] error_ctx read(T& value, const generic& source)
   {
      auto buffer = source.dump();
      if (buffer) {
         context ctx{};
         return read<Opts>(value, *buffer, ctx);
      }
      else {
         return buffer.error();
      }
   }

   // Optimized overload for types that support direct conversion
   template <read_supported<JSON> T>
      requires directly_convertible_from_generic<T>
   [[nodiscard]] error_ctx read_json(T& value, const generic& source)
   {
      return convert_from_generic(value, source);
   }

   // General overload for complex types (structs, etc.) that need JSON round-trip
   template <read_supported<JSON> T>
      requires(!directly_convertible_from_generic<T>)
   [[nodiscard]] error_ctx read_json(T& value, const generic& source)
   {
      auto buffer = source.dump();
      if (buffer) {
         return read_json(value, *buffer);
      }
      else {
         return buffer.error();
      }
   }

   // Optimized overload for types that support direct conversion
   template <read_supported<JSON> T>
      requires directly_convertible_from_generic<T>
   [[nodiscard]] expected<T, error_ctx> read_json(const generic& source)
   {
      T result;
      auto ec = convert_from_generic(result, source);
      if (ec) {
         return unexpected(ec);
      }
      return result;
   }

   // General overload for complex types (structs, etc.) that need JSON round-trip
   template <read_supported<JSON> T>
      requires(!directly_convertible_from_generic<T>)
   [[nodiscard]] expected<T, error_ctx> read_json(const generic& source)
   {
      auto buffer = source.dump();
      if (buffer) {
         return read_json<T>(*buffer);
      }
      else {
         return unexpected(buffer.error());
      }
   }
}

#if defined(_MSC_VER) && !defined(__clang__)
// restore disabled warning
#pragma warning(pop)
#endif

#include "glaze/core/seek.hpp"

namespace glz
{
   // Specialization for glz::generic
   template <>
   struct seek_op<glz::generic>
   {
      template <class F>
      static bool op(F&& func, auto&& value, sv json_ptr)
      {
         if (json_ptr.empty()) {
            // At target - call func with the actual variant data, not the generic wrapper
            std::visit([&func](auto&& variant_value) { func(variant_value); }, value.data);
            return true;
         }

         if (json_ptr[0] != '/' || json_ptr.size() < 2) return false;

         // Handle object access
         if (value.is_object()) {
            auto& obj = value.get_object();

            // Parse the key (with JSON Pointer escaping)
            std::string key;
            size_t i = 1;
            for (; i < json_ptr.size(); ++i) {
               auto c = json_ptr[i];
               if (c == '/')
                  break;
               else if (c == '~') {
                  if (++i == json_ptr.size()) return false;
                  c = json_ptr[i];
                  if (c == '0')
                     c = '~';
                  else if (c == '1')
                     c = '/';
                  else
                     return false;
               }
               key.push_back(c);
            }

            auto it = obj.find(key);
            if (it == obj.end()) return false;

            sv remaining_ptr = json_ptr.substr(i);
            return seek(std::forward<F>(func), it->second, remaining_ptr);
         }
         // Handle array access
         else if (value.is_array()) {
            auto& arr = value.get_array();

            // Parse the index
            size_t index{};
            auto [p, ec] = std::from_chars(&json_ptr[1], json_ptr.data() + json_ptr.size(), index);
            if (ec != std::errc{}) return false;

            if (index >= arr.size()) return false;

            sv remaining_ptr = json_ptr.substr(p - json_ptr.data());
            return seek(std::forward<F>(func), arr[index], remaining_ptr);
         }

         return false;
      }
   };

   // Helper function to navigate to a specific location in a glz::generic using a JSON pointer
   // Returns a pointer to the generic at that location, or nullptr if not found
   // Template implementation to handle both const and non-const cases
   template <class GenericPtr>
   inline auto navigate_to_impl(GenericPtr root, sv json_ptr) noexcept -> GenericPtr
   {
      if (!root) return nullptr;
      if (json_ptr.empty()) return root;
      // RFC 6901: "/" is valid (refers to empty string key), so don't require size >= 2
      if (json_ptr[0] != '/') return nullptr;

      GenericPtr current = root;
      sv remaining = json_ptr;

      while (!remaining.empty() && remaining[0] == '/') {
         remaining.remove_prefix(1); // Remove leading '/'

         // Find the next '/' or end of string
         size_t key_end = remaining.find('/');
         sv key = (key_end == sv::npos) ? remaining : remaining.substr(0, key_end);

         // Check if JSON Pointer escaping is needed
         const bool needs_unescape = key.find('~') != sv::npos;

         // Navigate based on current type
         if (current->is_object()) {
            auto& obj = current->get_object();

            if (needs_unescape) {
               // Only allocate string when escaping is present
               std::string unescaped_key;
               unescaped_key.reserve(key.size());

               for (size_t i = 0; i < key.size(); ++i) {
                  if (key[i] == '~' && i + 1 < key.size()) {
                     if (key[i + 1] == '0')
                        unescaped_key += '~';
                     else if (key[i + 1] == '1')
                        unescaped_key += '/';
                     else
                        return nullptr; // Invalid escape sequence
                     ++i;
                  }
                  else {
                     unescaped_key += key[i];
                  }
               }

               auto it = obj.find(unescaped_key);
               if (it == obj.end()) return nullptr;
               current = &(it->second);
            }
            else {
               // Use heterogeneous lookup with string_view (no allocation)
               auto it = obj.find(key);
               if (it == obj.end()) return nullptr;
               current = &(it->second);
            }
         }
         else if (current->is_array()) {
            // Array indices must be plain numbers (no escaping applies to indices)
            // If key contains '~', it's invalid as an array index and will fail to parse
            size_t index = 0;
            auto [ptr, ec] = std::from_chars(key.data(), key.data() + key.size(), index);
            if (ec != std::errc{} || ptr != key.data() + key.size()) {
               return nullptr; // Invalid index
            }
            auto& arr = current->get_array();
            if (index >= arr.size()) return nullptr;
            current = &arr[index];
         }
         else {
            return nullptr; // Can't navigate further
         }

         // Move to next segment
         if (key_end == sv::npos) {
            remaining = sv{};
         }
         else {
            remaining = remaining.substr(key_end);
         }
      }

      return current;
   }

   // Non-const version
   inline generic* navigate_to(generic* root, sv json_ptr) noexcept { return navigate_to_impl(root, json_ptr); }

   // Const version
   inline const generic* navigate_to(const generic* root, sv json_ptr) noexcept
   {
      return navigate_to_impl(root, json_ptr);
   }

   // Direct conversion from generic to target types without JSON serialization
   // Takes result by reference to allow memory reuse and avoid extra moves

   // Forward declaration for recursive conversion
   template <class T, num_mode Mode>
   error_ctx convert_from_generic(T& result, const generic_json<Mode>& source);

   // Specialization for bool
   template <num_mode Mode>
   inline error_ctx convert_from_generic(bool& result, const generic_json<Mode>& source)
   {
      if (!source.is_boolean()) {
         return error_ctx{0, error_code::syntax_error};
      }
      result = source.template get<bool>();
      return {};
   }

   // Specialization for double
   template <num_mode Mode>
   inline error_ctx convert_from_generic(double& result, const generic_json<Mode>& source)
   {
      if (!source.is_number()) {
         return error_ctx{0, error_code::syntax_error};
      }
      if constexpr (Mode == num_mode::u64) {
         if (source.template holds<uint64_t>()) {
            result = static_cast<double>(source.template get<uint64_t>());
         }
         else if (source.template holds<int64_t>()) {
            result = static_cast<double>(source.template get<int64_t>());
         }
         else {
            result = source.template get<double>();
         }
      }
      else if constexpr (Mode == num_mode::i64) {
         if (source.template holds<int64_t>()) {
            result = static_cast<double>(source.template get<int64_t>());
         }
         else {
            result = source.template get<double>();
         }
      }
      else {
         result = source.template get<double>();
      }
      return {};
   }

   // Specialization for uint64_t (only in u64 mode)
   template <num_mode Mode>
      requires(Mode == num_mode::u64)
   inline error_ctx convert_from_generic(uint64_t& result, const generic_json<Mode>& source)
   {
      if (!source.is_number()) {
         return error_ctx{0, error_code::syntax_error};
      }
      if (source.template holds<uint64_t>()) {
         result = source.template get<uint64_t>();
      }
      else if (source.template holds<int64_t>()) {
         result = static_cast<uint64_t>(source.template get<int64_t>());
      }
      else {
         result = static_cast<uint64_t>(source.template get<double>());
      }
      return {};
   }

   // Specialization for int64_t (in i64 and u64 modes)
   template <num_mode Mode>
      requires(Mode != num_mode::f64)
   inline error_ctx convert_from_generic(int64_t& result, const generic_json<Mode>& source)
   {
      if (!source.is_number()) {
         return error_ctx{0, error_code::syntax_error};
      }
      if constexpr (Mode == num_mode::u64) {
         if (source.template holds<uint64_t>()) {
            result = static_cast<int64_t>(source.template get<uint64_t>());
         }
         else if (source.template holds<int64_t>()) {
            result = source.template get<int64_t>();
         }
         else {
            result = static_cast<int64_t>(source.template get<double>());
         }
      }
      else {
         if (source.template holds<int64_t>()) {
            result = source.template get<int64_t>();
         }
         else {
            result = static_cast<int64_t>(source.template get<double>());
         }
      }
      return {};
   }

   // Specialization for string
   template <num_mode Mode>
   inline error_ctx convert_from_generic(std::string& result, const generic_json<Mode>& source)
   {
      if (!source.is_string()) {
         return error_ctx{0, error_code::syntax_error};
      }
      result = source.template get<std::string>();
      return {};
   }

   // Specialization for integer types (convert from integer types or double)
   template <class T, num_mode Mode>
      requires(std::integral<T> && !std::same_as<T, bool> && !std::same_as<T, int64_t> && !std::same_as<T, uint64_t>)
   error_ctx convert_from_generic(T& result, const generic_json<Mode>& source)
   {
      if (!source.is_number()) {
         return error_ctx{0, error_code::syntax_error};
      }
      if constexpr (Mode == num_mode::u64) {
         if (source.template holds<uint64_t>()) {
            result = static_cast<T>(source.template get<uint64_t>());
         }
         else if (source.template holds<int64_t>()) {
            result = static_cast<T>(source.template get<int64_t>());
         }
         else {
            result = static_cast<T>(source.template get<double>());
         }
      }
      else if constexpr (Mode == num_mode::i64) {
         if (source.template holds<int64_t>()) {
            result = static_cast<T>(source.template get<int64_t>());
         }
         else {
            result = static_cast<T>(source.template get<double>());
         }
      }
      else {
         result = static_cast<T>(source.template get<double>());
      }
      return {};
   }

   // Specialization for array-like containers
   template <class T, num_mode Mode>
      requires(readable_array_t<T> && !std::same_as<T, std::string>)
   error_ctx convert_from_generic(T& result, const generic_json<Mode>& source)
   {
      if (!source.is_array()) {
         return error_ctx{0, error_code::syntax_error};
      }

      const auto& arr = source.get_array();

      if constexpr (resizable<T>) {
         result.clear();
         if constexpr (has_reserve<T>) {
            result.reserve(arr.size());
         }
      }

      size_t index = 0;
      for (const auto& elem : arr) {
         using value_type = range_value_t<T>;
         value_type converted;
         auto ec = convert_from_generic(converted, elem);
         if (ec) {
            return ec;
         }

         if constexpr (resizable<T> && push_backable<T>) {
            result.push_back(std::move(converted));
         }
         else if constexpr (resizable<T> && emplace_backable<T>) {
            result.emplace_back(std::move(converted));
         }
         else if constexpr (accessible<T>) {
            if (index >= result.size()) {
               return error_ctx{0, error_code::syntax_error};
            }
            result[index] = std::move(converted);
         }
         else if constexpr (emplaceable<T>) {
            result.emplace(std::move(converted));
         }
         else {
            return error_ctx{0, error_code::syntax_error};
         }

         ++index;
      }

      return {};
   }

   // Specialization for map-like containers
   template <class T, num_mode Mode>
      requires readable_map_t<T>
   error_ctx convert_from_generic(T& result, const generic_json<Mode>& source)
   {
      if (!source.is_object()) {
         return error_ctx{0, error_code::syntax_error};
      }

      const auto& obj = source.get_object();

      for (const auto& [key, value] : obj) {
         using mapped_type = typename T::mapped_type;
         mapped_type converted;
         auto ec = convert_from_generic(converted, value);
         if (ec) {
            return ec;
         }

         if constexpr (map_subscriptable<T>) {
            result[key] = std::move(converted);
         }
         else if constexpr (emplaceable<T>) {
            result.emplace(key, std::move(converted));
         }
         else {
            return error_ctx{0, error_code::syntax_error};
         }
      }

      return {};
   }

   // Concept to determine if a type needs deserialization from generic
   // These are container types that can't be directly referenced from glz::generic
   // because they're not the exact types stored in generic's variant
   template <class V, class GenericType>
   concept needs_container_deserialization_for =
      (readable_array_t<V> || readable_map_t<V>) && !std::same_as<V, GenericType> &&
      !std::same_as<V, typename GenericType::array_t> && !std::same_as<V, typename GenericType::object_t> &&
      !std::same_as<V, std::string> && !std::same_as<V, double> && !std::same_as<V, bool> &&
      !std::same_as<V, std::nullptr_t>;

   // Legacy concept for backward compatibility
   template <class V>
   concept needs_container_deserialization = needs_container_deserialization_for<V, generic>;

   // Overload of get for glz::generic that deserializes containers
   // This overload is only selected for container types that need deserialization
   // Uses direct traversal instead of JSON serialization for better performance
   template <class V>
      requires needs_container_deserialization<V>
   expected<V, error_ctx> get(generic& root, sv json_ptr)
   {
      // Navigate to the target location
      generic* target = navigate_to(&root, json_ptr);
      if (!target) {
         return unexpected(error_ctx{0, error_code::nonexistent_json_ptr});
      }

      // Direct conversion without JSON serialization round-trip
      V result;
      auto ec = convert_from_generic(result, *target);
      if (ec) {
         return unexpected(ec);
      }
      return result;
   }

   // Const version
   template <class V>
      requires needs_container_deserialization<V>
   expected<V, error_ctx> get(const generic& root, sv json_ptr)
   {
      const generic* target = navigate_to(&root, json_ptr);
      if (!target) {
         return unexpected(error_ctx{0, error_code::nonexistent_json_ptr});
      }

      // Direct conversion without JSON serialization round-trip
      V result;
      auto ec = convert_from_generic(result, *target);
      if (ec) {
         return unexpected(ec);
      }
      return result;
   }
}
