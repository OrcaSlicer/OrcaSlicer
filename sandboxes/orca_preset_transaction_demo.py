# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# id = "preset-transaction-demo"
# name = "Preset Transaction Demo"
# description = "Stages several settings on one preset with begin_update()/set()/commit() (demo)."
# author = "OrcaSlicer contributors"
# version = "0.2.0"
# type = "script"
# ///
"""Preset transaction demo: change several settings with one commit.

Enable the plugin, then run "Preset Transaction Demo" from the Plugins dialog.
Edit CATEGORY, PRESET_NAME, FILAMENT_SLOT and UPDATES below to try other presets
and settings.

``begin_update()`` resolves the target through Orca's normal preset-selection
flow, so it may open Orca's usual "unsaved changes" dialog. The call is
synchronous: the background Script worker waits until the user answers, while
the UI thread stays responsive. ``set()`` only stages values; ``commit()``
validates and applies the whole batch. The edited preset stays unsaved (dirty)
until the user saves it through the normal preset workflow; nothing here writes
to disk.

Values are Orca's serialized config strings: numbers plain, percentages with
"%", booleans "1"/"0", choices by their internal key, lists ";"-separated.
"""

import orca

# ---------------------------------------------------------------------------
# Edit these. CATEGORY and UPDATES belong together: each presets category only
# accepts its own setting keys.
# ---------------------------------------------------------------------------

# Preset category: "process", "printer", or "filament".
CATEGORY = "process"

# Target preset name for process/printer. "" keeps the currently active preset.
# For filament this optionally selects and assigns the preset for FILAMENT_SLOT;
# "" keeps the preset the slot already has.
PRESET_NAME = ""

# 1-based filament slot. Only used when CATEGORY is "filament".
FILAMENT_SLOT = 1

# Settings staged in order, then committed together.
UPDATES = (
    ("layer_height", "0.25"),
    ("wall_loops", "3"),
)


def _begin_update():
    """Bind CATEGORY's target and return a transaction to stage changes on."""
    if CATEGORY not in ("process", "printer", "filament"):
        raise ValueError('CATEGORY must be "process", "printer", or "filament"')
    if CATEGORY == "filament":
        # Filament updates are addressed by their 1-based slot number.
        return orca.host.preset.filament.begin_update(FILAMENT_SLOT, PRESET_NAME)
    return getattr(orca.host.preset, CATEGORY).begin_update(PRESET_NAME)


class PresetTransactionDemo(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "Preset transaction demo"

    def execute(self):
        # A plain def runs on the background Script worker, so begin_update() may
        # block here through Orca's selection / unsaved-edit prompts.
        try:
            transaction = _begin_update()
            for key, value in UPDATES:
                transaction.set(key, value)
            result = transaction.commit()
        except Exception as error:
            return orca.ExecutionResult.failure(
                orca.PluginResult.RecoverableError,
                "Preset transaction demo failed: %s" % error,
            )

        return orca.ExecutionResult.success(
            'Committed %d setting(s) to the %s preset "%s"; %s.'
            % (
                len(result.keys),
                result.category,
                result.target_name,
                "it stays unsaved until you save it"
                if result.dirty
                else "it already matches its saved state",
            )
        )


@orca.plugin
class PresetTransactionDemoPackage(orca.base):
    def register_capabilities(self):
        orca.register_capability(PresetTransactionDemo)
