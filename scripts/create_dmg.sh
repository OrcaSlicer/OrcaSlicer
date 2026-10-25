#!/usr/bin/env bash
# Sourceable helper: `create_dmg <volname> <srcfolder> <output.dmg>` builds a
# compressed DMG, retrying via `retry` (source retry.sh first).
# hdiutil sizes the image from -srcfolder itself and sometimes leaves too
# little room, failing with "No space left on device" on every attempt. An
# explicit -size of the source plus 20% and 50 MB avoids that; the free space
# compresses away in UDZO.
create_dmg() {
  local volname="$1" srcfolder="$2" output="$3"
  local src_mb
  src_mb=$(( $(du -sk "$srcfolder" | cut -f1) / 1024 ))
  retry hdiutil create -volname "$volname" -srcfolder "$srcfolder" \
    -size "$(( src_mb + src_mb / 5 + 50 ))m" -ov -format UDZO "$output"
}
