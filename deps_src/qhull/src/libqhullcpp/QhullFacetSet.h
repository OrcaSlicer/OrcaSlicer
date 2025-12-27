/****************************************************************************
**
** Copyright (c) 2008-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/libqhullcpp/QhullFacetSet.h#2 $$Change: 2066 $
** $DateTime: 2016/01/18 19:29:17 $$Author: bbarber $
**
****************************************************************************/

#ifndef QHULLFACETSET_H
#define QHULLFACETSET_H

#include "libqhullcpp/QhullSet.h"
#include "libqhullcpp/QhullFacet.h"

#include <ostream>

namespace orgQhull {

#//!\name Used here
    class Qhull;

#//!\name Defined here
    //! QhullFacetSet -- a set of Qhull facets, as a C++ class.  See QhullFacetList.h
    class QhullFacetSet;
    typedef QhullSetIterator<QhullFacet> QhullFacetSetIterator;

class QhullFacetSet : public QhullSet<QhullFacet> {

#//!\name Defined here
public:
    typedef facetT *   base_type;  // for QhullVertexSet

private:
#//!\name Fields
    bool                select_all;   //! True if include bad facets.  Default is false.

public:
#//!\name Constructor
                        //Conversion from setT* is not type-safe.  Implicit conversion for void* to T
                        QhullFacetSet(const Qhull &q, setT *s) : QhullSet<QhullFacet>(q, s), select_all(false) {}
                        QhullFacetSet(QhullQh *qqh, setT *s) : QhullSet<QhullFacet>(qqh, s), select_all(false) {}
                        //!Copy constructor copies pointers but not contents.  Needed for return by value and parameter passing.
                        QhullFacetSet(const QhullFacetSet &other) : QhullSet<QhullFacet>(other), select_all(other.select_all) {}
                        //!Assignment copies pointers but not contents.
    QhullFacetSet &     operator=(const QhullFacetSet &other) { QhullSet<QhullFacet>::operator=(other); select_all= other.select_all; return *this; }

private:
                        //!Disable default constructor.  See QhullSetBase
                        QhullFacetSet();
public:

#//!\name Conversion
#ifndef QHULL_NO_STL
    std::vector<QhullFacet> toStdVector() const;
#endif //QHULL_NO_STL
#ifdef QHULL_USES_QT
    QList<QhullFacet>   toQList() const;
#endif //QHULL_USES_QT

#//!\name GetSet
                        //! Filtered by facet.isGood().  May be 0 when !isEmpty().
    countT              count() const;
    bool                contains(const QhullFacet &f) const;
    countT              count(const QhullFacet &f) const;
    bool                isSelectAll() const { return select_all; }
                        //! operator==() does not depend on isGood()
    void                selectAll() { select_all= true; }
    void                selectGood() { select_all= false; }

#//!\name IO
    // Not same as QhullFacetList#IO.  A QhullFacetSet is a component of a QhullFacetList.

    struct PrintFacetSet{
        const QhullFacetSet *facet_set;
        const char *    print_message;  //!< non-null message
                        PrintFacetSet(const char *message, const QhullFacetSet *s) : facet_set(s), print_message(message) {}
    };//PrintFacetSet
    const PrintFacetSet print(const char *message) const { return PrintFacetSet(message, this); }

    struct PrintIdentifiers{
        const QhullFacetSet *facet_set;
        const char *    print_message;  //!< non-null message
                        PrintIdentifiers(const char *message, const QhullFacetSet *s) : facet_set(s), print_message(message) {}
    };//PrintIdentifiers
    PrintIdentifiers    printIdentifiers(const char *message) const { return PrintIdentifiers(message, this); }

};//class QhullFacetSet

}//namespace orgQhull

#//!\name == Global namespace =========================================

std::ostream &operator<<(std::ostream &os, const orgQhull::QhullFacetSet &fs);
std::ostream &operator<<(std::ostream &os, const orgQhull::QhullFacetSet::PrintFacetSet &pr);
std::ostream &operator<<(std::ostream &os, const orgQhull::QhullFacetSet::PrintIdentifiers &p);

#endif // QHULLFACETSET_H
