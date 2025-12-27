# GLEW - The OpenGL Extension Wrangler Library

The OpenGL Extension Wrangler Library (GLEW) is a cross-platform open-source C/C++ extension loading library. GLEW provides efficient run-time mechanisms for determining which OpenGL extensions are supported on the target platform. OpenGL core and extension functionality is exposed in a single header file. GLEW has been tested on a variety of operating systems, including Windows, Linux, Mac OS X, FreeBSD, Irix, and Solaris.

![](http://glew.sourceforge.net/glew.png)

http://glew.sourceforge.net/

https://github.com/nigels-com/glew

[![Build Status](https://travis-ci.org/nigels-com/glew.svg?branch=master)](https://travis-ci.org/nigels-com/glew)
[![Gitter](https://badges.gitter.im/nigels-com/glew.svg)](https://gitter.im/nigels-com/glew?utm_source=badge&utm_medium=badge&utm_campaign=pr-badge)
[![Download](https://img.shields.io/sourceforge/dm/glew.svg)](https://sourceforge.net/projects/glew/files/latest/download)

## Table of Contents

* [Downloads](#downloads)
	* [Recent snapshots](#recent-snapshots)
* [Build](#build)
	* [Linux and Mac](#linux-and-mac)
		* [Using GNU Make](#using-gnu-make)
		* [Install build tools](#install-build-tools)
		* [Build](#build-1)
		* [Linux EGL](#linux-egl)
		* [Linux mingw-w64](#linux-mingw-w64)
	* [Using cmake](#using-cmake)
		* [Install build tools](#install-build-tools-1)
		* [Build](#build-2)
	* [Windows](#windows)
		* [Visual Studio](#visual-studio)
		* [MSYS/Mingw](#msysmingw)
		* [MSYS2/Mingw-w64](#msys2mingw-w64)
* [glewinfo](#glewinfo)
* [Code Generation](#code-generation)
* [Authors](#authors)
* [Contributions](#contributions)
* [Copyright and Licensing](#copyright-and-licensing)

## Downloads

Current release is [2.1.0](https://sourceforge.net/projects/glew/files/glew/2.1.0/).
[(Change Log)](http://glew.sourceforge.net/log.html)

Sources available as
[ZIP](https://sourceforge.net/projects/glew/files/glew/2.1.0/glew-2.1.0.zip/download) or
[TGZ](https://sourceforge.net/projects/glew/files/glew/2.1.0/glew-2.1.0.tgz/download).

Windows binaries for [32-bit and 64-bit](https://sourceforge.net/projects/glew/files/glew/2.1.0/glew-2.1.0-win32.zip/download).

### Recent snapshots

Snapshots may contain new features, bug-fixes or new OpenGL extensions ahead of tested, official releases.

[glew-20200115.tgz](https://sourceforge.net/projects/glew/files/glew/snapshots/glew-20200115.tgz/download) *GLEW 2.2.0 RC3: fixes*

[glew-20190928.tgz](https://sourceforge.net/projects/glew/files/glew/snapshots/glew-20190928.tgz/download) *GLEW 2.2.0 RC2: New extensions, bug fixes*

## Build

It is highly recommended to build from a tgz or zip release snapshot.
The code generation workflow is a complex brew of gnu make, perl and python, that works best on Linux or Mac.
The code generation is known to work on Windows using [MSYS2](https://www.msys2.org/).
For most end-users of GLEW the official releases are the best choice, with first class support.

### Linux and Mac

#### Using GNU Make

GNU make is the primary build system for GLEW, historically.
It includes targets for building the sources and headers, for maintenance purposes.

##### Install build tools

Debian/Ubuntu/Mint:    `$ sudo apt-get install build-essential libxmu-dev libxi-dev libgl-dev`

RedHat/CentOS/Fedora:  `$ sudo yum install libXmu-devel libXi-devel libGL-devel`

FreeBSD: `# pkg install xorg lang/gcc git cmake gmake bash python perl5`

##### Build

	$ make
	$ sudo make install
	$ make clean

Targets:    `all, glew.lib (sub-targets: glew.lib.shared, glew.lib.static), glew.bin, clean, install, uninstall`

Variables:  `SYSTEM=linux-clang, GLEW_DEST=/usr/local, STRIP=`

_Note: you may need to call `make` in the  **auto** folder first_

##### Linux EGL

	$ sudo apt install libegl1-mesa-dev
	$ make SYSTEM=linux-egl

##### Linux mingw-w64

	$ sudo apt install mingw-w64
	$ make SYSTEM=linux-mingw32
	$ make SYSTEM=linux-mingw64

#### Using cmake

The cmake build is mostly contributer maintained.
Due to the multitude of use cases this is maintained on a _best effort_ basis.
Pull requests are welcome.

*CMake 2.8.12 or higher is required.*

##### Install build tools

Debian/Ubuntu/Mint:   `$ sudo apt-get install build-essential libxmu-dev libxi-dev libgl-dev cmake git`

RedHat/CentOS/Fedora: `$ sudo yum install libXmu-devel libXi-devel libGL-devel cmake git`

##### Build

	$ cd build
	$ cmake ./cmake
	$ make -j4

| Target     | Description |
| ---------- | ----------- |
| glew       | Build the glew shared library. |
| glew_s     | Build the glew static library. |
| glewinfo   | Build the `glewinfo` executable (requires `BUILD_UTILS` to be `ON`). |
| visualinfo | Build the `visualinfo` executable (requires `BUILD_UTILS` to be `ON`). |
| install    | Install all enabled targets into `CMAKE_INSTALL_PREFIX`. |
| clean      | Clean up build artifacts. |
| all        | Build all enabled targets (default target). |

| Variables       | Description |
| --------------- | ----------- |
| BUILD_UTILS     | Build the `glewinfo` and `visualinfo` executables. |
| GLEW_REGAL      | Build in Regal mode. |
| BUILD_FRAMEWORK | Build as MacOSX Framework.  Setting `CMAKE_INSTALL_PREFIX` to `/Library/Frameworks` is recommended. |

### Windows

#### Visual Studio

Use the provided Visual Studio project file in build/vc15/

Projects for vc6, vc10, vc12 and vc14 are also provided

#### MSYS/Mingw

Available from [Mingw](http://www.mingw.org/)

Requirements: bash, make, gcc

	$ mingw32-make
	$ mingw32-make install
	$ mingw32-make install.all

Alternative toolchain:  `SYSTEM=mingw-win32`

#### MSYS2/Mingw-w64

Available from [Msys2](http://msys2.github.io/) and/or [Mingw-w64](http://mingw-w64.org/)

Requirements: bash, make, gcc

	$ pacman -S gcc make mingw-w64-i686-gcc mingw-w64-x86_64-gcc
	$ make
	$ make install
	$ make install.all

Alternative toolchain:  `SYSTEM=msys, SYSTEM=msys-win32, SYSTEM=msys-win64`

## glewinfo

`glewinfo` is a command-line tool useful for inspecting the capabilities of an
OpenGL implementation and GLEW support for that.  Please include `glewinfo.txt`
with bug reports, as appropriate.

	---------------------------
	    GLEW Extension Info
	---------------------------

	GLEW version 2.0.0
	Reporting capabilities of pixelformat 3
	Running on a Intel(R) HD Graphics 3000 from Intel
	OpenGL version 3.1.0 - Build 9.17.10.4229 is supported

	GL_VERSION_1_1:                                                OK
	---------------

	GL_VERSION_1_2:                                                OK
	---------------
	  glCopyTexSubImage3D:                                         OK
	  glDrawRangeElements:                                         OK
	  glTexImage3D:                                                OK
	  glTexSubImage3D:                                             OK

	...

## Code Generation

A Unix or Mac environment is needed for building GLEW from scratch to
include new extensions, or customize the code generation. The extension
data is regenerated from the top level source directory with:

	make extensions

An alternative to generating the GLEW sources from scratch is to
download a pre-generated (unsupported) snapshot:

https://sourceforge.net/projects/glew/files/glew/snapshots/

## Authors

GLEW is currently maintained by [Nigel Stewart](https://github.com/nigels-com)
with bug fixes, new OpenGL extension support and new releases.

GLEW was developed by [Milan Ikits](http://www.cs.utah.edu/~ikits/)
and [Marcelo Magallon](http://wwwvis.informatik.uni-stuttgart.de/~magallon/).
Aaron Lefohn, Joe Kniss, and Chris Wyman were the first users and also
assisted with the design and debugging process.

The acronym GLEW originates from Aaron Lefohn.
Pasi K&auml;rkk&auml;inen identified and fixed several problems with
GLX and SDL.  Nate Robins created the `wglinfo` utility, to
which modifications were made by Michael Wimmer.

## Contributions

GLEW welcomes community contributions.  Typically these are co-ordinated
via [Issues](https://github.com/nigels-com/glew/issues) or
[Pull Requests](https://github.com/nigels-com/glew/pulls) in the
GitHub web interface.

Be sure to mention platform and compiler toolchain details when filing
a bug report.  The output of `glewinfo` can be quite useful for discussion
also.

Generally GLEW is usually released once a year, around the time of the Siggraph
computer graphics conference.  If you're not using the current release
version of GLEW, be sure to check if the issue or bug is fixed there.

## Copyright and Licensing

GLEW is originally derived from the EXTGL project by Lev Povalahev.
The source code is licensed under the
[Modified BSD License](http://glew.sourceforge.net/glew.txt), the
[Mesa 3-D License](http://glew.sourceforge.net/mesa.txt) (MIT) and the
[Khronos License](http://glew.sourceforge.net/khronos.txt) (MIT).

The automatic code generation scripts are released under the
[GNU GPL](http://glew.sourceforge.net/gpl.txt).
