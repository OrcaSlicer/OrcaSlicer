---
name: orca-translations
description: Use when updating, filling, reviewing or fixing OrcaSlicer translations in localization/i18n — running scripts/run_gettext --full, translating empty or new msgids in the .po catalogs, AI translation passes and their "# AI Translated" / "# Needs human review" comments, plural forms, msgctxt homonyms, and keeping catalogs consistent with the localization glossary (OrcaSlicer_WIKI developer_reference/glossary), including adding new glossary terms. Also use for msgfmt or run_gettext failures, placeholder or line-break mismatches, and reviewing a translation PR against the glossary.
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
comes back gets its old translation revived from `#~`, which is one reason never to purge obsolete
entries. It also compiles the `.mo` files (gitignored). The bundled Windows gettext may re-wrap a long
`#:` reference line. That churn is harmless.

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
     - `similar`: earlier translations of similar English. `"human": true` marks a person's work: when
       the English barely changed, patch that translation instead of translating again (see the rules).
3. Read every apply message:
   - **REJECTED:** a hard rule broke. Fix the translation and apply the corrected items again.
   - **WARNING:** something worth a second look, such as a glossary rendering not found, ending
     punctuation, or snake_case. A glossary warning on an inflected form you checked is fine.
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
- Fill every empty entry.
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

## Rules

1. **Run the full gettext first.** Translate against the regenerated template, never a stale one.
2. **The glossary is binding.** Use the language's rendering for each term, in the right sense. When a
   rendering looks wrong, use it and flag the entry. Then raise the change with the user, rather than
   deviating in one string.
3. **New recurring terms go into the glossary before the translations that use them.** Without a wiki
   clone, stop and tell the user (step 3).
4. **Fill, don't rewrite.** A fill pass writes only empty and fuzzy entries.
   - Existing translations, human or AI, change only on request (`--overwrite`).
   - A glossary violation in a human translation is reported, not changed.
5. **Machine output is labelled.** Every AI translation gets `# AI Translated`; uncertain ones also get
   `# Needs human review: <reason>`. Never use `fuzzy`.
6. **Never edit `msgid`, `msgctxt`, the header, or obsolete `#~` entries,** and never fix the English in
   a translation. Report source problems (see the rules file).
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
- placeholders and line breaks (`check` errors);
- `# AI Translated` kept on text a person rewrote, or added to text a person wrote;
- purged obsolete entries;
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
