#ifndef slic3r_ScriptPluginCapability_hpp_
#define slic3r_ScriptPluginCapability_hpp_

#include "../../PythonPluginInterface.hpp"

namespace Slic3r {
class ScriptPluginCapability : public PluginCapabilityInterface
{
public:
    PluginCapabilityType get_type() const override { return PluginCapabilityType::Script; }

    virtual ExecutionResult execute() = 0;

    static void RegisterBindings(pybind11::module_ &module,
                                 pybind11::enum_<PluginCapabilityType> &pluginTypes);
};
} // namespace Slic3r

#endif /* slic3r_ScriptPluginCapability_hpp_ */
