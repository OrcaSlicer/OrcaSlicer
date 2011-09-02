#include "Tesselate.hpp"

#include "ExPolygon.hpp"

#include <glu-libtess.h>

namespace Slic3r {

class GluTessWrapper {
public:
    GluTessWrapper() : m_tesselator(gluNewTess()) {
        // register callback functions
        gluTessCallback(m_tesselator, GLU_TESS_BEGIN_DATA,   (_GLUfuncptr)tessBeginCB);
        gluTessCallback(m_tesselator, GLU_TESS_END_DATA,     (_GLUfuncptr)tessEndCB);
        gluTessCallback(m_tesselator, GLU_TESS_ERROR_DATA,   (_GLUfuncptr)tessErrorCB);
        gluTessCallback(m_tesselator, GLU_TESS_VERTEX_DATA,  (_GLUfuncptr)tessVertexCB);
        gluTessCallback(m_tesselator, GLU_TESS_COMBINE_DATA, (_GLUfuncptr)tessCombineCB);        
    }
    ~GluTessWrapper() {
        gluDeleteTess(m_tesselator);
    }

    std::vector<Vec3d> tesselate3d(const ExPolygon &expoly, double z_, bool flipped_)
    {
        m_z = z_;
        m_flipped = flipped_;
        m_output_triangles.clear();
        std::vector<GLdouble> coords;
        {
            size_t num_coords = expoly.contour.points.size();
            for (const Polygon &poly : expoly.holes)
                num_coords += poly.points.size();
            coords.reserve(num_coords * 3);
        }
        gluTessBeginPolygon(m_tesselator, (void*)this);
        gluTessBeginContour(m_tesselator);
        for (const Point &pt : expoly.contour.points) {
            coords.emplace_back(unscale<double>(pt[0]));
            coords.emplace_back(unscale<double>(pt[1]));
            coords.emplace_back(0.);
            gluTessVertex(m_tesselator, &coords[coords.size() - 3], &coords[coords.size() - 3]);
        }
        gluTessEndContour(m_tesselator);
        for (const Polygon &poly : expoly.holes) {
            gluTessBeginContour(m_tesselator);
            for (const Point &pt : poly.points) {
                coords.emplace_back(unscale<double>(pt[0]));
                coords.emplace_back(unscale<double>(pt[1]));
                coords.emplace_back(0.);
                gluTessVertex(m_tesselator, &coords[coords.size() - 3], &coords[coords.size() - 3]);
            }
            gluTessEndContour(m_tesselator);
        }
        gluTessEndPolygon(m_tesselator);
        m_intersection_points.clear();
        return std::move(m_output_triangles);
    }

    std::vector<Vec3d> tesselate3d(const ExPolygons &expolygons, double z_, bool flipped_)
    {
        m_z = z_;
        m_flipped = flipped_;
        m_output_triangles.clear();
        std::vector<GLdouble> coords;
        {
            size_t num_coords = 0;
            for (const ExPolygon &expoly : expolygons) {
                size_t num_coords_this = expoly.contour.points.size();
                for (const Polygon &poly : expoly.holes)
                    num_coords_this += poly.points.size();
                num_coords = std::max(num_coords, num_coords_this);
            }
            coords.assign(num_coords * 3, 0);
        }
        for (const ExPolygon &expoly : expolygons) {
            gluTessBeginPolygon(m_tesselator, (void*)this);
            gluTessBeginContour(m_tesselator);
            size_t idx = 0;
            for (const Point &pt : expoly.contour.points) {
                coords[idx ++] = unscale<double>(pt[0]);
                coords[idx ++] = unscale<double>(pt[1]);
                coords[idx ++] = 0.;
                gluTessVertex(m_tesselator, &coords[idx - 3], &coords[idx - 3]);
            }
            gluTessEndContour(m_tesselator);
            for (const Polygon &poly : expoly.holes) {
                gluTessBeginContour(m_tesselator);
                for (const Point &pt : poly.points) {
                    coords[idx ++] = unscale<double>(pt[0]);
                    coords[idx ++] = unscale<double>(pt[1]);
                    coords[idx ++] = 0.;
                    gluTessVertex(m_tesselator, &coords[idx - 3], &coords[idx - 3]);
                }
                gluTessEndContour(m_tesselator);
            }
            gluTessEndPolygon(m_tesselator);
        }
        m_intersection_points.clear();
        return std::move(m_output_triangles);
    }

private:
    static void tessBeginCB(GLenum which, void *polygonData)        { reinterpret_cast<GluTessWrapper*>(polygonData)->tessBegin(which); }
    static void tessEndCB(void *polygonData)                        { reinterpret_cast<GluTessWrapper*>(polygonData)->tessEnd(); }
    static void tessVertexCB(const GLvoid *data, void *polygonData) { reinterpret_cast<GluTessWrapper*>(polygonData)->tessVertex(data); }
    static void tessCombineCB(const GLdouble newVertex[3], const GLdouble *neighborVertex[4], const GLfloat neighborWeight[4], GLdouble **outData, void *polygonData)
                                                                    { reinterpret_cast<GluTessWrapper*>(polygonData)->tessCombine(newVertex, neighborVertex, neighborWeight, outData); }
    static void tessErrorCB(GLenum errorCode, void *polygonData)    { reinterpret_cast<GluTessWrapper*>(polygonData)->tessError(errorCode); }

    void tessBegin(GLenum which)
    {
        assert(which == GL_TRIANGLES || which == GL_TRIANGLE_FAN || which == GL_TRIANGLE_STRIP);
        m_primitive_type = which;
        m_num_points = 0;
    }

    void tessEnd()
    {
        m_num_points = 0;
    }

    void tessVertex(const GLvoid *data)
    {
        if (data == nullptr)
            return;
        const GLdouble *ptr = (const GLdouble*)data;
        ++ m_num_points;
        if (m_num_points == 1) {
            memcpy(m_pt0, ptr, sizeof(GLdouble) * 3);
        } else if (m_num_points == 2) {
            memcpy(m_pt1, ptr, sizeof(GLdouble) * 3);
        } else {
            bool flip = m_flipped;
            if (m_primitive_type == GL_TRIANGLE_STRIP && m_num_points == 4) {
                flip = !flip;
                m_num_points = 2;
            }
            m_output_triangles.emplace_back(m_pt0[0], m_pt0[1], m_z);
            if (flip) {
                m_output_triangles.emplace_back(ptr[0],   ptr[1],   m_z);
                m_output_triangles.emplace_back(m_pt1[0], m_pt1[1], m_z);
            } else {
                m_output_triangles.emplace_back(m_pt1[0], m_pt1[1], m_z);
                m_output_triangles.emplace_back(ptr[0],   ptr[1],   m_z);
            }
            if (m_primitive_type == GL_TRIANGLE_STRIP) {
                memcpy(m_pt0, m_pt1, sizeof(GLdouble) * 3);
                memcpy(m_pt1, ptr,   sizeof(GLdouble) * 3);
            } else if (m_primitive_type == GL_TRIANGLE_FAN) {
                memcpy(m_pt1, ptr,   sizeof(GLdouble) * 3);
            } else {
                assert(m_primitive_type == GL_TRIANGLES);
                assert(m_num_points == 3);
                m_num_points = 0;
            }
        }
    }

    void tessCombine(const GLdouble newVertex[3], const GLdouble *neighborVertex[4], const GLfloat neighborWeight[4], GLdouble **outData)
    {
        m_intersection_points.emplace_back(newVertex[0], newVertex[1], m_z);
        *outData = m_intersection_points.back().data();
    }

    static void tessError(GLenum errorCode)
    {
//        const GLubyte *errorStr;
//        errorStr = gluErrorString(errorCode);
//        printf("Error: %s\n", (const char*)errorStr);
    }

    // Instance owned over the life time of this wrapper.
    GLUtesselator  *m_tesselator;

    // Currently processed primitive type.
    GLenum          m_primitive_type;
    // Two last vertices received for m_primitive_type. Used for processing triangle strips, fans etc.
    GLdouble        m_pt0[3];
    GLdouble        m_pt1[3];
    // Number of points processed over m_primitive_type.
    int             m_num_points;
    // Triangles generated by the tesselator.
    Pointf3s        m_output_triangles;
    // Intersection points generated by tessCombine callback. There should be none if the input contour is not self intersecting.
    std::deque<Vec3d> m_intersection_points;
    // Fixed third coordinate.
    double          m_z;
    // Output triangles shall be flipped (normal points down).
    bool            m_flipped;
};

std::vector<Vec3d> triangulate_expolygon_3d(const ExPolygon &poly, coordf_t z, bool flip)
{
    GluTessWrapper tess;
    return tess.tesselate3d(poly, z, flip);
}

std::vector<Vec3d> triangulate_expolygons_3d(const ExPolygons &polys, coordf_t z, bool flip)
{
	GluTessWrapper tess;
    return tess.tesselate3d(polys, z, flip);
}

std::vector<Vec2d> triangulate_expolygon_2d(const ExPolygon &poly, bool flip)
{
    GluTessWrapper tess;
    std::vector<Vec3d> triangles = tess.tesselate3d(poly, 0, flip);
    std::vector<Vec2d> out;
    out.reserve(triangles.size());
    for (const Vec3d &pt : triangles)
        out.emplace_back(pt.x(), pt.y());
    return out;
}

std::vector<Vec2d> triangulate_expolygons_2d(const ExPolygons &polys, bool flip)
{
    GluTessWrapper tess;
    std::vector<Vec3d> triangles = tess.tesselate3d(polys, 0, flip);
    std::vector<Vec2d> out;
    out.reserve(triangles.size());
    for (const Vec3d &pt : triangles)
        out.emplace_back(pt.x(), pt.y());
    return out;
}

std::vector<Vec2f> triangulate_expolygon_2f(const ExPolygon &poly, bool flip)
{
    GluTessWrapper tess;
    std::vector<Vec3d> triangles = tess.tesselate3d(poly, 0, flip);
    std::vector<Vec2f> out;
    out.reserve(triangles.size());
    for (const Vec3d &pt : triangles)
        out.emplace_back(float(pt.x()), float(pt.y()));
    return out;
}

std::vector<Vec2f> triangulate_expolygons_2f(const ExPolygons &polys, bool flip)
{
    GluTessWrapper tess;
    std::vector<Vec3d> triangles = tess.tesselate3d(polys, 0, flip);
    std::vector<Vec2f> out;
    out.reserve(triangles.size());
    for (const Vec3d &pt : triangles)
        out.emplace_back(float(pt.x()), float(pt.y()));
    return out;
}

} // namespace Slic3r
