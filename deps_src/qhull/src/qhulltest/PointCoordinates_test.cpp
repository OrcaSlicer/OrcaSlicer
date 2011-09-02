/****************************************************************************
**
** Copyright (c) 2009-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/qhulltest/PointCoordinates_test.cpp#2 $$Change: 2062 $
** $DateTime: 2016/01/17 13:13:18 $$Author: bbarber $
**
****************************************************************************/

//pre-compiled headers
#include <iostream>
#include "qhulltest/RoadTest.h" // QT_VERSION

#include "libqhullcpp/PointCoordinates.h"
#include "libqhullcpp/QhullError.h"
#include "libqhullcpp/RboxPoints.h"
#include "libqhullcpp/Qhull.h"

using std::cout;
using std::endl;
using std::ostringstream;
using std::ostream;
using std::string;
using std::stringstream;

namespace orgQhull {

class PointCoordinates_test : public RoadTest
{
    Q_OBJECT

#//!\name Test slots
private slots:
    void t_construct_q();
    void t_construct_qh();
    void t_convert();
    void t_getset();
    void t_element();
    void t_foreach();
    void t_search();
    void t_modify();
    void t_append_points();
    void t_coord_iterator();
    void t_io();
};//PointCoordinates_test

void
add_PointCoordinates_test()
{
    new PointCoordinates_test();  // RoadTest::s_testcases
}

void PointCoordinates_test::
t_construct_q()
{
    Qhull q;
    PointCoordinates pc(q);
    QCOMPARE(pc.size(), 0U);
    QCOMPARE(pc.coordinateCount(), 0);
    QCOMPARE(pc.dimension(), 0);
    QCOMPARE(pc.coordinates(), (coordT *)0);
    QVERIFY(pc.isEmpty());
    pc.checkValid();
    PointCoordinates pc7(q, 2, "test explicit dimension");
    QCOMPARE(pc7.dimension(), 2);
    QCOMPARE(pc7.count(), 0);
    QVERIFY(pc7.isEmpty());
    QCOMPARE(pc7.comment(), std::string("test explicit dimension"));
    pc7.checkValid();
    PointCoordinates pc2(q, "Test pc2");
    QCOMPARE(pc2.count(), 0);
    QVERIFY(pc2.isEmpty());
    QCOMPARE(pc2.comment(), std::string("Test pc2"));
    pc2.checkValid();
    PointCoordinates pc3(q, 3, "Test 3-d pc3");
    QCOMPARE(pc3.dimension(), 3);
    QVERIFY(pc3.isEmpty());
    pc3.checkValid();
    coordT c[]= { 0.0, 1.0, 2.0, 3.0, 4.0, 5.0 };
    PointCoordinates pc4(q, 2, "Test 2-d pc4", 6, c);
    QCOMPARE(pc4.dimension(), 2);
    QCOMPARE(pc4.count(), 3);
    QCOMPARE(pc4.size(), 3u);
    QVERIFY(!pc4.isEmpty());
    pc4.checkValid();
    QhullPoint p= pc4[2];
    QCOMPARE(p[1], 5.0);
    // QhullPoint refers to PointCoordinates
    p[1] += 1.0;
    QCOMPARE(pc4[2][1], 6.0);
    PointCoordinates pc5(q, 4, "Test 4-d pc5 with insufficient coordinates", 6, c);
    QCOMPARE(pc5.dimension(), 4);
    QCOMPARE(pc5.count(), 1);
    QCOMPARE(pc5.extraCoordinatesCount(), 2);
    QCOMPARE(pc5.extraCoordinates()[1], 5.0);
    QVERIFY(!pc5.isEmpty());;
    std::vector<coordT> vc;
    vc.push_back(3.0);
    vc.push_back(4.0);
    vc.push_back(5.0);
    vc.push_back(6.0);
    vc.push_back(7.0);
    vc.push_back(9.0);
    pc5.append(2, &vc[3]); // Copy of vc[]
    pc5.checkValid();
    QhullPoint p5(q, 4, &vc[1]);
    QCOMPARE(pc5[1], p5);
    PointCoordinates pc6(pc5); // Makes copy of point_coordinates
    QCOMPARE(pc6[1], p5);
    QVERIFY(pc6==pc5);
    QhullPoint p6= pc5[1];  // Refers to pc5.coordinates
    pc5[1][0] += 1.0;
    QCOMPARE(pc5[1], p6);
    QVERIFY(pc5[1]!=p5);
    QVERIFY(pc6!=pc5);
    pc6= pc5;
    QVERIFY(pc6==pc5);
    PointCoordinates pc8(q);
    pc6= pc8;
    QVERIFY(pc6!=pc5);
    QVERIFY(pc6.isEmpty());
}//t_construct_q

void PointCoordinates_test::
t_construct_qh()
{
    QhullQh qh;
    PointCoordinates pc(&qh);
    QCOMPARE(pc.size(), 0U);
    QCOMPARE(pc.coordinateCount(), 0);
    QCOMPARE(pc.dimension(), 0);
    QCOMPARE(pc.coordinates(), (coordT *)0);
    QVERIFY(pc.isEmpty());
    pc.checkValid();
    PointCoordinates pc7(&qh, 2, "test explicit dimension");
    QCOMPARE(pc7.dimension(), 2);
    QCOMPARE(pc7.count(), 0);
    QVERIFY(pc7.isEmpty());
    QCOMPARE(pc7.comment(), std::string("test explicit dimension"));
    pc7.checkValid();
    PointCoordinates pc2(&qh, "Test pc2");
    QCOMPARE(pc2.count(), 0);
    QVERIFY(pc2.isEmpty());
    QCOMPARE(pc2.comment(), std::string("Test pc2"));
    pc2.checkValid();
    PointCoordinates pc3(&qh, 3, "Test 3-d pc3");
    QCOMPARE(pc3.dimension(), 3);
    QVERIFY(pc3.isEmpty());
    pc3.checkValid();
    coordT c[]= { 0.0, 1.0, 2.0, 3.0, 4.0, 5.0 };
    PointCoordinates pc4(&qh, 2, "Test 2-d pc4", 6, c);
    QCOMPARE(pc4.dimension(), 2);
    QCOMPARE(pc4.count(), 3);
    QCOMPARE(pc4.size(), 3u);
    QVERIFY(!pc4.isEmpty());
    pc4.checkValid();
    QhullPoint p= pc4[2];
    QCOMPARE(p[1], 5.0);
    // QhullPoint refers to PointCoordinates
    p[1] += 1.0;
    QCOMPARE(pc4[2][1], 6.0);
    PointCoordinates pc5(&qh, 4, "Test 4-d pc5 with insufficient coordinates", 6, c);
    QCOMPARE(pc5.dimension(), 4);
    QCOMPARE(pc5.count(), 1);
    QCOMPARE(pc5.extraCoordinatesCount(), 2);
    QCOMPARE(pc5.extraCoordinates()[1], 5.0);
    QVERIFY(!pc5.isEmpty());;
    std::vector<coordT> vc;
    vc.push_back(3.0);
    vc.push_back(4.0);
    vc.push_back(5.0);
    vc.push_back(6.0);
    vc.push_back(7.0);
    vc.push_back(9.0);
    pc5.append(2, &vc[3]); // Copy of vc[]
    pc5.checkValid();
    QhullPoint p5(&qh, 4, &vc[1]);
    QCOMPARE(pc5[1], p5);
    PointCoordinates pc6(pc5); // Makes copy of point_coordinates
    QCOMPARE(pc6[1], p5);
    QVERIFY(pc6==pc5);
    QhullPoint p6= pc5[1];  // Refers to pc5.coordinates
    pc5[1][0] += 1.0;
    QCOMPARE(pc5[1], p6);
    QVERIFY(pc5[1]!=p5);
    QVERIFY(pc6!=pc5);
    pc6= pc5;
    QVERIFY(pc6==pc5);
    PointCoordinates pc8(&qh);
    pc6= pc8;
    QVERIFY(pc6!=pc5);
    QVERIFY(pc6.isEmpty());
}//t_construct_qh

void PointCoordinates_test::
t_convert()
{
    Qhull q;
    //defineAs tested above
    coordT c[]= {0.0, 1.0, 2.0, 3.0, 4.0, 5.0};
    PointCoordinates ps(q, 3, "two 3-d points", 6, c);
    QCOMPARE(ps.dimension(), 3);
    QCOMPARE(ps.size(), 2u);
    const coordT *c2= ps.constData();
    QVERIFY(c!=c2);
    QCOMPARE(c[0], c2[0]);
    const coordT *c3= ps.data();
    QCOMPARE(c3, c2);
    coordT *c4= ps.data();
    QCOMPARE(c4, c2);
    std::vector<coordT> vs= ps.toStdVector();
    QCOMPARE(vs.size(), 6u);
    QCOMPARE(vs[5], 5.0);
    QList<coordT> qs= ps.toQList();
    QCOMPARE(qs.size(), 6);
    QCOMPARE(qs[5], 5.0);
}//t_convert

void PointCoordinates_test::
t_getset()
{
    // See t_construct() for test of coordinates, coordinateCount, dimension, empty, isEmpty, ==, !=
    // See t_construct() for test of checkValid, comment, setDimension
    Qhull q;
    PointCoordinates pc(q, "Coordinates c");
    pc.setComment("New comment");
    QCOMPARE(pc.comment(), std::string("New comment"));
    pc.checkValid();
    pc.makeValid();  // A no-op
    pc.checkValid();
    Coordinates cs= pc.getCoordinates();
    QVERIFY(cs.isEmpty());
    PointCoordinates pc2(pc);
    pc.setDimension(3);
    QVERIFY(pc2!=pc);
    coordT c[]= {0.0, 1.0, 2.0, 3.0, 4.0, 5.0};
    pc.append(6, c);
    pc.checkValid();
    pc.makeValid();  // A no-op
    QhullPoint p= pc[0];
    QCOMPARE(p[2], 2.0);
    try{
        pc.setDimension(2);
        QFAIL("setDimension(2) did not fail for 3-d.");
    }catch (const std::exception &e) {
        const char *s= e.what();
        cout << "INFO   : Caught " << s;
    }
}//t_getset

void PointCoordinates_test::
t_element()
{
    Qhull q;
    coordT c[]= {0.0, 1.0, 2.0, 3.0, 4.0, 5.0};
    PointCoordinates pc(q, 2, "2-d points", 6, c);
    QhullPoint p= pc.at(0);
    QCOMPARE(p, pc[0]);
    QCOMPARE(p, pc.first());
    QCOMPARE(p, pc.value(0));
    p= pc.back();
    QCOMPARE(p, pc[2]);
    QCOMPARE(p, pc.last());
    QCOMPARE(p, pc.value(2));
    QhullPoints ps= pc.mid(1, 2);
    QCOMPARE(ps[1], p);
}//t_element

void PointCoordinates_test::
t_foreach()
{
    Qhull q;
    coordT c[]= {0.0, 1.0, 2.0, 3.0, 4.0, 5.0};
    PointCoordinates pc(q, 2, "2-d points", 6, c);
    QhullPoints::Iterator i= pc.begin();
    QhullPoint p= pc[0];
    QCOMPARE(*i, p);
    QCOMPARE((*i)[0], 0.0);
    QhullPoint p3= pc[2];
    i= pc.end();
    QCOMPARE(i[-1], p3);
    const PointCoordinates pc2(q, 2, "2-d points", 6, c);
    QhullPoints::ConstIterator i2= pc.begin();
    const QhullPoint p0= pc2[0];
    QCOMPARE(*i2, p0);
    QCOMPARE((*i2)[0], 0.0);
    QhullPoints::ConstIterator i3= i2;
    QCOMPARE(i3, i2);
    QCOMPARE((*i3)[0], 0.0);
    i3= pc.constEnd();
    --i3;
    QhullPoint p2= pc2[2];
    QCOMPARE(*i3, p2);
    i= pc.end();
    QVERIFY(i-1==i3);
    i2= pc2.end();
    QVERIFY(i2-1!=i3);
    QCOMPARE(*(i2-1), *i3);
    foreach(QhullPoint p3, pc){ //Qt only
        QVERIFY(p3[0]>=0.0);
        QVERIFY(p3[0]<=5.0);
    }
    Coordinates::ConstIterator i4= pc.beginCoordinates();
    QCOMPARE(*i4, 0.0);
    Coordinates::Iterator i5= pc.beginCoordinates();
    QCOMPARE(*i5, 0.0);
    i4= pc.beginCoordinates(1);
    QCOMPARE(*i4, 2.0);
    i5= pc.beginCoordinates(1);
    QCOMPARE(*i5, 2.0);
    i4= pc.endCoordinates();
    QCOMPARE(*--i4, 5.0);
    i5= pc.endCoordinates();
    QCOMPARE(*--i5, 5.0);
}//t_foreach

void PointCoordinates_test::
t_search()
{
    Qhull q;
    coordT c[]= {0.0, 1.0, 2.0, 3.0, 4.0, 5.0};
    PointCoordinates pc(q, 2, "2-d points", 6, c);
    QhullPoint p0= pc[0];
    QhullPoint p2= pc[2];
    QVERIFY(pc.contains(p0));
    QVERIFY(pc.contains(p2));
    QCOMPARE(pc.count(p0), 1);
    QCOMPARE(pc.indexOf(p2), 2);
    QCOMPARE(pc.lastIndexOf(p0), 0);
}//t_search

void PointCoordinates_test::
t_modify()
{
    Qhull q;
    coordT c[]= {0.0, 1.0, 2.0, 3.0, 4.0, 5.0};
    PointCoordinates pc(q, 2, "2-d points", 6, c);
    coordT c3[]= {0.0, 1.0, 2.0, 3.0, 4.0, 5.0};
    PointCoordinates pc5(q, 2, "test explicit dimension");
    pc5.append(6, c3); // 0-5
    QVERIFY(pc5==pc);
    PointCoordinates pc2(q, 2, "2-d");
    coordT c2[]= {6.0, 7.0, 8.0, 9.0, 10.0, 11.0};
    pc2.append(6, c2);
    QCOMPARE(pc2.count(), 3);
    pc2.append(14.0);
    QCOMPARE(pc2.count(), 3);
    QCOMPARE(pc2.extraCoordinatesCount(), 1);
    pc2.append(15.0); // 6-11, 14,15
    QCOMPARE(pc2.count(), 4);
    QCOMPARE(pc2.extraCoordinatesCount(), 0);
    QhullPoint p(pc[0]);
    pc2.append(p); // 6-11, 14,15, 0,1
    QCOMPARE(pc2.count(), 5);
    QCOMPARE(pc2.extraCoordinatesCount(), 0);
    QCOMPARE(pc2.lastIndexOf(p), 4);
    pc.append(pc2); // Invalidates p
    QCOMPARE(pc.count(), 8); // 0-11, 14,15, 0,1
    QCOMPARE(pc.extraCoordinatesCount(), 0);
    QCOMPARE(pc.lastIndexOf(pc[0]), 7);
    pc.appendComment(" operators");
    QCOMPARE(pc.comment(), std::string("2-d points operators"));
    pc.checkValid();
    // see t_append_points for appendPoints
    PointCoordinates pc3= pc+pc2;
    pc3.checkValid();
    QCOMPARE(pc3.count(), 13);
    QCOMPARE(pc3[6][0], 14.0);
    QCOMPARE(pc3[8][0], 6.0);
    pc3 += pc;
    QCOMPARE(pc3.count(), 21);
    QCOMPARE(pc3[14][0], 2.0);
    pc3 += 12.0;
    pc3 += 14.0;
    QCOMPARE(pc3.count(), 22);
    QCOMPARE(pc3.last()[0], 12.0);
    // QhullPoint p3= pc3.first(); // += throws error because append may move the data
    QhullPoint p3= pc2.first();
    pc3 += p3;
    QCOMPARE(pc3.count(), 23);
    QCOMPARE(pc3.last()[0], 6.0);
    pc3 << pc;
    QCOMPARE(pc3.count(), 31);
    QCOMPARE(pc3.last()[0], 0.0);
    pc3 << 12.0 << 14.0;
    QCOMPARE(pc3.count(), 32);
    QCOMPARE(pc3.last()[0], 12.0);
    PointCoordinates pc4(pc3);
    pc4.reserveCoordinates(100);
    QVERIFY(pc3==pc4);
}//t_modify

void PointCoordinates_test::
t_append_points()
{
    Qhull q;
    PointCoordinates pc(q, 2, "stringstream");
    stringstream s("2 3 1 2 3 4 5 6");
    pc.appendPoints(s);
    QCOMPARE(pc.count(), 3);
}//t_append_points

void PointCoordinates_test::
t_coord_iterator()
{
    Qhull q;
    PointCoordinates c(q, 2, "2-d");
    c << 0.0 << 1.0 << 2.0 << 3.0 << 4.0 << 5.0;
    PointCoordinatesIterator i(c);
    QhullPoint p0(c[0]);
    QhullPoint p1(c[1]);
    QhullPoint p2(c[2]);
    coordT c2[] = {-1.0, -2.0};
    QhullPoint p3(q, 2, c2);
    PointCoordinatesIterator i2= c;
    QVERIFY(i.findNext(p1));
    QVERIFY(!i.findNext(p1));
    QVERIFY(!i.findNext(p2));
    QVERIFY(!i.findNext(p3));
    QVERIFY(i.findPrevious(p2));
    QVERIFY(!i.findPrevious(p2));
    QVERIFY(!i.findPrevious(p0));
    QVERIFY(!i.findPrevious(p3));
    QVERIFY(i2.findNext(p2));
    QVERIFY(i2.findPrevious(p0));
    QVERIFY(i2.findNext(p1));
    QVERIFY(i2.findPrevious(p0));
    QVERIFY(i2.hasNext());
    QVERIFY(!i2.hasPrevious());
    QVERIFY(i.hasNext());
    QVERIFY(!i.hasPrevious());
    i.toBack();
    i2.toFront();
    QVERIFY(!i.hasNext());
    QVERIFY(i.hasPrevious());
    QVERIFY(i2.hasNext());
    QVERIFY(!i2.hasPrevious());
    PointCoordinates c3(q);
    PointCoordinatesIterator i3= c3;
    QVERIFY(!i3.hasNext());
    QVERIFY(!i3.hasPrevious());
    i3.toBack();
    QVERIFY(!i3.hasNext());
    QVERIFY(!i3.hasPrevious());
    QCOMPARE(i.peekPrevious(), p2);
    QCOMPARE(i.previous(), p2);
    QCOMPARE(i.previous(), p1);
    QCOMPARE(i.previous(), p0);
    QVERIFY(!i.hasPrevious());
    QCOMPARE(i.peekNext(), p0);
    // i.peekNext()= 1.0; // compiler error
    QCOMPARE(i.next(), p0);
    QCOMPARE(i.peekNext(), p1);
    QCOMPARE(i.next(), p1);
    QCOMPARE(i.next(), p2);
    QVERIFY(!i.hasNext());
    i.toFront();
    QCOMPARE(i.next(), p0);
}//t_coord_iterator

void PointCoordinates_test::
t_io()
{
    Qhull q;
    PointCoordinates c(q);
    ostringstream os;
    os << "PointCoordinates 0-d\n" << c;
    c.setDimension(2);
    c << 1.0 << 2.0 << 3.0 << 1.0 << 2.0 << 3.0;
    os << "PointCoordinates 1,2 3,1 2,3\n" << c;
    cout << os.str();
    QString s= QString::fromStdString(os.str());
    QCOMPARE(s.count("0"), 3);
    QCOMPARE(s.count("2"), 5);
}//t_io

}//orgQhull

#include "moc/PointCoordinates_test.moc"
