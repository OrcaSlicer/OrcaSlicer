// Glaze Library
// For the license information refer to glaze.hpp

// Pure C plugin interface for ABI stability
// Plugins export these symbols with C linkage
// Request/response data uses REPE binary format

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// Current plugin interface version - increment when ABI changes
#define REPE_PLUGIN_INTERFACE_VERSION 3

// ABI-stable buffer for request/response data
typedef struct repe_buffer
{
   const char* data;
   uint64_t size;
} repe_buffer;

// Result codes for plugin operations
typedef enum repe_result { REPE_OK = 0, REPE_ERROR_INIT_FAILED = 1, REPE_ERROR_ALREADY_INITIALIZED = 2 } repe_result;

// Plugin metadata struct
// Returned by repe_plugin_info(), must remain valid for the plugin's lifetime
typedef struct repe_plugin_data
{
   const char* name; // Plugin name (e.g., "calculator")
   const char* version; // Plugin version (e.g., "1.0.0")
   const char* root_path; // RPC path prefix (e.g., "/calculator")
} repe_plugin_data;

// ---------------------------------------------------------------------------
// Plugin Interface Version (required)
// ---------------------------------------------------------------------------

// Returns the plugin interface version this plugin was built against.
// Hosts should check: repe_plugin_interface_version() == REPE_PLUGIN_INTERFACE_VERSION
// If versions don't match, the host should refuse to load the plugin.
//
// NOTE: This is kept as a standalone function (not in the struct) for ABI safety.
// The version must be checked BEFORE interpreting the struct layout.
uint32_t repe_plugin_interface_version(void);

// ---------------------------------------------------------------------------
// Plugin Information (required)
// ---------------------------------------------------------------------------

// Returns a pointer to the plugin's metadata struct.
// The returned pointer must remain valid for the entire lifetime of the plugin
// (until repe_plugin_shutdown() is called or the library is unloaded).
//
// Returns NULL on error (host should refuse to load the plugin).
//
// Recommended implementation pattern (file-scope static):
//
//   static const repe_plugin_data plugin_info = {
//       .name = "calculator",
//       .version = "1.0.0",
//       .root_path = "/calculator"
//   };
//
//   const repe_plugin_data* repe_plugin_info(void) {
//       return &plugin_info;
//   }
const repe_plugin_data* repe_plugin_info(void);

// ---------------------------------------------------------------------------
// Plugin Lifecycle (optional - may be NULL)
// ---------------------------------------------------------------------------

// Initialize the plugin.
// Called once by the host before any calls to repe_plugin_call.
// Returns: REPE_OK on success, error code on failure
// If not exported, the host assumes initialization is handled lazily.
repe_result repe_plugin_init(void);

// Shutdown the plugin and release all resources.
// Called once by the host before unloading the plugin.
// After this call, no further calls will be made to the plugin.
// If not exported, the host assumes no cleanup is needed.
void repe_plugin_shutdown(void);

// ---------------------------------------------------------------------------
// Request Processing (required)
// ---------------------------------------------------------------------------

// Process a REPE request and return a REPE response.
//
// Thread Safety: This function may be called concurrently from multiple threads.
// Each thread maintains its own response buffer.
//
// Buffer Lifetime: The returned buffer is valid only until the next call to
// repe_plugin_call on the SAME thread. Callers must copy the data if they
// need to retain it beyond the next call.
//
// WARNING: Do not store the returned buffer pointer for later use.
// The memory will be overwritten by subsequent calls.
repe_buffer repe_plugin_call(const char* request, uint64_t request_size);

#ifdef __cplusplus
}
#endif
