#ifndef IGL_COPYLEFT_CGAL_MESH_BOOLEAN_TYPE_TO_FUNCS_H
#define IGL_COPYLEFT_CGAL_MESH_BOOLEAN_TYPE_TO_FUNCS_H

#include "../../igl_inline.h"
#include "../../MeshBooleanType.h"
#include <Eigen/Core>
#include <functional>
namespace igl
{
  namespace copyleft
  {
    namespace cgal
    {
      // Convert a MeshBooleanType enum to a pair of winding number conversion
      // function and "keep" function used by mesh_boolean
      //
      // Inputs:
      //   type  MeshBooleanType enum value
      // Outputs:
      //    wind_num_op  function handle for filtering winding numbers from
      //      tuples of integer values to [0,1] outside/inside values
      //    keep  function handle for determining if a patch should be "kept"
      //      in the output based on the winding number on either side
      //
      // See also: string_to_mesh_boolean_type
      IGL_INLINE void mesh_boolean_type_to_funcs(
        const MeshBooleanType & type,
        std::function<int(const Eigen::Matrix<int,1,Eigen::Dynamic>) >& 
          wind_num_op,
        std::function<int(const int, const int)> & keep);
    }
  }
}
#ifndef IGL_STATIC_LIBRARY
#  include "mesh_boolean_type_to_funcs.cpp"
#endif
#endif
