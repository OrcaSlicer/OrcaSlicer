#ifndef MODELARRANGE_HPP
#define MODELARRANGE_HPP

#include <libslic3r/Arrange.hpp>
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Model.hpp"

namespace Slic3r {
using ModelInstancePtrs = std::vector<ModelInstance*>;

using arrangement::ArrangePolygon;
using arrangement::ArrangePolygons;
using arrangement::ArrangeParams;
using arrangement::InfiniteBed;
using arrangement::CircleBed;

// Do something with ArrangePolygons in virtual beds
using VirtualBedFn = std::function<void(arrangement::ArrangePolygon&)>;

[[noreturn]] inline void throw_if_out_of_bed(arrangement::ArrangePolygon& ap)
{
    throw Slic3r::RuntimeError("Objects could not fit on the bed; bed_idx==" + std::to_string(ap.bed_idx));
}

ArrangePolygons get_arrange_polys(const Model &model, ModelInstancePtrs &instances);
ArrangePolygon  get_arrange_poly(const Model &model);
bool apply_arrange_polys(ArrangePolygons &polys, ModelInstancePtrs &instances, VirtualBedFn);

void duplicate(Model &model, ArrangePolygons &copies, VirtualBedFn);
void duplicate_objects(Model &model, size_t copies_num);

template<class TBed>
bool arrange_objects(Model &              model,
                     const TBed &         bed,
                     const ArrangeParams &params,
                     VirtualBedFn         vfn = throw_if_out_of_bed)
{
    ModelInstancePtrs instances;
    auto&& input = get_arrange_polys(model, instances);
    arrangement::arrange(input, bed, params);
    
    return apply_arrange_polys(input, instances, vfn);
}

template<class TBed>
void duplicate(Model &              model,
               size_t               copies_num,
               const TBed &         bed,
               const ArrangeParams &params,
               VirtualBedFn         vfn = throw_if_out_of_bed)
{
    ArrangePolygons copies(copies_num, get_arrange_poly(model));
    arrangement::arrange(copies, bed, params);
    duplicate(model, copies, vfn);
}

template<class TBed>
void duplicate_objects(Model &              model,
                       size_t               copies_num,
                       const TBed &         bed,
                       const ArrangeParams &params,
                       VirtualBedFn         vfn = throw_if_out_of_bed)
{
    duplicate_objects(model, copies_num);
    arrange_objects(model, bed, params, vfn);
}

template<class T> struct PtrWrapper
{
    T* ptr;

    explicit PtrWrapper(T* p) : ptr{ p } {}

    arrangement::ArrangePolygon get_arrange_polygon(const Slic3r::DynamicPrintConfig &config = Slic3r::DynamicPrintConfig()) const
    {
        arrangement::ArrangePolygon ap;
        ptr->get_arrange_polygon(&ap, config);
        return ap;
    }

    void apply_arrange_result(const Vec2d& t, double rot, int item_id)
    {
        ptr->apply_arrange_result(t, rot);
        ptr->arrange_order = item_id;
    }
};

template<class T>
arrangement::ArrangePolygon get_arrange_poly(T obj, const DynamicPrintConfig &config = DynamicPrintConfig());

template<>
arrangement::ArrangePolygon get_arrange_poly(ModelInstance* inst, const DynamicPrintConfig& config);

ArrangePolygon get_instance_arrange_poly(ModelInstance* instance, const DynamicPrintConfig& config);
}

#endif // MODELARRANGE_HPP
