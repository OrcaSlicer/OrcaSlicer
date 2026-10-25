#!/usr/bin/env python3
"""Translation helper for the OrcaSlicer gettext catalogs and the wiki glossary.

Standard library only. Edits a catalog in place, rewriting just the entries it
changes in the layout `msgmerge --no-wrap` writes, so every other byte stays put.

    status     [--lang L ...]                  per-catalog counts
    pending    --lang L [--offset N --limit N] [--out FILE] [--no-include-fuzzy]   (adds similar earlier translations)
    apply      --lang L --input FILE [--overwrite] [--dry-run]    adds "# AI Translated"
    check      [--lang L ...] [--scope ai|all] [--warnings]
    lookup     --lang L PATTERN [--msgstr]     existing translations with ids, obsolete ones included
    glossary   --lang L [TERM]                 the language's glossary column and notes
    candidates [--min-count N] [--limit N]     recurring untranslated phrases not in the glossary
    glossary-set --term T [--description D] --set L=VALUE ... [--replace]

The glossary is read from an OrcaSlicer_WIKI clone (--wiki, $ORCA_WIKI_DIR, or a
sibling of this repository). Without one it is downloaded read-only, and
glossary-set refuses to run.
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import unicodedata
import urllib.error
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[4]
I18N = REPO / "localization" / "i18n"
SKIP_LANGS = {"en"}  # the English catalog is intentionally empty
GLOSSARY_DIR = Path("developer_reference") / "glossary"  # terms.tsv, languages.txt and one <lang>.tsv per language
GLOSSARY_MD = Path("developer_reference") / "localization_glossary.md"
MISSING = {"", "—"}
RAW_URL = "https://raw.githubusercontent.com/OrcaSlicer/OrcaSlicer_WIKI/main/"
AI_TAG = "AI Translated"
REVIEW_TAG = "Needs human review:"


# ---------------------------------------------------------------------------
# PO parsing and writing

_UNESCAPE = {"n": "\n", "t": "\t", "r": "\r", "a": "\a", "b": "\b", "f": "\f", "v": "\v", '"': '"', "\\": "\\"}
_ESCAPE = {v: "\\" + k for k, v in _UNESCAPE.items()}


def unescape(s):
    return re.sub(r'\\(.)', lambda m: _UNESCAPE.get(m.group(1), m.group(0)), s)


def escape(s):
    return "".join(_ESCAPE.get(c, c) for c in s)


def format_field(keyword, value, prefix=""):
    """Lines for `keyword "value"` as msgmerge --no-wrap writes them."""
    parts = value.split("\n")
    segments = [p + "\n" for p in parts[:-1]] + ([parts[-1]] if parts[-1] else [])
    if len(segments) <= 1:
        return [f'{prefix}{keyword} "{escape(value)}"']
    return [f'{prefix}{keyword} ""'] + [f'{prefix}"{escape(s)}"' for s in segments]


class Entry:
    """One blank-line-delimited block of a PO file."""

    def __init__(self, lines, first_line):
        self.lines = lines
        self.first_line = first_line  # 0-based index in the file
        self.obsolete = all(l.startswith("#~") for l in lines if not l.startswith("#") or l.startswith("#~"))
        self.tcomments, self.xcomments, self.flags = [], [], []
        self.fields = {}  # keyword -> [value, start, end)
        self.order = []
        current = None
        for i, raw in enumerate(lines):
            line = raw
            if self.obsolete and line.startswith("#~"):
                line = line[2:].lstrip(" ")
                if line.startswith("|"):
                    current = None
                    continue
            elif line.startswith("#"):
                current = None
                if line.startswith("#."):
                    self.xcomments.append(line[2:].strip())
                elif line.startswith("#,"):
                    self.flags += [f.strip() for f in line[2:].split(",") if f.strip()]
                elif line == "#" or line.startswith("# "):
                    self.tcomments.append(line[2:])
                continue
            m = re.match(r'^(msgctxt|msgid_plural|msgid|msgstr(?:\[\d+\])?)\s+"(.*)"\s*$', line)
            if m:
                current = m.group(1)
                self.fields[current] = [unescape(m.group(2)), i, i + 1]
                self.order.append(current)
            elif line.startswith('"') and current:
                self.fields[current][0] += unescape(line.strip()[1:-1])
                self.fields[current][2] = i + 1

    def get(self, key, default=None):
        f = self.fields.get(key)
        return f[0] if f else default

    @property
    def msgctxt(self):
        return self.get("msgctxt")

    @property
    def msgid(self):
        return self.get("msgid", "")

    @property
    def msgid_plural(self):
        return self.get("msgid_plural")

    @property
    def is_header(self):
        return self.msgid == "" and "msgid" in self.fields and not self.msgctxt

    def msgstrs(self):
        if self.msgid_plural is None:
            return [self.get("msgstr", "")]
        idx = sorted(int(k[7:-1]) for k in self.fields if k.startswith("msgstr["))
        return [self.get(f"msgstr[{i}]", "") for i in range(max(idx) + 1)] if idx else []

    @property
    def fuzzy(self):
        return "fuzzy" in self.flags

    @property
    def ai(self):
        return any(c.strip() == AI_TAG for c in self.tcomments)

    @property
    def review(self):
        return [c for c in self.tcomments if c.startswith(REVIEW_TAG)]

    def key(self):
        return (self.msgctxt, self.msgid)

    def id(self):
        """Short stable handle, so a translation file need not repeat the English."""
        return hashlib.sha1(f"{self.msgctxt or ''}\x04{self.msgid}".encode("utf-8")).hexdigest()[:10]

    def rewrite(self, msgstrs, ai=True, review=None):
        """Replace the translation; add the AI and review comments; clear fuzzy."""
        lines = list(self.lines)
        # translations
        keys = [k for k in self.order if k.startswith("msgstr")]
        start = min(self.fields[k][1] for k in keys)
        end = max(self.fields[k][2] for k in keys)
        if self.msgid_plural is None:
            new = format_field("msgstr", msgstrs[0])
        else:
            new = [l for i, s in enumerate(msgstrs) for l in format_field(f"msgstr[{i}]", s)]
        lines[start:end] = new
        # flags and previous-msgid lines of a fuzzy entry
        out = []
        for line in lines:
            if line.startswith("#,"):
                flags = [f.strip() for f in line[2:].split(",") if f.strip() and f.strip() != "fuzzy"]
                if flags:
                    out.append("#, " + ", ".join(flags))
                continue
            if line.startswith("#|") and self.fuzzy:
                continue
            out.append(line)
        lines = out
        # translator comments go first, before #. #: #,
        pos = 0
        while pos < len(lines) and (lines[pos] == "#" or lines[pos].startswith("# ")):
            pos += 1
        add = []
        if not ai:  # a person's words: drop a stale AI mark
            lines = [l for l in lines if l != "# " + AI_TAG]
            pos = 0
            while pos < len(lines) and (lines[pos] == "#" or lines[pos].startswith("# ")):
                pos += 1
        if review is not None:  # "" clears an earlier note, a reason replaces it
            lines = [l for l in lines if not l.startswith("# " + REVIEW_TAG)]
            pos = 0
            while pos < len(lines) and (lines[pos] == "#" or lines[pos].startswith("# ")):
                pos += 1
        if ai and not any(l == "# " + AI_TAG for l in lines[:pos]):
            add.append("# " + AI_TAG)
        if review:
            add.append(f"# {REVIEW_TAG} {' '.join(review.split())}")
        lines[pos:pos] = add
        return lines


class Catalog:
    def __init__(self, path):
        self.path = Path(path)
        data = self.path.read_bytes()
        self.bom = data.startswith(b"\xef\xbb\xbf")
        text = data.decode("utf-8-sig")
        self.newline = "\r\n" if "\r\n" in text else "\n"
        self.trailing = text.endswith(("\n", "\r"))
        self.lines = text.splitlines()
        self.entries = []
        block, first = [], 0
        for i, line in enumerate(self.lines + [""]):
            if line.strip() == "":
                if block:
                    self.entries.append(Entry(block, first))
                block = []
            else:
                if not block:
                    first = i
                block.append(line)
        self.header = next((e for e in self.entries if e.is_header), None)
        m = re.search(r"nplurals\s*=\s*(\d+)", self.header.msgstrs()[0] if self.header else "")
        self.nplurals = int(m.group(1)) if m else 2
        self.changes = {}  # entry index -> new lines

    def active(self):
        return [e for e in self.entries if not e.obsolete and not e.is_header]

    def index(self):
        return {e.key(): i for i, e in enumerate(self.entries) if not e.obsolete and not e.is_header}

    def save(self):
        lines = list(self.lines)
        for i in sorted(self.changes, reverse=True):
            e = self.entries[i]
            lines[e.first_line:e.first_line + len(e.lines)] = self.changes[i]
        text = self.newline.join(lines) + (self.newline if self.trailing else "")
        self.path.write_bytes((b"\xef\xbb\xbf" if self.bom else b"") + text.encode("utf-8"))


def needs_translation(e, nplurals, include_fuzzy=False):
    strs = e.msgstrs()
    if e.msgid_plural is not None and len(strs) < nplurals:
        return True
    return any(s == "" for s in strs) or (include_fuzzy and e.fuzzy)


def catalog_path(lang):
    p = I18N / lang / f"OrcaSlicer_{lang}.po"
    if not p.exists():
        sys.exit(f"no catalog for '{lang}': {p}")
    return p


def all_langs():
    return sorted(d.name for d in I18N.iterdir() if (d / f"OrcaSlicer_{d.name}.po").exists() and d.name not in SKIP_LANGS)


# ---------------------------------------------------------------------------
# Glossary

def find_wiki(arg):
    for cand in [arg, os.environ.get("ORCA_WIKI_DIR"), REPO.parent / "OrcaSlicer_WIKI", REPO.parent / "OrcaSlicer.wiki"]:
        if cand and (Path(cand) / GLOSSARY_DIR / "terms.tsv").exists():
            return Path(cand)
    return None


def _download(rel, required=True):
    cache = Path(tempfile.gettempdir()) / "orca_glossary" / rel
    if not cache.exists() or time.time() - cache.stat().st_mtime > 3600:
        try:
            with urllib.request.urlopen(RAW_URL + rel.as_posix(), timeout=30) as r:
                data = r.read()
        except urllib.error.HTTPError:
            if required:
                raise
            return None
        cache.parent.mkdir(parents=True, exist_ok=True)
        cache.write_bytes(data)
    return cache


class Glossary:
    """The glossary: rows of [English, Description, <one translation per language loaded>]."""

    def __init__(self, wiki_arg=None, quiet=False, langs=None):
        self.wiki = find_wiki(wiki_arg)
        self.langs = langs  # only these languages' files are read (and downloaded); None means all
        if self.wiki:
            self.dir, self.md = self.wiki / GLOSSARY_DIR, self.wiki / GLOSSARY_MD
            if not quiet:
                self.report_clone_state()
        else:
            if not quiet:
                print("note: no OrcaSlicer_WIKI clone found; using the published glossary read-only", file=sys.stderr)
            self.dir, self.md = _download(GLOSSARY_DIR / "terms.tsv").parent, _download(GLOSSARY_MD)
        self.load()

    def report_clone_state(self):
        git = lambda *args: subprocess.run(["git", "-C", str(self.wiki), *args], capture_output=True, text=True).stdout.strip()
        try:
            branch = git("rev-parse", "--abbrev-ref", "HEAD")
            behind = git("rev-list", "--count", "HEAD..origin/main")
        except OSError:
            return
        if branch and branch != "main":
            print(f"note: glossary read from wiki branch '{branch}', not main", file=sys.stderr)
        if behind and behind != "0":
            print(f"note: the wiki clone is {behind} commits behind origin/main (as of its last fetch)", file=sys.stderr)

    def _read(self, name, required=True):
        path = self.dir / name if self.wiki else _download(GLOSSARY_DIR / name, required)
        return path.read_text(encoding="utf-8-sig").splitlines() if path and path.exists() else None

    def load(self):
        terms = [l.split("	") for l in self._read("terms.tsv")[1:] if l.strip()]
        langs = [l.strip() for l in self._read("languages.txt") if l.strip()]
        langs = [l for l in langs if self.langs is None or l in self.langs]
        self.header = ["English", "Description"] + langs
        self.rows = [[r[0], r[1] if len(r) > 1 else ""] + [""] * len(langs) for r in terms]
        index = {r[0]: r for r in self.rows}
        for col, lang in enumerate(langs, start=2):
            for line in (self._read(f"{lang}.tsv", required=False) or [])[1:]:
                cells = line.split("	")
                # "—" marks a language with no established rendering yet
                if len(cells) > 2 and cells[0] in index and cells[2].strip() not in MISSING:
                    index[cells[0]][col] = cells[2].strip()
        self.matchers = []
        for row in self.rows:
            base = re.sub(r"\s*\([^)]*\)\s*$", "", row[0]).strip()
            alts = [a.strip() for a in base.split(" / ") if a.strip()]
            pats = [re.compile(r"(?<![\w-])" + r"[\s-]+".join(map(re.escape, a.split())) + r"(?:s|es)?(?![\w-])", re.I) for a in alts]
            self.matchers.append((row, pats))

    def column(self, lang):
        return self.header.index(lang) if lang in self.header else None

    def matches(self, text):
        """Glossary rows found in text, without terms only seen inside a longer match."""
        spans = []
        for row, pats in self.matchers:
            for p in pats:
                spans += [(m.start(), m.end(), row) for m in p.finditer(text)]
        keep = []
        for row in {id(r): r for _, _, r in spans}.values():
            mine = [(s, e) for s, e, r in spans if r is row]
            if any(not any(s2 <= s and e <= e2 and (e2 - s2) > (e - s) for s2, e2, r2 in spans if r2 is not row) for s, e in mine):
                keep.append(row)
        return keep

    def hints(self, text, lang):
        col = self.column(lang)
        return [{"term": r[0], "translation": (r[col] if col is not None else "") or None, "description": r[1]} for r in self.matches(text)]

    def language_notes(self, lang):
        text = self.md.read_text(encoding="utf-8-sig")
        m = re.search(r"^### [^\n]*\(" + re.escape(lang) + r"\)\s*$(.*?)(?=^##|\Z)", text, re.M | re.S)
        return m.group(0).strip() if m else None


def _norm(s):
    s = unicodedata.normalize("NFD", s.casefold())
    return "".join(c for c in s if unicodedata.category(c) != "Mn")


def glossary_ok(msgstr, translation):
    """Loose check that a glossary rendering appears in msgstr, tolerating inflection."""
    target = _norm(msgstr)
    for alt in re.split(r"\s*/\s*", re.sub(r"\([^)]*\)|⚠", "", translation)):
        words = [w for w in re.findall(r"\w+", _norm(alt)) if len(w) > 2] or re.findall(r"\w+", _norm(alt))
        if not words:
            return True
        if all(w in target or (len(w) > 4 and w[:max(4, len(w) * 2 // 3)] in target) for w in words):
            return True
    return False


# ---------------------------------------------------------------------------
# Validation

C_FMT = re.compile(r"%(?:(\d+)\$)?[-+ #0']*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|L|q|j|z|Z|t)?[diouxXeEfFgGaAcspn]")
BOOST_FMT = re.compile(r"%\d+%")
TAG = re.compile(r"</?[A-Za-z][^<>]*>")
SNAKE = re.compile(r"[a-z][a-z0-9]*(?:_[a-z0-9]+)+")
IDENT = re.compile(r"\b[a-z][a-z0-9]*(?:_[a-z0-9]+)+\b|\b[GM]\d{1,4}\b|\{[A-Za-z_][\w.\[\]]*\}|\[[a-z_]+\]")
END = re.compile(r"[.:?!。：？！]$")


def _placeholders(s, flags):
    s = s.replace("%%", "")
    boost = BOOST_FMT.findall(s)
    s2 = BOOST_FMT.sub("", s) if "boost-format" in flags or boost else s
    c = [m.group(0) for m in C_FMT.finditer(s2)] if "no-c-format" not in flags else []
    return boost, c


def validate(e, msgstrs, nplurals, glossary=None, lang=None):
    """Return (errors, warnings) for a proposed translation of entry e."""
    errors, warnings = [], []
    flags = set(e.flags)
    if e.msgid_plural is not None and len(msgstrs) != nplurals:
        errors.append(f"plural entry needs {nplurals} forms, got {len(msgstrs)}")
    for i, s in enumerate(msgstrs):
        src = e.msgid if (e.msgid_plural is None or (i == 0 and nplurals > 1)) else e.msgid_plural
        tag = f"[{i}] " if e.msgid_plural is not None else ""
        if not s:
            errors.append(f"{tag}empty translation")
            continue
        b1, c1 = _placeholders(src, flags)
        b2, c2 = _placeholders(s, flags)
        positional = any("$" in p for p in c1)
        c_bad = set(c1) != set(c2) if positional else c1 != c2
        bad = []
        if set(b1) != set(b2):
            bad.append(f"boost placeholders {b1} -> {b2}")
        if c_bad:
            bad.append(f"printf placeholders {c1} -> {c2}" + ("" if positional else " (order must match)"))
        if bad:
            strict = flags & {"c-format", "boost-format"} and not (e.msgid_plural is not None and i == 0 and nplurals > 1 and not c2 and not b2)
            (errors if strict else warnings).extend(tag + b for b in bad)
        if s.count("%%") != src.count("%%") and "c-format" in flags:
            errors.append(f"{tag}'%%' count differs")
        if src.count("\n") != s.count("\n"):
            errors.append(f"{tag}line breaks: {src.count(chr(10))} in source, {s.count(chr(10))} in translation")
        else:
            lead = lambda t: len(t) - len(t.lstrip("\n"))
            trail = lambda t: len(t) - len(t.rstrip("\n"))
            if lead(src) != lead(s) or trail(src) != trail(s):
                errors.append(f"{tag}leading/trailing line breaks differ")
        ls = lambda t: re.match(r"[ \t]*", t.strip("\n")).group(0)
        ts = lambda t: re.search(r"[ \t]*$", t.strip("\n")).group(0)
        if ls(src) != ls(s) or ts(src) != ts(s):
            errors.append(f"{tag}leading/trailing spaces differ")
        if sorted(TAG.findall(src)) != sorted(TAG.findall(s)):
            errors.append(f"{tag}HTML tags {TAG.findall(src)} -> {TAG.findall(s)}")
        if src.count("℃") != s.count("℃"):
            errors.append(f"{tag}'℃' count differs (keep U+2103, not °C)")
        missing = [t for t in set(IDENT.findall(src)) if t not in s]
        tokens = [t for t in missing if not SNAKE.fullmatch(t)]
        if tokens:
            errors.append(f"{tag}tokens must stay verbatim: {tokens}")
        if len(tokens) < len(missing):  # snake_case is usually a setting key, sometimes a label
            warnings.append(f"{tag}snake_case kept? {[t for t in missing if SNAKE.fullmatch(t)]}")
        e1, e2 = END.search(src.rstrip()), END.search(s.rstrip())
        if bool(e1) != bool(e2) and lang != "th":  # Thai has no sentence-final period
            warnings.append(f"{tag}ending punctuation: source {'ends with ' + repr(e1.group(0)) if e1 else 'has none'}, translation {'ends with ' + repr(e2.group(0)) if e2 else 'has none'}")
        if s == src and len(src.split()) >= 3:
            warnings.append(f"{tag}identical to the English")
        if glossary and lang:
            # Rows sharing a base term ("Seam", "Seam (UV)") are competing senses; one is enough.
            # A match on qualified rows only ("Feature (Design)") may be another sense entirely.
            for base, senses in group_senses(glossary.hints(src, lang)).items():
                if all(h["term"] != base for h in senses) or not strong_match(src, base):
                    continue
                if not any(glossary_ok(s, h["translation"]) for h in senses):
                    wanted = " or ".join(f"'{h['translation']}' ({h['term']})" for h in senses)
                    warnings.append(f"{tag}glossary: expected {wanted}")
    return errors, warnings


def strong_match(src, base):
    """Whether a term is surely meant as the glossary term, not as everyday English
    ("each part of a frame"): a multi-word term, a short label, or capitalised mid-sentence."""
    if any(" " in alt for alt in base.split(" / ")) or len(src.split()) <= 5:
        return True
    for alt in base.split(" / "):
        for m in re.finditer(r"(?<![\w-])" + re.escape(alt) + r"(?:s|es)?(?![\w-])", src, re.I):
            if m.group(0)[0].isupper() and not re.search(r"(^|[.!?:]\s+|\n)$", src[:m.start()]):
                return True
    return False


def base_term(term):
    return re.sub(r"\s*\([^)]*\)\s*$", "", term).strip()


def group_senses(hints):
    groups = {}
    for h in hints:
        if h["translation"]:
            groups.setdefault(base_term(h["term"]), []).append(h)
    return groups


# ---------------------------------------------------------------------------
# Commands

def out_json(obj, path):
    text = json.dumps(obj, ensure_ascii=False, indent=1)
    if path:
        Path(path).write_text(text + "\n", encoding="utf-8")
        print(f"wrote {path}")
    else:
        sys.stdout.write(text + "\n")


def cmd_status(a):
    print(f"{'lang':7} {'active':>7} {'empty':>6} {'plural':>6} {'fuzzy':>6} {'AI':>6} {'review':>6}")
    for lang in a.lang or all_langs():
        c = Catalog(catalog_path(lang))
        act = c.active()
        empty = sum(1 for e in act if needs_translation(e, c.nplurals) and e.msgid_plural is None)
        plural = sum(1 for e in act if needs_translation(e, c.nplurals) and e.msgid_plural is not None)
        print(f"{lang:7} {len(act):7} {empty:6} {plural:6} {sum(e.fuzzy for e in act):6} {sum(e.ai for e in act):6} {sum(bool(e.review) for e in act):6}")


def similar_entries(c, e, pool, cutoff=0.72):
    """The closest earlier translation of a similar English text: obsolete entries and translated
    active ones. A reworded or split string usually still has one, and reusing it keeps the wording."""
    import difflib
    sm = difflib.SequenceMatcher(autojunk=False)
    sm.set_seq2(e.msgid)
    best = []
    for other, msgstr, obsolete, human in pool:
        sm.set_seq1(other)
        if sm.real_quick_ratio() < cutoff or sm.quick_ratio() < cutoff:
            continue
        r = sm.ratio()
        if r >= cutoff and other != e.msgid:
            best.append((r, other, msgstr, obsolete, human))
    best.sort(key=lambda t: -t[0])
    return [dict({"msgid": o, "msgstr": s, "obsolete": ob, "similarity": round(r, 2)}, **({"human": True} if h else {}))
            for r, o, s, ob, h in best[:2]]


def similar_pool(c):
    return [(x.msgid, x.msgstrs()[0], x.obsolete, not x.ai) for x in c.entries
            if not x.is_header and x.msgstrs() and x.msgstrs()[0] and not x.fuzzy and 8 < len(x.msgid) < 600]


def human_drift(e, strs, pool):
    """Why a translation strays from a person's text more than the English change explains, or None."""
    import difflib
    alike = lambda x, y: difflib.SequenceMatcher(None, x, y, autojunk=False).ratio()
    new, old = "\n".join(strs), "\n".join(e.msgstrs())
    if old.strip() and not e.ai:
        r = alike(old, new)
        if r < 0.85:
            return (f"replaces a person's translation with text only {r:.0%} like it: keep their words and change "
                    f"only what the fix needs, or give \"rewrite\": \"<reason>\"")
        return None
    if old.strip() or len(e.msgid) <= 8:
        return None
    for s in similar_entries(None, e, pool, cutoff=0.85):
        if s.get("human"):
            r = alike(s["msgstr"], new)
            if r < s["similarity"] - 0.1:
                return (f"the English is {s['similarity']:.0%} like a string a person translated, but this translation is "
                        f"only {r:.0%} like theirs: patch their translation, or give \"rewrite\": \"<reason>\"")
            break
    return None


def cmd_pending(a):
    c = Catalog(catalog_path(a.lang))
    g = Glossary(a.wiki, langs=[a.lang])
    todo = [e for e in c.active() if needs_translation(e, c.nplurals, a.include_fuzzy)]
    batch = todo[a.offset:a.offset + a.limit] if a.limit else todo[a.offset:]
    pool = similar_pool(c)
    items = []
    for e in batch:
        item = {"id": e.id(), "msgctxt": e.msgctxt, "msgid": e.msgid}
        if e.msgid_plural is not None:
            item["msgid_plural"] = e.msgid_plural
            item["msgstr_plural"] = (e.msgstrs() + [""] * c.nplurals)[:c.nplurals]
        else:
            item["msgstr"] = e.msgstrs()[0]
        if e.xcomments:
            item["note"] = " ".join(e.xcomments)
        flags = [f for f in e.flags if f != "fuzzy"]
        if flags:
            item["flags"] = flags
        if e.fuzzy:
            item["fuzzy"] = True
            if e.msgstrs()[0]:
                item["previous_translation"] = e.msgstrs()[0]
        if len(e.msgid) > 8:
            sim = similar_entries(c, e, pool)
            if sim:
                item["similar"] = sim
        hints = g.hints(e.msgid + "\n" + (e.msgid_plural or ""), a.lang)
        if hints:
            # Descriptions only where the sense must be chosen or no rendering exists yet.
            bases = [base_term(h["term"]) for h in hints]
            item["glossary"] = [{k: v for k, v in h.items() if k != "description" or not h["translation"] or bases.count(b) > 1 or b != h["term"]}
                                for h, b in zip(hints, bases)]
        items.append({k: v for k, v in item.items() if v is not None})
    out_json({"lang": a.lang, "nplurals": c.nplurals, "plural_forms": re.search(r"Plural-Forms: ([^\n]*)", c.header.msgstrs()[0]).group(1) if c.header else None,
              "total_pending": len(todo), "offset": a.offset, "count": len(items), "entries": items}, a.out)


def cmd_apply(a):
    c = Catalog(catalog_path(a.lang))
    g = None if a.no_glossary else Glossary(a.wiki, quiet=True, langs=[a.lang])
    data = json.loads(Path(a.input).read_text(encoding="utf-8"))
    items = data["entries"] if isinstance(data, dict) else data
    idx = c.index()
    by_id = {c.entries[i].id(): i for i in idx.values()}
    pool = similar_pool(c)
    applied, skipped, rejected, warned = 0, 0, [], []
    for it in items:
        if "msgid" in it:
            key = (it.get("msgctxt"), it["msgid"])
            i = idx.get(key)
            label = (f"[{key[0]}] " if key[0] else "") + key[1][:70].replace("\n", "\\n")
        else:
            i = by_id.get(it.get("id"))
            label = f"id {it.get('id')}" if i is None else c.entries[i].msgid[:70].replace("\n", "\\n")
        if i is None:
            rejected.append((label, ["no such entry in the catalog (give its id, or msgid and msgctxt)"]))
            continue
        e = c.entries[i]
        if e.msgid_plural is not None and "msgstr_plural" not in it:
            rejected.append((label, [f"plural entry: give msgstr_plural with {c.nplurals} forms"]))
            continue
        strs = it["msgstr_plural"] if e.msgid_plural is not None else [it.get("msgstr", "")]
        if not strs or all(not s for s in strs):
            skipped += 1
            continue
        if not a.overwrite and not needs_translation(e, c.nplurals, include_fuzzy=True):
            rejected.append((label, ["already translated (pass --overwrite to replace)"]))
            continue
        errors, warnings = validate(e, strs, c.nplurals, g, a.lang)
        if errors:
            rejected.append((label, errors))
            continue
        drift = None if it.get("rewrite") else human_drift(e, strs, pool)
        if drift:
            rejected.append((label, [drift]))
            continue
        if warnings:
            warned.append((label, warnings))
        review = it.get("review")
        if it.get("rewrite") and not review:
            review = "Rewrote a person's translation: " + it["rewrite"]
        c.changes[i] = e.rewrite(strs, ai=not a.human, review=review)
        applied += 1
    if not a.dry_run and c.changes:
        c.save()
    for label, msgs in rejected:
        print(f"REJECTED {label}\n    " + "\n    ".join(msgs))
    for label, msgs in warned:
        print(f"WARNING  {label}\n    " + "\n    ".join(msgs))
    print(f"{'would apply' if a.dry_run else 'applied'} {applied}, rejected {len(rejected)}, warnings {len(warned)}, blank skipped {skipped}")
    return 1 if rejected else 0


def find_msgfmt():
    exe = REPO / "tools" / "msgfmt.exe"
    if os.name == "nt" and exe.exists():
        return str(exe)
    from shutil import which
    return which("msgfmt")


def cmd_check(a):
    g = None if a.no_glossary else Glossary(a.wiki, quiet=True, langs=a.lang or None)
    msgfmt = find_msgfmt()
    status = 0
    for lang in a.lang or all_langs():
        c = Catalog(catalog_path(lang))
        errs = warns = 0
        for e in c.active():
            if a.scope == "ai" and not e.ai:
                continue
            strs = e.msgstrs()
            if all(s == "" for s in strs) or e.fuzzy:
                continue
            errors, warnings = validate(e, strs, c.nplurals, g, lang)
            if errors or (warnings and a.warnings):
                label = (f"[{e.msgctxt}] " if e.msgctxt else "") + e.msgid[:70].replace("\n", "\\n")
                print(f"{lang} line {e.first_line + 1}: {label}")
                for m in errors:
                    print(f"    ERROR {m}")
                for m in warnings if a.warnings else []:
                    print(f"    warn  {m}")
            errs += bool(errors)
            warns += bool(warnings)
        if msgfmt:
            fd, mo = tempfile.mkstemp(suffix=".mo")  # Windows msgfmt cannot write to "nul"
            os.close(fd)
            r = subprocess.run([msgfmt, "--check-format", "--check-header", "-o", mo, str(c.path)], capture_output=True, text=True)
            os.unlink(mo)
            if r.returncode:
                print(f"{lang}: msgfmt --check-format failed\n{r.stderr.strip()}")
                status = 1
        print(f"{lang}: {errs} entries with errors, {warns} with warnings (scope: {a.scope})")
        status |= bool(errs)
    if not msgfmt:
        print("note: msgfmt not found; skipped --check-format", file=sys.stderr)
    return status


def cmd_lookup(a):
    c = Catalog(catalog_path(a.lang))
    rx = re.compile(a.pattern, re.I)
    n = 0
    for e in c.entries:
        strs = [s for s in e.msgstrs() if s]
        if e.is_header or not strs:
            continue
        texts = strs if a.msgstr else [e.msgid, e.msgid_plural or ""]
        if not any(rx.search(t) for t in texts):
            continue
        mark = "obsolete " if e.obsolete else f"{e.id()} " + ("AI " if e.ai else "")
        ctx = f"[{e.msgctxt}] " if e.msgctxt else ""
        print(f"{mark}{ctx}{json.dumps(e.msgid, ensure_ascii=False)}\n    -> {json.dumps(strs[0] if len(strs) == 1 else strs, ensure_ascii=False)}")
        n += 1
        if n >= a.limit:
            print("... (limit reached)")
            break


def cmd_glossary(a):
    g = Glossary(a.wiki, langs=[a.lang])
    col = g.column(a.lang)
    if col is None:
        print(f"'{a.lang}' has no glossary column yet; add one with glossary-set --set {a.lang}=...")
    rx = re.compile(a.term, re.I) if a.term else None
    for r in g.rows:
        if rx and not rx.search(r[0]):
            continue
        val = r[col] if col is not None and r[col] else "—"
        print(f"{r[0]}  =>  {val}\n    {r[1]}")
    notes = g.language_notes(a.lang)
    if notes and not a.term:
        print("\n" + notes)


STOP = set("""a an and are as at be been but by can could do does for from has have how if in into is it its
may more most must no not of on once one only or other out over same set should so some such than that the
their them then there these this those to too two up use used uses using very was when where whether which
while will with without would you your yet all any each every per both also between before after first last
off new now get gets let lets just already instead still even back down here make makes made via""".split())


def _singular(key):
    return " ".join(re.sub(r"(?<=[a-z]{3})(es|s)$", "", w) for w in key.split())


def kept_in_english(g):
    """Names from the glossary page's "Kept in English" table."""
    text = g.md.read_text(encoding="utf-8-sig")
    m = re.search(r"^## Kept in English.*?$(.*?)(?=^## )", text, re.M | re.S)
    names = set()
    for row in re.findall(r"^\| (.*?) \|", m.group(1) if m else "", re.M):
        for piece in re.split(r"[,/:]|\bplus\b", row.replace("**", "").replace("`", "")):
            if piece.strip():
                names.add(_norm(piece.strip()))
    return names


def cmd_candidates(a):
    g = Glossary(a.wiki)
    known = kept_in_english(g)
    for row, _ in g.matchers:
        known |= {_norm(x.strip()) for x in base_term(row[0]).split(" / ")}
    known |= {_singular(k) for k in known}
    known |= {w for k in known if " " in k for w in k.split()}  # "tower" of "prime tower"
    pending = {}
    labels = set()
    for lang in all_langs():
        c = Catalog(catalog_path(lang))
        for e in c.active():
            labels.add(_norm(e.msgid))
            if needs_translation(e, c.nplurals):
                pending.setdefault(e.key(), set()).add(lang)
    counts = {}  # phrase -> [strings, max langs, display, example, capitalised mid-sentence]
    for (ctx, msgid), langs in pending.items():
        seen = set()
        for chunk in re.split(r"[.,;:!?()\[\]\"\n—–]+|\s-\s", msgid):
            words = [re.sub(r"'s$", "", w) for w in re.findall(r"[A-Za-z][A-Za-z0-9-]*(?:'s)?", chunk)]
            # Capitals only mean a name inside an ordinary sentence, not in a Title Case label.
            sentence = len(words) >= 4 and sum(w[0].isupper() for w in words) * 2 < len(words)
            for n in (1, 2, 3):
                for i in range(len(words) - n + 1):
                    gram = words[i:i + n]
                    if gram[0].lower() in STOP or gram[-1].lower() in STOP or len("".join(gram)) < 4 \
                            or any(w.lower() in STOP and w.lower() != "of" for w in gram):
                        continue
                    key = _norm(" ".join(gram))
                    if key in seen or key in known or _singular(key) in known:
                        continue
                    seen.add(key)
                    d = counts.setdefault(key, [0, 0, " ".join(gram), msgid, False])
                    d[0] += 1
                    d[1] = max(d[1], len(langs))
                    d[4] |= sentence and i > 0 and all(w[0].isupper() for w in gram)
    # A term is a phrase, or a word capitalised mid-sentence or written as an acronym.
    keep = {k: v for k, v in counts.items()
            if v[0] >= a.min_count and (" " in k or v[4] or v[2].isupper())}
    # Drop fragments only ever seen inside a longer candidate ("IQEX parallel" in "IDEX IQEX parallel").
    keep = {k: v for k, v in keep.items()
            if not any(k != k2 and re.search(r"(^| )" + re.escape(k) + r"( |$)", k2) and v2[0] >= v[0] for k2, v2 in keep.items())}
    rows = sorted(keep.items(), key=lambda kv: (-kv[1][0], -len(kv[0].split())))
    print("strings  langs  phrase  [label = also a msgid of its own]  e.g.")
    for k, (cnt, langs, text, example, _) in rows[:a.limit]:
        ex = example.replace("\n", " ")
        print(f"{cnt:7} {langs:6}  {text}{'  [label]' if k in labels else ''}  e.g. {ex[:70]}{'…' if len(ex) > 70 else ''}")


def _edit_tsv(path, edit):
    """Apply edit(lines) to a glossary TSV, keeping its BOM and line endings."""
    data = path.read_bytes()
    bom = data.startswith(b"\xef\xbb\xbf")
    text = data.decode("utf-8-sig")
    nl = "\r\n" if "\r\n" in text else "\n"
    lines = text.splitlines()
    edit(lines)
    path.write_bytes((b"\xef\xbb\xbf" if bom else b"") + (nl.join(lines) + nl).encode("utf-8"))


def cmd_glossary_set(a):
    wiki = find_wiki(a.wiki)
    if not wiki:
        print("No OrcaSlicer_WIKI clone found (looked at --wiki, $ORCA_WIKI_DIR and next to this repository).\n"
              "The glossary must be updated with these terms before the translations that use them ship:\n"
              f"  {a.term}: " + ", ".join(a.set) + "\n"
              "Clone https://github.com/OrcaSlicer/OrcaSlicer_WIKI next to this repository, or pass --wiki.", file=sys.stderr)
        return 2
    gdir = wiki / GLOSSARY_DIR
    gen = wiki / "generate_glossary.py"
    if not gen.exists():
        sys.exit(f"{gen} not found")
    run_gen = lambda *args: subprocess.run([sys.executable, str(gen), *args], cwd=wiki, capture_output=True, text=True)
    langs = [l.strip() for l in (gdir / "languages.txt").read_text(encoding="utf-8-sig").splitlines() if l.strip()]
    values = {}
    for s in a.set:
        lang, _, val = s.partition("=")
        if lang not in langs:
            sys.exit(f"no glossary language '{lang}' (add it to {GLOSSARY_DIR.as_posix()}/languages.txt)")
        if "\t" in val or "\n" in val:
            sys.exit("values cannot contain tabs or line breaks")
        values[lang] = val.strip()
    lock = gdir / ".lock"
    for _ in range(100):
        try:
            fd = os.open(lock, os.O_CREAT | os.O_EXCL)
            break
        except FileExistsError:
            time.sleep(0.1)
    else:
        sys.exit(f"could not lock {lock}; remove it if no other run is active")
    try:
        terms = (gdir / "terms.tsv").read_text(encoding="utf-8-sig").splitlines()
        new = not any(l.split("\t")[0] == a.term for l in terms[1:])
        if new and not a.description:
            sys.exit("a new term needs --description (one sentence saying what it is)")
        # an established translation is only replaced on request
        conflicts = []
        for lang, v in values.items():
            for line in (gdir / f"{lang}.tsv").read_text(encoding="utf-8-sig").splitlines()[1:]:
                cells = line.split("\t")
                if cells[0] == a.term and len(cells) > 2 and cells[2].strip() not in MISSING and cells[2] != v:
                    conflicts.append(f"{lang}: '{cells[2]}' -> '{v}'")
        if conflicts and not a.replace:
            sys.exit("established translations differ (pass --replace, and update the catalogs to match):\n  " + "\n  ".join(conflicts))

        def edit_terms(lines):
            for i, l in enumerate(lines):
                if i and l.split("\t")[0] == a.term:
                    if a.description:
                        lines[i] = f"{a.term}\t{a.description}"
                    return
            lines.append(f"{a.term}\t{a.description}")
        if new or a.description:
            _edit_tsv(gdir / "terms.tsv", edit_terms)
            r = run_gen("--sync")  # copies the term and description into every language file
            if r.returncode:
                sys.exit((r.stdout + r.stderr).strip())

        def setter(lang, v):
            def edit(lines):
                for i, l in enumerate(lines):
                    cells = l.split("\t")
                    if i and cells[0] == a.term:
                        lines[i] = "\t".join(cells[:2] + [v])
                        return
                sys.exit(f"{lang}.tsv has no row for '{a.term}'")
            return edit
        for lang, v in values.items():
            _edit_tsv(gdir / f"{lang}.tsv", setter(lang, v))
    finally:
        os.close(fd)
        os.unlink(lock)
    print(f"glossary: {'added' if new else 'updated'} '{a.term}'")
    r = run_gen("--update")
    print((r.stdout + r.stderr).strip())
    return r.returncode


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--wiki", help="path to an OrcaSlicer_WIKI clone")
    p.add_argument("--i18n", help="catalog root to use instead of localization/i18n")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("status")
    s.add_argument("--lang", action="append")
    s.set_defaults(func=cmd_status)

    s = sub.add_parser("pending")
    s.add_argument("--lang", required=True)
    s.add_argument("--offset", type=int, default=0)
    s.add_argument("--limit", type=int, default=60)
    s.add_argument("--out")
    s.add_argument("--include-fuzzy", action=argparse.BooleanOptionalAction, default=True,
                   help="list fuzzy entries too (default): users never see them")
    s.set_defaults(func=cmd_pending)

    s = sub.add_parser("apply")
    s.add_argument("--lang", required=True)
    s.add_argument("--input", required=True)
    s.add_argument("--overwrite", action="store_true", help="also replace existing translations")
    s.add_argument("--human", action="store_true", help="do not add '# AI Translated' (a person wrote these)")
    s.add_argument("--dry-run", action="store_true")
    s.add_argument("--no-glossary", action="store_true")
    s.set_defaults(func=cmd_apply)

    s = sub.add_parser("check")
    s.add_argument("--lang", action="append")
    s.add_argument("--scope", choices=["ai", "all"], default="ai")
    s.add_argument("--warnings", action="store_true", help="also print warnings")
    s.add_argument("--no-glossary", action="store_true")
    s.set_defaults(func=cmd_check)

    s = sub.add_parser("lookup")
    s.add_argument("--lang", required=True)
    s.add_argument("pattern")
    s.add_argument("--msgstr", action="store_true", help="search the translations instead of the English")
    s.add_argument("--limit", type=int, default=40)
    s.set_defaults(func=cmd_lookup)

    s = sub.add_parser("glossary")
    s.add_argument("--lang", required=True)
    s.add_argument("term", nargs="?")
    s.set_defaults(func=cmd_glossary)

    s = sub.add_parser("candidates")
    s.add_argument("--min-count", type=int, default=3)
    s.add_argument("--limit", type=int, default=80)
    s.set_defaults(func=cmd_candidates)

    s = sub.add_parser("glossary-set")
    s.add_argument("--term", required=True)
    s.add_argument("--description")
    s.add_argument("--set", action="append", default=[], metavar="LANG=VALUE")
    s.add_argument("--replace", action="store_true")
    s.set_defaults(func=cmd_glossary_set)

    a = p.parse_args()
    if a.i18n:
        global I18N
        I18N = Path(a.i18n)
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    sys.exit(a.func(a) or 0)


if __name__ == "__main__":
    main()
