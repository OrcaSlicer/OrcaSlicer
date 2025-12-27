#ifndef slic3r_GUI_Geometry_hpp_
#define slic3r_GUI_Geometry_hpp_

namespace Slic3r {
namespace GUI {

enum class ECoordinatesType : unsigned char
{
    World = 0,
    Instance,
    Local
};

class TransformationType
{
public:
    enum Enum {
        // Transforming in a world coordinate system
        World = 0,
        // Transforming in a instance coordinate system
        Instance = 1,
        // Transforming in a local coordinate system
        Local = 2,
        // Absolute transformations, allowed in local coordinate system only.
        Absolute = 0,
        // Relative transformations, allowed in both local and world coordinate system.
        Relative = 4,
        // For group selection, the transformation is performed as if the group made a single solid body.
        Joint = 0,
        // For group selection, the transformation is performed on each object independently.
        Independent = 8,

        World_Relative_Joint = World | Relative | Joint,
        World_Relative_Independent = World | Relative | Independent,
        Instance_Absolute_Joint = Instance | Absolute | Joint,
        Instance_Absolute_Independent = Instance | Absolute | Independent,
        Instance_Relative_Joint = Instance | Relative | Joint,
        Instance_Relative_Independent = Instance | Relative | Independent,
        Local_Absolute_Joint = Local | Absolute | Joint,
        Local_Absolute_Independent = Local | Absolute | Independent,
        Local_Relative_Joint = Local | Relative | Joint,
        Local_Relative_Independent = Local | Relative | Independent,
    };

    TransformationType() : m_value(World) {}
    TransformationType(Enum value) : m_value(value) {}
    TransformationType& operator=(Enum value) { m_value = value; return *this; }

    Enum operator()() const { return m_value; }
    bool has(Enum v) const { return ((unsigned int)m_value & (unsigned int)v) != 0; }

    void set_world()      { this->remove(Instance); this->remove(Local); }
    void set_instance()   { this->remove(Local); this->add(Instance); }
    void set_local()      { this->remove(Instance); this->add(Local); }
    void set_absolute()    { this->remove(Relative); }
    void set_relative()    { this->add(Relative); }
    void set_joint()       { this->remove(Independent); }
    void set_independent() { this->add(Independent); }

    bool world()        const { return !this->has(Instance) && !this->has(Local); }
    bool instance()     const { return this->has(Instance); }
    bool local()        const { return this->has(Local); }
    bool absolute()     const { return !this->has(Relative); }
    bool relative()     const { return this->has(Relative); }
    bool joint()        const { return !this->has(Independent); }
    bool independent()  const { return this->has(Independent); }

private:
    void add(Enum v)    { m_value = Enum((unsigned int)m_value | (unsigned int)v); }
    void remove(Enum v) { m_value = Enum((unsigned int)m_value & (~(unsigned int)v)); }

    Enum    m_value;
};

} // namespace Slic3r
} // namespace GUI

#endif // slic3r_GUI_Geometry_hpp_
