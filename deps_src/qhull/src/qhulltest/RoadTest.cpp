/****************************************************************************
**
** Copyright (c) 2008-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/qhulltest/RoadTest.cpp#2 $$Change: 2062 $
** $Date: 2016/01/17 $$Author: bbarber $
**
****************************************************************************/

//pre-compiled headers
#include <iostream>
#include "RoadTest.h" // QT_VERSION

#include <stdexcept>

using std::cout;
using std::endl;

namespace orgQhull {

#//!\name class variable

QList<RoadTest*> RoadTest::
s_testcases;

int RoadTest::
s_test_count= 0;

int RoadTest::
s_test_fail= 0;

QStringList RoadTest::
s_failed_tests;

#//!\name Slot

//! Executed after each test
void RoadTest::
cleanup()
{
    s_test_count++;
    if(QTest::currentTestFailed()){
        recordFailedTest();
    }
}//cleanup

#//!\name Helper

void RoadTest::
recordFailedTest()
{
    s_test_fail++;
    QString className= metaObject()->className();
    s_failed_tests << className + "::" + QTest::currentTestFunction();
}

#//!\name class function

void RoadTest::
deleteTests()
{
    foreach(RoadTest *testcase, s_testcases){
        delete testcase;
    }
    s_failed_tests.clear();
}

int RoadTest::
runTests(QStringList arguments)
{
    int result= 0; // assume success

    foreach(RoadTest *testcase, s_testcases){
        try{
            result += QTest::qExec(testcase, arguments);
        }catch(const std::exception &e){
            cout << "FAIL!  : Threw error ";
            cout << e.what() << endl;
    s_test_count++;
            testcase->recordFailedTest();
            // Qt 4.5.2 OK.  In Qt 4.3.3, qtestcase did not clear currentTestObject
        }
    }
    if(s_test_fail){
        cout << "Failed " << s_test_fail << " of " << s_test_count << " tests.\n";
        cout << s_failed_tests.join("\n").toLocal8Bit().constData() << std::endl;
    }else{
        cout << "Passed " << s_test_count << " tests.\n";
    }
    return result;
}//runTests

}//orgQhull

#include "moc/moc_RoadTest.cpp"
