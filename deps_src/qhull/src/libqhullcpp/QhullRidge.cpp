/****************************************************************************
**
** Copyright (c) 2008-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/libqhullcpp/QhullRidge.cpp#3 $$Change: 2066 $
** $DateTime: 2016/01/18 19:29:17 $$Author: bbarber $
**
****************************************************************************/

#//! QhullRidge -- Qhull's ridge structure, ridgeT, as a C++ class

#include "libqhullcpp/QhullRidge.h"

#include "libqhullcpp/QhullSets.h"
#include "libqhullcpp/QhullVertex.h"
#include "libqhullcpp/Qhull.h"

#ifdef _MSC_VER  // Microsoft Visual C++ -- warning level 4
#pragma warning( disable : 4611)  // interaction between '_setjmp' and C++ object destruction is non-portable
#pragma warning( disable : 4996)  // function was declared deprecated(strcpy, localtime, etc.)
#endif

namespace orgQhull {

#//!\name Class objects
ridgeT QhullRidge::
s_empty_ridge= {0,0,0,0,0,
                0,0};

#//!\name Constructors

QhullRidge::QhullRidge(const Qhull &q)
: qh_ridge(&s_empty_ridge)
, qh_qh(q.qh())
{
}//Default

QhullRidge::QhullRidge(const Qhull &q, ridgeT *r)
: qh_ridge(r ? r : &s_empty_ridge)
, qh_qh(q.qh())
{
}//ridgeT

#//!\name foreach

//! Return True if nextRidge3d
//! Simplicial facets may have incomplete ridgeSets
//! Does not use qh_errexit()
bool QhullRidge::
hasNextRidge3d(const QhullFacet &f) const
{
    if(!qh_qh){
        return false;
    }
    vertexT *v= 0;
    // Does not call qh_errexit(), TRY_QHULL_ not needed
    ridgeT *ridge= qh_nextridge3d(getRidgeT(), f.getFacetT(), &v);
    return (ridge!=0);
}//hasNextRidge3d

//! Return next ridge and optional vertex for a 3d facet and ridge
//! Does not use qh_errexit()
QhullRidge QhullRidge::
nextRidge3d(const QhullFacet &f, QhullVertex *nextVertex) const
{
    vertexT *v= 0;
    ridgeT *ridge= 0;
    if(qh_qh){
        // Does not call qh_errexit(), TRY_QHULL_ not needed
        ridge= qh_nextridge3d(getRidgeT(), f.getFacetT(), &v);
        if(!ridge){
            throw QhullError(10030, "Qhull error nextRidge3d:  missing next ridge for facet %d ridge %d.  Does facet contain ridge?", f.id(), id());
        }
    }
    if(nextVertex!=0){
        *nextVertex= QhullVertex(qh_qh, v);
    }
    return QhullRidge(qh_qh, ridge);
}//nextRidge3d

}//namespace orgQhull

#//!\name Global functions

using std::endl;
using std::ostream;
using orgQhull::QhullRidge;
using orgQhull::QhullVertex;

ostream &
operator<<(ostream &os, const QhullRidge &r)
{
    os << r.print("");
    return os;
}//<< QhullRidge

//! Duplicate of qh_printridge [io_r.c]
ostream &
operator<<(ostream &os, const QhullRidge::PrintRidge &pr)
{
    if(*pr.print_message){
        os << pr.print_message << " ";
    }else{
        os << "     - ";
    }
    QhullRidge r= *pr.ridge;
    os << "r" << r.id();
    if(r.getRidgeT()->tested){
        os << " tested";
    }
    if(r.getRidgeT()->nonconvex){
        os << " nonconvex";
    }
    os << endl;
    os << r.vertices().print("           vertices:");
    if(r.getRidgeT()->top && r.getRidgeT()->bottom){
        os << "           between f" << r.topFacet().id() << " and f" << r.bottomFacet().id() << endl;
    }else if(r.getRidgeT()->top){
        os << "           top f" << r.topFacet().id() << endl;
    }else if(r.getRidgeT()->bottom){
        os << "           bottom f" << r.bottomFacet().id() << endl;
    }

    return os;
}//<< PrintRidge
