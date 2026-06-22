// Phase 5.3.4 — async preset accessors wrapping window.orca.presets.
//
// Backed by the presets_get_string / presets_set_string host actions
// added to WebViewPluginHost in Phase 5.3. Strings are JSON-encoded
// across the wire so the bridge can distinguish absent (null) from
// empty ("").

import './window-orca';

/**
 * Read a string preset by key. Resolves with `null` when the key does
 * not exist on the active config; rejects with `permission_denied`
 * when the plugin lacks ORCA_PERM_SETTINGS_READ.
 */
export async function getString(key: string): Promise<string | null> {
  if (typeof window === 'undefined' || !window.orca?.presets) {
    throw new Error('window.orca.presets is not available');
  }
  return window.orca.presets.getString(key);
}

/**
 * Set a string preset by key. Rejects with `permission_denied`
 * when the plugin lacks ORCA_PERM_SETTINGS_WRITE.
 */
export async function setString(key: string, value: string): Promise<void> {
  if (typeof window === 'undefined' || !window.orca?.presets) {
    throw new Error('window.orca.presets is not available');
  }
  await window.orca.presets.setString(key, value);
}
