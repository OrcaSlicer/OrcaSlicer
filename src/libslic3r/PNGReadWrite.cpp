#include "PNGReadWrite.hpp"

#include <memory>

#include <cstdio>
#include <png.h>

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/cstdio.hpp>

namespace Slic3r { namespace png {

struct PNGDescr {
    png_struct *png = nullptr; png_info *info = nullptr;

    PNGDescr() = default;
    PNGDescr(const PNGDescr&) = delete;
    PNGDescr(PNGDescr&&) = delete;
    PNGDescr& operator=(const PNGDescr&) = delete;
    PNGDescr& operator=(PNGDescr&&) = delete;

    ~PNGDescr()
    {
        if (png && info) png_destroy_info_struct(png, &info);
        if (png) png_destroy_read_struct( &png, nullptr, nullptr);
    }
};

bool is_png(const ReadBuf &rb)
{
    static const constexpr int PNG_SIG_BYTES = 8;

#if PNG_LIBPNG_VER_MINOR <= 2
    // Earlier libpng versions had png_sig_cmp(png_bytep, ...) which is not
    // a const pointer. It is not possible to cast away the const qualifier from
    // the input buffer so... yes... life is challenging...
    png_byte buf[PNG_SIG_BYTES];
    auto inbuf = static_cast<const std::uint8_t *>(rb.buf);
    std::copy(inbuf, inbuf + PNG_SIG_BYTES, buf);
#else
    auto buf = static_cast<png_const_bytep>(rb.buf);
#endif

    return rb.sz >= PNG_SIG_BYTES && !png_sig_cmp(buf, 0, PNG_SIG_BYTES);
}

// Buffer read callback for libpng. It provides an allocated output buffer and
// the amount of data it desires to read from the input.
static void png_read_callback(png_struct *png_ptr,
                              png_bytep   outBytes,
                              png_size_t  byteCountToRead)
{
    // Retrieve our input buffer through the png_ptr
    auto reader = static_cast<IStream *>(png_get_io_ptr(png_ptr));

    // libpng expects a short read to be reported through png_error(); returning quietly would leave
    // it decoding whatever happened to be in outBytes.
    if (!reader || !reader->is_ok() ||
        reader->read(static_cast<std::uint8_t *>(outBytes), byteCountToRead) != byteCountToRead)
        png_error(png_ptr, "PNG data is truncated");
}

// libpng reports a corrupt or truncated image by longjmp()ing back to the jump buffer set with
// setjmp(). The frame it lands in must own nothing that needs destroying: with exceptions enabled
// MSVC unwinds the stack as part of longjmp, and returning from a frame unwound that way crashes -
// which is what a truncated texture did on Windows while working everywhere else. So the calls that
// can fail live in these two helpers, which hold nothing but pointers, and every C++ object the
// decoders need stays in their own frames.
static bool png_read_header_guarded(png_struct *png, png_info *info, IStream *in_buf, int sig_bytes)
{
    if (setjmp(png_jmpbuf(png)))
        return false;

    png_set_read_fn(png, static_cast<void *>(in_buf), png_read_callback);
    // Tell that we have already read the first bytes to check the signature
    png_set_sig_bytes(png, sig_bytes);
    png_read_info(png, info);
    return true;
}

// `bottom_up` fills the buffer last row first, which is the order the colour decoder hands back.
static bool png_read_rows_guarded(png_struct *png, png_info *info, png_bytep dst, size_t rows, size_t rowbytes,
                                  bool bottom_up, bool read_end)
{
    if (setjmp(png_jmpbuf(png)))
        return false;

    for (size_t i = 0; i < rows; ++i)
        png_read_row(png, dst + (bottom_up ? rows - 1 - i : i) * rowbytes, nullptr);
    if (read_end)
        png_read_end(png, info);
    return true;
}

bool decode_png(IStream &in_buf, ImageGreyscale &out_img)
{
    static const constexpr int PNG_SIG_BYTES = 8;

    std::vector<png_byte> sig(PNG_SIG_BYTES, 0);
    in_buf.read(sig.data(), PNG_SIG_BYTES);
    if (!png_check_sig(sig.data(), PNG_SIG_BYTES))
        return false;

    PNGDescr dsc;
    dsc.png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr,
                                     nullptr);

    if(!dsc.png) return false;

    dsc.info = png_create_info_struct(dsc.png);
    if(!dsc.info) return false;

    if (!png_read_header_guarded(dsc.png, dsc.info, &in_buf, PNG_SIG_BYTES))
        return false;

    out_img.cols = png_get_image_width(dsc.png, dsc.info);
    out_img.rows = png_get_image_height(dsc.png, dsc.info);
    size_t color_type = png_get_color_type(dsc.png, dsc.info);
    size_t bit_depth  = png_get_bit_depth(dsc.png, dsc.info);

    if (color_type != PNG_COLOR_TYPE_GRAY || bit_depth != 8)
        return false;

    out_img.buf.resize(out_img.rows * out_img.cols);

    return png_read_rows_guarded(dsc.png, dsc.info, static_cast<png_bytep>(out_img.buf.data()), out_img.rows,
                                 out_img.cols, /* bottom_up */ false, /* read_end */ false);
}

bool decode_colored_png(IStream &in_buf, ImageColorscale &out_img)
{
    static const constexpr int PNG_SIG_BYTES = 8;

    std::vector<png_byte> sig(PNG_SIG_BYTES, 0);
    in_buf.read(sig.data(), PNG_SIG_BYTES);
    if (!png_check_sig(sig.data(), PNG_SIG_BYTES)) {
        BOOST_LOG_TRIVIAL(error) << boost::format("decode_colored_png: png_check_sig failed");
        return false;
    }

    PNGDescr dsc;
    dsc.png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr,
                                     nullptr);

    if(!dsc.png) {
        BOOST_LOG_TRIVIAL(error) << boost::format("decode_colored_png: png_create_read_struct failed");
        return false;
    }

    dsc.info = png_create_info_struct(dsc.png);
    if(!dsc.info) {
        BOOST_LOG_TRIVIAL(error) << boost::format("decode_colored_png: png_create_info_struct failed");
        png_destroy_read_struct(&dsc.png, &dsc.info, NULL);
        return false;
    }

    if (!png_read_header_guarded(dsc.png, dsc.info, &in_buf, PNG_SIG_BYTES)) {
        BOOST_LOG_TRIVIAL(error) << "decode_colored_png: corrupt or truncated PNG data";
        return false;
    }

    out_img.cols = png_get_image_width(dsc.png, dsc.info);
    out_img.rows = png_get_image_height(dsc.png, dsc.info);
    size_t color_type = png_get_color_type(dsc.png, dsc.info);
    size_t bit_depth  = png_get_bit_depth(dsc.png, dsc.info);
    unsigned long rowbytes = png_get_rowbytes(dsc.png, dsc.info);

    switch(color_type)
    {
        case PNG_COLOR_TYPE_RGB:
            out_img.bytes_per_pixel = 3;
            break;
        case PNG_COLOR_TYPE_RGB_ALPHA:
            out_img.bytes_per_pixel = 4;
            break;
        default: //not supported currently
            png_destroy_read_struct(&dsc.png, &dsc.info, NULL);
            return false;
    }

    BOOST_LOG_TRIVIAL(info) << boost::format("png's cols %1%, rows %2%, color_type %3%, bit_depth %4%, bytes_per_pixel %5%, rowbytes %6%")%out_img.cols %out_img.rows %color_type %bit_depth %out_img.bytes_per_pixel %rowbytes;
    out_img.buf.resize(out_img.rows * rowbytes);

    int filter_type = png_get_filter_type(dsc.png, dsc.info);
    int compression_type = png_get_compression_type(dsc.png, dsc.info);
    int interlace_type = png_get_interlace_type(dsc.png, dsc.info);
    BOOST_LOG_TRIVIAL(info) << boost::format("filter_type %1%, compression_type %2%, interlace_type %3%, rowbytes %4%")%filter_type %compression_type %interlace_type %rowbytes;

    if (!png_read_rows_guarded(dsc.png, dsc.info, static_cast<png_bytep>(out_img.buf.data()), out_img.rows, rowbytes,
                               /* bottom_up */ true, /* read_end */ true)) {
        BOOST_LOG_TRIVIAL(error) << "decode_colored_png: corrupt or truncated PNG data";
        return false;
    }

    png_destroy_read_struct(&dsc.png, &dsc.info, NULL);

    return true;
}

bool decode_colored_png(const ReadBuf &in_buf, ImageColorscale &out_img)
{
    struct ReadBufStream stream{in_buf};

    return decode_colored_png(stream, out_img);
}


// Down to earth function to store a packed RGB image to file. Mostly useful for debugging purposes.
// Based on https://www.lemoda.net/c/write-png/
// png_color_type is PNG_COLOR_TYPE_RGB or PNG_COLOR_TYPE_GRAY
//FIXME maybe better to use tdefl_write_image_to_png_file_in_memory() instead?
static bool write_rgb_or_gray_to_file(const char *file_name_utf8, size_t width, size_t height, int png_color_type, const uint8_t *data)
{
    bool         result       = false;

    // Forward declaration due to the gotos.
    png_structp  png_ptr      = nullptr;
    png_infop    info_ptr     = nullptr;
    png_byte   **row_pointers = nullptr;

    FILE        *fp = boost::nowide::fopen(file_name_utf8, "wb");
    if (! fp) {
        BOOST_LOG_TRIVIAL(error) << "write_png_file: File could not be opened for writing: " << file_name_utf8;
        goto fopen_failed;
    }

    png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (! png_ptr) {
        BOOST_LOG_TRIVIAL(error) << "write_png_file: png_create_write_struct() failed";
        goto png_create_write_struct_failed;
    }

    info_ptr = png_create_info_struct(png_ptr);
    if (! info_ptr) {
        BOOST_LOG_TRIVIAL(error) << "write_png_file: png_create_info_struct() failed";
        goto png_create_info_struct_failed;
    }

    // Set up error handling.
    if (setjmp(png_jmpbuf(png_ptr))) {
        BOOST_LOG_TRIVIAL(error) << "write_png_file: setjmp() failed";
        goto png_failure;
    }

    // Set image attributes.
    png_set_IHDR(png_ptr,
        info_ptr,
        png_uint_32(width),
        png_uint_32(height),
        8, // depth
        png_color_type,
        PNG_INTERLACE_NONE,
        PNG_COMPRESSION_TYPE_DEFAULT,
        PNG_FILTER_TYPE_DEFAULT);

    // Initialize rows of PNG.
    row_pointers = reinterpret_cast<png_byte**>(::png_malloc(png_ptr, height * sizeof(png_byte*)));
    {
        int line_width = width;
        if (png_color_type == PNG_COLOR_TYPE_RGB)
            line_width *= 3;
        for (size_t y = 0; y < height; ++ y) {
            auto row = reinterpret_cast<png_byte*>(::png_malloc(png_ptr, line_width));
            row_pointers[y] = row;
            memcpy(row, data + line_width * y, line_width);
        }
    }

    // Write the image data to "fp".
    png_init_io(png_ptr, fp);
    png_set_rows(png_ptr, info_ptr, row_pointers);
    png_write_png(png_ptr, info_ptr, PNG_TRANSFORM_IDENTITY, nullptr);

    for (size_t y = 0; y < height; ++ y)
        png_free(png_ptr, row_pointers[y]);
    png_free(png_ptr, row_pointers);

    result = true;

png_failure:
png_create_info_struct_failed:
    ::png_destroy_write_struct(&png_ptr, &info_ptr);
png_create_write_struct_failed:
    ::fclose(fp);
fopen_failed:
    return result;
}

bool write_rgb_to_file(const char *file_name_utf8, size_t width, size_t height, const uint8_t *data_rgb)
{
    return write_rgb_or_gray_to_file(file_name_utf8, width, height, PNG_COLOR_TYPE_RGB, data_rgb);
}

bool write_rgb_to_file(const std::string &file_name_utf8, size_t width, size_t height, const uint8_t *data_rgb)
{
    return write_rgb_to_file(file_name_utf8.c_str(), width, height, data_rgb);
}

bool write_rgb_to_file(const std::string &file_name_utf8, size_t width, size_t height, const std::vector<uint8_t> &data_rgb)
{
    assert(width * height * 3 == data_rgb.size());
    return write_rgb_to_file(file_name_utf8.c_str(), width, height, data_rgb.data());
}

bool write_gray_to_file(const char *file_name_utf8, size_t width, size_t height, const uint8_t *data_gray)
{
    return write_rgb_or_gray_to_file(file_name_utf8, width, height, PNG_COLOR_TYPE_GRAY, data_gray);
}

bool write_gray_to_file(const std::string &file_name_utf8, size_t width, size_t height, const uint8_t *data_gray)
{
    return write_gray_to_file(file_name_utf8.c_str(), width, height, data_gray);
}

bool write_gray_to_file(const std::string &file_name_utf8, size_t width, size_t height, const std::vector<uint8_t> &data_gray)
{
    assert(width * height == data_gray.size());
    return write_gray_to_file(file_name_utf8.c_str(), width, height, data_gray.data());
}

// Scaled variants are mostly useful for debugging purposes, for example to export images of low resolution distance fileds.
// Scaling is done by multiplying rows and columns without any smoothing to emphasise the original pixels.
// png_color_type is PNG_COLOR_TYPE_RGB or PNG_COLOR_TYPE_GRAY
static bool write_rgb_or_gray_to_file_scaled(const char *file_name_utf8, size_t width, size_t height, int png_color_type, const uint8_t *data, size_t scale)
{
    if (scale <= 1)
        return write_rgb_or_gray_to_file(file_name_utf8, width, height, png_color_type, data);
    else {
        size_t pixel_bytes = png_color_type == PNG_COLOR_TYPE_RGB ? 3 : 1;
        size_t line_width  = width * pixel_bytes;
        std::vector<uint8_t> scaled(line_width * height * scale * scale);
        uint8_t *dst = scaled.data();
        for (size_t r = 0; r < height; ++ r) {
            for (size_t repr = 0; repr < scale; ++ repr) {
                const uint8_t *row = data + line_width * r;
                for (size_t c = 0; c < width; ++ c) {
                    for (size_t repc = 0; repc < scale; ++ repc)
                        for (size_t b = 0; b < pixel_bytes; ++ b)
                            *dst ++ = row[b];
                    row += pixel_bytes;
                }
            }
        }
        return write_rgb_or_gray_to_file(file_name_utf8, width * scale, height * scale, png_color_type, scaled.data());
    }
}

bool write_rgb_to_file_scaled(const char *file_name_utf8, size_t width, size_t height, const uint8_t *data_rgb, size_t scale)
{
    return write_rgb_or_gray_to_file_scaled(file_name_utf8, width, height, PNG_COLOR_TYPE_RGB, data_rgb, scale);
}

bool write_rgb_to_file_scaled(const std::string &file_name_utf8, size_t width, size_t height, const uint8_t *data_rgb, size_t scale)
{
    return write_rgb_to_file_scaled(file_name_utf8.c_str(), width, height, data_rgb, scale);
}

bool write_rgb_to_file_scaled(const std::string &file_name_utf8, size_t width, size_t height, const std::vector<uint8_t> &data_rgb, size_t scale)
{
    assert(width * height * 3 == data_rgb.size());
    return write_rgb_to_file_scaled(file_name_utf8.c_str(), width, height, data_rgb.data(), scale);
}

bool write_gray_to_file_scaled(const char *file_name_utf8, size_t width, size_t height, const uint8_t *data_gray, size_t scale)
{
    return write_rgb_or_gray_to_file_scaled(file_name_utf8, width, height, PNG_COLOR_TYPE_GRAY, data_gray, scale);
}

bool write_gray_to_file_scaled(const std::string &file_name_utf8, size_t width, size_t height, const uint8_t *data_gray, size_t scale)
{
    return write_gray_to_file_scaled(file_name_utf8.c_str(), width, height, data_gray, scale);
}

bool write_gray_to_file_scaled(const std::string &file_name_utf8, size_t width, size_t height, const std::vector<uint8_t> &data_gray, size_t scale)
{
    assert(width * height == data_gray.size());
    return write_gray_to_file_scaled(file_name_utf8.c_str(), width, height, data_gray.data(), scale);
}

}} // namespace Slic3r::png
