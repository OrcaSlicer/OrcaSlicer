//! Multi-bed nesting driven by sparrow's overlap-tolerant separator.
//!
//! Per bed we build a fixed container (the bed rectangle), drop the candidate items in with
//! overlap allowed, and let `Separator::separate` drive total overlap to zero by guided
//! local search. If it cannot, we evict the worst item and try again.
//!
//! Pose contract (same as the C header): world = rotate(outline, rotation) + (x, y),
//! about the item-local origin. Shapes are built with an identity `pre_transform`, so
//! a `DTransformation` maps 1:1 onto the header; jagua-rs' importer would re-centre them.

use jagua_rs::collision_detection::CDEConfig;
use jagua_rs::entities::{Container, Item, Layout};
use jagua_rs::geometry::fail_fast::SPSurrogateConfig;
use jagua_rs::geometry::geo_enums::RotationRange;
use jagua_rs::geometry::geo_traits::TransformableFrom;
use jagua_rs::geometry::primitives::{Point, Rect, SPolygon};
use jagua_rs::geometry::shape_modification::{ShapeModifyConfig, ShapeModifyMode};
use jagua_rs::geometry::{DTransformation, OriginalShape};
use jagua_rs::probs::spp::entities::{SPInstance, SPPlacement, SPProblem, Strip};
use rand::{RngExt, SeedableRng};
use rand::rngs::Xoshiro256PlusPlus;
use sparrow::consts::LBF_SAMPLE_CONFIG;
use sparrow::eval::lbf_evaluator::LBFEvaluator;
use sparrow::eval::sample_eval::SampleEval;
use sparrow::optimizer::separator::{Separator, SeparatorConfig};
use sparrow::sample::search::{SampleConfig, search_placement};
use sparrow::sample::uniform_sampler::UniformBBoxSampler;
use sparrow::util::listener::DummySolListener;
use sparrow::util::terminator::Terminator;
use std::time::{Duration, Instant};

/// Caller-facing item, already in plain Rust types.
pub struct ItemIn {
    pub outline: Vec<(f64, f64)>,
    pub openings: Vec<Vec<(f64, f64)>>,
    pub fixed: bool,
    pub bed_idx: i32,
    pub x: f64,
    pub y: f64,
    pub rotation: f64,
    pub allow_rotation: bool,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Out {
    pub bed_idx: i32,
    pub x: f64,
    pub y: f64,
    pub rotation: f64,
}

impl Out {
    const UNPLACED: Out = Out { bed_idx: -1, x: 0.0, y: 0.0, rotation: 0.0 };
}

pub struct Params {
    pub bed_w: f64,
    pub bed_h: f64,
    pub max_beds: i32,
    pub time_limit_s: f64,
    pub seed: u64,
}

// Lifted from sparrow's DEFAULT_SPARROW_CONFIG.
const CDE_CONFIG: CDEConfig = CDEConfig {
    quadtree_depth: 4,
    cd_threshold: 16,
    item_surrogate_config: SPSurrogateConfig {
        n_pole_limits: [(64, 0.0), (16, 0.8), (8, 0.9)],
        ff_pole_area_ratio: 0.5,
        n_ff_piers: 0,
    },
};

const NO_MODIFY: ShapeModifyConfig = ShapeModifyConfig {
    simplify_tolerance: None,
    offset: None,
    narrow_concavity_cutoff: None,
};

const SEP_SAMPLE_CONFIG: SampleConfig =
    SampleConfig { n_container_samples: 50, n_focussed_samples: 25, n_coord_descents: 3 };

/// Initial fill target, as a fraction of obstacle-free area: just past capacity.
const TARGET_FILL: f32 = 0.92;

/// Evict-or-add attempts per bed; also the number of slices the budget is split into.
const MAX_ATTEMPTS: u32 = 8;

/// Track meaningful gains in the best feasible packing. Since usable bed area
/// is constant during a bed's search, relative fill improvement is exactly the
/// relative improvement in occupied area; no approximate obstacle-area sum is needed.
struct FillProgress {
    reference_area: Option<f32>,
    last_gain: Instant,
    patience: Duration,
}

impl FillProgress {
    fn new(now: Instant, patience: Duration) -> Self {
        Self { reference_area: None, last_gain: now, patience }
    }

    fn observe(&mut self, area: f32, now: Instant) {
        if area <= 0.0 || !area.is_finite() {
            return;
        }
        // Accumulate small gains against the last meaningful improvement rather
        // than restarting the timer for every tiny addition or numeric fluctuation.
        if self.reference_area.is_none_or(|previous| area >= previous * 1.01) {
            self.reference_area = Some(area);
            self.last_gain = now;
        }
    }

    fn deadline(&self) -> Option<Instant> {
        self.reference_area.map(|_| self.last_gain + self.patience)
    }
}

/// Termination for the separator: the caller's deadline plus the caller's cancel poll.
struct Deadline<'a> {
    until: Instant,
    stop: &'a dyn Fn() -> bool,
}

impl Terminator for Deadline<'_> {
    fn kill(&self) -> bool {
        Instant::now() >= self.until || (self.stop)()
    }
    fn new_timeout(&mut self, timeout: Duration) {
        self.until = Instant::now() + timeout;
    }
    fn timeout_at(&self) -> Option<Instant> {
        Some(self.until)
    }
}

/// Bounding-box fit test at 1 degree steps (0 only when rotation is locked). The
/// 16-angle sampler is too coarse to prove an item cannot fit. Returns the first
/// fitting angle and the rotated bbox centre.
fn fit_angle(vertices: &[Point], bed: Rect, allow_rotation: bool) -> Option<(f32, Point)> {
    let steps = if allow_rotation { 180 } else { 1 };
    (0..steps).find_map(|i| {
        let a = (i as f32).to_radians();
        let (s, c) = a.sin_cos();
        let (mut x0, mut x1, mut y0, mut y1) = (f32::MAX, f32::MIN, f32::MAX, f32::MIN);
        for p in vertices {
            let (x, y) = (p.0 * c - p.1 * s, p.0 * s + p.1 * c);
            x0 = x0.min(x);
            x1 = x1.max(x);
            y0 = y0.min(y);
            y1 = y1.max(y);
        }
        (x1 - x0 <= bed.width() && y1 - y0 <= bed.height())
            .then(|| (a, Point((x0 + x1) * 0.5, (y0 + y1) * 0.5)))
    })
}

fn to_spolygon(pts: &[(f64, f64)]) -> Option<SPolygon> {
    let mut v: Vec<Point> = Vec::with_capacity(pts.len());
    for &(x, y) in pts {
        if !x.is_finite() || !y.is_finite() {
            return None;
        }
        let p = Point(x as f32, y as f32);
        if v.last().is_none_or(|l| *l != p) {
            v.push(p);
        }
    }
    while v.len() > 1 && v[0] == v[v.len() - 1] {
        v.pop();
    }
    if v.len() < 3 {
        return None;
    }
    SPolygon::new(v).ok()
}

/// Items are centred on their centroid, as jagua-rs' importer does: `UniformBBoxSampler`
/// intersects translation and container ranges, which needs the shape to straddle the
/// origin. `centering` is undone in `to_header_pose`.
fn centering(shape: &SPolygon) -> Point {
    shape.centroid()
}

fn original(shape: SPolygon) -> OriginalShape {
    let c = centering(&shape);
    OriginalShape {
        shape,
        pre_transform: DTransformation::new(0.0, (-c.0, -c.1)),
        modify_mode: ShapeModifyMode::Inflate,
        modify_config: NO_MODIFY,
    }
}

fn make_item(id: usize, shape: SPolygon, rot: RotationRange) -> Option<Item> {
    Item::new(id, original(shape), rot, None, CDE_CONFIG.item_surrogate_config).ok()
}

/// Converts a jagua-rs placement of the *centred* shape back to the header's contract,
/// `world = rotate(outline, rotation) + (x, y)`.
///
/// jagua-rs places `outline - c` at `(r, t)`, giving
/// `world = rotate(outline, r) - rotate(c, r) + t`, so `(x, y) = t - rotate(c, r)`.
fn to_header_pose(dt: DTransformation, c: Point) -> (f64, f64, f64) {
    let r = dt.rotation();
    let (t_x, t_y) = dt.translation();
    let (sin, cos) = r.sin_cos();
    let x = t_x - (c.0 * cos - c.1 * sin);
    let y = t_y - (c.0 * sin + c.1 * cos);
    (f64::from(x), f64::from(y), f64::from(r))
}

/// Continuous is genuinely continuous: sparrow seeds 16 evenly spaced angles and then
/// refines the rotation by coordinate descent ("wiggle").
fn rotation_range(allow: bool) -> RotationRange {
    if allow { RotationRange::Continuous } else { RotationRange::None }
}

/// One movable item awaiting placement.
struct Candidate {
    out_idx: usize,
    shape: SPolygon,
    allow_rotation: bool,
    centering: Point,
}

/// Progress sink: `(bed_idx, movable items placed so far)`. Only invoked from the
/// thread that called [`arrange`]; the separator's workers have returned by then.
pub type Progress<'a> = &'a dyn Fn(i32, usize);

pub fn arrange(
    p: &Params,
    holes: &[Vec<(f64, f64)>],
    items: &[ItemIn],
    stop: &dyn Fn() -> bool,
    progress: Progress,
) -> Vec<Out> {
    let mut out = vec![Out::UNPLACED; items.len()];
    let bed_w = p.bed_w as f32;
    let bed_h = p.bed_h as f32;
    let max_beds = p.max_beds.max(1);
    let Ok(bed_rect) = Rect::try_new(0.0, 0.0, bed_w, bed_h) else {
        progress(0, 0);
        return out;
    };

    // Fixed items keep their pose and, with the holes, form each bed's forbidden region.
    let mut obstacles: Vec<Vec<SPolygon>> = vec![Vec::new(); max_beds as usize];
    let hole_shapes: Vec<SPolygon> = holes.iter().filter_map(|h| to_spolygon(h)).collect();
    for b in obstacles.iter_mut() {
        b.extend(hole_shapes.iter().cloned());
    }
    for (i, it) in items.iter().enumerate() {
        if !it.fixed {
            continue;
        }
        out[i] = Out { bed_idx: it.bed_idx, x: it.x, y: it.y, rotation: it.rotation };
        let Some(shape) = to_spolygon(&it.outline) else { continue };
        if it.bed_idx < 0 || it.bed_idx >= max_beds {
            continue;
        }
        let t = DTransformation::new(it.rotation as f32, (it.x as f32, it.y as f32)).compose();
        let mut placed = shape.clone();
        placed.transform_from(&shape, &t);
        obstacles[it.bed_idx as usize].push(placed);
    }

    // Movable items. Degenerate outlines never reach the solver and stay at -1.
    let mut pending: Vec<Candidate> = Vec::new();
    for (i, it) in items.iter().enumerate() {
        if it.fixed {
            continue;
        }
        let Some(shape) = to_spolygon(&it.outline) else { continue };
        // An item whose bounding box fits the bed at no angle fits on no bed at all.
        if make_item(0, shape.clone(), rotation_range(it.allow_rotation)).is_none()
            || fit_angle(&shape.vertices, bed_rect, it.allow_rotation).is_none()
        {
            continue;
        }
        let c = centering(&shape);
        pending.push(Candidate { out_idx: i, shape, allow_rotation: it.allow_rotation, centering: c });
    }
    if pending.is_empty() {
        progress(0, 0);
        return out;
    }

    // Largest first: the big pieces decide the layout, the small ones fill in.
    pending.sort_by(|a, b| {
        b.shape.area.partial_cmp(&a.shape.area).unwrap_or(std::cmp::Ordering::Equal)
    });

    // `time_limit_s` is per bed. A bed that places everything returns early.
    let bed_budget = p.time_limit_s.max(0.0);
    let mut rng = Xoshiro256PlusPlus::seed_from_u64(p.seed);
    let mut done = 0usize;
    let mut last_bed = 0i32;

    for bed in 0..max_beds {
        // Polled between beds as well as inside the separator, so a cancel aborts the whole
        // run rather than just the bed it landed in.
        if pending.is_empty() || stop() {
            break;
        }
        last_bed = bed;
        progress(bed, done);
        let base = done;
        let report = |n: usize| progress(bed, base + n);
        let (placed, leftover) = pack_bed(
            &pending,
            &obstacles[bed as usize],
            bed_w,
            bed_h,
            bed_budget,
            &mut rng,
            stop,
            &report,
        );
        // Gap fill: pack_bed tries only a handful of leftovers per bed, so give every
        // remaining item, largest first, a cheap collision-free placement before moving on.
        let mut placed = placed;
        let mut layout = obstacles[bed as usize].clone();
        for (idx, dt) in &placed {
            let (x, y, r) = to_header_pose(*dt, pending[*idx].centering);
            layout.push(world_outline(&pending[*idx].shape, x, y, r));
        }
        let filled = gap_fill(&pending, &leftover, &layout, bed_w, bed_h, &mut rng, stop);
        let leftover: Vec<usize> = leftover.iter().copied().filter(|i| !filled.iter().any(|(j, _)| j == i)).collect();
        placed.extend(filled);
        report(placed.len());
        done += placed.len();

        for (idx, dt) in placed {
            let (x, y, rotation) = to_header_pose(dt, pending[idx].centering);
            out[pending[idx].out_idx] = Out { bed_idx: bed, x, y, rotation };
        }
        // Keep the leftovers, preserving the largest-first order, for the next bed.
        let keep = leftover;
        let mut rest = Vec::with_capacity(keep.len());
        for (i, c) in pending.into_iter().enumerate() {
            if keep.binary_search(&i).is_ok() {
                rest.push(c);
            }
        }
        pending = rest;
    }
    consolidate(p, holes, items, &mut out, &mut rng, stop);
    fill_enclosed_openings(p, holes, items, &mut out, &mut rng, stop);
    progress(last_bed, done);
    out
}

fn opening_ancestors(mut host: usize, parents: &[Option<usize>]) -> Vec<usize> {
    let mut result = Vec::new();
    while let Some(parent) = parents[host] {
        if result.len() == parents.len() { break; }
        result.push(parent);
        host = parent;
    }
    result
}

/// Build an exact opening container with every relevant target-bed object registered
/// as a fixed hazard. Ancestors are omitted only after Sparrow has proved that the
/// descendant host fits inside their opening, so their solid rings cannot occupy it.
fn opening_layout(
    p: &Params,
    opening: SPolygon,
    host_idx: usize,
    ancestors: &[usize],
    shapes: &[Option<SPolygon>],
    bed_holes: &[SPolygon],
    out: &[Out],
    stop: &dyn Fn() -> bool,
) -> Option<Layout> {
    if opening.bbox.x_min < 0.0 || opening.bbox.y_min < 0.0
        || opening.bbox.x_max > p.bed_w as f32 || opening.bbox.y_max > p.bed_h as f32
    {
        return None;
    }
    let container = Container::new(
        host_idx,
        OriginalShape {
            shape: opening.clone(),
            pre_transform: DTransformation::empty(),
            modify_mode: ShapeModifyMode::Deflate,
            modify_config: NO_MODIFY,
        },
        vec![],
        CDE_CONFIG,
    ).ok()?;
    let mut layout = Layout::new(container);
    let target = out[host_idx].bed_idx;

    for (i, (shape, pose)) in shapes.iter().zip(out.iter()).enumerate() {
        if stop() { return None; }
        if i == host_idx || ancestors.contains(&i) || pose.bed_idx != target {
            continue;
        }
        let shape = shape.as_ref()?;
        let world = world_outline(shape, pose.x, pose.y, pose.rotation);
        if Rect::intersection(world.bbox, opening.bbox).is_none() {
            continue;
        }
        let obstacle = make_item(i, world.clone(), RotationRange::None)?;
        let c = centering(&world);
        layout.place_item(&obstacle, DTransformation::new(0.0, (c.0, c.1)));
    }
    for (i, shape) in bed_holes.iter().enumerate() {
        if stop() { return None; }
        if Rect::intersection(shape.bbox, opening.bbox).is_none() {
            continue;
        }
        let obstacle = make_item(shapes.len() + i, shape.clone(), RotationRange::None)?;
        let c = centering(shape);
        layout.place_item(&obstacle, DTransformation::new(0.0, (c.0, c.1)));
    }
    Some(layout)
}

/// Final, bounded cleanup. First nest opening-bearing objects, largest first; then
/// fill ordinary pieces from the deepest proven opening outward. Every host is handled
/// at most once per phase and every move goes to a lower bed index, preventing cycles.
fn fill_enclosed_openings(
    p: &Params,
    bed_holes: &[Vec<(f64, f64)>],
    items: &[ItemIn],
    out: &mut [Out],
    rng: &mut Xoshiro256PlusPlus,
    stop: &dyn Fn() -> bool,
) {
    if stop() {
        return;
    }
    let shapes: Vec<_> = items.iter().map(|it| to_spolygon(&it.outline)).collect();
    let Some(bed_holes): Option<Vec<_>> = bed_holes.iter().map(|h| to_spolygon(h)).collect() else {
        return;
    };
    let mut hosts: Vec<_> = items.iter().zip(out.iter()).enumerate()
        .filter(|(_, (it, pose))| pose.bed_idx >= 0 && !it.openings.is_empty())
        .map(|(i, _)| i)
        .collect();
    hosts.sort_by(|&a, &b| out[a].bed_idx.cmp(&out[b].bed_idx).then_with(|| {
        shapes[b].as_ref().map_or(0., |s| s.area)
            .total_cmp(&shapes[a].as_ref().map_or(0., |s| s.area))
    }));
    let mut parents = vec![None; items.len()];

    // Phase one: place larger rings before small fillers consume their parent opening.
    for &host_idx in &hosts {
        if stop() {
            return;
        }
        let host_pose = out[host_idx];
        let target = host_pose.bed_idx;
        let ancestors = opening_ancestors(host_idx, &parents);
        if !hosts.iter().any(|&i| !items[i].fixed && out[i].bed_idx > target) {
            continue;
        }
        for raw_opening in &items[host_idx].openings {
            if stop() {
                return;
            }
            let Some(local_opening) = to_spolygon(raw_opening) else { continue };
            let opening = world_outline(&local_opening, host_pose.x, host_pose.y, host_pose.rotation);
            let Some(mut layout) = opening_layout(
                p, opening.clone(), host_idx, &ancestors, &shapes, &bed_holes, out, stop,
            ) else { continue };
            let mut candidates: Vec<_> = hosts.iter().copied()
                .filter(|&i| i != host_idx && !items[i].fixed && out[i].bed_idx > target)
                .filter(|&i| shapes[i].as_ref().is_some_and(|s| s.area <= opening.area))
                .collect();
            candidates.sort_by(|&a, &b| shapes[b].as_ref().unwrap().area
                .total_cmp(&shapes[a].as_ref().unwrap().area));
            for idx in candidates {
                if stop() { return; }
                if out[idx].bed_idx <= target { continue; }
                let shape = shapes[idx].as_ref().unwrap();
                let Some(item) = make_item(idx, shape.clone(), rotation_range(items[idx].allow_rotation)) else {
                    continue;
                };
                let evaluator = LBFEvaluator::new(&layout, &item);
                if let (Some((dt, SampleEval::Clear { .. })), _) =
                    search_placement(&layout, &item, None, evaluator, LBF_SAMPLE_CONFIG, rng)
                {
                    let (x, y, rotation) = to_header_pose(dt, centering(shape));
                    out[idx] = Out { bed_idx: target, x, y, rotation };
                    parents[idx] = Some(host_idx);
                    layout.place_item(&item, dt);
                }
            }
        }
    }

    // Phase two: deepest nested openings get ordinary pieces before their ancestors.
    let mut fill_hosts = hosts;
    fill_hosts.sort_by_key(|&i| (
        out[i].bed_idx,
        std::cmp::Reverse(opening_ancestors(i, &parents).len()),
    ));
    for host_idx in fill_hosts {
        if stop() { return; }
        let host_pose = out[host_idx];
        let target = host_pose.bed_idx;
        if !items.iter().zip(out.iter())
            .any(|(it, pose)| !it.fixed && it.openings.is_empty() && pose.bed_idx > target)
        {
            continue;
        }
        let ancestors = opening_ancestors(host_idx, &parents);
        for raw_opening in &items[host_idx].openings {
            if stop() { return; }
            let Some(local_opening) = to_spolygon(raw_opening) else { continue };
            let opening = world_outline(&local_opening, host_pose.x, host_pose.y, host_pose.rotation);
            let Some(mut layout) = opening_layout(
                p, opening.clone(), host_idx, &ancestors, &shapes, &bed_holes, out, stop,
            ) else { continue };
            let mut candidates: Vec<_> = items.iter().zip(out.iter()).enumerate()
                .filter(|(_, (it, pose))| !it.fixed && it.openings.is_empty() && pose.bed_idx > target)
                .filter(|(i, _)| shapes[*i].as_ref().is_some_and(|s| s.area <= opening.area))
                .map(|(i, _)| i)
                .collect();
            candidates.sort_by(|&a, &b| shapes[a].as_ref().unwrap().area
                .total_cmp(&shapes[b].as_ref().unwrap().area));
            for idx in candidates {
                if stop() { return; }
                if out[idx].bed_idx <= target { continue; }
                let shape = shapes[idx].as_ref().unwrap();
                let Some(item) = make_item(idx, shape.clone(), rotation_range(items[idx].allow_rotation)) else {
                    continue;
                };
                let evaluator = LBFEvaluator::new(&layout, &item);
                if let (Some((dt, SampleEval::Clear { .. })), _) =
                    search_placement(&layout, &item, None, evaluator, LBF_SAMPLE_CONFIG, rng)
                {
                    let (x, y, rotation) = to_header_pose(dt, centering(shape));
                    out[idx] = Out { bed_idx: target, x, y, rotation };
                    layout.place_item(&item, dt);
                }
            }
        }
    }
}

/// Try to empty the last plate into earlier gaps without disturbing their occupants.
/// Only continue with the preceding plate if the current one emptied, avoiding an
/// expensive all-pairs search when the trailing plate still needs to exist.
pub(crate) fn consolidate(
    p: &Params,
    holes: &[Vec<(f64, f64)>],
    items: &[ItemIn],
    out: &mut [Out],
    rng: &mut Xoshiro256PlusPlus,
    stop: &dyn Fn() -> bool,
) {
    let last = items.iter().zip(out.iter()).filter(|(it, _)| !it.fixed)
        .map(|(_, pose)| pose.bed_idx).max().unwrap_or(0);
    if last <= 0 || stop() {
        return;
    }
    let shapes: Vec<_> = items.iter().map(|it| to_spolygon(&it.outline)).collect();
    let holes: Vec<_> = holes.iter().filter_map(|h| to_spolygon(h)).collect();
    for source in (1..=last).rev() {
        for target in 0..source {
            if stop() {
                break;
            }
            let mut layout = holes.clone();
            let mut candidates = Vec::new();
            for (i, ((it, pose), shape)) in items.iter().zip(out.iter()).zip(&shapes).enumerate() {
                let Some(shape) = shape else { continue };
                if pose.bed_idx == target {
                    layout.push(world_outline(shape, pose.x, pose.y, pose.rotation));
                } else if !it.fixed && pose.bed_idx == source {
                    candidates.push(Candidate {
                        out_idx: i, shape: shape.clone(), allow_rotation: it.allow_rotation,
                        centering: centering(shape),
                    });
                }
            }
            if candidates.is_empty() {
                break;
            }
            candidates.sort_by(|a, b| b.shape.area.total_cmp(&a.shape.area));
            let indices: Vec<_> = (0..candidates.len()).collect();
            let filled = gap_fill(&candidates, &indices, &layout, p.bed_w as f32, p.bed_h as f32, rng, stop);
            for (i, dt) in filled {
                let c = &candidates[i];
                let (x, y, rotation) = to_header_pose(dt, c.centering);
                out[c.out_idx] = Out { bed_idx: target, x, y, rotation };
            }
        }
        if stop() || out.iter().any(|pose| pose.bed_idx == source) {
            break;
        }
    }
}

/// The item's outline under the header pose `world = rotate(outline, rotation) + (x, y)`.
fn world_outline(shape: &SPolygon, x: f64, y: f64, rotation: f64) -> SPolygon {
    let t = DTransformation::new(rotation as f32, (x as f32, y as f32)).compose();
    let mut w = shape.clone();
    w.transform_from(shape, &t);
    w
}

/// Collision-free poses for as many of `leftover` (largest first) as LBF sampling can
/// find among `layout`, each placement becoming an obstacle for the next.
fn gap_fill(
    cands: &[Candidate],
    leftover: &[usize],
    layout: &[SPolygon],
    bed_w: f32,
    bed_h: f32,
    rng: &mut Xoshiro256PlusPlus,
    stop: &dyn Fn() -> bool,
) -> Vec<(usize, DTransformation)> {
    if leftover.is_empty() || stop() {
        return Vec::new();
    }
    let mut item_vec = Vec::with_capacity(leftover.len() + layout.len());
    for (k, &idx) in leftover.iter().enumerate() {
        if stop() { return Vec::new(); }
        let c = &cands[idx];
        let Some(it) = make_item(k, c.shape.clone(), rotation_range(c.allow_rotation)) else { return Vec::new() };
        item_vec.push(it);
    }
    for (k, o) in layout.iter().enumerate() {
        if stop() { return Vec::new(); }
        let Some(it) = make_item(leftover.len() + k, o.clone(), RotationRange::None) else { return Vec::new() };
        item_vec.push(it);
    }
    let Ok(strip) = Strip::new(bed_h, CDE_CONFIG, NO_MODIFY, bed_w) else { return Vec::new() };
    let instance = SPInstance::new(item_vec.iter().cloned().map(|i| (i, 1)).collect(), strip);
    let mut prob = SPProblem::new(instance);
    for (k, o) in layout.iter().enumerate() {
        let c = centering(o);
        prob.place_item(SPPlacement { item_id: leftover.len() + k, d_transf: DTransformation::new(0.0, (c.0, c.1)) });
    }
    let mut out = Vec::new();
    for (k, &idx) in leftover.iter().enumerate() {
        if stop() { break; }
        let evaluator = LBFEvaluator::new(&prob.layout, &item_vec[k]);
        if let (Some((dt, SampleEval::Clear { .. })), _) =
            search_placement(&prob.layout, &item_vec[k], None, evaluator, LBF_SAMPLE_CONFIG, rng)
        {
            prob.place_item(SPPlacement { item_id: k, d_transf: dt });
            out.push((idx, dt));
        }
    }
    out
}

/// Fills one bed. Returns the placed candidates (index into `cands`, pose) and the sorted
/// indices that did not fit.
fn pack_bed(
    cands: &[Candidate],
    obstacles: &[SPolygon],
    bed_w: f32,
    bed_h: f32,
    budget_s: f64,
    rng: &mut Xoshiro256PlusPlus,
    stop: &dyn Fn() -> bool,
    report: &dyn Fn(usize),
) -> (Vec<(usize, DTransformation)>, Vec<usize>) {
    let n_movable = cands.len();
    let all_leftover = || (Vec::new(), (0..n_movable).collect::<Vec<_>>());

    // Movable items take ids 0..n_movable; obstacles follow. The separator treats every id
    // at or above `n_movable` as pinned (see the vendored tracker patch).
    let mut item_vec: Vec<Item> = Vec::with_capacity(n_movable + obstacles.len());
    for (id, c) in cands.iter().enumerate() {
        match make_item(id, c.shape.clone(), rotation_range(c.allow_rotation)) {
            Some(it) => item_vec.push(it),
            None => return all_leftover(),
        }
    }
    for (k, o) in obstacles.iter().enumerate() {
        match make_item(n_movable + k, o.clone(), RotationRange::None) {
            Some(it) => item_vec.push(it),
            None => return all_leftover(),
        }
    }

    let Ok(strip) = Strip::new(bed_h, CDE_CONFIG, NO_MODIFY, bed_w) else {
        return all_leftover();
    };
    let instance = SPInstance::new(item_vec.iter().cloned().map(|i| (i, 1)).collect(), strip);
    let mut prob = SPProblem::new(instance.clone());

    // Obstacles are in bed coordinates; placing them at their centring offset restores them.
    for (k, o) in obstacles.iter().enumerate() {
        let c = centering(o);
        prob.place_item(SPPlacement {
            item_id: n_movable + k,
            d_transf: DTransformation::new(0.0, (c.0, c.1)),
        });
    }

    // Start near capacity: an over-full bed burns the budget shedding items. Items
    // beyond the target are deferred and added back if they fit.
    let free_area = bed_w * bed_h - obstacles.iter().map(|o| o.area).sum::<f32>();
    let mut placed_ids: Vec<usize> = Vec::new();
    let mut deferred: Vec<usize> = Vec::new();
    let mut acc = 0.0;
    for id in 0..n_movable {
        let a = cands[id].shape.area;
        if acc + a <= free_area * TARGET_FILL {
            acc += a;
            placed_ids.push(id);
        } else {
            deferred.push(id);
        }
    }

    let container_bbox = prob.layout.container.outer_cd.bbox;
    for &id in &placed_ids {
        let dt = warm_start_pose(&prob, &item_vec[id], container_bbox, rng);
        prob.place_item(SPPlacement { item_id: id, d_transf: dt });
    }

    let sep_config = SeparatorConfig {
        iter_no_imprv_limit: 200,
        strike_limit: 3,
        n_workers: 3,
        log_level: log::Level::Debug,
        sample_config: SEP_SAMPLE_CONFIG,
        n_movable,
    };
    let seed: u64 = rng.random();
    let mut sep =
        Separator::new(instance, prob, Xoshiro256PlusPlus::seed_from_u64(seed), sep_config);

    let bed_deadline = Instant::now() + Duration::from_secs_f64(budget_s);
    let mut listener = DummySolListener;
    // The initial fill, and the last deferred item, keep separating while overlap still
    // drops: evicting the moment a slice ran out shed items that would have fit a moment
    // later. Other adds get one slice each, so a hopeless one cannot burn the budget the
    // remaining deferred items need.
    let slice = Duration::from_secs_f64(budget_s / f64::from(MAX_ATTEMPTS));
    let mut prev_loss = f32::INFINITY;
    let mut flat_slices = 0u32;
    let mut attempts = 0u32;

    // Keep the best feasible layout, ranked by (items placed, area), so more budget
    // never does worse and reported progress stays monotone.
    let mut best: Option<(Vec<(usize, DTransformation)>, Vec<usize>)> = None;
    let mut best_rank = (0usize, -1.0f32);
    // Give the first feasible layout a full observation window. The existing
    // per-bed budget always wins when it is shorter than this patience period.
    let patience = Duration::from_secs_f64((budget_s / 4.0).clamp(0.25, 1.0));
    let mut fill_progress = FillProgress::new(Instant::now(), patience);

    loop {
        let now = Instant::now();
        if fill_progress.deadline().is_some_and(|deadline| now >= deadline) {
            break;
        }
        let search_deadline = fill_progress.deadline().unwrap_or(bed_deadline).min(bed_deadline);
        let slice_end = (now + slice).min(search_deadline);
        let term = Deadline { until: slice_end, stop };
        let (sol, cts) = sep.separate(&term, &mut listener);
        sep.rollback(&sol, Some(&cts));
        // Returned early: the separator struck out on its own.
        let stalled = Instant::now() < slice_end;
        let loss = sep.ct.get_total_loss();

        if loss == 0.0 {
            prev_loss = f32::INFINITY;
            flat_slices = 0;
            let (placed, leftover) = harvest(&sep, n_movable);
            let area: f32 = placed.iter().map(|(i, _)| cands[*i].shape.area).sum();
            let rank = (placed.len(), area);
            if rank > best_rank {
                if rank.0 > best_rank.0 {
                    report(rank.0);
                }
                best_rank = rank;
                fill_progress.observe(area, Instant::now());
                best = Some((placed, leftover));
            }
            // Feasible with time to spare: try to fit one more.
            attempts += 1;
            match deferred.pop() {
                Some(id) if attempts < MAX_ATTEMPTS && Instant::now() < bed_deadline && !stop() => {
                    let dt = warm_start_pose(&sep.prob, &item_vec[id], container_bbox, rng);
                    sep.prob.place_item(SPPlacement { item_id: id, d_transf: dt });
                    sep.resync();
                }
                _ => break,
            }
        } else {
            if Instant::now() >= bed_deadline || stop() {
                break;
            }
            // Tight layouts plateau for a slice before the last move, so allow one.
            flat_slices = if loss < prev_loss { 0 } else { flat_slices + 1 };
            if (attempts == 0 || deferred.is_empty()) && !stalled && flat_slices < 2 {
                prev_loss = loss.min(prev_loss);
                continue;
            }
            prev_loss = f32::INFINITY;
            flat_slices = 0;
            attempts += 1;
            if attempts >= MAX_ATTEMPTS {
                break;
            }
            match worst_item(&sep) {
                Some(pk) => sep.evict_item(pk),
                None => break,
            }
        }
    }

    match best {
        Some(r) => r,
        // Never separated: shed the worst offenders until the layout is overlap-free.
        None => {
            while sep.ct.get_total_loss() > 0.0 {
                if stop() {
                    return all_leftover();
                }
                match worst_item(&sep) {
                    Some(pk) => sep.evict_item(pk),
                    None => return all_leftover(),
                }
            }
            harvest(&sep, n_movable)
        }
    }
}

/// LBF where the item fits cleanly, otherwise anywhere in the bed. Overlap is the
/// separator's job.
fn warm_start_pose(
    prob: &SPProblem,
    item: &Item,
    container_bbox: Rect,
    rng: &mut Xoshiro256PlusPlus,
) -> DTransformation {
    let evaluator = LBFEvaluator::new(&prob.layout, item);
    let (best, _) = search_placement(&prob.layout, item, None, evaluator, LBF_SAMPLE_CONFIG, rng);
    match best {
        Some((dt, SampleEval::Clear { .. })) => dt,
        _ => match UniformBBoxSampler::new(container_bbox, item, container_bbox) {
            Some(s) => s.sample(rng),
            // Fits only between the sampler's angles: seed it there so descent can refine.
            None => {
                let allow = item.allowed_rotation == RotationRange::Continuous;
                match fit_angle(&item.shape_cd.vertices, container_bbox, allow) {
                    Some((a, bc)) => {
                        let cc = container_bbox.centroid();
                        DTransformation::new(a, (cc.0 - bc.0, cc.1 - bc.1))
                    }
                    None => DTransformation::empty(),
                }
            }
        },
    }
}

/// Next item to shed: most residual overlap per unit area. "Most overlap" evicts
/// big items; "smallest area" sheds bystanders. The ratio beats both.
fn worst_item(sep: &Separator) -> Option<jagua_rs::entities::PItemKey> {
    sep.prob
        .layout
        .placed_items
        .iter()
        .filter(|(pk, _)| !sep.ct.is_pinned(*pk))
        .max_by(|(a, pa), (b, pb)| {
            let ra = sep.ct.get_loss(*a) / pa.shape.area.max(f32::MIN_POSITIVE);
            let rb = sep.ct.get_loss(*b) / pb.shape.area.max(f32::MIN_POSITIVE);
            ra.partial_cmp(&rb).unwrap_or(std::cmp::Ordering::Equal)
        })
        .map(|(pk, _)| pk)
}

fn harvest(sep: &Separator, n_movable: usize) -> (Vec<(usize, DTransformation)>, Vec<usize>) {
    let mut placed = Vec::new();
    let mut seen = vec![false; n_movable];
    for pi in sep.prob.layout.placed_items.values() {
        if pi.item_id < n_movable {
            placed.push((pi.item_id, pi.d_transf));
            seen[pi.item_id] = true;
        }
    }
    let leftover = (0..n_movable).filter(|i| !seen[*i]).collect();
    (placed, leftover)
}

#[cfg(test)]
mod fill_progress_tests {
    use super::*;

    #[test]
    fn no_plateau_deadline_without_a_nonempty_feasible_layout() {
        let now = Instant::now();
        let mut progress = FillProgress::new(now, Duration::from_secs(1));
        for area in [0.0, -1.0, f32::NAN, f32::INFINITY] {
            progress.observe(area, now);
            assert!(progress.deadline().is_none());
        }
    }

    #[test]
    fn flat_or_worse_fill_does_not_extend_search() {
        let now = Instant::now();
        let window = Duration::from_secs(1);
        let mut progress = FillProgress::new(now, window);
        progress.observe(100.0, now);
        progress.observe(100.0, now + Duration::from_millis(400));
        progress.observe(90.0, now + Duration::from_millis(800));
        assert_eq!(progress.deadline(), Some(now + window));
    }

    #[test]
    fn small_gains_accumulate_but_do_not_individually_reset_patience() {
        let now = Instant::now();
        let mut progress = FillProgress::new(now, Duration::from_secs(1));
        progress.observe(100.0, now);
        progress.observe(100.4, now + Duration::from_millis(400));
        progress.observe(100.8, now + Duration::from_millis(800));
        assert_eq!(progress.deadline(), Some(now + Duration::from_secs(1)));
        progress.observe(101.2, now + Duration::from_millis(900));
        assert_eq!(progress.deadline(), Some(now + Duration::from_millis(1900)));
    }

    #[test]
    fn improvement_is_relative_to_fill_not_absolute_area() {
        let now = Instant::now();
        for scale in [0.1, 1.0, 1000.0] {
            let mut progress = FillProgress::new(now, Duration::from_secs(1));
            progress.observe(100.0 * scale, now);
            progress.observe(100.5 * scale, now + Duration::from_millis(300));
            assert_eq!(progress.deadline(), Some(now + Duration::from_secs(1)));
            progress.observe(102.0 * scale, now + Duration::from_millis(800));
            assert_eq!(progress.deadline(), Some(now + Duration::from_millis(1800)));
        }
    }
}
