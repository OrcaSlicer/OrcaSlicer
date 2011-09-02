/****************************************************************************
**
** Copyright (c) 2009-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/libqhullcpp/PointCoordinates.h#4 $$Change: 2079 $
** $DateTime: 2016/02/07 17:43:34 $$Author: bbarber $
**
****************************************************************************/

#ifndef QHPOINTCOORDINATES_H
#define QHPOINTCOORDINATES_H

#include "libqhull_r/qhull_ra.h"
#include "libqhullcpp/QhullPoints.h"
#include "libqhullcpp/Coordinates.h"

#include <ostream>
#include <string>

#ifndef QHULL_NO_STL
#include <vector>
#endif

namespace orgQhull {

#//!\name Defined here
    //! QhullPoints with Coordinates and description
    //! Inherited by RboxPoints
    class PointCoordinates;

class PointCoordinates : public QhullPoints {

private:
#//!\name Fields
    Coordinates         point_coordinates;      //! std::vector of point coordinates
                                                //! may have extraCoordinates()
    std::string         describe_points;          //! Comment describing PointCoordinates

public:
#//!\name Construct
    //! QhullPoint, PointCoordinates, and QhullPoints have similar constructors
    //! If Qhull/QhullQh is not initialized, then dimension()==0                        PointCoordinates();
                        PointCoordinates();
    explicit            PointCoordinates(const std::string &aComment);
                        PointCoordinates(int pointDimension, const std::string &aComment);
                        //! Qhull/QhullQh used for dimension() and QhullPoint equality
    explicit            PointCoordinates(const Qhull &q);
                        PointCoordinates(const Qhull &q, const std::string &aComment);
                        PointCoordinates(const Qhull &q, int pointDimension, const std::string &aComment);
                        PointCoordinates(const Qhull &q, int pointDimension, const std::string &aComment, countT coordinatesCount, const coordT *c); // may be invalid
                        //! Use append() and appendPoints() for Coordinates and vector<coordT>
    explicit            PointCoordinates(QhullQh *qqh);
                        PointCoordinates(QhullQh *qqh, const std::string &aComment);
                        PointCoordinates(QhullQh *qqh, int pointDimension, const std::string &aComment);
                        PointCoordinates(QhullQh *qqh, int pointDimension, const std::string &aComment, countT coordinatesCount, const coordT *c); // may be invalid
                        //! Use append() and appendPoints() for Coordinates and vector<coordT>
                        PointCoordinates(const PointCoordinates &other);
    PointCoordinates &  operator=(const PointCoordinates &other);
                        ~PointCoordinates();

#//!\name Convert
    //! QhullPoints coordinates, constData, data, count, size
#ifndef QHULL_NO_STL
    void                append(const std::vector<coordT> &otherCoordinates) { if(!otherCoordinates.empty()){ append((int)otherCoordinates.size(), &otherCoordinates[0]); } }
    std::vector<coordT> toStdVector() const { return point_coordinates.toStdVector(); }
#endif //QHULL_NO_STL
#ifdef QHULL_USES_QT
    void                append(const QList<coordT> &pointCoordinates) { if(!pointCoordinates.isEmpty()){ append(pointCoordinates.count(), &pointCoordinates[0]); } }
    QList<coordT>       toQList() const { return point_coordinates.toQList(); }
#endif //QHULL_USES_QT

#//!\name GetSet
    //! See QhullPoints for coordinates, coordinateCount, dimension, empty, isEmpty, ==, !=
    void                checkValid() const;
    std::string         comment() const { return describe_points; }
    void                makeValid() { defineAs(point_coordinates.count(), point_coordinates.data()); }
    const Coordinates & getCoordinates() const { return point_coordinates; }
    void                setComment(const std::string &s) { describe_points= s; }
    void                setDimension(int i);

private:
    //! disable QhullPoints.defineAs()
    void                defineAs(countT coordinatesCount, coordT *c) { QhullPoints::defineAs(coordinatesCount, c); }
public:

#//!\name ElementAccess
    //! See QhullPoints for at, back, first, front, last, mid, [], value

#//!\name Foreach
    //! See QhullPoints for begin, constBegin, end
    Coordinates::ConstIterator  beginCoordinates() const { return point_coordinates.begin(); }
    Coordinates::Iterator       beginCoordinates() { return point_coordinates.begin(); }
    Coordinates::ConstIterator  beginCoordinates(countT pointIndex) const;
    Coordinates::Iterator       beginCoordinates(countT pointIndex);
    Coordinates::ConstIterator  endCoordinates() const { return point_coordinates.end(); }
    Coordinates::Iterator       endCoordinates() { return point_coordinates.end(); }

#//!\name Search
    //! See QhullPoints for contains, count, indexOf, lastIndexOf

#//!\name GetSet
    PointCoordinates    operator+(const PointCoordinates &other) const;

#//!\name Modify
    //FIXUP QH11001: Add clear() and other modify operators from Coordinates.h.  Include QhullPoint::operator=()
    void                append(countT coordinatesCount, const coordT *c);  //! Dimension previously defined
    void                append(const coordT &c) { append(1, &c); } //! Dimension previously defined
    void                append(const QhullPoint &p);
    //! See convert for std::vector and QList
    void                append(const Coordinates &c) { append(c.count(), c.data()); }
    void                append(const PointCoordinates &other);
    void                appendComment(const std::string &s);
    void                appendPoints(std::istream &in);
    PointCoordinates &  operator+=(const PointCoordinates &other) { append(other); return *this; }
    PointCoordinates &  operator+=(const coordT &c) { append(c); return *this; }
    PointCoordinates &  operator+=(const QhullPoint &p) { append(p); return *this; }
    PointCoordinates &  operator<<(const PointCoordinates &other) { return *this += other; }
    PointCoordinates &  operator<<(const coordT &c) { return *this += c; }
    PointCoordinates &  operator<<(const QhullPoint &p) { return *this += p; }
    // reserve() is non-const
    void                reserveCoordinates(countT newCoordinates);

#//!\name Helpers
private:
    int                 indexOffset(int i) const;

};//PointCoordinates

// No references to QhullPoint.  Prevents use of QHULL_DECLARE_SEQUENTIAL_ITERATOR(PointCoordinates, QhullPoint)
class PointCoordinatesIterator
{
    typedef PointCoordinates::const_iterator const_iterator;

private:
    const PointCoordinates *c;
    const_iterator      i;

public:
                        PointCoordinatesIterator(const PointCoordinates &container) : c(&container), i(c->constBegin()) {}
                        PointCoordinatesIterator &operator=(const PointCoordinates &container) { c = &container; i = c->constBegin(); return *this; }

    void                toFront() { i = c->constBegin(); }
    void                toBack() { i = c->constEnd(); }
    bool                hasNext() const { return i != c->constEnd(); }
    const QhullPoint    next() { return *i++; }
    const QhullPoint    peekNext() const { return *i; }
    bool                hasPrevious() const { return i != c->constBegin(); }
    const QhullPoint    previous() { return *--i; }
    const QhullPoint    peekPrevious() const { const_iterator p = i; return *--p; }
    bool                findNext(const QhullPoint &t) { while(i != c->constEnd()){ if (*i++ == t) return true;} return false; }
    bool                findPrevious(const QhullPoint &t) { while(i != c->constBegin()){ if (*(--i) == t) return true;} return false;  }
};//CoordinatesIterator

// FIXUP QH11002:  Add MutablePointCoordinatesIterator after adding modify operators
\
}//namespace orgQhull

#//!\name Global

std::ostream &          operator<<(std::ostream &os, const orgQhull::PointCoordinates &p);

#endif // QHPOINTCOORDINATES_H
