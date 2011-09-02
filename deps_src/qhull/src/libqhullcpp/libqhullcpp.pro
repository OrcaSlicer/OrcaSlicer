# -------------------------------------------------
# libqhullcpp.pro -- Qt project for Qhull cpp shared library
#
# It uses reentrant Qhull
# -------------------------------------------------

include(../qhull-warn.pri)

DESTDIR = ../../lib
TEMPLATE = lib
# Do not create libqhullcpp as a shared library.  Qhull C++ classes may change layout and size. 
CONFIG += staticlib warn_on
CONFIG -= qt rtti
build_pass:CONFIG(debug, debug|release):{
   TARGET = qhullcpp_d
   OBJECTS_DIR = Debug
}else:build_pass:CONFIG(release, debug|release):{
   TARGET = qhullcpp
   OBJECTS_DIR = Release
}
MOC_DIR = moc

INCLUDEPATH += ../../src
INCLUDEPATH += $$PWD # for MOC_DIR

CONFIG += qhull_warn_shadow qhull_warn_conversion

SOURCES += ../libqhullcpp/Coordinates.cpp
SOURCES += ../libqhullcpp/PointCoordinates.cpp
SOURCES += ../libqhullcpp/Qhull.cpp
SOURCES += ../libqhullcpp/QhullFacet.cpp
SOURCES += ../libqhullcpp/QhullFacetList.cpp
SOURCES += ../libqhullcpp/QhullFacetSet.cpp
SOURCES += ../libqhullcpp/QhullHyperplane.cpp
SOURCES += ../libqhullcpp/QhullPoint.cpp
SOURCES += ../libqhullcpp/QhullPoints.cpp
SOURCES += ../libqhullcpp/QhullPointSet.cpp
SOURCES += ../libqhullcpp/QhullQh.cpp
SOURCES += ../libqhullcpp/QhullRidge.cpp
SOURCES += ../libqhullcpp/QhullSet.cpp
SOURCES += ../libqhullcpp/QhullStat.cpp
SOURCES += ../libqhullcpp/QhullVertex.cpp
SOURCES += ../libqhullcpp/QhullVertexSet.cpp
SOURCES += ../libqhullcpp/RboxPoints.cpp
SOURCES += ../libqhullcpp/RoadError.cpp
SOURCES += ../libqhullcpp/RoadLogEvent.cpp

HEADERS += ../libqhullcpp/Coordinates.h
HEADERS += ../libqhullcpp/functionObjects.h
HEADERS += ../libqhullcpp/PointCoordinates.h
HEADERS += ../libqhullcpp/Qhull.h
HEADERS += ../libqhullcpp/QhullError.h
HEADERS += ../libqhullcpp/QhullFacet.h
HEADERS += ../libqhullcpp/QhullFacetList.h
HEADERS += ../libqhullcpp/QhullFacetSet.h
HEADERS += ../libqhullcpp/QhullHyperplane.h
HEADERS += ../libqhullcpp/QhullIterator.h
HEADERS += ../libqhullcpp/QhullLinkedList.h
HEADERS += ../libqhullcpp/QhullPoint.h
HEADERS += ../libqhullcpp/QhullPoints.h
HEADERS += ../libqhullcpp/QhullPointSet.h
HEADERS += ../libqhullcpp/QhullQh.h
HEADERS += ../libqhullcpp/QhullRidge.h
HEADERS += ../libqhullcpp/QhullSet.h
HEADERS += ../libqhullcpp/QhullSets.h
HEADERS += ../libqhullcpp/QhullStat.h
HEADERS += ../libqhullcpp/QhullVertex.h
HEADERS += ../libqhullcpp/QhullVertexSet.h
HEADERS += ../libqhullcpp/RboxPoints.h
HEADERS += ../libqhullcpp/RoadError.h
HEADERS += ../libqhullcpp/RoadLogEvent.h
