#!/bin/sh

#  OrcaSlicer gettext
#  Created by SoftFever on 27/5/23.
#

list_file="./localization/i18n/list.txt"
pot_file="./localization/i18n/OrcaSlicer.pot"
filtered_list=""
missing_list=""
generated_root_dir=""

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
    [ -n "$generated_root_dir" ] && rm -rf "$generated_root_dir"
}

files_equal_ignoring_pot_date()
{
    file_a=$1
    file_b=$2
    norm_a=$(mktemp)
    norm_b=$(mktemp)

    sed '/^"POT-Creation-Date: /d' "$file_a" > "$norm_a"
    sed '/^"POT-Creation-Date: /d' "$file_b" > "$norm_b"

    if cmp -s "$norm_a" "$norm_b"; then
        rm -f "$norm_a" "$norm_b"
        return 0
    fi

    rm -f "$norm_a" "$norm_b"
    return 1
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
        generated_root_dir=$(mktemp -d)
        generated_i18n_dir="${generated_root_dir}/i18n"
        generated_pot_file="${generated_i18n_dir}/OrcaSlicer.pot"

        mkdir -p "$generated_i18n_dir"
        xgettext --keyword=L --keyword=_L --keyword=_u8L --keyword=L_CONTEXT:1,2c --keyword=_L_PLURAL:1,2 --add-comments=TRN --from-code=UTF-8 --no-location --debug --boost -f "$filtered_list" -o "$generated_pot_file"
        python3 scripts/HintsToPot.py ./resources "$generated_i18n_dir"

        if [ -f "$pot_file" ] && files_equal_ignoring_pot_date "$pot_file" "$generated_pot_file"; then
            echo "No changes detected in ${pot_file}; keeping existing file."
        else
            mv "$generated_pot_file" "$pot_file"
        fi
    else
        echo "No existing source files found in ${list_file}; skipping template regeneration."
    fi
fi


echo "$0: working dir = $PWD"
for dir in ./localization/i18n/*/
do
    dir=${dir%*/}      # remove the trailing "/"
    lang=${dir##*/}    # extract the language identifier

    if [ -f "$dir/OrcaSlicer_${lang}.po" ]; then
        if $FULL_MODE && [ -f "$pot_file" ]; then
            merged_po=$(mktemp)
            if ! msgmerge -N -o "$merged_po" "$dir/OrcaSlicer_${lang}.po" "$pot_file"; then
                echo "Error encountered with msgmerge command for language ${lang}."
                rm -f "$merged_po"
                exit 1
            fi

            if files_equal_ignoring_pot_date "$dir/OrcaSlicer_${lang}.po" "$merged_po"; then
                rm -f "$merged_po"
            else
                mv "$merged_po" "$dir/OrcaSlicer_${lang}.po"
            fi
        fi
        mkdir -p "resources/i18n/${lang}"
        if ! msgfmt --check-format -o "resources/i18n/${lang}/OrcaSlicer.mo" "$dir/OrcaSlicer_${lang}.po"; then
            echo "Error encountered with msgfmt command for language ${lang}."
            exit 1  # Exit the script with an error status
        fi
    fi
done
