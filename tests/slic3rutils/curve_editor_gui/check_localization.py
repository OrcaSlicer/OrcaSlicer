"""Verify source coverage and placeholder/whitespace contracts in curve catalogs."""
import argparse
import ast
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SOURCES = [
    "src/libslic3r/CurveModel.cpp",
    "src/libslic3r/GCode/SmallAreaInfillFlowCompensationModel.cpp",
    "src/slic3r/GUI/CurveEditorDialog.cpp",
    "src/slic3r/GUI/CurveEditorPanel.cpp",
    "src/slic3r/GUI/SmallAreaInfillFlowCompensationDialog.cpp",
]
FORMATS = re.compile(r"%(?:\d+\$)?[-+ #0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|ll|[hljztL])?[diouxXeEfFgGaAcspn%]|%\d+%")

def entries(path):
    result = {}
    for block in re.split(r"\r?\n\r?\n", path.read_text(encoding="utf-8")):
        values = {}
        current = None
        for line in block.splitlines():
            match = re.match(r'(msgctxt|msgid|msgstr) (".*")$', line)
            if match:
                current = match[1]
                values[current] = ast.literal_eval(match[2])
            elif line.startswith('"') and current:
                values[current] += ast.literal_eval(line)
            else:
                current = None
        if "msgid" in values:
            result[(values.get("msgctxt"), values["msgid"])] = (values, block)
    return result

def executable(name, supplied):
    candidate = supplied or shutil.which(name) or str(ROOT / "tools" / (name + ".exe"))
    if not Path(candidate).is_file():
        raise RuntimeError(f"Install gettext or provide --{name}.")
    return candidate

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xgettext")
    parser.add_argument("--msgfmt")
    args = parser.parse_args()
    xgettext = executable("xgettext", args.xgettext)
    msgfmt = executable("msgfmt", args.msgfmt)
    catalog_root = ROOT / "localization/i18n"
    with tempfile.TemporaryDirectory(prefix="orca-curve-l10n-") as temporary:
        source = Path(temporary) / "curve.pot"
        subprocess.run([xgettext, "--keyword=L", "--keyword=_L", "--keyword=_u8L",
                        "--keyword=_L_CONTEXT:1,2c", "--keyword=_u8L_CONTEXT:1,2c", "--from-code=UTF-8",
                        "--no-location", "--no-wrap", "-o", str(source), *SOURCES], cwd=ROOT, check=True)
        required = {key for key in entries(source) if key[1]}
        required.update({(None, "OK"), (None, "Cancel")})  # Shared dialog buttons use dynamic labels.
        template = entries(catalog_root / "OrcaSlicer.pot")
        missing = required - template.keys()
        if missing:
            raise RuntimeError(f"Messages missing from the template: {missing}")
        failures = []
        catalogs = sorted(catalog_root.glob("*/OrcaSlicer_*.po"))
        for path in catalogs:
            lang = path.parent.name
            translated = entries(path)
            for key in required:
                if key not in translated:
                    failures.append((lang, "missing", key[1]))
                    continue
                values, block = translated[key]
                if lang == "en":
                    continue  # English falls back to the source string.
                text = values.get("msgstr", "")
                if not text or re.search(r"^#,.*\bfuzzy\b", block, re.M):
                    failures.append((lang, "empty/fuzzy", key[1]))
                elif (FORMATS.findall(text) != FORMATS.findall(key[1]) or
                      text.count("\n") != key[1].count("\n") or
                      re.match(r"^\s*", text)[0] != re.match(r"^\s*", key[1])[0] or
                      re.search(r"\s*$", text)[0] != re.search(r"\s*$", key[1])[0]):
                    failures.append((lang, "placeholder/whitespace mismatch", key[1]))
            subprocess.run([msgfmt, "--check-format", "-o", str(Path(temporary) / (lang + ".mo")), str(path)], check=True)
        if failures:
            raise RuntimeError("\n".join(map(str, failures)))
        print(f"Verified {len(required)} curve messages in {len(catalogs)} catalogs.")

if __name__ == "__main__":
    main()
