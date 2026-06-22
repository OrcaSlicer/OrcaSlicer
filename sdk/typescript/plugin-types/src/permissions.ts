// Phase 5.3 — ORCA_PERM_* bitmask + string-name aliases.

export type PermissionName =
  | 'network'
  | 'filesystem_read'
  | 'filesystem_write'
  | 'settings_read'
  | 'settings_write'
  | 'profiles_install'
  | 'device_control'
  | 'slice_intercept'
  | 'gcode_modify'
  | 'ui_attach'
  | 'events_raw';

export const PERMISSION_BITS: Record<PermissionName, number> = {
  network:          1 << 0,
  filesystem_read:  1 << 1,
  filesystem_write: 1 << 2,
  settings_read:    1 << 3,
  settings_write:   1 << 4,
  profiles_install: 1 << 5,
  device_control:   1 << 6,
  slice_intercept:  1 << 7,
  gcode_modify:     1 << 8,
  ui_attach:        1 << 9,
  events_raw:       1 << 10,
};

export function permissionsToBits(perms: PermissionName[]): number {
  let bits = 0;
  for (const p of perms) bits |= PERMISSION_BITS[p];
  return bits;
}
