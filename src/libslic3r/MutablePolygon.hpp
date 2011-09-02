#ifndef slic3r_MutablePolygon_hpp_
#define slic3r_MutablePolygon_hpp_

#include "Point.hpp"
#include "Polygon.hpp"
#include "ExPolygon.hpp"

namespace Slic3r {

// Polygon implemented as a loop of double linked elements.
// All elements are allocated in a single std::vector<>, thus integer indices are used for
// referencing the previous and next element and inside iterators to survive reallocation
// of the vector.
class MutablePolygon
{
public:
    using IndexType = int32_t;
    using PointType = Point;
    class const_iterator {
    public:
        bool             operator==(const const_iterator &rhs) const { assert(m_data == rhs.m_data); assert(this->valid()); return m_idx == rhs.m_idx; }
        bool             operator!=(const const_iterator &rhs) const { return ! (*this == rhs); }
        const_iterator&  operator--()    { assert(this->valid()); m_idx = m_data->at(m_idx).prev; return *this; }
        const_iterator   operator--(int) { const_iterator result(*this); --(*this); return result; }
        const_iterator&  operator++()    { assert(this->valid()); m_idx = m_data->at(m_idx).next; return *this; }
        const_iterator   operator++(int) { const_iterator result(*this); ++(*this); return result; }
        const_iterator   prev()    const { assert(this->valid()); return { m_data, m_data->at(m_idx).prev }; }
        const_iterator   next()    const { assert(this->valid()); return { m_data, m_data->at(m_idx).next }; }
        bool             valid()   const { return m_idx >= 0; }
        const PointType& operator*() const { return m_data->at(m_idx).point; }
        const PointType* operator->() const { return &m_data->at(m_idx).point; }
        const MutablePolygon& polygon() const { assert(this->valid()); return *m_data; }
        IndexType        size()    const { assert(this->valid()); return m_data->size(); }
    private:
        const_iterator(const MutablePolygon *data, IndexType idx) : m_data(data), m_idx(idx) {}
        friend class MutablePolygon;
        const MutablePolygon  *m_data;
        IndexType              m_idx;
    };

    class iterator {
    public:
        bool            operator==(const iterator &rhs) const { assert(m_data == rhs.m_data); assert(this->valid()); return m_idx == rhs.m_idx; }
        bool            operator!=(const iterator &rhs) const { return !(*this == rhs); }
        iterator&       operator--()    { assert(this->valid()); m_idx = m_data->at(m_idx).prev; return *this; }
        iterator        operator--(int) { iterator result(*this); --(*this); return result; }
        iterator&       operator++()    { assert(this->valid()); m_idx = m_data->at(m_idx).next; return *this; }
        iterator        operator++(int) { iterator result(*this); ++(*this); return result; }
        iterator        prev()    const { assert(this->valid()); return { m_data, m_data->at(m_idx).prev }; }
        iterator        next()    const { assert(this->valid()); return { m_data, m_data->at(m_idx).next }; }
        bool            valid()   const { return m_idx >= 0; }
        PointType&      operator*() const { return m_data->at(m_idx).point; }
        PointType*      operator->() const { return &m_data->at(m_idx).point; }
        MutablePolygon& polygon() const { assert(this->valid()); return *m_data; }
        IndexType       size()    const { assert(this->valid()); return m_data->size(); }
        iterator&       remove()        { m_idx = m_data->remove(*this).m_idx; return *this; }
        iterator        insert(const PointType pt) const { return m_data->insert(*this, pt); }
    private:
        iterator(MutablePolygon *data, IndexType idx) : m_data(data), m_idx(idx) {}
        friend class MutablePolygon;
        MutablePolygon  *m_data;
        IndexType        m_idx;
        friend class range;
    };

    // Iterator range for maintaining a range of unprocessed items, see smooth_outward().
    class range
    {
    public:
        range(MutablePolygon& poly) : range(poly.begin(), poly.end()) {}
        range(MutablePolygon::iterator begin, MutablePolygon::iterator end) : m_begin(begin), m_end(end) {}

        // Start of a range, inclusive. If range is empty, then ! begin().valid().
        MutablePolygon::iterator    begin() const { return m_begin; }
        // End of a range, inclusive. If range is empty, then ! end().valid().
        MutablePolygon::iterator    end()   const { return m_end; }
        // Is the range empty?
        bool                        empty() const { return !m_begin.valid(); }

        // Return begin() and shorten the range by advancing front.
        MutablePolygon::iterator    process_next() {
            assert(!this->empty());
            MutablePolygon::iterator out = m_begin;
            this->advance_front();
            return out;
        }

        void advance_front() {
            assert(! this->empty());
            if (m_begin == m_end)
                this->make_empty();
            else
                ++ m_begin;
        }

        void retract_back() {
            assert(! this->empty());
            if (m_begin == m_end)
                this->make_empty();
            else
                -- m_end;
        }

        MutablePolygon::iterator remove_front(MutablePolygon::iterator it) {
            if (! this->empty() && m_begin == it)
                this->advance_front();
            return it.remove();
        }

        MutablePolygon::iterator remove_back(MutablePolygon::iterator it) {
            if (! this->empty() && m_end == it)
                this->retract_back();
            return it.remove();
        }

    private:
        // Range from begin to end, inclusive.
        // If the range is valid, then both m_begin and m_end are invalid.
        MutablePolygon::iterator    m_begin;
        MutablePolygon::iterator    m_end;

        void make_empty() {
            m_begin.m_idx = -1;
            m_end.m_idx = -1;
        }
    };

    MutablePolygon() = default;
    MutablePolygon(const Polygon &rhs, size_t reserve = 0) : MutablePolygon(rhs.points.begin(), rhs.points.end(), reserve) {}
    MutablePolygon(std::initializer_list<Point> rhs, size_t reserve = 0) : MutablePolygon(rhs.begin(), rhs.end(), reserve) {}

    template<typename IT>
    MutablePolygon(IT begin, IT end, size_t reserve = 0) {
        this->assign_inner(begin, end, reserve);
    };

    template<typename IT>
    void assign(IT begin, IT end, size_t reserve = 0) {
        m_data.clear();
        m_head      = IndexType(-1);
        m_head_free = { IndexType(-1) };
        this->assign_inner(begin, end, reserve);  
    };

    void assign(const Polygon &rhs, size_t reserve = 0) {
        assign(rhs.points.begin(), rhs.points.end(), reserve);
    }

    void polygon(Polygon &out) const {
        out.points.clear();
        if (this->valid()) {
            out.points.reserve(this->size());
            auto it = this->cbegin();
            out.points.emplace_back(*it);
            for (++ it; it != this->cbegin(); ++ it)
                out.points.emplace_back(*it);
        }
    };

    Polygon polygon() const {
        Polygon out;
        this->polygon(out);
        return out;
    };

    bool            empty()  const { return m_size == 0; }
    size_t          size()   const { return m_size; }
    size_t          capacity() const { return m_data.capacity(); }
    bool            valid()  const { return m_size >= 3; }
    void            clear()        { m_data.clear(); m_size = 0; m_head = IndexType(-1); m_head_free = IndexType(-1); }

    iterator        begin()        { return { this, m_head }; }
    const_iterator  cbegin() const { return { this, m_head }; }
    const_iterator  begin()  const { return this->cbegin(); }
    // End points to the last item before roll over. This is different from the usual end() concept!
    iterator        end()          { return { this, this->empty() ? -1 : this->at(m_head).prev }; }
    const_iterator  cend()   const { return { this, this->empty() ? -1 : this->at(m_head).prev }; }
    const_iterator  end()    const { return this->cend(); }

    // Returns iterator following the removed element. Returned iterator will become invalid if last point is removed.
    // If begin() is removed, then the next element will become the new begin().
    iterator        remove(const iterator it) { assert(it.m_data == this); return { this, this->remove(it.m_idx) }; }
    // Insert a new point before it. Returns iterator to the newly inserted point.
    // begin() will not change, end() may point to the newly inserted point.
    iterator        insert(const iterator it, const PointType pt) { assert(it.m_data == this); return { this, this->insert(it.m_idx, pt) }; }

private:
    struct LinkedPoint {
        // 8 bytes
        PointType point;
        // 4 bytes
        IndexType prev;
        // 4 bytes
        IndexType next;
    };
    std::vector<LinkedPoint>    m_data;
    // Number of points in the linked list.
    IndexType                   m_size { 0 };
    IndexType                   m_head { IndexType(-1) };
    // Head of the free list.
    IndexType                   m_head_free { IndexType(-1) };

    LinkedPoint&          at(IndexType i)       { return m_data[i]; }
    const LinkedPoint&    at(IndexType i) const { return m_data[i]; }

    template<typename IT>
    void assign_inner(IT begin, IT end, size_t reserve) {
        m_size = IndexType(end - begin);
        if (m_size > 0) {
            m_head = 0;
            m_data.reserve(std::max<size_t>(m_size, reserve));
            auto i = IndexType(-1);
            auto j = IndexType(1);
            for (auto it = begin; it != end; ++ it)
                m_data.push_back({ *it, i ++, j ++ });
            m_data.front().prev = m_size - 1;
            m_data.back ().next = 0;
        }
    };

    IndexType remove(const IndexType i) {
        assert(i >= 0);
        assert(m_size > 0);
        assert(m_head != -1);
        LinkedPoint &lp = this->at(i);
        IndexType prev = lp.prev;
        IndexType next = lp.next;
        lp.next = m_head_free;
        m_head_free = i;
        if (-- m_size == 0)
            m_head = -1;
        else if (m_head == i)
            m_head = next;
        assert(! this->empty() || (prev == i && next == i));
        if (this->empty())
            return IndexType(-1);
        this->at(prev).next = next;
        this->at(next).prev = prev;
        return next;
    }

    IndexType insert(const IndexType i, const Point pt) {
        assert(i >= 0);
        IndexType n;
        IndexType j = this->at(i).prev;
        if (m_head_free == -1) {
            // Allocate a new item.
            n = IndexType(m_data.size());
            m_data.push_back({ pt, j, i });
        } else {
            n = m_head_free;
            LinkedPoint &nlp = this->at(n);
            m_head_free = nlp.next;
            nlp = { pt, j, i };
        }
        this->at(j).next = n;
        this->at(i).prev = n;
        ++ m_size;
        return n;
    }

    /*
    IndexType insert(const IndexType i, const Point pt) {
        assert(i >= 0);
        if (this->at(i).point == pt)
            return i;
        IndexType j = this->at(i).next;
        if (this->at(j).point == pt)
            return i;
        IndexType n;
        if (m_head_free == -1) {
            // Allocate a new item.
            n = IndexType(m_data.size());
            m_data.push_back({ pt, i, j });
        } else {
            LinkedPoint &nlp = this->at(m_head_free);
            m_head_free = nlp.next;
            nlp = { pt, i, j };
        }
        this->at(i).next = n;
        this->at(j).prev = n;
        ++ m_size;
        return n;
    }
    */
};

inline bool operator==(const MutablePolygon &p1, const MutablePolygon &p2) 
{ 
    if (p1.size() != p2.size())
        return false;
    if (p1.empty())
        return true;
    auto begin = p1.cbegin();
    auto it  = begin;
    auto it2 = p2.cbegin();
    for (;;) {
        if (! (*it == *it2))
            return false;
        if (++ it == begin)
            return true;
        ++ it2;
    }
}

inline bool operator!=(const MutablePolygon &p1, const MutablePolygon &p2) { return ! (p1 == p2); }

// Remove exact duplicate points. May reduce the polygon down to empty polygon.
void remove_duplicates(MutablePolygon &polygon);
void remove_duplicates(MutablePolygon &polygon, double eps);
void remove_duplicates(MutablePolygon& polygon, coord_t scaled_eps, const double max_angle);
inline ExPolygons remove_duplicates(ExPolygons expolygons, coord_t scaled_eps, double max_angle)
{
    MutablePolygon mp;
    for (ExPolygon& expolygon : expolygons) {
        mp.assign(expolygon.contour, expolygon.contour.size() * 2);
        remove_duplicates(mp, scaled_eps, max_angle);
        mp.polygon(expolygon.contour);
        for (Polygon& hole : expolygon.holes) {
            mp.assign(hole, hole.size() * 2);
            remove_duplicates(mp, scaled_eps, max_angle);
            mp.polygon(hole);
        }
        expolygon.holes.erase(std::remove_if(expolygon.holes.begin(), expolygon.holes.end(), [](const auto& p) { return p.empty(); }), expolygon.holes.end());
    }
    expolygons.erase(std::remove_if(expolygons.begin(), expolygons.end(), [](const auto& p) { return p.empty(); }), expolygons.end());
    return expolygons;
}

void smooth_outward(MutablePolygon &polygon, coord_t clip_dist_scaled);

inline Polygon smooth_outward(Polygon polygon, coord_t clip_dist_scaled)
{ 
    MutablePolygon mp(polygon, polygon.size() * 2);
    smooth_outward(mp, clip_dist_scaled);
    mp.polygon(polygon);
    return polygon;
}

inline Polygons smooth_outward(Polygons polygons, coord_t clip_dist_scaled)
{ 
    MutablePolygon mp;
    for (Polygon &polygon : polygons) {
        mp.assign(polygon, polygon.size() * 2);
        smooth_outward(mp, clip_dist_scaled);
        mp.polygon(polygon);
    }
    polygons.erase(std::remove_if(polygons.begin(), polygons.end(), [](const auto &p){ return p.empty(); }), polygons.end());
    return polygons;
}

inline ExPolygons smooth_outward(ExPolygons expolygons, coord_t clip_dist_scaled)
{
    MutablePolygon mp;
    for (ExPolygon &expolygon : expolygons) {
        mp.assign(expolygon.contour, expolygon.contour.size() * 2);
        smooth_outward(mp, clip_dist_scaled);
        mp.polygon(expolygon.contour);
        for (Polygon &hole : expolygon.holes) {
            mp.assign(hole, hole.size() * 2);
            smooth_outward(mp, clip_dist_scaled);
            mp.polygon(hole);
        }
        expolygon.holes.erase(std::remove_if(expolygon.holes.begin(), expolygon.holes.end(), [](const auto &p) { return p.empty(); }), expolygon.holes.end());
    }
    expolygons.erase(std::remove_if(expolygons.begin(), expolygons.end(), [](const auto &p) { return p.empty(); }), expolygons.end());
    return expolygons;
}

}

#endif // slic3r_MutablePolygon_hpp_
