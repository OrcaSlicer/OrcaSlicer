//! Every geometric assertion re-derives world coordinates from the pose contract
//! (world = rotate(outline, rotation) + (x, y)) rather than trusting jagua-rs.

use crate::pack::{FilamentRules, ItemIn, Out, Params, Progress, arrange_live};
use crate::{sp_input, sp_item, sp_placement, sp_point, sp_polygon, sparrow_arrange};
use rand::{RngExt, SeedableRng};

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

const BED: f64 = 256.0;
const HOLE: [(f64, f64); 4] = [(0.0, 0.0), (20.0, 0.0), (20.0, 30.0), (0.0, 30.0)];

fn movable(outline: Poly, allow_rotation: bool) -> ItemIn {
    ItemIn { outline, openings: vec![], fixed: false, bed_idx: 0, x: 0.0, y: 0.0, rotation: 0.0, allow_rotation, filaments: 0, tower: false }
}

fn arrange(p: &Params, holes: &[Poly], items: &[ItemIn], stop: &dyn Fn() -> bool, progress: Progress) -> Vec<Out> {
    arrange_live(p, holes, items, stop, progress, &|_| {})
}

/// `time_limit_s` is the budget for each bed, not for the run, so keep it small here:
/// worst-case runtime is `max_beds * per_bed`.
fn params(max_beds: i32, per_bed: f64) -> Params {
    Params { bed_w: BED, bed_h: BED, max_beds, time_limit_s: per_bed, seed: 42, filaments: FilamentRules::default() }
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
        &mut rand::rngs::Xoshiro256PlusPlus::seed_from_u64(42), &|| false, &|_| {});
    assert_eq!(&out[..2], &earlier);
    assert_eq!(out[2].bed_idx, 0);
    assert_eq!(out[3].bed_idx, 0);
    check_all(&items, &holes, &out, BED, BED);
}

#[test]
fn consolidation_skips_a_full_plate_and_leaves_parts_that_cannot_move() {
    // Bed 0 is full: a hole and a fixed part. Bed 1's part cannot move, bed 2's joins it.
    let full = ItemIn { fixed: true, y: 36., ..movable(rect(BED, 220.), false) };
    let items = vec![full, movable(rect(80., 80.), false), movable(rect(20., 20.), false)];
    let mut out = vec![Out { bed_idx: 0, x: 0., y: 36., rotation: 0. },
        Out { bed_idx: 1, x: 100., y: 100., rotation: 0. },
        Out { bed_idx: 2, x: 100., y: 100., rotation: 0. }];
    let earlier = out[..2].to_vec();
    let holes = vec![rect(BED, 36.)];
    crate::pack::consolidate(&params(3, 0.), &holes, &items, &mut out,
        &mut rand::rngs::Xoshiro256PlusPlus::seed_from_u64(42), &|| false, &|_| {});
    assert_eq!(&out[..2], &earlier);
    assert_eq!(out[2].bed_idx, 1);
    check_all(&items, &holes, &out, BED, BED);
}

fn colored(outline: Poly, filaments: u64) -> ItemIn {
    ItemIn { filaments, ..movable(outline, false) }
}

fn rules(allow_mixing: bool, max_per_bed: u32, incompatible: Vec<u64>) -> Params {
    Params { filaments: FilamentRules { allow_mixing, max_per_bed, incompatible }, ..params(4, 0.5) }
}

#[test]
fn filament_sets_share_a_bed_only_when_the_rules_allow() {
    let items: Vec<ItemIn> = (0..12).map(|i| colored(rect(40., 25.), if i % 2 == 0 { 0b01 } else { 0b10 })).collect();
    let holes = vec![HOLE.to_vec()];
    let bed_filaments = |p: &Params| {
        let out = arrange(p, &holes, &items, &|| false, &|_, _| {});
        check_all(&items, &holes, &out, BED, BED);
        let mut beds = std::collections::BTreeMap::new();
        for (it, o) in items.iter().zip(&out) {
            assert!(o.bed_idx >= 0);
            *beds.entry(o.bed_idx).or_insert(0u64) |= it.filaments;
        }
        beds.into_values().collect::<Vec<_>>()
    };
    // Mixing off, over the cap, or incompatible: each set keeps beds of its own.
    assert_eq!(bed_filaments(&rules(false, u32::MAX, vec![])), [0b01, 0b10]);
    assert_eq!(bed_filaments(&rules(true, 1, vec![])), [0b01, 0b10]);
    assert_eq!(bed_filaments(&rules(true, u32::MAX, vec![0b10, 0b01])), [0b01, 0b10]);
    // Otherwise consolidation mixes them to save the bed.
    assert_eq!(bed_filaments(&rules(true, u32::MAX, vec![])), [0b11]);
}

#[test]
fn an_item_joins_a_bed_already_printing_its_filaments_without_mixing() {
    let moved = |filaments: u64| {
        let items = vec![colored(rect(80., 80.), 0b011), colored(rect(20., 20.), filaments)];
        let mut out = vec![Out { bed_idx: 0, x: 100., y: 100., rotation: 0. },
            Out { bed_idx: 1, x: 100., y: 100., rotation: 0. }];
        crate::pack::consolidate(&rules(false, u32::MAX, vec![]), &[], &items, &mut out,
            &mut rand::rngs::Xoshiro256PlusPlus::seed_from_u64(42), &|| false, &|_| {});
        check_all(&items, &[], &out, BED, BED);
        out[1].bed_idx == 0
    };
    assert!(moved(0b001));  // one of the bed's filaments: free
    assert!(!moved(0b100)); // a new filament: not without mixing
}

#[test]
fn an_unplaceable_item_leaves_beds_for_the_next_filament_set() {
    // 250 x 250 fits the bed's box but always hits the hole, so it fits no bed.
    let items = vec![colored(rect(250., 250.), 0b01), colored(rect(30., 30.), 0b01),
        colored(rect(30., 30.), 0b10), colored(rect(30., 30.), 0b10)];
    let holes = vec![HOLE.to_vec()];
    let out = arrange(&rules(false, u32::MAX, vec![]), &holes, &items, &|| false, &|_, _| {});
    assert_eq!(out[0].bed_idx, -1);
    assert!(out[1..].iter().all(|o| o.bed_idx >= 0));
    check_all(&items, &holes, &out, BED, BED);
}

#[test]
fn a_set_that_fits_no_bed_stops_at_the_first_bed_holding_only_its_tower() {
    // Every bed has a tower, which the two-filament part covers wherever it goes.
    let tower = |bed_idx| ItemIn { fixed: true, bed_idx, x: 200., y: 200., tower: true, ..movable(rect(40., 40.), false) };
    let mut items: Vec<ItemIn> = (0..8).map(tower).collect();
    items.push(colored(rect(250., 250.), 0b11));
    let beds = std::cell::RefCell::new(Vec::new());
    let out = arrange(&params(8, 0.1), &[], &items, &|| false, &|bed, _| beds.borrow_mut().push(bed));
    assert_eq!(out[8].bed_idx, -1);
    assert!(beds.borrow().iter().all(|&b| b <= 0), "searched beds {:?}", beds.borrow());
}

#[test]
fn a_set_joins_a_bed_with_fixed_items_only_when_the_rules_allow() {
    let bed_of_part = |allow_mixing: bool| {
        let mut fixed = colored(rect(30., 30.), 0b01);
        fixed.fixed = true;
        fixed.x = 100.;
        fixed.y = 100.;
        let items = vec![fixed, colored(rect(30., 30.), 0b10)];
        let out = arrange(&rules(allow_mixing, u32::MAX, vec![]), &[], &items, &|| false, &|_, _| {});
        check_all(&items, &[], &out, BED, BED);
        out[1].bed_idx
    };
    assert_eq!(bed_of_part(true), 0);
    assert_eq!(bed_of_part(false), 1);
}

#[test]
fn a_move_that_makes_a_bed_need_its_tower_first_clears_the_tower() {
    // Bed 0 prints one filament, so its red part may sit on the tower's spot. Moving the
    // white part there makes bed 0 print two: the red part makes room if it can.
    let moved = |red: f64, fixed: bool| {
        let tower = |bed_idx| ItemIn { fixed: true, bed_idx, x: 110., y: 100., tower: true, ..movable(rect(40., 40.), false) };
        let red = ItemIn { fixed, x: 18., y: 18., ..colored(rect(red, red), 0b01) };
        // Every bed has a tower placeholder; bed 1's must not stop it emptying.
        let items = vec![tower(0), red, colored(rect(30., 30.), 0b10), tower(1)];
        let mut out = vec![Out { bed_idx: 0, x: 110., y: 100., rotation: 0. },
            Out { bed_idx: 0, x: 18., y: 18., rotation: 0. },
            Out { bed_idx: 1, x: 100., y: 100., rotation: 0. },
            Out { bed_idx: 1, x: 110., y: 100., rotation: 0. }];
        crate::pack::consolidate(&rules(true, u32::MAX, vec![]), &[], &items, &mut out,
            &mut rand::rngs::Xoshiro256PlusPlus::seed_from_u64(42), &|| false, &|_| {});
        if out[2].bed_idx == 0 {
            check_all(&items, &[], &out, BED, BED); // nothing overlaps the tower now
        }
        out[2].bed_idx == 0
    };
    assert!(moved(100., false)); // the red part moves off the tower, then the white one joins
    assert!(!moved(220., false)); // a red part this big covers the tower wherever it goes
    assert!(!moved(100., true)); // a fixed red part cannot move off it
}

#[test]
fn every_filament_set_gets_to_empty_its_last_bed() {
    // White's last bed cannot empty (bed 2 is full), which must not stop red's from emptying.
    let mut full = colored(rect(BED, BED), 0b10);
    full.fixed = true;
    let items = vec![colored(rect(80., 80.), 0b01), colored(rect(20., 20.), 0b01), full,
        colored(rect(20., 20.), 0b10)];
    let mut out = vec![Out { bed_idx: 0, x: 100., y: 100., rotation: 0. },
        Out { bed_idx: 1, x: 100., y: 100., rotation: 0. },
        Out { bed_idx: 2, x: 0., y: 0., rotation: 0. },
        Out { bed_idx: 3, x: 100., y: 100., rotation: 0. }];
    crate::pack::consolidate(&rules(false, u32::MAX, vec![]), &[], &items, &mut out,
        &mut rand::rngs::Xoshiro256PlusPlus::seed_from_u64(42), &|| false, &|_| {});
    assert_eq!(out[1].bed_idx, 0);
    assert_eq!(out[3].bed_idx, 3);
    check_all(&items, &[], &out, BED, BED);
}

#[test]
fn moves_that_add_a_filament_are_undone_unless_their_bed_empties() {
    // Bed 0 leaves a 60 x 60 corner, room for one of bed 1's two items only.
    let mut wall = colored(rect(BED, 196.), 0b01);
    wall.fixed = true;
    let mut ledge = colored(rect(196., 60.), 0b01);
    ledge.fixed = true;
    ledge.y = 196.;
    let items = vec![wall, ledge, colored(rect(40., 40.), 0b10), colored(rect(40., 40.), 0b10)];
    let mut out = vec![Out { bed_idx: 0, x: 0., y: 0., rotation: 0. },
        Out { bed_idx: 0, x: 0., y: 196., rotation: 0. },
        Out { bed_idx: 1, x: 100., y: 100., rotation: 0. },
        Out { bed_idx: 1, x: 150., y: 150., rotation: 0. }];
    let original = out.clone();
    crate::pack::consolidate(&rules(true, u32::MAX, vec![]), &[], &items, &mut out,
        &mut rand::rngs::Xoshiro256PlusPlus::seed_from_u64(42), &|| false, &|_| {});
    assert_eq!(out, original);

    // Control: alone, the item fits the corner, empties bed 1 and stays.
    let mut items = items;
    items.truncate(3);
    out.truncate(3);
    crate::pack::consolidate(&rules(true, u32::MAX, vec![]), &[], &items, &mut out,
        &mut rand::rngs::Xoshiro256PlusPlus::seed_from_u64(42), &|| false, &|_| {});
    assert_eq!(out[2].bed_idx, 0);
    check_all(&items, &[], &out, BED, BED);
}

#[test]
fn rectangles_and_l_shapes_with_hole_and_fixed_item() {
    let mut rng = rand::rngs::Xoshiro256PlusPlus::seed_from_u64(7);
    let mut items: Vec<ItemIn> = Vec::new();
    // Every other rectangle is rotation-locked.
    for i in 0..20 {
        items.push(movable(offset_rect(rng.random_range(15.0..45.0), rng.random_range(15.0..45.0)), i % 2 == 0));
    }
    for _ in 0..3 {
        items.push(movable(l_shape(rng.random_range(25.0..40.0)), true));
    }
    // A fixed item parked mid-bed that nothing may touch.
    items.push(ItemIn { fixed: true, x: 100.0, y: 100.0, ..movable(rect(40.0, 40.0), false) });

    let holes = vec![HOLE.to_vec()];
    let out = arrange(&params(4, 1.0), &holes, &items, &|| false, &|_, _| {});

    // The fixed item is echoed back untouched.
    let f = out.last().unwrap();
    assert_eq!((f.bed_idx, f.x, f.y, f.rotation), (0, 100.0, 100.0, 0.0));
    for (i, (it, p)) in items.iter().zip(&out).enumerate() {
        assert!(p.bed_idx >= 0, "item {i} should fit on a 256x256 bed");
        assert!(it.allow_rotation || p.rotation == 0.0, "item {i} was rotated despite allow_rotation=0");
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
        &Params { bed_w: w + 0.01, bed_h: h + 0.01, max_beds: 1, time_limit_s: 0.5, seed: 1, filaments: FilamentRules::default() },
        &[],
        &items,
        &|| false,
        &|_, _| {},
    );
    assert_eq!(out[0].bed_idx, 0);
    assert!((out[0].x - -5.0).abs() < EPS, "x = {}, expected -5", out[0].x);
    assert!((out[0].y - -7.0).abs() < EPS, "y = {}, expected -7", out[0].y);
}

/// A non-positive budget must mean "one pass", not "restart until the heat death".
#[test]
fn zero_time_limit_terminates() {
    let items: Vec<ItemIn> = (0..12).map(|_| movable(rect(40.0, 40.0), true)).collect();
    let holes = vec![HOLE.to_vec()];
    for limit in [0.0, -1.0] {
        let out = arrange(
            &Params { bed_w: BED, bed_h: BED, max_beds: 3, time_limit_s: limit, seed: 3, filaments: FilamentRules::default() },
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
fn live_layouts_end_with_the_result() {
    let items: Vec<ItemIn> = (0..10).map(|_| movable(rect(40.0, 25.0), true)).collect();
    let holes = vec![HOLE.to_vec()];
    let snapshots = std::cell::RefCell::new(Vec::new());
    let out = arrange_live(&params(3, 0.5), &holes, &items, &|| false, &|_, _| {},
        &|s| snapshots.borrow_mut().push(s.to_vec()));
    assert_eq!(snapshots.into_inner().last(), Some(&out));
}

#[test]
fn ffi_rejects_invalid_input() {
    let mut out = [sp_placement { bed_idx: 0, x: 0.0, y: 0.0, rotation: 0.0 }];
    assert_eq!(unsafe { sparrow_arrange(std::ptr::null(), out.as_mut_ptr()) }, 1);

    // All zeroes is a valid sp_input: null pointers, no callbacks.
    let bad = sp_input { bed_w: 0.0, bed_h: 256.0, max_beds: 1, ..unsafe { std::mem::zeroed() } };
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
fn ffi_round_trip_through_raw_c_structs() {
    // 12 x 100x100 squares need several beds, so progress spans bed boundaries.
    let square: Vec<sp_point> = rect(100., 100.).iter().map(|&(x, y)| sp_point { x, y }).collect();
    let small: Vec<sp_point> = rect(10., 10.).iter().map(|&(x, y)| sp_point { x, y }).collect();
    let hole_pts: Vec<sp_point> = HOLE.iter().map(|&(x, y)| sp_point { x, y }).collect();
    let holes = [sp_polygon { pts: hole_pts.as_ptr(), n: hole_pts.len() }];
    let item = |pts: &[sp_point]| sp_item { outline: sp_polygon { pts: pts.as_ptr(), n: pts.len() }, ..unsafe { std::mem::zeroed() } };
    let mut items: Vec<sp_item> = (0..12).map(|_| item(&square)).collect();
    // A fixed item must not be counted in `total`, which is movable items only.
    items.push(sp_item { fixed: 1, x: 200., y: 200., ..item(&small) });

    let mut log = ProgressLog { calls: Vec::new() };
    let input = sp_input {
        bed_w: BED,
        bed_h: BED,
        holes: holes.as_ptr(),
        n_holes: holes.len(),
        items: items.as_ptr(),
        n_items: items.len(),
        max_beds: 6,
        time_limit_s: 1.0,
        seed: 5,
        user: std::ptr::addr_of_mut!(log).cast(),
        on_progress: Some(record_progress),
        ..unsafe { std::mem::zeroed() }
    };

    let mut out = vec![sp_placement { bed_idx: -9, x: 0.0, y: 0.0, rotation: 0.0 }; items.len()];
    let rc = unsafe { sparrow_arrange(&input, out.as_mut_ptr()) };
    assert_eq!(rc, 0);
    assert!(out.iter().all(|p| p.bed_idx >= 0), "all 12 squares should fit across the beds");
    let beds: std::collections::HashSet<i32> = out.iter().map(|p| p.bed_idx).collect();
    // 4 per bed is the geometric optimum for 100x100 into 256x256.
    assert!(beds.len() > 1 && beds.len() <= 4, "used beds {beds:?}");

    // Re-check the results through the same independent geometry as the Rust-level tests.
    let mut rust_items: Vec<ItemIn> = (0..12).map(|_| movable(rect(100., 100.), false)).collect();
    rust_items.push(ItemIn { fixed: true, ..movable(rect(10., 10.), false) });
    check_all(&rust_items, &[HOLE.to_vec()], &out, BED, BED);

    let mut prev = (-1, -1);
    for &(bed, placed, total) in &log.calls {
        assert_eq!(total, 12, "total must be the movable item count");
        assert!(placed >= prev.1, "placed went backwards: {} -> {placed}", prev.1);
        // Beds only advance, apart from the final cleanup (-1) and the closing report.
        assert!(bed >= prev.0 || bed == -1 || prev.0 == -1, "bed index went backwards: {} -> {bed}", prev.0);
        assert!((0..=12).contains(&placed), "placed {placed} out of range");
        prev = (bed, placed);
    }
    assert_eq!(log.calls.last().unwrap().1, 12, "the final call must report every placement");
}

#[test]
fn item_that_only_fits_between_sampler_angles_is_still_placed() {
    // 99x10 rectangle pre-rotated by 11.25 degrees: between the sampler's 22.5 degree
    // steps, so only a continuous rotation fits it on a 100x20 bed.
    let a = 11.25f64.to_radians();
    let (s, c) = a.sin_cos();
    let outline: Poly = rect(99.0, 10.0).iter().map(|&(x, y)| (x * c - y * s, x * s + y * c)).collect();
    let items = vec![movable(outline.clone(), true)];
    let p = Params { bed_w: 100.0, bed_h: 20.0, max_beds: 1, time_limit_s: 2.0, seed: 42, filaments: FilamentRules::default() };
    let out = arrange(&p, &[], &items, &|| false, &|_, _| {});
    assert_eq!(out[0].bed_idx, 0, "item was rejected as unplaceable");
    check_all(&items, &[], &out, 100.0, 20.0);
}
