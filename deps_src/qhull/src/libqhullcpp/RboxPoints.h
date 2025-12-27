/****************************************************************************
**
** Copyright (c) 2008-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/libqhullcpp/RboxPoints.h#4 $$Change: 2079 $
** $DateTime: 2016/02/07 17:43:34 $$Author: bbarber $
**
****************************************************************************/

#ifndef RBOXPOINTS_H
#define RBOXPOINTS_H

#include "libqhull_r/qhull_ra.h"
#include "libqhullcpp/QhullPoint.h"
#include "libqhullcpp/PointCoordinates.h"

#include <stdarg.h>
#include <string>
#include <vector>
#include <istream>
#include <ostream>
#include <sstream>

namespace orgQhull {

#//!\name Defined here
    //! RboxPoints -- generate random PointCoordinates for Qhull
    class RboxPoints;

class RboxPoints : public PointCoordinates {

private:
#//!\name Fields and friends
                        //! PointCoordinates.qh() is owned by RboxPoints
    countT              rbox_new_count;     //! Number of points for PointCoordinates
    int                 rbox_status;    //! error status from rboxpoints.  qh_ERRnone if none.
    std::string         rbox_message;   //! stderr from rboxpoints

    // '::' is required for friend references
    friend void ::qh_fprintf_rbox(qhT *qh, FILE *fp, int msgcode, const char *fmt, ... );

public:
#//!\name Construct
                        RboxPoints();
    explicit            RboxPoints(const char *rboxCommand);
                        ~RboxPoints();
private:                // Disable copy constructor and assignment.  RboxPoints owns QhullQh.
                        RboxPoints(const RboxPoints &);
                        RboxPoints &operator=(const RboxPoints &);
private:
    void                allocateQhullQh();

public:
#//!\name GetSet
    void                clearRboxMessage();
    countT              newCount() const { return rbox_new_count; }
    std::string         rboxMessage() const;
    int                 rboxStatus() const;
    bool                hasRboxMessage() const;
    void                setNewCount(countT pointCount) { QHULL_ASSERT(pointCount>=0); rbox_new_count= pointCount; }

#//!\name Modify
    void                appendPoints(const char* rboxCommand);
    using               PointCoordinates::appendPoints;
    void                reservePoints() { reserveCoordinates((count()+newCount())*dimension()); }
};//class RboxPoints

}//namespace orgQhull

#endif // RBOXPOINTS_H
