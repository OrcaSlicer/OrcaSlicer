#ifndef slic3r_Emboss_hpp_
#define slic3r_Emboss_hpp_

#include <vector>
#include <set>
#include <optional>
#include <memory>
#include <admesh/stl.h> // indexed_triangle_set
#include "Polygon.hpp"
#include "ExPolygon.hpp"
#include "EmbossShape.hpp" // ExPolygonsWithIds
#include "BoundingBox.hpp"
#include "TextConfiguration.hpp"

namespace Slic3r {

/// <summary>
/// class with only static function add ability to engraved OR raised
/// text OR polygons onto model surface
/// </summary>
namespace Emboss
{    
    static const float UNION_DELTA = 50.0f; // [approx in nano meters depends on volume scale]
    static const unsigned UNION_MAX_ITERATIN = 10; // [count]

    /// <summary>
    /// Collect fonts registred inside OS
    /// </summary>
    /// <returns>OS registred TTF font files(full path) with names</returns>
    EmbossStyles get_font_list();
#ifdef _WIN32
    EmbossStyles get_font_list_by_register();
    EmbossStyles get_font_list_by_enumeration();
    EmbossStyles get_font_list_by_folder();
#endif

    /// <summary>
    /// OS dependent function to get location of font by its name descriptor
    /// </summary>
    /// <param name="font_face_name">Unique identificator for font</param>
    /// <returns>File path to font when found</returns>
    std::optional<std::wstring> get_font_path(const std::wstring &font_face_name);

    // description of one letter
    struct Glyph
    {
        // NOTE: shape is scaled by SHAPE_SCALE 
        // to be able store points without floating points
        ExPolygons shape;

        // values are in font points
        int advance_width=0, left_side_bearing=0;
    };
    // cache for glyph by unicode
    using Glyphs = std::map<int, Glyph>;
        
    /// <summary>
    /// keep information from file about font 
    /// (store file data itself)
    /// + cache data readed from buffer
    /// </summary>
    struct FontFile
    {
        // loaded data from font file
        // must store data size for imgui rasterization
        // To not store data on heap and To prevent unneccesary copy
        // data are stored inside unique_ptr
        std::unique_ptr<std::vector<unsigned char>> data;

        struct Info
        {
            // vertical position is "scale*(ascent - descent + lineGap)"
            int ascent, descent, linegap;

            // for convert font units to pixel
            int unit_per_em;
        };
        // info for each font in data
        std::vector<Info> infos;

        FontFile(std::unique_ptr<std::vector<unsigned char>> data,
                 std::vector<Info>                         &&infos)
            : data(std::move(data)), infos(std::move(infos))
        {
            assert(this->data != nullptr);
            assert(!this->data->empty());
        }

        bool operator==(const FontFile &other) const {
            if (data->size() != other.data->size())
                return false;
            //if(*data != *other.data) return false;
            for (size_t i = 0; i < infos.size(); i++) 
                if (infos[i].ascent != other.infos[i].ascent ||
                    infos[i].descent == other.infos[i].descent ||
                    infos[i].linegap == other.infos[i].linegap)
                    return false;
            return true;
        }
    };

    /// <summary>
    /// Add caching for shape of glyphs
    /// </summary>
    struct FontFileWithCache
    {
        // Pointer on data of the font file
        std::shared_ptr<const FontFile> font_file;

        // Cache for glyph shape
        // IMPORTANT: accessible only in plater job thread !!!
        // main thread only clear cache by set to another shared_ptr
        std::shared_ptr<Emboss::Glyphs> cache;

        FontFileWithCache() : font_file(nullptr), cache(nullptr) {}
        explicit FontFileWithCache(std::unique_ptr<FontFile> font_file)
            : font_file(std::move(font_file))
            , cache(std::make_shared<Emboss::Glyphs>())
        {}
        bool has_value() const { return font_file != nullptr && cache != nullptr; }
    };

    /// <summary>
    /// Load font file into buffer
    /// </summary>
    /// <param name="file_path">Location of .ttf or .ttc font file</param>
    /// <returns>Font object when loaded.</returns>
    std::unique_ptr<FontFile> create_font_file(const char *file_path);
    // data = raw file data
    std::unique_ptr<FontFile> create_font_file(std::unique_ptr<std::vector<unsigned char>> data);
#ifdef _WIN32
    // fix for unknown pointer HFONT is replaced with "void *"
    void * can_load(void* hfont);
    std::unique_ptr<FontFile> create_font_file(void * hfont);
#endif // _WIN32

    /// <summary>
    /// convert letter into polygons
    /// </summary>
    /// <param name="font">Define fonts</param>
    /// <param name="font_index">Index of font in collection</param>
    /// <param name="letter">One character defined by unicode codepoint</param>
    /// <param name="flatness">Precision of lettter outline curve in conversion to lines</param>
    /// <returns>inner polygon cw(outer ccw)</returns>
    std::optional<Glyph> letter2glyph(const FontFile &font, unsigned int font_index, int letter, float flatness);

    /// <summary>
    /// Convert text into polygons
    /// </summary>
    /// <param name="font">Define fonts + cache, which could extend</param>
    /// <param name="text">Characters to convert</param>
    /// <param name="font_prop">User defined property of the font</param>
    /// <param name="was_canceled">Way to interupt processing</param>
    /// <returns>Inner polygon cw(outer ccw)</returns>
    HealedExPolygons  text2shapes (FontFileWithCache &font, const char *text,         const FontProp &font_prop, const std::function<bool()> &was_canceled = []() {return false;});
    ExPolygonsWithIds text2vshapes(FontFileWithCache &font, const std::wstring& text, const FontProp &font_prop, const std::function<bool()>& was_canceled = []() {return false;});

    /// <summary>
    /// Convert text into triangle set
    /// </summary>
    /// <param name="font">Define fonts + cache, which could extend</param>
    /// <param name="text">Characters to convert</param>
    /// <param name="font_prop">User defined property of the font</param>
    /// <param name="dx_y_z_size">The size of the printed text {dx, y, z}. "dx" is the width of the character relative to its size (default is 1.0). "y" is the font height in millimeters. "z"S is the height of the extruded text in millimeters.</param>
    /// <returns>Indexed triangle set</returns>
    indexed_triangle_set text2model(FontFileWithCache& font, const char *text, const FontProp &font_prop, Vec3d &dx_y_z_size);
    
    const unsigned ENTER_UNICODE = static_cast<unsigned>('\n');
    /// Sum of character '\n'
    unsigned get_count_lines(const std::wstring &ws);
    unsigned get_count_lines(const std::string &text);
    unsigned get_count_lines(const ExPolygonsWithIds &shape);

    /// <summary>
    /// Fix duplicit points and self intersections in polygons.
    /// Also try to reduce amount of points and remove useless polygon parts
    /// </summary>
    /// <param name="is_non_zero">Fill type ClipperLib::pftNonZero for overlapping otherwise </param>
    /// <param name="max_iteration">Look at heal_expolygon()::max_iteration</param>
    /// <returns>Healed shapes with flag is fully healed</returns>
    HealedExPolygons heal_polygons(const Polygons &shape, bool is_non_zero = true, unsigned max_iteration = 10);

    /// <summary>
    /// NOTE: call Slic3r::union_ex before this call
    /// 
    /// Heal (read: Fix) issues in expolygons:
    ///  - self intersections
    ///  - duplicit points
    ///  - points close to line segments
    /// </summary>
    /// <param name="shape">In/Out shape to heal</param>
    /// <param name="max_iteration">Heal could create another issue,
    /// After healing it is checked again until shape is good or maximal count of iteration</param>
    /// <returns>True when shapes is good otherwise False</returns>
    bool heal_expolygons(ExPolygons &shape, unsigned max_iteration = 10);

    /// <summary>
    /// Divide line segments in place near to point
    /// (which could lead to self intersection due to preccision)
    /// Remove same neighbors
    /// Note: Possible part of heal shape
    /// </summary>
    /// <param name="expolygons">Expolygon to edit</param>
    /// <param name="distance">(epsilon)Euclidean distance from point to line which divide line</param>
    /// <returns>True when some division was made otherwise false</returns>
    bool divide_segments_for_close_point(ExPolygons &expolygons, double distance);

    /// <summary>
    /// Use data from font property to modify transformation
    /// </summary>
    /// <param name="angle">Z-rotation as angle to Y axis</param>
    /// <param name="distance">Z-move as surface distance</param>
    /// <param name="transformation">In / Out transformation to modify by property</param>
    void apply_transformation(const std::optional<float> &angle, const std::optional<float> &distance, Transform3d &transformation);

    /// <summary>
    /// Read information from naming table of font file
    /// search for italic (or oblique), bold italic (or bold oblique)
    /// </summary>
    /// <param name="font">Selector of font</param>
    /// <param name="font_index">Index of font in collection</param>
    /// <returns>True when the font description contains italic/obligue otherwise False</returns>
    bool is_italic(const FontFile &font, unsigned int font_index);

    /// <summary>
    /// Create unique character set from string with filtered from text with only character from font
    /// </summary>
    /// <param name="text">Source vector of glyphs</param>
    /// <param name="font">Font descriptor</param>
    /// <param name="font_index">Define font in collection</param>
    /// <param name="exist_unknown">True when text contain glyph unknown in font</param>
    /// <returns>Unique set of character from text contained in font</returns>
    std::string create_range_text(const std::string &text, const FontFile &font, unsigned int font_index, bool* exist_unknown = nullptr);    

    /// <summary>
    /// Calculate scale for glyph shape convert from shape points to mm
    /// </summary>
    /// <param name="fp">Property of font</param>
    /// <param name="ff">Font data</param>
    /// <returns>Conversion to mm</returns>
    double get_text_shape_scale(const FontProp &fp, const FontFile &ff);

    /// <summary>
    /// getter of font info by collection defined in prop
    /// </summary>
    /// <param name="font">Contain infos about all fonts(collections) in file</param>
    /// <param name="prop">Index of collection</param>
    /// <returns>Ascent, descent, line gap</returns>
    const FontFile::Info &get_font_info(const FontFile &font, const FontProp &prop);

    /// <summary>
    /// Read from font file and properties height of line with spacing
    /// </summary>
    /// <param name="font">Infos for collections</param>
    /// <param name="prop">Collection index + Additional line gap</param>
    /// <returns>Line height with spacing in scaled font points (same as ExPolygons)</returns>
    int get_line_height(const FontFile &font, const FontProp &prop);

    /// <summary>
    /// Calculate Vertical align
    /// </summary>
    /// <param name="align">Top | Center | Bottom</param>
    /// <param name="count_lines"></param>
    /// <returns>Return align Y offset in mm</returns>
    double get_align_y_offset_in_mm(FontProp::VerticalAlign align, unsigned count_lines, const FontFile &ff, const FontProp &fp);

    /// <summary>
    /// Project spatial point
    /// </summary>
    class IProject3d
    {
    public:
        virtual ~IProject3d() = default;
        /// <summary>
        /// Move point with respect to projection direction
        /// e.g. Orthogonal projection will move with point by direction
        /// e.g. Spherical projection need to use center of projection
        /// </summary>
        /// <param name="point">Spatial point coordinate</param>
        /// <returns>Projected spatial point</returns>
        virtual Vec3d project(const Vec3d &point) const = 0;
    };

    /// <summary>
    /// Project 2d point into space
    /// Could be plane, sphere, cylindric, ...
    /// </summary>
    class IProjection : public IProject3d
    {
    public:
        virtual ~IProjection() = default;

        /// <summary>
        /// convert 2d point to 3d points
        /// </summary>
        /// <param name="p">2d coordinate</param>
        /// <returns>
        /// first - front spatial point
        /// second - back spatial point
        /// </returns>
        virtual std::pair<Vec3d, Vec3d> create_front_back(const Point &p) const = 0;

        /// <summary>
        /// Back projection
        /// </summary>
        /// <param name="p">Point to project</param>
        /// <param name="depth">[optional] Depth of 2d projected point. Be careful number is in 2d scale</param>
        /// <returns>Uprojected point when it is possible</returns>
        virtual std::optional<Vec2d> unproject(const Vec3d &p, double * depth = nullptr) const = 0;
    };

    /// <summary>
    /// Create triangle model for text
    /// </summary>
    /// <param name="shape2d">text or image</param>
    /// <param name="projection">Define transformation from 2d to 3d(orientation, position, scale, ...)</param>
    /// <returns>Projected shape into space</returns>
    indexed_triangle_set polygons2model(const ExPolygons &shape2d, const IProjection& projection);
    
    /// <summary>
    /// Suggest wanted up vector of embossed text by emboss direction
    /// </summary>
    /// <param name="normal">Normalized vector of emboss direction in world</param>
    /// <param name="up_limit">Is compared with normal.z to suggest up direction</param>
    /// <returns>Wanted up vector</returns>
    Vec3d suggest_up(const Vec3d normal, double up_limit = 0.9);
        
    /// <summary>
    /// By transformation calculate angle between suggested and actual up vector
    /// </summary>
    /// <param name="tr">Transformation of embossed volume in world</param>
    /// <param name="up_limit">Is compared with normal.z to suggest up direction</param>
    /// <returns>Rotation of suggested up-vector[in rad] in the range [-Pi, Pi], When rotation is not zero</returns>
    std::optional<float> calc_up(const Transform3d &tr, double up_limit = 0.9);

    /// <summary>
    /// Create transformation for emboss text object to lay on surface point
    /// </summary>
    /// <param name="position">Position of surface point</param>
    /// <param name="normal">Normal of surface point</param>
    /// <param name="up_limit">Is compared with normal.z to suggest up direction</param>
    /// <returns>Transformation onto surface point</returns>
    Transform3d create_transformation_onto_surface(
        const Vec3d &position, const Vec3d &normal, double up_limit = 0.9);

    class ProjectZ : public IProjection
    {
    public:
        explicit ProjectZ(double depth) : m_depth(depth) {}
        // Inherited via IProject
        std::pair<Vec3d, Vec3d> create_front_back(const Point &p) const override;
        Vec3d project(const Vec3d &point) const override;
        std::optional<Vec2d> unproject(const Vec3d &p, double * depth = nullptr) const override;
        double m_depth;
    };

    class ProjectScale : public IProjection
    {
        std::unique_ptr<IProjection> core;
        double m_scale;
    public:
        ProjectScale(std::unique_ptr<IProjection> core, double scale)
            : core(std::move(core)), m_scale(scale)
        {}

        // Inherited via IProject
        std::pair<Vec3d, Vec3d> create_front_back(const Point &p) const override
        {
            auto res = core->create_front_back(p);
            return std::make_pair(res.first * m_scale, res.second * m_scale);
        }
        Vec3d project(const Vec3d &point) const override{
            return core->project(point);
        }
        std::optional<Vec2d> unproject(const Vec3d &p, double *depth = nullptr) const override {
            auto res = core->unproject(p / m_scale, depth);
            if (depth != nullptr) *depth *= m_scale;
            return res;
        }
    };

    class ProjectTransform : public IProjection
    {
        std::unique_ptr<IProjection> m_core;
        Transform3d m_tr;
        Transform3d m_tr_inv;
        double z_scale;
    public:
        ProjectTransform(std::unique_ptr<IProjection> core, const Transform3d &tr) : m_core(std::move(core)), m_tr(tr)
        {
            m_tr_inv = m_tr.inverse();
            z_scale  = (m_tr.linear() * Vec3d::UnitZ()).norm();
        }

        // Inherited via IProject
        std::pair<Vec3d, Vec3d> create_front_back(const Point &p) const override
        {
            auto [front, back] = m_core->create_front_back(p);
            return std::make_pair(m_tr * front, m_tr * back);
        }
        Vec3d project(const Vec3d &point) const override{
            return m_core->project(point);
        }
        std::optional<Vec2d> unproject(const Vec3d &p, double *depth = nullptr) const override {
            auto res = m_core->unproject(m_tr_inv * p, depth);
            if (depth != nullptr)
                *depth *= z_scale;
            return res;
        }
    };

    class OrthoProject3d : public Emboss::IProject3d
    {
        // size and direction of emboss for ortho projection
        Vec3d m_direction;
    public:
        OrthoProject3d(Vec3d direction) : m_direction(direction) {}
        Vec3d project(const Vec3d &point) const override{ return point + m_direction;}
    };

    class OrthoProject: public Emboss::IProjection {
        Transform3d m_matrix;
        // size and direction of emboss for ortho projection
        Vec3d       m_direction;
        Transform3d m_matrix_inv;
    public:
        OrthoProject(Transform3d matrix, Vec3d direction)
            : m_matrix(matrix), m_direction(direction), m_matrix_inv(matrix.inverse())
        {}
        // Inherited via IProject
        std::pair<Vec3d, Vec3d> create_front_back(const Point &p) const override;
        Vec3d project(const Vec3d &point) const override;
        std::optional<Vec2d> unproject(const Vec3d &p, double * depth = nullptr) const override;     
    };

    /// <summary>
    /// Define polygon for draw letters
    /// </summary>
    struct TextLine
    {
        // slice of object
        Polygon polygon;

        // point laying on polygon closest to zero
        PolygonPoint start;

        // offset of text line in volume mm
        float y;
    };
    using TextLines = std::vector<TextLine>;

    /// <summary>
    /// Sample slice polygon by bounding boxes centers
    /// slice start point has shape_center_x coor
    /// </summary>
    /// <param name="slice">Polygon and start point[Slic3r scaled milimeters]</param>
    /// <param name="bbs">Bounding boxes of letter on one line[in font scales]</param>
    /// <param name="scale">Scale for bbs (after multiply bb is in milimeters)</param>
    /// <returns>Sampled polygon by bounding boxes</returns>
    PolygonPoints sample_slice(const TextLine &slice, const BoundingBoxes &bbs, double scale);

    /// <summary>
    /// Calculate angle for polygon point
    /// </summary>
    /// <param name="distance">Distance for found normal in point</param>
    /// <param name="polygon_point">Select point on polygon</param>
    /// <param name="polygon">Polygon know neighbor of point</param>
    /// <returns>angle(atan2) of normal in polygon point</returns>
    double calculate_angle(int32_t distance, PolygonPoint polygon_point, const Polygon &polygon);
    std::vector<double> calculate_angles(int32_t distance, const PolygonPoints& polygon_points, const Polygon &polygon);

} // namespace Emboss

///////////////////////
// Move to ExPolygonsWithIds Utils
void translate(ExPolygonsWithIds &e, const Point &p);
BoundingBox get_extents(const ExPolygonsWithIds &e);
void center(ExPolygonsWithIds &e);
// delta .. safe offset before union (use as boolean close)
// NOTE: remove unprintable spaces between neighbor curves (made by linearization of curve)
ExPolygons union_with_delta(EmbossShape &shape, float delta, unsigned max_heal_iteration);
} // namespace Slic3r
#endif // slic3r_Emboss_hpp_
