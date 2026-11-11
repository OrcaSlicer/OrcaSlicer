//! C ABI for the sparrow_arrange nesting library. Mirrors include/sparrow_arrange.h.
//!
//! Every entry point is wrapped in `catch_unwind`: unwinding across the FFI boundary is
//! undefined behaviour, so a panic is converted into a non-zero return instead.

#![allow(non_camel_case_types)]

pub mod pack;

#[cfg(test)]
mod tests;

use pack::{FilamentRules, ItemIn, Params};
pub use pack::Out as sp_placement;
use std::os::raw::{c_int, c_void};
use std::panic::{AssertUnwindSafe, catch_unwind};

#[repr(C)]
#[derive(Clone, Copy)]
pub struct sp_point {
    pub x: f64,
    pub y: f64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct sp_polygon {
    pub pts: *const sp_point,
    pub n: usize,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct sp_item {
    pub outline: sp_polygon,
    pub openings: *const sp_polygon,
    pub n_openings: usize,
    pub fixed: c_int,
    pub bed_idx: c_int,
    pub x: f64,
    pub y: f64,
    pub rotation: f64,
    pub allow_rotation: c_int,
    pub filaments: u64,
    pub tower: c_int,
}

#[repr(C)]
pub struct sp_input {
    pub bed_w: f64,
    pub bed_h: f64,
    pub holes: *const sp_polygon,
    pub n_holes: usize,
    pub items: *const sp_item,
    pub n_items: usize,
    pub max_beds: c_int,
    pub time_limit_s: f64,
    pub seed: u64,
    pub should_stop: Option<unsafe extern "C" fn(*mut c_void) -> c_int>,
    pub user: *mut c_void,
    pub on_progress: Option<unsafe extern "C" fn(*mut c_void, c_int, c_int, c_int)>,
    pub on_layout: Option<unsafe extern "C" fn(*mut c_void, *const sp_placement, usize)>,
    pub allow_mixing: c_int,
    pub max_filaments_per_bed: c_int,
    pub incompatible: *const u64,
    pub n_incompatible: usize,
}

const OK: c_int = 0;
const ERR_INVALID: c_int = 1;
const ERR_PANIC: c_int = 2;

/// The `n` elements at `p`, or none if `p` is null.
unsafe fn slice<'a, T>(p: *const T, n: usize) -> &'a [T] {
    if p.is_null() || n == 0 { &[] } else { unsafe { std::slice::from_raw_parts(p, n) } }
}

unsafe fn read_polygon(p: &sp_polygon) -> Vec<(f64, f64)> {
    unsafe { slice(p.pts, p.n) }.iter().map(|q| (q.x, q.y)).collect()
}

/// # Safety
/// `in_` must point to a valid `sp_input` and `out` to at least `in_->n_items`
/// writable `sp_placement`s.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sparrow_arrange(in_: *const sp_input, out: *mut sp_placement) -> c_int {
    catch_unwind(AssertUnwindSafe(|| unsafe { run(in_, out) })).unwrap_or(ERR_PANIC)
}

unsafe fn run(in_: *const sp_input, out: *mut sp_placement) -> c_int {
    if in_.is_null() || out.is_null() {
        return ERR_INVALID;
    }
    let inp = unsafe { &*in_ };
    if !inp.bed_w.is_finite() || !inp.bed_h.is_finite() || inp.bed_w <= 0.0 || inp.bed_h <= 0.0 {
        return ERR_INVALID;
    }
    if inp.max_beds < 1 {
        return ERR_INVALID;
    }
    if (inp.n_items > 0 && inp.items.is_null()) || (inp.n_holes > 0 && inp.holes.is_null()) {
        return ERR_INVALID;
    }

    let raw_items = unsafe { slice(inp.items, inp.n_items) };
    if raw_items.iter().any(|it| it.n_openings > 0 && it.openings.is_null()) {
        return ERR_INVALID;
    }
    let holes: Vec<Vec<(f64, f64)>> = unsafe { slice(inp.holes, inp.n_holes) }.iter().map(|h| unsafe { read_polygon(h) }).collect();
    let items: Vec<ItemIn> = raw_items
        .iter()
        .map(|it| ItemIn {
            outline: unsafe { read_polygon(&it.outline) },
            openings: unsafe { slice(it.openings, it.n_openings) }.iter().map(|p| unsafe { read_polygon(p) }).collect(),
            fixed: it.fixed != 0,
            bed_idx: it.bed_idx,
            x: it.x,
            y: it.y,
            rotation: it.rotation,
            allow_rotation: it.allow_rotation != 0,
            filaments: it.filaments,
            tower: it.tower != 0,
        })
        .collect();

    let user = inp.user;
    let cb = inp.should_stop;
    let stop = move || -> bool {
        match cb {
            Some(f) => (unsafe { f(user) }) != 0,
            None => false,
        }
    };

    // Reported only from this thread: `pack::arrange` never hands the sink to a worker.
    let total_movable = raw_items.iter().filter(|it| it.fixed == 0).count();
    let on_progress = inp.on_progress;
    let progress = move |bed_idx: i32, placed: usize| {
        if let Some(f) = on_progress {
            unsafe { f(user, bed_idx, placed as c_int, total_movable as c_int) };
        }
    };

    let params = Params {
        bed_w: inp.bed_w,
        bed_h: inp.bed_h,
        max_beds: inp.max_beds,
        time_limit_s: inp.time_limit_s,
        seed: inp.seed,
        filaments: FilamentRules {
            allow_mixing: inp.allow_mixing != 0,
            max_per_bed: if inp.max_filaments_per_bed > 0 { inp.max_filaments_per_bed as u32 } else { u32::MAX },
            incompatible: unsafe { slice(inp.incompatible, inp.n_incompatible) }.to_vec(),
        },
    };

    let on_layout = inp.on_layout;
    let layout = move |snapshot: &[sp_placement]| {
        if let Some(f) = on_layout {
            unsafe { f(user, snapshot.as_ptr(), snapshot.len()) };
        }
    };

    let placements = pack::arrange_live(&params, &holes, &items, &stop, &progress, &layout);
    unsafe { std::slice::from_raw_parts_mut(out, inp.n_items) }.copy_from_slice(&placements);
    OK
}
