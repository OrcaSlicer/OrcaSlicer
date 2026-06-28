//-----------------------------------------------------------------------------
// Discovery and loading of our resources (icons, fonts, templates, etc).
//
// Copyright 2016 whitequark
//-----------------------------------------------------------------------------

#ifndef __RESOURCE_H
#define __RESOURCE_H

class Camera;
class Point2d;
class Pixmap;
class Vector;

std::string LoadString(const std::string &name);
std::string LoadStringFromGzip(const std::string &name);
std::shared_ptr<Pixmap> LoadPng(const std::string &name);

class Pixmap {
public:
    enum class Format { BGRA, RGBA, BGR, RGB, A };

    Format                     format;
    size_t                     width;
    size_t                     height;
    size_t                     stride;
    std::vector<uint8_t>       data;

    static std::shared_ptr<Pixmap> Create(Format format, size_t width, size_t height);
    static std::shared_ptr<Pixmap> FromPng(const uint8_t *data, size_t size, bool flip = false);

    static std::shared_ptr<Pixmap> ReadPng(FILE *f, bool flip = false);
    static std::shared_ptr<Pixmap> ReadPng(const Platform::Path &filename, bool flip = false);
    bool WritePng(FILE *f, bool flip = false);
    bool WritePng(const Platform::Path &filename, bool flip = false);

    size_t GetBytesPerPixel() const;
    RgbaColor GetPixel(size_t x, size_t y) const;
    bool Equals(const Pixmap &other) const;

    void ConvertTo(Format newFormat);
    void SetPixel(size_t x, size_t y, RgbaColor color);
};

class BitmapFont {
public:
    struct Glyph {
        uint8_t  advanceCells;
        uint16_t position;
    };

    std::string                unifontData;
    std::map<char32_t, Glyph>  glyphs;
    std::shared_ptr<Pixmap>    texture;
    bool                       textureUpdated;
    uint16_t                   nextPosition;

    static BitmapFont From(std::string &&unifontData);
    static BitmapFont Create();

    bool IsEmpty() const { return unifontData.empty(); }
    const Glyph &GetGlyph(char32_t codepoint);
    void LocateGlyph(char32_t codepoint, double *s0, double *t0, double *s1, double *t1,
                     size_t *advanceWidth, size_t *boundingHeight);

    void AddGlyph(char32_t codepoint, std::shared_ptr<const Pixmap> pixmap);

    size_t GetWidth(char32_t codepoint);
    size_t GetWidth(const std::string &str);
};

class VectorFont {
public:
    struct Contour {
        std::vector<Point2d>   points;
    };

    struct Glyph {
        std::vector<Contour>   contours;
        double                 leftSideBearing;
        double                 boundingWidth;
        double                 advanceWidth;
    };

    std::string                lffData;
    std::map<char32_t, Glyph>  glyphs;
    double                     rightSideBearing;
    double                     capHeight;
    double                     ascender;
    double                     descender;

    static VectorFont From(std::string &&lffData);
    static VectorFont *Builtin();

    bool IsEmpty() const { return lffData.empty(); }
    const Glyph &GetGlyph(char32_t codepoint);

    double GetCapHeight(double forCapHeight) const;
    double GetHeight(double forCapHeight) const;
    double GetWidth(double forCapHeight, const std::string &str);
    Vector GetExtents(double forCapHeight, const std::string &str);

    void Trace(double forCapHeight, Vector o, Vector u, Vector v, const std::string &str,
               std::function<void(Vector, Vector)> traceEdge);
    void Trace(double forCapHeight, Vector o, Vector u, Vector v, const std::string &str,
               std::function<void(Vector, Vector)> traceEdge, const Camera &camera);
};

#endif
