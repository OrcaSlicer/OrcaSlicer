# MCP tools — High Level Design

## Purpose and scope

OrcaSlicer's MCP server gives a client commands, its MCP tools, for the app, the project, its objects,
parts and plates, presets and settings, filaments, printers, plugins, the view and windows, recordings,
replays and scripts, and for driving the GUI itself. An agent calls them by name. A person's Speed Dial actions
run them, Action Replay and recordings show them, and the journal records and replays them. A client that
knows the app's menus can guess the command that does a menu item's job and the argument that picks the
item, and can do from arguments what a person does through the item and its dialog. Where a gizmo or a print dialog holds
choices too many to take as arguments, its command opens it on the object or printer it names, and the client works
it with the commands that drive the GUI.

This document is that command set: how the commands are named and grouped, what each does and takes, which
menu items are actions of which command, which calls ask in a dialog, and the behaviour a caller relies on
across the commands. How the server serves them (the catalog, the guards, jobs, the journal, the operations
layer) is in [mcp-server.md](mcp-server.md).

The Design tab, OrcaSlicer's optional CAD component, adds commands of its own while the CAD feature is
on. They run on the same server and keep the same rule for menu items, but are named and grouped as CAD
programs name theirs, so they have a section of their own at the end, The Design tab's commands.

Each place's commands are written in its own file, `src/slic3r/GUI/Mcp/Tools/<Place>Tools.cpp` (Objects and
Parts share `ObjectTools.cpp`), and gathered by `register_all_commands()`. A menu item's function is in namespace `MenuItems` (`NativeCommandMenuItems.hpp`),
and what a command does that a dialog or another interface does too is an operation of
`src/slic3r/GUI/Operations/`. What every command takes and replies is in its description and input schema,
which `tools/list` gives a client and `tool_describe {tool}` gives for any command, as its manual.
`tests/data/command_catalog.json` holds every command's schema, labels and examples as an agent sees them, and
its Speed Dial actions as a person sees them.

## Naming

1. **A name is its place, then its verb**, every word in full: `object_delete`, `plate_add`,
   `preset_select`. The place is the thing the command acts on, in the singular and in the GUI's word
   (`settings` and `preferences` are the GUI's words as they stand). A read is its place, then what it
   returns: `app_state`, `object_list`, `plate_slice_stats`. `undo` and `redo` alone have no place: the
   Design tab shares them, and they act on the tab shown.
2. **The GUI's words**, so that the words of a person's request name the command an agent picks: `object_set_height_ranges`
   edits what the object list calls height ranges (Height Range Modifier), `object_set_variable_layer_height`
   runs the Variable layer height panel, and `replay_run` runs what Action Replay calls a replay file. Where
   the GUI has no name for the thing, the words are plain (`widget_list`, `dialog_answer`). A word the GUI
   uses for something else is avoided: the `script_` commands run Python, as the Run Script panel does.
3. **One job, one command, and one argument for its variants.** A menu item and the command that does its job
   are one command, and the item is one of its actions: a Speed Dial entry that runs the command with fixed
   arguments. Most of a command's actions match a menu item; Go to tab... and Go to layer (percent) collect a
   value in the Speed Dial and match none. Where the GUI has an item for each variant, one argument picks it:
   `format`, `shape`, `axis`, `type`, `mode`, `window`, `page`, `camera`.
4. **An argument's name means one thing across the commands**, so what an agent learns from one holds for
   the next:

| Argument | What it is |
|---|---|
| `target` | the window or element a GUI command acts on: a ref, a locator, or `main`, `dialog` and the like; `main` is the main window wherever a command names it, as `window_list` calls its kind |
| `canvas` | a 3D view: Prepare's (`3d`), the preview or the assemble view |
| `path` | the file a command is about; `save_path` where a capture writes its file beside its reply |
| `objects`, `instances`, `parts` | the targets of a command of several targets; `object`, `instance`, `part` of a command of one |
| `part` | a part of an object, the Objects panel's word for what the model calls a volume, which leaves `mesh_volume` and `scaled_volume` to mean cubic millimetres |
| `plate`, `plates` | one plate, by index; a command that also takes `"current"` or `"all"`, or a list, calls it `plates` |
| `filament_slot` | a filament slot, counted from 1 as the sidebar counts them |
| `discard_changes` | drop the project's unsaved changes |
| `preset_changes` | keep, transfer or discard the presets' unsaved changes |
| `overwrite` | replace a file that exists |
| `look_at`, `waypoints` | the point the camera looks at; a drag's intermediate points |

A command's group is named after its place, the first word of its name. `NativeCommands::place_heading()` maps
each first word to its group (`kPlaceWords` in `NativeCommands.cpp`, with the reasons for the less obvious ones)
and holds the few commands grouped apart from their first word (`kPlaceExceptions`); `kHeadingOrder` is the
groups' order, which the tables below follow and `tool_list`'s `group` argument lists.

## The command set

The commands are listed by group, each under a heading of its name, in the order `tools/list` gives them:
`tool_list`, `tool_describe`, `tool_call` and `app_state` first, as what an agent starts with, then each group's
commands. Main arguments
only; every argument, its default and its range are in the command's schema. "Targets" stands for the
targets of a command of several (`objects`, `instances`, `parts`), which default to the selection (see
Targets and the selection).

Every command is in `tools/list` but four, the menu items that take no arguments:
`settings_toggle_developer_mode`, `view_toggle_activity_panel`, `window_save_screenshot` and
`script_toggle_panel`. `tool_list` lists
them and `tool_call` runs them.

### Tools and actions

| Command | What it does | Variants and main arguments |
|---|---|---|
| `tool_list` | List the commands `tools/list` does not show (the Design tab's, the menu items that take no arguments, the plugins' operations), each with what it does, all of them or one group's | `group` |
| `tool_describe` | Read one command's manual, listed or not: every parameter with its type, whether it is required, its default, its values or range and the tools that list its values; the schema's rules in words; the menu items that run it; its examples | `tool` |
| `tool_call` | Call any command by name, one `tools/list` shows or one it does not, as that command's own call | `tool`, `args` |
| `action_list` | List what the Speed Dial runs, by section and title: the commands' actions, plates, recent projects, plugin scripts and pages, and settings on request | `section`, `include_settings` |
| `action_run` | Run one of those by its id, as a person's pick does | `id`; `args`: none, or exactly the action's |

`tool_list` and `tool_describe` read the catalog without the UI thread, so they answer while the app is busy or a
dialog is open. In a manual, a parameter's `required` is true, false, or the condition in words where a schema rule
decides it; `default` is the schema's alone, a default the command applies itself being in its description;
`goes_with` names the `type` or `target` a parameter belongs to; and an object's `other_keys` is false when it takes
nothing beside its properties, true when it takes anything, or the shape of the values it takes. `rules` gives the
schema's conditional parts as sentences, and says whether other arguments are accepted.

### The app

| Command | What it does | Variants and main arguments |
|---|---|---|
| `app_state` | A snapshot of the app, its view, the project and the selection, and what is going on: open dialogs read as shown, notifications, uploads, the open gizmo, running jobs and a recording; the call to make first |  |
| `app_wait` | Wait until the app is ready or idle, a dialog opens or closes, or a job ends | `for`: ready, idle, dialog, no_dialog, job; `job`, `timeout_s` (at most 50) |
| `app_log` | The last lines of the app's log (50 by default), in whole entries, from a level up | `lines`, `level` |
| `undo`, `redo` | Take back the last change of a history, or put it back, as Edit > Undo and Redo do | `history`: project, design |
| `app_open_web_page` | Open one of the app's web pages in the default browser | `page`: wiki, youtube, plugin_hub |
| `app_check_for_updates` | Check for a newer OrcaSlicer |  |
| `app_show_config_folder` | Open the configuration folder in the file manager |  |
| `app_benchmark_3d_scene` | Time the 3D views on a test model: it asks first, then replaces the project as New Project does |  |
| `preferences_get` | The preferences the Preferences dialog shows, as it shows them, with each one's kind, choices or range | `keys`, `tab` |
| `preferences_set` | Change preferences as their controls do, all checked first and written together | `values` |
| `job_cancel` | Stop the running slice or export, or one job an MCP call started | `job` |

### The project

| Command | What it does | Variants and main arguments |
|---|---|---|
| `project_open` | Open a project (.3mf), or a G-code file to show its toolpaths, in place of the current project | `path`; `discard_changes` |
| `project_import` | Add the models of files, or of a ZIP archive's entries, to the project, in one undo step | `paths`; `archive`, `entries` |
| `project_new` | Start a new, empty project | `discard_changes`; `preset_changes`: keep, discard |
| `project_save` | Save the project to its file, or as a .3mf at a path, which becomes its file | `path`, `overwrite`; `save_as` |
| `project_export` | Write a plate's G-code, a sliced 3MF, a 3MF, the objects as STL or Draco, or the shown toolpaths as OBJ | `format`: gcode, sliced_3mf, 3mf, stl, drc, toolpaths_obj; `path`, `overwrite`; `plates`; `per_object`, `objects` |
| `project_get_settings` | The project's own settings for its plates: the plate type, prime tower positions and filament grouping (read only: every plate carries its own, which `plate_set_settings` sets, so a project-wide one would change no slice) | `keys`, `include_meta` |
| `project_set_settings` | Change those settings as their controls write them | `values` |

`project_set_settings`'s `curr_bed_type` goes through the plate type selector, so it also sets the printer's
remembered plate type and the plates on the global type. It takes only the types the selector offers
(`meta.offered`), and none while the selector is hidden for the printer.

### Objects

| Command | What it does | Variants and main arguments |
|---|---|---|
| `object_list` | The project as the Objects panel lists it, plate by plate: each object's index, id, name, filament, part count, size and instances; with `detail`, its instances, its parts (a single one too, which the panel folds into the object's row, since painting, a part's filament and its settings address the part), overrides, mesh errors, height ranges and layer height profile (its point count and extreme heights; the points stay in the project file) | `plate`, `detail`, `screen` |
| `object_select` | Select objects, instances or parts in the 3D view, as clicks do, or everything or nothing | targets, `add`; `all`, `none` |
| `object_transform` | Move, rotate or scale one instance, or one part | `object`, `instance`, `part`; `translate`, `position`, `rotate`, `scale`, `size`, `plate`; `mode`: relative, absolute; `drop_to_bed` |
| `object_add_primitive` | Add a primitive shape as a new object, as Add Primitive does | `shape`: cube, cylinder, sphere, cone, disc, torus; `size`, `position`, `plate`, `name` |
| `object_add_handy_model` | Add one of the test models the app ships, as Add Handy models does | `model`: one of ten |
| `object_add_text` | Add a text part to the selected object, or to the object named, or a new object, and open the Text gizmo on it | `object`; `type`: part, negative, modifier |
| `object_add_svg` | Add an SVG as a new object or as a part of the selected one, or of the object named, and open the SVG gizmo on it | `path`; `object`; `type`: part, negative, modifier |
| `object_delete` | Delete objects, instances or parts, what stands on a plate, or every object | targets; `plate`; `all` |
| `object_rename` | Rename an object or a part | `object`, `part`, `name` |
| `object_set_instances` | Give an object a number of instances, or one more or one fewer | `object`; `count`, or `delta`: 1, -1 |
| `object_separate_instances` | Make instances separate objects, as Set as Individual Objects does | `object`, or `instances` |
| `object_split` | Split an object into objects or into parts | `object`; `into`: objects, parts; `keep_z` |
| `object_assemble` | Assemble objects into one object of parts | `objects` |
| `object_order` | Order a plate's objects by name, filament, height or size, or into a given order, which sets the list and the print order | `by`: name, filament, height, size; `descending`; or `order`; `plate`, `all` |
| `object_drop` | Drop objects or parts onto the bed | targets |
| `object_mirror` | Mirror objects or parts along an axis | `axis`: x, y, z; targets |
| `object_center` | Centre objects on their plate, or on another | targets; `plate` |
| `object_scale_to_fit` | Scale objects as one to fit the plate's print volume, and move them into it | targets |
| `object_auto_orient` | Turn objects to the orientation that prints best | targets; `plates`, a list of indices |
| `object_set_printable` | Mark objects, or some of their instances, printable or not | targets, `printable` |
| `object_set_auto_drop` | Turn Auto Drop on or off for objects | targets, `auto_drop` |
| `object_fix` | Repair meshes, as Fix Model does, with no progress dialog | targets; `plate`; `all` |
| `object_clone` | Copy objects as new objects, as the Clone dialog does | targets; `count`, `arrange` |
| `object_reload_from_disk` | Load parts again from the files they came from | targets; `all` |
| `object_arrange` | Arrange every unlocked plate, or one, or fill an object's plate with more of it | `plates`; `spacing`, `rotate`, `align_y`, `multi_material`; `fill_object`, `fill_with`: instances, copies |
| `object_paint` | Open a painting gizmo on an object's instance | `type`: supports, seam, color, fuzzy_skin; `object`, `instance` |
| `object_measure` | Open the Measure gizmo on objects, or on the selection as it is | targets |
| `object_simplify` | Open the Simplify gizmo on a part | `object`, `part` |
| `object_cut` | Cut an instance along a plane, as the Cut gizmo's Perform cut does, or open the Cut gizmo on it | `object`, `instance`; acting: `z` or `plane`, `keep`: both, upper, lower; `upper`, `lower`: as_cut, place_on_cut, flip; `to_parts` |
| `object_lay_on_face` | Turn an instance so that the face a direction names lies on the bed, or open Lay on Face on it | `object`, `instance`; acting: `normal`, `frame`: object, world |
| `object_set_brim_ears` | Set an object's brim ears at bed positions, or clear them, or open the Brim Ears gizmo on it | `object`, `instance`; acting: `points`, `clear` |
| `object_set_height_ranges` | Replace an object's height range modifiers | `object`, `ranges` |
| `object_set_variable_layer_height` | Run the Variable layer height panel's Adaptive, Smooth or Reset on an object | `action`: adaptive, smooth, reset; `object`; `quality`; `radius`, `keep_min` |

`object_arrange` refuses a locked or an empty plate given by index. An arrange of every plate skips the locked plates
and those that print in another sequence than the project, and a plate in another sequence arranged alone gets
automatic spacing, which its `note` says. `object_set_height_ranges` refuses overlapping ranges, which the slicer would
trim without a word, and keeps a range above the object, which slicing cuts, as the object list keeps it.
`object_set_variable_layer_height` needs what its toolbar item needs, an FFF printer and the object above the bed, and
its refusal names what is missing.

### Parts

| Command | What it does | Variants and main arguments |
|---|---|---|
| `part_add` | Add a part, a negative part, a modifier, a support blocker or enforcer, or a precise seam to an object: a primitive shape, or a model file other than an SVG, whose parts `object_add_svg` adds through the SVG gizmo, which places them where the camera looks | `type`: part, negative, modifier, support_blocker, support_enforcer, precise_seam; `shape` and `size`, or `path`; `object`, `position` |
| `part_change_type` | Change what parts are, as Change Type does, and a precise seam's subtype | `type`, as `part_add` takes it; `subtype`: center, left, right, enforced, blocked, neutral; `parts` |
| `part_replace` | Replace a part's mesh with a model file's, keeping its settings, type, filament and placement | `object`, `part`, `path` |
| `part_boolean` | Open the Mesh Boolean gizmo on an object of two parts or more, or on two whole objects or more | `object`, `objects` |

### Plates

| Command | What it does | Variants and main arguments |
|---|---|---|
| `calibration_generate_test` | Generate a Calibration menu test: a new project with the test model, and the presets changed as the test needs | `test`: temperature, max_volumetric, pressure_advance, flow_ratio, retraction, vfa, cornering, input_shaping_freq, input_shaping_damp; the test's values (`start`, `end`, `step`, `method`, `test_model` and others); `filament_slot` (a physical filament, default 1); `discard_changes`, `preset_changes`: keep, discard |
| `plate_get_settings` | A plate's own settings, as Plate Settings sets them: plate type, print sequence, spiral mode, the first layer's and the other layers' filament order, and the filament grouping | `plate`, `keys` |
| `plate_set_settings` | Change them, as the dialog's OK does | `plate`, `values` |
| `plate_list` | The plates, each with its name, lock, object count, settings, origin and size |  |
| `plate_add` | Add a plate |  |
| `plate_delete` | Delete a plate; its objects stay in the project, off the plates | `plate` |
| `plate_duplicate` | Add a plate with a copy of a plate's objects | `plate` |
| `plate_rename` | Name a plate | `plate`, `name` |
| `plate_lock` | Lock or unlock a plate, or toggle its lock | `plate`, `locked` |
| `plate_select` | Make a plate the current one | `plate` |
| `plate_slice` | Slice the current plate, all plates or one, as the Slice button does, and wait for the result | `plates`: "current", "all" or an index; `preview` |
| `plate_set_custom_gcode` | Add or remove pauses, custom G-code, templates and filament changes at layers, as the layer slider's menu does | `plate`; `add`, `remove`, `clear` |
| `plate_slice_stats` | What the Preview shows for a sliced plate: the summary, and times and weights per feature, per layer and per object | `plate`; `include`: features, layers, layer_features, objects, view |

`calibration_generate_test` with `preset_changes: "keep"` refuses while the filament tab edits another slot's modified
preset. Staying in the test project, `settings_set {reset_all: true, values}` with the preset's `type` drops the
test's settings and sets a measured value in one call. `plate_set_settings` applies the dialog's OK only when a value
changes, so a call that changes nothing leaves the project clean; the plate tab's `skirt_start_angle` is not among its
keys. In `plate_list`'s `layer_gcode`, a code's `z` is rounded to the G-code's 0.001 mm, and its `layer` is left out for
a code above the top layer.

### Presets and settings

| Command | What it does | Variants and main arguments |
|---|---|---|
| `preset_list` | Printer, process or filament presets, each with what it is and whether it is selected | `type`: printer, process, filament; `query`, `source`, `vendor`, `printer`, `filament_type`; `include_incompatible`, `include_uninstalled` |
| `preset_select` | Select a preset, as its selector does; a filament for a slot | `type`, `name`; `filament_slot`; `preset_changes`: transfer, discard |
| `preset_save` | Save the preset being edited, with its unsaved changes, under a name, as the tab's Save does | `type`, `name`; `filament_slot`, `overwrite`, `inside_project`, `detach` |
| `preset_delete` | Delete a user preset or one inside the project, its file and its cloud copy | `type`, `name`; `delete_attached` |
| `settings_list` | Every setting of a type: the edited preset's, a saved one's or a slot's; or those an object, a part or a height range takes | `type`: process, printer, filament, object, region; `preset`, `filament_slot`, `where` |
| `settings_list_pages` | The pages of a type's settings tab and the groups on each, with how many settings each holds: the places `settings_list` takes as `where` | `type`: process, printer, filament, object, region |
| `settings_get` | The settings named, with what each does; or what an object, a part or a height range overrides, and where each value comes from | `keys`, `compare`; `type`; `target`: global, object, part, height_range |
| `settings_set` | Change settings as typing them into their tab does: a preset's, or the overrides an object, a part or a height range carries | `values`; `type`, `target`; `reset_all`, `reset_to`: saved, parent |
| `preset_import` | Import presets from a preset file, an archive or a bundle, or the settings a G-code was sliced with | `path`; `on_conflict`: skip, overwrite |
| `preset_export` | Open the Export Preset Bundle dialog, which the caller answers | `path`, the folder |
| `preset_sync` | Synchronize user presets with the cloud account |  |
| `preset_migrate_filaments` | Migrate legacy user filament presets |  |
| `settings_set_mode` | Switch the settings view to Simple, Advanced or Expert | `mode`: simple, advanced, expert |
| `settings_toggle_developer_mode` | Turn Developer mode on or off; a menu item, not listed |  |

An agent finds a setting by what it does: `settings_list_pages {type}` gives a tab's pages and groups, and
`settings_list {type, where}` a group's settings with their values and meta, whose `description` is the setting's
whole tooltip and `where` its page and group. There is no search by words, since a goal's words ("add a brim", "stop
the ooze") are seldom a setting's, and an agent matches what it wants against what each setting does better than a
match of words does. The help text comes with the value rather than from a command of its own because that is where
agents look: in the bench, agents read settings three to five times as often as they called a separate explanation
tool, and with the tooltips out of the reads they set a draft shield without skirt loops and a support threshold
measured from the wrong side. So that replies stay small, only the keys a call names or lists by their group carry
meta: `settings_list` without `where` gives values alone, since an agent listing every setting is scanning values,
and `settings_get` with none of `keys`, `compare` or a target is refused, so `settings_list` is the one listing. An
enum's meta lists `labels`, the texts its combo shows, in the order of `values`, so a person's wording maps to a value.
`compare`'s `:saved` and `:parent` never clash with a preset, since no preset name holds `:`.

A printer's extruder settings are on its one Extruder page whatever its number of extruders: `where` names the page
("Extruder > Retraction"), and each extruder's value is in `value` and `meta.variants`. `settings_get`'s `compare`
follows the Compare presets dialog: a printer's or a filament's vectors are compared entry by entry, an item then
carrying `index`, and a printer with fewer extruders is compared from the other side. `preset_delete` refuses a
printer preset while a physical printer is selected, whose Delete only takes the preset off that printer.
`preset_save` of a printer preset points the selected physical printer at the new preset, as the dialog's default
does. `preset_select` of a printer the selectors hid also shows its model's default filaments when no shown filament
suits it, so the switch lands on one of them.

### Filaments

| Command | What it does | Variants and main arguments |
|---|---|---|
| `filament_list` | The filament list as the sidebar shows it: each slot's colour and preset, or a mixed filament's filaments, ratios, gradient and whether it is broken; the flushing volumes, multiplier and purge mode |  |
| `filament_add` | Add a filament slot, as the + button above the filament list does |  |
| `filament_delete` | Delete a slot, or merge it into another, as its Delete and Merge with items do | `filament_slot`, `merge_into`, `break_mix` |
| `filament_add_mixed` | Add a mixed filament, which prints physical slots in turn, layer by layer, in given ratios | `components`, `ratios` |
| `filament_edit_mixed` | Change a mixed filament's filaments and ratios, as the Edit Mixed Filament dialog's OK does | `filament_slot`, `components`, `ratios` |
| `filament_set_colour` | Set a physical slot's colour, as its colour button does | `filament_slot`, `colour` |
| `filament_set_flushing` | Change the flushing volumes, multiplier and purge mode, as their dialogs' OK does | `flush_multiplier`, `flush_volumes_matrix`, `prime_volume_mode` |
| `filament_sync_ams` | Synchronize the filament list from the connected printer's AMS | `mode`: map, overwrite |
| `filament_install` | Install system filaments, as the System Filaments dialog's Confirm does | `names`; or `vendor` and `filament_type` |
| `filament_uninstall` | Uninstall system filaments | `names` |

A mixed filament's slot holds no preset of its own: it has no selector and no filament tab, so `filament_list` gives
its row none, `preset_list`'s `filament_slots` leaves it out, `preset_select`, `settings_get`, `settings_set`,
`settings_list` and `preset_save` refuse it (`mixed_filament_slot`), and `plate_set_settings` refuses a custom
filament order while a slot is mixed, where the Plate Settings dialog greys it. With the Auto flush after changing
preference on, the flushing volumes are recalculated when a slot's colour changes, when a slot is added and, by the
preference, when a slot's preset changes, so set the matrix last; a replay of such calls matches only on a datadir
with the same preference. `filament_sync_ams` refuses a printer of another model than the project's.

### Printers and prints

| Command | What it does | Variants and main arguments |
|---|---|---|
| `printer_list_models` | Every printer model the app ships or has installed, of every vendor, with its nozzles | `vendor`, `query` |
| `printer_install` | Install printer models, as the Printer Selection dialog's Confirm does | `models`, each with its `nozzles`; `preset_changes`: transfer, discard |
| `printer_uninstall` | Uninstall printer models, or some of their nozzles | `models`; `preset_changes` |
| `print_start` | Open the dialog the Print button's Print plate, or Print all, opens for the selected printer | `plates`: an index, `"current"`, `"all"` |
| `printer_send` | Open the dialog the Print button's Send opens: a Bambu printer's storage dialog, or the print host's upload dialog | `plates` |
| `printer_test_host` | Ask the printer's print host whether it answers, as the connection dialog's Test button does |  |
| `printer_status` | What the Device tab shows of the printer it is connected to: state, progress, layers, the file, temperatures |  |
| `print_pause` | Pause that printer's print, as the Device tab's Pause does |  |
| `print_resume` | Resume its paused print, as the tab's Resume does |  |
| `print_stop` | Stop its print, as the tab's Stop does, without the tab's question |  |

### Plugins

| Command | What it does | Variants and main arguments |
|---|---|---|
| `plugin_list` | The plugins, as the Plugins window lists them, each with the key the other plugin commands take |  |
| `plugin_install_local` | Install a plugin from a .py or .whl file | `path`, `overwrite` |
| `plugin_enable` | Turn a plugin, or one of its capabilities, on or off, as the Plugins window's switches do | `plugin`, `enabled`; `capability` |
| `plugin_reload` | Load a plugin again after its files changed, or install a cloud plugin again | `plugin`; `clear_cache` |
| `plugin_uninstall` | Remove a plugin for good, unsubscribing a cloud one | `plugin` |
| `plugin_refresh` | Rescan the installed plugins and fetch the plugin hub's list again, then reply with the plugins |  |

A plugin's code calls the commands through `orca.host.tools` as an agent does, with the tools permission
([plugin-permissions.md](plugin-permissions.md)), scoped to the plugin: `replay_export` is refused, `job_cancel` and
`app_wait {for: "job"}` act only on its own jobs, and the rules, recordings and held presses its calls leave end when
it unloads or loses the permission, from which moment its calls are refused. An operation a plugin offers
(`plugin.<plugin>.<name>`, which `tool_list` lists and `tool_call` runs) runs as a job: as a change, refused behind a
dialog and journaled, when the plugin declares that it changes something, else as a read. A call of an operation
whose plugin has unloaded, or loaded again since, is refused.

### View and windows

| Command | What it does | Variants and main arguments |
|---|---|---|
| `view_toggle_activity_panel` | Show or hide the Agent Activity pane, where a person watches the calls of agents, Action Replay, Run Script, a `--run` and plugins |  |
| `view_set` | Set what the window shows: the tab, the 3D view and its camera, the Preview's colour scheme, legend and layers, the section view, the display switches and the sidebar | `tab`, `canvas`; `camera`, `look_at`, `orbit`, `zoom`, `projection`; `view_type`, `time_mode`, `layer`, `layer_range`, `layer_z`, `layer_percent`; `section`; the switches; `layout` |
| `window_open` | Open one of the app's windows or dialogs for a person, as its menu item does | `window`: preferences, plugins, action_replay, preset_bundle, keyboard_shortcuts, setup_wizard, network_test, troubleshoot, tip_of_the_day, about; `tab` and `key` for preferences |
| `window_list` | The open windows: the main window, then those that don't block the app, then the modal dialogs, the topmost last |  |
| `window_set` | Size, move, maximize, minimize or raise a window | `target`; `size`, `position`; `state`: normal, maximized, minimized; `raise` |

- **Units.** `window_set` takes sizes and positions in the units `window_list` and `app_state` report: physical
  pixels on Windows, points on macOS, logical pixels on Linux. A negative position is a place on a screen left of or
  above the primary one, and a window without a resize border is resized all the same.
- **Raising.** On macOS `maximized` is the window's zoomed state, not full screen. OrcaSlicer never activates itself
  for `raise`, which would take the keyboard from the person's app: while another app is active, the window comes in
  front of OrcaSlicer's own windows only and `active` reads false. On Windows `raise` may only flash the taskbar
  button while another app has the focus. No command needs the window active: a capture reads a covered window whole,
  and input goes through the app.
- **Wayland.** A window has no position: `window_set` refuses `position`, and `window_list`, `window_set` and
  `app_state.window` leave positions out; run OrcaSlicer with `GDK_BACKEND=x11` to have them. A window the person
  minimized from the desktop reads `normal`. `normal` and `raise` bring a minimized window back only while the
  compositor lets OrcaSlicer take the focus; otherwise the reply reads `minimized`, with a note that the person
  restores it from the taskbar, or `active: false`. sway never minimizes a window.
- **The camera.** `camera_state`, in `view_set`'s reply as in `app_state.camera`, gives `click_px_per_mm`, the zoom in
  the logical pixels `widget_click` and `mouse_gesture` take; `type`, which says whether `zoom` holds across the whole
  view (orthographic) or only at the depth of `look_at` (perspective); and `elevation` and `azimuth` in `orbit`'s
  convention, a camera straight above reading 90. The reply also echoes the `camera` and `orbit` it was given, which a
  replay checks. A `look_at`, `orbit` or `zoom` given right after a tab switch, a new project or a load first waits for
  the fit those queue on the camera, and a `camera` for the view to have drawn at its size; past a short wait the call
  applies its fields anyway and says in a `note` which of the two it did not see.

### Recordings, replays and scripts

| Command | What it does | Variants and main arguments |
|---|---|---|
| `window_screenshot` | Capture the main window, the 3D view as rendered, the topmost dialog or one window, as an image and a PNG | `target`, `max_dim`; `save_path`, `overwrite` |
| `window_save_screenshot` | Save a PNG of the main window where recordings are saved, and tell the person; a menu item, not listed |  |
| `replay_run` | Run a replay, a list of calls, and report each step | `path` or `replay`; `vars`, `out_dir` |
| `replay_export` | Turn this session's calls into a replay | `path`, `overwrite`, `name`; `clear` |
| `script_run` | Run Python inside the app as its main program, against the orca API | `code`; `args`, `filename`, `timeout_s` |
| `script_list_api` | List the orca Python API of the running build, one module or class at a time | `within` |
| `script_describe_api` | Read one entry of the orca Python API whole: its kind, signature and documentation | `name` |
| `script_toggle_panel` | Show or hide the Run Script panel; a menu item, not listed |  |
| `recording_start` | Record a video of the main window or the 3D view | `target`: main, canvas; `captions`, `pointer`; `max_seconds`, `fps`, `max_dim`; `encode` and the encoder's options; `save_path`, `overwrite` |
| `recording_stop` | Stop the recording; with `silent: true`, wait for its file and report it | `silent` |
| `recording_encode` | Encode a recording into an H.265 or H.264 MP4 with the system ffmpeg | `path`, `save_path`, `overwrite`; `codec`, `crf`, `speed`, `profile`; `keep_raw_video_file` |

### Driving the GUI

| Command | What it does | Variants and main arguments |
|---|---|---|
| `widget_list` | What is on screen, as a tree with a ref per element: windows, list rows, menus, the 3D view's toolbar items and gizmos, ImGui panels and web pages | `target`, `query`; `include_menus`, `include_imgui`, `include_web`; `max_nodes` |
| `widget_click` | Click an element as a person would | `target`; `x`, `y`; `button`, `double`, `modifiers`; `popup_item` |
| `widget_set_value` | Set a widget's value, with the events a person's edit sends | `target`, `value`; `submit` |
| `keyboard_press` | Send a key chord, or type text | `keys`, `text`; `target`; `popup_item` |
| `mouse_gesture` | Move, click, drag or turn the wheel on a 3D view, at a point of the model or at a pixel | `action`: move, down, up, click, drag, wheel; `at`, or `x` and `y`; `to`, `waypoints`; `canvas`; `button`, `modifiers`, `delta` |
| `gizmo_pick_file` | Do what a file picker inside the open gizmo does, with a path in place of its dialog | `action`: svg_change, svg_save_as, texture_import; `path`, `overwrite` |
| `dialog_answer` | Press a button of the topmost modal dialog, or of the one whose title is given | `button`; `dialog` |
| `dialog_auto_answer` | Answer modal dialogs whose title or message matches a rule, from now on | `rules` |
| `webview_run_javascript` | Evaluate JavaScript in one of the app's web views | `code`; `target`; `timeout_s` |

How the server sees and drives the GUI is in mcp-server.md, GUI automation. What a caller meets:

- **The tree.** `widget_list` returns its tree as a text block of its result beside the JSON reply, one element per
  line with its ref. A control reads `[checked]` or `[unchecked]`, or `[mixed]` for a checkbox that shows neither (an
  unset nullable setting). An option of a selector Orca builds of buttons (a radio group, a tab bar, a multi-switch)
  is a button that reads checked while it is the selected one, and a switch with a label at each end has the label it
  is on as its value.
- **Locators.** A ref may also be given as `{ref: "w12"}`. A widget of the main window takes no `within`, since that
  window's title follows the project name. A label that matches both a row and the one field it names resolves to the
  field. A web view itself is named by its kind, `{class: "webview"}`, since its class differs per platform;
  `widget_click` and `widget_set_value` refuse a web view's own ref and point to its page's elements, and
  `keyboard_press` aimed at it types into its page's focused element.
- **Menu paths.** A menu path, as `{menu}` and `popup_item` take it, is the item labels joined with `>` in the UI
  language, matched in any case, each label without the shortcut key the menu shows after it and with a trailing
  `...` optional, so a path is the same on every platform. A label repeated among its siblings is numbered from its
  second time on (`Change Filament>Generic PLA #2`), and the " (current)" marker the Change Filament list gives the
  slot in use is left out. A path that names a submenu, a disabled item or nothing fails with the items to pick from,
  and so does a `popup_item` when the input opened no menu: a right click on empty space opens none while objects are
  selected, in the Preview, or with a gizmo open.
- **Menus an input call opens.** An input call takes only the first menu it opens. A later menu in the same call is
  closed unseen and counted under `menus_closed`, and so is one that opens when no call is waiting for it (a call
  that queues a menu to open later); `app_state.closed_menus` keeps the last three, each with the tool that took it.
  A `popup_item` of the 3D view's right-click menu that opens a dialog leaves it open when the call returns, and the
  reply lists it under `dialogs`. The items of an object's context menu that no command does are reached by a right
  click on the object in the 3D view, or on its row of the object list, with `popup_item`.
- **Rows.** A row locator's labels go from the top, joined with `>`, or as an array when a label holds `>`, and may
  leave out the labels above (the plate above an object). The object list's rows show only while the sidebar's
  Global/Objects switch is on Objects (a switch `widget_list` names by the "Process" beside it). In the object list a
  click on the Sinking column drops the object and one on Editing resets the row's own settings, as one on Printable
  toggles printability; a double click frames the object, or on the Filament column opens the filament editor. A
  table's cells are only made current, never edited: `settings_set` with the target `object` writes the same values.
  A screenshot of a widget inside a popup (a drop-down list, the print button's menu) is refused; a screenshot of the
  window the popup opened from shows it.
- **Plate icons.** A plate icon takes clicks only in the Prepare view, and not while variable layer height is being
  edited or a gizmo is open, where the canvas takes none; an icon the plate does not show (plate settings turned off,
  filament grouping without two nozzles, IDEX mode off) is refused by name. A click on the plate itself, on move to
  front or on delete replies the plate that is then `current` (delete also the plate count), and one on the IDEX icon
  its mode. `started: false` says an arrange or orient click started nothing, with a note when an arrange that was
  cancelled or failed left the arrange icon stuck, which `object_arrange` gets past. A right click opens the plate's
  menu, or the IDEX icon's mode menu, as `popup_menu`.
- **ImGui items.** A field that asks for the keyboard as it first shows takes it a frame later, and keys sent before
  then are lost, so type into it once `widget_list` marks it `[focused]`. An ImGui item's locator takes its shown
  label when no other item of its window shares it, else its name (the label's `##` part, or the English name Orca
  gives a control with no label), else both; panel text is listed but never matched. Controls with the same name in
  one panel are told apart by their order, so a recorded target among them is not found once their number changes,
  and after a reorder names the control in its place.
- **Gizmos.** A `gizmo` locator takes the name `app_state.gizmo` gives, or the English title of its toolbar button
  where that differs (Support Painting for `FdmSupports`), in any case, and a reply uses the first. Simplify and SVG,
  which are not on the toolbar, are listed on the Prepare canvas while they can open: Simplify while it can, SVG on a
  selected SVG part.
- **Where a `mouse_gesture` may start.** With `check: "surface"` a model point must lie on the part, above the bed
  and on the side a clipping plane keeps, with nothing in front of it (the plates count only while the camera looks
  down). A gesture that starts under an ImGui window (a gizmo panel, a notification, an open popup) is refused, naming
  it. A pixel given as `x`/`y` is not checked, except in a painting gizmo, where a down or drag that starts off the
  object being painted is refused, naming the first pixel along the stroke that lands on it, so a stroke swept in from
  outside is retried in one call.
- **Held presses.** A held press also ends when a gesture fails, when the replay or script that pressed it ends, and
  when the client that pressed it disconnects. A press on a web page also ends with any click, `dialog_answer` or
  auto-answer press, even one then refused, while keys and typing leave it held. Only the press is hit tested, so a
  drag split across calls may move and end outside the element.
- **A dialog's message.** A dialog's message is at most 2000 bytes, cut at a character with `...`. A list shows up to
  25 rows, then a line pointing to `widget_list` for the rest; a gauge reads `[45%]`, a read-only field as its text, and
  a notebook as its tabs, `(*) A | ( ) B`, followed by the page shown. A URL in a field shows without its credentials.
  An icon option of a selector is named by its tooltip, which `dialog_answer` takes whole even where a cut message
  shows only its start. Text that custom widgets paint themselves is not in the message (the AMS mapping items,
  `RecenterDialog`'s explanation, Preferences' row labels, icon buttons with no label); a screenshot shows it. Colour
  and font choosers are wx dialogs on Linux, which `dialog_answer` ends, and the system's own on macOS and Windows,
  best answered ahead with a `dialog_auto_answer` rule.
- **Auto-answer rules.** A path rule bypasses the picker's own overwrite question, and a caller that refuses the name
  and asks again (exporting G-code, for characters a FAT file system can't hold) is answered with the same path again.
  It answers a picker only as it opens, never one already open. GTK's own file chooser button, as in the SLA import
  dialog, opens its dialog without the hook, so no rule answers it. A wx dialog whose matched button is disabled by the
  time its loop runs stays open, and so does one a modal opened above first; `dialog_answer` then answers it. When a
  new rule matches a dialog already open but the first rule naming one of its buttons finds that button disabled or
  hidden, the dialog stays open and `answered_open_dialog` says `disabled: true` or `hidden: true`.
- **Web page locators.** A `{web}` locator matches a text line only when no element matches, and `css` is matched
  through shadow roots and frames. A field with no name of its own in a table cell, such as an entry of the flushing
  volumes matrix, is named `row / column` by its row's and its column's headers. While another page that could hold
  the element has not answered, a match is refused, since its uniqueness and `index` are unknown; `within` or a ref
  narrows the search.
- **What a page call does not reach.** A web view in a popup window (the setting tooltip's help), a dialog's hidden
  web view (the dialog is answered as a wx dialog), and cross-origin frames other than plugin configuration UIs, which
  are listed but not read; a read-only field takes no typed text. Points in a frame that a CSS transform or zoom
  scales are placed as in the untransformed frame. A page button whose handler outlasts `dialog_answer`'s 3 s may
  still be running when `cancel` or `close` ends its dialog.
- **The windows a person keeps.** The Run Script panel, the Agent Activity pane and the Python scripts dialog are the
  person's, as are the prompts and file dialogs of Action Replay and the Run Script panel: `widget_list`, locators and
  `webview_run_javascript` don't reach them, a key sent without a target never goes into them, no auto-answer rule
  answers them, and `window_list` lists a floating pane as hidden, with no ref. Docked, the two panes show in the main
  window's screenshots and recordings; floating, they don't. Action Replay's window itself is reached as the app's
  other windows are: `keyboard_press Escape` on its ref closes it.
- **URLs and timeouts.** A URL a reply shows (`navigated`'s, `webview_run_javascript`'s `url`, a web view's value in
  `widget_list`) reads `REDACTED` for its user info and for each query or fragment parameter whose name has a
  credential word in it, as a plugin page's bridge token would. A page call that times out says whether the page
  refused it unrun, having reached it past its deadline, or never answered, when whether it ran is unknown. A slow
  page call working in the topmost dialog replies `running`, not `blocked_by_dialog`, while that dialog stays the
  topmost.

## The arguments

### The arguments that say which variant

| Command | Argument | Values |
|---|---|---|
| `undo`, `redo` | `history` | project, design; left out, the Design tab's while it is shown, else the project's |
| `app_wait` | `for` | ready, idle, dialog, no_dialog, job |
| `app_open_web_page` | `page` | wiki, youtube, plugin_hub |
| `project_export` | `format` | gcode, sliced_3mf, 3mf, stl, drc, toolpaths_obj; left out, the path's extension says which |
| `object_add_primitive`, `part_add` | `shape` | cube, cylinder, sphere, cone, disc, torus |
| `object_add_handy_model` | `model` | orca_cube, orcasliced_combo, orca_badge, orca_tolerance_test, 3dbenchy, cali_cat, autodesk_fdm_test, voron_cube, stanford_bunny, orca_string_hell |
| `object_set_instances` | `delta` | 1, -1; or `count` |
| `object_split` | `into` | objects, parts |
| `object_order` | `by` | name, filament, height, size; or `order` |
| `object_mirror` | `axis` | x, y, z |
| `object_arrange` | `fill_with` | instances, copies, beside `fill_object` |
| `object_set_variable_layer_height` | `action` | adaptive, smooth, reset |
| `part_add`, `part_change_type` | `type` | part, negative, modifier, support_blocker, support_enforcer, precise_seam |
| `part_change_type` | `subtype` | center, left, right, enforced, blocked, neutral, beside `type` precise_seam or alone |
| `calibration_generate_test` | `test` | temperature, max_volumetric, pressure_advance, flow_ratio, retraction, vfa, cornering, input_shaping_freq, input_shaping_damp |
| `preset_list`, `preset_select`, `preset_save`, `preset_delete` | `type` | printer, process, filament |
| `settings_list`, `settings_list_pages` | `type` | process, printer, filament, object, region |
| `settings_get`, `settings_set` | `type`; `target` | printer, process, filament; global, object, part, height_range |
| `settings_set_mode` | `mode` | simple, advanced, expert |
| `filament_sync_ams` | `mode` | map, overwrite |
| `view_set` | `camera` | iso, top, bottom, front, rear, left, right, default, fit_bed, fit_objects, fit_selection, sketch_normal |
| `view_set` | `canvas` | 3d, preview, assemble |
| `view_set` | `projection` | perspective, orthographic, toggle |
| `view_set` | `view_type` | the Preview legend's color schemes: summary, feature_type, color_print, speed, actual_speed, height, width, volumetric_flow_rate, actual_volumetric_flow_rate, layer_time_linear, layer_time_logarithmic, fan_speed, temperature, pressure_advance, acceleration, jerk |
| `window_open` | `window` | preferences, plugins, action_replay, preset_bundle, keyboard_shortcuts, setup_wizard, network_test, troubleshoot, tip_of_the_day, about |
| `window_set` | `state` | normal, maximized, minimized |
| `recording_start` | `target` | main, canvas |
| `mouse_gesture` | `action` | move, down, up, click, drag, wheel |
| `gizmo_pick_file` | `action` | svg_change, svg_save_as, texture_import |

### Targets and the selection

A command that acts on objects, instances or parts takes them in its arguments, and one resolver reads them
for every command (`Mcp::resolve_targets()`, `McpTargets.hpp`):

- **Several targets**: `objects` (indices), `instances` (`[[object, instance], ...]`) and `parts`
  (`[[object, part], ...]`), as `object_select`, `object_delete`, `object_drop`, `object_mirror`,
  `object_center`, `object_scale_to_fit`, `object_auto_orient`, `object_set_printable`,
  `object_set_auto_drop`, `object_fix`, `object_clone`, `object_reload_from_disk`, `object_assemble` and
  `part_change_type` take them, each the kinds it acts on. `project_export` takes `objects` for the meshes it
  writes, and the selection does not stand in for them.
- **One target**: `object`, `instance` and `part`, as `object_transform`, `object_rename`,
  `object_set_instances`, `object_separate_instances`, `object_split`, `object_set_height_ranges`,
  `object_set_variable_layer_height`, `part_add`, `part_replace`, and the object and part targets of
  `settings_get` and `settings_set` take them.
- **Nothing named**: the targets are what is selected in the Prepare view, which the command shows first. Every
  instance of an object selected is the object, and an instance or a part stands for its object only where the
  command says so. Nothing selected is refused (`nothing_selected`, "Nothing is selected: name the objects, or
  select them first."), a selection of a kind the command does not take is refused (`wrong_selection`), and a
  command of one target refuses a selection of several (`one_object_only`). Where a menu item acts on the object
  list's own rows (`object_assemble`, `part_change_type`, `object_split`, and `object_delete`'s person's form), a
  call that names no targets reads those rows, since the list and the 3D selection disagree when every instance,
  or every part, of an object is selected.
- **Checked first.** Every index is checked before anything changes; naming one target two ways, an instance
  or a part without its object, and an empty list are refused.
- **By index or by id.** Every index a command takes has a twin that takes the id `object_list` reports
  for the same thing: `object_id`, `part_id` and `instance_id` beside one target, `object_ids`, `part_ids` and
  `instance_ids` beside lists, and `fill_object_id`, `order_ids` and `replace_ids`. An id stays with its object,
  part or instance when a delete, split or reorder moves the indices, so a call may name a target by id alone;
  given with its index, the id is a guard, and a call whose index names another thing now is refused before
  anything changes. Ids belong to one process, so the journal records each call with the indices its ids named
  and a replay addresses by index.
- **The reply names what was acted on** (`objects`, `instances`, `parts`, each with its index, id and name),
  so a caller that gave no targets learns what the selection was. A command that makes, removes or renumbers
  things replies with their indices as they are afterwards.

A plate command with no `plate` acts on the current plate and names it in its reply. An index the project
lacks is refused in the same words by every command, which give the plates there are.

### Refused and ignored arguments

- Every schema is closed: an argument a command does not have is refused, unless the server reads it as one
  the command has. Before validating, the server reads what agents commonly send in place of the schema's
  form, where the call leaves the schema's argument out and the value fits it: `object` as `objects`, `plate`
  as `plates`, `key` as `keys`, `settings` as `values`, `filter` as `query`, `ref` as `target`, `text` as
  `value`, `action` as `id` or `button`, and `path` as `save_path` where the command has no `path` of its own. A
  value in another JSON form is converted: a number, boolean, array or object written as a string, an index as a
  string where a name may also go (`"0"` for a plate), a single item for an array, also where null may stand for
  the array. A value outside its argument's enum moves to the one other argument whose enum takes it, when the
  call leaves that one out: `view_set`'s `canvas: "front"` is its `camera`. What still does not fit is refused.
  The journal records the schema's form.
- A null argument is an argument not given.
- An argument of one variant is refused with another: `per_object` with a `format` other than stl or drc,
  `fill_with` without `fill_object`, `quality` with an `action` other than adaptive, `descending` with
  `order`, `size` with a file's part.
- A path is absolute, and the folder of an output must exist. `overwrite: true` is what replaces something
  that exists: a file for `project_save`, `project_export`, `window_screenshot`, `replay_export`,
  `recording_start`, `recording_encode` and `gizmo_pick_file`, another preset for `preset_save`, an installed
  plugin for `plugin_install_local`. A screenshot or recording saved without `save_path` goes in the session's
  output folder: the `--outputdir` folder of a `--run` launch, else `<datadir>/automation/out`.

### How a failure shows

- A call that is refused, or that fails, is an `isError` result: the English message, and a stable `code`
  (`nothing_selected`, `slicing_in_progress`) where an operation refused it, or `invalid_argument` where its
  arguments are what is wrong, by every route (`tools/call`, `tool_call`'s target, `action_run`, a script's or
  plugin's `orca.host.tools`). That covers what the shared checks refuse: the schema (an unknown or misspelt name,
  a wrong type, a missing one), a command, action or group name given as an argument that names none
  (`tool_call`'s and `tool_describe`'s `tool`, `tool_list`'s `group`, `action_run`'s `id`, a script's call), the
  slicer commands' target indices and forms, `[x, y, z]` vectors and scale factors, a setting's value or key; and
  what a Design command's own checks refuse. A refusal of a call's arguments by its schema carries the `hint`
  that `tool_describe {tool}` gives the command's parameters, the command being a `tool_call`'s target or an
  action's command. A name that is right but that this run cannot use is not
  `invalid_argument`: a Design command or action left out of the run (the G-code viewer, the CAD feature off at
  startup), or a Design action while the CAD feature is off, is `design_unavailable`; a Design tab action outside
  the tab or mode that shows it is `wrong_mode`, and the `tool_call` its message names then says what keeps the
  command from running, a Design panel that cannot be built included (`design_unavailable`). The other checks inside a slicer command's own
  function carry no code (`replay_run`'s "give exactly one of path and replay", `object_transform`'s "give scale or
  size, not both", the menu commands' file paths), nor does a direct `tools/call` of an unknown tool, a JSON-RPC
  error, or a Python `TypeError` for arguments that are not a dict. The names that help (the closest names to an
  unknown one, or every name there is) come under `similar`, `did_you_mean`, `groups` or `presets`. A refusal whose text, which a person reads too,
  asks for a GUI step or a selection that a call can do instead (close a toolbar menu, deselect, select a part)
  carries a `hint` with the commands that do it (`refusal_hint()`), as a Design menu item of the other mode and a
  busy Design tab do; one whose step only a person takes (sign in, connect a printer) has none. An operation refuses
  before it changes anything; a command that can fail after it acted says what it left (`plugin_enable` names how
  the plugin is left, `object_fix` keeps what it repaired before a cancel).
- A call that opens a modal dialog replies `{status: "blocked_by_dialog", job, dialogs}`, and the caller
  answers the dialog (`dialog_answer`), after which `app_wait {for: "job"}` returns the call's result.
- A call whose work outlasts the reply budget (45 s) replies `{status: "running", job}`, and
  `app_wait {for: "job"}` returns its result; `job_cancel {job}` stops it. While the server's slots for calls that
  may block are all taken, a job call replies `running` at once and any other call `{status: "pending"}`.
- A direct `tools/call` of an unknown command is a JSON-RPC error that names the closest commands; an unknown
  command given as an argument (`tool_call`'s `tool`, a script's call) is an `isError` result with
  `invalid_argument`, as above.
- `replay_export` leaves out the calls that failed, at once or later as a job.

## When a call opens a dialog or a tool

One rule holds for every command: **a call whose arguments are exactly those of one of the command's actions
is the person's form, and does what a person's pick of that action does; any other call runs from its arguments
and opens nothing.** A command's own action is `{}`.

- The person's form may ask in a dialog, as the menu item does, or act on the selection. An agent's call of it
  replies `blocked_by_dialog` where the item asks.
- Where the command's own function needs nothing more than an action gives, `silent: true` runs it from its
  arguments without asking, with its defaults: `object_add_primitive {shape: "cube", silent: true}` adds the
  cube at its built-in size, where `{shape: "cube"}` asks for the size. Sixteen commands take `silent`:
  `project_open`, `project_import`, `project_new`, `project_save`, `project_export`, `object_add_primitive`,
  `object_delete`, `object_rename`, `object_set_instances`, `object_split`, `calibration_generate_test`,
  `plate_rename`, `preset_import`, `plugin_install_local`, `recording_start` and `recording_stop`. The
  journal keeps `silent`, so a replay of such a call asks nothing either.
- `project_save`'s `path` is required in every call that is not one of its actions'. `recording_start` given
  arguments records for up to an hour unless `max_seconds` says otherwise, and its reply says the limit it runs
  under.
- A call that runs from its arguments and meets on its way the question a menu item asks takes the answer as
  an argument, or refuses with the question as its reason: Split's Auto-Drop question (`keep_z`, else
  `auto_drop_question`), deleting part of a cut object, replacing an installed plugin (`overwrite`), the
  presets' unsaved changes (`preset_changes`).
- Where a menu item does just what the command does, its action is the command's own form and a call of it
  replies as any call: the camera actions of `view_set`, Select All and Deselect All, Auto-Arrange, Auto-Orient
  and Refresh Plugins.
- A call of the person's form is refused while a modal dialog is open or another call's nested loop runs,
  unless that form opens nothing: `recording_stop {}` stops the recording wherever the command runs.

### The actions that ask, and the call that does not

| Command, called as its action | It asks | The call that does not ask |
|---|---|---|
| `project_open {}` | the unsaved changes question, then the Open dialog | `path`, with `discard_changes` where the project has unsaved changes |
| `project_import {}`, `project_import {archive: true}` | a file dialog; the archive, then its file list | `paths`, and `entries` for an archive |
| `project_new {}` | the unsaved changes question | `discard_changes`, `preset_changes`, or `silent: true` |
| `project_save {}`, `project_save {save_as: true}` | a file, when the project has none; always | `path` |
| `project_export {format}`, also with `plates: "all"` or `per_object: true` | a file or folder dialog | `path` |
| `object_add_primitive {shape}` | the size, in the Primitive size dialog | `size`, or `silent: true` for the built-in size |
| `object_rename {}`, `plate_rename {}` | the name | `name` |
| `object_set_instances {}` | the count | `count` or `delta` |
| `object_split {into}` | Split's Auto-Drop question, where pieces would float | `keep_z` |
| `object_delete {}` | the object list's message or question for a cut object or a last solid part | the targets; the question becomes the refusal |
| `object_add_svg {}` | a file dialog | `path` |
| `calibration_generate_test {test}` | the test's dialog | any value of the test, or `silent: true` |
| `preset_import {}` | a file dialog | `path` |
| `plugin_install_local {}` | a file dialog, and whether to replace an installed plugin | `path`, with `overwrite` |
| `filament_sync_ams {}` | the sync dialog | `mode` |
| `filament_add_mixed {}` | the Add Mixed Filament dialog, where a gradient is set | `components` and `ratios` |
| `filament_edit_mixed {filament_slot}` | the Edit Mixed Filament dialog on that mix, where a gradient is set | `components` and `ratios` |
| `filament_delete {filament_slot, merge_into}` into a mixed filament that mixes the slot | Merge with's question | `break_mix` |
| `recording_start {}` | the Start Recording dialog | any argument, or `silent: true` |
| `window_open {window}`, `app_open_web_page {page}` | nothing: opening is the job | the same call |

These open a dialog or a gizmo whatever their arguments, and the caller answers it:

| Command | What opens | How a caller answers it |
|---|---|---|
| `preset_export` | the Export Preset Bundle dialog, on the folder `path` names when one is given | click the label of the kind (`widget_click`), check the presets' rows (`widget_set_value` on the checkboxes named after the printer preset or filament they stand for), press OK (`dialog_answer`) and answer the result message; a dialog closed without exporting makes the call an error |
| `project_import`, `part_add` and `part_replace` with a path | the loader's questions: a STEP file's mesh options, the AMF and CAD 3MF options, the names clean-up, inches or metres, one object of several parts | the call replies `blocked_by_dialog` where the loader asks, and `dialog_answer` answers it; no file dialog opens |
| `print_start`, `printer_send` | the dialog of the Print button's action of that name for the selected printer (see The printing commands' dialogs) | its fields with `widget_list` and `widget_set_value`, its buttons with `dialog_answer`; in a print host's upload dialog, the file name, then Upload or Upload and Print, after which the plate is sliced if needed and uploaded in the background (`app_state.uploads`) |
| `app_benchmark_3d_scene` | Start or Cancel, then New Project's unsaved changes question | `dialog_answer` |
| `preset_migrate_filaments`, `app_check_for_updates` | the migration's dialog; the answer to the check | `dialog_answer` |
| `object_add_text`, `object_add_svg` | none: the Text or SVG gizmo opens on the new part | the gizmo, through `widget_list` and the input commands |

### The gizmo commands: opening or acting

`object_paint`, `object_measure`, `object_simplify`, `part_boolean`, `object_cut`, `object_lay_on_face` and
`object_set_brim_ears` each stand for a gizmo of the Prepare view. Their rules add to the one above:

- **A call that names only its targets opens the gizmo on them.** It shows the Prepare view, selects the target as
  `object_select` does, and opens the gizmo as its toolbar icon does; with no target, on the selection as it is. The
  reply is the gizmo's name, as `app_state.gizmo` gives it, `active`, and what is selected. The client reads the
  gizmo's panel with `widget_list {include_imgui: true}`, sets its fields with `widget_set_value`, presses its
  buttons with `widget_click`, clicks or drags on the model with `mouse_gesture`, and closes it with
  `object_select {none: true}`.
- **A call with any of its command's acting arguments does the job with no gizmo**, as the gizmo's own button or click
  does, through the same functions, as one undo step. A command's description names its acting arguments in one fixed
  sentence ("Acts from arguments: ..."). Acting arguments that need a plane or a direction and have none are refused
  (`needs_plane`, `needs_normal`).
- **None of them takes `silent`**: whether a call opens or acts is read from its arguments.
- **Opening is not a toggle.** The gizmo already open on the same target stays open, and the reply is the same; a
  click on its icon would close it.
- **A gizmo that cannot open says why**, in the gizmo's own condition, and the selection is put back as the call found
  it, with no undo step: `needs_two_parts` for Mesh Boolean, `needs_two_filaments` for Color Painting, `needs_fff_printer` for the
  painting gizmos, `needs_one_instance`, `needs_one_part`, `needs_instance_or_part`, `needs_selection`,
  `layers_editing_on`, `dowel_alone`.
- **A call that acts closes the open gizmo first**, as Perform cut does, since a gizmo writes its own state to the
  model as it closes; a refused call leaves it open.

| Command | Opens | Acting arguments | Left to the gizmo |
|---|---|---|---|
| `object_cut` | Cut, on `object`, `instance` | `z`, or `plane` (`point`, `normal`); `keep`; `upper`, `lower`; `to_parts` | connectors, the dovetail, a cut along a contour; `object_fix` repairs the open edges a reply reports |
| `object_lay_on_face` | Lay on Face, on `object`, `instance` | `normal`, in the object's frame or with `frame: "world"` in the 3D view's | picking one of the flat faces the gizmo finds; the other instances turn with it |
| `object_set_brim_ears` | Brim Ears, on `object`, `instance` | `points` (bed positions, each with an optional `radius` within 0.1 to 100 mm), or `clear` | Auto-generate, with its angle and detection radius; moving an ear. The ears are the object's, stored in its coordinates, and take effect where its brim type is `painted` |
| `object_paint` | the painting gizmo of `type`, on `object`, `instance` | none | all painting |
| `object_measure` | Measure, on `objects` | none | everything |
| `object_simplify` | Simplify Model, on a part: the one named, or an object's only one | none | the detail level or ratio, and Apply |
| `part_boolean` | Mesh Boolean, on `object` (its parts) or `objects` (whole, on one plate) | none | the operation, the inputs, Delete input, and the button |

### The printing commands' dialogs

`print_start` and `printer_send` are the Print button's actions of those names, read from the button's own list for
the selected printer (`MainFrame::available_print_actions()`), so a call and the button cannot drift apart. Each is
refused where the button offers no such action (`all_plates_not_offered`), and a route through a print host is
refused with no `print_host` address (`no_print_host`). As the button's click does, `print_start` applies the slicing
result first and is refused where the button would be disabled (`not_sliced`), and so is `printer_send` where it opens
the storage dialog; a send to a print host slices the plate itself. `plates` names a plate, which becomes the current
one; a refusal after that makes the plate that was current before current again.

| The selected printer | `print_start` | with `plates: "all"` | `printer_send` | with `plates: "all"` |
|---|---|---|---|---|
| On the Bambu network, or with printer agents on | the Print dialog | the Print dialog, for every plate | the dialog that sends to the printer's storage | the same, for every plate |
| A Bambu preset set to a print host | the host's upload dialog | the upload of every plate, for a SimplyPrint host; refused for the others | the host's upload dialog | refused |
| Another vendor's printer with a print host | the host's upload dialog | refused | the host's upload dialog | refused |

The dialog is left to the client: the printer, bed levelling, flow calibration, timelapse, the AMS mapping and the
button that prints, or the file name, the storage and the button that sends. Nothing prints or sends until that
button is pressed.

A printer's print host is a printer setting (`host_type`, `print_host`, `printhost_apikey`, and `bbl_use_printhost`
on a Bambu printer), which `settings_set {type: "printer"}` writes. An upload is a row of `app_state.uploads`, a
person's uploads included: `id` (the queue's ID column, from 1), `state` (`queued`, `uploading`, `error`,
`cancelling`, `cancelled` or `completed`), `progress` (percent), `host`, `filename` and the queue's `error`. A row
reads `completed` only once the host has accepted the file, so a host that refuses it after the last byte turns the
row to `error` without it reading completed on the way. A failed upload also opens the queue's error dialog, which
holds every change until `dialog_answer` closes it.

`printer_status`, `print_pause`, `print_resume` and `print_stop` act on the printer the Device tab is connected to
(`DeviceManager::get_selected_machine()`), whether or not the tab is shown, and open nothing. With none connected
`printer_status` replies `{"connected": false}` and the job commands are refused `no_printer_connected`. A job
command is refused, naming the state, where the printer's state does not allow it (`printer_state`: Pause while
running, Resume while paused, Stop while running, paused, preparing or slicing), and Resume where a job resumes
only at the printer (`resume_at_printer`, in the app's own sentence). The printer's new state arrives when it
reports, and `printer_status` reads it. `printer_test_host` asks the print host whether it answers, with the
connection settings as they stand; a cloud host that wants a login is a failed test with the host's message, since
logging in stays in the printer's connection dialog.

### Every action and the call it makes

An action's id is `orca_command:<key>:orca`, under the key of the menu item it was: a command's own action
under the command's key (`undo`, `redo`, `plate_add`, `plate_delete`, `plate_duplicate`, `plate_rename`,
`filament_add`, `filament_delete`), any other under the item's id.

| Command | Its actions, and the call each makes |
|---|---|
| `undo` | Undo `{}` |
| `redo` | Redo `{}` |
| `app_open_web_page` | Open Wiki `{"page": "wiki"}`; Open YouTube Channel `{"page": "youtube"}`; Install Plugin `{"page": "plugin_hub"}` |
| `app_check_for_updates` | Check for Updates `{}` |
| `app_show_config_folder` | Show Configuration Folder `{}` |
| `app_benchmark_3d_scene` | Benchmark 3D Scene `{}` |
| `project_open` | Load Project `{}` |
| `project_import` | Import 3MF/STL/STEP/SVG/OBJ/AMF `{}`; Import ZIP Archive `{"archive": true}` |
| `project_new` | New Project `{}` |
| `project_save` | Save Project `{}`; Save Project As `{"save_as": true}` |
| `project_export` | Export G-code `{"format": "gcode"}`; Export Sliced File `{"format": "sliced_3mf"}`; Export All Sliced Files `{"format": "sliced_3mf", "plates": "all"}`; Export 3MF `{"format": "3mf"}`; Export STL `{"format": "stl"}`; Export All Objects as STLs `{"format": "stl", "per_object": true}`; Export All Objects as DRC (one file) `{"format": "drc"}`; Export All Objects as DRCs `{"format": "drc", "per_object": true}`; Export Toolpaths as OBJ `{"format": "toolpaths_obj"}` |
| `object_select` | Select All Objects `{"all": true}`; Deselect All `{"none": true}` |
| `object_add_primitive` | Cube `{"shape": "cube"}`; Cylinder `{"shape": "cylinder"}`; Sphere `{"shape": "sphere"}`; Cone `{"shape": "cone"}`; Disc `{"shape": "disc"}`; Torus `{"shape": "torus"}` |
| `object_add_handy_model` | Orca Cube `{"model": "orca_cube"}`; OrcaSliced Combo `{"model": "orcasliced_combo"}`; Orca Badge `{"model": "orca_badge"}`; Orca Tolerance Test `{"model": "orca_tolerance_test"}`; 3DBenchy `{"model": "3dbenchy"}`; Cali Cat `{"model": "cali_cat"}`; Autodesk FDM Test `{"model": "autodesk_fdm_test"}`; Voron Cube `{"model": "voron_cube"}`; Stanford Bunny `{"model": "stanford_bunny"}`; Orca String Hell `{"model": "orca_string_hell"}` |
| `object_add_text` | Text `{}` |
| `object_add_svg` | SVG `{}` |
| `object_delete` | Delete Selected `{}`; Delete All Objects `{"all": true}` |
| `object_rename` | Rename `{}` |
| `object_set_instances` | Set Number of Instances `{}`; Increase Instances `{"delta": 1}`; Decrease Instances `{"delta": -1}` |
| `object_separate_instances` | Set as Individual Objects `{}` |
| `object_split` | Split to Objects `{"into": "objects"}`; Split to Parts `{"into": "parts"}` |
| `object_assemble` | Assemble `{}` |
| `object_order` | Order by Name `{"by": "name"}`; Order by Filament `{"by": "filament"}`; Order by Height `{"by": "height"}`; Order by Size `{"by": "size"}` |
| `object_drop` | Drop to Bed `{}` |
| `object_mirror` | Mirror X `{"axis": "x"}`; Mirror Y `{"axis": "y"}`; Mirror Z `{"axis": "z"}` |
| `object_center` | Center Selected on Plate `{}` |
| `object_scale_to_fit` | Scale to Fit Print Volume `{}` |
| `object_auto_orient` | Auto-Orient `{}` |
| `object_arrange` | Auto-Arrange `{}` |
| `part_change_type` | Change Type to Part `{"type": "part"}`; Change Type to Negative Part `{"type": "negative"}`; Change Type to Modifier `{"type": "modifier"}`; Change Type to Support Blocker `{"type": "support_blocker"}`; Change Type to Support Enforcer `{"type": "support_enforcer"}`; Change Type to Precise Seam `{"type": "precise_seam"}` |
| `calibration_generate_test` | Temperature Calibration `{"test": "temperature"}`; Max Volumetric Speed Calibration `{"test": "max_volumetric"}`; Pressure Advance Calibration `{"test": "pressure_advance"}`; Flow Ratio Calibration `{"test": "flow_ratio"}`; Retraction Calibration `{"test": "retraction"}`; Cornering Calibration `{"test": "cornering"}`; Input Shaping Frequency Calibration `{"test": "input_shaping_freq"}`; Input Shaping Damping Calibration `{"test": "input_shaping_damp"}`; VFA Calibration `{"test": "vfa"}` |
| `plate_add` | Add Plate `{}` |
| `plate_delete` | Delete Plate `{}` |
| `plate_duplicate` | Duplicate Plate `{}` |
| `plate_rename` | Rename Plate `{}` |
| `plate_lock` | Toggle Plate Lock `{}` |
| `plate_select` | Go to Plate `{"plate": 0}` |
| `plate_slice` | Slice and Preview `{"preview": true}`; Slice All Plates `{"plates": "all", "preview": true}` |
| `preset_import` | Import Configs `{}` |
| `preset_export` | Export Preset Bundle `{}` |
| `preset_sync` | Sync Presets `{}` |
| `preset_migrate_filaments` | Migrate Filament Presets `{}` |
| `settings_set_mode` | Mode: Simple `{"mode": "simple"}`; Mode: Advanced `{"mode": "advanced"}`; Mode: Expert `{"mode": "expert"}` |
| `settings_toggle_developer_mode` | Toggle Developer Mode `{}` |
| `filament_add` | Add Filament Slot `{}` |
| `filament_delete` | Delete Filament Slot `{}` |
| `filament_sync_ams` | Synchronize Filament List from AMS `{}` |
| `printer_send` | Send G-code to printer host `{}` |
| `plugin_install_local` | Install Local Plugin `{}` |
| `plugin_refresh` | Refresh Plugins `{}` |
| `view_toggle_activity_panel` | Agent Activity `{}` |
| `view_set` | View: Top `{"camera": "top"}`; View: Bottom `{"camera": "bottom"}`; View: Front `{"camera": "front"}`; View: Rear `{"camera": "rear"}`; View: Left `{"camera": "left"}`; View: Right `{"camera": "right"}`; View: Isometric `{"camera": "iso"}`; View: Default `{"camera": "default"}`; Fit Bed to View `{"camera": "fit_bed"}`; Toggle Perspective `{"projection": "toggle"}`; Reset Window Layout `{"layout": "default"}`; Go to tab... `{"tab": <the tab>}`; Go to layer (percent) `{"layer_percent": <the percentage>}` |
| `window_open` | Preferences `{"window": "preferences"}`; Open Plugins `{"window": "plugins"}`; Action Replay `{"window": "action_replay"}`; Open Preset Bundle `{"window": "preset_bundle"}`; Keyboard Shortcuts `{"window": "keyboard_shortcuts"}`; Setup Wizard `{"window": "setup_wizard"}`; Open Network Test `{"window": "network_test"}`; Troubleshoot Center `{"window": "troubleshoot"}`; Show Tip of the Day `{"window": "tip_of_the_day"}`; About OrcaSlicer `{"window": "about"}` |
| `window_save_screenshot` | Take Screenshot `{}` |
| `script_toggle_panel` | Run Script `{}` |
| `recording_start` | Start Recording `{}` |
| `recording_stop` | Stop Recording `{}` |

Go to tab and Go to layer collect a value in the Speed Dial's second phase, which the action passes on under
`tab` or `layer_percent`; `action_run` takes it under the action's own name for it, `tab` or `percent`. Go to
Plate goes to the first plate, as its menu item did, and the Speed Dial's plate family calls `plate_select`
with each plate. An action of `object_order` reverses the order the objects already have, as Order By does;
`descending` says which way.

## Actions, titles, the journal and replays

- An action's id is what a person's pins and run counts are stored under, and the ids of the menu items a
  released build stored stay the ids of actions, whatever their command is called. The unit test "every Speed
  Dial action of today is still a command's action" holds each one to its title, section, icon and value.
- An action runs its command with its arguments, with the MCP server on or off. `action_run` takes no `args`
  for an action, or exactly the action's, or the value of an action that collects one, and journals the step
  as that `action_run`, so a replay runs it again. Its reply names the action that ran (`ran`), only when the
  command ran.
- A step does the same on another machine. A command's defaults are fixed values, never what a person last set in a
  dialog or a panel (a primitive's built-in size, the Variable layer height panel's defaults, filament slot 1), and
  where a call takes values from the app's saved state the journal records the values it ran with:
  `object_arrange`'s arrange settings, `calibration_generate_test`'s parameters with their defaults, the plate
  `plate_set_settings` and `plate_set_custom_gcode` resolved. A value the presets give is left out, so that the
  replay computes it again from its own presets (a pressure advance pattern's acceleration and speed).
- How `replay_run` runs its steps: a string that is just one placeholder becomes the JSON value it names, and a
  `script_run` step's `code` is never substituted. A nested `replay_run` step gives its replay as an object, since
  placeholders are spliced into a replay's text as plain text, where a quote or a Windows path's backslash would
  break its JSON. A step that returns a job is awaited up to its `timeout_s` (default 120); a wait that times out
  with a dialog open names it, since a step can't answer a dialog it opened, which `"await": false` lets later steps
  do. An `app_wait` on a call that finished within its reply budget gives that step's result at once. A `script_run`
  step whose outcome is not `finished` fails. A stop fails the step it came in as `cancelled` and ends the run,
  `optional` or not, nested replays included.
- `replay_run`'s reply lists each step as `{index, tool, args, ok, duration_ms, result | error, expect_failure,
  cancelled, reason}`. A failed step's `reason` says why in parts a host can word: `cancelled`, `timeout {seconds}`,
  `expect {pointer, op, expected, actual, exists}`, `expect_invalid`, `placeholder {text}`, `missing_job {name,
  saved_by}` (the step that saves that job, when one does) or `tool`, the tool's own error. A replay whose recording
  was still being written when it ended names the file as `unfinished_recording`, since an encode of it would fail.
- A replay naming an unknown tool, with a field of the wrong type or with broken JSON (reported with its line and
  column), or of a format version this build can't read (saying whether a newer OrcaSlicer is needed), is refused
  before any step runs.
- A call whose job was still running when a later call acted on a modal dialog stays in `replay_export`'s replay as
  an `"await": false` step, since that dialog may be its job's, and each call that acted with a dialog open is
  preceded by `app_wait {"for": "dialog"}`, since its replayed reply can come before the dialog opens. The journal
  keeps only a session's latest calls (mcp-server.md, Replays and the journal), so an export of a very long session
  starts partway through.
- A call of a command with exactly an action's arguments is titled as that action wherever a person is shown
  the call: a job notice, a recording's caption, an Action Replay step.
- An action picked by a person runs in place, on the UI thread, but for a long command's: Slice and Preview
  and Slice All Plates start the slice as a job and show its result as a notification, where a call waits for
  the result.

How the actions are made and found is in mcp-server.md, Speed Dial catalog.

## When a command may run

Each command runs under a guard (mcp-server.md, Threading):

- **Inspect**, 29 commands: the lists and reads, `app_state`, `app_wait`, `app_log`, `printer_status`,
  `window_screenshot`, `widget_list`, `replay_export` and the three recording commands. They run behind a
  modal dialog and inside another call.
- **Recover**, 9 commands: `job_cancel`, `window_set`, `dialog_answer`, `dialog_auto_answer`, and the input
  commands that act on what is on screen, `widget_click`, `widget_set_value`, `keyboard_press`,
  `mouse_gesture` and `webview_run_javascript`. They act on a modal dialog that holds the app; aimed outside
  the topmost dialog, the input commands are held to the mutate rules.
- **Mutate**, the other 97. A call is refused while a modal dialog is open, or while it would run inside
  another call's nested loop, and changes to the project are refused while a slice, an export or a
  background job holds it.

Access labels, which `tools/list` sends as MCP's annotations (mcp-server.md, Tool catalog, says why each is
set as it is):

- **read_only**, 25 commands: `tool_list`, `tool_describe`, `action_list`, `app_state`, `app_wait`, `app_log`,
  `preferences_get`, `project_get_settings`, `filament_list`, `object_list`, `plate_get_settings`, `plate_list`,
  `plate_slice_stats`, `preset_list`, `settings_list`, `settings_list_pages`, `settings_get`, `printer_list_models`, `printer_status`,
  `plugin_list`, `window_list`, `window_screenshot`, `script_list_api`, `script_describe_api` and `widget_list`. These are idempotent too, and so
  is `dialog_auto_answer`.
- **destructive**, 3: `preset_delete` and `plugin_uninstall`, whose every call removes what a person made,
  with no undo, and `print_stop`, which ends a print with no question.
- **open_world**, 21, which can reach a printer, the cloud account or the network: `tool_call`, `action_run`,
  `replay_run` and `script_run` through what they run; `app_open_web_page`, `app_check_for_updates`,
  `preset_save`, `preset_delete`, `preset_sync`, `filament_sync_ams`, `print_start`, `printer_send`,
  `printer_test_host`, `print_pause`, `print_resume`, `print_stop`, `plugin_enable`, `plugin_reload`,
  `plugin_uninstall`, `plugin_refresh` and `webview_run_javascript`. `printer_status` reads what the app holds and
  is not among them.

## Behaviour a caller relies on

- **Indices move.** An object's index numbers it in the project and a part's in its object, whatever the
  plate. A delete moves the objects after it down; `object_split` into objects, `object_assemble` and
  `object_separate_instances` add their objects last; an arrange or `object_order` may reorder them. Each
  reply gives the indices afterwards, and `object_delete` lists under `kept` what it was asked for and did not
  delete. Ids stay with what they name: pass them beside indices (see Targets and the selection).
- **Placement.** `object_transform` on an instance rotates and scales that instance alone, where the Object
  manipulation panel applies a rotation or a scale to every instance of the object. `object_list`'s `mirrored` says a
  placement is mirrored, not along which axis, since a mirror and a rotation do not split one way (mirrored in X and
  in Y is a half turn about Z). A variable layer height profile is the object's and is saved with the project; the
  slicer drops it once it no longer ends at the object's top, as after a scale, so set it after scaling.
- **Undo.** A command that changes the objects, their parts or the plates is one undo step; `object_clone`
  with `arrange` is two, the copies and then the arrangement, and `project_set_settings` takes one for a prime
  tower move. A gizmo that records undo steps of its own as it opens or closes, as Cut, Brim Ears and the painting
  gizmos do, adds them around a gizmo command's call. Not in the undo history: `filament_add`, `filament_delete`, `filament_add_mixed`,
  `filament_edit_mixed`, `filament_set_colour`, `filament_set_flushing`, `filament_sync_ams`,
  `filament_install`, `filament_uninstall`, `printer_install`, `printer_uninstall`, a preset's change by
  `settings_set`, `preferences_set`, and `plate_set_custom_gcode`, as in the layer slider. These calls take no
  undo step when they change nothing: `object_set_height_ranges` given the ranges an object has, a Reset of an object
  with no variable layer height profile, and `plate_lock` and `object_order` given what the plate has already.
- **Unsaved changes.** `project_open`, `project_new` and `calibration_generate_test` refuse while the project
  has unsaved changes, unless `discard_changes`. The presets' unsaved changes are answered by
  `preset_changes`: keep or discard for a new project and a calibration test, transfer or discard where a
  preset or a printer is selected (`preset_select`, `printer_install`, `printer_uninstall`); `preset_select`
  without it opens the Transfer-or-discard prompt. Modified presets count as the project's unsaved changes too, and
  so does a change made only in the Design tab, so these commands then need `discard_changes` as well. Without
  `preset_changes`, `project_new` keeps the modified presets for the new project, while `calibration_generate_test`
  refuses and lists their changed keys, since a previous test's settings would spoil the next. `transfer` moves what
  the prompt would list, the changed settings the current mode shows, so a setting the mode hides is dropped, as in
  the GUI.
- **Settings values** are strings in OrcaSlicer's format (`"15%"`, `"0.2"`, `"1,2"`) or JSON numbers,
  booleans and arrays. `settings_set`, `preferences_set`, `project_set_settings`, `filament_set_flushing` and `plate_set_settings`
  check every value first and write them together, so nothing changes when one is refused. Without `type`,
  `settings_set` writes the process settings and `settings_get` reads them, but for named `keys` with neither `preset`
  nor `compare`, which it looks up in the process, filament and printer settings, since an agent naming a key seldom
  knows which tab holds it. A write takes one type, whose values apply together. The values of one call are not
  ordered by what depends on what: a setting that applies only once another is set goes in a second call. An array
  for a per-variant value has one entry per variant; another length is refused, since it would collapse the vector
  and leave the preset inconsistent with its variants. On an object, `extruder` clears its parts' own filaments, as
  Change Filament does, and a filament number past the project's slots is refused, as is a mixed slot for a support
  filament.
- **Preferences a call does not set.** `preferences_set` refuses a preference whose control opens a dialog, restarts
  OrcaSlicer, signs out or reaches outside it, this server's own switch and port among them, which would end or move
  the connection the call came through. Those change in the dialog, which `window_open {window: "preferences", tab,
  key}` opens at the key's control: `widget_set_value` on the control named after the key changes a checkbox, a
  number or text field and the language list through its own handler, but the other drop-down lists (the log level,
  the login region, the network plug-in version and the like) save from a handler on their list, where a person's
  pick arrives, so `widget_set_value` shows the new choice there and saves nothing.
- **What is shown.** A command that acts on the 3D selection shows the Prepare view first; `plate_slice`
  shows the Preview, as the Slice button does; `plate_set_custom_gcode` selects its plate and shows the
  Preview, since it acts on the plate's sliced layers. A command that works through the object list's rows
  (`object_transform` on a part, `part_add`, `object_set_height_ranges`, `object_set_variable_layer_height` and
  `object_arrange`'s Fill bed) selects the object's or the part's row first, since the GUI functions it runs act on
  the selection, so it refuses the assemble view and an open gizmo, and leaves that row selected.
- **Layer G-codes.** `plate_set_custom_gcode` keeps the layer slider's rules: a plate that prints by object, or has
  nothing to print, takes no codes; a template needs the printer's `template_custom_gcode`; a filament change needs
  two filaments or more, a slider that offers one (the plate's objects on one filament, no spiral vase) and the
  slice's filament count; custom G-code is 1 to 1023 bytes, as the Custom G-code dialog takes. A call that changes
  nothing leaves the slice valid, and a pause on a printer with no `machine_pause_gcode` is a warning. When the
  slider next loads the plate it drops the codes it cannot keep: the filament changes once the objects print with
  different filaments, and every code once the plate prints by object or across a spiral vase change. `plate_list`'s
  `layer_gcode` can also hold `color_change`, an old project's colour change, which no call adds.
- **Work in the background.** `plate_slice`, `project_export` of G-code, `object_arrange`,
  `object_auto_orient`, `object_fix`, `script_run`, `replay_run`, `recording_encode` and `plugin_refresh`
  wait for their work within the reply budget, then reply with a job. A slice or export holds the project
  until it ends, and `job_cancel` stops it.
- **The same rules by every route.** A command, its Speed Dial action and the plugin API share one operation, so
  they refuse alike. An arrangement shows the Prepare view first, runs with a gizmo open as the toolbar's does, and
  is refused while the job worker is busy. A slice is requested as the Slice button requests it, with no check of
  the printer's technology. A printer or process selection that leaves another preset selected is refused, and that
  preset stays selected. An STL, a sliced file and G-code need objects, while a 3MF of the model does not, since a
  project may hold only a Design-tab recipe.
- **One change at a time.** A slice is superseded when someone else starts one or edits the project while it
  runs, and `plate_slice` says so per plate; an exported G-code says `superseded` when the project changed
  while it was written.
- **Screenshots.** A `window_screenshot` right after a call shows that call's effect settled, a `view_set`'s layers
  and legend included, so no wait is needed between them. A minimized or hidden window can't be captured. A 3D view
  that reads back blank has not been drawn yet: it is captured again for a few seconds, then returned `approximate`,
  the `note` saying whether the window is being drawn.
- **Recordings.** One recording runs at a time, and it counts as running until its file is closed, which
  `recording_stop {silent: true}` waits for and `recording_stop {}` does not. Besides `recording_stop` and
  `max_seconds`, a recording ends at 4 GiB, on a write error, with the replay or script that started it and, for a
  client's, when the server is turned off; `recording_stop` captures a last frame first, so the last call's effect is
  in the video. On macOS a main-window recording
  reaches about 12 frames a second whatever its `fps`, since each capture waits for the window server. The encode of
  a person's recording (Start Recording with Encode to MP4) is a job that no reply names: `app_state.jobs` lists it,
  and `app_wait {for: "job"}` waits for it by its id.
- **Preview data.** `plate_slice_stats` reads any plate without selecting it. It is refused while anything slices or
  exports, since a result may be being rewritten, and a plate of a project opened from a sliced 3MF that the Preview
  has not shown holds only the file's totals, so it is refused until it is sliced again. The feature shares leave out
  retractions, wipes, Z moves, tool changes and the prepare time, as the legend's do, and each layer carries its Z,
  so a tool that numbers layers by the slider's label joins on it. `view`, and `app_state.preview`, are null for a
  plate the Preview has not loaded. `view_set` changes the Preview as the legend's own controls do, so a `view_type`
  it picks is also saved as the "Last used" preview default (`preview_last_view_type`), and before a slice it only
  sets the type the next toolpaths show. Every legend key and the time mode are checked before anything changes, and
  a row the print lacks is ignored, so a caller can hide everything but one feature by naming rows from the whole
  list.
- **Scripts.** `script_run` returns at most 256 KiB of output. A `result` that is not JSON makes code sent as text
  `raised`; in a file run, where a standalone script may well have a variable of that name, it is no result, and the
  output says so. `SystemExit` with code 0 or None finishes the run. `timeout_s` and `job_cancel` don't interrupt a
  file run's PEP 723 install, which uv's own timeout bounds. `threading.main_thread()` is the app's main thread, and a
  daemon thread still running after the run sees the run's globals cleared and can't call tools. The auto-answer
  rules, recording and held presses a run's calls leave end with the run.

## Where the app's label differs from the command's name

| The app says | Command |
|---|---|
| Load Project | `project_open` |
| Import 3MF/STL/STEP/SVG/OBJ/AMF, Import ZIP Archive | `project_import`, `{archive: true}` |
| Add Handy models | `object_add_handy_model` |
| Set as Individual Objects | `object_separate_instances` |
| Scale to Fit Print Volume (Scale to build volume) | `object_scale_to_fit` |
| Auto-Arrange (the toolbar's Arrange), Fill bed with instances or copies | `object_arrange`, `{fill_object, fill_with}` |
| Auto-Orient | `object_auto_orient` |
| Order By | `object_order {by}` |
| the object list's Printable cell | `object_set_printable` |
| Fix Model | `object_fix` |
| Height Range Modifier | `object_set_height_ranges` |
| Variable layer height: Adaptive, Smooth, Reset | `object_set_variable_layer_height {action}` |
| the Add items of an object's context menu | `part_add {type}` |
| Change Type, Precise Seam Type | `part_change_type {type}`, `{subtype}` |
| Replace 3D file | `part_replace` |
| Plate Settings | `plate_get_settings`, `plate_set_settings` |
| Toggle Plate Lock | `plate_lock` |
| Go to Plate | `plate_select {plate: 0}` |
| Slice and Preview, Slice All Plates | `plate_slice {preview: true}`, `{plates: "all", preview: true}` |
| the Preview layer slider's menu: a pause, custom G-code, a template, a filament change | `plate_set_custom_gcode` |
| Import Configs | `preset_import` |
| Export Preset Bundle | `preset_export` |
| Mode: Simple, Advanced, Expert | `settings_set_mode {mode}` |
| the filament list's + and -, Merge with | `filament_add`, `filament_delete`, `{merge_into}` |
| a slot's colour button | `filament_set_colour` |
| Flushing volumes, Purge mode | `filament_set_flushing` |
| Add Mixed Filament, a mixed filament's swatch (Edit Mixed Filament) | `filament_add_mixed`, `filament_edit_mixed` |
| System Filaments dialog | `filament_install`, `filament_uninstall` |
| Printer Selection dialog | `printer_install`, `printer_uninstall` |
| Send G-code to printer host, the Print button's Send and Send all | `printer_send`, `{plates: "all"}` |
| the Print button's Print plate and Print all | `print_start`, `{plates: "all"}` |
| the connection dialog's Test | `printer_test_host` |
| the Device tab's status, Pause, Resume and Stop | `printer_status`, `print_pause`, `print_resume`, `print_stop` |
| Cut, Lay on Face, Brim Ears, Measure, Simplify, Mesh Boolean | `object_cut`, `object_lay_on_face`, `object_set_brim_ears`, `object_measure`, `object_simplify`, `part_boolean` |
| Support Painting, Seam Painting, Color Painting, Paint-on fuzzy skin | `object_paint {type}`: supports, seam, color, fuzzy_skin |
| Install Plugin | `app_open_web_page {page: "plugin_hub"}`, which opens the plugin hub's page in the browser |
| Fit Bed to View, Toggle Perspective, Reset Window Layout, Go to tab, Go to layer | `view_set {camera: "fit_bed"}`, `{projection: "toggle"}`, `{layout: "default"}`, `{tab}`, `{layer_percent}` |
| Open Preset Bundle | `window_open {window: "preset_bundle"}` |
| Take Screenshot | `window_save_screenshot` |
| Run Script | `script_toggle_panel`; `script_run` runs Python from a call |
| Action Replay | `window_open {window: "action_replay"}`; `replay_run` runs a replay file from a call |

## The Design tab's commands

The Design tab is a parametric CAD modeller inside OrcaSlicer, an optional component shown while the CAD
feature is on. The MCP server gives a client commands for it, in the command catalog's group `Design`. A
client that knows a mainstream CAD program can guess the command, the argument that picks its variant and
that argument's values, and can do from arguments what a person does with the tab's tools. The same commands
are what a person's Speed Dial actions run while the tab is shown, what Action Replay and recordings show, and
what the journal records and replays. `tools/list` does not show them: `tool_list`'s description names each
under its heading, `tool_list {group: "Design"}` lists them with what each does, `tool_describe {tool}` gives
one's manual, and `tool_call` runs it.

The commands, and a person's picks of their Speed Dial actions, need the CAD feature on (Preferences >
Features, or `--cad` for one run). The catalog holds them only when the feature is on at startup, so turning it
on takes a restart; with it off they are unknown commands, whose error says how a person turns it on, as
`app_state`'s `cad_note` does. Turned off while OrcaSlicer runs, the tab stays until the restart, but the Speed
Dial has none of its actions.

This section is their command set: how they are named and grouped, what each does and takes, how a call
opens a tool for a person instead of running, and how the commands relate to the right-click menu's items.
How the server holds them (the catalog with the CAD feature on, the gate) is in
[mcp-server.md](mcp-server.md), "Design tab: the Design commands", and the tab itself in
[design-tab.md](design-tab.md).

One table defines them all: `scripts/CAD/tool_atlas.json`. `scripts/CAD/gen_offer_table.py` writes
`src/slic3r/GUI/CAD/DesignOffer.hpp` from it, and the commands' code is in `src/slic3r/GUI/Mcp/Tools/`:
one file per Design heading: `DesignTools.cpp` (the gate, the registration and the Document heading),
`DesignSketchTools.cpp`, `DesignSolidTools.cpp`, `DesignSurfaceTools.cpp`, `DesignConstructTools.cpp`,
`DesignInspectTools.cpp`, `DesignBodyTools.cpp` and `DesignHistoryTools.cpp`.
What every command takes and replies is in its entry's tool block, which the agent reads through
`tool_describe {tool}`; `tests/data/command_catalog.json` holds every command's schema, actions and examples as
an agent sees them.

### Naming

The commands' names, their variant arguments and those arguments' values use the industry's terminology, as
popular CAD programs such as Fusion 360, Onshape and SolidWorks use it, so that people and agents who know
one of them learn the commands quickly. Three nouns are the app's own, as it shows them everywhere:
variable, body and feature.

1. **Every name starts with the place the command works in**, as every command outside the Design tab does
   (`object_`, `plate_`, `project_`):
   - `sketch_`: the sketch.
   - `solid_` and `surface_`: the features that make or change solid bodies and sheet bodies, and the reads that
     serve them, as the SOLID and SURFACE tabs of CAD programs group them: `solid_extrude` beside
     `surface_extrude`, `solid_fillet`, `solid_boolean`, `solid_list_align_types`. The same verbs work in a sketch
     with its own place: `sketch_fillet`, `sketch_mirror`, `sketch_pattern`.
   - `construct_`: reference geometry: planes, axes, coordinate systems and the helix a sweep follows.
   - `body_`: one body as an object: name and colour it, delete it, place it, describe and measure it.
     `body_transform` is here too, since `object_transform` is the Prepare tab's command.
   - `feature_`: one entry of the history: describe, edit, suppress, move, rename, delete.
   - `design_`: the Design tab as a whole: its document (`design_new`, `design_describe`,
     `design_import_step`, `design_commit_to_plate`), its open tool (`design_confirm_tool`), its selection
     and view (`design_select`, `design_zoom_to_selection`), and the reads that are not one body's
     (`design_measure`, `design_interference`).
2. **One command per operation, and one argument for its variants.** `type` says which kind of thing the
   command makes, or which way it makes it: a shape, a pattern's layout, a hole's kind, how a plane is
   defined. A shape drawn more than one way names the way (`rectangle_3_point`).
3. **`operation` says what the result does to the bodies already there**, in four words: `new_body`,
   `join`, `cut`, `intersect`. `solid_extrude`, `solid_revolve`, `solid_sweep`, `solid_loft`, `solid_mirror`
   and `solid_boolean` take it.
4. **Full words**: no abbreviations such as `sk_`, `surf_` or `props`.
5. **A menu item is a way to call a command, not a second command.** A call that gives only the arguments
   choosing an item opens that item's tool for a person; a call that gives more runs it.

**Body, solid and sheet.** A body is what the Bodies list shows: a solid body, which encloses a volume, or a
sheet body, which is open faces. "Solid" and "Surface" in the headings below name sets of commands, not
kinds of object: a command under the Solid heading has no prefix whether it takes a sketch, faces and edges, or
whole bodies.

### The command set

The commands are listed under the headings `tool_list`'s description names them under, in its order:
Document, Sketch, Solid, Surface, Construct, Inspect, Bodies, History and Menu items. Main arguments only;
every argument, its default and its range are in the command's schema.

#### Document

| Command | What it does | Variants and main arguments |
|---|---|---|
| `design_new` | Erase every feature, body and variable, and the Design undo history; committed objects stay on the plate | `discard_changes`, needed when the design is not empty |
| `design_describe` | The document: features, variables with their values, bodies with their placement, undo and redo depth, the Commit button's mode, the selection, the picked plane and the Feature list's selected row |  |
| `design_import_step` | Import a STEP file as bodies | `path` |
| `design_import_mesh` | Convert an STL or OBJ mesh into an editable body, reporting whether it is a solid or an open shell | `path`, `tolerance`, `merge_angle_deg` |
| `design_export_step` | Write every body to a STEP file, at its placement | `path`, `overwrite` |
| `design_commit_to_plate` | Send the visible bodies (a lone body even when hidden) to Prepare at their placement, in one Prepare undo step, and show Prepare | `mode`: assembly, bodies (left out, the Commit button's mode); `replace` |
| `design_set_variable` | Set a design variable to an expression, or remove it | `name`, `expr` |
| `design_select` | Select a body, a face, edges, a vertex, a reference plane, a sketch's loop or a feature by id, as a click selects it, for the menu-item forms to act on | one of: `body`; `body` with `face`, `edges` or `vertex`; `plane`; `sketch` with `loop`; `feature`; nothing, which clears |
| `design_confirm_tool` | Confirm what is open, as its Confirm button and Enter do |  |
| `design_cancel_tool` | Cancel what is open, as its Cancel button does |  |

#### Sketch

**The sketch session**

| Command | What it does | Variants and main arguments |
|---|---|---|
| `sketch_create` | Open a sketch on a plane or a planar face | `plane`: XY, XZ, YZ, `{datum}` or `{body, face}` |
| `sketch_edit` | Re-open a committed sketch to change its entities and constraints | `sketch` |
| `sketch_finish` | Commit the open sketch as a Sketch feature, or replace the re-opened one in place |  |
| `sketch_cancel` | Discard the open sketch; a re-opened one keeps what it had |  |
| `sketch_describe` | A sketch's entities, constraint count and loops; the open one's degrees of freedom, armed tool and selection | `sketch`, left out for the open one |
| `sketch_validate` | The open sketch's closed loops, open ends, defects and degrees of freedom | `tolerance` |
| `sketch_select` | Select sketch entities by index | `entities` |

**Create**

| Command | What it does | Variants and main arguments |
|---|---|---|
| `sketch_draw` | Draw a shape, or a list of shapes | `type`: the 19 shapes below, with the shape's fields; or `entities`, a list of shapes; `construction` |
| `sketch_text` | Text as a Text feature on a plane or a planar face, outlined in a font | `text`, `font`, `bold`, `italic`, `height` (0.5 to 500 mm); `plane`, `position` |
| `sketch_insert_svg` | An SVG file's filled outlines: into the open sketch as lines, or onto the model as art on a plane | `path`, `scale`, `position`; `plane` with no sketch open |
| `sketch_mirror` | A mirrored copy of entities, bound Symmetric to their sources | `axis`: a line of the sketch, `x_axis` or `y_axis`; or `axis_a` and `axis_b`, two free points that bind nothing; `entities` |
| `sketch_pattern` | Copies of entities in a row or around a point, each bound to its source | `type`: linear, circular; `count`; `dx`, `dy` (linear); `angle`, `pivot` (circular); `entities` |
| `sketch_project` | A body's edges: with no sketch open a Project feature, a sketch on a plane that follows the body; with one open, copies in it as construction entities | `source_body`, `face` or `edges`; `plane` with no sketch open |
| `sketch_dimension` | A dimension: a distance, angle, radius or diameter | `type`: distance, distance_x, distance_y, angle, radius, diameter; `entities` or `refs`; `value` |
| `sketch_set_value` | The size of the selected entities, as the value field sets it: a line's length, a circle's diameter, an arc's radius, the angle or distance between two | `value` |

**Modify**

| Command | What it does | Variants and main arguments |
|---|---|---|
| `sketch_fillet` | Round the corner of two lines | `entities`, `radius` |
| `sketch_chamfer` | Bevel the corner of two lines | `entities`, `distance` |
| `sketch_trim` | Trim the entity nearest a point, as a Trim click there does | `at`, `tolerance` |
| `sketch_extend` | Extend the line or arc nearest a point to the next entity | `at`, `tolerance` |
| `sketch_offset` | An offset copy of lines, arcs and circles; one entity's copy is bound Parallel or Concentric to it | `distance`, `entities` |
| `sketch_transform` | Move, rotate or scale entities | `type`: move, rotate, scale; `dx`, `dy`; `angle`; `scale`; `pivot`; `entities` |
| `sketch_blend_curve` | A smooth curve joining the ends of two entities of a committed sketch | `sketch`, `ent_a`, `end_a`, `ent_b`, `end_b` |
| `sketch_toggle_construction` | Switch entities between construction and normal | `entities` |
| `sketch_delete` | Delete entities | `entities` |
| `sketch_repair` | Weld endpoints that nearly meet and constrain them coincident, so loops close | `tolerance`, `ignore_construction` |

**Constraints**

| Command | What it does | Variants and main arguments |
|---|---|---|
| `sketch_constrain` | Add a geometric constraint | `type`: fix, coincident, horizontal, vertical, lock_x, lock_y, equal_length, parallel, perpendicular, concentric, tangent, midpoint, symmetric, point_on_line, point_on_object, equal_radius, collinear, symmetric_about_y, symmetric_about_x, equal; `entities` or `refs`; `value` for lock_x, lock_y and point_on_line |
| `sketch_list_constraints` | A sketch's constraints and dimensions, by index, with their references and values | `sketch`, left out for the open one |
| `sketch_delete_constraint` | Delete one constraint or dimension; those after it move down one index | `index` |

#### Solid

**Create**

| Command | What it does | Variants and main arguments |
|---|---|---|
| `solid_extrude` | Extrude a committed sketch, one of its loops, a face (push/pull) or a profile drawn in the call | `operation`: auto, new_body, join, cut, intersect; `end`: blind, symmetric, two_sided, through_all, up_to_face; `sketch`, `loop`, `face` or `profile`; `distance`, `taper` |
| `solid_revolve` | Revolve a committed sketch or a profile drawn in the call about an axis | `operation`; `sketch`, `axis_entity` or `axis`; `angle`, `flip` |
| `solid_sweep` | Sweep a committed sketch's closed profile along a sketch or a helix; along a helix the profile keeps a fixed angle to the helix axis, as a spring's or a thread's wire does | `operation`; `sketch`, `path` |
| `solid_loft` | Loft through two or more committed sketches, in order | `operation`; `profiles`, `ruled` |
| `solid_rib` | A thin wall grown from a sketch line and fused to a body | `sketch`, `entity`, `thickness`, `depth` |
| `solid_hole` | Drill a hole on a plane or a face | `type`: simple, counterbore, countersink; `diameter` or `designation` (a standard clearance size, M6); `depth`, `through`; `x`, `y` or `at` |
| `solid_thread` | Cut a thread along a cylindrical face or a circular edge, or on a plane | `face` or `edge`; `designation`, `internal`, `diameter`, `pitch`, `length` |
| `solid_thicken` | Thicken a face of a body, or a whole sheet body, into a solid | `type`: face, surface; `body`, `face`, `thickness`, `flip` |
| `solid_pattern` | Copies of a body in a row, around the plane's normal, or along a sketch curve | `type`: linear, circular, curve; `count`; `spacing`, `dir` (linear); `angle` (circular); `sketch`, `entity` (curve) |
| `solid_mirror` | Mirror a body about a plane | `plane`; `operation`: new_body, join; `keep_original` |

**Modify**

| Command | What it does | Variants and main arguments |
|---|---|---|
| `solid_fillet` | Round edges of a body | `edges`: edge ids, or the card's edge group top, bottom, lateral or all; `radius` |
| `solid_chamfer` | Chamfer edges of a body | `edges`, as `solid_fillet` takes them; `distance` |
| `solid_shell` | Hollow a body to a wall thickness, inward | `thickness`; `face` to leave open |
| `solid_draft` | Taper a face by an angle about its base | `face`, `angle` |
| `solid_delete_face` | Remove faces and heal the gap | `body`, `faces` |
| `solid_boolean` | Join two bodies, cut one from the other, or keep their overlap | `operation`: join, cut, intersect, required in a call that runs; `target`, `tool`, `keep_tool` |
| `solid_split` | Split a body with a plane, or along the plane of a face | `plane`, `offset`, `flip`; or `face`; `keep_upper`, `keep_lower` |
| `solid_align` | Move one body so its coordinate system lines up with another's; the two stay separate bodies | `type`: fastened, planar, revolute, slider, cylindrical; `cs_a`, `cs_b`, `offset`, `angle`, `flip` |
| `solid_list_align_types` | Which `solid_align` types fit a pair of coordinate systems, and why each other one does not | `cs_a`, `cs_b` |

#### Surface

| Command | What it does | Variants and main arguments |
|---|---|---|
| `surface_extrude` | Extrude a sketch into an open sheet | `sketch`, `distance` |
| `surface_revolve` | Revolve a sketch into an open sheet | `sketch`, `angle`; `axis_entity` or `axis`; `flip` |
| `surface_loft` | Skin two or more sketches into an open sheet | `profiles`, `ruled` |
| `surface_patch` | Fill a sketch's closed boundary with one face | `sketch` |
| `surface_offset` | Offset a sheet body into another sheet | `body`, `offset` |

#### Construct

| Command | What it does | Variants and main arguments |
|---|---|---|
| `construct_plane` | Add a reference plane | `type`: offset, angle, midplane, tangent, two_edges, coincident; `base`, `offset`, `angle`, `face_a`, `face_b`, `edge_a`, `edge_b` |
| `construct_axis` | Add a reference axis | `type`: two_points, face_normal, cylinder, plane_intersection, along_edge |
| `construct_coordinate_system` | Add a coordinate system, which `solid_align` lines up | `type`: point_world, face_and_direction |
| `construct_helix` | A helical curve, to sweep along | `radius`, `pitch`, `height`, `left_handed`, `taper_deg`, `plane` |

#### Inspect

| Command | What it does | Variants and main arguments |
|---|---|---|
| `design_measure` | Distance, closest points, angle, and the gap between parallel faces, of two faces, edges, axes or points | `a`, `b` |
| `design_interference` | The solid bodies that overlap, with the overlapping volume | `min_volume` |
| `body_describe` | A body's faces and edges, with ids, kinds and geometry, filtered and paged; its vertices on request | `body`; filters by kind, direction, extreme and convexity; `adjacency`; `vertices`; `before`: a feature, for the body as it stood just before it |
| `body_mass_properties` | Volume, surface area, centre of mass, inertia | `body` |
| `body_section` | The cross-section of a body at a base plane through the world origin, as contours | `body`, `plane`, `offset` |
| `body_compare` | Compare a body with another body or a STEP file: volume, bounding box, centroid, surface deviation | `body`, `reference` |

#### Bodies

| Command | What it does | Variants and main arguments |
|---|---|---|
| `body_transform` | Move or rotate a body, as a feature in the history | `dx`, `dy`, `dz`; `axis_x`, `axis_y`, `axis_z`, `angle`, pivot; `copy` |
| `body_set` | Rename, colour, show or hide a body | `name`, `color`, `visible` |
| `body_delete` | Delete a body with the feature that made it | `body` |
| `body_place` | Place a body where Commit to Plate and Export STEP send it, outside the history: move it, turn it, lay a face on the bed, or put it back | `body`; `move` and `rotate`, or `face`, or `reset` |

#### History

| Command | What it does | Variants and main arguments |
|---|---|---|
| `feature_describe` | One feature as its card shows it: parameters, the editable ones with their ranges, expressions, references, what uses it, the bodies it made and the bodies just before it | `feature` |
| `feature_edit` | Change a feature's fields and what it refers to, as its card's Confirm does when editing | `feature`, `fields`, `refs` |
| `feature_set_expression` | Bind a feature's numeric field to an expression over the variables, or unbind it | `feature`, `field`, `expr` |
| `feature_suppress` | Leave a feature out of the rebuild, or bring it back | `feature`, `suppressed` |
| `feature_move` | Move a feature one place up or down the history | `feature`, `delta` |
| `feature_rename` | Rename a feature | `feature`, `name` |
| `feature_delete` | Delete a feature, with what is built on it | `feature` |

#### Menu items

These take no arguments and do what their right-click menu item does.

| Command | What it does |
|---|---|
| `design_rename_selected` | Rename the selected body, else the selected feature, in place |
| `design_zoom_to_selection` | Fit the view to what is picked in the view |
| `sketch_polygon_inscribed` | Set the Polygon tool to fit inside its circle |
| `sketch_polygon_circumscribed` | Set the Polygon tool to fit around its circle |
| `sketch_constrain_selected` | Open Constrain mode on the selected committed sketch |

#### Outside the Design table

`undo` and `redo` with `{history: "design"}` step the Design history. A replay or a script passes `history`, since a
Design command runs with Prepare in front, where `undo` without it would take back a project change. A refusal, such
as nothing to undo, is the call's error and never falls through to the other history. `view_set` with `tab: "design"` takes
the Design view's options: `camera: "sketch_normal"`, which looks straight at the open sketch and is refused
with none open; `section` `{on, z, keep}`, a horizontal cut at world `z` (left out, the model's middle) that
keeps the lower or the upper half; and `origin_planes`, `world_axes`, `origin` and `bed`, which show or hide
the origin helper planes, the world axes, the Origin row's pickable planes and the bed. Those switches are refused
during a rebuild and without `tab: "design"`. With the Design tab, `fit_selection` is refused when nothing is picked,
and `fit_bed`, `canvas` and the Preview fields are refused, since they show Prepare or Preview.

### `sketch_draw`: one `type` for every shape

| `type` | The app's tool | Fields |
|---|---|---|
| `line` | Line | `p0`, `p1` |
| `polyline` | Polyline | `points`, two or more; a loop when the last is the first |
| `rectangle_2_point` | Corner rectangle | `p0`, `p1`: opposite corners |
| `rectangle_center` | Center rectangle | `center`, `corner` |
| `rectangle_3_point` | Oblique rectangle | `p0`, `p1`: one side; `width`, signed, to the left of `p0` to `p1` |
| `rectangle_rounded` | Rounded rectangle | `p0`, `p1`: opposite corners; `radius`, less than half the shorter side |
| `circle_center` | Center circle | `center`, `radius` |
| `circle_2_point` | 2-point circle | `p0`, `p1`: the ends of a diameter |
| `circle_3_point` | 3-point circle | `p0`, `p1`, `p2` |
| `arc_3_point` | 3-point arc | `p0`: start; `p1`: end; `on_arc` |
| `arc_center` | Center-point arc | `center`, `radius`, `start_angle`, `end_angle` |
| `arc_tangent` | Tangent arc | `end`; `entity`, the line or arc it leaves, at that one's end along its direction there: one the sketch has or one an earlier item of the call adds; left out, the entity added last |
| `slot` | Slot | `c0`, `c1`, `width` |
| `slot_arc` | Arc slot | `center`, `start`, `end` (its direction from the centre), `width` |
| `ellipse` | Ellipse | `center`, `radius` (semi-major), `rminor`, `rotation` |
| `ellipse_arc` | Elliptical arc | the ellipse's, `start_angle`, `end_angle` (parametric, counter-clockwise) |
| `spline` | Spline | `ctrl`, two or more poles |
| `polygon` | Triangle to Dodecagon | `center`, `sides` (3 to 1000), `radius` (to a vertex) or `across_flats`, `angle` |
| `point` | Point | `p` |

Points are `[x, y]` in the sketch plane, in mm; angles are radians, or degrees under the `_deg` name
(`start_angle_deg`, `rotation_deg`). A type refuses a field it does not take. The fields are those
`sketch_describe` reads back (`radius`, `rminor`, `rotation`, `ctrl`).

- **One shape or a list.** A call gives `type` with its fields beside it, or `entities`, a list of shapes
  drawn in order, each an object with its own `type` and fields. Beside `entities` a shape's field, `type`
  among them, is refused, and an empty list is refused. `construction` beside `type` is that shape's;
  beside `entities` it is each item's that gives none of its own. A call refused at any item leaves the
  sketch as it was.
- **Built as its tool builds it.** Each shape is turned into the clicks its tool takes and built by the
  tool's own builder (`DesignSketchTool::add_shape_scripted()`), with the tool's grouping: a rectangle, a
  slot or a polygon is one shape, which one undo of the sketch takes off whole, and a polyline is its lines,
  as its tool leaves them, so an undo takes off its last line.
- **Constraints.** Each shape carries the constraints its tool gives it, at any slant: a 3-point rectangle's
  and a square's right angles, a tangent arc's tangency to the line it leaves, a slot's tangencies and equal
  ends, an arc slot's equal ends, a rounded rectangle's level sides, tangencies and equal radii. A
  constraint between a shape and the sketch's axes, or what is already in the sketch, or between plain
  lines, is inferred only where it holds exactly, so that none moves a point the caller gave. A polygon's
  side on an axis is built exactly on it, so a square drawn on the axes has its four sides pinned Horizontal
  and Vertical, as its tool pins them, on every platform.
- A polygon is lines; its side count is not kept once the sketch is committed.
- Reply: `first_index`, `added`, `shapes` (`{first, count}` per item), `entities`, `dof`.

### The arguments

#### The arguments that say which variant

| Command | Argument | Values |
|---|---|---|
| `sketch_draw` | `type` | the 19 shapes above |
| `sketch_transform` | `type` | move, rotate, scale |
| `sketch_pattern` | `type` | linear, circular |
| `sketch_dimension` | `type` | distance, distance_x, distance_y, angle, radius, diameter |
| `sketch_constrain` | `type` | the 20 geometric constraints |
| `solid_hole` | `type` | simple (the default), counterbore, countersink |
| `solid_thicken` | `type` | face, surface; left out, face when `face` is given, else surface |
| `solid_pattern` | `type` | linear (the default), circular, curve |
| `solid_align` | `type` | fastened (the default), planar, revolute, slider, cylindrical |
| `construct_plane` | `type` | offset (the default), angle, midplane, tangent, two_edges, coincident |
| `construct_axis` | `type` | two_points (the default), face_normal, cylinder, plane_intersection, along_edge |
| `construct_coordinate_system` | `type` | point_world (the default), face_and_direction |
| `solid_boolean` | `operation` | join, cut, intersect; no default, so a call that gives other arguments needs it |
| `solid_extrude` | `operation` | auto, new_body, join, cut, intersect; left out, auto with a sketch or a face (join the body the profile touches, else a new body, as the card's default) and new_body without |
| `solid_revolve`, `solid_sweep`, `solid_loft` | `operation` | new_body (the default), join, cut, intersect |
| `solid_mirror` | `operation` | new_body (the default), join |

An argument of one variant is refused with another: a seat size on a plain hole, `spacing` with a circular
pattern, `dx` with a circular sketch pattern, `face` with `solid_thicken {type: "surface"}`.

**A guessed word.** Where a command has no argument of the name a call gives and the value fits, the server
reads `type` as `operation` and `operation` as `type`, so `solid_boolean {type: "cut"}` and `solid_extrude {type:
"cut"}` cut; it reads `index` as `feature`, and `params` and `values` as `fields`. Each schema shows one name.

**A feature's fields.** `feature_describe`, `feature_edit` and `feature_set_expression` use the engine's
field names (`hole_diameter`, `mate_offset`, `cut_offset`), which the project file stores and expressions bind
to, except where a field holds a variant these commands name: the mode of a feature that makes or changes a
body is read and written as `operation` (new_body, join, cut, intersect), and an align's kind, a hole's style
and a pattern's layout as `type` in the commands' words. A reply key that names a feature carries the verb of
the command that makes it, without its place (`align_index`, `align_conflicts`, `fillet_index`). A feature's type
name (`Mate`, `Cut`, `SurfaceFill`, `CoordSys`) is the app's name for it in the history.

**A plane.** The commands that take a plane (`sketch_create`, `solid_extrude`, `solid_revolve`, `solid_hole`,
`solid_thread`, `solid_split`, `solid_pattern`, `solid_mirror`, `construct_helix`, `sketch_project`, `sketch_text`,
`sketch_insert_svg`) take it in one of three forms: `"XY"`, `"XZ"` or `"YZ"`, through the modeling origin;
`{"datum": i}`, a Plane feature; or `{"body": b, "face": f}`, a planar face. A sketch on a face looks along its outward normal; a
hole or a thread drills into the face's body, on the Hole card's plane, its normal into the body and its x along the
face's longest edge. The reply carries the `frame` used. A sketch's base plane passes through the modeling origin, as
every feature's does, so a sketch lands where the features drawn on the same plane land, and the coordinates in a
replay are relative to the modeling origin on any printer.

#### Refused and ignored arguments

- A null argument is an argument not given, as the schemas' null defaults say: `solid_extrude {"sketch": null}`
  extrudes the default rectangle, as `solid_extrude {"silent": true}` does. A few arguments count as given even as
  null, and are refused where they do not apply: `solid_thread`'s `plane` beside `face` or `edge`,
  `sketch_transform`'s `angle`, `angle_deg`, `scale` and `pivot`, and `sketch_pattern`'s `count`, `angle`,
  `angle_deg` and `pivot`, beside a `type` that does not take them.
- Every command has a closed schema, so an argument it does not have is refused as an unknown argument rather
  than ignored: `construct_helix {pich: 3}` is refused, not built with the default pitch. The names agents often use
  instead (`index` for `feature`, `params` or `values` for `fields`) are read as the schema's before it is
  checked.
- **`generation`.** Every command but the menu-item ones takes `generation`, the generation
  (`design_describe`, `body_describe`) the face and edge ids it passes were read at. Ids expire when a feature
  rebuilds the model, and a stale id can still name a real face or edge, so a call whose ids were read before
  the model changed is refused.

#### How a failure shows

A command that fails is an error whose `code` names the cause, so that a caller knows from the error alone
whether to change the call or the design:

- `invalid_argument`: an argument is wrong in itself, names nothing, or names an object whose kind or geometry
  the operation does not take (a face that is not planar, edges square to the sketch plane, a plane that misses
  the body). The caller changes the arguments, or which object they name.
- `precondition`: the arguments are right, but the state of what they name, of the design or of the session is
  not one the operation takes: no body yet, a body hidden or without a shape, a sketch open or none, a datum
  hidden, the feature already first, a tool open, nothing to undo. The caller changes the design or the session
  first.
- `rebuild_failed`: a rebuild reported failure, with the call's change (taken back) or at the earlier state a
  call reads (`body_describe {before}`); its message is the rebuild's reason, a kernel exception included.
- `kernel_failure`: a CAD kernel error; its message starts with `OCCT: `.
- `export_failed`: `design_export_step` could not transfer or write the STEP file.
- `cut_object`: `design_commit_to_plate`'s `replace` names a part of a cut, which Prepare's Delete refuses too;
  refused before the commit.
- `nothing_deleted`: `design_commit_to_plate` sent the design to the plate and then deleted none of its `replace`
  objects; the new objects stay on the plate, and one Prepare undo takes the commit back.
- The gate's (`design_unavailable`, `design_busy`, `stale_ids`) and the menu items' (`wrong_mode`,
  `wrong_selection`), and `no_silent_form` for an entry with no silent form called as one (only by
  `run_design_tool()` called directly).

The line between the first two is between geometry and state. Geometry is what the named objects are and where they
lie, which the call chose, even where a modeling step could make it fit; state is what the Design tab tracks apart
from the geometry (whether an object exists, is shown or has a shape, a feature's place in the list, what is open or
selected), which a call cannot choose. A failed call leaves the design as it was, so `replay_export` leaves it out.
That holds for argument errors and for a rebuild the engine rejects alike. Twenty-one commands instead report a
rejected rebuild in their reply, `ok` false or `error` not empty, after the same rollback, so such a call is journaled
and replays the same rejection: `body_transform`, `solid_boolean`, `construct_axis`, `construct_coordinate_system`,
`solid_delete_face`, `solid_draft`, `feature_set_expression`, `design_import_mesh`, `design_import_step`,
`solid_mirror`, `solid_rib`, `design_set_variable`, `solid_shell`, `sketch_blend_curve`, `sketch_finish`,
`sketch_project` and the five `surface_` commands (`ok_false` in the tool block); their replies that are no error
carry no `"ok": true` and no empty `"error"`, which would say nothing more. Each says so in its description, and
for some `ok: false` means more (`sketch_finish` of a re-opened sketch that was not replaced). `solid_extrude`,
`solid_revolve`, `solid_sweep`, `solid_loft`, `solid_hole`, `solid_thread`, `solid_split`, `solid_fillet`,
`solid_chamfer`, `solid_thicken`, `solid_pattern`, `solid_align` and `sketch_draw` fail as errors, so none of them
fails two ways depending on its arguments.

### When a call opens a tool

One rule holds for every Design command: **a call whose arguments are exactly those that choose a menu item
opens that item's tool for a person, as a pick of the item does; any other call runs from its arguments.**

- No arguments choose the command's own menu item. A command with no menu item of its own is chosen with no
  arguments by the items folded into it with `{}`: Color for `body_set`, and the three value items for
  `sketch_set_value`.
- A menu item may be chosen by arguments: `solid_boolean {operation: "join"}`, `solid_pattern {type: "circular"}`,
  `sketch_draw {type: "slot"}`, `sketch_draw {type: "polygon", sides: 6}`. All of a command's variants open
  the same way.
- A command that a call with no arguments opens has `silent` in its schema. `silent: true` beside the
  choosing arguments, or beside none, runs the command with its defaults. `sketch_draw`, `sketch_transform`
  and `sketch_pattern` have no `silent`: their items are chosen by `type`, and `silent` beside it is refused.
- Arguments are compared after the server normalizes them, as JSON values. `generation`, or anything else
  beside the choosing arguments, makes the call run.
- The tool opens through `open_tool()` on the menu item's entry, only in the mode the item belongs to (a
  sketch item in a sketch, a model item outside one); in the other the call is refused, naming the mode, with a
  `hint` naming the commands that change it: `sketch_create {plane}` and `sketch_edit {sketch}` (with their
  arguments, since `sketch_create {}` opens the plane picker rather than a sketch), or `design_confirm_tool` and
  `design_cancel_tool`, which end a sketch or Constrain mode alike. An
  item the menu greys for the selection is refused for it, naming the selection and the kinds the item
  accepts: its details are `selection_kind`, `selection` and `accepts`, the atlas ids of the selections the item
  takes. Only an item that takes a kind of selection the tab can report is refused so: Boolean's items take two
  bodies and Rib's a sketch line, which the tab never reports as what is selected, so `solid_boolean {}` opens the
  Boolean card as its toolbar button does, whatever is selected. A call made while a row of the Feature list is selected and nothing is picked in the view takes
  that row as what four menu items act on: `feature_edit`, `design_rename_selected`, `feature_delete` and
  `sketch_constrain_selected`.
- For the menu, and so for the commands, a sketch in Constrain mode is open, while the tab's keys there are the
  model's: there a command does what its menu item does, not what its key does.
- The reply is `{"interactive": true, "tool", "applies", "modal", "selection_kind", "busy",
  "status"}`: the command called, whether the menu item accepts the selection, whether a dialog the item's
  handler opened held the call until it was answered, the selection's kind, what the tab has open after the
  handler ran, and its status line. It claims nothing about what the handler did, which may be to open a
  card, arm a sketch tool, act at once, or say on the status line why it cannot.
- A command's description ends with the calls that open a tool, each with its menu item's name, so that the
  name a person knows from the menu, such as "rounded rectangle" or "polar array", is in the command's description.

#### The menu items and the calls they make

| Command | Mode | Its items, and the call each makes |
|---|---|---|
| `sketch_create` | model | Sketch `{}` |
| `solid_extrude` | model | Extrude `{}` |
| `solid_revolve` | model | Revolve `{}` |
| `solid_sweep` | model | Sweep `{}` |
| `solid_loft` | model | Loft `{}` |
| `solid_thicken` | model | Thicken `{"type": "face"}`; Thicken Surface `{"type": "surface"}` |
| `solid_rib` | model | Rib `{}` |
| `solid_boolean` | model | Join `{"operation": "join"}`; Subtract `{"operation": "cut"}`; Intersect `{"operation": "intersect"}` |
| `surface_extrude` | model | Surface Extrude `{}` |
| `surface_revolve` | model | Surface Revolve `{}` |
| `surface_loft` | model | Surface Loft `{}` |
| `surface_patch` | model | Surface Fill `{}` |
| `solid_hole` | model | Hole `{}` |
| `solid_thread` | model | Thread `{}` |
| `solid_shell` | model | Shell `{}` |
| `solid_split` | model | Cut `{}` |
| `solid_fillet` | model | Fillet `{}` |
| `solid_chamfer` | model | Chamfer `{}` |
| `solid_draft` | model | Draft `{}` |
| `surface_offset` | model | Surface Offset `{}` |
| `solid_pattern` | model | Linear pattern `{"type": "linear"}`; Circular pattern `{"type": "circular"}` |
| `solid_mirror` | model | Mirror `{}` |
| `body_transform` | model | Move `{}` |
| `solid_align` | model | Mate `{}` |
| `construct_plane` | model | Plane `{}` |
| `construct_axis` | model | Axis `{}` |
| `construct_coordinate_system` | model | Coordinate system `{}` |
| `construct_helix` | model | Helix `{}` |
| `sketch_project` | model | Project `{}` |
| `body_mass_properties` | model | Volume and area `{}` |
| `design_interference` | model | Interference `{}` |
| `feature_edit` | model | Edit `{}` |
| `design_rename_selected` | model | Rename… `{}` |
| `solid_delete_face` | model | Delete Face `{}` |
| `design_zoom_to_selection` | model | Zoom to selection `{}` |
| `feature_delete` | model | Delete `{}` |
| `body_delete` | model | Delete Body `{}` |
| `body_set` | model | Color `{}` |
| `sketch_constrain_selected` | model | Constrain sketch `{}` |
| `sketch_polygon_inscribed` | sketch | Inscribed `{}` |
| `sketch_polygon_circumscribed` | sketch | Circumscribed `{}` |
| `sketch_text` | sketch | Text `{}` |
| `sketch_insert_svg` | sketch | SVG `{}` |
| `sketch_offset` | sketch | Offset `{}` |
| `sketch_trim` | sketch | Trim `{}` |
| `sketch_fillet` | sketch | Fillet `{}` |
| `sketch_chamfer` | sketch | Chamfer `{}` |
| `sketch_mirror` | sketch | Mirror `{}` |
| `sketch_dimension` | sketch | Dimension `{}` |
| `sketch_constrain` | sketch | Constrain `{}` |
| `sketch_toggle_construction` | sketch | Construction `{}` |
| `sketch_extend` | sketch | Extend `{}` |
| `sketch_delete` | sketch | Delete `{}` |
| `sketch_set_value` | sketch | Length… `{}`; Radius / diameter… `{}`; Angle / distance… `{}` |
| `sketch_transform` | sketch | Move `{"type": "move"}`; Rotate `{"type": "rotate"}`; Scale `{"type": "scale"}` |
| `sketch_pattern` | sketch | Linear array `{"type": "linear"}`; Polar array `{"type": "circular"}` |
| `sketch_draw` | sketch | Line `{"type": "line"}`; Polyline `{"type": "polyline"}`; Corner rectangle `{"type": "rectangle_2_point"}`; Center rectangle `{"type": "rectangle_center"}`; Oblique rectangle `{"type": "rectangle_3_point"}`; Rounded rectangle `{"type": "rectangle_rounded"}`; Center circle `{"type": "circle_center"}`; 2-point circle `{"type": "circle_2_point"}`; 3-point circle `{"type": "circle_3_point"}`; 3-point arc `{"type": "arc_3_point"}`; Tangent arc `{"type": "arc_tangent"}`; Center-point arc `{"type": "arc_center"}`; Slot `{"type": "slot"}`; Arc slot `{"type": "slot_arc"}`; Ellipse `{"type": "ellipse"}`; Elliptical arc `{"type": "ellipse_arc"}`; Spline `{"type": "spline"}`; Triangle `{"type": "polygon", "sides": 3}`; Square `{"type": "polygon", "sides": 4}`; Pentagon `{"type": "polygon", "sides": 5}`; Hexagon `{"type": "polygon", "sides": 6}`; Octagon `{"type": "polygon", "sides": 8}`; Dodecagon `{"type": "polygon", "sides": 12}`; Point `{"type": "point"}` |

`sketch_text {}` opens the Text dialog in a sketch, where its menu item is; the dialog is modeless, so the call
replies at once with a `busy` that names it, and its own Cancel or Escape closes it. Its argument form runs on the
model and is refused while a sketch is open.

### The Design table's entries

`tool_atlas.json` has 127 entries. Each is one of these:

- **A command with a menu item**: the item's slot, key, handler (`action`), accepts, mode, name, icon and
  hint, and the command's tool block, under the command's name. 47 commands.
- **An argument-only command**: a tool block and no handler. 36 commands: 35 have no menu item, and
  `design_measure`'s item, which has no handler, is shown disabled.
- **A menu-only command**: a handler and no tool block. Five, under Menu items. It takes no argument at all,
  `silent` included.
- **A folded item**: a menu item that is no command. It keeps its id, which the menu, the keys, the toolbar,
  the Speed Dial actions and the GUI tests use, and names its command and the arguments that choose it:
  `"fold": {"into": "sketch_draw", "args": {"type": "circle_center"}}`. 37 items, folded into eight commands.
- **A greyed item**: an item with neither, shown disabled. Two: Pattern on Curve (`pat_curve`), whose function
  is `solid_pattern {type: "curve"}`, and Align to (`align_to`), which is not built.

A command that changes Prepare's model also names the operations it reaches (`operations`), as the commands
outside the Design tab do: `design_commit_to_plate` names `delete_objects`, which deletes the objects its
`replace` lists.

Each command and each menu item with a handler has a Speed Dial action.

The right-click menu's rows and their order are ratified (`_ratified` in the atlas): a row's index is its
address, and changing one is a breaking change.

### Actions, titles, the journal and replays

- Every entry with a handler has a Speed Dial action while the Design tab is shown, in its mode. A command's
  own menu item's action is under the command's key (`orca_command:sketch_trim:orca`); a folded item keeps
  its action under its own id (`orca_command:sk_array:orca`) with the item's title and icon.
- A pick of a folded item's action runs its command with the item's arguments, with the MCP server on or
  off. It shows an agent no arguments, and `action_run` takes none for it, or exactly the item's; that is
  what the journal records, so a replay runs it again.
- A call of a command with a menu item's arguments is titled as that item's action wherever a person is
  shown the call: a job notice, a recording's caption, an Action Replay step (`replay_what_text()`).
- A folded item's action that is not shown is named, when a call asks for it, with the call that runs it
  from any tab: `tool_call {"tool": "sketch_pattern", "args": {"type": "linear"}}`.
- `generation` guards ids only within the session that read them, so the journal records a Design call without it,
  and with `silent: true` added where the call without `generation` would open a tool, so that its replay runs rather
  than opens; the replay rebuilds the same ids. A call that opened a tool is recorded as given and opens it again.
- The MCP skill's reference (`tools/mcp/gen_skill_reference.py`) gives a command a row of its own only when
  it has an action of its own, and one row per folded item's action with its arguments.

### When a command may run

A command's tool block has an `access`, which decides when the Design tab lets it run
(`DesignPanel::mcp_busy()`):

- **read** only looks, and runs under anything but a rebuild.
- **sketch** drives the sketch session: it opens, re-opens, finishes or cancels it, or acts on the open
  sketch, and `sketch_project` and `sketch_insert_svg` add to the open sketch when there is one.
- **change** edits the feature list or the variables, or sets what is selected in the model.
- Both sketch and change are refused under an open card, the Text dialog, Constrain mode or a body Move,
  and change under a sketch session too.
- **ui** acts on what the tab has open (`design_confirm_tool`, `design_cancel_tool`), so only a rebuild refuses it.
- The opening of a tool is refused only by a rebuild, as the Design tab's keys and menu are.
- A Thread, Sweep or Loft that takes seconds rebuilds behind a progress dialog titled "Design". Meanwhile `app_state`,
  `widget_list` and `window_screenshot` answer, and every change is refused as made inside the rebuilding call. On
  Windows the dialog is the system's task dialog, which `widget_list` does not list.

### Behaviour a caller relies on

- **Finishing a new sketch.** `sketch_finish` of a new sketch left empty is refused before anything changes,
  and the session stays open, since an exported replay leaves the failed call out and its next sketch call needs
  that session.
- **A re-opened sketch.** `sketch_edit {sketch}` re-opens a committed sketch, as Edit on its Feature list
  row does; the `sketch_` commands then act on it. It is refused while a sketch is open, and for text,
  imported art, a Project sketch and a sketch not drawn from entities. `sketch_finish` replaces it in place,
  in one undo step, rebuilds what is built on it, and replies `replaced: true`. A replace the rebuild
  refuses, or a re-opened sketch left empty, ends the edit with the design as it was and replies `ok: false`,
  `replaced: false`, its `sketch_index` and an `error` that says why, so the call is journaled and its replay
  ends the same edit. A click inside one of the sketch's loops, the Text dialog (`sketch_text {}`) and Constrain
  mode (K) finish the sketch first, and a re-opened one they cannot put back stops them there, with the reason
  on the status line, so nothing opens on the sketch as it was. `sketch_cancel` drops the changes. An extrude of
  one loop keeps its own copy of that loop and does not follow the edit.
- **What `refs` takes.** `feature_edit {refs}` changes what a feature refers to, under the names
  `feature_describe` reports, for what the feature's card can pick again when it edits: a revolve's axis, a
  sweep's path, a loft's profiles, a boolean's two bodies, a mirror's, split's, helix's, hole's or thread's
  plane, a rib's line, an align's coordinate systems, a fillet's or chamfer's edges or edge group, a shell's,
  draft's or thicken's face, the faces of a Delete Face, the picks of a plane, axis or coordinate system,
  and the target body. An extrude's profile and target are fixed. A feature index must name an earlier
  feature of the right type. Body indices and face and edge ids are those of the model just before the
  feature, which `body_describe {body, before: feature}` reads and `feature_describe`'s `bodies_before`
  lists (`{index, name}`, as the last rebuild left it, not saved, and null where that rebuild did not reach the
  feature); a Plane's, an Axis's and a Coordinate system's are the finished model's. `feature_describe`'s
  `ids_of` says which a feature's ids index, `"before"` or `"finished"`; `bodies_before` and
  `body_describe {before}` always give the model just before the feature. `fields` takes a Text feature's words,
  height and font (a face name, with bold and italic beside it, or the stored value `feature_describe` reports),
  which outline it again, and imported art's position and scale. One undo step; a rebuild that fails is rolled back.
- **The selection.** `design_select` selects as a click does, in the view and in the lists, so that a menu item's call
  with no arguments, `design_zoom_to_selection` and `design_rename_selected` have something to act on: a body as a
  click on its Bodies row, a face, edges or a vertex as a click in the view (a vertex's id is its place among the
  body's vertices), a reference plane, a committed sketch's loop, or a feature as a click on its Feature list row; a
  call with nothing clears it. It replies the selection as `design_describe` reports it. A loop is numbered as
  `sketch_describe {sketch}` numbers `closed_loops` and `solid_extrude {sketch, loop}` takes it, and is selected only
  while the view shows it: an extrude of the whole sketch hides its loops and an extrude of one loop hides that loop,
  and a hidden loop is refused, naming the extrude. A plane is picked only where a click can pick one: while the
  Origin row shows the planes, or while Sketch waits for its plane, where picking it opens the sketch. It is refused
  while a card, a sketch, Constrain mode, a body Move or the Text dialog is open, where a click means something else.
- **What is open.** `design_confirm_tool` takes, in this order: the Text dialog, as its OK does (a new text goes
  into the design with its placement then open), then, as Enter does, a value field, a body Move, a text or art
  placement, a feature card, a sketch, Constrain mode. A card adds or edits its feature in one undo step and
  closes; an add the card refuses, whose rebuild fails or that adds nothing leaves no feature and no undo step
  and keeps the card open with the reason on the status line, and the reply says `ok: false`. It is refused
  when nothing is open, while Sketch waits for its plane, while the Text dialog's OK is off, with the dialog's
  size line as the reason, and while Confirm is off, with the status line as the reason. `design_cancel_tool` takes the
  same states, and Sketch waiting for its plane, as the card's or the Text dialog's Cancel does; an open sketch
  is discarded without the dialog a person gets, and a re-opened one is kept as it was. Both reply `was`, what
  was open; `design_confirm_tool` also whether it took (`ok`), the feature added or edited, what is still open and the
  status line.
- **A body's placement.** `body_place` sets where Commit to Plate and Export STEP send a body, as the Bodies
  row's Move and Place on Face do: outside the history, with no undo step, and not kept in the project file.
  `move` and `rotate` add to where the body stands, the turn about its centre; `face` lays that face on the
  bed; `reset` puts it back where the model has it. A placement follows its body when the bodies are
  renumbered. `design_describe` reports each body's `placement`.
- **The bindings the edit commands keep.** `sketch_fillet`, `sketch_chamfer`, `sketch_trim`, `sketch_extend`,
  `sketch_transform` and `sketch_pattern` keep the constraint upkeep a click gets: a fillet arc is bound
  coincident and tangent to its legs, the constraints an edit invalidates are released (a fillet or a chamfer
  lets go of a Symmetric constraint on an end it trims, a trim or an extend the line's), and pattern copies are
  bound to their source. `sketch_mirror {axis}` binds each end of a mirrored line or arc Symmetric to its
  source's opposite end about the axis, as the Mirror tool's Confirm does, unless that would move anything off
  its reflection; `axis_a` and `axis_b`, two free points, bind nothing. `sketch_offset` binds one entity's copy
  Parallel or Concentric to it, and that copy gets no inferred constraints. The mirror's and the offset's
  replies list the constraints added.
- **What the reads and edits report.** A feature a command adds is named as its card names it ("Extrude 3").
  `body_describe` marks a bore's or a countersink's face `internal`. `design_describe` gives the document's
  `auto_close_loops`, and `variables_error`, with every variable null, when one does not evaluate. `feature_edit`
  also replies `plane`, the plane the feature builds on, where it changed, and refuses an end condition that needs a
  pick the feature does not hold.
- **A thread's size.** `solid_thread` without a `designation` takes the standard nearest the cylinder, over ISO metric
  and UTS together: a bore is matched on its minor diameter, a boss on its major, so a 5 mm bore takes 1/4-20 UNC.
- **Committing.** `design_commit_to_plate` refuses, before anything changes, an open gizmo, a design with no body and
  every body hidden. A body named in the Bodies list names its object, or its part of an assembly; the others are
  named "Design Body" or "Design Assembly".

### Where the app's label differs from the command's name

The app's labels follow the right-click menu, which is ratified, and in these places they differ from the
command a call names.

| Where | The app says | Command |
|---|---|---|
| Sketch, Repeat submenu | Linear array, Polar array | `sketch_pattern {type: linear}`, `{type: circular}` |
| Remove submenu | Cut, which divides a body with a plane | `solid_split` |
| Boolean card and menu items | Join, Subtract, Intersect | `solid_boolean {operation: join}`, `{operation: cut}`, `{operation: intersect}` |
| Surface flyout | Surface Fill | `surface_patch` |
| Reference submenu | Volume and area | `body_mass_properties` |
| Rectangle flyout | Corner rectangle, Oblique rectangle | `sketch_draw {type: rectangle_2_point}`, `{type: rectangle_3_point}` |
| Transform submenu | Move in the menu, Transform on the toolbar and the card | `body_transform` |
| Placement flyout | Mate | `solid_align` |
| Feature row | the eye, show or hide | `feature_suppress` |
