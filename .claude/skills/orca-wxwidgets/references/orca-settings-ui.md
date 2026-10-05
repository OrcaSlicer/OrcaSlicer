# OrcaSlicer settings UI: PrintConfig → Tab

How a `ConfigOptionDef` becomes a row in a settings tab, how edits flow back into the config, and everything
a print/filament/printer setting must touch to load, save, show, search, translate, toggle and override.
Read it before adding or changing a setting, writing a `Field` type or custom row widget, adding dependency
rules, or debugging a settings row that does not show, save, search, revert or translate.

Contents: [Rules](#rules) · [Pipeline](#pipeline-at-a-glance) · [ConfigOptionDef](#configoptiondef-the-data-side) ·
[Config classes and preset lists](#config-classes-preset-lists-and-variants) · [Tabs and placement](#tabs-pages-and-where-they-live) ·
[Groups, options, lines](#optionsgroup-option-and-line) · [build_field](#optionsgroupbuild_field) ·
[Field and value flow](#field-and-the-value-flow) · [Control pooling](#field-control-pooling) ·
[Lazy building and OG_CustomCtrl](#lazy-building-and-og_customctrl) · [ConfigManipulation](#configmanipulation-toggles-and-fix-ups) ·
[Search index](#search-index-registration) · [Localization](#localization-of-option-definitions) ·
[Per-object overrides](#per-object-part-layer-and-plate-overrides) · [Checklist](#checklist-adding-a-setting)

## Rules

1. Declare a setting once, in `PrintConfigDef::init_*_params()`, with `label`, `category`, `tooltip`, `sidetext`,
   `mode`, limits and a default; mark every user-visible string with `L()`, never `_L()`. → [ConfigOptionDef](#configoptiondef-the-data-side)
2. Put the member in the `PRINT_CONFIG_CLASS_DEFINE` block of the scope it belongs to; the class decides whether
   the setting can be overridden per object, part or layer range. → [Config classes](#config-classes-preset-lists-and-variants), [Overrides](#per-object-part-layer-and-plate-overrides)
3. List the key in the `Preset` option list of its preset type; without it the key is absent from the tab's
   config and the row cannot be built. → [Config classes](#config-classes-preset-lists-and-variants)
4. A per-variant key is a vector option listed in the matching `*_options_with_variant` set, appended with index
   `0`, and toggled with the variant index. → [Variants](#per-extruder-variant-options)
5. Add the row with `optgroup->append_single_option_line(key, wiki_path)` in `TabX::build()`; widget, tooltip,
   undo/system icons, dirty tracking and search all come from the def. → [Groups](#optionsgroup-option-and-line)
6. Page titles, group titles, `Line` labels and tooltips are English `L()` strings; translation happens at
   display time. → [Localization](#localization-of-option-definitions)
7. Do not rely on `L_CONTEXT` in a def: the display path translates without context. → [Localization](#localization-of-option-definitions)
8. A row built from a custom widget has no `Field`; give its key name branches in `Tab::decorate`,
   `Tab::on_roll_back_value` and `ConfigOptionsGroup::back_to_config_value` (plus `Tab::options_list_storage_key`
   for a vector key). → [Custom widgets](#custom-widgets-on-a-line)
9. In a `Field::BUILD()`, create controls through a `static Builder<T>` per construction style, re-set every
   property, and bind every handler with the control's id. → [Pooling](#field-control-pooling)
10. Bind with an id only events the widget emits with its id; `::ComboBox` sends `wxEVT_COMBOBOX_DROPDOWN/CLOSEUP`
    with id 0. → [Pooling](#field-control-pooling)
11. Fields exist only for the active page: null-check `get_field()`, and use `toggle_line` (not field state) for
    anything that must hold on every page. → [Lazy building](#lazy-building-and-og_customctrl)
12. Dependent enable/hide rules go in `ConfigManipulation::toggle_*_options`; value fix-ups go in
    `ConfigManipulation::update_*_config` through `apply()` under the `is_msg_dlg_already_exist` guard.
    → [ConfigManipulation](#configmanipulation-toggles-and-fix-ups)
13. Set field values from code with `set_value(value, false)`; the `boost::any` must hold the display type that
    `ConfigOptionsGroup::get_config_value` produces for the option type. → [Field](#field-and-the-value-flow)
14. Teach `Print::invalidate_state_by_config_options` / `PrintObject::invalidate_state_by_config_options` which
    steps the key invalidates; an unknown key reslices everything. → [Checklist](#checklist-adding-a-setting)
15. A setting is searchable, offered by the Speed Dial and listed in the unsaved-changes/compare dialogs only if a
    settings tab registers it in a titled group and it has a label. → [Search index](#search-index-registration)
16. Filament and printer tabs live in the modeless `ParamsDialog`; code that must run after editing hooks its
    close path, not the line after `Popup()`. → [Placement](#where-the-tabs-live)
17. Renaming or removing a key needs `PrintConfigDef::handle_legacy`, and a new key's default must reproduce the
    old behaviour for existing profiles and projects. → [Checklist](#checklist-adding-a-setting)

## Pipeline at a glance

```
PrintConfigDef::init_*_params()      def = this->add(key, coX) ...           libslic3r/PrintConfig.cpp
PRINT_CONFIG_CLASS_DEFINE(...)       ((ConfigOptionX, key))                  libslic3r/PrintConfig.hpp
s_Preset_*_options                   key saved/loaded/diffed per preset      libslic3r/Preset.cpp
TabX::build()                        add_options_page → Page::new_optgroup   slic3r/GUI/Tab.cpp
  ConfigOptionsGroup::append_single_option_line(key, wiki, idx)
    get_option(): m_opt_map["key#idx"], settings_index().add_key(...)       slic3r/GUI/OptionsGroup.cpp
    create_single_option_line(): Line{label, formatted tooltip}
    append_line(): index.set_path / set_line_label
page shown → Page::activate → OptionsGroup::activate → activate_line
    → OptionsGroup::build_field → Field::Create<T> → T::BUILD()             slic3r/GUI/Field.cpp
    → OG_CustomCtrl paints labels, sidetext, undo icons                      slic3r/GUI/OG_CustomCtrl.cpp
edit → Field::on_change_field → OptionsGroup::on_change_OG → ConfigOptionsGroup::on_change_OG
    → change_opt_value(config) → group m_on_change (Page::new_optgroup)
    → Tab::update_dirty() + Tab::on_value_change() → TabX::update()
    → ConfigManipulation::update_*_config → toggle_options() → MainFrame::on_config_changed
```

## ConfigOptionDef: the data side

**Contract.** `ConfigOptionDef` (`src/libslic3r/Config.hpp`) is the static description of one key: type,
default, GUI presentation, limits and legacy names. Defs are registered with `ConfigDef::add(key, type)` (or
`add_nullable`) inside `PrintConfigDef::init_common_params` / `init_fff_params` / `init_sla_params`
(`src/libslic3r/PrintConfig.cpp`); the def map owns the default value object.

```cpp
def = this->add("brim_width", coFloat);
def->label    = L("Brim width");          // row label; L() is an extraction marker (no-op)
def->category = L("Support");             // per-object settings grouping (not the tab page)
def->tooltip  = L("This is the distance from the model to the outermost brim line.");
def->sidetext = L("mm");                  // unit
def->min = 0; def->max = 100;             // Field clamps to these
def->mode = comSimple;                    // visibility gate
def->set_default_value(new ConfigOptionFloat(0.));
```

Enums need three parts: the `enum class` plus `CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(Name)` in `PrintConfig.hpp`,
a `static t_config_enum_values s_keys_map_Name` plus `CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(Name)` in
`PrintConfig.cpp`, and in the def `enum_keys_map = &ConfigOptionEnum<Name>::get_enum_values()` with parallel
`enum_values` (serialized keys) and `enum_labels` (`L()` display labels) — see `wall_generator`
(`PerimeterGeneratorType`).

| Field | Meaning for the GUI |
|---|---|
| `type` | `coFloat/coFloats/coInt/coInts/coString/coStrings/coPercent(s)/coFloatOrPercent(s)/coBool(s)/coEnum(s)/coPoint(s)/…`; picks the `Field` when `gui_type` is `undefined`. |
| `gui_type` | `GUIType {undefined, i_enum_open, f_enum_open, color, select_open, slider, legend, one_string, plugin_picker, plugin_config, printer_agent_select}`; checked first by `build_field`. `slider` is marked "currently unused" in `Config.hpp`. |
| `gui_flags` | `"serialized"`: a vector edited as one `;`-separated string; `"show_value"`: show the value even when enum labels exist. |
| `label` / `full_label` | `label` is the short row label (a sub-label inside a multi-option row). `full_label`, when set, names the setting on its own: sidebar search titles, the per-object "Add Settings" menu, the compare dialogs. The Speed Dial titles a setting with the label its row draws and keeps `full_label`/`label` only as a search alias (`ActionRegistry`). |
| `category` | English group name for per-object settings (`SettingsFactory::get_bundle`, the settings menus, the ObjectList settings item) and the transfer view's fallback in `UnsavedChangesDialog`. Empty = left out of the ObjectList settings item and the "Add Settings" menus (the model tabs still show the key). Search uses the tab page title instead. |
| `tooltip`, `sidetext` | Translated at display; `sidetext` is drawn inside most inputs (see [Field](#field-and-the-value-flow)). |
| `min`, `max`, `max_literal` | `Field` clamps to `[min, max]` with "Value is out of range."; `max_literal` bounds the absolute (non-%) value of `coFloatOrPercent` keys whose `sidetext` contains `"mm "`. Both `min`/`max` bounded → "Range:" line in the tooltip. |
| `mode` | `comSimple < comAdvanced < comExpert < comDevelop`; a row shows when its **first** option's mode ≤ the tab's mode. |
| `nullable` | Vector values may hold nil ("N/A"); used by model-scope overrides. |
| `multiline`, `full_width`, `is_code`, `height`, `width`, `readonly` | Text box shape (in em units for `height`/`width`); `is_code` sets `normal_font()` (not a monospace font, despite the `Config.hpp` comment) and adds the "Edit Custom G-code" button when the group has `edit_custom_gcode`; disabled control (`readonly`). |
| `ratio_over` | For `coFloatOrPercent`: the key a percentage refers to. |
| `aliases`, `shortcut` | Legacy names; one value expanding to several keys. |
| `plugin_type` | Makes the option plugin-backed (`is_plugin_backed()`). |

An unadorned `coBool` def becomes a checkbox with no GUI code at all.

Pitfalls:
- **Rule:** Give `coFloatOrPercent` defs a `sidetext` of the form `"mm or %"` / `"mm/s or %"` and a sensible
  `max_literal`.
  **Why:** `Field::get_value_by_opt_type` asks "Is it N% or N mm?" by matching the English `sidetext`: a unitless
  value above `max` when it contains `"mm/s"`, above `max_literal` when it contains `"mm "` (only that form also
  clamps literal values to `max_literal`); another unit text skips the check.
  Cite: `Field::get_value_by_opt_type`.
- **Rule:** Give every key that can be overridden per object a non-empty `category`.
  **Why:** `is_improper_category` (`GUI_Factories.cpp`) drops empty categories (and `"Extruders"`/`"Wipe options"`
  with one filament, `"Support material"` for parts), so the override never appears in the ObjectList settings item
  and does not light the Objects switch (`ParamsPanel::notify_object_config_changed`).
  Cite: `SettingsFactory::get_bundle`.

## Config classes, preset lists and variants

**Static config classes.** Every key that the slicing core reads is a member of a `PRINT_CONFIG_CLASS_DEFINE`
block in `src/libslic3r/PrintConfig.hpp`:

| Class | Scope | Overridable in model tabs |
|---|---|---|
| `PrintObjectConfig` | per object | object (`TabPrintObject`) |
| `PrintRegionConfig` | per region | object, part (`TabPrintPart`), layer range (`TabPrintLayer`) |
| `MachineEnvelopeConfig`, `GCodeConfig`, `PrintConfig` (derives from both) | global | no |
| `SLA*Config` | SLA | — |

`layer_height` is added for layer ranges, and the plate tab offers the fixed `plate_keys` list in `Tab.cpp`.

**Preset option lists.** `src/libslic3r/Preset.cpp` lists which keys each preset type owns: `s_Preset_print_options`,
`s_Preset_filament_options`, `s_Preset_printer_options` (+ `s_Preset_machine_limits_options` and the nozzle-sized
`PrintConfigDef::extruder_option_keys()`, joined in `Preset::printer_options()`). `PresetBundle` builds each
collection's default config from its list (`prints(Preset::TYPE_PRINT, Preset::print_options(), …)`), so the list
decides what is saved, loaded, diffed, inherited and shown in the tab.

### Per-extruder variant options

Multi-extruder printers store some vectors once per extruder variant. A key joins one of the sets in
`src/libslic3r/PrintConfig.cpp`: `print_options_with_variant`, `filament_options_with_variant`,
`printer_options_with_variant_1` (one value per variant) or `printer_options_with_variant_2` (a normal/silent pair
per variant, stride 2). Printer per-extruder keys sized to `nozzle_diameter` go in
`PrintConfigDef::init_extruder_option_keys` (`m_extruder_option_keys`; a retract key also joins
`m_extruder_retract_keys`, which is asserted sorted).

GUI mechanics: the row is appended with index 0 (`append_single_option_line("outer_wall_speed", wiki, 0)`, field id
`outer_wall_speed#0`). `Tab::switch_excluder` rewrites each group's `m_opt_map` index to the selected variant, so
edits write `values[variant]`, and fills `Page::m_opt_id_map` (`"key#<variant>"` → shown field id).
`Tab::get_config_manipulation` passes a variant index to `toggle_option`/`toggle_line`/`set_option_label` as
`index + 256`; `Page::get_field`/`Page::get_line` see `>= 256` and translate through `m_opt_id_map`. The print-side
variant speeds are nullable vectors (`nullable = true`, `ConfigOptionFloatsNullable`) so a model override can set
one variant's element and leave the others nil (`TabPrintModel::on_value_change`); copy the declaration of an
existing key in the same set.

Pitfalls:
- **Rule:** Add the key to the `Preset` list together with the def and the class member.
  **Why:** the tab's config lacks the key, so `ConfigOptionsGroup::get_option` only prints
  `No <key> in ConfigOptionsGroup config.` to stderr and the row's value read (`get_config_value`) dereferences a
  missing option when the page builds [source].
  Cite: `ConfigOptionsGroup::get_option`, `ConfigOptionsGroup::get_config_value`.
- **Rule:** In `ConfigManipulation`, toggle variant keys with the variant index.
  ```cpp
  toggle_field("outer_wall_speed", have_perimeters);                 // Wrong: finds no field (the row's id is key#0)
  toggle_field("outer_wall_speed", have_perimeters, variant_index);  // Right
  ```
  Cite: `ConfigManipulation::toggle_print_fff_options`, `Page::get_field`.

## Tabs, pages and where they live

**Classes.** `Tab : wxPanel` (`src/slic3r/GUI/Tab.hpp`) owns a `PresetCollection* m_presets`, the edited
`DynamicPrintConfig* m_config`, its `Page`s and a `ConfigManipulation`. Concrete tabs: `TabPrint`, `TabFilament`,
`TabPrinter`, and the model-scope `TabPrintModel` → `TabPrintPlate`, `TabPrintObject`, `TabPrintPart`,
`TabPrintLayer`. `MainFrame::create_preset_tabs` creates them and `MainFrame::add_created_tab` calls
`Tab::create_preset_tab()` (top bar + `build()`). `GUI_App::get_tab(type)` returns null until a tab is
`completed()`; `get_plate_tab()`, `get_model_tab(part)`, `get_layer_tab()` reach the model tabs.

**Declaring pages.** `TabX::build()` is declarative:

```cpp
auto page = add_options_page(L("Quality"), "custom-gcode_quality");      // English title, page icon
auto optgroup = page->new_optgroup(L("Layer height"), L"param_layer_height");  // L"..." is a wide literal, not L()
optgroup->append_single_option_line("layer_height", "quality_settings_layer_height");
```

`Page::new_optgroup(title, icon, noncommon_label_width, is_extruder_og)` creates a tab group
(`ConfigOptionsGroup(..., is_tab_opt = true)`, or `ExtruderOptionsGroup`), records the page title and preset type
for search (`set_config_category_and_type`), and installs the callbacks: `m_on_change` →
`Tab::update_dirty()` + `Tab::on_value_change()` (called directly; deferring it re-runs `update()`),
`m_get_initial_config` (selected preset), `m_get_sys_config` / `have_sys_config` (parent system preset). On
`TabPrint` pages (model tabs included) `m_split_multi_line` stacks a multi-option row's fields vertically and `m_option_label_at_right`
makes `OG_CustomCtrl` draw sub-labels to the right of the fields. `TabPrinter` creates its "Motion ability",
"Multimaterial" and `"Extruder N"` pages with `add_options_page(..., is_extruder_pages = true)`, which does not
append them to `m_pages`; the caller inserts each at its position.

### Where the tabs live

- **Process.** `TabPrint` and the model tabs sit on `MainFrame::m_param_panel`, a `ParamsPanel` whose top bar
  (`get_top_panel()`) and body are reparented into the sidebar's scrolled panel in `Sidebar::Sidebar`: the top bar
  above the object list, the body (the tab with its own `TabPresetComboBox`, `Tab::get_combo_box()`) below it.
  Print parameters are therefore in the sidebar. `ParamsPanel::switch_to_global` / `switch_to_object` flip its
  Global/Objects switch (`m_mode_region`).
- **Filament and printer.** `TabFilament` and `TabPrinter` live on `ParamsDialog::panel()`, a second
  `ParamsPanel` inside `ParamsDialog : DPIDialog`, created once with the plater as parent. `ParamsDialog::Popup()`
  applies `UpdateDlgDarkUI`, reparents to the main frame on MSW, centres and `Show()`s it — modeless. A
  `wxWindowDisabler(this)` created in its `wxEVT_SHOW` handler disables every other shown top-level window while it
  is visible and is deleted on hide; the close handler validates (`Tab::validate_filament_temperature_pairs`, may
  veto), hides, queues `EVT_MODIFY_FILAMENT` when a filament was being edited, and calls
  `Sidebar::finish_param_edit()`. It never destroys the dialog, so the panel and its tabs are reused across opens.
  `MainFrame::select_tab(wxPanel*)` given a `ParamsPanel` other than `m_param_panel` opens the dialog.
  Modality mechanics: `references/windows-dialogs.md` §6.
- **Sidebar map.** `Sidebar` (pimpl `Sidebar::priv`, `Plater.cpp`) holds the printer block (`combo_printer`,
  nozzle/bed-type combos, `ExtruderGroup`s, sync buttons), the filament block (`combos_filament`, add/delete/edit,
  flushing-volume button), the `ParamsPanel` top bar, the object-list block (the plate/object/part search bar,
  `ObjectList`, `ObjectLayers`; `ObjectSettings` is created but not laid out under `NEW_OBJECT_SETTING`) and the
  process `ParamsPanel`. There is no separate process combo
  (`Sidebar::priv::combo_print` is never created). It owns the `Search::OptionsSearcher`.
  `Sidebar::update_presets(type)` refreshes the combos after a preset change. Full component map:
  `references/orca-architecture.md`.
- **No quick-settings group.** `Sidebar::og_freq_chng_params()` returns null in Orca (the frequently-changed
  parameters group is compiled out); the process tab itself is the sidebar's settings UI.

Showing the tab of a preset type follows `PlaterPresetComboBox::switch_to_tab`:

```cpp
if (tab->GetParent() == wxGetApp().params_panel())
    wxGetApp().mainframe->select_tab(TAB_ID_PREPARE);   // process: it is in the sidebar
else {
    wxGetApp().params_dialog()->Popup();                // filament/printer
    tab->OnActivate();
}
```

**Contract (wx).** `wxWindowDisabler` disables all top-level windows except the skipped one in its constructor and
re-enables them in its destructor; it affects only windows shown and not already disabled at construction
(`interface/wx/utils.h:59-69`, `:87-110`).

Pitfalls:
- **Rule:** Run post-edit work from the `ParamsDialog` close path (or the tab's value-change path), never after
  `Popup()`.
  ```cpp
  wxGetApp().params_dialog()->Popup(); refresh_after_edit();   // Wrong: Popup() returns at once
  // Right: react in the dialog's close handler / Sidebar::finish_param_edit / EVT_MODIFY_FILAMENT
  ```
  **Why:** the dialog is shown modeless and only emulates modality with `wxWindowDisabler`; the main frame stays
  disabled until it hides.

## OptionsGroup, Option and Line

**`Option`** (`OptionsGroup.hpp`) is a *copy* of the def plus the field id (`opt_id`, `"key"` or `"key#idx"`) and
an optional `side_widget`. **`Line`** holds `label`, `label_tooltip`, `label_path` (wiki path), one or more
`Option`s, and optional widgets: `widget` (replaces the fields), `append_widget` extras, `near_label_widget`, plus
`full_width`, `toggle_visible`, `undo_to_sys`. `Line(label, tooltip)` applies `_()` to both, so pass English `L()`
strings. `Line()` is a separator (`OptionsGroup::append_separator()`).

`ConfigOptionsGroup` binds a group to a `DynamicPrintConfig` (or a `ModelConfig`, then `ModelConfig::touch()` runs
after each change). Its API:

- `get_option(key, idx = -1)` → `Option` with id `key` or `key#idx`; records `m_opt_map[id] = {key, idx}`; for tab
  groups registers the key in the search index (see [Search](#search-index-registration)).
- `append_single_option_line(key, wiki_path = "", idx = -1)` = `get_option` + `create_single_option_line` (label
  `_(label)`, tooltip from `get_formatted_tooltip_text`) + `append_line`.
- `append_single_option_line(const Option&, wiki_path)` appends a modified copy:

```cpp
Option option = optgroup->get_option("small_area_infill_flow_compensation_model");
option.opt.full_width = true; option.opt.is_code = true; option.opt.height = 15;   // changes this row only
optgroup->append_single_option_line(option, "quality_settings_wall_and_surfaces#small-area-flow-compensation");
```

**Multi-option rows** build the `Line` by hand (as the "Overhang speed" and "Bridge" rows in `TabPrint::build` and
"Recommended nozzle temperature" in `TabFilament::build`):

```cpp
Line line = { L("Bridge"), L("Set speed for external and internal bridges") };
line.append_option(optgroup->get_option("bridge_speed", 0));
line.append_option(optgroup->get_option("internal_bridge_speed", 0));
optgroup->append_line(line);
```

The row's mode is its first option's `mode`; each field gets a sub-label from its own `label`.

**Wiki link.** A non-empty `label_path` makes the row label a link: hovering highlights it and a click calls
`OptionsGroup::launch_browser` → `https://www.orcaslicer.com/wiki/<path>` with the path appended verbatim, so write
anchors as the wiki slugs them (`page#lowercase-hyphenated`). `append_line` also records the path for the Speed
Dial's "open wiki" action.

**Groups outside tabs.** A `ConfigOptionsGroup` created without `is_tab_opt` (`PhysicalPrinterDialog`,
`BedShapeDialog`) draws a `LabeledStaticBox` with a `wxFlexGridSizer` of `wxStaticText` labels and plain sizer
layout, no `OG_CustomCtrl`, no search registration. The owner calls `activate()`, adds `optgroup->sizer`, sets
`m_on_change`, and loads values (`reload_config()` / `set_value`).

### Custom widgets on a line

`Tab::create_line_with_widget(optgroup, key, wiki_path, widget)` makes a row whose `widget` (a
`std::function<wxSizer*(wxWindow*)>`) replaces the field — bed shape (`printable_area`), `compatible_printers`,
`compatible_prints`, `filament_ramming_parameters`. It presets white-bullet undo icons and the default label colour.
`Line::full_width` with `widget`/extra widgets builds a description row that `append_line` does not register as
options. `near_label_widget` draws a window before the label (in tab groups `activate_line` creates it as a child of
the `OG_CustomCtrl`, which positions it); the group's `rescale_near_label_widget` / `rescale_extra_column_item`
callbacks rescale them on DPI change.

Pitfalls:
- **Rule:** When a key is edited by a custom widget, add it to the name branches in `Tab::decorate` (the
  `option_without_field` keys), `Tab::on_roll_back_value` (keyed by group title, then `load_key_value` to refresh
  the widget), `ConfigOptionsGroup::back_to_config_value` and, for a vector key stored whole,
  `Tab::options_list_storage_key`.
  **Why:** `decorate` looks the key up with `get_field()` and skips it when there is none, so the row's modified
  colour and undo/lock icons never update; the revert paths have no field to push the restored value into, so the
  widget keeps showing the old value [source].
  Cite: `Tab::decorate`, `Tab::on_roll_back_value` (`printable_area`, `compatible_prints`, `compatible_printers`).

## OptionsGroup::build_field

`OptionsGroup::build_field(id, def)` switches on `gui_type` first, then on `type`:

| `gui_type` | Field |
|---|---|
| `select_open` | `Choice` (read-only) |
| `i_enum_open`, `f_enum_open` | `Choice` (editable: any value, enum entries as presets) |
| `color` | `ColourPicker` |
| `slider` | `SliderCtrl` |
| `legend` | `StaticText` |
| `one_string` | `TextCtrl` (vector edited as one string) |
| `plugin_picker` / `plugin_config` / `printer_agent_select` | `PluginField` / `PluginConfigField` / `PrinterAgentChoice` (Orca) |

| `type` (when `gui_type` is `undefined`) | Field → widget |
|---|---|
| `coFloat(s)`, `coPercent(s)`, `coFloatOrPercent(s)`, `coString(s)` | `TextCtrl` → `::TextInput` (raw `wxTextCtrl` when `multiline`) |
| `coBool(s)` | `CheckBox` → `::CheckBox` |
| `coInt(s)` | `SpinCtrl` → `SpinInput` |
| `coEnum(s)` | `Choice` → `::ComboBox` (`choice_ctrl`) |
| `coPoint(s)` | `PointCtrl` → two `::TextInput` |
| `coNone` | nothing |
| anything else (`coPoint3`, `coIntsGroups`, …) | throws `Slic3r::LogicError("This control doesn't exist till now")` |

It then wires the field: `m_on_change` / `m_on_kill_focus` → the group (ignored while the group is `m_disabled`),
`m_back_to_initial_value` / `m_back_to_sys_value`, the edit button for `is_code` options when the group has
`edit_custom_gcode`, the plugin picker and the preset type of a `PluginConfigField`. Widget event contracts
(`wxEVT_TOGGLEBUTTON` for `::CheckBox`, commit events of `SpinInput`): `references/controls-dataview.md`
§Field widgets, `references/orca-widgets.md`.

**`Choice` specifics** (`Choice::BUILD`): read-only for plain enums, `select_open` and keys with a registered
`DynamicList`, editable (`wxTE_PROCESS_ENTER`) for open enums; entries are `_(enum_labels[i])`, or untranslated `enum_values` when there are
no labels; an entry gets an icon when `resources/images/param_<enum_value>.svg` exists. Lists computed at runtime
(filament pickers) register a `DynamicList` with `Choice::register_dynamic_list(key, list)` (done in
`Sidebar::Sidebar` for `support_filament`, `sparse_infill_filament_id`, …). A tab may narrow the offered entries per
state by rewriting the field's `m_opt.enum_values/enum_labels` and the combo items, as `TabPrint::toggle_options`
does for `support_style`.

Pitfall:
- **Rule:** A new option type or presentation needs a `gui_type` (or a custom-widget line), not a new `type` case
  left unmapped.
  **Why:** an unmapped type throws from `build_field` when the page activates, aborting the page build.

## Field and the value flow

`Field` (`src/slic3r/GUI/Field.hpp`, abstract, `Slic3r::GUI`) keeps a copy of the def (`m_opt`), the id
(`m_opt_id`), the vector index (`m_opt_idx`, parsed from `#idx` in `PostInitialize` for most vector types) and the
current `boost::any m_value`. Virtuals: `BUILD()`, `set_value(any, change_event)`, `get_value()`, `enable()`,
`disable()`, `msw_rescale()`, `sys_color_changed()` (MSW only: `UpdateDarkUI` on the window), `propagate_value()`;
`toggle(en)` enables only when not `readonly`. Subclasses: `TextCtrl`, `CheckBox`, `SpinCtrl`, `Choice`,
`ColourPicker`, `PointCtrl`, `StaticText`, `SliderCtrl`, `PrinterAgentChoice`, `PluginField`,
`PluginConfigField`. `Field::Create<T>(parent, def, id)` constructs, runs `PostInitialize()` (em unit,
`parent_is_custom_ctrl`, `BUILD()`, readonly → `disable()`, Ctrl+1..4 tab shortcuts on the window) and returns a
`std::unique_ptr<Field>` owned by `OptionsGroup::m_fields`. The subclass `CheckBox` shadows the global `::CheckBox`
widget inside `Slic3r::GUI` wherever `Field.hpp` is visible, which is why widget code there writes `::CheckBox`
(and qualifies `::TextInput` / `::ComboBox` the same way) (`references/orca-widgets.md`).

`Field` and `Line` derive from `UndoValueUIManager`: the per-row "revert to system" (lock) and "revert to saved"
(undo arrow) icons, their tooltips and the modified label colour (`#F1754E` by default, `label_clr_modified` in
app config) come for free; `Tab::update_changed_ui` / `Tab::decorate` set them from the option status.

**Value types in the `boost::any`** (`Slic3r::GUI::change_opt_value`, `GUI.cpp`):

These are the config-side values that `Field::get_value()`, `on_change_OG` and `Tab::on_value_change` carry.
`Field::set_value` takes the display form that `ConfigOptionsGroup::get_config_value` produces instead: a
`wxString` for float, percent, float-or-percent and string fields, `bool`/`unsigned char` for checkboxes, `int` for
ints and enums, `Vec2d` for points.

| Option type | `any` holds |
|---|---|
| `coFloat(s)`, `coPercent(s)` | `double` |
| `coFloatOrPercent(s)`, `coString`, single `coStrings` element | `std::string` (`"serialized"` `coStrings`: the whole `;`-joined string; `compatible_printers`/`compatible_prints`: `std::vector<std::string>`) |
| `coInt(s)`, `coEnum(s)` | `int` |
| `coBool` | `bool` |
| `coBools` (incl. nullable) | `unsigned char` (`ConfigOptionBoolsNullable::nil_value()` = nil) |
| `coPoint` / `coPoints` element | `Vec2d`; whole `printable_area`-style lists: `std::vector<Vec2d>` |

A wrong type throws `boost::bad_any_cast` inside `change_opt_value`, which logs "Internal error when changing value
for <key>" and leaves the config unchanged.

**Commit points.** Text fields commit on Enter or kill focus (`propagate_value`), not per keystroke; `TextCtrl`
ignores a kill focus raised while its Enter commit is still running (`EnterPressed` guard, e.g. a dialog the commit
opens). `Choice` commits on `wxEVT_COMBOBOX` (editable: also Enter/kill focus); `CheckBox` on `wxEVT_TOGGLEBUTTON`;
`SpinCtrl` on `wxEVT_SPINCTRL`, Enter and kill focus (skipping the first kill focus after an Enter). Validation
(`Field::get_value_by_opt_type`) clamps to the def's limits and reports through `show_error` (asynchronous).

**Flow after a commit.** `Field::on_change_field` (no-op while `m_disable_change_event`) → `OptionsGroup::on_change_OG`
→ `ConfigOptionsGroup::on_change_OG` (resolves `key#idx` through `m_opt_map`, `change_opt_value` on the group's
config, `ModelConfig::touch()` for model configs) → group `m_on_change` → `Tab::update_dirty()` +
`Tab::on_value_change()` (key-specific branches, then `update()`, `Page::update_visibility`, `Layout()`) →
`TabX::update()` (in `TabPrint::update`: `ConfigManipulation::update_print_fff_config`, then, when the update counter
`m_update_cnt` returns to zero, `toggle_options()`, the ObjectList settings refresh and `MainFrame::on_config_changed`
→ `Plater::on_config_change`).
Revert clicks go `OG_CustomCtrl::OnLeftDown` → `ConfigOptionsGroup::back_to_initial_value` / `back_to_sys_value`.

Pitfalls:
- **Rule:** Update a field from code with `set_value(value, false)`; to push a programmatic value into the config,
  follow it with `field_changed()` (or `propagate_value()`).
  **Why:** `set_value` only brackets the widget update with `m_disable_change_event = !change_event`, so the flag
  decides whether events the setter itself emits (e.g. the `wxEVT_TEXT` of `SliderCtrl`'s text box) reach
  `on_change_field`; where a setter does emit, `true` re-enters `on_value_change` → `update()`. Most Orca widget
  setters emit nothing (`::ComboBox::SetValue`/`SetSelection`, `::CheckBox::SetValue`), so `set_value(v, true)`
  alone usually leaves the config unchanged [source].
  ```cpp
  m_optgroup->set_value("print_host", new_url, true);     // Wrong: the widget shows it, the config is unchanged
  m_optgroup->set_value("print_host", new_url, false);    // Right: show it ...
  m_optgroup->get_field("print_host")->field_changed();   // ... then commit through the group
  ```
  Cite: `PhysicalPrinterDialog::build_printhost_settings`, `Tab::on_value_change` (`set_value` + `propagate_value`).
- **Rule:** Implement `msw_rescale()` in a new `Field` by calling `Field::msw_rescale()` first (refreshes
  `m_em_unit`), then rescaling the widget (`Rescale()`), and size controls in em units. DPI fan-out:
  `references/dpi-bitmaps-fonts.md`.

## Field control pooling

Field controls are recycled, not destroyed. `Builder<T>::build(parent, args...)` (`Field.cpp`) takes a window from
its pool when one exists (`Reparent(parent)`, `Enable()`, `Show()`) and otherwise constructs `T(parent, args...)` and
stores the pool pointer in the window's client data. When a page is cleared (`OptionsGroup::clear`), each field
window goes to `free_window`:

- non-GTK: unbind every dynamic handler whose id is a single explicit id (`m_id != wxID_ANY && m_lastId ==
  wxID_ANY`) on the window and on its `wxTextCtrl` children, hide, clear the containing sizer, reparent to the main
  frame, push back into the pool named by the client data;
- GTK: `delete` the window.

`GUI_App::recreate_GUI` calls `switch_window_pools()` (fresh pools for the new frame) and releases the old pools
when the old frame is destroyed (`release_window_pools()` from a client object on the old frame).

The unbinding walks `wxEvtHandler::GetFirstDynamicEntry/GetNextDynamicEntry`, which wx marks "for internal use
only" (`include/wx/event.h:4027-4033`), and unbinds each entry by its stored functor pointer, which sidesteps
"functors are compared by their address" (`interface/wx/event.h:967-970`) [source]. `Bind`'s `id` defaults to
`wxID_ANY` (`interface/wx/event.h:913-916`); the widgets' own internal handlers are bound that way, which is what
keeps them alive across reuse.

```cpp
// Right: shape of a Field::BUILD
static Builder<::CheckBox> builder;                  // one static builder per construction style
auto temp = builder.build(m_parent);                 // may be a reused window
temp->SetValue(check_value);                         // re-set every property you rely on
temp->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) { on_change_field(); e.Skip(); },
           temp->GetId());                           // explicit id: unbound by free_window
temp->SetToolTip(get_tooltip_text(check_value ? "true" : "false"));
window = temp;
```

Pitfalls:
- **Rule:** Bind every `Field` handler with the control's id.
  **Why:** an id-less `Bind` survives `free_window`; when the pooled control is reused by another field, the old
  lambda still fires with a dangling `this` (the old `Field` is gone) — a use-after-free on MSW/macOS — and one more
  copy of the handler accumulates per reuse. GTK deletes instead, so the bug does not reproduce there.
  ```cpp
  temp->Bind(wxEVT_TOGGLEBUTTON, [this](auto& e) { on_change_field(); });                 // Wrong
  temp->Bind(wxEVT_TOGGLEBUTTON, [this](auto& e) { on_change_field(); }, temp->GetId());  // Right
  ```
  Cite: `free_window`, `CheckBox::BUILD`.
- **Rule:** Bind with the id only events the widget sends with its id.
  **Why:** the id filter must match the event id. `::ComboBox` sends `wxEVT_COMBOBOX` with its id, and `::TextInput`
  re-sends `wxEVT_TEXT_ENTER`/`wxEVT_KILL_FOCUS` with the wrapper's id, but `wxEVT_COMBOBOX_DROPDOWN/CLOSEUP` are
  built as `wxCommandEvent e(type)` (id 0: `interface/wx/event.h:2137`), and an entry bound with one id matches
  only an equal event id (`src/common/event.cpp:1454-1457`) [source], so an id-filtered bind never fires — and an
  unfiltered one is never unbound. `Choice::BUILD`'s own `m_is_dropped` binds are such dead binds. Query
  `ComboBox::is_drop_down()` instead of tracking those events.
  Cite: `ComboBox::ComboBox` (`EVT_DISMISS` lambda), `ComboBox::mouseDown`, `ComboBox::keyDown`,
  `ComboBox::ForceDropdownOpen`, `Choice::BUILD`.
- **Rule:** Keep one `static Builder<T>` per distinct constructor style, and re-apply size, label, value, colours
  and tooltip in `BUILD()`.
  **Why:** a reused window ignores the new constructor arguments (style, size, id, label) — `Choice::BUILD` keeps
  separate builders for editable and `wxCB_READONLY` combos for this reason.
- **Rule:** Do not use `SetClientData` on a pooled control; it holds the pool pointer.

**Contract (wx).** `Reparent` removes the window from its parent and inserts it into another; a notebook page must be
removed from its book first (`interface/wx/window.h:731-742`).

## Lazy building and OG_CustomCtrl

**Lazy fields.** Rows are declared at tab build time; controls are created only when a page activates.
`Tab::activate_selected_page` → `Page::activate` → `Page::activate_group` per group (`OptionsGroup::activate` →
`activate_line` → `build_field`; then `update_visibility`, `reload_config`), followed by `update_changed_ui()`,
`toggle_options()` and `update_visibility()`. Switching pages clears the other pages' controls back to the pool
(`Tab::update_current_page_in_background` → `Page::clear`). The idle prebuild builds the selected settings page one
option group per slice (`Page::build_step`, `Tab::page_build_step`, `ParamsPanel::settings_page_prebuild()`; design
in `docs/HLSD/deferred-page-construction.md`, mechanics in `references/orca-architecture.md`). On GTK the page view is
hidden while a page builds, because GTK crashes when it desensitizes a multi-line text view built on screen and hidden
before its first size allocation (`Tab::activate_selected_page`). Building can be cancelled: `activate(throw_if_canceled)`
throws `UIBuildCanceled`, and the group clears itself.

Consequences: `Tab::get_field` / `Page::get_field` return null for any key not on the built active page;
`Tab::toggle_option` acts only on `m_active_page`; `Tab::toggle_line` and `Tab::set_option_label` write
`Line::toggle_visible` / `Line::label` on **every** page, so they persist and reach the search titles before a page
is ever shown.

**`OG_CustomCtrl`** (`OG_CustomCtrl.hpp/.cpp`, a `wxPanel`) hosts the fields of a tab group (`m_use_custom_ctrl`):
`activate_line` creates it with the first line and fields are parented to it. It paints labels (with the label
colour and blinking search highlight), sub-labels, sidetext of fields that do not combine it, the separator lines and
the undo/lock/edit icons in `OnPaint` (`CtrlLine::render`), positions field windows itself
(`correct_window_position`, `CtrlLine::correct_items_positions`; MSW re-fixes positions in a `CallAfter` after
`Page::activate`), shows/hides fields per line in `CtrlLine::update_visibility` (`toggle_visible && first option mode
<= mode`), and handles clicks (`OnLeftDown`: wiki link, revert to saved, revert to system, edit button).

Pitfall:
- **Rule:** Null-check every `get_field()` and never cache `Field*` across page switches.
  ```cpp
  m_active_page->get_field("support_style")->m_opt;             // Wrong: null when not on this page
  if (auto f = dynamic_cast<Choice*>(m_active_page->get_field("support_style"))) { /* … */ }  // Right
  ```
  **Why:** the field object is destroyed with its page's controls; the window behind it is pooled for another key.

## ConfigManipulation: toggles and fix-ups

`ConfigManipulation` (`ConfigManipulation.hpp/.cpp`) is UI-agnostic dependency logic with two roles:

1. **Value fix-ups** — `update_print_fff_config(config, is_global_config, is_plate_config)` (and the filament/printer
   `check_*` helpers) detect invalid combinations, warn with `MessageDialog`, and write corrections through
   `apply(config, &new_conf)`, which copies the diff and calls the tab's `load_config` callback (`update_dirty()`,
   `reload_config()`, `update()`).
2. **Visibility** — `toggle_print_fff_options(config, variant_index, is_global_config)` calls `toggle_field` (grey
   out, → `Tab::toggle_option` → `Field::toggle`), `toggle_line` (hide the row, → `Tab::toggle_line`) and
   `set_option_label` (rename a row at runtime, e.g. `brim_width` → "Brim ear radius").

`Tab::get_config_manipulation()` builds the callbacks (variant index → `+256`, see [Variants](#per-extruder-variant-options));
`TabX::toggle_options()` calls the `toggle_*` function and adds tab-local tweaks. They run on every page activation,
after every `update()`, and after a variant switch.

```cpp
// fix-up shape (update_print_fff_config)
if (config->opt_float("layer_height") < EPSILON) {
    MessageDialog dialog(m_msg_dlg_parent, _L("Layer height too small\nIt has been reset to 0.2"), "", wxICON_WARNING | wxOK);
    DynamicPrintConfig new_conf = *config;
    is_msg_dlg_already_exist = true;           // ShowModal's loop re-enters update() (field kill focus)
    dialog.ShowModal();
    new_conf.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    apply(config, &new_conf);
    is_msg_dlg_already_exist = false;
}
// toggle shape (toggle_print_fff_options)
bool have_infill = config->option<ConfigOptionPercent>("sparse_infill_density")->value > 0;
toggle_line("infill_combination_max_layer_height", config->opt_bool("infill_combination") && have_infill);
```

Pitfalls:
- **Rule:** Guard every modal fix-up with `is_msg_dlg_already_exist` and write values only through `apply()`.
  **Why:** `ShowModal` runs a nested event loop; focus leaving the edited field commits it again
  (`propagate_value` on kill focus) and re-enters `update()` — the guard exists "to except the duplicate call of
  the update() after dialog->ShowModal()". Without it the dialog repeats; a direct `set_key_value` on the tab config
  skips `load_config`, leaving fields and dirty state stale. Modality rules: `references/windows-dialogs.md`.
- **Rule:** Hide with `toggle_line`, grey out with `toggle_field`; do not call `Show()` on field windows.
  **Why:** `OG_CustomCtrl` re-applies `toggle_visible` and mode on every `update_visibility`, and only `Line` state
  survives page rebuilds and feeds the Speed Dial (`Tab::setting_row_state`).
- **Rule:** Gate rules that only make sense for the global preset on `is_global_config`.
  **Why:** the model tabs run the same `update_print_fff_config` / `toggle_print_fff_options` on an object's config
  with `is_global_config == false` (`m_type < Preset::TYPE_COUNT` in `TabPrint`); see
  `toggle_line("flush_into_objects", !is_global_config)` and the global-only support checks.

## Search index registration

`Sidebar` owns `Search::OptionsSearcher searcher`; its `Search::SettingsIndex` (`SettingsIndex.hpp`) is reached as
`wxGetApp().sidebar().settings_index()`. Registration is automatic for tab rows:

- `ConfigOptionsGroup::get_option` → `settings_index().add_key(opt_id, type, group title, page title, group icon)`
  — **only when `m_use_custom_ctrl`** (tab groups);
- `OptionsGroup::append_line` → `set_path(opt_id, type, label_path)` and `set_line_label(...)` with the label the
  row draws (`Search::compose_display_label`: row label, or "row – sub-label" for multi-option rows).

`SettingsIndex::apply/init` → `append_options` then builds two views from the tab config: `options()` filtered by
mode (sidebar search, `SearchDialog`) and `all_options()` for every mode (the Speed Dial, `ActionRegistry`). An
option enters only if its group and category are non-empty and it has a label (`full_label`, else `label`); vector
keys of print/printer presets get one entry per element (`key#i`), filament variant keys `key#0`. Each entry stores the
English and translated label/group/category, so search matches either. `UnsavedChangesDialog` and `DiffPresetDialog`
name settings from the same index and skip a changed key the index lacks; only the extruder-transfer view
(`UnsavedChangesDialog::update_tree(type, config, from, to)`) falls back to the def's label/category, then "Others".

`Sidebar::jump_to_option(key, type, category)` → (model tab if it has the key, else switch to global) →
`Tab::activate_option`, which selects the page by translated category, focuses the field and blinks it
(`get_custom_ctrl_with_blinking_ptr`). `Tab::apply_searcher()` refreshes the index for one tab.

Pitfalls:
- **Rule:** Create tab groups with a non-empty English title; a key that should be findable must be on a tab.
  **Why:** `append_options` skips entries with an empty group or category, so `page->new_optgroup("")` rows and
  dialog-only keys are invisible to search and the Speed Dial, and their changes are left out of the unsaved-changes
  and compare dialogs.
- **Rule:** Keep `is_tab_opt = false` (the default) for `ConfigOptionsGroup`s outside tabs; the one-argument
  `ConfigOptionsGroup(parent)` constructor sets it to `true`.
  **Why:** a custom-ctrl group registers its keys with the index under its `config_type()`; outside a tab that type
  and category are not set up.

## Localization of option definitions

In `PrintConfig.cpp`, `L(s)` is `(s)` and `L_CONTEXT(s, ctx)` returns `s` (`libslic3r/I18N.hpp`): they only mark the
string for xgettext. The defs live in the static `print_config_def`, built before the GUI installs its translate
callback (`Slic3r::I18N::set_translate_callback`) and reused across language switches, so defs, page titles and group
titles hold English and the GUI translates at display time:

| String | Translated in |
|---|---|
| Row label | `OptionsGroup::create_single_option_line` (`_(label)`) and `Line`'s constructor |
| Sub-labels of multi-option rows | `OG_CustomCtrl` / `activate_line` (`_(label)`; labels exactly `"Top"`/`"Bottom"` in the `"Layers"` context) |
| Tooltip | `get_formatted_tooltip_text` (`Field.cpp`): `_(tooltip)` + "parameter name: `key[idx]`" + for keys in the selected print preset's parent, "Default: value+sidetext" and, when `min`/`max` are both bounded, "Range: [min, max]" |
| Sidetext | `Field::BUILD` (`_L(sidetext)`, drawn inside the input when `m_combine_side_text`) or `OG_CustomCtrl::CtrlLine::render` / `activate_line` (`_(sidetext)`) |
| Enum labels | `Choice::BUILD` (`_(enum_labels[i])`) |
| Page titles | `Tab::translate_category` (`"Extruder N"` composed as `_("Extruder")` + N, or "Left/Right Extruder" on BBL printers) |
| Group titles | `OptionsGroup::activate` (`_(title)`) |
| Category (per object) | ObjectList settings item and menus (`_(category)`) |

Translator workflow and catalog rules: `references/strings-i18n-files.md` and the AGENTS.md localization section.

**Contract (wx).** A message with `msgctxt` is found only when the lookup passes the same context
(`interface/wx/translation.h:594-603`); the catalog keys context entries as `context + '\x04' + msgid`, so a
context-less lookup never sees them (`src/common/translation.cpp:1154-1157`) [source].

Pitfalls:
- **Rule:** Mark def strings with `L()`; never `_L()`/`_()` in `PrintConfig.cpp` or in tab titles.
  ```cpp
  def->label = _L("Brim width");   // Wrong: _L is GUI-only; a def is built once, before any language is set
  def->label = L("Brim width");    // Right: English marker, translated where it is displayed
  ```
  **Why:** a translated tab or group title also breaks the English keys below (`translate_category`, the search
  index); `PrintConfig.cpp`'s `_()` (`Slic3r::I18N::translate`) runs at static init with no callback installed.
- **Rule:** Do not expect `L_CONTEXT` in a def to select a context translation.
  **Why:** the display paths call `_()`/`_L()` without context, so `L_CONTEXT("s", "second")` sidetext shows the
  context-less `"s"` entry and the translator's context entry is never used; only the hard-coded `"Top"`/`"Bottom"`
  `"Layers"` labels are looked up with context. To disambiguate, use a distinct English string or add a context
  lookup in the GUI.
- **Rule:** Keep `category`, page and group titles in English and stable.
  **Why:** they are keys: page titles match tab items through `translate_category`, categories key
  `SettingsFactory::CATEGORY_ICON` (unknown → no icon) and the `*_CATEGORY_SETTINGS` maps, and the index stores the
  English form for search.

## Per-object, part, layer and plate overrides

The model tabs reuse the process tab: `TabPrintModel::build()` runs `TabPrint::build()`, inserts a "Frequent" page
(`layer_height`, `sparse_infill_density`, `wall_loops`, `enable_support`), removes every option not in `m_keys`
(`remove_option_if`), and drops empty groups and pages. `m_keys` is `Preset::print_options()` ∩:

| Tab | Keys |
|---|---|
| `TabPrintObject` | `PrintObjectConfig().keys()` ∪ `PrintRegionConfig().keys()` |
| `TabPrintPart` | `PrintRegionConfig().keys()` |
| `TabPrintLayer` | `layer_height` + `PrintRegionConfig().keys()` |
| `TabPrintPlate` | `plate_keys` (`Tab.cpp`), appended whole, so plate-only keys such as `curr_bed_type` survive the intersection |

So any print setting added to `TabPrint::build()` and to `PrintObjectConfig`/`PrintRegionConfig` is overridable
with no extra GUI code. Selecting an object, part, layer range or plate in `ObjectList` runs
`ObjectSettings::update_settings_list` (`GUI_ObjectSettings.cpp`, `NEW_OBJECT_SETTING` path), which hands the
selected `ModelConfig`s to the tabs with `TabPrintModel::set_model_config`; `TabPrintModel::on_value_change` writes
only the edited key into each `ModelConfig` (nil elements for untouched variants) after a `take_snapshot`. The
`ObjectList` shows an `itSettings` child grouped by `def->category` (`SettingsFactory::get_bundle`), and
`ParamsPanel::notify_object_config_changed` highlights the Objects switch when any object or part has overrides.
ObjectList and its data model: `references/controls-dataview.md`.

`SettingsFactory` (`GUI_Factories.hpp/.cpp`):
- `get_options(is_part)` — `PrintRegionConfig` keys, plus `PrintObjectConfig` keys for objects (SLA: object keys
  minus `layer_height`); feeds the "Add Settings" menus (`MenuFactory::append_menu_item_settings`) and
  `get_bundle`.
- `OBJECT_CATEGORY_SETTINGS` / `PART_CATEGORY_SETTINGS` — curated category → `std::vector<SimpleSettingData>`
  (`{name, label, priority}`, `name` = the option key) lists for
  the Object Table dialog (`ObjectTableDialog`, `ObjectTableSettings` via `get_visible_options` /
  `get_all_visible_options`). They do not decide whether a key can be overridden.
- `CATEGORY_ICON` — category → icon name.

## Checklist: adding a setting

1. **`src/libslic3r/PrintConfig.cpp`** — the `def = this->add("my_option", coX)` block in the right
   `init_*_params()` (label, category, tooltip, sidetext, min/max, mode, default; `L()` strings; enum maps if an enum).
2. **`src/libslic3r/PrintConfig.hpp`** — `((ConfigOptionX, my_option))` in the `PRINT_CONFIG_CLASS_DEFINE` block of
   its scope (`PrintObjectConfig` / `PrintRegionConfig` / `PrintConfig` / `GCodeConfig` / `MachineEnvelopeConfig`).
3. **`src/libslic3r/Preset.cpp`** — append the key to `s_Preset_print_options` / `s_Preset_filament_options` /
   `s_Preset_printer_options` / `s_Preset_machine_limits_options`. Per-variant keys also go into the
   `*_options_with_variant` sets in `PrintConfig.cpp`; nozzle-sized printer keys into
   `PrintConfigDef::init_extruder_option_keys`.
4. **`src/slic3r/GUI/Tab.cpp`** — `optgroup->append_single_option_line("my_option", "wiki_page#anchor")` on the right
   page and group of `TabPrint::build` / `TabFilament::build` / `TabPrinter::build_fff` (index `0` for variant keys).
   That alone yields the widget, tooltip, undo/system decoration, dirty tracking, search and Speed Dial entries, and
   (for object/region keys) the model-tab rows.
5. **Slicing invalidation** — add the key to the right step list in `Print::invalidate_state_by_config_options` or
   `PrintObject::invalidate_state_by_config_options`; a key in neither falls through to `invalidate_all_steps()`
   (correct, but every edit reslices everything).
6. **Optional GUI:** dependencies in `ConfigManipulation::toggle_*_options` / `update_*_config` (+ `TabX::toggle_options`);
   Object Table exposure in `SettingsFactory::OBJECT_CATEGORY_SETTINGS` / `PART_CATEGORY_SETTINGS`; a runtime list
   via `Choice::register_dynamic_list`; enum icons `resources/images/param_<value>.svg`; a custom widget row via
   `Tab::create_line_with_widget` plus the name branches in [Custom widgets](#custom-widgets-on-a-line).
7. **Compatibility** — a renamed or removed key needs `PrintConfigDef::handle_legacy` (old key/value → new;
   `PrintConfigDef::handle_legacy_composite` when the migration needs other keys of the loaded config); the
   default must keep existing profiles and 3MF projects slicing as before; profile edits follow the `orca-profiles`
   skill (vendor `version` bump).
