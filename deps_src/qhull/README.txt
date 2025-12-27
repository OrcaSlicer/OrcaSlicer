This distribution of qhull library is only meant for interfacing qhull with Slic3rPE
(https://github.com/prusa3d/PrusaSlicer).

The qhull source file was acquired from https://github.com/qhull/qhull at revision
f0bd8ceeb84b554d7cdde9bbfae7d3351270478c.

No changes to the qhull library were made, except for
- setting REALfloat=1 in user_r.h to enforce calculations in floats
- modifying CMakeLists.txt (the original was renamed to origCMakeLists.txt)

Many thanks to C. Bradford Barber and all contributors.

Lukas Matena (lukasmatena@seznam.cz)
25.7.2018


See original contents of the README file below.

======================================================================================
======================================================================================
======================================================================================


Name

      qhull, rbox         2015.2       2016/01/18
  
Convex hull, Delaunay triangulation, Voronoi diagrams, Halfspace intersection
 
      Documentation:
        html/index.htm
        <http://www.qhull.org/html>

      Available from:
        <http://www.qhull.org>
        <http://www.qhull.org/download>
        <http://github.com/qhull/qhull> (git@github.com:qhull/qhull.git)
 
      News and a paper:
        <http://www.qhull.org/news>
        <http://citeseerx.ist.psu.edu/viewdoc/summary?doi=10.1.1.117.405>

     Version 1 (simplicial only):
        <http://www.qhull.org/download/qhull-1.0.tar.gz>
       
Purpose

  Qhull is a general dimension convex hull program that reads a set 
  of points from stdin, and outputs the smallest convex set that contains 
  the points to stdout.  It also generates Delaunay triangulations, Voronoi 
  diagrams, furthest-site Voronoi diagrams, and halfspace intersections
  about a point.

  Rbox is a useful tool in generating input for Qhull; it generates 
  hypercubes, diamonds, cones, circles, simplices, spirals, 
  lattices, and random points.
  
  Qhull produces graphical output for Geomview.  This helps with
  understanding the output. <http://www.geomview.org>

Environment requirements

  Qhull and rbox should run on all 32-bit and 64-bit computers.  Use
  an ANSI C or C++ compiler to compile the program.  The software is 
  self-contained.  It comes with examples and test scripts.
  
  Qhull's C++ interface uses the STL.  The C++ test program uses QTestLib 
  from the Qt Framework.  Qhull's C++ interface may change without 
  notice.  Eventually, it will move into the qhull shared library.
  
  Qhull is copyrighted software.  Please read COPYING.txt and REGISTER.txt
  before using or distributing Qhull.

To cite Qhull, please use

  Barber, C.B., Dobkin, D.P., and Huhdanpaa, H.T., "The Quickhull 
  algorithm for convex hulls," ACM Trans. on Mathematical Software,
  22(4):469-483, Dec 1996, http://www.qhull.org.

To modify Qhull, particularly the C++ interface

  Qhull is on GitHub 
     (http://github.com/qhull/qhull, git@github.com:qhull/qhull.git)
  
  For internal documentation, see html/qh-code.htm

To install Qhull

  Qhull is precompiled for Windows 32-bit, otherwise it needs compilation.
  
  Qhull includes Makefiles for gcc and other targets, CMakeLists.txt for CMake,
  .sln/.vcproj/.vcxproj files for Microsoft Visual Studio, and .pro files 
  for Qt Creator.  It compiles under Windows with mingw.
  
  Install and build instructions follow.  
  
  See the end of this document for a list of distributed files.

-----------------
Installing Qhull on Windows 10, 8, 7 (32- or 64-bit), Windows XP, and Windows NT

  The zip file contains rbox.exe, qhull.exe, qconvex.exe, qdelaunay.exe, 
  qhalf.exe, qvoronoi.exe, testqset.exe, user_eg*.exe, documentation files, 
  and source files.  Qhull.exe and user-eg3.exe are compiled with the reentrant 
  library while the other executables use the non-reentrant library.
  
  To install Qhull:
  - Unzip the files into a directory (e.g., named 'qhull')
  - Click on QHULL-GO or open a command window into Qhull's bin directory.
  - Test with 'rbox D4 | qhull'
    
  To uninstall Qhull
  - Delete the qhull directory
  
  To learn about Qhull:
  - Execute 'qconvex' for a synopsis and examples.
  - Execute 'rbox 10 | qconvex' to compute the convex hull of 10 random points.
  - Execute 'rbox 10 | qconvex i TO file' to write results to 'file'.
  - Browse the documentation: qhull\html\index.htm
  - If an error occurs, Windows sends the error to stdout instead of stderr.
    Use 'TO xxx' to send normal output to xxx

  To improve the command window
  - Double-click the window bar to increase the size of the window
  - Right-click the window bar
  - Select Properties
  - Check QuickEdit Mode
    Select text with right-click or Enter
    Paste text with right-click
  - Change Font to Lucinda Console
  - Change Layout to Screen Buffer Height 999, Window Size Height 55
  - Change Colors to Screen Background White, Screen Text Black
  - Click OK
  - Select 'Modify shortcut that started this window', then OK

  If you use qhull a lot, install a bash shell such as
    MSYS (www.mingw.org/wiki/msys), Road Bash (www.qhull.org/bash), 
    or Cygwin (www.cygwin.com).

-----------------
Installing Qhull on Unix with gcc

  To build Qhull, static libraries, shared library, and C++ interface
  - Download and extract Qhull (either GitHub, .tgz file, or .zip file)
  - make
  - export LD_LIBRARY_PATH=$PWD/lib:$LD_LIBRARY_PATH

  The Makefiles may be edited for other compilers.
  If 'testqset' exits with an error, qhull is broken
  
  A simple Makefile for Qhull is in src/libqhull and src/libqhull_r.
  To build the Qhull executables and libqhullstatic
  - Extract Qhull from qhull...tgz or qhull...zip
  - cd src/libqhull_r  # cd src/libqhull 
  - make

  
-----------------
Installing Qhull with CMake 2.6 or later

  See CMakeLists.txt for examples and further build instructions
  
  To build Qhull, static libraries, shared library, and C++ interface
  - Download and extract Qhull (either GitHub, .tgz file, or .zip file)
  - cd build
  - cmake --help  # List build generators
  - make -G "<generator>" .. && cmake ..  
  - cmake ..
  - make
  - make install

  The ".." is important.  It refers to the parent directory (i.e., qhull/)

  On Windows, CMake installs to C:/Program Files/qhull.  64-bit generators
  have a "Win64" tag.
  
  If creating a qhull package, please include a pkg-config file based on build/qhull*.pc.in

  If cmake fails with "No CMAKE_C_COMPILER could be found"
  - cmake was not able to find the build environment specified by -G "..."
  
-----------------
Installing Qhull with Qt

  To build Qhull, including its C++ test (qhulltest)
  - Download and extract Qhull (either GitHub, .tgz file, or .zip file)
  - Load src/qhull-all.pro into QtCreator
  - Build

-------------------
Working with Qhull's C++ interface

  See html/qh-code.htm#cpp for calling Qhull from C++ programs
  
  See html/qh-code.htm#reentrant for converting from Qhull-2012

  Examples of using the C++ interface
    user_eg3_r.cpp
    qhulltest/*_test.cpp

  Qhull's C++ interface is likely to change.  Stay current with GitHub.

  To clone Qhull's next branch from http://github.com/qhull/qhull
    git init
    git clone git@github.com:qhull/qhull.git
    cd qhull
    git checkout next
    ...
    git pull origin next
    
  Compile qhullcpp and libqhullstatic_r with the same compiler.  Both libraries
  use the C routines setjmp() and longjmp() for error handling.  They must 
  be compiled with the same compiler.
  
-------------------
Calling Qhull from C programs

  See html/qh-code.htm#library for calling Qhull from C programs

  See html/qh-code.htm#reentrant for converting from Qhull-2012

  Warning: You will need to understand Qhull's data structures and read the 
  code.  Most users will find it easier to call Qhull as an external command.

  The new, reentrant 'C' code (src/libqhull_r), passes a pointer to qhT 
  to most Qhull routines.  This allows multiple instances of Qhull to run 
  at the same time.  It simplifies the C++ interface.

  The non-reentrant 'C' code (src/libqhull) looks unusual.  It refers to 
  Qhull's global data structure, qhT, through a 'qh' macro (e.g., 'qh ferr'). 
  This allows the same code to use static memory or heap memory. 
  If qh_QHpointer is defined, qh_qh is a pointer to an allocated qhT; 
  otherwise qh_qh is a global static data structure of type qhT.

------------------
Compiling Qhull with Microsoft Visual C++

  To compile 32-bit Qhull with Microsoft Visual C++ 2010 and later
  - Download and extract Qhull (either GitHub, .tgz file, or .zip file)
  - Load solution build/qhull-32.sln 
  - Build target 'Win32'
  - Project qhulltest requires Qt for DevStudio (http://www.qt.io)
    Set the QTDIR environment variable to your Qt directory (e.g., c:/qt/5.2.0/5.2.0/msvc2012)
    If QTDIR is incorrect, precompile will fail with 'Can not locate the file specified'

  To compile 64-bit Qhull with Microsoft Visual C++ 2010 and later
  - 64-bit Qhull has larger data structures due to 64-bit pointers
  - Download and extract Qhull (either GitHub, .tgz file, or .zip file)
  - Load solution build/qhull-64.sln 
  - Build target 'Win32'
  - Project qhulltest requires Qt for DevStudio (http://www.qt.io)
    Set the QTDIR environment variable to your Qt directory (e.g., c:/qt/5.2.0/5.2.0/msvc2012_64)
    If QTDIR is incorrect, precompile will fail with 'Can not locate the file specified'
  
  To compile Qhull with Microsoft Visual C++ 2005 (vcproj files)
  - Download and extract Qhull (either GitHub, .tgz file, or .zip file)
  - Load solution build/qhull.sln 
  - Build target 'win32' (not 'x64')
  - Project qhulltest requires Qt for DevStudio (http://www.qt.io)
    Set the QTDIR environment variable to your Qt directory (e.g., c:/qt/4.7.4)
    If QTDIR is incorrect, precompile will fail with 'Can not locate the file specified'
  
-----------------
Compiling Qhull with Qt Creator

  Qt (http://www.qt.io) is a C++ framework for Windows, Linux, and Macintosh

  Qhull uses QTestLib to test qhull's C++ interface (see src/qhulltest/)
  
  To compile Qhull with Qt Creator
  - Download and extract Qhull (either GitHub, .tgz file, or .zip file)
  - Download the Qt SDK
  - Start Qt Creator
  - Load src/qhull-all.pro
  - Build

-----------------
Compiling Qhull with mingw on Windows

  To compile Qhull with MINGW
  - Download and extract Qhull (either GitHub, .tgz file, or .zip file)
  - Install Road Bash (http://www.qhull.org/bash)
    or install MSYS (http://www.mingw.org/wiki/msys)
  - Install MINGW-w64 (http://sourceforge.net/projects/mingw-w64).  
    Mingw is included with Qt SDK.  
  - make
  
-----------------
Compiling Qhull with cygwin on Windows

  To compile Qhull with cygwin
  - Download and extract Qhull (either GitHub, .tgz file, or .zip file)
  - Install cygwin (http://www.cygwin.com)
  - Include packages for gcc, make, ar, and ln
  - make

-----------------
Compiling from Makfile without gcc

  The file, qhull-src.tgz, contains documentation and source files for
  qhull and rbox.  
  
  To unpack the tgz file
  - tar zxf qhull-src.tgz
  - cd qhull
  - Use qhull/Makefile
   Simpler Makefiles are qhull/src/libqhull/Makefile and qhull/src/libqhull_r/Makefile
  
  Compiling qhull and rbox with Makefile
  - in Makefile, check the CC, CCOPTS1, PRINTMAN, and PRINTC defines
      - the defaults are gcc and enscript
      - CCOPTS1 should include the ANSI flag.  It defines __STDC__
  - in user.h, check the definitions of qh_SECticks and qh_CPUclock.
      - use '#define qh_CLOCKtype 2' for timing runs longer than 1 hour
  - type: make 
      - this builds: qhull qconvex qdelaunay qhalf qvoronoi rbox libqhull.a libqhull_r.a
  - type: make doc
      - this prints the man page
      - See also qhull/html/index.htm
  - if your compiler reports many errors, it is probably not a ANSI C compiler
      - you will need to set the -ansi switch or find another compiler
  - if your compiler warns about missing prototypes for fprintf() etc.
      - this is ok, your compiler should have these in stdio.h
  - if your compiler warns about missing prototypes for memset() etc.
      - include memory.h in qhull_a.h
  - if your compiler reports "global.c: storage size of 'qh_qh' isn't known"
      - delete the initializer "={0}" in global.c, stat.c and mem.c
  - if your compiler warns about "stat.c: improper initializer"
      - this is ok, the initializer is not used
  - if you have trouble building libqhull.a with 'ar'
      - try 'make -f Makefile.txt qhullx' 
  - if the code compiles, the qhull test case will automatically execute
  - if an error occurs, there's an incompatibility between machines
      - If you can, try a different compiler 
      - You can turn off the Qhull memory manager with qh_NOmem in mem.h
      - You can turn off compiler optimization (-O2 in Makefile)
      - If you find the source of the problem, please let us know
  - to install the programs and their man pages:
      - define MANDIR and BINDIR
      - type 'make install'

  - if you have Geomview (www.geomview.org)
       - try  'rbox 100 | qconvex G >a' and load 'a' into Geomview
       - run 'q_eg' for Geomview examples of Qhull output (see qh-eg.htm)

------------------
Compiling on other machines and compilers

  Qhull may compile with Borland C++ 5.0 bcc32.  A Makefile is included.
  Execute 'cd src/libqhull; make -f Mborland'.  If you use the Borland IDE, set
  the ANSI option in Options:Project:Compiler:Source:Language-compliance.
  
  Qhull may compile with Borland C++ 4.02 for Win32 and DOS Power Pack.  
  Use 'cd src/libqhull; make -f Mborland -D_DPMI'.  Qhull 1.0 compiles with 
  Borland C++ 4.02.  For rbox 1.0, use "bcc32 -WX -w- -O2-e -erbox -lc rbox.c".  
  Use the same options for Qhull 1.0. [D. Zwick]
  
  If you have troubles with the memory manager, you can turn it off by
  defining qh_NOmem in mem.h.

-----------------
Distributed files

  README.txt           // Instructions for installing Qhull 
  REGISTER.txt         // Qhull registration 
  COPYING.txt          // Copyright notice 
  QHULL-GO.lnk         // Windows icon for eg/qhull-go.bat
  Announce.txt         // Announcement 
  CMakeLists.txt       // CMake build file (2.6 or later)
  CMakeModules/CheckLFS.cmake // enables Large File Support in cmake
  File_id.diz          // Package descriptor
  index.htm            // Home page 
  Makefile             // Makefile for gcc and other compilers
  qhull*.md5sum        // md5sum for all files

  bin/*                // Qhull executables and dll (.zip only)
  build/qhull*.pc.in   // pkg-config templates for qhull_r, qhull, and qhull_p
  build/qhull-32.sln   // 32-bit DevStudio solution and project files (2010 and later)
  build/*-32.vcxproj
  build/qhull-64.sln   // 64-bit DevStudio solution and project files (2010 and later)
  build/*-64.vcxproj
  build/qhull.sln      // DevStudio solution and project files (2005 and 2009)
  build/*.vcproj
  eg/*                 // Test scripts and geomview files from q_eg
  html/index.htm       // Manual
  html/qh-faq.htm      // Frequently asked questions
  html/qh-get.htm      // Download page
  html/qhull-cpp.xml   // C++ style notes as a Road FAQ (www.qhull.org/road)
  src/Changes.txt      // Change history for Qhull and rbox 
  src/qhull-all.pro    // Qt project

eg/ 
  q_eg                 // shell script for Geomview examples (eg.01.cube)
  q_egtest             // shell script for Geomview test examples
  q_test               // shell script to test qhull
  q_test-ok.txt        // output from q_test
  qhulltest-ok.txt     // output from qhulltest (Qt only)
  make-vcproj.sh       // bash shell script to create vcproj and vcxprog files
  qhull-zip.sh	       // bash shell script for distribution files

rbox consists of (bin, html):
  rbox.exe             // Win32 executable (.zip only) 
  rbox.htm             // html manual 
  rbox.man             // Unix man page 
  rbox.txt

qhull consists of (bin, html):
  qconvex.exe          // Win32 executables and dlls (.zip download only) 
  qhull.exe            // Built with the reentrant library (about 2% slower)
  qdelaunay.exe
  qhalf.exe
  qvoronoi.exe
  qhull_r.dll
  qhull-go.bat         // command window
  qconvex.htm          // html manual
  qdelaun.htm
  qdelau_f.htm        
  qhalf.htm
  qvoronoi.htm
  qvoron_f.htm
  qh-eg.htm
  qh-code.htm
  qh-impre.htm
  index.htm
  qh-opt*.htm
  qh-quick.htm
  qh--*.gif            // images for manual
  normal_voronoi_knauss_oesterle.jpg
  qhull.man            // Unix man page 
  qhull.txt

bin/
  msvcr80.dll          // Visual C++ redistributable file (.zip download only)

src/
  qhull/unix.c         // Qhull and rbox applications using non-reentrant libqhullstatic.a
  rbox/rbox.c
  qconvex/qconvex.c    
  qhalf/qhalf.c
  qdelaunay/qdelaunay.c
  qvoronoi/qvoronoi.c

  qhull/unix_r.c        // Qhull and rbox applications using reentrant libqhullstatic_r.a
  rbox/rbox_r.c
  qconvex/qconvex_r.c   // Qhull applications built with reentrant libqhull_r/Makefile  
  qhalf/qhalf_r.c
  qdelaunay/qdelaun_r.c
  qvoronoi/qvoronoi_r.c

  user_eg/user_eg_r.c     // example of using qhull_r.dll from a user program
  user_eg2/user_eg2_r.c   // example of using libqhullstatic_r.a from a user program
  user_eg3/user_eg3_r.cpp // example of Qhull's C++ interface libqhullcpp with libqhullstatic_r.a
  qhulltest/qhulltest.cpp // Test of Qhull's C++ interface using Qt's QTestLib
  qhull-*.pri             // Include files for Qt projects
  testqset_r/testqset_r.c  // Test of reentrant qset_r.c and mem_r.c
  testqset/testqset.c     // Test of non-rentrant qset.c and mem.c


src/libqhull
  libqhull.pro         // Qt project for non-rentrant, shared library (qhull.dll)
  index.htm            // design documentation for libqhull
  qh-*.htm
  qhull-exports.def    // Export Definition file for Visual C++
  Makefile             // Simple gcc Makefile for qhull and libqhullstatic.a
  Mborland             // Makefile for Borland C++ 5.0

  libqhull.h           // header file for qhull
  user.h               // header file of user definable constants 
  libqhull.c           // Quickhull algorithm with partitioning
  user.c               // user re-definable functions 
  usermem.c
  userprintf.c
  userprintf_rbox.c

  qhull_a.h            // include files for libqhull/*.c 
  geom.c               // geometric routines 
  geom2.c
  geom.h     
  global.c             // global variables 
  io.c                 // input-output routines 
  io.h   
  mem.c                // memory routines, this is stand-alone code 
  mem.h
  merge.c              // merging of non-convex facets 
  merge.h
  poly.c               // polyhedron routines 
  poly2.c
  poly.h 
  qset.c               // set routines, this only depends on mem.c 
  qset.h
  random.c             // utilities w/ Park & Miller's random number generator
  random.h
  rboxlib.c            // point set generator for rbox
  stat.c               // statistics 
  stat.h

src/libqhull_r
  libqhull_r.pro       // Qt project for rentrant, shared library (qhull_r.dll)
  index.htm            // design documentation for libqhull_r
  qh-*_r.htm
  qhull-exports_r.def  // Export Definition file for Visual C++
  Makefile             // Simple gcc Makefile for qhull and libqhullstatic.a

  libqhull_r.h          // header file for qhull
  user_r.h              // header file of user definable constants 
  libqhull_r.c          // Quickhull algorithm wi_r.hpartitioning
  user_r.c              // user re-definable functions 
  usermem.c
  userprintf.c
  userprintf_rbox.c
  qhull_ra.h            // include files for libqhull/*_r.c
  geom_r.c              // geometric routines 
  geom2.c
  geom_r.h    
  global_r.c            // global variables 
  io_r.c                // input-output routines 
  io_r.h  
  mem_r.c               // memory routines, this is stand-alone code 
  mem.h
  merge_r.c             // merging of non-convex facets 
  merge.h
  poly_r.c              // polyhedron routines 
  poly2.c
  poly_r.h
  qset_r.c              // set routines, this only depends on mem_r.c
  qset.h
  random_r.c            // utilities w/ Park & Miller's random number generator
  random.h
  rboxlib_r.c           // point set generator for rbox
  stat_r.c              // statistics 
  stat.h

src/libqhullcpp/
  libqhullcpp.pro      // Qt project for renentrant, static C++ library     
  Qhull.cpp            // Calls libqhull_r.c from C++
  Qhull.h
  qt-qhull.cpp         // Supporting methods for Qt
    
  Coordinates.cpp      // input classes
  Coordinates.h

  PointCoordinates.cpp
  PointCoordinates.h
  RboxPoints.cpp       // call rboxlib.c from C++
  RboxPoints.h

  QhullFacet.cpp       // data structure classes
  QhullFacet.h
  QhullHyperplane.cpp
  QhullHyperplane.h
  QhullPoint.cpp
  QhullPoint.h
  QhullQh.cpp
  QhullRidge.cpp
  QhullRidge.h
  QhullVertex.cpp
  QhullVertex.h
  
  QhullFacetList.cpp   // collection classes
  QhullFacetList.h
  QhullFacetSet.cpp
  QhullFacetSet.h
  QhullIterator.h
  QhullLinkedList.h
  QhullPoints.cpp
  QhullPoints.h
  QhullPointSet.cpp
  QhullPointSet.h
  QhullSet.cpp
  QhullSet.h
  QhullSets.h
  QhullVertexSet.cpp
  QhullVertexSet.h

  functionObjects.h    // supporting classes
  QhullError.cpp
  QhullError.h
  QhullQh.cpp
  QhullQh.h
  QhullStat.cpp
  QhullStat.h
  RoadError.cpp        // Supporting base classes
  RoadError.h
  RoadLogEvent.cpp
  RoadLogEvent.h
  usermem_r-cpp.cpp    // Optional override for qh_exit() to throw an error

src/libqhullstatic/
  libqhullstatic.pro   // Qt project for non-reentrant, static library     
     
src/libqhullstatic_r/
  libqhullstatic_r.pro // Qt project for reentrant, static library     
     
src/qhulltest/
  qhulltest.pro        // Qt project for test of C++ interface     
  Coordinates_test.cpp // Test of each class
  PointCoordinates_test.cpp
  Qhull_test.cpp
  QhullFacet_test.cpp
  QhullFacetList_test.cpp
  QhullFacetSet_test.cpp
  QhullHyperplane_test.cpp
  QhullLinkedList_test.cpp
  QhullPoint_test.cpp
  QhullPoints_test.cpp
  QhullPointSet_test.cpp
  QhullRidge_test.cpp
  QhullSet_test.cpp
  QhullVertex_test.cpp
  QhullVertexSet_test.cpp
  RboxPoints_test.cpp
  RoadTest.cpp         // Run multiple test files with QTestLib
  RoadTest.h

-----------------
Authors:

  C. Bradford Barber                  Hannu Huhdanpaa (Version 1.0)
  bradb@shore.net                     hannu@qhull.org
  
  Qhull 1.0 and 2.0 were developed under NSF grants NSF/DMS-8920161 
  and NSF-CCR-91-15793 750-7504 at the Geometry Center and Harvard 
  University.  If you find Qhull useful, please let us know.
