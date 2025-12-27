/****************************************************************************
**
** Copyright (c) 2009-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/libqhullcpp/QhullPoint.h#4 $$Change: 2079 $
** $DateTime: 2016/02/07 17:43:34 $$Author: bbarber $
**
****************************************************************************/

#ifndef QHPOINT_H
#define QHPOINT_H

#include "libqhull_r/qhull_ra.h"
#include "libqhullcpp/QhullError.h"
#include "libqhullcpp/QhullIterator.h"
#include "libqhullcpp/QhullQh.h"
#include "libqhullcpp/Coordinates.h"

#include <ostream>

namespace orgQhull {

#//!\name Defined here
    class QhullPoint;  //!<  QhullPoint as a pointer and dimension to shared memory
    class QhullPointIterator; //!< Java-style iterator for QhullPoint coordinates

#//!\name Used here
    class Qhull;

//! A QhullPoint is a dimension and an array of coordinates.
//! With Qhull/QhullQh, a QhullPoint has an identifier.  Point equality is relative to qh.distanceEpsilon
class QhullPoint {

#//!\name Iterators
public:
    typedef coordT *                    base_type;  // for QhullPointSet
    typedef const coordT *              iterator;
    typedef const coordT *              const_iterator;
    typedef QhullPoint::iterator        Iterator;
    typedef QhullPoint::const_iterator  ConstIterator;

#//!\name Fields
protected: // For QhullPoints::iterator, QhullPoints::const_iterator
    coordT *            point_coordinates;  //!< Pointer to first coordinate,   0 if undefined
    QhullQh *           qh_qh;              //!< qhT for this instance of Qhull.  0 if undefined.
                                            //!< operator==() returns true if points within sqrt(qh_qh->distanceEpsilon())
                                            //!< If !qh_qh, id() is -3, and operator==() requires equal coordinates
    int                 point_dimension;    //!< Default dimension is qh_qh->hull_dim
public:

#//!\name Constructors
    //! QhullPoint, PointCoordinates, and QhullPoints have similar constructors
    //! If Qhull/QhullQh is not initialized, then QhullPoint.dimension() is zero unless explicitly set
    //! Cannot define QhullPoints(int pointDimension) since it is ambiguous with QhullPoints(QhullQh *qqh)
                        QhullPoint() : point_coordinates(0), qh_qh(0), point_dimension(0) {}
                        QhullPoint(int pointDimension, coordT *c) : point_coordinates(c), qh_qh(0), point_dimension(pointDimension) { QHULL_ASSERT(pointDimension>0); }
    explicit            QhullPoint(const Qhull &q);
                        QhullPoint(const Qhull &q, coordT *c);
                        QhullPoint(const Qhull &q, Coordinates &c);
                        QhullPoint(const Qhull &q, int pointDimension, coordT *c);
    explicit            QhullPoint(QhullQh *qqh) : point_coordinates(0), qh_qh(qqh), point_dimension(qqh->hull_dim) {}
                        QhullPoint(QhullQh *qqh, coordT *c) : point_coordinates(c), qh_qh(qqh), point_dimension(qqh->hull_dim) { QHULL_ASSERT(qqh->hull_dim>0); }
                        QhullPoint(QhullQh *qqh, Coordinates &c) : point_coordinates(c.data()), qh_qh(qqh), point_dimension(c.count()) {}
                        QhullPoint(QhullQh *qqh, int pointDimension, coordT *c) : point_coordinates(c), qh_qh(qqh), point_dimension(pointDimension) {}
                        //! Creates an alias.  Does not make a deep copy of the point.  Needed for return by value and parameter passing.
                        QhullPoint(const QhullPoint &other) : point_coordinates(other.point_coordinates), qh_qh(other.qh_qh), point_dimension(other.point_dimension) {}
                        //! Creates an alias.  Does not make a deep copy of the point.  Needed for vector<QhullPoint>
    QhullPoint &        operator=(const QhullPoint &other) { point_coordinates= other.point_coordinates; qh_qh= other.qh_qh; point_dimension= other.point_dimension; return *this; }
                        ~QhullPoint() {}


#//!\name Conversions

#ifndef QHULL_NO_STL
    std::vector<coordT> toStdVector() const;
#endif //QHULL_NO_STL
#ifdef QHULL_USES_QT
    QList<coordT>       toQList() const;
#endif //QHULL_USES_QT

#//!\name GetSet
public:
    const coordT *      coordinates() const { return point_coordinates; }  //!< 0 if undefined
    coordT *            coordinates() { return point_coordinates; }        //!< 0 if undefined
    void                defineAs(coordT *c) { QHULL_ASSERT(point_dimension>0); point_coordinates= c; }
    void                defineAs(int pointDimension, coordT *c) { QHULL_ASSERT(pointDimension>=0); point_coordinates= c; point_dimension= pointDimension; }
    void                defineAs(QhullPoint &other) { point_coordinates= other.point_coordinates; qh_qh= other.qh_qh; point_dimension= other.point_dimension; }
    int                 dimension() const { return point_dimension; }
    coordT *            getBaseT() const { return point_coordinates; } // for QhullPointSet
    countT              id() const { return qh_pointid(qh_qh, point_coordinates); } // NOerrors
    bool                isValid() const { return (point_coordinates!=0 && point_dimension>0); };
    bool                operator==(const QhullPoint &other) const;
    bool                operator!=(const QhullPoint &other) const { return ! operator==(other); }
    const coordT &      operator[](int idx) const { QHULL_ASSERT(point_coordinates!=0 && idx>=0 && idx<point_dimension); return *(point_coordinates+idx); } //!< 0 to hull_dim-1
    coordT &            operator[](int idx) { QHULL_ASSERT(point_coordinates!=0 && idx>=0 && idx<point_dimension); return *(point_coordinates+idx); } //!< 0 to hull_dim-1
    QhullQh *           qh() { return qh_qh; }
    void                setCoordinates(coordT *c) { point_coordinates= c; }
    void                setDimension(int pointDimension) { point_dimension= pointDimension; }

#//!\name foreach
    iterator            begin() { return point_coordinates; }
    const_iterator      begin() const { return point_coordinates; }
    const_iterator      constBegin() const { return point_coordinates; }
    const_iterator      constEnd() const { return (point_coordinates ? point_coordinates+point_dimension : 0); }
    int                 count() { return (point_coordinates ? point_dimension : 0); }
    iterator            end() { return (point_coordinates ? point_coordinates+point_dimension : 0); }
    const_iterator      end() const { return (point_coordinates ? point_coordinates+point_dimension : 0); }
    size_t              size() { return (size_t)(point_coordinates ? point_dimension : 0); }

#//!\name Methods
    void                advancePoint(countT idx) { if(point_coordinates) { point_coordinates += idx*point_dimension; } }
    double              distance(const QhullPoint &p) const;

#//!\name IO

    struct PrintPoint{
        const QhullPoint *point;
        const char *    point_message;
        bool            with_identifier;
                        PrintPoint(const char *message, bool withIdentifier, const QhullPoint &p) : point(&p), point_message(message), with_identifier(withIdentifier) {}
    };//PrintPoint
    PrintPoint          print(const char *message) const { return PrintPoint(message, false, *this); }
    PrintPoint          printWithIdentifier(const char *message) const { return PrintPoint(message, true, *this); }

};//QhullPoint

QHULL_DECLARE_SEQUENTIAL_ITERATOR(QhullPoint, coordT)

}//namespace orgQhull

#//!\name Global

std::ostream &operator<<(std::ostream &os, const orgQhull::QhullPoint::PrintPoint &pr);
std::ostream &operator<<(std::ostream &os, const orgQhull::QhullPoint &p);

#endif // QHPOINT_H

