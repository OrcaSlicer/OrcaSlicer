/****************************************************************************
**
** Copyright (c) 2008-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/qhulltest/qhulltest.cpp#5 $$Change: 2079 $
** $DateTime: 2016/02/07 17:43:34 $$Author: bbarber $
**
****************************************************************************/

//pre-compiled headers
#include "libqhull_r/user_r.h"

#include <iostream>
#include "RoadTest.h" // QT_VERSION

#include "libqhullcpp/RoadError.h"
#include "libqhull_r/qhull_ra.h"

#include <sstream>
#include <stdexcept>
#include <string>

using std::cout;
using std::endl;

namespace orgQhull {

void addQhullTests(QStringList &args)
{
    TESTadd_(add_Qhull_test);

    if(args.contains("--all")){
        args.removeAll("--all");
        // up-to-date
        TESTadd_(add_Coordinates_test);
        TESTadd_(add_PointCoordinates_test);
        TESTadd_(add_QhullFacet_test);
        TESTadd_(add_QhullFacetList_test);
        TESTadd_(add_QhullFacetSet_test);
        TESTadd_(add_QhullHyperplane_test);
        TESTadd_(add_QhullLinkedList_test);
        TESTadd_(add_QhullPoint_test);
        TESTadd_(add_QhullPoints_test);
        TESTadd_(add_QhullPointSet_test);
        TESTadd_(add_QhullRidge_test);
        TESTadd_(add_QhullSet_test);
        TESTadd_(add_QhullVertex_test);
        TESTadd_(add_QhullVertexSet_test);
        TESTadd_(add_RboxPoints_test);
        // qhullStat
        TESTadd_(add_Qhull_test);
    }//--all
}//addQhullTests

int main(int argc, char *argv[])
{

    QCoreApplication app(argc, argv);
    QStringList args= app.arguments();
    bool isAll= args.contains("--all");

    QHULL_LIB_CHECK /* Check for compatible library */

    addQhullTests(args);
    int status=1010;
    try{
        status= RoadTest::runTests(args);
    }catch(const std::exception &e){
        cout << "FAIL!  : runTests() did not catch error\n";
        cout << e.what() << endl;
        if(!RoadError::emptyGlobalLog()){
            cout << RoadError::stringGlobalLog() << endl;
            RoadError::clearGlobalLog();
        }
    }
    if(!RoadError::emptyGlobalLog()){
        cout << RoadError::stringGlobalLog() << endl;
        RoadError::clearGlobalLog();
    }
    if(isAll){
        cout << "Finished test of libqhullcpp.  Test libqhull_r with eg/q_test after building libqhull_r/Makefile" << endl;
    }else{
        cout << "Finished test of one class.  Test all classes with 'qhulltest --all'" << endl;
    }
    RoadTest::deleteTests();
    return status;
}

}//orgQhull

int main(int argc, char *argv[])
{
    return orgQhull::main(argc, argv); // Needs RoadTest:: for TESTadd_() linkage
}

