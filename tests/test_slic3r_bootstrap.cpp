// Gives every test binary that links libslic3r the part of the runtime environment the application sets
// up at startup: resources_dir() pointing at the shipped resources, and the data files that live there
// loaded (the material tables). Without it the tests would silently run on the built-in fallback tables.
// Compiled into each test binary through the test_slic3r_bootstrap interface library.

#include <catch2/catch_all.hpp>

#include "libslic3r/MaterialType.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

namespace {
class ResourcesListener : public Catch::EventListenerBase
{
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(const Catch::TestRunInfo&) override
    {
        Slic3r::set_resources_dir(RESOURCES_DIR);
        // No mirroring: the tests have no data directory to keep their own copy of the data files in.
        Slic3r::MaterialType::load(/*mirror_to_data_dir=*/false);
        Slic3r::refresh_material_type_config_defs();
    }
};
} // namespace

CATCH_REGISTER_LISTENER(ResourcesListener)
