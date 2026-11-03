# Auditing the source for translatable text

Use this when asked to find hard-coded English, strings that need a context, or strings that need plural
forms. It changes C++ and the `.pot`, so it runs before a translation pass and is followed by one.

## How a string reaches translators

- `xgettext` reads only the files named in `localization/i18n/list.txt`, and only the macros
  `L`, `_L`, `_u8L`, `L_CONTEXT`, `_L_CONTEXT`, `_u8L_CONTEXT` and `_L_PLURAL`.
  - A file that marks strings but is missing from `list.txt` is silently never extracted. Find such
    files by comparing `grep -l` for the macros with the list.
  - `_()`, `_utf8()` and `wxGetTranslation()` do not extract. Calls that rely on them need an `L()`
    or `_L()` somewhere xgettext sees.
- **GUI code:** `_L` gives a `wxString`, `_u8L` a UTF-8 `std::string` (use it for ImGui). `L` only
  marks the string; the display site must translate it.
- **libslic3r:** the macro is whatever the file defines. Some files `#define L(s)
  Slic3r::I18N::translate(s)`, so `L()` translates at runtime there. Others, such as `PrintConfig.cpp`,
  define it as a no-op marker, and the GUI translates the text later. Read the top of the file before
  judging a `format(L(...))` call. libslic3r has no plural or context lookup at runtime.
- The libslic3r translate callback is installed in `GUI_App::on_init_inner`. Text produced before it
  is installed stays in English.

## What to look for

1. **Visible English with no macro:** labels, tooltips, menu items, dialog text, notifications and
   ImGui text. `scripts/find_untranslated.py OUT_DIR` lists candidates; most are noise, so trace each
   one to the place it is shown.
2. **Sentences built from translated pieces:** `_L("A") + name + _L(" is B")`, or a translated word
   spliced into another sentence. Translators cannot reorder or inflect these. Replace each with one
   msgid with placeholders. A spliced fragment with a leading or trailing space is the usual sign.
3. **Counts next to words that must agree:** `(s)`, "%d objects", a verb that follows a number. Use
   `_L_PLURAL(singular, plural, n)`. If the singular and plural are identical on purpose, translators
   still inflect each form.
4. **One English word with several meanings:** a short msgid used in files with different meanings
   (a verb in one place, a noun in another, a mode name, a CAD term). Generate locations with
   `xgettext` *without* `--no-location` and list short msgids that occur in two or more files.
5. **Code that compares translated text with English,** or decides behaviour from a message text.
   Translation breaks it. Compare ids or flags instead.
6. **Tests that quote message text:** `grep` the tests for a message before rewording it.

## What not to translate

Keys, config values and identifiers; log and debug output; assertion and check messages; developer-only
UI behind debug switches; undo/redo snapshot names (the history is never shown); text returned to a
plugin's code; HTML, JS and desktop-file content in string literals; brand, printer-model and format
names.

## Adding a context

- Keep the most common sense without a context, so its existing translations stay valid. Give the
  other senses a short, readable context.
- A context cannot be carried through code that translates generically: enum labels that
  `Field.cpp` translates with `_()`, tables translated by a loop, and anything in libslic3r. Leave that
  sense on the plain entry and give the contexts to the other uses.

## After the edits

1. Build and run the tests (`libslic3r_tests` in particular).
2. Run `run_gettext --full`. Reworded strings become new empty entries and the old ones become
   obsolete, which keeps their translations as a memory. Strings that gain a plural form leave a fuzzy
   entry.
3. Run the translation pass from SKILL.md. `po_tool.py pending` suggests the closest earlier translation
   for each entry.
4. Fix English typos *before* the translation pass; fixing one afterwards orphans every translation.
