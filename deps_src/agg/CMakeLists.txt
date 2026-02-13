cmake_minimum_required(VERSION 3.13)
project(agg)

add_library(agg INTERFACE)

target_include_directories(agg SYSTEM
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_sources(agg INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/agg_array.h
    ${CMAKE_CURRENT_SOURCE_DIR}/agg_basics.h
    ${CMAKE_CURRENT_SOURCE_DIR}/agg_bezier_arc.h
    ${CMAKE_CURRENT_SOURCE_DIR}/agg_clip_liang_barsky.h
    ${CMAKE_CURRENT_SOURCE_DIR}/agg_color_gray.h
    ${CMAKE_CURRENT_SOURCE_DIR}/agg_color_rgba.h
    ${CMAKE_CURRENT_SOURCE_DIR}/agg_config.h
    ${CMAKE_CURRENT_SOURCE_DIR}/agg_conv_transform.h
    ${CMAKE_CURRENT_SOURCE_DIR}/agg_gamma_functions.h
    ${CMAKE_CURRENT_SOURCE_DIR}/agg_gamma_lut.h
    ${CMAKE_CURRENT_SOURCE_DIR}/agg_math.h
    ${CMAKE_CURRENT_SOURCE_DIR}/agg_path_storage.h
    ${CMAKE_CURRENT_SOURCE_DIR}/agg_pixfmt_base.h
    ${CMAKE_CURRENT_SOURCE_DIR}/agg_pixfmt_gray.h
    ${CMAKE_CURRENT_SOURCE_DIR}/agg_pixfmt_rgb.h
    ${CMAKE_CURRENT_SOURCE_DIR}/agg_rasterizer_cells_aa.h
    ${CMAKE_CURRENT_SOURCE_DIR}/agg_rasterizer_scanline_aa.h
    ${CMAKE_CURRENT_SOURCE_DIR}/agg_rasterizer_scanline_aa_nogamma.h
    ${CMAKE_CURRENT_SOURCE_DIR}/agg_rasterizer_sl_clip.h
    ${CMAKE_CURRENT_SOURCE_DIR}/agg_renderer_base.h
    ${CMAKE_CURRENT_SOURCE_DIR}/agg_renderer_scanline.h
    ${CMAKE_CURRENT_SOURCE_DIR}/agg_rendering_buffer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/agg_scanline_p.h
    ${CMAKE_CURRENT_SOURCE_DIR}/agg_trans_affine.h
)
