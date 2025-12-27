/****************************************************************************
**
** Copyright (c) 2008-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/libqhullcpp/QhullIterator.h#4 $$Change: 2079 $
** $DateTime: 2016/02/07 17:43:34 $$Author: bbarber $
**
****************************************************************************/

#ifndef QHULLITERATOR_H
#define QHULLITERATOR_H

#include "libqhull_r/qhull_ra.h"

#include <assert.h>
#include <iterator>
#include <string>
#include <vector>

namespace orgQhull {

#//!\name Defined here
    //! Only QHULL_DECLARE_SEQUENTIAL_ITERATOR is used in libqhullcpp.  The others need further development
    //! QHULL_DECLARE_SEQUENTIAL_ITERATOR(C) -- Declare a Java-style iterator
    //! QHULL_DECLARE_MUTABLE_SEQUENTIAL_ITERATOR(C) -- Declare a mutable Java-style iterator
    //! QHULL_DECLARE_SET_ITERATOR(C) -- Declare a set iterator
    //! QHULL_DECLARE_MUTABLE_SET_ITERATOR(C) -- Declare a mutable set iterator
    //! Derived from Qt/core/tools/qiterator.h and qset_r.h/FOREACHsetelement_()

// Stores C* as done in Mutable...  Assumes the container is not deleted.
// C::const_iterator is an STL-style iterator that returns T&
#define QHULL_DECLARE_SEQUENTIAL_ITERATOR(C, T) \
    \
    class C##Iterator \
    { \
        typedef C::const_iterator const_iterator; \
        const C *c; \
        const_iterator i; \
        public: \
        inline C##Iterator(const C &container) \
        : c(&container), i(c->constBegin()) {} \
        inline C##Iterator &operator=(const C &container) \
        { c = &container; i = c->constBegin(); return *this; } \
        inline void toFront() { i = c->constBegin(); } \
        inline void toBack() { i = c->constEnd(); } \
        inline bool hasNext() const { return i != c->constEnd(); } \
        inline const T &next() { return *i++; } \
        inline const T &peekNext() const { return *i; } \
        inline bool hasPrevious() const { return i != c->constBegin(); } \
        inline const T &previous() { return *--i; } \
        inline const T &peekPrevious() const { const_iterator p = i; return *--p; } \
        inline bool findNext(const T &t) \
        { while (i != c->constEnd()) if (*i++ == t) return true; return false; } \
        inline bool findPrevious(const T &t) \
        { while (i != c->constBegin()) if (*(--i) == t) return true; \
        return false;  } \
    };//C##Iterator

// Remove setShareable() from Q_DECLARE_MUTABLE_SEQUENTIAL_ITERATOR
// Uses QHULL_ASSERT (assert.h)
// Duplicated in MutablePointIterator without insert or remove
// Not used in libqhullcpp.  See Coordinates.h
#define QHULL_DECLARE_MUTABLE_SEQUENTIAL_ITERATOR(C, T) \
    class Mutable##C##Iterator \
    { \
        typedef C::iterator iterator; \
        typedef C::const_iterator const_iterator; \
        C *c; \
        iterator i, n; \
        inline bool item_exists() const { return const_iterator(n) != c->constEnd(); } \
        public: \
        inline Mutable##C##Iterator(C &container) \
        : c(&container) \
        { i = c->begin(); n = c->end(); } \
        inline ~Mutable##C##Iterator() \
        {} \
        inline Mutable##C##Iterator &operator=(C &container) \
        { c = &container; \
        i = c->begin(); n = c->end(); return *this; } \
        inline void toFront() { i = c->begin(); n = c->end(); } \
        inline void toBack() { i = c->end(); n = i; } \
        inline bool hasNext() const { return c->constEnd() != const_iterator(i); } \
        inline T &next() { n = i++; return *n; } \
        inline T &peekNext() const { return *i; } \
        inline bool hasPrevious() const { return c->constBegin() != const_iterator(i); } \
        inline T &previous() { n = --i; return *n; } \
        inline T &peekPrevious() const { iterator p = i; return *--p; } \
        inline void remove() \
        { if (c->constEnd() != const_iterator(n)) { i = c->erase(n); n = c->end(); } } \
        inline void setValue(const T &t) const { if (c->constEnd() != const_iterator(n)) *n = t; } \
        inline T &value() { QHULL_ASSERT(item_exists()); return *n; } \
        inline const T &value() const { QHULL_ASSERT(item_exists()); return *n; } \
        inline void insert(const T &t) { n = i = c->insert(i, t); ++i; } \
        inline bool findNext(const T &t) \
        { while (c->constEnd() != const_iterator(n = i)) if (*i++ == t) return true; return false; } \
        inline bool findPrevious(const T &t) \
        { while (c->constBegin() != const_iterator(i)) if (*(n = --i) == t) return true; \
        n = c->end(); return false;  } \
    };//Mutable##C##Iterator

// Not used in libqhullcpp.
#define QHULL_DECLARE_SET_ITERATOR(C) \
\
    template <class T> \
    class Qhull##C##Iterator \
    { \
        typedef typename Qhull##C<T>::const_iterator const_iterator; \
        Qhull##C<T> c; \
        const_iterator i; \
    public: \
        inline Qhull##C##Iterator(const Qhull##C<T> &container) \
        : c(container), i(c.constBegin()) {} \
        inline Qhull##C##Iterator &operator=(const Qhull##C<T> &container) \
        { c = container; i = c.constBegin(); return *this; } \
        inline void toFront() { i = c.constBegin(); } \
        inline void toBack() { i = c.constEnd(); } \
        inline bool hasNext() const { return i != c.constEnd(); } \
        inline const T &next() { return *i++; } \
        inline const T &peekNext() const { return *i; } \
        inline bool hasPrevious() const { return i != c.constBegin(); } \
        inline const T &previous() { return *--i; } \
        inline const T &peekPrevious() const { const_iterator p = i; return *--p; } \
        inline bool findNext(const T &t) \
        { while (i != c.constEnd()) if (*i++ == t) return true; return false; } \
        inline bool findPrevious(const T &t) \
        { while (i != c.constBegin()) if (*(--i) == t) return true; \
        return false;  } \
    };//Qhull##C##Iterator

// Not used in libqhullcpp.
#define QHULL_DECLARE_MUTABLE_SET_ITERATOR(C) \
\
template <class T> \
class QhullMutable##C##Iterator \
{ \
    typedef typename Qhull##C::iterator iterator; \
    typedef typename Qhull##C::const_iterator const_iterator; \
    Qhull##C *c; \
    iterator i, n; \
    inline bool item_exists() const { return const_iterator(n) != c->constEnd(); } \
public: \
    inline Mutable##C##Iterator(Qhull##C &container) \
        : c(&container) \
    { c->setSharable(false); i = c->begin(); n = c->end(); } \
    inline ~Mutable##C##Iterator() \
    { c->setSharable(true); } \
    inline Mutable##C##Iterator &operator=(Qhull##C &container) \
    { c->setSharable(true); c = &container; c->setSharable(false); \
      i = c->begin(); n = c->end(); return *this; } \
    inline void toFront() { i = c->begin(); n = c->end(); } \
    inline void toBack() { i = c->end(); n = i; } \
    inline bool hasNext() const { return c->constEnd() != const_iterator(i); } \
    inline T &next() { n = i++; return *n; } \
    inline T &peekNext() const { return *i; } \
    inline bool hasPrevious() const { return c->constBegin() != const_iterator(i); } \
    inline T &previous() { n = --i; return *n; } \
    inline T &peekPrevious() const { iterator p = i; return *--p; } \
    inline void remove() \
    { if (c->constEnd() != const_iterator(n)) { i = c->erase(n); n = c->end(); } } \
    inline void setValue(const T &t) const { if (c->constEnd() != const_iterator(n)) *n = t; } \
    inline T &value() { Q_ASSERT(item_exists()); return *n; } \
    inline const T &value() const { Q_ASSERT(item_exists()); return *n; } \
    inline void insert(const T &t) { n = i = c->insert(i, t); ++i; } \
    inline bool findNext(const T &t) \
    { while (c->constEnd() != const_iterator(n = i)) if (*i++ == t) return true; return false; } \
    inline bool findPrevious(const T &t) \
    { while (c->constBegin() != const_iterator(i)) if (*(n = --i) == t) return true; \
      n = c->end(); return false;  } \
};//QhullMutable##C##Iterator

}//namespace orgQhull

#endif // QHULLITERATOR_H

