#ifndef slic3r_Format_STL_hpp_
#define slic3r_Format_STL_hpp_

#include <admesh/stl.h>

namespace Slic3r {

class Model;
class TriangleMesh;
class ModelObject;

// Load an STL file into a provided model.
extern bool load_stl(const char *path, Model *model, const char *object_name = nullptr, ImportstlProgressFn stlFn = nullptr, int custom_header_length = 80);

extern bool store_stl(const char *path, TriangleMesh *mesh, bool binary);
extern bool store_stl(const char *path, ModelObject *model_object, bool binary);
extern bool store_stl(const char *path, Model *model, bool binary);

}; // namespace Slic3r

#endif /* slic3r_Format_STL_hpp_ */
