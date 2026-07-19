# Repoint the installed interpreter's RUNPATH from the absolute deps dir to a
# self-relative entry so the bundled runtime is relocatable (the deps tree,
# Flatpak /app/libpython, and AppImage $APPDIR/lib/python all keep bin/ and
# lib/ as siblings). $ORIGIN is expanded by the dynamic loader; CMake leaves
# it alone (only ${...} is expanded here). RPATH_CHANGE edits the ELF in
# place, so the new entry must not be longer than the old one: the reserved
# ${DESTDIR}/libpython/lib is at least 18 bytes even for the shortest
# supported DESTDIR (Flatpak's /app), longer than the 14-byte $ORIGIN/../lib.
# Invoked from python3.cmake with -DPYTHON_BIN=... -DOLD_RPATH=...
file(RPATH_CHANGE FILE "${PYTHON_BIN}" OLD_RPATH "${OLD_RPATH}" NEW_RPATH "$ORIGIN/../lib")
