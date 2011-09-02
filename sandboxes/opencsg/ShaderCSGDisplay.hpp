#ifndef SHADERCSGDISPLAY_HPP
#define SHADERCSGDISPLAY_HPP

#include "Engine.hpp"

namespace Slic3r { namespace GL {

class CSGVolume: public Volume
{
    // Extend...    
};

class ShaderCSGDisplay: public Display {
protected:
    vector<std::shared_ptr<CSGVolume>> m_volumes;
    
    void add_mesh(const TriangleMesh &mesh);
public:
    
    void render_scene() override;
    
    void on_scene_updated(const Scene &scene) override;
};

}}

#endif // SHADERCSGDISPLAY_HPP
