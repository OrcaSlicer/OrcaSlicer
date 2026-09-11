#ifndef slic3r_PagesPluginCapability_hpp_
#define slic3r_PagesPluginCapability_hpp_

#include "../../PythonPluginInterface.hpp"
#include "pybind11/pybind11.h"

#include <functional>
#include <mutex>
#include <string>

namespace Slic3r {
class PagesPluginCapability : public PluginCapabilityInterface
{
public:
    static void RegisterBindings(pybind11::module_& module);

    PluginCapabilityType get_type() const override { return PluginCapabilityType::Pages; }

    virtual std::string get_ui() = 0;
    virtual void on_message(std::string message) { (void) message; }
    virtual std::string get_icon() { return {}; }

    void post_message(std::string message);
    void set_message_sender(std::function<void(const std::string&)> sender);
    void clear_message_sender();

private:
    mutable std::mutex m_message_mutex;
    std::function<void(const std::string&)> m_message_sender;
};
} // namespace Slic3r

#endif
