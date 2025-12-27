Files in this directory are named for the **exact** output of `awk -F= '/^ID=/ {print $2}' /etc/os-release` for their respective distribution.

When `build_linux.sh` is executed, the respective file for the distribution will be sourced so the distribution specific instructions/logic are used.
