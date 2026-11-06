#ifndef SLA_TEST_UTILS_HPP
#define SLA_TEST_UTILS_HPP

#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>

// Debug
#include <cstddef>
#include <climits>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include "libslic3r/SLA/Hollowing.hpp"
#include <unordered_map>
#include <random>
#include <type_traits>
#include "libslic3r/SLA/RasterBase.hpp"
#include "libslic3r/SLA/SupportPoint.hpp"
#include <unordered_set>
#include <vector>
#include <utility>

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/SLA/SupportTreeBuilder.hpp"
#include "libslic3r/SLA/SupportTreeBuildsteps.hpp"
#include "libslic3r/SLA/SupportPointGenerator.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"

namespace Slic3r::sla { class RasterGrayscaleAA; }
namespace Slic3r::sla { struct PadConfig; }
namespace Slic3r::sla { struct SupportTreeConfig; }


enum e_validity {
    ASSUME_NO_EMPTY = 1,
    ASSUME_MANIFOLD = 2,
    ASSUME_NO_REPAIR = 4
};

void check_validity(const Slic3r::TriangleMesh &input_mesh,
                    int flags = ASSUME_NO_EMPTY | ASSUME_MANIFOLD |
                                ASSUME_NO_REPAIR);

struct PadByproducts
{
    Slic3r::ExPolygons   model_contours;
    Slic3r::ExPolygons   support_contours;
    Slic3r::TriangleMesh mesh;
};

void test_concave_hull(const Slic3r::ExPolygons &polys);

void test_pad(const std::string &   obj_filename,
              const Slic3r::sla::PadConfig &padcfg,
              PadByproducts &       out);

inline void test_pad(const std::string &   obj_filename,
              const Slic3r::sla::PadConfig &padcfg = {})
{
    PadByproducts byproducts;
    test_pad(obj_filename, padcfg, byproducts);
}

struct SupportByproducts
{
    std::string             obj_fname;
    std::vector<float>      slicegrid;
    std::vector<Slic3r::ExPolygons> model_slices;
    Slic3r::sla::SupportTreeBuilder supporttree;
    Slic3r::TriangleMesh            input_mesh;
};

const constexpr float CLOSING_RADIUS = 0.005f;

void check_support_tree_integrity(const Slic3r::sla::SupportTreeBuilder &stree,
                                  const Slic3r::sla::SupportTreeConfig &cfg);

void test_supports(const std::string          &obj_filename,
                   const Slic3r::sla::SupportTreeConfig   &supportcfg,
                   const Slic3r::sla::HollowingConfig &hollowingcfg,
                   const Slic3r::sla::DrainHoles      &drainholes,
                   SupportByproducts          &out);

inline void test_supports(const std::string &obj_filename,
                   const Slic3r::sla::SupportTreeConfig &supportcfg,
                   SupportByproducts        &out) 
{
    Slic3r::sla::HollowingConfig hcfg;
    hcfg.enabled = false;
    test_supports(obj_filename, supportcfg, hcfg, {}, out);    
}

inline void test_supports(const std::string &obj_filename,
                   const Slic3r::sla::SupportTreeConfig &supportcfg = {})
{
    SupportByproducts byproducts;
    test_supports(obj_filename, supportcfg, byproducts);
}

void export_failed_case(const std::vector<Slic3r::ExPolygons> &support_slices,
                        const SupportByproducts &byproducts);


void test_support_model_collision(
    const std::string          &obj_filename,
    const Slic3r::sla::SupportTreeConfig   &input_supportcfg,
    const Slic3r::sla::HollowingConfig &hollowingcfg,
    const Slic3r::sla::DrainHoles      &drainholes);

inline void test_support_model_collision(
    const std::string        &obj_filename,
    const Slic3r::sla::SupportTreeConfig &input_supportcfg = {}) 
{
    Slic3r::sla::HollowingConfig hcfg;
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
        
        II hash_ab = Slic3r::sla::pairhash<I, II>(a, b);
        II hash_ba = Slic3r::sla::pairhash<I, II>(b, a);
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

void check_raster_transformations(Slic3r::sla::RasterBase::Orientation o,
                                  Slic3r::sla::RasterBase::TMirroring  mirroring);

Slic3r::ExPolygon square_with_hole(double v);

inline double pixel_area(TPixel px, const Slic3r::sla::PixelDim &pxdim)
{
    return (pxdim.h_mm * pxdim.w_mm) * px * 1. / (FullWhite - FullBlack);
}

double raster_white_area(const Slic3r::sla::RasterGrayscaleAA &raster);
long raster_pxsum(const Slic3r::sla::RasterGrayscaleAA &raster);

double predict_error(const Slic3r::ExPolygon &p, const Slic3r::sla::PixelDim &pd);

Slic3r::sla::SupportPoints calc_support_pts(
    const Slic3r::TriangleMesh &                      mesh,
    const Slic3r::sla::SupportPointGenerator::Config &cfg = {});

#endif // SLA_TEST_UTILS_HPP
