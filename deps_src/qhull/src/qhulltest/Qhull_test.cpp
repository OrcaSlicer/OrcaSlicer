/****************************************************************************
**
** Copyright (c) 2008-2015 C.B. Barber. All rights reserved.
** $Id: //main/2015/qhull/src/qhulltest/Qhull_test.cpp#4 $$Change: 2062 $
** $DateTime: 2016/01/17 13:13:18 $$Author: bbarber $
**
****************************************************************************/

//pre-compiled headers
#include <iostream>
#include "qhulltest/RoadTest.h" // QT_VERSION

#include "libqhullcpp/Qhull.h"
#include "libqhullcpp/QhullError.h"
#include "libqhullcpp/RboxPoints.h"
#include "libqhullcpp/QhullFacetList.h"

using std::cout;
using std::endl;
using std::string;

namespace orgQhull {

//! Test C++ interface to Qhull
//! See eg/q_test for tests of Qhull commands
class Qhull_test : public RoadTest
{
    Q_OBJECT

#//!\name Test slots
private slots:
    void cleanup();
    void t_construct();
    void t_attribute();
    void t_message();
    void t_getSet();
    void t_getQh();
    void t_getValue();
    void t_foreach();
    void t_modify();
};//Qhull_test

void
add_Qhull_test()
{
    new Qhull_test();  // RoadTest::s_testcases
}

//Executed after each testcase
void Qhull_test::
cleanup()
{
    RoadTest::cleanup();
}

void Qhull_test::
t_construct()
{
    {
        Qhull q;
        QCOMPARE(q.dimension(),0);
        QVERIFY(q.qh()!=0);
        QCOMPARE(QString(q.qhullCommand()),QString(""));
        QCOMPARE(QString(q.rboxCommand()),QString(""));
        try{
            QCOMPARE(q.area(),0.0);
            QFAIL("area() did not fail.");
        }catch (const std::exception &e) {
            cout << "INFO   : Caught " << e.what();
        }
    }
    {
        RboxPoints rbox("10000");
        Qhull q(rbox, "QR0"); // Random points in a randomly rotated cube.
        QCOMPARE(q.dimension(),3);
        QVERIFY(q.volume() < 1.0);
        QVERIFY(q.volume() > 0.99);
    }
    {
        double points[] = {
            0, 0,
            1, 0,
            1, 1
        };
        Qhull q("triangle", 2, 3, points, "");
        QCOMPARE(q.dimension(),2);
        QCOMPARE(q.facetCount(),3);
        QCOMPARE(q.vertexCount(),3);
        QCOMPARE(q.dimension(),2);
        QCOMPARE(q.area(), 2.0+sqrt(2.0)); // length of boundary
        QCOMPARE(q.volume(), 0.5);        // the 2-d area
    }
}//t_construct

void Qhull_test::
t_attribute()
{
    RboxPoints rcube("c");
    {
        double normals[] = {
            0,  -1, -0.5,
           -1,   0, -0.5,
            1,   0, -0.5,
            0,   1, -0.5
        };
        Qhull q;
        Coordinates feasible;
        feasible << 0.0 << 0.0;
        q.setFeasiblePoint(feasible);
        Coordinates c(std::vector<double>(2, 0.0));
        QVERIFY(q.feasiblePoint()==c);
        q.setOutputStream(&cout);
        q.runQhull("normals of square", 3, 4, normals, "H"); // halfspace intersect
        QVERIFY(q.feasiblePoint()==c); // from qh.feasible_point after runQhull()
        QCOMPARE(q.facetList().count(), 4); // Vertices of square
        cout << "Expecting summary of halfspace intersection\n";
        q.outputQhull();
        q.qh()->disableOutputStream();  // Same as q.disableOutputStream()
        cout << "Expecting no output from qh_fprintf() in Qhull.cpp\n";
        q.outputQhull();
    }
}//t_attribute

//! No QhullMessage for errors outside of qhull
void Qhull_test::
t_message()
{
    RboxPoints rcube("c");
    {
        Qhull q;
        QCOMPARE(q.qhullMessage(), string(""));
        QCOMPARE(q.qhullStatus(), qh_ERRnone);
        QVERIFY(!q.hasQhullMessage());
        try{
            q.runQhull(rcube, "Fd");
            QFAIL("runQhull Fd did not fail.");
        }catch (const std::exception &e) {
            const char *s= e.what();
            cout << "INFO   : Caught " << s;
            QCOMPARE(QString::fromStdString(s).left(6), QString("QH6029"));
            // FIXUP QH11025 -- review decision to clearQhullMessage at QhullError()            // Cleared when copied to QhullError
            QVERIFY(!q.hasQhullMessage());
            // QCOMPARE(q.qhullMessage(), QString::fromStdString(s).remove(0, 7));
            // QCOMPARE(q.qhullStatus(), 6029);
            q.clearQhullMessage();
            QVERIFY(!q.hasQhullMessage());
        }
        q.appendQhullMessage("Append 1");
        QVERIFY(q.hasQhullMessage());
        QCOMPARE(QString::fromStdString(q.qhullMessage()), QString("Append 1"));
        q.appendQhullMessage("\nAppend 2\n");
        QCOMPARE(QString::fromStdString(q.qhullMessage()), QString("Append 1\nAppend 2\n"));
        q.clearQhullMessage();
        QVERIFY(!q.hasQhullMessage());
        QCOMPARE(QString::fromStdString(q.qhullMessage()), QString(""));
    }
    {
        cout << "INFO   : Error stream without output stream\n";
        Qhull q;
        q.setErrorStream(&cout);
        q.setOutputStream(0);
        try{
            q.runQhull(rcube, "Fd");
            QFAIL("runQhull Fd did not fail.");
        }catch (const QhullError &e) {
            cout << "INFO   : Caught " << e;
            QCOMPARE(e.errorCode(), 6029);
        }
        //FIXUP QH11025 Qhullmessage cleared when QhullError thrown.  Switched to e
        //QVERIFY(q.hasQhullMessage());
        //QCOMPARE(QString::fromStdString(q.qhullMessage()).left(6), QString("QH6029"));
        q.clearQhullMessage();
        QVERIFY(!q.hasQhullMessage());
    }
    {
        cout << "INFO   : Error output sent to output stream without error stream\n";
        Qhull q;
        q.setErrorStream(0);
        q.setOutputStream(&cout);
        try{
            q.runQhull(rcube, "Tz H0");
            QFAIL("runQhull TZ did not fail.");
        }catch (const std::exception &e) {
            const char *s= e.what();
            cout << "INFO   : Caught " << s;
            QCOMPARE(QString::fromLatin1(s).left(6), QString("QH6023"));
        }
        //FIXUP QH11025 Qhullmessage cleared when QhullError thrown.  Switched to e
        //QVERIFY(q.hasQhullMessage());
        //QCOMPARE(QString::fromStdString(q.qhullMessage()).left(17), QString("qhull: no message"));
        //QCOMPARE(q.qhullStatus(), 6023);
        q.clearQhullMessage();
        QVERIFY(!q.hasQhullMessage());
    }
    {
        cout << "INFO   : No error stream or output stream\n";
        Qhull q;
        q.setErrorStream(0);
        q.setOutputStream(0);
        try{
            q.runQhull(rcube, "Fd");
            QFAIL("outputQhull did not fail.");
        }catch (const std::exception &e) {
            const char *s= e.what();
            cout << "INFO   : Caught " << s;
            QCOMPARE(QString::fromLatin1(s).left(6), QString("QH6029"));
        }
        //FIXUP QH11025 Qhullmessage cleared when QhullError thrown.  Switched to e
        //QVERIFY(q.hasQhullMessage());
        //QCOMPARE(QString::fromStdString(q.qhullMessage()).left(9), QString("qhull err"));
        //QCOMPARE(q.qhullStatus(), 6029);
        q.clearQhullMessage();
        QVERIFY(!q.hasQhullMessage());
    }
}//t_message

void Qhull_test::
t_getSet()
{
    RboxPoints rcube("c");
    {
        Qhull q;
        QVERIFY(!q.initialized());
        q.runQhull(rcube, "s");
        QVERIFY(q.initialized());
        QCOMPARE(q.dimension(), 3);
        QhullPoint p= q.origin();
        QCOMPARE(p.dimension(), 3);
        QCOMPARE(p[0]+p[1]+p[2], 0.0);
        q.setErrorStream(&cout);
        q.outputQhull();
    }
    {
        Qhull q;
        q.runQhull(rcube, "");
        q.setOutputStream(&cout);
        q.outputQhull();
    }
}//t_getSet

void Qhull_test::
t_getQh()
{
    RboxPoints rcube("c");
    {
        Qhull q;
        q.runQhull(rcube, "s");
        QCOMPARE(QString(q.qhullCommand()), QString("qhull s"));
        QCOMPARE(QString(q.rboxCommand()), QString("rbox \"c\""));
        QCOMPARE(q.facetCount(), 6);
        QCOMPARE(q.vertexCount(), 8);
        // Sample fields from Qhull's qhT [libqhull.h]
        QCOMPARE(q.qh()->ALLpoints, 0u);
        QCOMPARE(q.qh()->GOODpoint, 0);
        QCOMPARE(q.qh()->IStracing, 0);
        QCOMPARE(q.qh()->MAXcoplanar+1.0, 1.0); // fuzzy compare
        QCOMPARE(q.qh()->MERGING, 1u);
        QCOMPARE(q.qh()->input_dim, 3);
        QCOMPARE(QString(q.qh()->qhull_options).left(8), QString("  run-id"));
        QCOMPARE(q.qh()->num_facets, 6);
        QCOMPARE(q.qh()->hasTriangulation, 0u);
        QCOMPARE(q.qh()->max_outside - q.qh()->min_vertex + 1.0, 1.0); // fuzzy compare
        QCOMPARE(*q.qh()->gm_matrix+1.0, 1.0); // fuzzy compare
    }
}//t_getQh

void Qhull_test::
t_getValue()
{
    RboxPoints rcube("c");
    {
        Qhull q;
        q.runQhull(rcube, "");
        QCOMPARE(q.area(), 6.0);
        QCOMPARE(q.volume(), 1.0);
    }
}//t_getValue

void Qhull_test::
t_foreach()
{
    RboxPoints rcube("c");
    {
        Qhull q;
        QCOMPARE(q.beginFacet(),q.endFacet());
        QCOMPARE(q.beginVertex(),q.endVertex());
        q.runQhull(rcube, "");
        QCOMPARE(q.facetList().count(), 6);

        // defineVertexNeighborFacets() tested in QhullVertex_test::t_io()

        QhullFacetList facets(q.beginFacet(), q.endFacet());
        QCOMPARE(facets.count(), 6);
        QCOMPARE(q.firstFacet(), q.beginFacet());
        QhullVertexList vertices(q.beginVertex(), q.endVertex());
        QCOMPARE(vertices.count(), 8);
        QCOMPARE(q.firstVertex(), q.beginVertex());
        QhullPoints ps= q.points();
        QCOMPARE(ps.count(), 8);
        QhullPointSet ps2= q.otherPoints();
        QCOMPARE(ps2.count(), 0);
        // ps2= q.otherPoints(); //disabled, would not copy the points
        QCOMPARE(q.facetCount(), 6);
        QCOMPARE(q.vertexCount(), 8);
        coordT *c= q.pointCoordinateBegin(); // of q.points()
        QVERIFY(*c==0.5 || *c==-0.5);
        coordT *c3= q.pointCoordinateEnd();
        QVERIFY(c3[-1]==0.5 || c3[-1]==-0.5);
        QCOMPARE(c3-c, 8*3);
        QCOMPARE(q.vertexList().count(), 8);
    }
}//t_foreach

void Qhull_test::
t_modify()
{
    //addPoint() tested in t_foreach
    RboxPoints diamond("d");
    Qhull q(diamond, "o");
    q.setOutputStream(&cout);
    cout << "Expecting vertexList and facetList of a 3-d diamond.\n";
    q.outputQhull();
    cout << "Expecting normals of a 3-d diamond.\n";
    q.outputQhull("n");
    // runQhull tested in t_attribute(), t_message(), etc.
}//t_modify

}//orgQhull

// Redefine Qhull's usermem_r.c in order to report erroneous calls to qh_exit
void qh_exit(int exitcode) {
    cout << "FAIL!  : Qhull called qh_exit().  Qhull's error handling not available.\n.. See the corresponding Qhull:qhull_message or setErrorStream().\n";
    exit(exitcode);
}
void qh_fprintf_stderr(int msgcode, const char *fmt, ... ) {
    va_list args;

    va_start(args, fmt);
    if(msgcode)
        fprintf(stderr, "QH%.4d ", msgcode);
    vfprintf(stderr, fmt, args);
    va_end(args);
} /* fprintf_stderr */
void qh_free(void *mem) {
    free(mem);
}
void *qh_malloc(size_t size) {
    return malloc(size);
}

#if 0
template<> char * QTest::
toString(const std::string &s)
{
    QByteArray ba = s.c_str();
    return qstrdup(ba.data());
}
#endif

#include "moc/Qhull_test.moc"
