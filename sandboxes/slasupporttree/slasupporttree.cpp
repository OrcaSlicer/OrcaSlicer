#include <iostream>
#include <fstream>
#include <string>

#include <libslic3r/libslic3r.h>
#include <libslic3r/Model.hpp>
#include <libslic3r/Tesselate.hpp>
#include <libslic3r/ClipperUtils.hpp>
#include <libslic3r/SLA/SLAAutoSupports.hpp>
#include <libslic3r/SLA/SLASupportTree.hpp>
#include <libslic3r/SLAPrint.hpp>
#include <libslic3r/MTUtils.hpp>

#include <tbb/parallel_for.h>
#include <tbb/mutex.h>
#include <future>

const std::string USAGE_STR = {
    "Usage: slasupporttree stlfilename.stl"
};

int main(const int argc, const char *argv[]) {
    using namespace Slic3r;
    using std::cout; using std::endl;

    if(argc < 2) {
        cout << USAGE_STR << endl;
        return EXIT_SUCCESS;
    }

    DynamicPrintConfig config;

    Model model = Model::read_from_file(argv[1], &config);

    SLAPrint print;

    print.apply(model, config);
    print.process();


    return EXIT_SUCCESS;
}
