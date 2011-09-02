#include "../../fast_winding_number.h"
#include "../../octree.h"
#include "../../knn.h"
#include "../../parallel_for.h"
#include "point_areas.h"
#include <vector>

namespace igl {
  namespace copyleft{
    namespace cgal{
      template <typename DerivedP, typename DerivedN, typename DerivedQ,
        typename BetaType, typename DerivedWN>
      IGL_INLINE void fast_winding_number(const Eigen::MatrixBase<DerivedP>& P,
                                          const Eigen::MatrixBase<DerivedN>& N,
                                          const Eigen::MatrixBase<DerivedQ>& Q,
                                          const int expansion_order,
                                          const BetaType beta,
                                          Eigen::PlainObjectBase<DerivedWN>& WN
                                          ){
        typedef typename DerivedWN::Scalar real;
        typedef typename Eigen::Matrix<real,Eigen::Dynamic,Eigen::Dynamic>
                                                                  RealMatrix;
        
        std::vector<std::vector<int> > point_indices;
        Eigen::Matrix<int,Eigen::Dynamic,8> CH;
        Eigen::Matrix<real,Eigen::Dynamic,3> CN;
        Eigen::Matrix<real,Eigen::Dynamic,1> W;
        Eigen::MatrixXi I;
        Eigen::Matrix<real,Eigen::Dynamic,1> A;
        
        octree(P,point_indices,CH,CN,W);
        knn(P,21,point_indices,CH,CN,W,I);
        point_areas(P,I,N,A);
        
        Eigen::Matrix<real,Eigen::Dynamic,Eigen::Dynamic> EC;
        Eigen::Matrix<real,Eigen::Dynamic,3> CM;
        Eigen::Matrix<real,Eigen::Dynamic,1> R;
        
        igl::fast_winding_number(P,N,A,point_indices,CH,
                                 expansion_order,CM,R,EC);
        igl::fast_winding_number(P,N,A,point_indices,CH,
                                 CM,R,EC,Q,beta,WN);
      }
      
      template <typename DerivedP, typename DerivedN,
        typename DerivedQ, typename DerivedWN>
      IGL_INLINE void fast_winding_number(
                                          const Eigen::MatrixBase<DerivedP>& P,
                                          const Eigen::MatrixBase<DerivedN>& N,
                                          const Eigen::MatrixBase<DerivedQ>& Q,
                                          Eigen::PlainObjectBase<DerivedWN>& WN
                                          ){
        fast_winding_number(P,N,Q,2,2.0,WN);
      }
    }
  }
}

























