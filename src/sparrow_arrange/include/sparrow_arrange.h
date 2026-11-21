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
    double x, y, rotation;/* fixed items: pose. movable items: ignored */
    int    allow_rotation;/* movable items: 1 = any rotation, 0 = rotation must stay 0 */
    uint64_t filaments;   /* bit f set: prints with filament f. 0 = prints nothing */
    int    tower;         /* fixed items: 1 = a prime tower, an obstacle only on a bed printing 2+ filaments */
} sp_item;

typedef struct {
    int    bed_idx;                   /* -1 = could not be placed on any bed */
    double x, y, rotation;
} sp_placement;

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
     * an item is committed: bed_idx = current bed (0-based), or -1 once the final cleanup
     * starts, placed = movable items placed so far across all beds, total = movable items. */
    void (*on_progress)(void *user, int bed_idx, int placed, int total);
    /* optional, may be NULL: the best layout so far, n entries in input order, called
     * from the calling thread whenever the search improves. Unplaced items have
     * bed_idx -1. The buffer is only valid during the call. */
    void (*on_layout)(void *user, const sp_placement *layout, size_t n);
    /* Filament rules. Each filament set packs onto beds of its own; the cleanup passes
     * may move an item onto a bed whose filaments already include its own, and, only if
     * allow_mixing, add a filament to a bed when that empties the item's bed, keeps the
     * bed at max_filaments_per_bed or fewer (0 = no limit) and respects incompatible:
     * incompatible[f] = the filaments that cannot share a bed with filament f. */
    int    allow_mixing;
    int    max_filaments_per_bed;
    const uint64_t *incompatible;
    size_t n_incompatible;
} sp_input;

/* Items are pre-inflated by the caller (spacing is baked into outlines), so
 * touching-but-not-overlapping placements are valid.
 * `out` must have room for in->n_items entries, written in input order
 * (fixed items are echoed back unchanged).
 * Returns 0 on success, non-zero on invalid input. Never throws/panics across FFI. */
int sparrow_arrange(const sp_input *in, sp_placement *out);

#ifdef __cplusplus
}
#endif
