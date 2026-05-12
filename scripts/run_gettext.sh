#!/bin/sh

#  OrcaSlicer gettext
#  Created by SoftFever on 27/5/23.
#

list_file="./localization/i18n/list.txt"
filtered_list=""
missing_list=""

report_missing_files()
{
    if [ -n "$missing_list" ] && [ -s "$missing_list" ]; then
        echo
        echo "Skipped missing source files listed in ${list_file}:"
        while IFS= read -r missing || [ -n "$missing" ]; do
            echo "  - $missing"
        done < "$missing_list"
    fi
}

cleanup_temp_files()
{
    [ -n "$filtered_list" ] && rm -f "$filtered_list"
    [ -n "$missing_list" ] && rm -f "$missing_list"
}

trap 'report_missing_files; cleanup_temp_files' EXIT

# Check for --full argument
FULL_MODE=false
for arg in "$@"
do
    if [ "$arg" = "--full" ]; then
        FULL_MODE=true
    fi
done

if $FULL_MODE; then
    filtered_list=$(mktemp)
    missing_list=$(mktemp)
    has_sources=false

    while IFS= read -r entry || [ -n "$entry" ]; do
        case "$entry" in
            ""|\#*)
                printf '%s\n' "$entry" >> "$filtered_list"
                ;;
            *)
                if [ -f "$entry" ]; then
                    printf '%s\n' "$entry" >> "$filtered_list"
                    has_sources=true
                else
                    printf '%s\n' "$entry" >> "$missing_list"
                fi
                ;;
        esac
    done < "$list_file"

    if $has_sources; then
        xgettext --keyword=L --keyword=_L --keyword=_u8L --keyword=L_CONTEXT:1,2c --keyword=_L_PLURAL:1,2 --add-comments=TRN --from-code=UTF-8 --no-location --debug --boost -f "$filtered_list" -o ./localization/i18n/OrcaSlicer.pot
        python3 scripts/HintsToPot.py ./resources ./localization/i18n
    else
        echo "No existing source files found in ${list_file}; skipping template regeneration."
    fi
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
