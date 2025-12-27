#!/bin/bash
# Update and upgrade all system packages
apt update
apt upgrade -y          

build_linux="./build_linux.sh -u"
echo "-----------------------------------------"	
echo "Running ${build_linux}..."
echo "-----------------------------------------"	
${build_linux}

echo "------------------------------"
echo "Installing missing packages..."
echo "------------------------------"
apt install -y libgl1-mesa-dev m4 autoconf libtool
