/****************************************************************************
**
** Copyright (c) 2008-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/libqhullcpp/QhullQh.h#2 $$Change: 2079 $
** $DateTime: 2016/02/07 17:43:34 $$Author: bbarber $
**
****************************************************************************/

#ifndef QHULLQH_H
#define QHULLQH_H

#include "libqhull_r/qhull_ra.h"

#include <string>

#ifdef _MSC_VER  // Microsoft Visual C++ -- warning level 4
#pragma warning( disable : 4611)  /* interaction between '_setjmp' and C++ object destruction is non-portable */
/* setjmp should not be implemented with 'catch' */
#endif

//! Use QH_TRY_ or QH_TRY_NOTHROW_ to call a libqhull_r routine that may invoke qh_errexit()
//! QH_TRY_(qh){...} qh->NOerrexit=true;
//! No object creation -- longjmp() skips object destructors
//! To test for error when done -- qh->maybeThrowQhullMessage(QH_TRY_status);
//! Use the same compiler for QH_TRY_, libqhullcpp, and libqhull_r.  setjmp() is not portable between compilers.

#define QH_TRY_ERROR 10071

#define QH_TRY_(qh) \
    int QH_TRY_status; \
    if(qh->NOerrexit){ \
        qh->NOerrexit= False; \
        QH_TRY_status= setjmp(qh->errexit); \
    }else{ \
        throw QhullError(QH_TRY_ERROR, "Cannot invoke QH_TRY_() from inside a QH_TRY_.  Or missing 'qh->NOerrexit=true' after previously called QH_TRY_(qh){...}"); \
    } \
    if(!QH_TRY_status)

#define QH_TRY_NO_THROW_(qh) \
    int QH_TRY_status; \
    if(qh->NOerrexit){ \
        qh->NOerrexit= False; \
        QH_TRY_status= setjmp(qh->errexit); \
    }else{ \
        QH_TRY_status= QH_TRY_ERROR; \
    } \
    if(!QH_TRY_status)

namespace orgQhull {

#//!\name Defined here
    //! QhullQh -- Qhull's global data structure, qhT, as a C++ class
    class QhullQh;

//! POD type equivalent to qhT.  No virtual members
class QhullQh : public qhT {

#//!\name Constants

#//!\name Fields
private:
    int                 qhull_status;   //!< qh_ERRnone if valid
    std::string         qhull_message;  //!< Returned messages from libqhull_r
    std::ostream *      error_stream;   //!< overrides errorMessage, use appendQhullMessage()
    std::ostream *      output_stream;  //!< send output to stream
    double              factor_epsilon; //!< Factor to increase ANGLEround and DISTround for hyperplane equality
    bool                use_output_stream; //!< True if using output_stream

    friend void         ::qh_fprintf(qhT *qh, FILE *fp, int msgcode, const char *fmt, ... );

    static const double default_factor_epsilon;  //!< Default factor_epsilon is 1.0, never updated

#//!\name Constructors
public:
                        QhullQh();
                        ~QhullQh();
private:
                        //!disable copy constructor and assignment
                        QhullQh(const QhullQh &);
    QhullQh &           operator=(const QhullQh &);
public:

#//!\name GetSet
    double              factorEpsilon() const { return factor_epsilon; }
    void                setFactorEpsilon(double a) { factor_epsilon= a; }
    void                disableOutputStream() { use_output_stream= false; }
    void                enableOutputStream() { use_output_stream= true; }

#//!\name Messaging
    void                appendQhullMessage(const std::string &s);
    void                clearQhullMessage();
    std::string         qhullMessage() const;
    bool                hasOutputStream() const { return use_output_stream; }
    bool                hasQhullMessage() const;
    void                maybeThrowQhullMessage(int exitCode);
    void                maybeThrowQhullMessage(int exitCode, int noThrow) throw();
    int                 qhullStatus() const;
    void                setErrorStream(std::ostream *os);
    void                setOutputStream(std::ostream *os);

#//!\name Methods
    double              angleEpsilon() const { return this->ANGLEround*factor_epsilon; } //!< Epsilon for hyperplane angle equality
    void                checkAndFreeQhullMemory();
    double              distanceEpsilon() const { return this->DISTround*factor_epsilon; } //!< Epsilon for distance to hyperplane

};//class QhullQh

}//namespace orgQhull

#endif // QHULLQH_H
