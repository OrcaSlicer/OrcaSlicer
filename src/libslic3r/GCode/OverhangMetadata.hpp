#pragma once

namespace Slic3r {

// Orca: The largest overhang percentage the metadata carries. Walls report how far their bead's outer edge reaches past
// the lower contour, which may pass a full bead, up to ten widths, so the angle preview stays continuous; a wall with
// no lower edge within that reach reads this cap.
inline constexpr float MAX_OVERHANG_PERCENTAGE = 1000.0f;

} // namespace Slic3r
