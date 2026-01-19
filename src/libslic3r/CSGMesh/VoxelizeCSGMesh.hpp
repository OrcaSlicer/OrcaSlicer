#ifndef VOXELIZECSGMESH_HPP
#define VOXELIZECSGMESH_HPP

#include <functional>
#include <stack>

#include "CSGMesh.hpp"
#include "libslic3r/OpenVDBUtils.hpp"
#include "libslic3r/Execution/ExecutionTBB.hpp"

namespace Slic3r { namespace csg {

using VoxelizeParams = MeshToGridParams;

// This method can be overriden when a specific CSGPart type supports caching
// of the voxel grid
template<class CSGPartT>
VoxelGridPtr get_voxelgrid(const CSGPartT &csgpart, VoxelizeParams params)
{
    const indexed_triangle_set *its = csg::get_mesh(csgpart);
    VoxelGridPtr ret;

    params.trafo(params.trafo() * csg::get_transform(csgpart));

    if (its)
        ret = mesh_to_grid(*its, params);

    return ret;
}

namespace detail {

inline void perform_csg(CSGType op, VoxelGridPtr &dst, VoxelGridPtr &src)
{
    if (!dst || !src)
        return;

    switch (op) {
    case CSGType::Union:
        if (is_grid_empty(*dst) && !is_grid_empty(*src))
            dst = clone(*src);
        else
            grid_union(*dst, *src);

        break;
    case CSGType::Difference:
        grid_difference(*dst, *src);
        break;
    case CSGType::Intersection:
        grid_intersection(*dst, *src);
        break;
    }
}

} // namespace detail

template<class It>
VoxelGridPtr voxelize_csgmesh(const Range<It>      &csgrange,
                              const VoxelizeParams &params = {})
{
    using namespace detail;

    VoxelGridPtr ret;

    std::vector<VoxelGridPtr> grids (csgrange.size());

    execution::for_each(ex_tbb, size_t(0), csgrange.size(), [&](size_t csgidx) {
        if (params.statusfn() && params.statusfn()(-1))
            return;

        auto it = csgrange.begin();
        std::advance(it, csgidx);
        auto &csgpart = *it;
        grids[csgidx] = get_voxelgrid(csgpart, params);
    }, execution::max_concurrency(ex_tbb));

    size_t csgidx = 0;
    struct Frame { CSGType op = CSGType::Union; VoxelGridPtr grid; };
    std::stack opstack{std::vector<Frame>{}};

    opstack.push({CSGType::Union, mesh_to_grid({}, params)});

    for (auto &csgpart : csgrange) {
        if (params.statusfn() && params.statusfn()(-1))
            break;

        auto &partgrid = grids[csgidx++];

        auto op = get_operation(csgpart);

        if (get_stack_operation(csgpart) == CSGStackOp::Push) {
            opstack.push({op, mesh_to_grid({}, params)});
            op = CSGType::Union;
        }

        Frame *top = &opstack.top();

        perform_csg(get_operation(csgpart), top->grid, partgrid);

        if (get_stack_operation(csgpart) == CSGStackOp::Pop) {
            VoxelGridPtr popgrid = std::move(top->grid);
            auto popop = opstack.top().op;
            opstack.pop();
            VoxelGridPtr &grid = opstack.top().grid;
            perform_csg(popop, grid, popgrid);
        }
    }

    ret = std::move(opstack.top().grid);

    return ret;
}

}} // namespace Slic3r::csg

#endif // VOXELIZECSGMESH_HPP
