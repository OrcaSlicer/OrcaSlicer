/****************************************************************************
**
** Copyright (c) 2009-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/libqhullcpp/Coordinates.cpp#4 $$Change: 2066 $
** $DateTime: 2016/01/18 19:29:17 $$Author: bbarber $
**
****************************************************************************/

#include "libqhullcpp/Coordinates.h"

#include "libqhullcpp/functionObjects.h"
#include "libqhullcpp/QhullError.h"

#include <iostream>
#include <iterator>
#include <algorithm>

#ifdef _MSC_VER  // Microsoft Visual C++ -- warning level 4
#endif

namespace orgQhull {

#//! Coordinates -- vector of coordT (normally double)

#//!\name Constructor

#//!\name Element access

// Inefficient without result-value-optimization or implicitly shared object
Coordinates Coordinates::
mid(countT idx, countT length) const
{
    countT newLength= length;
    if(length<0 || idx+length > count()){
        newLength= count()-idx;
    }
    Coordinates result;
    if(newLength>0){
        std::copy(begin()+idx, begin()+(idx+newLength), std::back_inserter(result));
    }
    return result;
}//mid

coordT Coordinates::
value(countT idx, const coordT &defaultValue) const
{
    return ((idx < 0 || idx >= count()) ? defaultValue : (*this)[idx]);
}//value

#//!\name GetSet

Coordinates Coordinates::
operator+(const Coordinates &other) const
{
    Coordinates result(*this);
    std::copy(other.begin(), other.end(), std::back_inserter(result));
    return result;
}//operator+

Coordinates & Coordinates::
operator+=(const Coordinates &other)
{
    if(&other==this){
        Coordinates clone(other);
        std::copy(clone.begin(), clone.end(), std::back_inserter(*this));
    }else{
        std::copy(other.begin(), other.end(), std::back_inserter(*this));
    }
    return *this;
}//operator+=

#//!\name Read-write

void Coordinates::
append(int pointDimension, coordT *c)
{
    if(c){
        coordT *p= c;
        for(int i= 0; i<pointDimension; ++i){
            coordinate_array.push_back(*p++);
        }
    }
}//append dim coordT

coordT Coordinates::
takeAt(countT idx)
{
    coordT c= at(idx);
    erase(begin()+idx);
    return c;
}//takeAt

coordT Coordinates::
takeLast()
{
    coordT c= last();
    removeLast();
    return c;
}//takeLast

void Coordinates::
swap(countT idx, countT other)
{
    coordT c= at(idx);
    at(idx)= at(other);
    at(other)= c;
}//swap

#//!\name Search

bool Coordinates::
contains(const coordT &t) const
{
    CoordinatesIterator i(*this);
    return i.findNext(t);
}//contains

countT Coordinates::
count(const coordT &t) const
{
    CoordinatesIterator i(*this);
    countT result= 0;
    while(i.findNext(t)){
        ++result;
    }
    return result;
}//count

countT Coordinates::
indexOf(const coordT &t, countT from) const
{
    if(from<0){
        from += count();
        if(from<0){
            from= 0;
        }
    }
    if(from<count()){
        const_iterator i= begin()+from;
        while(i!=constEnd()){
            if(*i==t){
                return (static_cast<countT>(i-begin())); // WARN64 coordinate index
            }
            ++i;
        }
    }
    return -1;
}//indexOf

countT Coordinates::
lastIndexOf(const coordT &t, countT from) const
{
    if(from<0){
        from += count();
    }else if(from>=count()){
        from= count()-1;
    }
    if(from>=0){
        const_iterator i= begin()+from+1;
        while(i-- != constBegin()){
            if(*i==t){
                return (static_cast<countT>(i-begin())); // WARN64 coordinate index
            }
        }
    }
    return -1;
}//lastIndexOf

void Coordinates::
removeAll(const coordT &t)
{
    MutableCoordinatesIterator i(*this);
    while(i.findNext(t)){
        i.remove();
    }
}//removeAll

}//namespace orgQhull

#//!\name Global functions

using std::endl;
using std::istream;
using std::ostream;
using std::string;
using std::ws;
using orgQhull::Coordinates;

ostream &
operator<<(ostream &os, const Coordinates &cs)
{
    Coordinates::const_iterator c= cs.begin();
    for(countT i=cs.count(); i--; ){
        os << *c++ << " ";
    }
    return os;
}//operator<<

