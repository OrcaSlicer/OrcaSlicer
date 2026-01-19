/****************************************************************************
**
** Copyright (c) 2008-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/qhulltest/QhullVertexSet_test.cpp#3 $$Change: 2062 $
** $DateTime: 2016/01/17 13:13:18 $$Author: bbarber $
**
****************************************************************************/

//pre-compiled headers
#include <iostream>
#include "qhulltest/RoadTest.h" // QT_VERSION

#include "libqhullcpp/QhullVertexSet.h"
#include "libqhullcpp/QhullVertex.h"
#include "libqhullcpp/Qhull.h"
#include "libqhullcpp/QhullError.h"
#include "libqhullcpp/QhullFacet.h"
#include "libqhullcpp/RboxPoints.h"

using std::cout;
using std::endl;
using std::ostringstream;
using std::ostream;
using std::string;

namespace orgQhull {

class QhullVertexSet_test : public RoadTest
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
};//QhullVertexSet_test

void
add_QhullVertexSet_test()
{
    new QhullVertexSet_test();  // RoadTest::s_testcases
}

//Executed after each testcase
void QhullVertexSet_test::
cleanup()
{
    RoadTest::cleanup();
}

void QhullVertexSet_test::
t_construct()
{
    RboxPoints rcube("c");
    Qhull q(rcube,"QR0");  // rotated unit cube
    cout << "INFO   : Cube rotated by QR" << q.rotateRandom() << std::endl;
    QhullFacet f= q.firstFacet();
    QhullVertexSet vs= f.vertices();
    QVERIFY(!vs.isEmpty());
    QCOMPARE(vs.count(),4);
    QhullVertexSet vs4= vs; // copy constructor
    QVERIFY(vs4==vs);
    QhullVertexSet vs3(q, q.qh()->del_vertices);
    QVERIFY(vs3.isEmpty());
}//t_construct

void QhullVertexSet_test::
t_convert()
{
    RboxPoints rcube("c");
    Qhull q(rcube,"QR0 QV2");  // rotated unit cube with "good" facets adjacent to point 0
    cout << "INFO   : Cube rotated by QR" << q.rotateRandom() << std::endl;
    QhullFacet f= q.firstFacet();
    QhullVertexSet vs2= f.vertices();
    QCOMPARE(vs2.count(),4);
    std::vector<QhullVertex> fv= vs2.toStdVector();
    QCOMPARE(fv.size(), 4u);
    QList<QhullVertex> fv2= vs2.toQList();
    QCOMPARE(fv2.size(), 4);
    std::vector<QhullVertex> fv3= vs2.toStdVector();
    QCOMPARE(fv3.size(), 4u);
    QList<QhullVertex> fv4= vs2.toQList();
    QCOMPARE(fv4.size(), 4);
}//t_convert

//! Spot check properties and read-only.  See QhullSet_test
void QhullVertexSet_test::
t_readonly()
{
    RboxPoints rcube("c");
    Qhull q(rcube,"QV0");  // good facets are adjacent to point 0
    QhullVertexSet vs= q.firstFacet().vertices();
    QCOMPARE(vs.count(), 4);
    QCOMPARE(vs.count(), 4);
    QhullVertex v= vs.first();
    QhullVertex v2= vs.last();
    QVERIFY(vs.contains(v));
    QVERIFY(vs.contains(v2));
}//t_readonly

void QhullVertexSet_test::
t_foreach()
{
    RboxPoints rcube("c");
    // Spot check predicates and accessors.  See QhullLinkedList_test
    Qhull q(rcube,"QR0");  // rotated unit cube
    cout << "INFO   : Cube rotated by QR" << q.rotateRandom() << std::endl;
    QhullVertexSet vs= q.firstFacet().vertices();
    QVERIFY(vs.contains(vs.first()));
    QVERIFY(vs.contains(vs.last()));
    QCOMPARE(vs.first(), *vs.begin());
    QCOMPARE(*(vs.end()-1), vs.last());
}//t_foreach

void QhullVertexSet_test::
t_io()
{
    RboxPoints rcube("c");
    {
        Qhull q(rcube,"QR0 QV0");   // good facets are adjacent to point 0
        cout << "INFO   : Cube rotated by QR" << q.rotateRandom() << std::endl;
        QhullVertexSet vs= q.firstFacet().vertices();
        ostringstream os;
        os << vs.print("Vertices of first facet with point 0");
        os << vs.printIdentifiers("\nVertex identifiers: ");
        cout<< os.str();
        QString vertices= QString::fromStdString(os.str());
        QCOMPARE(vertices.count(QRegExp(" v[0-9]")), 4);
    }
}//t_io

#ifdef QHULL_USES_QT
QList<QhullVertex> QhullVertexSet::
toQList() const
{
    QhullSetIterator<QhullVertex> i(*this);
    QList<QhullVertex> vs;
    while(i.hasNext()){
        QhullVertex v= i.next();
        vs.append(v);
    }
    return vs;
}//toQList
#endif //QHULL_USES_QT

}//orgQhull

#include "moc/QhullVertexSet_test.moc"
