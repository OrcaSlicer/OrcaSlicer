# -------------------------------------------------
# testqset.pro -- Qt project file for testqset.exe
# -------------------------------------------------

include(../qhull-warn.pri)

TARGET = testqset

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

SOURCES += testqset.c
SOURCES += ../libqhull/qset.c
SOURCES += ../libqhull/mem.c
SOURCES += ../libqhull/usermem.c

HEADERS += ../libqhull/mem.h
HEADERS += ../libqhull/qset.h

