#include "knn.h"
#include "sort.h"
#include "parallel_for.h"

#include <cmath>
#include <queue>
#include <set>
#include <algorithm>

namespace igl {
  template <typename DerivedP, typename IndexType,
  typename DerivedCH, typename DerivedCN, typename DerivedW,
  typename DerivedI>
  IGL_INLINE void knn(const Eigen::MatrixBase<DerivedP>& P,
                      size_t k,
                      const std::vector<std::vector<IndexType> > & point_indices,
                      const Eigen::MatrixBase<DerivedCH>& CH,
                      const Eigen::MatrixBase<DerivedCN>& CN,
                      const Eigen::MatrixBase<DerivedW>& W,
                      Eigen::PlainObjectBase<DerivedI> & I) {
      knn(P,P,k,point_indices,CH,CN,W,I);
  }

  template <typename DerivedP, typename DerivedV, typename IndexType,
  typename DerivedCH, typename DerivedCN, typename DerivedW,
  typename DerivedI>
      IGL_INLINE void knn(
              const Eigen::MatrixBase<DerivedP>& P,
              const Eigen::MatrixBase<DerivedV>& V,
              size_t k,
              const std::vector<std::vector<IndexType> > & point_indices,
              const Eigen::MatrixBase<DerivedCH>& CH,
              const Eigen::MatrixBase<DerivedCN>& CN,
              const Eigen::MatrixBase<DerivedW>& W,
              Eigen::PlainObjectBase<DerivedI> & I) {
    typedef typename DerivedCN::Scalar CentersType;
    typedef typename DerivedW::Scalar WidthsType;

    using Scalar = typename DerivedP::Scalar;
    typedef Eigen::Matrix<Scalar, 1, 3> RowVector3PType;


    const size_t Psize = P.rows();
    const size_t Vsize = V.rows();
    if(Vsize <= k) {
        I.resize(Psize,Vsize);
        for(size_t i = 0; i < Psize; ++i) {
            Eigen::Matrix<Scalar,Eigen::Dynamic,1> D = (V.rowwise() - P.row(i)).rowwise().norm();
            Eigen::Matrix<Scalar,Eigen::Dynamic,1> S;
            Eigen::Vector<typename DerivedI::Scalar,Eigen::Dynamic> R;
            igl::sort(D,1,true,S,R);
            I.row(i) = R.transpose();
        }
        return;
    }

    I.resize(Psize,k);


    auto distance_to_width_one_cube = [](const RowVector3PType& point) -> Scalar {
      return std::sqrt(std::pow<Scalar>(std::max<Scalar>(std::abs(point(0))-1,0.0),2)
                       + std::pow<Scalar>(std::max<Scalar>(std::abs(point(1))-1,0.0),2)
                       + std::pow<Scalar>(std::max<Scalar>(std::abs(point(2))-1,0.0),2));
    };

    auto distance_to_cube = [&distance_to_width_one_cube]
              (const RowVector3PType& point,
               Eigen::Matrix<CentersType,1,3> cube_center,
               WidthsType cube_width) -> Scalar {
      RowVector3PType transformed_point = (point-cube_center)/cube_width;
      return cube_width*distance_to_width_one_cube(transformed_point);
    };


    igl::parallel_for(Psize,[&](size_t i)
    {
      int points_found = 0;
      RowVector3PType point_of_interest = P.row(i);

      //To make my priority queue take both points and octree cells,
      //I use the indices 0 to n-1 for the n points,
      // and the indices n to n+m-1 for the m octree cells

      // Using lambda to compare elements.
      auto cmp = [&point_of_interest, &V, &CN, &W,
                  Vsize, &distance_to_cube](int left, int right) {
        Scalar leftdistance, rightdistance;
        if(left < Vsize){ //left is a point index
          leftdistance = (V.row(left) - point_of_interest).norm();
        } else { //left is an octree cell
          leftdistance = distance_to_cube(point_of_interest,
                                            CN.row(left-Vsize),
                                            W(left-Vsize));
        }

        if(right < Vsize){ //left is a point index
          rightdistance = (V.row(right) - point_of_interest).norm();
        } else { //left is an octree cell
          rightdistance = distance_to_cube(point_of_interest,
                                             CN.row(right-Vsize),
                                             W(right-Vsize));
        }
        return leftdistance > rightdistance;
      };

      std::priority_queue<IndexType, std::vector<IndexType>,
        decltype(cmp)> queue(cmp);

      queue.push(Vsize); //This is the 0th octree cell (ie the root)
      while(points_found < k){
        IndexType curr_cell_or_point = queue.top();
        queue.pop();
        if(curr_cell_or_point < Vsize){ //current index is for is a point
          I(i,points_found) = curr_cell_or_point;
          points_found++;
        } else {
          IndexType curr_cell = curr_cell_or_point - Vsize;
          if(CH(curr_cell,0) == -1){ //In the case of a leaf
            if(point_indices.at(curr_cell).size() > 0){
              //Assumption: Leaves either have one point, or none
              queue.push(point_indices.at(curr_cell).at(0));
            }
          } else { //Not a leaf
            for(int j = 0; j < 8; j++){
              //+n to adjust for the octree cells
              queue.push(CH(curr_cell,j)+Vsize);
            }
          }
        }
      }
    },1000);
  }
}



#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh

template void igl::knn<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, int, Eigen::Matrix<int, -1, 8, 0, -1, 8>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, unsigned long, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, 8, 0, -1, 8> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
template void igl::knn<Eigen::Matrix<double, -1, -1, 0, -1, -1>, int, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1>, Eigen::Matrix<int, -1, -1, 0, -1, -1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, unsigned long, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&);
#ifdef WIN32
template void igl::knn<Eigen::Matrix<double,-1,-1,0,-1,-1>,int,Eigen::Matrix<int,-1,-1,0,-1,-1>,Eigen::Matrix<double,-1,-1,0,-1,-1>,Eigen::Matrix<double,-1,1,0,-1,1>,Eigen::Matrix<int,-1,-1,0,-1,-1> >(Eigen::MatrixBase<Eigen::Matrix<double,-1,-1,0,-1,-1> > const &,unsigned __int64,std::vector<std::vector<int,std::allocator<int> >,std::allocator<std::vector<int,std::allocator<int> > > > const &,Eigen::MatrixBase<Eigen::Matrix<int,-1,-1,0,-1,-1> > const &,Eigen::MatrixBase<Eigen::Matrix<double,-1,-1,0,-1,-1> > const &,Eigen::MatrixBase<Eigen::Matrix<double,-1,1,0,-1,1> > const &,Eigen::PlainObjectBase<Eigen::Matrix<int,-1,-1,0,-1,-1> > &);
template void igl::knn<Eigen::Matrix<double,-1,-1,0,-1,-1>,Eigen::Matrix<double,-1,-1,0,-1,-1>,int,Eigen::Matrix<int,-1,8,0,-1,8>,Eigen::Matrix<double,-1,3,0,-1,3>,Eigen::Matrix<double,-1,1,0,-1,1>,Eigen::Matrix<int,-1,-1,0,-1,-1> >(Eigen::MatrixBase<Eigen::Matrix<double,-1,-1,0,-1,-1> > const &,Eigen::MatrixBase<Eigen::Matrix<double,-1,-1,0,-1,-1> > const &,unsigned __int64,std::vector<std::vector<int,std::allocator<int> >,std::allocator<std::vector<int,std::allocator<int> > > > const &,Eigen::MatrixBase<Eigen::Matrix<int,-1,8,0,-1,8> > const &,Eigen::MatrixBase<Eigen::Matrix<double,-1,3,0,-1,3> > const &,Eigen::MatrixBase<Eigen::Matrix<double,-1,1,0,-1,1> > const &,Eigen::PlainObjectBase<Eigen::Matrix<int,-1,-1,0,-1,-1> > &);
#endif

#endif
