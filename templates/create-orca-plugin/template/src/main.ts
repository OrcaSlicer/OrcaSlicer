// {{plugin_name}} entry point. Runs inside WebViewPluginHost; the
// window.orca bridge is injected before this script evaluates.

import { defineWebviewPlugin } from '@orca/plugin-types';

const plugin = defineWebviewPlugin({
  id:          '{{plugin_id}}',
  name:        '{{plugin_name}}',
  version:     '0.1.0',
  description: '{{plugin_description}}',
  permissions: [],

  async onMount() {
    await window.orca.log(2 /* info */, '{{plugin_name}} mounted');
    const status = document.getElementById('status');
    if (status) status.textContent = 'Hooked into Orca.';

    window.orca.events.on('SlicingFinished', (payload) => {
      const text = `slice #${payload.handle} ` +
                   (payload.success ? 'finished' : `failed: ${payload.error}`);
      if (status) status.textContent = text;
    });
  },

  async onUnmount() {
    await window.orca.log(2, '{{plugin_name}} unmounted');
  },
});

plugin.mount();
window.addEventListener('beforeunload', () => plugin.unmount());
