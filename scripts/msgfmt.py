#!/usr/bin/env python3
"""Compile .po file to .mo file (replacement for msgfmt)."""

import struct
import sys
import os
import re

def unescape_po(s):
    """Unescape PO file escape sequences."""
    result = []
    i = 0
    while i < len(s):
        if s[i] == '\\' and i + 1 < len(s):
            c = s[i + 1]
            if c == 'n':
                result.append('\n')
            elif c == 't':
                result.append('\t')
            elif c == 'r':
                result.append('\r')
            elif c == '\\':
                result.append('\\')
            elif c == '"':
                result.append('"')
            else:
                result.append(s[i:i + 2])
            i += 2
        else:
            result.append(s[i])
            i += 1
    return ''.join(result)

def parse_po(filepath):
    entries = []
    with open(filepath, 'r', encoding='utf-8') as f:
        content = f.read()

    blocks = re.split(r'\n\n+', content)
    for block in blocks:
        block = block.strip()
        if not block:
            continue
        msgid = None
        msgstr = None
        msgid_plural = None
        current_key = None

        lines = block.split('\n')
        for line in lines:
            line_stripped = line.strip()
            if not line_stripped or line_stripped.startswith('#'):
                continue

            m = re.match(r'msg(id|id_plural|str)\s+"', line)
            if m:
                key = m.group(1)
                val_start = line.index('"') + 1
                if line.endswith('"'):
                    val = line[val_start:-1]
                else:
                    val = line[val_start:]
                current_key = key
            elif line_stripped.startswith('"') and current_key is not None:
                val = line_stripped[1:-1] if line_stripped.endswith('"') else line_stripped[1:]
            else:
                current_key = None
                continue

            if current_key == 'id':
                if msgid is None:
                    msgid = unescape_po(val)
                else:
                    msgid_plural = unescape_po(val)
            elif current_key == 'str':
                if msgstr is None:
                    msgstr = unescape_po(val)
                else:
                    msgstr += unescape_po(val)

        if msgid is not None and msgstr is not None:
            entries.append((msgid, msgstr, msgid_plural))

    return entries

def write_mo(entries, output_path):
    raw_entries = [(msgid, msgstr) for msgid, msgstr, _ in entries]

    sorted_entries = sorted(raw_entries, key=lambda x: (0 if x[0] == '' else 1, x[0]))

    N = len(sorted_entries)
    magic = 0x950412de
    revision = 0
    H_size = 0
    H_offset = 0

    orig_strings = b''
    trans_strings = b''
    for msgid, msgstr in sorted_entries:
        orig_strings += msgid.encode('utf-8') + b'\0'
        trans_strings += msgstr.encode('utf-8') + b'\0'

    header_size = 28
    orig_table_offset = header_size
    trans_table_offset = header_size + N * 8
    orig_strings_offset = header_size + 2 * N * 8
    trans_strings_offset = header_size + 2 * N * 8 + len(orig_strings)

    header = struct.pack('<7I', magic, revision, N,
                         orig_table_offset, trans_table_offset,
                         H_size, H_offset)

    orig_table = b''
    offset = 0
    for msgid, msgstr in sorted_entries:
        length = len(msgid.encode('utf-8')) + 1
        orig_table += struct.pack('<2I', length, orig_strings_offset + offset)
        offset += length

    trans_table = b''
    offset = 0
    for msgid, msgstr in sorted_entries:
        length = len(msgstr.encode('utf-8')) + 1
        trans_table += struct.pack('<2I', length, trans_strings_offset + offset)
        offset += length

    with open(output_path, 'wb') as f:
        f.write(header)
        f.write(orig_table)
        f.write(trans_table)
        f.write(orig_strings)
        f.write(trans_strings)

def main():
    if len(sys.argv) < 2:
        print("Usage: python msgfmt.py <input.po> [-o <output.mo>]", file=sys.stderr)
        sys.exit(1)

    input_path = sys.argv[1]
    output_path = None
    if '-o' in sys.argv:
        idx = sys.argv.index('-o')
        output_path = sys.argv[idx + 1]
    else:
        output_path = os.path.splitext(input_path)[0] + '.mo'

    entries = parse_po(input_path)
    write_mo(entries, output_path)
    print(f"Generated {output_path} with {len(entries)} entries.")

if __name__ == '__main__':
    main()
