/****************************************************************************
**
** Copyright (c) 2009-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/libqhullcpp/QhullPointSet.cpp#2 $$Change: 2066 $
** $DateTime: 2016/01/18 19:29:17 $$Author: bbarber $
**
****************************************************************************/

#include "libqhullcpp/QhullPointSet.h"

#include <iostream>
#include <algorithm>

#ifdef _MSC_VER  // Microsoft Visual C++ -- warning level 4
#endif

namespace orgQhull {

// Implemented via QhullSet.h

}//namespace orgQhull

#//!\name Global functions

using std::endl;
using std::ostream;
using orgQhull::QhullPoint;
using orgQhull::QhullPointSet;
using orgQhull::QhullPointSetIterator;

ostream &
operator<<(ostream &os, const QhullPointSet::PrintIdentifiers &pr)
{
    os << pr.print_message;
    const QhullPointSet s= *pr.point_set;
    QhullPointSetIterator i(s);
    while(i.hasNext()){
        if(i.hasPrevious()){
            os << " ";
        }
        const QhullPoint point= i.next();
        countT id= point.id();
        os << "p" << id;

    }
    os << endl;
    return os;
}//PrintIdentifiers

ostream &
operator<<(ostream &os, const QhullPointSet::PrintPointSet &pr)
{
    os << pr.print_message;
    const QhullPointSet s= *pr.point_set;
    for(QhullPointSet::const_iterator i=s.begin(); i != s.end(); ++i){
        const QhullPoint point= *i;
        os << point;
    }
    return os;
}//printPointSet


