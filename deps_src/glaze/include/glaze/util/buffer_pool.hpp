// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace glz
{
   /// Thread-safe buffer pool for coroutine-based servers.
   ///
   /// thread_local buffers are unsafe with coroutines because when a coroutine
   /// suspends (co_await), the thread may process other connections, overwriting
   /// thread_local data. This pool provides per-connection buffers that survive
   /// coroutine suspension.
   ///
   /// Usage:
   ///   buffer_pool pool{};
   ///   auto buf = pool.borrow();  // RAII - auto-returned on destruction
   ///   buf.value().resize(100);
   ///   // ... use buffer ...
   ///   // buffer automatically returned when buf goes out of scope
   class buffer_pool final
   {
     private:
      std::vector<std::unique_ptr<std::string>> buffers_;
      mutable std::mutex mutex_;
      size_t max_buffers_;
      size_t max_buffer_size_;

     public:
      /// RAII handle for borrowed buffer - automatically returns to pool on destruction
      class scoped_buffer final
      {
         friend class buffer_pool;

         buffer_pool* pool_{};
         std::unique_ptr<std::string> buffer_{};

         scoped_buffer(buffer_pool* pool, std::unique_ptr<std::string> buf) noexcept
            : pool_(pool), buffer_(std::move(buf))
         {}

        public:
         scoped_buffer() = default;

         ~scoped_buffer()
         {
            if (pool_ && buffer_) {
               pool_->release(std::move(buffer_));
            }
         }

         // Move-only
         scoped_buffer(scoped_buffer&& other) noexcept : pool_(other.pool_), buffer_(std::move(other.buffer_))
         {
            other.pool_ = nullptr;
         }

         scoped_buffer& operator=(scoped_buffer&& other) noexcept
         {
            if (this != &other) {
               if (pool_ && buffer_) {
                  pool_->release(std::move(buffer_));
               }
               pool_ = other.pool_;
               buffer_ = std::move(other.buffer_);
               other.pool_ = nullptr;
            }
            return *this;
         }

         scoped_buffer(const scoped_buffer&) = delete;
         scoped_buffer& operator=(const scoped_buffer&) = delete;

         /// Get reference to the underlying string buffer
         std::string& value() noexcept { return *buffer_; }
         const std::string& value() const noexcept { return *buffer_; }

         /// Pointer access to the underlying string buffer
         std::string* operator->() noexcept { return buffer_.get(); }
         const std::string* operator->() const noexcept { return buffer_.get(); }

         /// Dereference to get reference
         std::string& operator*() noexcept { return *buffer_; }
         const std::string& operator*() const noexcept { return *buffer_; }

         /// Check if buffer is valid
         explicit operator bool() const noexcept { return buffer_ != nullptr; }
      };

      /// Construct a buffer pool
      /// @param max_buffers Maximum number of buffers to keep in pool (default 1024)
      /// @param max_buffer_size Buffers larger than this are shrunk when returned (default 1MB)
      explicit buffer_pool(size_t max_buffers = 1024, size_t max_buffer_size = 1024 * 1024) noexcept
         : max_buffers_(max_buffers), max_buffer_size_(max_buffer_size)
      {
         buffers_.reserve(std::min(max_buffers, size_t{64})); // Pre-allocate some capacity
      }

      // Non-copyable, non-movable (due to mutex and pointers in scoped_buffers)
      buffer_pool(const buffer_pool&) = delete;
      buffer_pool& operator=(const buffer_pool&) = delete;
      buffer_pool(buffer_pool&&) = delete;
      buffer_pool& operator=(buffer_pool&&) = delete;

      ~buffer_pool() = default;

      /// Borrow a buffer from the pool (thread-safe)
      /// The returned scoped_buffer automatically returns the buffer to the pool on destruction
      [[nodiscard]] scoped_buffer borrow()
      {
         std::unique_ptr<std::string> buf;
         {
            std::lock_guard lock{mutex_};
            if (!buffers_.empty()) {
               buf = std::move(buffers_.back());
               buffers_.pop_back();
            }
         }
         if (!buf) {
            buf = std::make_unique<std::string>();
         }
         buf->clear();
         return scoped_buffer{this, std::move(buf)};
      }

      /// Get current number of buffers in the pool
      [[nodiscard]] size_t size() const noexcept
      {
         std::lock_guard lock{mutex_};
         return buffers_.size();
      }

      /// Get maximum number of buffers the pool will hold
      [[nodiscard]] size_t max_size() const noexcept { return max_buffers_; }

      /// Get maximum buffer size before shrinking
      [[nodiscard]] size_t max_buffer_size() const noexcept { return max_buffer_size_; }

     private:
      void release(std::unique_ptr<std::string> buf)
      {
         if (!buf) return;

         // Shrink oversized buffers to prevent memory bloat
         if (buf->capacity() > max_buffer_size_) {
            buf->shrink_to_fit();
         }

         std::lock_guard lock{mutex_};
         if (buffers_.size() < max_buffers_) {
            buffers_.push_back(std::move(buf));
         }
         // Otherwise buffer is deallocated
      }
   };
}
