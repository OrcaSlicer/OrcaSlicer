/****************************************************************************
**
** Copyright (c) 2008-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/qhulltest/QhullFacetSet_test.cpp#3 $$Change: 2062 $
** $DateTime: 2016/01/17 13:13:18 $$Author: bbarber $
**
****************************************************************************/

//pre-compiled headers
#include <iostream>
#include "qhulltest/RoadTest.h" // QT_VERSION

#include "libqhullcpp/QhullFacetSet.h"
#include "libqhullcpp/QhullError.h"
#include "libqhullcpp/QhullFacet.h"
#include "libqhullcpp/Qhull.h"
#include "libqhullcpp/RboxPoints.h"

using std::cout;
using std::endl;
using std::ostringstream;
using std::ostream;
using std::string;

namespace orgQhull {

class QhullFacetSet_test : public RoadTest
{
    Q_OBJECT

#//!\name Test slots
private slots:
    void cleanup();
    void t_construct();
    void t_convert();
    void t_readonly();
    void t_foreach();
    void t_io();
};//QhullFacetSet_test

void
add_QhullFacetSet_test()
{
    new QhullFacetSet_test();  // RoadTest::s_testcases
}

//Executed after each testcase
void QhullFacetSet_test::
cleanup()
{
    RoadTest::cleanup();
}

void QhullFacetSet_test::
t_construct()
{
    RboxPoints rcube("c");
    Qhull q(rcube,"QR0");  // rotated unit cube
    QhullFacet f= q.firstFacet();
    QhullFacetSet fs2= f.neighborFacets();
    QVERIFY(!fs2.isEmpty());
    QCOMPARE(fs2.count(),4);
    QhullFacetSet fs4= fs2; // copy constructor
    QVERIFY(fs4==fs2);
    QhullFacetSet fs3(q, q.qh()->facet_mergeset);
    QVERIFY(fs3.isEmpty());
}//t_construct

void QhullFacetSet_test::
t_convert()
{
    RboxPoints rcube("c");
    Qhull q2(rcube,"QR0 QV2");  // rotated unit cube
    QhullFacet f2= q2.firstFacet();
    QhullFacetSet fs2= f2.neighborFacets();
    QVERIFY(!fs2.isSelectAll());
    QCOMPARE(fs2.count(),2);
    std::vector<QhullFacet> fv= fs2.toStdVector();
    QCOMPARE(fv.size(), 2u);
    QList<QhullFacet> fv2= fs2.toQList();
    QCOMPARE(fv2.size(), 2);
    fs2.selectAll();
    QVERIFY(fs2.isSelectAll());
    std::vector<QhullFacet> fv3= fs2.toStdVector();
    QCOMPARE(fv3.size(), 4u);
    QList<QhullFacet> fv4= fs2.toQList();
    QCOMPARE(fv4.size(), 4);
}//t_convert

//! Spot check properties and read-only.  See QhullSet_test
void QhullFacetSet_test::
t_readonly()
{
    RboxPoints rcube("c");
    Qhull q(rcube,"QV0");  // good facets are adjacent to point 0
    QhullFacetSet fs= q.firstFacet().neighborFacets();
    QVERIFY(!fs.isSelectAll());
    QCOMPARE(fs.count(), 2);
    fs.selectAll();
    QVERIFY(fs.isSelectAll());
    QCOMPARE(fs.count(), 4);
    fs.selectGood();
    QVERIFY(!fs.isSelectAll());
    QCOMPARE(fs.count(), 2);
    QhullFacet f= fs.first();
    QhullFacet f2= fs.last();
    fs.selectAll();
    QVERIFY(fs.contains(f));
    QVERIFY(fs.contains(f2));
    QVERIFY(f.isGood());
    QVERIFY(!f2.isGood());
    fs.selectGood();
    QVERIFY(fs.contains(f));
    QVERIFY(!fs.contains(f2));
}//t_readonly

void QhullFacetSet_test::
t_foreach()
{
    RboxPoints rcube("c");
    // Spot check predicates and accessors.  See QhullLinkedList_test
    Qhull q(rcube,"QR0");  // rotated unit cube
    QhullFacetSet fs= q.firstFacet().neighborFacets();
    QVERIFY(!fs.contains(q.firstFacet()));
    QVERIFY(fs.contains(fs.first()));
    QhullFacet f= q.firstFacet().next();
    if(!fs.contains(f)){  // check if 'f' is the facet opposite firstFacet()
        f= f.next();
    }
    QVERIFY(fs.contains(f));
    QCOMPARE(fs.first(), *fs.begin());
    QCOMPARE(*(fs.end()-1), fs.last());
}//t_foreach

void QhullFacetSet_test::
t_io()
{
    RboxPoints rcube("c");
    {
        Qhull q(rcube,"QR0 QV0");   // good facets are adjacent to point 0
        QhullFacetSet fs= q.firstFacet().neighborFacets();
        ostringstream os;
        os << fs.print("Neighbors of first facet with point 0");
        os << fs.printIdentifiers("\nFacet identifiers: ");
        cout << os.str();
        QString facets= QString::fromStdString(os.str());
        QCOMPARE(facets.count(QRegExp(" f[0-9]")), 2+13*2);
    }
}//t_io

}//orgQhull

#include "moc/QhullFacetSet_test.moc"
