# -------------------------------------------------
# libqhullstatic.pro -- Qt project for Qhull static library
#   Built with qh_QHpointer=0.  See libqhullp.pro
# -------------------------------------------------

include(../qhull-warn.pri)
include(../qhull-libqhull-src.pri)

DESTDIR = ../../lib
TEMPLATE = lib
CONFIG += staticlib warn_on
CONFIG -= qt
build_pass:CONFIG(debug, debug|release):{
    TARGET = qhullstatic_d
    OBJECTS_DIR = Debug
}else:build_pass:CONFIG(release, debug|release):{
    TARGET = qhullstatic
    OBJECTS_DIR = Release
}
