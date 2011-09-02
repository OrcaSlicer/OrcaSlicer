/****************************************************************************
**
** Copyright (c) 2008-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/libqhullcpp/RoadError.h#4 $$Change: 2079 $
** $DateTime: 2016/02/07 17:43:34 $$Author: bbarber $
**
****************************************************************************/

#ifndef ROADERROR_H
#define ROADERROR_H

#include "libqhull_r/user_r.h"  /* for QHULL_CRTDBG */
#include "libqhullcpp/RoadLogEvent.h"

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

using std::endl;

namespace orgQhull {

#//!\name Defined here
    //! RoadError -- Report and log errors
    //!  See discussion in Saylan, G., "Practical C++ error handling in hybrid environments," Dr. Dobb's Journal, p. 50-55, March 2007.
    //!   He uses an auto_ptr to track a stringstream.  It constructs a string on the fly.  RoadError uses the copy constructor to transform RoadLogEvent into a string
    class RoadError;

class RoadError : public std::exception {

private:
#//!\name Fields
    int                 error_code;  //! Non-zero code (not logged), maybe returned as program status
    RoadLogEvent        log_event;   //! Format string w/ arguments
    mutable std::string error_message;  //! Formated error message.  Must be after log_event.

#//!\name Class fields
    static const char  *  ROADtag;
    static std::ostringstream  global_log; //!< May be replaced with any ostream object
                                    //!< Not reentrant -- only used by RoadError::logErrorLastResort()

public:
#//!\name Constants

#//!\name Constructors
    RoadError();
    RoadError(const RoadError &other);  //! Called on throw, generates error_message
    RoadError(int code, const std::string &message);
    RoadError(int code, const char *fmt);
    RoadError(int code, const char *fmt, int d);
    RoadError(int code, const char *fmt, int d, int d2);
    RoadError(int code, const char *fmt, int d, int d2, float f);
    RoadError(int code, const char *fmt, int d, int d2, float f, const char *s);
    RoadError(int code, const char *fmt, int d, int d2, float f, const void *x);
    RoadError(int code, const char *fmt, int d, int d2, float f, int i);
    RoadError(int code, const char *fmt, int d, int d2, float f, long long i);
    RoadError(int code, const char *fmt, int d, int d2, float f, double e);

    RoadError &         operator=(const RoadError &other);
                        ~RoadError() throw() {};

#//!\name Class methods

    static void         clearGlobalLog() { global_log.seekp(0); }
    static bool         emptyGlobalLog() { return global_log.tellp()<=0; }
    static const char  *stringGlobalLog() { return global_log.str().c_str(); }

#//!\name Virtual
    virtual const char *what() const throw();

#//!\name GetSet
    bool                isValid() const { return log_event.isValid(); }
    int                 errorCode() const { return error_code; };
   // FIXUP QH11021 should RoadError provide errorMessage().  Currently what()
    RoadLogEvent        roadLogEvent() const { return log_event; };

#//!\name Update
    void                logErrorLastResort() const;
};//class RoadError

}//namespace orgQhull

#//!\name Global

inline std::ostream &   operator<<(std::ostream &os, const orgQhull::RoadError &e) { return os << e.what(); }

#endif // ROADERROR_H
