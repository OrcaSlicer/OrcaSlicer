// ThumbnailRenderer.cpp
//
// Headless GL thumbnail renderer — Tier-2 preview for SliceCore.
//
// Reference implementation mirrored from src/OrcaSlicer.cpp lines 6649–6835
// (the --export-3mf CLI thumbnail path).  All GL/GUI work is guarded by
// #ifdef SLIC3R_GUI so that non-GUI builds compile to a pure no-op stub.
//
// Thread safety: a function-local static std::mutex serialises all GL
// operations.  GLFW / GLAD are process-global singletons and must not be
// driven concurrently from multiple threads.

#include "ThumbnailRenderer.hpp"

// Always include the type definitions so the non-GUI stub compiles too.
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"

#ifdef SLIC3R_GUI

#include "libslic3r/miniz_extension.hpp"   // brings in <miniz.h> -> tdefl_write_image_to_png_file_in_memory_ex
#include "libslic3r/GCode/ThumbnailData.hpp"

#include "slic3r/GUI/3DScene.hpp"          // GLVolumeCollection
#include "slic3r/GUI/GLShader.hpp"         // GLShaderProgram (defined in namespace Slic3r)
#include "slic3r/GUI/OpenGLManager.hpp"    // OpenGLManager, EFramebufferType
#include "slic3r/GUI/GLCanvas3D.hpp"       // GLCanvas3D::render_thumbnail_framebuffer[_ext]
#include "slic3r/GUI/Camera.hpp"           // Camera::EType, Camera::ViewAngleType
#include "slic3r/GUI/BitmapCache.hpp"      // BitmapCache::parse_color4
#include "slic3r/GUI/PartPlate.hpp"        // PartPlateList (needed by render_thumbnail_framebuffer signature)

#include "libslic3r/Color.hpp"             // ColorRGBA

#include <GLFW/glfw3.h>

#include <boost/log/trivial.hpp>

#include <miniz.h>   // tdefl_write_image_to_png_file_in_memory_ex, mz_free, MZ_DEFAULT_LEVEL
#include <mutex>
#include <cstring>

namespace Slic3r {
namespace SliceCore {

namespace {

// Process-wide GL serialisation mutex.
// GLFW context, GLAD loader state, and GLShadersManager are all global;
// serialising here matches what the CLI does (single-threaded).
static std::mutex s_gl_mutex;

// Convert an RGBA ThumbnailData to a raw PNG byte buffer using miniz's
// tdefl_write_image_to_png_file_in_memory_ex — the same function used by
// compress_thumbnail_png() in GCode/Thumbnails.cpp:49 and bbs_3mf.cpp:6548.
// Returns true on success; the returned buffer is heap-allocated by miniz
// (via mz_free-compatible allocator) and is copied into out before freeing.
bool thumbnail_data_to_png(const ThumbnailData &data,
                            std::vector<unsigned char> &out,
                            std::string &err)
{
    if (!data.is_valid() || data.pixels.empty()) {
        err = "thumbnail render produced an empty pixel buffer";
        return false;
    }

    size_t png_size = 0;
    // 4 channels = RGBA; flip=1 flips rows (GL reads bottom-up, PNG is top-down).
    void *png_data = tdefl_write_image_to_png_file_in_memory_ex(
        static_cast<const void *>(data.pixels.data()),
        static_cast<int>(data.width),
        static_cast<int>(data.height),
        4,             // num_chans: RGBA
        &png_size,
        MZ_DEFAULT_LEVEL,
        1              // flip: vertical flip so top row is row 0 in PNG
    );

    if (png_data == nullptr || png_size == 0) {
        err = "tdefl_write_image_to_png_file_in_memory_ex failed (null output)";
        if (png_data) mz_free(png_data);
        return false;
    }

    out.assign(static_cast<const unsigned char *>(png_data),
               static_cast<const unsigned char *>(png_data) + png_size);
    mz_free(png_data);
    return true;
}

} // anonymous namespace

bool render_model_thumbnail(const Model              &model,
                            const DynamicPrintConfig &cfg,
                            int                       width,
                            int                       height,
                            std::vector<unsigned char> &png_bytes,
                            std::string               &err)
{
    // Clamp to sane bounds to avoid absurd framebuffer allocations.
    if (width  <= 0) width  = 512;
    if (height <= 0) height = 512;
    if (width  > 4096) width  = 4096;
    if (height > 4096) height = 4096;

    // Serialise all GL work.
    std::lock_guard<std::mutex> lock(s_gl_mutex);

    // ------------------------------------------------------------------
    // 1) Resolve filament colours from the effective config.
    //    Mirrors OrcaSlicer.cpp:6650-6663.
    // ------------------------------------------------------------------
    const ConfigOptionStrings *filament_color =
        cfg.option<ConfigOptionStrings>("filament_colour");

    std::vector<std::string> color_strings;
    if (filament_color && !filament_color->values.empty()) {
        color_strings = filament_color->values;
    } else {
        color_strings.push_back("#FFFFFFFF");
    }

    std::vector<ColorRGBA> colors_out(color_strings.size());
    for (size_t i = 0; i < color_strings.size(); ++i) {
        unsigned char rgba[4] = {};
        Slic3r::GUI::BitmapCache::parse_color4(color_strings[i], rgba);
        colors_out[i] = ColorRGBA(
            float(rgba[0]) / 255.f,
            float(rgba[1]) / 255.f,
            float(rgba[2]) / 255.f,
            float(rgba[3]) / 255.f
        );
    }

    // ------------------------------------------------------------------
    // 2) GLFW setup — create a hidden offscreen context.
    //    Mirrors OrcaSlicer.cpp:6665-6703.
    // ------------------------------------------------------------------
    int glfw_major = 0, glfw_minor = 0, glfw_rev = 0;
    glfwGetVersion(&glfw_major, &glfw_minor, &glfw_rev);
    BOOST_LOG_TRIVIAL(info)
        << "[ThumbnailRenderer] GLFW version "
        << glfw_major << "." << glfw_minor << "." << glfw_rev;

    // Set error callback before glfwInit so startup errors are logged.
    glfwSetErrorCallback([](int code, const char *msg) {
        BOOST_LOG_TRIVIAL(warning)
            << "[ThumbnailRenderer] GLFW error " << code << ": " << msg;
    });

    if (glfwInit() == GLFW_FALSE) {
        const char *glfw_err_msg = nullptr;
        int         glfw_err_code = glfwGetError(&glfw_err_msg);
        err = std::string("glfwInit failed, code=") +
              std::to_string(glfw_err_code) +
              (glfw_err_msg ? std::string(": ") + glfw_err_msg : "");
        BOOST_LOG_TRIVIAL(warning) << "[ThumbnailRenderer] " << err;
        return false;
    }

    // Request an OpenGL 3.3 context — the minimum that supports the 'thumbnail'
    // shader and framebuffer extensions.  We do NOT pass the GLFW *library*
    // version here: glfwGetVersion() returns e.g. 3.4, which is not a valid
    // OpenGL version and causes glfwCreateWindow to fail on all drivers.
    // GLFW_VISIBLE=false → offscreen/invisible window (no desktop window pops up).
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_RED_BITS,   8);
    glfwWindowHint(GLFW_GREEN_BITS, 8);
    glfwWindowHint(GLFW_BLUE_BITS,  8);
    glfwWindowHint(GLFW_ALPHA_BITS, 8);
    glfwWindowHint(GLFW_VISIBLE,    GLFW_FALSE);

#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_PROFILE,        GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);
#endif

    GLFWwindow *window = glfwCreateWindow(
        static_cast<int>(width), static_cast<int>(height),
        "thumbnail_offscreen", nullptr, nullptr);
    if (window == nullptr) {
        err = "glfwCreateWindow failed — GL context unavailable for headless rendering";
        BOOST_LOG_TRIVIAL(warning) << "[ThumbnailRenderer] " << err;
        glfwTerminate();
        return false;
    }

    glfwMakeContextCurrent(window);

    // ------------------------------------------------------------------
    // 3) Init GLAD (loader lives inside OpenGLManager).
    //    Mirrors OrcaSlicer.cpp:6715-6721.
    //    init_gl(false) = don't show error popup dialogs (headless).
    // ------------------------------------------------------------------
    Slic3r::GUI::OpenGLManager opengl_mgr;
    if (!opengl_mgr.init_gl(/*popup_error=*/false)) {
        err = "OpenGLManager::init_gl() failed — GLAD loader or shader compilation error";
        BOOST_LOG_TRIVIAL(warning) << "[ThumbnailRenderer] " << err;
        glfwDestroyWindow(window);
        glfwTerminate();
        return false;
    }
    BOOST_LOG_TRIVIAL(info) << "[ThumbnailRenderer] GLAD loaded successfully";

    // ------------------------------------------------------------------
    // 4) Build GLVolumeCollection from model objects/instances.
    //    Mirrors OrcaSlicer.cpp:6722-6764.
    //    load_object_volume(obj, obj_idx, vol_idx, inst_idx, color_by,
    //                       opengl_initialized, in_assemble_view=false,
    //                       use_loaded_id=false, need_raycaster=true)
    //    Confirmed in 3DScene.hpp:464.
    // ------------------------------------------------------------------
    GLVolumeCollection glvolume_collection;

    int obj_extruder_id    = 1;
    int volume_extruder_id = 1;

    for (unsigned int obj_idx = 0;
         obj_idx < static_cast<unsigned int>(model.objects.size());
         ++obj_idx)
    {
        const ModelObject &model_object = *model.objects[obj_idx];

        // Per-object extruder override.
        if (const ConfigOption *opt = model_object.config.option("extruder"))
            obj_extruder_id = dynamic_cast<const ConfigOptionInt *>(opt)->getInt();
        else
            obj_extruder_id = 1;

        for (int volume_idx = 0;
             volume_idx < static_cast<int>(model_object.volumes.size());
             ++volume_idx)
        {
            const ModelVolume &model_volume = *model_object.volumes[volume_idx];

            // Per-volume extruder override.
            if (const ConfigOption *opt = model_volume.config.option("extruder"))
                volume_extruder_id = dynamic_cast<const ConfigOptionInt *>(opt)->getInt();
            else
                volume_extruder_id = obj_extruder_id;

            for (int instance_idx = 0;
                 instance_idx < static_cast<int>(model_object.instances.size());
                 ++instance_idx)
            {
                const ModelInstance &model_instance = *model_object.instances[instance_idx];

                // opengl_initialized=true because we called init_gl() above.
                glvolume_collection.load_object_volume(
                    &model_object,
                    static_cast<int>(obj_idx),
                    volume_idx,
                    instance_idx,
                    "volume",
                    /*opengl_initialized=*/true,
                    /*in_assemble_view=*/false,
                    /*use_loaded_id=*/false,
                    /*need_raycaster=*/false  // no raycaster needed for thumbnail
                );

                // Determine the colour for this volume from the filament colour list.
                // Index is (extruder_id - 1) clamped to available colours.
                const int color_idx =
                    (volume_extruder_id >= 1 &&
                     static_cast<size_t>(volume_extruder_id - 1) < color_strings.size())
                    ? (volume_extruder_id - 1)
                    : 0;

                unsigned char rgba[4] = {};
                Slic3r::GUI::BitmapCache::parse_color4(color_strings[color_idx], rgba);
                ColorRGBA render_color(
                    float(rgba[0]) / 255.f,
                    float(rgba[1]) / 255.f,
                    float(rgba[2]) / 255.f,
                    float(rgba[3]) / 255.f
                );

                if (!glvolume_collection.volumes.empty()) {
                    glvolume_collection.volumes.back()->set_render_color(render_color);
                    glvolume_collection.volumes.back()->set_color(render_color);
                    glvolume_collection.volumes.back()->printable = model_instance.printable;
                }
            }
        }
    }

    // ------------------------------------------------------------------
    // 5) Obtain the "thumbnail" shader.
    //    Mirrors OrcaSlicer.cpp:6768-6771.
    // ------------------------------------------------------------------
    GLShaderProgram *shader = opengl_mgr.get_shader("thumbnail");
    if (shader == nullptr) {
        err = "failed to obtain 'thumbnail' shader from OpenGLManager";
        BOOST_LOG_TRIVIAL(warning) << "[ThumbnailRenderer] " << err;
        glfwDestroyWindow(window);
        glfwTerminate();
        return false;
    }

    // ------------------------------------------------------------------
    // 6) Build PartPlateList (headless — nullptr plater, same as SliceService).
    //    We need it only to satisfy render_thumbnail_framebuffer's signature;
    //    plate_id=0 renders all objects (same as the first thumbnail in the
    //    reference block, OrcaSlicer.cpp:6811).
    // ------------------------------------------------------------------
    // Determine printer technology from cfg (defaulting to ptFFF).
    PrinterTechnology printer_tech = ptFFF;
    if (const auto *opt =
            cfg.option<ConfigOptionEnum<PrinterTechnology>>("printer_technology"))
        printer_tech = opt->value;

    // We need a non-const model for PartPlateList; take a local copy.
    // The copy is cheap relative to GL setup — the mesh data is shared via
    // TriangleMesh's reference-counted internal storage.
    Model model_copy = model;
    Slic3r::GUI::PartPlateList partplate_list(
        /*plater=*/nullptr, &model_copy, printer_tech);

    // ------------------------------------------------------------------
    // 7) Set up ThumbnailsParams and ThumbnailData, then render.
    //    plate_id=0 → first/only plate (all objects); transparent_background=true.
    //    Mirrors OrcaSlicer.cpp:6811 & 6814-6834.
    // ------------------------------------------------------------------
    const ThumbnailsParams thumbnail_params = {
        /*sizes=*/{},
        /*printable_only=*/false,
        /*parts_only=*/true,
        /*show_bed=*/true,
        /*transparent_background=*/true,
        /*plate_id=*/0
    };

    ThumbnailData thumbnail_data;

    using EFBType = Slic3r::GUI::OpenGLManager::EFramebufferType;
    const EFBType fb_type = Slic3r::GUI::OpenGLManager::get_framebuffers_type();

    bool rendered = false;
    if (fb_type == EFBType::Arb) {
        BOOST_LOG_TRIVIAL(info) << "[ThumbnailRenderer] framebuffer type: ARB";
        Slic3r::GUI::GLCanvas3D::render_thumbnail_framebuffer(
            thumbnail_data,
            static_cast<unsigned int>(width),
            static_cast<unsigned int>(height),
            thumbnail_params,
            partplate_list,
            model_copy.objects,
            glvolume_collection,
            colors_out,
            shader,
            Slic3r::GUI::Camera::EType::Ortho);
        rendered = true;
    } else if (fb_type == EFBType::Ext) {
        BOOST_LOG_TRIVIAL(info) << "[ThumbnailRenderer] framebuffer type: EXT";
        Slic3r::GUI::GLCanvas3D::render_thumbnail_framebuffer_ext(
            thumbnail_data,
            static_cast<unsigned int>(width),
            static_cast<unsigned int>(height),
            thumbnail_params,
            partplate_list,
            model_copy.objects,
            glvolume_collection,
            colors_out,
            shader,
            Slic3r::GUI::Camera::EType::Ortho);
        rendered = true;
    } else {
        // EFBType::Unknown — framebuffers not supported on this driver.
        err = "no supported framebuffer extension (ARB/EXT) detected; "
              "thumbnail rendering unavailable on this GL driver";
        BOOST_LOG_TRIVIAL(warning) << "[ThumbnailRenderer] " << err;
    }

    // ------------------------------------------------------------------
    // 8) Cleanup GLFW resources.
    //    Destroy the window and terminate GLFW.
    //    NOTE: we always terminate here because this is the only GLFW context
    //    in the process (headless server); the reference block does the same
    //    implicit cleanup at process exit.  We call glfwDestroyWindow before
    //    glfwTerminate to be correct.
    // ------------------------------------------------------------------
    glfwDestroyWindow(window);
    glfwTerminate();

    if (!rendered) {
        // err already set above.
        return false;
    }

    // ------------------------------------------------------------------
    // 9) Validate rendered pixels and encode to PNG.
    // ------------------------------------------------------------------
    if (!thumbnail_data.is_valid()) {
        err = "render_thumbnail_framebuffer produced an invalid (empty) ThumbnailData";
        BOOST_LOG_TRIVIAL(warning) << "[ThumbnailRenderer] " << err;
        return false;
    }

    BOOST_LOG_TRIVIAL(info)
        << "[ThumbnailRenderer] rendered " << thumbnail_data.width
        << "x" << thumbnail_data.height << " thumbnail; encoding to PNG";

    std::string png_err;
    if (!thumbnail_data_to_png(thumbnail_data, png_bytes, png_err)) {
        err = "PNG encoding failed: " + png_err;
        BOOST_LOG_TRIVIAL(warning) << "[ThumbnailRenderer] " << err;
        return false;
    }

    BOOST_LOG_TRIVIAL(info)
        << "[ThumbnailRenderer] PNG encoded successfully ("
        << png_bytes.size() << " bytes)";
    return true;
}

} // namespace SliceCore
} // namespace Slic3r

#else // !SLIC3R_GUI

namespace Slic3r {
namespace SliceCore {

bool render_model_thumbnail(const Model              & /*model*/,
                            const DynamicPrintConfig & /*cfg*/,
                            int                        /*width*/,
                            int                        /*height*/,
                            std::vector<unsigned char> & /*png_bytes*/,
                            std::string               &err)
{
    err = "thumbnail rendering requires a SLIC3R_GUI build "
          "(OpenGL/GLAD/GLFW stack not available in this configuration)";
    return false;
}

} // namespace SliceCore
} // namespace Slic3r

#endif // SLIC3R_GUI
