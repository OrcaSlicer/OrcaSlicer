// Phase 5.3.3 — defineDevicePlugin + defineWebviewPlugin factories.
//
// Authors call these from the entry point of their plugin's JS bundle
// to declare its identity + lifecycle hooks. The host wires them
// through the same ORCA_SLOT_* surface a native plugin would use.

import type { PermissionName } from './permissions';

/**
 * Common identity fields every plugin manifests.
 */
export interface PluginIdentity {
  id:           string;
  name:         string;
  version:      string;
  author?:      string;
  description?: string;
  permissions?: PermissionName[];
}

// ---------- Webview plugin --------------------------------------------

/**
 * Hooks fired by WebViewPluginHost in response to JS-side activity.
 * Every callback is async-able; throwing is caught and logged.
 */
export interface WebviewLifecycle {
  /** Page just loaded and `window.orca` is available. */
  onMount?(): void | Promise<void>;
  /** WebView is about to be torn down. Clean up subscriptions. */
  onUnmount?(): void | Promise<void>;
}

export interface DefineWebviewArgs extends PluginIdentity, WebviewLifecycle {}

export interface WebviewPlugin {
  identity: PluginIdentity;
  mount(): void;
  unmount(): void;
}

/**
 * Build a webview plugin instance from declarative args. Authors put
 * the result on `window.__orcaPlugin` in their entry script; the host
 * calls mount/unmount through the window.orca bridge.
 */
export function defineWebviewPlugin(args: DefineWebviewArgs): WebviewPlugin {
  const identity: PluginIdentity = {
    id:      args.id,
    name:    args.name,
    version: args.version,
    ...(args.author      !== undefined && { author:      args.author }),
    ...(args.description !== undefined && { description: args.description }),
    ...(args.permissions !== undefined && { permissions: args.permissions }),
  };

  return {
    identity,
    mount() {
      void args.onMount?.();
    },
    unmount() {
      void args.onUnmount?.();
    },
  };
}

// ---------- Device-agent plugin (called from a webview script) -------

/**
 * Wire-shape mirroring orca::PrinterAgent's connection schema. Used as
 * the input to defineDevicePlugin's `connect` callback.
 */
export interface DeviceConnection {
  deviceId:   string;
  hostOrIp:   string;
  port:       number;
  username?:  string;
  password?:  string;
  useTls:     boolean;
}

export interface DeviceAgentLifecycle {
  connect(conn: DeviceConnection): void | Promise<void>;
  disconnect(): void | Promise<void>;

  /** Plain-text or binary command to send to the printer. */
  sendCommand(payload: Uint8Array | string): void | Promise<void>;

  startPrint(gcodePath: string, jobName: string, startImmediately: boolean):
    void | Promise<void>;
  cancelPrint(): void | Promise<void>;

  /** 'disconnected' | 'connecting' | 'connected' | 'error' */
  currentState(): DeviceState;
}

export type DeviceState = 'disconnected' | 'connecting' | 'connected' | 'error';

export interface DefineDeviceArgs extends PluginIdentity, DeviceAgentLifecycle {
  /** Stable internal id the engine uses to look up this agent type. */
  agentId: string;
}

export interface DevicePlugin {
  identity: PluginIdentity;
  agentId:  string;
  hooks:    DeviceAgentLifecycle;
}

export function defineDevicePlugin(args: DefineDeviceArgs): DevicePlugin {
  return {
    identity: {
      id:      args.id,
      name:    args.name,
      version: args.version,
      ...(args.author      !== undefined && { author:      args.author }),
      ...(args.description !== undefined && { description: args.description }),
      ...(args.permissions !== undefined && { permissions: args.permissions }),
    },
    agentId:  args.agentId,
    hooks: {
      connect:      args.connect.bind(args),
      disconnect:   args.disconnect.bind(args),
      sendCommand:  args.sendCommand.bind(args),
      startPrint:   args.startPrint.bind(args),
      cancelPrint:  args.cancelPrint.bind(args),
      currentState: args.currentState.bind(args),
    },
  };
}
