/****************************************************************************
**
** Copyright (c) 2008-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/libqhullcpp/RboxPoints.cpp#3 $$Change: 2066 $
** $DateTime: 2016/01/18 19:29:17 $$Author: bbarber $
**
****************************************************************************/

#include "libqhullcpp/RboxPoints.h"

#include "libqhullcpp/QhullError.h"

#include <iostream>

using std::cerr;
using std::endl;
using std::istream;
using std::ostream;
using std::ostringstream;
using std::string;
using std::vector;
using std::ws;

#ifdef _MSC_VER  // Microsoft Visual C++ -- warning level 4
#pragma warning( disable : 4996)  // function was declared deprecated(strcpy, localtime, etc.)
#endif

namespace orgQhull {

#//! RboxPoints -- generate random PointCoordinates for qhull (rbox)


#//!\name Constructors
RboxPoints::
RboxPoints()
: PointCoordinates("rbox")
, rbox_new_count(0)
, rbox_status(qh_ERRnone)
, rbox_message()
{
    allocateQhullQh();
}

//! Allocate and generate points according to rboxCommand
//! For rbox commands, see http://www.qhull.org/html/rbox.htm or html/rbox.htm
//! Same as appendPoints()
RboxPoints::
RboxPoints(const char *rboxCommand)
: PointCoordinates("rbox")
, rbox_new_count(0)
, rbox_status(qh_ERRnone)
, rbox_message()
{
    allocateQhullQh();
    // rbox arguments added to comment() via qh_rboxpoints > qh_fprintf_rbox
    appendPoints(rboxCommand);
}

RboxPoints::
~RboxPoints()
{
    delete qh();
    resetQhullQh(0);
}

// RboxPoints and qh_rboxpoints has several fields in qhT (rbox_errexit..cpp_object)
// It shares last_random with qh_rand and qh_srand
// The other fields are unused
void RboxPoints::
allocateQhullQh()
{
    QHULL_LIB_CHECK /* Check for compatible library */
    resetQhullQh(new QhullQh);
}//allocateQhullQh

#//!\name Messaging

void RboxPoints::
clearRboxMessage()
{
    rbox_status= qh_ERRnone;
    rbox_message.clear();
}//clearRboxMessage

std::string RboxPoints::
rboxMessage() const
{
    if(rbox_status!=qh_ERRnone){
        return rbox_message;
    }
    if(isEmpty()){
        return "rbox warning: no points generated\n";
    }
    return "rbox: OK\n";
}//rboxMessage

int RboxPoints::
rboxStatus() const
{
    return rbox_status;
}

bool RboxPoints::
hasRboxMessage() const
{
    return (rbox_status!=qh_ERRnone);
}

#//!\name Methods

//! Appends points as defined by rboxCommand
//! Appends rboxCommand to comment
//! For rbox commands, see http://www.qhull.org/html/rbox.htm or html/rbox.htm
void RboxPoints::
appendPoints(const char *rboxCommand)
{
    string s("rbox ");
    s += rboxCommand;
    char *command= const_cast<char*>(s.c_str());
    if(qh()->cpp_object){
        throw QhullError(10001, "Qhull error: conflicting user of cpp_object for RboxPoints::appendPoints() or corrupted qh_qh");
    }
    if(extraCoordinatesCount()!=0){
        throw QhullError(10067, "Qhull error: Extra coordinates (%d) prior to calling RboxPoints::appendPoints.  Was %s", extraCoordinatesCount(), 0, 0.0, comment().c_str());
    }
    countT previousCount= count();
    qh()->cpp_object= this;           // for qh_fprintf_rbox()
    int status= qh_rboxpoints(qh(), command);
    qh()->cpp_object= 0;
    if(rbox_status==qh_ERRnone){
        rbox_status= status;
    }
    if(rbox_status!=qh_ERRnone){
        throw QhullError(rbox_status, rbox_message);
    }
    if(extraCoordinatesCount()!=0){
        throw QhullError(10002, "Qhull error: extra coordinates (%d) for PointCoordinates (%x)", extraCoordinatesCount(), 0, 0.0, coordinates());
    }
    if(previousCount+newCount()!=count()){
        throw QhullError(10068, "Qhull error: rbox specified %d points but got %d points for command '%s'", newCount(), count()-previousCount, 0.0, comment().c_str());
    }
}//appendPoints

}//namespace orgQhull

#//!\name Global functions

/*-<a                             href="qh-user.htm#TOC"
>-------------------------------</a><a name="qh_fprintf_rbox">-</a>

  qh_fprintf_rbox(qh, fp, msgcode, format, list of args )
    fp is ignored (replaces qh_fprintf_rbox() in userprintf_rbox.c)
    cpp_object == RboxPoints

notes:
    only called from qh_rboxpoints()
    same as fprintf() and Qhull.qh_fprintf()
    fgets() is not trapped like fprintf()
    Do not throw errors from here.  Use qh_errexit_rbox;
    A similar technique can be used for qh_fprintf to capture all of its output
*/
extern "C"
void qh_fprintf_rbox(qhT *qh, FILE*, int msgcode, const char *fmt, ... ) {
    va_list args;

    using namespace orgQhull;

    if(!qh->cpp_object){
        qh_errexit_rbox(qh, 10072);
    }
    RboxPoints *out= reinterpret_cast<RboxPoints *>(qh->cpp_object);
    va_start(args, fmt);
    if(msgcode<MSG_OUTPUT){
        char newMessage[MSG_MAXLEN];
        // RoadError provides the message tag
        vsnprintf(newMessage, sizeof(newMessage), fmt, args);
        out->rbox_message += newMessage;
        if(out->rbox_status<MSG_ERROR || out->rbox_status>=MSG_STDERR){
            out->rbox_status= msgcode;
        }
        va_end(args);
        return;
    }
    switch(msgcode){
    case 9391:
    case 9392:
        out->rbox_message += "RboxPoints error: options 'h', 'n' not supported.\n";
        qh_errexit_rbox(qh, 10010);
        /* never returns */
    case 9393:  // FIXUP QH11026 countT vs. int
        {
            int dimension= va_arg(args, int);
            string command(va_arg(args, char*));
            countT count= va_arg(args, countT);
            out->setDimension(dimension);
            out->appendComment(" \"");
            out->appendComment(command.substr(command.find(' ')+1));
            out->appendComment("\"");
            out->setNewCount(count);
            out->reservePoints();
        }
        break;
    case 9407:
        *out << va_arg(args, int);
        // fall through
    case 9405:
        *out << va_arg(args, int);
        // fall through
    case 9403:
        *out << va_arg(args, int);
        break;
    case 9408:
        *out << va_arg(args, double);
        // fall through
    case 9406:
        *out << va_arg(args, double);
        // fall through
    case 9404:
        *out << va_arg(args, double);
        break;
    }
    va_end(args);
} /* qh_fprintf_rbox */

