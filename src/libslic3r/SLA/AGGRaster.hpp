#ifndef AGGRASTER_HPP
#define AGGRASTER_HPP

#include <libslic3r/SLA/RasterBase.hpp>
#include "libslic3r/ExPolygon.hpp"

// For rasterizing
#include <agg/agg_basics.h>
#include <agg/agg_rendering_buffer.h>
#include <agg/agg_pixfmt_gray.h>
#include <agg/agg_pixfmt_rgb.h>
#include <agg/agg_renderer_base.h>
#include <agg/agg_renderer_scanline.h>

#include <agg/agg_scanline_p.h>
#include <agg/agg_rasterizer_scanline_aa.h>
#include <agg/agg_path_storage.h>

namespace Slic3r {

inline const Polygon& contour(const ExPolygon& p) { return p.contour; }
inline const Polygons& holes(const ExPolygon& p) { return p.holes; }

namespace sla {

template<class Color> struct Colors {
    static const Color White;
    static const Color Black;
};

template<class Color> const Color Colors<Color>::White = Color{255};
template<class Color> const Color Colors<Color>::Black = Color{0};

template<class PixelRenderer,
         template<class /*agg::renderer_base<PixelRenderer>*/> class Renderer,
         class Rasterizer = agg::rasterizer_scanline_aa<>,
         class Scanline   = agg::scanline_p8>
class AGGRaster: public RasterBase {
public:
    using TColor = typename PixelRenderer::color_type;
    using TValue = typename TColor::value_type;
    using TPixel = typename PixelRenderer::pixel_type;
    using TRawBuffer = agg::rendering_buffer;

protected:
    
    Resolution m_resolution;
    PixelDim m_pxdim_scaled;    // used for scaled coordinate polygons
    
    std::vector<TPixel> m_buf;
    agg::rendering_buffer m_rbuf;
    
    PixelRenderer m_pixrenderer;
    
    agg::renderer_base<PixelRenderer> m_raw_renderer;
    Renderer<agg::renderer_base<PixelRenderer>> m_renderer;
    
    Trafo m_trafo;
    Scanline m_scanlines;
    Rasterizer m_rasterizer;
    
    void flipy(agg::path_storage &path) const
    {
        path.flip_y(0, double(m_resolution.height_px));
    }
    
    void flipx(agg::path_storage &path) const
    {
        path.flip_x(0, double(m_resolution.width_px));
    }
    
    double getPx(const Point &p) { return p(0) * m_pxdim_scaled.w_mm; }
    double getPy(const Point &p) { return p(1) * m_pxdim_scaled.h_mm; }
    agg::path_storage to_path(const Polygon &poly) { return to_path(poly.points); }
    
    template<class PointVec> agg::path_storage _to_path(const PointVec& v)
    {
        agg::path_storage path;
        
        auto it = v.begin();
        path.move_to(getPx(*it), getPy(*it));
        while(++it != v.end()) path.line_to(getPx(*it), getPy(*it));
        path.line_to(getPx(v.front()), getPy(v.front()));
        
        return path;
    }
    
    template<class PointVec> agg::path_storage _to_path_flpxy(const PointVec& v)
    {
        agg::path_storage path;
        
        auto it = v.begin();
        path.move_to(getPy(*it), getPx(*it));
        while(++it != v.end()) path.line_to(getPy(*it), getPx(*it));
        path.line_to(getPy(v.front()), getPx(v.front()));
        
        return path;
    }
    
    template<class PointVec> agg::path_storage to_path(const PointVec &v)
    {
        auto path = m_trafo.flipXY ? _to_path_flpxy(v) : _to_path(v);
        
        path.translate_all_paths(m_trafo.center_x * m_pxdim_scaled.w_mm,
                                 m_trafo.center_y * m_pxdim_scaled.h_mm);
        
        if(m_trafo.mirror_x) flipx(path);
        if(m_trafo.mirror_y) flipy(path);
        
        return path;
    }
    
    template<class P> void _draw(const P &poly)
    {
        m_rasterizer.reset();
        
        m_rasterizer.add_path(to_path(contour(poly)));
        for(auto& h : holes(poly)) m_rasterizer.add_path(to_path(h));
        
        agg::render_scanlines(m_rasterizer, m_scanlines, m_renderer);
    }
    
public:
    template<class GammaFn>
    AGGRaster(const Resolution &res,
              const PixelDim &  pd,
              const Trafo &     trafo,
              const TColor &    foreground,
              const TColor &    background,
              GammaFn &&        gammafn)
        : m_resolution(res)
        , m_pxdim_scaled(SCALING_FACTOR, SCALING_FACTOR)
        , m_buf(res.pixels())
        , m_rbuf(reinterpret_cast<TValue *>(m_buf.data()),
                 unsigned(res.width_px),
                 unsigned(res.height_px),
                 int(res.width_px *PixelRenderer::num_components))
        , m_pixrenderer(m_rbuf)
        , m_raw_renderer(m_pixrenderer)
        , m_renderer(m_raw_renderer)
        , m_trafo(trafo)
    {
        // Visual Studio compiler gives warnings about possible division by zero.
        assert(pd.w_mm != 0 && pd.h_mm != 0);
        if (pd.w_mm != 0 && pd.h_mm != 0) {
            m_pxdim_scaled.w_mm /= pd.w_mm;
            m_pxdim_scaled.h_mm /= pd.h_mm;
        }
        m_renderer.color(foreground);
        clear(background);
        
        m_rasterizer.gamma(gammafn);
    }
    
    Trafo trafo() const override { return m_trafo; }
    Resolution resolution() const { return m_resolution; }
    PixelDim   pixel_dimensions() const
    {
        return {SCALING_FACTOR / m_pxdim_scaled.w_mm,
                SCALING_FACTOR / m_pxdim_scaled.h_mm};
    }
    
    void draw(const ExPolygon &poly) override { _draw(poly); }
    
    EncodedRaster encode(RasterEncoder encoder) const override
    {
        return encoder(m_buf.data(), m_resolution.width_px, m_resolution.height_px, 1);    
    }
    
    void clear(const TColor color) { m_raw_renderer.clear(color); }
};

/*
 * Captures an anti-aliased monochrome canvas where vectorial
 * polygons can be rasterized. Fill color is always white and the background is
 * black. Contours are anti-aliased.
 * 
 * A gamma function can be specified at compile time to make it more flexible.
 */
using _RasterGrayscaleAA =
    AGGRaster<agg::pixfmt_gray8, agg::renderer_scanline_aa_solid>;

class RasterGrayscaleAA : public _RasterGrayscaleAA {
    using Base = _RasterGrayscaleAA;
    using typename Base::TColor;
    using typename Base::TValue;
public:
    template<class GammaFn>
    RasterGrayscaleAA(const Resolution        &res,
                      const PixelDim          &pd,
                      const RasterBase::Trafo &trafo,
                      GammaFn                &&fn)
        : Base(res,
               pd,
               trafo,
               Colors<TColor>::White,
               Colors<TColor>::Black,
               std::forward<GammaFn>(fn))
    {}
    
    uint8_t read_pixel(size_t col, size_t row) const
    {
        static_assert(std::is_same<TValue, uint8_t>::value, "Not grayscale pix");
        
        uint8_t px;
        Base::m_buf[row * Base::resolution().width_px + col].get(px);
        return px;
    }
    
    void clear() { Base::clear(Colors<TColor>::Black); }
};

class RasterGrayscaleAAGammaPower: public RasterGrayscaleAA {
public:
    RasterGrayscaleAAGammaPower(const Resolution        &res,
                                const PixelDim          &pd,
                                const RasterBase::Trafo &trafo,
                                double                   gamma = 1.)
        : RasterGrayscaleAA(res, pd, trafo, agg::gamma_power(gamma))
    {}
};

}} // namespace Slic3r::sla

#endif // AGGRASTER_HPP
