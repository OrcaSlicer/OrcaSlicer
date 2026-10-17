//! Every geometric assertion re-derives world coordinates from the pose contract
//! (world = rotate(outline, rotation) + (x, y)) rather than trusting jagua-rs.

use crate::pack::{ItemIn, Out, Params, arrange};
use crate::{sp_input, sp_item, sp_placement, sp_point, sp_polygon, sparrow_arrange};
use rand::SeedableRng;

type Poly = Vec<(f64, f64)>;

// Placements are f32, so a 256 mm bed carries ~3e-3 mm of slack. Overlap checks
// shrink each polygon by this much, keeping "touching is allowed" true.
const SHRINK: f64 = 1e-3;
const EPS: f64 = 1e-2;

fn transform(outline: &[(f64, f64)], p: &Out) -> Poly {
    let (s, c) = p.rotation.sin_cos();
    outline
        .iter()
        .map(|&(x, y)| (x * c - y * s + p.x, x * s + y * c + p.y))
        .collect()
}

fn centroid(p: &[(f64, f64)]) -> (f64, f64) {
    let n = p.len() as f64;
    (p.iter().map(|q| q.0).sum::<f64>() / n, p.iter().map(|q| q.1).sum::<f64>() / n)
}

fn shrink(p: &[(f64, f64)]) -> Poly {
    let (cx, cy) = centroid(p);
    p.iter().map(|&(x, y)| (cx + (x - cx) * (1.0 - SHRINK), cy + (y - cy) * (1.0 - SHRINK))).collect()
}

fn seg_hit(a: (f64, f64), b: (f64, f64), c: (f64, f64), d: (f64, f64)) -> bool {
    let o = |p: (f64, f64), q: (f64, f64), r: (f64, f64)| {
        (q.0 - p.0) * (r.1 - p.1) - (q.1 - p.1) * (r.0 - p.0)
    };
    let (d1, d2, d3, d4) = (o(a, b, c), o(a, b, d), o(c, d, a), o(c, d, b));
    ((d1 > 0.0) != (d2 > 0.0)) && ((d3 > 0.0) != (d4 > 0.0))
}

fn inside(pt: (f64, f64), poly: &[(f64, f64)]) -> bool {
    let mut c = false;
    let n = poly.len();
    for i in 0..n {
        let (a, b) = (poly[i], poly[(i + 1) % n]);
        if (a.1 > pt.1) != (b.1 > pt.1)
            && pt.0 < (b.0 - a.0) * (pt.1 - a.1) / (b.1 - a.1) + a.0
        {
            c = !c;
        }
    }
    c
}

/// Works for concave outlines (the L-shapes) as well as convex ones.
fn overlaps(a: &[(f64, f64)], b: &[(f64, f64)]) -> bool {
    let (a, b) = (shrink(a), shrink(b));
    for i in 0..a.len() {
        for j in 0..b.len() {
            if seg_hit(a[i], a[(i + 1) % a.len()], b[j], b[(j + 1) % b.len()]) {
                return true;
            }
        }
    }
    a.iter().any(|&p| inside(p, &b)) || b.iter().any(|&p| inside(p, &a))
}

fn rect(w: f64, h: f64) -> Poly {
    vec![(0.0, 0.0), (w, 0.0), (w, h), (0.0, h)]
}

/// Not anchored at the origin, so a rotation about the centroid or bbox corner would fail.
fn offset_rect(w: f64, h: f64) -> Poly {
    vec![(5.0, 7.0), (5.0 + w, 7.0), (5.0 + w, 7.0 + h), (5.0, 7.0 + h)]
}

fn l_shape(s: f64) -> Poly {
    vec![(0.0, 0.0), (s, 0.0), (s, s / 2.0), (s / 2.0, s / 2.0), (s / 2.0, s), (0.0, s)]
}

struct Lcg(u64);

impl Lcg {
    fn next(&mut self, lo: f64, hi: f64) -> f64 {
        self.0 = self.0.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        lo + ((self.0 >> 33) as f64 / (1u64 << 31) as f64) * (hi - lo)
    }
}

const BED: f64 = 256.0;
const HOLE: [(f64, f64); 4] = [(0.0, 0.0), (20.0, 0.0), (20.0, 30.0), (0.0, 30.0)];

fn movable(outline: Poly, allow_rotation: bool) -> ItemIn {
    ItemIn { outline, openings: vec![], fixed: false, bed_idx: 0, x: 0.0, y: 0.0, rotation: 0.0, allow_rotation }
}

/// `time_limit_s` is the budget for each bed, not for the run, so keep it small here:
/// worst-case runtime is `max_beds * per_bed`.
fn params(max_beds: i32, per_bed: f64) -> Params {
    Params { bed_w: BED, bed_h: BED, max_beds, time_limit_s: per_bed, seed: 42 }
}

/// Every constraint from the header, checked against independently transformed outlines.
fn check_all(items: &[ItemIn], holes: &[Poly], out: &[Out], bed_w: f64, bed_h: f64) {
    let mut world: Vec<(usize, i32, Poly)> = Vec::new();
    for (i, (it, p)) in items.iter().zip(out).enumerate() {
        if p.bed_idx < 0 {
            continue;
        }
        world.push((i, p.bed_idx, transform(&it.outline, p)));
    }

    for (i, bed, poly) in &world {
        for (x, y) in poly {
            assert!(
                *x >= -EPS && *x <= bed_w + EPS && *y >= -EPS && *y <= bed_h + EPS,
                "item {i} escapes the bed at ({x}, {y})"
            );
        }
        for (h, hole) in holes.iter().enumerate() {
            assert!(!overlaps(poly, hole), "item {i} on bed {bed} overlaps hole {h}");
        }
    }

    for a in 0..world.len() {
        for b in (a + 1)..world.len() {
            if world[a].1 != world[b].1 {
                continue;
            }
            assert!(
                !overlaps(&world[a].2, &world[b].2),
                "items {} and {} overlap on bed {}",
                world[a].0,
                world[b].0,
                world[a].1
            );
        }
    }
}

#[test]
fn consolidation_empties_a_small_final_plate_without_disturbing_earlier_parts() {
    let items = vec![movable(rect(80., 80.), false), movable(rect(200., 200.), false),
        movable(offset_rect(20., 15.), false), movable(l_shape(25.), true)];
    let mut out = vec![
        Out { bed_idx: 0, x: 100., y: 100., rotation: 0. },
        Out { bed_idx: 1, x: 30., y: 40., rotation: 0. },
        Out { bed_idx: 2, x: 40., y: 40., rotation: 0. },
        Out { bed_idx: 2, x: 120., y: 120., rotation: 0. },
    ];
    let earlier = out[..2].to_vec();
    let holes = vec![HOLE.to_vec()];
    check_all(&items, &holes, &out, BED, BED);
    crate::pack::consolidate(&params(3, 0.), &holes, &items, &mut out,
        &mut rand::rngs::Xoshiro256PlusPlus::seed_from_u64(42), &|| false);
    assert_eq!(&out[..2], &earlier);
    assert_eq!(out[2].bed_idx, 0);
    assert_eq!(out[3].bed_idx, 0);
    check_all(&items, &holes, &out, BED, BED);
}

#[test]
fn consolidation_preserves_fixed_parts_and_keeps_parts_that_cannot_fit() {
    let mut fixed = movable(rect(BED, 220.), false);
    fixed.fixed = true;
    fixed.x = 0.;
    fixed.y = 36.;
    let items = vec![fixed, movable(rect(20., 20.), false)];
    let mut out = vec![Out { bed_idx: 0, x: 0., y: 36., rotation: 0. },
        Out { bed_idx: 1, x: 100., y: 100., rotation: 0. }];
    let original = out.clone();
    let holes = vec![rect(BED, 36.)];
    crate::pack::consolidate(&params(2, 0.), &holes, &items, &mut out,
        &mut rand::rngs::Xoshiro256PlusPlus::seed_from_u64(42), &|| false);
    assert_eq!(out, original);
    check_all(&items, &holes, &out, BED, BED);
}

#[test]
fn consolidation_cancellation_preserves_the_existing_layout() {
    let items = vec![movable(rect(20., 20.), false)];
    let mut out = vec![Out { bed_idx: 2, x: 100., y: 100., rotation: 0. }];
    let original = out.clone();
    crate::pack::consolidate(&params(3, 0.), &[], &items, &mut out,
        &mut rand::rngs::Xoshiro256PlusPlus::seed_from_u64(42), &|| true);
    assert_eq!(out, original);
}

#[test]
fn consolidation_tries_the_next_plate_when_the_first_is_full() {
    let mut fixed = movable(rect(BED, BED), false);
    fixed.fixed = true;
    let items = vec![fixed, movable(rect(80., 80.), false), movable(rect(20., 20.), false)];
    let mut out = vec![Out { bed_idx: 0, x: 0., y: 0., rotation: 0. },
        Out { bed_idx: 1, x: 100., y: 100., rotation: 0. },
        Out { bed_idx: 2, x: 100., y: 100., rotation: 0. }];
    let earlier = out[..2].to_vec();
    crate::pack::consolidate(&params(3, 0.), &[], &items, &mut out,
        &mut rand::rngs::Xoshiro256PlusPlus::seed_from_u64(42), &|| false);
    assert_eq!(&out[..2], &earlier);
    assert_eq!(out[2].bed_idx, 1);
    check_all(&items, &[], &out, BED, BED);
}

#[test]
fn rectangles_and_l_shapes_with_hole_and_fixed_item() {
    let mut rng = Lcg(7);
    let mut items: Vec<ItemIn> = Vec::new();
    for _ in 0..20 {
        items.push(movable(offset_rect(rng.next(15.0, 45.0), rng.next(15.0, 45.0)), true));
    }
    for _ in 0..3 {
        items.push(movable(l_shape(rng.next(25.0, 40.0)), true));
    }
    // A fixed item parked mid-bed that nothing may touch.
    items.push(ItemIn {
        outline: rect(40.0, 40.0),
        openings: vec![],
        fixed: true,
        bed_idx: 0,
        x: 100.0,
        y: 100.0,
        rotation: 0.0,
        allow_rotation: false,
    });

    let holes = vec![HOLE.to_vec()];
    let out = arrange(&params(4, 3.0), &holes, &items, &|| false, &|_, _| {});

    // The fixed item is echoed back untouched.
    let f = out.last().unwrap();
    assert_eq!((f.bed_idx, f.x, f.y, f.rotation), (0, 100.0, 100.0, 0.0));

    let placed = out.iter().filter(|p| p.bed_idx >= 0).count();
    assert_eq!(placed, items.len(), "everything should fit on a 256x256 bed");
    check_all(&items, &holes, &out, BED, BED);

    // The fixed item is an obstacle for the movable ones, not just an echo.
    let fixed_world = transform(&items.last().unwrap().outline, f);
    for (i, (it, p)) in items.iter().zip(&out).enumerate().take(items.len() - 1) {
        assert!(
            !overlaps(&transform(&it.outline, p), &fixed_world),
            "item {i} overlaps the fixed item"
        );
    }
}

#[test]
fn too_many_items_spill_onto_multiple_beds() {
    // 12 x 100x100 squares cannot share one 256x256 bed.
    let items: Vec<ItemIn> = (0..12).map(|_| movable(rect(100.0, 100.0), false)).collect();
    let holes: Vec<Poly> = vec![];
    let (max_beds, per_bed) = (6, 1.0);
    let t0 = std::time::Instant::now();
    let out = arrange(&params(max_beds, per_bed), &holes, &items, &|| false, &|_, _| {});
    let elapsed = t0.elapsed().as_secs_f64();

    // The contract is per-bed: the run may take up to `max_beds * per_bed`, no more.
    assert!(
        elapsed <= f64::from(max_beds) * per_bed + 4.0,
        "run took {elapsed:.2}s, over the per-bed budget contract"
    );

    let beds: std::collections::HashSet<i32> =
        out.iter().filter(|p| p.bed_idx >= 0).map(|p| p.bed_idx).collect();
    assert!(beds.len() > 1, "expected multiple beds, got {beds:?}");
    assert!(beds.len() <= 6);
    // 4 per bed is the geometric optimum for 100x100 into 256x256.
    assert!(beds.len() <= 4, "should not waste beds, used {}", beds.len());
    assert_eq!(out.iter().filter(|p| p.bed_idx >= 0).count(), 12);
    check_all(&items, &holes, &out, BED, BED);
}

#[test]
fn rotation_disabled_stays_exactly_zero() {
    let items: Vec<ItemIn> = (0..8).map(|_| movable(offset_rect(60.0, 30.0), false)).collect();
    let holes = vec![HOLE.to_vec()];
    let out = arrange(&params(2, 2.0), &holes, &items, &|| false, &|_, _| {});

    for (i, p) in out.iter().enumerate() {
        if p.bed_idx >= 0 {
            assert_eq!(p.rotation, 0.0, "item {i} was rotated despite allow_rotation=0");
        }
    }
    check_all(&items, &holes, &out, BED, BED);
}

#[test]
fn degenerate_items_are_unplaced_and_do_not_panic() {
    let items = vec![
        movable(vec![], false),                                     // empty
        movable(vec![(1.0, 1.0)], false),                           // single point
        movable(vec![(0.0, 0.0), (5.0, 5.0)], false),               // two points
        movable(vec![(0.0, 0.0), (0.0, 0.0), (0.0, 0.0)], false),   // all duplicates
        movable(vec![(0.0, 0.0), (10.0, 0.0), (20.0, 0.0)], false), // collinear, zero area
        movable(vec![(0.0, 0.0), (1.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)], false), // dup pt
        movable(rect(20.0, 20.0), false),                           // clockwise-safe control
        movable(vec![(0.0, 0.0), (0.0, 20.0), (20.0, 20.0), (20.0, 0.0)], false), // CW winding
        movable(rect(9999.0, 9999.0), false),                       // cannot fit any bed
    ];
    let holes: Vec<Poly> = vec![];
    let out = arrange(&params(2, 2.0), &holes, &items, &|| false, &|_, _| {});

    for i in [0, 1, 2, 3, 4, 8] {
        assert_eq!(out[i].bed_idx, -1, "item {i} should be unplaced");
    }
    // The duplicate-point polygon and both windings are recoverable, not degenerate.
    for i in [5, 6, 7] {
        assert_eq!(out[i].bed_idx, 0, "item {i} should have been placed");
    }
    check_all(&items, &holes, &out, BED, BED);
}

#[test]
fn cancellation_returns_early_without_overlaps() {
    let items: Vec<ItemIn> = (0..30).map(|_| movable(rect(30.0, 30.0), true)).collect();
    let holes: Vec<Poly> = vec![];
    // 30s per bed across 4 beds: without the cancel poll this would run for two minutes.
    let t0 = std::time::Instant::now();
    let out = arrange(&params(4, 30.0), &holes, &items, &|| true, &|_, _| {});
    assert!(
        t0.elapsed().as_secs_f64() < 5.0,
        "should_stop must abort across all beds, took {:.2}s",
        t0.elapsed().as_secs_f64()
    );
    check_all(&items, &holes, &out, BED, BED);
}

/// Decisive check on the pose convention: the bed is a hair larger than the item, so
/// the only feasible pose is (x, y) = (-5, -7). Any other convention is off by tens of mm.
#[test]
fn pose_translation_is_applied_to_the_item_local_origin() {
    let (w, h) = (60.0, 40.0);
    let items = vec![movable(offset_rect(w, h), false)];
    let out = arrange(
        &Params { bed_w: w + 0.01, bed_h: h + 0.01, max_beds: 1, time_limit_s: 0.5, seed: 1 },
        &[],
        &items,
        &|| false,
        &|_, _| {},
    );
    assert_eq!(out[0].bed_idx, 0);
    assert!((out[0].x - -5.0).abs() < EPS, "x = {}, expected -5", out[0].x);
    assert!((out[0].y - -7.0).abs() < EPS, "y = {}, expected -7", out[0].y);

    let world = transform(&items[0].outline, &out[0]);
    let xs: Vec<f64> = world.iter().map(|p| p.0).collect();
    let ys: Vec<f64> = world.iter().map(|p| p.1).collect();
    assert!(xs.iter().cloned().fold(f64::MAX, f64::min).abs() < EPS);
    assert!(ys.iter().cloned().fold(f64::MAX, f64::min).abs() < EPS);
}

/// A non-positive budget must mean "one pass", not "restart until the heat death".
#[test]
fn zero_time_limit_terminates() {
    let items: Vec<ItemIn> = (0..12).map(|_| movable(rect(40.0, 40.0), true)).collect();
    let holes = vec![HOLE.to_vec()];
    for limit in [0.0, -1.0] {
        let out = arrange(
            &Params { bed_w: BED, bed_h: BED, max_beds: 3, time_limit_s: limit, seed: 3 },
            &holes,
            &items,
            &|| false,
            &|_, _| {},
        );
        assert_eq!(out.iter().filter(|p| p.bed_idx >= 0).count(), 12);
        check_all(&items, &holes, &out, BED, BED);
    }
}

#[test]
fn same_seed_is_deterministic() {
    let items: Vec<ItemIn> = (0..10).map(|_| movable(rect(40.0, 25.0), true)).collect();
    let holes = vec![HOLE.to_vec()];
    let a = arrange(&params(3, 1.0), &holes, &items, &|| false, &|_, _| {});
    let b = arrange(&params(3, 1.0), &holes, &items, &|| false, &|_, _| {});
    assert_eq!(a, b);
}

#[test]
fn ffi_entry_point_through_raw_c_structs() {
    let outlines: Vec<Vec<sp_point>> = (0..6)
        .map(|i| {
            let w = 40.0 + i as f64 * 5.0;
            vec![
                sp_point { x: 0.0, y: 0.0 },
                sp_point { x: w, y: 0.0 },
                sp_point { x: w, y: 30.0 },
                sp_point { x: 0.0, y: 30.0 },
            ]
        })
        .collect();
    let hole_pts: Vec<sp_point> =
        HOLE.iter().map(|&(x, y)| sp_point { x, y }).collect();
    let holes = [sp_polygon { pts: hole_pts.as_ptr(), n: hole_pts.len() }];

    let items: Vec<sp_item> = outlines
        .iter()
        .map(|o| sp_item {
            outline: sp_polygon { pts: o.as_ptr(), n: o.len() },
            openings: std::ptr::null(),
            n_openings: 0,
            fixed: 0,
            bed_idx: 0,
            x: 0.0,
            y: 0.0,
            rotation: 0.0,
            allow_rotation: 1,
        })
        .collect();

    let input = sp_input {
        bed_w: BED,
        bed_h: BED,
        holes: holes.as_ptr(),
        n_holes: holes.len(),
        items: items.as_ptr(),
        n_items: items.len(),
        max_beds: 2,
        time_limit_s: 1.0,
        seed: 1,
        should_stop: None,
        user: std::ptr::null_mut(),
        on_progress: None,
    };

    let mut out = vec![sp_placement { bed_idx: -9, x: 0.0, y: 0.0, rotation: 0.0 }; items.len()];
    let rc = unsafe { sparrow_arrange(&input, out.as_mut_ptr()) };
    assert_eq!(rc, 0);
    assert!(out.iter().all(|p| p.bed_idx >= 0));

    // Re-check the results through the same independent geometry as the Rust-level tests.
    let rust_items: Vec<ItemIn> = outlines
        .iter()
        .map(|o| movable(o.iter().map(|p| (p.x, p.y)).collect(), true))
        .collect();
    let rust_out: Vec<Out> = out
        .iter()
        .map(|p| Out { bed_idx: p.bed_idx, x: p.x, y: p.y, rotation: p.rotation })
        .collect();
    check_all(&rust_items, &[HOLE.to_vec()], &rust_out, BED, BED);
}

#[test]
fn ffi_rejects_invalid_input() {
    let mut out = [sp_placement { bed_idx: 0, x: 0.0, y: 0.0, rotation: 0.0 }];
    assert_eq!(unsafe { sparrow_arrange(std::ptr::null(), out.as_mut_ptr()) }, 1);

    let bad = sp_input {
        bed_w: 0.0,
        bed_h: 256.0,
        holes: std::ptr::null(),
        n_holes: 0,
        items: std::ptr::null(),
        n_items: 0,
        max_beds: 1,
        time_limit_s: 1.0,
        seed: 0,
        should_stop: None,
        user: std::ptr::null_mut(),
        on_progress: None,
    };
    assert_eq!(unsafe { sparrow_arrange(&bad, out.as_mut_ptr()) }, 1);
}

/// Progress reporting through the raw C struct: counts must never go backwards, must stay
/// within [0, total], and the final call must report the real number of movable placements.
struct ProgressLog {
    calls: Vec<(i32, i32, i32)>,
}

unsafe extern "C" fn record_progress(user: *mut std::ffi::c_void, bed: i32, placed: i32, total: i32) {
    let log = unsafe { &mut *user.cast::<ProgressLog>() };
    log.calls.push((bed, placed, total));
}

#[test]
fn ffi_progress_callback_is_monotone_and_final() {
    // 12 x 100x100 squares need several beds, so progress spans bed boundaries.
    let outlines: Vec<Vec<sp_point>> = (0..12)
        .map(|_| {
            vec![
                sp_point { x: 0.0, y: 0.0 },
                sp_point { x: 100.0, y: 0.0 },
                sp_point { x: 100.0, y: 100.0 },
                sp_point { x: 0.0, y: 100.0 },
            ]
        })
        .collect();
    let mut items: Vec<sp_item> = outlines
        .iter()
        .map(|o| sp_item {
            outline: sp_polygon { pts: o.as_ptr(), n: o.len() },
            openings: std::ptr::null(),
            n_openings: 0,
            fixed: 0,
            bed_idx: 0,
            x: 0.0,
            y: 0.0,
            rotation: 0.0,
            allow_rotation: 0,
        })
        .collect();
    // A fixed item must not be counted in `total`, which is movable items only.
    let fixed_pts = vec![
        sp_point { x: 0.0, y: 0.0 },
        sp_point { x: 10.0, y: 0.0 },
        sp_point { x: 10.0, y: 10.0 },
        sp_point { x: 0.0, y: 10.0 },
    ];
    items.push(sp_item {
        outline: sp_polygon { pts: fixed_pts.as_ptr(), n: fixed_pts.len() },
        openings: std::ptr::null(),
        n_openings: 0,
        fixed: 1,
        bed_idx: 0,
        x: 200.0,
        y: 200.0,
        rotation: 0.0,
        allow_rotation: 0,
    });

    let mut log = ProgressLog { calls: Vec::new() };
    let input = sp_input {
        bed_w: BED,
        bed_h: BED,
        holes: std::ptr::null(),
        n_holes: 0,
        items: items.as_ptr(),
        n_items: items.len(),
        max_beds: 6,
        time_limit_s: 1.0,
        seed: 5,
        should_stop: None,
        user: std::ptr::addr_of_mut!(log).cast(),
        on_progress: Some(record_progress),
    };

    let mut out = vec![sp_placement { bed_idx: -9, x: 0.0, y: 0.0, rotation: 0.0 }; items.len()];
    let rc = unsafe { sparrow_arrange(&input, out.as_mut_ptr()) };
    assert_eq!(rc, 0);

    assert!(!log.calls.is_empty(), "progress callback was never invoked");
    let total_movable = 12;
    let mut prev_placed = -1;
    let mut prev_bed = -1;
    for &(bed, placed, total) in &log.calls {
        assert_eq!(total, total_movable, "total must be the movable item count");
        assert!(placed >= prev_placed, "placed went backwards: {prev_placed} -> {placed}");
        assert!(bed >= prev_bed, "bed index went backwards: {prev_bed} -> {bed}");
        assert!((0..=total_movable).contains(&placed), "placed {placed} out of range");
        prev_placed = placed;
        prev_bed = bed;
    }

    // A bed-start call reports the count carried in from earlier beds, so the first call is 0.
    assert_eq!(log.calls[0], (0, 0, total_movable));

    let actually_placed = out[..12].iter().filter(|p| p.bed_idx >= 0).count() as i32;
    assert_eq!(
        log.calls.last().unwrap().1,
        actually_placed,
        "final callback must report the real number of movable placements"
    );
    assert_eq!(actually_placed, 12, "all 12 squares should fit across the beds");
}

#[test]
fn item_that_only_fits_between_sampler_angles_is_still_placed() {
    // 99x10 rectangle pre-rotated by 11.25 degrees: between the sampler's 22.5 degree
    // steps, so only a continuous rotation fits it on a 100x20 bed.
    let a = 11.25f64.to_radians();
    let (s, c) = a.sin_cos();
    let outline: Poly = rect(99.0, 10.0).iter().map(|&(x, y)| (x * c - y * s, x * s + y * c)).collect();
    let items = vec![movable(outline.clone(), true)];
    let p = Params { bed_w: 100.0, bed_h: 20.0, max_beds: 1, time_limit_s: 2.0, seed: 42 };
    let out = arrange(&p, &[], &items, &|| false, &|_, _| {});
    assert_eq!(out[0].bed_idx, 0, "item was rejected as unplaceable");
    check_all(&items, &[], &out, 100.0, 20.0);
}
