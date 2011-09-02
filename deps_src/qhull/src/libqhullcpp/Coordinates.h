/****************************************************************************
**
** Copyright (c) 2009-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/libqhullcpp/Coordinates.h#6 $$Change: 2079 $
** $DateTime: 2016/02/07 17:43:34 $$Author: bbarber $
**
****************************************************************************/

#ifndef QHCOORDINATES_H
#define QHCOORDINATES_H

#include "libqhull_r/qhull_ra.h"
#include "libqhullcpp/QhullError.h"
#include "libqhullcpp/QhullIterator.h"

#include <cstddef> // ptrdiff_t, size_t
#include <ostream>
// Requires STL vector class.  Can use with another vector class such as QList.
#include <vector>

namespace orgQhull {

#//!\name Defined here
    //! An std::vector of point coordinates independent of dimension
    //! Used by PointCoordinates for RboxPoints and by Qhull for feasiblePoint
    //! A QhullPoint refers to previously allocated coordinates
    class Coordinates;
    class MutableCoordinatesIterator;

class Coordinates {

private:
#//!\name Fields
    std::vector<coordT> coordinate_array;

public:
#//!\name Subtypes

    class const_iterator;
    class iterator;
    typedef iterator Iterator;
    typedef const_iterator ConstIterator;

    typedef coordT              value_type;
    typedef const value_type   *const_pointer;
    typedef const value_type &  const_reference;
    typedef value_type *        pointer;
    typedef value_type &        reference;
    typedef ptrdiff_t           difference_type;
    typedef countT              size_type;

#//!\name Construct
                        Coordinates() {};
    explicit            Coordinates(const std::vector<coordT> &other) : coordinate_array(other) {}
                        Coordinates(const Coordinates &other) : coordinate_array(other.coordinate_array) {}
    Coordinates &       operator=(const Coordinates &other) { coordinate_array= other.coordinate_array; return *this; }
    Coordinates &       operator=(const std::vector<coordT> &other) { coordinate_array= other; return *this; }
                        ~Coordinates() {}

#//!\name Conversion

#ifndef QHULL_NO_STL
    std::vector<coordT> toStdVector() const { return coordinate_array; }
#endif //QHULL_NO_STL
#ifdef QHULL_USES_QT
    QList<coordT>       toQList() const;
#endif //QHULL_USES_QT

#//!\name GetSet
    countT              count() const { return static_cast<countT>(size()); }
    coordT *            data() { return isEmpty() ? 0 : &at(0); }
    const coordT *      data() const { return const_cast<const pointT*>(isEmpty() ? 0 : &at(0)); }
    bool                isEmpty() const { return coordinate_array.empty(); }
    bool                operator==(const Coordinates &other) const  { return coordinate_array==other.coordinate_array; }
    bool                operator!=(const Coordinates &other) const  { return coordinate_array!=other.coordinate_array; }
    size_t              size() const { return coordinate_array.size(); }

#//!\name Element access
    coordT &            at(countT idx) { return coordinate_array.at(idx); }
    const coordT &      at(countT idx) const { return coordinate_array.at(idx); }
    coordT &            back() { return coordinate_array.back(); }
    const coordT &      back() const { return coordinate_array.back(); }
    coordT &            first() { return front(); }
    const coordT &      first() const { return front(); }
    coordT &            front() { return coordinate_array.front(); }
    const coordT &      front() const { return coordinate_array.front(); }
    coordT &            last() { return back(); }
    const coordT &      last() const { return back(); }
    Coordinates         mid(countT idx, countT length= -1) const; //!<\todo countT -1 indicates
    coordT &            operator[](countT idx) { return coordinate_array.operator[](idx); }
    const coordT &      operator[](countT idx) const { return coordinate_array.operator[](idx); }
    coordT              value(countT idx, const coordT &defaultValue) const;

#//!\name Iterator
    iterator            begin() { return iterator(coordinate_array.begin()); }
    const_iterator      begin() const { return const_iterator(coordinate_array.begin()); }
    const_iterator      constBegin() const { return begin(); }
    const_iterator      constEnd() const { return end(); }
    iterator            end() { return iterator(coordinate_array.end()); }
    const_iterator      end() const { return const_iterator(coordinate_array.end()); }

#//!\name GetSet
    Coordinates         operator+(const Coordinates &other) const;

#//!\name Modify
    void                append(int pointDimension, coordT *c);
    void                append(const coordT &c) { push_back(c); }
    void                clear() { coordinate_array.clear(); }
    iterator            erase(iterator idx) { return iterator(coordinate_array.erase(idx.base())); }
    iterator            erase(iterator beginIterator, iterator endIterator) { return iterator(coordinate_array.erase(beginIterator.base(), endIterator.base())); }
    void                insert(countT before, const coordT &c) { insert(begin()+before, c); }
    iterator            insert(iterator before, const coordT &c) { return iterator(coordinate_array.insert(before.base(), c)); }
    void                move(countT from, countT to) { insert(to, takeAt(from)); }
    Coordinates &       operator+=(const Coordinates &other);
    Coordinates &       operator+=(const coordT &c) { append(c); return *this; }
    Coordinates &       operator<<(const Coordinates &other) { return *this += other; }
    Coordinates &       operator<<(const coordT &c) { return *this += c; }
    void                pop_back() { coordinate_array.pop_back(); }
    void                pop_front() { removeFirst(); }
    void                prepend(const coordT &c) { insert(begin(), c); }
    void                push_back(const coordT &c) { coordinate_array.push_back(c); }
    void                push_front(const coordT &c) { insert(begin(), c); }
                        //removeAll below
    void                removeAt(countT idx) { erase(begin()+idx); }
    void                removeFirst() { erase(begin()); }
    void                removeLast() { erase(--end()); }
    void                replace(countT idx, const coordT &c) { (*this)[idx]= c; }
    void                reserve(countT i) { coordinate_array.reserve(i); }
    void                swap(countT idx, countT other);
    coordT              takeAt(countT idx);
    coordT              takeFirst() { return takeAt(0); }
    coordT              takeLast();

#//!\name Search
    bool                contains(const coordT &t) const;
    countT              count(const coordT &t) const;
    countT              indexOf(const coordT &t, countT from = 0) const;
    countT              lastIndexOf(const coordT &t, countT from = -1) const;
    void                removeAll(const coordT &t);

#//!\name Coordinates::iterator -- from QhullPoints, forwarding to coordinate_array
    // before const_iterator for conversion with comparison operators
    // Reviewed corelib/tools/qlist.h and corelib/tools/qvector.h w/o QT_STRICT_ITERATORS
    class iterator {

    private:
        std::vector<coordT>::iterator i;
        friend class const_iterator;

    public:
        typedef std::random_access_iterator_tag  iterator_category;
        typedef coordT      value_type;
        typedef value_type *pointer;
        typedef value_type &reference;
        typedef ptrdiff_t   difference_type;

                        iterator() {}
                        iterator(const iterator &other) { i= other.i; }
        explicit        iterator(const std::vector<coordT>::iterator &vi) { i= vi; }
        iterator &      operator=(const iterator &other) { i= other.i; return *this; }
        std::vector<coordT>::iterator &base() { return i; }
        coordT &        operator*() const { return *i; }
        // No operator->() when the base type is double
        coordT &        operator[](countT idx) const { return i[idx]; }

        bool            operator==(const iterator &other) const { return i==other.i; }
        bool            operator!=(const iterator &other) const { return i!=other.i; }
        bool            operator<(const iterator &other) const { return i<other.i; }
        bool            operator<=(const iterator &other) const { return i<=other.i; }
        bool            operator>(const iterator &other) const { return i>other.i; }
        bool            operator>=(const iterator &other) const { return i>=other.i; }
              // reinterpret_cast to break circular dependency
        bool            operator==(const Coordinates::const_iterator &other) const { return *this==reinterpret_cast<const iterator &>(other); }
        bool            operator!=(const Coordinates::const_iterator &other) const { return *this!=reinterpret_cast<const iterator &>(other); }
        bool            operator<(const Coordinates::const_iterator &other) const { return *this<reinterpret_cast<const iterator &>(other); }
        bool            operator<=(const Coordinates::const_iterator &other) const { return *this<=reinterpret_cast<const iterator &>(other); }
        bool            operator>(const Coordinates::const_iterator &other) const { return *this>reinterpret_cast<const iterator &>(other); }
        bool            operator>=(const Coordinates::const_iterator &other) const { return *this>=reinterpret_cast<const iterator &>(other); }

        iterator &      operator++() { ++i; return *this; }
        iterator        operator++(int) { return iterator(i++); }
        iterator &      operator--() { --i; return *this; }
        iterator        operator--(int) { return iterator(i--); }
        iterator &      operator+=(countT idx) { i += idx; return *this; }
        iterator &      operator-=(countT idx) { i -= idx; return *this; }
        iterator        operator+(countT idx) const { return iterator(i+idx); }
        iterator        operator-(countT idx) const { return iterator(i-idx); }
        difference_type operator-(iterator other) const { return i-other.i; }
    };//Coordinates::iterator

#//!\name Coordinates::const_iterator
    class const_iterator {

    private:
        std::vector<coordT>::const_iterator i;

    public:
        typedef std::random_access_iterator_tag  iterator_category;
        typedef coordT            value_type;
        typedef const value_type *pointer;
        typedef const value_type &reference;
        typedef ptrdiff_t         difference_type;

                        const_iterator() {}
                        const_iterator(const const_iterator &other) { i= other.i; }
                        const_iterator(const iterator &o) : i(o.i) {}
        explicit        const_iterator(const std::vector<coordT>::const_iterator &vi) { i= vi; }
        const_iterator &operator=(const const_iterator &other) { i= other.i; return *this; }
        const coordT &  operator*() const { return *i; }
        // No operator->() when the base type is double
        const coordT &  operator[](countT idx) const { return i[idx]; }

        bool            operator==(const const_iterator &other) const { return i==other.i; }
        bool            operator!=(const const_iterator &other) const { return i!=other.i; }
        bool            operator<(const const_iterator &other) const { return i<other.i; }
        bool            operator<=(const const_iterator &other) const { return i<=other.i; }
        bool            operator>(const const_iterator &other) const { return i>other.i; }
        bool            operator>=(const const_iterator &other) const { return i>=other.i; }

        const_iterator & operator++() { ++i; return *this; } 
        const_iterator  operator++(int) { return const_iterator(i++); }
        const_iterator & operator--() { --i; return *this; }
        const_iterator  operator--(int) { return const_iterator(i--); }
        const_iterator & operator+=(countT idx) { i += idx; return *this; }
        const_iterator & operator-=(countT idx) { i -= idx; return *this; }
        const_iterator  operator+(countT idx) const { return const_iterator(i+idx); }
        const_iterator  operator-(countT idx) const { return const_iterator(i-idx); }
        difference_type operator-(const_iterator other) const { return i-other.i; }
    };//Coordinates::const_iterator

};//Coordinates

//class CoordinatesIterator
//QHULL_DECLARE_SEQUENTIAL_ITERATOR(Coordinates, coordT)

class CoordinatesIterator
{
    typedef Coordinates::const_iterator const_iterator;

private:
    const Coordinates * c;
    const_iterator      i;

public:
                        CoordinatesIterator(const Coordinates &container): c(&container), i(c->constBegin()) {}
    CoordinatesIterator &operator=(const Coordinates &container) { c= &container; i= c->constBegin(); return *this; }
                        ~CoordinatesIterator() {}

    bool                findNext(const coordT &t) { while (i != c->constEnd()) if(*i++ == t){ return true;} return false; }
    bool                findPrevious(const coordT &t) { while (i != c->constBegin())if (*(--i) == t){ return true;} return false;  }
    bool                hasNext() const { return i != c->constEnd(); }
    bool                hasPrevious() const { return i != c->constBegin(); }
    const coordT &      next() { return *i++; }
    const coordT &      previous() { return *--i; }
    const coordT &      peekNext() const { return *i; }
    const coordT &      peekPrevious() const { const_iterator p= i; return *--p; }
    void                toFront() { i= c->constBegin(); }
    void                toBack() { i= c->constEnd(); }
};//CoordinatesIterator

//class MutableCoordinatesIterator
//QHULL_DECLARE_MUTABLE_SEQUENTIAL_ITERATOR(Coordinates, coordT)
class MutableCoordinatesIterator
{
    typedef Coordinates::iterator iterator;
    typedef Coordinates::const_iterator const_iterator;

private:
    Coordinates *       c;
    iterator            i;
    iterator            n;
    bool                item_exists() const { return const_iterator(n) != c->constEnd(); }

public:
                        MutableCoordinatesIterator(Coordinates &container) : c(&container) { i= c->begin(); n= c->end(); }
    MutableCoordinatesIterator &operator=(Coordinates &container) { c= &container; i= c->begin(); n= c->end(); return *this; }
                        ~MutableCoordinatesIterator() {}

    bool                findNext(const coordT &t) { while(c->constEnd()!=const_iterator(n= i)){ if(*i++==t){ return true;}} return false; }
    bool                findPrevious(const coordT &t) { while(c->constBegin()!=const_iterator(i)){ if(*(n= --i)== t){ return true;}} n= c->end(); return false;  }
    bool                hasNext() const { return (c->constEnd()!=const_iterator(i)); }
    bool                hasPrevious() const { return (c->constBegin()!=const_iterator(i)); }
    void                insert(const coordT &t) { n= i= c->insert(i, t); ++i; }
    coordT &            next() { n= i++; return *n; }
    coordT &            peekNext() const { return *i; }
    coordT &            peekPrevious() const { iterator p= i; return *--p; }
    coordT &            previous() { n= --i; return *n; }
    void                remove() { if(c->constEnd()!=const_iterator(n)){ i= c->erase(n); n= c->end();} }
    void                setValue(const coordT &t) const { if(c->constEnd()!=const_iterator(n)){ *n= t;} }
    void                toFront() { i= c->begin(); n= c->end(); }
    void                toBack() { i= c->end(); n= i; }
    coordT &            value() { QHULL_ASSERT(item_exists()); return *n; }
    const coordT &      value() const { QHULL_ASSERT(item_exists()); return *n; }
};//MutableCoordinatesIterator


}//namespace orgQhull

#//!\name Global

std::ostream &operator<<(std::ostream &os, const orgQhull::Coordinates &c);

#endif // QHCOORDINATES_H
