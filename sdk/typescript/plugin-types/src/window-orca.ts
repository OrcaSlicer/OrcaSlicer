// Phase 5.3.5 — `window.orca.*` ambient global types.
//
// Mirrors the JS shim WebViewPluginHost.cpp injects via AddUserScript.
// Every method returns a Promise that resolves with the host's reply
// (`window.__orca_resolve`) or rejects with the host's error string.

import type {
  EventKind,
  EventPayload,
  EventSubscriptionId,
} from './events';

declare global {
  interface Window {
    orca: OrcaApi;

    /** @internal — invoked by the host to resolve a pending call. */
    __orca_resolve(
      requestId: number,
      result:    unknown,
      error:     string | null,
    ): void;

    /** @internal — invoked by the host to dispatch an event. */
    __orca_event_dispatch(
      jsSubId: number,
      payload: unknown,
    ): void;
  }
}

export interface OrcaApi {
  log(level: 0 | 1 | 2 | 3 | 4, message: string): Promise<null>;
  checkPermission(bit: number): Promise<boolean>;
  placeholderSetString(name: string, value: string): Promise<null>;
  placeholderSetInt(name: string, value: number):    Promise<null>;
  placeholderSetFloat(name: string, value: number):  Promise<null>;
  loadProfilePack(dir: string): Promise<null>;

  events: {
    on<K extends EventKind>(
      kind:    K,
      handler: (payload: EventPayload<K>) => void,
    ): EventSubscriptionId;

    off(id: EventSubscriptionId): Promise<null>;
  };

  presets: {
    getString(key: string): Promise<string | null>;
    setString(key: string, value: string): Promise<null>;
  };
}

// Without `export` the file becomes a script, and ambient `declare
// global` blocks have to live in a module. Re-exporting the OrcaApi
// type covers both bases.
