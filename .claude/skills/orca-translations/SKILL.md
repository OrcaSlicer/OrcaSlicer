---
name: orca-translations
description: Use when updating, filling, reviewing or fixing OrcaSlicer translations in localization/i18n — running scripts/run_gettext --full, translating empty or new msgids in the .po catalogs, reviewing and fixing a language's existing translations for wrong meanings, competing terms and unnatural wording, AI translation passes and their "# AI Translated" / "# Needs human review" comments, plural forms, msgctxt homonyms, and keeping catalogs consistent with the localization glossary (OrcaSlicer_WIKI developer_reference/glossary), including adding new glossary terms. Also use for msgfmt or run_gettext failures, placeholder or line-break mismatches, and reviewing a translation PR against the glossary.
---

# OrcaSlicer translations

The UI strings live in gettext catalogs: `localization/i18n/OrcaSlicer.pot` is the template, and
`localization/i18n/<lang>/OrcaSlicer_<lang>.po` holds each language. The `en` catalog is empty on
purpose, so skip it. Recurring terms are fixed by the
[localization glossary](https://github.com/OrcaSlicer/OrcaSlicer_WIKI/blob/main/developer_reference/localization_glossary.md),
whose source is the per-language files in `developer_reference/glossary/` (`terms.tsv`, `languages.txt` and one `<lang>.tsv`) in the **OrcaSlicer_WIKI** repository.
A translation pass therefore touches two repositories: the catalogs here, and the glossary there.

All catalog editing goes through [`scripts/po_tool.py`](scripts/po_tool.py) (standard library only).
It lists empty entries with the glossary terms they contain and validates each translation. It writes
only the entries it changes, in the layout `msgmerge --no-wrap` produces, and adds the AI comments.
Run commands from the repository root, with `python3` (`python` or `py -3` on Windows):

```bash
T=.claude/skills/orca-translations/scripts/po_tool.py
python3 $T --help
```

Read [translation-rules.md](references/translation-rules.md) before translating. It holds the
mechanical rules, how to pick words, style, and when to flag an entry for review. To find hard-coded
English, strings that need a plural form or a context, or files missing from the extraction list, read
[source-audit.md](references/source-audit.md); that work changes C++ and comes before a translation pass.

## Workflow: fill every empty translation

Unless the user asks for something narrower, a translation pass means the steps below, in order.

### 1. Find the glossary

The tool looks for an `OrcaSlicer_WIKI` clone in `--wiki`, then `$ORCA_WIKI_DIR`, then next to this
repository.
- **A clone is found:** `git -C <wiki> fetch` and make sure it is on an up-to-date `main`. The tool
  notes when it reads another branch, or a clone that is behind.
- **No clone:** the tool reads the published glossary read-only. That is enough to translate, but not
  to add terms (see step 3).

### 2. Run the full gettext

This comes first, always. It regenerates the template from the sources and merges it into every
catalog:

```bash
./scripts/run_gettext.sh --full          # Linux/macOS: needs gettext and python3 on PATH
scripts\run_gettext.bat --full           # Windows (PowerShell: & .\scripts\run_gettext.bat --full)
```

It must exit 0. New msgids arrive empty, and removed ones become obsolete `#~` entries. A msgid that
comes back gets its old translation revived from `#~`, and `pending` offers obsolete entries as similar
earlier translations, so they stay until the translations are done and are purged in step 5. It also
compiles the `.mo` files (gitignored). The bundled Windows gettext may re-wrap a long `#:` reference
line. That churn is harmless.

Then take stock: `python3 $T status`.

### 3. Settle new glossary terms before translating anything

Terms are decided once, for every language, so all the catalogs agree.

1. `python3 $T candidates` lists recurring phrases in the empty entries that the glossary lacks. Read it
   together with the new msgids themselves.
2. Pick the real terms: feature and mode names, labels that tooltips repeat, and anything translators
   would otherwise render in several ways.
3. Brand names and acronyms belong in the glossary page's hand-written *Kept in English* table instead.
4. Skip ordinary words.
5. For each term, choose a rendering per language: check `lookup` for existing usage, and follow each
   language's conventions. Then record it:

   ```bash
   python3 $T glossary-set --term "Belt printer" \
       --description "One sentence saying what it is, and what it is not." \
       --set de=... --set es=... --set pt_BR=...
   ```

   The command adds or updates the row, keeps the TSV's BOM and line endings, and regenerates the page.
   - **An established cell:** it refuses to change it without `--replace`. Changing an established term
     also means fixing every catalog entry that uses it, so do that only on request.
   - **An empty cell:** fill it the same way when a pending string establishes the term for that
     language.

**No clone, and a new term is needed: stop.** Tell the user the glossary must be updated with these
terms, and list each one with its description and proposed renderings (`glossary-set` prints them too).
Ask them to clone `https://github.com/OrcaSlicer/OrcaSlicer_WIKI` next to this repository, or to give
its path. Translate nothing that uses those terms until they answer.

### 4. Translate, language by language, in batches

For each language:

1. Read its glossary column and notes once: `python3 $T glossary --lang <L>`.
2. Loop over the empty entries:

   ```bash
   python3 $T pending --lang <L> --limit 60 --out <scratch>/<L>.json
   # write <scratch>/<L>-tr.json: [{"id": "...", "msgstr": "..."}, ...], "msgstr_plural" for plurals,
   # plus "review": "<what to check>" where unsure
   python3 $T apply --lang <L> --input <scratch>/<L>-tr.json
   ```

   - **Batch files go in a scratch directory, not the repository.**
   - **Each entry's `id` identifies it,** so the translation file need not repeat the English. `apply`
     also accepts `msgid` with `msgctxt`, or the pending file itself with its `msgstr` fields filled.
   - **Translate every entry in the batch.** A blank `msgstr` is skipped, not cleared.
   - **Next batch:** applied entries leave the list, so run `pending` again at offset 0. Use `--offset`
     only to step past entries you deliberately leave blank.
   - **What each entry carries:**
     - `glossary`: the terms it contains, with the language's rendering.
     - `note`: the developer's `TRN` comment.
     - `flags`: `c-format` or `boost-format`.
     - `description`: present when a sense has to be chosen.
     - `refs`, `code` and `nearby`: where the source uses the string, the lines around that use (for
       short strings and placeholder strings), and the translations of the short strings used next to
       it, such as the label a tooltip describes or the other items of a menu. The catalogs carry no
       source references, so this is the only place the context shows. Read it before writing; the
       rules file's *Context* says what to take from it.
     - `similar`: earlier translations of similar English. `"human": true` marks a person's work: when
       the English barely changed, patch that translation instead of translating again (see the rules).
3. Read every apply message:
   - **REJECTED:** a hard rule broke. Fix the translation and apply the corrected items again.
   - **WARNING:** something worth a second look, such as a glossary rendering not found, ending
     punctuation, or snake_case. A glossary warning is fine on an inflected form you checked, and
     on a word the string uses in another sense than the row's description (the printer's `Model:` is
     not the glossary's 3D *Model*).
     Otherwise correct the translation (`apply --overwrite` replaces your own entry), or add a
     `review` reason.

`apply` adds `# AI Translated` to every entry it writes, and `# Needs human review: <reason>` where an
item has `review`. On an `--overwrite`, `"review": ""` removes an earlier note. Never add either to a translation a person wrote. `--human` is for applying their
text, and for a fix that keeps their words; it also drops a stale AI mark. `apply` rejects text that
strays from a person's translation more than the English change explains, unless the item says why
with `"rewrite": "<reason>"`.

**The work is large:** every new string, times every language. When subagents are available, give each
one a language or a few. Each agent only touches its own catalogs, so they can run
in parallel. Settle step 3 first. Give each agent the language list, this skill's path, the rules file,
and these instructions:
- Fill every empty entry, reading each one's `code` and `nearby` first.
- Patch a person's earlier translation (`"human": true` in `similar`) instead of translating again, and
  keep their words when fixing their entries.
- Don't add glossary rows.
- Report what it could not translate, the review notes it added, and any term it thinks the glossary
  still lacks.

Add the terms they report yourself, then have the affected languages revisit the entries.

### 5. Verify

```bash
python3 $T check --lang <L> --warnings   # every AI-translated entry, plus msgfmt --check-format
```

- **Remove the obsolete entries:** `python3 $T purge-obsolete` drops every `#~` entry, with its
  comments, from every catalog (`--lang L` for a pass limited to some languages).
- **Run the full gettext again.** It must exit 0 and leave the catalogs as the pass left them.
- **Run `python generate_glossary.py --check` in the wiki clone.**
- **Errors on entries you didn't write predate the pass.** Report them rather than fixing them in the
  same change.

### 6. Report

Report:
- the entries filled, per language;
- the review notes added;
- anything left empty, and why;
- the glossary rows added or filled.

The catalogs (and `OrcaSlicer.pot`) and the glossary (`developer_reference/glossary/*.tsv` and `localization_glossary.md`) are
separate changes in separate repositories. The wiki change should land with, or before, the catalog
change. Commit or open PRs only when asked.

## Workflow: review and fix an existing catalog

When the user asks for more than a fill (review a language, fix its translations, a complete pass),
fill it first with the workflow above, then audit what the catalog already holds. The request is what
allows existing entries to change (rule 4). Work one language at a time, and stop to ask only about
established glossary terms (step 4); fix everything else directly.

1. **Save a copy of the catalog** in the scratch directory, to diff the audit against.
2. **Read the catalog area by area,** one pass each:
   - the 3D view, navigation, camera and rendering;
   - gizmos, CAD and mesh tools;
   - printer hardware and the device tab;
   - print-process settings and their tooltips;
   - errors, warnings and dialogs.

   The catalogs carry no `#:` references, so split them with `lookup --lang <L> . --file '<regex>'
   --limit 0`, which keeps the entries used in matching source files (`Gizmos/`, `Device`,
   `PrintConfig`), and add `--code` to see each use. Read every pair in an area: short labels in full,
   with their code, and long tooltips at least for their terms. When subagents are available, give
   each one an area, the rules file and the term decisions made so far, have it return findings rather
   than edits, and merge them yourself.
3. **Look for:**
   - **Competing renderings of one term.** For each recurring English term, list every rendering it
     has (`lookup --lang <L> "<term>"`, then `lookup --msgstr` for each rendering found), and settle on
     one per sense: the glossary's where it has a row, otherwise by the rules file's order of sources.
     A recurring term the glossary lacks goes into it with `glossary-set`, as in step 3 of the fill.
   - **Wrong meanings:** a word that means something else in context, the wrong sense of a homonym
     (one `msgctxt` translated with another's word), a setting quoted differently from its own label.
     A short msgid used in several places must fit every use `--code` lists; when no wording can,
     report the missing `msgctxt`.
   - **Sets that don't read as one:** the items of a menu, a choice or a toolbar in different
     grammatical forms, or two items translated alike.
   - **Broken entries:** a lost `\n`, text left over from another string, untranslated fragments,
     a `msgstr` left identical to the English (a symbol, token or bare placeholder may be), typos.
     `check --lang <L> --scope all --warnings` finds the mechanical ones.
   - **Unnatural wording,** judged by the rules file's *Style*.
4. **Hold back changes to established glossary terms.** When the right fix changes a term the
   glossary already fixes, leave those entries and the cell alone and keep a list. After the whole
   audit, ask the user once: each term, the proposed rendering, why, and how many entries it touches,
   with a recommendation. Apply the approved ones as in *Changing an established term*.
5. **Fix the rest** with `apply --overwrite`, from items `{"id": ..., "msgstr": ...}`.
   - The patch rules for a person's translation still hold: change only what is wrong. When more than
     a patch is needed, add `"rewrite": "<what was wrong>"`; it becomes the entry's review note.
   - Entries you rewrite are marked `# AI Translated`; a patch that keeps a person's words goes in
     with `--human`. Add `review` where unsure.
6. **Verify** as in step 5 of the fill workflow. Then compare every glossary cell for the language
   with the final catalog (`check --scope all --warnings` reports renderings not found), and list the
   disagreements left.
7. **Report:**
   - the changes grouped by area, with representative before → after examples;
   - the term decisions and the sources behind them;
   - the review notes added;
   - the source problems found;
   - the glossary changes made, and the disagreements left.

## Rules

1. **Run the full gettext first.** Translate against the regenerated template, never a stale one.
2. **The glossary is binding.** Use the language's rendering for each term, in the right sense. When a
   rendering looks wrong, use it and flag the entry. Then raise the change with the user, rather than
   deviating in one string.
3. **New recurring terms go into the glossary before the translations that use them.** Without a wiki
   clone, stop and tell the user (step 3).
4. **Fill, don't rewrite.** A fill pass writes only empty and fuzzy entries.
   - Existing translations, human or AI, change only on request (`--overwrite`), as in the review
     workflow.
   - Outside that workflow, a glossary violation in a human translation is reported, not changed.
5. **Machine output is labelled.** Every AI translation gets `# AI Translated`; uncertain ones also get
   `# Needs human review: <reason>`. Never use `fuzzy`.
6. **Never edit `msgid`, `msgctxt`, the header, or obsolete `#~` entries,** and never fix the English in
   a translation. Report source problems (see the rules file). Obsolete entries are only removed whole,
   with `purge-obsolete` at the end of a pass.
7. **Write catalogs only through `po_tool.py` or gettext.** PO libraries such as polib reflow unrelated
   entries, and hand edits skip the validation.

## Changing an established term

A glossary cell is usually copied from the catalog's own label. When the label is wrong, the glossary
inherits the mistake, so fix both in the same change:

1. **Check the catalog's usage first.** `lookup --lang <L> "<term>"` searches the English, and with
   `--msgstr` it searches the translations.
   - When most entries already use the correct form, move the cell to it.
   - When the wrong form is used consistently throughout, leave both as they are and ask a
     translator.
2. **Update the cell:** `glossary-set --term "<Term>" --replace --set <L>=<value>`.
3. **Rewrite the entries that still use the old form,** only where the term has that meaning.
   - Write `[{"id": "<id from lookup>", "msgstr": "<full corrected text>"}]` and run
     `apply --overwrite`.
   - The rewritten entries are marked `# AI Translated`.

## Reviewing a translation change

Review it for these:
- glossary compliance (`check --scope all --warnings` on the language, then read the hits);
- the sense of each changed short string where it is used (`lookup --code`);
- placeholders and line breaks (`check` errors);
- `# AI Translated` kept on text a person rewrote, or added to text a person wrote;
- obsolete entries left in the catalogs, or removed other than by `purge-obsolete`;
- reflowed unrelated entries;
- plural forms short of `nplurals`.

When a reviewer disputes a term, compare it with the glossary first. A machine translation that follows
the glossary is correct even if the term itself is debatable. Then the fix is a glossary change, made
with the translators, followed by the catalogs.

## Source of truth

When this skill and the checkouts disagree, the checkouts win:
- `scripts/run_gettext.sh` / `.bat` for extraction and merging;
- `localization/i18n/list.txt` for the scanned sources;
- the wiki's `localization_guide.md` and `localization_glossary.md` and `glossary/*.tsv` for the rules and terms;
- `AGENTS.md` (Localization & translations) for the repository's conventions.

## Editing this skill

State rules and procedures, not the state of the catalogs: no counts, lists of untranslated strings or
per-language defects. Language-specific terminology belongs in the glossary's *Language specifics*
section, not here.
