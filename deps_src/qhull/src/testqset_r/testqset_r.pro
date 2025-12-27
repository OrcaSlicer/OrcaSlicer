# -------------------------------------------------
# testqset_r.pro -- Qt project file for testqset_r.exe
# -------------------------------------------------

include(../qhull-warn.pri)

TARGET = testqset_r

DESTDIR = ../../bin
TEMPLATE = app
CONFIG += console warn_on
CONFIG -= qt
CONFIG += qhull_warn_conversion

build_pass:CONFIG(debug, debug|release){
   OBJECTS_DIR = Debug
}else:build_pass:CONFIG(release, debug|release){
   OBJECTS_DIR = Release
}

INCLUDEPATH += ..

SOURCES += testqset_r.c
SOURCES += ../libqhull_r/qset_r.c
SOURCES += ../libqhull_r/mem_r.c
SOURCES += ../libqhull_r/usermem_r.c

HEADERS += ../libqhull_r/mem_r.h
HEADERS += ../libqhull_r/qset_r.h

