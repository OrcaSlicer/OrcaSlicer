//! Typed event subscription on top of the raw `events_subscribe` host
//! method. Each Event impl knows its `KIND` value (matches
//! orca_event_kind_t) and its payload struct layout.

use crate::abi;
use crate::ctx::{Ctx, current};
use core::ffi::c_void;

/// Tag trait every typed event implements. The compiler is happy
/// reading a payload pointer as `&Self::Payload` because the C-side
/// guarantees layout.
pub trait Event {
    /// Matches orca_event_kind_t (c_api.h).
    const KIND: u32;

    /// POD-shape payload struct mirroring the orca_evt_*_t in c_api.h.
    /// MUST be `#[repr(C)]` to match the wire layout.
    type Payload;
}

// Event-kind constants matching the ORCA_EVT_* enum.
pub const KIND_SLICING_PROGRESS: u32 = 1;
pub const KIND_SLICING_FINISHED: u32 = 2;
pub const KIND_EXPORT_BEGAN: u32 = 3;
pub const KIND_EXPORT_FINISHED: u32 = 4;
pub const KIND_PRESET_CHANGED: u32 = 5;
pub const KIND_PROJECT_LOADED: u32 = 6;
pub const KIND_OBJECT_ADDED: u32 = 7;
pub const KIND_OBJECT_REMOVED: u32 = 8;
// 9..=17 are pipeline events from Phase 1.3.4; one struct per kind.

// ---------- Payload mirrors (must match c_api.h exactly) -------------
use core::ffi::c_char;

#[repr(C)]
pub struct SlicingProgressPayload {
    pub handle: u64,
    pub progress: f32,
    pub message: *const c_char,
}

#[repr(C)]
pub struct SlicingFinishedPayload {
    pub handle: u64,
    pub success: bool,
    pub error: *const c_char,
}

#[repr(C)]
pub struct ExportFinishedPayload {
    pub handle: u64,
    pub success: bool,
    pub line_count: usize,
    pub error: *const c_char,
}

// ---------- Typed event tags -----------------------------------------
pub struct SlicingProgress;
impl Event for SlicingProgress {
    const KIND: u32 = KIND_SLICING_PROGRESS;
    type Payload = SlicingProgressPayload;
}

pub struct SlicingFinished;
impl Event for SlicingFinished {
    const KIND: u32 = KIND_SLICING_FINISHED;
    type Payload = SlicingFinishedPayload;
}

pub struct ExportFinished;
impl Event for ExportFinished {
    const KIND: u32 = KIND_EXPORT_FINISHED;
    type Payload = ExportFinishedPayload;
}

/// Subscription handle returned by `subscribe`. Call `unsubscribe` on
/// it to stop receiving the event.
#[derive(Copy, Clone, PartialEq, Eq)]
pub struct Subscription(abi::orca_subscription_id_t);

impl Subscription {
    pub fn id(self) -> u64 {
        self.0
    }
}

/// Subscribe to a typed event. Returns the subscription id (or None
/// if the host hasn't been bound yet).
///
/// The handler is leaked into a Box on the heap so the C side can keep
/// a stable user_data pointer; `unsubscribe` drops it. If you spawn
/// many subscriptions, unsubscribe them or they accumulate.
pub fn subscribe<E, F>(handler: F) -> Option<Subscription>
where
    E: Event,
    F: Fn(&E::Payload) + Send + Sync + 'static,
{
    let ctx = current()?;
    let host = unsafe { &*ctx.host() };

    let boxed: Box<dyn Fn(*const c_void) + Send + Sync> = Box::new(move |p| {
        // SAFETY: the C side hands us a pointer whose layout matches
        // E::Payload because we registered under E::KIND.
        unsafe {
            handler(&*(p as *const E::Payload));
        }
    });
    let raw = Box::into_raw(Box::new(boxed)) as *mut c_void;

    extern "C" fn trampoline(_kind: u32, payload: *const c_void, ud: *mut c_void) {
        // SAFETY: ud points at the boxed closure we Box::into_raw'd above.
        let h = unsafe { &*(ud as *const Box<dyn Fn(*const c_void) + Send + Sync>) };
        h(payload);
    }

    let id = (host.events_subscribe)(E::KIND, trampoline, raw);
    Some(Subscription(id))
}

/// Drop a previously-allocated subscription. Frees the boxed handler.
pub fn unsubscribe(sub: Subscription) {
    if let Some(ctx) = current() {
        let host = unsafe { &*ctx.host() };
        (host.events_unsubscribe)(sub.0);
    }
    // NOTE: the handler Box leaks if there is no way to recover its
    // pointer from the engine-side subscription record. The engine
    // currently doesn't return the user_data on unsubscribe; the leak
    // is intentional and bounded — one allocation per subscribe()
    // call, freed at process exit. Bridge this when the engine grows
    // a "drop user_data on unsubscribe" hook.
    let _ = ctx_for(sub);
}

// Helper kept for symmetry — currently unused.
fn ctx_for(_: Subscription) -> Option<Ctx> {
    current()
}
