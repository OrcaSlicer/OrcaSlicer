// Unit test for the shared fuzzy matcher. Run: node resources/web/js/fuzzy-search.test.js
// why: fuzzy-search.js is plain global-scope (no exports, so a browser <script src> works) - load it into
//      a vm context the same way the page and the speed-dial test do, then assert against the globals.
const vm = require("vm"), assert = require("assert"), fs = require("fs");
const ctx = {};
vm.createContext(ctx);
vm.runInContext(fs.readFileSync(__dirname + "/fuzzy-search.js", "utf8"), ctx);
const { FoldChar, Norm, NormText, EscapeRegExp, FuzzyRanges, WholeWordRanges, FuzzyRangesNorm } = ctx;

// FoldChar / Norm: accents fold, case only folds when case-insensitive.
assert.equal(FoldChar("é"), "e");
assert.equal(Norm("É", false), "e");
assert.equal(Norm("É", true), "E");

// FuzzyRanges: ordered subsequence, ranges in ORIGINAL coordinates, adjacent runs merged.
assert.deepEqual(FuzzyRanges("Auto Arrange", "aa", false), [[0, 1], [5, 6]]);
assert.deepEqual(FuzzyRanges("Measure", "eas", false), [[1, 4]]); // contiguous run merges to one range
assert.equal(FuzzyRanges("Measure", "xyz", false), null);         // no subsequence -> null
assert.equal(FuzzyRanges("Measure", "", false), null);            // empty query -> null

// Accent-insensitive matching, offsets stay in the original (accented) string.
assert.deepEqual(FuzzyRanges("Café", "cafe", false), [[0, 4]]);

// Case sensitivity is the only toggle.
assert.equal(FuzzyRanges("Measure", "MEAS", true), null);         // case-sensitive: no match
assert.deepEqual(FuzzyRanges("Measure", "Meas", true), [[0, 4]]); // case-sensitive: matches exact case

// WholeWordRanges: \b-bounded literal, bypasses fuzzy. Substring inside a word does NOT match.
assert.deepEqual(WholeWordRanges("Auto Arrange", "arrange", false), [[5, 12]]);
assert.equal(WholeWordRanges("Rearrange", "arrange", false), null); // not on a word boundary
assert.deepEqual(WholeWordRanges("a.b", "a", false), [[0, 1]]);      // '.' is a boundary

// EscapeRegExp: regex metachars in the query are treated literally by whole-word.
assert.equal(EscapeRegExp("a.b*"), "a\\.b\\*");
assert.deepEqual(WholeWordRanges("c++ tool", "c", false), [[0, 1]]); // '+' would be a regex error unescaped

// FuzzyRangesNorm: prefers the most-contiguous (smallest-span) occurrence over greedy-leftmost, so a
// stray earlier character does not steal the highlight from a later tight word.
assert.deepEqual(FuzzyRangesNorm(NormText("Auto-Orient", false), NormText("orient", false)), [[5, 11]]);
// A contiguous run inside a larger word is also preferred over a scattered greedy pick.
assert.deepEqual(FuzzyRangesNorm(NormText("AutoOriented", false), NormText("orient", false)), [[4, 10]]);
assert.equal(FuzzyRangesNorm(NormText("Measure", false), NormText("xyz", false)), null); // no subsequence

// NormText stays 1:1 with the original even when NFD expands (Hangul syllables) or drops (combining
// diacritics, U+0130) a code unit, so highlight ranges remain aligned to the original string.
assert.equal(NormText("각x", false).length, "각x".length);
assert.equal(NormText("e\u0301x", false).length, "e\u0301x".length);
assert.equal(NormText("\u0130x", false).length, "\u0130x".length);
assert.equal(FuzzyRangesNorm(NormText("각abcdeXfgh", false), NormText("X", false))[0][0], 6);

console.log("ok");
