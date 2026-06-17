#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import re
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

import polib


# File extensions to scan
SOURCE_EXTS = {".cpp", ".hpp", ".h", ".c", ".cc", ".cxx", ".mm", ".qml"}
HINTS_EXTS = {".ini"}  # hints.ini and similar
ALL_SCAN_EXTS = SOURCE_EXTS | HINTS_EXTS

SKIP_DIRS = {
    ".git",
    ".github",
    ".vs",
    ".vscode",
    "build",
    "deps",
    "deps_src",
    "out",
    "dist",
    "cmake-build-debug",
    "cmake-build-release",
}

# Recognized translation macros (these are the ones we want to replace their string arguments)
TRANSLATION_MACROS = {"L", "_", "_u8L", "_L"}

# Wrapper macros that just wrap a string literal (e.g., L("..."))
WRAPPER_MACROS = {"L", "U", "u8"}

# Keys in .ini files that may contain translatable text
INI_TRANSLATABLE_KEYS = {"text", "label", "tooltip", "hint", "title", "description"}


# ----------------------------------------------------------------------
# PO file saving helper (always no-wrap)
# ----------------------------------------------------------------------

def save_po(po: polib.POFile, path: Path) -> None:
    """Save a PO file with no wrapping."""
    po.wrapwidth = 0
    po.save(str(path))


def load_po(path: Path) -> polib.POFile:
    """Load a PO file and set no-wrap mode."""
    po = polib.pofile(str(path), encoding="utf-8")
    po.wrapwidth = 0
    return po


# ----------------------------------------------------------------------
# Helper functions
# ----------------------------------------------------------------------

def normalize_newlines(text: str) -> str:
    return text.replace("\r\n", "\n").replace("\r", "\n")


def remove_line_continuations(text: str) -> str:
    """Remove backslash-newline pairs (line continuations) from C++ source."""
    return re.sub(r"\\\n", "", text)


def decode_cpp_string_body(body: str) -> str:
    chars: List[str] = []
    i = 0
    n = len(body)
    while i < n:
        ch = body[i]
        if ch != "\\":
            chars.append(ch)
            i += 1
            continue

        i += 1
        if i >= n:
            chars.append("\\")
            break
        esc = body[i]

        if esc == "n":
            chars.append("\n")
            i += 1
        elif esc == "r":
            chars.append("\r")
            i += 1
        elif esc == "t":
            chars.append("\t")
            i += 1
        elif esc == "b":
            chars.append("\b")
            i += 1
        elif esc == "f":
            chars.append("\f")
            i += 1
        elif esc == "v":
            chars.append("\v")
            i += 1
        elif esc in {'"', "'", "?", "\\"}:
            chars.append(esc)
            i += 1
        elif esc == "a":
            chars.append("\a")
            i += 1
        elif esc == "x":
            i += 1
            start = i
            while i < n and body[i].lower() in "0123456789abcdef":
                i += 1
            hexdigits = body[start:i]
            if hexdigits:
                chars.append(chr(int(hexdigits, 16)))
            else:
                chars.append("x")
        elif esc in "01234567":
            digits = esc
            i += 1
            count = 1
            while i < n and count < 3 and body[i] in "01234567":
                digits += body[i]
                i += 1
                count += 1
            chars.append(chr(int(digits, 8)))
        elif esc == "u":
            i += 1
            digits = body[i : i + 4]
            if len(digits) == 4 and all(c.lower() in "0123456789abcdef" for c in digits):
                chars.append(chr(int(digits, 16)))
                i += 4
            else:
                chars.append("u")
        elif esc == "U":
            i += 1
            digits = body[i : i + 8]
            if len(digits) == 8 and all(c.lower() in "0123456789abcdef" for c in digits):
                chars.append(chr(int(digits, 16)))
                i += 8
            else:
                chars.append("U")
        else:
            chars.append(esc)
            i += 1

    return "".join(chars)


def escape_cpp_string(value: str) -> str:
    """Escape a string for use as a C++ string literal."""
    escaped: List[str] = []
    for ch in value:
        if ch == "\\":
            escaped.append("\\\\")
        elif ch == '"':
            escaped.append('\\"')
        elif ch == "'":
            escaped.append("\\'")
        elif ch == "\n":
            escaped.append("\\n")
        elif ch == "\r":
            escaped.append("\\r")
        elif ch == "\t":
            escaped.append("\\t")
        elif ch == "\b":
            escaped.append("\\b")
        elif ch == "\f":
            escaped.append("\\f")
        elif ch == "\v":
            escaped.append("\\v")
        elif ch == "\a":
            escaped.append("\\a")
        elif ch == "?":
            escaped.append("\\?")
        else:
            code = ord(ch)
            if code < 0x20 or code == 0x7F:
                escaped.append(f"\\x{code:02x}")
            elif code > 0x7F:
                if code <= 0xFFFF:
                    escaped.append(f"\\u{code:04x}")
                else:
                    escaped.append(f"\\U{code:08x}")
            else:
                escaped.append(ch)
    return "".join(escaped)


def decode_ini_value(value: str) -> str:
    """Decode escape sequences found in .ini values (like \n, \t, \\, etc.)."""
    result = []
    i = 0
    n = len(value)
    while i < n:
        ch = value[i]
        if ch == '\\' and i + 1 < n:
            esc = value[i+1]
            if esc == 'n':
                result.append('\n')
                i += 2
            elif esc == 'r':
                result.append('\r')
                i += 2
            elif esc == 't':
                result.append('\t')
                i += 2
            elif esc == '\\':
                result.append('\\')
                i += 2
            elif esc == '"':
                result.append('"')
                i += 2
            elif esc == "'":
                result.append("'")
                i += 2
            else:
                result.append(ch)
                i += 1
        else:
            result.append(ch)
            i += 1
    return ''.join(result)


def escape_ini_value(value: str) -> str:
    """Escape a string for storage in an INI file."""
    escaped = []
    for ch in value:
        if ch == '\n':
            escaped.append('\\n')
        elif ch == '\r':
            escaped.append('\\r')
        elif ch == '\t':
            escaped.append('\\t')
        elif ch == '\\':
            escaped.append('\\\\')
        elif ch == '"':
            escaped.append('\\"')
        else:
            escaped.append(ch)
    return ''.join(escaped)


# ----------------------------------------------------------------------
# C++ parsing functions
# ----------------------------------------------------------------------

def skip_space_and_comments(text: str, i: int, end: int) -> int:
    while i < end:
        if text[i].isspace():
            i += 1
            continue
        if i + 1 < end and text[i] == "/" and text[i + 1] == "/":
            i += 2
            while i < end and text[i] != "\n":
                i += 1
            continue
        if i + 1 < end and text[i] == "/" and text[i + 1] == "*":
            i += 2
            while i + 1 < end and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i = min(i + 2, end)
            continue
        break
    return i


def parse_string_literal_token(text: str, i: int, end: int) -> Optional[Tuple[int, str, str]]:
    start = i
    while i < end and text[i] in "uULR8":
        i += 1
    prefix = text[start:i]
    if i >= end or text[i] != '"':
        return None

    if "R" in prefix:
        return None

    i += 1
    body_chars: List[str] = []
    while i < end:
        ch = text[i]
        if ch == "\\":
            if i + 1 >= end:
                body_chars.append("\\")
                i += 1
            else:
                body_chars.append("\\")
                body_chars.append(text[i + 1])
                i += 2
            continue
        if ch == '"':
            i += 1
            return i, prefix, "".join(body_chars)
        body_chars.append(ch)
        i += 1
    return None


def parse_concat_string_expression(arg_text: str) -> Optional[Tuple[str, str]]:
    i = 0
    end = len(arg_text)
    pieces: List[str] = []
    first_prefix = ""
    found_any = False

    while True:
        i = skip_space_and_comments(arg_text, i, end)
        if i >= end:
            break

        token = parse_string_literal_token(arg_text, i, end)
        if token is None:
            return None

        i, prefix, body = token
        found_any = True
        if not first_prefix:
            first_prefix = prefix
        pieces.append(decode_cpp_string_body(body))

    if not found_any:
        return None
    return first_prefix, "".join(pieces)


def parse_wrapped_macro_call(text: str) -> Optional[Tuple[str, str]]:
    i = 0
    end = len(text)
    i = skip_space_and_comments(text, i, end)
    if i >= end:
        return None

    start = i
    while i < end and (text[i].isalnum() or text[i] == "_"):
        i += 1
    name = text[start:i]
    if name not in WRAPPER_MACROS:
        return None

    i = skip_space_and_comments(text, i, end)
    if i >= end or text[i] != '(':
        return None
    i += 1

    depth = 1
    arg_start = i
    while i < end and depth > 0:
        if text[i] in ('"', "'"):
            quote = text[i]
            i += 1
            while i < end:
                if text[i] == "\\":
                    i += 2
                elif text[i] == quote:
                    i += 1
                    break
                else:
                    i += 1
            continue
        if i + 1 < end and text[i] == "/" and text[i + 1] == "/":
            i += 2
            while i < end and text[i] != "\n":
                i += 1
            continue
        if i + 1 < end and text[i] == "/" and text[i + 1] == "*":
            i += 2
            while i + 1 < end and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i = min(i + 2, end)
            continue
        if text[i] == '(':
            depth += 1
        elif text[i] == ')':
            depth -= 1
        i += 1

    if depth != 0:
        return None

    arg_end = i - 1
    inner_text = text[arg_start:arg_end]
    parsed = parse_concat_string_expression(inner_text)
    if parsed is None:
        return None
    prefix, inner_string = parsed
    return name, inner_string


def find_macro_calls(text: str) -> List[Tuple[int, int, int, int, str]]:
    calls: List[Tuple[int, int, int, int, str]] = []
    i = 0
    n = len(text)

    while i < n:
        ch = text[i]

        if ch in ('"', "'"):
            quote = ch
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                elif text[i] == quote:
                    i += 1
                    break
                else:
                    i += 1
            continue

        if i + 1 < n and text[i] == "/" and text[i + 1] == "/":
            i += 2
            while i < n and text[i] != "\n":
                i += 1
            continue

        if i + 1 < n and text[i] == "/" and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i = min(i + 2, n)
            continue

        if ch.isalpha() or ch == "_":
            start = i
            i += 1
            while i < n and (text[i].isalnum() or text[i] == "_"):
                i += 1
            name = text[start:i]
            if name not in TRANSLATION_MACROS:
                continue

            j = i
            while j < n and text[j].isspace():
                j += 1
            if j >= n or text[j] != "(":
                continue

            depth = 1
            k = j + 1
            while k < n and depth > 0:
                if text[k] in ('"', "'"):
                    quote = text[k]
                    k += 1
                    while k < n:
                        if text[k] == "\\":
                            k += 2
                        elif text[k] == quote:
                            k += 1
                            break
                        else:
                            k += 1
                    continue

                if k + 1 < n and text[k] == "/" and text[k + 1] == "/":
                    k += 2
                    while k < n and text[k] != "\n":
                        k += 1
                    continue

                if k + 1 < n and text[k] == "/" and text[k + 1] == "*":
                    k += 2
                    while k + 1 < n and not (text[k] == "*" and text[k + 1] == "/"):
                        k += 1
                    k = min(k + 2, n)
                    continue

                if text[k] == "(":
                    depth += 1
                elif text[k] == ")":
                    depth -= 1
                k += 1

            if depth == 0:
                calls.append((start, k, j + 1, k - 1, name))
                i = k
                continue
        else:
            i += 1

    return calls


# ----------------------------------------------------------------------
# PO file handling
# ----------------------------------------------------------------------

def entry_key(entry: polib.POEntry, msgid: Optional[str] = None) -> Tuple[Optional[str], str, str]:
    return (entry.msgctxt, msgid if msgid is not None else entry.msgid, entry.msgid_plural or "")


def merge_entries(target: polib.POEntry, source: polib.POEntry, previous_old: Optional[str] = None) -> None:
    if previous_old and not target.previous_msgid:
        target.previous_msgid = previous_old

    if not target.msgstr and source.msgstr:
        target.msgstr = source.msgstr

    source_plural = source.msgstr_plural or {}
    target_plural = target.msgstr_plural or {}
    for idx, value in source_plural.items():
        if idx not in target_plural or not target_plural[idx]:
            target_plural[idx] = value
    if target_plural:
        target.msgstr_plural = target_plural

    target.flags = sorted(set(target.flags or []).union(source.flags or []).union({"fuzzy"}))

    if source.occurrences:
        merged_occ = list(dict.fromkeys((target.occurrences or []) + source.occurrences))
        target.occurrences = merged_occ

    if source.comment:
        if target.comment:
            if source.comment not in target.comment:
                target.comment = f"{target.comment}\n{source.comment}"
        else:
            target.comment = source.comment

    if source.tcomment:
        if target.tcomment:
            if source.tcomment not in target.tcomment:
                target.tcomment = f"{target.tcomment}\n{source.tcomment}"
        else:
            target.tcomment = source.tcomment


def dedupe_po_entries(po: polib.POFile) -> bool:
    changed = False
    seen: Dict[Tuple[Optional[str], str, str], polib.POEntry] = {}
    to_remove: List[polib.POEntry] = []

    for entry in po:
        key = entry_key(entry)
        existing = seen.get(key)
        if existing is None:
            seen[key] = entry
            continue

        if existing.obsolete and not entry.obsolete:
            merge_entries(entry, existing)
            to_remove.append(existing)
            seen[key] = entry
            changed = True
            continue

        if not existing.obsolete and entry.obsolete:
            merge_entries(existing, entry)
            to_remove.append(entry)
            changed = True
            continue

        merge_entries(existing, entry)
        to_remove.append(entry)
        changed = True

    for dup in to_remove:
        po.remove(dup)

    return changed


def collect_en_mapping(en_dir: Path) -> Tuple[Dict[str, str], List[Path], List[str]]:
    mapping: Dict[str, str] = {}
    modified_files: List[Path] = []
    conflicts: List[str] = []

    for po_path in sorted(en_dir.glob("*.po")):
        po = load_po(po_path)
        changed = False
        to_remove: List[polib.POEntry] = []
        index: Dict[Tuple[Optional[str], str, str], polib.POEntry] = {}
        for existing in po:
            if existing.obsolete:
                continue
            index.setdefault(entry_key(existing), existing)

        for entry in po:
            if entry.obsolete:
                continue
            if not entry.msgid:
                continue
            if not entry.msgstr:
                continue

            old = entry.msgid
            new = entry.msgstr
            if old in mapping and mapping[old] != new:
                conflicts.append(f"Conflicting remap for '{old}' in {po_path}: '{mapping[old]}' vs '{new}'")
                continue

            mapping[old] = new
            new_key = entry_key(entry, new)
            target = index.get(new_key)
            if target is not None and target is not entry:
                merge_entries(target, entry, previous_old=old)
                to_remove.append(entry)
                changed = True
            else:
                old_key = entry_key(entry)
                entry.msgid = new
                entry.msgstr = new
                if old_key in index and index[old_key] is entry:
                    del index[old_key]
                index[new_key] = entry
                changed = True

        for entry in to_remove:
            po.remove(entry)

        if dedupe_po_entries(po):
            changed = True

        if changed:
            save_po(po, po_path)
            modified_files.append(po_path)

    return mapping, modified_files, conflicts


# ----------------------------------------------------------------------
# Source file rewriting (C++ and INI)
# ----------------------------------------------------------------------

def iter_files(root: Path) -> List[Path]:
    files: List[Path] = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for filename in filenames:
            path = Path(dirpath) / filename
            if path.suffix.lower() in ALL_SCAN_EXTS:
                files.append(path)
    return files


def rewrite_cpp_file(path: Path, mapping: Dict[str, str]) -> Tuple[bool, int]:
    try:
        original = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        original = path.read_text(encoding="utf-8-sig")

    text = normalize_newlines(original)
    text = remove_line_continuations(text)

    calls = find_macro_calls(text)
    replacements: List[Tuple[int, int, str]] = []

    for _call_start, _call_end, arg_start, arg_end, _name in calls:
        arg_text = text[arg_start:arg_end]

        parsed = parse_concat_string_expression(arg_text)
        wrapper = None
        final_value = None
        if parsed is not None:
            prefix, final_value = parsed
        else:
            wrapped = parse_wrapped_macro_call(arg_text)
            if wrapped is not None:
                wrapper, final_value = wrapped
            else:
                continue

        if final_value not in mapping:
            continue

        new_value = mapping[final_value]
        escaped = escape_cpp_string(new_value)

        if wrapper is not None:
            replacement = f'{wrapper}("{escaped}")'
        else:
            replacement = f'{prefix}"{escaped}"'

        replacements.append((arg_start, arg_end, replacement))

    if not replacements:
        return False, 0

    replacements.sort(reverse=True)
    mutable = text
    for start, end, repl in replacements:
        mutable = mutable[:start] + repl + mutable[end:]

    if mutable != text:
        path.write_text(mutable, encoding="utf-8", newline="\n")
        return True, len(replacements)
    return False, 0


def rewrite_ini_file(path: Path, mapping: Dict[str, str]) -> Tuple[bool, int]:
    try:
        content = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        content = path.read_text(encoding="utf-8-sig")

    lines = content.splitlines(keepends=True)
    changed = False
    replacements = 0
    new_lines: List[str] = []

    for line in lines:
        stripped = line.strip()
        if not stripped or stripped[0] in (';', '#'):
            new_lines.append(line)
            continue

        match = re.match(r'^\s*([a-zA-Z0-9_]+)\s*=\s*(.*)$', line)
        if not match:
            new_lines.append(line)
            continue

        key = match.group(1)
        if key.lower() not in INI_TRANSLATABLE_KEYS:
            new_lines.append(line)
            continue

        value = match.group(2).rstrip('\n\r')
        decoded = decode_ini_value(value)
        if decoded in mapping:
            new_value = mapping[decoded]
            encoded = escape_ini_value(new_value)
            indent = line[:line.find(key)]
            new_line = f'{indent}{key} = {encoded}\n'
            new_lines.append(new_line)
            changed = True
            replacements += 1
        else:
            new_lines.append(line)

    if changed:
        path.write_text(''.join(new_lines), encoding="utf-8", newline="\n")
        return True, replacements
    return False, 0


def rewrite_source_file(path: Path, mapping: Dict[str, str]) -> Tuple[bool, int]:
    if path.suffix.lower() in SOURCE_EXTS:
        return rewrite_cpp_file(path, mapping)
    elif path.suffix.lower() in HINTS_EXTS:
        return rewrite_ini_file(path, mapping)
    return False, 0


# ----------------------------------------------------------------------
# Non-English PO update
# ----------------------------------------------------------------------

def update_non_english_po(
    i18n_dir: Path,
    mapping: Dict[str, str],
) -> Tuple[List[Path], int]:
    modified: List[Path] = []
    skipped_duplicate_target = 0

    for lang_dir in sorted(i18n_dir.iterdir()):
        if not lang_dir.is_dir() or lang_dir.name == "en":
            continue

        for po_path in sorted(lang_dir.glob("*.po")):
            po = load_po(po_path)
            changed = False
            to_remove: List[polib.POEntry] = []
            index: Dict[Tuple[Optional[str], str, str], polib.POEntry] = {}
            for existing in po:
                if existing.obsolete:
                    continue
                index.setdefault(entry_key(existing), existing)

            for entry in po:
                if entry.obsolete:
                    continue
                if entry.msgid_plural:
                    continue
                old = entry.msgid
                if old not in mapping:
                    continue

                new = mapping[old]
                new_key = entry_key(entry, new)
                target = index.get(new_key)

                if target is not None and target is not entry:
                    merge_entries(target, entry, previous_old=old)
                    entry.obsolete = True
                    if entry.comment:
                        entry.comment += f"\nReplaced by msgid '{new}'"
                    else:
                        entry.comment = f"Replaced by msgid '{new}'"
                    changed = True
                    skipped_duplicate_target += 1
                    continue

                entry.previous_msgid = old
                old_key = entry_key(entry)
                entry.msgid = new
                flags = set(entry.flags or [])
                flags.add("fuzzy")
                entry.flags = sorted(flags)
                if old_key in index and index[old_key] is entry:
                    del index[old_key]
                index[new_key] = entry
                changed = True

            for entry in to_remove:
                po.remove(entry)

            recovered = recover_from_obsolete_variants(po)
            if recovered:
                changed = True

            if dedupe_po_entries(po):
                changed = True

            if changed:
                save_po(po, po_path)
                modified.append(po_path)

    return modified, skipped_duplicate_target


def dedupe_all_po(i18n_dir: Path) -> List[Path]:
    modified: List[Path] = []
    for po_path in sorted(i18n_dir.glob("*/*.po")):
        po = load_po(po_path)
        if dedupe_po_entries(po):
            save_po(po, po_path)
            modified.append(po_path)
    return modified


# ----------------------------------------------------------------------
# Recovery from obsolete variants
# ----------------------------------------------------------------------

def _has_translation(entry: polib.POEntry) -> bool:
    if entry.msgstr:
        return True
    return any(v for v in (entry.msgstr_plural or {}).values())


def _normalize_msgid_for_recovery(msgid: str) -> str:
    compact = " ".join((msgid or "").split())
    return re.sub(r"[.!?]+$", "", compact).strip()


def _similar_enough(a: str, b: str) -> bool:
    norm_a = _normalize_msgid_for_recovery(a)
    norm_b = _normalize_msgid_for_recovery(b)
    return norm_a == norm_b


def recover_from_obsolete_variants(po: polib.POFile) -> int:
    obsolete_map: Dict[Tuple[Optional[str], str], List[polib.POEntry]] = {}
    for entry in po:
        if not entry.obsolete:
            continue
        if entry.msgid_plural:
            continue
        if not _has_translation(entry):
            continue
        key = (entry.msgctxt, _normalize_msgid_for_recovery(entry.msgid))
        obsolete_map.setdefault(key, []).append(entry)

    recovered = 0
    for entry in po:
        if entry.obsolete:
            continue
        if entry.msgid_plural:
            continue
        if _has_translation(entry):
            continue

        norm = _normalize_msgid_for_recovery(entry.msgid)
        key = (entry.msgctxt, norm)
        candidates = obsolete_map.get(key, [])
        if not candidates:
            continue

        source = max(candidates, key=lambda e: len(e.msgstr or ""))
        if not source.msgstr:
            continue

        if not _similar_enough(entry.msgid, source.msgid):
            continue

        entry.msgstr = source.msgstr
        if not entry.previous_msgid:
            entry.previous_msgid = source.msgid
        flags = set(entry.flags or [])
        flags.add("fuzzy")
        entry.flags = sorted(flags)
        recovered += 1

    return recovered


# ----------------------------------------------------------------------
# Main
# ----------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description="Refactor msgids using English PO msgstr values.")
    parser.add_argument("--root", type=Path, default=Path.cwd(), help="Repository root")
    parser.add_argument(
        "--i18n-dir",
        type=Path,
        default=Path("localization/i18n"),
        help="Localization i18n directory relative to root",
    )
    parser.add_argument(
        "--sources-root",
        type=Path,
        default=Path("."),
        help="Source scan root relative to root",
    )
    args = parser.parse_args()

    root = args.root.resolve()
    i18n_dir = (root / args.i18n_dir).resolve()
    en_dir = i18n_dir / "en"
    sources_root = (root / args.sources_root).resolve()

    if not en_dir.exists():
        print(f"English PO directory not found: {en_dir}")
        return 1

    mapping, en_modified, conflicts = collect_en_mapping(en_dir)
    if conflicts:
        print("Warnings:")
        for line in conflicts:
            print(f"  - {line}")

    src_modified: List[Path] = []
    total_replacements = 0
    non_en_modified: List[Path] = []
    skipped_duplicate_target = 0

    if mapping:
        files = iter_files(sources_root)
        for file_path in files:
            changed, count = rewrite_source_file(file_path, mapping)
            if changed:
                src_modified.append(file_path)
                total_replacements += count

    non_en_modified, skipped_duplicate_target = update_non_english_po(i18n_dir, mapping)

    dedup_modified = dedupe_all_po(i18n_dir)

    print("Summary:")
    print(f"  - Mapping entries: {len(mapping)}")
    print(f"  - English PO files modified: {len(en_modified)}")
    print(f"  - Source/INI files modified: {len(src_modified)} (replacements: {total_replacements})")
    print(f"  - Non-English PO files modified: {len(non_en_modified)}")
    print(f"  - Non-English entries skipped (target already existed, marked obsolete): {skipped_duplicate_target}")
    print(f"  - PO files deduplicated: {len(dedup_modified)}")

    if en_modified:
        print("\nModified English PO files:")
        for path in en_modified:
            print(f"  - {path.relative_to(root)}")

    if src_modified:
        print("\nModified source/INI files:")
        for path in src_modified:
            print(f"  - {path.relative_to(root)}")

    if non_en_modified:
        print("\nModified non-English PO files:")
        for path in non_en_modified:
            print(f"  - {path.relative_to(root)}")

    if dedup_modified:
        print("\nDeduplicated PO files:")
        for path in dedup_modified:
            print(f"  - {path.relative_to(root)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())