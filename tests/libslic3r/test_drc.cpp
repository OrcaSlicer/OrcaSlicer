#include <catch2/catch_all.hpp>

#include "libslic3r/Format/DRC.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <boost/nowide/fstream.hpp>

#include <draco/compression/encode.h>
#include <draco/mesh/mesh.h>

#include "test_utils.hpp"

using namespace Slic3r;

namespace {

// Encodes one triangle whose vertices carry a single attribute of the given type.
// The decoder accepts both a mesh without POSITION and a face index past the point count.
void write_drc_triangle(const std::string &path, draco::GeometryAttribute::Type attribute_type, uint32_t second_point)
{
    draco::Mesh mesh;
    mesh.set_num_points(3);
    draco::GeometryAttribute attribute;
    attribute.Init(attribute_type, nullptr, 3, draco::DT_FLOAT32, false, sizeof(float) * 3, 0);
    const int attribute_id = mesh.AddAttribute(attribute, true, 3);
    const float points[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    for (int i = 0; i < 3; ++i)
        mesh.attribute(attribute_id)->SetAttributeValue(draco::AttributeValueIndex(i), points[i]);
    draco::Mesh::Face face;
    face[0] = draco::PointIndex(0);
    face[1] = draco::PointIndex(second_point);
    face[2] = draco::PointIndex(2);
    mesh.AddFace(face);

    draco::Encoder encoder;
    encoder.SetEncodingMethod(draco::MESH_SEQUENTIAL_ENCODING);
    draco::EncoderBuffer buffer;
    REQUIRE(encoder.EncodeMeshToBuffer(mesh, &buffer).ok());
    boost::nowide::ofstream out(path, std::ios::binary);
    out.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
}

} // namespace

TEST_CASE("A Draco triangle with positions loads", "[DRC]")
{
    ScopedTemporaryFile drc(".drc");
    write_drc_triangle(drc.string(), draco::GeometryAttribute::POSITION, 1);

    TriangleMesh mesh;
    REQUIRE(load_drc(drc.string().c_str(), &mesh));
    CHECK(mesh.facets_count() == 1);
}

TEST_CASE("A Draco mesh without a POSITION attribute fails to load", "[DRC][Regression]")
{
    ScopedTemporaryFile drc(".drc");
    write_drc_triangle(drc.string(), draco::GeometryAttribute::GENERIC, 1);

    TriangleMesh mesh;
    CHECK_FALSE(load_drc(drc.string().c_str(), &mesh));
}

TEST_CASE("A Draco face referencing a missing point fails to load", "[DRC][Regression]")
{
    ScopedTemporaryFile drc(".drc");
    write_drc_triangle(drc.string(), draco::GeometryAttribute::POSITION, 200);

    TriangleMesh mesh;
    CHECK_FALSE(load_drc(drc.string().c_str(), &mesh));
}
