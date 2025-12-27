/****************************************************************************
**
** Copyright (c) 2006-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/qhulltest/RboxPoints_test.cpp#2 $$Change: 2062 $
** $DateTime: 2016/01/17 13:13:18 $$Author: bbarber $
**
****************************************************************************/

//pre-compiled headers
#include <iostream>
#include "RoadTest.h" // QT_VERSION

#include "libqhullcpp/RboxPoints.h"
#include "libqhullcpp/QhullError.h"

using std::cout;
using std::endl;
using std::ostringstream;
using std::string;
using std::stringstream;

namespace orgQhull {

//! Test C++ interface to Rbox
//! See eg/q_test for tests of rbox commands
class RboxPoints_test : public RoadTest
{
    Q_OBJECT

#//!\name Test slots
private slots:
    void t_construct();
    void t_error();
    void t_test();
    void t_getSet();
    void t_foreach();
    void t_change();
    void t_ostream();
};

void
add_RboxPoints_test()
{
    new RboxPoints_test();  // RoadTest::s_testcases
}

void RboxPoints_test::
t_construct()
{
    RboxPoints rp;
    QCOMPARE(rp.dimension(), 0);
    QCOMPARE(rp.count(), 0);
    QVERIFY(QString::fromStdString(rp.comment()) != QString(""));
    QVERIFY(rp.isEmpty());
    QVERIFY(!rp.hasRboxMessage());
    QCOMPARE(rp.rboxStatus(), qh_ERRnone);
    QCOMPARE(QString::fromStdString(rp.rboxMessage()), QString("rbox warning: no points generated\n"));

    RboxPoints rp2("c"); // 3-d cube
    QCOMPARE(rp2.dimension(), 3);
    QCOMPARE(rp2.count(), 8);
    QCOMPARE(QString::fromStdString(rp2.comment()), QString("rbox \"c\""));
    QVERIFY(!rp2.isEmpty());
    QVERIFY(!rp2.hasRboxMessage());
    QCOMPARE(rp2.rboxStatus(), qh_ERRnone);
    QCOMPARE(QString::fromStdString(rp2.rboxMessage()), QString("rbox: OK\n"));
}//t_construct

void RboxPoints_test::
t_error()
{
    RboxPoints rp;
    try{
        rp.appendPoints("D0 c");
        QFAIL("'D0 c' did not fail.");
    }catch (const std::exception &e) {
        const char *s= e.what();
        cout << "INFO   : Caught " << s;
        QCOMPARE(QString(s).left(6), QString("QH6189"));
        QVERIFY(rp.hasRboxMessage());
        QCOMPARE(QString::fromStdString(rp.rboxMessage()).left(8), QString("rbox err"));
        QCOMPARE(rp.rboxStatus(), 6189);
        rp.clearRboxMessage();
        QVERIFY(!rp.hasRboxMessage());
    }
    try{
        RboxPoints rp2;
        rp2.setDimension(-1);
        QFAIL("setDimension(-1) did not fail.");
    }catch (const RoadError &e) {
        const char *s= e.what();
        cout << "INFO   : Caught " << s;
        QCOMPARE(QString(s).left(7), QString("QH10062"));
        QCOMPARE(e.errorCode(), 10062);
        QCOMPARE(QString::fromStdString(e.what()), QString(s));
        RoadLogEvent logEvent= e.roadLogEvent();
        QCOMPARE(logEvent.int1(), -1);
    }
}//t_error

void RboxPoints_test::
t_test()
{
    // isEmpty -- t_construct
}//t_test

void RboxPoints_test::
t_getSet()
{
    // comment -- t_construct
    // count -- t_construct
    // dimension -- t_construct

    RboxPoints rp;
    QCOMPARE(rp.dimension(), 0);
    rp.setDimension(2);
    QCOMPARE(rp.dimension(), 2);
    try{
        rp.setDimension(102);
        QFAIL("setDimension(102) did not fail.");
    }catch (const std::exception &e) {
        cout << "INFO   : Caught " << e.what();
    }
    QCOMPARE(rp.newCount(), 0);
    rp.appendPoints("D2 P1 P2");
    QCOMPARE(rp.count(), 2);
    QCOMPARE(rp.newCount(), 2); // From previous appendPoints();
    PointCoordinates pc(rp.qh(), 2, "Test qh() and <<");
    pc << 1.0 << 0.0 << 2.0 << 0.0;
    QCOMPARE(pc.dimension(), 2);
    QCOMPARE(pc.count(), 2);
    QVERIFY(rp==pc);
    rp.setNewCount(10);  // Normally only used by appendPoints for rbox processing
    QCOMPARE(rp.newCount(), 10);
    rp.reservePoints();
    QVERIFY(rp==pc);
}//t_getSet

void RboxPoints_test::
t_foreach()
{
    RboxPoints rp("c");
    Coordinates::ConstIterator cci= rp.beginCoordinates();
    orgQhull::Coordinates::Iterator ci= rp.beginCoordinates();
    QCOMPARE(*cci, -0.5);
    QCOMPARE(*ci, *cci);
    int i=1;
    while(++cci<rp.endCoordinates()){
        QVERIFY(++ci<rp.endCoordinates());
        QCOMPARE(*cci, *ci);
        i++;
    }
    QVERIFY(++ci==rp.endCoordinates());
    QCOMPARE(i, 8*3);
    orgQhull::Coordinates::Iterator ci4= rp.beginCoordinates(4);
    QCOMPARE(rp.endCoordinates()-ci4, 4*3);
    orgQhull::Coordinates::ConstIterator cci4= rp.beginCoordinates(4);
    orgQhull::Coordinates::ConstIterator cci5= rp.endCoordinates();
    QCOMPARE(cci5-cci4, 4*3);
}//t_foreach

void RboxPoints_test::
t_change()
{
    RboxPoints rp("c D2");
    stringstream s;
    s << "4 count" << endl;
    s << "2 dimension" << endl;
    s << "1 2 3 4 5 6 7 8" << endl;
    rp.appendPoints(s);
    QCOMPARE(rp.count(), 8);
    orgQhull::Coordinates::Iterator ci= rp.beginCoordinates(7);
    QCOMPARE(*ci, 7.0);
    try{
        stringstream s2;
        s2 << "4 count" << endl;
        s2 << "2 dimension" << endl;
        s2 << "1 2 3 4 5 6 7 " << endl;
        rp.appendPoints(s2);
        QFAIL("incomplete appendPoints() did not fail.");
    }catch (const std::exception &e) {
        cout << "INFO   : Caught " << e.what();
    }
    RboxPoints rp2;
    rp2.append(rp);
    QCOMPARE(rp2.count(), 8);
    orgQhull::Coordinates::ConstIterator cci2= rp2.beginCoordinates(6);
    QCOMPARE(*(cci2+1), 6.0);
    rp2.appendPoints("D2 10 P0");
    QCOMPARE(rp2.count(), 19);
    orgQhull::Coordinates::ConstIterator cie= rp2.beginCoordinates(8);
    QCOMPARE(*cie, 0.0);
    RboxPoints rp3;
    coordT points[] = { 0, 1,1,0,1,1,0,0};
    rp3.setDimension(2);
    rp3.append(8,points);
    QCOMPARE(rp3.count(), 4);
    orgQhull::Coordinates::Iterator ci3= rp3.beginCoordinates(3);
    QCOMPARE(*ci3, 0.0);
}//t_change

void RboxPoints_test::
t_ostream()
{
    RboxPoints rp("c D2");
    ostringstream oss;
    oss << rp;
    string s= oss.str();
    QString qs= QString::fromStdString(s);
    QCOMPARE(qs.count("-0.5"), 4);
}//t_ostream

}//orgQhull

#include "moc/RboxPoints_test.moc"
