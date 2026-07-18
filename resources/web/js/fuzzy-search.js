// Shared fuzzy-search core for the webview dialogs (Plugins dialog, Speed Dial popup).
// why: both pages carried their own copy of this matcher and had already drifted; one source of truth.
// note: keep this DOM-free and plain global-scope (no export/module) - it is loaded by <script src> in
//       each page AND by a node vm.runInContext in the speed-dial logic test. It MUST be loaded before
//       the page script that calls it.

// Fold per-character so matched offsets stay in ORIGINAL string coordinates (highlighting slices the
// original text; a separately-folded string would desync offsets).
function FoldChar(ch) {
  return ch.normalize("NFD").replace(/\p{Diacritic}/gu, ""); // accents always folded
}

function Norm(ch, caseSensitive) {
  const folded = FoldChar(ch);
  return caseSensitive ? folded : folded.toLowerCase(); // case-sensitivity is the only toggle
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
