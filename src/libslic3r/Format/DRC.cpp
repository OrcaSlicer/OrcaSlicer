#include <string>
#include <utility>
#include <cstring>

#include <boost/iostreams/device/mapped_file.hpp>
#include <boost/nowide/cstdio.hpp>

#include <draco/compression/encode.h>
#include <draco/compression/decode.h>
#include <draco/io/mesh_io.h>
#include <draco/mesh/mesh.h>
using namespace draco;

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "DRC.hpp"

#ifdef _WIN32
#define DIR_SEPARATOR '\\'
#else
#define DIR_SEPARATOR '/'
#endif

namespace Slic3r {

bool load_drc(const char *path, TriangleMesh *meshptr)
{
    try {
        boost::iostreams::mapped_file_source file(path);

        DecoderBuffer buffer;
        buffer.Init(file.data(), file.size());

        auto geotype = Decoder::GetEncodedGeometryType(&buffer);
        if ((!geotype.ok()) || geotype.value() != TRIANGULAR_MESH) {
            return false;
        }

        Decoder decoder;
        Mesh dracoMesh;
        Status status = decoder.DecodeBufferToGeometry(&buffer, &dracoMesh);
        if (!status.ok()) {
            return false;
        }
        file.close();


        indexed_triangle_set its;

        const PointAttribute *const positions = dracoMesh.GetNamedAttribute(GeometryAttribute::POSITION);
        size_t num_vertices = positions->size();
        its.vertices.reserve(num_vertices);
        for (AttributeValueIndex i(0); i < num_vertices; ++ i) {
            float pos[3];
            positions->ConvertValue<float>(i, 3, pos);
            its.vertices.emplace_back(pos[0], pos[1], pos[2]);
        }

        size_t num_faces = dracoMesh.num_faces();
        its.indices.reserve(num_faces);
        for (FaceIndex i(0); i < num_faces; ++ i) {
            Mesh::Face face = dracoMesh.face(i);

            its.indices.emplace_back(
                positions->mapped_index(face[0]).value(),
                positions->mapped_index(face[1]).value(),
                positions->mapped_index(face[2]).value()
            );
        }

        *meshptr = TriangleMesh(std::move(its));
        if (meshptr->volume() < 0)
            meshptr->flip_triangles();
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "load_drc: " << e.what();
        return false;
    }
    return true;
}

bool load_drc(const char *path, Model *model, const char *object_name_in)
{
    TriangleMesh mesh;

    bool ret = load_drc(path, &mesh);

    if (ret) {
        std::string  object_name;
        if (object_name_in == nullptr) {
            const char *last_slash = strrchr(path, DIR_SEPARATOR);
            object_name.assign((last_slash == nullptr) ? path : last_slash + 1);
        } else
           object_name.assign(object_name_in);

        model->add_object(object_name.c_str(), path, std::move(mesh));
    }

    return ret;
}

bool store_drc(const char *path, TriangleMesh *mesh, int bits, int speed)
{
    try {
        const std::vector<stl_triangle_vertex_indices>* indices = &(mesh->its.indices);
        const std::vector<stl_vertex>* vertices = &(mesh->its.vertices);
        
        Mesh dracoMesh;

        dracoMesh.set_num_points(vertices->size());
        
        GeometryAttribute gaPos;
        gaPos.Init(GeometryAttribute::POSITION, nullptr, 3, DT_FLOAT32, false, sizeof(float)*3, 0);
        int32_t idPos = dracoMesh.AddAttribute(gaPos, true, indices->size() * 3);
        
        dracoMesh.attribute(idPos)->Resize(vertices->size());
        
        for (size_t i = 0; i < vertices->size(); ++ i) {
            float vertex[3];
            vertex[0] = vertices->at(i)(0);
            vertex[1] = vertices->at(i)(1);
            vertex[2] = vertices->at(i)(2);
            dracoMesh.attribute(idPos)->SetAttributeValue(AttributeValueIndex(i), vertex);
        }
        
        dracoMesh.SetNumFaces(indices->size());
        for (size_t i = 0; i < indices->size(); ++ i) {
            Mesh::Face face;
            face[0] = PointIndex(indices->at(i)[0]);
            face[1] = PointIndex(indices->at(i)[1]);
            face[2] = PointIndex(indices->at(i)[2]);
            dracoMesh.SetFace(FaceIndex(i), face);
        }
        
        Encoder encoder;
        encoder.SetSpeedOptions(speed, speed);
        encoder.SetAttributeQuantization(GeometryAttribute::POSITION, bits);
        
        EncoderBuffer buffer;
        encoder.EncodeMeshToBuffer(dracoMesh, &buffer);
        
        FILE* fp = boost::nowide::fopen(path, "wb");
        if (!fp) return false;
        size_t written = fwrite(buffer.data(), 1, buffer.size(), fp);
        fclose(fp);
        
        if (written != buffer.size()) return false;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "store_drc: " << e.what();
        return false;
    }
    return true;
}

bool store_drc(const char *path, ModelObject *model_object, int bits, int speed)
{
    TriangleMesh mesh = model_object->mesh();
    return store_drc(path, &mesh, bits, speed);
}

bool store_drc(const char *path, Model *model, int bits, int speed)
{
    TriangleMesh mesh = model->mesh();
    return store_drc(path, &mesh, bits, speed);
}

}; // namespace Slic3r
