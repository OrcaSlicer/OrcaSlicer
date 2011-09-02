#include "Clustering.hpp"
#include "boost/geometry/index/rtree.hpp"

#include <libslic3r/SLA/SpatIndex.hpp>
#include <libslic3r/SLA/BoostAdapter.hpp>

namespace Slic3r { namespace sla {

namespace bgi = boost::geometry::index;
using Index3D = bgi::rtree< PointIndexEl, bgi::rstar<16, 4> /* ? */ >;

namespace {

bool cmp_ptidx_elements(const PointIndexEl& e1, const PointIndexEl& e2)
{
    return e1.second < e2.second;
};

ClusteredPoints cluster(Index3D &sindex,
                        unsigned max_points,
                        std::function<std::vector<PointIndexEl>(
                            const Index3D &, const PointIndexEl &)> qfn)
{
    using Elems = std::vector<PointIndexEl>;

    // Recursive function for visiting all the points in a given distance to
    // each other
    std::function<void(Elems&, Elems&)> group =
        [&sindex, &group, max_points, qfn](Elems& pts, Elems& cluster)
    {
        for(auto& p : pts) {
            std::vector<PointIndexEl> tmp = qfn(sindex, p);

            std::sort(tmp.begin(), tmp.end(), cmp_ptidx_elements);

            Elems newpts;
            std::set_difference(tmp.begin(), tmp.end(),
                                cluster.begin(), cluster.end(),
                                std::back_inserter(newpts), cmp_ptidx_elements);

            int c = max_points && newpts.size() + cluster.size() > max_points?
                        int(max_points - cluster.size()) : int(newpts.size());

            cluster.insert(cluster.end(), newpts.begin(), newpts.begin() + c);
            std::sort(cluster.begin(), cluster.end(), cmp_ptidx_elements);

            if(!newpts.empty() && (!max_points || cluster.size() < max_points))
                group(newpts, cluster);
        }
    };

    std::vector<Elems> clusters;
    for(auto it = sindex.begin(); it != sindex.end();) {
        Elems cluster = {};
        Elems pts = {*it};
        group(pts, cluster);

        for(auto& c : cluster) sindex.remove(c);
        it = sindex.begin();

        clusters.emplace_back(cluster);
    }

    ClusteredPoints result;
    for(auto& cluster : clusters) {
        result.emplace_back();
        for(auto c : cluster) result.back().emplace_back(c.second);
    }

    return result;
}

std::vector<PointIndexEl> distance_queryfn(const Index3D& sindex,
                                           const PointIndexEl& p,
                                           double dist,
                                           unsigned max_points)
{
    std::vector<PointIndexEl> tmp; tmp.reserve(max_points);
    sindex.query(
        bgi::nearest(p.first, max_points),
        std::back_inserter(tmp)
        );

    for(auto it = tmp.begin(); it < tmp.end(); ++it)
        if((p.first - it->first).norm() > dist) it = tmp.erase(it);

    return tmp;
}

} // namespace

// Clustering a set of points by the given criteria
ClusteredPoints cluster(
    const std::vector<unsigned>& indices,
    std::function<Vec3d(unsigned)> pointfn,
    double dist,
    unsigned max_points)
{
    // A spatial index for querying the nearest points
    Index3D sindex;

    // Build the index
    for(auto idx : indices) sindex.insert( std::make_pair(pointfn(idx), idx));

    return cluster(sindex, max_points,
                   [dist, max_points](const Index3D& sidx, const PointIndexEl& p)
                   {
                       return distance_queryfn(sidx, p, dist, max_points);
                   });
}

// Clustering a set of points by the given criteria
ClusteredPoints cluster(
    const std::vector<unsigned>& indices,
    std::function<Vec3d(unsigned)> pointfn,
    std::function<bool(const PointIndexEl&, const PointIndexEl&)> predicate,
    unsigned max_points)
{
    // A spatial index for querying the nearest points
    Index3D sindex;

    // Build the index
    for(auto idx : indices) sindex.insert( std::make_pair(pointfn(idx), idx));

    return cluster(sindex, max_points,
                   [max_points, predicate](const Index3D& sidx, const PointIndexEl& p)
                   {
                       std::vector<PointIndexEl> tmp; tmp.reserve(max_points);
                       sidx.query(bgi::satisfies([p, predicate](const PointIndexEl& e){
                                      return predicate(p, e);
                                  }), std::back_inserter(tmp));
                       return tmp;
                   });
}

ClusteredPoints cluster(const Eigen::MatrixXd& pts, double dist, unsigned max_points)
{
    // A spatial index for querying the nearest points
    Index3D sindex;

    // Build the index
    for(Eigen::Index i = 0; i < pts.rows(); i++)
        sindex.insert(std::make_pair(Vec3d(pts.row(i)), unsigned(i)));

    return cluster(sindex, max_points,
                   [dist, max_points](const Index3D& sidx, const PointIndexEl& p)
                   {
                       return distance_queryfn(sidx, p, dist, max_points);
                   });
}

}} // namespace Slic3r::sla
