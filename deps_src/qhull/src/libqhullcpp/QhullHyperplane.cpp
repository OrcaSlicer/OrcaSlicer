/****************************************************************************
**
** Copyright (c) 2009-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/libqhullcpp/QhullHyperplane.cpp#3 $$Change: 2066 $
** $DateTime: 2016/01/18 19:29:17 $$Author: bbarber $
**
****************************************************************************/

#include "libqhullcpp/QhullHyperplane.h"

#include "libqhullcpp/Qhull.h"
#include "libqhullcpp/QhullPoint.h"

#include <iostream>


#ifdef _MSC_VER  // Microsoft Visual C++ -- warning level 4
#endif

namespace orgQhull {

#//!\name Constructors

QhullHyperplane::
QhullHyperplane(const Qhull &q) 
: hyperplane_coordinates(0)
, qh_qh(q.qh())
, hyperplane_offset(0.0)
, hyperplane_dimension(0)
{
}

QhullHyperplane::
QhullHyperplane(const Qhull &q, int hyperplaneDimension, coordT *c, coordT hyperplaneOffset) 
: hyperplane_coordinates(c)
, qh_qh(q.qh())
, hyperplane_offset(hyperplaneOffset)
, hyperplane_dimension(hyperplaneDimension)
{
}

#//!\name Conversions

// See qt-qhull.cpp for QList conversions

#ifndef QHULL_NO_STL
std::vector<coordT> QhullHyperplane::
toStdVector() const
{
    QhullHyperplaneIterator i(*this);
    std::vector<coordT> fs;
    while(i.hasNext()){
        fs.push_back(i.next());
    }
    fs.push_back(hyperplane_offset);
    return fs;
}//toStdVector
#endif //QHULL_NO_STL

#//!\name GetSet

//! Return true if equal
//! If qh_qh defined, tests qh.distanceEpsilon and qh.angleEpsilon
//! otherwise, tests equal coordinates and offset
bool QhullHyperplane::
operator==(const QhullHyperplane &other) const
{
    if(hyperplane_dimension!=other.hyperplane_dimension || !hyperplane_coordinates || !other.hyperplane_coordinates){
        return false;
    }
    double d= fabs(hyperplane_offset-other.hyperplane_offset);
    if(d > (qh_qh ? qh_qh->distanceEpsilon() : 0.0)){
        return false;
    }
    double angle= hyperplaneAngle(other);

    double a= fabs(angle-1.0);
    if(a > (qh_qh ? qh_qh->angleEpsilon() : 0.0)){
        return false;
    }
    return true;
}//operator==

#//!\name Methods

//! Return distance from point to hyperplane.
//!   If greater than zero, the point is above the facet (i.e., outside).
// qh_distplane [geom_r.c], QhullFacet::distance, and QhullHyperplane::distance are copies
//    Does not support RANDOMdist or logging
double QhullHyperplane::
distance(const QhullPoint &p) const
{
    const coordT *point= p.coordinates();
    int dim= p.dimension();
    QHULL_ASSERT(dim==dimension());
    const coordT *normal= coordinates();
    double dist;

    switch (dim){
  case 2:
      dist= offset() + point[0] * normal[0] + point[1] * normal[1];
      break;
  case 3:
      dist= offset() + point[0] * normal[0] + point[1] * normal[1] + point[2] * normal[2];
      break;
  case 4:
      dist= offset()+point[0]*normal[0]+point[1]*normal[1]+point[2]*normal[2]+point[3]*normal[3];
      break;
  case 5:
      dist= offset()+point[0]*normal[0]+point[1]*normal[1]+point[2]*normal[2]+point[3]*normal[3]+point[4]*normal[4];
      break;
  case 6:
      dist= offset()+point[0]*normal[0]+point[1]*normal[1]+point[2]*normal[2]+point[3]*normal[3]+point[4]*normal[4]+point[5]*normal[5];
      break;
  case 7:
      dist= offset()+point[0]*normal[0]+point[1]*normal[1]+point[2]*normal[2]+point[3]*normal[3]+point[4]*normal[4]+point[5]*normal[5]+point[6]*normal[6];
      break;
  case 8:
      dist= offset()+point[0]*normal[0]+point[1]*normal[1]+point[2]*normal[2]+point[3]*normal[3]+point[4]*normal[4]+point[5]*normal[5]+point[6]*normal[6]+point[7]*normal[7];
      break;
  default:
      dist= offset();
      for (int k=dim; k--; )
          dist += *point++ * *normal++;
      break;
    }
    return dist;
}//distance

double QhullHyperplane::
hyperplaneAngle(const QhullHyperplane &other) const
{
    volatile realT result= 0.0;
    QH_TRY_(qh_qh){ // no object creation -- destructors skipped on longjmp()
        result= qh_getangle(qh_qh, hyperplane_coordinates, other.hyperplane_coordinates);
    }
    qh_qh->NOerrexit= true;
    qh_qh->maybeThrowQhullMessage(QH_TRY_status);
    return result;
}//hyperplaneAngle

double QhullHyperplane::
norm() const {
    double d= 0.0;
    const coordT *c= coordinates();
    for (int k=dimension(); k--; ){
        d += *c * *c;
        ++c;
    }
    return sqrt(d);
}//norm

}//namespace orgQhull

#//!\name Global functions

using std::ostream;
using orgQhull::QhullHyperplane;

#//!\name GetSet<<

ostream &
operator<<(ostream &os, const QhullHyperplane &p)
{
    os << p.print("");
    return os;
}

ostream &
operator<<(ostream &os, const QhullHyperplane::PrintHyperplane &pr)
{
    os << pr.print_message;
    QhullHyperplane p= *pr.hyperplane;
    const realT *c= p.coordinates();
    for(int k=p.dimension(); k--; ){
        realT r= *c++;
        if(pr.print_message){
            os << " " << r; // FIXUP QH11010 %8.4g
        }else{
            os << " " << r; // FIXUP QH11010 qh_REAL_1
        }
    }
    os << pr.hyperplane_offset_message << " " << p.offset();
    os << std::endl;
    return os;
}//PrintHyperplane

