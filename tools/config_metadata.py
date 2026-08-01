#!/usr/bin/env python3
"""
Access to the orca.* option extensions declared in config_metadata.proto.

The extensions are read straight out of the compiled descriptor set: protoc runs
with --include_imports, so config_metadata.proto travels inside the .desc file
that the codegen already consumes. Registering it in the default descriptor pool
before the descriptor set is parsed is what makes the custom options resolve
instead of landing in unknown fields.

Doing it this way keeps generated code out of git. The alternative -- a checked-in
config_metadata_pb2.py -- also pinned the protobuf runtime to whichever protoc
produced it, because gencode embeds a hard ValidateProtobufRuntimeVersion() check.
"""

from google.protobuf import descriptor_pb2, descriptor_pool

METADATA_PROTO = "config_metadata.proto"


class Metadata:
    """
    Stand-in for the generated config_metadata_pb2 module.

    Exposes each orca extension as an attribute holding its FieldDescriptor
    (usable as `options.Extensions[meta.label]`) and each enum value as an int
    constant (`meta.MODE_SIMPLE`, `meta.STEP_SLICE`, ...), matching how the
    generated module was used.
    """

    def __init__(self, file_descriptor):
        for name, extension in file_descriptor.extensions_by_name.items():
            setattr(self, name, extension)
        for enum in file_descriptor.enum_types_by_name.values():
            for value in enum.values:
                setattr(self, value.name, value.number)


def load_descriptor_set(path):
    """
    Read a protoc descriptor set and return (FileDescriptorSet, Metadata).

    The file is parsed twice on purpose: the first pass only locates the embedded
    config_metadata.proto so its extensions can be registered, the second one
    parses with those extensions known.
    """
    with open(path, 'rb') as f:
        raw = f.read()

    probe = descriptor_pb2.FileDescriptorSet()
    probe.ParseFromString(raw)
    metadata_file = next((f for f in probe.file if f.name == METADATA_PROTO), None)
    if metadata_file is None:
        raise RuntimeError(
            f"{path} does not contain {METADATA_PROTO} -- protoc must be run with "
            "--include_imports")

    pool = descriptor_pool.Default()
    try:
        file_descriptor = pool.FindFileByName(METADATA_PROTO)
    except KeyError:
        pool.Add(metadata_file)
        file_descriptor = pool.FindFileByName(METADATA_PROTO)

    descriptor_set = descriptor_pb2.FileDescriptorSet()
    descriptor_set.ParseFromString(raw)
    return descriptor_set, Metadata(file_descriptor)
