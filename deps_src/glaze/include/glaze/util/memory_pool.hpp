#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace glz
{
   template <class T>
      requires(std::is_default_constructible_v<T>)
   struct memory_pool final
   {
      memory_pool() = default;

      // Disable copy and move semantics
      memory_pool(const memory_pool&) = delete;
      memory_pool& operator=(const memory_pool&) = delete;
      memory_pool(memory_pool&&) = delete;
      memory_pool& operator=(memory_pool&&) = delete;

      // Ensures that release is called exactly once per acquired object
      struct handle final
      {
         memory_pool<T>* pool;
         size_t index;

         handle(memory_pool<T>* p, size_t i) : pool(p), index(i) {}
         ~handle() { pool->release(index); }
      };

      std::shared_ptr<T> borrow()
      {
         std::lock_guard lock{mtx};
         if (available.empty()) {
            const auto current_size = values.size();
            const auto new_size = values.size() * 2;
            values.resize(new_size);
            for (size_t i = current_size; i < new_size; ++i) {
               available.emplace_back(i);
            }
         }

         const auto index = available.back();
         available.pop_back();

         // Use an aliasing constructor with a handle that will return the value
         // to the pool
         return std::shared_ptr<T>{std::make_shared<handle>(this, index), &values[index]};
      }

      size_t size() const
      {
         std::lock_guard lock{mtx};
         return values.size();
      }

      size_t available_size() const
      {
         std::lock_guard lock{mtx};
         return available.size();
      }

     private:
      void release(size_t index)
      {
         std::lock_guard lock{mtx};
         available.emplace_back(index);
      }

      mutable std::mutex mtx{};
      std::deque<T> values{2};
      std::vector<size_t> available{0, 1}; // indices of available values
   };
}
