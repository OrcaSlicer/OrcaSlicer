#!/usr/bin/env python3
"""
OrcaSlicer Config Code Generator

Reads compiled protobuf descriptor set and generates C++ source files
that replace hand-written config registration, preset lists, and invalidation chains.

Usage:
    # Step 1: Compile .proto files to a descriptor set
    protoc --proto_path=src/PrintConfigs --descriptor_set_out=config.desc \
           --include_imports src/PrintConfigs/*.proto

    # Step 2: Run codegen
    python tools/config_codegen.py config.desc src/slic3r/GUI/generated/

    (tools/run_codegen.py does both, and resolves protoc for you)

Outputs:
    - PrintConfigDef_generated.cpp  (init_fff_params body)
    - Preset_options_generated.cpp  (s_Preset_*_options arrays)
    - Invalidation_generated.cpp    (opt_key -> steps map)
    - OptionKeys_generated.cpp      (extruder/filament key lists)
"""

import sys
import os
import re
import argparse
from pathlib import Path

# Add tools/ to path so we can import the sibling helper modules
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    from google.protobuf import descriptor_pb2
    from config_metadata import load_descriptor_set
except ImportError as e:
    print(f"ERROR: {e}")
    print("Ensure protobuf is installed: pip install protobuf")
    print("Or just run the pipeline, which bootstraps it: python tools/run_codegen.py")
    sys.exit(1)

# The orca.* option extensions and enum constants, bound by main() from the
# descriptor set (see config_metadata.py for why they aren't imported).
meta_pb2 = None


# Proto FieldDescriptorProto.Type enum values
TYPE_DOUBLE   = 1
TYPE_FLOAT    = 2
TYPE_INT64    = 3
TYPE_UINT64   = 4
TYPE_INT32    = 5
TYPE_FIXED64  = 6
TYPE_FIXED32  = 7
TYPE_BOOL     = 8
TYPE_STRING   = 9
TYPE_MESSAGE  = 11
TYPE_UINT32   = 13
TYPE_ENUM     = 14
TYPE_SINT32   = 17
TYPE_SINT64   = 18

# Proto label
LABEL_OPTIONAL = 1
LABEL_REQUIRED = 2
LABEL_REPEATED = 3


def _enum_opt_name(opts, ext):
    """Return the value *name* of an enum-typed field extension, or None if
    unset / the 0 sentinel. The proto enum constrains valid values at protoc
    time, while the codegen keeps using the name string (which matches the
    coXXX / ConfigOptionDef::GUIType C++ identifier)."""
    if not opts.HasExtension(ext):
        return None
    val = opts.Extensions[ext]
    if val == 0:
        return None
    return ext.enum_type.values_by_number[val].name


def mode_to_cpp(mode_val):
    """Convert mode enum value to C++ constant."""
    return {
        meta_pb2.MODE_SIMPLE:   "comSimple",
        meta_pb2.MODE_ADVANCED: "comAdvanced",
        meta_pb2.MODE_DEVELOP:  "comDevelop",
        meta_pb2.MODE_EXPERT:   "comExpert",
    }.get(mode_val, "comAdvanced")


_PRINT_STEPS = None   # set after meta_pb2 import resolves enum values
_OBJECT_STEPS = None


def _init_step_sets():
    global _PRINT_STEPS, _OBJECT_STEPS
    if _PRINT_STEPS is None:
        _PRINT_STEPS = {meta_pb2.STEP_GCODE_EXPORT, meta_pb2.STEP_SKIRT_BRIM, meta_pb2.STEP_WIPE_TOWER}
        _OBJECT_STEPS = {meta_pb2.STEP_SLICE, meta_pb2.STEP_PERIMETERS, meta_pb2.STEP_INFILL, meta_pb2.STEP_SUPPORT}


def step_to_cpp(step_val):
    """Convert invalidation step to C++ constant."""
    return {
        meta_pb2.STEP_GCODE_EXPORT: "psGCodeExport",
        meta_pb2.STEP_SKIRT_BRIM:   "psSkirtBrim",
        meta_pb2.STEP_WIPE_TOWER:   "psWipeTower",
        meta_pb2.STEP_SLICE:        "posSlice",
        meta_pb2.STEP_PERIMETERS:   "posPerimeters",
        meta_pb2.STEP_INFILL:       "posInfill",
        meta_pb2.STEP_SUPPORT:      "posSupportMaterial",
        meta_pb2.STEP_NONE:         "",
    }.get(step_val, "")


def proto_type_to_co_type(field_desc, is_nullable=False):
    """
    Map a protobuf field descriptor to OrcaSlicer's coXXX type constant
    and ConfigOptionXXX class name.

    Returns: (co_type_str, config_option_class, is_vector)
    """
    ftype = field_desc.type
    is_repeated = (field_desc.label == LABEL_REPEATED)
    type_name = field_desc.type_name  # For message types

    # Handle message types (FloatOrPercent, Point2D)
    if ftype == TYPE_MESSAGE:
        if "FloatOrPercent" in type_name:
            if is_repeated:
                cls = "ConfigOptionFloatsOrPercentsNullable" if is_nullable else "ConfigOptionFloatsOrPercents"
                return ("coFloatsOrPercents", cls, True)
            return ("coFloatOrPercent", "ConfigOptionFloatOrPercent", False)
        elif "Point2D" in type_name:
            if is_repeated:
                return ("coPoints", "ConfigOptionPoints", True)
            return ("coPoint", "ConfigOptionPoint", False)

    # Handle enum types
    if ftype == TYPE_ENUM:
        if is_repeated:
            return ("coEnums", "ConfigOptionEnumsGeneric", True)
        return ("coEnum", "ConfigOptionEnum", False)

    # Scalar/vector types
    if ftype in (TYPE_FLOAT, TYPE_DOUBLE):
        if is_repeated:
            if is_nullable:
                return ("coFloats", "ConfigOptionFloatsNullable", True)
            return ("coFloats", "ConfigOptionFloats", True)
        return ("coFloat", "ConfigOptionFloat", False)

    if ftype in (TYPE_INT32, TYPE_INT64, TYPE_SINT32, TYPE_SINT64,
                 TYPE_UINT32, TYPE_UINT64, TYPE_FIXED32, TYPE_FIXED64):
        if is_repeated:
            if is_nullable:
                return ("coInts", "ConfigOptionIntsNullable", True)
            return ("coInts", "ConfigOptionInts", True)
        return ("coInt", "ConfigOptionInt", False)

    if ftype == TYPE_BOOL:
        if is_repeated:
            if is_nullable:
                return ("coBools", "ConfigOptionBoolsNullable", True)
            return ("coBools", "ConfigOptionBools", True)
        return ("coBool", "ConfigOptionBool", False)

    if ftype == TYPE_STRING:
        if is_repeated:
            return ("coStrings", "ConfigOptionStrings", True)
        return ("coString", "ConfigOptionString", False)

    return ("coNone", "ConfigOption", False)


def parse_field_options(field_desc_proto):
    """
    Re-parse FieldOptions from a FieldDescriptorProto with extensions registered.
    This is needed because the FileDescriptorSet parser doesn't know about our
    custom extensions, so they end up as unknown fields. Re-parsing with the
    extensions registered (see config_metadata.load_descriptor_set) resolves them.
    """
    opts = field_desc_proto.options
    if not opts.ByteSize():
        return descriptor_pb2.FieldOptions()

    # Re-parse the serialized options with extensions registered
    reparsed = descriptor_pb2.FieldOptions()
    reparsed.ParseFromString(opts.SerializeToString())
    return reparsed


class FieldInfo:
    """Parsed information about a single config field from proto descriptor."""

    def __init__(self, field_desc):
        self.name = field_desc.name
        self.field_desc = field_desc

        # Re-parse options with extensions registered
        opts = parse_field_options(field_desc)

        # Read extensions using the proper protobuf API
        self.label = opts.Extensions[meta_pb2.label] or None
        self.full_label = opts.Extensions[meta_pb2.full_label] or None
        self.tooltip = opts.Extensions[meta_pb2.tooltip] or None
        self.category = opts.Extensions[meta_pb2.category] or None
        self.sidetext = opts.Extensions[meta_pb2.sidetext] or None
        self.min_value = opts.Extensions[meta_pb2.min_value] if opts.HasExtension(meta_pb2.min_value) else None
        self.max_value = opts.Extensions[meta_pb2.max_value] if opts.HasExtension(meta_pb2.max_value) else None
        self.max_literal = opts.Extensions[meta_pb2.max_literal] if opts.HasExtension(meta_pb2.max_literal) else None
        self.mode = opts.Extensions[meta_pb2.mode]  # 0 = MODE_SIMPLE (default)
        self.has_mode = opts.HasExtension(meta_pb2.mode)
        self.ratio_over = opts.Extensions[meta_pb2.ratio_over] or None
        self.multiline = opts.Extensions[meta_pb2.multiline]
        self.full_width = opts.Extensions[meta_pb2.full_width]
        self.height = opts.Extensions[meta_pb2.height] or None
        self.is_nullable = opts.Extensions[meta_pb2.is_nullable]
        self.gui_type = _enum_opt_name(opts, meta_pb2.gui_type)
        self.gui_flags = opts.Extensions[meta_pb2.gui_flags] or None
        self.enum_keys_map = opts.Extensions[meta_pb2.enum_keys_map_ref] or None
        self.no_cli = opts.Extensions[meta_pb2.no_cli]
        self.readonly = opts.Extensions[meta_pb2.readonly]
        self.preset = opts.Extensions[meta_pb2.preset]  # 0 = PRESET_PRINT
        self.invalidates = list(opts.Extensions[meta_pb2.invalidates])
        self.list_membership = list(opts.Extensions[meta_pb2.list_membership])
        self.legacy_name = opts.Extensions[meta_pb2.legacy_name] or None

        # Default value and enum metadata
        self.has_default = opts.Extensions[meta_pb2.has_default]
        self.default_value = opts.Extensions[meta_pb2.default_value] if self.has_default else None
        self.enum_value_entries = list(opts.Extensions[meta_pb2.enum_value_entries])
        self.enum_label_entries = list(opts.Extensions[meta_pb2.enum_label_entries])
        self.co_type_hint = _enum_opt_name(opts, meta_pb2.co_type_hint)

        # Resolve C++ type info - co_type_hint overrides auto-detection
        co_type, option_class, is_vec = proto_type_to_co_type(
            field_desc, self.is_nullable)
        if self.co_type_hint:
            co_type = self.co_type_hint
            # Fix up option_class for hint-overridden types
            hint_class_map = {
                "coPercent": "ConfigOptionPercent",
                "coPercents": "ConfigOptionPercents",
                "coEnum": "ConfigOptionEnum",
                "coEnums": "ConfigOptionEnumsGeneric",
            }
            if self.co_type_hint in hint_class_map:
                option_class = hint_class_map[self.co_type_hint]
        self.co_type = co_type
        self.option_class = option_class
        self.is_vector = is_vec


class CodeGenerator:
    """Generates C++ source files from parsed proto descriptors."""

    def __init__(self, descriptor_set):
        self.descriptor_set = descriptor_set
        self.fields = []  # All FieldInfo objects
        self.virtual_keys_by_preset = {  # virtual_preset_keys per preset type
            meta_pb2.PRESET_PRINT: [],
            meta_pb2.PRESET_FILAMENT: [],
            meta_pb2.PRESET_PRINTER: [],
        }
        self._parse_all_fields()

    @staticmethod
    def _preset_type_from_filename(name: str) -> int:
        """Infer preset type from proto filename (printer/filament/print)."""
        n = name.lower()
        if "printer" in n:
            return meta_pb2.PRESET_PRINTER
        if "filament" in n:
            return meta_pb2.PRESET_FILAMENT
        return meta_pb2.PRESET_PRINT

    def _parse_all_fields(self):
        """Parse all message fields from all proto files in the descriptor set."""
        for file_desc in self.descriptor_set.file:
            # Skip google/protobuf imports
            if file_desc.name.startswith("google/"):
                continue
            # Skip config_metadata.proto (it's just extensions, no settings)
            if "config_metadata" in file_desc.name:
                continue

            preset_type = self._preset_type_from_filename(file_desc.name)

            for msg_desc in file_desc.message_type:
                # Skip wrapper messages (FloatOrPercent, Point2D)
                if msg_desc.name in ("FloatOrPercent", "Point2D"):
                    continue

                # Collect message-level virtual_preset_keys
                vkeys = list(msg_desc.options.Extensions[meta_pb2.virtual_preset_keys])
                self.virtual_keys_by_preset[preset_type].extend(vkeys)

                for field_desc in msg_desc.field:
                    self.fields.append(FieldInfo(field_desc))

    def generate_init_fff_params(self) -> str:
        """
        Generate the body of PrintConfigDef::init_fff_params().
        Output: C++ code that's a drop-in replacement for the hand-written registrations.
        """
        lines = []
        lines.append("// ===== AUTO-GENERATED by tools/config_codegen.py =====")
        lines.append("// DO NOT EDIT MANUALLY. Edit .proto files and re-run codegen.")
        lines.append("")

        for field in self.fields:
            lines.append(f'    def = this->add("{field.name}", {field.co_type});')

            if field.label:
                lines.append(f'    def->label = L("{self._escape_cpp(field.label)}");')

            if field.full_label:
                lines.append(f'    def->full_label = L("{self._escape_cpp(field.full_label)}");')

            if field.category:
                lines.append(f'    def->category = L("{self._escape_cpp(field.category)}");')

            if field.tooltip:
                tooltip_escaped = self._escape_cpp(field.tooltip)
                # Split long tooltips across lines
                if len(tooltip_escaped) > 80:
                    lines.append(f'    def->tooltip = L("{tooltip_escaped}");')
                else:
                    lines.append(f'    def->tooltip = L("{tooltip_escaped}");')

            if field.sidetext:
                lines.append(f'    def->sidetext = L("{self._escape_cpp(field.sidetext)}");')

            if field.min_value is not None:
                lines.append(f'    def->min = {self._format_number(field.min_value)};')

            if field.max_value is not None:
                lines.append(f'    def->max = {self._format_number(field.max_value)};')

            if field.max_literal is not None:
                lines.append(f'    def->max_literal = {self._format_number(field.max_literal)};')

            if field.ratio_over:
                lines.append(f'    def->ratio_over = "{field.ratio_over}";')

            if field.has_mode:
                lines.append(f'    def->mode = {mode_to_cpp(field.mode)};')

            if field.is_nullable:
                lines.append(f'    def->nullable = true;')

            if field.readonly:
                lines.append(f'    def->readonly = true;')

            if field.multiline:
                lines.append(f'    def->multiline = true;')

            if field.full_width:
                lines.append(f'    def->full_width = true;')

            if field.height:
                lines.append(f'    def->height = {field.height};')

            if field.gui_type:
                lines.append(f'    def->gui_type = ConfigOptionDef::GUIType::{field.gui_type};')

            if field.gui_flags:
                lines.append(f'    def->gui_flags = "{field.gui_flags}";')

            if field.no_cli:
                lines.append(f'    def->cli = ConfigOptionDef::nocli;')

            if field.enum_keys_map:
                lines.append(f'    def->enum_keys_map = &{field.enum_keys_map};')

            # Enum values/labels
            for ev in field.enum_value_entries:
                lines.append(f'    def->enum_values.push_back("{self._escape_cpp(ev)}");')
            for el in field.enum_label_entries:
                lines.append(f'    def->enum_labels.push_back(L("{self._escape_cpp(el)}"));')

            # Default value - reconstruct full C++ from co_type + default_value
            if field.has_default:
                cpp_expr = self._reconstruct_default_cpp(
                    field.default_value or "", field.co_type, field.enum_keys_map,
                    field.is_nullable)
                lines.append(f'    def->set_default_value({cpp_expr});')

            lines.append("")

        return "\n".join(lines)

    def generate_preset_options(self) -> str:
        """Generate s_Preset_print_options, s_Preset_filament_options, etc."""
        lines = []
        lines.append("// ===== AUTO-GENERATED by tools/config_codegen.py =====")
        lines.append("")

        for var_name, preset_type in [
            ("s_Preset_print_options",    meta_pb2.PRESET_PRINT),
            ("s_Preset_filament_options", meta_pb2.PRESET_FILAMENT),
            ("s_Preset_printer_options",  meta_pb2.PRESET_PRINTER),
        ]:
            # Field-derived keys + message-level virtual keys, deduplicated and sorted
            field_names = [f.name for f in self.fields if f.preset == preset_type]
            virtual_names = self.virtual_keys_by_preset[preset_type]
            all_names = sorted(set(field_names) | set(virtual_names))

            lines.append(f"static const std::vector<std::string> {var_name} = {{")
            for name in all_names:
                lines.append(f'    "{name}",')
            lines.append("};")
            lines.append("")

        return "\n".join(lines)

    def generate_invalidation_map(self) -> str:
        """Generate opt_key -> invalidation steps mapping, split by PrintStep vs PrintObjectStep."""
        _init_step_sets()
        lines = []
        lines.append("// ===== AUTO-GENERATED by tools/config_codegen.py =====")
        lines.append("")

        lines.append("static const std::unordered_map<std::string, std::vector<PrintStep>> "
                     "s_print_steps_map = {")
        for field in sorted(self.fields, key=lambda x: x.name):
            if field.invalidates:
                steps = [step_to_cpp(s) for s in field.invalidates
                         if s in _PRINT_STEPS and step_to_cpp(s)]
                if steps:
                    lines.append(f'    {{"{field.name}", {{{", ".join(steps)}}}}},')
        lines.append("};")
        lines.append("")

        lines.append("static const std::unordered_map<std::string, std::vector<PrintObjectStep>> "
                     "s_object_steps_map = {")
        for field in sorted(self.fields, key=lambda x: x.name):
            if field.invalidates:
                steps = [step_to_cpp(s) for s in field.invalidates
                         if s in _OBJECT_STEPS and step_to_cpp(s)]
                if steps:
                    lines.append(f'    {{"{field.name}", {{{", ".join(steps)}}}}},')
        lines.append("};")

        return "\n".join(lines)

    def generate_option_key_lists(self) -> str:
        """Generate extruder_option_keys, filament_option_keys, etc."""
        lines = []
        lines.append("// ===== AUTO-GENERATED by tools/config_codegen.py =====")
        lines.append("")

        extruder_keys = [f for f in self.fields
                         if meta_pb2.LIST_EXTRUDER_OPTION_KEYS in f.list_membership]
        filament_keys = [f for f in self.fields
                         if meta_pb2.LIST_FILAMENT_OPTION_KEYS in f.list_membership]

        for var_name, keys in [
            ("s_extruder_option_keys", extruder_keys),
            ("s_filament_option_keys", filament_keys),
        ]:
            lines.append(f"static const std::vector<std::string> {var_name} = {{")
            for f in sorted(keys, key=lambda x: x.name):
                lines.append(f'    "{f.name}",')
            lines.append("};")
            lines.append("")

        return "\n".join(lines)

    @staticmethod
    def _reconstruct_default_cpp(default_value, co_type, enum_keys_map=None, is_nullable=False):
        """Reconstruct full C++ default expression from co_type + extracted value args.

        Maps (co_type, args) -> 'new ConfigOptionXxx(args)' or 'new ConfigOptionXxx{args}'.
        """
        import re as _re

        # Type -> C++ class mappings
        SCALAR_CLASS = {
            "coFloat":            "ConfigOptionFloat",
            "coBool":             "ConfigOptionBool",
            "coInt":              "ConfigOptionInt",
            "coString":           "ConfigOptionString",
            "coPercent":          "ConfigOptionPercent",
            "coFloatOrPercent":   "ConfigOptionFloatOrPercent",
            "coPoint":            "ConfigOptionPoint",
            "coPoint3":           "ConfigOptionPoint3",
        }
        LIST_CLASS = {
            "coFloats":           "ConfigOptionFloats",
            "coInts":             "ConfigOptionInts",
            "coBools":            "ConfigOptionBools",
            "coStrings":          "ConfigOptionStrings",
            "coPercents":         "ConfigOptionPercents",
            "coFloatsOrPercents": "ConfigOptionFloatsOrPercents",
            "coPoints":           "ConfigOptionPoints",
        }

        # Unescape escaped quotes, then re-escape actual newlines so they remain valid
        # in C++ string literals (proto \n is parsed as actual newline by protobuf).
        args = default_value.replace('\\"', '"').replace('\n', '\\n')

        # Empty args -> default constructor for any type
        if not args:
            if co_type == "coEnum":
                enum_type = "int"
                if enum_keys_map:
                    m = _re.match(r'ConfigOptionEnum<(\w+)>::', enum_keys_map)
                    if m:
                        enum_type = m.group(1)
                    else:
                        m2 = _re.match(r's_keys_map_(\w+)$', enum_keys_map)
                        if m2:
                            enum_type = m2.group(1)
                return f"new ConfigOptionEnum<{enum_type}>()"
            if co_type == "coEnums":
                cls = "ConfigOptionEnumsGenericNullable" if is_nullable else "ConfigOptionEnumsGeneric"
                return f"new {cls}{{}}"
            NULLABLE_LIST_CLASS = {
                "coFloats":            "ConfigOptionFloatsNullable",
                "coInts":              "ConfigOptionIntsNullable",
                "coBools":             "ConfigOptionBoolsNullable",
                "coPercents":          "ConfigOptionPercentsNullable",
                "coFloatsOrPercents":  "ConfigOptionFloatsOrPercentsNullable",
            }
            all_classes = {**SCALAR_CLASS, **LIST_CLASS}
            if is_nullable and co_type in NULLABLE_LIST_CLASS:
                cls = NULLABLE_LIST_CLASS[co_type]
            else:
                cls = all_classes.get(co_type, "ConfigOption")
            return f"new {cls}()"

        if co_type in SCALAR_CLASS:
            return f"new {SCALAR_CLASS[co_type]}({args})"

        if co_type == "coFloatsOrPercents":
            # Vector default is a single FloatOrPercent element (broadcast across
            # extruders for nullable per-extruder configs). args e.g. "0, false".
            cls = "ConfigOptionFloatsOrPercentsNullable" if is_nullable else "ConfigOptionFloatsOrPercents"
            return f"new {cls}{{FloatOrPercent({args})}}"

        if co_type in LIST_CLASS:
            NULLABLE_LIST_CLASS = {
                "coFloats":    "ConfigOptionFloatsNullable",
                "coInts":      "ConfigOptionIntsNullable",
                "coBools":     "ConfigOptionBoolsNullable",
                "coPercents":  "ConfigOptionPercentsNullable",
            }
            cls = NULLABLE_LIST_CLASS[co_type] if (is_nullable and co_type in NULLABLE_LIST_CLASS) else LIST_CLASS[co_type]
            return f"new {cls}{{{args}}}"

        if co_type == "coEnum":
            # Extract enum type from two possible enum_keys_map formats:
            #   "ConfigOptionEnum<BedType>::get_enum_values()" -> "BedType"
            #   "s_keys_map_BedType" -> "BedType"
            enum_type = "int"
            if enum_keys_map:
                m = _re.match(r'ConfigOptionEnum<(\w+)>::', enum_keys_map)
                if m:
                    enum_type = m.group(1)
                else:
                    m2 = _re.match(r's_keys_map_(\w+)$', enum_keys_map)
                    if m2:
                        enum_type = m2.group(1)
            return f"new ConfigOptionEnum<{enum_type}>({args})"

        if co_type == "coEnums":
            cls = "ConfigOptionEnumsGenericNullable" if is_nullable else "ConfigOptionEnumsGeneric"
            return f"new {cls}{{ {args} }}"

        # Fallback: try generic
        return f"new ConfigOption({args})"

    @staticmethod
    def _escape_cpp(s):
        """Escape a string for C++ string literal.

        Proto strings already contain C++ escape sequences (\\n, \\", etc.)
        as literal backslash + char. We pass those through and only escape
        unescaped quotes and actual newlines.
        """
        if not s:
            return ""
        # Replace actual newline characters (rare) with \n escape
        s = s.replace('\n', '\\n')
        # Don't double-escape backslashes that are already part of escape sequences.
        # The proto strings store them as literal \n, \", \t etc.
        return s

    @staticmethod
    def _format_number(val):
        """Format a number for C++ (int vs float)."""
        if val is None:
            return "0"
        if isinstance(val, float) and val == int(val):
            return str(int(val))
        return str(val)


# ---------------------------------------------------------------------------
# Lint pass — catch typos that protoc accepts but break the build or runtime.
# protoc only validates proto-level correctness (field numbers, option types).
# It has no idea that option strings get pasted into C++ L("...") literals, or
# that a default must lie within [min_value, max_value]. These checks close that
# gap so a bad edit fails fast at codegen time instead of at C++ compile / DLL
# load / slice time.
# ---------------------------------------------------------------------------

# String options that the generator emits verbatim into C++ L("...") literals.
_CPP_STRING_ATTRS = ("label", "full_label", "tooltip", "category", "sidetext")

_NUMERIC_SCALAR = {"coFloat", "coInt", "coPercent"}
_NUMERIC_VECTOR = {"coFloats", "coInts", "coPercents"}
_INT_TYPES      = {"coInt", "coInts"}
_BOOL_TYPES     = {"coBool", "coBools"}


def _has_unescaped_quote(s):
    """True if the (proto-parsed) string contains a double-quote that is NOT
    backslash-escaped. Such a quote would terminate the generated C++ L("...")
    literal and break compilation. Mirrors how _escape_cpp passes strings
    through unchanged, so the proto must already carry '\\"' for a literal quote.
    """
    if not s:
        return False
    t = s.replace('\\\\', '')   # drop escaped backslashes so they don't mask a following quote
    t = t.replace('\\"', '')    # drop already-escaped quotes
    return '"' in t


# A plain C++ numeric literal: optional sign, digits/decimal, optional exponent,
# optional float suffix (e.g. "200", "0.", "-5", "1.5e3", "60.0f"). Anything else
# in default_value (identifiers, macros, "nil_value()", "{0.0}") is an intentional
# C++ expression that the generator passes through verbatim — not range-checkable.
_NUM_LIT = re.compile(r'^[+-]?(\d+\.?\d*|\.\d+)([eE][+-]?\d+)?[fF]?$')


def _num_tokens(dv):
    """Split a default into comma-separated tokens. Returns (tokens, all_numeric)."""
    tokens = [t.strip() for t in dv.split(',') if t.strip() != ""]
    all_numeric = bool(tokens) and all(_NUM_LIT.match(t) for t in tokens)
    return tokens, all_numeric


def _lint_default(field, errors):
    """Validate a field's default_value against its type and numeric range.

    Only plain numeric-literal defaults are checked; defaults that are C++
    expressions/constants are passed through by the generator and skipped here.
    """
    dv = field.default_value
    name = field.name

    if field.co_type in _NUMERIC_SCALAR or field.co_type in _NUMERIC_VECTOR:
        tokens, all_numeric = _num_tokens(dv)
        if not all_numeric:
            return  # intentional C++ expression (e.g. macro, nil_value(), braced init)
        for tok in tokens:
            num = float(tok.rstrip('fF'))
            if field.co_type in _INT_TYPES and num != int(num):
                errors.append(f"{name}: default_value '{dv}' is not an integer for {field.co_type}")
            if field.min_value is not None and num < field.min_value:
                errors.append(f"{name}: default {tok} is below min_value {field.min_value}")
            if field.max_value is not None and num > field.max_value:
                errors.append(f"{name}: default {tok} is above max_value {field.max_value}")

    elif field.co_type in _BOOL_TYPES:
        for tok in (t.strip().lower() for t in dv.split(',') if t.strip() != ""):
            # Only flag clear numeric mistakes; bare identifiers are C++ constants.
            if _NUM_LIT.match(tok) and tok not in ("0", "1"):
                errors.append(f"{name}: default_value token '{tok}' is not a valid bool for {field.co_type}")


def lint_fields(fields):
    """Return (errors, warnings) lists for the parsed fields.

    Errors block code generation; warnings are advisory.
    """
    errors = []
    warnings = []
    field_names = {f.name for f in fields}
    seen = set()

    for f in fields:
        # Duplicate field name -> duplicate add() in generated C++ (redefinition).
        if f.name in seen:
            errors.append(f"{f.name}: duplicate field name across proto files")
        seen.add(f.name)

        # min > max is always a mistake.
        if (f.min_value is not None and f.max_value is not None
                and f.min_value > f.max_value):
            errors.append(f"{f.name}: min_value ({f.min_value}) > max_value ({f.max_value})")

        # Unescaped quotes in strings emitted into C++ L("...") break compilation.
        for attr in _CPP_STRING_ATTRS:
            val = getattr(f, attr)
            if val and _has_unescaped_quote(val):
                errors.append(f'{f.name}: ({attr}) has an unescaped double-quote — '
                              f'use \\\\\\" in the proto for a literal quote')
        for el in f.enum_label_entries:
            if _has_unescaped_quote(el):
                errors.append(f"{f.name}: enum label '{el}' has an unescaped double-quote")

        # coEnum with a keys map but no value entries can fail to deserialize unless
        # the keys map is supplied in C++ (see AGENTS.md). Advisory — some enums
        # legitimately source their values from a get_enum_values() keys map.
        if f.co_type == "coEnum" and f.enum_keys_map and not f.enum_value_entries:
            warnings.append(f"{f.name}: coEnum has (enum_keys_map_ref) but no (enum_value_entries) "
                            f"— verify profile values deserialize (see AGENTS.md)")

        # default_value type/range checks.
        if f.has_default and f.default_value:
            _lint_default(f, errors)

        # ratio_over should point at a real key (advisory — may reference a virtual key).
        if f.ratio_over and f.ratio_over not in field_names:
            warnings.append(f"{f.name}: ratio_over references unknown key '{f.ratio_over}'")

    return errors, warnings


def _group_name_to_hook(name):
    """Convert group name to a C++ hook method suffix: 'Cooling Fan' -> 'cooling_fan'."""
    return re.sub(r'[^a-z0-9]+', '_', name.lower()).strip('_')


def _extract_field_paths(tab_layout_cpp):
    """
    Read existing TabLayout_generated.cpp to build a field -> doc-path lookup.
    Used as a bootstrap so the yaml doesn't need to repeat every path.
    Returns dict: field_key -> path_string
    """
    mapping = {}
    if not Path(tab_layout_cpp).exists():
        return mapping
    with open(tab_layout_cpp, 'r', encoding='utf-8', errors='replace') as f:
        content = f.read()
    for key, path in re.findall(
            r'append_single_option_line\("([^"]+)",\s*"([^"]+)"\)', content):
        mapping[key] = path
    return mapping


def generate_tab_layout(layout_yaml_path, output_path, existing_cpp_path=None,
                        variant_fields=None):
    """
    Generate TabLayout_generated.cpp from layout.yaml.

    YAML group fields can be:
      - key                      string  → append_single_option_line(key, <lookup>)
      - {key: "path"}            dict    → append_single_option_line(key, "path")
      - [k1, k2, ...]            list    → multi-option Line
      - _separator_              string  → optgroup->append_separator()

    Group attributes:
      hook: true    → tab.layout_hook_<name>(optgroup.get())  (no field generation)
      gcode: true   → validate_custom_gcode_cb + edit_custom_gcode lambdas + gcode fields
      icon: "..."   → second arg to new_optgroup()
    """
    try:
        import yaml
    except ImportError:
        print("  ERROR: PyYAML not installed. Run: pip install pyyaml")
        return False

    with open(layout_yaml_path, 'r', encoding='utf-8') as f:
        layout = yaml.safe_load(f)

    # Bootstrap: extract existing field→path mappings so yaml doesn't need them all
    path_map = _extract_field_paths(existing_cpp_path) if existing_cpp_path else {}

    variant_fields = variant_fields or set()

    def _line_args(key, path):
        """Build the trailing args for append_single_option_line(key, path, idx).
        Emits the variant opt index (0) for per-extruder multi-variant fields."""
        if key in variant_fields:
            return f', "{path}", 0'   # path may be empty; idx=0 marks variant-aware
        return f', "{path}"' if path else ''

    lines = [
        "// ===== AUTO-GENERATED by tools/config_codegen.py from layout.yaml =====",
        "// DO NOT EDIT MANUALLY. Edit layout.yaml and re-run: python tools/run_codegen.py",
        "//",
        "// Included inside namespace Slic3r::GUI in Tab.cpp after validate_custom_gcode_cb",
        "// forward declaration. No namespace wrapper needed here.",
        "",
        "namespace { constexpr int gcode_field_height = 15; }",
        "namespace { constexpr int notes_field_height = 25; }",
        "",
    ]

    for tab in layout.get('tabs', []):
        tab_name = tab['name']  # e.g. TabPrint, TabFilament, TabPrinter
        pages = tab.get('pages', [])
        if not pages:
            continue

        # One inline function per page (or per tab if single page makes sense)
        # Convention: TabPrint_build_layout, TabFilament_build_main_layout,
        #             TabPrinter_build_basic_info_layout, TabPrinter_build_gcode_layout, etc.
        for page in pages:
            page_name = page['name']
            page_icon = page.get('icon', '')
            # Derive function name: TabPrint/"Quality" → TabPrint_build_quality_layout
            fn_suffix = _group_name_to_hook(page_name)
            fn_name = f"{tab_name}_build_{fn_suffix}_layout"

            lines.append(f"inline void {fn_name}({tab_name}& tab)")
            lines.append("{")
            lines.append(f"    PageShp page = tab.add_options_page(L(\"{page_name}\"), \"{page_icon}\");")

            for group in page.get('groups', []):
                gname     = group['name']
                gicon     = group.get('icon', '')
                is_hook   = group.get('hook', False)
                is_gcode  = group.get('gcode', False)
                fields    = group.get('fields', [])
                indent_n  = group.get('indent', 15)

                icon_arg = f', L"{gicon}"' if gicon else ''
                indent_arg = f', {indent_n}' if (is_gcode and indent_n != 15) else ''
                if is_gcode:
                    icon_arg = f', L"{gicon}"' if gicon else ', L"param_gcode"'
                    indent_arg = ', 0'

                lines.append("    {")
                lines.append(f"        auto optgroup = page->new_optgroup(L(\"{gname}\"){icon_arg}{indent_arg});")

                if is_hook:
                    hook_method = f"layout_hook_{_group_name_to_hook(gname)}"
                    lines.append(f"        tab.{hook_method}(optgroup.get());")

                elif is_gcode:
                    # Standard g-code group: validate callback + edit button + gcode fields
                    lines.append("        optgroup->m_on_change = [&tab, &optgroup_title = optgroup->title](const t_config_option_key& opt_key, const boost::any& value) {")
                    lines.append("            validate_custom_gcode_cb(&tab, optgroup_title, opt_key, value);")
                    lines.append("        };")
                    lines.append("        optgroup->edit_custom_gcode = [&tab](const t_config_option_key& opt_key) { tab.edit_custom_gcode(opt_key); };")
                    for field in fields:
                        key, path = _resolve_field(field, path_map)
                        if key:
                            path_arg = f', "{path}"' if path else ''
                            lines.append("        {")
                            lines.append(f"            Option option = optgroup->get_option(\"{key}\");")
                            lines.append("            option.opt.full_width = true;")
                            lines.append("            option.opt.is_code = true;")
                            lines.append("            option.opt.height = gcode_field_height;")
                            lines.append(f"            optgroup->append_single_option_line(option{path_arg});")
                            lines.append("        }")

                else:
                    # Regular group: generate append_single_option_line / multi-option line
                    for field in fields:
                        if isinstance(field, list):
                            # Multi-option line: [key1, key2, ...]. Variant fields
                            # take the opt index via get_option(key, 0).
                            lines.append("        {")
                            first = field[0]
                            first_idx = ", 0" if first in variant_fields else ""
                            lines.append(f"            Line line_{{optgroup->get_option(\"{first}\"{first_idx}).opt.label, optgroup->get_option(\"{first}\"{first_idx}).opt.tooltip}};")
                            for k in field:
                                k_idx = ", 0" if k in variant_fields else ""
                                lines.append(f"            line_.append_option(optgroup->get_option(\"{k}\"{k_idx}));")
                            lines.append("            optgroup->append_line(line_);")
                            lines.append("        }")
                        elif isinstance(field, str):
                            if field == '_separator_':
                                lines.append("        optgroup->append_separator();")
                            else:
                                path = path_map.get(field, '')
                                lines.append(f"        optgroup->append_single_option_line(\"{field}\"{_line_args(field, path)});")
                        elif isinstance(field, dict):
                            # {key: path} explicit path
                            for key, path in field.items():
                                lines.append(f"        optgroup->append_single_option_line(\"{key}\"{_line_args(key, path)});")

                lines.append("    }")

            lines.append("}")
            lines.append("")

    # Add backward-compatible wrapper functions that aggregate per-page functions
    # so Tab.cpp can call e.g. TabPrint_build_layout(*this) as before.
    wrappers = _build_wrappers(layout)
    lines.extend(wrappers)

    content = "\n".join(lines) + "\n"
    output_dir = Path(output_path).parent
    output_dir.mkdir(parents=True, exist_ok=True)
    with open(output_path, 'w', encoding='utf-8') as f:
        f.write(content)
    print(f"Generated: {output_path}")
    return True


def _build_wrappers(layout):
    """Generate aggregate wrapper functions for backward compatibility with Tab.cpp."""
    lines = ["// ── Aggregate wrappers (backward-compatible with Tab.cpp call sites) ──", ""]

    # Known wrappers: map from legacy function name → (tab_type, [page_names_to_include])
    # The tab_name and page_names determine which per-page functions get called.
    wrapper_specs = {
        "TabPrint":    ("TabPrint_build_layout",            None),   # all pages
        "TabFilament": ("TabFilament_build_main_layout",    ["Filament", "Cooling", "Multimaterial"]),
    }

    for tab in layout.get('tabs', []):
        tab_name = tab['name']
        pages = tab.get('pages', [])

        if tab_name == "TabPrinter":
            # Per-page wrappers for printer tab
            basic_info = [p for p in pages if p['name'] == "Basic information"]
            gcode_pages = [p for p in pages if p['name'] in ("Machine G-code", "Notes")]

            if basic_info:
                fn = f"TabPrinter_build_basic_info_layout"
                lines.append(f"inline void {fn}(TabPrinter& tab)")
                lines.append("{")
                for p in basic_info:
                    pf = f"TabPrinter_build_{_group_name_to_hook(p['name'])}_layout"
                    lines.append(f"    {pf}(tab);")
                lines.append("}")
                lines.append("")

            if gcode_pages:
                fn = "TabPrinter_build_gcode_layout"
                lines.append(f"inline void {fn}(TabPrinter& tab)")
                lines.append("{")
                for p in gcode_pages:
                    pf = f"TabPrinter_build_{_group_name_to_hook(p['name'])}_layout"
                    lines.append(f"    {pf}(tab);")
                lines.append("}")
                lines.append("")

        elif tab_name in wrapper_specs:
            legacy_fn, page_filter = wrapper_specs[tab_name]
            filtered = [p for p in pages if page_filter is None or p['name'] in page_filter]
            if not filtered:
                continue
            lines.append(f"inline void {legacy_fn}({tab_name}& tab)")
            lines.append("{")
            for p in filtered:
                pf = f"{tab_name}_build_{_group_name_to_hook(p['name'])}_layout"
                lines.append(f"    {pf}(tab);")
            lines.append("}")
            lines.append("")

    return lines


def _resolve_field(field, path_map):
    """Return (key, path) from a field entry in various yaml formats."""
    if isinstance(field, str):
        return field, path_map.get(field, '')
    elif isinstance(field, dict):
        for k, v in field.items():
            return k, (v or path_map.get(k, ''))
    return None, ''


def main():
    parser = argparse.ArgumentParser(
        description="Generate C++ config code from protobuf descriptors")
    parser.add_argument("descriptor_set",
                        help="Path to compiled .desc file (protoc --descriptor_set_out)")
    parser.add_argument("output_dir",
                        help="Directory to write generated C++ files")
    parser.add_argument("--lint-only", action="store_true",
                        help="Run proto lint checks and exit without writing files")
    args = parser.parse_args()

    # Read descriptor set
    desc_path = Path(args.descriptor_set)
    if not desc_path.exists():
        print(f"ERROR: Descriptor file not found: {desc_path}")
        sys.exit(1)

    global meta_pb2
    file_descriptor_set, meta_pb2 = load_descriptor_set(desc_path)

    print(f"Loaded {len(file_descriptor_set.file)} proto files")
    for fd in file_descriptor_set.file:
        if not fd.name.startswith("google/"):
            print(f"  - {fd.name}: {len(fd.message_type)} messages")

    # Generate code
    gen = CodeGenerator(file_descriptor_set)

    # Lint before writing anything so a bad proto edit never produces broken
    # generated files (fails fast at codegen instead of C++ compile / runtime).
    errors, warnings = lint_fields(gen.fields)
    for w in warnings:
        print(f"  LINT WARNING: {w}")
    if errors:
        print(f"\n*** Proto lint FAILED ({len(errors)} error(s)) ***")
        for e in errors:
            print(f"  LINT ERROR: {e}")
        sys.exit(2)
    print(f"Lint passed ({len(gen.fields)} fields checked, {len(warnings)} warning(s))")

    if args.lint_only:
        return

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    outputs = {
        "PrintConfigDef_generated.cpp": gen.generate_init_fff_params(),
        "Preset_options_generated.cpp": gen.generate_preset_options(),
        "Invalidation_generated.cpp": gen.generate_invalidation_map(),
        "OptionKeys_generated.cpp": gen.generate_option_key_lists(),
    }

    for filename, content in outputs.items():
        out_path = output_dir / filename
        with open(out_path, 'w', encoding='utf-8') as f:
            f.write(content)
        print(f"Generated: {out_path}")

    print(f"\nDone. {len(gen.fields)} settings processed.")

    # Generate tab layout from layout.yaml
    layout_yaml = desc_path.parent.parent / "src" / "PrintConfigs" / "layout.yaml"
    if not layout_yaml.exists():
        # Try repo root relative path
        layout_yaml = Path(__file__).resolve().parent.parent / "src" / "PrintConfigs" / "layout.yaml"
    if layout_yaml.exists():
        tab_layout_out = output_dir / "TabLayout_generated.cpp"
        existing = tab_layout_out if tab_layout_out.exists() else None
        # Per-extruder multi-variant fields = nullable PRESET_PRINT settings.
        # These get the variant opt index so the generated layout binds them to
        # the extruder/variant switch (filament nullable overrides use a separate
        # checkbox mechanism and must NOT get an index).
        variant_fields = {f.name for f in gen.fields
                          if f.is_nullable and f.preset == meta_pb2.PRESET_PRINT}
        generate_tab_layout(str(layout_yaml), str(tab_layout_out),
                            str(existing) if existing else None, variant_fields)
    else:
        print(f"  NOTE: layout.yaml not found at {layout_yaml}, skipping tab layout generation")


if __name__ == "__main__":
    main()
