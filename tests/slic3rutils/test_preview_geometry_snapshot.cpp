#include <catch2/catch_all.hpp>

#include <slic3r/GUI/ToolpathMeshBuilder.hpp>
#include <slic3r/plugin/host/PreviewGeometrySnapshot.hpp>
#include <libslic3r/GCode/GCodeProcessor.hpp>
#include <libvgcode/include/GCodeInputData.hpp>
#include <libvgcode/include/Viewer.hpp>

#include "test_utils.hpp"

#include <boost/filesystem.hpp>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <vector>

using namespace Slic3r;
namespace fs = boost::filesystem;
namespace ORPM = PreviewGeometrySnapshot;

namespace {
ORPM::Snapshot sample_snapshot()
{
    ORPM::Snapshot s; s.scene_id=42;s.plate_index=3;s.printable_height=250;s.bounds_min={{0,0,0}};s.bounds_max={{1,1,1}};
    s.vertices={{{0,0,0},{0,0,1},{0,0}},{{1,0,0},{0,0,1},{1,0}},{{1,1,0},{0,0,1},{1,1}},{{0,1,0},{0,0,1},{0,1}}};
    s.indices={0,1,2,0,2,3};s.groups={{0,6,0,1,0,0,0}};s.materials={{0,{{10,20,30,255}},"preset","Preset"}};
    s.printable_area={{0,0},{200,0},{200,200},{0,200}};return s;
}
template<class T> void put_le(std::vector<uint8_t>& d,size_t o,T v){using U=std::make_unsigned_t<T>;U u=static_cast<U>(v);for(size_t i=0;i<sizeof(T);++i)d[o+i]=uint8_t(u>>(8*i));}
}

TEST_CASE("ORPM serializes authoritative indexed mesh", "[PreviewGeometrySnapshot]")
{
    const auto bytes=ORPM::serialize(sample_snapshot());const auto h=ORPM::validate(bytes);
    CHECK(h.major_version==1);CHECK(h.header_size==256);CHECK(h.vertex_count==4);CHECK(h.index_count==6);
    CHECK(h.group_count==1);CHECK(h.material_slot_count==1);CHECK(h.file_size==bytes.size());
}

TEST_CASE("ORPM rejects malformed mesh", "[PreviewGeometrySnapshot]")
{
    const auto valid=ORPM::serialize(sample_snapshot());
    SECTION("magic"){auto d=valid;d[0]='X';CHECK_THROWS(ORPM::validate(d));}
    SECTION("index"){auto d=valid;put_le<uint32_t>(d,256+4*32,99);CHECK_THROWS(ORPM::validate(d));}
    SECTION("group coverage"){auto d=valid;const uint64_t groups=256+4*32+6*4;put_le<uint32_t>(d,groups+4,3);CHECK_THROWS(ORPM::validate(d));}
    SECTION("non-finite"){auto d=valid;uint32_t nan=0x7fc00000;put_le(d,256,nan);CHECK_THROWS(ORPM::validate(d));}
    SECTION("limit"){CHECK_THROWS(ORPM::validate(valid,3));}
}

TEST_CASE("ORPM capture preserves shared preview mesh and result metadata", "[PreviewGeometrySnapshot]")
{
    GUI::PreviewTriangleMesh mesh;mesh.vertices={{{0,0,0},{0,0,1},{0,0}},{{1,0,0},{0,0,1},{0,0}},{{0,1,0},{0,0,1},{0,0}}};mesh.indices={0,1,2};mesh.groups={{0,3,0,1,0,0,4,{255,0,0}}};for(const auto&v:mesh.vertices)mesh.bounds.merge(v.position.cast<double>());
    GCodeProcessorResult result;result.id=77;result.printable_height=256;result.filaments_count=1;result.extruder_colors={"#123456"};result.settings_ids.filament={"filament"};
    const auto snapshot=ORPM::capture(mesh,result,2,77);CHECK(snapshot.scene_id==77);CHECK(snapshot.vertices.size()==3);CHECK(snapshot.indices==mesh.indices);CHECK(snapshot.groups.size()==1);CHECK(snapshot.materials.size()==1);CHECK_THROWS(ORPM::capture(mesh,result,2,76));
}

TEST_CASE("shared libvgcode mesh builder emits Orca caps and no travel", "[PreviewGeometrySnapshot]")
{
    libvgcode::GCodeInputData data;data.tools_colors.push_back({255,0,0});
    libvgcode::PathVertex start;start.position={0,0,0.2f};start.width=0.45f;start.height=0.2f;start.type=libvgcode::EMoveType::Extrude;start.role=libvgcode::EGCodeExtrusionRole::Perimeter;start.gcode_id=5;
    auto end=start;end.position={10,0,0.2f};
    auto travel=end;travel.position={20,0,0.2f};travel.type=libvgcode::EMoveType::Travel;travel.gcode_id=6;
    data.vertices={start,end,travel};
    const auto mesh=GUI::build_preview_triangle_mesh(data);CHECK(mesh.vertices.size()==10);CHECK(mesh.indices.size()==48);CHECK(mesh.groups.size()>=1);CHECK(mesh.bounds.min.x()<0);CHECK(mesh.bounds.max.x()>10);
}

TEST_CASE("ORPM snapshots publish atomically", "[PreviewGeometrySnapshot]")
{
    ScopedTemporaryDir temp("orpm-atomic");const fs::path target=temp.path()/"scene.orpm";ORPM::write_atomic(sample_snapshot(),target);REQUIRE(fs::exists(target));std::ifstream in(target.string(),std::ios::binary);std::vector<uint8_t>d((std::istreambuf_iterator<char>(in)),{});CHECK(ORPM::validate(d).scene_id==42);CHECK_FALSE(fs::exists(target.string()+".tmp"));
}
