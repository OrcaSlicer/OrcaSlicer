/****************************************************************************
**
** Copyright (c) 2008-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/libqhullcpp/QhullVertex.h#4 $$Change: 2079 $
** $DateTime: 2016/02/07 17:43:34 $$Author: bbarber $
**
****************************************************************************/

#ifndef QHULLVERTEX_H
#define QHULLVERTEX_H

#include "libqhull_r/qhull_ra.h"
#include "libqhullcpp/QhullPoint.h"
#include "libqhullcpp/QhullLinkedList.h"
#include "libqhullcpp/QhullSet.h"

#include <ostream>

namespace orgQhull {

#//!\name Used here
    class QhullFacetSet;

#//!\name Defined here
    //! QhullVertex -- Qhull's vertex structure, vertexT [libqhull_r.h], as a C++ class
    class QhullVertex;
    typedef QhullLinkedList<QhullVertex> QhullVertexList;
    typedef QhullLinkedListIterator<QhullVertex> QhullVertexListIterator;


/*********************
  topological information:
    next,previous       doubly-linked list of all vertices
    neighborFacets           set of adjacent facets (only if qh.VERTEXneighbors)

  geometric information:
    point               array of DIM coordinates
*/

class QhullVertex {

#//!\name Defined here
public:
    typedef vertexT *   base_type;  // for QhullVertexSet

private:
#//!\name Fields
    vertexT *           qh_vertex;  //!< Corresponding vertexT, never 0
    QhullQh *           qh_qh;      //!< QhullQh/qhT for vertexT, may be 0

#//!\name Class objects
    static vertexT      s_empty_vertex;  // needed for shallow copy

public:
#//!\name Constants

#//!\name Constructors
                        QhullVertex() : qh_vertex(&s_empty_vertex), qh_qh(0) {}
    explicit            QhullVertex(const Qhull &q);
                        QhullVertex(const Qhull &q, vertexT *v);
    explicit            QhullVertex(QhullQh *qqh) : qh_vertex(&s_empty_vertex), qh_qh(qqh) {}
                        QhullVertex(QhullQh *qqh, vertexT *v) : qh_vertex(v ? v : &s_empty_vertex), qh_qh(qqh) {}
                        // Creates an alias.  Does not copy QhullVertex.  Needed for return by value and parameter passing
                        QhullVertex(const QhullVertex &other) : qh_vertex(other.qh_vertex), qh_qh(other.qh_qh) {}
                        // Creates an alias.  Does not copy QhullVertex.  Needed for vector<QhullVertex>
    QhullVertex &       operator=(const QhullVertex &other) { qh_vertex= other.qh_vertex; qh_qh= other.qh_qh; return *this; }
                        ~QhullVertex() {}

#//!\name GetSet
    int                 dimension() const { return (qh_qh ? qh_qh->hull_dim : 0); }
    vertexT *           getBaseT() const { return getVertexT(); } //!< For QhullSet<QhullVertex>
    vertexT *           getVertexT() const { return qh_vertex; }
    countT              id() const { return qh_vertex->id; }
    bool                isValid() const { return (qh_qh && qh_vertex != &s_empty_vertex); }
                        //! True if defineVertexNeighborFacets() already called.  Auotomatically set for facet merging, Voronoi diagrams
    bool                neighborFacetsDefined() const { return qh_vertex->neighbors != 0; }
    QhullVertex         next() const { return QhullVertex(qh_qh, qh_vertex->next); }
    bool                operator==(const QhullVertex &other) const { return qh_vertex==other.qh_vertex; }
    bool                operator!=(const QhullVertex &other) const { return !operator==(other); }
    QhullPoint          point() const { return QhullPoint(qh_qh, qh_vertex->point); }
    QhullVertex         previous() const { return QhullVertex(qh_qh, qh_vertex->previous); }
    QhullQh *           qh() const { return qh_qh; }

#//!\name foreach
    //See also QhullVertexList
    QhullFacetSet       neighborFacets() const;

#//!\name IO
    struct PrintVertex{
        const QhullVertex *vertex;
        const char *    print_message;    //!< non-null message
                        PrintVertex(const char *message, const QhullVertex &v) : vertex(&v), print_message(message) {}
    };//PrintVertex
    PrintVertex         print(const char *message) const { return PrintVertex(message, *this); }
};//class QhullVertex

}//namespace orgQhull

#//!\name GLobal

std::ostream &operator<<(std::ostream &os, const orgQhull::QhullVertex::PrintVertex &pr);
inline std::ostream &operator<<(std::ostream &os, const orgQhull::QhullVertex &v) { os << v.print(""); return os; }

#endif // QHULLVERTEX_H
