#pragma once

#include <catch2/catch_test_macros.hpp>

#include "core/Catalog.hpp"
#include "core/Workload.hpp"

#include "libslic3r/Utils.hpp"

#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <vector>

namespace Slic3r { namespace Bench { namespace Test {

// The source tree's resources as resources_dir() for the scope's lifetime.
class TreeResources
{
public:
    TreeResources() : m_previous(resources_dir()) { set_resources_dir(ORCABENCH_RESOURCES_DIR); }
    ~TreeResources() { set_resources_dir(m_previous); }

    TreeResources(const TreeResources&)            = delete;
    TreeResources& operator=(const TreeResources&) = delete;

private:
    const std::string m_previous;
};

// One catalog file shipped in benchmarks/catalog, without the others a developer may keep there.
inline std::vector<CatalogEntry> shipped_catalog(const std::string& file_name)
{
    std::ifstream file(std::string(ORCABENCH_CATALOG_DIR) + "/" + file_name, std::ios::binary);
    REQUIRE(file);
    return read_catalog(std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()));
}

}}} // namespace Slic3r::Bench::Test
