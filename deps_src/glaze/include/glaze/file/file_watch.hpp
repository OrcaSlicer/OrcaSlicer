// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <chrono>
#include <filesystem>
#include <future>

namespace glz
{
   struct thread_handler final
   {
      std::atomic<bool> alive = true;
      std::future<void> future{};
   };

   // This runs a thread that continually checks if a file has been modified.
   // If the file has been modified, a callaback is invoked.
   // This is quite cheap for one file, but is not designed to run on many files at once.
   // It is best employed on a single file as a user input interface.
   inline auto file_watch(const std::filesystem::path& path, auto&& callback)
   {
      std::shared_ptr<thread_handler> h{new thread_handler(), [](auto* ptr) {
                                           ptr->alive = false;
                                           delete ptr;
                                        }};

      h->future = std::async([alive = &h->alive, path, callback = std::move(callback)] {
         namespace fs = std::filesystem;
         if (!fs::exists(path)) {
            throw std::runtime_error("File " + path.string() + " does not exist.\n");
         }

         auto prev = fs::last_write_time(path);
         while (*alive) {
            auto t = fs::last_write_time(path);

            if (prev != t) {
               callback();
               prev = t;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10)); // every 0.01 s
         }
      });

      return h;
   }
}
