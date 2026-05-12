#include "octree.h"
#include <vector>

namespace igl {
  template <typename DerivedP, typename IndexType, typename DerivedCH,
    typename DerivedCN, typename DerivedW>
  IGL_INLINE void octree(const Eigen::MatrixBase<DerivedP>& P,
                         std::vector<std::vector<IndexType> > & point_indices,
                         Eigen::PlainObjectBase<DerivedCH>& CH,
                         Eigen::PlainObjectBase<DerivedCN>& CN,
                         Eigen::PlainObjectBase<DerivedW>& W)
  {
    
    
    
    const int MAX_DEPTH = 30000;

    typedef typename DerivedCH::Scalar ChildrenType;
    typedef typename DerivedCN::Scalar CentersType;
    typedef typename DerivedW::Scalar WidthsType;
    typedef typename DerivedP::Scalar PointScalar;
    typedef Eigen::Matrix<ChildrenType,8,1> Vector8i;
    typedef Eigen::Matrix<PointScalar, 1, 3> RowVector3PType;
    typedef Eigen::Matrix<CentersType, 1, 3>       RowVector3CentersType;
    
    std::vector<Eigen::Matrix<ChildrenType,8,1>,
        Eigen::aligned_allocator<Eigen::Matrix<ChildrenType,8,1> > > children;
    std::vector<Eigen::Matrix<CentersType,1,3>,
        Eigen::aligned_allocator<Eigen::Matrix<CentersType,1,3> > > centers;
    std::vector<WidthsType> widths;
    
    auto get_octant = [](const RowVector3PType& location,
                         const RowVector3CentersType& center){
      // We use a binary numbering of children. Treating the parent cell's
      // center as the origin, we number the octants in the following manner:
      // The first bit is 1 iff the octant's x coordinate is positive
      // The second bit is 1 iff the octant's y coordinate is positive
      // The third bit is 1 iff the octant's z coordinate is positive
      //
      // For example, the octant with negative x, positive y, positive z is:
      // 110 binary = 6 decimal
      IndexType index = 0;
      if( location(0) >= center(0)){
        index = index + 1;
      }
      if( location(1) >= center(1)){
        index = index + 2;
      }
      if( location(2) >= center(2)){
        index = index + 4;
      }
      return index;
    };

    
    std::function< RowVector3CentersType(const RowVector3CentersType,
                                         const CentersType,
                                         const ChildrenType) >
    translate_center =
        [](const RowVector3CentersType & parent_center,
           const CentersType h,
           const ChildrenType child_index){
      RowVector3CentersType change_vector;
      change_vector << -h,-h,-h;
          
      //positive x chilren are 1,3,4,7
      if(child_index % 2){
        change_vector(0) = h;
      }
      //positive y children are 2,3,6,7
      if(child_index == 2 || child_index == 3 ||
        child_index == 6 || child_index == 7){
        change_vector(1) = h;
      }
      //positive z children are 4,5,6,7
      if(child_index > 3){
        change_vector(2) = h;
      }
      RowVector3CentersType output = parent_center + change_vector;
      return output;
    };
  
    // How many cells do we have so far?
    IndexType m = 0;
  
    // Useful list of number 0..7
    const Vector8i zero_to_seven = (Vector8i()<<0,1,2,3,4,5,6,7).finished();
    const Vector8i neg_ones = Vector8i::Constant(-1);
  
    std::function< void(const ChildrenType, const int) > helper;
    // VSC and clang don't agree on whether MAX_DEPTH needs to be in the capture
    // list.
    helper = [&helper,&translate_center,&get_octant,&m,
              &zero_to_seven,&neg_ones,&P,
              &point_indices,&children,&centers,&widths,&MAX_DEPTH]
    (const ChildrenType index, const int depth)-> void
    {
      if(point_indices.at(index).size() > 1 && depth < MAX_DEPTH){
        //give the parent access to the children
        children.at(index) = zero_to_seven.array() + m;
        //make the children's data in our arrays
      
        //Add the children to the lists, as default children
        CentersType h = widths.at(index)/2;
        RowVector3CentersType curr_center = centers.at(index);
        

        for(ChildrenType i = 0; i < 8; i++){
          children.emplace_back(neg_ones);
          point_indices.emplace_back(std::vector<IndexType>());
          centers.emplace_back(translate_center(curr_center,h/2,i));
          widths.emplace_back(h);
        }

      
        //Split up the points into the corresponding children
        for(int j = 0; j < point_indices.at(index).size(); j++){
          IndexType curr_point_index = point_indices.at(index).at(j);
          IndexType cell_of_curr_point =
            get_octant(P.row(curr_point_index),curr_center)+m;
          point_indices.at(cell_of_curr_point).emplace_back(curr_point_index);
        }
      
        //Now increase m
        m += 8;
        

        // Look ma, I'm calling myself.
        for(int i = 0; i < 8; i++){
          helper(children.at(index)(i),depth+1);
        }
      }
    };
  
    {
      std::vector<IndexType> all(P.rows());
      for(IndexType i = 0;i<all.size();i++) all[i]=i;
      point_indices.emplace_back(all);
    }
    children.emplace_back(neg_ones);
  
    //Get the minimum AABB for the points
    RowVector3PType backleftbottom = P.colwise().minCoeff();
    RowVector3PType frontrighttop = P.colwise().maxCoeff();
    RowVector3CentersType aabb_center = (backleftbottom+frontrighttop)/PointScalar(2.0);
    WidthsType aabb_width = (frontrighttop - backleftbottom).maxCoeff();
    centers.emplace_back( aabb_center );
  
    //Widths are the side length of the cube, (not half the side length):
    widths.emplace_back( aabb_width );
    m++;
    // then you have to actually call the function
    helper(0,0);
    
    //Now convert from vectors to Eigen matricies:
    CH.resize(children.size(),8);
    CN.resize(centers.size(),3);
    W.resize(widths.size(),1);
    
    for(int i = 0; i < children.size(); i++){
      CH.row(i) = children.at(i);
    }
    for(int i = 0; i < centers.size(); i++){
      CN.row(i) = centers.at(i);
    }
    for(int i = 0; i < widths.size(); i++){
      W(i) = widths.at(i);
    }
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
// generated by autoexplicit.sh
template void igl::octree<Eigen::Matrix<double, -1, -1, 0, -1, -1>, int, Eigen::Matrix<int, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, -1, 0, -1, -1>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
template void igl::octree<Eigen::Matrix<double, -1, -1, 0, -1, -1>, int, Eigen::Matrix<int, -1, 8, 0, -1, 8>, Eigen::Matrix<double, -1, 3, 0, -1, 3>, Eigen::Matrix<double, -1, 1, 0, -1, 1> >(Eigen::MatrixBase<Eigen::Matrix<double, -1, -1, 0, -1, -1> > const&, std::vector<std::vector<int, std::allocator<int> >, std::allocator<std::vector<int, std::allocator<int> > > >&, Eigen::PlainObjectBase<Eigen::Matrix<int, -1, 8, 0, -1, 8> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 3, 0, -1, 3> >&, Eigen::PlainObjectBase<Eigen::Matrix<double, -1, 1, 0, -1, 1> >&);
#endif
