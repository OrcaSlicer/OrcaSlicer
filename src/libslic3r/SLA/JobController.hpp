#ifndef SLA_JOBCONTROLLER_HPP
#define SLA_JOBCONTROLLER_HPP

#include <functional>
#include <string>

namespace Slic3r { namespace sla {

/// A Control structure for the support calculation. Consists of the status
/// indicator callback and the stop condition predicate.
struct JobController
{
    using StatusFn = std::function<void(unsigned, const std::string&)>;
    using StopCond = std::function<bool(void)>;
    using CancelFn = std::function<void(void)>;
    
    // This will signal the status of the calculation to the front-end
    StatusFn statuscb = [](unsigned, const std::string&){};
    
    // Returns true if the calculation should be aborted.
    StopCond stopcondition = [](){ return false; };
    
    // Similar to cancel callback. This should check the stop condition and
    // if true, throw an appropriate exception. (TriangleMeshSlicer needs this)
    // consider it a hard abort. stopcondition is permits the algorithm to
    // terminate itself
    CancelFn cancelfn = [](){};
};

}} // namespace Slic3r::sla

#endif // JOBCONTROLLER_HPP
