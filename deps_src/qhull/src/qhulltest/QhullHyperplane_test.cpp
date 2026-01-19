/****************************************************************************
**
** Copyright (c) 2009-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/qhulltest/QhullHyperplane_test.cpp#4 $$Change: 2064 $
** $DateTime: 2016/01/18 12:36:08 $$Author: bbarber $
**
****************************************************************************/

//pre-compiled headers
#include <iostream>
#include "qhulltest/RoadTest.h" // QT_VERSION

#include "libqhullcpp/QhullHyperplane.h"
#include "libqhullcpp/QhullError.h"
#include "libqhullcpp/RboxPoints.h"
#include "libqhullcpp/QhullFacet.h"
#include "libqhullcpp/QhullFacetList.h"
#include "libqhullcpp/QhullFacetSet.h"
#include "libqhullcpp/Qhull.h"

#include <numeric>
#include <vector>

using std::cout;
using std::endl;
using std::ostringstream;
using std::ostream;
using std::string;

namespace orgQhull {

class QhullHyperplane_test : public RoadTest
{
    Q_OBJECT

#//!\name Test slots
private slots:
    void cleanup();
    void t_construct();
    void t_construct_qh();
    void t_convert();
    void t_readonly();
    void t_define();
    void t_value();
    void t_operator();
    void t_iterator();
    void t_const_iterator();
    void t_qhullHyperplane_iterator();
    void t_io();
};//QhullHyperplane_test

void
add_QhullHyperplane_test()
{
    new QhullHyperplane_test();  // RoadTest::s_testcases
}

//Executed after each testcase
void QhullHyperplane_test::
cleanup()
{
    RoadTest::cleanup();
}

void QhullHyperplane_test::
t_construct()
{
    QhullHyperplane h4;
    QVERIFY(!h4.isValid());
    QCOMPARE(h4.dimension(), 0);
    // Qhull.runQhull() constructs QhullFacets as facetT
    RboxPoints rcube("c");
    Qhull q(rcube,"Qt QR0");  // triangulation of rotated unit cube
    QhullHyperplane h(q);
    QVERIFY(!h.isValid());
    QCOMPARE(h.dimension(), 0);
    QCOMPARE(h.coordinates(),static_cast<double *>(0));
    QhullFacet f= q.firstFacet();
    QhullHyperplane h2(f.hyperplane());
    QVERIFY(h2.isValid());
    QCOMPARE(h2.dimension(), 3);
    h= h2;
    QCOMPARE(h, h2);
    QhullHyperplane h3(q, h2.dimension(), h2.coordinates(), h2.offset());
    QCOMPARE(h2, h3);
    QhullHyperplane h5= h2; // copy constructor
    QVERIFY(h5==h2);
}//t_construct

void QhullHyperplane_test::
t_construct_qh()
{
    // Qhull.runQhull() constructs QhullFacets as facetT
    RboxPoints rcube("c");
    Qhull q(rcube,"Qt QR0");  // triangulation of rotated unit cube
    QhullFacet f= q.firstFacet();
    QhullHyperplane h2(f.hyperplane());
    QVERIFY(h2.isValid());
    QCOMPARE(h2.dimension(), 3);
    // h= h2;  // copy assignment disabled, ambiguous
    QhullHyperplane h3(q.qh(), h2.dimension(), h2.coordinates(), h2.offset());
    QCOMPARE(h2, h3);
    QhullHyperplane h5= h2; // copy constructor
    QVERIFY(h5==h2);
}//t_construct_qh

void QhullHyperplane_test::
t_convert()
{
    RboxPoints rcube("c");
    Qhull q(rcube,"Qt QR0");  // triangulation of rotated unit cube
    QhullHyperplane h= q.firstFacet().hyperplane();
    std::vector<double> fs= h.toStdVector();
    QCOMPARE(fs.size(), 4u);
    double offset= fs.back();
    fs.pop_back();
    QCOMPARE(offset, -0.5);

    double squareNorm= inner_product(fs.begin(), fs.end(), fs.begin(), 0.0);
    QCOMPARE(squareNorm, 1.0);
    QList<double> qs= h.toQList();
    QCOMPARE(qs.size(), 4);
    double offset2= qs.takeLast();
    QCOMPARE(offset2, -0.5);
    double squareNorm2= std::inner_product(qs.begin(), qs.end(), qs.begin(), 0.0);
    QCOMPARE(squareNorm2, 1.0);
}//t_convert

void QhullHyperplane_test::
t_readonly()
{
    RboxPoints rcube("c");
    {
        Qhull q(rcube,"Qt QR0");  // triangulation of rotated unit cube
        QhullFacetList fs= q.facetList();
        QhullFacetListIterator i(fs);
        while(i.hasNext()){
            QhullFacet f= i.next();
            QhullHyperplane h= f.hyperplane();
            int id= f.id();
            cout << "h" << id << endl;
            QVERIFY(h.isValid());
            QCOMPARE(h.dimension(),3);
            const coordT *c= h.coordinates();
            coordT *c2= h.coordinates();
            QCOMPARE(c, c2);
            const coordT *c3= h.begin();
            QCOMPARE(c, c3);
            QCOMPARE(h.offset(), -0.5);
            int j= h.end()-h.begin();
            QCOMPARE(j, 3);
            double squareNorm= std::inner_product(h.begin(), h.end(), h.begin(), 0.0);
            QCOMPARE(squareNorm, 1.0);
        }
        QhullHyperplane h2= fs.first().hyperplane();
        QhullHyperplane h3= fs.last().hyperplane();
        QVERIFY(h2!=h3);
        QVERIFY(h3.coordinates()!=h2.coordinates());
    }
}//t_readonly

void QhullHyperplane_test::
t_define()
{
    RboxPoints rcube("c");
    {
        Qhull q(rcube,"Qt QR0");  // triangulation of rotated unit cube
        QhullFacetList fs= q.facetList();
        QhullHyperplane h= fs.first().hyperplane();
        QhullHyperplane h2= h;
        QVERIFY(h==h2);
        QhullHyperplane h3= fs.last().hyperplane();
        QVERIFY(h2!=h3);

        QhullHyperplane h4= h3;
        h4.defineAs(h2);
        QVERIFY(h2==h4);
        QhullHyperplane p5= h3;
        p5.defineAs(h2.dimension(), h2.coordinates(), h2.offset());
        QVERIFY(h2==p5);
        QhullHyperplane h6= h3;
        h6.setCoordinates(h2.coordinates());
        QCOMPARE(h2.coordinates(), h6.coordinates());
        h6.setOffset(h2.offset());
        QCOMPARE(h2.offset(), h6.offset());
        QVERIFY(h2==h6);
        h6.setDimension(2);
        QCOMPARE(h6.dimension(), 2);
        QVERIFY(h2!=h6);
    }
}//t_define

void QhullHyperplane_test::
t_value()
{
    RboxPoints rcube("c G1");
    Qhull q(rcube,"Qt QR0");  // triangulation of rotated unit cube
    QhullFacet f= q.firstFacet();
    QhullFacet f2= f.neighborFacets().at(0);
    const QhullHyperplane h= f.hyperplane();
    const QhullHyperplane h2= f2.hyperplane();   // At right angles
    double dist= h.distance(q.origin());
    QCOMPARE(dist, -1.0);
    double norm= h.norm();
    QCOMPARE(norm, 1.0);
    double angle= h.hyperplaneAngle(h2);
    cout << "angle " << angle << endl;
    QCOMPARE(angle+1.0, 1.0); // qFuzzyCompare does not work for 0.0
    QVERIFY(h==h);
    QVERIFY(h!=h2);
}//t_value

void QhullHyperplane_test::
t_operator()
{
    RboxPoints rcube("c");
    Qhull q(rcube,"Qt QR0");  // triangulation of rotated unit cube
    const QhullHyperplane h= q.firstFacet().hyperplane();
    //operator== and operator!= tested elsewhere
    const coordT *c= h.coordinates();
    for(int k=h.dimension(); k--; ){
        QCOMPARE(c[k], h[k]);
    }
    //h[0]= 10.0; // compiler error, const
    QhullHyperplane h2= q.firstFacet().hyperplane();
    h2[0]= 10.0;  // Overwrites Hyperplane coordinate!
    QCOMPARE(h2[0], 10.0);
}//t_operator

void QhullHyperplane_test::
t_iterator()
{
    RboxPoints rcube("c");
    {
        Qhull q(rcube,"QR0");  // rotated unit cube
        QhullHyperplane h= q.firstFacet().hyperplane();
        QCOMPARE(h.count(), 3);
        QCOMPARE(h.size(), 3u);
        QhullHyperplane::Iterator i= h.begin();
        QhullHyperplane::iterator i2= h.begin();
        QVERIFY(i==i2);
        QVERIFY(i>=i2);
        QVERIFY(i<=i2);
        i= h.begin();
        QVERIFY(i==i2);
        i2= h.end();
        QVERIFY(i!=i2);
        double d3= *i;
        i2--;
        double d2= *i2;
        QCOMPARE(d3, h[0]);
        QCOMPARE(d2, h[2]);
        QhullHyperplane::Iterator i3(i2);
        QCOMPARE(*i2, *i3);

        (i3= i)++;
        QCOMPARE((*i3), h[1]);
        QVERIFY(i==i);
        QVERIFY(i!=i2);
        QVERIFY(i<i2);
        QVERIFY(i<=i2);
        QVERIFY(i2>i);
        QVERIFY(i2>=i);

        QhullHyperplane::ConstIterator i4= h.begin();
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
        QVERIFY(i<i4);
        QVERIFY(i<=i4);
        QVERIFY(i4>i);
        QVERIFY(i4>=i);

        i= h.begin();
        i2= h.begin();
        QCOMPARE(i, i2++);
        QCOMPARE(*i2, h[1]);
        QCOMPARE(++i, i2);
        QCOMPARE(i, i2--);
        QCOMPARE(i2, h.begin());
        QCOMPARE(--i, i2);
        QCOMPARE(i2 += 3, h.end());
        QCOMPARE(i2 -= 3, h.begin());
        QCOMPARE(i2+0, h.begin());
        QCOMPARE(i2+3, h.end());
        i2 += 3;
        i= i2-0;
        QCOMPARE(i, i2);
        i= i2-3;
        QCOMPARE(i, h.begin());
        QCOMPARE(i2-i, 3);

        //h.begin end tested above

        // QhullHyperplane is const-only
    }
}//t_iterator

void QhullHyperplane_test::
t_const_iterator()
{
    RboxPoints rcube("c");
    {
        Qhull q(rcube,"QR0");  // rotated unit cube
        QhullHyperplane h= q.firstFacet().hyperplane();
        QhullHyperplane::ConstIterator i= h.begin();
        QhullHyperplane::const_iterator i2= h.begin();
        QVERIFY(i==i2);
        QVERIFY(i>=i2);
        QVERIFY(i<=i2);
        i= h.begin();
        QVERIFY(i==i2);
        i2= h.end();
        QVERIFY(i!=i2);
        double d3= *i;
        i2--;
        double d2= *i2;
        QCOMPARE(d3, h[0]);
        QCOMPARE(d2, h[2]);
        QhullHyperplane::ConstIterator i3(i2);
        QCOMPARE(*i2, *i3);

        (i3= i)++;
        QCOMPARE((*i3), h[1]);
        QVERIFY(i==i);
        QVERIFY(i!=i2);
        QVERIFY(i<i2);
        QVERIFY(i<=i2);
        QVERIFY(i2>i);
        QVERIFY(i2>=i);

        // See t_iterator for const_iterator COMP iterator

        i= h.begin();
        i2= h.constBegin();
        QCOMPARE(i, i2++);
        QCOMPARE(*i2, h[1]);
        QCOMPARE(++i, i2);
        QCOMPARE(i, i2--);
        QCOMPARE(i2, h.constBegin());
        QCOMPARE(--i, i2);
        QCOMPARE(i2+=3, h.constEnd());
        QCOMPARE(i2-=3, h.constBegin());
        QCOMPARE(i2+0, h.constBegin());
        QCOMPARE(i2+3, h.constEnd());
        i2 += 3;
        i= i2-0;
        QCOMPARE(i, i2);
        i= i2-3;
        QCOMPARE(i, h.constBegin());
        QCOMPARE(i2-i, 3);

        // QhullHyperplane is const-only
    }
}//t_const_iterator

void QhullHyperplane_test::
t_qhullHyperplane_iterator()
{
    RboxPoints rcube("c");
    Qhull q(rcube,"QR0");  // rotated unit cube
    QhullHyperplane h = q.firstFacet().hyperplane();
    QhullHyperplaneIterator i2(h);
    QCOMPARE(h.dimension(), 3);
    QhullHyperplaneIterator i= h;
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

    // i at front, i2 at end/back, 3 coordinates
    QCOMPARE(i.peekNext(), h[0]);
    QCOMPARE(i2.peekPrevious(), h[2]);
    QCOMPARE(i2.previous(), h[2]);
    QCOMPARE(i2.previous(), h[1]);
    QCOMPARE(i2.previous(), h[0]);
    QVERIFY(!i2.hasPrevious());
    QCOMPARE(i.peekNext(), h[0]);
    // i.peekNext()= 1.0; // compiler error, i is const
    QCOMPARE(i.next(), h[0]);
    QCOMPARE(i.peekNext(), h[1]);
    QCOMPARE(i.next(), h[1]);
    QCOMPARE(i.next(), h[2]);
    QVERIFY(!i.hasNext());
    i.toFront();
    QCOMPARE(i.next(), h[0]);
}//t_qhullHyperplane_iterator

void QhullHyperplane_test::
t_io()
{
    RboxPoints rcube("c");
    {
        Qhull q(rcube, "");
        QhullHyperplane h= q.firstFacet().hyperplane();
        ostringstream os;
        os << "Hyperplane:\n";
        os << h;
        os << h.print("message");
        os << h.print(" and a message ", " offset ");
        cout << os.str();
        QString s= QString::fromStdString(os.str());
        QCOMPARE(s.count("1"), 3);
        // QCOMPARE(s.count(QRegExp("f\\d")), 3*7 + 13*3*2);
    }
}//t_io


}//orgQhull

#include "moc/QhullHyperplane_test.moc"
