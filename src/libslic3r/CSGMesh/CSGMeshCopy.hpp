#ifndef CSGMESHCOPY_HPP
#define CSGMESHCOPY_HPP

#include "CSGMesh.hpp"

namespace Slic3r { namespace csg {

// Copy a csg range but for the meshes, only copy the pointers. If the copy
// is made from a CSGPart compatible object, and the pointer is a shared one,
// it will be copied with reference counting.
template<class It, class OutIt>
void copy_csgrange_shallow(const Range<It> &csgrange, OutIt out)
{
    for (const auto &part : csgrange) {
        CSGPart cpy{{},
                    get_operation(part),
                    get_transform(part)};

        cpy.stack_operation = get_stack_operation(part);

        if constexpr (std::is_convertible_v<decltype(part), const CSGPart&>) {
            if (auto shptr = part.its_ptr.get_shared_cpy()) {
                cpy.its_ptr = shptr;
            }
        }

        if (!cpy.its_ptr)
            cpy.its_ptr = AnyPtr<const indexed_triangle_set>{get_mesh(part)};

        *out = std::move(cpy);
        ++out;
    }
}

// Copy the csg range, allocating new meshes
template<class It, class OutIt>
void copy_csgrange_deep(const Range<It> &csgrange, OutIt out)
{
    for (const auto &part : csgrange) {

        CSGPart cpy{{}, get_operation(part), get_transform(part)};

        if (auto meshptr = get_mesh(part)) {
            cpy.its_ptr = std::make_unique<const indexed_triangle_set>(*meshptr);
        }

        cpy.stack_operation = get_stack_operation(part);

        *out = std::move(cpy);
        ++out;
    }
}

template<class ItA, class ItB>
bool is_same(const Range<ItA> &A, const Range<ItB> &B)
{
    bool ret = true;

    size_t s = A.size();

    if (B.size() != s)
        ret = false;

    size_t i = 0;
    auto itA = A.begin();
    auto itB = B.begin();
    for (; ret && i < s; ++itA, ++itB, ++i) {
        ret = ret &&
              get_mesh(*itA) == get_mesh(*itB) &&
              get_operation(*itA) == get_operation(*itB) &&
              get_stack_operation(*itA) == get_stack_operation(*itB) &&
              get_transform(*itA).isApprox(get_transform(*itB));
    }

    return ret;
}

}} // namespace Slic3r::csg

#endif // CSGCOPY_HPP
