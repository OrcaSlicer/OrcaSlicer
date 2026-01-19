#include <iostream>
#include <vector>

#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/SLAPrint.hpp>
#include <libslic3r/SLAPrintSteps.hpp>
#include <libslic3r/MeshBoolean.hpp>

#include <libnest2d/tools/benchmark.h>

#include <boost/log/trivial.hpp>

int main(const int argc, const char * argv[])
{
    using namespace Slic3r;
    
    if (argc <= 1) {
        std::cout << "Usage: meshboolean <input_file.3mf>" << std::endl;
        return EXIT_FAILURE;
    }
    
    
    TriangleMesh input;
    
    input.ReadSTLFile(argv[1]);
    
    Benchmark bench;
    
    bench.start();
    bool fckd = MeshBoolean::cgal::does_self_intersect(input);
    bench.stop();
    
    std::cout << "Self intersect test: " << fckd << " duration: " << bench.getElapsedSec() << std::endl;
    
    bench.start();
    MeshBoolean::self_union(input);
    bench.stop();
    
    std::cout << "Self union duration: " << bench.getElapsedSec() << std::endl;
    
    return 0;
}
