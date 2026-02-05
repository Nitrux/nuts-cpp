#!/usr/bin/env bash

# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2024-2026 <Nitrux Latinoamericana S.C. <hello@nxos.org>>


# -- Exit on errors.

set -e


# -- Compile Source

mkdir -p build && cd build

HOST_MULTIARCH=$(dpkg-architecture -qDEB_HOST_MULTIARCH)
HOST_ARCH=$(dpkg-architecture -qDEB_HOST_ARCH)

# Determine if we should use x86-64-v3 optimizations
USE_X86_64_V3_OPT="ON"
if [ "$HOST_ARCH" != "amd64" ]; then
	USE_X86_64_V3_OPT="OFF"
	echo "Building for $HOST_ARCH - x86-64-v3 optimizations disabled"
else
	echo "Building for $HOST_ARCH - x86-64-v3 optimizations enabled"
fi

cmake \
	-DCMAKE_INSTALL_PREFIX=/usr \
	-DENABLE_BSYMBOLICFUNCTIONS=OFF \
	-DQUICK_COMPILER=ON \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_SYSCONFDIR=/etc \
	-DCMAKE_INSTALL_LOCALSTATEDIR=/var \
	-DCMAKE_EXPORT_NO_PACKAGE_REGISTRY=ON \
	-DCMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY=ON \
	-DCMAKE_INSTALL_RUNSTATEDIR=/run "-GUnix Makefiles" \
	-DCMAKE_VERBOSE_MAKEFILE=ON \
	-DCMAKE_INSTALL_LIBDIR="/usr/lib/${HOST_MULTIARCH}" \
	-DUSE_X86_64_V3="${USE_X86_64_V3_OPT}" \
	..

make -j"$(nproc)"

make install


# -- Run checkinstall and Build Debian Package

>> description-pak printf "%s\n" \
	'Nitrux Update Tool System.' \
	'' \
	'A MauiKit-based system updater for Nitrux OS with XFS backup/restore and OTA updates.' \
	''

checkinstall -D -y \
	--install=no \
	--fstrans=yes \
	--pkgname=nuts \
	--pkgversion="$PACKAGE_VERSION" \
	--pkgarch="$(dpkg --print-architecture)" \
	--pkgrelease="1" \
	--pkglicense=BSD-3 \
	--pkggroup=admin \
	--pkgsource=nuts \
	--pakdir=. \
	--maintainer=uri_herrera@nxos.org \
	--provides=nuts \
	--requires="libc6,libqt6core6,libqt6dbus6,libqt6quick6,libqt6qml6,libkf6coreaddons6,libkf6i18n6,libkf6notifications6,mauikit \(\>= 4.0.2\),polkitd,xfsdump,zstd,axel,wget,nx-overlayroot" \
	--nodoc \
	--strip=no \
	--stripso=yes \
	--reset-uids=yes \
	--deldesc=yes
