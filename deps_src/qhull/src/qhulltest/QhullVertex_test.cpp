/****************************************************************************
**
** Copyright (c) 2008-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/qhulltest/QhullVertex_test.cpp#3 $$Change: 2062 $
** $DateTime: 2016/01/17 13:13:18 $$Author: bbarber $
**
****************************************************************************/

//pre-compiled headers
#include <iostream>
#include "RoadTest.h" // QT_VERSION

#include "libqhullcpp/QhullVertex.h"
#include "libqhullcpp/Coordinates.h"
#include "libqhullcpp/QhullError.h"
#include "libqhullcpp/RboxPoints.h"
#include "libqhullcpp/QhullFacet.h"
#include "libqhullcpp/QhullFacetSet.h"
#include "libqhullcpp/QhullVertexSet.h"
#include "libqhullcpp/Qhull.h"

using std::cout;
using std::endl;
using std::ostringstream;
using std::ostream;
using std::string;

namespace orgQhull {

class QhullVertex_test : public RoadTest
{
    Q_OBJECT

#//!\name Test slots
private slots:
    void cleanup();
    void t_constructConvert();
    void t_getSet();
    void t_foreach();
    void t_io();
};//QhullVertex_test

void
add_QhullVertex_test()
{
    new QhullVertex_test();  // RoadTest::s_testcases
}

//Executed after each testcase
void QhullVertex_test::
cleanup()
{
    RoadTest::cleanup();
}

void QhullVertex_test::
t_constructConvert()
{
    QhullVertex v6;
    QVERIFY(!v6.isValid());
    QCOMPARE(v6.dimension(),0);
    // Qhull.runQhull() constructs QhullFacets as facetT
    RboxPoints rcube("c");
    Qhull q(rcube,"Qt QR0");  // triangulation of rotated unit cube
    QhullVertex v(q);
    QVERIFY(!v.isValid());
    QCOMPARE(v.dimension(),3);
    QhullVertex v2(q.beginVertex());
    QCOMPARE(v2.dimension(),3);
    v= v2;  // copy assignment
    QVERIFY(v.isValid());
    QCOMPARE(v.dimension(),3);
    QhullVertex v5= v2; // copy constructor
    QVERIFY(v5==v2);
    QVERIFY(v5==v);
    QhullVertex v3(q, v2.getVertexT());
    QCOMPARE(v,v3);
    QhullVertex v4(q, v2.getBaseT());
    QCOMPARE(v,v4);
}//t_constructConvert

void QhullVertex_test::
t_getSet()
{
    RboxPoints rcube("c");
    {
        Qhull q(rcube,"Qt QR0");  // triangulation of rotated unit cube
        QCOMPARE(q.facetCount(), 12);
        QCOMPARE(q.vertexCount(), 8);

        // Also spot-test QhullVertexList.  See QhullLinkedList_test.cpp
        QhullVertexList vs= q.vertexList();
        QhullVertexListIterator i(vs);
        while(i.hasNext()){
            const QhullVertex v= i.next();
            cout << v.id() << endl;
            QCOMPARE(v.dimension(),3);
            QVERIFY(v.id()>=0 && v.id()<9);
            QVERIFY(v.isValid());
            if(i.hasNext()){
                QCOMPARE(v.next(), i.peekNext());
                QVERIFY(v.next()!=v);
                QVERIFY(v.next().previous()==v);
            }
            QVERIFY(i.hasPrevious());
            QCOMPARE(v, i.peekPrevious());
        }

        // test point()
        foreach (QhullVertex v, q.vertexList()){  // Qt only
            QhullPoint p= v.point();
            int j= p.id();
            cout << "Point " << j << ":\n" << p << endl;
            QVERIFY(j>=0 && j<8);
        }
    }
}//t_getSet

void QhullVertex_test::
t_foreach()
{
    RboxPoints rcube("c W0 300");  // 300 points on surface of cube
    {
        Qhull q(rcube, "QR0 Qc"); // keep coplanars, thick facet, and rotate the cube
        foreach (QhullVertex v, q.vertexList()){  // Qt only
            QhullFacetSet fs= v.neighborFacets();
            QCOMPARE(fs.count(), 3);
            foreach (QhullFacet f, fs){  // Qt only
                QVERIFY(f.vertices().contains(v));
            }
        }
    }
}//t_foreach

void QhullVertex_test::
t_io()
{
    RboxPoints rcube("c");
    {
        Qhull q(rcube, "");
        QhullVertex v= q.beginVertex();
        ostringstream os;
        os << "Vertex and vertices:\n";
        os << v;
        QhullVertexSet vs= q.firstFacet().vertices();
        os << vs;
        os << "\nVertex and vertices with message:\n";
        os << v.print("Vertex");
        os << vs.print("\nVertices:");
        cout << os.str();
        QString s= QString::fromStdString(os.str());
        QCOMPARE(s.count("(v"), 10);
        QCOMPARE(s.count(": f"), 2);
    }
    RboxPoints r10("10 D3");  // Without QhullVertex::facetNeighbors
    {
        Qhull q(r10, "");
        QhullVertex v= q.beginVertex();
        ostringstream os;
        os << "\nTry again with simplicial facets.  No neighboring facets listed for vertices.\n";
        os << "Vertex and vertices:\n";
        os << v;
        q.defineVertexNeighborFacets();
        os << "This time with neighborFacets() defined for all vertices:\n";
        os << v;
        cout << os.str();
        QString s= QString::fromStdString(os.str());
        QCOMPARE(s.count(": f"), 1);

        Qhull q2(r10, "v"); // Voronoi diagram
        QhullVertex v2= q2.beginVertex();
        ostringstream os2;
        os2 << "\nTry again with Voronoi diagram of simplicial facets.  Neighboring facets automatically defined for vertices.\n";
        os2 << "Vertex and vertices:\n";
        os2 << v2;
        cout << os2.str();
        QString s2= QString::fromStdString(os2.str());
        QCOMPARE(s2.count(": f"), 1);
    }
}//t_io

}//orgQhull

#include "moc/QhullVertex_test.moc"
