// Glaze Library
// For the license information refer to glaze.hpp

#pragma once

#include <cstdint>

namespace glz
{
   // ============================================================================
   // Optimization Level
   // ============================================================================
   //
   // Controls the trade-off between binary size and runtime performance.
   //
   // - Embedded systems: Use `size` to minimize binary footprint
   // - General applications: Use `normal` for maximum performance (default)
   //
   // ============================================================================

   enum struct optimization_level : uint8_t {
      // -------------------------------------------------------------------------
      // SIZE (Level 0)
      // -------------------------------------------------------------------------
      // Priority: Minimize binary size for embedded systems
      //
      // Current behavior:
      //   - Integer serialization: glz::to_chars (400B lookup tables)
      //   - Float serialization: std::to_chars (no dragonbox ~238KB tables)
      //   - Key matching: linear_search=true by default (no hash tables)
      //
      // Binary size savings (approximate):
      //   - ~39KB from using smaller integer tables (400B vs 40KB)
      //   - ~238KB from avoiding dragonbox tables
      //   - Variable savings from hash table elimination
      //
      size = 0,

      // -------------------------------------------------------------------------
      // NORMAL (Level 1) - DEFAULT
      // -------------------------------------------------------------------------
      // Priority: Maximum performance (default)
      //
      // Current behavior:
      //   - Integer serialization: glz::to_chars_40kb (40KB digit_quads table)
      //   - Float serialization: dragonbox (~238KB tables)
      //   - Key matching: hash-based lookup
      //
      // This is the default level optimized for speed.
      //
      normal = 1
   };

} // namespace glz
