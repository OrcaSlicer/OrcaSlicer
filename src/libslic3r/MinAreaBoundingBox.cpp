#include "MinAreaBoundingBox.hpp"

#include <libslic3r/ExPolygon.hpp>

#if defined(_MSC_VER) && defined(__clang__)
#define BOOST_NO_CXX17_HDR_STRING_VIEW
#endif

#include <boost/rational.hpp>

#include <libslic3r/Int128.hpp>

#if !defined(HAS_INTRINSIC_128_TYPE) || defined(__APPLE__)
#include <boost/multiprecision/integer.hpp>
#endif

#include <libnest2d/backends/libslic3r/geometries.hpp>
#include <libnest2d/utils/rotcalipers.hpp>

namespace Slic3r {

// Used as compute type.
using Unit = int64_t;

#if !defined(HAS_INTRINSIC_128_TYPE) || defined(__APPLE__)
using Rational = boost::rational<boost::multiprecision::int128_t>;
#else
using Rational = boost::rational<__int128>;
#endif

template<class P>
libnest2d::RotatedBox<Point, Unit> minAreaBoundigBox_(
    const P &p, MinAreaBoundigBox::PolygonLevel lvl)
{
    P chull = lvl == MinAreaBoundigBox::pcConvex ?
                        p :
                        libnest2d::sl::convexHull(p);

    libnest2d::removeCollinearPoints(chull);

    return libnest2d::minAreaBoundingBox<P, Unit, Rational>(chull);
}

MinAreaBoundigBox::MinAreaBoundigBox(const Polygon &p, PolygonLevel pc)
{
    libnest2d::RotatedBox<Point, Unit> box = minAreaBoundigBox_(p, pc);

    m_right  = libnest2d::cast<long double>(box.right_extent());
    m_bottom = libnest2d::cast<long double>(box.bottom_extent());
    m_axis   = box.axis();
}

MinAreaBoundigBox::MinAreaBoundigBox(const ExPolygon &p, PolygonLevel pc)
{
    libnest2d::RotatedBox<Point, Unit> box = minAreaBoundigBox_(p, pc);

    m_right  = libnest2d::cast<long double>(box.right_extent());
    m_bottom = libnest2d::cast<long double>(box.bottom_extent());
    m_axis   = box.axis();
}

MinAreaBoundigBox::MinAreaBoundigBox(const Points &pts, PolygonLevel pc)
{
    libnest2d::RotatedBox<Point, Unit> box = minAreaBoundigBox_(pts, pc);

    m_right  = libnest2d::cast<long double>(box.right_extent());
    m_bottom = libnest2d::cast<long double>(box.bottom_extent());
    m_axis   = box.axis();
}

double MinAreaBoundigBox::angle_to_X() const
{
    double ret = std::atan2(m_axis.y(), m_axis.x());
    auto   s   = std::signbit(ret);
    if (s) ret += 2 * PI;
    return -ret;
}

long double MinAreaBoundigBox::width() const
{
    return std::abs(m_bottom) /
           std::sqrt(libnest2d::pl::magnsq<Point, long double>(m_axis));
}

long double MinAreaBoundigBox::height() const
{
    return std::abs(m_right) /
           std::sqrt(libnest2d::pl::magnsq<Point, long double>(m_axis));
}

long double MinAreaBoundigBox::area() const
{
    long double asq = libnest2d::pl::magnsq<Point, long double>(m_axis);
    return m_bottom * m_right / asq;
}

void remove_collinear_points(Polygon &p)
{
    p = libnest2d::removeCollinearPoints<Polygon>(p, Unit(0));
}

void remove_collinear_points(ExPolygon &p)
{
    p = libnest2d::removeCollinearPoints<ExPolygon>(p, Unit(0));
}
} // namespace Slic3r
