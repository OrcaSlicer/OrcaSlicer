#ifndef SLA_SPATINDEX_HPP
#define SLA_SPATINDEX_HPP

#include <memory>
#include <utility>
#include <vector>

#include <Eigen/Geometry>

#include <libslic3r/BoundingBox.hpp>

namespace Slic3r {
namespace sla {

typedef Eigen::Matrix<double,   3, 1, Eigen::DontAlign> Vec3d;
using PointIndexEl = std::pair<Vec3d, unsigned>;

class PointIndex {
    class Impl;

    // We use Pimpl because it takes a long time to compile boost headers which
    // is the engine of this class. We include it only in the cpp file.
    std::unique_ptr<Impl> m_impl;
public:

    PointIndex();
    ~PointIndex();

    PointIndex(const PointIndex&);
    PointIndex(PointIndex&&);
    PointIndex& operator=(const PointIndex&);
    PointIndex& operator=(PointIndex&&);

    void insert(const PointIndexEl&);
    bool remove(const PointIndexEl&);

    inline void insert(const Vec3d& v, unsigned idx)
    {
        insert(std::make_pair(v, unsigned(idx)));
    }

    std::vector<PointIndexEl> query(std::function<bool(const PointIndexEl&)>) const;
    std::vector<PointIndexEl> nearest(const Vec3d&, unsigned k) const;
    std::vector<PointIndexEl> query(const Vec3d &v, unsigned k) const // wrapper
    {
        return nearest(v, k);
    }

    // For testing
    size_t size() const;
    bool empty() const { return size() == 0; }

    void foreach(std::function<void(const PointIndexEl& el)> fn);
    void foreach(std::function<void(const PointIndexEl& el)> fn) const;
};

using BoxIndexEl = std::pair<Slic3r::BoundingBox, unsigned>;

class BoxIndex {
    class Impl;
    
    // We use Pimpl because it takes a long time to compile boost headers which
    // is the engine of this class. We include it only in the cpp file.
    std::unique_ptr<Impl> m_impl;
public:
    
    BoxIndex();
    ~BoxIndex();
    
    BoxIndex(const BoxIndex&);
    BoxIndex(BoxIndex&&);
    BoxIndex& operator=(const BoxIndex&);
    BoxIndex& operator=(BoxIndex&&);
    
    void insert(const BoxIndexEl&);
    void insert(const BoundingBox& bb, unsigned idx)
    {
        insert(std::make_pair(bb, unsigned(idx)));
    }
    
    bool remove(const BoxIndexEl&);

    enum QueryType { qtIntersects, qtWithin };

    std::vector<BoxIndexEl> query(const BoundingBox&, QueryType qt);
    
    // For testing
    size_t size() const;
    bool empty() const { return size() == 0; }
    
    void foreach(std::function<void(const BoxIndexEl& el)> fn);
};

}
}

#endif // SPATINDEX_HPP
