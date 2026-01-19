#ifndef SLA_ROTFINDER_HPP
#define SLA_ROTFINDER_HPP

#include <functional>
#include <array>

#include <libslic3r/Point.hpp>

namespace Slic3r {

class ModelObject;
class SLAPrintObject;
class TriangleMesh;
class DynamicPrintConfig;

namespace sla {

using RotOptimizeStatusCB = std::function<bool(int)>;

class RotOptimizeParams {
    float m_accuracy = 1.;
    const DynamicPrintConfig *m_print_config = nullptr;
    RotOptimizeStatusCB m_statuscb = [](int) { return true; };

public:

    RotOptimizeParams &accuracy(float a) { m_accuracy = a; return *this; }
    RotOptimizeParams &print_config(const DynamicPrintConfig *c)
    {
        m_print_config = c;
        return *this;
    }
    RotOptimizeParams &statucb(RotOptimizeStatusCB cb)
    {
        m_statuscb = std::move(cb);
        return *this;
    }

    float accuracy() const { return m_accuracy; }
    const DynamicPrintConfig * print_config() const { return m_print_config; }
    const RotOptimizeStatusCB &statuscb() const { return m_statuscb; }
};

/**
  * The function should find the best rotation for SLA upside down printing.
  *
  * @param modelobj The model object representing the 3d mesh.
  * @param accuracy The optimization accuracy from 0.0f to 1.0f. Currently,
  * the nlopt genetic optimizer is used and the number of iterations is
  * accuracy * 100000. This can change in the future.
  * @param statuscb A status indicator callback called with the int
  * argument spanning from 0 to 100. May not reach 100 if the optimization finds
  * an optimum before max iterations are reached. It should return a boolean
  * signaling if the operation may continue (true) or not (false). A status
  * value lower than 0 shall not update the status but still return a valid
  * continuation indicator.
  *
  * @return Returns the rotations around each axis (x, y, z)
  */
Vec2d find_best_misalignment_rotation(const ModelObject &modelobj,
                                      const RotOptimizeParams & = {});

Vec2d find_least_supports_rotation(const ModelObject &modelobj,
                                   const RotOptimizeParams & = {});

Vec2d find_min_z_height_rotation(const ModelObject &mo,
                                 const RotOptimizeParams &params = {});

} // namespace sla
} // namespace Slic3r

#endif // SLAROTFINDER_HPP
