#include "PreviewGeometrySnapshot.hpp"

#include "slic3r/GUI/ToolpathMeshBuilder.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

#include <boost/filesystem.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <type_traits>

namespace Slic3r::PreviewGeometrySnapshot {
namespace {

uint64_t checked_add(uint64_t a, uint64_t b) { if (b > std::numeric_limits<uint64_t>::max()-a) throw std::runtime_error("ORPM size overflow"); return a+b; }
uint64_t checked_mul(uint64_t a, uint64_t b) { if (a && b > std::numeric_limits<uint64_t>::max()/a) throw std::runtime_error("ORPM size overflow"); return a*b; }
void require_range(size_t size, uint64_t off, uint64_t len) { if (off > size || len > size-off) throw std::runtime_error("ORPM region out of bounds"); }
template<class T> void put_le(std::vector<uint8_t>& d, size_t o, T v) { static_assert(std::is_integral_v<T>); using U=std::make_unsigned_t<T>; U u=static_cast<U>(v); if(o>d.size()||sizeof(T)>d.size()-o)throw std::runtime_error("ORPM writer overflow"); for(size_t i=0;i<sizeof(T);++i)d[o+i]=uint8_t(u>>(8*i)); }
template<class T> T get_le(const uint8_t* d,size_t s,uint64_t o){static_assert(std::is_integral_v<T>);require_range(s,o,sizeof(T));using U=std::make_unsigned_t<T>;U u{};for(size_t i=0;i<sizeof(T);++i)u|=U(d[o+i])<<(8*i);return static_cast<T>(u);}
void put_float(std::vector<uint8_t>& d,size_t o,float v){uint32_t u;std::memcpy(&u,&v,4);put_le(d,o,u);} float get_float(const uint8_t*d,size_t s,uint64_t o){uint32_t u=get_le<uint32_t>(d,s,o);float v;std::memcpy(&v,&u,4);return v;}
void finite(float v,const char* what){if(!std::isfinite(v))throw std::runtime_error(std::string("ORPM non-finite ")+what);}
uint8_t channel(float value){return uint8_t(std::lround(std::clamp(value,0.0f,1.0f)*255.0f));}
uint64_t region_end(uint64_t off,uint64_t count,uint64_t stride,uint64_t size){const auto bytes=checked_mul(count,stride);if(off>size||bytes>size-off)throw std::runtime_error("ORPM section out of bounds");return off+bytes;}

} // namespace

Snapshot capture(const GUI::PreviewTriangleMesh& mesh,const GCodeProcessorResult& result,int plate_index,uint64_t expected_scene_id)
{
    if(mesh.vertices.empty()||mesh.indices.empty()||mesh.groups.empty())throw std::runtime_error("Preview mesh is empty");
    Snapshot out; out.scene_id=expected_scene_id; out.plate_index=plate_index; out.flags=FLAG_INDEXED;
    std::vector<std::string> colors,filament_ids; size_t filament_count{};
    {
        std::lock_guard<std::mutex> lock(result.result_mutex);
        if(result.id!=expected_scene_id)throw std::runtime_error("Slice result changed before ORPM capture");
        out.flags|=result.spiral_vase_mode?FLAG_SPIRAL_VASE:0; out.z_offset=result.z_offset;out.printable_height=result.printable_height;
        colors=result.extruder_colors;filament_ids=result.settings_ids.filament;filament_count=result.filaments_count;
        auto copy=[](const Pointfs& src,std::vector<Point>& dst){dst.reserve(src.size());for(const Vec2d&p:src)dst.push_back({float(p.x()),float(p.y())});};
        copy(result.printable_area,out.printable_area);copy(result.bed_exclude_area,out.bed_excluded_area);copy(result.wrapping_exclude_area,out.wrapping_excluded_area);
    }
    out.vertices.reserve(mesh.vertices.size());
    for(const auto& v:mesh.vertices)out.vertices.push_back({{v.position.x(),v.position.y(),v.position.z()},{v.normal.x(),v.normal.y(),v.normal.z()},{v.uv.x(),v.uv.y()}});
    out.indices=mesh.indices; out.groups.reserve(mesh.groups.size());
    uint16_t max_slot=0;
    for(const auto& g:mesh.groups){out.groups.push_back({g.first_index,g.index_count,g.material_slot,g.extrusion_role,g.extruder_id,g.colour_id,g.layer_id});max_slot=std::max(max_slot,g.material_slot);}
    const size_t material_count=std::max({colors.size(),filament_ids.size(),filament_count,size_t(max_slot)+1});
    if(material_count>DEFAULT_MAX_MATERIALS)throw std::runtime_error("ORPM material limit exceeded");
    for(size_t i=0;i<material_count;++i){MaterialSlot m;m.extruder_id=uint8_t(std::min<size_t>(i,255));ColorRGBA c=ColorRGBA::GRAY();if(i<colors.size())decode_color(colors[i],c);m.rgba={{channel(c.r()),channel(c.g()),channel(c.b()),channel(c.a())}};if(i<filament_ids.size()){m.preset_id=filament_ids[i];m.display_name=filament_ids[i];}out.materials.push_back(std::move(m));}
    if(mesh.bounds.defined){for(size_t i=0;i<3;++i){out.bounds_min[i]=mesh.bounds.min[int(i)];out.bounds_max[i]=mesh.bounds.max[int(i)];}}
    return out;
}

std::vector<uint8_t> serialize(const Snapshot& s)
{
    if(s.vertices.empty()||s.vertices.size()>DEFAULT_MAX_VERTICES||s.indices.empty()||s.indices.size()>DEFAULT_MAX_INDICES||s.groups.empty()||s.groups.size()>DEFAULT_MAX_GROUPS)throw std::runtime_error("ORPM count exceeds limit");
    struct Ref{uint32_t off,len;};struct MRef{Ref preset,name;};std::vector<uint8_t> strings;std::vector<MRef> refs;
    auto add_string=[&](const std::string& value){if(value.size()>UINT32_MAX||strings.size()>UINT32_MAX-value.size())throw std::runtime_error("ORPM string table too large");Ref r{uint32_t(strings.size()),uint32_t(value.size())};strings.insert(strings.end(),value.begin(),value.end());return r;};
    for(const auto&m:s.materials)refs.push_back({add_string(m.preset_id),add_string(m.display_name)});
    const uint64_t vo=HEADER_BYTE_SIZE,io=checked_add(vo,checked_mul(s.vertices.size(),VERTEX_RECORD_SIZE));
    const uint64_t go=checked_add(io,checked_mul(s.indices.size(),4));
    const uint64_t mo=checked_add(go,checked_mul(s.groups.size(),GROUP_RECORD_SIZE));
    const uint64_t po=checked_add(mo,checked_mul(s.materials.size(),MATERIAL_RECORD_SIZE));
    const uint64_t eo=checked_add(po,checked_mul(s.printable_area.size(),POINT_RECORD_SIZE));
    const uint64_t so=checked_add(eo,checked_mul(s.bed_excluded_area.size()+s.wrapping_excluded_area.size(),POINT_RECORD_SIZE));
    const uint64_t fs=checked_add(so,strings.size());if(fs>DEFAULT_MAX_FILE_SIZE||fs>SIZE_MAX)throw std::runtime_error("ORPM file too large");
    std::vector<uint8_t>d(size_t(fs),0);std::memcpy(d.data(),"ORPM",4);put_le(d,4,FORMAT_MAJOR);put_le(d,6,FORMAT_MINOR);put_le(d,8,HEADER_BYTE_SIZE);put_le(d,12,VERTEX_RECORD_SIZE);put_le(d,16,GROUP_RECORD_SIZE);put_le(d,20,POINT_RECORD_SIZE);put_le(d,24,s.scene_id);put_le(d,32,s.plate_index);put_le(d,36,s.flags|FLAG_INDEXED);put_le(d,40,uint64_t(s.vertices.size()));put_le(d,48,uint64_t(s.indices.size()));put_le(d,56,uint64_t(s.groups.size()));put_le(d,64,uint32_t(s.materials.size()));put_le(d,68,uint32_t(s.printable_area.size()));put_le(d,72,uint32_t(s.bed_excluded_area.size()+s.wrapping_excluded_area.size()));put_le(d,76,uint32_t(s.bed_excluded_area.size()));put_le(d,80,vo);put_le(d,88,io);put_le(d,96,go);put_le(d,104,mo);put_le(d,112,po);put_le(d,120,eo);put_le(d,128,so);put_le(d,136,fs);put_float(d,144,s.z_offset);put_float(d,148,s.printable_height);for(size_t i=0;i<3;++i){put_float(d,152+4*i,s.bounds_min[i]);put_float(d,164+4*i,s.bounds_max[i]);}
    size_t o=size_t(vo);for(const auto&v:s.vertices){for(float f:v.position){finite(f,"position");put_float(d,o,f);o+=4;}for(float f:v.normal){finite(f,"normal");put_float(d,o,f);o+=4;}for(float f:v.uv){finite(f,"uv");put_float(d,o,f);o+=4;}}
    o=size_t(io);for(uint32_t index:s.indices){if(index>=s.vertices.size())throw std::runtime_error("ORPM index out of range");put_le(d,o,index);o+=4;}
    o=size_t(go);uint64_t covered=0;for(const auto&g:s.groups){if(g.first_index!=covered||g.index_count==0||g.index_count%3||uint64_t(g.first_index)+g.index_count>s.indices.size()||g.material_slot>=s.materials.size())throw std::runtime_error("ORPM invalid group");put_le(d,o,g.first_index);put_le(d,o+4,g.index_count);put_le(d,o+8,g.material_slot);put_le(d,o+10,g.extrusion_role);put_le(d,o+11,g.extruder_id);put_le(d,o+12,g.colour_id);put_le(d,o+16,g.layer_id);covered+=g.index_count;o+=GROUP_RECORD_SIZE;}if(covered!=s.indices.size())throw std::runtime_error("ORPM groups do not cover indices");
    o=size_t(mo);for(size_t i=0;i<s.materials.size();++i){const auto&m=s.materials[i];const auto&r=refs[i];d[o]=m.extruder_id;std::copy(m.rgba.begin(),m.rgba.end(),d.begin()+o+1);put_le(d,o+8,so+r.preset.off);put_le(d,o+16,r.preset.len);put_le(d,o+20,r.name.len);put_le(d,o+24,so+r.name.off);o+=MATERIAL_RECORD_SIZE;}
    auto write_points=[&](const std::vector<Point>&pts){for(const auto&p:pts){finite(p.x,"plate x");finite(p.y,"plate y");put_float(d,o,p.x);put_float(d,o+4,p.y);o+=8;}};write_points(s.printable_area);write_points(s.bed_excluded_area);write_points(s.wrapping_excluded_area);std::copy(strings.begin(),strings.end(),d.begin()+size_t(so));validate(d);return d;
}

Header validate(const uint8_t*d,size_t size,uint64_t maxv,uint64_t maxi,uint64_t maxg,uint64_t maxfile)
{
    if(!d||size<HEADER_BYTE_SIZE||size>maxfile||std::memcmp(d,"ORPM",4))throw std::runtime_error("Invalid ORPM header");Header h;h.major_version=get_le<uint16_t>(d,size,4);h.minor_version=get_le<uint16_t>(d,size,6);h.header_size=get_le<uint32_t>(d,size,8);h.vertex_record_size=get_le<uint32_t>(d,size,12);h.group_record_size=get_le<uint32_t>(d,size,16);h.point_record_size=get_le<uint32_t>(d,size,20);if(h.major_version!=FORMAT_MAJOR||h.header_size<HEADER_BYTE_SIZE||h.header_size>size||h.vertex_record_size<VERTEX_RECORD_SIZE||h.group_record_size<GROUP_RECORD_SIZE||h.point_record_size<POINT_RECORD_SIZE)throw std::runtime_error("Unsupported ORPM layout");h.scene_id=get_le<uint64_t>(d,size,24);h.plate_index=get_le<int32_t>(d,size,32);h.flags=get_le<uint32_t>(d,size,36);h.vertex_count=get_le<uint64_t>(d,size,40);h.index_count=get_le<uint64_t>(d,size,48);h.group_count=get_le<uint64_t>(d,size,56);h.material_slot_count=get_le<uint32_t>(d,size,64);h.printable_area_count=get_le<uint32_t>(d,size,68);h.excluded_area_count=get_le<uint32_t>(d,size,72);h.bed_excluded_area_count=get_le<uint32_t>(d,size,76);h.vertices_offset=get_le<uint64_t>(d,size,80);h.indices_offset=get_le<uint64_t>(d,size,88);h.groups_offset=get_le<uint64_t>(d,size,96);h.materials_offset=get_le<uint64_t>(d,size,104);h.printable_area_offset=get_le<uint64_t>(d,size,112);h.excluded_area_offset=get_le<uint64_t>(d,size,120);h.strings_offset=get_le<uint64_t>(d,size,128);h.file_size=get_le<uint64_t>(d,size,136);h.z_offset=get_float(d,size,144);h.printable_height=get_float(d,size,148);for(size_t i=0;i<3;++i){h.bounds_min[i]=get_float(d,size,152+4*i);h.bounds_max[i]=get_float(d,size,164+4*i);finite(h.bounds_min[i],"bounds");finite(h.bounds_max[i],"bounds");}finite(h.z_offset,"z offset");finite(h.printable_height,"height");if(h.file_size!=size||!h.vertex_count||h.vertex_count>maxv||!h.index_count||h.index_count>maxi||!h.group_count||h.group_count>maxg||h.material_slot_count>DEFAULT_MAX_MATERIALS||h.bed_excluded_area_count>h.excluded_area_count)throw std::runtime_error("ORPM invalid count");const auto ve=region_end(h.vertices_offset,h.vertex_count,h.vertex_record_size,size),ie=region_end(h.indices_offset,h.index_count,4,size),ge=region_end(h.groups_offset,h.group_count,h.group_record_size,size),me=region_end(h.materials_offset,h.material_slot_count,MATERIAL_RECORD_SIZE,size),pe=region_end(h.printable_area_offset,h.printable_area_count,h.point_record_size,size),ee=region_end(h.excluded_area_offset,h.excluded_area_count,h.point_record_size,size);if(h.vertices_offset<h.header_size||h.indices_offset<ve||h.groups_offset<ie||h.materials_offset<ge||h.printable_area_offset<me||h.excluded_area_offset<pe||h.strings_offset<ee||h.strings_offset>size)throw std::runtime_error("ORPM overlapping sections");for(uint64_t i=0;i<h.vertex_count;++i){const uint64_t o=h.vertices_offset+i*h.vertex_record_size;float n2=0;for(int j=0;j<8;++j){float v=get_float(d,size,o+4*j);finite(v,"vertex");if(j>=3&&j<6)n2+=v*v;}if(n2<0.25f||n2>2.25f)throw std::runtime_error("ORPM invalid normal");}for(uint64_t i=0;i<h.index_count;++i)if(get_le<uint32_t>(d,size,h.indices_offset+4*i)>=h.vertex_count)throw std::runtime_error("ORPM index out of range");uint64_t covered=0;for(uint64_t i=0;i<h.group_count;++i){const uint64_t o=h.groups_offset+i*h.group_record_size;const auto first=get_le<uint32_t>(d,size,o),count=get_le<uint32_t>(d,size,o+4);const auto mat=get_le<uint16_t>(d,size,o+8);if(first!=covered||!count||count%3||uint64_t(first)+count>h.index_count||mat>=h.material_slot_count)throw std::runtime_error("ORPM invalid group");covered+=count;}if(covered!=h.index_count)throw std::runtime_error("ORPM group coverage mismatch");return h;
}
Header validate(const std::vector<uint8_t>&d,uint64_t a,uint64_t b,uint64_t c,uint64_t e){return validate(d.data(),d.size(),a,b,c,e);}
void write_atomic(const Snapshot&s,const boost::filesystem::path&target){const auto bytes=serialize(s);boost::filesystem::create_directories(target.parent_path());auto temp=target;temp += ".tmp";{std::ofstream out(temp.string(),std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));out.flush();if(!out)throw std::runtime_error("Failed to write ORPM snapshot");}boost::system::error_code ec;
#ifdef _WIN32
boost::filesystem::remove(target,ec);ec.clear();
#endif
boost::filesystem::rename(temp,target,ec);if(ec){boost::filesystem::remove(temp);throw std::runtime_error("Failed to publish ORPM snapshot: "+ec.message());}}

} // namespace Slic3r::PreviewGeometrySnapshot
