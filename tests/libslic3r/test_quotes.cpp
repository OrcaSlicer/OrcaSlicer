#include <catch2/catch_all.hpp>

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "libslic3r/CAD/Quotes.hpp"

using namespace Slic3r;

namespace {
const Quotes::Quote* find(const std::vector<Quotes::Quote>& v, Quotes::Kind k, double value)
{
    auto it = std::find_if(v.begin(), v.end(), [&](const Quotes::Quote& q) { return q.kind == k && std::abs(q.value - value) < 1e-6; });
    return it == v.end() ? nullptr : &(*it);
}
}

TEST_CASE("quotes: a plain box has its three extents and nothing else", "[CadDocument][quotes]")
{
    const std::vector<Quotes::Quote> q = Quotes::compute(BRepPrimAPI_MakeBox(40, 30, 20).Shape(), 0);
    REQUIRE(q.size() == 3);
    CHECK(q[0].text() == "40");
    CHECK(q[1].text() == "30");
    CHECK(q[2].text() == "20");
}

TEST_CASE("quotes: two holes of one size read as one quote with a count", "[CadDocument][quotes]")
{
    TopoDS_Shape box = BRepPrimAPI_MakeBox(40, 30, 20).Shape();
    for (double x : {10.0, 30.0}) {
        TopoDS_Shape bore = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(x, 15, -1), gp_Dir(0, 0, 1)), 3.0, 22).Shape();
        box = BRepAlgoAPI_Cut(box, bore).Shape();
    }
    const std::vector<Quotes::Quote> q = Quotes::compute(box, 0);
    const Quotes::Quote* hole = find(q, Quotes::Kind::Diameter, 3.0);
    REQUIRE(hole);
    CHECK(hole->count() == 2);
    CHECK(hole->concave);
    CHECK(hole->text() == "2\xC3\x97 \xC3\x98" "6");
}

TEST_CASE("quotes: a fillet reads as a radius, not a diameter", "[CadDocument][quotes]")
{
    TopoDS_Shape box = BRepPrimAPI_MakeBox(40, 30, 20).Shape();
    BRepFilletAPI_MakeFillet fillet(box);
    TopExp_Explorer ex(box, TopAbs_EDGE);
    fillet.Add(2.5, TopoDS::Edge(ex.Current()));
    const std::vector<Quotes::Quote> q = Quotes::compute(fillet.Shape(), 0);
    const Quotes::Quote* r = find(q, Quotes::Kind::Radius, 2.5);
    REQUIRE(r);
    CHECK(r->text() == "R2.5");
    CHECK_FALSE(r->concave);
}

TEST_CASE("quotes: format_mm drops the trailing zeros", "[CadDocument][quotes]")
{
    CHECK(Quotes::format_mm(6.0) == "6");
    CHECK(Quotes::format_mm(2.50) == "2.5");
    CHECK(Quotes::format_mm(12.346) == "12.35");
}
