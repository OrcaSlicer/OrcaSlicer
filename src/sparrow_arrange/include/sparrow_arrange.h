// C ABI between libslic3r (C++) and the sparrow_arrange Rust crate.
// All lengths in millimetres, angles in radians, bed origin at bottom-left (0,0).
// Pose semantics match Slic3r::arrangement::ArrangePolygon::transformed_poly():
//   world = rotate(outline, rotation) + (x, y)
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { double x, y; } sp_point;

/* Simple polygon (outer contour only), either winding, not self-intersecting. */
typedef struct { const sp_point *pts; size_t n; } sp_polygon;

typedef struct {
    sp_polygon outline;   /* item-local coordinates */
    const sp_polygon *openings; /* enclosed item-local openings, already spacing-adjusted */
    size_t n_openings;
    int    fixed;         /* 1 = immovable, already placed on bed `bed_idx` at (x,y,rotation) */
    int    bed_idx;       /* fixed items: bed index (>=0). movable items: ignored on input */
    double x, y, rotation;/* fixed items: pose. movable items: optional hint, may be ignored */
    int    allow_rotation;/* movable items: 1 = any rotation, 0 = rotation must stay 0 */
} sp_item;

typedef struct {
    double bed_w, bed_h;              /* usable bed rectangle [0,bed_w] x [0,bed_h] */
    const sp_polygon *holes;          /* exclusion regions in bed coords, identical on every bed */
    size_t n_holes;
    const sp_item *items;
    size_t n_items;
    int    max_beds;                  /* upper bound on beds to create (>=1) */
    double time_limit_s;              /* wall-clock budget PER BED, best effort; total
                                       * runtime scales with the number of beds used.
                                       * A bed that packs everything returns early. */
    uint64_t seed;
    int  (*should_stop)(void *user);  /* optional cancel poll, may be NULL */
    void  *user;
    /* optional progress callback, may be NULL. Called when a bed starts and each time
     * an item is committed: bed_idx = current bed (0-based), placed = movable items
     * placed so far across all beds, total = movable items. */
    void (*on_progress)(void *user, int bed_idx, int placed, int total);
} sp_input;

typedef struct {
    int    bed_idx;                   /* -1 = could not be placed on any bed */
    double x, y, rotation;
} sp_placement;

/* Items are pre-inflated by the caller (spacing is baked into outlines), so
 * touching-but-not-overlapping placements are valid.
 * `out` must have room for in->n_items entries, written in input order
 * (fixed items are echoed back unchanged).
 * Returns 0 on success, non-zero on invalid input. Never throws/panics across FFI. */
int sparrow_arrange(const sp_input *in, sp_placement *out);

#ifdef __cplusplus
}
#endif
