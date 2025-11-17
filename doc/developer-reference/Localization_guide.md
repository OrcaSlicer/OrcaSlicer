# Localization and translation guide

The purpose of this guide is to describe how to contribute to the OrcaSlicer translations. We use GNUgettext for extracting string resources from the project and PoEdit for editing translations.

Those can be downloaded here:

- [GNUgettext](https://www.gnu.org/software/gettext/) package contains a set of tools to extract strings from the source code and to create the translation Catalog.
- [PoEdit](https://poedit.net) provides good interface for the translators.

After GNUgettext is installed, it is recommended to add the path to gettext/bin to PATH variable.

Full manual for GNUgettext can be seen here: [http://www.gnu.org/software/gettext/manual/gettext.html](http://www.gnu.org/software/gettext/manual/gettext.html)

### Scenario 1. How do I add a translation or fix an existing translation

1. Get PO-file 'OrcaSlicer_xx.pot' from corresponding sub-folder here:
   [https://github.com/OrcaSlicer/OrcaSlicer/tree/master/localization/i18n](https://github.com/OrcaSlicer/OrcaSlicer/tree/master/localization/i18n)
2. Open this file in PoEdit as "Edit a translation"
3. Apply your corrections to the translation
4. Push changed OrcaSlicer_xx.po into the original folder
5. copy OrcaSlicer_xx.mo into resources/i18n/xx and rename it to OrcaSlicer.mo, then push the changed file.

### Scenario 2. How do I add a new language support

1. Get file OrcaSlicer.pot here :
   [https://github.com/OrcaSlicer/OrcaSlicer/tree/master/localization/i18n](https://github.com/OrcaSlicer/OrcaSlicer/tree/master/localization/i18n)
2. Open it in PoEdit for "Create new translation"
3. Select Translation Language (for example French).
4. As a result you will have fr.po - the file containing translation to French.
Notice. When the translation is complete you need to:
    - Rename the file to OrcaSlicer_fr.po
    - Click "Save file" button. OrcaSlicer_fr.mo will be created immediately
    - Bambu_Studio_fr.po needs to be copied into the sub-folder fr of [https://github.com/OrcaSlicer/OrcaSlicer/tree/master/localization/i18n](https://github.com/OrcaSlicer/OrcaSlicer/tree/master/localization/i18n), and be pushed
    - copy OrcaSlicer_xx.mo into resources/i18n/xx and rename it to OrcaSlicer.mo, then push the changed file.
( name of folder "fr" means "French" - the translation language).

### Scenario 3. How do I add a new text resource when implementing a feature to OrcaSlicer

Each string resource in OrcaSlicer available for translation needs to be explicitly marked using L() macro like this:

```C++
auto msg = L("This message to be localized")
```

To get translated text use one of needed macro/function (`_(s)` or `_CHB(s)` ).
If you add new file resource, add it to the list of files containing macro `L()`

### Scenario 4. How do I use GNUgettext to localize my own application taking OrcaSlicer as an example

1. For convenience create a list of files with this macro `L(s)`. We have
    https://github.com/OrcaSlicer/OrcaSlicer/blob/master/localization/i18n/list.txt.

2. Create template file(*.POT) with GNUgettext command:

    ```shell
    xgettext --keyword=L --add-comments=TRN --from-code=UTF-8 --debug -o OrcaSlicer.pot -f list.txt
    ```

    Use flag `--from-code=UTF-8` to specify that the source strings are in UTF-8 encoding
    Use flag `--debug` to correctly extract formatted strings(used %d, %s etc.)

3. Create PO- and MO-files for your project as described above.

4. To merge old PO-file with strings from created new POT-file use command:

    ```shell
    msgmerge -N -o new.po old.po new.pot
    ```

    Use option `-N` to not using fuzzy matching when an exact match is not found.

5. To concatenate old PO-file with strings from new PO-file use command:

    ```shell
    msgcat -o new.po old.po
    ```

6. Create an English translation catalog with command:

   ```shell
   msgen -o new.po old.po
   ```

    > [!NOTE]
    > In this Catalog it will be totally same strings for initial text and translated.

When you have Catalog to translation open POT or PO file in PoEdit and start translating.

## General guidelines for OrcaSlicer translators

- We recommend using _PoEdit_ application for translation (as described above). It will help you eliminate most punctuation errors and will show you strings with "random" translations (if the fuzzy parameter was used).

- To check how the translated text looks on the UI elements, test it :) If you use _PoEdit_, all you need to do is save the file. At this point, a MO file will be created. Rename it OrcaSlicer.mo, and you can run OrcaSlicer (see above).

- If you see an encoding error (garbage characters instead of Unicode) somewhere in OrcaSlicer, report it. It is likely not a problem of your translation, but a bug in the software.

- See on which UI elements the translated phrase will be used. Especially if it's a button, it is very important to decide on the translation and not write alternative translations in parentheses, as this will significantly increase the width of the button, which is sometimes highly undesirable:

- If you decide to use autocorrect or any batch processing tool, the output requires very careful proofreading. It is very easy to make it do changes that break things big time.

- **Any formatting parts of the phrases must remain unchanged.** For example, you should not change `%1%` to `%1 %`, you should not change `%%` to `%` (for percent sign) and similar. This will lead to application crashes.

- Please pay attention to spaces, line breaks (\n) and punctuation marks. **Don't add extra line breaks.** This is especially important for parameter names.

- Description of the parameters should not contain units of measurement. For example, "Enable fan if layer print time is less than ~~n seconds~~"

- For units of measurement, use the international system of units. Use "s" instead of "sec".

- If the phrase doesn't have a dot at the end, don't add it. And if it does, then don't forget to :)

- It is useful to stick to the same terminology in the application (especially with basic terms such as "filament" and similar). Stay consistent. Otherwise it will confuse users.
