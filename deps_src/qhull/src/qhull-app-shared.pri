# -------------------------------------------------
# qhull-app-shared.pri -- Deprecated Qt include project for C qhull applications linked with libqhull (shared library)
# -------------------------------------------------

include(qhull-warn.pri)

DESTDIR = ../../bin
TEMPLATE = app
CONFIG += console warn_on
CONFIG -= qt

LIBS += -L../../lib
build_pass:CONFIG(debug, debug|release){
   LIBS += -lqhull_d
   OBJECTS_DIR = Debug
}else:build_pass:CONFIG(release, debug|release){
   LIBS += -lqhull
   OBJECTS_DIR = Release
}
win32-msvc* : QMAKE_LFLAGS += /INCREMENTAL:NO

win32-msvc* : DEFINES += qh_dllimport # libqhull/user.h

INCLUDEPATH += ../libqhull
CONFIG += qhull_warn_conversion


