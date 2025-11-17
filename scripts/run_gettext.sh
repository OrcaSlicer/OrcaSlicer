#!/bin/sh

#  OrcaSlicer gettext
#  Created by SoftFever on 27/5/23.
#

# Check for --full argument
FULL_MODE=false
for arg in "$@"
do
    if [ "$arg" = "--full" ]; then
        FULL_MODE=true
    fi
done

if $FULL_MODE; then
    xgettext --keyword=L --keyword=_L --keyword=_u8L --keyword=L_CONTEXT:1,2c --keyword=_L_PLURAL:1,2 --add-comments=TRN --from-code=UTF-8 --no-location --debug --boost -f ./localization/i18n/list.txt -o ./localization/i18n/OrcaSlicer.pot
    python3 scripts/HintsToPot.py ./resources ./localization/i18n
fi


echo "$0: working dir = $PWD"
pot_file="./localization/i18n/OrcaSlicer.pot"
for dir in ./localization/i18n/*/
do
    dir=${dir%*/}      # remove the trailing "/"
    lang=${dir##*/}    # extract the language identifier

    if [ -f "$dir/OrcaSlicer_${lang}.po" ]; then
        if $FULL_MODE; then
            msgmerge -N -o "$dir/OrcaSlicer_${lang}.po" "$dir/OrcaSlicer_${lang}.po" "$pot_file"
        fi
        mkdir -p "resources/i18n/${lang}"
        if ! msgfmt --check-format -o "resources/i18n/${lang}/OrcaSlicer.mo" "$dir/OrcaSlicer_${lang}.po"; then
            echo "Error encountered with msgfmt command for language ${lang}."
            exit 1  # Exit the script with an error status
        fi
    fi
done
