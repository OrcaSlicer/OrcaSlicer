#include "point_areas.h"
#include "delaunay_triangulation.h"

#include "../../find.h"
#include "../../parallel_for.h"
#include "../../placeholders.h"

#include "CGAL/Exact_predicates_inexact_constructions_kernel.h"
#include "CGAL/Triangulation_vertex_base_with_info_2.h"
#include "CGAL/Triangulation_data_structure_2.h"
#include "CGAL/Delaunay_triangulation_2.h"



typedef CGAL::Exact_predicates_inexact_constructions_kernel           Kernel;
typedef CGAL::Triangulation_vertex_base_with_info_2<unsigned int, Kernel> Vb;
typedef CGAL::Triangulation_data_structure_2<Vb>                      Tds;
typedef CGAL::Delaunay_triangulation_2<Kernel, Tds>                   Delaunay;
typedef Kernel::Point_2                                               Point;

namespace igl {
  namespace copyleft{
    namespace cgal{
      
      template <typename DerivedP, typename DerivedI, typename DerivedN,
      typename DerivedA>
      IGL_INLINE void point_areas(
                                  const Eigen::MatrixBase<DerivedP>& P,
                                  const Eigen::MatrixBase<DerivedI>& I,
                                  const Eigen::MatrixBase<DerivedN>& N,
                                  Eigen::PlainObjectBase<DerivedA> & A)
      {
        Eigen::MatrixXd T;
        point_areas(P,I,N,A,T);
      }
      
      
      template <typename DerivedP, typename DerivedI, typename DerivedN,
      typename DerivedA, typename DerivedT>
      IGL_INLINE void point_areas(
                                        const Eigen::MatrixBase<DerivedP>& P,
                                        const Eigen::MatrixBase<DerivedI>& I,
                                        const Eigen::MatrixBase<DerivedN>& N,
                                        Eigen::PlainObjectBase<DerivedA> & A,
                                        Eigen::PlainObjectBase<DerivedT> & T)
      {
        typedef typename DerivedP::Scalar real;
        typedef typename DerivedN::Scalar scalarN;
        typedef typename DerivedA::Scalar scalarA;
        typedef Eigen::Matrix<real,1,3> RowVec3;
        
        typedef Eigen::Matrix<real, Eigen::Dynamic, Eigen::Dynamic> MatrixP;
        typedef Eigen::Matrix<scalarN, Eigen::Dynamic, Eigen::Dynamic> MatrixN;
        typedef Eigen::Matrix<typename DerivedI::Scalar,
                  Eigen::Dynamic, Eigen::Dynamic> MatrixI;
        
        
        
        const int n = P.rows();
        
        assert(P.cols() == 3 && "P must have exactly 3 columns");
        assert(P.rows() == N.rows()
               && "P and N must have the same number of rows");
        assert(P.rows() == I.rows()
               && "P and I must have the same number of rows");
        
        A.setZero(n,1);
        T.setZero(n,3);
        igl::parallel_for(P.rows(),[&](int i)
        {
          MatrixP neighbors;
          neighbors = P(I.row(i),igl::placeholders::all);
          if(N.rows() && neighbors.rows() > 1){
            MatrixN neighbor_normals;
            neighbor_normals = N(I.row(i),igl::placeholders::all);
            Eigen::Matrix<scalarN,1,3> poi_normal = neighbor_normals.row(0);
            Eigen::Matrix<scalarN,Eigen::Dynamic,1> dotprod =
                            poi_normal(0)*neighbor_normals.col(0)
            + poi_normal(1)*neighbor_normals.col(1)
            + poi_normal(2)*neighbor_normals.col(2);
            Eigen::Array<bool,Eigen::Dynamic,1> keep = dotprod.array() > 0;
            neighbors = neighbors(igl::find(keep),igl::placeholders::all).eval();
          }
          if(neighbors.rows() <= 2){
            A(i) = 0;
          } else {
            //subtract the mean from neighbors, then take svd,
            //the scores will be U*S. This is our pca plane fitting
            RowVec3 mean = neighbors.colwise().mean();
            MatrixP mean_centered = neighbors.rowwise() - mean;
            Eigen::JacobiSVD<MatrixP> svd(mean_centered,
                                    Eigen::ComputeThinU | Eigen::ComputeThinV);
            MatrixP scores = svd.matrixU() * svd.singularValues().asDiagonal();
            
            T.row(i) = svd.matrixV().col(2).transpose();
            if(T.row(i).dot(N.row(i)) < 0){
              T.row(i) *= -1;
            }
            
            MatrixP plane = scores(igl::placeholders::all,{0,1});
            
            std::vector< std::pair<Point,unsigned> > points;
            //This is where we obtain a delaunay triangulation of the points
            for(unsigned iter = 0; iter < plane.rows(); iter++){
              points.push_back( std::make_pair(
                      Point(plane(iter,0),plane(iter,1)), iter ) );
            }
            Delaunay triangulation;
            triangulation.insert(points.begin(),points.end());
            Eigen::MatrixXi F(triangulation.number_of_faces(),3);
            int f_row = 0;
            for(Delaunay::Finite_faces_iterator fit =
                triangulation.finite_faces_begin();
                fit != triangulation.finite_faces_end(); ++fit) {
              Delaunay::Face_handle face = fit;
              F.row(f_row) = Eigen::RowVector3i((int)face->vertex(0)->info(),
                                                (int)face->vertex(1)->info(),
                                                (int)face->vertex(2)->info());
              f_row++;
            }
            
            //Here we calculate the voronoi area of the point
            scalarA area_accumulator = 0;
            for(int f = 0; f < F.rows(); f++){
              int X = -1;
              for(int face_iter = 0; face_iter < 3; face_iter++){
                if(F(f,face_iter) == 0){
                  X = face_iter;
                }
              }
              if(X >= 0){
              //Triangle XYZ with X being the point we want the area of
                int Y = (X+1)%3;
                int Z = (X+2)%3;
                scalarA x = (plane.row(F(f,Y))-plane.row(F(f,Z))).norm();
                scalarA y = (plane.row(F(f,X))-plane.row(F(f,Z))).norm();
                scalarA z = (plane.row(F(f,Y))-plane.row(F(f,X))).norm();
                scalarA cosX = (z*z + y*y - x*x)/(2*y*z);
                scalarA cosY = (z*z + x*x - y*y)/(2*x*z);
                scalarA cosZ = (x*x + y*y - z*z)/(2*y*x);
                Eigen::Matrix<scalarA,1,3> barycentric;
                barycentric << x*cosX, y*cosY, z*cosZ;
                barycentric /= (barycentric(0)+barycentric(1)+barycentric(2));
                
                //TODO: to make numerically stable, reorder so that x≥y≥z:
                scalarA full_area = 0.25*std::sqrt(
                                    (x+(y+z))*(z-(x-y))*(z+(x-y))*(x+(y-z)));
                Eigen::Matrix<scalarA,1,3> partial_area =
                                                    barycentric * full_area;
                if(cosX < 0){
                  area_accumulator += 0.5*full_area;
                } else if (cosY < 0 || cosZ < 0){
                  area_accumulator += 0.25*full_area;
                } else {
                  area_accumulator += (partial_area(1) + partial_area(2))/2;
                }
              }
            }
            
            if(std::isfinite(area_accumulator)){
              A(i) = area_accumulator;
            } else {
              A(i) = 0;
            }
          }
        },1000);
      }
    }
  }
}




#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template void igl::copyleft::cgal::point_areas<Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> > const&, Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
#endif
