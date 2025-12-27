# -------------------------------------------------
# qhull-all.pro -- Qt project to build executables and static libraries
#
# To build with Qt on mingw
#   Download Qt SDK, install Perl
#   /c/qt/2010.05/qt> ./configure -static -platform win32-g++ -fast -no-qt3support
#
# To build DevStudio sln and proj files (Qhull ships with cmake derived files)
# qmake is in Qt's bin directory
# mkdir -p build && cd build && qmake -tp vc -r ../src/qhull-all.pro
# Additional Library Directories -- C:\qt\Qt5.2.0\5.2.0\msvc2012_64\lib
# libqhullcpp and libqhullstatic refered to $(QTDIR) but apparently didn't retrieve (should be %QTDIR%?)
# libqhull_r also needs ..\..\lib
# Need to change build/x64/Debug/*.lib to lib/ (or copy libs by hand, each time)
# Additional Build Dependencies
# See README.txt -- Need to add Build Dependencies, disable rtti, rename targets to qhull.dll, qhull6_p.dll and qhull6_pd.dll
# -------------------------------------------------

TEMPLATE = subdirs
CONFIG += ordered

SUBDIRS += libqhull_r      #shared library with reentrant code
SUBDIRS += libqhullstatic  #static library
SUBDIRS += libqhullstatic_r #static library with reentrant code
SUBDIRS += libqhullcpp     #static library for C++ interface with libqhullstatic_r

SUBDIRS += qhull           #qhull program linked to libqhullstatic_r
SUBDIRS += rbox         
SUBDIRS += qconvex         #qhull programs linked to libqhullstatic
SUBDIRS += qdelaunay
SUBDIRS += qhalf
SUBDIRS += qvoronoi

SUBDIRS += user_eg         #user programs linked to libqhull_r
SUBDIRS += user_eg2  
SUBDIRS += user_eg3        #user program with libqhullcpp and libqhullstatic_r

SUBDIRS += qhulltest       #C++ test program with Qt, libqhullcpp, and libqhullstatic_r
SUBDIRS += testqset        #test program for qset.c with mem.c
SUBDIRS += testqset_r      #test program for qset_r.c with mem_r.c
                           #See eg/q_test for qhull tests

OTHER_FILES += Changes.txt
OTHER_FILES += CMakeLists.txt
OTHER_FILES += Make-config.sh
OTHER_FILES += ../Announce.txt
OTHER_FILES += ../CMakeLists.txt
OTHER_FILES += ../COPYING.txt
OTHER_FILES += ../File_id.diz
OTHER_FILES += ../index.htm
OTHER_FILES += ../Makefile
OTHER_FILES += ../README.txt
OTHER_FILES += ../REGISTER.txt
OTHER_FILES += ../eg/q_eg
OTHER_FILES += ../eg/q_egtest
OTHER_FILES += ../eg/q_test
OTHER_FILES += ../html/index.htm
OTHER_FILES += ../html/qconvex.htm
OTHER_FILES += ../html/qdelau_f.htm
OTHER_FILES += ../html/qdelaun.htm
OTHER_FILES += ../html/qhalf.htm
OTHER_FILES += ../html/qh-code.htm
OTHER_FILES += ../html/qh-eg.htm
OTHER_FILES += ../html/qh-faq.htm
OTHER_FILES += ../html/qh-get.htm
OTHER_FILES += ../html/qh-impre.htm
OTHER_FILES += ../html/qh-optc.htm
OTHER_FILES += ../html/qh-optf.htm
OTHER_FILES += ../html/qh-optg.htm
OTHER_FILES += ../html/qh-opto.htm
OTHER_FILES += ../html/qh-optp.htm
OTHER_FILES += ../html/qh-optq.htm
OTHER_FILES += ../html/qh-optt.htm
OTHER_FILES += ../html/qh-quick.htm
OTHER_FILES += ../html/qhull.htm
OTHER_FILES += ../html/qhull.man
OTHER_FILES += ../html/qhull.txt
OTHER_FILES += ../html/qhull-cpp.xml
OTHER_FILES += ../html/qvoron_f.htm
OTHER_FILES += ../html/qvoronoi.htm
OTHER_FILES += ../html/rbox.htm
OTHER_FILES += ../html/rbox.man
OTHER_FILES += ../html/rbox.txt
OTHER_FILES += ../src/libqhull/Makefile
OTHER_FILES += ../src/libqhull_r/Makefile
OTHER_FILES += ../src/libqhull_r/qhull_r-exports.def
OTHER_FILES += ../src/qconvex/qconvex_r.c
OTHER_FILES += ../src/qdelaunay/qdelaun_r.c
OTHER_FILES += ../src/qhalf/qhalf_r.c
OTHER_FILES += ../src/qhull/rbox_r.c
OTHER_FILES += ../src/qvoronoi/qvoronoi_r.c
OTHER_FILES += ../src/qhull/unix.c
OTHER_FILES += ../src/user_eg/user_eg.c
OTHER_FILES += ../src/user_eg2/user_eg2.c
