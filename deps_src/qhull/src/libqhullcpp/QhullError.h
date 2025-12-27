/****************************************************************************
**
** Copyright (c) 2008-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/libqhullcpp/QhullError.h#2 $$Change: 2066 $
** $DateTime: 2016/01/18 19:29:17 $$Author: bbarber $
**
****************************************************************************/

#ifndef QHULLERROR_H
#define QHULLERROR_H

#include "libqhullcpp/RoadError.h"
// No dependencies on libqhull

#ifndef QHULL_ASSERT
#define QHULL_ASSERT assert
#include <assert.h>
#endif

namespace orgQhull {

#//!\name Defined here
    //! QhullError -- std::exception class for Qhull
    class QhullError;

class QhullError : public RoadError {

public:
#//!\name Constants
    enum {
        QHULLfirstError= 10000, //MSG_QHULL_ERROR in Qhull's user.h
        QHULLlastError= 10078,
        NOthrow= 1 //! For flag to indexOf()
    };

#//!\name Constructors
    // default constructors
    QhullError() : RoadError() {};
    QhullError(const QhullError &other) : RoadError(other) {}
    QhullError(int code, const std::string &message) : RoadError(code, message) {};
    QhullError(int code, const char *fmt) : RoadError(code, fmt) {};
    QhullError(int code, const char *fmt, int d) : RoadError(code, fmt, d) {};
    QhullError(int code, const char *fmt, int d, int d2) : RoadError(code, fmt, d, d2) {};
    QhullError(int code, const char *fmt, int d, int d2, float f) : RoadError(code, fmt, d, d2, f) {};
    QhullError(int code, const char *fmt, int d, int d2, float f, const char *s) : RoadError(code, fmt, d, d2, f, s) {};
    QhullError(int code, const char *fmt, int d, int d2, float f, const void *x) : RoadError(code, fmt, d, d2, f, x) {};
    QhullError(int code, const char *fmt, int d, int d2, float f, int i) : RoadError(code, fmt, d, d2, f, i) {};
    QhullError(int code, const char *fmt, int d, int d2, float f, long long i) : RoadError(code, fmt, d, d2, f, i) {};
    QhullError(int code, const char *fmt, int d, int d2, float f, double e) : RoadError(code, fmt, d, d2, f, e) {};
    QhullError &operator=(const QhullError &other) { this->RoadError::operator=(other); return *this; }
    ~QhullError() throw() {}

};//class QhullError


}//namespace orgQhull

#//!\name Global

inline std::ostream &operator<<(std::ostream &os, const orgQhull::QhullError &e) { return os << e.what(); }

#endif // QHULLERROR_H
