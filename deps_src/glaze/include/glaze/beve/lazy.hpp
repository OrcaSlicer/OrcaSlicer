// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <string_view>

#include "glaze/beve/header.hpp"
#include "glaze/beve/read.hpp"
#include "glaze/beve/skip.hpp"
#include "glaze/beve/write.hpp"
#include "glaze/util/expected.hpp"

namespace glz
{
   // Forward declarations
   template <opts Opts>
   struct lazy_beve_document;
   template <opts Opts>
   class lazy_beve_iterator;
   template <opts Opts>
   struct indexed_lazy_beve_view;
   template <opts Opts>
   class indexed_lazy_beve_iterator;

   // ============================================================================
   // Helper functions for BEVE lazy parsing
   // ============================================================================

   namespace detail
   {
      // Skip a BEVE value and return the new position (no context needed for position tracking)
      template <opts Opts>
      GLZ_ALWAYS_INLINE const char* skip_value_beve_lazy(const char* p, const char* end) noexcept
      {
         if (p >= end) return p;

         context ctx{};
         auto it = p;
         skip_value<BEVE>::op<Opts>(ctx, it, end);
         return it;
      }

      // Read compressed integer without modifying iterator, returns {value, bytes_consumed}
      GLZ_ALWAYS_INLINE std::pair<size_t, size_t> peek_compressed_int(const char* p, const char* end) noexcept
      {
         if (p >= end) return {0, 0};

         uint8_t header;
         std::memcpy(&header, p, 1);
         const uint8_t config = header & 0b000000'11;

         switch (config) {
         case 0:
            return {header >> 2, 1};
         case 1: {
            if (p + 2 > end) return {0, 0};
            uint16_t h;
            std::memcpy(&h, p, 2);
            if constexpr (std::endian::native == std::endian::big) {
               h = std::byteswap(h);
            }
            return {h >> 2, 2};
         }
         case 2: {
            if (p + 4 > end) return {0, 0};
            uint32_t h;
            std::memcpy(&h, p, 4);
            if constexpr (std::endian::native == std::endian::big) {
               h = std::byteswap(h);
            }
            return {h >> 2, 4};
         }
         case 3: {
            if constexpr (sizeof(size_t) > sizeof(uint32_t)) {
               if (p + 8 > end) return {0, 0};
               uint64_t h;
               std::memcpy(&h, p, 8);
               if constexpr (std::endian::native == std::endian::big) {
                  h = std::byteswap(h);
               }
               return {static_cast<size_t>(h >> 2), 8};
            }
            else {
               return {0, 0};
            }
         }
         default:
            return {0, 0};
         }
      }

      // Read compressed integer and advance pointer
      GLZ_ALWAYS_INLINE size_t read_compressed_int(const char*& p, const char* end) noexcept
      {
         auto [value, bytes] = peek_compressed_int(p, end);
         p += bytes;
         return value;
      }

      // Skip a BEVE string (tag already consumed) - returns position after string
      GLZ_ALWAYS_INLINE const char* skip_string_content(const char* p, const char* end) noexcept
      {
         const auto len = read_compressed_int(p, end);
         if (static_cast<size_t>(end - p) < len) return end;
         return p + len;
      }

      // Get size of a BEVE number based on tag
      GLZ_ALWAYS_INLINE size_t number_size_from_tag(uint8_t t) noexcept { return byte_count_lookup[t >> 5]; }
   } // namespace detail

   // ============================================================================
   // lazy_beve_view - Truly lazy view into BEVE data
   // ============================================================================

   /**
    * @brief A truly lazy view into BEVE data.
    *
    * No upfront processing - all navigation uses on-demand scanning.
    *
    * For objects, parse_pos_ tracks the current scan position to enable
    * efficient sequential key access (O(n) total instead of O(n²)).
    */
   template <opts Opts = opts{}>
   struct lazy_beve_view
   {
     private:
      const lazy_beve_document<Opts>* doc_{};
      const char* data_{};
      mutable const char* parse_pos_{}; // Current scan position (advances on key access)
      std::string_view key_{};
      error_code error_{error_code::none};
      uint8_t synthetic_tag_{}; // Non-zero for typed array elements (tag info without tag byte in data)

      lazy_beve_view(error_code ec) noexcept : error_(ec) {}

     public:
      lazy_beve_view() = default;
      lazy_beve_view(const lazy_beve_document<Opts>* doc, const char* data) noexcept : doc_(doc), data_(data) {}

      [[nodiscard]] static lazy_beve_view make_error(error_code ec) noexcept { return lazy_beve_view{ec}; }

      [[nodiscard]] bool has_error() const noexcept { return error_ != error_code::none; }
      [[nodiscard]] error_code error() const noexcept { return error_; }

      // Type checking - direct from BEVE tag byte
      [[nodiscard]] bool is_null() const noexcept
      {
         if (has_error() || !data_) return true;
         const uint8_t t = uint8_t(*data_);
         // null tag is 0, but boolean also uses low bits differently
         // null: tag == 0
         return t == tag::null;
      }

      [[nodiscard]] bool is_boolean() const noexcept
      {
         if (has_error() || !data_) return false;
         const uint8_t t = uint8_t(*data_);
         // boolean: (tag & 0b0000'1111) == tag::boolean (which is 0b00001'000)
         return (t & 0b0000'1111) == tag::boolean;
      }

      [[nodiscard]] bool is_number() const noexcept
      {
         if (has_error() || !data_) return false;
         const uint8_t t = uint8_t(*data_) & 0b00000'111;
         return t == tag::number;
      }

      [[nodiscard]] bool is_string() const noexcept
      {
         if (has_error() || !data_) return false;
         const uint8_t t = uint8_t(*data_) & 0b00000'111;
         return t == tag::string;
      }

      [[nodiscard]] bool is_object() const noexcept
      {
         if (has_error() || !data_) return false;
         const uint8_t t = uint8_t(*data_) & 0b00000'111;
         return t == tag::object;
      }

      [[nodiscard]] bool is_array() const noexcept
      {
         if (has_error() || !data_) return false;
         const uint8_t t = uint8_t(*data_) & 0b00000'111;
         return t == tag::typed_array || t == tag::generic_array;
      }

      [[nodiscard]] bool is_typed_array() const noexcept
      {
         if (has_error() || !data_) return false;
         const uint8_t t = uint8_t(*data_) & 0b00000'111;
         return t == tag::typed_array;
      }

      [[nodiscard]] bool is_generic_array() const noexcept
      {
         if (has_error() || !data_) return false;
         const uint8_t t = uint8_t(*data_) & 0b00000'111;
         return t == tag::generic_array;
      }

      explicit operator bool() const noexcept { return !has_error() && data_ && !is_null(); }

      [[nodiscard]] const char* data() const noexcept { return data_; }
      [[nodiscard]] const char* beve_end() const noexcept;

      /// @brief Get the raw BEVE bytes for this value
      [[nodiscard]] std::string_view raw_beve() const noexcept
      {
         if (has_error() || !data_) return {};
         const char* end_ptr = detail::skip_value_beve_lazy<Opts>(data_, beve_end());
         return {data_, static_cast<size_t>(end_ptr - data_)};
      }

      /// @brief Parse this value directly into a C++ type
      template <class T>
      [[nodiscard]] error_ctx read_into(T& value) const
      {
         if (has_error()) {
            return error_ctx{0, error_};
         }
         if (!data_) {
            return error_ctx{0, error_code::unexpected_end};
         }
         context ctx{};
         auto it = data_;
         auto end = beve_end();
         parse<BEVE>::op<Opts>(value, ctx, it, end);
         if (bool(ctx.error)) {
            return error_ctx{static_cast<size_t>(it - data_), ctx.error};
         }
         return {};
      }

      template <class T>
      [[nodiscard]] expected<T, error_ctx> get() const;

      [[nodiscard]] lazy_beve_view operator[](size_t index) const;
      [[nodiscard]] lazy_beve_view operator[](std::string_view key) const;
      [[nodiscard]] bool contains(std::string_view key) const;
      [[nodiscard]] size_t size() const;
      [[nodiscard]] bool empty() const noexcept;

      // Key access for object iteration
      [[nodiscard]] std::string_view key() const noexcept { return key_; }

      [[nodiscard]] lazy_beve_iterator<Opts> begin() const;
      [[nodiscard]] lazy_beve_iterator<Opts> end() const;

      /// @brief Build an index for O(1) iteration and random access
      [[nodiscard]] indexed_lazy_beve_view<Opts> index() const;

     private:
      friend struct lazy_beve_document<Opts>;
      friend class lazy_beve_iterator<Opts>;
      friend struct indexed_lazy_beve_view<Opts>;
      friend class indexed_lazy_beve_iterator<Opts>;

      // Constructor with key for iteration
      lazy_beve_view(const lazy_beve_document<Opts>* doc, const char* data, std::string_view key) noexcept
         : doc_(doc), data_(data), key_(key)
      {}

      // Constructor with synthetic tag for typed array elements
      lazy_beve_view(const lazy_beve_document<Opts>* doc, const char* data, std::string_view key,
                     uint8_t synthetic_tag) noexcept
         : doc_(doc), data_(data), key_(key), synthetic_tag_(synthetic_tag)
      {}

      void set_synthetic_tag(uint8_t tag) noexcept { synthetic_tag_ = tag; }

      // Helper to read numeric value directly given tag info (for typed array elements)
      template <class T>
      [[nodiscard]] expected<T, error_ctx> read_numeric_from_tag(uint8_t tag, const char* value_ptr,
                                                                 const char* end) const;
   };

   // ============================================================================
   // lazy_beve_document - Minimal container
   // ============================================================================

   template <opts Opts = opts{}>
   struct lazy_beve_document
   {
     private:
      const char* beve_{};
      size_t len_{};
      const char* root_data_{};
      mutable lazy_beve_view<Opts> root_view_{};

      friend struct lazy_beve_view<Opts>;
      friend class lazy_beve_iterator<Opts>;

      template <opts O, class Buffer>
      friend expected<lazy_beve_document<O>, error_ctx> lazy_beve(Buffer&&);

      void init_root_view() noexcept { root_view_ = lazy_beve_view<Opts>{this, root_data_}; }

     public:
      lazy_beve_document() = default;

      lazy_beve_document(const lazy_beve_document& other)
         : beve_(other.beve_), len_(other.len_), root_data_(other.root_data_), size{this}
      {
         init_root_view();
         root_view_.parse_pos_ = other.root_view_.parse_pos_;
      }

      lazy_beve_document& operator=(const lazy_beve_document& other)
      {
         if (this != &other) {
            beve_ = other.beve_;
            len_ = other.len_;
            root_data_ = other.root_data_;
            init_root_view();
            root_view_.parse_pos_ = other.root_view_.parse_pos_;
            size = {this};
         }
         return *this;
      }

      lazy_beve_document(lazy_beve_document&& other) noexcept
         : beve_(other.beve_), len_(other.len_), root_data_(other.root_data_), size{this}
      {
         init_root_view();
         root_view_.parse_pos_ = other.root_view_.parse_pos_;
      }

      lazy_beve_document& operator=(lazy_beve_document&& other) noexcept
      {
         if (this != &other) {
            beve_ = other.beve_;
            len_ = other.len_;
            root_data_ = other.root_data_;
            init_root_view();
            root_view_.parse_pos_ = other.root_view_.parse_pos_;
            size = {this};
         }
         return *this;
      }

      [[nodiscard]] lazy_beve_view<Opts>& root() noexcept { return root_view_; }
      [[nodiscard]] const lazy_beve_view<Opts>& root() const noexcept { return root_view_; }

      [[nodiscard]] lazy_beve_view<Opts> operator[](std::string_view key) const { return root_view_[key]; }
      [[nodiscard]] lazy_beve_view<Opts> operator[](size_t index) const { return root_view_[index]; }

      [[nodiscard]] bool is_null() const noexcept { return !root_data_ || uint8_t(*root_data_) == tag::null; }

      [[nodiscard]] bool is_array() const noexcept
      {
         if (!root_data_) return false;
         const uint8_t t = uint8_t(*root_data_) & 0b00000'111;
         return t == tag::typed_array || t == tag::generic_array;
      }

      [[nodiscard]] bool is_object() const noexcept
      {
         if (!root_data_) return false;
         const uint8_t t = uint8_t(*root_data_) & 0b00000'111;
         return t == tag::object;
      }

      explicit operator bool() const noexcept { return !is_null(); }

      [[nodiscard]] const char* beve_data() const noexcept { return beve_; }
      [[nodiscard]] size_t beve_size() const noexcept { return len_; }

      void reset_parse_pos() noexcept { root_view_.parse_pos_ = nullptr; }

      /// @brief Size accessor proxy for getting value sizes without parsing
      /// Usage: doc.size["key"] returns the string length or element count
      struct size_accessor
      {
         const lazy_beve_document* doc;

         [[nodiscard]] size_t operator[](std::string_view key) const { return doc->root_view_[key].size(); }

         [[nodiscard]] size_t operator[](size_t index) const { return doc->root_view_[index].size(); }
      };

      size_accessor size{this};
   };

   // ============================================================================
   // lazy_beve_iterator - Forward iterator with lazy scanning
   // ============================================================================

   template <opts Opts>
   class lazy_beve_iterator
   {
     private:
      const lazy_beve_document<Opts>* doc_{};
      const char* beve_end_{};
      const char* current_pos_{}; // Current position in iteration
      size_t remaining_count_{}; // Elements remaining
      bool is_object_{};
      bool is_typed_array_{};
      bool has_string_keys_{true}; // For objects: string keys (default) vs number keys
      uint8_t key_byte_count_{}; // For number-keyed objects: bytes per key
      uint8_t element_size_{}; // For typed arrays
      bool at_end_{true};
      lazy_beve_view<Opts> current_view_{};

     public:
      using iterator_category = std::forward_iterator_tag;
      using value_type = lazy_beve_view<Opts>;
      using difference_type = std::ptrdiff_t;
      using pointer = void;
      using reference = lazy_beve_view<Opts>&;

      lazy_beve_iterator() = default;

      lazy_beve_iterator(const lazy_beve_document<Opts>* doc, const char* container_start, const char* end,
                         bool is_object, bool is_typed_array);

      reference operator*() { return current_view_; }
      const lazy_beve_view<Opts>& operator*() const { return current_view_; }

      lazy_beve_iterator& operator++();

      lazy_beve_iterator operator++(int)
      {
         auto tmp = *this;
         ++*this;
         return tmp;
      }

      bool operator==(const lazy_beve_iterator& other) const { return at_end_ == other.at_end_; }
      bool operator!=(const lazy_beve_iterator& other) const { return !(*this == other); }

     private:
      void advance_to_current_element();
   };

   // ============================================================================
   // indexed_lazy_beve_view - Pre-built index for O(1) access
   // ============================================================================

   template <opts Opts>
   struct indexed_lazy_beve_view
   {
     private:
      const lazy_beve_document<Opts>* doc_{};
      const char* beve_end_{};
      std::vector<const char*> value_starts_;
      std::vector<std::string_view> keys_;
      bool is_object_{};
      bool is_typed_array_{}; // True if indexing a typed array
      uint8_t element_tag_{}; // Synthetic element tag for typed arrays

      friend class indexed_lazy_beve_iterator<Opts>;

     public:
      indexed_lazy_beve_view() = default;

      [[nodiscard]] size_t size() const noexcept { return value_starts_.size(); }
      [[nodiscard]] bool empty() const noexcept { return value_starts_.empty(); }
      [[nodiscard]] bool is_object() const noexcept { return is_object_; }
      [[nodiscard]] bool is_array() const noexcept { return !is_object_; }
      [[nodiscard]] bool is_typed_array() const noexcept { return is_typed_array_; }

      [[nodiscard]] lazy_beve_view<Opts> operator[](size_t index) const
      {
         if (index >= value_starts_.size()) {
            return lazy_beve_view<Opts>::make_error(error_code::exceeded_static_array_size);
         }
         std::string_view key = is_object_ ? keys_[index] : std::string_view{};
         return lazy_beve_view<Opts>{doc_, value_starts_[index], key, element_tag_};
      }

      [[nodiscard]] lazy_beve_view<Opts> operator[](std::string_view key) const
      {
         if (!is_object_) {
            return lazy_beve_view<Opts>::make_error(error_code::get_wrong_type);
         }
         for (size_t i = 0; i < keys_.size(); ++i) {
            if (keys_[i] == key) {
               return lazy_beve_view<Opts>{doc_, value_starts_[i], keys_[i]};
            }
         }
         return lazy_beve_view<Opts>::make_error(error_code::key_not_found);
      }

      [[nodiscard]] bool contains(std::string_view key) const
      {
         if (!is_object_) return false;
         for (const auto& k : keys_) {
            if (k == key) return true;
         }
         return false;
      }

      [[nodiscard]] indexed_lazy_beve_iterator<Opts> begin() const;
      [[nodiscard]] indexed_lazy_beve_iterator<Opts> end() const;

     private:
      friend struct lazy_beve_view<Opts>;

      indexed_lazy_beve_view(const lazy_beve_document<Opts>* doc, const char* beve_end, bool is_object)
         : doc_(doc), beve_end_(beve_end), is_object_(is_object)
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
   // indexed_lazy_beve_iterator - O(1) advancement
   // ============================================================================

   template <opts Opts>
   class indexed_lazy_beve_iterator
   {
     private:
      const indexed_lazy_beve_view<Opts>* parent_{};
      size_t index_{};
      mutable lazy_beve_view<Opts> current_view_{};

     public:
      using iterator_category = std::random_access_iterator_tag;
      using value_type = lazy_beve_view<Opts>;
      using difference_type = std::ptrdiff_t;
      using pointer = void;
      using reference = lazy_beve_view<Opts>&;

      indexed_lazy_beve_iterator() = default;
      indexed_lazy_beve_iterator(const indexed_lazy_beve_view<Opts>* parent, size_t index)
         : parent_(parent), index_(index)
      {}

      reference operator*() const
      {
         std::string_view key = parent_->is_object_ ? parent_->keys_[index_] : std::string_view{};
         current_view_ =
            lazy_beve_view<Opts>{parent_->doc_, parent_->value_starts_[index_], key, parent_->element_tag_};
         return current_view_;
      }

      lazy_beve_view<Opts>* operator->() const
      {
         operator*();
         return &current_view_;
      }

      indexed_lazy_beve_iterator& operator++()
      {
         ++index_;
         return *this;
      }

      indexed_lazy_beve_iterator operator++(int)
      {
         auto tmp = *this;
         ++index_;
         return tmp;
      }

      indexed_lazy_beve_iterator& operator--()
      {
         --index_;
         return *this;
      }

      indexed_lazy_beve_iterator operator--(int)
      {
         auto tmp = *this;
         --index_;
         return tmp;
      }

      indexed_lazy_beve_iterator& operator+=(difference_type n)
      {
         index_ = static_cast<size_t>(static_cast<difference_type>(index_) + n);
         return *this;
      }

      indexed_lazy_beve_iterator& operator-=(difference_type n)
      {
         index_ = static_cast<size_t>(static_cast<difference_type>(index_) - n);
         return *this;
      }

      indexed_lazy_beve_iterator operator+(difference_type n) const
      {
         return {parent_, static_cast<size_t>(static_cast<difference_type>(index_) + n)};
      }

      indexed_lazy_beve_iterator operator-(difference_type n) const
      {
         return {parent_, static_cast<size_t>(static_cast<difference_type>(index_) - n)};
      }

      difference_type operator-(const indexed_lazy_beve_iterator& other) const
      {
         return static_cast<difference_type>(index_) - static_cast<difference_type>(other.index_);
      }

      reference operator[](difference_type n) const
      {
         std::string_view key =
            parent_->is_object_ ? parent_->keys_[index_ + static_cast<size_t>(n)] : std::string_view{};
         current_view_ =
            lazy_beve_view<Opts>{parent_->doc_, parent_->value_starts_[index_ + static_cast<size_t>(n)], key};
         return current_view_;
      }

      bool operator==(const indexed_lazy_beve_iterator& other) const { return index_ == other.index_; }
      bool operator!=(const indexed_lazy_beve_iterator& other) const { return index_ != other.index_; }
      bool operator<(const indexed_lazy_beve_iterator& other) const { return index_ < other.index_; }
      bool operator<=(const indexed_lazy_beve_iterator& other) const { return index_ <= other.index_; }
      bool operator>(const indexed_lazy_beve_iterator& other) const { return index_ > other.index_; }
      bool operator>=(const indexed_lazy_beve_iterator& other) const { return index_ >= other.index_; }

      friend indexed_lazy_beve_iterator operator+(difference_type n, const indexed_lazy_beve_iterator& it)
      {
         return it + n;
      }
   };

   // ============================================================================
   // Implementation: lazy_beve_view methods
   // ============================================================================

   template <opts Opts>
   inline const char* lazy_beve_view<Opts>::beve_end() const noexcept
   {
      return doc_ ? doc_->beve_ + doc_->len_ : nullptr;
   }

   template <opts Opts>
   inline lazy_beve_view<Opts> lazy_beve_view<Opts>::operator[](size_t index) const
   {
      if (has_error()) return *this;
      if (!is_array()) return make_error(error_code::get_wrong_type);

      const char* end = beve_end();
      const char* p = data_;
      const uint8_t t = uint8_t(*p);
      const uint8_t type_bits = t & 0b00000'111;
      ++p;

      const auto count = detail::read_compressed_int(p, end);
      if (index >= count) {
         return make_error(error_code::exceeded_static_array_size);
      }

      if (type_bits == tag::generic_array) {
         // Generic array - each element has its own tag
         for (size_t i = 0; i < index; ++i) {
            p = detail::skip_value_beve_lazy<Opts>(p, end);
         }
         return {doc_, p};
      }
      else {
         // Typed array - homogeneous elements
         const uint8_t element_type = (t & 0b000'11'000) >> 3;

         if (element_type == 3) {
            // Bool or string array
            const bool is_string = (t & 0b00'1'00'000) >> 5;
            if (is_string) {
               // String array - variable length elements
               for (size_t i = 0; i < index; ++i) {
                  const auto len = detail::read_compressed_int(p, end);
                  p += len;
               }
               // Return view with synthetic string tag
               return {doc_, p, {}, tag::string};
            }
            else {
               // Boolean array - packed bits, can't easily index
               // For now, return error for direct indexing of bool arrays
               return make_error(error_code::get_wrong_type);
            }
         }
         else {
            // Numeric typed array - fixed size elements
            const size_t elem_size = byte_count_lookup[t >> 5];
            p += index * elem_size;
            // Return view with synthetic numeric tag
            const uint8_t synthetic_tag = tag::number | (t & 0b11111000);
            return {doc_, p, {}, synthetic_tag};
         }
      }
   }

   template <opts Opts>
   inline lazy_beve_view<Opts> lazy_beve_view<Opts>::operator[](std::string_view key) const
   {
      if (has_error()) return *this;
      if (!is_object()) return make_error(error_code::get_wrong_type);

      const char* end = beve_end();
      const char* p = data_;
      const uint8_t t = uint8_t(*p);
      ++p;

      // Check for string-keyed object (key_type bits should be 0 for string keys)
      const uint8_t key_type = t & 0b000'11'000;
      if (key_type != 0) {
         // Number-keyed object - string lookup not supported
         return make_error(error_code::get_wrong_type);
      }

      const auto n_keys = detail::read_compressed_int(p, end);

      // Determine search start position for progressive scanning
      const char* search_start = p;
      size_t start_index = 0;

      if (parse_pos_ && parse_pos_ > data_) {
         // Skip past the last found value
         const char* skip_from = parse_pos_;
         skip_from = detail::skip_value_beve_lazy<Opts>(skip_from, end);
         search_start = skip_from;

         // Count how many keys we've passed
         const char* counter = p;
         while (counter < search_start && start_index < n_keys) {
            const auto key_len = detail::read_compressed_int(counter, end);
            counter += key_len;
            counter = detail::skip_value_beve_lazy<Opts>(counter, end);
            ++start_index;
         }
      }

      // Forward pass: search from current position
      const char* iter = search_start;
      for (size_t i = start_index; i < n_keys; ++i) {
         const auto key_len = detail::read_compressed_int(iter, end);
         std::string_view current_key{iter, key_len};
         iter += key_len;

         if (current_key == key) {
            parse_pos_ = iter;
            return {doc_, iter};
         }

         iter = detail::skip_value_beve_lazy<Opts>(iter, end);
      }

      // Wrap-around pass: search from beginning to where we started
      if (start_index > 0) {
         iter = p;
         for (size_t i = 0; i < start_index; ++i) {
            const auto key_len = detail::read_compressed_int(iter, end);
            std::string_view current_key{iter, key_len};
            iter += key_len;

            if (current_key == key) {
               parse_pos_ = iter;
               return {doc_, iter};
            }

            iter = detail::skip_value_beve_lazy<Opts>(iter, end);
         }
      }

      return make_error(error_code::key_not_found);
   }

   template <opts Opts>
   inline bool lazy_beve_view<Opts>::contains(std::string_view key) const
   {
      auto result = (*this)[key];
      return !result.has_error();
   }

   template <opts Opts>
   inline size_t lazy_beve_view<Opts>::size() const
   {
      if (has_error() || !data_) return 0;

      // Handle synthetic tags (typed array elements)
      if (synthetic_tag_ != 0) {
         const uint8_t base_type = synthetic_tag_ & 0b00000'111;
         if (base_type == tag::string) {
            // String element in typed string array - length prefixed
            auto [len, bytes] = detail::peek_compressed_int(data_, beve_end());
            return len;
         }
         return 0;
      }

      const uint8_t t = uint8_t(*data_) & 0b00000'111;

      if (t == tag::string) {
         const char* p = data_ + 1; // Skip tag
         auto [len, bytes] = detail::peek_compressed_int(p, beve_end());
         return len;
      }

      if (t == tag::object || t == tag::typed_array || t == tag::generic_array) {
         const char* p = data_ + 1;
         return detail::read_compressed_int(p, beve_end());
      }

      return 0;
   }

   template <opts Opts>
   inline bool lazy_beve_view<Opts>::empty() const noexcept
   {
      if (has_error() || !data_) return true;
      if (is_null()) return true;
      if (!is_array() && !is_object()) return false;

      const char* p = data_ + 1;
      const auto count = detail::read_compressed_int(p, beve_end());
      return count == 0;
   }

   // ============================================================================
   // lazy_beve_iterator implementation
   // ============================================================================

   template <opts Opts>
   inline lazy_beve_iterator<Opts>::lazy_beve_iterator(const lazy_beve_document<Opts>* doc, const char* container_start,
                                                       const char* end, bool is_object, bool is_typed_array)
      : doc_(doc), beve_end_(end), is_object_(is_object), is_typed_array_(is_typed_array), at_end_(false)
   {
      const char* p = container_start;
      const uint8_t t = uint8_t(*p);
      ++p;

      remaining_count_ = detail::read_compressed_int(p, end);

      if (remaining_count_ == 0) {
         at_end_ = true;
         return;
      }

      if (is_object_) {
         // Check key type from bits 3-4 of tag
         const uint8_t key_type_bits = t & 0b000'11'000;
         has_string_keys_ = (key_type_bits == 0);
         if (!has_string_keys_) {
            key_byte_count_ = static_cast<uint8_t>(byte_count_lookup[t >> 5]);
         }
      }

      if (is_typed_array_) {
         // For typed arrays, calculate element size
         const uint8_t element_type = (t & 0b000'11'000) >> 3;
         if (element_type < 3) {
            element_size_ = static_cast<uint8_t>(byte_count_lookup[t >> 5]);
         }
         else {
            // Bool or string array - variable handling needed
            element_size_ = 0;
         }
      }

      current_pos_ = p;
      advance_to_current_element();
   }

   template <opts Opts>
   inline void lazy_beve_iterator<Opts>::advance_to_current_element()
   {
      std::string_view key{};

      if (is_object_) {
         if (has_string_keys_) {
            // String key: length prefix + string data
            const auto key_len = detail::read_compressed_int(current_pos_, beve_end_);
            key = std::string_view{current_pos_, key_len};
            current_pos_ += key_len;
         }
         else {
            // Number key: just raw bytes (no key view for number keys)
            current_pos_ += key_byte_count_;
         }
      }

      current_view_ = lazy_beve_view<Opts>{doc_, current_pos_, key};
   }

   template <opts Opts>
   inline lazy_beve_iterator<Opts>& lazy_beve_iterator<Opts>::operator++()
   {
      if (at_end_) return *this;

      --remaining_count_;
      if (remaining_count_ == 0) {
         at_end_ = true;
         return *this;
      }

      // Skip current value
      if (is_typed_array_ && element_size_ > 0) {
         current_pos_ += element_size_;
      }
      else {
         current_pos_ = detail::skip_value_beve_lazy<Opts>(current_pos_, beve_end_);
      }

      advance_to_current_element();
      return *this;
   }

   template <opts Opts>
   inline lazy_beve_iterator<Opts> lazy_beve_view<Opts>::begin() const
   {
      if (has_error() || !data_) return end();
      if (!is_array() && !is_object()) return end();

      const uint8_t t = uint8_t(*data_) & 0b00000'111;
      const bool is_typed = (t == tag::typed_array);
      return lazy_beve_iterator<Opts>{doc_, data_, beve_end(), is_object(), is_typed};
   }

   template <opts Opts>
   inline lazy_beve_iterator<Opts> lazy_beve_view<Opts>::end() const
   {
      return lazy_beve_iterator<Opts>{};
   }

   // ============================================================================
   // indexed_lazy_beve_view implementation
   // ============================================================================

   template <opts Opts>
   inline indexed_lazy_beve_iterator<Opts> indexed_lazy_beve_view<Opts>::begin() const
   {
      return indexed_lazy_beve_iterator<Opts>{this, 0};
   }

   template <opts Opts>
   inline indexed_lazy_beve_iterator<Opts> indexed_lazy_beve_view<Opts>::end() const
   {
      return indexed_lazy_beve_iterator<Opts>{this, value_starts_.size()};
   }

   template <opts Opts>
   inline indexed_lazy_beve_view<Opts> lazy_beve_view<Opts>::index() const
   {
      if (has_error() || !data_ || (!is_array() && !is_object())) {
         return indexed_lazy_beve_view<Opts>{};
      }

      const char* end = beve_end();
      indexed_lazy_beve_view<Opts> result{doc_, end, is_object()};

      const char* p = data_;
      const uint8_t t = uint8_t(*p);
      const uint8_t type_bits = t & 0b00000'111;
      ++p;

      const auto count = detail::read_compressed_int(p, end);
      if (count == 0) return result;

      result.reserve(count);

      if (type_bits == tag::object) {
         // Check key type from bits 3-4 of tag
         const uint8_t key_type_bits = t & 0b000'11'000;
         const bool has_string_keys = (key_type_bits == 0);

         if (has_string_keys) {
            // Object with string keys
            for (size_t i = 0; i < count; ++i) {
               const auto key_len = detail::read_compressed_int(p, end);
               std::string_view key{p, key_len};
               p += key_len;

               result.add_element(p, key);
               p = detail::skip_value_beve_lazy<Opts>(p, end);
            }
         }
         else {
            // Object with number keys
            const size_t key_size = byte_count_lookup[t >> 5];
            for (size_t i = 0; i < count; ++i) {
               p += key_size; // Skip the number key
               result.add_element(p); // No key stored for number keys
               p = detail::skip_value_beve_lazy<Opts>(p, end);
            }
         }
      }
      else if (type_bits == tag::generic_array) {
         // Generic array
         for (size_t i = 0; i < count; ++i) {
            result.add_element(p);
            p = detail::skip_value_beve_lazy<Opts>(p, end);
         }
      }
      else if (type_bits == tag::typed_array) {
         // Typed array
         const uint8_t element_type = (t & 0b000'11'000) >> 3;
         result.is_typed_array_ = true;

         if (element_type < 3) {
            // Numeric typed array - fixed size elements
            // Synthesize element tag: number base type + same type/width bits from array tag
            result.element_tag_ = tag::number | (t & 0b11111000);
            const size_t elem_size = byte_count_lookup[t >> 5];
            for (size_t i = 0; i < count; ++i) {
               result.add_element(p);
               p += elem_size;
            }
         }
         else {
            // Bool or string array
            const bool is_string_arr = (t & 0b00'1'00'000) >> 5;
            if (is_string_arr) {
               // String array - elements are length-prefixed strings without tag
               result.element_tag_ = tag::string;
               for (size_t i = 0; i < count; ++i) {
                  result.add_element(p);
                  const auto len = detail::read_compressed_int(p, end);
                  p += len;
               }
            }
            else {
               // Boolean array - packed bits, index each bit position conceptually
               // For simplicity, we don't index individual bools
               // Users should use size() and iteration instead
            }
         }
      }

      return result;
   }

   // ============================================================================
   // lazy_beve_view::get<T>() implementation
   // ============================================================================

   template <opts Opts>
   template <class T>
   [[nodiscard]] inline expected<T, error_ctx> lazy_beve_view<Opts>::get() const
   {
      if (has_error()) {
         return unexpected(error_ctx{0, error_});
      }

      const char* end = beve_end();

      // For typed array elements, synthetic_tag_ is non-zero and data_ points directly to value
      // For normal values, tag is read from data_ and value starts at data_+1
      const bool has_synthetic_tag = (synthetic_tag_ != 0);
      const uint8_t tag = has_synthetic_tag ? synthetic_tag_ : uint8_t(*data_);
      const char* value_ptr = has_synthetic_tag ? data_ : data_ + 1;

      if constexpr (std::is_same_v<T, bool>) {
         if (has_synthetic_tag) {
            return unexpected(error_ctx{0, error_code::get_wrong_type}); // Bool arrays not supported this way
         }
         if (!is_boolean()) {
            return unexpected(error_ctx{0, error_code::get_wrong_type});
         }
         // Boolean value is in bit 4 of the tag
         return (uint8_t(*data_) >> 4) & 1;
      }
      else if constexpr (std::is_same_v<T, std::nullptr_t>) {
         if (has_synthetic_tag || !is_null()) {
            return unexpected(error_ctx{0, error_code::get_wrong_type});
         }
         return nullptr;
      }
      else if constexpr (std::is_same_v<T, std::string>) {
         const uint8_t base_type = tag & 0b00000'111;
         if (base_type != tag::string) {
            return unexpected(error_ctx{0, error_code::get_wrong_type});
         }
         // For typed string arrays, value_ptr points to length-prefixed string (no tag)
         // For normal strings, we already skipped the tag
         const char* p = value_ptr;
         const auto len = detail::read_compressed_int(p, end);
         if (static_cast<size_t>(end - p) < len) {
            return unexpected(error_ctx{0, error_code::unexpected_end});
         }
         return std::string{p, len};
      }
      else if constexpr (std::is_same_v<T, std::string_view>) {
         const uint8_t base_type = tag & 0b00000'111;
         if (base_type != tag::string) {
            return unexpected(error_ctx{0, error_code::get_wrong_type});
         }
         const char* p = value_ptr;
         const auto len = detail::read_compressed_int(p, end);
         if (static_cast<size_t>(end - p) < len) {
            return unexpected(error_ctx{0, error_code::unexpected_end});
         }
         return std::string_view{p, len};
      }
      else if constexpr (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) {
         const uint8_t base_type = tag & 0b00000'111;
         if (base_type != tag::number) {
            return unexpected(error_ctx{0, error_code::get_wrong_type});
         }

         if (has_synthetic_tag) {
            // Typed array element - read directly from value_ptr
            return read_numeric_from_tag<T>(tag, value_ptr, end);
         }
         else {
            // Normal value - use standard BEVE parsing
            T value{};
            auto it = data_;
            context ctx{};
            parse<BEVE>::op<Opts>(value, ctx, it, end);
            if (bool(ctx.error)) {
               return unexpected(error_ctx{0, ctx.error});
            }
            return value;
         }
      }
      else {
         static_assert(false_v<T>, "Unsupported type for lazy_beve_view::get<T>()");
      }
   }

   // Helper to read numeric value directly given tag info (for typed array elements)
   template <opts Opts>
   template <class T>
   [[nodiscard]] inline expected<T, error_ctx> lazy_beve_view<Opts>::read_numeric_from_tag(uint8_t tag,
                                                                                           const char* value_ptr,
                                                                                           const char* end) const
   {
      const uint8_t type = (tag & 0b000'11'000) >> 3; // 0=float, 1=signed, 2=unsigned
      const size_t byte_count = byte_count_lookup[tag >> 5];

      if (value_ptr + byte_count > end) {
         return unexpected(error_ctx{0, error_code::unexpected_end});
      }

      if (type == 0) {
         // Floating point
         if (byte_count == 4) {
            float f;
            std::memcpy(&f, value_ptr, 4);
            return static_cast<T>(f);
         }
         else if (byte_count == 8) {
            double d;
            std::memcpy(&d, value_ptr, 8);
            return static_cast<T>(d);
         }
      }
      else if (type == 1) {
         // Signed integer
         int64_t val = 0;
         switch (byte_count) {
         case 1: {
            int8_t v;
            std::memcpy(&v, value_ptr, 1);
            val = v;
            break;
         }
         case 2: {
            int16_t v;
            std::memcpy(&v, value_ptr, 2);
            val = v;
            break;
         }
         case 4: {
            int32_t v;
            std::memcpy(&v, value_ptr, 4);
            val = v;
            break;
         }
         case 8: {
            std::memcpy(&val, value_ptr, 8);
            break;
         }
         }
         return static_cast<T>(val);
      }
      else if (type == 2) {
         // Unsigned integer
         uint64_t val = 0;
         switch (byte_count) {
         case 1: {
            uint8_t v;
            std::memcpy(&v, value_ptr, 1);
            val = v;
            break;
         }
         case 2: {
            uint16_t v;
            std::memcpy(&v, value_ptr, 2);
            val = v;
            break;
         }
         case 4: {
            uint32_t v;
            std::memcpy(&v, value_ptr, 4);
            val = v;
            break;
         }
         case 8: {
            std::memcpy(&val, value_ptr, 8);
            break;
         }
         }
         return static_cast<T>(val);
      }

      return unexpected(error_ctx{0, error_code::get_wrong_type});
   }

   // ============================================================================
   // BEVE writer for lazy_beve_view
   // ============================================================================

   template <opts Opts>
   struct to<BEVE, lazy_beve_view<Opts>>
   {
      template <auto WriteOpts, class B>
      GLZ_ALWAYS_INLINE static void op(const lazy_beve_view<Opts>& view, is_context auto&& ctx, B&& b, auto& ix)
      {
         if (view.has_error()) {
            ctx.error = view.error();
            return;
         }
         if (!view.data()) {
            // Write null
            dump_type<tag::null>(b, ix);
            return;
         }

         auto it = view.data();
         context parse_ctx{};
         skip_value<BEVE>::op<opts{}>(parse_ctx, it, view.beve_end());
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
   // lazy_beve - Main entry point
   // ============================================================================

   /**
    * @brief Create a lazy BEVE document - minimal upfront processing.
    *
    * @tparam Opts Options for parsing
    * @param buffer The BEVE buffer (must remain valid for document lifetime)
    * @return lazy_beve_document on success, error_ctx on failure
    */
   template <opts Opts = opts{}, class Buffer>
   [[nodiscard]] inline expected<lazy_beve_document<Opts>, error_ctx> lazy_beve(Buffer&& buffer)
   {
      lazy_beve_document<Opts> doc;
      doc.beve_ = reinterpret_cast<const char*>(buffer.data());
      doc.len_ = buffer.size();

      if (buffer.empty()) {
         return unexpected(error_ctx{0, error_code::unexpected_end});
      }

      // Validate first byte is a valid BEVE tag
      const uint8_t first_byte = static_cast<uint8_t>(buffer[0]);
      const uint8_t type_bits = first_byte & 0b00000'111;

      // Valid types: null(0), number(1), string(2), object(3), typed_array(4), generic_array(5), extensions(6)
      // Also boolean has tag::boolean pattern
      const bool is_boolean = (first_byte & 0b0000'1111) == tag::boolean;
      const bool is_valid_type = is_boolean || type_bits <= 6;

      if (!is_valid_type) {
         return unexpected(error_ctx{0, error_code::syntax_error});
      }

      doc.root_data_ = doc.beve_;
      doc.init_root_view();
      return doc;
   }

   // ============================================================================
   // read_beve overload for lazy_beve_view
   // ============================================================================

   template <class T, opts Opts>
   [[nodiscard]] inline error_ctx read_beve(T& value, const lazy_beve_view<Opts>& view)
   {
      return view.template read_into<T>(value);
   }

   template <class T, opts Opts>
   [[nodiscard]] inline error_ctx read_beve(T& value, lazy_beve_view<Opts>&& view)
   {
      return view.template read_into<T>(value);
   }

} // namespace glz
