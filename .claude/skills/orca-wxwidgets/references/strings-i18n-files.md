# Strings, translation, files and app services

How text moves between UTF-8 `std::string` and `wxString` under Orca's build flags, how to format
and translate it, and the wx services that carry text or files in and out of the app: file and
directory dialogs, paths, browser/app launching, clipboard, drag and drop, logging, native message
boxes, settings storage, secrets and single-instance handling. Read it before touching any
user-visible string, any file path or any of those services, and when debugging mojibake, empty
strings, untranslated text or a lost `&`.

Contents: [Rules](#rules) · [Build facts](#build-facts-that-decide-string-behaviour) ·
[wxString conversions](#wxstring-encodings-and-conversions) · [Formatting](#formatting) ·
[Translation](#translation) · [Language switching](#language-switching-and-the-translation-lifecycle) ·
[Labels & mnemonics](#labels-mnemonics-and-markup) · [Paths](#paths-and-standard-locations) ·
[File & dir dialogs](#file-and-directory-dialogs) · [Launching](#launching-the-browser-files-and-programs) ·
[Clipboard](#clipboard) · [Drag & drop](#drag-and-drop) · [Logging](#logging-wxlog-vs-boost-log) ·
[Message boxes](#native-message-boxes) · [AppConfig](#settings-appconfig-not-wxconfig) ·
[Secrets](#secrets-wxsecretstore) · [Single instance](#single-instance)

## Rules

1. `std::string` is UTF-8 everywhere outside wx; `wxString` exists only at the wx boundary. Convert
   with `from_u8`/`into_u8`, paths with `from_path`/`into_path`. → §wxString conversions
2. Never let a UTF-8 `std::string`/`const char*` reach a `wxString` parameter implicitly (default dir
   of a file dialog, `SetLabel`, `wxString::Format("%s", …)`): it is decoded with the C-locale
   encoding, the ANSI code page on Windows. → §wxString conversions
3. Never take narrow text out of a `wxString` with `ToStdString()`, `mb_str()` or `c_str()`; use
   `into_u8(w)` or `w.utf8_string()`. → §wxString conversions
4. Never keep the pointer from `ToUTF8().data()`, `utf8_str()`, `mb_str()` or `c_str()` beyond the
   full-expression; own a `std::string`. → §Pointer lifetime
5. Non-ASCII text in source only through `_L(...)`, `wxString::FromUTF8(u8"…")` or `L"…"`, never
   `wxString("°C")`. → §Source literals
6. Build user-visible strings with `format_wxstr`/`GUI::format` and `%1%` placeholders; pass
   `std::string` arguments as they are; write a literal percent as `%%`. → §Formatting
7. With `wxString::Format`/`Printf`, match every specifier to its argument type yourself (`%zu`,
   casts); Orca compiles the check out. → §Formatting
8. Serialise numbers locale-independently; only display goes through the UI locale. → §Numbers
9. Mark every user-visible literal with an extracted keyword (`L`, `_L`, `_u8L`, `L_CONTEXT`,
   `_L_CONTEXT`, `_u8L_CONTEXT`, `_L_PLURAL`); never `_()`, `_utf8()`, `_CHB()` for a new string.
   → §Extraction
10. A new source file with translatable strings is added to `localization/i18n/list.txt`.
    → §Extraction
11. Translate at construction/display time; never cache translated text in a static, namespace-scope
    variable or long-lived singleton without a re-localise hook. → §Language switching
12. One complete sentence per msgid, placeholders instead of concatenated fragments, the number
    placeholder in both plural forms, `// TRN` for ambiguous strings, a context for homonyms.
    → §Plurals and contexts
13. A wrapper around `wxGetTranslation` returns `wxString` by value. → §Translation
14. User-supplied text (preset, filament, file, printer names) in a mnemonic-interpreting label
    goes through `SetLabelText` or `wxControl::EscapeMnemonics`. → §Labels
15. File dialogs: filters from `file_wildcards(FT_*)`, default dir `from_u8(app_config->get_last_dir())`,
    results through `GetPaths`/`GetPath` + `into_path`, never `wxFD_CHANGE_DIR`/`wxDD_CHANGE_DIR`,
    a real parent window. → §File and directory dialogs
16. Pass only the dialog's own style flags; `wxDD_NEW_DIR_BUTTON` is 0, use `wxDD_DEFAULT_STYLE`.
    → §File and directory dialogs
17. Folders open through `desktop_open_any_folder`/`desktop_open_datadir_folder`; a file that came
    from a project is launched only after `is_safe_to_open_file_name`. → §Launching
18. Clipboard access goes through a checked `wxClipboardLocker`; use `SetText`/`wxTextDataObject(text)`,
    not `wxTextDataObject::SetData`. → §Clipboard
19. Start `DoDragDrop` from the mouse handler; never replace a drop target from inside its own
    callbacks. → §Drag and drop
20. wxLog never reaches the user in Orca; tell the user with `show_error` or a MsgDialog, log with
    `BOOST_LOG_TRIVIAL` and stream UTF-8. → §Logging
21. `wxMessageBox` returns `wxYES/wxNO/wxCANCEL/wxOK/wxHELP`, `ShowModal` returns `wxID_*`; UI code
    uses the MsgDialog family. → §Native message boxes
22. AppConfig values are set with a `std::string`, never a bare `const char*` third argument, and only
    on the main thread; no wxConfig. → §AppConfig
23. Never touch `wxSecretStore` from a timer, poll or other per-tick UI path. → §Secrets
24. Cross-instance requests go through `instance_check` and `OtherInstanceMessageHandler`; do not add
    another checker or a wxIPC server. → §Single instance

## Build facts that decide string behaviour

Generic wx advice is often wrong for Orca because of these settings. The installed `setup.h` is
`<wx install>/lib/wx/include/<port>-unicode-static-3.3/wx/setup.h`.

| Setting | Value in Orca | Where |
|---|---|---|
| `wxString` storage | `std::wstring` (`wxUSE_UNICODE_UTF8 0`, `wxUSE_UTF8_LOCALE_ONLY 0`), deep copy, never copy-on-write | installed setup.h; `include/wx/string.h:121-132` |
| implicit `wxString` → `const char*` / `const void*` | **off**: the app defines `wxNO_UNSAFE_WXSTRING_CONV` | top-level `CMakeLists.txt` (`add_definitions(-DwxNO_UNSAFE_WXSTRING_CONV)`, comment "This implicit conversion breaks the UTF-8 encoding quite often"); `include/wx/string.h:1631-1637` |
| implicit `wxString` → `std::string` | **off** (`wxUSE_STD_STRING_CONV_IN_WXSTRING 0`) | installed setup.h; `include/wx/string.h:1376-1385` |
| implicit `std::string` / `const char*` → `wxString` | **on**, decoded with the current locale (`wxNO_IMPLICIT_WXSTRING_ENCODING` is not defined) | `include/wx/string.h:1215-1217, 1323-1325` |
| wx's own `_()` macro | not defined (`-DWXINTL_NO_GETTEXT_MACRO`); Orca defines its own | `CMakeLists.txt`; `include/wx/translation.h:47-49`; `src/slic3r/GUI/I18N.hpp` |
| debug level | wx built with `wxBUILD_DEBUG_LEVEL=0`; `libslic3r_gui` gets `wxDEBUG_LEVEL=0` under `SLIC3R_STATIC` (default ON): `wxASSERT`, `Format` type checks, `wxLogDebug`, `wxLogTrace` compile out; `wxCHECK*` still return, silently | `deps/wxWidgets/wxWidgets.cmake`, `src/slic3r/CMakeLists.txt`; `include/wx/log.h:62-77` |
| printf positional parameters | on (`wxUSE_PRINTF_POS_PARAMS 1`) | installed setup.h |
| MSVC source charset | `/utf-8`, so narrow literals hold UTF-8 bytes | `CMakeLists.txt` (`add_compile_options(... /utf-8)`) |
| clipboard, DnD, secret store, config, single-instance checker, IPC | compiled in (all `wxUSE_* 1`); Orca uses its own config and messaging, and the wx checker only on Windows | installed setup.h |

Because asserts are compiled out, every misuse described below fails silently in Orca: wrong or
empty text, a dropped call, a leaked object. wx would assert in a debug build; Orca never shows it.

## wxString encodings and conversions

**Contract.** A narrow `char*`, `std::string` or `std::string_view` given to wxString "supposes that
the string contains data in the current locale encoding, use FromUTF8() if the string contains
UTF-8-encoded data instead"; the implicit constructors are "dangerous … the resulting string will be
empty if the conversion from the current locale encoding fails" (`interface/wx/string.h:58-89`).
In the other direction `c_str()`/`mb_str()` are "potentially destructive … an empty string is
returned if the conversion fails", and `ToStdString()` loses data unless given `wxConvUTF8`; use
`utf8_string()` (`interface/wx/string.h:108-120, 843-863`). `FromUTF8`: "If s is not a valid UTF-8
string, an empty string is returned" (`interface/wx/string.h:2037`). The Unicode overview calls
`FromUTF8()` followed by `c_str()` "a recipe for disaster … may work perfectly well during testing on
Unix systems using UTF-8 locale but completely fail under Windows" (`docs/doxygen/overviews/unicode.h:314-320`).

**What "current locale encoding" is** (`wxConvLibc`, `src/common/strconv.cpp:3403-3409`) [source]:

| Platform | Narrow ↔ wide conversion | Consequence for UTF-8 data |
|---|---|---|
| MSW | `wxMBConv_win32` with `CP_ACP`, the system ANSI code page (`src/common/strconv.cpp:2600-2607`), independent of the UI language Orca picks; Orca's manifest does not opt into a UTF-8 active code page | non-ASCII text becomes mojibake (single-byte code pages) or an empty string (DBCS code pages). A developer machine with Windows' system-wide "Use Unicode UTF-8" option hides the bug |
| macOS | `wxMBConvLibc` (`mbstowcs`, follows `LC_CTYPE`) | the C runtime stays in the `"C"` locale until `GUI_App::load_language` calls `wxLocale::Init`; in `"C"` macOS maps each byte to one character (mojibake) [tested]; region locales afterwards decode UTF-8 [tested] |
| GTK | `wxMBConvLibc` | wxGTK calls `gtk_disable_setlocale()` (`src/gtk/app.cpp:526-537`), so the locale is `"C"` until `load_language`; `wxUILocale` then prefers a UTF-8 codeset (`src/unix/uilocale.cpp` `TryCreateLocaleWithUTF8`, `wxSetlocaleTryUTF8`) |

The implicit conversions therefore work on macOS and Linux after startup and break on Windows, so a
bug of this class survives testing on a Mac.

**What compiles in Orca and what it does:**

| Expression | Compiles? | Encoding |
|---|---|---|
| `wxString w = s;` or `f(const wxString&)` called with `std::string`/`const char*` | yes | locale (CP_ACP on MSW): mojibake or empty for UTF-8 |
| `wxString w(sv)` from `std::string_view` | only explicitly: the ctor is `explicit` (`include/wx/string.h:1327-1328`) although `interface/wx/string.h:71` calls it implicit | locale |
| `std::string s = w;`, `const char* p = w;` | **no** | — |
| `w.ToStdString()`, `w.mb_str()`, `(const char*)w.c_str()` | yes | locale; `""` on failure (`wxCStrData::AsChar`, `include/wx/string.h:4292-4303`) |
| `w.utf8_string()`, `w.ToUTF8()`/`w.utf8_str()`, `into_u8(w)` | yes | UTF-8, never fails |
| `wxString::FromUTF8(s)`, `from_u8(s)` | yes | UTF-8; `""` on invalid input |
| `w.ToStdWstring()`, `w.wc_str()`, `wxString(L"…")` | yes | lossless |

**OrcaSlicer helpers** (`src/slic3r/GUI/GUI.hpp`/`GUI.cpp`):

| Helper | Does | Note |
|---|---|---|
| `from_u8(const std::string&)` | `wxString::FromUTF8(str.c_str())` | stops at an embedded NUL, `""` on invalid UTF-8; for binary-safe input use `wxString::FromUTF8(s)` (the `std::string` overload passes the length, `include/wx/string.h:1782-1783`) |
| `into_u8(const wxString&)` | `std::string(str.utf8_str().data())` | owning copy |
| `from_path(const boost::filesystem::path&)` | `wstring` on Windows, `from_u8(path.string())` elsewhere | |
| `into_path(const wxString&)` | `boost::filesystem::path(str.wx_str())` | wide on every platform |
| `file_url_from_path(path)` | `wxFileSystem::FileNameToURL(wxFileName(from_path(path)))` | |
| `I18N::translate*`, `L_str(std::string)` | decode their narrow input with `wxConvUTF8` | UTF-8 msgids are safe |

ImGui takes UTF-8 `const char*`: `ImGuiWrapper::text(const wxString&)` converts with `into_u8`; raw
`ImGui::` calls take `_u8L(...)`/`into_u8(...)` (see `references/webview-gl-aui-media.md` for the
ImGui layer).

### Source literals

MSVC compiles with `/utf-8`, so `"°C"` is a UTF-8 byte string on every compiler, and
`wxString("°C")` decodes it with the locale like any other narrow string ("never use 8-bit
characters directly in the program source", `docs/doxygen/overviews/unicode.h:145-160`). Non-ASCII
text goes through `_L("…")` (decodes UTF-8), `wxString::FromUTF8(u8"\u2103")` (the
`AMSDryControl.cpp` style for `℃`) or a wide literal `L"…"`. In C++20 `u8"…"` becomes `char8_t`,
which `FromUTF8` does not accept; Orca builds C++17. Mixing plain ASCII literals with a `wxString`
(`_L("Version") + " " + v`) is fine.

### Pointer lifetime

`utf8_str()`/`ToUTF8()` return a `wxScopedCharBuffer` and `mb_str()` a `wxCharBuffer` by value
(`interface/wx/string.h:745-755, 884-886`); each dies at the end of the full-expression. `c_str()`
returns a `wxCStrData` proxy whose narrow pointer points into a conversion buffer owned by the
`wxString` itself (`m_convertedToChar`, `src/common/string.cpp` `wxString::AsChar`) [source]: it
stays valid only until that string is modified, converted again or destroyed, so `_L("…").c_str()`
dangles at the end of the statement. Note that `wxCStrData` still converts implicitly to
`const char*` under `wxNO_UNSAFE_WXSTRING_CONV` (`include/wx/string.h:210-217`), so
`const char* p = w.c_str();` compiles.

- **Rule:** never keep a pointer into a temporary conversion buffer beyond the statement that
  created it.
  **Why:** the pointer dangles once the buffer is destroyed; the read returns garbage or crashes
  later, intermittently, on every platform.
  ```cpp
  // Wrong:
  const char* p = name.ToUTF8().data();
  use(p);
  // Right:
  std::string s = into_u8(name);          // or name.utf8_string()
  use(s.c_str());
  ```
  Cite: `ImGuiWrapper::clipboard_get` keeps the converted text in the member
  `m_clipboard_text` so the `const char*` it returns stays valid.

Passing a temporary such as `_L("…") + dots` straight into a `const wxString&` parameter is safe,
even if the callee yields or repaints: the temporary lives until the end of the full-expression and
`wxString` is a deep-copy `std::wstring`. (The Linux splash crash once blamed on such a temporary
was the splash screen's event filter, fixed in 4088a36095; see `references/threads-timers-app.md`.)

### Other traps

- `s[n]` returns a `wxUniCharRef` proxy: it cannot be `switch`ed on, and `auto c = s[0]; c = 'x';`
  **modifies the string**. Use `s[n].GetValue()` or an explicit `int`/`wchar_t` type
  (`interface/wx/string.h:171-226`).
- Since 3.3 `wxstr = {"Hello", 2}` is ambiguous (the `string_view` constructor); write
  `wxString{"Hello", 2}` (`docs/changes.txt:191-194`).
- Never pass a `wxString`, `c_str()` or `mb_str()` to a real C vararg function (`printf`); use
  `wxString::Format`/`wxPrintf` or convert explicitly (`interface/wx/string.h:262-300`).
- `wxUSE_STL` no longer exists; wx 3.3 re-enables implicit `wxString` → `std::string` only through
  `wxUSE_STD_STRING_CONV_IN_WXSTRING=1` (`docs/changes.txt:163-166`). Orca keeps it 0 and adds
  `wxNO_UNSAFE_WXSTRING_CONV`; do not enable either conversion, the compile error is the guard.

### Pitfalls

- **Rule:** wrap every UTF-8 `std::string` in `from_u8()` (or pass it to a helper that takes
  `std::string`) before it reaches a wx API.
  **Why:** the implicit constructor decodes with CP_ACP on MSW, so a non-ASCII preset name, path or
  translated `_u8L` string turns into mojibake or `""`; macOS/Linux look correct.
  ```cpp
  // Wrong:
  label->SetLabel(preset.name);
  wxFileDialog dlg(this, title, app_config->get_last_dir(), "", file_wildcards(FT_3MF), wxFD_OPEN);
  // Right:
  label->SetLabel(from_u8(preset.name));
  wxFileDialog dlg(this, title, from_u8(app_config->get_last_dir()), "", file_wildcards(FT_3MF), wxFD_OPEN);
  ```
  Cite: `GUI_App::import_model` (model opener with `from_u8` default dir); `CMakeLists.txt`
  `wxNO_UNSAFE_WXSTRING_CONV` comment.
- **Rule:** get UTF-8 out of a `wxString` with `into_u8`/`utf8_string()`.
  **Why:** `ToStdString()`, `mb_str()` and `c_str()` use `wxConvLibc` and return `""` when a character
  is not representable in the ANSI code page (`include/wx/string.h:4292-4303`).
  ```cpp
  // Wrong:
  std::string path = dlg.GetPath().ToStdString();
  // Right:
  boost::filesystem::path path = into_path(dlg.GetPath());   // or into_u8(dlg.GetPath())
  ```

## Formatting

**`wxString::Format`/`Printf` contract.** Variadic templates that normalise each argument; positional
`%2$d %1$d` is supported because `wxUSE_PRINTF_POS_PARAMS` is 1 (`interface/wx/string.h:1533-1546`).
The specifier/argument type check is a `wxASSERT_MSG` (`include/wx/strvararg.h:336-347`), so at
debug level 0 nothing checks it: `%d` with `size_t`, `%s` with an `int` or a missing argument is
silent undefined behaviour. `const char*`, `std::string` and `std::string_view` arguments are
decoded with `wxConvLibc` (`wxArgNormalizerWchar<const char*>`, `include/wx/strvararg.h:612-624,
772-781`), so UTF-8 data garbles on MSW exactly as in the table above.

**OrcaSlicer: `format_wxstr` and `GUI::format`** (`src/slic3r/GUI/format.hpp`, on top of
`Slic3r::format` in `src/libslic3r/format.hpp`) wrap `boost::format`:

| Helper | Returns | Arguments |
|---|---|---|
| `format_wxstr(fmt, args...)` | `wxString`, built with `wxString::FromUTF8(result)` | `fmt` as `const char*`, `std::string` (UTF-8) or `wxString`; args of any streamable type |
| `GUI::format(fmt, args...)` | UTF-8 `std::string` | same |
| `Slic3r::format(fmt, args...)` (libslic3r) | UTF-8 `std::string` | narrow only |

- Placeholders: boost's `%1%` (preferred: position-independent, checked by `xgettext --boost`),
  printf-style `%s`/`%d` and positional `%1$s`/`%1$d` all work.
- `std::string`/`const char*` arguments are inserted as raw bytes, i.e. as UTF-8; pass them directly
  instead of converting to `wxString` first.
- `boost::format` throws `boost::io::too_many_args`/`too_few_args` on a placeholder/argument count
  mismatch, and a stray `%` throws too (Orca's helpers keep boost's default "all errors throw"):
  `bad_format_string` when it cannot start a directive (`"50%"`, `"5%/s"`), `too_few_args` when it
  happens to parse as one (`"50% done"` reads `% d`, a space-flag `%d`) [tested]. A literal percent
  is `%%`. A string that never reaches a formatter but contains `%` needs
  `// xgettext:no-c-format, no-boost-format` above it, otherwise `msgfmt --check-format` fails
  (AGENTS.md; e.g. `ConfigManipulation.cpp`).
- `wxString` arguments [source + tested]: the `cook(const wxString&)` overloads that convert to
  UTF-8 live in `Slic3r::internal::format` in `slic3r/GUI/format.hpp`, declared after the
  `format_recursive` template in `libslic3r/format.hpp`. Under two-phase lookup (clang, GCC) a
  dependent call only finds later overloads through ADL, and `wxString`'s namespace is the global
  one, so the generic `cook` is chosen and the `wxString` reaches boost through wx's
  `operator<<(std::ostream&, const wxString&)` → `wxConvWhateverWorks` (C locale first, UTF-8
  fallback; `src/common/string.cpp:158-170`, `src/common/strconv.cpp:3354-3363`). That is correct
  while the C locale is UTF-8 (macOS and Linux after `load_language`); MSVC without `/permissive-`
  finds the overloads. `into_u8(w)` as the argument is exact everywhere.

**Choosing.** New user-visible strings use `format_wxstr(_L("… %1% …"), args)` or
`GUI::format(_u8L(...), args)`: translators can reorder `%1%`/`%2%`, while plain `%s %d` order is
fixed (a reordered c-format translation fails `msgfmt --check-format`; never reorder positional
arguments in a c-format string). `wxString::Format` with printf specifiers is accepted in existing
code; plural strings often use `%1$d` with `GUI::format` (`NotificationManager.cpp`).

```cpp
wxString msg = format_wxstr(_L("The file %1% was loaded"), filename);   // filename: UTF-8 std::string
wxString pl  = wxString::Format(_L_PLURAL("%d object", "%d objects", n), n);  // n: int/unsigned, not size_t
```

### Numbers

`wxLocale::Init` "changes the application locale … this will affect many of standard C library
functions such as printf()" (`interface/wx/intl.h:584-590`). Orca deliberately leaves `LC_NUMERIC`
localised (the `wxSetlocale(LC_NUMERIC, "C")` in `GUI_App::load_language` is commented out), so in
the GUI thread `wxString::Format("%.2f")`, `wxString::ToDouble` and `std::to_string` use the UI
language's decimal separator; `wxString::ToCDouble`/`FromCDouble` do not.

- Display: `double_to_string(value, precision)` (`Field.cpp`, `wxNumberFormatter` plus the locale
  separator from `is_decimal_separator_point()`); parse user input by replacing the other separator
  with the locale's and then calling the locale-aware `wxString::ToDouble` (`Field::get_value_by_opt_type`
  style in `Field.cpp`), not `ToCDouble`.
- Data (config values, G-code, project files, URLs): `CNumericLocalesSetter` (RAII `LC_NUMERIC="C"`),
  `float_to_string_decimal_point`, `string_to_double_decimal_point` (`libslic3r/LocalesUtils.hpp`).
  TBB worker threads set `"C"` per thread (`libslic3r/Thread.cpp`); the GUI thread does not.

### Pitfalls

- **Rule:** pass UTF-8 `std::string` values to `format_wxstr`, or `from_u8` them for
  `wxString::Format`.
  **Why:** a `std::string` `%s` argument to `wxString::Format` is decoded with CP_ACP on MSW.
  ```cpp
  // Wrong:
  wxString::Format(_L("Preset %s not found"), preset_name);          // std::string
  // Right:
  format_wxstr(_L("Preset %1% not found"), preset_name);
  ```
  Cite: `include/wx/strvararg.h:772-781`.
- **Rule:** escape literal percent signs in strings that go through `format_wxstr`/`GUI::format`.
  **Why:** a stray `%` throws at runtime (`boost::io::too_few_args` here, because `% d` parses as a
  directive; `bad_format_string` for `"50%"` at the end), in whatever handler builds the message.
  ```cpp
  // Wrong:
  format_wxstr(_L("Progress: 50% done"));
  // Right:
  format_wxstr(_L("Progress: 50%% done"));
  ```
- **Rule:** use `%zu` or cast for `size_t` in `wxString::Format`.
  **Why:** the type check is compiled out; `%d` with a 64-bit `size_t` is undefined behaviour that
  truncates the value or misreads the following arguments, depending on the ABI.
  ```cpp
  // Wrong:
  wxString::Format("%d items", vec.size());
  // Right:
  wxString::Format("%d items", static_cast<int>(vec.size()));
  ```

## Translation

**wx contract** (`interface/wx/translation.h:577-640`): `wxGetTranslation(string, domain = "",
context = "")` returns the original string when no catalog has it; a non-empty context needs a
matching `msgctxt` in the catalog; the plural overload returns `string` for `n == 1` and `plural`
otherwise when no catalog is found; "This function is thread-safe". Since 3.3 it returns
**`wxString` by value**, not a const reference: "please change the return type of the function to
wxString" (`docs/changes.txt:139-142`). Orca's `I18N::translate` overloads already return by value;
keep any new wrapper that way, a `const wxString&` return now dangles.

**OrcaSlicer macros** (`src/slic3r/GUI/I18N.hpp`):

| Macro | Returns | Extracted by xgettext? |
|---|---|---|
| `_L(s)` | translated `wxString` | yes |
| `_u8L(s)` | translated UTF-8 `std::string` | yes |
| `_L_CONTEXT(s, ctx)` / `_u8L_CONTEXT(s, ctx)` | `msgctxt`-disambiguated `wxString` / `std::string` | yes (`1,2c`) |
| `_L_PLURAL(s, plural, n)` | plural-aware `wxString` (`n` is `unsigned int`) | yes (`1,2`) |
| `L(s)` / `L_CONTEXT(s, ctx)` | the literal unchanged: a **marker** for xgettext, translated later at display time | yes |
| `_(s)`, `_utf8(s)` | same as `_L`/`_u8L` | **no** |
| `_CHB(s)` | translated `wxScopedCharBuffer` (a temporary, see §Pointer lifetime) | **no** |
| `_devL(s)` | `wxString(s)`, untranslated and locale-decoded | **no** |
| `_omitL(s)` | `""` | **no** |

There is no `_CTX` macro and no plural-with-context macro. All overloads decode `const char*` and
`std::string` input with `wxConvUTF8`, and accept `wxString`/`std::wstring` too, so `_L(var)` works
for a runtime string, but only finds a translation if that exact string was marked with `L()`
somewhere. `L_str(std::string)` (`I18N.cpp`) is the function form of `_L` for a UTF-8 string.

libslic3r has its own `src/libslic3r/I18N.hpp`: `L`/`L_CONTEXT` markers (functions returning the
literal) and `_u8L`, which calls a callback that `GUI_App` installs
(`Slic3r::I18N::set_translate_callback(libslic3r_translate_callback)`); `#error` guards stop either
header being included in the other module.

### Extraction

`scripts/run_gettext.sh --full` (and `.bat`) run xgettext with exactly these keywords
`--keyword=L --keyword=_L --keyword=_u8L --keyword=L_CONTEXT:1,2c --keyword=_L_CONTEXT:1,2c
--keyword=_u8L_CONTEXT:1,2c --keyword=_L_PLURAL:1,2`, plus `--add-comments=TRN --from-code=UTF-8 --boost`
(among other flags), over the files listed in `localization/i18n/list.txt`, then `scripts/HintsToPot.py` appends the
strings of `resources/data/hints.ini`, and `msgfmt --check-format` compiles each catalog into
`resources/i18n/<lang>/OrcaSlicer.mo`. Consequences:

- A string inside `_()`/`_utf8()`/`_CHB()`/`_devL()` never reaches the `.pot`, so it stays English.
  `_()` is fine only around a value that was already marked with `L()`.
- xgettext extracts only string literals (wx states the same for its own `_()`,
  `interface/wx/translation.h:590-592`); a variable inside `_L()` is translated at runtime only if its
  value was marked elsewhere.
- A file missing from `list.txt` is not scanned at all.
- `// TRN …` immediately above the line carries a translator comment into the catalog
  (`//TRN To be shown in the main menu View->Top` in `MainFrame.cpp`, `// TRN %1% = file path` in
  `DownloaderFileGet.cpp`).
- `--boost` marks `%1%` strings as boost-format, and `msgfmt --check-format` then rejects a
  translation that drops or changes a placeholder.
- Only `OrcaSlicer.mo` ships; there is no `wxstd` catalog, so wx's own internal strings (standard
  dialog buttons it creates, wx error messages) stay English.

### Deferred translation

Static tables, option definitions and enum labels hold `L("…")`-marked literals and are translated
where they are shown: `file_wildcards_by_type` stores `L("STL files")` titles and `file_wildcards()`
calls `I18N::translate(data.title_id)`; option labels marked `L_CONTEXT("Top", "Layers")` are shown
with `_L_CONTEXT(option.label, "Layers")` (`OG_CustomCtrl.cpp`) — only those hard-coded `"Top"`/`"Bottom"`
labels: every other def string is translated without context, so an `L_CONTEXT` in a def is ignored at
display. The settings side of this pattern is in `references/orca-settings-ui.md` §Localization of option
definitions.

### Plurals and contexts

```cpp
wxString s = wxString::Format(_L_PLURAL("%d object", "%d objects", n), n);
text += GUI::format(_L_PLURAL("%1$d Object has custom supports.", "%1$d Objects have custom supports.", cnt), cnt);
wxString top = _L_CONTEXT("Top", "Camera View");   // msgctxt "Camera View" (MainFrame.cpp)
```

- **Rule:** keep the number placeholder in both forms and give the full sentence to one msgid.
  **Why:** the catalog's `Plural-Forms` decides which form applies (Russian uses the "singular" form
  for 21, 31, …; ja/ko/zh have one form), so "One object" without `%d` is wrong for 21 objects;
  glued fragments (`_L("Delete") + " " + _L("object")`) cannot be reordered or inflected.
- **Rule:** disambiguate homonyms with a context in the source (`_L_CONTEXT`/`_u8L_CONTEXT`), never
  by tweaking a translation; the context string must match exactly at the marker and the call site.

## Language switching and the translation lifecycle

**OrcaSlicer design** (`GUI_App::load_language(wxString language, bool initial)`):

1. Initial call only: `wxFileTranslationsLoader::AddCatalogLookupPathPrefix(from_u8(localization_dir()))`
   (a static wx list), then the language from AppConfig key `language`, else the system language
   (MSW: `LCIDToLocaleName`), else `wxTranslations::GetBestTranslation(SLIC3R_APP_KEY, wxLANGUAGE_ENGLISH)`.
   RTL languages are not supported and fall back.
2. The dictionary language and the C-runtime locale are chosen separately: Slovak uses the Czech
   dictionary; when the locale is not available the code tries `linux_get_existing_locale_language`
   (Linux), the base language (`en` from `en_IL`), then a fallback chain (current, system, best,
   en_US, en_GB) while keeping the requested dictionary. If nothing is available it shows a
   `wxMessageBox` and, on the initial call, exits.
3. `m_wxLocale.release()` (deliberate leak: "wxWidgets cause havoc if the current locale is
   deleted"), `m_wxLocale = make_unique<wxLocale>()`, `m_wxLocale->Init(lang)`,
   `wxTranslations::Get()->SetLanguage(language_dict)`, `m_wxLocale->AddCatalog(SLIC3R_APP_KEY)`,
   `m_imgui->set_language(...)`, then rebuilds two caches: `Preset::update_suffix_modified(...)` and
   `HintDatabase::get_instance().reinit()`.

Changing the language at runtime (`GUI_App::open_preferences` with a pending language) calls
`load_language(..., false)`, rebuilds the action-registry titles (`m_action_registry.relocalize_builtins()`)
and then `GUI_App::recreate_GUI`, which destroys and rebuilds `MainFrame` (and drops the cached
Speed Dial dialog). Everything constructed under the new `MainFrame` re-translates by construction;
anything that outlives it does not.

**wx caveats.** On macOS "it is impossible to change the application UI locale after launching it …
using this class doesn't affect the native controls and dialogs", and on macOS 11.0–12.2 changing the
C locale can break the menus (`interface/wx/intl.h:282-292`): native file dialogs, the app menu and
standard buttons follow the system language. `wxLocale::IsAvailable` builds a region tag and asks
`wxUILocale(...).IsSupported()` (`src/common/intl.cpp` `wxLocale::IsAvailable`), which on Unix no
longer falls back to another region of the same language (`docs/changes.txt:75-79`); keep the
fallbacks in `load_language`.

- **Rule:** translate at use; never store a translated string in a namespace-scope or function-local
  `static`, or in a singleton/registry, unless it has a re-localise hook wired into the language
  switch.
  **Why:** a namespace-scope static is initialised before `load_language`, so it is English forever;
  a function-local static captures the language of its first call and survives `recreate_GUI`.
  ```cpp
  // Wrong:
  static const wxString NA_STR = _L("N/A");
  // Right:
  m_label->SetLabel(_L("N/A"));       // or keep L("N/A") in the table and translate when shown
  ```
  Cite: `ActionRegistry::relocalize_builtins`, `HintDatabase::reinit`, `Preset::update_suffix_modified`
  are the existing hooks.

## Labels, mnemonics and markup

**Contract** (`interface/wx/control.h:173-195, 380-398`): in `SetLabel` "All "&" characters … indicate
that the following character is a mnemonic … To insert a literal ampersand character, you need to
double it"; `SetLabelText` shows the text exactly (implemented as `SetLabel(EscapeMnemonics(text))`,
`include/wx/control.h:63-67`); `EscapeMnemonics()` is for a label combining program mnemonics with
user text. Constructor labels are interpreted like `SetLabel` (the ports' `wxStaticText::Create`
call `SetLabel(label)`) [source]. `SetLabelMarkup` also treats an
unescaped `&` as a mnemonic, needs `&amp;`/`&lt;` for literal characters, strips the markup where it
is unsupported, and leaves the label unchanged (returns false) when the string is not well-formed
(`interface/wx/control.h:200-360`); user text inside markup must be XML-escaped.
Since 3.3 wxListbook/wxChoicebook also interpret mnemonics in page titles (`docs/changes.txt:128-130`);
Orca uses neither, but the rule is the same for every book control.

**OrcaSlicer.** `Label` (`Widgets/Label.cpp`) derives from `wxStaticText`; `Label::SetLabel` stores
the text and goes through `wxStaticText::SetLabel` (or `Wrap` for `LB_AUTO_WRAP`, `SetLabelMarkup` for
`LB_HYPERLINK` on macOS), so `&` is a mnemonic in every `Label` too. The inherited `SetLabelText`
escapes and then calls `Label::SetLabel`, so it works for `Label`. Menu labels use `&File`-style
mnemonics plus `"\t" + accelerator`, and translations must keep the `&`. A plain-text MsgDialog
message is rendered by a `Label` (mnemonics interpreted); a message with a link, `is_marked_msg`,
code excerpts or a `<tr>` table goes to `wxHtmlWindow` through `xml_escape` instead (see
`references/windows-dialogs.md` §MsgDialog family).

- **Rule:** user-supplied text goes through `SetLabelText` or `wxControl::EscapeMnemonics`.
  **Why:** the `&` disappears and the next character becomes a mnemonic: `PLA & PETG` shows as
  `PLA  PETG` on every port.
  ```cpp
  // Wrong:
  label->SetLabel(from_u8(preset.name));
  menu->Append(id, from_u8(printer_name));
  // Right:
  label->SetLabelText(from_u8(preset.name));
  menu->Append(id, wxControl::EscapeMnemonics(from_u8(printer_name)));
  ```

## Paths and standard locations

**OrcaSlicer path model.** Paths live as `boost::filesystem::path` or UTF-8 `std::string`.
`OrcaSlicer.cpp` calls `boost::nowide::nowide_filesystem()` at startup, which imbues boost paths with a
UTF-8 codecvt, so on Windows `boost::filesystem::path(utf8_string)` and `path.string()` are UTF-8.
`std::filesystem::path` is not imbued: on Windows a narrow `std::string` is read in the ANSI code page,
so build it from the wide form (`into_path(w).wstring()`) or stay with boost. At the wx boundary use
`from_path`/`into_path`.

**`wxFileName`** (`interface/wx/filename.h`): `Normalize()` without flags is deprecated
(`include/wx/filename.h:358-364`) because `wxPATH_NORM_ALL` includes `wxPATH_NORM_ENV_VARS` and
expands `$VAR`/`%VAR%` inside file names (`interface/wx/filename.h:90-103`); use `MakeAbsolute()` or
explicit `wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE`. 3.3 adds `IsMSWExtendedLengthPath()` for `\\?\`
paths "avoiding the 260 character path length restriction" (`interface/wx/filename.h:1040-1051`,
`docs/changes.txt:551`). `wxFileSystem::FileNameToURL`/`URLToFileName` convert to and from `file:`
URLs (`interface/wx/filesys.h:96-103, 175-180`).

**`wxStandardPaths`** (`interface/wx/stdpaths.h`): the directories "may or may not exist"
(`:39`); `GetUserDataDir()` is `~/.appinfo` on Unix, `%APPDATA%\appinfo` on Windows and
`~/Library/Application Support/appinfo` on macOS, and on Unix ignores `FileLayout_XDG` (`:406-420`);
`GetUserDir()` always follows XDG on Unix (`:422-433`).

**OrcaSlicer data dir** (`GUI_App::init_app_config`): a `data_dir` folder next to the executable if it
exists (portable mode), else `GetUserDataDir()` on MSW/macOS, or `$XDG_CONFIG_HOME/OrcaSlicer`
(default `~/.config/OrcaSlicer`) built by hand on Linux; then the process **chdirs to
`data_dir()/log`**. Relative paths therefore resolve into the log folder, and anything that changes the
working directory (`wxFD_CHANGE_DIR`, `wxDD_CHANGE_DIR`, `chdir`) breaks code that relies on it. Use
absolute paths built from `data_dir()`, `resources_dir()`, `localization_dir()`.

## File and directory dialogs

**Usage:**

```cpp
wxFileDialog dlg(this, _L("Choose one or more files (3MF/STEP/STL/SVG/OBJ/AMF):"),
                 from_u8(wxGetApp().app_config->get_last_dir()), wxEmptyString,
                 file_wildcards(FT_MODEL), wxFD_OPEN | wxFD_MULTIPLE | wxFD_FILE_MUST_EXIST);
if (dlg.ShowModal() != wxID_OK)
    return;
wxArrayString paths;
dlg.GetPaths(paths);                         // GetPath() returns "" with wxFD_MULTIPLE
for (const wxString& p : paths)
    load(into_path(p));
```

**Contract** (`interface/wx/filedlg.h`):

- `wxFD_OPEN` and `wxFD_SAVE` are exclusive; `wxFD_MULTIPLE`/`wxFD_FILE_MUST_EXIST` are open-only,
  `wxFD_OVERWRITE_PROMPT` save-only (`:163-200`). Contradictory styles are only asserted
  (`src/common/fldlgcmn.cpp:776-787`), i.e. ignored silently in Orca.
- Style bits are reused between classes: `wxPD_APP_MODAL` (0x0002) equals `wxFD_SAVE`
  (`include/wx/progdlg.h:21`, `include/wx/filedlg.h:45-46`), so a foreign flag can change the dialog
  type. Pass only `wxFD_*`.
- `GetPath()`/`GetFilename()` "can't be used with dialogs which have the `wxFD_MULTIPLE` style"
  (`:343-347, 380-384`); they return `""` there (`docs/changes_32.txt:119-120`). Use
  `GetPaths()`/`GetFilenames()`.
- Wildcard format `"Desc (*.a;*.b)|*.a;*.b|Desc2 (*.c)|*.c"` (`:106-115`); the default wildcard is
  `"*.*"` on MSW and `"*"` elsewhere (`:31-36`).
- `SetFilename()` in wxGTK has "little effect unless a default directory has previously been set"
  (`:452-456`).
- `SetExtraControlCreator()` forces old XP-style dialogs on MSW; `SetCustomizeHook()` is native
  (`:131-155`). New-style MSW dialogs need a single-threaded COM apartment (`:157-161`).
- `wxDirDialog`: `wxDD_NEW_DIR_BUTTON` is `0` ("deprecated, on by default now",
  `interface/wx/dirdlg.h:14`); the "Create new directory" button is shown exactly when
  `wxDD_DIR_MUST_EXIST` is absent (`interface/wx/dirdlg.h:42-46`); on macOS 10.11+ there is no title
  bar, the `message` argument is what the user sees (`interface/wx/dirdlg.h:59-62`).

**Platforms:**

| | Behaviour |
|---|---|
| MSW | New-style `IFileDialog`: wx calls `SetDefaultExtension` with the selected filter's first extension and no longer calls `AppendExtension` itself (`src/msw/filedlg.cpp:1644-1656, 1738-1740`; `docs/changes.txt:556`). `SetExtraControlCreator` → XP-style dialog. |
| macOS | Open dialogs show **no filter choice** and apply all wildcards at once unless `wxSystemOptions::SetOption(wxOSX_FILEDIALOG_ALWAYS_SHOW_TYPES, 1)`, and even then non-matching files are only greyed (`interface/wx/filedlg.h:117-128`). Matching compares the lower-cased last path extension (`wxOpenSavePanelDelegate panel:shouldEnableURL:`, `src/osx/cocoa/filedlg.mm:58-88`) [source]: case-insensitive, and a multi-dot pattern such as `*.gcode.3mf` or `*.zip.amf` never matches by itself. `wxFD_OVERWRITE_PROMPT` is always on (`:172-175`); `wxFD_OPEN` always behaves as `wxFD_FILE_MUST_EXIST` (`:184-189`). The save panel replaces the initial file name's extension with the first one in the wildcard (Orca's comment on `file_wildcards`). The native panel runs `runModal` and does not re-raise the parent dialog afterwards (`src/osx/cocoa/filedlg.mm` `ShowModal`); see `references/windows-dialogs.md` for the deferred re-raise. |
| GTK3 | `GtkFileChooserNative` (portal-capable, e.g. Flatpak) when GTK ≥ 3.20 at runtime and neither `wxFD_PREVIEW` nor an extra control/customize hook is used (`src/gtk/filedlg.cpp:265-274, 437-443`; `src/gtk/dirdlg.cpp:122-130`; `docs/changes.txt:393`). It runs through `gtk_native_dialog_run`: there is no wx window, so size/position calls do nothing (`wxFileDialog::DoSetSize` is empty). Patterns go to `gtk_file_filter_add_pattern` per token and are **case-sensitive** (`src/gtk/filectrl.cpp:169`). The chooser is transient for the parent left after `GetParentForModalDialog`, which replaces a null (or hidden, dying or `wxWS_EX_TRANSIENT`) parent with the active top-level window, else the app's main top window (`src/gtk/filedlg.cpp:206, 223-225`; `src/common/dlgcmn.cpp:180-203` `DoGetParentForDialog`) [source]. `wxFD_PREVIEW` is GTK-only (`interface/wx/filedlg.h:195-197`). |
| GTK2 (opt-out build) | never uses the native chooser; patterns case-sensitive. |

**OrcaSlicer:**

- `file_wildcards(FileType, custom_extension)` (`GUI_App.cpp`) builds every filter from
  `file_wildcards_by_type`: translated title, and an upper-case twin of every extension
  (`*.stl;*.STL`) because GTK patterns are case-sensitive. A `custom_extension` is put first because
  the macOS save panel substitutes the first extension into the initial file name.
- Openers to copy: `GUI_App::import_model`, `GUI_App::import_zip` (default dir `from_u8(...)`).
  Last directories come from `AppConfig::get_last_dir()`/`get_last_output_dir()`; the app stores
  them itself (`update_config_dir`, `update_last_output_dir`) instead of using `wxFD_CHANGE_DIR`.
- `Plater::priv::get_export_file` appends the expected extension on `__WXMSW__` when the returned name
  lacks it and then asks its own overwrite question, because the native overwrite prompt only checked
  the name the user typed.
- `CheckboxFileDialog` (`GUI_Utils.hpp/.cpp`) uses `SetExtraControlCreator`, which costs the native
  dialog on MSW and GTK3; prefer `SetCustomizeHook` for new extra controls.

**Pitfalls:**

- **Rule:** give file and directory dialogs the top-level window they belong to as parent.
  **Why:** with `nullptr`, wxGTK (`wxFileDialog::Create`, `wxDirDialog::Create`) and wxMSW
  (`wxFileDialog::ShowModal`) substitute whatever top-level window is active at that moment, else
  the app's main top window (`wxDialogBase::DoGetParentForDialog`, `src/common/dlgcmn.cpp:180-203`)
  [source], so the chooser becomes transient for and modal over an arbitrary window (a modeless
  dialog or web window that happened to be active), or gets no parent when none qualifies. Gizmo
  code has no `this` window to hand, which is where `nullptr` creeps in.
  ```cpp
  // Wrong:
  wxFileDialog dialog(nullptr, _L("Choose SVG file"), ...);
  // Right:
  wxFileDialog dialog(wxGetApp().mainframe, _L("Choose SVG file"), ...);   // or GetTopWindow()
  ```
  Cite: `src/gtk/filedlg.cpp` `wxFileDialog::Create` (`GetParentForModalDialog`, then `gtk_parent`).
- **Rule:** use `wxDD_DEFAULT_STYLE` for directory dialogs; add `wxDD_DIR_MUST_EXIST` only when the
  user must not create a folder.
  **Why:** `wxDD_NEW_DIR_BUTTON` is 0, so passing it alone means style 0: no
  `wxDEFAULT_DIALOG_STYLE|wxRESIZE_BORDER` (the generic dialog loses its frame). It does not control
  the new-folder button either: that appears whenever `wxDD_DIR_MUST_EXIST` is absent, so adding
  `wxDD_DIR_MUST_EXIST` removes it (and restricts the choice to existing folders).
  ```cpp
  // Wrong:
  wxDirDialog dlg(this, msg, path, wxDD_NEW_DIR_BUTTON);
  // Right:
  wxDirDialog dlg(this, msg, path, wxDD_DEFAULT_STYLE);   // | wxDD_DIR_MUST_EXIST: existing folders only, no new-folder button
  ```
  Cite: `include/wx/dirdlg.h:45-47`; `interface/wx/dirdlg.h:42-46`.
- **Rule:** never pass `wxFD_CHANGE_DIR`/`wxDD_CHANGE_DIR`.
  **Why:** Orca's working directory is `data_dir()/log`; the native GTK path even `chdir`s directly
  (`src/gtk/filedlg.cpp:449-458`).

## Launching the browser, files and programs

**Contract** (`interface/wx/utils.h`):

- `wxLaunchDefaultBrowser(url, flags)`: `wxBROWSER_NEW_WINDOW` is honoured only on Windows; a URL
  without a scheme is tested as a local file/dir (then prefixed with `file:`), otherwise `http:` is
  prepended; returns false on failure (`:468-490`). wxGTK tries `gtk_show_uri` and then `xdg-open`
  (`src/unix/utilsx11.cpp:2674-2737`).
- `wxLaunchDefaultApplication(document, flags)` opens the file in its associated application; `flags`
  is unused (`:454-462`).
- `wxExecute`: `wxEXEC_ASYNC` returns the pid; `wxEXEC_SYNC` "will call wxYield()" and disables all
  windows unless `wxEXEC_NODISABLE` (`:1196-1212`), so a synchronous call re-enters your handlers;
  main thread only (`:1250-1252`).

**OrcaSlicer:**

- Links: `wxGetApp().open_browser_with_warning_dialog(url, flags)` is the app-level entry point (it
  forwards to `wxLaunchDefaultBrowser`, so a direct `wxLaunchDefaultBrowser` call behaves the same).
- Folders: `desktop_open_any_folder(path)` / `desktop_open_datadir_folder()` (`GUI.cpp`): `explorer`
  on Windows (`explorer /select,<path>` for `desktop_open_any_folder`), `openFolderForFile`
  (any folder) or `open` (data dir) on macOS, `xdg-open` on Linux (of the containing folder when
  `path` is a file) with the AppImage variables (`APPIMAGE`, `APPDIR`, `LD_LIBRARY_PATH`,
  `LD_PRELOAD`, `UNION_PRELOAD`) removed and `OWD` as the working directory. A bare `wxExecute("xdg-open …")` or
  `wxLaunchDefaultApplication` from an AppImage passes Orca's bundled libraries to the file manager.
- Project attachments: `desktop_open_project_attachment` accepts only a regular file inside the
  project's auxiliary temp folder (`is_absolute_path_within_root`) and asks before opening anything
  `is_safe_to_open_file_name` (`libslic3r/utils.cpp`) does not allow-list, because the desktop would
  run a script or executable without a download warning. Reuse it for any file that arrived inside a
  project or from the network.

## Clipboard

**Contract** (`interface/wx/clipbrd.h`): `Open()` "should be tested"; keep the clipboard open "only
momentarily" (`:24-29, 145-155`). `SetData` replaces any previously set object, so several formats
need one composite object; "After this function has been called, the clipboard owns the data"
(`:157-170`). `Flush()` keeps the data after exit; implemented on MSW and GTK only, on GTK only for
the CLIPBOARD selection and with a clipboard manager running (`:56-61, 100-115`).
`UsePrimarySelection(true)` makes every operation fail on platforms without a PRIMARY selection
(`:172-187`). `wxClipboardLocker` (`include/wx/clipbrd.h:167-191`) opens in its ctor, closes in its
dtor, and `!lock` tests `IsOpened()`.

**Platforms** [source]:

| | Behaviour |
|---|---|
| MSW | `SetData`/`GetData` do not check `Open()` (`src/msw/clipbrd.cpp` `wxClipboard::SetData`), so code that forgets it works here only. `wxTextDataObject::SetData()` size must now include the 2-byte NUL (`docs/changes.txt:62-66`); use `SetText()` or the ctor. |
| macOS | without `Open()`, `SetData`/`AddData`/`GetData` return false through `wxCHECK_MSG(m_open, …)` without taking the object (`src/osx/carbon/clipbrd.cpp:84-105, 140-147`); a successful write is flushed to the pasteboard immediately (`:110-114`). |
| GTK | same `wxCHECK_MSG(m_open, …)` early return (`src/gtk/clipbrd.cpp:643-647, 775`). `GetData`/`IsSupported` are asynchronous underneath: `wxClipboardSync` spins `YieldFor(wxEVT_CATEGORY_CLIPBOARD)` until GTK answers and forbids re-entrancy (`src/gtk/clipbrd.cpp:68-92`). PRIMARY selection exists. Under Wayland, Wayland MIME types are advertised next to the X11 atoms (`src/gtk/clipbrd.cpp:655-700`). |

```cpp
wxClipboardLocker lock;
if (!lock)
    return;
wxTheClipboard->SetData(new wxTextDataObject(text));   // clipboard owns it on success
```

**OrcaSlicer.** Models: the copy button in `TroubleshootDialog` (`wxClipboardLocker`) and
`ImGuiWrapper::clipboard_set`/`clipboard_get` (tested `Open()`, UTF-8 via `wxString::FromUTF8`/
`into_u8`). The 3D scene's copy/paste is an internal clipboard (`Selection::Clipboard`,
`Selection::copy_to_clipboard`/`paste_from_clipboard`), not the wx one.

- **Rule:** open the clipboard through a checked `wxClipboardLocker` before `SetData`/`GetData`, and
  test the result.
  **Why:** on GTK and macOS an unopened clipboard makes `SetData` return false without taking the
  object (leak, nothing copied) and `GetData` return false (nothing pasted); MSW does not check, so
  the bug passes Windows testing.
  ```cpp
  // Wrong:
  wxTheClipboard->Open();
  wxTheClipboard->SetData(new wxTextDataObject(t));
  wxTheClipboard->Close();
  // Right:
  wxClipboardLocker lock;
  if (lock)
      wxTheClipboard->SetData(new wxTextDataObject(t));
  ```

## Drag and drop

**Contract.** `SetDropTarget`: "If the window already has a drop target, it is deleted"
(`interface/wx/window.h:3651-3657`); the window also deletes its target in its destructor
(`src/common/wincmn.cpp:515`), so the window owns it. `DragAcceptFiles` "Cannot be used together
with SetDropTarget() on non-Windows platforms" (`interface/wx/window.h:3659-3672`). The
`wxDropTarget` destructor deletes its data object and `SetDataObject` deletes the previous one
(`interface/wx/dnd.h:59-62, 138-146`). Call sequence: `OnEnter` → `OnDragOver`* → `OnDrop` (return
false to refuse) → `OnData` (`interface/wx/dnd.h:44-45, 75-127`). `wxFileDropTarget::OnDropFiles(x, y,
filenames)` returns true to accept (`interface/wx/dnd.h:395-422`). `wxDropSource::SetData` "will not
delete any previously associated data", i.e. the source does not own it (`interface/wx/dnd.h:335-338`).
`DoDragDrop(flags)` "blocks the program until the user releases the mouse button", and the target
cannot change the result code the source gets (`docs/doxygen/overviews/dnd.h:44-50, 84-90`).
`wxDragMove` is reported on MSW only (`interface/wx/dnd.h:26`).

**Platforms** [source]:

| | Behaviour |
|---|---|
| MSW | `SetDropTarget` revokes and deletes the old target immediately (`src/msw/window.cpp:1753-1761`). `wxDropTarget::MSWUpdateDragImageOnLeave()` (undocumented, `include/wx/msw/ole/droptgt.h:73`) hides the shell drag image. |
| GTK | `SetDropTarget` unregisters and deletes immediately (`src/gtk/window.cpp:6605-6616`). `DoDragDrop` returns `wxDragNone` unless a mouse button is down after a mouse event (`src/gtk/dnd.cpp:835-850`), then runs a nested `gtk_main_iteration` loop with `g_blockEventsOnDrag` set (`:906-916`). Under Wayland it also hooks button/motion events because "drag-end" may never arrive (`:945-955`). |
| macOS | `SetDropTarget` deletes the old target immediately (`src/osx/window_osx.cpp:616-622`). |

**wxDataViewCtrl DnD** (`interface/wx/dataview.h`): `EnableDropTargets` is fully implemented in the
generic and native macOS versions, wxGTK uses only the first format (`:1376-1386`); `SetDragFlags` is
honoured only by the generic control (MSW), not native GTK/macOS (`:4010-4024`); `GetDropEffect`
returns `wxDragNone` on native GTK and macOS (`:4026-4041`); `GetProposedDropIndex` works from
`ITEM_DROP` everywhere and from `ITEM_DROP_POSSIBLE` except native GTK (`:4053-4064`). The control
itself is in `references/controls-dataview.md`.

**OrcaSlicer:**

- Model-file drops: `PlaterDropTarget : wxFileDropTarget` (`Plater.cpp`), installed with
  `q->SetDropTarget(new PlaterDropTarget(...))` (the window takes ownership); `SetDefaultAction(wxDragCopy)`.
  `OnDropFiles` calls `MSWUpdateDragImageOnLeave()` under `WIN32` before showing UI, raises the main
  frame, switches to the Prepare tab, routes a single `.svg` to the SVG gizmo (`GLGizmoSVG::create_volume`
  at the drop point) and otherwise calls `Plater::load_files`. `Preview::set_drop_target(target)` is
  the helper for putting a target on the preview window.
- Custom payload: `DragDropPanel.cpp` (filament-group dragging). `ColorDataObject : wxCustomDataObject`
  with `wxDataFormat("application/customize_format")` and fixed-size POD data; `ColorDropSource` keeps
  the data object as a member and is constructed on the stack in `DragDropPanel::DoDragDrop`, which
  `ColorPanel::OnLeftDown` calls (satisfying the GTK button-down rule); `ColorDropTarget` owns its
  `ColorDataObject` through `SetDataObject`.
- ObjectList row reordering (`GUI_ObjectList.cpp`): `ObjectList::OnBeginDrag` keeps the real payload in
  `m_dragged_data`, gives wx a dummy `wxTextDataObject` with non-empty text ("needed for GTK") and
  `SetDragFlags(wxDrag_DefaultMove)`; `m_prevent_list_events` suppresses the selection events GTK fires
  because it drops *between* rows where MSW/macOS drop *on* a row.

**Pitfalls:**

- **Rule:** call `wxDropSource::DoDragDrop` synchronously from the mouse-down/drag handler.
  **Why:** wxGTK refuses (returns `wxDragNone`) without a current button press after a mouse event; a
  `CallAfter` or timer loses it.
  ```cpp
  // Wrong:
  CallAfter([this] { wxDropSource src(this); src.SetData(m_obj); src.DoDragDrop(); });
  // Right:
  void ColorPanel::OnLeftDown(wxMouseEvent&) { m_parent->DoDragDrop(this, ...); }
  ```
- **Rule:** never call `SetDropTarget` on a window from inside that window's current target
  (`OnDrop`/`OnData`/`OnDropFiles`); defer it with `CallAfter`.
  **Why:** every port deletes the old target immediately, so the callback returns into a freed object.

## Logging: wxLog vs boost log

**Contract** (`docs/doxygen/overviews/log.h`): with the default `wxLogGui`, `wxLogError`/`wxLogWarning`/
`wxLogMessage` pop up a message box (`:31-38`); messages from other threads are buffered until the main
thread flushes (`:228-240`); `wxLogNull`/`EnableLogging(false)` affect only the current thread
(`:242-244`). `wxLog::SetActiveTarget` takes ownership of the new target and the caller must delete the
returned old one (`interface/wx/log.h:326-340`). `wxLogNull` suppresses **all** messages, not only the
one you expect (`interface/wx/log.h:1026-1035`).

**OrcaSlicer.** `GUI_App::on_init_inner` first calls `wxLog::SetActiveTarget(new wxBoostLog())`;
`wxBoostLog::DoLogText` writes every wx message to `BOOST_LOG_TRIVIAL(warning)` as UTF-8, and its
destructor flushes pending thread messages into itself so they never reach a `wxLogGui`. Release builds
(`BBL_RELEASE_TO_PUBLIC`) also `wxLog::SetLogLevel(wxLOG_Message)`. `wxLogDebug`/`wxLogTrace` compile to
nothing at debug level 0 (`include/wx/log.h:62-77`). So **wxLog never shows UI in Orca**, including
the errors wx itself logs (failed file operations, image loading), which end up only in the log file.
Use `wxLogNull` around a probing call whose failure wx would log (`OpenGLManager.cpp`), and still check
the return value.

Write diagnostics with `BOOST_LOG_TRIVIAL(level) << __FUNCTION__ << …` (or
`boost::format("%1%: …") % …`) and stream UTF-8: `into_u8(w)` or `w.ToUTF8().data()` inside the
statement. Streaming a `wxString` directly goes through `wxConvWhateverWorks` (locale first;
`src/common/string.cpp:158-170`) and writes ANSI bytes into the UTF-8 log on MSW.

User-facing errors go through `show_error(parent, msg)` (`GUI.hpp`), which is **asynchronous**: it
queues an `ErrorDialog` with `wxGetApp().CallAfter` and captures the raw `parent`, so pass a parent that
outlives the call. `show_info` and `warning_catcher` are synchronous MsgDialogs; the `const char*`/
`std::string` overloads decode UTF-8. The dialogs themselves are in `references/windows-dialogs.md`
§MsgDialog family.

- **Rule:** never use `wxLogError`/`wxLogWarning`/`wxLogMessage` to inform the user.
  **Why:** `wxBoostLog` sends them to the log file only; the user sees nothing.
  ```cpp
  // Wrong:
  wxLogError(_L("Export failed"));
  // Right:
  show_error(this, _L("Export failed"));
  BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": export failed: " << into_u8(path);
  ```

## Native message boxes

**Contract** (`interface/wx/msgdlg.h`): `wxCANCEL` "Must be combined with either wxOK or wxYES_NO"
(`:30`); `wxYES_NO` without `wxCANCEL` has no close button on MSW (`:31-35`); `wxHELP` is unsupported
from a non-main thread on wxOSX (`:39-40`); `wxCANCEL_DEFAULT` is ignored on wxOSX (`:44-46`);
`wxSTAY_ON_TOP` works only on MSW and GTK (`:89-91`). `wxMessageDialog::ShowModal()` returns
`wxID_OK/wxID_CANCEL/wxID_YES/wxID_NO/wxID_HELP` (`:269-275`), but **`wxMessageBox()` returns
`wxYES/wxNO/wxCANCEL/wxOK/wxHELP`** (`:308-312`). `wxRichMessageDialog` is native only on MSW and
generic elsewhere (`interface/wx/richmsgdlg.h:18-23`). On MSW every `TaskDialog`-based dialog
(`wxMessageBox`, `wxMessageDialog`, `wxRichMessageDialog`, `wxProgressDialog`) ignores dark mode
(`interface/wx/app.h:1436-1440`).

**OrcaSlicer.** UI code uses the MsgDialog family (`MessageDialog`, `RichMessageDialog`,
`WarningDialog`, `ErrorDialog`, `InfoDialog`; `references/windows-dialogs.md` §MsgDialog family) for
dark mode, DPI and a consistent look; its `ShowModal()` returns `wxID_*`. Native boxes are for code that
runs before the GUI exists (`wxMessageBox` in `GUI_App::load_language`, `MessageBoxA` in `OrcaSlicer.cpp`).

- **Rule:** compare each API's result with its own constants.
  **Why:** `wxYES` (0x2) is not `wxID_YES` (5103); the comparison is always false and the "Yes" branch
  never runs.
  ```cpp
  // Wrong:
  if (wxMessageBox(msg, title, wxYES_NO) == wxID_YES) ...
  // Right:
  if (MessageDialog(this, msg, title, wxYES_NO).ShowModal() == wxID_YES) ...   // or wxMessageBox(...) == wxYES
  ```

## Settings: AppConfig, not wxConfig

Orca does not use `wxConfig`/`wxFileConfig` (so the 3.3 change of the Unix default location to XDG,
`docs/changes.txt:25-29`, does not affect it). Settings live in `AppConfig`
(`src/libslic3r/AppConfig.hpp`), a JSON file `OrcaSlicer.conf` under `data_dir()`
(`AppConfig::config_path`), reached through `wxGetApp().app_config` (the object map is in
`references/orca-architecture.md`).

- String-typed API: `get(key)`/`get(section, key)` return `std::string` (UTF-8) and `""` when absent;
  `set(key, value)`, `set(section, key, value)`, `set_str(section, key, value)`, `set_bool(key, bool)`;
  `has`, `erase`. `get_bool(section, key)` is `get(section, key) == "true" || get(key) == "1"` (the
  `"1"` test reads the `app` section). Some keys hold `"true"`/`"false"` (what `set(section, key, bool)`
  writes), others `"1"`/`"0"`; check how a key is written before testing it.
- `set` marks the config dirty when the value changes; `GUI_App`'s idle handler saves a dirty config
  after post-init. Call `app_config->save()` explicitly only when the value must be on disk before control
  returns to the event loop (before a restart, exit or launching another instance); Preferences rows also
  `save()` at once by convention (`references/orca-architecture.md` §Preferences).
- `AppConfig::save()` throws `CriticalException` off the main thread, and nothing in `AppConfig` is
  locked: read and write it on the main thread only.
- Values are UTF-8: `from_u8(app_config->get(...))` at the wx boundary, `into_u8(w)` going back.

- **Rule:** pass a `std::string` (or use `set_str`) when setting a string value with a section.
  **Why:** for `set("app", "key", "value")` the `const char*` → `bool` conversion is a standard
  conversion and beats the user-defined `std::string` one, so `set(section, key, bool)` wins and stores
  `"true"` [tested with clang].
  ```cpp
  // Wrong:
  app_config->set("app", "theme", "dark");          // stores "true"
  // Right:
  app_config->set_str("app", "theme", "dark");      // or std::string("dark"), or set("theme", "dark")
  ```
  Cite: `AppConfig::set` overloads in `AppConfig.hpp`.

## Secrets: wxSecretStore

**Contract** (`interface/wx/secretstore.h`): on Unix it needs libsecret and a running secret service, so
always check `IsOk(&errmsg)` (`:193-210, 257`); libsecret is no longer required at run time
(`docs/changes.txt:533`), so a missing library or service shows up only as `IsOk()` false.
`GetDefault()` "may show a dialog to the user under some platforms, so it can take an arbitrarily long
time to return" (`:243-246`). One username per service (`:260-268`).

**OrcaSlicer** (`OrcaCloudServiceAgent`): AppConfig `SETTING_USE_ENCRYPTED_TOKEN_FILE` selects either an
AES-GCM encrypted token file in the data dir or the system store; `secret_stored` records whether this
process read or wrote a secret, and `clear_user_secret` touches the store only then (or on an explicit
all-backends logout).

- **Rule:** never call `wxSecretStore::GetDefault()`, `Load`, `Save` or `Delete` from a timer, poll or
  repeated UI path; touch the store on explicit login/logout and cache the state.
  **Why:** each call is a blocking keychain/D-Bus round trip on the UI thread; an unresponsive keychain
  froze the UI for 25 s per poll, and the delete also removed a login another instance had just saved.
  Cite: 1a5bc8982d (`OrcaCloudServiceAgent::clear_user_secret`).

## Single instance

**Contract** (`interface/wx/snglinst.h`): `wxSingleInstanceChecker::Create(name, path)`: `name` "is used
as the mutex name under Win32 and the lock file name under Unix", and `path` "is ignored under Win32"
(`:97-103`); the default name includes the user id, so different users may run concurrently (`:49-53`).

**OrcaSlicer** (`instance_check(argc, argv, app_config_single_instance)`, `InstanceCheck.cpp`), run at
startup from `GUI_Init.cpp` before the GUI: the executable path (the AppImage file on Linux) is hashed into a lock name.
Windows checks it with a `wxSingleInstanceChecker` (`GUI_App::init_single_instance_checker`, name
`<hash>.lock`); macOS and Linux use Orca's own lock file `data_dir()/cache/<hash>.lock`
(`instance_check_internal::get_lock`). When another instance holds the lock and single-instance mode
applies (command line, else AppConfig), the command line is forwarded and this process exits: Windows
`WM_COPYDATA` to the other instance's window, macOS `NSDistributedNotificationCenter`
(`InstanceCheckMac.mm`, `send_message_mac`), Linux D-Bus. No wxIPC is involved. The receiving
`OtherInstanceMessageHandler` turns messages into `EVT_LOAD_MODEL_OTHER_INSTANCE`,
`EVT_START_DOWNLOAD_OTHER_INSTANCE` and `EVT_INSTANCE_GO_TO_FRONT` for the main frame. A new kind of
cross-instance request extends this message path on all three transports rather than adding a second
checker or a wxIPC server.
