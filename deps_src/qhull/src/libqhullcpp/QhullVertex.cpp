/****************************************************************************
**
** Copyright (c) 2008-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/libqhullcpp/QhullVertex.cpp#3 $$Change: 2066 $
** $DateTime: 2016/01/18 19:29:17 $$Author: bbarber $
**
****************************************************************************/

#//! QhullVertex -- Qhull's vertex structure, vertexT, as a C++ class

#include "libqhullcpp/QhullVertex.h"

#include "libqhullcpp/Qhull.h"
#include "libqhullcpp/QhullPoint.h"
#include "libqhullcpp/QhullFacetSet.h"
#include "libqhullcpp/QhullFacet.h"

#ifdef _MSC_VER  // Microsoft Visual C++ -- warning level 4
#pragma warning( disable : 4611)  // interaction between '_setjmp' and C++ object destruction is non-portable
#pragma warning( disable : 4996)  // function was declared deprecated(strcpy, localtime, etc.)
#endif

namespace orgQhull {

#//!\name Class objects
vertexT QhullVertex::
s_empty_vertex= {0,0,0,0,0,
                 0,0,0,0,0,
                 0};

#//!\name Constructors

QhullVertex::QhullVertex(const Qhull &q)
: qh_vertex(&s_empty_vertex)
, qh_qh(q.qh())
{
}//Default

QhullVertex::QhullVertex(const Qhull &q, vertexT *v)
: qh_vertex(v ? v : &s_empty_vertex)
, qh_qh(q.qh())
{
}//vertexT

#//!\name foreach

//! Return neighboring facets for a vertex
//! If neither merging nor Voronoi diagram, requires Qhull::defineVertexNeighborFacets() beforehand.
QhullFacetSet QhullVertex::
neighborFacets() const
{
    if(!neighborFacetsDefined()){
        throw QhullError(10034, "Qhull error: neighboring facets of vertex %d not defined.  Please call Qhull::defineVertexNeighborFacets() beforehand.", id());
    }
    return QhullFacetSet(qh_qh, qh_vertex->neighbors);
}//neighborFacets

}//namespace orgQhull

#//!\name Global functions

using std::endl;
using std::ostream;
using std::string;
using std::vector;
using orgQhull::QhullPoint;
using orgQhull::QhullFacet;
using orgQhull::QhullFacetSet;
using orgQhull::QhullFacetSetIterator;
using orgQhull::QhullVertex;

//! Duplicate of qh_printvertex [io_r.c]
ostream &
operator<<(ostream &os, const QhullVertex::PrintVertex &pr)
{
    QhullVertex v= *pr.vertex;
    QhullPoint p= v.point();
    if(*pr.print_message){
        os << pr.print_message << " ";
    }else{
        os << "- ";
    }
    os << "p" << p.id() << " (v" << v.id() << "): ";
    const realT *c= p.coordinates();
    for(int k= p.dimension(); k--; ){
        os << " " << *c++; // FIXUP QH11010 %5.2g
    }
    if(v.getVertexT()->deleted){
        os << " deleted";
    }
    if(v.getVertexT()->delridge){
        os << " ridgedeleted";
    }
    os << endl;
    if(v.neighborFacetsDefined()){
        QhullFacetSetIterator i= v.neighborFacets();
        if(i.hasNext()){
            os << " neighborFacets:";
            countT count= 0;
            while(i.hasNext()){
                if(++count % 100 == 0){
                    os << endl << "     ";
                }
                QhullFacet f= i.next();
                os << " f" << f.id();
            }
            os << endl;
        }
    }
    return os;
}//<< PrintVertex

