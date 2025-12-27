// This file is part of libigl, a simple c++ geometry processing library.
// 
// Copyright (C) 2015 Qingnan Zhou <qnzhou@gmail.com>
// 
// This Source Code Form is subject to the terms of the Mozilla Public License 
// v. 2.0. If a copy of the MPL was not distributed with this file, You can 
// obtain one at http://mozilla.org/MPL/2.0/.
#include "outer_element.h"
#include <iostream>
#include <vector>

template <
     typename DerivedV,
     typename DerivedF,
     typename DerivedI,
     typename IndexType,
     typename DerivedA
     >
IGL_INLINE void igl::outer_vertex(
        const Eigen::PlainObjectBase<DerivedV> & V,
        const Eigen::PlainObjectBase<DerivedF> & F,
        const Eigen::PlainObjectBase<DerivedI> & I,
        IndexType & v_index,
        Eigen::PlainObjectBase<DerivedA> & A)
{
    // Algorithm: 
    //    Find an outer vertex (i.e. vertex reachable from infinity)
    //    Return the vertex with the largest X value.
    //    If there is a tie, pick the one with largest Y value.
    //    If there is still a tie, pick the one with the largest Z value.
    //    If there is still a tie, then there are duplicated vertices within the
    //    mesh, which violates the precondition.
    typedef typename DerivedF::Scalar Index;
    const Index INVALID = std::numeric_limits<Index>::max();
    const size_t num_selected_faces = I.rows();
    std::vector<size_t> candidate_faces;
    Index outer_vid = INVALID;
    typename DerivedV::Scalar outer_val = 0;
    for (size_t i=0; i<num_selected_faces; i++)
    {
        size_t f = I(i);
        for (size_t j=0; j<3; j++)
        {
            Index v = F(f, j);
            auto vx = V(v, 0);
            if (outer_vid == INVALID || vx > outer_val)
            {
                outer_val = vx;
                outer_vid = v;
                candidate_faces = {f};
            } else if (v == outer_vid)
            {
                candidate_faces.push_back(f);
            } else if (vx == outer_val)
            {
                // Break tie.
                auto vy = V(v,1);
                auto vz = V(v, 2);
                auto outer_y = V(outer_vid, 1);
                auto outer_z = V(outer_vid, 2);
                assert(!(vy == outer_y && vz == outer_z));
                bool replace = (vy > outer_y) ||
                    ((vy == outer_y) && (vz > outer_z));
                if (replace)
                {
                    outer_val = vx;
                    outer_vid = v;
                    candidate_faces = {f};
                }
            }
        }
    }

    assert(outer_vid != INVALID);
    assert(candidate_faces.size() > 0);
    v_index = outer_vid;
    A.resize(candidate_faces.size());
    std::copy(candidate_faces.begin(), candidate_faces.end(), A.data());
}

template<
    typename DerivedV,
    typename DerivedF,
    typename DerivedI,
    typename IndexType,
    typename DerivedA
    >
IGL_INLINE void igl::outer_edge(
        const Eigen::PlainObjectBase<DerivedV> & V,
        const Eigen::PlainObjectBase<DerivedF> & F,
        const Eigen::PlainObjectBase<DerivedI> & I,
        IndexType & v1,
        IndexType & v2,
        Eigen::PlainObjectBase<DerivedA> & A) {
    // Algorithm:
    //    Find an outer vertex first.
    //    Find the incident edge with largest abs slope when projected onto XY plane.
    //    If there is a tie, check the signed slope and use the positive one.
    //    If there is still a tie, break it using the projected slope onto ZX plane.
    //    If there is still a tie, again check the signed slope and use the positive one.
    //    If there is still a tie, then there are multiple overlapping edges,
    //    which violates the precondition.
    typedef typename DerivedV::Scalar Scalar;
    typedef typename DerivedV::Index Index;
    typedef typename Eigen::Matrix<Scalar, 3, 1> ScalarArray3;
    typedef typename Eigen::Matrix<typename DerivedF::Scalar, 3, 1> IndexArray3;
    const Index INVALID = std::numeric_limits<Index>::max();

    Index outer_vid;
    Eigen::Matrix<Index,Eigen::Dynamic,1> candidate_faces;
    outer_vertex(V, F, I, outer_vid, candidate_faces);
    const ScalarArray3& outer_v = V.row(outer_vid);
    assert(candidate_faces.size() > 0);

    auto get_vertex_index = [&](const IndexArray3& f, Index vid) -> Index 
    {
        if (f[0] == vid) return 0;
        if (f[1] == vid) return 1;
        if (f[2] == vid) return 2;
        assert(false);
        return -1;
    };

    auto unsigned_value = [](Scalar v) -> Scalar {
        if (v < 0) return v * -1;
        else return v;
    };

    Scalar outer_slope_YX = 0;
    Scalar outer_slope_ZX = 0;
    Index outer_opp_vid = INVALID;
    bool infinite_slope_detected = false;
    std::vector<Index> incident_faces;
    auto check_and_update_outer_edge = [&](Index opp_vid, Index fid) {
        if (opp_vid == outer_opp_vid)
        {
            incident_faces.push_back(fid);
            return;
        }

        const ScalarArray3 opp_v = V.row(opp_vid);
        if (!infinite_slope_detected && outer_v[0] != opp_v[0])
        {
            // Finite slope
            const ScalarArray3 diff = opp_v - outer_v;
            const Scalar slope_YX = diff[1] / diff[0];
            const Scalar slope_ZX = diff[2] / diff[0];
            const Scalar u_slope_YX = unsigned_value(slope_YX);
            const Scalar u_slope_ZX = unsigned_value(slope_ZX);
            bool update = false;
            if (outer_opp_vid == INVALID) {
                update = true;
            } else {
                const Scalar u_outer_slope_YX = unsigned_value(outer_slope_YX);
                if (u_slope_YX > u_outer_slope_YX) {
                    update = true;
                } else if (u_slope_YX == u_outer_slope_YX &&
                        slope_YX > outer_slope_YX) {
                    update = true;
                } else if (slope_YX == outer_slope_YX) {
                    const Scalar u_outer_slope_ZX =
                        unsigned_value(outer_slope_ZX);
                    if (u_slope_ZX > u_outer_slope_ZX) {
                        update = true;
                    } else if (u_slope_ZX == u_outer_slope_ZX &&
                            slope_ZX > outer_slope_ZX) {
                        update = true;
                    } else if (slope_ZX == u_outer_slope_ZX) {
                        assert(false);
                    }
                }
            }

            if (update) {
                outer_opp_vid = opp_vid;
                outer_slope_YX = slope_YX;
                outer_slope_ZX = slope_ZX;
                incident_faces = {fid};
            }
        } else if (!infinite_slope_detected)
        {
            // Infinite slope
            outer_opp_vid = opp_vid;
            infinite_slope_detected = true;
            incident_faces = {fid};
        }
    };

    const size_t num_candidate_faces = candidate_faces.size();
    for (size_t i=0; i<num_candidate_faces; i++)
    {
        const Index fid = candidate_faces(i);
        const IndexArray3& f = F.row(fid);
        size_t id = get_vertex_index(f, outer_vid);
        Index next_vid = f((id+1)%3);
        Index prev_vid = f((id+2)%3);
        check_and_update_outer_edge(next_vid, fid);
        check_and_update_outer_edge(prev_vid, fid);
    }

    v1 = outer_vid;
    v2 = outer_opp_vid;
    A.resize(incident_faces.size());
    std::copy(incident_faces.begin(), incident_faces.end(), A.data());
}

template<
    typename DerivedV,
    typename DerivedF,
    typename DerivedN,
    typename DerivedI,
    typename IndexType
    >
IGL_INLINE void igl::outer_facet(
        const Eigen::PlainObjectBase<DerivedV> & V,
        const Eigen::PlainObjectBase<DerivedF> & F,
        const Eigen::PlainObjectBase<DerivedN> & N,
        const Eigen::PlainObjectBase<DerivedI> & I,
        IndexType & f,
        bool & flipped) {
    // Algorithm:
    //    Find an outer edge.
    //    Find the incident facet with the largest absolute X normal component.
    //    If there is a tie, keep the one with positive X component.
    //    If there is still a tie, pick the face with the larger signed index
    //    (flipped face has negative index).
    typedef typename DerivedV::Scalar Scalar;
    typedef typename DerivedV::Index Index;
    const size_t INVALID = std::numeric_limits<size_t>::max();

    Index v1,v2;
    Eigen::Matrix<Index,Eigen::Dynamic,1> incident_faces;
    outer_edge(V, F, I, v1, v2, incident_faces);
    assert(incident_faces.size() > 0);

    auto generic_fabs = [&](const Scalar& val) -> const Scalar {
        if (val >= 0) return val;
        else return -val;
    };

    Scalar max_nx = 0;
    size_t outer_fid = INVALID;
    const size_t num_incident_faces = incident_faces.size();
    for (size_t i=0; i<num_incident_faces; i++) 
    {
        const auto& fid = incident_faces(i);
        const Scalar nx = N(fid, 0);
        if (outer_fid == INVALID) {
            max_nx = nx;
            outer_fid = fid;
        } else {
            if (generic_fabs(nx) > generic_fabs(max_nx)) {
                max_nx = nx;
                outer_fid = fid;
            } else if (nx == -max_nx && nx > 0) {
                max_nx = nx;
                outer_fid = fid;
            } else if (nx == max_nx) {
                if ((max_nx >= 0 && outer_fid < fid) ||
                    (max_nx <  0 && outer_fid > fid)) {
                    max_nx = nx;
                    outer_fid = fid;
                }
            }
        }
    }

    assert(outer_fid != INVALID);
    f = outer_fid;
    flipped = max_nx < 0;
}


#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::outer_facet<Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<long, -1, 1, 0, -1, 1>, int>(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<long, -1, 1, 0, -1, 1> > const&, int&, bool&);
template void igl::outer_facet<Eigen::Matrix<double, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 1, -1, -1>, Eigen::Matrix<double, -1, -1, 1, -1, -1>, Eigen::Matrix<int, -1, -1, 1, -1, -1>, unsigned long>(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 1, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 1, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 1, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 1, -1, -1> > const&, unsigned long&, bool&);
template void igl::outer_facet<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, int>(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, int&, bool&);
template void igl::outer_facet<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, int>(Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> > const&, int&, bool&);
#endif
