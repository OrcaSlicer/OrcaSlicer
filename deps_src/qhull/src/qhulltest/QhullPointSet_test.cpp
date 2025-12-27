/****************************************************************************
**
** Copyright (p) 2009-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/qhulltest/QhullPointSet_test.cpp#3 $$Change: 2062 $
** $DateTime: 2016/01/17 13:13:18 $$Author: bbarber $
**
****************************************************************************/

//pre-compiled header
#include <iostream>
#include "RoadTest.h" // QT_VERSION

#include "libqhullcpp/QhullPointSet.h"
#include "libqhullcpp/RboxPoints.h"
#include "libqhullcpp/QhullPoint.h"
#include "libqhullcpp/QhullFacet.h"
#include "libqhullcpp/QhullFacetList.h"
#include "libqhullcpp/Qhull.h"

using std::cout;
using std::endl;
using std::ostringstream;

namespace orgQhull {

class QhullPointSet_test : public RoadTest
{
    Q_OBJECT

#//!\name Test slots
private slots:
    void cleanup();
    void t_construct();
    void t_convert();
    void t_element();
    void t_iterator();
    void t_const_iterator();
    void t_search();
    void t_pointset_iterator();
    void t_io();
};//QhullPointSet_test

void
add_QhullPointSet_test()
{
    new QhullPointSet_test();  // RoadTest::s_testcases
}

//Executed after each testcase
void QhullPointSet_test::
cleanup()
{
    RoadTest::cleanup();
}

void QhullPointSet_test::
t_construct()
{
    // Default constructor is disallowed (i.e., private)
    RboxPoints rcube("c W0 1000");
    Qhull q(rcube,"Qc");  // cube with 1000 coplanar points
    int coplanarCount= 0;
    foreach(QhullFacet f, q.facetList()){
        QhullPointSet ps(q, f.getFacetT()->outsideset);
        QVERIFY(ps.isEmpty());
        QCOMPARE(ps.count(), 0);
        QCOMPARE(ps.size(), 0u);
        QhullPointSet ps2(q.qh(), f.getFacetT()->coplanarset);
        QVERIFY(!ps2.isEmpty());
        coplanarCount += ps2.count();
        QCOMPARE(ps2.count(), (int)ps2.size());
        QhullPointSet ps3(ps2);
        QVERIFY(!ps3.isEmpty());
        QCOMPARE(ps3.count(), ps2.count());
        QVERIFY(ps3==ps2);
        QVERIFY(ps3!=ps);
        QhullPointSet ps4= ps3;
        QVERIFY(ps4==ps2);
    }
    QCOMPARE(coplanarCount, 1000);
}//t_construct

void QhullPointSet_test::
t_convert()
{
    RboxPoints rcube("c W0 1000");
    Qhull q(rcube,"Qc");  // cube with 1000 coplanar points
    QhullFacet f= q.firstFacet();
    QhullPointSet ps= f.coplanarPoints();
    QVERIFY(ps.count()>=1);   // Sometimes no coplanar points
    std::vector<QhullPoint> vs= ps.toStdVector();
    QCOMPARE(vs.size(), ps.size());
    QhullPoint p= ps[0];
    QhullPoint p2= vs[0];
    QCOMPARE(p, p2);
    QList<QhullPoint> qs= ps.toQList();
    QCOMPARE(qs.size(), static_cast<int>(ps.size()));
    QhullPoint p3= qs[0];
    QCOMPARE(p3, p);
}//t_convert

// readonly tested in t_construct
//   empty, isEmpty, ==, !=, size

void QhullPointSet_test::
t_element()
{
    RboxPoints rcube("c W0 1000");
    Qhull q(rcube,"Qc");  // cube with 1000 coplanar points
    QhullFacet f= q.firstFacet();
    QhullPointSet ps= f.coplanarPoints();
    QVERIFY(ps.count()>=3);  // Sometimes no coplanar points
    QhullPoint p= ps[0];
    QCOMPARE(p, ps[0]);
    QhullPoint p2= ps[ps.count()-1];
    QCOMPARE(ps.at(1), ps[1]);
    QCOMPARE(ps.second(), ps[1]);
    QCOMPARE(ps.first(), p);
    QCOMPARE(ps.front(), ps.first());
    QCOMPARE(ps.last(), p2);
    QCOMPARE(ps.back(), ps.last());
    QhullPoint p8(q);
    QCOMPARE(ps.value(2), ps[2]);
    QCOMPARE(ps.value(-1), p8);
    QCOMPARE(ps.value(ps.count()), p8);
    QCOMPARE(ps.value(ps.count(), p), p);
    QVERIFY(ps.value(1, p)!=p);
    QhullPointSet ps8= f.coplanarPoints();
    QhullPointSet::Iterator i= ps8.begin();
    foreach(QhullPoint p9, ps){  // Qt only
        QCOMPARE(p9.dimension(), 3);
        QCOMPARE(p9, *i++);
    }
}//t_element

void QhullPointSet_test::
t_iterator()
{
    RboxPoints rcube("c W0 1000");
    Qhull q(rcube,"Qc");  // cube with 1000 coplanar points
    QhullFacet f= q.firstFacet();
    QhullPointSet ps= f.coplanarPoints();
    QVERIFY(ps.count()>=3);  // Sometimes no coplanar points
    QhullPointSet::Iterator i= ps.begin();
    QhullPointSet::iterator i2= ps.begin();
    QVERIFY(i==i2);
    QVERIFY(i>=i2);
    QVERIFY(i<=i2);
    i= ps.begin();
    QVERIFY(i==i2);
    i2= ps.end();
    QVERIFY(i!=i2);
    QhullPoint p= *i;
    QCOMPARE(p.dimension(), q.dimension());
    QCOMPARE(p, ps[0]);
    i2--;
    QhullPoint p2= *i2;
    QCOMPARE(p2.dimension(), q.dimension());
    QCOMPARE(p2, ps.last());
    QhullPointSet::Iterator i5(i2);
    QCOMPARE(*i2, *i5);
    QhullPointSet::Iterator i3= i+1;
    QVERIFY(i!=i3);
    QCOMPARE(i[1], *i3);
    (i3= i)++;
    QCOMPARE((*i3)[0], ps[1][0]);
    QCOMPARE((*i3).dimension(), 3);

    QVERIFY(i==i);
    QVERIFY(i!=i3);
    QVERIFY(i<i3);
    QVERIFY(i<=i3);
    QVERIFY(i3>i);
    QVERIFY(i3>=i);

    QhullPointSet::ConstIterator i4= ps.begin();
    QVERIFY(i==i4); // iterator COMP const_iterator
    QVERIFY(i<=i4);
    QVERIFY(i>=i4);
    QVERIFY(i4==i); // const_iterator COMP iterator
    QVERIFY(i4<=i);
    QVERIFY(i4>=i);
    QVERIFY(i>=i4);
    QVERIFY(i4<=i);
    QVERIFY(i2!=i4);
    QVERIFY(i2>i4);
    QVERIFY(i2>=i4);
    QVERIFY(i4!=i2);
    QVERIFY(i4<i2);
    QVERIFY(i4<=i2);
    ++i4;
    QVERIFY(i!=i4); // iterator COMP const_iterator
    QVERIFY(i<i4);
    QVERIFY(i<=i4);
    QVERIFY(i4>i);
    QVERIFY(i4>=i);
    i4= ps.constBegin();
    QVERIFY(i==i4); // iterator COMP const_iterator
    QCOMPARE(i4+ps.count(), ps.constEnd());

    i= ps.begin();
    i2= ps.begin();
    QCOMPARE(i, i2++);
    QCOMPARE(*i2, ps[1]);
    QCOMPARE(++i, i2);
    QCOMPARE(i, i2--);
    QCOMPARE(i2, ps.begin());
    QCOMPARE(--i, i2);
    QCOMPARE(i2+=ps.count(), ps.end());
    QCOMPARE(i2-=ps.count(), ps.begin());
    QCOMPARE(i2+0, ps.begin());
    QCOMPARE(i2+ps.count(), ps.end());
    i2 += ps.count();
    i= i2-0;
    QCOMPARE(i, i2);
    i= i2-ps.count();
    QCOMPARE(i, ps.begin());
    QCOMPARE(i2-i, ps.count());

    //ps.begin end tested above

    // QhullPointSet is const-only
}//t_iterator

void QhullPointSet_test::
t_const_iterator()
{
    RboxPoints rcube("c W0 1000");
    Qhull q(rcube,"Qc");  // cube with 1000 coplanar points
    QhullFacet f= q.firstFacet();
    QhullPointSet ps= f.coplanarPoints();
    QVERIFY(ps.count()>=3);  // Sometimes no coplanar points
    QhullPointSet::ConstIterator i= ps.begin();
    QhullPointSet::const_iterator i2= ps.begin();
    QVERIFY(i==i2);
    QVERIFY(i>=i2);
    QVERIFY(i<=i2);

    // See t_iterator for const_iterator COMP iterator

    i= ps.begin();
    QVERIFY(i==i2);
    i2= ps.end();
    QVERIFY(i!=i2);
    QhullPoint p= *i; // QhullPoint is the base class for QhullPointSet::iterator
    QCOMPARE(p.dimension(), q.dimension());
    QCOMPARE(p, ps[0]);
    i2--;
    QhullPoint p2= *i2;
    QCOMPARE(p2.dimension(), q.dimension());
    QCOMPARE(p2, ps.last());
    QhullPointSet::ConstIterator i5(i2);
    QCOMPARE(*i2, *i5);


    QhullPointSet::ConstIterator i3= i+1;
    QVERIFY(i!=i3);
    QCOMPARE(i[1], *i3);

    QVERIFY(i==i);
    QVERIFY(i!=i3);
    QVERIFY(i<i3);
    QVERIFY(i<=i3);
    QVERIFY(i3>i);
    QVERIFY(i3>=i);

    // QhullPointSet is const-only
}//t_const_iterator


void QhullPointSet_test::
t_search()
{
    RboxPoints rcube("c W0 1000");
    Qhull q(rcube,"Qc");  // cube with 1000 coplanar points
    QhullFacet f= q.firstFacet();
    QhullPointSet ps= f.coplanarPoints();
    QVERIFY(ps.count()>=3);  // Sometimes no coplanar points
    QhullPoint p= ps.first();
    QhullPoint p2= ps.last();
    QVERIFY(ps.contains(p));
    QVERIFY(ps.contains(p2));
    QVERIFY(p!=p2);
    QhullPoint p3= ps[2];
    QVERIFY(ps.contains(p3));
    QVERIFY(p!=p3);
    QCOMPARE(ps.indexOf(p), 0);
    QCOMPARE(ps.indexOf(p2), ps.count()-1);
    QCOMPARE(ps.indexOf(p3), 2);
    QhullPoint p4(q);
    QCOMPARE(ps.indexOf(p4), -1);
    QCOMPARE(ps.lastIndexOf(p), 0);
    QCOMPARE(ps.lastIndexOf(p2), ps.count()-1);
    QCOMPARE(ps.lastIndexOf(p3), 2);
    QCOMPARE(ps.lastIndexOf(p4), -1);
}//t_search

void QhullPointSet_test::
t_pointset_iterator()
{
    RboxPoints rcube("c W0 1000");
    Qhull q(rcube,"Qc");  // cube with 1000 coplanar points
    QhullFacet f= q.firstFacet();
    QhullPointSet ps2= f.outsidePoints();
    QVERIFY(ps2.count()==0); // No outside points after constructing the convex hull
    QhullPointSetIterator i2= ps2;
    QCOMPARE(i2.countRemaining(), 0);
    QVERIFY(!i2.hasNext());
    QVERIFY(!i2.hasPrevious());
    i2.toBack();
    QVERIFY(!i2.hasNext());
    QVERIFY(!i2.hasPrevious());

    QhullPointSet ps= f.coplanarPoints();
    QVERIFY(ps.count()>=3);  // Sometimes no coplanar points
    QhullPointSetIterator i(ps);
    i2= ps;
    QCOMPARE(i2.countRemaining(), ps.count());
    QVERIFY(i2.hasNext());
    QVERIFY(!i2.hasPrevious());
    QVERIFY(i.hasNext());
    QVERIFY(!i.hasPrevious());
    i2.toBack();
    QCOMPARE(i2.countRemaining(), 0);
    i.toFront();
    QCOMPARE(i.countRemaining(), ps.count());
    QCOMPARE(i2.countRemaining(), 0);
    QVERIFY(!i2.hasNext());
    QVERIFY(i2.hasPrevious());
    QVERIFY(i.hasNext());
    QVERIFY(!i.hasPrevious());

    QhullPoint p= ps[0];
    QhullPoint p2(ps[0]);
    QCOMPARE(p, p2);
    QVERIFY(p==p2);
    QhullPoint p3(ps.last());
 // p2[0]= 0.0;
    QVERIFY(p==p2);
    QCOMPARE(i2.peekPrevious(), p3);
    QCOMPARE(i2.previous(), p3);
    QCOMPARE(i2.previous(), ps[ps.count()-2]);
    QVERIFY(i2.hasPrevious());
    QCOMPARE(i.peekNext(), p);
    // i.peekNext()= 1.0; // compiler error
    QCOMPARE(i.next(), p);
    QCOMPARE(i.countRemaining(), ps.count()-1);
    QhullPoint p4= i.peekNext();
    QVERIFY(p4!=p3);
    QCOMPARE(i.next(), p4);
    QVERIFY(i.hasNext());
    i.toFront();
    QCOMPARE(i.next(), p);
}//t_pointset_iterator

void QhullPointSet_test::
t_io()
{
    ostringstream os;
    RboxPoints rcube("c W0 120");
    Qhull q(rcube,"Qc");  // cube with 100 coplanar points
    QhullFacet f= q.firstFacet();
    QhullPointSet ps= f.coplanarPoints();
    QVERIFY(ps.count()>=3);  // Sometimes no coplanar points
    os << "QhullPointSet from coplanarPoints\n" << ps << endl;
    os << ps.print("\nWith message\n");
    os << ps.printIdentifiers("\nCoplanar points: ");
    os << "\nAs a point set:\n";
    os << ps;
    cout << os.str();
    QString s= QString::fromStdString(os.str());
    QCOMPARE(s.count(" 0.5\n"), 3*ps.count());
    QCOMPARE(s.count("p"), ps.count()+4);
}//t_io

}//orgQhull

#include "moc/QhullPointSet_test.moc"
