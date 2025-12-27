# -------------------------------------------------
# libqhullstatic_r.pro -- Qt project for Qhull static library
#
# It uses reeentrant Qhull
# -------------------------------------------------

include(../qhull-warn.pri)

DESTDIR = ../../lib
TEMPLATE = lib
CONFIG += staticlib warn_on
CONFIG -= qt
build_pass:CONFIG(debug, debug|release):{
    TARGET = qhullstatic_rd
    OBJECTS_DIR = Debug
}else:build_pass:CONFIG(release, debug|release):{
    TARGET = qhullstatic_r
    OBJECTS_DIR = Release
}

include(../qhull-libqhull-src_r.pri)
