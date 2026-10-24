# Translation rules

The rules a translation must follow, condensed from the wiki's
[Localization guide](https://github.com/OrcaSlicer/OrcaSlicer_WIKI/blob/main/developer_reference/localization_guide.md),
the [glossary](https://github.com/OrcaSlicer/OrcaSlicer_WIKI/blob/main/developer_reference/localization_glossary.md)
and the localization section of `AGENTS.md`. `po_tool.py apply` rejects the mechanical violations it
can detect; everything else is on the translator.

## Mechanical: enforced by `apply`

A rejected entry is not written. Fix it and apply again.

- **Placeholders stay exactly as written:** `%s`, `%d`, `%.1f`, `%1$d`, `%1%`, `%%`. Never `%1 %`, never
  `%` for `%%`. Plain printf placeholders keep their order; only positional ones (`%1$s`, `%1%`) may move,
  and one the msgid repeats may appear just once.
  A broken placeholder crashes the app.
- **Line breaks:** the same number of `\n`, with the same leading and trailing ones. Don't add breaks to fit
  text, least of all in parameter names.
- **Leading and trailing spaces** are kept. A leading space usually means the string is appended to another.
- **HTML tags** are kept, and the text between them is translated: `Click <a>here</a> to install it.`
- **`℃`** (U+2103) stays one character, written exactly as in the source: `%d℃`, never `°C`, `° C` or
  `%d ℃`.
- **Tokens stay verbatim:** `{placeholders}`, `[macros]`, G-code commands (`M141`, `G28`). Setting keys
  in snake_case (`use_relative_e_distances`) are flagged as warnings, because a few snake_case msgids
  are really labels (`top_front`). Keep a key unless the string uses it as a display name.
- **Plural entries** need every form from the catalog's `Plural-Forms` header (`nplurals`: 1 for
  ja/ko/zh/th/vi, 3 for ru/cs/pl/lt, 4 for uk). Each form is really inflected for its quantity. Repeating
  one sentence is a bug in Slavic and Baltic languages, but correct in Turkish and Hungarian.

## A person's translation: patch it, don't rewrite it

A translator's wording is kept unless it is wrong. Machine output never replaces it wholesale.

- **A reworded string:** `pending` lists earlier translations of similar English under `similar`, and
  marks a person's work with `"human": true`. When the English barely changed, start from that
  translation and change only what the English change requires: a renamed term, an added clause, a
  period. Keep the translator's wording, word order, terms and line structure. If they already wrote what
  the new English says, the translation does not change at all.
- **A fix in a person's translation:** for a check error (a line break, a space, placeholder order, a
  missing clause), change only what the error needs. Keep the rest of their words, even where they differ
  from the glossary or from how you would phrase it. Report such glossary differences instead.
- **Rewrite only what is plainly wrong:** another string's text, a cut-off sentence, a mistranslation.
  Say why with `"rewrite": "<what was wrong>"` in the item; the reason becomes the review note.
- `apply` enforces this. It rejects text that strays further from a person's translation than the
  English change explains (below 85% alike for a fix, or clearly less alike than the English for a
  reworded string), unless the item carries `rewrite`.
- Apply a fix that keeps the person's words with `--human`, so the entry stays theirs. Text you wrote
  gets the default `# AI Translated`.

## Mechanical: not detectable, still required

- Edit only `msgstr`. Never change `msgid`, `msgctxt`, the header or another entry's text.
- **Ending punctuation follows the source:** no added period, and no dropped one. `apply` warns.
- Units follow the SI convention of the language: `s`, not `sec`. A parameter description holds no
  unit, because the field shows it.
- **A button or short label holds one translation:** no alternatives in parentheses. They widen the
  control.
- Don't use `fuzzy` for uncertainty. Fuzzy entries are hidden, and users then see English.

## Choosing the words

- **The glossary is binding.** Use the language's rendering for every glossary term the `pending`
  output lists, inflected as the sentence needs.
    - **Rows that share a base term are different senses:** `Seam` (the print seam) versus `Seam (UV)`
      (the unwrap cut), and `Instance` versus `Instance (application)`. Pick by the description.
    - **A qualified row may be a false match.** `Feature (Design)` does not apply to "features of the
      app".
    - **A cell marked ⚠ (or holding two renderings) is unsettled.** Use the form the catalog already
      uses for that sense (`lookup`), and flag the entry for review.
- **Kept in English:** brand and product names (OrcaSlicer, Bambu Lab, Orca Cloud, AMS, Micro-Lidar),
  acronyms (FDM, LAN, HTTPS), file formats, material codes (PLA, PETG-CF), G-code, macros and variables
  stay as written. In "Adaptive Pressure Advance", only "Pressure Advance" stays English. See the
  glossary page's *Kept in English* table.
- **Language specifics** on the glossary page (`po_tool.py glossary --lang L` prints them) override
  general habits for that language.
- **Consistency beyond the glossary:** before translating a label, or a tooltip that names a setting,
  run `lookup` for the setting name. Quote the label's existing translation exactly, so the tooltip and
  the field match. Reuse obsolete (`#~`) entries as a translation memory.
- **`msgctxt` separates homonyms.** `Back` in *Camera View* is the rear view, and `Back` in
  *Navigation* is the go-back button. Translate each context on its own.
- **Overloaded English: translate the meaning.**
    - *Flow ratio* is a multiplier, *Flow Rate* is throughput, and *Flow Dynamics* is pressure
      compensation. These are three different terms.
    - *Extruder* can mean the toolhead, the feeder motor ("High extruder current on filament swap") or
      the nozzle ("all extruders must have the same diameter").
    - When a short label is ambiguous, grep `src/` for the msgid to see the widget it labels.
- **Boilerplate stays uniform.** `Failed to connect to …`, `Are you sure you want to …?` and
  `If expressed as a %, it will be computed over …` each get one template in the language, even where
  the English varies.

## Style

- **Localize, don't translate word-for-word.** Know what the string controls before translating it.
- **Simplify developer phrasing** when the meaning survives. *"This option determines whether
  OrcaSlicer should apply the retract reduction on infill"* reads better as *"Disable retractions while
  printing infill."* Introduce nuances in following sentences, not in one packed clause.
- **Don't expand.** A machine translation adds no facts the English doesn't state. Expanding
  descriptions is for human translators, who can confirm the extra content.
- **Write for both audiences:** professionals and people with a printer at home. Keep the established
  terminology, used as plainly as the string allows.

## When to add a review note

Add `"review": "<what to check>"` to an item when the translation is probable but unconfirmed. It
becomes `# Needs human review: …`, and the entry still ships. Typical reasons:

- The English is ambiguous out of context: "Tile" as a verb or a noun.
- A short label's role only shows in the app.
- Two established terms compete, or the glossary cell is unsettled.
- The string's grammar depends on a value substituted at runtime: `%zu %s` counts with no plural entry.

Say exactly what to look at and where: *"Row name in the Design tab tool-offer menu; check it reads
right for the whole row."*

## Problems that belong in the source, not the translation

Report these; don't work around them in a catalog.

- **The English is wrong.** Never "fix" it in the translation only.
- **One msgid needs two translations.** The source needs `_L_CONTEXT` / `_u8L_CONTEXT` (a code change).
- **A literal `%` in a string xgettext flagged `possible-c-format`** fails `msgfmt`. The fix is a
  `// xgettext:no-c-format, no-boost-format` comment above the string in the source, not `%%` in the
  translation.
