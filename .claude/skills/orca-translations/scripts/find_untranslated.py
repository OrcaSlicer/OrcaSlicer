"""List C++ string literals that look like user-facing English but are not marked for translation.

Usage: find_untranslated.py OUT_DIR   (one hits file per source area; each line is file:line: [call] "text" | code)
Works on whole files, so literals continuing a translation macro call from an earlier line are
recognised. Skips comments, log/assert/check calls, includes, printer model lists and markup."""
import re, sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[4]
OUT = Path(sys.argv[1]); OUT.mkdir(parents=True, exist_ok=True)
MACROS = {"_L", "_u8L", "L", "_L_CONTEXT", "_u8L_CONTEXT", "L_CONTEXT", "_L_PLURAL", "_devL", "_omitL",
          "_CHB", "_utf8", "_", "translate", "translate_utf8", "wxGetTranslation", "T"}
NOISE_CALLS = {"BOOST_LOG_TRIVIAL", "wxCHECK", "wxCHECK_MSG", "wxCHECK_RET", "wxASSERT", "wxASSERT_MSG", "wxFAIL_MSG",
               "assert", "static_assert", "printf", "fprintf", "sprintf", "snprintf", "wxLogDebug", "wxLogTrace",
               "wxLogMessage", "wxLogError", "wxLogWarning", "OutputDebugStringA", "OutputDebugStringW", "DEBUG_LOG",
               "glsafe", "GLShaderProgram", "find", "count", "contains", "starts_with", "ends_with", "get_value",
               "has", "opt_bool", "opt_int", "opt_float", "opt_string", "option", "get", "set_key_value", "at",
               "boost_log", "LOGD", "LOGE", "LOGI", "LOGW", "SPDLOG", "CLI_LOG"}
LIT = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
WORD = re.compile(r"[A-Za-z]{2,}")

def strip_comments(text):
    # Replace comments with spaces, keeping offsets and string literals intact.
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1]); i = j + 1
        elif text.startswith("//", i):
            j = text.find("\n", i); j = n if j < 0 else j
            out.append(" " * (j - i)); i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2); j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j])); i = j
        else:
            out.append(c); i += 1
    return "".join(out)

def enclosing_call(text, pos):
    depth = 0
    i = pos - 1
    while i >= 0:
        c = text[i]
        if c == '"':  # skip over a literal backwards
            i -= 1
            while i >= 0 and not (text[i] == '"' and text[i - 1] != "\\"):
                i -= 1
        elif c in ")]}":
            depth += 1
        elif c in "([{":
            if depth == 0:
                if c != "(":
                    return None
                m = re.search(r"([A-Za-z_][\w:]*)\s*$", text[max(0, i - 80):i])
                return m.group(1).split("::")[-1] if m else None
            depth -= 1
        elif c == ";" and depth == 0:
            return None
        i -= 1
    return None

def looks_user_facing(s):
    if len(WORD.findall(s)) < 2 and not re.fullmatch(r"[A-Z][a-z]{2,}[.!?:]?", s):
        return False
    if " " not in s and re.fullmatch(r"[\w./\\:#%-]+", s):
        return False
    if re.search(r"https?://|<\w+[^>]*>|\.(png|svg|json|ini|3mf|stl|gcode|bbl|js|html|css|txt|xml|zip)\b|=\s*\w+;|^\s*[-\w]+\s*:\s*[\w#]", s):
        return False
    if re.fullmatch(r"[a-z0-9_ ]+", s) and "_" in s:
        return False
    return bool(re.search(r"[A-Za-z]{2,} [a-z]{2,}|^[A-Z][a-z]{2,}", s))

groups = {}
for path in sorted((REPO / "src").rglob("*.[ch]pp")):
    rel = path.relative_to(REPO).as_posix()
    if "/dev-utils/" in rel or "/libvgcode/" in rel or rel.startswith("src/libnest2d") or "/tests/" in rel:
        continue
    raw = path.read_text(encoding="utf-8", errors="replace")
    text = strip_comments(raw)
    line_starts = [0] + [m.end() for m in re.finditer("\n", text)]
    lines = raw.splitlines()
    hits = []
    for m in LIT.finditer(text):
        s = m.group(1)
        if not looks_user_facing(s):
            continue
        call = enclosing_call(text, m.start())
        if call in MACROS or call in NOISE_CALLS:
            continue
        ln = next(k for k in range(len(line_starts) - 1, -1, -1) if line_starts[k] <= m.start()) + 1
        src = lines[ln - 1].strip()
        if re.search(r"BOOST_LOG|#\s*include|<<\s*\"|\"\s*<<|std::cerr|std::cout", src):
            continue
        # printer model / vendor tables
        if len(LIT.findall(src)) >= 4 and not re.search(r"_L|_u8L", src):
            continue
        hits.append(f"{rel}:{ln}: [{call or '-'}] \"{s[:150]}\"    | {src[:170]}")
    if hits:
        parts = rel.split("/")
        if parts[1] == "slic3r":
            key = f"GUI/{parts[3] if len(parts) > 4 else 'root'}" if parts[2] == "GUI" else f"slic3r/{parts[2]}"
        else:
            key = parts[1].replace(".cpp", "")
        groups.setdefault(key, []).extend(hits)
total = 0
for key, hits in sorted(groups.items()):
    (OUT / (key.replace("/", "_") + ".txt")).write_text("\n".join(hits) + "\n", encoding="utf-8")
    total += len(hits)
    print(f"{len(hits):5}  {key}")
print("total", total)
