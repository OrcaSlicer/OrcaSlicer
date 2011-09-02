#include "Thumbnails.hpp"
#include "../miniz_extension.hpp"
#include "format.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <qoi/qoi.h>
#include <jpeglib.h>
#include <jerror.h>
#include <vector>
#include <boost/algorithm/string.hpp>

namespace Slic3r::GCodeThumbnails {

using namespace std::literals;

struct CompressedPNG : CompressedImageBuffer
{
    ~CompressedPNG() override { if (data) mz_free(data); }
    std::string_view tag() const override { return "thumbnail"sv; }
};

struct CompressedJPG : CompressedImageBuffer
{
    ~CompressedJPG() override { free(data); }
    std::string_view tag() const override { return "thumbnail_JPG"sv; }
};

struct CompressedQOI : CompressedImageBuffer
{
    ~CompressedQOI() override { free(data); }
    std::string_view tag() const override { return "thumbnail_QOI"sv; }
};

struct CompressedBIQU : CompressedImageBuffer
{
    ~CompressedBIQU() override { free(data); }
    std::string_view tag() const override { return "thumbnail_BIQU"sv; }
};

struct CompressedColPic : CompressedImageBuffer
{
    ~CompressedColPic() override { free(data); }
    std::string_view tag() const override { return "thumbnail_QIDI"sv; }
};

std::unique_ptr<CompressedImageBuffer> compress_thumbnail_png(const ThumbnailData &data)
{
    auto out = std::make_unique<CompressedPNG>();
    out->data = tdefl_write_image_to_png_file_in_memory_ex((const void*)data.pixels.data(), data.width, data.height, 4, &out->size, MZ_DEFAULT_LEVEL, 1);
    return out;
}

std::unique_ptr<CompressedImageBuffer> compress_thumbnail_jpg(const ThumbnailData& data)
{
    // Take vector of RGBA pixels and flip the image vertically
    std::vector<unsigned char> rgba_pixels(data.pixels.size());
    const unsigned int row_size = data.width * 4;
    for (unsigned int y = 0; y < data.height; ++y) {
        ::memcpy(rgba_pixels.data() + (data.height - y - 1) * row_size, data.pixels.data() + y * row_size, row_size);
    }

    // Store pointers to scanlines start for later use
    std::vector<unsigned char*> rows_ptrs;
    rows_ptrs.reserve(data.height);
    for (unsigned int y = 0; y < data.height; ++y) {
        rows_ptrs.emplace_back(&rgba_pixels[y * row_size]);
    }

    std::vector<unsigned char> compressed_data(data.pixels.size());
    unsigned char* compressed_data_ptr = compressed_data.data();
    unsigned long compressed_data_size = data.pixels.size();

    jpeg_error_mgr err;
    jpeg_compress_struct info;
    info.err = jpeg_std_error(&err);
    jpeg_create_compress(&info);
    jpeg_mem_dest(&info, &compressed_data_ptr, &compressed_data_size);

    info.image_width = data.width;
    info.image_height = data.height;
    info.input_components = 4;
    info.in_color_space = JCS_EXT_RGBA;

    jpeg_set_defaults(&info);
    jpeg_set_quality(&info, 85, TRUE);
    jpeg_start_compress(&info, TRUE);

    jpeg_write_scanlines(&info, rows_ptrs.data(), data.height);
    jpeg_finish_compress(&info);
    jpeg_destroy_compress(&info);

    // FIXME -> Add error checking

    auto out = std::make_unique<CompressedJPG>();
    out->data = malloc(compressed_data_size);
    out->size = size_t(compressed_data_size);
    ::memcpy(out->data, (const void*)compressed_data.data(), out->size);
    return out;
}

std::unique_ptr<CompressedImageBuffer> compress_thumbnail_qoi(const ThumbnailData &data)
{
    qoi_desc desc;
    desc.width      = data.width;
    desc.height     = data.height;
    desc.channels   = 4;
    desc.colorspace = QOI_SRGB;

    // Take vector of RGBA pixels and flip the image vertically
    std::vector<uint8_t> rgba_pixels(data.pixels.size() * 4);
    size_t row_size = data.width * 4;
    for (size_t y = 0; y < data.height; ++ y)
        memcpy(rgba_pixels.data() + (data.height - y - 1) * row_size, data.pixels.data() + y * row_size, row_size);

    auto out = std::make_unique<CompressedQOI>();
    int  size;
    out->data = qoi_encode((const void*)rgba_pixels.data(), &desc, &size);
    out->size = size;
    return out;
}

int ColPic_EncodeStr(unsigned short* fromcolor16, int picw, int pich, unsigned char* outputdata, int outputmaxtsize, int colorsmax);

std::unique_ptr<CompressedImageBuffer> compress_thumbnail_colpic(const ThumbnailData &data)
{
    const int MAX_SIZE = 512;
    int width = int(data.width);
    int height = int(data.height);

    // Orca: cap data size to MAX_SIZE while maintaining aspect ratio
    if (width > MAX_SIZE || height > MAX_SIZE) {
        double aspectRatio = static_cast<double>(width) / height;
        if (aspectRatio > 1.0) {
            width = MAX_SIZE;
            height = static_cast<int>(MAX_SIZE / aspectRatio);
        } else {
            height = MAX_SIZE;
            width = static_cast<int>(MAX_SIZE * aspectRatio);
        }
    }

    std::vector<unsigned short> color16_buf(width * height);
    std::vector<unsigned char> output_buf(height * width * 10);

    std::vector<uint8_t> rgba_pixels(data.pixels.size() * 4);
    size_t               row_size = width * 4;
    for (size_t y = 0; y < height; ++y)
        memcpy(rgba_pixels.data() + y * row_size, data.pixels.data() + y * row_size, row_size);
    const unsigned char* pixels;
    pixels = (const unsigned char*) rgba_pixels.data();
    int r = 0, g = 0, b = 0, a = 0, rgb = 0;
    int time = width * height - 1;
    for (int row = 0; row < height; ++row) {
        int rr = row * width;
        for (int col = 0; col < width; ++col) {
            const int pix_idx = 4 * (rr + width - col - 1);
            r                 = int(pixels[pix_idx]) >> 3;
            g                 = int(pixels[pix_idx + 1]) >> 2;
            b                 = int(pixels[pix_idx + 2]) >> 3;
            a                 = int(pixels[pix_idx + 3]);
            if (a == 0) {
                r = 46 >> 3;
                g = 51 >> 2;
                b = 72 >> 3;
            }
            rgb             = (r << 11) | (g << 5) | b;
            color16_buf[time--] = rgb;
        }
    }

    ColPic_EncodeStr(color16_buf.data(), width, height, output_buf.data(), output_buf.size(), 1024);

    auto out  = std::make_unique<CompressedColPic>();
    out->size = output_buf.size();
    out->data = malloc(out->size);
    ::memcpy(out->data, output_buf.data(), out->size);
    return out;
}

std::unique_ptr<CompressedImageBuffer> compress_thumbnail_btt_tft(const ThumbnailData &data) {

    // Take vector of RGBA pixels and flip the image vertically
    std::vector<unsigned char> rgba_pixels(data.pixels.size());
    const unsigned int row_size = data.width * 4;
    for (unsigned int y = 0; y < data.height; ++y) {
        ::memcpy(rgba_pixels.data() + (data.height - y - 1) * row_size, data.pixels.data() + y * row_size, row_size);
    }

    auto out = std::make_unique<CompressedBIQU>();

    // get the output size of the data
    // add 4 bytes to the row_size to account for end of line (\r\n)
    // add 1 byte for the 0 of the c_str
    out->size = data.height * (row_size + 4) + 1;
    out->data = malloc(out->size);

    std::stringstream out_data;
    typedef struct {unsigned char r, g, b, a;} pixel;
    pixel px;
    for (unsigned int ypos = 0; ypos < data.height; ypos++) {
        std::stringstream line;
        line << ";";
        for (unsigned int xpos = 0; xpos < row_size; xpos+=4) {
            px.r = rgba_pixels[ypos * row_size + xpos];
            px.g = rgba_pixels[ypos * row_size + xpos + 1];
            px.b = rgba_pixels[ypos * row_size + xpos + 2];
            px.a = rgba_pixels[ypos * row_size + xpos + 3];

            // calculate values for RGB with alpha
            const uint8_t rv = ((px.a * px.r) / 255);
            const uint8_t gv = ((px.a * px.g) / 255);
            const uint8_t bv = ((px.a * px.b) / 255);

            // convert the RGB values to RGB565 hex that is right justified (same algorithm BTT firmware uses)
            auto color_565 = rjust(get_hex(((rv >> 3) << 11) | ((gv >> 2) << 5) | (bv >> 3)), 4, '0');

            //BTT original converter specifies these values should be '0000'
            if (color_565 == "0020" || color_565 == "0841" || color_565 == "0861")
                color_565 = "0000";
            //add the color to the line
            line << color_565;
        }
        // output line and end line (\r\n is important. BTT firmware requires it)
        out_data << line.str() << "\r\n";
        line.clear();
    }
    ::memcpy(out->data, (const void*) out_data.str().c_str(), out->size);
    return out;
}

std::string get_hex(const unsigned int input) {
    std::stringstream stream;
    stream << std::hex << input;
    return stream.str();
}

std::string rjust(std::string input, unsigned int width, char fill_char) {
    std::stringstream stream;
    stream.fill(fill_char);
    stream.width(width);
    stream << input;
    return stream.str();
}

std::unique_ptr<CompressedImageBuffer> compress_thumbnail(const ThumbnailData &data, GCodeThumbnailsFormat format)
{
    switch (format) {
    case GCodeThumbnailsFormat::PNG:
    default:
        return compress_thumbnail_png(data);
    case GCodeThumbnailsFormat::JPG:
        return compress_thumbnail_jpg(data);
    case GCodeThumbnailsFormat::QOI:
        return compress_thumbnail_qoi(data);
    case GCodeThumbnailsFormat::BTT_TFT:
        return compress_thumbnail_btt_tft(data);
    case GCodeThumbnailsFormat::ColPic:
        return compress_thumbnail_colpic(data);
    }
}

typedef struct
{
    unsigned short colo16;
    unsigned char  A0;
    unsigned char  A1;
    unsigned char  A2;
    unsigned char  res0;
    unsigned short res1;
    unsigned int   qty;
} U16HEAD;
typedef struct
{
    unsigned char  encodever;
    unsigned char  res0;
    unsigned short oncelistqty;
    unsigned int   PicW;
    unsigned int   PicH;
    unsigned int   mark;
    unsigned int   ListDataSize;
    unsigned int   ColorDataSize;
    unsigned int   res1;
    unsigned int   res2;
} ColPicHead3;

static void colmemmove(unsigned char* dec, unsigned char* src, int lenth)
{
    if (src < dec) {
        dec += lenth - 1;
        src += lenth - 1;
        while (lenth > 0) {
            *(dec--) = *(src--);
            lenth--;
        }
    } else {
        while (lenth > 0) {
            *(dec++) = *(src++);
            lenth--;
        }
    }
}
static void colmemcpy(unsigned char* dec, unsigned char* src, int lenth)
{
    while (lenth > 0) {
        *(dec++) = *(src++);
        lenth--;
    }
}
static void colmemset(unsigned char* dec, unsigned char val, int lenth)
{
    while (lenth > 0) {
        *(dec++) = val;
        lenth--;
    }
}

static void ADList0(unsigned short val, U16HEAD* listu16, int* listqty, int maxqty)
{
    unsigned char A0;
    unsigned char A1;
    unsigned char A2;
    int           qty = *listqty;
    if (qty >= maxqty)
        return;
    for (int i = 0; i < qty; i++) {
        if (listu16[i].colo16 == val) {
            listu16[i].qty++;
            return;
        }
    }
    A0         = (unsigned char) (val >> 11);
    A1         = (unsigned char) ((val << 5) >> 10);
    A2         = (unsigned char) ((val << 11) >> 11);
    U16HEAD* a = &listu16[qty];
    a->colo16  = val;
    a->A0      = A0;
    a->A1      = A1;
    a->A2      = A2;
    a->qty     = 1;
    *listqty   = qty + 1;
}

static int Byte8bitEncode(
    unsigned short* fromcolor16, unsigned short* listu16, int listqty, int dotsqty, unsigned char* outputdata, int decMaxBytesize)
{
    unsigned char tid, sid;
    int           dots     = 0;
    int           srcindex = 0;
    int           decindex = 0;
    int           lastid   = 0;
    int           temp     = 0;
    while (dotsqty > 0) {
        dots = 1;
        for (int i = 0; i < (dotsqty - 1); i++) {
            if (fromcolor16[srcindex + i] != fromcolor16[srcindex + i + 1])
                break;
            dots++;
            if (dots == 255)
                break;
        }
        temp = 0;
        for (int i = 0; i < listqty; i++) {
            if (listu16[i] == fromcolor16[srcindex]) {
                temp = i;
                break;
            }
        }
        tid = (unsigned char) (temp % 32);
        sid = (unsigned char) (temp / 32);
        if (lastid != sid) {
            if (decindex >= decMaxBytesize)
                goto IL_END;
            outputdata[decindex] = 7;
            outputdata[decindex] <<= 5;
            outputdata[decindex] += sid;
            decindex++;
            lastid = sid;
        }
        if (dots <= 6) {
            if (decindex >= decMaxBytesize)
                goto IL_END;
            outputdata[decindex] = (unsigned char) dots;
            outputdata[decindex] <<= 5;
            outputdata[decindex] += tid;
            decindex++;
        } else {
            if (decindex >= decMaxBytesize)
                goto IL_END;
            outputdata[decindex] = 0;
            outputdata[decindex] += tid;
            decindex++;
            if (decindex >= decMaxBytesize)
                goto IL_END;
            outputdata[decindex] = (unsigned char) dots;
            decindex++;
        }
        srcindex += dots;
        dotsqty -= dots;
    }
IL_END:
    return decindex;
}

static int ColPicEncode(unsigned short* fromcolor16, int picw, int pich, unsigned char* outputdata, int outputmaxtsize, int colorsmax)
{
    U16HEAD      l0;
    int          cha0, cha1, cha2, fid, minval;
    ColPicHead3* Head0 = nullptr;
    U16HEAD      Listu16[1024];
    int          ListQty = 0;
    int          enqty   = 0;
    int          dotsqty = picw * pich;
    if (colorsmax > 1024)
        colorsmax = 1024;
    for (int i = 0; i < dotsqty; i++) {
        int ch = (int) fromcolor16[i];
        ADList0(ch, Listu16, &ListQty, 1024);
    }

    for (int index = 1; index < ListQty; index++) {
        l0 = Listu16[index];
        for (int i = 0; i < index; i++) {
            if (l0.qty >= Listu16[i].qty) {
                colmemmove((unsigned char*) &Listu16[i + 1], (unsigned char*) &Listu16[i], (index - i) * sizeof(U16HEAD));
                colmemcpy((unsigned char*) &Listu16[i], (unsigned char*) &l0, sizeof(U16HEAD));
                break;
            }
        }
    }
    while (ListQty > colorsmax) {
        l0     = Listu16[ListQty - 1];
        minval = 255;
        fid    = -1;
        for (int i = 0; i < colorsmax; i++) {
            cha0 = Listu16[i].A0 - l0.A0;
            if (cha0 < 0)
                cha0 = 0 - cha0;
            cha1 = Listu16[i].A1 - l0.A1;
            if (cha1 < 0)
                cha1 = 0 - cha1;
            cha2 = Listu16[i].A2 - l0.A2;
            if (cha2 < 0)
                cha2 = 0 - cha2;
            int chall = cha0 + cha1 + cha2;
            if (chall < minval) {
                minval = chall;
                fid    = i;
            }
        }
        for (int i = 0; i < dotsqty; i++) {
            if (fromcolor16[i] == l0.colo16)
                fromcolor16[i] = Listu16[fid].colo16;
        }
        ListQty = ListQty - 1;
    }
    Head0 = ((ColPicHead3*) outputdata);
    colmemset(outputdata, 0, sizeof(ColPicHead3));
    Head0->encodever    = 3;
    Head0->oncelistqty  = 0;
    Head0->mark         = 0x05DDC33C;
    Head0->ListDataSize = ListQty * 2;
    for (int i = 0; i < ListQty; i++) {
        unsigned short* l0 = (unsigned short*) &outputdata[sizeof(ColPicHead3)];
        l0[i]              = Listu16[i].colo16;
    }
    enqty = Byte8bitEncode(fromcolor16, (unsigned short*) &outputdata[sizeof(ColPicHead3)], Head0->ListDataSize >> 1, dotsqty,
                           &outputdata[sizeof(ColPicHead3) + Head0->ListDataSize],
                           outputmaxtsize - sizeof(ColPicHead3) - Head0->ListDataSize);
    Head0->ColorDataSize = enqty;
    Head0->PicW          = picw;
    Head0->PicH          = pich;
    return sizeof(ColPicHead3) + Head0->ListDataSize + Head0->ColorDataSize;
}

int ColPic_EncodeStr(unsigned short* fromcolor16, int picw, int pich, unsigned char* outputdata, int outputmaxtsize, int colorsmax)
{
    int           qty      = 0;
    int           temp     = 0;
    int           strindex = 0;
    int           hexindex = 0;
    unsigned char TempBytes[4];
    qty = ColPicEncode(fromcolor16, picw, pich, outputdata, outputmaxtsize, colorsmax);
    if (qty == 0)
        return 0;
    temp = 3 - (qty % 3);
    while (temp > 0) {
        outputdata[qty] = 0;
        qty++;
        temp--;
    }
    if ((qty * 4 / 3) >= outputmaxtsize)
        return 0;
    hexindex = qty;
    strindex = (qty * 4 / 3);
    while (hexindex > 0) {
        hexindex -= 3;
        strindex -= 4;

        TempBytes[0] = (unsigned char) (outputdata[hexindex] >> 2);
        TempBytes[1] = (unsigned char) (outputdata[hexindex] & 3);
        TempBytes[1] <<= 4;
        TempBytes[1] += ((unsigned char) (outputdata[hexindex + 1] >> 4));
        TempBytes[2] = (unsigned char) (outputdata[hexindex + 1] & 15);
        TempBytes[2] <<= 2;
        TempBytes[2] += ((unsigned char) (outputdata[hexindex + 2] >> 6));
        TempBytes[3] = (unsigned char) (outputdata[hexindex + 2] & 63);

        TempBytes[0] += 48;
        if (TempBytes[0] == (unsigned char) '\\')
            TempBytes[0] = 126;
        TempBytes[0 + 1] += 48;
        if (TempBytes[0 + 1] == (unsigned char) '\\')
            TempBytes[0 + 1] = 126;
        TempBytes[0 + 2] += 48;
        if (TempBytes[0 + 2] == (unsigned char) '\\')
            TempBytes[0 + 2] = 126;
        TempBytes[0 + 3] += 48;
        if (TempBytes[0 + 3] == (unsigned char) '\\')
            TempBytes[0 + 3] = 126;

        outputdata[strindex]     = TempBytes[0];
        outputdata[strindex + 1] = TempBytes[1];
        outputdata[strindex + 2] = TempBytes[2];
        outputdata[strindex + 3] = TempBytes[3];
    }
    qty             = qty * 4 / 3;
    outputdata[qty] = 0;
    return qty;
}
std::pair<GCodeThumbnailDefinitionsList, ThumbnailErrors> make_and_check_thumbnail_list(const std::string& thumbnails_string, const std::string_view def_ext /*= "PNG"sv*/)
{
    if (thumbnails_string.empty())
        return {};

    std::istringstream is(thumbnails_string);
    std::string point_str;

    ThumbnailErrors errors;

    // generate thumbnails data to process it

    GCodeThumbnailDefinitionsList thumbnails_list;
    while (std::getline(is, point_str, ',')) {
        Vec2d point(Vec2d::Zero());
        GCodeThumbnailsFormat format;
        std::istringstream iss(point_str);
        std::string coord_str;
        if (std::getline(iss, coord_str, 'x') && !coord_str.empty()) {
            std::istringstream(coord_str) >> point(0);
            if (std::getline(iss, coord_str, '/') && !coord_str.empty()) {
                std::istringstream(coord_str) >> point(1);

                if (0 < point(0) && point(0) < 1000 && 0 < point(1) && point(1) < 1000) {
                    std::string ext_str;
                    std::getline(iss, ext_str, '/');

                    if (ext_str.empty())
                        ext_str = def_ext.empty() ? "PNG"sv : def_ext;

                    // check validity of extention
                    boost::to_upper(ext_str);
                    if (!ConfigOptionEnum<GCodeThumbnailsFormat>::from_string(ext_str, format)) {
                        format = GCodeThumbnailsFormat::PNG;
                        errors = enum_bitmask(errors | ThumbnailError::InvalidExt);
                    }

                    thumbnails_list.emplace_back(std::make_pair(format, point));
                }
                else
                    errors = enum_bitmask(errors | ThumbnailError::OutOfRange);
                continue;
            }
        }
        errors = enum_bitmask(errors | ThumbnailError::InvalidVal);
    }

    return std::make_pair(std::move(thumbnails_list), errors);
}

std::pair<GCodeThumbnailDefinitionsList, ThumbnailErrors> make_and_check_thumbnail_list(const ConfigBase& config)
{
    // ??? Unit tests or command line slicing may not define "thumbnails" or "thumbnails_format".
    // ??? If "thumbnails_format" is not defined, export to PNG.

    // generate thumbnails data to process it

    if (const auto thumbnails_value = config.option<ConfigOptionString>("thumbnails"))
        return make_and_check_thumbnail_list(thumbnails_value->value);

    return {};
}

std::string get_error_string(const ThumbnailErrors& errors)
{
    std::string error_str;

    if (errors.has(ThumbnailError::InvalidVal))
        error_str += "\n - " + Slic3r::format("Invalid input format. Expected vector of dimensions in the following format: \"%1%\"", "XxY/EXT, XxY/EXT, ...");
    if (errors.has(ThumbnailError::OutOfRange))
        error_str += "\n - Input value is out of range";
    if (errors.has(ThumbnailError::InvalidExt))
        error_str += "\n - Some extension in the input is invalid";

    return error_str;
}

} // namespace Slic3r::GCodeThumbnails
