# In PrusaSlicer 2.6.0 we switched from https://github.com/memononen/nanosvg to its fork https://github.com/fltk/nanosvg
# because this last implements the new function nsvgRasterizeXY() which we now use in GLTexture::load_from_svg()
# for rasterizing svg files from their original size to a squared power of two texture on Windows systems using
# AMD Radeon graphics cards

orcaslicer_add_cmake_project(NanoSVG
    DEPENDS dep_Boost
    URL https://github.com/SoftFever/nanosvg/archive/863f6aa97ef62028126fa2c19bd4350394c2e15e.zip
        URL_HASH SHA256=8d9c1624ad6518dd6dfa31e4f8dc7da9ec243d88bae595c7a037450617fec851
)