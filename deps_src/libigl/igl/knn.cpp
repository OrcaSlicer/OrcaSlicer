#include "knn.h"
#include "parallel_for.h"

#include <cmath>
#include <queue>

namespace igl {
  template <typename DerivedP, typename KType, typename IndexType,
  typename DerivedCH, typename DerivedCN, typename DerivedW,
  typename DerivedI>
  IGL_INLINE void knn(const Eigen::MatrixBase<DerivedP>& P,
                      const KType & k,
                      const std::vector<std::vector<IndexType> > & point_indices,
                      const Eigen::MatrixBase<DerivedCH>& CH,
                      const Eigen::MatrixBase<DerivedCN>& CN,
                      const Eigen::MatrixBase<DerivedW>& W,
                      Eigen::PlainObjectBase<DerivedI> & I)
  {
    typedef typename DerivedCN::Scalar CentersType;
    typedef typename DerivedW::Scalar WidthsType;
    
    typedef Eigen::Matrix<typename DerivedP::Scalar, 1, 3> RowVector3PType;
    
    int n = P.rows();
    const KType real_k = std::min(n,k);
    
    auto distance_to_width_one_cube = [](RowVector3PType point){
      return std::sqrt(std::pow(std::max(std::abs(point(0))-1,0.0),2)
                       + std::pow(std::max(std::abs(point(1))-1,0.0),2)
                       + std::pow(std::max(std::abs(point(2))-1,0.0),2));
    };
    
    auto distance_to_cube = [&distance_to_width_one_cube]
              (RowVector3PType point,
               Eigen::Matrix<CentersType,1,3> cube_center,
               WidthsType cube_width){
      RowVector3PType transformed_point = (point-cube_center)/cube_width;
      return cube_width*distance_to_width_one_cube(transformed_point);
    };
    
    I.resize(n,real_k);
    
    igl::parallel_for(n,[&](int i)
    {
      int points_found = 0;
      RowVector3PType point_of_interest = P.row(i);
      
      //To make my priority queue take both points and octree cells,
      //I use the indices 0 to n-1 for the n points,
      // and the indices n to n+m-1 for the m octree cells
      
      // Using lambda to compare elements.
      auto cmp = [&point_of_interest, &P, &CN, &W,
                  &n, &distance_to_cube](int left, int right) {
        double leftdistance, rightdistance;
        if(left < n){ //left is a point index
          leftdistance = (P.row(left) - point_of_interest).norm();
        } else { //left is an octree cell
          leftdistance = distance_to_cube(point_of_interest,
                                            CN.row(left-n),
                                            W(left-n));
        }
      
        if(right < n){ //left is a point index
          rightdistance = (P.row(right) - point_of_interest).norm();
        } else { //left is an octree cell
          rightdistance = distance_to_cube(point_of_interest,
                                             CN.row(right-n),
                                             W(right-n));
        }
        return leftdistance >= rightdistance;
      };
      
      std::priority_queue<IndexType, std::vector<IndexType>,
        decltype(cmp)> queue(cmp);
      
      queue.push(n); //This is the 0th octree cell (ie the root)
      while(points_found < real_k){
        IndexType curr_cell_or_point = queue.top();
        queue.pop();
        if(curr_cell_or_point < n){ //current index is for is a point
          I(i,points_found) = curr_cell_or_point;
          points_found++;
        } else {
          IndexType curr_cell = curr_cell_or_point - n;
          if(CH(curr_cell,0) == -1){ //In the case of a leaf
            if(point_indices.at(curr_cell).size() > 0){
              //Assumption: Leaves either have one point, or none
              queue.push(point_indices.at(curr_cell).at(0));
            }
          } else { //Not a leaf
            for(int j = 0; j < 8; j++){
              //+n to adjust for the octree cells
              queue.push(CH(curr_cell,j)+n);
            }
          }
        }
      }
    },1000);
  }
}




