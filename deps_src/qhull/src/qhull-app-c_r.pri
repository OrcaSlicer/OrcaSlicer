# -------------------------------------------------
# qhull-app-c_r.pri -- Qt include project for C qhull applications linked to qhullstatic_r
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
   LIBS += -lqhullstatic_rd
   OBJECTS_DIR = Debug
}else:build_pass:CONFIG(release, debug|release){
   LIBS += -lqhullstatic_r
   OBJECTS_DIR = Release
}
win32-msvc* : QMAKE_LFLAGS += /INCREMENTAL:NO

INCLUDEPATH += ..
CONFIG += qhull_warn_conversion

