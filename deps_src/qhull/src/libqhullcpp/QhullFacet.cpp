/****************************************************************************
**
** Copyright (c) 2008-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/libqhullcpp/QhullFacet.cpp#3 $$Change: 2066 $
** $DateTime: 2016/01/18 19:29:17 $$Author: bbarber $
**
****************************************************************************/

#//! QhullFacet -- Qhull's facet structure, facetT, as a C++ class

#include "libqhullcpp/QhullFacet.h"

#include "libqhullcpp/QhullError.h"
#include "libqhullcpp/Qhull.h"
#include "libqhullcpp/QhullSet.h"
#include "libqhullcpp/QhullPoint.h"
#include "libqhullcpp/QhullPointSet.h"
#include "libqhullcpp/QhullRidge.h"
#include "libqhullcpp/QhullFacetSet.h"
#include "libqhullcpp/QhullVertex.h"

#include <ostream>

using std::endl;
using std::ostream;

#ifdef _MSC_VER  // Microsoft Visual C++ -- warning level 4
#pragma warning( disable : 4611)  // interaction between '_setjmp' and C++ object destruction is non-portable
#pragma warning( disable : 4996)  // function was declared deprecated(strcpy, localtime, etc.)
#endif

namespace orgQhull {

#//!\name Class objects
facetT QhullFacet::
s_empty_facet= {0,0,0,0,{0},
        0,0,0,0,0,
        0,0,0,0,0,
        0,0,0,0,0,
        0,0,0,0,0,
        0,0,0,0,0,
        0,0,0,0,0,
        0,0,0,0};

#//!\name Constructors

QhullFacet::
QhullFacet(const Qhull &q) 
: qh_facet(&s_empty_facet)
, qh_qh(q.qh())
{
}

QhullFacet::
QhullFacet(const Qhull &q, facetT *f) 
: qh_facet(f ? f : &s_empty_facet)
, qh_qh(q.qh())
{
}

#//!\name GetSet

//! Return voronoi center or facet centrum.  Derived from qh_printcenter [io_r.c]
//! if printFormat=qh_PRINTtriangles and qh.DELAUNAY, returns centrum of a Delaunay facet
//! Sets center if needed
//! Code duplicated for PrintCenter and getCenter
//! Returns QhullPoint() if none or qh_INFINITE
QhullPoint QhullFacet::
getCenter(qh_PRINT printFormat)
{
    if(!qh_qh){
        // returns QhullPoint()
    }else if(qh_qh->CENTERtype==qh_ASvoronoi){
        if(!qh_facet->normal || !qh_facet->upperdelaunay || !qh_qh->ATinfinity){
            if(!qh_facet->center){
                QH_TRY_(qh_qh){ // no object creation -- destructors skipped on longjmp()
                    qh_facet->center= qh_facetcenter(qh_qh, qh_facet->vertices);
                }
                qh_qh->NOerrexit= true;
                qh_qh->maybeThrowQhullMessage(QH_TRY_status);
            }
            return QhullPoint(qh_qh, qh_qh->hull_dim-1, qh_facet->center);
        }
    }else if(qh_qh->CENTERtype==qh_AScentrum){
        volatile int numCoords= qh_qh->hull_dim;
        if(printFormat==qh_PRINTtriangles && qh_qh->DELAUNAY){
            numCoords--;
        }
        if(!qh_facet->center){
            QH_TRY_(qh_qh){ // no object creation -- destructors skipped on longjmp()
                qh_facet->center= qh_getcentrum(qh_qh, getFacetT());
            }
            qh_qh->NOerrexit= true;
            qh_qh->maybeThrowQhullMessage(QH_TRY_status);
        }
        return QhullPoint(qh_qh, numCoords, qh_facet->center);
    }
    return QhullPoint();
 }//getCenter

//! Return innerplane clearly below the vertices
//! from io_r.c[qh_PRINTinner]
QhullHyperplane QhullFacet::
innerplane() const{
    QhullHyperplane h;
    if(qh_qh){
        realT inner;
        // Does not error, TRY_QHULL_ not needed
        qh_outerinner(qh_qh, const_cast<facetT *>(getFacetT()), NULL, &inner);
        h= hyperplane();
        h.setOffset(h.offset()-inner); //inner is negative
    }
    return h;
}//innerplane

//! Return outerplane clearly above all points
//! from io_r.c[qh_PRINTouter]
QhullHyperplane QhullFacet::
outerplane() const{
    QhullHyperplane h;
    if(qh_qh){
        realT outer;
        // Does not error, TRY_QHULL_ not needed
        qh_outerinner(qh_qh, const_cast<facetT *>(getFacetT()), &outer, NULL);
        h= hyperplane();
        h.setOffset(h.offset()-outer); //outer is positive
    }
    return h;
}//outerplane

//! Set by qh_triangulate for option 'Qt'.
//! Errors if tricoplanar and facetArea() or qh_getarea() called first.
QhullFacet QhullFacet::
tricoplanarOwner() const
{
    if(qh_facet->tricoplanar){
        if(qh_facet->isarea){
            throw QhullError(10018, "Qhull error: facetArea() or qh_getarea() previously called.  triCoplanarOwner() is not available.");
        }
        return QhullFacet(qh_qh, qh_facet->f.triowner);
    }
    return QhullFacet(qh_qh); 
}//tricoplanarOwner

QhullPoint QhullFacet::
voronoiVertex()
{
    if(qh_qh && qh_qh->CENTERtype!=qh_ASvoronoi){
          throw QhullError(10052, "Error: QhullFacet.voronoiVertex() requires option 'v' (qh_ASvoronoi)");
    }
    return getCenter();
}//voronoiVertex

#//!\name Value

//! Disables tricoplanarOwner()
double QhullFacet::
facetArea()
{
    if(qh_qh && !qh_facet->isarea){
        QH_TRY_(qh_qh){ // no object creation -- destructors skipped on longjmp()
            qh_facet->f.area= qh_facetarea(qh_qh, qh_facet);
            qh_facet->isarea= True;
        }
        qh_qh->NOerrexit= true;
        qh_qh->maybeThrowQhullMessage(QH_TRY_status);
    }
    return qh_facet->f.area;
}//facetArea

#//!\name Foreach

QhullPointSet QhullFacet::
coplanarPoints() const
{
    return QhullPointSet(qh_qh, qh_facet->coplanarset);
}//coplanarPoints

QhullFacetSet QhullFacet::
neighborFacets() const
{
    return QhullFacetSet(qh_qh, qh_facet->neighbors);
}//neighborFacets

QhullPointSet QhullFacet::
outsidePoints() const
{
    return QhullPointSet(qh_qh, qh_facet->outsideset);
}//outsidePoints

QhullRidgeSet QhullFacet::
ridges() const
{
    return QhullRidgeSet(qh_qh, qh_facet->ridges);
}//ridges

QhullVertexSet QhullFacet::
vertices() const
{
    return QhullVertexSet(qh_qh, qh_facet->vertices);
}//vertices

}//namespace orgQhull

#//!\name operator<<

using std::ostream;

using orgQhull::QhullFacet;
using orgQhull::QhullFacetSet;
using orgQhull::QhullPoint;
using orgQhull::QhullPointSet;
using orgQhull::QhullRidge;
using orgQhull::QhullRidgeSet;
using orgQhull::QhullSetBase;
using orgQhull::QhullVertexSet;

ostream &
operator<<(ostream &os, const QhullFacet::PrintFacet &pr)
{
    os << pr.message;
    QhullFacet f= *pr.facet;
    if(f.getFacetT()==0){ // Special values from set iterator
        os << " NULLfacet" << endl;
        return os;
    }
    if(f.getFacetT()==qh_MERGEridge){
        os << " MERGEridge" << endl;
        return os;
    }
    if(f.getFacetT()==qh_DUPLICATEridge){
        os << " DUPLICATEridge" << endl;
        return os;
    }
    os << f.printHeader();
    if(!f.ridges().isEmpty()){
        os << f.printRidges();
    }
    return os;
}//operator<< PrintFacet

//! Print Voronoi center or facet centrum to stream.  Same as qh_printcenter [_r.]
//! Code duplicated for PrintCenter and getCenter
//! Sets center if needed
ostream &
operator<<(ostream &os, const QhullFacet::PrintCenter &pr)
{
    facetT *f= pr.facet->getFacetT();
    if(pr.facet->qh()->CENTERtype!=qh_ASvoronoi && pr.facet->qh()->CENTERtype!=qh_AScentrum){
        return os;
    }
    if (pr.message){
        os << pr.message;
    }
    int numCoords;
    if(pr.facet->qh()->CENTERtype==qh_ASvoronoi){
        numCoords= pr.facet->qh()->hull_dim-1;
        if(!f->normal || !f->upperdelaunay || !pr.facet->qh()->ATinfinity){
            if(!f->center){
                f->center= qh_facetcenter(pr.facet->qh(), f->vertices);
            }
            for(int k=0; k<numCoords; k++){
                os << f->center[k] << " "; // FIXUP QH11010 qh_REAL_1
            }
        }else{
            for(int k=0; k<numCoords; k++){
                os << qh_INFINITE << " "; // FIXUP QH11010 qh_REAL_1
            }
        }
    }else{ // qh CENTERtype==qh_AScentrum
        numCoords= pr.facet->qh()->hull_dim;
        if(pr.print_format==qh_PRINTtriangles && pr.facet->qh()->DELAUNAY){
            numCoords--;
        }
        if(!f->center){
            f->center= qh_getcentrum(pr.facet->qh(), f);
        }
        for(int k=0; k<numCoords; k++){
            os << f->center[k] << " "; // FIXUP QH11010 qh_REAL_1
        }
    }
    if(pr.print_format==qh_PRINTgeom && numCoords==2){
        os << " 0";
    }
    os << endl;
    return os;
}//operator<< PrintCenter

//! Print flags for facet to stream.  Space prefix.  From qh_printfacetheader [io_r.c]
ostream &
operator<<(ostream &os, const QhullFacet::PrintFlags &p)
{
    const facetT *f= p.facet->getFacetT();
    if(p.message){
        os << p.message;
    }

    os << (p.facet->isTopOrient() ? " top" : " bottom");
    if(p.facet->isSimplicial()){
        os << " simplicial";
    }
    if(p.facet->isTriCoplanar()){
        os << " tricoplanar";
    }
    if(p.facet->isUpperDelaunay()){
        os << " upperDelaunay";
    }
    if(f->visible){
        os << " visible";
    }
    if(f->newfacet){
        os << " new";
    }
    if(f->tested){
        os << " tested";
    }
    if(!f->good){
        os << " notG";
    }
    if(f->seen){
        os << " seen";
    }
    if(f->coplanar){
        os << " coplanar";
    }
    if(f->mergehorizon){
        os << " mergehorizon";
    }
    if(f->keepcentrum){
        os << " keepcentrum";
    }
    if(f->dupridge){
        os << " dupridge";
    }
    if(f->mergeridge && !f->mergeridge2){
        os << " mergeridge1";
    }
    if(f->mergeridge2){
        os << " mergeridge2";
    }
    if(f->newmerge){
        os << " newmerge";
    }
    if(f->flipped){
        os << " flipped";
    }
    if(f->notfurthest){
        os << " notfurthest";
    }
    if(f->degenerate){
        os << " degenerate";
    }
    if(f->redundant){
        os << " redundant";
    }
    os << endl;
    return os;
}//operator<< PrintFlags

//! Print header for facet to stream. Space prefix.  From qh_printfacetheader [io_r.c]
ostream &
operator<<(ostream &os, const QhullFacet::PrintHeader &pr)
{
    QhullFacet facet= *pr.facet;
    facetT *f= facet.getFacetT();
    os << "- f" << facet.id() << endl;
    os << facet.printFlags("    - flags:");
    if(f->isarea){
        os << "    - area: " << f->f.area << endl; //FIXUP QH11010 2.2g
    }else if(pr.facet->qh()->NEWfacets && f->visible && f->f.replace){
        os << "    - replacement: f" << f->f.replace->id << endl;
    }else if(f->newfacet){
        if(f->f.samecycle && f->f.samecycle != f){
            os << "    - shares same visible/horizon as f" << f->f.samecycle->id << endl;
        }
    }else if(f->tricoplanar /* !isarea */){
        if(f->f.triowner){
            os << "    - owner of normal & centrum is facet f" << f->f.triowner->id << endl;
        }
    }else if(f->f.newcycle){
        os << "    - was horizon to f" << f->f.newcycle->id << endl;
    }
    if(f->nummerge){
        os << "    - merges: " << f->nummerge << endl;
    }
    os << facet.hyperplane().print("    - normal: ", "\n    - offset: "); // FIXUP QH11010 %10.7g
    if(pr.facet->qh()->CENTERtype==qh_ASvoronoi || f->center){
        os << facet.printCenter(qh_PRINTfacets, "    - center: ");
    }
#if qh_MAXoutside
    if(f->maxoutside > pr.facet->qh()->DISTround){
        os << "    - maxoutside: " << f->maxoutside << endl; //FIXUP QH11010 %10.7g
    }
#endif
    QhullPointSet ps= facet.outsidePoints();
    if(!ps.isEmpty()){
        QhullPoint furthest= ps.last();
        if (ps.size() < 6) {
            os << "    - outside set(furthest p" << furthest.id() << "):" << endl;
            for(QhullPointSet::iterator i=ps.begin(); i!=ps.end(); ++i){
                QhullPoint p= *i;
                os << p.print("     ");
            }
        }else if(ps.size()<21){
            os << ps.print("    - outside set:");
        }else{
            os << "    - outside set:  " << ps.size() << " points.";
            os << furthest.print("  Furthest");
        }
#if !qh_COMPUTEfurthest
        os << "    - furthest distance= " << f->furthestdist << endl; //FIXUP QH11010 %2.2g
#endif
    }
    QhullPointSet cs= facet.coplanarPoints();
    if(!cs.isEmpty()){
        QhullPoint furthest= cs.last();
        if (cs.size() < 6) {
            os << "    - coplanar set(furthest p" << furthest.id() << "):" << endl;
            for(QhullPointSet::iterator i=cs.begin(); i!=cs.end(); ++i){
                QhullPoint p= *i;
                os << p.print("     ");
            }
        }else if(cs.size()<21){
            os << cs.print("    - coplanar set:");
        }else{
            os << "    - coplanar set:  " << cs.size() << " points.";
            os << furthest.print("  Furthest");
        }
        // FIXUP QH11027 Can/should zinc_(Zdistio) be called from C++ interface
        double d= facet.distance(furthest);
        os << "      furthest distance= " << d << endl; //FIXUP QH11010 %2.2g
    }
    QhullVertexSet vs= facet.vertices();
    if(!vs.isEmpty()){
        os << vs.print("    - vertices:");
    }
    QhullFacetSet fs= facet.neighborFacets();
    fs.selectAll();
    if(!fs.isEmpty()){
        os << fs.printIdentifiers("    - neighboring facets:");
    }
    return os;
}//operator<< PrintHeader


//! Print ridges of facet to stream.  Same as qh_printfacetridges [io_r.c]
ostream &
operator<<(ostream &os, const QhullFacet::PrintRidges &pr)
{
    const QhullFacet facet= *pr.facet;
    facetT *f= facet.getFacetT();
    QhullRidgeSet rs= facet.ridges();
    if(!rs.isEmpty()){
        if(f->visible && pr.facet->qh()->NEWfacets){
            os << "    - ridges(ids may be garbage):";
            for(QhullRidgeSet::iterator i=rs.begin(); i!=rs.end(); ++i){
                QhullRidge r= *i;
                os << " r" << r.id();
            }
            os << endl;
        }else{
            os << "    - ridges:" << endl;
        }

        // Keep track of printed ridges
        for(QhullRidgeSet::iterator i=rs.begin(); i!=rs.end(); ++i){
            QhullRidge r= *i;
            r.getRidgeT()->seen= false;
        }
        int ridgeCount= 0;
        if(facet.dimension()==3){
            for(QhullRidge r= rs.first(); !r.getRidgeT()->seen; r= r.nextRidge3d(facet)){
                r.getRidgeT()->seen= true;
                os << r.print("");
                ++ridgeCount;
                if(!r.hasNextRidge3d(facet)){
                    break;
                }
            }
        }else {
            QhullFacetSet ns(facet.neighborFacets());
            for(QhullFacetSet::iterator i=ns.begin(); i!=ns.end(); ++i){
                QhullFacet neighbor= *i;
                QhullRidgeSet nrs(neighbor.ridges());
                for(QhullRidgeSet::iterator j=nrs.begin(); j!=nrs.end(); ++j){
                    QhullRidge r= *j;
                    if(r.otherFacet(neighbor)==facet){
                        r.getRidgeT()->seen= true;
                        os << r.print("");
                        ridgeCount++;
                    }
                }
            }
        }
        if(ridgeCount!=rs.count()){
            os << "     - all ridges:";
            for(QhullRidgeSet::iterator i=rs.begin(); i!=rs.end(); ++i){
                QhullRidge r= *i;
                os << " r" << r.id();
            }
            os << endl;
        }
        for(QhullRidgeSet::iterator i=rs.begin(); i!=rs.end(); ++i){
            QhullRidge r= *i;
            if(!r.getRidgeT()->seen){
                os << r.print("");
            }
        }
    }
    return os;
}//operator<< PrintRidges

// "No conversion" error if defined inline
ostream &
operator<<(ostream &os, QhullFacet &f)
{
    os << f.print("");
    return os;
}//<< QhullFacet
