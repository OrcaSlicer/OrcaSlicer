# Example — webview SlicingFinished listener

Updates a status element whenever a slice finishes. Demonstrates the
end-to-end webview bridge: JS calls `window.orca.events.on`, the host
subscribes to the typed `SlicingFinished` event on the engine bus, and
dispatches back to the JS callback on the UI thread.

```html
<!DOCTYPE html>
<html>
<body>
<h1>Slice watcher</h1>
<p id="status">Waiting…</p>
<script type="module" src="src/main.ts"></script>
</body>
</html>
```

```ts
// src/main.ts
import { defineWebviewPlugin } from '@orca/plugin-types';

const plugin = defineWebviewPlugin({
  id:         'com.example.slice-watcher',
  name:       'Slice Watcher',
  version:    '0.1.0',
  async onMount() {
    const status = document.getElementById('status')!;

    window.orca.events.on('SlicingFinished', ({ handle, success, error }) => {
      status.textContent = success
        ? `Slice #${handle} finished cleanly`
        : `Slice #${handle} failed: ${error}`;
    });
  },
});

plugin.mount();
```

```json
{
  "id":          "com.example.slice-watcher",
  "name":        "Slice Watcher",
  "version":     "0.1.0",
  "kind":        "webview",
  "entry":       { "webview": "index.html" },
  "permissions": []
}
```

`pnpm run build` packages the result as
`com.example.slice-watcher-0.1.0.orcaplugin`, ready for the Plugin
Manager's `Install...` button.
