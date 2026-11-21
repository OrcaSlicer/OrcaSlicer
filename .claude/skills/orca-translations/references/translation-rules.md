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

## Context: know what the string is before choosing its words

English reuses one word for different things, and a string seen alone often reads several ways. Settle
what each entry is before translating it. `pending` gives every entry the context the source holds:
`refs` (where it is used), `code` (the lines around the use, for short strings and placeholder
strings) and `nearby` (the translated short strings used next to it). `lookup --code` shows the same
for existing entries. The catalogs themselves carry no source references.

- **What kind of text it is,** from `code`: a button or menu item is a command, a field label or column
  header names a value, a checkbox states something that is on or off, and a tooltip, status line,
  error or dialog title each have their own register. Keep a label as short as a label.
- **What it belongs to.** A setting's strings sit together in `PrintConfig.cpp`: `this->add("<key>", …)`,
  then `label`, `category`, `tooltip`, `sidetext`. The tooltip describes that option, and quotes the
  label exactly as `nearby` shows it translated. The items of one menu, choice or toolbar
  (`enum_labels`, `Append`, an array of names) are a set: give them one grammatical form, and words that
  tell them apart.
- **`msgctxt` separates homonyms.** `Back` in *Camera View* is the rear view, and `Back` in
  *Navigation* is the go-back button. Translate each context on its own.
- **Overloaded English: translate the meaning.**
    - *Flow ratio* is a multiplier, *Flow Rate* is throughput, and *Flow Dynamics* is pressure
      compensation. These are three different terms.
    - *Extruder* can mean the toolhead, the feeder motor ("High extruder current on filament swap") or
      the nozzle ("all extruders must have the same diameter").
    - *Home* is the homing move ("Please home all axes"), the home page, or the Home key (*Keyboard
      Shortcut*), which takes the name printed on that language's keyboards. *Fill* is infill, gap fill,
      or the paint tools' bucket and smart fill. *Normal* is the ordinary kind ("Normal support",
      "Normal printing") or the surface normal ("Normal X", "Smooth normals"). *Model* is the 3D model,
      or the printer model in "Model:". *Process* is the process preset, or an operation under way ("the
      slicing process").
    - The glossary's rows record the senses that recur (`Plate` versus `Bed / Build plate`, `Feature
      (print)` versus `Feature (Design)`); the code tells which one a string means.
- **Check the meaning in the code** when the English leaves it open: read the call site, and further
  in the file when `code` is not enough.
    - See what each placeholder is filled with. A fallback string substituted into a sentence must
      fit the slot it fills.
    - Settle the semantics the English blurs: a ratio or a difference, which way a rotation turns,
      which control a hint names.
    - **A msgid used in several places** (`left` is a view name and a hotend; `Sphere` is a primitive
      and a paint brush) needs a wording that fits every use. When none can, report the missing
      `msgctxt` as a source problem.
- **When the context still leaves two readings,** take the likelier one and add a review note naming
  the other.

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
- **The catalog's existing words come next.** A tooltip or message that names a setting, a button or a
  menu item quotes that control's translation exactly, so the text matches what the user sees. `nearby`
  shows the controls next to the string; `lookup` finds the rest. For a recurring term the glossary
  lacks, `lookup` the English and `lookup --msgstr` the rendering you intend, and use the one the
  catalog already settled on in that sense. Reuse obsolete (`#~`) entries as a translation memory.
- **No settled term: look outside the catalog, in this order.**
    1. **Other slicers' shipped catalogs** for the same language:
       [BambuStudio](https://github.com/bambulab/BambuStudio) `bbl/i18n/<lang>/BambuStudio_<lang>.po`
       and [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer)
       `resources/localization/<lang>/PrusaSlicer_<lang>.po` (a local clone, if there is one). Orca
       shares much of its UI with both, so their translators have often rendered the same string
       already. Their choice is evidence, not a rule: the glossary and Orca's own catalog win.
    2. **The 3D-printing community in that language:** manuals, wikis and forums of printer vendors
       (Bambu Lab, Prusa, Creality, Snapmaker) and their users. Take the term printers actually use,
       a loanword where they use the loanword.
    3. **Neighbouring fields,** when printing has no term of its own: CAD (SolidWorks, Fusion,
       Onshape, AutoCAD), CNC/CAM and machining, then engineering and mathematics. General UI
       vocabulary (*Settings*, *Preferences*, *Undo*, *Browse*, *Quit*, *Copy*, *Paste*) follows the
       operating systems' own translations, which users already know.
    4. **English,** when nothing is established: brand names, acronyms, file formats, G-code tokens,
       variables, and Orca-specific feature names with no equivalent.

  Translate paired terms as a pair, so they read as the two sides of one choice (direct drive and
  Bowden, inner and outer wall). Name the sources a term came from in the report.
- **Each field keeps its own vocabulary.** 3D-view and CAD strings follow CAD software and drafting
  standards: the standard view names, *isometric*, *orbit*, *pan* and *zoom*, *cut plane* versus
  *section plane*, the primitive names. The camera of the 3D view (the viewpoint) and the printer's
  camera (the device) are different things and take different words wherever the language has two.
- **A regional variant is its own language.** Never derive one from the other character by character
  or word by word: zh_TW and zh_CN differ in vocabulary, not only in script. Start from the
  variant's own glossary column and catalog. A converter (OpenCC `s2twp` for zh_CN → zh_TW) gives a
  draft at most; review every entry it produced.
- **Boilerplate stays uniform.** `Failed to connect to …`, `Are you sure you want to …?` and
  `If expressed as a %, it will be computed over …` each get one template in the language, even where
  the English varies.

## Style

- **Localize, don't translate word-for-word.** Translate what the string means where it appears (see
  *Context*), not each English word.
- **Write native UI text.** The result should read as if the app had been written in that language.
    - Restructure the sentence instead of following English word order.
    - Break up long noun chains.
    - Drop passive and filler constructions the language doesn't use in UI text.
    - Don't calque English idioms or compounds where the language has its own expression.
    - Don't carry English Title Case over: capitalize as the language does (sentence case in most,
      German nouns as German writes them).
    - Use the language's own punctuation. Don't carry English marks over, such as ` — ` or straight
      quotes where the language has its own. ASCII quotes around a literal value the user types or
      the app matches stay as written.
- **Follow the catalog's conventions,** which a new entry must match:
    - formal or informal address;
    - full-width or half-width punctuation;
    - spacing between scripts (CJK next to Latin letters or digits);
    - the separator sibling labels use before their value;
    - the verb form of buttons and menu commands (infinitive, imperative or noun);
    - the form of a confirmation question.
- **Simplify developer phrasing** when the meaning survives. *"This option determines whether
  OrcaSlicer should apply the retract reduction on infill"* reads better as *"Disable retractions while
  printing infill."* Introduce nuances in following sentences, not in one packed clause.
- **Don't expand.** A machine translation adds no facts the English doesn't state. Expanding
  descriptions is for human translators, who can confirm the extra content.
- **Write for both audiences:** professionals and people with a printer at home. Keep the established
  terminology, used as plainly as the string allows.
- **Read the batch back without the English** before applying it, as a user of the app would see it.
  Rewrite whatever reads like a translation, is ambiguous on its own, or uses a word differently from
  the strings around it.

## When to add a review note

Add `"review": "<what to check>"` to an item when the translation is probable but unconfirmed. It
becomes `# Needs human review: …`, and the entry still ships. Typical reasons:

- The English stays ambiguous even with its code: "Tile" as a verb or a noun.
- A short label's role shows only in the running app, not in its code.
- Two established terms compete, or the glossary cell is unsettled.
- The string's grammar depends on a value substituted at runtime: `%zu %s` counts with no plural entry.

Say exactly what to look at and where: *"Row name in the Design tab tool-offer menu; check it reads
right for the whole row."*

## Problems that belong in the source, not the translation

Report these; don't work around them in a catalog.

- **The English is wrong.** Never "fix" it in the translation only.
- **A hint names a control that doesn't exist,** or a user-visible literal isn't wrapped in `_L`.
- **One msgid needs two translations.** The source needs `_L_CONTEXT` / `_u8L_CONTEXT` (a code change).
- **A literal `%` in a string xgettext flagged `possible-c-format`** fails `msgfmt`. The fix is a
  `// xgettext:no-c-format, no-boost-format` comment above the string in the source, not `%%` in the
  translation.
