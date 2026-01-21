// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <cstddef>
#include <cstring>
#include <istream>
#include <limits>
#include <vector>

#include "glaze/core/buffer_traits.hpp"

namespace glz
{
   // Concept for byte-oriented input streams
   // Requires read(char*, std::streamsize), gcount(), and eof() methods
   // Excludes wide streams (wistream) which use wchar_t
   // Note: read() returns std::istream& for derived streams, so we check for base type compatibility
   template <class S>
   concept byte_input_stream = requires(S& s, char* data, std::streamsize n) {
      { s.read(data, n) } -> std::convertible_to<std::istream&>;
      { s.gcount() } -> std::convertible_to<std::streamsize>;
      { s.eof() } -> std::convertible_to<bool>;
   };

   // A streaming buffer adapter that reads from an input stream.
   // Supports incremental refilling during deserialization for bounded memory usage.
   //
   // Usage:
   //   std::ifstream file("input.json");
   //   glz::basic_istream_buffer<std::ifstream> buffer(file);
   //   MyType value;
   //   auto ec = glz::read_json(value, buffer);
   //
   // Template parameters:
   //   Stream - Byte-oriented input stream (must satisfy byte_input_stream concept)
   //   DefaultCapacity - Buffer size in bytes (default 64KB).
   //                     Larger values reduce refill frequency but use more memory.
   //
   // EOF Detection:
   //   This buffer detects end-of-stream when gcount() returns 0, not by checking
   //   eofbit directly. This provides robustness against stream implementations that
   //   may set eofbit before all data is exhausted. The eofbit is cleared after each
   //   successful read to allow continued reading. For standard file streams, this
   //   has no practical impact beyond one extra read attempt at EOF.

   // Minimum buffer capacity for streaming.
   // Must be large enough to hold any single JSON value (floats can be ~24 bytes,
   // plus overhead for keys, syntax, etc.). Set to 2 * write_padding_bytes for
   // consistency with output buffers which resize to this value on first write.
   inline constexpr size_t min_streaming_buffer_size = 512;

   template <byte_input_stream Stream, size_t DefaultCapacity = 65536>
      requires(DefaultCapacity >= min_streaming_buffer_size)
   class basic_istream_buffer
   {
      static_assert(DefaultCapacity >= min_streaming_buffer_size,
                    "Buffer capacity must be at least 256 bytes to handle all JSON value types");

      Stream* stream_;
      std::vector<char> buffer_;
      size_t read_pos_ = 0; // Current read position in buffer
      size_t data_end_ = 0; // End of valid data in buffer
      size_t total_consumed_ = 0; // Total bytes consumed (for error reporting)
      bool eof_reached_ = false;

     public:
      using value_type = char;
      using const_reference = const char&;
      using size_type = size_t;
      using const_iterator = const char*;
      using stream_type = Stream;

      // Construct with input stream and optional initial capacity
      explicit basic_istream_buffer(Stream& stream, size_t initial_capacity = DefaultCapacity)
         : stream_(&stream), buffer_(initial_capacity)
      {
         // Perform initial fill
         refill();
      }

      // Move-only to prevent accidental copies
      basic_istream_buffer(const basic_istream_buffer&) = delete;
      basic_istream_buffer& operator=(const basic_istream_buffer&) = delete;
      basic_istream_buffer(basic_istream_buffer&&) noexcept = default;
      basic_istream_buffer& operator=(basic_istream_buffer&&) noexcept = default;

      ~basic_istream_buffer() = default;

      // Satisfy contiguous concept for existing read() function
      const char* data() const noexcept { return buffer_.data() + read_pos_; }

      // Number of unread bytes currently in buffer
      size_t size() const noexcept { return data_end_ - read_pos_; }

      // Check if buffer has no unread data
      bool empty() const noexcept { return read_pos_ >= data_end_; }

      // Read more data from stream into buffer
      // Shifts unconsumed data to beginning and fills rest of buffer
      // For slow streams, keeps reading until buffer is full or EOF
      // Returns true if buffer has data available, false if EOF and empty
      // Note: Invalidates pointers/iterators obtained before this call
      bool refill()
      {
         if (eof_reached_) {
            return size() > 0;
         }

         // Shift unconsumed data to beginning of buffer
         const size_t remaining = size();
         if (read_pos_ > 0 && remaining > 0) {
            std::memmove(buffer_.data(), buffer_.data() + read_pos_, remaining);
         }
         data_end_ = remaining;
         read_pos_ = 0;

         // Read data from stream until buffer is full or EOF
         // This handles slow streams that return fewer bytes than requested
         while (data_end_ < buffer_.size() && stream_ && !eof_reached_) {
            const size_t space = buffer_.size() - data_end_;
            stream_->read(buffer_.data() + data_end_, static_cast<std::streamsize>(space));
            const auto bytes_read = static_cast<size_t>(stream_->gcount());
            data_end_ += bytes_read;

            if (bytes_read == 0) {
               eof_reached_ = true;
            }
            else if (stream_->eof()) {
               // Stream set eofbit but returned data. Clear eofbit to allow retry.
               // True EOF is detected above when bytes_read == 0.
               stream_->clear();
            }
         }

         return size() > 0;
      }

      // Mark bytes as consumed after successful parsing
      void consume(size_t bytes) noexcept
      {
         read_pos_ += bytes;
         total_consumed_ += bytes;
      }

      // Total bytes consumed across all refills (for error position reporting)
      size_t bytes_consumed() const noexcept { return total_consumed_; }

      // Check if stream is exhausted and buffer is empty
      bool eof() const noexcept { return eof_reached_ && empty(); }

      // Reset for reuse with same stream
      void reset()
      {
         read_pos_ = 0;
         data_end_ = 0;
         total_consumed_ = 0;
         eof_reached_ = false;
         refill();
      }

      // Check if underlying stream is in good state
      bool good() const noexcept { return stream_ && stream_->good(); }

      // Check if underlying stream has failed
      bool fail() const noexcept { return !stream_ || stream_->fail(); }

      // Access underlying stream
      Stream* stream() const noexcept { return stream_; }

      // Current buffer capacity
      size_t buffer_capacity() const noexcept { return buffer_.size(); }

      // Iterator support for range concepts
      const_iterator begin() const noexcept { return data(); }
      const_iterator end() const noexcept { return buffer_.data() + data_end_; }
   };

   // buffer_traits specialization for basic_istream_buffer
   template <class Stream, size_t N>
   struct buffer_traits<basic_istream_buffer<Stream, N>>
   {
      static constexpr bool is_resizable = false;
      static constexpr bool has_bounded_capacity = false;
      static constexpr bool is_output_streaming = false; // Output streaming
      static constexpr bool is_input_streaming = true; // Input streaming

      GLZ_ALWAYS_INLINE static constexpr size_t capacity(const basic_istream_buffer<Stream, N>& b) noexcept
      {
         return b.size();
      }

      GLZ_ALWAYS_INLINE static bool ensure_capacity(basic_istream_buffer<Stream, N>&, size_t) noexcept
      {
         return true; // Input buffer capacity is managed by refill
      }

      GLZ_ALWAYS_INLINE static void finalize(basic_istream_buffer<Stream, N>&, size_t) noexcept
      {
         // No-op for input buffers
      }

      GLZ_ALWAYS_INLINE static void flush(basic_istream_buffer<Stream, N>&, size_t) noexcept
      {
         // No-op for input buffers (flush is for output)
      }

      GLZ_ALWAYS_INLINE static bool refill(basic_istream_buffer<Stream, N>& b) { return b.refill(); }

      GLZ_ALWAYS_INLINE static void consume(basic_istream_buffer<Stream, N>& b, size_t bytes) noexcept
      {
         b.consume(bytes);
      }

      GLZ_ALWAYS_INLINE static size_t bytes_consumed(const basic_istream_buffer<Stream, N>& b) noexcept
      {
         return b.bytes_consumed();
      }
   };

   // Convenience alias for std::istream (polymorphic, works with any standard stream)
   template <size_t DefaultCapacity = 65536>
   using istream_buffer = basic_istream_buffer<std::istream, DefaultCapacity>;

} // namespace glz
