// This file is part of libigl, a simple c++ geometry processing library.
//
// Copyright (C) 2020 Alec Jacobson <alecjacobson@gmail.com>
//
// This Source Code Form is subject to the terms of the Mozilla Public License
// v. 2.0. If a copy of the MPL was not distributed with this file, You can
// obtain one at http://mozilla.org/MPL/2.0/.
#include "blue_noise.h"
#include "doublearea.h"
#include "random_points_on_mesh.h"
#include "sortrows.h"
#include "placeholders.h"
#include "PI.h"
#include "get_seconds.h"
#include <unordered_map>
#include <algorithm>
#include <vector>
#include <random>
#include <cstdint>

namespace igl
{
  // It is very important that we use 64bit keys to avoid out of bounds (easy to
  // get to happen with dense samplings (e.g., r = 0.0005*bbd)
  typedef std::int64_t BlueNoiseKeyType;
}

// Helper functions
namespace igl
{
  // Should probably find and replace with less generic name
  //
  // Map 3D subscripts (x,y,z) to unique index (return value)
  //
  // Inputs:
  //   w  side length of w×w×w integer cube lattice
  //   x  subscript along x direction
  //   y  subscript along y direction
  //   z  subscript along z direction
  // Returns index value
  //
  inline BlueNoiseKeyType blue_noise_key(
    const BlueNoiseKeyType w, // pass by copy --> int64_t so that multiplication is OK
    const BlueNoiseKeyType x, // pass by copy --> int64_t so that multiplication is OK
    const BlueNoiseKeyType y, // pass by copy --> int64_t so that multiplication is OK
    const BlueNoiseKeyType z) // pass by copy --> int64_t so that multiplication is OK
  {
    return x+w*(y+w*z);
  }
  // Determine if a query candidate at position X.row(i) is too close to already
  // selected sites (stored in S).
  //
  // Inputs:
  //   X  #X by 3 list of raw candidate positions
  //   Xs  #Xs by 3 list of corresponding integer cell subscripts
  //   i  index of candidate in question
  //   S   map from cell index to index into X of selected candidate (or -1 if
  //     cell is currently empty)
  //   rr  Poisson disk radius squared
  //   w  side length of w×w×w integer cube lattice (into which Xs subscripts)
  template <
    typename DerivedX,
    typename DerivedXs>
  inline bool blue_noise_far_enough(
    const Eigen::MatrixBase<DerivedX> & X,
    const Eigen::MatrixBase<DerivedXs> & Xs,
    const std::unordered_map<BlueNoiseKeyType,int> & S,
    const double & rr,
    const int & w,
    const int i)
  {
    const int xi = Xs(i,0);
    const int yi = Xs(i,1);
    const int zi = Xs(i,2);
    int g = 2; // ceil(r/s)
    for(int x = std::max(xi-g,0);x<=std::min(xi+g,w-1);x++)
    for(int y = std::max(yi-g,0);y<=std::min(yi+g,w-1);y++)
    for(int z = std::max(zi-g,0);z<=std::min(zi+g,w-1);z++)
    {
      if(x!=xi || y!=yi || z!=zi)
      {
        const BlueNoiseKeyType nk = blue_noise_key(w,x,y,z);
        // have already selected from this cell
        const auto Siter = S.find(nk);
        if(Siter !=S.end() && Siter->second >= 0)
        {
          const int ni = Siter->second;
          // too close
          if( (X.row(i)-X.row(ni)).squaredNorm() < rr)
          {
            return false;
          }
        }
      }
    }
    return true;
  }
  // Try to activate a candidate in a given cell
  //
  // Inputs:
  //   X  #X by 3 list of raw candidate positions
  //   Xs  #Xs by 3 list of corresponding integer cell subscripts
  //   rr  Poisson disk radius squared
  //   w  side length of w×w×w integer cube lattice (into which Xs subscripts)
  //   nk  index of cell in which we'd like to activate a candidate
  //   M   map from cell index to list of candidates
  //   S   map from cell index to index into X of selected candidate (or -1 if
  //     cell is currently empty)
  //   active   list of indices into X of active candidates
  // Outputs:
  //   M  visited candidates deemed too close to already selected points are
  //      removed
  //   S  updated to reflect activated point (if successful)
  //   active   updated to reflect activated point (if successful)
  // Returns true iff activation was successful
  template <
    typename DerivedX,
    typename DerivedXs>
  inline bool activate(
    const Eigen::MatrixBase<DerivedX> & X,
    const Eigen::MatrixBase<DerivedXs> & Xs,
    const double & rr,
    const int & i,
    const int & w,
    const BlueNoiseKeyType & nk,
    std::unordered_map<BlueNoiseKeyType,std::vector<int> > & M,
    std::unordered_map<BlueNoiseKeyType,int> & S,
    std::vector<int> & active)
  {
    assert(M.count(nk));
    auto & Mvec = M.find(nk)->second;
    auto miter = Mvec.begin();
    while(miter != Mvec.end())
    {
      const int mi = *miter;
      // mi is our candidate sample. Is it far enough from all existing
      // samples?
      if(i>=0 && (X.row(i)-X.row(mi)).squaredNorm() > 4.*rr)
      {
        // too far skip (reject)
       miter++;
      } else if(blue_noise_far_enough(X,Xs,S,rr,w,mi))
      {
        active.push_back(mi);
        S.find(nk)->second = mi;
        //printf("  found %d\n",mi);
        return true;
      }else
      {
        // remove forever (instead of incrementing we swap and eat from the
        // back)
        //std::swap(*miter,Mvec.back());
        *miter = Mvec.back();
        bool was_last = (std::next(miter) == Mvec.end());
        Mvec.pop_back();
        if (was_last) {
          // popping from the vector can invalidate the iterator, if it was
          // pointing to the last element that was popped. Alternatively,
          // one could use indices directly...
          miter = Mvec.end();
        }
      }
    }
    return false;
  }

  template <
    typename DerivedX,
    typename DerivedXs,
    typename URBG>
  inline bool step(
    const Eigen::MatrixBase<DerivedX> & X,
    const Eigen::MatrixBase<DerivedXs> & Xs,
    const double & rr,
    const int & w,
    URBG && urbg,
    std::unordered_map<BlueNoiseKeyType,std::vector<int> > & M,
    std::unordered_map<BlueNoiseKeyType,int> & S,
    std::vector<int> & active,
    std::vector<int> & collected
    )
  {
    //considered.clear();
    if(active.size() == 0) return false;
    // random entry
    std::uniform_int_distribution<> dis(0, active.size()-1);
    const int e = dis(urbg);
    const int i = active[e];
    //printf("%d\n",i);
    const int xi = Xs(i,0);
    const int yi = Xs(i,1);
    const int zi = Xs(i,2);
    //printf("%d %d %d - %g %g %g\n",xi,yi,zi,X(i,0),X(i,1),X(i,2));
    // cell indices of neighbors
    int g = 4;
    std::vector<BlueNoiseKeyType> N;N.reserve((1+g*1)^3-1);
    for(int x = std::max(xi-g,0);x<=std::min(xi+g,w-1);x++)
    for(int y = std::max(yi-g,0);y<=std::min(yi+g,w-1);y++)
    for(int z = std::max(zi-g,0);z<=std::min(zi+g,w-1);z++)
    {
      if(x!=xi || y!=yi || z!=zi)
      {
        //printf("  %d %d %d\n",x,y,z);
        const BlueNoiseKeyType nk = blue_noise_key(w,x,y,z);
        // haven't yet selected from this cell?
        const auto Siter = S.find(nk);
        if(Siter !=S.end() && Siter->second < 0)
        {
          assert(M.find(nk) != M.end());
          N.emplace_back(nk);
        }
      }
    }
        //printf("  --------\n");
    // randomize order: this might be a little paranoid...
    std::shuffle(std::begin(N), std::end(N), urbg);
    bool found = false;
    for(const BlueNoiseKeyType & nk : N)
    {
      assert(M.find(nk) != M.end());
      if(activate(X,Xs,rr,i,w,nk,M,S,active))
      {
        found = true;
        break;
      }
    }
    if(!found)
    {
      // remove i from active list
      // https://stackoverflow.com/a/60765833/148668
      collected.push_back(i);
      //printf("  before: "); for(const int j : active) { printf("%d ",j); } printf("\n");
      std::swap(active[e], active.back());
      //printf("  after : "); for(const int j : active) { printf("%d ",j); } printf("\n");
      active.pop_back();
      //printf("  removed %d\n",i);
    }
    //printf("  active: "); for(const int j : active) { printf("%d ",j); } printf("\n");
    return true;
  }
}

template <
  typename DerivedV,
  typename DerivedF,
  typename DerivedB,
  typename DerivedFI,
  typename DerivedP,
  typename URBG>
IGL_INLINE void igl::blue_noise(
    const Eigen::MatrixBase<DerivedV> & V,
    const Eigen::MatrixBase<DerivedF> & F,
    const typename DerivedV::Scalar r,
    Eigen::PlainObjectBase<DerivedB> & B,
    Eigen::PlainObjectBase<DerivedFI> & FI,
    Eigen::PlainObjectBase<DerivedP> & P,
    URBG && urbg)
{
  typedef typename DerivedV::Scalar Scalar;
  // float+RowMajor is faster...
  typedef Eigen::Matrix<Scalar,Eigen::Dynamic,3,Eigen::RowMajor> MatrixX3S;
  assert(V.cols() == 3 && "Only 3D embeddings allowed");
  // minimum radius
  const Scalar min_r = r;
  // cell size based on 3D distance
  // It works reasonably well (but is probably biased to use s=2*r/√3 here and
  // g=1 in the outer loop below.
  //
  // One thing to try would be to store a list in S (rather than a single point)
  // or equivalently a mask over M and just use M as a generic spatial hash
  // (with arbitrary size) and then tune its size (being careful to make g a
  // function of r and s; and removing the `if S=-1 checks`)
  const Scalar s = r/sqrt(3.0);

  const double area =
    [&](){Eigen::VectorXd A;igl::doublearea(V,F,A);return A.array().sum()/2;}();
  // Circle packing in the plane has igl::PI*sqrt(3)/6 efficiency
  const double expected_number_of_points =
    area * (igl::PI * sqrt(3.0) / 6.0) / (igl::PI * min_r * min_r / 4.0);

  // Make a uniform random sampling with 30*expected_number_of_points.
  const int nx = 30.0*expected_number_of_points;
  MatrixX3S X,XB;
  Eigen::VectorXi XFI;
  igl::random_points_on_mesh(nx,V,F,XB,XFI,X,urbg);

  // Rescale so that s = 1
  Eigen::Matrix<int,Eigen::Dynamic,3,Eigen::RowMajor> Xs =
    ((X.rowwise()-X.colwise().minCoeff())/s).template cast<int>();
  const int w = Xs.maxCoeff()+1;
  {
    Eigen::VectorXi I;
    igl::sortrows(decltype(Xs)(Xs),true,Xs,I);
    X = X(I,igl::placeholders::all).eval();
    // These two could be spun off in their own thread.
    XB = XB(I,igl::placeholders::all).eval();
    XFI = XFI(I,igl::placeholders::all).eval();
  }
  // Initialization
  std::unordered_map<BlueNoiseKeyType,std::vector<int> > M;
  std::unordered_map<BlueNoiseKeyType, int > S;
  // attempted to seed
  std::unordered_map<BlueNoiseKeyType, int > A;
  // Q: Too many?
  // A: Seems to help though.
  M.reserve(Xs.rows());
  S.reserve(Xs.rows());
  for(int i = 0;i<Xs.rows();i++)
  {
    BlueNoiseKeyType k = blue_noise_key(w,Xs(i,0),Xs(i,1),Xs(i,2));
    const auto Miter = M.find(k);
    if(Miter  == M.end())
    {
      M.insert({k,{i}});
    }else
    {
      Miter->second.push_back(i);
    }
    S.emplace(k,-1);
    A.emplace(k,false);
  }

  std::vector<int> active;
  // precompute r²
  // Q: is this necessary?
  const double rr = r*r;
  std::vector<int> collected;
  collected.reserve(2.0*expected_number_of_points);

  auto Mouter = M.begin();
  // Just take the first point as the initial seed
  const auto initialize = [&]()->bool
  {
    while(true)
    {
      if(Mouter == M.end())
      {
        return false;
      }
      const BlueNoiseKeyType k = Mouter->first;
      // Haven't placed in this cell yet
      if(S[k]<0)
      {
        if(activate(X,Xs,rr,-1,w,k,M,S,active)) return true;
      }
      Mouter++;
    }
    assert(false && "should not be reachable.");
  };

  // important if mesh contains many connected components
  while(initialize())
  {
    while(active.size()>0)
    {
      step(X,Xs,rr,w,urbg,M,S,active,collected);
    }
  }
  {
    const int n = collected.size();
    P.resize(n,3);
    B.resize(n,3);
    FI.resize(n);
    for(int i = 0;i<n;i++)
    {
      const int c = collected[i];
      P.row(i) = X.row(c).template cast<typename DerivedP::Scalar>();
      B.row(i) = XB.row(c).template cast<typename DerivedB::Scalar>();
      FI(i) = XFI(c);
    }
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
template void igl::blue_noise<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, std::mt19937_64 >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<double, -1, -1, 0, -1, -1>::Scalar, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, std::mt19937_64&&);
template void igl::blue_noise<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, 1, 0, -1, 1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, std::mt19937 >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::Matrix<double, -1, -1, 0, -1, -1>::Scalar, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 1, 0, -1, 1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, std::mt19937&&);
#endif
