# -------------------------------------------------
# qhull-app-shared_r.pri -- Qt include project for C qhull applications linked with libqhull_r (shared library)
#
# It uses reentrant Qhull
# -------------------------------------------------

include(qhull-warn.pri)

DESTDIR = ../../bin
TEMPLATE = app
CONFIG += console warn_on
CONFIG -= qt

LIBS += -L../../lib
build_pass:CONFIG(debug, debug|release){
   LIBS += -lqhull_rd
   OBJECTS_DIR = Debug
}else:build_pass:CONFIG(release, debug|release){
   LIBS += -lqhull_r
   OBJECTS_DIR = Release
}
win32-msvc* : QMAKE_LFLAGS += /INCREMENTAL:NO

win32-msvc* : DEFINES += qh_dllimport # libqhull_r/user.h

INCLUDEPATH += ..
CONFIG += qhull_warn_conversion


