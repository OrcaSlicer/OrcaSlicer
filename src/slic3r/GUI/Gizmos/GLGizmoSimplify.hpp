#ifndef slic3r_GLGizmoSimplify_hpp_
#define slic3r_GLGizmoSimplify_hpp_

// Include GLGizmoBase.hpp before I18N.hpp as it includes some libigl code,
// which overrides our localization "L" macro.
#include "GLGizmoBase.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "admesh/stl.h" // indexed_triangle_set
#include <mutex>
#include <thread>

namespace Slic3r {
class ModelVolume;
class Model;

namespace GUI {
class NotificationManager; // for simplify suggestion

class GLGizmoSimplify: public GLGizmoBase
{    
public:
    GLGizmoSimplify(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);
    virtual ~GLGizmoSimplify();
    bool on_esc_key_down();
    static void add_simplify_suggestion_notification(
        const std::vector<size_t> &object_ids,
        const std::vector<ModelObject*> &    objects,
        NotificationManager &      manager);

protected:
    virtual std::string on_get_name() const override;
    virtual void on_render_input_window(float x, float y, float bottom_limit) override;
    virtual bool on_is_activable() const override;
    virtual bool on_is_selectable() const override { return false; }
    virtual void on_set_state() override;

    // must implement
    virtual bool on_init() override { return true;};
    virtual void on_render() override;

    CommonGizmosDataID on_get_requirements() const override;

private:
    void apply_simplify();
    void close();

    void process();
    void stop_worker_thread_request();
    void worker_finished();

    void create_gui_cfg();
    void request_rerender(bool force = false);
    void init_model(const indexed_triangle_set& its);

    void set_center_position();

    struct Configuration
    {
        bool use_count = false;
        float    decimate_ratio = 50.f; // in percent
        uint32_t wanted_count = 0; // initialize by percents
        float max_error = 1.; // maximal quadric error

        void fix_count_by_ratio(size_t triangle_count);
        bool operator==(const Configuration& rhs) {
            return (use_count == rhs.use_count && decimate_ratio == rhs.decimate_ratio
                && wanted_count == rhs.wanted_count && max_error == rhs.max_error);
        }
        bool operator!=(const Configuration& rhs) {
            return ! (*this == rhs);
        }
    };

    Configuration m_configuration;

    bool m_move_to_center; // opening gizmo
        
    const ModelVolume *m_volume; // keep pointer to actual working volume

    bool m_show_wireframe;
    GLModel m_glmodel;
    size_t m_triangle_count; // triangle count of the model currently shown

    // Timestamp of the last rerender request. Only accessed from UI thread.
    int64_t m_last_rerender_timestamp = std::numeric_limits<int64_t>::min();

    // Following struct is accessed by both UI and worker thread.
    // Accesses protected by a mutex.
    struct State {
        enum Status {
            idle,
            running,
            cancelling
        };

        Status status = idle;
        int progress = 0; // percent of done work
        Configuration config; // Configuration we started with.
        const ModelVolume* mv = nullptr;
        std::unique_ptr<indexed_triangle_set> result;
    };

    std::thread m_worker;
    std::mutex m_state_mutex; // guards m_state
    State m_state; // accessed by both threads


    // This configs holds GUI layout size given by translated texts.
    // etc. When language changes, GUI is recreated and this class constructed again,
    // so the change takes effect. (info by GLGizmoFdmSupports.hpp)
    struct GuiCfg
    {
        int top_left_width    = 100;
        int bottom_left_width = 100;
        int input_width       = 100;
        int window_offset_x   = 100;
        int window_offset_y   = 100;
        int window_padding    = 0;

        // trunc model name when longer
        size_t max_char_in_name = 30;

        // to prevent freezing when move in gui
        // delay before process in [ms]
        std::chrono::duration<long int, std::milli> prcess_delay = std::chrono::milliseconds(250);
    };
    std::optional<GuiCfg> m_gui_cfg;

    // translations used for calc window size
    const std::string tr_mesh_name;
    const std::string tr_triangles;
    const std::string tr_detail_level;
    const std::string tr_decimate_ratio;

    // cancel exception
    class SimplifyCanceledException: public std::exception
    {
    public:
        const char *what() const throw()
        {
            return L("Model simplification has been canceled");
        }
    };
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoSimplify_hpp_
