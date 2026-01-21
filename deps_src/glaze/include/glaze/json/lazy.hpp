// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <string_view>

#include "glaze/json/read.hpp"
#include "glaze/json/skip.hpp"
#include "glaze/json/write.hpp"
#include "glaze/util/expected.hpp"

namespace glz
{
   // Forward declarations (needed for circular references)
   template <opts Opts>
   struct lazy_document;
   template <opts Opts>
   class lazy_iterator;
   template <opts Opts>
   struct indexed_lazy_view;
   template <opts Opts>
   class indexed_lazy_iterator;

   // ============================================================================
   // Truly Lazy JSON Parser - No upfront processing
   // ============================================================================

   namespace detail
   {
      // Skip string - returns position after closing quote
      template <opts Opts>
      GLZ_ALWAYS_INLINE const char* skip_string_fast(const char* p, const char* end) noexcept
      {
         const char* const start = p;
         ++p; // skip opening quote

         // Find closing quote using memchr (SIMD-optimized in libc)
         while (p < end) {
            const char* q = static_cast<const char*>(std::memchr(p, '"', static_cast<size_t>(end - p)));
            if (!q) [[unlikely]] {
               return end; // unclosed string
            }

            // Count preceding backslashes to check if escaped
            size_t backslashes = 0;
            const char* check = q - 1;
            while (check > start && *check == '\\') {
               ++backslashes;
               --check;
            }

            p = q + 1;
            if ((backslashes & 1) == 0) {
               return p; // even backslashes = real quote
            }
            // odd backslashes = escaped quote, continue searching
         }
         return p;
      }

      // Skip from current position to depth zero (end of enclosing container)
      // Used after partial scanning to find container end efficiently
      template <opts Opts>
      GLZ_ALWAYS_INLINE const char* skip_to_depth_zero(const char* p, const char* end, int depth) noexcept
      {
         using enum lazy_char_type;
         while (depth > 0) {
            if constexpr (Opts.null_terminated) {
               if (*p == '\0') break;
            }
            else {
               if (p >= end) break;
            }
            switch (lazy_char_class[uint8_t(*p)]) {
            case quote:
               p = skip_string_fast<Opts>(p, end);
               break;
            case open:
               ++depth;
               ++p;
               break;
            case close:
               --depth;
               ++p;
               break;
            case number:
               ++p;
               if constexpr (Opts.null_terminated) {
                  while (numeric_table[uint8_t(*p)]) ++p;
               }
               else {
                  while (p < end && numeric_table[uint8_t(*p)]) ++p;
               }
               break;
            default:
               ++p;
               break;
            }
         }
         return p;
      }

      // Skip any JSON value - optimized container skip
      // Returns pointer after the value
      template <opts Opts>
      GLZ_ALWAYS_INLINE const char* skip_value_lazy(const char* p, const char* end) noexcept
      {
         using enum lazy_char_type;
         switch (*p) {
         case '"':
            return skip_string_fast<Opts>(p, end);
         case 't':
            return p + 4;
         case 'f':
            return p + 5;
         case 'n':
            return p + 4;
         case '[':
         case '{': {
            int depth = 1;
            ++p;

            while (depth > 0) {
               if constexpr (Opts.null_terminated) {
                  if (*p == '\0') break;
               }
               else {
                  if (p >= end) break;
               }
               switch (lazy_char_class[uint8_t(*p)]) {
               case quote:
                  p = skip_string_fast<Opts>(p, end);
                  break;
               case open:
                  ++depth;
                  ++p;
                  break;
               case close:
                  --depth;
                  ++p;
                  break;
               case number:
                  ++p;
                  if constexpr (Opts.null_terminated) {
                     while (numeric_table[uint8_t(*p)]) ++p;
                  }
                  else {
                     while (p < end && numeric_table[uint8_t(*p)]) ++p;
                  }
                  break;
               default:
                  ++p;
                  break;
               }
            }
            return p;
         }
         default:
            // Number
            if constexpr (Opts.null_terminated) {
               while (numeric_table[uint8_t(*p)]) ++p;
            }
            else {
               while (p < end && numeric_table[uint8_t(*p)]) ++p;
            }
            return p;
         }
      }
   } // namespace detail

   // ============================================================================
   // lazy_json_view - Truly lazy view with on-demand scanning
   // ============================================================================

   /**
    * @brief A truly lazy view into JSON data.
    *
    * No upfront processing - all navigation uses SWAR-accelerated scanning.
    *
    * For objects, parse_pos_ tracks the current scan position to enable
    * efficient sequential key access (O(n) total instead of O(n²)).
    *
    * Memory layout (48 bytes on 64-bit, 24 bytes on 32-bit):
    * - doc_: pointer to document (8/4 bytes)
    * - data_: pointer to value start in JSON (8/4 bytes)
    * - parse_pos_: current scan position for progressive parsing (8/4 bytes)
    * - key_: stored key for iteration (16/8 bytes - string_view)
    * - error_: error code (4 bytes)
    * - padding: 4/0 bytes
    */
   template <opts Opts = opts{}>
   struct lazy_json_view
   {
     private:
      const lazy_document<Opts>* doc_{};
      const char* data_{};
      mutable const char* parse_pos_{}; // Current scan position (advances on key access)
      std::string_view key_{};
      error_code error_{error_code::none};

      lazy_json_view(error_code ec) noexcept : error_(ec) {}

     public:
      lazy_json_view() = default;
      lazy_json_view(const lazy_document<Opts>* doc, const char* data) noexcept : doc_(doc), data_(data) {}

      [[nodiscard]] static lazy_json_view make_error(error_code ec) noexcept { return lazy_json_view{ec}; }

      [[nodiscard]] bool has_error() const noexcept { return error_ != error_code::none; }
      [[nodiscard]] error_code error() const noexcept { return error_; }

      // Type checking - direct from JSON byte
      [[nodiscard]] bool is_null() const noexcept { return has_error() || !data_ || *data_ == 'n'; }
      [[nodiscard]] bool is_boolean() const noexcept
      {
         return !has_error() && data_ && (*data_ == 't' || *data_ == 'f');
      }
      [[nodiscard]] bool is_number() const noexcept
      {
         return !has_error() && data_ && (is_digit(uint8_t(*data_)) || *data_ == '-');
      }
      [[nodiscard]] bool is_string() const noexcept { return !has_error() && data_ && *data_ == '"'; }
      [[nodiscard]] bool is_array() const noexcept { return !has_error() && data_ && *data_ == '['; }
      [[nodiscard]] bool is_object() const noexcept { return !has_error() && data_ && *data_ == '{'; }

      explicit operator bool() const noexcept { return !has_error() && data_ && *data_ != 'n'; }

      [[nodiscard]] const char* data() const noexcept { return data_; }
      [[nodiscard]] const char* json_end() const noexcept;

      /// @brief Get the raw JSON bytes for this value
      /// @return string_view of the raw JSON, or empty if error
      /// @note Useful for passing to glz::read_json to deserialize into a struct
      /// @note Consider using read_into<T>() instead for better performance
      [[nodiscard]] std::string_view raw_json() const noexcept
      {
         if (has_error() || !data_) return {};
         const char* end = detail::skip_value_lazy<Opts>(data_, json_end());
         return {data_, static_cast<size_t>(end - data_)};
      }

      /// @brief Parse this value directly into a C++ type (single-pass, no double scanning)
      /// @tparam T The type to parse into
      /// @param value The object to populate
      /// @return Error context (check error_ctx.error for success/failure)
      /// @note This is more efficient than raw_json() + read_json() as it avoids scanning twice
      template <class T>
      [[nodiscard]] error_ctx read_into(T& value) const
      {
         if (has_error()) {
            return error_ctx{0, error_};
         }
         if (!data_) {
            return error_ctx{0, error_code::unexpected_end};
         }
         // Use parse<JSON>::op which naturally stops at value end
         // This avoids the need to pre-scan to find the value's extent
         context ctx{};
         auto it = data_;
         auto end = json_end();
         parse<JSON>::op<opts{}>(value, ctx, it, end);
         if (bool(ctx.error)) {
            return error_ctx{static_cast<size_t>(it - data_), ctx.error};
         }
         return {};
      }

      template <class T>
      [[nodiscard]] expected<T, error_ctx> get() const;

      [[nodiscard]] lazy_json_view operator[](size_t index) const;
      [[nodiscard]] lazy_json_view operator[](std::string_view key) const;
      [[nodiscard]] bool contains(std::string_view key) const;
      [[nodiscard]] size_t size() const;
      [[nodiscard]] bool empty() const noexcept;

      // Key access for object iteration
      [[nodiscard]] std::string_view key() const noexcept { return key_; }

      [[nodiscard]] lazy_iterator<Opts> begin() const;
      [[nodiscard]] lazy_iterator<Opts> end() const;

      /// @brief Build an index for O(1) iteration and random access
      /// @return indexed_lazy_view with pre-computed element positions
      [[nodiscard]] indexed_lazy_view<Opts> index() const;

     private:
      friend struct lazy_document<Opts>;
      friend class lazy_iterator<Opts>;
      friend struct indexed_lazy_view<Opts>;
      friend class indexed_lazy_iterator<Opts>;

      // Constructor with key for iteration
      lazy_json_view(const lazy_document<Opts>* doc, const char* data, std::string_view key) noexcept
         : doc_(doc), data_(data), key_(key)
      {}

      // Skip whitespace helper
      // Note: The minified option is not used here because branch prediction
      // makes the whitespace loop essentially free when there's no whitespace.
      // Benchmarks show no benefit from skipping this check.
      static void skip_ws(const char*& p, [[maybe_unused]] const char* end) noexcept
      {
         if constexpr (Opts.null_terminated) {
            while (whitespace_table[uint8_t(*p)]) ++p;
         }
         else {
            while (p < end && whitespace_table[uint8_t(*p)]) ++p;
         }
      }

      // Parse a key from current position, return key and advance p
      static std::string_view parse_key(const char*& p, const char* end) noexcept;
   };

   // ============================================================================
   // lazy_document - Minimal container, no upfront processing
   // ============================================================================

   template <opts Opts = opts{}>
   struct lazy_document
   {
     private:
      const char* json_{};
      size_t len_{};
      const char* root_data_{};
      mutable lazy_json_view<Opts> root_view_{}; // Cached root view with parse_pos_

      friend struct lazy_json_view<Opts>;
      friend class lazy_iterator<Opts>;

      // Factory method for lazy_json
      template <opts O, class Buffer>
      friend expected<lazy_document<O>, error_ctx> lazy_json(Buffer&&);

      // Helper to initialize root_view_ with correct doc_ pointer
      void init_root_view() noexcept { root_view_ = lazy_json_view<Opts>{this, root_data_}; }

     public:
      lazy_document() = default;

      // Copy constructor - fix doc_ pointer and preserve parse_pos_
      lazy_document(const lazy_document& other) : json_(other.json_), len_(other.len_), root_data_(other.root_data_)
      {
         init_root_view();
         root_view_.parse_pos_ = other.root_view_.parse_pos_;
      }

      lazy_document& operator=(const lazy_document& other)
      {
         if (this != &other) {
            json_ = other.json_;
            len_ = other.len_;
            root_data_ = other.root_data_;
            init_root_view();
            root_view_.parse_pos_ = other.root_view_.parse_pos_;
         }
         return *this;
      }

      // Move constructor - fix doc_ pointer and transfer parse_pos_
      lazy_document(lazy_document&& other) noexcept : json_(other.json_), len_(other.len_), root_data_(other.root_data_)
      {
         init_root_view();
         root_view_.parse_pos_ = other.root_view_.parse_pos_;
      }

      lazy_document& operator=(lazy_document&& other) noexcept
      {
         if (this != &other) {
            json_ = other.json_;
            len_ = other.len_;
            root_data_ = other.root_data_;
            init_root_view();
            root_view_.parse_pos_ = other.root_view_.parse_pos_;
         }
         return *this;
      }

      /// @brief Get the root view (cached, enables progressive key scanning)
      [[nodiscard]] lazy_json_view<Opts>& root() noexcept { return root_view_; }

      [[nodiscard]] const lazy_json_view<Opts>& root() const noexcept { return root_view_; }

      [[nodiscard]] lazy_json_view<Opts> operator[](std::string_view key) const { return root_view_[key]; }
      [[nodiscard]] lazy_json_view<Opts> operator[](size_t index) const { return root_view_[index]; }

      [[nodiscard]] bool is_null() const noexcept { return !root_data_ || *root_data_ == 'n'; }
      [[nodiscard]] bool is_array() const noexcept { return root_data_ && *root_data_ == '['; }
      [[nodiscard]] bool is_object() const noexcept { return root_data_ && *root_data_ == '{'; }

      explicit operator bool() const noexcept { return !is_null(); }

      [[nodiscard]] const char* json_data() const noexcept { return json_; }
      [[nodiscard]] size_t json_size() const noexcept { return len_; }

      /// @brief Reset parse position to beginning (for re-scanning from start)
      void reset_parse_pos() noexcept { root_view_.parse_pos_ = nullptr; }
   };

   // ============================================================================
   // lazy_iterator - Forward iterator with lazy scanning
   // ============================================================================

   template <opts Opts>
   class lazy_iterator
   {
     private:
      const lazy_document<Opts>* doc_{};
      const char* json_end_{};
      char close_char_{};
      bool is_object_{};
      bool at_end_{true};
      lazy_json_view<Opts> current_view_{}; // Stored view for parse_pos_ optimization

     public:
      using iterator_category = std::forward_iterator_tag;
      using value_type = lazy_json_view<Opts>;
      using difference_type = std::ptrdiff_t;
      using pointer = void;
      using reference = lazy_json_view<Opts>&;

      lazy_iterator() = default;

      lazy_iterator(const lazy_document<Opts>* doc, const char* container_start, const char* end, bool is_object);

      // Return reference to stored view - allows parse_pos_ optimization when user uses auto&
      reference operator*() { return current_view_; }
      const lazy_json_view<Opts>& operator*() const { return current_view_; }

      lazy_iterator& operator++();

      lazy_iterator operator++(int)
      {
         auto tmp = *this;
         ++*this;
         return tmp;
      }

      bool operator==(const lazy_iterator& other) const { return at_end_ == other.at_end_; }
      bool operator!=(const lazy_iterator& other) const { return !(*this == other); }

     private:
      void advance_to_next_element(const char*& pos);
      void skip_ws(const char*& p) noexcept
      {
         if constexpr (Opts.null_terminated) {
            while (whitespace_table[uint8_t(*p)]) ++p;
         }
         else {
            while (p < json_end_ && whitespace_table[uint8_t(*p)]) ++p;
         }
      }
   };

   // ============================================================================
   // indexed_lazy_view - Lazy view with pre-built element index for O(1) access
   // ============================================================================

   /**
    * @brief A lazy view with a pre-built index for O(1) iteration and random access.
    *
    * Created by calling `index()` on a lazy_json_view. Scans the container once
    * to build an index of element positions, then provides:
    * - O(1) iteration advancement (vs O(element_size) for lazy_json_view)
    * - O(1) random access by index
    * - O(1) size query
    *
    * Elements returned are still lazy_json_view objects, so nested access remains lazy.
    *
    * Memory: Base view + 8 bytes per element (+ 16 bytes per element for objects with keys)
    */
   template <opts Opts>
   struct indexed_lazy_view
   {
     private:
      const lazy_document<Opts>* doc_{};
      const char* json_end_{};
      std::vector<const char*> value_starts_;
      std::vector<std::string_view> keys_; // Only populated for objects
      bool is_object_{};

      friend class indexed_lazy_iterator<Opts>;

     public:
      indexed_lazy_view() = default;

      /// @brief Number of elements - O(1)
      [[nodiscard]] size_t size() const noexcept { return value_starts_.size(); }

      /// @brief Check if empty - O(1)
      [[nodiscard]] bool empty() const noexcept { return value_starts_.empty(); }

      /// @brief Check if this is an indexed object
      [[nodiscard]] bool is_object() const noexcept { return is_object_; }

      /// @brief Check if this is an indexed array
      [[nodiscard]] bool is_array() const noexcept { return !is_object_; }

      /// @brief O(1) random access by index
      [[nodiscard]] lazy_json_view<Opts> operator[](size_t index) const
      {
         if (index >= value_starts_.size()) {
            return lazy_json_view<Opts>::make_error(error_code::exceeded_static_array_size);
         }
         std::string_view key = is_object_ ? keys_[index] : std::string_view{};
         return lazy_json_view<Opts>{doc_, value_starts_[index], key};
      }

      /// @brief O(n) key lookup for objects (linear search in index)
      [[nodiscard]] lazy_json_view<Opts> operator[](std::string_view key) const
      {
         if (!is_object_) {
            return lazy_json_view<Opts>::make_error(error_code::get_wrong_type);
         }
         for (size_t i = 0; i < keys_.size(); ++i) {
            if (keys_[i] == key) {
               return lazy_json_view<Opts>{doc_, value_starts_[i], keys_[i]};
            }
         }
         return lazy_json_view<Opts>::make_error(error_code::key_not_found);
      }

      /// @brief Check if object contains key - O(n) linear search
      [[nodiscard]] bool contains(std::string_view key) const
      {
         if (!is_object_) return false;
         for (const auto& k : keys_) {
            if (k == key) return true;
         }
         return false;
      }

      [[nodiscard]] indexed_lazy_iterator<Opts> begin() const;
      [[nodiscard]] indexed_lazy_iterator<Opts> end() const;

     private:
      friend struct lazy_json_view<Opts>;

      // Private constructor used by lazy_json_view::index()
      indexed_lazy_view(const lazy_document<Opts>* doc, const char* json_end, bool is_object)
         : doc_(doc), json_end_(json_end), is_object_(is_object)
      {}

      void reserve(size_t n)
      {
         value_starts_.reserve(n);
         if (is_object_) {
            keys_.reserve(n);
         }
      }

      void add_element(const char* value_start, std::string_view key = {})
      {
         value_starts_.push_back(value_start);
         if (is_object_) {
            keys_.push_back(key);
         }
      }
   };

   // ============================================================================
   // indexed_lazy_iterator - O(1) advancement using pre-built index
   // ============================================================================

   template <opts Opts>
   class indexed_lazy_iterator
   {
     private:
      const indexed_lazy_view<Opts>* parent_{};
      size_t index_{};
      mutable lazy_json_view<Opts> current_view_{}; // Cached for reference return

     public:
      using iterator_category = std::random_access_iterator_tag;
      using value_type = lazy_json_view<Opts>;
      using difference_type = std::ptrdiff_t;
      using pointer = void;
      using reference = lazy_json_view<Opts>&;

      indexed_lazy_iterator() = default;
      indexed_lazy_iterator(const indexed_lazy_view<Opts>* parent, size_t index) : parent_(parent), index_(index) {}

      reference operator*() const
      {
         std::string_view key = parent_->is_object_ ? parent_->keys_[index_] : std::string_view{};
         current_view_ = lazy_json_view<Opts>{parent_->doc_, parent_->value_starts_[index_], key};
         return current_view_;
      }

      lazy_json_view<Opts>* operator->() const
      {
         operator*(); // Update current_view_
         return &current_view_;
      }

      indexed_lazy_iterator& operator++()
      {
         ++index_;
         return *this;
      }

      indexed_lazy_iterator operator++(int)
      {
         auto tmp = *this;
         ++index_;
         return tmp;
      }

      indexed_lazy_iterator& operator--()
      {
         --index_;
         return *this;
      }

      indexed_lazy_iterator operator--(int)
      {
         auto tmp = *this;
         --index_;
         return tmp;
      }

      indexed_lazy_iterator& operator+=(difference_type n)
      {
         index_ = static_cast<size_t>(static_cast<difference_type>(index_) + n);
         return *this;
      }

      indexed_lazy_iterator& operator-=(difference_type n)
      {
         index_ = static_cast<size_t>(static_cast<difference_type>(index_) - n);
         return *this;
      }

      indexed_lazy_iterator operator+(difference_type n) const
      {
         return {parent_, static_cast<size_t>(static_cast<difference_type>(index_) + n)};
      }

      indexed_lazy_iterator operator-(difference_type n) const
      {
         return {parent_, static_cast<size_t>(static_cast<difference_type>(index_) - n)};
      }

      difference_type operator-(const indexed_lazy_iterator& other) const
      {
         return static_cast<difference_type>(index_) - static_cast<difference_type>(other.index_);
      }

      reference operator[](difference_type n) const
      {
         std::string_view key =
            parent_->is_object_ ? parent_->keys_[index_ + static_cast<size_t>(n)] : std::string_view{};
         current_view_ =
            lazy_json_view<Opts>{parent_->doc_, parent_->value_starts_[index_ + static_cast<size_t>(n)], key};
         return current_view_;
      }

      bool operator==(const indexed_lazy_iterator& other) const { return index_ == other.index_; }
      bool operator!=(const indexed_lazy_iterator& other) const { return index_ != other.index_; }
      bool operator<(const indexed_lazy_iterator& other) const { return index_ < other.index_; }
      bool operator<=(const indexed_lazy_iterator& other) const { return index_ <= other.index_; }
      bool operator>(const indexed_lazy_iterator& other) const { return index_ > other.index_; }
      bool operator>=(const indexed_lazy_iterator& other) const { return index_ >= other.index_; }

      friend indexed_lazy_iterator operator+(difference_type n, const indexed_lazy_iterator& it) { return it + n; }
   };

   // ============================================================================
   // Implementation: lazy_json_view methods (truly lazy - no structural index)
   // ============================================================================

   template <opts Opts>
   inline const char* lazy_json_view<Opts>::json_end() const noexcept
   {
      return doc_ ? doc_->json_ + doc_->len_ : nullptr;
   }

   template <opts Opts>
   inline std::string_view lazy_json_view<Opts>::parse_key(const char*& p, const char* end) noexcept
   {
      if (*p != '"') return {};
      const char* const start = p;
      ++p; // skip opening quote
      const char* key_start = p;

      while (p < end) {
         const char* quote = static_cast<const char*>(std::memchr(p, '"', static_cast<size_t>(end - p)));
         if (!quote) [[unlikely]] {
            p = end;
            return std::string_view{key_start, static_cast<size_t>(end - key_start)};
         }

         // Count preceding backslashes to check if escaped
         size_t backslashes = 0;
         const char* check = quote - 1;
         while (check > start && *check == '\\') {
            ++backslashes;
            --check;
         }

         if ((backslashes & 1) == 0) {
            // Even backslashes = real quote
            std::string_view key{key_start, static_cast<size_t>(quote - key_start)};
            p = quote + 1; // skip closing quote
            return key;
         }
         // Odd backslashes = escaped quote, continue searching
         p = quote + 1;
      }
      return std::string_view{key_start, static_cast<size_t>(p - key_start)};
   }

   template <opts Opts>
   inline lazy_json_view<Opts> lazy_json_view<Opts>::operator[](size_t index) const
   {
      if (has_error()) return *this;
      if (!is_array()) return make_error(error_code::get_wrong_type);

      const char* end = json_end();
      const char* p = data_ + 1; // skip '['
      skip_ws(p, end);

      // Check for empty array
      if constexpr (Opts.null_terminated) {
         if (*p == ']') return make_error(error_code::exceeded_static_array_size);
      }
      else {
         if (p >= end || *p == ']') return make_error(error_code::exceeded_static_array_size);
      }

      // Skip 'index' elements using lazy scanning
      for (size_t i = 0; i < index; ++i) {
         p = detail::skip_value_lazy<Opts>(p, end);
         skip_ws(p, end);

         if constexpr (Opts.null_terminated) {
            if (*p == ']') return make_error(error_code::exceeded_static_array_size);
         }
         else {
            if (p >= end || *p == ']') return make_error(error_code::exceeded_static_array_size);
         }

         if (*p == ',') {
            ++p;
            skip_ws(p, end);
         }
      }

      return {doc_, p};
   }

   template <opts Opts>
   inline lazy_json_view<Opts> lazy_json_view<Opts>::operator[](std::string_view key) const
   {
      if (has_error()) return *this;
      if (!is_object()) return make_error(error_code::get_wrong_type);

      const char* end = json_end();
      const char* obj_start = data_ + 1; // skip '{'

      // Determine search start position
      // parse_pos_ points to the VALUE of the last found key (lazy skip)
      // We need to skip past it before continuing the search
      const char* search_start = obj_start;
      if (parse_pos_ && parse_pos_ > data_) {
         // Lazily skip past the last found value now
         const char* p = parse_pos_;
         p = detail::skip_value_lazy<Opts>(p, end);
         skip_ws(p, end);
         if (*p == ',') {
            ++p;
            skip_ws(p, end);
         }
         search_start = p;
      }

      const char* p = search_start;
      skip_ws(p, end);

      // Forward pass: search from current position to end of object
      while (true) {
         // Check for end of object
         if constexpr (Opts.null_terminated) {
            if (*p == '}') break;
         }
         else {
            if (p >= end || *p == '}') break;
         }

         // Parse key
         auto k = parse_key(p, end);
         skip_ws(p, end);

         // Skip ':'
         if (*p == ':') {
            ++p;
            skip_ws(p, end);
         }

         // Check if key matches
         if (k == key) {
            parse_pos_ = p; // Store value position (lazy - don't skip yet)
            return {doc_, p};
         }

         // Skip value using lazy scanning
         p = detail::skip_value_lazy<Opts>(p, end);
         skip_ws(p, end);

         // Skip comma
         if (*p == ',') {
            ++p;
            skip_ws(p, end);
         }
      }

      // Wrap-around pass: search from beginning to where we started
      if (search_start != obj_start) {
         p = obj_start;
         skip_ws(p, end);

         while (p < search_start) {
            // Check for end of object
            if constexpr (Opts.null_terminated) {
               if (*p == '}') break;
            }
            else {
               if (p >= end || *p == '}') break;
            }

            // Parse key
            auto k = parse_key(p, end);
            skip_ws(p, end);

            // Skip ':'
            if (*p == ':') {
               ++p;
               skip_ws(p, end);
            }

            // Check if key matches
            if (k == key) {
               parse_pos_ = p; // Store value position (lazy - don't skip yet)
               return {doc_, p};
            }

            // Skip value using lazy scanning
            p = detail::skip_value_lazy<Opts>(p, end);
            skip_ws(p, end);

            // Skip comma
            if (*p == ',') {
               ++p;
               skip_ws(p, end);
            }
         }
      }

      return make_error(error_code::key_not_found);
   }

   template <opts Opts>
   inline bool lazy_json_view<Opts>::contains(std::string_view key) const
   {
      auto result = (*this)[key];
      return !result.has_error();
   }

   template <opts Opts>
   inline size_t lazy_json_view<Opts>::size() const
   {
      if (has_error() || !data_) return 0;
      if (!is_array() && !is_object()) return 0;

      const char* end = json_end();
      const char* p = data_ + 1;
      skip_ws(p, end);

      const char close_char = is_array() ? ']' : '}';

      if constexpr (Opts.null_terminated) {
         if (*p == close_char) return 0;
      }
      else {
         if (p >= end || *p == close_char) return 0;
      }

      size_t count = 0;
      const bool is_obj = is_object();

      while (true) {
         if (is_obj) {
            // Skip key
            p = detail::skip_string_fast<Opts>(p, end);
            skip_ws(p, end);
            if (*p == ':') {
               ++p;
               skip_ws(p, end);
            }
         }

         // Skip value
         p = detail::skip_value_lazy<Opts>(p, end);
         ++count;
         skip_ws(p, end);

         if constexpr (Opts.null_terminated) {
            if (*p == close_char) return count;
         }
         else {
            if (p >= end || *p == close_char) return count;
         }

         if (*p == ',') {
            ++p;
            skip_ws(p, end);
         }
      }
   }

   template <opts Opts>
   inline bool lazy_json_view<Opts>::empty() const noexcept
   {
      if (has_error() || !data_) return true;
      if (is_null()) return true;
      if (!is_array() && !is_object()) return false;

      const char* end = json_end();
      const char* p = data_ + 1;
      skip_ws(p, end);

      const char close_char = is_array() ? ']' : '}';
      if constexpr (Opts.null_terminated) {
         return *p == close_char;
      }
      else {
         return p >= end || *p == close_char;
      }
   }

   // ============================================================================
   // lazy_iterator implementation (truly lazy)
   // ============================================================================

   template <opts Opts>
   inline lazy_iterator<Opts>::lazy_iterator(const lazy_document<Opts>* doc, const char* container_start,
                                             const char* end, bool is_object)
      : doc_(doc), json_end_(end), is_object_(is_object), at_end_(false)
   {
      close_char_ = is_object ? '}' : ']';
      const char* pos = container_start + 1; // skip '[' or '{'
      skip_ws(pos);

      // Check for empty container
      if constexpr (Opts.null_terminated) {
         if (*pos == close_char_) {
            at_end_ = true;
            return;
         }
      }
      else {
         if (pos >= json_end_ || *pos == close_char_) {
            at_end_ = true;
            return;
         }
      }

      // Position at first element and initialize current_view_
      advance_to_next_element(pos);
   }

   template <opts Opts>
   inline void lazy_iterator<Opts>::advance_to_next_element(const char*& pos)
   {
      std::string_view key{};
      if (is_object_) {
         // Parse key
         key = lazy_json_view<Opts>::parse_key(pos, json_end_);
         skip_ws(pos);

         // Skip ':'
         if (*pos == ':') {
            ++pos;
            skip_ws(pos);
         }
      }
      // Store key in current_view_ (will be set properly after this call)
      current_view_ = lazy_json_view<Opts>{doc_, pos, key};
   }

   template <opts Opts>
   inline lazy_iterator<Opts>& lazy_iterator<Opts>::operator++()
   {
      if (at_end_) return *this;

      const char* pos = current_view_.data();

      // Optimization: if user accessed fields via operator[], parse_pos_ tells us how far we scanned
      // We can skip from there instead of re-scanning the entire element
      // Note: check current_view_.is_object(), not is_object_ (which is about the container)
      if (current_view_.is_object() && current_view_.parse_pos_ && current_view_.parse_pos_ > pos) {
         // Skip the last accessed value, then scan to end of object
         pos = current_view_.parse_pos_;
         pos = detail::skip_value_lazy<Opts>(pos, json_end_);
         // Skip remaining content until we close this object (depth 1 -> 0)
         pos = detail::skip_to_depth_zero<Opts>(pos, json_end_, 1);
      }
      else {
         // No optimization possible, skip entire value
         pos = detail::skip_value_lazy<Opts>(pos, json_end_);
      }

      skip_ws(pos);

      // Check for end
      if constexpr (Opts.null_terminated) {
         if (*pos == close_char_) {
            at_end_ = true;
            return *this;
         }
      }
      else {
         if (pos >= json_end_ || *pos == close_char_) {
            at_end_ = true;
            return *this;
         }
      }

      // Skip comma
      if (*pos == ',') {
         ++pos;
         skip_ws(pos);
      }

      // Advance to next element (updates current_view_)
      advance_to_next_element(pos);

      return *this;
   }

   template <opts Opts>
   inline lazy_iterator<Opts> lazy_json_view<Opts>::begin() const
   {
      if (has_error() || !data_) return end();
      if (!is_array() && !is_object()) return end();
      return lazy_iterator<Opts>{doc_, data_, json_end(), is_object()};
   }

   template <opts Opts>
   inline lazy_iterator<Opts> lazy_json_view<Opts>::end() const
   {
      return lazy_iterator<Opts>{};
   }

   // ============================================================================
   // indexed_lazy_view implementation
   // ============================================================================

   template <opts Opts>
   inline indexed_lazy_iterator<Opts> indexed_lazy_view<Opts>::begin() const
   {
      return indexed_lazy_iterator<Opts>{this, 0};
   }

   template <opts Opts>
   inline indexed_lazy_iterator<Opts> indexed_lazy_view<Opts>::end() const
   {
      return indexed_lazy_iterator<Opts>{this, value_starts_.size()};
   }

   // ============================================================================
   // lazy_json_view::index() implementation
   // ============================================================================

   template <opts Opts>
   inline indexed_lazy_view<Opts> lazy_json_view<Opts>::index() const
   {
      // Return empty indexed view for non-containers or errors
      if (has_error() || !data_ || (!is_array() && !is_object())) {
         return indexed_lazy_view<Opts>{};
      }

      const char* end = json_end();
      indexed_lazy_view<Opts> result{doc_, end, is_object()};

      const char* p = data_ + 1; // skip '[' or '{'
      skip_ws(p, end);

      const char close_char = is_array() ? ']' : '}';
      const bool is_obj = is_object();

      // Check for empty container
      if constexpr (Opts.null_terminated) {
         if (*p == close_char) return result;
      }
      else {
         if (p >= end || *p == close_char) return result;
      }

      // Scan and record all element positions
      while (true) {
         std::string_view key{};
         if (is_obj) {
            // Parse and record key
            key = parse_key(p, end);
            skip_ws(p, end);

            // Skip ':'
            if (*p == ':') {
               ++p;
               skip_ws(p, end);
            }
         }

         // Record element start position
         result.add_element(p, key);

         // Skip value
         p = detail::skip_value_lazy<Opts>(p, end);
         skip_ws(p, end);

         // Check for end of container
         if constexpr (Opts.null_terminated) {
            if (*p == close_char) break;
         }
         else {
            if (p >= end || *p == close_char) break;
         }

         // Skip comma
         if (*p == ',') {
            ++p;
            skip_ws(p, end);
         }
      }

      return result;
   }

   // ============================================================================
   // lazy_json_view::get<T>() implementation
   // ============================================================================

   template <opts Opts>
   template <class T>
   [[nodiscard]] inline expected<T, error_ctx> lazy_json_view<Opts>::get() const
   {
      if (has_error()) {
         return unexpected(error_ctx{0, error_});
      }

      const char* end = json_end();

      if constexpr (std::is_same_v<T, bool>) {
         if (!is_boolean()) {
            return unexpected(error_ctx{0, error_code::get_wrong_type});
         }
         return *data_ == 't';
      }
      else if constexpr (std::is_same_v<T, std::nullptr_t>) {
         if (!is_null()) {
            return unexpected(error_ctx{0, error_code::get_wrong_type});
         }
         return nullptr;
      }
      else if constexpr (std::is_same_v<T, std::string>) {
         if (!is_string()) {
            return unexpected(error_ctx{0, error_code::get_wrong_type});
         }
         auto it = data_;
         context ctx{};
         skip_value<JSON>::op<opts{}>(ctx, it, end);
         if (bool(ctx.error)) {
            return unexpected(error_ctx{0, ctx.error});
         }
         std::string_view raw{data_, static_cast<size_t>(it - data_)};
         return glz::read_json<std::string>(raw);
      }
      else if constexpr (std::is_same_v<T, std::string_view>) {
         if (!is_string()) {
            return unexpected(error_ctx{0, error_code::get_wrong_type});
         }
         auto it = data_ + 1;
         context ctx{};
         skip_string_view(ctx, it, end);
         if (bool(ctx.error)) {
            return unexpected(error_ctx{0, ctx.error});
         }
         return std::string_view{data_ + 1, static_cast<size_t>(it - data_ - 1)};
      }
      else if constexpr (std::is_same_v<T, double>) {
         if (!is_number()) {
            return unexpected(error_ctx{0, error_code::get_wrong_type});
         }
         double value{};
         auto [ptr, ec] = glz::from_chars<false>(data_, end, value);
         if (ec != std::errc()) {
            return unexpected(error_ctx{0, error_code::parse_number_failure});
         }
         return value;
      }
      else if constexpr (std::is_same_v<T, float>) {
         if (!is_number()) {
            return unexpected(error_ctx{0, error_code::get_wrong_type});
         }
         float value{};
         auto [ptr, ec] = glz::from_chars<false>(data_, end, value);
         if (ec != std::errc()) {
            return unexpected(error_ctx{0, error_code::parse_number_failure});
         }
         return value;
      }
      else if constexpr (std::is_same_v<T, int64_t>) {
         if (!is_number()) {
            return unexpected(error_ctx{0, error_code::get_wrong_type});
         }
         int64_t value{};
         auto it = data_;
         if (!glz::atoi(value, it, end)) {
            return unexpected(error_ctx{0, error_code::parse_number_failure});
         }
         return value;
      }
      else if constexpr (std::is_same_v<T, uint64_t>) {
         if (!is_number()) {
            return unexpected(error_ctx{0, error_code::get_wrong_type});
         }
         uint64_t value{};
         auto it = data_;
         if (!glz::atoi(value, it, end)) {
            return unexpected(error_ctx{0, error_code::parse_number_failure});
         }
         return value;
      }
      else if constexpr (std::is_same_v<T, int32_t>) {
         auto result = get<int64_t>();
         if (!result) return unexpected(result.error());
         return static_cast<int32_t>(*result);
      }
      else if constexpr (std::is_same_v<T, uint32_t>) {
         auto result = get<uint64_t>();
         if (!result) return unexpected(result.error());
         return static_cast<uint32_t>(*result);
      }
      else {
         static_assert(false_v<T>, "Unsupported type for lazy_json_view::get<T>()");
      }
   }

   // ============================================================================
   // JSON writer for lazy_json_view
   // ============================================================================

   template <opts Opts>
   struct to<JSON, lazy_json_view<Opts>>
   {
      template <auto WriteOpts, class B>
      GLZ_ALWAYS_INLINE static void op(const lazy_json_view<Opts>& view, is_context auto&& ctx, B&& b, auto& ix)
      {
         if (view.has_error()) {
            ctx.error = view.error();
            return;
         }
         if (!view.data()) {
            dump<false>("null", b, ix);
            return;
         }

         auto it = view.data();
         context parse_ctx{};
         skip_value<JSON>::op<opts{}>(parse_ctx, it, view.json_end());
         if (bool(parse_ctx.error)) [[unlikely]] {
            ctx.error = parse_ctx.error;
            return;
         }

         const size_t n = static_cast<size_t>(it - view.data());
         if constexpr (resizable<B>) {
            if (ix + n > b.size()) [[unlikely]] {
               b.resize((std::max)(b.size() * 2, ix + n));
            }
         }
         else {
            if (ix + n > b.size()) [[unlikely]] {
               ctx.error = error_code::buffer_overflow;
               return;
            }
         }
         std::memcpy(&b[ix], view.data(), n);
         ix += n;
      }
   };

   // ============================================================================
   // lazy_json - Main entry point (truly lazy - minimal upfront work)
   // ============================================================================

   /**
    * @brief Create a lazy JSON document - no upfront processing.
    *
    * - Just validates first byte and stores buffer reference - O(1)
    * - All work happens on-demand when accessing fields
    *
    * @tparam Opts Options for parsing
    * @param buffer The JSON text buffer (must remain valid for document lifetime)
    * @return lazy_document on success, error_ctx on failure
    */
   template <opts Opts = opts{}, class Buffer>
   [[nodiscard]] inline expected<lazy_document<Opts>, error_ctx> lazy_json(Buffer&& buffer)
   {
      lazy_document<Opts> doc;
      doc.json_ = buffer.data();
      doc.len_ = buffer.size();

      // Find root value (skip leading whitespace)
      const char* p = buffer.data();
      const char* end = buffer.data() + buffer.size();

      if constexpr (Opts.null_terminated) {
         while (whitespace_table[uint8_t(*p)]) ++p;
      }
      else {
         while (p < end && whitespace_table[uint8_t(*p)]) ++p;
      }

      if (p >= end) {
         return unexpected(error_ctx{0, error_code::unexpected_end});
      }

      // Validate first character is valid JSON start
      const char c = *p;
      if (c != '{' && c != '[' && c != '"' && c != 't' && c != 'f' && c != 'n' && !is_digit(uint8_t(c)) && c != '-') {
         return unexpected(error_ctx{0, error_code::syntax_error});
      }

      doc.root_data_ = p;
      doc.init_root_view(); // Initialize cached root view
      return doc;
   }

   // ============================================================================
   // read_json overload for lazy_json_view (single-pass deserialization)
   // ============================================================================

   /// @brief Read JSON from a lazy_json_view into a C++ type
   /// @tparam T The type to parse into
   /// @tparam Opts The lazy view options
   /// @param value The object to populate
   /// @param view The lazy view to read from
   /// @return Error context if parsing failed
   /// @note This provides the familiar glz::read_json API while using the efficient single-pass read_into internally
   template <class T, opts Opts>
   [[nodiscard]] inline error_ctx read_json(T& value, const lazy_json_view<Opts>& view)
   {
      return view.template read_into<T>(value);
   }

   /// @brief Read JSON from a lazy_json_view rvalue into a C++ type
   /// @note Handles temporaries like glz::read_json(value, doc["field"])
   template <class T, opts Opts>
   [[nodiscard]] inline error_ctx read_json(T& value, lazy_json_view<Opts>&& view)
   {
      return view.template read_into<T>(value);
   }

} // namespace glz
