/****************************************************************************
**
** Copyright (c) 2009-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/libqhullcpp/QhullVertexSet.h#2 $$Change: 2066 $
** $DateTime: 2016/01/18 19:29:17 $$Author: bbarber $
**
****************************************************************************/

#ifndef QHULLVERTEXSET_H
#define QHULLVERTEXSET_H

#include "libqhullcpp/QhullSet.h"
#include "libqhullcpp/QhullVertex.h"

#include <ostream>

namespace orgQhull {

#//!\name Used here

#//!\name Defined here
    //! QhullVertexSet -- a set of Qhull Vertices, as a C++ class.
    //! See Qhull
    class QhullVertexSet;
    typedef QhullSetIterator<QhullVertex> QhullVertexSetIterator;

class QhullVertexSet : public QhullSet<QhullVertex> {

private:
#//!\name Fields
    bool                qhsettemp_defined;  //! Set was allocated with qh_settemp()

public:
#//!\name Constructor
                        QhullVertexSet(const Qhull &q, setT *s) : QhullSet<QhullVertex>(q, s), qhsettemp_defined(false) {}
                        QhullVertexSet(const Qhull &q, facetT *facetlist, setT *facetset, bool allfacets);
                        //Conversion from setT* is not type-safe.  Implicit conversion for void* to T
                        QhullVertexSet(QhullQh *qqh, setT *s) : QhullSet<QhullVertex>(qqh, s), qhsettemp_defined(false) {}
                        QhullVertexSet(QhullQh *qqh, facetT *facetlist, setT *facetset, bool allfacets);
                        //Copy constructor and assignment copies pointer but not contents.  Throws error if qhsettemp_defined.  Needed for return by value.
                        QhullVertexSet(const QhullVertexSet &other);
    QhullVertexSet &    operator=(const QhullVertexSet &other);
                        ~QhullVertexSet();

private:                //!Default constructor disabled.  Will implement allocation later
                        QhullVertexSet();
public:

#//!\name Destructor
    void                freeQhSetTemp();

#//!\name Conversion
#ifndef QHULL_NO_STL
    std::vector<QhullVertex> toStdVector() const;
#endif //QHULL_NO_STL
#ifdef QHULL_USES_QT
    QList<QhullVertex>   toQList() const;
#endif //QHULL_USES_QT

#//!\name IO
    struct PrintVertexSet{
        const QhullVertexSet *vertex_set;
        const char *    print_message;     //!< non-null message
                        
                        PrintVertexSet(const char *message, const QhullVertexSet *s) : vertex_set(s), print_message(message) {}
    };//PrintVertexSet
    const PrintVertexSet print(const char *message) const { return PrintVertexSet(message, this); }

    struct PrintIdentifiers{
        const QhullVertexSet *vertex_set;
        const char *    print_message;    //!< non-null message
                        PrintIdentifiers(const char *message, const QhullVertexSet *s) : vertex_set(s), print_message(message) {}
    };//PrintIdentifiers
    PrintIdentifiers    printIdentifiers(const char *message) const { return PrintIdentifiers(message, this); }

};//class QhullVertexSet

}//namespace orgQhull

#//!\name Global

std::ostream &operator<<(std::ostream &os, const orgQhull::QhullVertexSet::PrintVertexSet &pr);
std::ostream &operator<<(std::ostream &os, const orgQhull::QhullVertexSet::PrintIdentifiers &p);
inline std::ostream &operator<<(std::ostream &os, const orgQhull::QhullVertexSet &vs) { os << vs.print(""); return os; }

#endif // QHULLVERTEXSET_H
