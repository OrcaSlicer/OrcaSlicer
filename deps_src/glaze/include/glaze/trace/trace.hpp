// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <chrono>
#include <deque>
#include <mutex>
#include <thread>

#include "glaze/json/write.hpp"

// This code allows profiling and time tracing using tools like Perfetto https://perfetto.dev/
// The specification adheres to Chrome's tracing format document:
// https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU/preview
// ```c++
// glz::trace trace{};
// trace.begin("my event name");
// run computations
// trace.end("my event name");
// ```

namespace glz
{
   enum struct display_time_unit : uint32_t {
      s, // seconds
      ms, // milliseconds
      us, // microseconds
      ns // nanoseconds
   };

   template <>
   struct meta<display_time_unit>
   {
      static constexpr sv name = "glz::display_time_unit";
      using enum display_time_unit;
      static constexpr auto value = enumerate("s", s, //
                                              "ms", ms, //
                                              "us", us, //
                                              "ns", ns);
   };

   struct trace_event
   {
      std::string_view name{}; // The name of the event, as displayed in Trace Viewer
      std::optional<std::string_view>
         cat{}; // The event categories. A comma separated list of categories for the event.
      char ph{}; // The event type.
      uint64_t ts{}; // The tracing clock timestamp of the event. Provided at microsecond granularity.
      std::optional<uint64_t> tts{}; // The thread clock timestamp of the event. Provided at microsecond granularity.
      uint64_t pid{}; // The process ID for the process that output this event.
      uint64_t tid{}; // The thread ID for the thread that output this event.
      std::optional<uint64_t>
         id{}; // For async events. Events with the same category and id are treated as from the same event tree.
      glz::raw_json args = "{}"; // metadata
   };

   template <bool Enabled = true>
   struct trace
   {
      std::atomic<bool> disabled = false;
      display_time_unit displayTimeUnit = display_time_unit::ms;
      std::deque<trace_event> traceEvents{};
      std::optional<std::chrono::time_point<std::chrono::steady_clock>> t0{}; // the time of the first event
      std::mutex mtx{};

      void clear() { traceEvents.clear(); }

      template <class... Args>
         requires(sizeof...(Args) <= 1)
      void begin(const std::string_view name, Args&&... args) noexcept
      {
         if constexpr (Enabled) {
            if (disabled) {
               return;
            }
            duration(name, 'B', std::forward<Args>(args)...);
         }
      }

      template <class... Args>
         requires(sizeof...(Args) <= 1)
      void end(const std::string_view name, Args&&... args) noexcept
      {
         if constexpr (Enabled) {
            if (disabled) {
               return;
            }
            duration(name, 'E', std::forward<Args>(args)...);
         }
      }

      template <class... Args>
         requires(sizeof...(Args) <= 1)
      void duration(const std::string_view name, const char phase, Args&&... args) noexcept
      {
         if constexpr (Enabled) {
            if (disabled) {
               return;
            }

            const auto tnow = std::chrono::steady_clock::now();
            trace_event* event{};
            {
               std::unique_lock lock{mtx};
               if (!t0) {
                  t0 = tnow;
               }
               event = &traceEvents.emplace_back();
            }
            event->name = name;
            event->ph = phase;
            event->ts = std::chrono::duration_cast<std::chrono::microseconds>(tnow - t0.value()).count();
            event->tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
            if constexpr (sizeof...(Args) > 0) {
               std::ignore = glz::write_json(std::forward<Args>(args)..., event->args.str);
            }
         }
      }

      template <class... Args>
         requires(sizeof...(Args) <= 1)
      void async_begin(const std::string_view name, Args&&... args) noexcept
      {
         if constexpr (Enabled) {
            if (disabled) {
               return;
            }
            async(name, 'b', std::forward<Args>(args)...);
         }
      }

      template <class... Args>
         requires(sizeof...(Args) <= 1)
      void async_end(const std::string_view name, Args&&... args) noexcept
      {
         if constexpr (Enabled) {
            if (disabled) {
               return;
            }
            async(name, 'e', std::forward<Args>(args)...);
         }
      }

      template <class... Args>
         requires(sizeof...(Args) <= 1)
      void async(const std::string_view name, const char phase, Args&&... args) noexcept
      {
         if constexpr (Enabled) {
            if (disabled) {
               return;
            }

            const auto tnow = std::chrono::steady_clock::now();
            trace_event* event{};
            {
               std::unique_lock lock{mtx};
               if (!t0) {
                  t0 = tnow;
               }
               event = &traceEvents.emplace_back();
            }
            event->name = name;
            event->ph = phase;
            event->ts = std::chrono::duration_cast<std::chrono::microseconds>(tnow - t0.value()).count();
            event->tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
            event->id = std::hash<std::string_view>{}(name);
            if constexpr (sizeof...(Args) > 0) {
               std::ignore = glz::write_json(std::forward<Args>(args)..., event->args.str);
            }
         }
      }

      // Automatically adds the end event when it leave scope
      struct duration_scoper final
      {
         trace& tr;

         duration_scoper(trace& tr, const std::string_view name) noexcept : tr(tr), name(name)
         {
            if constexpr (Enabled) {
               if (not tr.disabled) {
                  tr.begin(name);
               }
            }
         }
         ~duration_scoper() noexcept
         {
            if constexpr (Enabled) {
               if (not tr.disabled) {
                  tr.end(name);
               }
            }
         }

         const std::string_view name{};
      };

      struct async_scoper final
      {
         trace& tr;

         async_scoper(trace& tr, const std::string_view name) noexcept : tr(tr), name(name)
         {
            if constexpr (Enabled) {
               if (not tr.disabled) {
                  tr.async_begin(name);
               }
            }
         }
         ~async_scoper() noexcept
         {
            if constexpr (Enabled) {
               if (not tr.disabled) {
                  tr.async_end(name);
               }
            }
         }

         const std::string_view name{};
      };

      duration_scoper scope(const std::string_view name) { return {*this, name}; }

      async_scoper async_scope(const std::string_view name) { return {*this, name}; }
   };

   template <>
   struct meta<trace<true>>
   {
      using T = trace<true>;
      static constexpr auto value = object(&T::traceEvents, &T::displayTimeUnit);
   };

   template <>
   struct meta<trace<false>>
   {
      using T = trace<false>;
      // Compile time disabled trace writes out an empty object
      static constexpr auto value = object();
   };
}
