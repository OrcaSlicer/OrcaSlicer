# -------------------------------------------------
# qhull-libqhull-src.pri -- Qt include project for libqhull sources and headers
#   libqhull.pro, libqhullp.pro, and libqhulldll.pro are the same for SOURCES and HEADERS
# -------------------------------------------------

# Order object files by frequency of execution.  Small files at end.
# Current directory is caller

# libqhull/libqhull.pro and ../qhull-libqhull-src.pri have the same SOURCES and HEADERS
SOURCES += ../libqhull/global.c
SOURCES += ../libqhull/stat.c
SOURCES += ../libqhull/geom2.c
SOURCES += ../libqhull/poly2.c
SOURCES += ../libqhull/merge.c
SOURCES += ../libqhull/libqhull.c
SOURCES += ../libqhull/geom.c
SOURCES += ../libqhull/poly.c
SOURCES += ../libqhull/qset.c
SOURCES += ../libqhull/mem.c
SOURCES += ../libqhull/random.c
SOURCES += ../libqhull/usermem.c
SOURCES += ../libqhull/userprintf.c
SOURCES += ../libqhull/io.c
SOURCES += ../libqhull/user.c
SOURCES += ../libqhull/rboxlib.c
SOURCES += ../libqhull/userprintf_rbox.c

# [2014] qmake locates the headers in the shadow build directory not the src directory
HEADERS += ../libqhull/geom.h
HEADERS += ../libqhull/io.h
HEADERS += ../libqhull/libqhull.h
HEADERS += ../libqhull/mem.h
HEADERS += ../libqhull/merge.h
HEADERS += ../libqhull/poly.h
HEADERS += ../libqhull/random.h
HEADERS += ../libqhull/qhull_a.h
HEADERS += ../libqhull/qset.h
HEADERS += ../libqhull/stat.h
HEADERS += ../libqhull/user.h
