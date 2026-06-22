// Phase 5.3.6 — JSON Schema helpers for plugin settings.
//
// Plugin manifests ship a `settings.schema.json` whose object form is
// rendered into the Plugin Manager's per-plugin settings panel + the
// Tab::add_options_page settings page. The host already drives the
// orca_ui_builder_t imperative API; this file describes a declarative
// alternative authors can keep alongside the imperative entry.

export type FieldType = 'string' | 'int' | 'float' | 'bool' | 'enum' | 'group';

export interface BaseField {
  key:   string;
  type:  FieldType;
  label: string;
  default?: unknown;
  description?: string;
}

export interface StringField extends BaseField {
  type:   'string';
  default?: string;
}

export interface IntField extends BaseField {
  type:   'int';
  default?: number;
  min?:   number;
  max?:   number;
}

export interface FloatField extends BaseField {
  type:   'float';
  default?: number;
  min?:   number;
  max?:   number;
  step?:  number;
}

export interface BoolField extends BaseField {
  type:   'bool';
  default?: boolean;
}

export interface EnumField extends BaseField {
  type:    'enum';
  options: string[];
  default?: string;
}

export interface GroupField extends BaseField {
  type:     'group';
  children: SettingsField[];
}

export type SettingsField =
  | StringField
  | IntField
  | FloatField
  | BoolField
  | EnumField
  | GroupField;

export interface SettingsSchema {
  $schema?: string;
  title:    string;
  fields:   SettingsField[];
}

/**
 * Type-narrowing helper for fields whose tag is known at the call site.
 */
export function isFieldType<T extends FieldType>(
  f: SettingsField, t: T,
): f is Extract<SettingsField, { type: T }> {
  return f.type === t;
}
