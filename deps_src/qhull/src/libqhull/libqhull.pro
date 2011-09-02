# -------------------------------------------------
# libqhull.pro -- Qt project for Qhull shared library
# -------------------------------------------------

include(../qhull-warn.pri)

DESTDIR = ../../lib
DLLDESTDIR = ../../bin
TEMPLATE = lib
CONFIG += shared warn_on
CONFIG -= qt

build_pass:CONFIG(debug, debug|release):{
    TARGET = qhull_d
    OBJECTS_DIR = Debug
}else:build_pass:CONFIG(release, debug|release):{
    TARGET = qhull
    OBJECTS_DIR = Release
}
win32-msvc* : QMAKE_LFLAGS += /INCREMENTAL:NO

win32-msvc* : DEF_FILE += ../../src/libqhull/qhull-exports.def

# Order object files by frequency of execution.  Small files at end.

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

OTHER_FILES += Mborland
OTHER_FILES += qh-geom.htm
OTHER_FILES += qh-globa.htm
OTHER_FILES += qh-io.htm
OTHER_FILES += qh-mem.htm
OTHER_FILES += qh-merge.htm
OTHER_FILES += qh-poly.htm
OTHER_FILES += qh-qhull.htm
OTHER_FILES += qh-set.htm
OTHER_FILES += qh-stat.htm
OTHER_FILES += qh-user.htm
