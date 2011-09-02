/****************************************************************************
**
** Copyright (c) 2009-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/qhulltest/QhullLinkedList_test.cpp#3 $$Change: 2062 $
** $DateTime: 2016/01/17 13:13:18 $$Author: bbarber $
**
****************************************************************************/

//pre-compiled headers
#include <QtCore/QList>
#include "qhulltest/RoadTest.h"

#include "libqhullcpp/QhullLinkedList.h"
#include "libqhullcpp/Qhull.h"
#include "libqhullcpp/QhullVertex.h"
#include "libqhullcpp/RboxPoints.h"

namespace orgQhull {

class QhullLinkedList_test : public RoadTest
{
    Q_OBJECT

#//!\name Test slots
private slots:
    void cleanup();
    void t_construct();
    void t_convert();
    void t_element();
    void t_search();
    void t_iterator();
    void t_const_iterator();
    void t_QhullLinkedList_iterator();
    void t_io();
};//QhullLinkedList_test

void
add_QhullLinkedList_test()
{
    new QhullLinkedList_test();  // RoadTest::s_testcases
}

//Executed after each testcase
void QhullLinkedList_test::
cleanup()
{
    RoadTest::cleanup();
}

void QhullLinkedList_test::
t_construct()
{
    // QhullLinkedList vs; //private (compiler error).  No memory allocation
    RboxPoints rcube("c");
    {
        Qhull q(rcube,"Qt QR0");  // triangulation of rotated unit cube
        QCOMPARE(q.facetCount(), 12);
        QhullVertexList vs = QhullVertexList(q.beginVertex(), q.endVertex());
        QCOMPARE(vs.count(), 8);
        QCOMPARE(vs.size(), 8u);
        QVERIFY(!vs.isEmpty());
        QhullVertexList vs2 = q.vertexList();
        QCOMPARE(vs2.count(), 8);
        QCOMPARE(vs2.size(),8u);
        QVERIFY(!vs2.isEmpty());
        QVERIFY(vs==vs2);
        // vs= vs2; // disabled.  Would not copy the vertices
        QhullVertexList vs3= vs2; // copy constructor
        QVERIFY(vs3==vs2);
    }
}//t_construct

void QhullLinkedList_test::
t_convert()
{
    RboxPoints rcube("c");
    {
        Qhull q(rcube,"Qt QR0");  // triangulation of rotated unit cube
        QCOMPARE(q.facetCount(), 12);
        QhullVertexList vs = q.vertexList();
        QCOMPARE(vs.size(), 8u);
        QVERIFY(!vs.isEmpty());
        std::vector<QhullVertex> vs2= vs.toStdVector();
        QCOMPARE(vs2.size(), vs.size());
        QhullVertexList::Iterator i= vs.begin();
        for(int k= 0; k<(int)vs2.size(); k++){
            QCOMPARE(vs2[k], *i++);
        }
        QList<QhullVertex> vs3= vs.toQList();
        QCOMPARE(vs3.count(), vs.count());
        i= vs.begin();
        for(int k= 0; k<vs3.count(); k++){
            QCOMPARE(vs3[k], *i++);
        }
        QhullVertexList vs4(q.endVertex(), q.endVertex());
        QVERIFY(vs4.isEmpty());
        QVERIFY(vs==vs);
        QVERIFY(vs4==vs4);
        QVERIFY(vs!=vs4);
    }
}//t_convert

//ReadOnly tested by t_convert

void QhullLinkedList_test::
t_element()
{
    RboxPoints rcube("c");
    Qhull q(rcube,"QR0");  // rotated unit cube
    QhullVertexList vs = q.vertexList();
    QhullVertex v= vs.first();
    QCOMPARE(v.previous(), QhullVertex(NULL));
    QCOMPARE(vs.front(), vs.first());
    QhullVertex v2= vs.last();
    QCOMPARE(v2.next().next(), QhullVertex(NULL)); // sentinel has NULL next
    QCOMPARE(vs.back(), v2);
    QCOMPARE(vs.back(), vs.last());
}//t_element

void QhullLinkedList_test::
t_search()
{
    RboxPoints rcube("c");
    Qhull q(rcube,"QR0");  // rotated unit cube
    QhullVertexList vs = q.vertexList();
    QhullVertex v(q);
    QVERIFY(!vs.contains(v));
    QCOMPARE(vs.count(v), 0);
    QhullVertex v2= *vs.begin();
    QhullVertex v3= vs.last();
    QVERIFY(vs.contains(v2));
    QCOMPARE(vs.count(v2), 1);
    QVERIFY(vs.contains(v3));
    QCOMPARE(vs.count(v3), 1);
}//t_search

void QhullLinkedList_test::
t_iterator()
{
    RboxPoints rcube("c");
    {
        Qhull q(rcube,"QR0");  // rotated unit cube
        QhullVertexList vs = q.vertexList();
        QhullVertexList::Iterator i= vs.begin();
        QhullVertexList::iterator i2= vs.begin();
        QVERIFY(i==i2);
        // No comparisons
        i= vs.begin();
        QVERIFY(i==i2);
        i2= vs.end();
        QVERIFY(i!=i2);
        QhullVertex v3(*i);
        i2--;
        QhullVertex v8= *i2;
        QhullVertex v= vs.first();
        QhullVertex v2= v.next();
        QCOMPARE(v3.id(), v.id());
        QCOMPARE(v8.id(), vs.last().id());
        QhullVertexList::Iterator i3(i2);
        QCOMPARE(*i2, *i3);

        (i3= i)++;
        QCOMPARE((*i3).id(), v2.id());
        QVERIFY(i==i);
        QVERIFY(i!=i2);

        QhullVertexList::ConstIterator i4= vs.begin();
        QVERIFY(i==i4); // iterator COMP const_iterator
        QVERIFY(i4==i); // const_iterator COMP iterator
        QVERIFY(i2!=i4);
        QVERIFY(i4!=i2);
        ++i4;

        i= vs.begin();
        i2= vs.begin();
        QCOMPARE(i, i2++);
        QCOMPARE(*i2, v2);
        QCOMPARE(++i, i2);
        QCOMPARE(i, i2--);
        QCOMPARE(i2, vs.begin());
        QCOMPARE(--i, i2);
        QCOMPARE(i2 += 8, vs.end());
        QCOMPARE(i2 -= 8, vs.begin());
        QCOMPARE(i2+0, vs.begin());
        QCOMPARE(i2+8, vs.end());
        i2 += 8;
        i= i2-0;
        QCOMPARE(i, i2);
        i= i2-8;
        QCOMPARE(i, vs.begin());

        //vs.begin end tested above

        // QhullVertexList is const-only
    }
}//t_iterator

void QhullLinkedList_test::
t_const_iterator()
{
    RboxPoints rcube("c");
    {
        Qhull q(rcube,"QR0");  // rotated unit cube
        QhullVertexList vs = q.vertexList();
        QhullVertexList::ConstIterator i= vs.begin();
        QhullVertexList::const_iterator i2= vs.begin();
        QVERIFY(i==i2);
        i= vs.begin();
        QVERIFY(i==i2);
        i2= vs.end();
        QVERIFY(i!=i2);
        QhullVertex v3(*i);
        i2--;
        QhullVertex v8= *i2;
        QhullVertex v= vs.first();
        QhullVertex v2= v.next();
        QCOMPARE(v3.id(), v.id());
        QCOMPARE(v8.id(), vs.last().id());
        QhullVertexList::ConstIterator i3(i2);
        QCOMPARE(*i2, *i3);

        (i3= i)++;
        QCOMPARE((*i3).id(), v2.id());
        QVERIFY(i==i);
        QVERIFY(i!=i2);

        // See t_iterator for const_iterator COMP iterator

        i= vs.begin();
        i2= vs.constBegin();
        QCOMPARE(i, i2++);
        QCOMPARE(*i2, v2);
        QCOMPARE(++i, i2);
        QCOMPARE(i, i2--);
        QCOMPARE(i2, vs.constBegin());
        QCOMPARE(--i, i2);
        QCOMPARE(i2 += 8, vs.constEnd());
        QCOMPARE(i2 -= 8, vs.constBegin());
        QCOMPARE(i2+0, vs.constBegin());
        QCOMPARE(i2+8, vs.constEnd());
        i2 += 8;
        i= i2-0;
        QCOMPARE(i, i2);
        i= i2-8;
        QCOMPARE(i, vs.constBegin());

        // QhullVertexList is const-only
    }
}//t_const_iterator

void QhullLinkedList_test::
t_QhullLinkedList_iterator()
{
    RboxPoints rcube("c");
    Qhull q(rcube,"QR0");  // rotated unit cube
    QhullVertexList vs(q.endVertex(), q.endVertex());
    QhullVertexListIterator i= vs;
    QCOMPARE(vs.count(), 0);
    QVERIFY(!i.hasNext());
    QVERIFY(!i.hasPrevious());
    i.toBack();
    QVERIFY(!i.hasNext());
    QVERIFY(!i.hasPrevious());

    QhullVertexList vs2 = q.vertexList();
    QhullVertexListIterator i2(vs2);
    QCOMPARE(vs2.count(), 8);
    i= vs2;
    QVERIFY(i2.hasNext());
    QVERIFY(!i2.hasPrevious());
    QVERIFY(i.hasNext());
    QVERIFY(!i.hasPrevious());
    i2.toBack();
    i.toFront();
    QVERIFY(!i2.hasNext());
    QVERIFY(i2.hasPrevious());
    QVERIFY(i.hasNext());
    QVERIFY(!i.hasPrevious());

    // i at front, i2 at end/back, 4 neighbors
    QhullVertexList vs3 = q.vertexList(); // same as vs2
    QhullVertex v3(vs3.first());
    QhullVertex v4= vs3.first();
    QCOMPARE(v3, v4);
    QVERIFY(v3==v4);
    QhullVertex v5(v4.next());
    QVERIFY(v4!=v5);
    QhullVertex v6(v5.next());
    QhullVertex v7(v6.next());
    QhullVertex v8(vs3.last());
    QCOMPARE(i2.peekPrevious(), v8);
    i2.previous();
    i2.previous();
    i2.previous();
    i2.previous();
    QCOMPARE(i2.previous(), v7);
    QCOMPARE(i2.previous(), v6);
    QCOMPARE(i2.previous(), v5);
    QCOMPARE(i2.previous(), v4);
    QVERIFY(!i2.hasPrevious());
    QCOMPARE(i.peekNext(), v4);
    // i.peekNext()= 1.0; // compiler error
    QCOMPARE(i.next(), v4);
    QCOMPARE(i.peekNext(), v5);
    QCOMPARE(i.next(), v5);
    QCOMPARE(i.next(), v6);
    QCOMPARE(i.next(), v7);
    i.next();
    i.next();
    i.next();
    QCOMPARE(i.next(), v8);
    QVERIFY(!i.hasNext());
    i.toFront();
    QCOMPARE(i.next(), v4);
}//t_QhullLinkedList_iterator

void QhullLinkedList_test::
t_io()
{
    RboxPoints rcube("c");
    Qhull q(rcube,"QR0");  // rotated unit cube
    QhullVertexList vs(q.endVertex(), q.endVertex());
    std::cout << "INFO:     empty QhullVertextList" << vs << std::endl;
    QhullVertexList vs2= q.vertexList();
    std::cout << "INFO:   " << vs2 << std::endl;
}//t_io

}//namespace orgQhull

#include "moc/QhullLinkedList_test.moc"
