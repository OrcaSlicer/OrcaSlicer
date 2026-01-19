/****************************************************************************
**
** Copyright (c) 2008-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/libqhullcpp/QhullQh.cpp#5 $$Change: 2066 $
** $DateTime: 2016/01/18 19:29:17 $$Author: bbarber $
**
****************************************************************************/

#//! QhullQh -- Qhull's global data structure, qhT, as a C++ class


#include "libqhullcpp/QhullQh.h"

#include "libqhullcpp/QhullError.h"
#include "libqhullcpp/QhullStat.h"

#include <sstream>
#include <iostream>

#include <stdarg.h>

using std::cerr;
using std::string;
using std::vector;
using std::ostream;

#ifdef _MSC_VER  // Microsoft Visual C++ -- warning level 4
#pragma warning( disable : 4611)  // interaction between '_setjmp' and C++ object destruction is non-portable
#pragma warning( disable : 4996)  // function was declared deprecated(strcpy, localtime, etc.)
#endif

namespace orgQhull {

#//!\name Global variables
const double QhullQh::
default_factor_epsilon= 1.0;

#//!\name Constructor, destructor, etc.

//! Derived from qh_new_qhull[user.c]
QhullQh::
QhullQh()
: qhull_status(qh_ERRnone)
, qhull_message()
, error_stream(0)
, output_stream(0)
, factor_epsilon(QhullQh::default_factor_epsilon)
, use_output_stream(false)
{
    // NOerrors: TRY_QHULL_ not needed since these routines do not call qh_errexit()
    qh_meminit(this, NULL);
    qh_initstatistics(this);
    qh_initqhull_start2(this, NULL, NULL, qh_FILEstderr);  // Initialize qhT
    this->ISqhullQh= True;
}//QhullQh

QhullQh::
~QhullQh()
{
    checkAndFreeQhullMemory();
}//~QhullQh

#//!\name Methods

//! Check memory for internal consistency
//! Free global memory used by qh_initbuild and qh_buildhull
//! Zero the qhT data structure, except for memory (qhmemT) and statistics (qhstatT)
//! Check and free short memory (e.g., facetT)
//! Zero the qhmemT data structure
void QhullQh::
checkAndFreeQhullMemory()
{
#ifdef qh_NOmem
    qh_freeqhull(this, qh_ALL);
#else
    qh_memcheck(this);
    qh_freeqhull(this, !qh_ALL);
    countT curlong;
    countT totlong;
    qh_memfreeshort(this, &curlong, &totlong);
    if (curlong || totlong)
        throw QhullError(10026, "Qhull error: qhull did not free %d bytes of long memory (%d pieces).", totlong, curlong);
#endif
}//checkAndFreeQhullMemory

#//!\name Messaging

void QhullQh::
appendQhullMessage(const string &s)
{
    if(output_stream && use_output_stream && this->USEstdout){ 
        *output_stream << s;
    }else if(error_stream){
        *error_stream << s;
    }else{
        qhull_message += s;
    }
}//appendQhullMessage

//! clearQhullMessage does not throw errors (~Qhull)
void QhullQh::
clearQhullMessage()
{
    qhull_status= qh_ERRnone;
    qhull_message.clear();
    RoadError::clearGlobalLog();
}//clearQhullMessage

//! hasQhullMessage does not throw errors (~Qhull)
bool QhullQh::
hasQhullMessage() const
{
    return (!qhull_message.empty() || qhull_status!=qh_ERRnone);
    //FIXUP QH11006 -- inconsistent usage with Rbox.  hasRboxMessage just tests rbox_status.  No appendRboxMessage()
}

void QhullQh::
maybeThrowQhullMessage(int exitCode)
{
    if(!NOerrexit){
        if(qhull_message.size()>0){
            qhull_message.append("\n");
        }
        if(exitCode || qhull_status==qh_ERRnone){
            qhull_status= 10073;
        }else{
            qhull_message.append("QH10073: ");
        }
        qhull_message.append("Cannot call maybeThrowQhullMessage() from QH_TRY_().  Or missing 'qh->NOerrexit=true;' after QH_TRY_(){...}.");
    }
    if(qhull_status==qh_ERRnone){
        qhull_status= exitCode;
    }
    if(qhull_status!=qh_ERRnone){
        QhullError e(qhull_status, qhull_message);
        clearQhullMessage();
        throw e; // FIXUP QH11007: copy constructor is expensive if logging
    }
}//maybeThrowQhullMessage

void QhullQh::
maybeThrowQhullMessage(int exitCode, int noThrow)  throw()
{
    QHULL_UNUSED(noThrow);

    if(qhull_status==qh_ERRnone){
        qhull_status= exitCode;
    }
    if(qhull_status!=qh_ERRnone){
        QhullError e(qhull_status, qhull_message);
        e.logErrorLastResort();
    }
}//maybeThrowQhullMessage

//! qhullMessage does not throw errors (~Qhull)
std::string QhullQh::
qhullMessage() const
{
    if(qhull_message.empty() && qhull_status!=qh_ERRnone){
        return "qhull: no message for error.  Check cerr or error stream\n";
    }else{
        return qhull_message;
    }
}//qhullMessage

int QhullQh::
qhullStatus() const
{
    return qhull_status;
}//qhullStatus

void QhullQh::
setErrorStream(ostream *os)
{
    error_stream= os;
}//setErrorStream

//! Updates use_output_stream
void QhullQh::
setOutputStream(ostream *os)
{
    output_stream= os;
    use_output_stream= (os!=0);
}//setOutputStream

}//namespace orgQhull

/*-<a                             href="qh_qh-user.htm#TOC"
 >-------------------------------</a><a name="qh_fprintf">-</a>

  qh_fprintf(qhT *qh, fp, msgcode, format, list of args )
    replaces qh_fprintf() in userprintf_r.c

notes:
    only called from libqhull
    same as fprintf() and RboxPoints.qh_fprintf_rbox()
    fgets() is not trapped like fprintf()
    Do not throw errors from here.  Use qh_errexit;
*/
extern "C"
void qh_fprintf(qhT *qh, FILE *fp, int msgcode, const char *fmt, ... ) {
    va_list args;

    using namespace orgQhull;

    if(!qh->ISqhullQh){
        qh_fprintf_stderr(10025, "Qhull error: qh_fprintf called from a Qhull instance without QhullQh defined\n");
        qh_exit(10025);
    }
    QhullQh *qhullQh= static_cast<QhullQh *>(qh);
    va_start(args, fmt);
    if(msgcode<MSG_OUTPUT || fp == qh_FILEstderr){
        if(msgcode>=MSG_ERROR && msgcode<MSG_WARNING){
            if(qhullQh->qhull_status<MSG_ERROR || qhullQh->qhull_status>=MSG_WARNING){
                qhullQh->qhull_status= msgcode;
            }
        }
        char newMessage[MSG_MAXLEN];
        // RoadError will add the message tag
        vsnprintf(newMessage, sizeof(newMessage), fmt, args);
        qhullQh->appendQhullMessage(newMessage);
        va_end(args);
        return;
    }
    if(qhullQh->output_stream && qhullQh->use_output_stream){
        char newMessage[MSG_MAXLEN];
        vsnprintf(newMessage, sizeof(newMessage), fmt, args);
        *qhullQh->output_stream << newMessage;
        va_end(args);
        return;
    }
    // FIXUP QH11008: how do users trap messages and handle input?  A callback?
    char newMessage[MSG_MAXLEN];
    vsnprintf(newMessage, sizeof(newMessage), fmt, args);
    qhullQh->appendQhullMessage(newMessage);
    va_end(args);
} /* qh_fprintf */
