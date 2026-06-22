// Phase 5.3 — @orca/plugin-types public surface.

export type {
  EventKind,
  EventPayload,
  EventSubscriptionId,
  ExportFinishedPayload,
  ExportHandle,
  LoadHandle,
  SliceHandle,
  SlicingFinishedPayload,
  SlicingProgressPayload,
} from './events';

export {
  PERMISSION_BITS,
  permissionsToBits,
} from './permissions';
export type { PermissionName } from './permissions';

export * as presets from './presets';

export {
  defineDevicePlugin,
  defineWebviewPlugin,
} from './device';
export type {
  DefineDeviceArgs,
  DefineWebviewArgs,
  DeviceAgentLifecycle,
  DeviceConnection,
  DevicePlugin,
  DeviceState,
  PluginIdentity,
  WebviewLifecycle,
  WebviewPlugin,
} from './device';

export type {
  BaseField,
  BoolField,
  EnumField,
  FieldType,
  FloatField,
  GroupField,
  IntField,
  SettingsField,
  SettingsSchema,
  StringField,
} from './settings-schema';
export { isFieldType } from './settings-schema';

// Ambient declarations — pulling this file into a project also brings
// `window.orca` and `window.__orca_resolve` types into global scope.
import './window-orca';
