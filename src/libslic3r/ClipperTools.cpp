#include <ClipperTools.hpp>

#include <algorithm>
#include <cmath>
#include <vector>
#include <cstdlib>
#include "libslic3r.h"
#include <clipper.hpp>
#include "Point.hpp"
#include "Line.hpp"
#include "Polygon.hpp"
#include "Polyline.hpp"
#include "ExPolygon.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "Arachne/utils/ExtrusionJunction.hpp"

using Lines = std::vector<Slic3r::Line>;
using ThickLine = std::vector<Slic3r::ThickLines>;
using Polylines = std::vector<Slic3r::Polyline>;
using ThickPolylines = std::vector<Slic3r::ThickPolyline>;
using Line = Slic3r::Line;
using ThickPolyline = Slic3r::ThickPolyline;

namespace Slic3r {

double constrain2PI(double x)
{
    while (x <= 0.)
        x += M_2PI;
    return fmod(x, M_2PI);
};

double constrainPI(double x)
{
    while (x <= 0.)
        x += M_2PI;
    return fmod(x + M_PI, M_2PI) - M_PI;
};

// ---------------------------------------------------------------------------
// ****** Polylines Section ******
// ---------------------------------------------------------------------------

Polyline to_polyline(const Lines& in)
{
    Polyline out;
    out.points.reserve(in.size() * 2); // get max capacity
    if (in.size()) {
        out.points.push_back(in.front().a);
        for (const Line& l : in) {
            if (out.points.back() != l.a)
                out.points.push_back(l.a);
            out.points.push_back(l.b);
        }
    }
    return out;
};

Polyline to_polyline(const ThickLines& in)
{
    Polyline out;
    out.points.reserve(in.size() * 2); // get max capacity
    if (in.size()) {
        out.points.emplace_back(in.front().a);
        for (const ThickLine& l : in) {
            if (out.points.back() != l.a)
                out.points.push_back(l.a);
            out.points.push_back(l.b);
        }
    }
    return out;
};

Polyline to_polyline(const Arachne::ExtrusionJunctions& in, bool close)
{
    Polyline out;
    coord_t s = in.size();
    if (s > 1) {
        out.points.reserve(s + close);
        for (coord_t i = 0; i < s;)
            out.points.push_back(in[i++].p);
        if (close && (out[0] != out.back()))
            out.points.push_back(out[0]);
    }
    return out;
};

Polyline to_polyline_reverse(const Arachne::ExtrusionJunctions& in, bool close)
{
    Polyline out;
    coord_t s = in.size();
    if (s > 1) {
        out.points.reserve(s + close);
        while (s)
            out.points.push_back(in[--s].p);
        if (close && (out[0] != out.back()))
            out.points.push_back(out[0]);
    }
    return out;
};

// ---------------------------------------------------------------------------
// ****** Thick Polylines Section ******
// ---------------------------------------------------------------------------

ThickPolyline to_thick_polyline(const ThickLines& in)
{
    ThickPolyline out;
    out.points.reserve(in.size() * 2); // get max capacity
    if (in.size()) {
        for (const ThickLine& tl : in) {
            if (!out.size() || (out.points.back() != tl.a) || (out.width.back() != tl.a_width)) {
                out.points.push_back(tl.a);
                out.width.push_back(tl.a_width);
                out.width.push_back(tl.a_width);
            }
            out.points.push_back(tl.b);
            out.width.push_back(tl.b_width);
            out.width.push_back(tl.b_width);
        }
    }
    return out;
};

ThickPolyline to_thick_polyline(const Polyline3& in, const double z_factor, bool close)
{
    ThickPolyline out;
    out.points.reserve(in.size());
    out.width.reserve(in.size() * 2);
    out.points.push_back(Point(in.points[0][0], in.points[0][1]));
    out.width.push_back(in.points[0][2] / z_factor);
    for (size_t i = 1; i < in.size(); i++) {
        out.points.push_back(Point(in.points[i][0], in.points[i][1]));
        out.width.push_back(in.points[i][2] / z_factor);
        out.width.push_back(in.points[i][2] / z_factor);
    }
    out.width.push_back(in.points.back()[2]);
    if (close && (out.points[0] != out.points.back())) {
        out.points.push_back(out.points[0]);
        out.width.push_back(out.width.back());
        out.width.push_back(out.width[0]);
    }
    return out;
};

ThickPolylines to_thick_polylines(const ThickLines& in, bool separate_lines)
{
    ThickPolylines out;
    ThickLines tls;
    tls.reserve(in.size());
    for (auto& tl : in) {
        if (separate_lines) {
            bool null_line = !tl.a_width && !tl.b_width;
            if (tls.size() && (tl.a != tls.back().b || null_line)) {
                if (tls.size())
                    out.push_back(to_thick_polyline(tls));
                tls.clear();
            }
            if (!null_line)
                tls.push_back(tl);
        } else
            tls.push_back(tl);
    }
    if (tls.size())
        out.push_back(to_thick_polyline(tls));
    return out;
};

ThickPolyline to_thick_polyline(const Arachne::ExtrusionJunctions& in, bool close)
{
    Slic3r::ThickPolyline out;
    coord_t s = in.size();
    if (s > 1) {
        out.width.reserve((s + close) * 2);
        out.points.reserve(s + close);
        for (coord_t i = 0; i < s;) {
            const Arachne::ExtrusionJunction& j(in[i++]);
            out.points.push_back(j.p);
            out.width.push_back(j.w);
            out.width.push_back(j.w);
        }
        if (close) {
            out.points.push_back(out.points[0]);
            out.width.push_back(out.width[0]);
            out.width.push_back(out.width[0]);
        }
    }
    return out;
};

ThickPolyline to_thick_polyline_reverse(const Arachne::ExtrusionJunctions& in, bool close)
{
    Slic3r::ThickPolyline out;
    coord_t s = in.size();
    if (s > 1) {
        out.width.reserve((s + close) * 2);
        out.points.reserve(s + close);
        while (s) {
            const Arachne::ExtrusionJunction& j(in[--s]);
            out.points.push_back(j.p);
            out.width.push_back(j.w);
            out.width.push_back(j.w);
        }
        if (close) {
            out.points.push_back(out.points[0]);
            out.width.push_back(out.width[0]);
            out.width.push_back(out.width[0]);
        }
    }
    return out;
};

// ---------------------------------------------------------------------------
// ****** Thick Lines Section ******
// ---------------------------------------------------------------------------
//


ThickLines to_thick_lines(const Polyline& in, coordf_t width, bool close)
{
    ThickLines out;
    for (Line l : in.lines())
        out.emplace_back(l.a, l.b, width, width);
    if (close && (out.back().b != out[0].a))
        out.push_back(ThickLine(out.back().b, out[0].a, out.back().b_width, out[0].a_width));
    return out;
};

ThickLines to_thick_lines(const Polylines& in, coordf_t width, bool close)
{
    ThickLines out;
    for (const Polyline& pl : in)
        for (ThickLine& tl : to_thick_lines(pl, width, close))
            out.push_back(tl);
    return out;
};

ThickLines to_thick_lines(const Polyline3& in, const double z_factor, bool close, bool separate_lines)
{
    size_t size(in.size());
    ThickLines out;
    out.reserve(size-- * 2);
    for (size_t i = 0; i < size;) {
        const Point3& a(in.points[i]);
        const Point3& b(in.points[++i]);
        out.push_back(ThickLine(Point(a[0], a[1]), Point(b[0], b[1]), a[2] / z_factor, b[2] / z_factor));
    }
    if (close && (out[0].a != out.back().b))
        out.push_back(ThickLine(out.back().b, out[0].a, out.back().b_width, out[0].a_width));
    return out;
};

ThickLines to_thick_lines(const Arachne::ExtrusionJunctions& in, bool close)
{
    ThickLines out;
    coord_t s = in.size();
    if (s > 1) {
        out.reserve(--s + close);
        for (coord_t i = 0; i < s;) {
            const Arachne::ExtrusionJunction& p0(in[i]);
            const Arachne::ExtrusionJunction& p1(in[++i]);
            out.push_back(ThickLine(p0.p, p1.p, p0.w, p1.w));
        }
        if (close && (out.back().b != out[0].a))
            out.push_back(ThickLine(out.back().b, out[0].a, out.back().b_width, out[0].a_width));
    }
    return out;
};

ThickLines to_thick_lines_reverse(const Arachne::ExtrusionJunctions& in, bool close)
{
    ThickLines out;
    coord_t s = in.size();
    if (s > 1) {
        out.reserve(s-- + close);
        while (s) {
            const Arachne::ExtrusionJunction& p0(in[s]);
            const Arachne::ExtrusionJunction& p1(in[--s]);
            out.push_back(ThickLine(p0.p, p1.p, p0.w, p1.w));
        }
        if (close)
            out.push_back(ThickLine(out.back().b, out[0].a, out.back().b_width, out[0].a_width));
    }
    return out;
};

// ---------------------------------------------------------------------------
// ****** 3D Lines Section ******
// ---------------------------------------------------------------------------

Polyline3 to_polyline3(const ThickLines& in, const double z_factor, bool separate_lines)
{
    Polyline3 out;
    out.points.reserve(in.size() * 2); // get max capacity
    for (const ThickLine& tl : in) {
        Point3 a(tl.a, tl.a_width * z_factor);
        Point3 b(out.points.back());
        if (!out.points.size() || !b[2])
            out.points.push_back(a);
        else if (b != a) {
            if (separate_lines && (b[0] != a[0] || b[1] != a[1])) {
                out.points.push_back(Point3({b[0], b[1]}, 0));
                out.points.push_back(Point3(tl.a, 0));
            } else
                out.points.push_back(a);
        }
        out.points.push_back(Point3(tl.b, tl.b_width * z_factor));
    }
    return out;
};

Polyline3 to_polyline3(const ThickPolyline& in, const double z_factor)
{
    Polyline3 out;
    out.points.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        size_t i2 = i * 2;
        double width(in.width[i2]);
        if (i) {
            double width2(in.width[i2 - 1]);
            if (width2 != width)
                out.points.push_back(Point3(in.points[i - 1], width2 * z_factor));
        }
        out.points.push_back(Point3(in.points[i], width * z_factor));
    }
    return out;
};

// ---------------------------------------------------------------------------
// ****** Extrusion Lines Section ******
// ---------------------------------------------------------------------------

Arachne::ExtrusionJunctions to_extrusion(const Polyline& in, size_t width, size_t index, bool reverse_lines)
{
    Arachne::ExtrusionJunctions out;
    out.reserve(in.points.size());
    for (const Point& p : in.points) {
        out.push_back(Arachne::ExtrusionJunction(p, width, index));
    }
    if (reverse_lines)
        std::reverse(out.begin(), out.end());
    return out;
};

Arachne::ExtrusionJunctions to_extrusion(const Polylines& in, size_t width, size_t index, bool reverse_lines, bool separate_lines)
{
    Arachne::ExtrusionJunctions out;
    for (const Polyline& pl : in) {
        if (separate_lines && out.size()) { // Setting the extrusion movement conditions without printing to display an additional polyline if they appear
            out.push_back(Arachne::ExtrusionJunction(out.back().p, 1, index)); // use minimal extrusion line for separators
            out.push_back(Arachne::ExtrusionJunction(pl.points.front(), 1, index));
        }
        for (const Arachne::ExtrusionJunction& ej : to_extrusion(pl, width, index, false))
            out.push_back(ej);
    }
    if (reverse_lines)
        std::reverse(out.begin(), out.end());
    return out;
};

Arachne::ExtrusionJunctions to_extrusion(const ThickLines& in, size_t index, bool reverse_lines, bool separate_lines)
{
    Arachne::ExtrusionJunctions out;
    if (in.size()) {
        out.reserve(in.size() * 2);
        // if (reverse_lines) reverse(in); // first variant
        for (size_t i = 0; i < in.size(); i++) {
            ThickLine tl = in[i];
            if (out.size() && separate_lines && (out.back().p != tl.a || !tl.a_width)) {
                out.push_back(Arachne::ExtrusionJunction(out.back().p, bool(tl.a_width), index));
                out.push_back(Arachne::ExtrusionJunction(tl.a, bool(tl.a_width), index)); // use minimal extrusion line for separators
                out.push_back(Arachne::ExtrusionJunction(tl.a, tl.a_width, index));
            } else if (!out.size() || out.back().w != tl.a_width) {
                out.push_back(Arachne::ExtrusionJunction(tl.a, tl.a_width, index));
            }
            out.push_back(Arachne::ExtrusionJunction(tl.b, tl.b_width, index));
        }
        if (reverse_lines) // second variant
            std::reverse(out.begin(), out.end());
    }
    return out;
};

Arachne::ExtrusionJunctions to_extrusion(const ThickPolyline& in, size_t index)
{
    Arachne::ExtrusionJunctions out;
    out.reserve(in.points.size());
    for (size_t i = 0; i < in.size(); i++) {
        if (i && in.width[i * 2] != in.width[i * 2 - 1])
            out.push_back(Arachne::ExtrusionJunction(in.points[i], in.width[i * 2 - 1], index));
        out.push_back(Arachne::ExtrusionJunction(in.points[i], in.width[i * 2], index));
    }
    return out;
};

Arachne::ExtrusionJunctions to_extrusion(const ThickPolylines& in, size_t index, bool separate_lines)
{
    Arachne::ExtrusionJunctions out;
    for (const ThickPolyline& tpl : in) {
        if (separate_lines &&
            out.size()) { // Setting the extrusion movement conditions without printing to display an additional polyline if they appear
            out.push_back(Arachne::ExtrusionJunction(out.back().p, 1, index));
            out.push_back(Arachne::ExtrusionJunction(tpl.points.front(), 1, index));
        }
        for (const Arachne::ExtrusionJunction& ej : to_extrusion(tpl, index))
            out.emplace_back(ej);
    }
    return out;
};

// ---------------------------------------------------------------------------
// ****** Geometry Direction Section ******
// ---------------------------------------------------------------------------

ThickLines reverse(const ThickLines& in)
{
    ThickLines out(in);
    for (ThickLine& tl : out)
        tl.reverse();
    std::reverse(out.begin(), out.end());
    return out;
};

bool is_clockwise(const Polyline& in)
{
    Polygon out(in.points);
    return out.is_clockwise();
};

bool is_counter_clockwise(const Polyline& in)
{
    Polygon out(in.points);
    return out.is_counter_clockwise();
};

bool is_clockwise(const ThickLines& in) { return is_clockwise(to_polyline(in)); }

bool is_counter_clockwise(const ThickLines& in) { return is_counter_clockwise(to_polyline(in)); }

Polyline make_counter_clockwise(const Polyline& in)
{
    Polygon pg(in.points);
    pg.make_counter_clockwise();
    return Polyline(pg.points);
};

Polyline make_clockwise(const Polyline& in)
{
    Polygon pg(in.points);
    pg.make_clockwise();
    return Polyline(pg.points);
};

Polylines make_counter_clockwise(const Polylines& in)
{
    Polylines out;
    for (Polyline pl : in)
        out.push_back(make_counter_clockwise(pl));
    return out;
};

Polylines make_clockwise(const Polylines& in)
{
    Polylines out;
    for (Polyline pl : in)
        out.push_back(make_clockwise(pl));
    return out;
};

// ---------------------------------------------------------------------------
// ****** Geometry Choose Section ******
// ---------------------------------------------------------------------------

Polyline get_longest(const Polylines& in, bool invert)
{
    Polyline out;
    for (auto& pl : in)
        if (!out.size() || (is_clockwise(pl) != (invert && out.length() < pl.length())))
            out = pl;
    return out;
};

ThickPolyline get_longest(const ThickPolylines& in, bool invert)
{
    ThickPolyline out;
    for (auto& tpl : in)
        if (!out.size() || (is_clockwise(tpl) != (invert && out.length() < tpl.length())))
            out = tpl;
    return out;
};

Polylines get_clockwise(const Polylines& in)
{
    Polylines out;
    for (auto& pl : in)
        if (is_clockwise(pl))
            out.push_back(pl);
    return out;
};

Polylines get_counter_clockwise(const Polylines& in)
{
    Polylines out;
    for (auto& pl : in)
        if (is_counter_clockwise(pl))
            out.push_back(pl);
    return out;
};

// ---------------------------------------------------------------------------
// ****** Geometry Simplify Section ******
// ---------------------------------------------------------------------------
// 
// Simplifying thic (poly)lines using the standard 3-dimensional polyline simplification procedure.
// You can use the 'width_factor' parameter to filter points by width more accurately.

ThickLines simplify(const ThickLines& in, double tolerance, const double width_factor)
{
    const bool closed(in[0].a == in.back().b);
    Polyline3 out(to_polyline3(in, width_factor)); // convert thickpolyline into 3D-polyline
    out.simplify(tolerance);
    return to_thick_lines(out, width_factor, closed);
};

ThickPolyline simplify(const ThickPolyline& in, double tolerance, const double width_factor)
{
    const bool closed(in.points[0] == in.points.back());
    Polyline3 out(to_polyline3(in, width_factor)); // convert thickpolyline into 3D-polyline
    out.simplify(tolerance);
    return to_thick_polyline(out, width_factor, closed);
};

ThickPolylines simplify(const ThickPolylines& in, double tolerance, const double width_factor)
{
    ThickPolylines out;
    for (ThickPolyline thickpolyline : in) {
        ThickPolyline tpl(simplify(thickpolyline, tolerance, width_factor));
        if (tpl.size())
            out.emplace_back(tpl);
    }
    return out;
};

// ---------------------------------------------------------------------------
// ****** Geometry Checking Section ******
// ---------------------------------------------------------------------------

bool contour_intersection(Line line, Polylines polylines, Point* intersection, bool from_back)
{
    if (from_back) {
        line.reverse();
        std::reverse(polylines.begin(), polylines.end());
    }
    for (Polyline pl : polylines) {
        if (from_back)
            pl.reverse();
        if (pl.intersection(line, intersection))
            return true;
    }
    return false;
};

bool contour_contains(Polylines& polylines, Point& point)
{
    for (Polyline& pl : polylines) {
        Polygon pg(pl.points);
        pg.make_counter_clockwise();
        if (pg.contains(point))
            return true;
    }
    return false;
};

// ---------------------------------------------------------------------------
// ****** Geometry Filtering Section ******
// ---------------------------------------------------------------------------

ThickLines filter_curls_reject(ThickLines in, double curl_length)
{
    const size_t size = in.size();
    if (size < 3 || !curl_length)
        return in;
    ThickLines out;
    out.reserve(size);
    const bool closed(in[0].a == in.back().b);

    for (size_t i = 0; i < size;) {
        ThickLine i_line(in[i]);
        size_t j(++i);
        if (j < size) {
            ThickLine j_line(in[j++]); // the next line is not compared, but takes into account the length
            double lenght(curl_length > 0 ? j_line.length() : 0.);
            size_t last_line(0);
            Point p;
            Point* const point(&p);
            while (closed ? lenght < abs(curl_length) : j < size) {
                j_line = in[j % size];
                if (i_line.intersection(j_line, point))
                    last_line = j;
                lenght += j_line.length(); // measure next line
                j++;
            }
            if (last_line) {
                ThickLine& k_line(in[last_line % size]);
                i_line.b       = p;
                k_line.a       = p;
                i_line.b_width = i_line.a_width + (i_line.b_width - i_line.a_width) * (i_line.b - i_line.a).norm() / i_line.length();
                k_line.a_width = k_line.b_width + (k_line.a_width - k_line.b_width) * (k_line.a - k_line.b).norm() / k_line.length();
                i              = last_line;
            }
        }
        out.push_back(i_line);
    }
    // bypass results
    if (out.size() < 2)
        return in;

    // close the lines
    if (closed && (out[0].a != out.back().b))
        out.emplace_back(out.back().b, out[0].a, out.back().b_width, out[0].a_width);
    return out;
};

Lines filter_curls_reject(Lines in, double curl_length)
{
    const size_t size = in.size();
    if (size < 3 || !curl_length)
        return in;
    Lines out;
    out.reserve(size);
    const bool closed(in[0].a == in.back().b);

    for (size_t i = 0; i < size;) {
        Line i_line(in[i]);
        size_t j(++i);
        if (j < size) {
            Line j_line(in[j++]); // the next line is not compared, but takes into account the length
            double lenght(curl_length > 0 ? j_line.length() : 0.);
            size_t last_line(0);
            Point p;
            Point* const point(&p);
            while (closed ? lenght < abs(curl_length) : j < size) {
                j_line = in[j % size];
                if (i_line.intersection(j_line, point))
                    last_line = j;
                lenght += j_line.length(); // measure next line
                j++;
            }
            if (last_line) {
                Line& k_line(in[last_line % size]);
                i_line.b = p;
                k_line.a = p;
                i        = last_line;
            }
        }
        out.push_back(i_line);
    }

    // bypass results
    if (out.size() < 2)
        return in;

    // close the lines
    if (closed && (out[0].a != out.back().b))
        out.emplace_back(out.back().b, out[0].a);
    return out;
};

Polyline filter_curls_reject(const Polyline& in, double curl_length)
    { return to_polyline(filter_curls_reject(in.lines(), curl_length)); }

Polylines filter_contour(Polyline in,  Polygon crop_contour, const double filter_offset, const double filter_perimeter, const double filter_area, const double tolerance)
{
    if (in.points.size() < 2) // return if negative conditions
        return {in};

    const bool closed(in[0] == in.back());

    Polyline in2(in); // simplify contour
    in2.simplify(tolerance);

    if (!in2.size()) // bypass simplify filter
        in2 = in;
    else
        in = in2;

    bool dir(is_counter_clockwise(in2)); // store direction
    if (!dir)
        in2.reverse();

    if (in2.points.front() != in2.points.back()) // close the contour
        in2.points.push_back(Point(in2.points.front()));
    in = in2;

    if (!in2.size()) // bypass offset filter
        in2 = in;
    else
        in = in2;

    Polygons pgs; // reserve for bypass
    Polygons pgs2(to_polygons({in2}));

    if (filter_offset) { // offset filter
         ExPolygons expgs(union_ex(pgs2));
         pgs = offset2(expgs, filter_offset, -filter_offset);
    }

    if (crop_contour.points.size()) { // contour filter
        crop_contour.make_counter_clockwise();
        pgs = dir ? diff(pgs2, {crop_contour}) : intersection(pgs2, crop_contour);
    }

    if (pgs.size()) // bypass contour filter
        pgs2 = pgs;

    Polylines out; // limits filter
    for (Polygon& pg : simplify_polygons(union_(pgs2))) {
        if (abs(pg.area()) > filter_area && pg.length() > filter_perimeter) {
            if (closed && (pg.points.front() != pg.points.back()))
                pg.points.push_back(pg.front());
            if (!dir)
                pg.reverse();
            out.push_back(Polyline(pg.points));
        }
    }
    return out.size() ? out : to_polylines(pgs2); // bypass out for negative conditions
};

ThickLines filter_by_crop_contour(const ThickLines& in, Polylines contour, double curl_length)
{
    size_t size(in.size());
    if (size < 2 || !contour.size())
        return in;

    bool dir(is_counter_clockwise(contour[0]));
    make_counter_clockwise(contour);
    ThickLines out;
    out.reserve(size);

    for (ThickLine& tl : filter_curls_reject(in, curl_length)) {
        bool _ais(contour_contains(contour, tl.a) != dir);
        bool _bis(contour_contains(contour, tl.b) != dir);

        if (_ais)
            if (_bis)
                out.emplace_back(tl);
            else {
                Point p = tl.b;
                Point* const point(&p);
                if (contour_intersection(tl, contour, point, true))
                    out.emplace_back(tl.a, p, tl.a_width, tl.a_width + (tl.b_width - tl.a_width) * (tl.b - tl.a).norm() / tl.length());
                else
                    out.emplace_back(tl);
            }
        else if (_bis) {
            Point p = tl.a;
            Point* const point(&p);
            if (contour_intersection(tl, contour, point))
                out.emplace_back(p, tl.b, tl.b_width + (tl.a_width - tl.b_width) * (tl.a - tl.b).norm() / tl.length(), tl.b_width);
            else
                out.emplace_back(tl);
        }
    }

    // bypass intersection filter
    if (!out.size())
        return in;

    return out;
};

ThickPolylines filter_by_crop_contour(const ThickPolyline& in, Polylines contour, double curl_length)
{
    if (in.size() < 2)
        return {in};
    return simplify(to_thick_polylines(filter_by_crop_contour(in.thicklines(), contour, curl_length)), SCALED_EPSILON);
};

Polyline filter_by_polygon(Polyline in, const double filter_offset)
{
    if (!filter_offset) // offset filter
        return in;
    
    const bool closed(in[0] == in.back());
    bool dir(is_counter_clockwise(in)); // store direction
    if (!dir)
        in.reverse();
    
    Polyline out; // limits filter
    for (Polygon& pg : union_(offset2(to_expolygons({Polygon(in.points)}), filter_offset, -filter_offset))) {
        if (out.length() < pg.length()) {
            if (dir != pg.is_counter_clockwise())
                pg.reverse();
            out.points = pg.points;
        }
    }
    if (!out.length())
        return in; // bypass out for negative conditions
    if (closed && out.points[0] != out.points.back())
        out.points.push_back(out.points[0]);
    return out;
};

// ---------------------------------------------------------------------------
// ****** Geometry Offset Section ******
// ---------------------------------------------------------------------------

Polyline offset_(const Polyline& in, const double delta, double curl_length)
{
    if (in.points.size() < 2)
        return in;
    Polyline out;
    for (Line& l : in.lines()) {
        if (l.length()) {
            Vec2d dir{l.b(1) - l.a(1), -(l.b(0) - l.a(0))};
            Point v((dir * delta / l.length()).cast<coord_t>());
            out.points.push_back(l.a + v);
            out.points.push_back(l.b + v);
        }
    }
    out.simplify(SCALED_EPSILON);
    return filter_curls_reject(out, curl_length);
};

Polylines offset_(const Polylines& in, const double delta, double curl_length)
{
    Polylines out;
    for (const Polyline& pl : in) {
        if (pl.size())
            out.push_back(offset_(pl, delta, curl_length));
    }
    return out;
};

Polyline offset_by_polygon(const Polyline& in, const double delta, ClipperLib::JoinType joinType)
{
    if (in.points.size() < 2)
        return in;
    Polyline out;
    bool dir(is_counter_clockwise(in));
    Polygon pg(in.points);
    Polygons pgs(union_(offset(pg, delta, joinType)));
    if (!pgs.size())
        return in;
    for (Polygon& pg2 : pgs)
        if (out.length() < pg2.length()) { // choose longest
            dir ? pg2.make_counter_clockwise() : pg2.make_clockwise();
            out.points = pg2.points;
        }
    out.simplify(SCALED_EPSILON);
    if (out.points.size() && in.is_closed() && !out.is_closed())
        out.points.push_back(out.points.front());
    return out;
};

Polylines offset_by_polygon(const Polylines& in, const double delta, ClipperLib::JoinType joinType)
{
    Polylines out;
    for (const Polyline& pl : in) {
        if (pl.size())
            out.push_back(offset_by_polygon(pl, delta, joinType));
    }
    return out;
};

// ---------------------------------------------------------------------------
// ****** Geometry Contour Section ******
// ---------------------------------------------------------------------------

Polyline combine(Polyline polyline1, Polyline polyline2)
{
    for (Point& p : polyline2)
        polyline1.points.push_back(p);
    return polyline1;
};

Polylines combine_3(const Polylines& polylines1, Polylines polylines2, Polylines polylines3)
{
    Polylines out;
    for (auto pl : polylines1) {
        bool dir(is_counter_clockwise(pl));
        Polygons pgs(to_polygons({pl}));
        if (dir) {
            if (polylines3.size())
                pgs = union_(pgs, to_polygons(polylines3));
            if (polylines2.size())
                pgs = intersection(pgs, to_polygons(polylines2));
        } else {
            if (polylines2.size())
                pgs = union_(pgs, to_polygons(polylines2));
            if (polylines3.size())
                pgs = intersection(pgs, to_polygons(polylines3));
        }
        for (auto pg : pgs) {
            if (pg.points.front() != pg.points.back())
                pg.points.push_back(pg.points.front());
            dir ? pg.make_counter_clockwise() : pg.make_clockwise();
            out.push_back(Polyline(pg.points));
        }
    }
    return out;
};

Polylines combine_2(const Polylines& polylines1, Polylines& polylines2)
{
    Polylines out;
    for (Polyline pl : polylines1) {
        bool dir(is_clockwise(pl));
        Polygons pgs(to_polygons({pl.points}));
        Polygons cpgs(to_polygons(polylines2));
        if (!dir) {
            pgs = union_(pgs, cpgs);
            pgs = intersection(pgs, cpgs);
        } else {
            pgs = union_(pgs, cpgs);
            pgs = intersection(pgs, cpgs);
        }
        for (auto pg : pgs) {
            if (pg.points.front() != pg.points.back())
                pg.points.push_back(pg.points.front());
            dir ? pg.make_clockwise() : pg.make_counter_clockwise();
            out.push_back(Polyline(pg.points));
        }
    }
    return out;
};

} // namespace Slic3r