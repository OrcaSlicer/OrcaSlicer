# -------------------------------------------------
# user_eg.pro -- Qt project for Qhull demonstration using shared Qhull library
#
# It uses reentrant Qhull
# -------------------------------------------------

include(../qhull-app-shared_r.pri)

TARGET = user_eg

SOURCES += user_eg_r.c
