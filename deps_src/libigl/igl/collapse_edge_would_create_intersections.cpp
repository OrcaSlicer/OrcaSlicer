#include "collapse_edge_would_create_intersections.h"
#include "AABB.h"
#include "circulation.h"
#include "writePLY.h"
#include "triangle_triangle_intersect.h"
#include <Eigen/Geometry>
#include <vector>
#include <iostream>
#include <algorithm>
#include <cassert>

template <
  typename Derivedp,
  typename DerivedV,
  typename DerivedF,
  typename DerivedE,
  typename DerivedEMAP,
  typename DerivedEF,
  typename DerivedEI>
IGL_INLINE bool igl::collapse_edge_would_create_intersections(
  const int e,
  const Eigen::MatrixBase<Derivedp> & p,
  const Eigen::MatrixBase<DerivedV> & V,
  const Eigen::MatrixBase<DerivedF> & F,
  const Eigen::MatrixBase<DerivedE> & E,
  const Eigen::MatrixBase<DerivedEMAP> & EMAP,
  const Eigen::MatrixBase<DerivedEF> & EF,
  const Eigen::MatrixBase<DerivedEI> & EI,
  const igl::AABB<DerivedV,3> & tree,
  const int inf_face_id)
{
  // Merge two lists of integers
  const auto merge = [&](
    const std::vector<int> & A, const std::vector<int> & B)->
    std::vector<int>
  {
    std::vector<int> C;
    C.reserve( A.size() + B.size() ); // preallocate memory
    C.insert( C.end(), A.begin(), A.end() );
    C.insert( C.end(), B.begin(), B.end() );
    // https://stackoverflow.com/a/1041939/148668
    std::sort( C.begin(), C.end() );
    C.erase( std::unique( C.begin(), C.end() ), C.end() );
    return C;
  };

  std::vector<int> old_one_ring;
  {
    std::vector<int> Nsv,Nsf,Ndv,Ndf;
    igl::circulation(e, true,F,EMAP,EF,EI,Nsv,Nsf);
    igl::circulation(e,false,F,EMAP,EF,EI,Ndv,Ndf);
    old_one_ring = merge(Nsf,Ndf);
  }
  int f1 = EF(e,0);
  int f2 = EF(e,1);
  std::vector<int> new_one_ring = old_one_ring;
  // erase if ==f1 or ==f2
  new_one_ring.erase(
    std::remove(new_one_ring.begin(), new_one_ring.end(), f1), 
    new_one_ring.end());
  new_one_ring.erase(
    std::remove(new_one_ring.begin(), new_one_ring.end(), f2), 
    new_one_ring.end());


  // big box containing new_one_ring
  Eigen::AlignedBox<double,3> big_box;
  // Extend box by placement point
  big_box.extend(p.transpose());
  // Extend box by all other corners (skipping old edge vertices)
  for(const auto f : new_one_ring)
  {
    Eigen::RowVector3d corners[3];
    for(int c = 0;c<3;c++)
    {
      if(F(f,c) == E(e,0) || F(f,c) == E(e,1))
      {
        corners[c] = p;
      }else
      {
        corners[c] = V.row(F(f,c));
        big_box.extend(V.row(F(f,c)).transpose());
      }
    }
    // Degenerate triangles are considered intersections
    if((corners[0]-corners[1]).cross(corners[0]-corners[2]).squaredNorm() < 1e-16)
    {
      return true;
    }
  }
  

  std::vector<const igl::AABB<Eigen::MatrixXd,3>*> candidates;
  tree.append_intersecting_leaves(big_box,candidates);
  

  // Exclude any candidates that are in old_one_ring.
  // consider using unordered_set above so that this is O(n+m) rather than O(nm)
  candidates.erase(
    std::remove_if(candidates.begin(), candidates.end(),
        [&](const igl::AABB<Eigen::MatrixXd,3>* candidate) {
            return std::find(old_one_ring.begin(), old_one_ring.end(), candidate->m_primitive) != old_one_ring.end();
        }),
    candidates.end());
  // print candidates
  //const bool stinker = e==2581;
  constexpr bool stinker = false;
  if(stinker)
  {
    igl::writePLY("before.ply",V,F);
    std::cout<<"Ee = ["<<E(e,0)<<" "<<E(e,1)<<"]+1;"<<std::endl;
    std::cout<<"p = ["<<p<<"];"<<std::endl;
    // print new_one_ring as matlab vector of indices
    std::cout<<"new_one_ring = [";
    for(const auto f : new_one_ring)
    {
      std::cout<<f<<" ";
    }
    std::cout<<"]+1;"<<std::endl;
    // print candidates as matlab vector of indices
    std::cout<<"candidates = [";
    for(const auto * candidate : candidates)
    {
      std::cout<<candidate->m_primitive<<" ";
    }
    std::cout<<"]+1;"<<std::endl;
  }
  
  // For each pair of candidate and new_one_ring, check if they intersect
  bool found_intersection = false;
  for(const int & f : new_one_ring)
  {
    if(inf_face_id >= 0 && f >= inf_face_id) { continue; }

    Eigen::AlignedBox<double,3> small_box;
    small_box.extend(p.transpose());
    for(int c = 0;c<3;c++)
    {
      if(F(f,c) != E(e,0) && F(f,c) != E(e,1))
      {
        small_box.extend(V.row(F(f,c)).transpose());
      }
    }
    for(const auto * candidate : candidates)
    {
      const int g = candidate->m_primitive;
      //constexpr bool inner_stinker = false;
      const bool inner_stinker = stinker && (f==1492 && g==1554);
      if(inner_stinker){ printf("  f: %d g: %d\n",f,g); }
      if(!small_box.intersects(candidate->m_box))
      {
        if(inner_stinker){ printf("  ✅ boxes don't overlap\n"); }
        continue;
      }
      // Corner replaced by p
      int c;
      for(c = 0;c<3;c++)
      {
        if(F(f,c) == E(e,0) || F(f,c) == E(e,1))
        {
          break;
        }
      }
      assert(c<3);
      found_intersection = triangle_triangle_intersect(V,F,E,EMAP,EF,f,c,p,g);
      if(found_intersection) { break; }
    }
    if(found_intersection) { break; }
  }
  return found_intersection;
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template bool igl::collapse_edge_would_create_intersections<Eigen::Matrix<double, 1, -1, 1, 1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>>(int, Eigen::MatrixBase<Eigen::Matrix<double, 1, -1, 1, 1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 1, 0, -1, 1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1>> const&, igl::AABB<Eigen::Matrix<double, -1, -1, 0, -1, -1>, 3> const&, int);
#endif
