/****************************************************************************
**
** Copyright (c) 2008-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/libqhullcpp/QhullSet.cpp#3 $$Change: 2066 $
** $DateTime: 2016/01/18 19:29:17 $$Author: bbarber $
**
****************************************************************************/

#//! QhullSet -- Qhull's set structure, setT, as a C++ class

#include "libqhullcpp/QhullSet.h"

#include "libqhullcpp/Qhull.h"
#include "libqhullcpp/QhullError.h"

#ifdef _MSC_VER  // Microsoft Visual C++ -- warning level 4
#endif

namespace orgQhull {

#//!\name Class objects

setT QhullSetBase::
s_empty_set;

#//!\name Constructors

QhullSetBase::
QhullSetBase(const Qhull &q, setT *s) 
: qh_set(s ? s : &s_empty_set)
, qh_qh(q.qh())
{
}

#//!\name Class methods

// Same code for qh_setsize [qset_r.c] and QhullSetBase::count [static]
countT QhullSetBase::
count(const setT *set)
{
    countT size;
    const setelemT *sizep;

    if (!set){
        return(0);
    }
    sizep= SETsizeaddr_(set);
    if ((size= sizep->i)) {
        size--;
        if (size > set->maxsize) {
            // FIXUP QH11022 How to add additional output to a error? -- qh_setprint(qhmem.ferr, "set: ", set);
            throw QhullError(10032, "QhullSet internal error: current set size %d is greater than maximum size %d\n",
                size, set->maxsize);
        }
    }else{
        size= set->maxsize;
    }
    return size;
}//count

}//namespace orgQhull

