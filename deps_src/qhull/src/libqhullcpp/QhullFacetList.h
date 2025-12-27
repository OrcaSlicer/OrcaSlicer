/****************************************************************************
**
** Copyright (c) 2008-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/libqhullcpp/QhullFacetList.h#2 $$Change: 2066 $
** $DateTime: 2016/01/18 19:29:17 $$Author: bbarber $
**
****************************************************************************/

#ifndef QHULLFACETLIST_H
#define QHULLFACETLIST_H

#include "libqhullcpp/QhullLinkedList.h"
#include "libqhullcpp/QhullFacet.h"

#include <ostream>

#ifndef QHULL_NO_STL
#include <vector>
#endif

namespace orgQhull {

#//!\name Used here
    class Qhull;
    class QhullFacet;
    class QhullQh;

#//!\name Defined here
    //! QhullFacetList -- List of QhullFacet/facetT, as a C++ class.  
    //!\see QhullFacetSet.h
    class QhullFacetList;
    //! QhullFacetListIterator -- if(f.isGood()){ ... }
    typedef QhullLinkedListIterator<QhullFacet> QhullFacetListIterator;

class QhullFacetList : public QhullLinkedList<QhullFacet> {

#//!\name  Fields
private:
    bool                select_all;   //! True if include bad facets.  Default is false.

#//!\name Constructors
public:
                        QhullFacetList(const Qhull &q, facetT *b, facetT *e);
                        QhullFacetList(QhullQh *qqh, facetT *b, facetT *e);
                        QhullFacetList(QhullFacet b, QhullFacet e) : QhullLinkedList<QhullFacet>(b, e), select_all(false) {}
                        //Copy constructor copies pointer but not contents.  Needed for return by value and parameter passing.
                        QhullFacetList(const QhullFacetList &other) : QhullLinkedList<QhullFacet>(*other.begin(), *other.end()), select_all(other.select_all) {}
    QhullFacetList &    operator=(const QhullFacetList &other) { QhullLinkedList<QhullFacet>::operator =(other); select_all= other.select_all; return *this; }
                        ~QhullFacetList() {}

private:                //!Disable default constructor.  See QhullLinkedList
                    QhullFacetList();
public:

#//!\name Conversion
#ifndef QHULL_NO_STL
    std::vector<QhullFacet> toStdVector() const;
    std::vector<QhullVertex> vertices_toStdVector() const;
#endif //QHULL_NO_STL
#ifdef QHULL_USES_QT
    QList<QhullFacet>   toQList() const;
    QList<QhullVertex>  vertices_toQList() const;
#endif //QHULL_USES_QT

#//!\name GetSet
                        //! Filtered by facet.isGood().  May be 0 when !isEmpty().
    countT              count() const;
    bool                contains(const QhullFacet &f) const;
    countT              count(const QhullFacet &f) const;
    bool                isSelectAll() const { return select_all; }
    QhullQh *           qh() const { return first().qh(); }
    void                selectAll() { select_all= true; }
    void                selectGood() { select_all= false; }
                        //!< operator==() does not depend on isGood()

#//!\name IO
    struct PrintFacetList{
        const QhullFacetList *facet_list;
        const char *    print_message;   //!< non-null message
                        PrintFacetList(const QhullFacetList &fl, const char *message) : facet_list(&fl), print_message(message) {}
    };//PrintFacetList
    PrintFacetList      print(const char *message) const  { return PrintFacetList(*this, message); }

    struct PrintFacets{
        const QhullFacetList *facet_list;
                        PrintFacets(const QhullFacetList &fl) : facet_list(&fl) {}
    };//PrintFacets
    PrintFacets         printFacets() const { return PrintFacets(*this); }

    struct PrintVertices{
        const QhullFacetList *facet_list;
                        PrintVertices(const QhullFacetList &fl) : facet_list(&fl) {}
    };//PrintVertices
    PrintVertices       printVertices() const { return PrintVertices(*this); }
};//class QhullFacetList

}//namespace orgQhull

#//!\name == Global namespace =========================================

std::ostream &operator<<(std::ostream &os, const orgQhull::QhullFacetList::PrintFacetList &p);
std::ostream &operator<<(std::ostream &os, const orgQhull::QhullFacetList::PrintFacets &p);
std::ostream &operator<<(std::ostream &os, const orgQhull::QhullFacetList::PrintVertices &p);
std::ostream &operator<<(std::ostream &os, const orgQhull::QhullFacetList &fs);

#endif // QHULLFACETLIST_H
