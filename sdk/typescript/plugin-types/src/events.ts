// Phase 5.3.2 — typed event kinds + payload lookup.
//
// Each `EventKind` value mirrors the engine's `orca_event_kind_t` enum
// (engine/include/orca/c_api.h) and the JS strings used by
// `window.orca.events.on(kind, ...)` in WebViewPluginHost. Plugin
// authors get strong typing on event payloads via the `EventPayload<K>`
// conditional-mapped type.

/**
 * Event kind names that the engine emits. Matches the strings the
 * WebViewPluginHost dispatcher recognizes — see
 * `src/slic3r/Utils/WebViewPluginHost.cpp::SubscribeEvent`.
 */
export type EventKind =
  | 'SlicingProgress'
  | 'SlicingFinished'
  | 'ExportFinished';

/**
 * Slice / export / load handle types. The engine uses opaque u64
 * identifiers; in JS they round-trip as `number` (safe for slice
 * counts up to 2^53).
 */
export type SliceHandle  = number;
export type ExportHandle = number;
export type LoadHandle   = number;

export interface SlicingProgressPayload {
  handle: SliceHandle;
  progress: number;
  message: string;
}

export interface SlicingFinishedPayload {
  handle: SliceHandle;
  success: boolean;
  error: string;
}

export interface ExportFinishedPayload {
  handle: ExportHandle;
  success: boolean;
  line_count: number;
  error: string;
}

/**
 * Conditional-mapped lookup: `EventPayload<'SlicingFinished'>` is
 * `SlicingFinishedPayload`. Use to type callbacks at the call site.
 */
export type EventPayload<K extends EventKind> =
  K extends 'SlicingProgress' ? SlicingProgressPayload :
  K extends 'SlicingFinished' ? SlicingFinishedPayload :
  K extends 'ExportFinished'  ? ExportFinishedPayload  :
  never;

/**
 * Handler returned by `window.orca.events.on` — opaque numeric id
 * that the host returns + the plugin passes back to `events.off`.
 */
export type EventSubscriptionId = number;
