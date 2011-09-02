#ifndef ROTOPTIMIZEJOB_HPP
#define ROTOPTIMIZEJOB_HPP

#include "Job.hpp"

#include "libslic3r/SLA/Rotfinder.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/I18N.hpp"

namespace Slic3r {

namespace GUI {

class Plater;

class RotoptimizeJob : public Job
{
    using FindFn = std::function<Vec2d(const ModelObject &           mo,
                                       const sla::RotOptimizeParams &params)>;

    struct FindMethod { std::string name; FindFn findfn; std::string descr; };

    static inline const FindMethod Methods[]
        = {{L("Best surface quality"),
            sla::find_best_misalignment_rotation,
            L("Optimize object rotation for best surface quality.")},
           {L("Reduced overhang slopes"),
            sla::find_least_supports_rotation,
            L("Optimize object rotation to have minimum amount of overhangs needing support "
              "structures.\nNote that this method will try to find the best surface of the object "
              "for touching the print bed if no elevation is set.")},
           // Just a min area bounding box that is done for all methods anyway.
           {L("Lowest Z height"),
            sla::find_min_z_height_rotation,
            L("Rotate the model to have the lowest z height for faster print time.")}};

    size_t m_method_id = 0;
    float  m_accuracy  = 0.75;

    DynamicPrintConfig m_default_print_cfg;

    struct ObjRot
    {
        size_t               idx;
        std::optional<Vec2d> rot;
        ObjRot(size_t id): idx{id}, rot{} {}
    };

    std::vector<ObjRot> m_selected_object_ids;
    Plater *m_plater;

public:

    void prepare();
    void process(Ctl &ctl) override;

    RotoptimizeJob();

    void finalize(bool canceled, std::exception_ptr &) override;

    static constexpr size_t get_methods_count() { return std::size(Methods); }

    static std::string get_method_name(size_t i)
    {
        return _utf8(Methods[i].name);
    }

    static std::string get_method_description(size_t i)
    {
        return _utf8(Methods[i].descr);
    }
};

}} // namespace Slic3r::GUI

#endif // ROTOPTIMIZEJOB_HPP
