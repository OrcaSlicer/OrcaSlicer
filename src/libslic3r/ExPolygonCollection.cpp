#include "ExPolygonCollection.hpp"
#include "Geometry/ConvexHull.hpp"
#include "BoundingBox.hpp"

namespace Slic3r {

ExPolygonCollection::ExPolygonCollection(const ExPolygon &expolygon)
{
    this->expolygons.push_back(expolygon);
}

ExPolygonCollection::operator Points() const
{
    Points points;
    Polygons pp = (Polygons)*this;
    for (Polygons::const_iterator poly = pp.begin(); poly != pp.end(); ++poly) {
        for (Points::const_iterator point = poly->points.begin(); point != poly->points.end(); ++point)
            points.push_back(*point);
    }
    return points;
}

ExPolygonCollection::operator Polygons() const
{
    Polygons polygons;
    for (ExPolygons::const_iterator it = this->expolygons.begin(); it != this->expolygons.end(); ++it) {
        polygons.push_back(it->contour);
        for (Polygons::const_iterator ith = it->holes.begin(); ith != it->holes.end(); ++ith) {
            polygons.push_back(*ith);
        }
    }
    return polygons;
}

ExPolygonCollection::operator ExPolygons&()
{
    return this->expolygons;
}

void
ExPolygonCollection::scale(double factor)
{
    for (ExPolygons::iterator it = expolygons.begin(); it != expolygons.end(); ++it) {
        (*it).scale(factor);
    }
}

void
ExPolygonCollection::translate(double x, double y)
{
   for (ExPolygons::iterator it = expolygons.begin(); it != expolygons.end(); ++it) {
        (*it).translate(x, y);
    }
}

void
ExPolygonCollection::rotate(double angle, const Point &center)
{
    for (ExPolygons::iterator it = expolygons.begin(); it != expolygons.end(); ++it) {
        (*it).rotate(angle, center);
    }
}

template <class T>
bool ExPolygonCollection::contains(const T &item) const
{
    for (const ExPolygon &poly : this->expolygons)
        if (poly.contains(item))
            return true;
    return false;
}
template bool ExPolygonCollection::contains<Point>(const Point &item) const;
template bool ExPolygonCollection::contains<Line>(const Line &item) const;
template bool ExPolygonCollection::contains<Polyline>(const Polyline &item) const;

bool
ExPolygonCollection::contains_b(const Point &point) const
{
    for (ExPolygons::const_iterator it = this->expolygons.begin(); it != this->expolygons.end(); ++it) {
        if (it->contains_b(point)) return true;
    }
    return false;
}

void
ExPolygonCollection::simplify(double tolerance)
{
    ExPolygons expp;
    for (ExPolygons::const_iterator it = this->expolygons.begin(); it != this->expolygons.end(); ++it) {
        it->simplify(tolerance, &expp);
    }
    this->expolygons = expp;
}

Polygon
ExPolygonCollection::convex_hull() const
{
    Points pp;
    for (ExPolygons::const_iterator it = this->expolygons.begin(); it != this->expolygons.end(); ++it)
        pp.insert(pp.end(), it->contour.points.begin(), it->contour.points.end());
    return Slic3r::Geometry::convex_hull(pp);
}

Lines
ExPolygonCollection::lines() const
{
    Lines lines;
    for (ExPolygons::const_iterator it = this->expolygons.begin(); it != this->expolygons.end(); ++it) {
        Lines ex_lines = it->lines();
        lines.insert(lines.end(), ex_lines.begin(), ex_lines.end());
    }
    return lines;
}

Polygons
ExPolygonCollection::contours() const
{
    Polygons contours;
    contours.reserve(this->expolygons.size());
    for (ExPolygons::const_iterator it = this->expolygons.begin(); it != this->expolygons.end(); ++it)
        contours.push_back(it->contour);
    return contours;
}

void
ExPolygonCollection::append(const ExPolygons &expp)
{
    this->expolygons.insert(this->expolygons.end(), expp.begin(), expp.end());
}

BoundingBox get_extents(const ExPolygonCollection &expolygon)
{
    return get_extents(expolygon.expolygons);
}

}
