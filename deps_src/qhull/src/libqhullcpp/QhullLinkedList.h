/****************************************************************************
**
** Copyright (c) 2008-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/libqhullcpp/QhullLinkedList.h#7 $$Change: 2079 $
** $DateTime: 2016/02/07 17:43:34 $$Author: bbarber $
**
****************************************************************************/

#ifndef QHULLLINKEDLIST_H
#define QHULLLINKEDLIST_H

#include "libqhull_r/qhull_ra.h"
#include "libqhullcpp/QhullError.h"

#include <cstddef>  // ptrdiff_t, size_t

#ifdef QHULL_USES_QT
#include <QtCore/QList>
#endif

#ifndef QHULL_NO_STL
#include <algorithm>
#include <iterator>
#include <vector>
#endif

namespace orgQhull {

#//!\name Defined here
    //! QhullLinkedList<T> -- A linked list modeled on QLinkedList.
    //!   T is an opaque type with T(B *b), b=t.getBaseT(), t=t.next(), and t=t.prev().  The end node is a sentinel.
    //!   QhullQh/qhT owns the contents.
    //!   QhullLinkedList does not define erase(), clear(), removeFirst(), removeLast(), pop_back(), pop_front(), fromStdList()
    //!   Derived from Qt/core/tools/qlinkedlist.h and libqhull_r.h/FORALLfacets_()
    //! QhullLinkedList<T>::const_iterator -- STL-style iterator
    //! QhullLinkedList<T>::iterator -- STL-style iterator
    //! QhullLinkedListIterator<T> -- Java-style iterator
    //!   Derived from Qt/core/tools/qiterator.h
    //!   Works with Qt's foreach keyword [Qt/src/corelib/global/qglobal.h]

template <typename T>
class QhullLinkedList
{
#//!\name Defined here
public:
    class const_iterator;
    class iterator;
    typedef const_iterator  ConstIterator;
    typedef iterator    Iterator;
    typedef ptrdiff_t   difference_type;
    typedef countT      size_type;
    typedef T           value_type;
    typedef const value_type *const_pointer;
    typedef const value_type &const_reference;
    typedef value_type *pointer;
    typedef value_type &reference;

#//!\name Fields
private:
    T                   begin_node;
    T                   end_node;     //! Sentinel node at end of list

#//!\name Constructors
public:
                        QhullLinkedList<T>(T b, T e) : begin_node(b), end_node(e) {}
                        //! Copy constructor copies begin_node and end_node, but not the list elements.  Needed for return by value and parameter passing.
                        QhullLinkedList<T>(const QhullLinkedList<T> &other) : begin_node(other.begin_node), end_node(other.end_node) {}
                        //! Copy assignment copies begin_node and end_node, but not the list elements.
                        QhullLinkedList<T> & operator=(const QhullLinkedList<T> &other) { begin_node= other.begin_node; end_node= other.end_node; return *this; }
                        ~QhullLinkedList<T>() {}

private:
                        //!disabled since a sentinel must be allocated as the private type
                        QhullLinkedList<T>() {}

public:

#//!\name Conversions
#ifndef QHULL_NO_STL
    std::vector<T>      toStdVector() const;
#endif
#ifdef QHULL_USES_QT
    QList<T>            toQList() const;
#endif

#//!\name GetSet
    countT              count() const;
                        //count(t) under #//!\name Search
    bool                isEmpty() const { return (begin_node==end_node); }
    bool                operator==(const QhullLinkedList<T> &o) const;
    bool                operator!=(const QhullLinkedList<T> &o) const { return !operator==(o); }
    size_t              size() const { return count(); }

#//!\name Element access
    //! For back() and last(), return T instead of T& (T is computed)
    const T             back() const { return last(); }
    T                   back() { return last(); }
    const T &           first() const { QHULL_ASSERT(!isEmpty()); return begin_node; }
    T &                 first() { QHULL_ASSERT(!isEmpty()); return begin_node; }
    const T &           front() const { return first(); }
    T &                 front() { return first(); }
    const T             last() const { QHULL_ASSERT(!isEmpty()); return *--end(); }
    T                   last() { QHULL_ASSERT(!isEmpty()); return *--end(); }

#//!\name Modify -- Allocation of opaque types not implemented.

#//!\name Search
    bool                contains(const T &t) const;
    countT              count(const T &t) const;

#//!\name Iterator
    iterator            begin() { return begin_node; }
    const_iterator      begin() const { return begin_node; }
    const_iterator      constBegin() const { return begin_node; }
    const_iterator      constEnd() const { return end_node; }
    iterator            end() { return end_node; }
    const_iterator      end() const { return end_node; }

    class iterator {

    private:
        T               i;
        friend class const_iterator;

    public:
        typedef std::bidirectional_iterator_tag  iterator_category;
        typedef T           value_type;
        typedef value_type *pointer;
        typedef value_type &reference;
        typedef ptrdiff_t   difference_type;

                        iterator() : i() {}
                        iterator(const T &t) : i(t) {}  //!< Automatic conversion to iterator
                        iterator(const iterator &o) : i(o.i) {}
        iterator &      operator=(const iterator &o) { i= o.i; return *this; }

        const T &       operator*() const { return i; }
        T &             operator*() { return i; }
        // Do not define operator[]
        const T *       operator->() const { return &i; }
        T *             operator->() { return &i; }
        bool            operator==(const iterator &o) const { return i == o.i; }
        bool            operator!=(const iterator &o) const { return !operator==(o); }
        bool            operator==(const const_iterator &o) const { return i==reinterpret_cast<const iterator &>(o).i; }
        bool            operator!=(const const_iterator &o) const { return !operator==(o); }
        iterator &      operator++() { i= i.next(); return *this; }
        iterator        operator++(int) { iterator o= i; i= i.next(); return o; }
        iterator &      operator--() { i= i.previous(); return *this; }
        iterator        operator--(int) { iterator o= i; i= i.previous(); return o; }
        iterator        operator+(int j) const;
        iterator        operator-(int j) const { return operator+(-j); }
        iterator &      operator+=(int j) { return (*this= *this + j); }
        iterator &      operator-=(int j) { return (*this= *this - j); }
    };//QhullLinkedList::iterator

    class const_iterator {

    private:
        T               i;

    public:
        typedef std::bidirectional_iterator_tag  iterator_category;
        typedef T                 value_type;
        typedef const value_type *pointer;
        typedef const value_type &reference;
        typedef ptrdiff_t         difference_type;

                        const_iterator() : i() {}
                        const_iterator(const T &t) : i(t) {}  //!< Automatic conversion to const_iterator
                        const_iterator(const iterator &o) : i(o.i) {}
                        const_iterator(const const_iterator &o) : i(o.i) {}
        const_iterator &operator=(const const_iterator &o) { i= o.i; return *this; }

        const T &       operator*() const { return i; }
        const T *       operator->() const { return i; }
        bool            operator==(const const_iterator &o) const { return i == o.i; }
        bool            operator!=(const const_iterator &o) const { return !operator==(o); }
                        // No comparisons or iterator diff
        const_iterator &operator++() { i= i.next(); return *this; }
        const_iterator  operator++(int) { const_iterator o= i; i= i.next(); return o; }
        const_iterator &operator--() { i= i.previous(); return *this; }
        const_iterator  operator--(int) { const_iterator o= i; i= i.previous(); return o; }
        const_iterator  operator+(int j) const;
        const_iterator  operator-(int j) const { return operator+(-j); }
        const_iterator &operator+=(int j) { return (*this= *this + j); }
        const_iterator &operator-=(int j) { return (*this= *this - j); }
    };//QhullLinkedList::const_iterator

};//QhullLinkedList

template <typename T>
class QhullLinkedListIterator // FIXUP QH11016 define QhullMutableLinkedListIterator
{
    typedef typename QhullLinkedList<T>::const_iterator const_iterator;
    const QhullLinkedList<T> *c;
    const_iterator      i;

public:
                        QhullLinkedListIterator(const QhullLinkedList<T> &container) : c(&container), i(c->constBegin()) {}
    QhullLinkedListIterator & operator=(const QhullLinkedList<T> &container) { c= &container; i= c->constBegin(); return *this; }
    bool                findNext(const T &t);
    bool                findPrevious(const T &t);
    bool                hasNext() const { return i != c->constEnd(); }
    bool                hasPrevious() const { return i != c->constBegin(); }
    T                   next() { return *i++; }
    T                   peekNext() const { return *i; }
    T                   peekPrevious() const { const_iterator p= i; return *--p; }
    T                   previous() { return *--i; }
    void                toFront() { i= c->constBegin(); }
    void                toBack() { i= c->constEnd(); }
};//QhullLinkedListIterator

#//!\name == Definitions =========================================

#//!\name Conversion

#ifndef QHULL_NO_STL
template <typename T>
std::vector<T> QhullLinkedList<T>::
toStdVector() const
{
    std::vector<T> tmp;
    std::copy(constBegin(), constEnd(), std::back_inserter(tmp));
    return tmp;
}//toStdVector
#endif

#ifdef QHULL_USES_QT
template <typename T>
QList<T>  QhullLinkedList<T>::
toQList() const
{
    QhullLinkedListIterator<T> i(*this);
    QList<T> ls;
    while(i.hasNext()){
        ls.append(i.next());
    }
    return ls;
}//toQList
#endif

#//!\name GetSet

template <typename T>
countT QhullLinkedList<T>::
count() const
{
    const_iterator i= begin_node;
    countT c= 0;
    while(i != end_node){
        c++;
        i++;
    }
    return c;
}//count

#//!\name Search

template <typename T>
bool QhullLinkedList<T>::
contains(const T &t) const
{
    const_iterator i= begin_node;
    while(i != end_node){
        if(i==t){
            return true;
        }
        i++;
    }
    return false;
}//contains

template <typename T>
countT QhullLinkedList<T>::
count(const T &t) const
{
    const_iterator i= begin_node;
    countT c= 0;
    while(i != end_node){
        if(i==t){
            c++;
        }
        i++;
    }
    return c;
}//count

template <typename T>
bool QhullLinkedList<T>::
operator==(const QhullLinkedList<T> &l) const
{
    if(begin_node==l.begin_node){
        return (end_node==l.end_node);
    }
    T i= begin_node;
    T il= l.begin_node;
    while(i != end_node){
        if(i != il){
            return false;
        }
        i= static_cast<T>(i.next());
        il= static_cast<T>(il.next());
    }
    if(il != l.end_node){
        return false;
    }
    return true;
}//operator==

#//!\name Iterator

template <typename T>
typename QhullLinkedList<T>::iterator  QhullLinkedList<T>::iterator::
operator+(int j) const
{
    T n= i;
    if(j>0){
        while(j--){
            n= n.next();
        }
    }else{
        while(j++){
            n= n.previous();
        }
    }
    return iterator(n);
}//operator+

template <typename T>
typename QhullLinkedList<T>::const_iterator  QhullLinkedList<T>::const_iterator::
operator+(int j) const
{
    T n= i;
    if(j>0){
        while(j--){
            n= n.next();
        }
    }else{
        while(j++){
            n= n.previous();
        }
    }
    return const_iterator(n);
}//operator+

#//!\name QhullLinkedListIterator

template <typename T>
bool QhullLinkedListIterator<T>::
findNext(const T &t)
{
    while(i != c->constEnd()){
        if (*i++ == t){
            return true;
        }
    }
    return false;
}//findNext

template <typename T>
bool QhullLinkedListIterator<T>::
findPrevious(const T &t)
{
    while(i!=c->constBegin()){
        if(*(--i)==t){
            return true;
        }
    }
    return false;
}//findNext

}//namespace orgQhull

#//!\name Global

template <typename T>
std::ostream &
operator<<(std::ostream &os, const orgQhull::QhullLinkedList<T> &qs)
{
    typename orgQhull::QhullLinkedList<T>::const_iterator i;
    for(i= qs.begin(); i != qs.end(); ++i){
        os << *i;
    }
    return os;
}//operator<<

#endif // QHULLLINKEDLIST_H

