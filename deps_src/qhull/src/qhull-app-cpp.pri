# -------------------------------------------------
# qhull-app-cpp.pri -- Qt include project for qhull as C++ classes
# -------------------------------------------------

include(qhull-warn.pri)

DESTDIR = ../../bin
TEMPLATE = app
CONFIG += console warn_on
CONFIG -= rtti
LIBS += -L../../lib
build_pass:CONFIG(debug, debug|release){
   LIBS += -lqhullcpp_d
   LIBS += -lqhullstatic_rd  # Must be last, otherwise qh_fprintf,etc. are loaded from here instead of qhullcpp-d.lib
   OBJECTS_DIR = Debug
}else:build_pass:CONFIG(release, debug|release){
   LIBS += -lqhullcpp
   LIBS += -lqhullstatic_r  # Must be last, otherwise qh_fprintf,etc. are loaded from here instead of qhullcpp.lib
   OBJECTS_DIR = Release
}
win32-msvc* : QMAKE_LFLAGS += /INCREMENTAL:NO

INCLUDEPATH += ../../src # "libqhull_r/qhull_a.h"
