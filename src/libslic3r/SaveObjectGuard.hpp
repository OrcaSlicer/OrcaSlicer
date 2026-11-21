#pragma once

namespace Slic3r {

class ModelObject;

class SaveObjectGaurd {
public:
    SaveObjectGaurd(ModelObject& object);
    ~SaveObjectGaurd();
};

void save_object_mesh(ModelObject& object);

} // namespace Slic3r
