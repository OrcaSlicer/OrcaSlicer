Files in this directory are selected from `/etc/os-release` values used by `build_linux.sh`.

Selection behavior:

- The script starts with `ID`.
- `ubuntu` and `linuxmint` are remapped to `debian`.
- If `ID_LIKE` contains `debian` or `ubuntu`, it is also remapped to `debian`.
- If `ID_LIKE` contains `arch`, it is remapped to `arch`.
- Otherwise it falls back to the raw `ID` value.

When `build_linux.sh` runs, it sources `scripts/linux.d/<resolved-id>` and executes the distribution-specific dependency logic from that file.
