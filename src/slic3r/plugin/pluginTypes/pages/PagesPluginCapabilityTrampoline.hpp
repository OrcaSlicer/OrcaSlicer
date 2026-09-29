#pragma once

#include "PagesPluginCapability.hpp"
#include "../../PluginFsUtils.hpp"
#include "../../PyPluginTrampoline.hpp"

#include <nlohmann/json.hpp>

namespace Slic3r {

class PyPagesPluginCapabilityTrampoline : public PyPluginCommonTrampoline<PagesPluginCapability>
{
public:
    using PyPluginCommonTrampoline<PagesPluginCapability>::PyPluginCommonTrampoline;

    std::string get_icon() override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {},
            PYBIND11_OVERRIDE,
            std::string,
            PagesPluginCapability,
            get_icon);
    }

    std::string get_ui() override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {},
            PYBIND11_OVERRIDE_PURE,
            std::string,
            PagesPluginCapability,
            get_ui);
    }

    void on_message(std::string message) override
    {
        PluginCapabilityInterface::RefCounter ref_counter(*this);
        PythonGILState                         gil;
        if (!gil)
            throw std::runtime_error("Python interpreter is shutting down");

        ORCA_PY_AUDIT_SCOPE();

        pybind11::function override = pybind11::get_override(static_cast<PagesPluginCapability*>(this), "on_message");
        if (!override)
            return;

        nlohmann::json data = nlohmann::json::parse(message, nullptr, false);
        if (data.is_discarded())
            data = message;

        ORCA_PY_LOGGED_OVERRIDE_BODY(override(::Slic3r::json_to_py(data)));
    }
};

} // namespace Slic3r
