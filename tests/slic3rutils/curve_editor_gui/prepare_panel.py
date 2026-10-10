"""Prepare production panel code for native input tests, excluding painting only."""
import re
import sys
from pathlib import Path

source = Path(sys.argv[1]).read_text(encoding="utf-8")
signature = "void CurveEditorPanel::paint_chart()"
start = source.index(signature)
body = source.index("{", start)
depth = 0
end = None
tokens = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)
for token in tokens.finditer(source, body):
    if token.group() == "{":
        depth += 1
    elif token.group() == "}":
        depth -= 1
        if depth == 0:
            end = token.end()
            break
if end is None:
    raise RuntimeError("Could not isolate the painting function")
source = source[:start] + signature + " {}" + source[end:]
for header in ("GUI_App.hpp", "Widgets/StateColor.hpp"):
    source = source.replace(f'#include "{header}"', "")
Path(sys.argv[2]).write_text(source, encoding="utf-8")
