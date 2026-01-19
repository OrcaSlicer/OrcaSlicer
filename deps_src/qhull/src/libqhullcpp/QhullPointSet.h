/****************************************************************************
**
** Copyright (c) 2009-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/libqhullcpp/QhullPointSet.h#4 $$Change: 2079 $
** $DateTime: 2016/02/07 17:43:34 $$Author: bbarber $
**
****************************************************************************/

#ifndef QHULLPOINTSET_H
#define QHULLPOINTSET_H

#include "libqhull_r/qhull_ra.h"
#include "libqhullcpp/QhullSet.h"
#include "libqhullcpp/QhullPoint.h"

#include <ostream>

namespace orgQhull {

#//!\name Used here
    class Qhull;
    class QhullPoint;

#//!\name Defined here
    //! QhullPointSet -- a set of coordinate pointers with input dimension
    // with const_iterator and iterator
    class QhullPointSet;

class QhullPointSet : public QhullSet<QhullPoint> {

private:
#//!\name Fields
    // no fields
public:

#//!\name Construct
                        QhullPointSet(const Qhull &q, setT *s) : QhullSet<QhullPoint>(q, s) {}
                        //Conversion from setT* is not type-safe.  Implicit conversion for void* to T
                        QhullPointSet(QhullQh *qqh, setT *s) : QhullSet<QhullPoint>(qqh, s) {}
                        //Copy constructor copies pointer but not contents.  Needed for return by value and parameter passing.
                        QhullPointSet(const QhullPointSet &other) : QhullSet<QhullPoint>(other) {}
                        //!Assignment copies pointers but not contents.
    QhullPointSet &     operator=(const QhullPointSet &other) { QhullSet<QhullPoint>::operator=(other); return *this; }
                        ~QhullPointSet() {}

                        //!Default constructor disabled.
private:
                        QhullPointSet();
public:

#//!\name IO
    struct PrintIdentifiers{
        const QhullPointSet *point_set;
        const char *    print_message; //!< non-null message
        PrintIdentifiers(const char *message, const QhullPointSet *s) : point_set(s), print_message(message) {}
    };//PrintIdentifiers
    PrintIdentifiers printIdentifiers(const char *message) const { return PrintIdentifiers(message, this); }

    struct PrintPointSet{
        const QhullPointSet *point_set;
        const char *    print_message;  //!< non-null message
        PrintPointSet(const char *message, const QhullPointSet &s) : point_set(&s), print_message(message) {}
    };//PrintPointSet
    PrintPointSet       print(const char *message) const { return PrintPointSet(message, *this); }

};//QhullPointSet

typedef QhullSetIterator<QhullPoint>  QhullPointSetIterator;

}//namespace orgQhull

#//!\name Global

std::ostream &operator<<(std::ostream &os, const orgQhull::QhullPointSet::PrintIdentifiers &pr);
std::ostream &operator<<(std::ostream &os, const orgQhull::QhullPointSet::PrintPointSet &pr);

#endif // QHULLPOINTSET_H
