// Shared fuzzy-search core for the webview dialogs (Plugins dialog, Speed Dial popup).
// why: both pages carried their own copy of this matcher and had already drifted; one source of truth.
// note: keep this DOM-free and plain global-scope (no export/module) - it is loaded by <script src> in
//       each page AND by a node vm.runInContext in the speed-dial logic test. It MUST be loaded before
//       the page script that calls it.

// Fold per-character so matched offsets stay in ORIGINAL string coordinates (highlighting slices the
// original text; a separately-folded string would desync offsets).
function FoldChar(ch) {
  // why: fast path - ASCII is already NFD-stable and diacritic-free, so the normalize/regex below are
  //      no-ops. This skip is the hot cost in the Speed Dial search (thousands of settings per keystroke).
  if (ch.length === 1 && ch.charCodeAt(0) < 0x80)
    return ch;
  return ch.normalize("NFD").replace(/\p{Diacritic}/gu, ""); // accents always folded
}

function Norm(ch, caseSensitive) {
  const folded = FoldChar(ch);
  return caseSensitive ? folded : folded.toLowerCase(); // case-sensitivity is the only toggle
}

// Pre-normalize a whole haystack with a length-preserving fold, so a caller can match it repeatedly
// against one cached string. One input UTF-16 code unit always maps to one output code unit, so
// indices stay aligned to the ORIGINAL text - the highlight ranges that FuzzyRangesNorm returns slice
// the original correctly. Iterate by UTF-16 code unit (not Array.from code point) to mirror
// FuzzyRanges' own indexing exactly.
function NormText(text, caseSensitive) {
  const src = text || "";
  let out = "";
  for (let i = 0; i < src.length; i++)
    out += NormStable(src[i], caseSensitive);
  return out;
}

// Length-preserving variant of Norm: NFD can expand a code unit (Hangul syllables become Jamo) or
// drop it (combining diacritics), and toLowerCase can expand one too (U+0130). Any of those would
// desync highlight offsets, so fall back to the original unit whenever the fold is not 1:1.
function NormStable(ch, caseSensitive) {
  const folded = FoldChar(ch);
  const stable = folded.length === 1 ? folded : ch;
  if (caseSensitive)
    return stable;
  const lower = stable.toLowerCase();
  return lower.length === 1 ? lower : stable;
}

// Match a PRE-normalized haystack against a PRE-normalized needle (both produced by NormText with the
// same caseSensitive flag). Skipping the per-character fold makes repeated matching (per keystroke over a
// cached pool) cheap. Returns ranges in original coordinates, or null on no match.
// Prefers the most-contiguous (smallest-span) occurrence over greedy-leftmost: a scattered match that
// spans a stray earlier character is worse than a tight run later, so "orient" against a normalized
// "auto-orient" returns [[5,11]] (the word) not [[3,4],[6,11]]. A fully contiguous run is the optimum
// and short-circuits early.
function FuzzyRangesNorm(haystackNorm, needleNorm) {
  const t = haystackNorm || "";
  const needle = needleNorm || "";
  if (!needle)
    return null;
  const n = t.length, nl = needle.length;
  let best = null; // {ranges, span, start}
  for (let start = 0; start < n; start++) {
    if (t[start] !== needle[0])
      continue;
    let qi = 0;
    const ranges = [];
    let lastEnd = start;
    for (let i = start; i < n && qi < nl; i++) {
      if (t[i] === needle[qi]) {
        const last = ranges[ranges.length - 1];
        if (last && last[1] === i)
          last[1] = i + 1;
        else
          ranges.push([i, i + 1]);
        lastEnd = i + 1;
        qi++;
      }
    }
    if (qi !== nl)
      continue;
    const span = lastEnd - start;
    if (!best || span < best.span || (span === best.span && start < best.start)) {
      best = { ranges, span, start };
      if (span === nl)
        return ranges; // can't beat a fully contiguous run
    }
  }
  return best ? best.ranges : null;
}

function EscapeRegExp(value) {
  return value.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

// Fuzzy: ordered subsequence. Builds ranges in original coordinates, merging adjacent runs on the fly.
function FuzzyRanges(text, query, caseSensitive) {
  const t = text || "";
  const needle = Array.from(query || "").map((ch) => Norm(ch, caseSensitive)).join("");
  if (!needle)
    return null;
  const ranges = [];
  let qi = 0;
  for (let i = 0; i < t.length && qi < needle.length; i++) {
    if (Norm(t[i], caseSensitive) === needle[qi]) {
      const last = ranges[ranges.length - 1];
      if (last && last[1] === i)
        last[1] = i + 1;
      else
        ranges.push([i, i + 1]);
      qi++;
    }
  }
  return qi === needle.length ? ranges : null;
}

// Whole word: literal \b-bounded match that bypasses fuzzy. The per-char fold keeps the haystack
// length-aligned to the original text, so regex indices map straight back to original offsets.
// note: one-to-many folds (ligatures, eszett) shift offsets by a char; rare in names, cosmetic only.
function WholeWordRanges(text, query, caseSensitive) {
  const haystack = Array.from(text || "").map((ch) => Norm(ch, caseSensitive)).join("");
  const needle = Array.from(query || "").map((ch) => Norm(ch, caseSensitive)).join("");
  if (!needle)
    return null;
  const re = new RegExp(`\\b${EscapeRegExp(needle)}\\b`, "g");
  const ranges = [];
  let match;
  // why: needle is non-empty, so \b-bounded matches are never zero-length - no empty-match guard needed.
  while ((match = re.exec(haystack)) !== null)
    ranges.push([match.index, match.index + match[0].length]);
  return ranges.length > 0 ? ranges : null;
}
