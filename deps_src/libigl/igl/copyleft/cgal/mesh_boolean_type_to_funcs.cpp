#include "mesh_boolean_type_to_funcs.h"
#include "BinaryWindingNumberOperations.h"

IGL_INLINE void igl::copyleft::cgal::mesh_boolean_type_to_funcs(
  const MeshBooleanType & type,
  std::function<int(const Eigen::Matrix<int,1,Eigen::Dynamic>) >& wind_num_op,
  std::function<int(const int, const int)> & keep)
{
  switch (type) 
  {
    case MESH_BOOLEAN_TYPE_UNION:
      wind_num_op = BinaryUnion();
      keep = KeepInside();
      return;
    case MESH_BOOLEAN_TYPE_INTERSECT:
      wind_num_op = BinaryIntersect();
      keep = KeepInside();
      return;
    case MESH_BOOLEAN_TYPE_MINUS:
      wind_num_op = BinaryMinus();
      keep = KeepInside();
      return;
    case MESH_BOOLEAN_TYPE_XOR:
      wind_num_op = BinaryXor();
      keep = KeepInside();
      return;
    case MESH_BOOLEAN_TYPE_RESOLVE:
      wind_num_op = BinaryResolve();
      keep = KeepAll();
      return;
    default:
      assert(false && "Unsupported boolean type.");
      return;
  }
}

#ifdef IGL_STATIC_LIBRARY
// Explicit template instantiation
#endif
