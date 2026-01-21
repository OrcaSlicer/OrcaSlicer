// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <iterator>
#include <optional>

#include "glaze/core/istream_buffer.hpp"
#include "glaze/core/read.hpp"
#include "glaze/json/read.hpp"

namespace glz
{
   // A streaming JSON reader that reads complete values one at a time from an input stream.
   // Supports both JSON arrays and NDJSON (newline-delimited JSON) formats.
   //
   // Usage:
   //   std::ifstream file("events.ndjson");
   //   glz::json_stream_reader<Event> reader(file);
   //   Event event;
   //   while (!reader.read_next(event)) {
   //      process(event);
   //   }
   //
   // Or with range-based for loop:
   //   for (auto&& event : glz::json_stream_reader<Event>(file)) {
   //      process(event);
   //   }
   //   if (reader.last_error()) { /* handle error */ }
   //
   // Template parameters:
   //   T - The type to deserialize each value into
   //   Stream - Byte-oriented input stream (default: std::istream)

   template <class T, byte_input_stream Stream = std::istream, size_t BufferCapacity = 65536>
   class json_stream_reader
   {
      basic_istream_buffer<Stream, BufferCapacity> buffer_;
      streaming_context ctx_{};
      error_ctx last_error_{};
      bool eof_ = false;

     public:
      using value_type = T;
      using stream_type = Stream;

      explicit json_stream_reader(Stream& stream) : buffer_(stream) {}

      // Read the next complete JSON value
      // Returns error_ctx with error_code::none on success, or an error on EOF/failure
      error_ctx read_next(T& value)
      {
         if (eof_) {
            return last_error_;
         }

         // Skip leading whitespace and newlines (for NDJSON)
         skip_whitespace_and_newlines();

         if (buffer_.eof()) {
            eof_ = true;
            last_error_ = {buffer_.bytes_consumed(), error_code::end_reached};
            return last_error_;
         }

         // Try to read a complete value
         auto ec = read_streaming<opts{}>(value, buffer_, ctx_);

         if (ec) {
            // Error occurred
            eof_ = true;
            last_error_ = ec;
            return ec;
         }

         // Reset context for next value
         ctx_ = streaming_context{};
         return {};
      }

      // Check if more values might be available
      bool has_more() const noexcept { return !eof_ && !buffer_.eof(); }

      // Check if EOF has been reached
      bool eof() const noexcept { return eof_; }

      // Get the last error (if any)
      error_ctx last_error() const noexcept { return last_error_; }

      // Total bytes consumed from the stream
      size_t bytes_consumed() const noexcept { return buffer_.bytes_consumed(); }

      // Access to underlying buffer for advanced use
      basic_istream_buffer<Stream, BufferCapacity>& buffer() noexcept { return buffer_; }
      const basic_istream_buffer<Stream, BufferCapacity>& buffer() const noexcept { return buffer_; }

      // Iterator for range-based for loop support
      class iterator
      {
         json_stream_reader* reader_ = nullptr;
         std::optional<T> current_{};
         bool at_end_ = true;

        public:
         using iterator_category = std::input_iterator_tag;
         using value_type = T;
         using difference_type = std::ptrdiff_t;
         using pointer = const T*;
         using reference = const T&;

         iterator() = default;

         explicit iterator(json_stream_reader& reader) : reader_(&reader), at_end_(false) { advance(); }

         reference operator*() const { return *current_; }
         pointer operator->() const { return &*current_; }

         iterator& operator++()
         {
            advance();
            return *this;
         }

         iterator operator++(int)
         {
            iterator tmp = *this;
            ++(*this);
            return tmp;
         }

         bool operator==(std::default_sentinel_t) const noexcept { return at_end_; }
         bool operator!=(std::default_sentinel_t) const noexcept { return !at_end_; }

        private:
         void advance()
         {
            if (reader_ && !at_end_) {
               T value{};
               if (!reader_->read_next(value)) {
                  current_ = std::move(value);
               }
               else {
                  current_.reset();
                  at_end_ = true;
               }
            }
         }
      };

      iterator begin() { return iterator(*this); }
      std::default_sentinel_t end() const noexcept { return {}; }

     private:
      void skip_whitespace_and_newlines()
      {
         while (!buffer_.eof()) {
            bool found_non_ws = false;
            const char* data = buffer_.data();
            size_t size = buffer_.size();
            size_t i = 0;

            for (; i < size; ++i) {
               char c = data[i];
               if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                  found_non_ws = true;
                  break;
               }
            }

            if (i > 0) {
               buffer_.consume(i);
            }

            if (found_non_ws) {
               return;
            }

            // Buffer exhausted, try to refill
            if (!buffer_.refill()) {
               return;
            }
         }
      }
   };

   // Convenience alias for NDJSON streaming
   template <class T, byte_input_stream Stream = std::istream>
   using ndjson_stream = json_stream_reader<T, Stream>;

   // Convenience function to read all values from a stream into a container
   template <class T, byte_input_stream Stream>
   [[nodiscard]] error_ctx read_json_stream(std::vector<T>& values, Stream& stream)
   {
      json_stream_reader<T, Stream> reader(stream);
      T value;
      while (!reader.read_next(value)) {
         values.push_back(std::move(value));
      }
      // end_reached is expected (not an error), only return actual parse errors
      auto ec = reader.last_error();
      if (ec.ec == error_code::end_reached) {
         return {};
      }
      return ec;
   }

} // namespace glz
