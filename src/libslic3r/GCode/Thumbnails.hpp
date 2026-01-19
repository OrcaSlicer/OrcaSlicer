#ifndef slic3r_GCodeThumbnails_hpp_
#define slic3r_GCodeThumbnails_hpp_

#include "../Point.hpp"
#include "../PrintConfig.hpp"
#include "../enum_bitmask.hpp"
#include "ThumbnailData.hpp"
#include "../enum_bitmask.hpp"

#include <vector>
#include <memory>
#include <string_view>

#include <boost/beast/core/detail/base64.hpp>

namespace Slic3r {
    enum class ThumbnailError : int { InvalidVal, OutOfRange, InvalidExt };
    using ThumbnailErrors = enum_bitmask<ThumbnailError>;
    ENABLE_ENUM_BITMASK_OPERATORS(ThumbnailError);
}

namespace Slic3r::GCodeThumbnails {

struct CompressedImageBuffer
{
    void       *data { nullptr };
    size_t      size { 0 };
    virtual ~CompressedImageBuffer() {}
    virtual std::string_view tag() const = 0;
};

std::string get_hex(const unsigned int input);
std::string rjust(std::string input, unsigned int width, char fill_char);
std::unique_ptr<CompressedImageBuffer> compress_thumbnail(const ThumbnailData &data, GCodeThumbnailsFormat format);
std::string get_error_string(const ThumbnailErrors& errors);


typedef std::vector<std::pair<GCodeThumbnailsFormat, Vec2d>> GCodeThumbnailDefinitionsList;
using namespace std::literals;
std::pair<GCodeThumbnailDefinitionsList, ThumbnailErrors> make_and_check_thumbnail_list(const std::string& thumbnails_string, const std::string_view def_ext = "PNG"sv);
std::pair<GCodeThumbnailDefinitionsList, ThumbnailErrors> make_and_check_thumbnail_list(const ConfigBase &config);


template<typename WriteToOutput, typename ThrowIfCanceledCallback>
inline void export_thumbnails_to_file(ThumbnailsGeneratorCallback&                                thumbnail_cb,
                                      int                                                         plate_id,
                                      const std::vector<std::pair<GCodeThumbnailsFormat, Vec2d>>& thumbnails_list,
                                      WriteToOutput                                               output,
                                      ThrowIfCanceledCallback                                     throw_if_canceled)
{
    // Write thumbnails using base64 encoding
    if (thumbnail_cb == nullptr)
        return;
    short i = 0;
    bool first_ColPic = true;
    for (const auto& [format, size] : thumbnails_list) {
        static constexpr const size_t max_row_length = 78;
        ThumbnailsList                thumbnails     = thumbnail_cb(ThumbnailsParams{{size}, true, true, true, true, plate_id});
        for (const ThumbnailData &data : thumbnails) {
            if (data.is_valid()) {
                auto compressed = compress_thumbnail(data, format);
                if (compressed->data && compressed->size) {
                    if (format == GCodeThumbnailsFormat::BTT_TFT) {
                        // write BTT_TFT header
                        output((";" + rjust(get_hex(data.width), 4, '0') + rjust(get_hex(data.height), 4, '0') + "\r\n").c_str());
                        output((char *) compressed->data);
                        if (i == (thumbnails_list.size() - 1))
                            output("; bigtree thumbnail end\r\n\r\n");
                    }
                    else if (format == GCodeThumbnailsFormat::ColPic) {
                        if (first_ColPic) {
                            output((boost::format("\n\n;gimage:%s\n\n") % reinterpret_cast<char*>(compressed->data)).str().c_str());
                        } else {
                            output((boost::format("\n\n;simage:%s\n\n") % reinterpret_cast<char*>(compressed->data)).str().c_str());
                        }
                        first_ColPic = false;
                    } 
                    else {
                        output("; THUMBNAIL_BLOCK_START\n");
                        std::string encoded;
                        encoded.resize(boost::beast::detail::base64::encoded_size(compressed->size));
                        encoded.resize(boost::beast::detail::base64::encode((void *) encoded.data(), (const void *) compressed->data,
                                                                            compressed->size));                        
                        output((boost::format("\n;\n; %s begin %dx%d %d\n") % compressed->tag() % data.width % data.height % encoded.size())
                                   .str()
                                   .c_str());                        
                        while (encoded.size() > max_row_length) {
                            output((boost::format("; %s\n") % encoded.substr(0, max_row_length)).str().c_str());
                            encoded = encoded.substr(max_row_length);
                        }

                        // Orca write remaining ecoded data
                        if (encoded.size() > 0)
                            output((boost::format("; %s\n") % encoded).str().c_str());

                        output((boost::format("; %s end\n") % compressed->tag()).str().c_str());
                        output("; THUMBNAIL_BLOCK_END\n\n");
                    }
                    throw_if_canceled();
                }
            }
        }
        i++;
    }
}

} // namespace Slic3r::GCodeThumbnails

#endif // slic3r_GCodeThumbnails_hpp_
