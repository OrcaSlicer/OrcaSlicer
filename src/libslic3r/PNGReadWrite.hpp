#ifndef PNGREAD_HPP
#define PNGREAD_HPP

#include <cstdint>
#include <vector>
#include <string>
#include <istream>

namespace Slic3r { namespace png {

// Interface for an input stream of encoded png image data.
struct IStream {
    virtual ~IStream() = default;
    virtual size_t read(std::uint8_t *outp, size_t amount) = 0;
    virtual bool is_ok() const = 0;
};

// The output format of decode_png: a 2D pixel matrix stored continuously row
// after row (row major layout).
template<class PxT> struct Image {
    std::vector<PxT> buf;
    size_t rows, cols;
    PxT get(size_t row, size_t col) const { return buf[row * cols + col]; }
};

using ImageGreyscale = Image<uint8_t>;
struct ImageColorscale:Image<unsigned char>
{
    int bytes_per_pixel;
};


// Only decodes true 8 bit grayscale png images. Returns false for other formats
// TODO (if needed): implement transformation of rgb images into grayscale...
bool decode_png(IStream &stream, ImageGreyscale &out_img);

//BBS: decode png for other format
bool decode_colored_png(IStream &in_buf, ImageColorscale &out_img);

// TODO (if needed)
// struct RGB { uint8_t r, g, b; };
// using ImageRGB = Image<RGB>;
// bool decode_png(IStream &stream, ImageRGB &img);


// Encoded png data buffer: a simple read-only buffer and its size.
struct ReadBuf { const void *buf = nullptr; const size_t sz = 0; };

bool is_png(const ReadBuf &pngbuf);

struct ReadBufStream: public IStream {
    const ReadBuf &rbuf_ref;
    size_t pos = 0;

    explicit ReadBufStream(const ReadBuf &buf): rbuf_ref{buf} {}

    size_t read(std::uint8_t *outp, size_t amount) override
    {
        if (amount > rbuf_ref.sz - pos) return 0;

        auto buf = static_cast<const std::uint8_t *>(rbuf_ref.buf);
        std::copy(buf + pos, buf + (pos + amount), outp);
        pos += amount;

        return amount;
    }

    bool is_ok() const override { return pos < rbuf_ref.sz; }
};

template<class Img> bool decode_png(const ReadBuf &in_buf, Img &out_img)
{
    struct ReadBufStream stream{in_buf};

    return decode_png(stream, out_img);
}

bool decode_colored_png(const ReadBuf &in_buf, ImageColorscale &out_img);


// TODO: std::istream of FILE* could be similarly adapted in case its needed...



// Down to earth function to store a packed RGB image to file. Mostly useful for debugging purposes.
bool write_rgb_to_file(const char *file_name_utf8, size_t width, size_t height, const uint8_t *data_rgb);
bool write_rgb_to_file(const std::string &file_name_utf8, size_t width, size_t height, const uint8_t *data_rgb);
bool write_rgb_to_file(const std::string &file_name_utf8, size_t width, size_t height, const std::vector<uint8_t> &data_rgb);
// Grayscale variants
bool write_gray_to_file(const char *file_name_utf8, size_t width, size_t height, const uint8_t *data_gray);
bool write_gray_to_file(const std::string &file_name_utf8, size_t width, size_t height, const uint8_t *data_gray);
bool write_gray_to_file(const std::string &file_name_utf8, size_t width, size_t height, const std::vector<uint8_t> &data_gray);

// Scaled variants are mostly useful for debugging purposes, for example to export images of low resolution distance fileds.
// Scaling is done by multiplying rows and columns without any smoothing to emphasise the original pixels.
bool write_rgb_to_file_scaled(const char *file_name_utf8, size_t width, size_t height, const uint8_t *data_rgb, size_t scale);
bool write_rgb_to_file_scaled(const std::string &file_name_utf8, size_t width, size_t height, const uint8_t *data_rgb, size_t scale);
bool write_rgb_to_file_scaled(const std::string &file_name_utf8, size_t width, size_t height, const std::vector<uint8_t> &data_rgb, size_t scale);
// Grayscale variants
bool write_gray_to_file_scaled(const char *file_name_utf8, size_t width, size_t height, const uint8_t *data_gray, size_t scale);
bool write_gray_to_file_scaled(const std::string &file_name_utf8, size_t width, size_t height, const uint8_t *data_gray, size_t scale);
bool write_gray_to_file_scaled(const std::string &file_name_utf8, size_t width, size_t height, const std::vector<uint8_t> &data_gray, size_t scale);


}}     // namespace Slic3r::png

#endif // PNGREAD_HPP
