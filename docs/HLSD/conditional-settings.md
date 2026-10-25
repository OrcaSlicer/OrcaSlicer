# Conditional settings

Conditional settings let a process preset adapt to the plate being sliced without
overwriting its configured base values. For example, a multicolor plate can use
Z-hop and a different wall order while a single-color plate keeps the normal
settings from the same preset.

## Rule model

`conditional_settings` is an empty-by-default process option containing serialized
rules. Each rule has a name, an enabled flag, one or more typed conditions, an
all/any condition mode, and one or more setting/value effects. Effects apply across
the plate. There is no per-filament rule scope.

Conditions read an immutable snapshot of the original resolved configuration and
the printable model context. They do not observe changes made by earlier rules.
Rules run in list order; the last matching effect for a setting wins. A false or
disabled rule leaves the configured base value intact.

The context includes used filament components, printable instance count, model
height, and support/raft enablement. Unprintable instances are excluded. Material
and tool predicates inspect the used filaments: positive comparisons match when
any used filament satisfies them; negative comparisons require that none has the
excluded value. AND/OR combines those plate-wide predicates.

The setting catalog derives from existing process definitions plus supported
printer retraction and machine-limit options. Numeric, percentage, boolean, and
enum effects retain their existing types and ranges. Conditions additionally
support selected string-valued material and preset fields. Script/string effects,
read-only settings, and unsupported nullable options are excluded.

## Evaluation and validation

`Print::apply` works on a copy of the incoming configuration. Process effects run
before the normal configuration normalization so dependent settings follow their
usual resolution path. Printer effects run after normalization so a plate-wide
retraction effect also supersedes explicit filament overrides. Both stages use
the same original snapshot for their predicates. Process effects enter the normal
global configuration; explicit object/modifier settings retain their usual
precedence. Support predicates reflect enablement before slicing, not the
presence of generated support paths.

Every enabled rule and every effect is validated before applying any effects.
Incomplete enabled rules and unsupported schemas block slicing through normal
configuration validation. Disabled incomplete rows can remain in a preset.
The resulting effective configuration also goes through normal print validation.
Runtime match information is transient and is not stored in the process preset.

## Editing and persistence

The editor appears in Process > Others, before the large script fields. Its
setting picker reuses the Process search index and popup. Values use existing
Orca numeric, enum, and checkbox controls. New rows start with blank selections;
choosing a setting samples its current resolved profile/project/plate value.
Changing a profile later does not rewrite an explicitly saved rule value.

Rules are stored through the existing preset and 3MF configuration machinery.
The editor writes version 4 JSON rows, with names and multiple effects. Compact
legacy rules can retain their version 1 representation.
The reader also accepts version 1 tab-separated rows and version 2/3 prototype
rows from existing custom projects. Older scope flags are accepted, but their
effects are plate-wide. Projects and presets without rules retain normal behavior.
