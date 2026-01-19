/****************************************************************************
**
** Copyright (c) 2008-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/libqhullcpp/RoadLogEvent.h#1 $$Change: 1981 $
** $DateTime: 2015/09/28 20:26:32 $$Author: bbarber $
**
****************************************************************************/

#ifndef ROADLOGEVENT_H
#define ROADLOGEVENT_H

#include <ostream>
#include <stdexcept>
#include <string>

namespace orgQhull {

#//!\name Defined here
    //! RoadLogEvent -- Record an event for the RoadLog
    struct RoadLogEvent;

struct RoadLogEvent {

public:
#//!\name Fields
    const char *    format_string; //! Format string (a literal with format codes, for logging)
    int             int_1;       //! Integer argument (%d, for logging)
    int             int_2;       //! Integer argument (%d, for logging)
    float           float_1;     //! Float argument (%f, for logging)
    union {                      //! One additional argument (for logging)
        const char *cstr_1;      //!   Cstr argument (%s) -- type checked at construct-time
        const void *void_1;      //!   Void* argument (%x) -- Use upper-case codes for object types
        long long   int64_1;     //!   signed int64 (%i).  Ambiguous if unsigned is also defined.
        double      double_1;    //!   Double argument (%e)
    };

#//!\name Constants

#//!\name Constructors
    RoadLogEvent() : format_string(0), int_1(0), int_2(0), float_1(0), int64_1(0) {};
    explicit RoadLogEvent(const char *fmt) : format_string(fmt), int_1(0), int_2(0), float_1(0), int64_1(0) {};
    RoadLogEvent(const char *fmt, int d) : format_string(fmt), int_1(d), int_2(0), float_1(0), int64_1(0) {};
    RoadLogEvent(const char *fmt, int d, int d2) : format_string(fmt), int_1(d), int_2(d2), float_1(0), int64_1(0) {};
    RoadLogEvent(const char *fmt, int d, int d2, float f) : format_string(fmt), int_1(d), int_2(d2), float_1(f), int64_1(0) {};
    RoadLogEvent(const char *fmt, int d, int d2, float f, const char *s) : format_string(fmt), int_1(d), int_2(d2), float_1(f), cstr_1(s) {};
    RoadLogEvent(const char *fmt, int d, int d2, float f, const void *x) : format_string(fmt), int_1(d), int_2(d2), float_1(f), void_1(x) {};
    RoadLogEvent(const char *fmt, int d, int d2, float f, int i) : format_string(fmt), int_1(d), int_2(d2), float_1(f), int64_1(i) {};
    RoadLogEvent(const char *fmt, int d, int d2, float f, long long i) : format_string(fmt), int_1(d), int_2(d2), float_1(f), int64_1(i) {};
    RoadLogEvent(const char *fmt, int d, int d2, float f, double g) : format_string(fmt), int_1(d), int_2(d2), float_1(f), double_1(g) {};
    ~RoadLogEvent() {};
    //! Default copy constructor and assignment

#//!\name GetSet
    bool                isValid() const { return format_string!=0; }
    int                 int1() const { return int_1; };
    int                 int2() const { return int_2; };
    float               float1() const { return float_1; };
    const char *        format() const { return format_string; };
    const char *        cstr1() const { return cstr_1; };
    const void *        void1() const { return void_1; };
    long long           int64() const { return int64_1; };
    double              double1() const { return double_1; };

#//!\name Conversion

    std::string        toString(const char* tag, int code) const;

private:
#//!\name Class helpers
    static bool         firstExtraCode(std::ostream &os, char c, char *extraCode);


};//class RoadLogEvent

}//namespace orgQhull

#endif // ROADLOGEVENT_H
