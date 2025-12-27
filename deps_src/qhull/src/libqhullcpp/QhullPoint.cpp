/****************************************************************************
**
** Copyright (c) 2009-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/libqhullcpp/QhullPoint.cpp#3 $$Change: 2066 $
** $DateTime: 2016/01/18 19:29:17 $$Author: bbarber $
**
****************************************************************************/

#include "libqhullcpp/QhullPoint.h"

#include "libqhullcpp/QhullError.h"
#include "libqhullcpp/Qhull.h"

#include <iostream>
#include <algorithm>

#ifdef _MSC_VER  // Microsoft Visual C++ -- warning level 4
#endif

namespace orgQhull {

#//!\name Constructors


QhullPoint::
QhullPoint(const Qhull &q) 
: point_coordinates(0)
, qh_qh(q.qh())
, point_dimension(q.hullDimension())
{
}//QhullPoint

QhullPoint::
QhullPoint(const Qhull &q, coordT *c) 
: point_coordinates(c)
, qh_qh(q.qh())
, point_dimension(q.hullDimension())
{
    QHULL_ASSERT(q.hullDimension()>0);
}//QhullPoint dim, coordT

QhullPoint::
QhullPoint(const Qhull &q, int pointDimension, coordT *c) 
: point_coordinates(c)
, qh_qh(q.qh())
, point_dimension(pointDimension)
{
}//QhullPoint dim, coordT

//! QhullPoint of Coordinates with point_dimension==c.count()
QhullPoint::
QhullPoint(const Qhull &q, Coordinates &c) 
: point_coordinates(c.data())
, qh_qh(q.qh())
, point_dimension(c.count())
{
}//QhullPoint Coordinates

#//!\name Conversions

// See qt-qhull.cpp for QList conversion

#ifndef QHULL_NO_STL
std::vector<coordT> QhullPoint::
toStdVector() const
{
    QhullPointIterator i(*this);
    std::vector<coordT> vs;
    while(i.hasNext()){
        vs.push_back(i.next());
    }
    return vs;
}//toStdVector
#endif //QHULL_NO_STL

#//!\name GetSet

//! QhullPoint is equal if it has the same address and dimension
//! If !qh_qh, returns true if dimension and coordinates are equal
//! If qh_qh, returns true if the distance between points is less than qh_qh->distanceEpsilon()
//!\todo Compares distance with distance-to-hyperplane (distanceEpsilon).   Is that correct?
bool QhullPoint::
operator==(const QhullPoint &other) const
{
    if(point_dimension!=other.point_dimension){
        return false;
    }
    const coordT *c= point_coordinates;
    const coordT *c2= other.point_coordinates;
    if(c==c2){
        return true;
    }
    if(!c || !c2){
        return false;
    }
    if(!qh_qh || qh_qh->hull_dim==0){
        for(int k= point_dimension; k--; ){
            if(*c++ != *c2++){
                return false;
            }
        }
        return true;
    }
    double dist2= 0.0;
    for(int k= point_dimension; k--; ){
        double diff= *c++ - *c2++;
        dist2 += diff*diff;
    }
    dist2= sqrt(dist2);
    return (dist2 < qh_qh->distanceEpsilon());
}//operator==

#//!\name Methods

//! Return distance between two points.
double QhullPoint::
distance(const QhullPoint &p) const
{
    const coordT *c= point_coordinates;
    const coordT *c2= p.point_coordinates;
    int dim= point_dimension;
    if(dim!=p.point_dimension){
        throw QhullError(10075, "QhullPoint error: Expecting dimension %d for distance().  Got %d", dim, p.point_dimension);
    }
    if(!c || !c2){
        throw QhullError(10076, "QhullPoint error: Cannot compute distance() for undefined point");
    }
    double dist;

    switch(dim){
  case 2:
      dist= (c[0]-c2[0])*(c[0]-c2[0]) + (c[1]-c2[1])*(c[1]-c2[1]);
      break;
  case 3:
      dist= (c[0]-c2[0])*(c[0]-c2[0]) + (c[1]-c2[1])*(c[1]-c2[1]) + (c[2]-c2[2])*(c[2]-c2[2]);
      break;
  case 4:
      dist= (c[0]-c2[0])*(c[0]-c2[0]) + (c[1]-c2[1])*(c[1]-c2[1]) + (c[2]-c2[2])*(c[2]-c2[2]) + (c[3]-c2[3])*(c[3]-c2[3]);
      break;
  case 5:
      dist= (c[0]-c2[0])*(c[0]-c2[0]) + (c[1]-c2[1])*(c[1]-c2[1]) + (c[2]-c2[2])*(c[2]-c2[2]) + (c[3]-c2[3])*(c[3]-c2[3]) + (c[4]-c2[4])*(c[4]-c2[4]);
      break;
  case 6:
      dist= (c[0]-c2[0])*(c[0]-c2[0]) + (c[1]-c2[1])*(c[1]-c2[1]) + (c[2]-c2[2])*(c[2]-c2[2]) + (c[3]-c2[3])*(c[3]-c2[3]) + (c[4]-c2[4])*(c[4]-c2[4]) + (c[5]-c2[5])*(c[5]-c2[5]);
      break;
  case 7:
      dist= (c[0]-c2[0])*(c[0]-c2[0]) + (c[1]-c2[1])*(c[1]-c2[1]) + (c[2]-c2[2])*(c[2]-c2[2]) + (c[3]-c2[3])*(c[3]-c2[3]) + (c[4]-c2[4])*(c[4]-c2[4]) + (c[5]-c2[5])*(c[5]-c2[5]) + (c[6]-c2[6])*(c[6]-c2[6]);
      break;
  case 8:
      dist= (c[0]-c2[0])*(c[0]-c2[0]) + (c[1]-c2[1])*(c[1]-c2[1]) + (c[2]-c2[2])*(c[2]-c2[2]) + (c[3]-c2[3])*(c[3]-c2[3]) + (c[4]-c2[4])*(c[4]-c2[4]) + (c[5]-c2[5])*(c[5]-c2[5]) + (c[6]-c2[6])*(c[6]-c2[6]) + (c[7]-c2[7])*(c[7]-c2[7]);
      break;
  default:
      dist= 0.0;
      for(int k=dim; k--; ){
          dist += (*c - *c2) * (*c - *c2);
          ++c;
          ++c2;
      }
      break;
    }
    return sqrt(dist);
}//distance

}//namespace orgQhull

#//!\name Global functions

using std::ostream;
using orgQhull::QhullPoint;

//! Same as qh_printpointid [io.c]
ostream &
operator<<(ostream &os, const QhullPoint::PrintPoint &pr)
{
    QhullPoint p= *pr.point; 
    countT i= p.id();
    if(pr.point_message){
        if(*pr.point_message){
            os << pr.point_message << " ";
        }
        if(pr.with_identifier && (i!=qh_IDunknown) && (i!=qh_IDnone)){
            os << "p" << i << ": ";
        }
    }
    const realT *c= p.coordinates();
    for(int k=p.dimension(); k--; ){
        realT r= *c++;
        if(pr.point_message){
            os << " " << r; // FIXUP QH11010 %8.4g
        }else{
            os << " " << r; // FIXUP QH11010 qh_REAL_1
        }
    }
    os << std::endl;
    return os;
}//printPoint

ostream & 
operator<<(ostream &os, const QhullPoint &p)
{
    os << p.print(""); 
    return os;
}//operator<<
