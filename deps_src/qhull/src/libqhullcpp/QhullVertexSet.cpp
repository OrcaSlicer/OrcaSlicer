/****************************************************************************
**
** Copyright (c) 2009-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/libqhullcpp/QhullVertexSet.cpp#3 $$Change: 2066 $
** $DateTime: 2016/01/18 19:29:17 $$Author: bbarber $
**
****************************************************************************/

#//! QhullVertexSet -- Qhull's linked Vertexs, as a C++ class

#include "libqhullcpp/QhullVertexSet.h"

#include "libqhullcpp/QhullVertex.h"
#include "libqhullcpp/QhullPoint.h"
#include "libqhullcpp/QhullRidge.h"
#include "libqhullcpp/QhullVertex.h"
#include "libqhullcpp/Qhull.h"

using std::string;
using std::vector;

#ifdef _MSC_VER  // Microsoft Visual C++ -- warning level 4
#pragma warning( disable : 4611)  /* interaction between '_setjmp' and C++ object destruction is non-portable */
                                    /* setjmp should not be implemented with 'catch' */
#endif

namespace orgQhull {

QhullVertexSet::
QhullVertexSet(const Qhull &q, facetT *facetlist, setT *facetset, bool allfacets)
: QhullSet<QhullVertex>(q.qh(), 0)
, qhsettemp_defined(false)
{
    QH_TRY_(q.qh()){ // no object creation -- destructors skipped on longjmp()
        setT *vertices= qh_facetvertices(q.qh(), facetlist, facetset, allfacets);
        defineAs(vertices);
        qhsettemp_defined= true;
    }
    q.qh()->NOerrexit= true;
    q.qh()->maybeThrowQhullMessage(QH_TRY_status);
}//QhullVertexSet facetlist facetset

//! Return tempory QhullVertexSet of vertices for a list and/or a set of facets
//! Sets qhsettemp_defined (disallows copy constructor and assignment to prevent double-free)
QhullVertexSet::
QhullVertexSet(QhullQh *qqh, facetT *facetlist, setT *facetset, bool allfacets)
: QhullSet<QhullVertex>(qqh, 0)
, qhsettemp_defined(false)
{
    QH_TRY_(qh()){ // no object creation -- destructors skipped on longjmp()
        setT *vertices= qh_facetvertices(qh(), facetlist, facetset, allfacets);
        defineAs(vertices);
        qhsettemp_defined= true;
    }
    qh()->NOerrexit= true;
    qh()->maybeThrowQhullMessage(QH_TRY_status);
}//QhullVertexSet facetlist facetset

//! Copy constructor for argument passing and returning a result
//! Only copies a pointer to the set.
//! Throws an error if qhsettemp_defined, otherwise have a double-free
//!\todo Convert QhullVertexSet to a shared pointer with reference counting
QhullVertexSet::
QhullVertexSet(const QhullVertexSet &other)
: QhullSet<QhullVertex>(other)
, qhsettemp_defined(false)
{
    if(other.qhsettemp_defined){
        throw QhullError(10077, "QhullVertexSet: Cannot use copy constructor since qhsettemp_defined (e.g., QhullVertexSet for a set and/or list of QhFacet).  Contains %d vertices", other.count());
    }
}//copy constructor

//! Copy assignment only copies a pointer to the set.
//! Throws an error if qhsettemp_defined, otherwise have a double-free
QhullVertexSet & QhullVertexSet::
operator=(const QhullVertexSet &other)
{
    QhullSet<QhullVertex>::operator=(other);
    qhsettemp_defined= false;
    if(other.qhsettemp_defined){
        throw QhullError(10078, "QhullVertexSet: Cannot use copy constructor since qhsettemp_defined (e.g., QhullVertexSet for a set and/or list of QhFacet).  Contains %d vertices", other.count());
    }
    return *this;
}//assignment

void QhullVertexSet::
freeQhSetTemp()
{
    if(qhsettemp_defined){
        qhsettemp_defined= false;
        QH_TRY_(qh()){ // no object creation -- destructors skipped on longjmp()
            qh_settempfree(qh(), referenceSetT()); // errors if not top of tempstack or if qhmem corrupted
        }
        qh()->NOerrexit= true;
        qh()->maybeThrowQhullMessage(QH_TRY_status, QhullError::NOthrow);
    }
}//freeQhSetTemp

QhullVertexSet::
~QhullVertexSet()
{
    freeQhSetTemp();
}//~QhullVertexSet

//FIXUP -- Move conditional, QhullVertexSet code to QhullVertexSet.cpp
#ifndef QHULL_NO_STL
std::vector<QhullVertex> QhullVertexSet::
toStdVector() const
{
    QhullSetIterator<QhullVertex> i(*this);
    std::vector<QhullVertex> vs;
    while(i.hasNext()){
        QhullVertex v= i.next();
        vs.push_back(v);
    }
    return vs;
}//toStdVector
#endif //QHULL_NO_STL

}//namespace orgQhull

#//!\name Global functions

using std::endl;
using std::ostream;
using orgQhull::QhullPoint;
using orgQhull::QhullVertex;
using orgQhull::QhullVertexSet;
using orgQhull::QhullVertexSetIterator;

//! Print Vertex identifiers to stream.  Space prefix.  From qh_printVertexheader [io_r.c]
ostream &
operator<<(ostream &os, const QhullVertexSet::PrintIdentifiers &pr)
{
    os << pr.print_message;
    for(QhullVertexSet::const_iterator i= pr.vertex_set->begin(); i!=pr.vertex_set->end(); ++i){
        const QhullVertex v= *i;
        os << " v" << v.id();
    }
    os << endl;
    return os;
}//<<QhullVertexSet::PrintIdentifiers

//! Duplicate of printvertices [io_r.c]
ostream &
operator<<(ostream &os, const QhullVertexSet::PrintVertexSet &pr){

    os << pr.print_message;
    const QhullVertexSet *vs= pr.vertex_set;
    QhullVertexSetIterator i= *vs;
    while(i.hasNext()){
        const QhullVertex v= i.next();
        const QhullPoint p= v.point();
        os << " p" << p.id() << "(v" << v.id() << ")";
    }
    os << endl;

    return os;
}//<< PrintVertexSet


