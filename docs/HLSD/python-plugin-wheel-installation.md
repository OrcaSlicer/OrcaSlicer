# Python plugin wheel installation

## Purpose and identity

OrcaSlicer runs Python plugins in its embedded interpreter. A plugin's entry is a `.py` file or a wheel stored in its plugin directory. Discovery and metadata reading use that entry file; the wheel remains available after installation. Local plugin keys derive from the entry filename, while cloud plugins use their UUID. Those identities connect plugins to their settings, permissions, capabilities and other references and must not change because of how a wheel is installed.

Writable plugin data is stored separately from the plugin package. Package removal or replacement must not inadvertently remove that data.

## Installing wheels

When a wheel's legacy entry directory `__whl_extracted__/<entry-package>/` exists, OrcaSlicer loads it as before. This preserves files plugins may have written there. Otherwise, the wheel is installed with bundled `uv` and bundled Python into a private directory under the plugin root, `__wheel_install__/`, when the plugin is loaded. The parent `__whl_extracted__/` directory alone does not select legacy loading: it may contain separate dependency wheels. If both entry directories exist, the legacy entry takes precedence. There is no per-plugin installation-method switch. The stored wheel, plugin key and `.install_state.json` sidecar remain where they are, preserving their settings, permissions and capability choices. A `.py` entry is still imported directly.

uv runs without contacting a package index and without installing dependencies:

```
uv --no-config pip install --python <bundled python> --no-python-downloads
    --no-deps --no-index --no-cache --link-mode copy
    --target <staging target> <wheel>
```

Declared dependencies continue to use the existing shared installation policy: they are installed separately by uv into the shared packages directory and added to the interpreter's import path. Wheel files next to the entry that are not the entry itself are extracted under `__whl_extracted__/` as before.

The installed directory provides real filesystem paths for package resources. Wheel installation can relocate files compared with literal extraction, especially files in a wheel's `.data` directory. Plugins that derive resource paths from their package location should be checked against the installed layout.

## Wheel filename reconstruction

uv requires a valid PEP 427 wheel filename. A stored cloud download or a locally renamed wheel may not have one, so installation stages a copy under a name built from the wheel's own `METADATA` name and version and its declared `WHEEL` tags. The stored entry filename is not changed. The tag is selected from the tags the wheel declares; it is never invented, and uv performs the final interpreter/platform compatibility check.

## Cache and reuse

Installation happens in a staging directory, `__wheel_staging__-*`, and is promoted to `__wheel_install__/` only after uv succeeds and the entry package is present. The previous target is moved to `__wheel_prev__-*` during promotion and restored if promotion fails; it is removed after a successful promotion. All of these names start with `__`, so discovery ignores them.

A marker inside the installed target records the installation format, interpreter ABI, entry package, wheel size and a fingerprint of the wheel bytes. A later load reuses the target when all of them match, so unchanged wheels do not run uv again. The marker is a cache hint, not evidence that a plugin is trustworthy. The entry package must still be identified independently of the distribution name.

The local plugin cache-clear action removes both `__whl_extracted__/` and `__wheel_install__/`, including any files written inside them. It does not remove the stored wheel, the sidecar or the separate plugin data directory. After the legacy entry directory is removed, the next load uses uv.

## Failure and lifecycle

If uv fails or the promoted target is incomplete, the load fails with an error naming the installation problem. When no legacy entry directory exists, there is no silent fallback to extraction. The stored wheel, sidecar, settings and permissions are left in place, so a later load can retry once the problem is resolved.

Native extensions may remain loaded after a plugin is stopped, particularly on Windows, so replacing or removing their files may require an application restart.

Each plugin's installation directory is private, but plugins share one interpreter, `sys.path`, `sys.modules` and the dependency directory. Separate directories do not isolate conflicting imports. Installation and execution of plugin code are not security sandboxes.
