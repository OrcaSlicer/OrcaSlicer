// Glaze Library
// For the license information refer to glaze.hpp

// C++ helper for implementing REPE plugins
// Provides common functionality to reduce code duplication

#pragma once

#include <span>
#include <string>

#include "glaze/rpc/registry.hpp"
#include "glaze/rpc/repe/buffer.hpp"
#include "glaze/rpc/repe/header.hpp"
#include "glaze/rpc/repe/plugin.h"

namespace glz::repe
{
   // Thread-local response buffer for plugin implementations
   // Note: This buffer grows as needed but does not shrink during the thread's lifetime
   inline thread_local std::string plugin_response_buffer;

   // Create an error response with proper REPE format (zero-copy to thread-local buffer)
   inline void plugin_error_response(error_code ec, std::string_view error_msg, uint64_t id = 0)
   {
      encode_error_buffer(ec, plugin_response_buffer, error_msg, id);
   }

   // Zero-copy plugin call implementation
   // Dispatches a REPE request to a registry and returns the response
   // Note: Plugin initialization should be done via repe_plugin_init before any calls
   template <typename Registry>
   repe_buffer plugin_call(Registry& registry, const char* request, uint64_t request_size)
   {
      // Use zero-copy span-based call - parses request in-place, writes directly to response buffer
      // Exception handling is done internally by the registry
      registry.call(std::span<const char>{request, static_cast<size_t>(request_size)}, plugin_response_buffer);
      return {plugin_response_buffer.data(), plugin_response_buffer.size()};
   }

} // namespace glz::repe
