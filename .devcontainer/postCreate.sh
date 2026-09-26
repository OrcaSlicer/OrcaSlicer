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

echo "----------------------------------------------------------------"
echo "Installing clang-format 18 (matches CI; Ubuntu 22.04 default repo only ships 14)"
echo "----------------------------------------------------------------"
apt install -y --no-install-recommends wget gnupg ca-certificates
wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key \
  | gpg --dearmor \
  | tee /usr/share/keyrings/llvm-archive-keyring.gpg > /dev/null
echo "deb [signed-by=/usr/share/keyrings/llvm-archive-keyring.gpg] http://apt.llvm.org/jammy/ llvm-toolchain-jammy-18 main" \
  > /etc/apt/sources.list.d/llvm-18.list
apt update
apt install -y --no-install-recommends clang-format-18
update-alternatives --install /usr/bin/clang-format clang-format /usr/bin/clang-format-18 100
update-alternatives --install /usr/bin/git-clang-format git-clang-format /usr/bin/git-clang-format-18 100
