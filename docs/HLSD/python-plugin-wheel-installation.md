# Python plugin wheel installation

## Purpose and identity

OrcaSlicer runs Python plugins in its embedded interpreter. A plugin's entry is a `.py` file or a wheel stored in its plugin directory. Discovery and metadata reading use that entry file; the wheel remains available after installation. Local plugin keys derive from the entry filename, while cloud plugins use their UUID. Those identities connect plugins to their settings, permissions, capabilities and other references and must not change merely because the way a wheel is installed changes.

Writable plugin data is stored separately from the plugin package. Package removal or replacement must not inadvertently remove that data.

## Loading wheels

The legacy loader extracts a wheel under the plugin root and imports the resolved entry package from the extracted directory. Declared dependencies are installed separately by the bundled `uv` into a shared packages directory, which is added to the interpreter's import path.

The optional wheel installation path uses the bundled `uv` and bundled Python to install an entry wheel into a private directory under the plugin root. It passes `--no-deps` so that the existing shared dependency policy remains separate, and `--no-index` so entry-wheel installation does not fetch packages from an index. A temporary copy with a valid wheel filename may be needed for a cloud download or a locally renamed wheel; the stored entry filename and plugin identity are retained.

The installed directory provides real filesystem paths for package resources. Wheel installation can relocate files compared with literal extraction, especially files in a wheel's `.data` directory. Plugins that derive resource paths from their package location should be checked against the installed layout.

## Installation and lifecycle constraints

An installation is prepared in a temporary directory before it is made available for import. A cache marker records the wheel fingerprint, interpreter ABI and installation format to decide whether the target can be reused. The marker is a cache hint, not evidence that a plugin is trustworthy. The entry package must still be identified independently of the distribution name.

The installation method is selected per plugin. Changing it requires the plugin to be stopped. The legacy method remains available as an explicit choice; an installation failure is reported rather than silently switching methods. Clearing the plugin cache needs to cover both the extracted and installed copies without removing the retained wheel or plugin data.

An update must preserve a usable backup when installation or promotion fails. A retained backup also provides a recovery option if the new plugin installs successfully but fails when it is started later. Native extensions may remain loaded after a plugin is stopped, particularly on Windows, so replacing or removing their files may require an application restart.

Each plugin's installation directory is private, but plugins share one interpreter, `sys.path`, `sys.modules` and the dependency directory. Separate directories do not isolate conflicting imports. Installation and execution of plugin code are not security sandboxes.
