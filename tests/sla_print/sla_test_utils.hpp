#ifndef SLA_TEST_UTILS_HPP
#define SLA_TEST_UTILS_HPP

#include <catch2/catch_all.hpp>
#include "test_utils.hpp"

// Debug
#include <fstream>
#include <unordered_set>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/SLA/Pad.hpp"
#include "libslic3r/SLA/SupportTreeBuilder.hpp"
#include "libslic3r/SLA/SupportTreeBuildsteps.hpp"
#include "libslic3r/SLA/SupportPointGenerator.hpp"
#include "libslic3r/SLA/AGGRaster.hpp"
#include "libslic3r/SLA/ConcaveHull.hpp"
#include "libslic3r/MTUtils.hpp"

#include "libslic3r/SVG.hpp"
#include "libslic3r/Format/OBJ.hpp"

using namespace Slic3r;

enum e_validity {
    ASSUME_NO_EMPTY = 1,
    ASSUME_MANIFOLD = 2,
    ASSUME_NO_REPAIR = 4
};

void check_validity(const TriangleMesh &input_mesh,
                    int flags = ASSUME_NO_EMPTY | ASSUME_MANIFOLD |
                                ASSUME_NO_REPAIR);

struct PadByproducts
{
    ExPolygons   model_contours;
    ExPolygons   support_contours;
    TriangleMesh mesh;
};

void test_concave_hull(const ExPolygons &polys);

void test_pad(const std::string &   obj_filename,
              const sla::PadConfig &padcfg,
              PadByproducts &       out);

inline void test_pad(const std::string &   obj_filename,
              const sla::PadConfig &padcfg = {})
{
    PadByproducts byproducts;
    test_pad(obj_filename, padcfg, byproducts);
}

struct SupportByproducts
{
    std::string             obj_fname;
    std::vector<float>      slicegrid;
    std::vector<ExPolygons> model_slices;
    sla::SupportTreeBuilder supporttree;
    TriangleMesh            input_mesh;
};

const constexpr float CLOSING_RADIUS = 0.005f;

void check_support_tree_integrity(const sla::SupportTreeBuilder &stree,
                                  const sla::SupportTreeConfig &cfg);

void test_supports(const std::string          &obj_filename,
                   const sla::SupportTreeConfig   &supportcfg,
                   const sla::HollowingConfig &hollowingcfg,
                   const sla::DrainHoles      &drainholes,
                   SupportByproducts          &out);

inline void test_supports(const std::string &obj_filename,
                   const sla::SupportTreeConfig &supportcfg,
                   SupportByproducts        &out) 
{
    sla::HollowingConfig hcfg;
    hcfg.enabled = false;
    test_supports(obj_filename, supportcfg, hcfg, {}, out);    
}

inline void test_supports(const std::string &obj_filename,
                   const sla::SupportTreeConfig &supportcfg = {})
{
    SupportByproducts byproducts;
    test_supports(obj_filename, supportcfg, byproducts);
}

void export_failed_case(const std::vector<ExPolygons> &support_slices,
                        const SupportByproducts &byproducts);


void test_support_model_collision(
    const std::string          &obj_filename,
    const sla::SupportTreeConfig   &input_supportcfg,
    const sla::HollowingConfig &hollowingcfg,
    const sla::DrainHoles      &drainholes);

inline void test_support_model_collision(
    const std::string        &obj_filename,
    const sla::SupportTreeConfig &input_supportcfg = {}) 
{
    sla::HollowingConfig hcfg;
    hcfg.enabled = false;
    test_support_model_collision(obj_filename, input_supportcfg, hcfg, {});
}

// Test pair hash for 'nums' random number pairs.
template <class I, class II> void test_pairhash()
{
    const constexpr size_t nums = 1000;
    I A[nums] = {0}, B[nums] = {0};
    std::unordered_set<I> CH;
    std::unordered_map<II, std::pair<I, I>> ints;
    
    std::random_device rd;
    std::mt19937 gen(rd());
    
    const I Ibits = int(sizeof(I) * CHAR_BIT);
    const II IIbits = int(sizeof(II) * CHAR_BIT);
    
    int bits = IIbits / 2 < Ibits ? Ibits / 2 : Ibits;
    if (std::is_signed<I>::value) bits -= 1;
    const I Imin = 0;
    const I Imax = I(std::pow(2., bits) - 1);
    
    std::uniform_int_distribution<I> dis(Imin, Imax);
    
    for (size_t i = 0; i < nums;) {
        I a = dis(gen);
        if (CH.find(a) == CH.end()) { CH.insert(a); A[i] = a; ++i; }
    }
    
    for (size_t i = 0; i < nums;) {
        I b = dis(gen);
        if (CH.find(b) == CH.end()) { CH.insert(b); B[i] = b; ++i; }
    }
    
    for (size_t i = 0; i < nums; ++i) {
        I a = A[i], b = B[i];
        
        REQUIRE(a != b);
        
        II hash_ab = sla::pairhash<I, II>(a, b);
        II hash_ba = sla::pairhash<I, II>(b, a);
        REQUIRE(hash_ab == hash_ba);
        
        auto it = ints.find(hash_ab);
        
        if (it != ints.end()) {
            REQUIRE((
                (it->second.first == a && it->second.second == b) ||
                (it->second.first == b && it->second.second == a)
                ));
        } else
            ints[hash_ab] = std::make_pair(a, b);
    }
}

// SLA Raster test utils:

using TPixel = uint8_t;
static constexpr const TPixel FullWhite = 255;
static constexpr const TPixel FullBlack = 0;

template <class A, int N> constexpr int arraysize(const A (&)[N]) { return N; }

void check_raster_transformations(sla::RasterBase::Orientation o,
                                  sla::RasterBase::TMirroring  mirroring);

ExPolygon square_with_hole(double v);

inline double pixel_area(TPixel px, const sla::PixelDim &pxdim)
{
    return (pxdim.h_mm * pxdim.w_mm) * px * 1. / (FullWhite - FullBlack);
}

double raster_white_area(const sla::RasterGrayscaleAA &raster);
long raster_pxsum(const sla::RasterGrayscaleAA &raster);

double predict_error(const ExPolygon &p, const sla::PixelDim &pd);

sla::SupportPoints calc_support_pts(
    const TriangleMesh &                      mesh,
    const sla::SupportPointGenerator::Config &cfg = {});

#endif // SLA_TEST_UTILS_HPP
