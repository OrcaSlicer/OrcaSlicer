#pragma once

// Boost.Polygon traits for Polygon, Polygons, ExPolygon and ExPolygons, for the code that runs
// Boost.Polygon algorithms on them. The point and segment traits stay in Point.hpp and Line.hpp.
// Kept apart because boost/polygon/polygon.hpp is costly to parse in every file.

#include <cstddef>

#include <boost/polygon/polygon.hpp>

#include "ExPolygon.hpp"
#include "Polygon.hpp"

namespace boost { namespace polygon {
    template <>
    struct geometry_concept<Slic3r::Polygon>{ typedef polygon_concept type; };

    template <>
    struct polygon_traits<Slic3r::Polygon> {
        typedef coord_t coordinate_type;
        typedef Slic3r::Points::const_iterator iterator_type;
        typedef Slic3r::Point point_type;

        // Get the begin iterator
        static inline iterator_type begin_points(const Slic3r::Polygon& t) {
            return t.points.begin();
        }

        // Get the end iterator
        static inline iterator_type end_points(const Slic3r::Polygon& t) {
            return t.points.end();
        }

        // Get the number of sides of the polygon
        static inline std::size_t size(const Slic3r::Polygon& t) {
            return t.points.size();
        }

        // Get the winding direction of the polygon
        static inline winding_direction winding(const Slic3r::Polygon& /* t */) {
            return unknown_winding;
        }
    };

    template <>
    struct polygon_mutable_traits<Slic3r::Polygon> {
        // expects stl style iterators
        template <typename iT>
        static inline Slic3r::Polygon& set_points(Slic3r::Polygon& polygon, iT input_begin, iT input_end) {
            polygon.points.clear();
            while (input_begin != input_end) {
                polygon.points.push_back(Slic3r::Point());
                boost::polygon::assign(polygon.points.back(), *input_begin);
                ++input_begin;
            }
            // skip last point since Boost will set last point = first point
            polygon.points.pop_back();
            return polygon;
        }
    };
    
    template <>
    struct geometry_concept<Slic3r::Polygons> { typedef polygon_set_concept type; };

    //next we map to the concept through traits
    template <>
    struct polygon_set_traits<Slic3r::Polygons> {
        typedef coord_t coordinate_type;
        typedef Slic3r::Polygons::const_iterator iterator_type;
        typedef Slic3r::Polygons operator_arg_type;

        static inline iterator_type begin(const Slic3r::Polygons& polygon_set) {
            return polygon_set.begin();
        }

        static inline iterator_type end(const Slic3r::Polygons& polygon_set) {
            return polygon_set.end();
        }

        //don't worry about these, just return false from them
        static inline bool clean(const Slic3r::Polygons& /* polygon_set */) { return false; }
        static inline bool sorted(const Slic3r::Polygons& /* polygon_set */) { return false; }
    };

    template <>
    struct polygon_set_mutable_traits<Slic3r::Polygons> {
        template <typename input_iterator_type>
        static inline void set(Slic3r::Polygons& polygons, input_iterator_type input_begin, input_iterator_type input_end) {
          polygons.assign(input_begin, input_end);
        }
    };
} }

namespace boost { namespace polygon {
    template <>
        struct polygon_traits<Slic3r::ExPolygon> {
        typedef coord_t coordinate_type;
        typedef Slic3r::Points::const_iterator iterator_type;
        typedef Slic3r::Point point_type;

        // Get the begin iterator
        static inline iterator_type begin_points(const Slic3r::ExPolygon& t) {
            return t.contour.points.begin();
        }

        // Get the end iterator
        static inline iterator_type end_points(const Slic3r::ExPolygon& t) {
            return t.contour.points.end();
        }

        // Get the number of sides of the polygon
        static inline std::size_t size(const Slic3r::ExPolygon& t) {
            return t.contour.points.size();
        }

        // Get the winding direction of the polygon
        static inline winding_direction winding(const Slic3r::ExPolygon& /* t */) {
            return unknown_winding;
        }
    };

    template <>
    struct polygon_mutable_traits<Slic3r::ExPolygon> {
        //expects stl style iterators
        template <typename iT>
        static inline Slic3r::ExPolygon& set_points(Slic3r::ExPolygon& expolygon, iT input_begin, iT input_end) {
            expolygon.contour.points.assign(input_begin, input_end);
            // skip last point since Boost will set last point = first point
            expolygon.contour.points.pop_back();
            return expolygon;
        }
    };
    
    
    template <>
    struct geometry_concept<Slic3r::ExPolygon> { typedef polygon_with_holes_concept type; };

    template <>
    struct polygon_with_holes_traits<Slic3r::ExPolygon> {
        typedef Slic3r::Polygons::const_iterator iterator_holes_type;
        typedef Slic3r::Polygon hole_type;
        static inline iterator_holes_type begin_holes(const Slic3r::ExPolygon& t) {
            return t.holes.begin();
        }
        static inline iterator_holes_type end_holes(const Slic3r::ExPolygon& t) {
            return t.holes.end();
        }
        static inline unsigned int size_holes(const Slic3r::ExPolygon& t) {
            return (int)t.holes.size();
        }
    };

    template <>
    struct polygon_with_holes_mutable_traits<Slic3r::ExPolygon> {
         template <typename iT>
         static inline Slic3r::ExPolygon& set_holes(Slic3r::ExPolygon& t, iT inputBegin, iT inputEnd) {
              t.holes.assign(inputBegin, inputEnd);
              return t;
         }
    };
    
    //first we register CPolygonSet as a polygon set
    template <>
    struct geometry_concept<Slic3r::ExPolygons> { typedef polygon_set_concept type; };

    //next we map to the concept through traits
    template <>
    struct polygon_set_traits<Slic3r::ExPolygons> {
        typedef coord_t coordinate_type;
        typedef Slic3r::ExPolygons::const_iterator iterator_type;
        typedef Slic3r::ExPolygons operator_arg_type;

        static inline iterator_type begin(const Slic3r::ExPolygons& polygon_set) {
            return polygon_set.begin();
        }

        static inline iterator_type end(const Slic3r::ExPolygons& polygon_set) {
            return polygon_set.end();
        }

        //don't worry about these, just return false from them
        static inline bool clean(const Slic3r::ExPolygons& /* polygon_set */) { return false; }
        static inline bool sorted(const Slic3r::ExPolygons& /* polygon_set */) { return false; }
    };

    template <>
    struct polygon_set_mutable_traits<Slic3r::ExPolygons> {
        template <typename input_iterator_type>
        static inline void set(Slic3r::ExPolygons& expolygons, input_iterator_type input_begin, input_iterator_type input_end) {
            expolygons.assign(input_begin, input_end);
        }
    };
} }
