#include <iostream>
#include <fstream>
#include <vector>
#include <tuple>
#include <random>

#include "ItsNeighborIndex.hpp"

#include "libnest2d/tools/benchmark.h"
#include "libnest2d/utils/metaloop.hpp"

namespace Slic3r {

enum { IndexCreation, Split };
struct MeasureResult
{
    static constexpr const char * Names[] = {
        "Index creation [s]",
        "Split [s]"
    };

    double measurements[std::size(Names)] = {0.};
};

template<class IndexCreatorFn>
static MeasureResult measure_index(const indexed_triangle_set &its, IndexCreatorFn fn)
{
    Benchmark b;

    MeasureResult r;
    for (int i = 0; i < 10; ++i) {
        b.start();
        ItsNeighborsWrapper itsn{its, fn(its)};
        b.stop();

        r.measurements[IndexCreation] += b.getElapsedSec();

        b.start();
        auto res = its_split(itsn);
        b.stop();

//        if (res.size() != 2 || res[0].indices.size() != res[1].indices.size() )
//            std::cerr << "Something is wrong, split result invalid" << std::endl;

        r.measurements[Split] += b.getElapsedSec();
    }

    r.measurements[IndexCreation] /= 10;
    r.measurements[Split] /= 10;

    return r;
}

const auto Seed = 0;// std::random_device{}();

static indexed_triangle_set make_sphere_rnd(double radius, double detail)
{
    using namespace Slic3r;

    auto sphere = its_make_sphere(radius, detail);

    auto vfidx = create_vertex_faces_index(sphere);

    const size_t vertexnum = sphere.vertices.size();
    const size_t facenum   = sphere.indices.size();

    std::mt19937 rng{Seed};
    std::uniform_int_distribution<size_t> distv(sphere.vertices.size() / 2, sphere.vertices.size() - 1);
    std::uniform_int_distribution<size_t> distf(sphere.indices.size() / 2, sphere.indices.size() - 1) ;

    std::vector<bool> was(vertexnum / 2, false);

    for (size_t i = 0; i < vertexnum / 2; ++i) {
        size_t image = distv(rng);
        if (was[image - vertexnum / 2]) continue;
        was[image - vertexnum / 2] = true;

        std::swap(sphere.vertices[i], sphere.vertices[image]);
        for (size_t face_id : vfidx[i]) {
            for (int &vi : sphere.indices[face_id])
                if (vi == int(i)) vi = image;
        }

        for (size_t face_id : vfidx[image]) {
            for (int &vi : sphere.indices[face_id])
                if (vi == int(image)) vi = i;
        }

        std::swap(vfidx[i], vfidx[image]);
    }

    for (size_t i = 0; i < facenum / 2; ++i) {
        size_t image = distf(rng);
        std::swap(sphere.indices[i], sphere.indices[image]);
    }

    return sphere;
}

static indexed_triangle_set two_spheres(double detail)
{
    auto sphere1 = make_sphere_rnd(10., 2 * PI / detail), sphere2 = sphere1;

    its_transform(sphere1, identity3f().translate(Vec3f{-5.f, 0.f, 0.f}));
    its_transform(sphere2, identity3f().translate(Vec3f{5.f, 0.f, 0.f}));

    its_merge(sphere1, sphere2);

    return sphere1;
}

static indexed_triangle_set make_spheres(unsigned N, double detail)
{
    indexed_triangle_set ret, sphere = make_sphere_rnd(10., 2. * PI / detail);

    for (unsigned i = 0u ; i < N; ++i)
        its_merge(ret, sphere);

    return ret;
}

constexpr double sq2 = std::sqrt(2.);

static const std::pair<const std::string, indexed_triangle_set> ToMeasure[] = {
//    {"two_spheres_1x", two_spheres(60.)},
//    {"two_spheres_2x", two_spheres(120.)},
//    {"two_spheres_4x", two_spheres(240.)},
//    {"two_spheres_8x", two_spheres(480.)},
//    {"two_spheres_16x", two_spheres(2 * 480.)},
//    {"two_spheres_32x", two_spheres(2 * 2 * 480.)},

    {"two_spheres_1x", two_spheres(60.)},
    {"two_spheres_2x", two_spheres(sq2 * 60.)},
    {"two_spheres_4x", two_spheres(2 * 60.)},
    {"two_spheres_8x", two_spheres(sq2 * 2. * 60.)},
    {"two_spheres_16x", two_spheres(4. * 60.)},
    {"two_spheres_32x", two_spheres(sq2 * 4. * 60.)},
    {"two_spheres_64x", two_spheres(8. * 60.)},
    {"two_spheres_128x", two_spheres(sq2 * 8. * 60.)},
    {"two_spheres_256x", two_spheres(16. * 60.)},
    {"two_spheres_512x", two_spheres(sq2 * 16. * 60.)},

    {"2_spheres", make_spheres(2, 60.)},
    {"4_spheres", make_spheres(4, 60.)},
    {"8_spheres", make_spheres(8, 60.)},
    {"16_spheres", make_spheres(16,  60.)},
    {"32_spheres", make_spheres(32, 60.)},
    {"64_spheres", make_spheres(64, 60.)},
    {"128_spheres", make_spheres(128, 60.)},
    {"256_spheres", make_spheres(256, 60.)},
    {"512_spheres", make_spheres(512, 60.)},
    {"1024_spheres", make_spheres(1024, 60.)}
};

static const auto IndexFunctions = std::make_tuple(
    std::make_pair("tamas's unordered_map based", [](const auto &its) { return measure_index(its, its_create_neighbors_index_1); }),
    std::make_pair("vojta std::sort based", [](const auto &its) { return measure_index(its, its_create_neighbors_index_2); }),
    std::make_pair("vojta tbb::parallel_sort based", [](const auto &its) { return measure_index(its, its_create_neighbors_index_3); }),
    std::make_pair("filip's vertex->face based", [](const auto &its) { return measure_index(its, its_create_neighbors_index_5); }),
    std::make_pair("vojta's vertex->face", [](const auto &its) { return measure_index(its, its_create_neighbors_index_9); }),
    std::make_pair("vojta's vertex->face parallel", [](const auto &its) { return measure_index(its, its_create_neighbors_index_10); }),
    std::make_pair("tamas's std::sort based", [](const auto &its) { return measure_index(its, its_create_neighbors_index_6); }),
    std::make_pair("tamas's tbb::parallel_sort based", [](const auto &its) { return measure_index(its, its_create_neighbors_index_7); }),
    std::make_pair("tamas's map based", [](const auto &its) { return measure_index(its, its_create_neighbors_index_8); }),
    std::make_pair("TriangleMesh split", [](const auto &its) {

        MeasureResult r;
        for (int i = 0; i < 10; ++i) {
            TriangleMesh m{its};
            Benchmark b;

            b.start();
            m.repair(); // FIXME: this does more than just create neighborhood map
            b.stop();
            r.measurements[IndexCreation] += b.getElapsedSec();

            b.start();
            auto res = m.split();
            b.stop();
            r.measurements[Split] += b.getElapsedSec();

//            if (res.size() != 2 || res[0]->size() != res[1]->size())
//                std::cerr << "Something is wrong, split result invalid" << std::endl;
        }
        r.measurements[IndexCreation] /= 10;
        r.measurements[Split] /= 10;

        return r;
    })

//    std::make_pair("Vojta's vertex->face index", [](const auto &its){
//        Benchmark b;
//        b.start();
//        auto index = create_vertex_faces_index(its);
//        b.stop();

//        if (index.size() != its.vertices.size())
//            std::cerr << "Something went wrong!";

//        return  MeasureResult{b.getElapsedSec(), 0., 0.};
//    }),
//    std::make_pair("Tamas's vertex->face index", [](const auto &its){
//        Benchmark b;
//        b.start();
//        VertexFaceIndex index{its};
//        b.stop();

//        if (index.size() < its.vertices.size())
//            std::cerr << "Something went wrong!";

//        return  MeasureResult{b.getElapsedSec(), 0., 0.};
//    })
);

static constexpr size_t IndexFuncNum = std::tuple_size_v<decltype (IndexFunctions)>;

} // namespace Slic3r

int main(const int argc, const char * argv[])
{
    using namespace Slic3r;

    std::array<MeasureResult, IndexFuncNum> results[std::size(ToMeasure)];
    std::array<std::string, IndexFuncNum> funcnames;

    for (size_t i = 0; i < std::size(ToMeasure); ++i) {
        auto &m = ToMeasure[i];
        auto &name = m.first;
        auto &mesh = m.second;

//        its_write_obj(mesh, (std::string(name) + ".obj").c_str());

        std::cout << "Mesh " << name << " has " << mesh.indices.size() << " faces and " << mesh.vertices.size() << " vertices." << std::endl;
        libnest2d::opt::metaloop::apply([&mesh, i, &results, &funcnames](int N, auto &e) {
            MeasureResult r = e.second(mesh);
            funcnames[N] = e.first;
            results[i][N] = r;
        }, IndexFunctions);
    }

    std::string outfilename = "out.csv";
    std::fstream outfile;
    if (argc > 1) {
        outfilename = argv[1];
        outfile.open(outfilename, std::fstream::out);
        std::cout << outfilename << " will be used" << std::endl;
    }

    std::ostream &out = outfile.is_open() ? outfile : std::cout;

    for (size_t m = 0; m < std::size(MeasureResult::Names); ++m) {
        out << MeasureResult::Names[m] << "\n";
        out << std::endl;
        out << "model;" ;
        for (const std::string &funcname : funcnames) {
            out << funcname << ";";
        }

        out << std::endl;

        for (size_t i = 0; i < std::size(ToMeasure); ++i) {
            const auto &result_row = results[i];
            const std::string &name = ToMeasure[i].first;
            out << name << ";";
            for (auto &r : result_row)
                out << r.measurements[m] << ";";

            out << std::endl;
        }
    }

    return 0;
}
