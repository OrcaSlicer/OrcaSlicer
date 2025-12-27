# -------------------------------------------------
# user_eg3.pro -- Qt project for cpp demonstration user_eg3.exe
#
# The C++ interface requires reentrant Qhull.
# -------------------------------------------------

include(../qhull-app-cpp.pri)

TARGET = user_eg3
CONFIG -= qt

SOURCES += user_eg3_r.cpp
