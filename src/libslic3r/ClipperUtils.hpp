#ifndef slic3r_ClipperUtils_hpp_
#define slic3r_ClipperUtils_hpp_

#include "libslic3r.h"
#include "clipper.hpp"
#include "ExPolygon.hpp"
#include "Polygon.hpp"
#include "Surface.hpp"

// import these wherever we're included
using Slic3r::ClipperLib::jtMiter;
using Slic3r::ClipperLib::jtRound;
using Slic3r::ClipperLib::jtSquare;

namespace Slic3r {

static constexpr const float                        ClipperSafetyOffset     = 10.f;

static constexpr const Slic3r::ClipperLib::JoinType DefaultJoinType         = Slic3r::ClipperLib::jtMiter;

static constexpr const Slic3r::ClipperLib::EndType DefaultEndType           = Slic3r::ClipperLib::etOpenButt;

//FIXME evaluate the default miter limit. 3 seems to be extreme, Cura uses 1.2.
// Mitter Limit 3 is useful for perimeter generator, where sharp corners are extruded without needing a gap fill.
// However such a high limit causes issues with large positive or negative offsets, where a sharp corner
// is extended excessively.
static constexpr const double                       DefaultMiterLimit       = 3.;

static constexpr const Slic3r::ClipperLib::JoinType DefaultLineJoinType     = Slic3r::ClipperLib::jtSquare;
// Miter limit is ignored for jtSquare.
static constexpr const double                       DefaultLineMiterLimit   = 0.;

// Decimation factor applied on input contour when doing offset, multiplied by the offset distance.
static constexpr const double                       ClipperOffsetShortestEdgeFactor = 0.005;

enum class ApplySafetyOffset {
    No,
    Yes
};

namespace ClipperUtils {
    class PathsProviderIteratorBase {
    public:
        using value_type        = Points;
        using difference_type   = std::ptrdiff_t;
        using pointer           = const Points*;
        using reference         = const Points&;
        using iterator_category = std::input_iterator_tag;
    };

    class EmptyPathsProvider {
    public:
        struct iterator : public PathsProviderIteratorBase {
        public:
            const Points& operator*() { assert(false); return s_empty_points; }
            // all iterators point to end.
            constexpr bool operator==(const iterator &rhs) const { return true; }
            constexpr bool operator!=(const iterator &rhs) const { return false; }
            const Points& operator++(int) { assert(false); return s_empty_points; }
            const iterator& operator++() { assert(false); return *this; }
        };

        constexpr EmptyPathsProvider() {}
        static constexpr iterator cend()   throw() { return iterator{}; }
        static constexpr iterator end()    throw() { return cend(); }
        static constexpr iterator cbegin() throw() { return cend(); }
        static constexpr iterator begin()  throw() { return cend(); }
        static constexpr size_t   size()   throw() { return 0; }

        static Points s_empty_points;
    };

    class SinglePathProvider {
    public:
        SinglePathProvider(const Points &points) : m_points(points) {}

        struct iterator : public PathsProviderIteratorBase {
        public:
            explicit iterator(const Points &points) : m_ptr(&points) {}
            const Points& operator*() const { return *m_ptr; }
            bool operator==(const iterator &rhs) const { return m_ptr == rhs.m_ptr; }
            bool operator!=(const iterator &rhs) const { return !(*this == rhs); }
            const Points& operator++(int) { auto out = m_ptr; m_ptr = &s_end; return *out; }
            iterator& operator++() { m_ptr = &s_end; return *this; }
        private:
            const Points *m_ptr;
        };

        iterator cbegin() const { return iterator(m_points); }
        iterator begin()  const { return this->cbegin(); }
        iterator cend()   const { return iterator(s_end); }
        iterator end()    const { return this->cend(); }
        size_t   size()   const { return 1; }

    private:
        const  Points &m_points;
        static Points  s_end;
    };

    template<typename PathType>
    class PathsProvider {
    public:
        PathsProvider(const std::vector<PathType> &paths) : m_paths(paths) {}

        struct iterator : public PathsProviderIteratorBase {
        public:
            explicit iterator(typename std::vector<PathType>::const_iterator it) : m_it(it) {}
            const Points& operator*() const { return *m_it; }
            bool operator==(const iterator &rhs) const { return m_it == rhs.m_it; }
            bool operator!=(const iterator &rhs) const { return !(*this == rhs); }
            const Points& operator++(int) { return *(m_it ++); }
            iterator& operator++() { ++ m_it; return *this; }
        private:
            typename std::vector<PathType>::const_iterator m_it;
        };

        iterator cbegin() const { return iterator(m_paths.begin()); }
        iterator begin()  const { return this->cbegin(); }
        iterator cend()   const { return iterator(m_paths.end()); }
        iterator end()    const { return this->cend(); }
        size_t   size()   const { return m_paths.size(); }

    private:
        const std::vector<PathType> &m_paths;
    };

    template<typename MultiPointsType>
    class MultiPointsProvider {
    public:
        MultiPointsProvider(const MultiPointsType &multipoints) : m_multipoints(multipoints) {}

        struct iterator : public PathsProviderIteratorBase {
        public:
            explicit iterator(typename MultiPointsType::const_iterator it) : m_it(it) {}
            const Points& operator*() const { return m_it->points; }
            bool operator==(const iterator &rhs) const { return m_it == rhs.m_it; }
            bool operator!=(const iterator &rhs) const { return !(*this == rhs); }
            const Points& operator++(int) { return (m_it ++)->points; }
            iterator& operator++() { ++ m_it; return *this; }
        private:
            typename MultiPointsType::const_iterator m_it;
        };

        iterator cbegin() const { return iterator(m_multipoints.begin()); }
        iterator begin()  const { return this->cbegin(); }
        iterator cend()   const { return iterator(m_multipoints.end()); }
        iterator end()    const { return this->cend(); }
        size_t   size()   const { return m_multipoints.size(); }

    private:
        const MultiPointsType &m_multipoints;
    };

    using PolygonsProvider  = MultiPointsProvider<Polygons>;
    using PolylinesProvider = MultiPointsProvider<Polylines>;

    struct ExPolygonProvider {
        ExPolygonProvider(const ExPolygon &expoly) : m_expoly(expoly) {}

        struct iterator : public PathsProviderIteratorBase {
        public:
            explicit iterator(const ExPolygon &expoly, int idx) : m_expoly(expoly), m_idx(idx) {}
            const Points& operator*() const { return (m_idx == 0) ? m_expoly.contour.points : m_expoly.holes[m_idx - 1].points; }
            bool operator==(const iterator &rhs) const { assert(m_expoly == rhs.m_expoly); return m_idx == rhs.m_idx; }
            bool operator!=(const iterator &rhs) const { return !(*this == rhs); }
            const Points& operator++(int) { const Points &out = **this; ++ m_idx; return out; }
            iterator& operator++() { ++ m_idx; return *this; }
        private:
            const ExPolygon &m_expoly;
            int              m_idx;
        };

        iterator cbegin() const { return iterator(m_expoly, 0); }
        iterator begin()  const { return this->cbegin(); }
        iterator cend()   const { return iterator(m_expoly, m_expoly.holes.size() + 1); }
        iterator end()    const { return this->cend(); }
        size_t   size()   const { return m_expoly.holes.size() + 1; }

    private:
        const ExPolygon &m_expoly;
    };

    struct ExPolygonsProvider {
        ExPolygonsProvider(const ExPolygons &expolygons) : m_expolygons(expolygons) {
            m_size = 0;
            for (const ExPolygon &expoly : expolygons)
                m_size += expoly.holes.size() + 1;
        }

        struct iterator : public PathsProviderIteratorBase {
        public:
            explicit iterator(ExPolygons::const_iterator it) : m_it_expolygon(it), m_idx_contour(0) {}
            const Points& operator*() const { return (m_idx_contour == 0) ? m_it_expolygon->contour.points : m_it_expolygon->holes[m_idx_contour - 1].points; }
            bool operator==(const iterator &rhs) const { return m_it_expolygon == rhs.m_it_expolygon && m_idx_contour == rhs.m_idx_contour; }
            bool operator!=(const iterator &rhs) const { return !(*this == rhs); }
            iterator& operator++() { 
                if (++ m_idx_contour == m_it_expolygon->holes.size() + 1) {
                    ++ m_it_expolygon;
                    m_idx_contour = 0;
                }
                return *this;
            }
            const Points& operator++(int) { 
                const Points &out = **this;
                ++ (*this);
                return out;
            }
        private:
            ExPolygons::const_iterator  m_it_expolygon;
            size_t                      m_idx_contour;
        };

        iterator cbegin() const { return iterator(m_expolygons.cbegin()); }
        iterator begin()  const { return this->cbegin(); }
        iterator cend()   const { return iterator(m_expolygons.cend()); }
        iterator end()    const { return this->cend(); }
        size_t   size()   const { return m_size; }

    private:
        const ExPolygons &m_expolygons;
        size_t            m_size;
    };

    struct SurfacesProvider {
        SurfacesProvider(const Surfaces &surfaces) : m_surfaces(surfaces) {
            m_size = 0;
            for (const Surface &surface : surfaces)
                m_size += surface.expolygon.holes.size() + 1;
        }

        struct iterator : public PathsProviderIteratorBase {
        public:
            explicit iterator(Surfaces::const_iterator it) : m_it_surface(it), m_idx_contour(0) {}
            const Points& operator*() const { return (m_idx_contour == 0) ? m_it_surface->expolygon.contour.points : m_it_surface->expolygon.holes[m_idx_contour - 1].points; }
            bool operator==(const iterator &rhs) const { return m_it_surface == rhs.m_it_surface && m_idx_contour == rhs.m_idx_contour; }
            bool operator!=(const iterator &rhs) const { return !(*this == rhs); }
            iterator& operator++() { 
                if (++ m_idx_contour == m_it_surface->expolygon.holes.size() + 1) {
                    ++ m_it_surface;
                    m_idx_contour = 0;
                }
                return *this;
            }
            const Points& operator++(int) { 
                const Points &out = **this;
                ++ (*this);
                return out;
            }
        private:
            Surfaces::const_iterator  m_it_surface;
            size_t                    m_idx_contour;
        };

        iterator cbegin() const { return iterator(m_surfaces.cbegin()); }
        iterator begin()  const { return this->cbegin(); }
        iterator cend()   const { return iterator(m_surfaces.cend()); }
        iterator end()    const { return this->cend(); }
        size_t   size()   const { return m_size; }

    private:
        const Surfaces &m_surfaces;
        size_t          m_size;
    };

    struct SurfacesPtrProvider {
        SurfacesPtrProvider(const SurfacesPtr &surfaces) : m_surfaces(surfaces) {
            m_size = 0;
            for (const Surface *surface : surfaces)
                m_size += surface->expolygon.holes.size() + 1;
        }

        struct iterator : public PathsProviderIteratorBase {
        public:
            explicit iterator(SurfacesPtr::const_iterator it) : m_it_surface(it), m_idx_contour(0) {}
            const Points& operator*() const { return (m_idx_contour == 0) ? (*m_it_surface)->expolygon.contour.points : (*m_it_surface)->expolygon.holes[m_idx_contour - 1].points; }
            bool operator==(const iterator &rhs) const { return m_it_surface == rhs.m_it_surface && m_idx_contour == rhs.m_idx_contour; }
            bool operator!=(const iterator &rhs) const { return !(*this == rhs); }
            iterator& operator++() { 
                if (++ m_idx_contour == (*m_it_surface)->expolygon.holes.size() + 1) {
                    ++ m_it_surface;
                    m_idx_contour = 0;
                }
                return *this;
            }
            const Points& operator++(int) { 
                const Points &out = **this;
                ++ (*this);
                return out;
            }
        private:
            SurfacesPtr::const_iterator  m_it_surface;
            size_t                       m_idx_contour;
        };

        iterator cbegin() const { return iterator(m_surfaces.cbegin()); }
        iterator begin()  const { return this->cbegin(); }
        iterator cend()   const { return iterator(m_surfaces.cend()); }
        iterator end()    const { return this->cend(); }
        size_t   size()   const { return m_size; }

    private:
        const SurfacesPtr &m_surfaces;
        size_t             m_size;
    };

    
    // For ClipperLib with Z coordinates.
    using ZPoint  = Vec3i32;
    using ZPoints = std::vector<Vec3i32>;

    // Clip source polygon to be used as a clipping polygon with a bouding box around the source (to be clipped) polygon.
    // Useful as an optimization for expensive ClipperLib operations, for example when clipping source polygons one by one
    // with a set of polygons covering the whole layer below.
    void                   clip_clipper_polygon_with_subject_bbox(const Points &src, const BoundingBox &bbox, Points &out, const bool get_entire_polygons = false);
    void                   clip_clipper_polygon_with_subject_bbox(const ZPoints &src, const BoundingBox &bbox, ZPoints &out);
    [[nodiscard]] Points   clip_clipper_polygon_with_subject_bbox(const Points &src, const BoundingBox &bbox);
    [[nodiscard]] ZPoints  clip_clipper_polygon_with_subject_bbox(const ZPoints &src, const BoundingBox &bbox);
    void                   clip_clipper_polygon_with_subject_bbox(const Polygon &src, const BoundingBox &bbox, Polygon &out);
    [[nodiscard]] Polygon  clip_clipper_polygon_with_subject_bbox(const Polygon &src, const BoundingBox &bbox, const bool get_entire_polygons = false);
    [[nodiscard]] Polygons clip_clipper_polygons_with_subject_bbox(const Polygons &src, const BoundingBox &bbox);
    [[nodiscard]] Polygons clip_clipper_polygons_with_subject_bbox(const ExPolygon &src, const BoundingBox &bbox, const bool get_entire_polygons = false);
    [[nodiscard]] Polygons clip_clipper_polygons_with_subject_bbox(const ExPolygons &src, const BoundingBox &bbox, const bool get_entire_polygons = false);

    }

// Perform union of input polygons using the non-zero rule, convert to ExPolygons.
ExPolygons ClipperPaths_to_Slic3rExPolygons(const ClipperLib::Paths &input, bool do_union = false);

// offset Polygons
// Wherever applicable, please use the expand() / shrink() variants instead, they convey their purpose better.
Slic3r::Polygons offset(const Slic3r::Polygon &polygon, const float delta, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit);

// offset Polylines
// Wherever applicable, please use the expand() / shrink() variants instead, they convey their purpose better.
// Input polygons for negative offset shall be "normalized": There must be no overlap / intersections between the input polygons.
Slic3r::Polygons   offset(const Slic3r::Polyline &polyline, const float delta, ClipperLib::JoinType joinType = DefaultLineJoinType, double miterLimit = DefaultLineMiterLimit, ClipperLib::EndType end_type = DefaultEndType);
Slic3r::Polygons   offset(const Slic3r::Polylines &polylines, const float delta, ClipperLib::JoinType joinType = DefaultLineJoinType, double miterLimit = DefaultLineMiterLimit, ClipperLib::EndType end_type = DefaultEndType);
Slic3r::Polygons   offset(const Slic3r::Polygons &polygons, const float delta, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit);
Slic3r::Polygons   offset(const Slic3r::ExPolygon &expolygon, const float delta, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit);
Slic3r::Polygons   offset(const Slic3r::ExPolygons &expolygons, const float delta, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit);
Slic3r::Polygons   offset(const Slic3r::Surfaces &surfaces, const float delta, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit);
Slic3r::Polygons   offset(const Slic3r::SurfacesPtr &surfaces, const float delta, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit);
Slic3r::ExPolygons offset_ex(const Slic3r::Polygons &polygons, const float delta, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit);
Slic3r::ExPolygons offset_ex(const Slic3r::ExPolygon &expolygon, const float delta, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit);
Slic3r::ExPolygons offset_ex(const Slic3r::ExPolygons &expolygons, const float delta, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit);
Slic3r::ExPolygons offset_ex(const Slic3r::Surfaces &surfaces, const float delta, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit);
Slic3r::ExPolygons offset_ex(const Slic3r::SurfacesPtr &surfaces, const float delta, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit);
// BBS
inline Slic3r::ExPolygons offset_ex(const Slic3r::Polygon &polygon, const float delta, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit)
{
    Slic3r::Polygons temp;
    temp.push_back(polygon);

    return offset_ex(temp, delta, joinType, miterLimit);
}

// convert stroke to path by offsetting of contour
Polygons contour_to_polygons(const Polygon &polygon, const float line_width, ClipperLib::JoinType join_type = DefaultJoinType, double miter_limit = DefaultMiterLimit);
Polygons contour_to_polygons(const Polygons &polygon, const float line_width, ClipperLib::JoinType join_type = DefaultJoinType, double miter_limit = DefaultMiterLimit);

inline Slic3r::Polygons   union_safety_offset   (const Slic3r::Polygons   &polygons)   { return offset   (polygons,   ClipperSafetyOffset); }
inline Slic3r::Polygons   union_safety_offset   (const Slic3r::ExPolygons &expolygons) { return offset   (expolygons, ClipperSafetyOffset); }
inline Slic3r::ExPolygons union_safety_offset_ex(const Slic3r::Polygons   &polygons)   { return offset_ex(polygons,   ClipperSafetyOffset); }
inline Slic3r::ExPolygons union_safety_offset_ex(const Slic3r::ExPolygons &expolygons) { return offset_ex(expolygons, ClipperSafetyOffset); }

Slic3r::Polygons   union_safety_offset(const Slic3r::Polygons &expolygons);
Slic3r::Polygons   union_safety_offset(const Slic3r::ExPolygons &expolygons);
Slic3r::ExPolygons union_safety_offset_ex(const Slic3r::Polygons &polygons);
Slic3r::ExPolygons union_safety_offset_ex(const Slic3r::ExPolygons &expolygons);

// Aliases for the various offset(...) functions, conveying the purpose of the offset.
inline Slic3r::Polygons   expand(const Slic3r::Polygon &polygon, const float delta, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit) 
    { assert(delta > 0); return offset(polygon, delta, joinType, miterLimit); }
inline Slic3r::Polygons   expand(const Slic3r::Polygons &polygons, const float delta, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit) 
    { assert(delta > 0); return offset(polygons, delta, joinType, miterLimit); }
inline Slic3r::Polygons   expand(const Slic3r::ExPolygons &polygons, const float delta, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit) 
    { assert(delta > 0); return offset(polygons, delta, joinType, miterLimit); }
inline Slic3r::ExPolygons expand_ex(const Slic3r::Polygons &polygons, const float delta, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit) 
    { assert(delta > 0); return offset_ex(polygons, delta, joinType, miterLimit); }
// Input polygons for shrinking shall be "normalized": There must be no overlap / intersections between the input polygons.
inline Slic3r::Polygons   shrink(const Slic3r::Polygons &polygons, const float delta, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit) 
    { assert(delta > 0); return offset(polygons, -delta, joinType, miterLimit); }
inline Slic3r::ExPolygons shrink_ex(const Slic3r::Polygons &polygons, const float delta, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit) 
    { assert(delta > 0); return offset_ex(polygons, -delta, joinType, miterLimit); }
inline Slic3r::ExPolygons shrink_ex(const Slic3r::ExPolygons &polygons, const float delta, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit) 
    { assert(delta > 0); return offset_ex(polygons, -delta, joinType, miterLimit); }

// Wherever applicable, please use the opening() / closing() variants instead, they convey their purpose better.
// Input polygons for negative offset shall be "normalized": There must be no overlap / intersections between the input polygons.
Slic3r::Polygons   offset2(const Slic3r::ExPolygons &expolygons, const float delta1, const float delta2, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit);
Slic3r::ExPolygons offset2_ex(const Slic3r::ExPolygons &expolygons, const float delta1, const float delta2, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit);
Slic3r::ExPolygons offset2_ex(const Slic3r::Surfaces &surfaces, const float delta1, const float delta2, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit);

// BBS
Slic3r::ExPolygons _clipper_ex(ClipperLib::ClipType clipType,
    const Slic3r::Polygons &subject, const Slic3r::Polygons &clip, bool safety_offset_ = false);


// Offset outside, then inside produces morphological closing. All deltas should be positive.
Slic3r::Polygons          closing(const Slic3r::Polygons &polygons, const float delta1, const float delta2, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit);
inline Slic3r::Polygons   closing(const Slic3r::Polygons &polygons, const float delta, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit) 
    { return closing(polygons, delta, delta, joinType, miterLimit); }
Slic3r::ExPolygons        closing_ex(const Slic3r::Polygons &polygons, const float delta1, const float delta2, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit);
inline Slic3r::ExPolygons closing_ex(const Slic3r::Polygons &polygons, const float delta, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit) 
    { return closing_ex(polygons, delta, delta, joinType, miterLimit); }
inline Slic3r::ExPolygons closing_ex(const Slic3r::ExPolygons &polygons, const float delta, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit) 
    { assert(delta > 0); return offset2_ex(polygons, delta, - delta, joinType, miterLimit); }
inline Slic3r::ExPolygons closing_ex(const Slic3r::Surfaces &surfaces, const float delta, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit) 
    { assert(delta > 0); return offset2_ex(surfaces, delta, - delta, joinType, miterLimit); }

// Offset inside, then outside produces morphological opening. All deltas should be positive.
// Input polygons for opening shall be "normalized": There must be no overlap / intersections between the input polygons.
Slic3r::Polygons          opening(const Slic3r::Polygons &polygons, const float delta1, const float delta2, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit);
Slic3r::Polygons          opening(const Slic3r::ExPolygons &expolygons, const float delta1, const float delta2, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit);
Slic3r::Polygons          opening(const Slic3r::Surfaces &surfaces, const float delta1, const float delta2, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit);
inline Slic3r::Polygons   opening(const Slic3r::Polygons &polygons, const float delta, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit) 
    { return opening(polygons, delta, delta, joinType, miterLimit); }
inline Slic3r::Polygons   opening(const Slic3r::ExPolygons &expolygons, const float delta, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit) 
    { return opening(expolygons, delta, delta, joinType, miterLimit); }
inline Slic3r::Polygons   opening(const Slic3r::Surfaces &surfaces, const float delta, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit) 
    { return opening(surfaces, delta, delta, joinType, miterLimit); }
inline Slic3r::ExPolygons opening_ex(const Slic3r::ExPolygons &polygons, const float delta, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit) 
    { assert(delta > 0); return offset2_ex(polygons, - delta, delta, joinType, miterLimit); }
inline Slic3r::ExPolygons opening_ex(const Slic3r::Surfaces &surfaces, const float delta, ClipperLib::JoinType joinType = DefaultJoinType, double miterLimit = DefaultMiterLimit) 
    { assert(delta > 0); return offset2_ex(surfaces, - delta, delta, joinType, miterLimit); }

Slic3r::Lines _clipper_ln(ClipperLib::ClipType clipType, const Slic3r::Lines &subject, const Slic3r::Polygons &clip);

// Safety offset is applied to the clipping polygons only.
Slic3r::Polygons   diff(const Slic3r::Polygon &subject, const Slic3r::Polygon &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::Polygons   diff(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::Polygons   diff(const Slic3r::Polygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
// Optimized version clipping the "clipping" polygon using clip_clipper_polygon_with_subject_bbox().
// To be used with complex clipping polygons, where majority of the clipping polygons are outside of the source polygon.
Slic3r::Polygons   diff_clipped(const Slic3r::Polygons &src, const Slic3r::Polygons &clipping, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::ExPolygons diff_clipped(const Slic3r::ExPolygons &src, const Slic3r::Polygons &clipping, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::ExPolygons diff_clipped(const Slic3r::ExPolygons &src, const Slic3r::ExPolygons &clipping, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::Polygons   diff(const Slic3r::ExPolygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::Polygons   diff(const Slic3r::ExPolygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::Polygons   diff(const Slic3r::Surfaces &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::ExPolygons diff_ex(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::ExPolygons diff_ex(const Slic3r::Polygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::ExPolygons diff_ex(const Slic3r::Polygons &subject, const Slic3r::Surfaces &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::ExPolygons diff_ex(const Slic3r::Polygon &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::ExPolygons diff_ex(const Slic3r::ExPolygon &subject, const Slic3r::Polygon &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::ExPolygons diff_ex(const Slic3r::ExPolygon &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::ExPolygons diff_ex(const Slic3r::ExPolygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::ExPolygons diff_ex(const Slic3r::ExPolygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::ExPolygons diff_ex(const Slic3r::Surfaces &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::ExPolygons diff_ex(const Slic3r::Surfaces &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::ExPolygons diff_ex(const Slic3r::ExPolygons &subject, const Slic3r::Surfaces &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::ExPolygons diff_ex(const Slic3r::Surfaces &subject, const Slic3r::Surfaces &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::ExPolygons diff_ex(const Slic3r::SurfacesPtr &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::ExPolygons diff_ex(const Slic3r::SurfacesPtr &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::Polylines  diff_pl(const Slic3r::Polyline &subject, const Slic3r::Polygons &clip);
Slic3r::Polylines  diff_pl(const Slic3r::Polylines &subject, const Slic3r::Polygons &clip);
Slic3r::Polylines  diff_pl(const Slic3r::Polyline &subject, const Slic3r::ExPolygon &clip);
Slic3r::Polylines  diff_pl(const Slic3r::Polylines &subject, const Slic3r::ExPolygon &clip);
Slic3r::Polylines  diff_pl(const Slic3r::Polylines &subject, const Slic3r::ExPolygons &clip);
Slic3r::Polylines  diff_pl(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip);

// BBS
inline Slic3r::ExPolygons diff_ex(const Slic3r::ExPolygon& subject, const Slic3r::ExPolygon& clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No)
{
    Slic3r::ExPolygons subject_temp;
    Slic3r::ExPolygons clip_temp;

    subject_temp.push_back(subject);
    clip_temp.push_back(clip);
    return diff_ex(subject_temp, clip_temp);
}

inline Slic3r::ExPolygons diff_ex(const Slic3r::ExPolygon& subject, const Slic3r::ExPolygons& clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No)
{
    Slic3r::ExPolygons subject_temp;
    subject_temp.push_back(subject);

    return diff_ex(subject_temp, clip, do_safety_offset);
}

inline Slic3r::ExPolygons diff_ex(const Slic3r::ExPolygons& subject, const Slic3r::ExPolygon& clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No)
{
    Slic3r::ExPolygons clip_temp;
    clip_temp.push_back(clip);

    return diff_ex(subject, clip_temp, do_safety_offset);
}

inline Slic3r::Lines diff_ln(const Slic3r::Lines &subject, const Slic3r::Polygons &clip)
{
    return _clipper_ln(ClipperLib::ctDifference, subject, clip);
}

// Safety offset is applied to the clipping polygons only.
Slic3r::Polygons   intersection(const Slic3r::Polygon &subject, const Slic3r::Polygon &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::Polygons   intersection(const Slic3r::Polygons &subject, const Slic3r::ExPolygon &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::Polygons   intersection(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::Polygons   intersection(const Slic3r::ExPolygon &subject, const Slic3r::ExPolygon &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
// Optimized version clipping the "clipping" polygon using clip_clipper_polygon_with_subject_bbox().
// To be used with complex clipping polygons, where majority of the clipping polygons are outside of the source polygon.
Slic3r::Polygons   intersection_clipped(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::Polygons   intersection(const Slic3r::ExPolygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::Polygons   intersection(const Slic3r::ExPolygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::Polygons   intersection(const Slic3r::Surfaces &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::Polygons   intersection(const Slic3r::Surfaces &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
// BBS
Slic3r::Polygons   intersection(const Slic3r::Polygons& subject, const Slic3r::Polygon& clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::ExPolygons intersection_ex(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::ExPolygons intersection_ex(const Slic3r::ExPolygon &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::ExPolygons intersection_ex(const Slic3r::ExPolygon& subject, const Slic3r::ExPolygon& clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::ExPolygons intersection_ex(const Slic3r::Polygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::ExPolygons intersection_ex(const Slic3r::ExPolygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::ExPolygons intersection_ex(const Slic3r::ExPolygons& subject, const Slic3r::ExPolygon& clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::ExPolygons intersection_ex(const Slic3r::ExPolygon& subject, const Slic3r::ExPolygons& clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::ExPolygons intersection_ex(const Slic3r::ExPolygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::ExPolygons intersection_ex(const Slic3r::Surfaces &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::ExPolygons intersection_ex(const Slic3r::Surfaces &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::ExPolygons intersection_ex(const Slic3r::Surfaces &subject, const Slic3r::Surfaces &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::ExPolygons intersection_ex(const Slic3r::SurfacesPtr &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::Polylines  intersection_pl(const Slic3r::Polylines &subject, const Slic3r::Polygon &clip);
Slic3r::Polylines  intersection_pl(const Slic3r::Polyline &subject, const Slic3r::ExPolygon &clip);
Slic3r::Polylines  intersection_pl(const Slic3r::Polylines &subject, const Slic3r::ExPolygon &clip);
Slic3r::Polylines  intersection_pl(const Slic3r::Polyline &subject, const Slic3r::Polygons &clip);
Slic3r::Polylines  intersection_pl(const Slic3r::Polylines &subject, const Slic3r::Polygons &clip);
Slic3r::Polylines  intersection_pl(const Slic3r::Polylines &subject, const Slic3r::ExPolygons &clip);
Slic3r::Polylines  intersection_pl(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip);

inline Slic3r::Lines intersection_ln(const Slic3r::Lines &subject, const Slic3r::Polygons &clip)
{
    return _clipper_ln(ClipperLib::ctIntersection, subject, clip);
}

inline Slic3r::Lines intersection_ln(const Slic3r::Line &subject, const Slic3r::Polygons &clip)
{
    Slic3r::Lines lines;
    lines.emplace_back(subject);
    return _clipper_ln(ClipperLib::ctIntersection, lines, clip);
}

Slic3r::Polygons union_(const Slic3r::Polygons &subject);
Slic3r::Polygons union_(const Slic3r::ExPolygons &subject);
Slic3r::Polygons union_(const Slic3r::Polygons &subject, const ClipperLib::PolyFillType fillType);
Slic3r::Polygons union_(const Slic3r::Polygons &subject, const Slic3r::Polygons &subject2);
// May be used to "heal" unusual models (3DLabPrints etc.) by providing fill_type (pftEvenOdd, pftNonZero, pftPositive, pftNegative).
Slic3r::ExPolygons union_ex(const Slic3r::Polygons &subject, ClipperLib::PolyFillType fill_type = ClipperLib::pftNonZero);
Slic3r::ExPolygons union_ex(const Slic3r::ExPolygons &subject);
Slic3r::ExPolygons union_ex(const Slic3r::ExPolygons &subject, const Slic3r::Polygons &subject2);
Slic3r::ExPolygons union_ex(const Slic3r::Surfaces &subject);

Slic3r::ExPolygons union_ex(const Slic3r::ExPolygons& poly1, const Slic3r::ExPolygons& poly2, bool safety_offset_ = false);

// Convert polygons / expolygons into ClipperLib::PolyTree using ClipperLib::pftEvenOdd, thus union will NOT be performed.
// If the contours are not intersecting, their orientation shall not be modified by union_pt().
ClipperLib::PolyTree union_pt(const Slic3r::Polygons &subject);
ClipperLib::PolyTree union_pt(const Slic3r::ExPolygons &subject);

Slic3r::ExPolygons xor_ex(const Slic3r::ExPolygons &subject, const Slic3r::ExPolygon &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);
Slic3r::ExPolygons xor_ex(const Slic3r::ExPolygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset = ApplySafetyOffset::No);

Slic3r::Polygons union_pt_chained_outside_in(const Slic3r::Polygons &subject);

ClipperLib::PolyNodes order_nodes(const ClipperLib::PolyNodes &nodes);

// Implementing generalized loop (foreach) over a list of nodes which can be
// ordered or unordered (performance gain) based on template parameter
enum class e_ordering {
    ON,
    OFF
};

// Create a template struct, template functions can not be partially specialized
template<e_ordering o, class Fn> struct _foreach_node {
    void operator()(const ClipperLib::PolyNodes &nodes, Fn &&fn);
};

// Specialization with NO ordering
template<class Fn> struct _foreach_node<e_ordering::OFF, Fn> {
    void operator()(const ClipperLib::PolyNodes &nodes, Fn &&fn)
    {
        for (auto &n : nodes) fn(n);    
    }
};

// Specialization with ordering
template<class Fn> struct _foreach_node<e_ordering::ON, Fn> {
    void operator()(const ClipperLib::PolyNodes &nodes, Fn &&fn)
    {
        auto ordered_nodes = order_nodes(nodes);
        for (auto &n : nodes) fn(n);    
    }
};

// Wrapper function for the foreach_node which can deduce arguments automatically
template<e_ordering o, class Fn>
void foreach_node(const ClipperLib::PolyNodes &nodes, Fn &&fn)
{
    _foreach_node<o, Fn>()(nodes, std::forward<Fn>(fn));
}

// Collecting polygons of the tree into a list of Polygons, holes have clockwise
// orientation.
template<e_ordering ordering = e_ordering::OFF>
void traverse_pt(const ClipperLib::PolyNode *tree, Polygons *out)
{
    if (!tree) return; // terminates recursion
    
    // Push the contour of the current level
    out->emplace_back(tree->Contour);
    
    // Do the recursion for all the children.
    traverse_pt<ordering>(tree->Childs, out);
}

// Collecting polygons of the tree into a list of ExPolygons.
template<e_ordering ordering = e_ordering::OFF>
void traverse_pt(const ClipperLib::PolyNode *tree, ExPolygons *out)
{
    if (!tree) return;
    else if(tree->IsHole()) {
        // Levels of holes are skipped and handled together with the
        // contour levels.
        traverse_pt<ordering>(tree->Childs, out);
        return;
    }
    
    ExPolygon level;
    level.contour.points = tree->Contour;
    
    foreach_node<ordering>(tree->Childs, 
                           [out, &level] (const ClipperLib::PolyNode *node) {
        
        // Holes are collected here. 
        level.holes.emplace_back(node->Contour);
        
        // By doing a recursion, a new level expoly is created with the contour
        // and holes of the lower level. Doing this for all the childs.
        traverse_pt<ordering>(node->Childs, out);
    }); 
    
    out->emplace_back(level);
}

template<e_ordering o = e_ordering::OFF, class ExOrJustPolygons>
void traverse_pt(const ClipperLib::PolyNodes &nodes, ExOrJustPolygons *retval)
{
    foreach_node<o>(nodes, [&retval](const ClipperLib::PolyNode *node) {
        traverse_pt<o>(node, retval);
    });
}


/* OTHER */
Slic3r::Polygons simplify_polygons(const Slic3r::Polygons &subject);
Slic3r::ExPolygons simplify_polygons_ex(const Slic3r::Polygons &subject);

Polygons top_level_islands(const Slic3r::Polygons &polygons);

ClipperLib::Path mittered_offset_path_scaled(const Points &contour, const std::vector<float> &deltas, double miter_limit);
Polygons  variable_offset_inner(const ExPolygon &expoly, const std::vector<std::vector<float>> &deltas, double miter_limit = 2.);
Polygons  variable_offset_outer(const ExPolygon &expoly, const std::vector<std::vector<float>> &deltas, double miter_limit = 2.);
ExPolygons variable_offset_outer_ex(const ExPolygon &expoly, const std::vector<std::vector<float>> &deltas, double miter_limit = 2.);
ExPolygons variable_offset_inner_ex(const ExPolygon &expoly, const std::vector<std::vector<float>> &deltas, double miter_limit = 2.);

Pointfs make_counter_clockwise(const Pointfs& pointfs);

} // namespace Slic3r

#endif // slic3r_ClipperUtils_hpp_
