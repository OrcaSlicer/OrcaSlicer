#ifndef ORIENTJOB_HPP
#define ORIENTJOB_HPP

#include "Job.hpp"
#include "libslic3r/Orient.hpp"
#include <vector>
#include <exception>
#include "libslic3r/Model.hpp"

#include <functional>
#include <vector>

namespace Slic3r {

class ModelObject;

namespace GUI {

class Plater;

class OrientJob : public Job
{
    using OrientMesh = orientation::OrientMesh;
    using OrientMeshs = orientation::OrientMeshs;

    OrientMeshs m_selected, m_unselected, m_unprintable;
    Plater     *m_plater;

    // clear m_selected and m_unselected, reserve space for next usage
    void clear_input();

    //BBS: add only one plate mode
    void prepare_selection(std::vector<bool> obj_sel, bool only_one_plate);
    
    // Prepare the selected and unselected items separately. If nothing is
    // selected, takes the current plate's objects (UNLAYERED_MOD: upstream took every plate).
    void prepare_selected();

    //BBS:prepare the items from current selected partplate
    void prepare_partplate();

public:
    void prepare();
    
    void process(Ctl &ctl) override;

    // UNLAYERED_MOD: which instances an orient run takes, [object][instance]. A plate-scoped run, or
    // one with no object selected, takes the instances on the current plate (on_current_plate(o, i));
    // otherwise every instance of the selected objects. Never every plate by default.
    static std::vector<std::vector<bool>> selected_instances(const std::vector<size_t>&           instance_counts,
                                                             const std::vector<bool>&             obj_sel,
                                                             bool                                 plate_scoped,
                                                             const std::function<bool(int, int)>& on_current_plate);

    OrientJob();
    
    void finalize(bool canceled, std::exception_ptr &e) override;
#if 0
    static
    orientation::OrientMesh get_orient_mesh(ModelObject* obj, const Plater* plater)
    {
        using OrientMesh = orientation::OrientMesh;
        OrientMesh om;
        om.name = obj->name;
        om.mesh = obj->mesh(); // don't know the difference to obj->raw_mesh(). Both seem OK
        om.setter = [obj, plater](const OrientMesh& p) {
            obj->rotate(p.angle, p.axis);
            obj->ensure_on_bed();
        };
        return om;
    }
#endif
    static orientation::OrientMesh get_orient_mesh(ModelInstance* instance);
};


}} // namespace Slic3r::GUI

#endif // ORIENTJOB_HPP
