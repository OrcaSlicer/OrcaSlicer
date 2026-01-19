#ifndef SLAPRINTSTEPS_HPP
#define SLAPRINTSTEPS_HPP

#include <random>

#include <libslic3r/SLAPrint.hpp>

#include <libslic3r/SLA/Hollowing.hpp>
#include <libslic3r/SLA/SupportTree.hpp>

namespace Slic3r {

class SLAPrint::Steps
{
private:
    SLAPrint *m_print = nullptr;
    std::mt19937 m_rng;
    
public:    
    // where the per object operations start and end
    static const constexpr unsigned min_objstatus = 0;   
    static const constexpr unsigned max_objstatus = 50;
    
private:
    const size_t objcount;
    
    // shortcut to initial layer height
    const double  ilhd;
    const float   ilh;
    const coord_t ilhs;
    
    // the coefficient that multiplies the per object status values which
    // are set up for <0, 100>. They need to be scaled into the whole process
    const double objectstep_scale;
    
    template<class...Args> void report_status(Args&&...args)
    {
        m_print->m_report_status(*m_print, std::forward<Args>(args)...);
    }
    
    double current_status() const { return m_print->m_report_status.status(); }
    void throw_if_canceled() const { m_print->throw_if_canceled(); }
    bool canceled() const { return m_print->canceled(); }
    void initialize_printer_input();
    
    void apply_printer_corrections(SLAPrintObject &po, SliceOrigin o);
    
public:
    explicit Steps(SLAPrint *print);
    
    void hollow_model(SLAPrintObject &po);
    void drill_holes (SLAPrintObject &po);
    void slice_model(SLAPrintObject& po);
    void support_points(SLAPrintObject& po);
    void support_tree(SLAPrintObject& po);
    void generate_pad(SLAPrintObject& po);
    void slice_supports(SLAPrintObject& po);
    
    void merge_slices_and_eval_stats();
    void rasterize();
    
    void execute(SLAPrintObjectStep step, SLAPrintObject &obj);
    void execute(SLAPrintStep step);
    
    static std::string label(SLAPrintObjectStep step);
    static std::string label(SLAPrintStep step);
    
    double progressrange(SLAPrintObjectStep step) const;
    double progressrange(SLAPrintStep step) const;
};

}

#endif // SLAPRINTSTEPS_HPP
