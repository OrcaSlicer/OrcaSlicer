/****************************************************************************
**
** Copyright (c) 2009-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/qhulltest/Coordinates_test.cpp#2 $$Change: 2062 $
** $DateTime: 2016/01/17 13:13:18 $$Author: bbarber $
**
****************************************************************************/

//pre-compiled headers
#include <iostream>
#include "qhulltest/RoadTest.h" // QT_VERSION

#include "libqhullcpp/Coordinates.h"
#include "libqhullcpp/QhullError.h"
#include "libqhullcpp/RboxPoints.h"
#include "libqhullcpp/Qhull.h"

using std::cout;
using std::endl;
using std::ostringstream;
using std::ostream;
using std::string;

namespace orgQhull {

class Coordinates_test : public RoadTest
{
    Q_OBJECT

#//!\name Test slots
private slots:
    void t_construct();
    void t_convert();
    void t_element();
    void t_readonly();
    void t_operator();
    void t_const_iterator();
    void t_iterator();
    void t_coord_iterator();
    void t_mutable_coord_iterator();
    void t_readwrite();
    void t_search();
    void t_io();
};//Coordinates_test

void
add_Coordinates_test()
{
    new Coordinates_test();  // RoadTest::s_testcases
}

void Coordinates_test::
t_construct()
{
    Coordinates c;
    QCOMPARE(c.size(), 0U);
    QVERIFY(c.isEmpty());
    c << 1.0;
    QCOMPARE(c.count(), 1);
    Coordinates c2(c);
    c2 << 2.0;
    QCOMPARE(c2.count(), 2);
    Coordinates c3;
    c3 = c2;
    QCOMPARE(c3.count(), 2);
    QCOMPARE(c3[0]+c3[1], 3.0);
    QVERIFY(c2==c3);
    std::vector<coordT> vc;
    vc.push_back(3.0);
    vc.push_back(4.0);
    Coordinates c4(vc);
    QCOMPARE(c4[0]+c4[1], 7.0);
    Coordinates c5(c3);
    QVERIFY(c5==c3);
    c5= vc;
    QVERIFY(c5!=c3);
    QVERIFY(c5==c4);
}//t_construct

void Coordinates_test::
t_convert()
{
    Coordinates c;
    c << 1.0 << 3.0;
    QCOMPARE(c.data()[1], 3.0);
    coordT *c2= c.data();
    const coordT *c3= c.data();
    QCOMPARE(c2, c3);
    std::vector<coordT> vc= c.toStdVector();
    QCOMPARE((size_t)vc.size(), c.size());
    for(int k= (int)vc.size(); k--; ){
        QCOMPARE(vc[k], c[k]);
    }
    QList<coordT> qc= c.toQList();
    QCOMPARE(qc.count(), c.count());
    for(int k= qc.count(); k--; ){
        QCOMPARE(qc[k], c[k]);
    }
    Coordinates c4;
    c4= std::vector<double>(2, 0.0);
    QCOMPARE(c4.back(), 0.0);
    Coordinates c5(std::vector<double>(2, 0.0));
    QCOMPARE(c4.size(), c5.size());
    QVERIFY(c4==c5);
}//t_convert

void Coordinates_test::
t_element()
{
    Coordinates c;
    c << 1.0 << -2.0;
    c.at(1)= -3;
    QCOMPARE(c.at(1), -3.0);
    QCOMPARE(c.back(), -3.0);
    QCOMPARE(c.front(), 1.0);
    c[1]= -2.0;
    QCOMPARE(c[1],-2.0);
    QCOMPARE(c.first(), 1.0);
    c.first()= 2.0;
    QCOMPARE(c.first(), 2.0);
    QCOMPARE(c.last(), -2.0);
    c.last()= 0.0;
    QCOMPARE(c.first()+c.last(), 2.0);
    coordT *c4= &c.first();
    const coordT *c5= &c.first();
    QCOMPARE(c4, c5);
    coordT *c6= &c.last();
    const coordT *c7= &c.last();
    QCOMPARE(c6, c7);
    Coordinates c2= c.mid(1);
    QCOMPARE(c2.count(), 1);
    c << 3.0;
    Coordinates c3= c.mid(1,1);
    QCOMPARE(c2, c3);
    QCOMPARE(c3.value(-1, -1.0), -1.0);
    QCOMPARE(c3.value(3, 4.0), 4.0);
    QCOMPARE(c.value(2, 4.0), 3.0);
}//t_element

void Coordinates_test::
t_readonly()
{
    Coordinates c;
    QCOMPARE(c.size(), 0u);
    QCOMPARE(c.count(), 0);
    QVERIFY(c.isEmpty());
    c << 1.0 << -2.0;
    QCOMPARE(c.size(), 2u);
    QCOMPARE(c.count(), 2);
    QVERIFY(!c.isEmpty());
}//t_readonly

void Coordinates_test::
t_operator()
{
    Coordinates c;
    Coordinates c2(c);
    QVERIFY(c==c2);
    QVERIFY(!(c!=c2));
    c << 1.0;
    QVERIFY(!(c==c2));
    QVERIFY(c!=c2);
    c2 << 1.0;
    QVERIFY(c==c2);
    QVERIFY(!(c!=c2));
    c[0]= 0.0;
    QVERIFY(c!=c2);
    Coordinates c3= c+c2;
    QCOMPARE(c3.count(), 2);
    QCOMPARE(c3[0], 0.0);
    QCOMPARE(c3[1], 1.0);
    c3 += c3;
    QCOMPARE(c3.count(), 4);
    QCOMPARE(c3[2], 0.0);
    QCOMPARE(c3[3], 1.0);
    c3 += c2;
    QCOMPARE(c3[4], 1.0);
    c3 += 5.0;
    QCOMPARE(c3.count(), 6);
    QCOMPARE(c3[5], 5.0);
    // << checked above
}//t_operator

void Coordinates_test::
t_const_iterator()
{
    Coordinates c;
    QCOMPARE(c.begin(), c.end());
    // begin and end checked elsewhere
    c << 1.0 << 3.0;
    Coordinates::const_iterator i= c.begin();
    QCOMPARE(*i, 1.0);
    QCOMPARE(i[1], 3.0);
    // i[1]= -3.0; // compiler error
    // operator-> is not applicable to double
    QCOMPARE(*i++, 1.0);
    QCOMPARE(*i, 3.0);
    QCOMPARE(*i--, 3.0);
    QCOMPARE(*i, 1.0);
    QCOMPARE(*(i+1), 3.0);
    QCOMPARE(*++i, 3.0);
    QCOMPARE(*(i-1), 1.0);
    QCOMPARE(*--i, 1.0);
    QVERIFY(i==c.begin());
    QVERIFY(i==c.constBegin());
    QVERIFY(i!=c.end());
    QVERIFY(i!=c.constEnd());
    QVERIFY(i<c.end());
    QVERIFY(i>=c.begin());
    QVERIFY(i+1<=c.end());
    QVERIFY(i+1>c.begin());
    Coordinates::iterator i2= c.begin();
    Coordinates::const_iterator i3(i2);
    QCOMPARE(*i3, 1.0);
    QCOMPARE(i3[1], 3.0);
}//t_const_iterator

void Coordinates_test::
t_iterator()
{
    Coordinates c;
    QCOMPARE(c.begin(), c.end());
    // begin and end checked elsewhere
    c << 1.0 << 3.0;
    Coordinates::iterator i= c.begin();
    QCOMPARE(*i, 1.0);
    QCOMPARE(i[1], 3.0);
    *i= -1.0;
    QCOMPARE(*i, -1.0);
    i[1]= -3.0;
    QCOMPARE(i[1], -3.0);
    *i= 1.0;
    // operator-> is not applicable to double
    QCOMPARE(*i++, 1.0);
    QCOMPARE(*i, -3.0);
    *i= 3.0;
    QCOMPARE(*i--, 3.0);
    QCOMPARE(*i, 1.0);
    QCOMPARE(*(i+1), 3.0);
    QCOMPARE(*++i, 3.0);
    QCOMPARE(*(i-1), 1.0);
    QCOMPARE(*--i, 1.0);
    QVERIFY(i==c.begin());
    QVERIFY(i==c.constBegin());
    QVERIFY(i!=c.end());
    QVERIFY(i!=c.constEnd());
    QVERIFY(i<c.end());
    QVERIFY(i>=c.begin());
    QVERIFY(i+1<=c.end());
    QVERIFY(i+1>c.begin());
}//t_iterator

void Coordinates_test::
t_coord_iterator()
{
    Coordinates c;
    c << 1.0 << 3.0;
    CoordinatesIterator i(c);
    CoordinatesIterator i2= c;
    QVERIFY(i.findNext(1.0));
    QVERIFY(!i.findNext(2.0));
    QVERIFY(!i.findNext(3.0));
    QVERIFY(i.findPrevious(3.0));
    QVERIFY(!i.findPrevious(2.0));
    QVERIFY(!i.findPrevious(1.0));
    QVERIFY(i2.findNext(3.0));
    QVERIFY(i2.findPrevious(3.0));
    QVERIFY(i2.findNext(3.0));
    QVERIFY(i2.findPrevious(1.0));
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
    Coordinates c2;
    i2= c2;
    QVERIFY(!i2.hasNext());
    QVERIFY(!i2.hasPrevious());
    i2.toBack();
    QVERIFY(!i2.hasNext());
    QVERIFY(!i2.hasPrevious());
    QCOMPARE(i.peekPrevious(), 3.0);
    QCOMPARE(i.previous(), 3.0);
    QCOMPARE(i.previous(), 1.0);
    QVERIFY(!i.hasPrevious());
    QCOMPARE(i.peekNext(), 1.0);
    // i.peekNext()= 1.0; // compiler error
    QCOMPARE(i.next(), 1.0);
    QCOMPARE(i.peekNext(), 3.0);
    QCOMPARE(i.next(), 3.0);
    QVERIFY(!i.hasNext());
    i.toFront();
    QCOMPARE(i.next(), 1.0);
}//t_coord_iterator

void Coordinates_test::
t_mutable_coord_iterator()
{
    // Same tests as CoordinatesIterator
    Coordinates c;
    c << 1.0 << 3.0;
    MutableCoordinatesIterator i(c);
    MutableCoordinatesIterator i2= c;
    QVERIFY(i.findNext(1.0));
    QVERIFY(!i.findNext(2.0));
    QVERIFY(!i.findNext(3.0));
    QVERIFY(i.findPrevious(3.0));
    QVERIFY(!i.findPrevious(2.0));
    QVERIFY(!i.findPrevious(1.0));
    QVERIFY(i2.findNext(3.0));
    QVERIFY(i2.findPrevious(3.0));
    QVERIFY(i2.findNext(3.0));
    QVERIFY(i2.findPrevious(1.0));
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
    Coordinates c2;
    i2= c2;
    QVERIFY(!i2.hasNext());
    QVERIFY(!i2.hasPrevious());
    i2.toBack();
    QVERIFY(!i2.hasNext());
    QVERIFY(!i2.hasPrevious());
    QCOMPARE(i.peekPrevious(), 3.0);
    QCOMPARE(i.peekPrevious(), 3.0);
    QCOMPARE(i.previous(), 3.0);
    QCOMPARE(i.previous(), 1.0);
    QVERIFY(!i.hasPrevious());
    QCOMPARE(i.peekNext(), 1.0);
    QCOMPARE(i.next(), 1.0);
    QCOMPARE(i.peekNext(), 3.0);
    QCOMPARE(i.next(), 3.0);
    QVERIFY(!i.hasNext());
    i.toFront();
    QCOMPARE(i.next(), 1.0);

    // Mutable tests
    i.toFront();
    i.peekNext()= -1.0;
    QCOMPARE(i.peekNext(), -1.0);
    QCOMPARE((i.next()= 1.0), 1.0);
    QCOMPARE(i.peekPrevious(), 1.0);
    i.remove();
    QCOMPARE(c.count(), 1);
    i.remove();
    QCOMPARE(c.count(), 1);
    QCOMPARE(i.peekNext(), 3.0);
    i.insert(1.0);
    i.insert(2.0);
    QCOMPARE(c.count(), 3);
    QCOMPARE(i.peekNext(), 3.0);
    QCOMPARE(i.peekPrevious(), 2.0);
    i.peekPrevious()= -2.0;
    QCOMPARE(i.peekPrevious(), -2.0);
    QCOMPARE((i.previous()= 2.0), 2.0);
    QCOMPARE(i.peekNext(), 2.0);
    i.toBack();
    i.remove();
    QCOMPARE(c.count(), 3); // unchanged
    i.toFront();
    i.remove();
    QCOMPARE(c.count(), 3); // unchanged
    QCOMPARE(i.peekNext(), 1.0);
    i.remove();
    QCOMPARE(c.count(), 3); // unchanged
    i.insert(0.0);
    QCOMPARE(c.count(), 4);
    QCOMPARE(i.value(), 0.0);
    QCOMPARE(i.peekPrevious(), 0.0);
    i.setValue(-10.0);
    QCOMPARE(c.count(), 4); // unchanged
    QCOMPARE(i.peekNext(), 1.0);
    QCOMPARE(i.peekPrevious(), -10.0);
    i.findNext(1.0);
    i.setValue(-1.0);
    QCOMPARE(i.peekPrevious(), -1.0);
    i.setValue(1.0);
    QCOMPARE(i.peekPrevious(), 1.0);
    QCOMPARE(i.value(), 1.0);
    i.findPrevious(1.0);
    i.setValue(-1.0);
    QCOMPARE(i.peekNext(), -1.0);
    i.toBack();
    QCOMPARE(i.previous(), 3.0);
    i.setValue(-3.0);
    QCOMPARE(i.peekNext(), -3.0);
    double d= i.value();
    QCOMPARE(d, -3.0);
    QCOMPARE(i.previous(), 2.0);
}//t_mutable_coord_iterator

void Coordinates_test::
t_readwrite()
{
    Coordinates c;
    c.clear();
    QCOMPARE(c.count(), 0);
    c << 1.0 << 3.0;
    c.clear();
    QCOMPARE(c.count(), 0);
    coordT c2[4]= { 0.0, 1.0, 2.0, 3.0};
    c.append(4, c2);
    QCOMPARE(c.count(), 4);
    QCOMPARE(c[0], 0.0);
    QCOMPARE(c[1], 1.0);
    QCOMPARE(c[3], 3.0);
    c.clear();
    c << 1.0 << 3.0;
    c.erase(c.begin(), c.end());
    QCOMPARE(c.count(), 0);
    c << 1.0 << 0.0;
    Coordinates::iterator i= c.erase(c.begin());
    QCOMPARE(*i, 0.0);
    i= c.insert(c.end(), 1.0);
    QCOMPARE(*i, 1.0);
    QCOMPARE(c.count(), 2);
    c.pop_back();
    QCOMPARE(c.count(), 1);   // 0
    QCOMPARE(c[0], 0.0);
    c.push_back(2.0);
    QCOMPARE(c.count(), 2);
    c.append(3.0);
    QCOMPARE(c.count(), 3);   // 0, 2, 3
    QCOMPARE(c[2], 3.0);
    c.insert(0, 4.0);
    QCOMPARE(c[0], 4.0);
    QCOMPARE(c[3], 3.0);
    c.insert(c.count(), 5.0);
    QCOMPARE(c.count(), 5);   // 4, 0, 2, 3, 5
    QCOMPARE(c[4], 5.0);
    c.move(4, 0);
    QCOMPARE(c.count(), 5);   // 5, 4, 0, 2, 3
    QCOMPARE(c[0], 5.0);
    c.pop_front();
    QCOMPARE(c.count(), 4);
    QCOMPARE(c[0], 4.0);
    c.prepend(6.0);
    QCOMPARE(c.count(), 5);   // 6, 4, 0, 2, 3
    QCOMPARE(c[0], 6.0);
    c.push_front(7.0);
    QCOMPARE(c.count(), 6);
    QCOMPARE(c[0], 7.0);
    c.removeAt(1);
    QCOMPARE(c.count(), 5);   // 7, 4, 0, 2, 3
    QCOMPARE(c[1], 4.0);
    c.removeFirst();
    QCOMPARE(c.count(), 4);   // 4, 0, 2, 3
    QCOMPARE(c[0], 4.0);
    c.removeLast();
    QCOMPARE(c.count(), 3);
    QCOMPARE(c.last(), 2.0);
    c.replace(2, 8.0);
    QCOMPARE(c.count(), 3);   // 4, 0, 8
    QCOMPARE(c[2], 8.0);
    c.swap(0, 2);
    QCOMPARE(c[2], 4.0);
    double d= c.takeAt(2);
    QCOMPARE(c.count(), 2);   // 8, 0
    QCOMPARE(d, 4.0);
    double d2= c.takeFirst();
    QCOMPARE(c.count(), 1);   // 0
    QCOMPARE(d2, 8.0);
    double d3= c.takeLast();
    QVERIFY(c.isEmpty()); \
    QCOMPARE(d3, 0.0);
}//t_readwrite

void Coordinates_test::
t_search()
{
    Coordinates c;
    c << 1.0 << 3.0 << 1.0;
    QVERIFY(c.contains(1.0));
    QVERIFY(c.contains(3.0));
    QVERIFY(!c.contains(0.0));
    QCOMPARE(c.count(1.0), 2);
    QCOMPARE(c.count(3.0), 1);
    QCOMPARE(c.count(0.0), 0);
    QCOMPARE(c.indexOf(1.0), 0);
    QCOMPARE(c.indexOf(3.0), 1);
    QCOMPARE(c.indexOf(1.0, -1), 2);
    QCOMPARE(c.indexOf(3.0, -1), -1);
    QCOMPARE(c.indexOf(3.0, -2), 1);
    QCOMPARE(c.indexOf(1.0, -3), 0);
    QCOMPARE(c.indexOf(1.0, -4), 0);
    QCOMPARE(c.indexOf(1.0, 1), 2);
    QCOMPARE(c.indexOf(3.0, 2), -1);
    QCOMPARE(c.indexOf(1.0, 2), 2);
    QCOMPARE(c.indexOf(1.0, 3), -1);
    QCOMPARE(c.indexOf(1.0, 4), -1);
    QCOMPARE(c.lastIndexOf(1.0), 2);
    QCOMPARE(c.lastIndexOf(3.0), 1);
    QCOMPARE(c.lastIndexOf(1.0, -1), 2);
    QCOMPARE(c.lastIndexOf(3.0, -1), 1);
    QCOMPARE(c.lastIndexOf(3.0, -2), 1);
    QCOMPARE(c.lastIndexOf(1.0, -3), 0);
    QCOMPARE(c.lastIndexOf(1.0, -4), -1);
    QCOMPARE(c.lastIndexOf(1.0, 1), 0);
    QCOMPARE(c.lastIndexOf(3.0, 2), 1);
    QCOMPARE(c.lastIndexOf(1.0, 2), 2);
    QCOMPARE(c.lastIndexOf(1.0, 3), 2);
    QCOMPARE(c.lastIndexOf(1.0, 4), 2);
    c.removeAll(3.0);
    QCOMPARE(c.count(), 2);
    c.removeAll(4.0);
    QCOMPARE(c.count(), 2);
    c.removeAll(1.0);
    QCOMPARE(c.count(), 0);
    c.removeAll(4.0);
    QCOMPARE(c.count(), 0);
}//t_search

void Coordinates_test::
t_io()
{
    Coordinates c;
    c << 1.0 << 2.0 << 3.0;
    ostringstream os;
    os << "Coordinates 1-2-3\n" << c;
    cout << os.str();
    QString s= QString::fromStdString(os.str());
    QCOMPARE(s.count("2"), 2);
}//t_io

}//orgQhull

#include "moc/Coordinates_test.moc"
