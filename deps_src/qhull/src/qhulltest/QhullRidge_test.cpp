/****************************************************************************
**
** Copyright (c) 2008-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/qhulltest/QhullRidge_test.cpp#3 $$Change: 2062 $
** $DateTime: 2016/01/17 13:13:18 $$Author: bbarber $
**
****************************************************************************/

//pre-compiled headers
#include <iostream>
#include "RoadTest.h" // QT_VERSION

#include "libqhullcpp/QhullRidge.h"
#include "libqhullcpp/QhullError.h"
#include "libqhullcpp/RboxPoints.h"
#include "libqhullcpp/QhullFacet.h"
#include "libqhullcpp/Qhull.h"

using std::cout;
using std::endl;
using std::ostringstream;
using std::ostream;
using std::string;

namespace orgQhull {

class QhullRidge_test : public RoadTest
{
    Q_OBJECT

#//!\name Test slots
private slots:
    void cleanup();
    void t_construct();
    void t_getSet();
    void t_foreach();
    void t_io();
};//QhullRidge_test

void
add_QhullRidge_test()
{
    new QhullRidge_test();  // RoadTest::s_testcases
}

//Executed after each testcase
void QhullRidge_test::
cleanup()
{
    RoadTest::cleanup();
}

void QhullRidge_test::
t_construct()
{
    // Qhull.runQhull() constructs QhullFacets as facetT
    RboxPoints rcube("c");
    Qhull q(rcube,"QR0");  // triangulation of rotated unit cube
    QhullRidge r(q);
    QVERIFY(!r.isValid());
    QCOMPARE(r.dimension(),2);
    QhullFacet f(q.firstFacet());
    QhullRidgeSet rs(f.ridges());
    QVERIFY(!rs.isEmpty()); // Simplicial facets do not have ridges()
    QhullRidge r2(rs.first());
    QCOMPARE(r2.dimension(), 2); // One dimension lower than the facet
    r= r2;
    QVERIFY(r.isValid());
    QCOMPARE(r.dimension(), 2);
    QhullRidge r3(q, r2.getRidgeT());
    QCOMPARE(r,r3);
    QhullRidge r4(q, r2.getBaseT());
    QCOMPARE(r,r4);
    QhullRidge r5= r2; // copy constructor
    QVERIFY(r5==r2);
    QVERIFY(r5==r);
}//t_construct

void QhullRidge_test::
t_getSet()
{
    RboxPoints rcube("c");
    {
        Qhull q(rcube,"QR0");  // triangulation of rotated unit cube
        QCOMPARE(q.facetCount(), 6);
        QCOMPARE(q.vertexCount(), 8);
        QhullFacet f(q.firstFacet());
        QhullRidgeSet rs= f.ridges();
        QhullRidgeSetIterator i(rs);
        while(i.hasNext()){
            const QhullRidge r= i.next();
            cout << r.id() << endl;
            QVERIFY(r.bottomFacet()!=r.topFacet());
            QCOMPARE(r.dimension(), 2); // Ridge one-dimension less than facet
            QVERIFY(r.id()>=0 && r.id()<9*27);
            QVERIFY(r.isValid());
            QVERIFY(r==r);
            QVERIFY(r==i.peekPrevious());
            QCOMPARE(r.otherFacet(r.bottomFacet()),r.topFacet());
            QCOMPARE(r.otherFacet(r.topFacet()),r.bottomFacet());
        }
    }
}//t_getSet

void QhullRidge_test::
t_foreach()
{
    RboxPoints rcube("c");  // cube
    {
        Qhull q(rcube, "QR0"); // rotated cube
        QhullFacet f(q.firstFacet());
        foreach (const QhullRidge &r, f.ridges()){  // Qt only
            QhullVertexSet vs= r.vertices();
            QCOMPARE(vs.count(), 2);
            foreach (const QhullVertex &v, vs){  // Qt only
                QVERIFY(f.vertices().contains(v));
            }
        }
        QhullRidgeSet rs= f.ridges();
        QhullRidge r= rs.first();
        QhullRidge r2= r;
        QList<QhullVertex> vs;
        int count= 0;
        while(!count || r2!=r){
            ++count;
            QhullVertex v(q);
            QVERIFY2(r2.hasNextRidge3d(f),"A cube should only have non-simplicial facets.");
            QhullRidge r3= r2.nextRidge3d(f, &v);
            QVERIFY(!vs.contains(v));
            vs << v;
            r2= r2.nextRidge3d(f);
            QCOMPARE(r3, r2);
        }
        QCOMPARE(vs.count(), rs.count());
        QCOMPARE(count, rs.count());
    }
}//t_foreach

void QhullRidge_test::
t_io()
{
    RboxPoints rcube("c");
    {
        Qhull q(rcube, "");
        QhullFacet f(q.firstFacet());
        QhullRidgeSet rs= f.ridges();
        QhullRidge r= rs.first();
        ostringstream os;
        os << "Ridges\n" << rs << "Ridge\n" << r;
        os << r.print("\nRidge with message");
        cout << os.str();
        QString s= QString::fromStdString(os.str());
        QCOMPARE(s.count(" r"), 6);
    }
}//t_io

}//orgQhull

#include "moc/QhullRidge_test.moc"
