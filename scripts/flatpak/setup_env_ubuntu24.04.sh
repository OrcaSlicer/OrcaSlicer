#! /bin/bash

sudo apt update
sudo apt install build-essential flatpak flatpak-builder gnome-software-plugin-flatpak -y
flatpak remote-add --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install flathub org.gnome.Platform//48 org.gnome.Sdk//48


##
# in OrcaSlicer folder, run following command to build Orca
# # First time build
# flatpak-builder --state-dir=.flatpak-builder --keep-build-dirs --user --force-clean build-dir scripts/flatpak/io.github.softfever.OrcaSlicer.yml

# # Subsequent builds (only rebuilding OrcaSlicer)
# flatpak-builder --state-dir=.flatpak-builder --keep-build-dirs --user build-dir scripts/flatpak/io.github.softfever.OrcaSlicer.yml --build-only=OrcaSlicer