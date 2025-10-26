#ifndef FILLBEDJOB_HPP
#define FILLBEDJOB_HPP

#include "ArrangeJob.hpp"

namespace Slic3r { namespace GUI {

class Plater;

class FillBedJob : public Job
{
    int     m_object_idx = -1;

    using ArrangePolygon  = arrangement::ArrangePolygon;
    using ArrangePolygons = arrangement::ArrangePolygons;

    ArrangePolygons m_selected;
    ArrangePolygons m_unselected;
    //BBS: add partplate related logic
    ArrangePolygons m_locked;;

    Points m_bedpts;

    arrangement::ArrangeParams params;

    int m_status_range = 0;
    Plater *m_plater;

    bool m_instances;

public:

    void prepare();
    void process(Ctl &ctl) override;

    FillBedJob(bool instances = false);

    int status_range() const
    {
        return m_status_range;
    }

    void finalize(bool canceled, std::exception_ptr &e) override;
};

}} // namespace Slic3r::GUI

#endif // FILLBEDJOB_HPP
