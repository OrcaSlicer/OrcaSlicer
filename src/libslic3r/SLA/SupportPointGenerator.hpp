#ifndef SLA_SUPPORTPOINTGENERATOR_HPP
#define SLA_SUPPORTPOINTGENERATOR_HPP

#include <random>

#include <libslic3r/SLA/SupportPoint.hpp>
#include <libslic3r/SLA/IndexedMesh.hpp>

#include <libslic3r/BoundingBox.hpp>
#include <libslic3r/ClipperUtils.hpp>
#include <libslic3r/Point.hpp>

#include <boost/container/small_vector.hpp>

// #define SLA_SUPPORTPOINTGEN_DEBUG

namespace Slic3r { namespace sla {

class SupportPointGenerator {
public:
    struct Config {
        float density_relative {1.f};
        float minimal_distance {1.f};
        float head_diameter {0.4f};

        // Originally calibrated to 7.7f, reduced density by Tamas to 70% which is 11.1 (7.7 / 0.7) to adjust for new algorithm changes in tm_suppt_gen_improve
        inline float support_force() const { return 11.1f / density_relative; } // a force one point can support       (arbitrary force unit)
        inline float tear_pressure() const { return 1.f; }  // pressure that the display exerts    (the force unit per mm2)
    };
    
    SupportPointGenerator(const IndexedMesh& emesh, const std::vector<ExPolygons>& slices,
                    const std::vector<float>& heights, const Config& config, std::function<void(void)> throw_on_cancel, std::function<void(int)> statusfn);
    
    SupportPointGenerator(const IndexedMesh& emesh, const Config& config, std::function<void(void)> throw_on_cancel, std::function<void(int)> statusfn);
    
    const std::vector<SupportPoint>& output() const { return m_output; }
    std::vector<SupportPoint>& output() { return m_output; }
    
    struct MyLayer;
    
    struct Structure {
        Structure(MyLayer &layer, const ExPolygon& poly, const BoundingBox &bbox, const Vec2f &centroid, float area, float h) :
            layer(&layer), polygon(&poly), bbox(bbox), centroid(centroid), area(area), zlevel(h)
#ifdef SLA_SUPPORTPOINTGEN_DEBUG
            , unique_id(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()))
#endif /* SLA_SUPPORTPOINTGEN_DEBUG */
        {}
        MyLayer *layer;
        const ExPolygon* polygon = nullptr;
        const BoundingBox bbox;
        const Vec2f centroid = Vec2f::Zero();
        const float area = 0.f;
        float zlevel = 0;
        // How well is this ExPolygon held to the print base?
        // Positive number, the higher the better.
        float supports_force_this_layer     = 0.f;
        float supports_force_inherited      = 0.f;
        float supports_force_total() const { return this->supports_force_this_layer + this->supports_force_inherited; }
#ifdef SLA_SUPPORTPOINTGEN_DEBUG
        std::chrono::milliseconds unique_id;
#endif /* SLA_SUPPORTPOINTGEN_DEBUG */
        
        struct Link {
            Link(Structure *island, float overlap_area) : island(island), overlap_area(overlap_area) {}
            Structure   *island;
            float        overlap_area;
        };

#ifdef NDEBUG
        // In release mode, use the optimized container.
        boost::container::small_vector<Link, 4> islands_above;
        boost::container::small_vector<Link, 4> islands_below;
#else
        // In debug mode, use the standard vector, which is well handled by debugger visualizer.
        std::vector<Link>					 	islands_above;
        std::vector<Link>						islands_below;
#endif
        // Overhangs, that are dangling considerably.
        ExPolygons                              dangling_areas;
        // Complete overhands.
        ExPolygons                              overhangs;
        // Overhangs, where the surface must slope.
        ExPolygons                              overhangs_slopes;
        float                                   overhangs_area = 0.f;
        
        bool overlaps(const Structure &rhs) const { 
            //FIXME ExPolygon::overlaps() shall be commutative, it is not!
            return this->bbox.overlap(rhs.bbox) && (this->polygon->overlaps(*rhs.polygon) || rhs.polygon->overlaps(*this->polygon)); 
        }
        float overlap_area(const Structure &rhs) const { 
            double out = 0.;
            if (this->bbox.overlap(rhs.bbox)) {
                Polygons polys = intersection(*this->polygon, *rhs.polygon);
                for (const Polygon &poly : polys)
                    out += poly.area();
            }
            return float(out);
        }
        float area_below() const { 
            float area = 0.f; 
            for (const Link &below : this->islands_below) 
                area += below.island->area; 
            return area;
        }
        Polygons polygons_below() const { 
            size_t cnt = 0;
            for (const Link &below : this->islands_below)
                cnt += 1 + below.island->polygon->holes.size();
            Polygons out;
            out.reserve(cnt);
            for (const Link &below : this->islands_below) {
                out.emplace_back(below.island->polygon->contour);
                append(out, below.island->polygon->holes);
            }
            return out;
        }
        ExPolygons expolygons_below() const { 
            ExPolygons out;
            out.reserve(this->islands_below.size());
            for (const Link &below : this->islands_below)
                out.emplace_back(*below.island->polygon);
            return out;
        }
        // Positive deficit of the supports. If negative, this area is well supported. If positive, more supports need to be added.
        float support_force_deficit(const float tear_pressure) const { return this->area * tear_pressure - this->supports_force_total(); }
    };
    
    struct MyLayer {
        MyLayer(const size_t layer_id, coordf_t print_z) : layer_id(layer_id), print_z(print_z) {}
        size_t                  layer_id;
        coordf_t                print_z;
        std::vector<Structure>  islands;
    };
    
    struct RichSupportPoint {
        Vec3f        position;
        Structure   *island;
    };
    
    struct PointGrid3D {
        struct GridHash {
            std::size_t operator()(const Vec3i32 &cell_id) const {
                return std::hash<int>()(cell_id.x()) ^ std::hash<int>()(cell_id.y() * 593) ^ std::hash<int>()(cell_id.z() * 7919);
            }
        };
        typedef std::unordered_multimap<Vec3i32, RichSupportPoint, GridHash> Grid;
        
        Vec3f   cell_size;
        Grid    grid;
        
        Vec3i32 cell_id(const Vec3f &pos) {
            return Vec3i32(int(floor(pos.x() / cell_size.x())),
                         int(floor(pos.y() / cell_size.y())),
                         int(floor(pos.z() / cell_size.z())));
        }
        
        void insert(const Vec2f &pos, Structure *island) {
            RichSupportPoint pt;
            pt.position = Vec3f(pos.x(), pos.y(), float(island->layer->print_z));
            pt.island   = island;
            grid.emplace(cell_id(pt.position), pt);
        }
        
        bool collides_with(const Vec2f &pos, float print_z, float radius) {
            Vec3f pos3d(pos.x(), pos.y(), print_z);
            Vec3i32 cell = cell_id(pos3d);
            std::pair<Grid::const_iterator, Grid::const_iterator> it_pair = grid.equal_range(cell);
            if (collides_with(pos3d, radius, it_pair.first, it_pair.second))
                return true;
            for (int i = -1; i < 2; ++ i)
                for (int j = -1; j < 2; ++ j)
                    for (int k = -1; k < 1; ++ k) {
                        if (i == 0 && j == 0 && k == 0)
                            continue;
                        it_pair = grid.equal_range(cell + Vec3i32(i, j, k));
                        if (collides_with(pos3d, radius, it_pair.first, it_pair.second))
                            return true;
                    }
            return false;
        }
        
    private:
        bool collides_with(const Vec3f &pos, float radius, Grid::const_iterator it_begin, Grid::const_iterator it_end) {
            for (Grid::const_iterator it = it_begin; it != it_end; ++ it) {
                float dist2 = (it->second.position - pos).squaredNorm();
                if (dist2 < radius * radius)
                    return true;
            }
            return false;
        }
    };
    
    void execute(const std::vector<ExPolygons> &slices,
                 const std::vector<float> &     heights);
    
    void seed(std::mt19937::result_type s) { m_rng.seed(s); }
private:
    std::vector<SupportPoint> m_output;
    
    SupportPointGenerator::Config m_config;
    
    void process(const std::vector<ExPolygons>& slices, const std::vector<float>& heights);

public:
    enum IslandCoverageFlags : uint8_t { icfNone = 0x0, icfIsNew = 0x1, icfWithBoundary = 0x2 };

private:

    void uniformly_cover(const ExPolygons& islands, Structure& structure, float deficit, PointGrid3D &grid3d, IslandCoverageFlags flags = icfNone);

    void add_support_points(Structure& structure, PointGrid3D &grid3d);

    void project_onto_mesh(std::vector<SupportPoint>& points) const;

#ifdef SLA_SUPPORTPOINTGEN_DEBUG
    static void output_expolygons(const ExPolygons& expolys, const std::string &filename);
    static void output_structures(const std::vector<Structure> &structures);
#endif // SLA_SUPPORTPOINTGEN_DEBUG
    
    const IndexedMesh& m_emesh;
    std::function<void(void)> m_throw_on_cancel;
    std::function<void(int)>  m_statusfn;
    
    std::mt19937 m_rng;
};

void remove_bottom_points(std::vector<SupportPoint> &pts, float lvl);

std::vector<Vec2f> sample_expolygon(const ExPolygon &expoly, float samples_per_mm2, std::mt19937 &rng);
void sample_expolygon_boundary(const ExPolygon &expoly, float samples_per_mm, std::vector<Vec2f> &out, std::mt19937 &rng);

}} // namespace Slic3r::sla

#endif // SUPPORTPOINTGENERATOR_HPP
