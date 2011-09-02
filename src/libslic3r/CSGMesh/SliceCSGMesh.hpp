#ifndef SLICECSGMESH_HPP
#define SLICECSGMESH_HPP

#include "CSGMesh.hpp"

#include <stack>

#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Execution/ExecutionTBB.hpp"

namespace Slic3r { namespace csg {

namespace detail {

inline void merge_slices(csg::CSGType op, size_t i,
                  std::vector<ExPolygons> &target,
                  std::vector<ExPolygons> &source)
{
    switch(op) {
    case CSGType::Union:
        for (ExPolygon &expoly : source[i])
            target[i].emplace_back(std::move(expoly));
        break;
    case CSGType::Difference:
        target[i] = diff_ex(target[i], source[i]);
        break;
    case CSGType::Intersection:
        target[i] = intersection_ex(target[i], source[i]);
        break;
    }
}

inline void collect_nonempty_indices(csg::CSGType                   op,
                              const std::vector<float>      &slicegrid,
                              const std::vector<ExPolygons> &slices,
                              std::vector<size_t>           &indices)
{
    indices.clear();
    for (size_t i = 0; i < slicegrid.size(); ++i) {
        if (op == CSGType::Intersection || !slices[i].empty())
            indices.emplace_back(i);
    }
}

} // namespace detail

template<class ItCSG>
std::vector<ExPolygons> slice_csgmesh_ex(
    const Range<ItCSG>          &csgrange,
    const std::vector<float>    &slicegrid,
    const MeshSlicingParamsEx   &params,
    const std::function<void()> &throw_on_cancel = [] {})
{
    using namespace detail;

    struct Frame { CSGType op; std::vector<ExPolygons> slices; };

    std::stack opstack{std::vector<Frame>{}};

    MeshSlicingParamsEx params_cpy = params;
    auto trafo = params.trafo;
    auto nonempty_indices = reserve_vector<size_t>(slicegrid.size());

    opstack.push({CSGType::Union, std::vector<ExPolygons>(slicegrid.size())});

    for (const auto &csgpart : csgrange) {
        const indexed_triangle_set *its = csg::get_mesh(csgpart);

        auto op = get_operation(csgpart);

        if (get_stack_operation(csgpart) == CSGStackOp::Push) {
            opstack.push({op, std::vector<ExPolygons>(slicegrid.size())});
            op = CSGType::Union;
        }

        Frame *top = &opstack.top();

        if (its) {
            params_cpy.trafo = trafo * csg::get_transform(csgpart).template cast<double>();
            std::vector<ExPolygons> slices = slice_mesh_ex(*its,
                                                           slicegrid, params_cpy,
                                                           throw_on_cancel);

            assert(slices.size() == slicegrid.size());

            collect_nonempty_indices(op, slicegrid, slices, nonempty_indices);

            execution::for_each(
                ex_tbb, nonempty_indices.begin(), nonempty_indices.end(),
                [op, &slices, &top](size_t i) {
                    merge_slices(op, i, top->slices, slices);
                }, execution::max_concurrency(ex_tbb));
        }

        if (get_stack_operation(csgpart) == CSGStackOp::Pop) {
            std::vector<ExPolygons> popslices = std::move(top->slices);
            auto popop = opstack.top().op;
            opstack.pop();
            std::vector<ExPolygons> &prev_slices = opstack.top().slices;

            collect_nonempty_indices(popop, slicegrid, popslices, nonempty_indices);

            execution::for_each(
                ex_tbb, nonempty_indices.begin(), nonempty_indices.end(),
                [&popslices, &prev_slices, popop](size_t i) {
                    merge_slices(popop, i, prev_slices, popslices);
                }, execution::max_concurrency(ex_tbb));
        }
    }

    std::vector<ExPolygons> ret = std::move(opstack.top().slices);

    // TODO: verify if this part can be omitted or not.
    execution::for_each(ex_tbb, ret.begin(), ret.end(), [](ExPolygons &slice) {
        auto it = std::remove_if(slice.begin(), slice.end(), [](const ExPolygon &p){
            return p.area() < double(SCALED_EPSILON) * double(SCALED_EPSILON);
        });

        // Hopefully, ExPolygons are moved, not copied to new positions
        // and that is cheap for expolygons
        slice.erase(it, slice.end());
        slice = union_ex(slice);
    }, execution::max_concurrency(ex_tbb));

    return ret;
}

}} // namespace Slic3r::csg

#endif // SLICECSGMESH_HPP
