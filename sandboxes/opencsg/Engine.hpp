#ifndef SLIC3R_OCSG_EXMP_ENGINE_HPP
#define SLIC3R_OCSG_EXMP_ENGINE_HPP

#include <vector>
#include <memory>
#include <chrono>

#include <libslic3r/Geometry.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/SLA/Hollowing.hpp>
#include <opencsg/opencsg.h>

namespace Slic3r {

class SLAPrint;

namespace GL {

template<class T, class A = std::allocator<T>> using vector = std::vector<T, A>;

// remove empty weak pointers from a vector
template<class L> inline void cleanup(vector<std::weak_ptr<L>> &listeners) {
    auto it = std::remove_if(listeners.begin(), listeners.end(),
                             [](auto &l) { return !l.lock(); });
    listeners.erase(it, listeners.end());
}

// Call a class method on each element of a vector of objects (weak pointers)
// of the same type.
template<class F, class L, class...Args>
inline void call(F &&f, vector<std::weak_ptr<L>> &listeners, Args&&... args) {
    for (auto &l : listeners)
        if (auto p = l.lock()) ((p.get())->*f)(std::forward<Args>(args)...);
}

// A representation of a mouse input for the engine.
class MouseInput
{
public:
    enum WheelAxis { waVertical, waHorizontal };
    
    // Interface to implement if an object wants to receive notifications
    // about mouse events.
    class Listener {
    public:
        virtual ~Listener();
        
        virtual void on_left_click_down() {}
        virtual void on_left_click_up() {}
        virtual void on_right_click_down() {}
        virtual void on_right_click_up() {}
        virtual void on_double_click() {}
        virtual void on_scroll(long /*v*/, long /*delta*/, WheelAxis ) {}
        virtual void on_moved_to(long /*x*/, long /*y*/) {}
    };
    
private:
    vector<std::weak_ptr<Listener>> m_listeners;
        
public:
    virtual ~MouseInput() = default;

    virtual void left_click_down()
    {
        call(&Listener::on_left_click_down, m_listeners);
    }
    virtual void left_click_up()
    {
        call(&Listener::on_left_click_up, m_listeners);
    }
    virtual void right_click_down()
    {
        call(&Listener::on_right_click_down, m_listeners);
    }
    virtual void right_click_up()
    {
        call(&Listener::on_right_click_up, m_listeners);
    }
    virtual void double_click()
    {
        call(&Listener::on_double_click, m_listeners);
    }
    virtual void scroll(long v, long d, WheelAxis wa)
    {
        call(&Listener::on_scroll, m_listeners, v, d, wa);
    }
    virtual void move_to(long x, long y)
    {
        call(&Listener::on_moved_to, m_listeners, x, y);
    }
    
    void add_listener(std::shared_ptr<Listener> listener)
    {
        m_listeners.emplace_back(listener);
        cleanup(m_listeners);
    }
};

// This is a stripped down version of Slic3r::IndexedVertexArray
class IndexedVertexArray {
public:
    ~IndexedVertexArray() { release_geometry(); }

    // Vertices and their normals, interleaved to be used by void
    // glInterleavedArrays(GL_N3F_V3F, 0, x)
    vector<float> vertices_and_normals_interleaved;
    vector<int>   triangle_indices;
    vector<int>   quad_indices;

    // When the geometry data is loaded into the graphics card as Vertex
    // Buffer Objects, the above mentioned std::vectors are cleared and the
    // following variables keep their original length.
    size_t vertices_and_normals_interleaved_size{ 0 };
    size_t triangle_indices_size{ 0 };
    size_t quad_indices_size{ 0 };
    
    // IDs of the Vertex Array Objects, into which the geometry has been loaded.
    // Zero if the VBOs are not sent to GPU yet.
    unsigned int       vertices_and_normals_interleaved_VBO_id{ 0 };
    unsigned int       triangle_indices_VBO_id{ 0 };
    unsigned int       quad_indices_VBO_id{ 0 };
    
    
    void push_geometry(float x, float y, float z, float nx, float ny, float nz);

    inline void push_geometry(
        double x, double y, double z, double nx, double ny, double nz)
    {
        push_geometry(float(x), float(y), float(z), float(nx), float(ny), float(nz));
    }

    inline void push_geometry(const Vec3d &p, const Vec3d &n)
    {
        push_geometry(p(0), p(1), p(2), n(0), n(1), n(2));
    }

    void push_triangle(int idx1, int idx2, int idx3);
    
    void load_mesh(const TriangleMesh &mesh);

    inline bool has_VBOs() const
    {
        return vertices_and_normals_interleaved_VBO_id != 0;
    }

    // Finalize the initialization of the geometry & indices,
    // upload the geometry and indices to OpenGL VBO objects
    // and shrink the allocated data, possibly relasing it if it has been
    // loaded into the VBOs.
    void finalize_geometry();
    // Release the geometry data, release OpenGL VBOs.
    void release_geometry();
    
    void render() const;
    
    // Is there any geometry data stored?
    bool empty() const { return vertices_and_normals_interleaved_size == 0; }
    
    void clear();
    
    // Shrink the internal storage to tighly fit the data stored.
    void shrink_to_fit();
};

// Try to enable or disable multisampling.
bool enable_multisampling(bool e = true);

class Volume {
    IndexedVertexArray m_geom;
    Geometry::Transformation m_trafo;
    
public:
    
    void render();
    
    void translation(const Vec3d &offset) { m_trafo.set_offset(offset); }
    void rotation(const Vec3d &rot) { m_trafo.set_rotation(rot); }
    void scale(const Vec3d &scaleing) { m_trafo.set_scaling_factor(scaleing); }
    void scale(double s) { scale({s, s, s}); }
    
    inline void load_mesh(const TriangleMesh &mesh)
    {
        m_geom.load_mesh(mesh);
        m_geom.finalize_geometry();
    }
};

// A primitive that can be used with OpenCSG rendering algorithms.
// Does a similar job to GLVolume.
class Primitive : public Volume, public OpenCSG::Primitive
{
public:
    using OpenCSG::Primitive::Primitive;
    
    Primitive() : OpenCSG::Primitive(OpenCSG::Intersection, 1) {}
    
    void render() override { Volume::render(); }
};

// A simple representation of a camera in a 3D scene
class Camera {
protected:
    Vec2f m_rot = {0., 0.};
    Vec3d m_referene = {0., 0., 0.};
    double m_zoom = 0.;
    double m_clip_z = 0.;
public:
    
    virtual ~Camera() = default;
    
    virtual void view();
    virtual void set_screen(long width, long height) = 0;
    
    void set_rotation(const Vec2f &rotation) { m_rot = rotation; }    
    void rotate(const Vec2f &rotation) { m_rot += rotation; }
    void set_zoom(double z) { m_zoom = z; }
    void set_reference_point(const Vec3d &p) { m_referene = p; }
    void set_clip_z(double z) { m_clip_z = z; }
};

// Reset a camera object
inline void reset(Camera &cam)
{
    cam.set_rotation({0., 0.});
    cam.set_zoom(0.);
    cam.set_reference_point({0., 0., 0.});
    cam.set_clip_z(0.);
}

// Specialization of a camera which shows in perspective projection
class PerspectiveCamera: public Camera {
public:
    
    void set_screen(long width, long height) override;
};

// A simple counter of FPS. Subscribed objects will receive updates of the
// current fps.
class FpsCounter {
    vector<std::function<void(double)>> m_listeners;
    
    using Clock = std::chrono::high_resolution_clock;
    using Duration = Clock::duration;
    using TimePoint = Clock::time_point;
    
    int m_frames = 0;
    TimePoint m_last = Clock::now(), m_window = m_last;
    
    double m_resolution = 0.1, m_window_size = 1.0;
    double m_fps = 0.;
    
    static double to_sec(Duration d)
    {
        return d.count() * double(Duration::period::num) / Duration::period::den;
    }    
    
public:
    
    void update();

    void add_listener(std::function<void(double)> lst)
    {
        m_listeners.emplace_back(lst);
    }
    
    void clear_listeners() { m_listeners = {}; }

    void set_notification_interval(double seconds);
    void set_measure_window_size(double seconds);
    
    double get_notification_interval() const { return m_resolution; }
    double get_mesure_window_size() const { return m_window_size; }
};

// Collection of the used OpenCSG library settings.
class CSGSettings {
public:
    static const constexpr unsigned DEFAULT_CONVEXITY = 10;
    
private:
    OpenCSG::Algorithm m_csgalg = OpenCSG::Algorithm::Automatic;
    OpenCSG::DepthComplexityAlgorithm m_depth_algo = OpenCSG::NoDepthComplexitySampling;
    OpenCSG::Optimization m_optim = OpenCSG::OptimizationDefault;
    bool m_enable = true;
    unsigned int m_convexity = DEFAULT_CONVEXITY;
    
public:
    int get_algo() const { return int(m_csgalg); }
    void set_algo(int alg)
    {
        if (alg < OpenCSG::Algorithm::AlgorithmUnused)
            m_csgalg = OpenCSG::Algorithm(alg);
    }
    
    int get_depth_algo() const { return int(m_depth_algo); }
    void set_depth_algo(int alg)
    {
        if (alg < OpenCSG::DepthComplexityAlgorithmUnused)
            m_depth_algo = OpenCSG::DepthComplexityAlgorithm(alg);
    }
    
    int  get_optimization() const { return int(m_optim); }
    void set_optimization(int o)
    {
        if (o < OpenCSG::Optimization::OptimizationUnused)
            m_optim = OpenCSG::Optimization(o);
    }
    
    void enable_csg(bool en = true) { m_enable = en; }
    bool is_enabled() const { return m_enable; }
    
    unsigned get_convexity() const { return m_convexity; }
    void set_convexity(unsigned c) { m_convexity = c; }
};

// The scene is a wrapper around SLAPrint which holds the data to be visualized.
class Scene
{
    std::unique_ptr<SLAPrint> m_print;
public:
    
    // Subscribers will be notified if the model is changed. This might be a
    // display which will have to load the meshes and repaint itself when
    // the scene data changes.
    // eg. We load a new 3mf through the UI, this will notify the controller
    // associated with the scene and all the displays that the controller is
    // connected with.
    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void on_scene_updated(const Scene &scene) = 0;
    };
    
    Scene();
    ~Scene();
    
    void set_print(std::unique_ptr<SLAPrint> &&print);
    const SLAPrint * get_print() const { return m_print.get(); }
    
    BoundingBoxf3 get_bounding_box() const;
    
    void add_listener(std::shared_ptr<Listener> listener)
    {
        m_listeners.emplace_back(listener);
        cleanup(m_listeners);
    }
    
private:
    vector<std::weak_ptr<Listener>> m_listeners;
};

// The basic Display. This is almost just an interface but will do all the
// initialization and show the fps values. Overriding the render_scene is
// needed to show the scene content. The specific method of displaying the
// scene is up the the particular implementation (OpenCSG or other screen space
// boolean algorithms)
class Display : public Scene::Listener
{
protected:
    Vec2i32 m_size;
    bool m_initialized = false;
    
    std::shared_ptr<Camera>  m_camera;
    FpsCounter m_fps_counter;
    
public:
    
    explicit Display(std::shared_ptr<Camera> camera = nullptr)
        : m_camera(camera ? camera : std::make_shared<PerspectiveCamera>())
    {}
    
    ~Display() override;
    
    std::shared_ptr<const Camera> get_camera() const { return m_camera; }
    std::shared_ptr<Camera> get_camera() { return m_camera; }
    void set_camera(std::shared_ptr<Camera> cam) { m_camera = cam; }
    
    virtual void swap_buffers() = 0;
    virtual void set_active(long width, long height);
    virtual void set_screen_size(long width, long height);
    Vec2i32 get_screen_size() const { return m_size; }
    
    virtual void repaint();
    
    bool is_initialized() const { return m_initialized; }
    
    virtual void clear_screen();
    virtual void render_scene() {}
    
    template<class _FpsCounter> void set_fps_counter(_FpsCounter &&fpsc)
    {
        m_fps_counter = std::forward<_FpsCounter>(fpsc);
    }

    const FpsCounter &get_fps_counter() const { return m_fps_counter; }
    FpsCounter &get_fps_counter() { return m_fps_counter; }
};

// Special dispaly using OpenCSG for rendering the scene.
class CSGDisplay : public Display {
protected:
    CSGSettings m_csgsettings;
    
    // Cache the renderable primitives. These will be fetched when the scene
    // is modified.
    struct SceneCache {
        vector<std::shared_ptr<Primitive>> primitives;
        vector<Primitive *> primitives_free;
        vector<OpenCSG::Primitive *> primitives_csg;
        
        void clear();
        
        std::shared_ptr<Primitive> add_mesh(const TriangleMesh &mesh);
        std::shared_ptr<Primitive> add_mesh(const TriangleMesh &mesh,
                                  OpenCSG::Operation  op,
                                  unsigned            covexity);
    } m_scene_cache;
    
public:
    
    // Receive or apply the new settings.
    const CSGSettings & get_csgsettings() const { return m_csgsettings; }
    void apply_csgsettings(const CSGSettings &settings);
    
    void render_scene() override;
    
    void on_scene_updated(const Scene &scene) override;
};


// The controller is a hub which dispatches mouse events to the connected
// displays. It keeps track of the mouse wheel position, the states whether
// the mouse is being held, dragged, etc... All the connected displays will
// mirror the camera movement (if there is more than one display).
class Controller : public std::enable_shared_from_this<Controller>,
                   public MouseInput::Listener,
                   public Scene::Listener
{
    long m_wheel_pos = 0;
    Vec2i32 m_mouse_pos, m_mouse_pos_rprev, m_mouse_pos_lprev;
    bool m_left_btn = false, m_right_btn = false;

    std::shared_ptr<Scene>           m_scene;
    vector<std::weak_ptr<Display>> m_displays;
    
    // Call a method of Camera on all the cameras of the attached displays
    template<class F, class...Args>
    void call_cameras(F &&f, Args&&... args) {
        for (std::weak_ptr<Display> &l : m_displays)
            if (auto disp = l.lock()) if (auto cam = disp->get_camera())
                (cam.get()->*f)(std::forward<Args>(args)...);
    }
    
public:
    
    // Set the scene that will be controlled.
    void set_scene(std::shared_ptr<Scene> scene)
    {
        m_scene = scene;
        m_scene->add_listener(shared_from_this());
    }
    
    const Scene * get_scene() const { return m_scene.get(); }

    void add_display(std::shared_ptr<Display> disp)
    {
        m_displays.emplace_back(disp);
        cleanup(m_displays);
    }
    
    void remove_displays() { m_displays = {}; }
    
    void on_scene_updated(const Scene &scene) override;
    
    void on_left_click_down() override { m_left_btn = true; }
    void on_left_click_up() override { m_left_btn = false;  }
    void on_right_click_down() override { m_right_btn = true;  }
    void on_right_click_up() override { m_right_btn = false; }
    
    void on_scroll(long v, long d, MouseInput::WheelAxis wa) override;
    void on_moved_to(long x, long y) override;

    void move_clip_plane(double z) { call_cameras(&Camera::set_clip_z, z); }
};

}}     // namespace Slic3r::GL
#endif // SLIC3R_OCSG_EXMP_ENGINE_HPP
