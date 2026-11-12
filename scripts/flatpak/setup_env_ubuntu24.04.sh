#! /bin/bash

sudo apt update
sudo apt install build-essential flatpak flatpak-builder gnome-software-plugin-flatpak -y
flatpak remote-add --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install flathub org.gnome.Platform//50 org.gnome.Sdk//50 org.freedesktop.Sdk.Extension.llvm21//25.08


##
# in OrcaSlicer folder, run following command to build Orca

# # First time build
# ./scripts/flatpak/make_deps_tar.sh && flatpak-builder --state-dir=.flatpak-builder --keep-build-dirs --user --force-clean build-dir scripts/flatpak/com.orcaslicer.OrcaSlicer.yml

# # Subsequent builds (only rebuilding OrcaSlicer; run make_deps_tar.sh first if deps/ changed)
# flatpak-builder --state-dir=.flatpak-builder --keep-build-dirs --user build-dir scripts/flatpak/com.orcaslicer.OrcaSlicer.yml --build-only=OrcaSlicer