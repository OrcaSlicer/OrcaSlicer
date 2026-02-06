#ifndef slic3r_Format_DRC_hpp_
#define slic3r_Format_DRC_hpp_

namespace Slic3r {

#define DRC_BITS_MIN 8
#define DRC_BITS_MAX 30
#define DRC_BITS_DEFAULT 0
#define DRC_BITS_DEFAULT_STR "0"
#define DRC_SPEED_DEFAULT 0

class TriangleMesh;
class ModelObject;
class Model;

// Load a Draco file into a provided model.
extern bool load_drc(const char *path, TriangleMesh *meshptr);
extern bool load_drc(const char *path, Model *model, const char *object_name = nullptr);

extern bool store_drc(const char* path, TriangleMesh* mesh, int bits, int speed = DRC_SPEED_DEFAULT);
extern bool store_drc(const char* path, ModelObject* model_object, int bits, int speed = DRC_SPEED_DEFAULT);
extern bool store_drc(const char* path, Model* model, int bits, int speed = DRC_SPEED_DEFAULT);

}; // namespace Slic3r

#endif /* slic3r_Format_DRC_hpp_ */
