#include "tinyply.h"
// Moved from origin tinyply.h
////////////////////////////////
//   tinyply implementation   //
////////////////////////////////

#include <algorithm>
#include <functional>
#include <type_traits>
#include <iostream>
#include <cstring>
#include <cassert>
#include <stdint.h>

namespace igl
{
namespace tinyply
{
template<typename T, typename T2> T2 endian_swap(const T & /*v*/) noexcept {assert(false);} //{ return v; }

template<>  uint16_t IGL_INLINE endian_swap<uint16_t, uint16_t>(const uint16_t & v) noexcept { return (v << 8) | (v >> 8); }
template<>  uint32_t IGL_INLINE endian_swap<uint32_t, uint32_t>(const uint32_t & v) noexcept { return (v << 24) | ((v << 8) & 0x00ff0000) | ((v >> 8) & 0x0000ff00) | (v >> 24); }
template<>  uint64_t IGL_INLINE endian_swap<uint64_t, uint64_t>(const uint64_t & v) noexcept
{
    return (((v & 0x00000000000000ffLL) << 56) |
            ((v & 0x000000000000ff00LL) << 40) |
            ((v & 0x0000000000ff0000LL) << 24) |
            ((v & 0x00000000ff000000LL) << 8)  |
            ((v & 0x000000ff00000000LL) >> 8)  |
            ((v & 0x0000ff0000000000LL) >> 24) |
            ((v & 0x00ff000000000000LL) >> 40) |
            ((v & 0xff00000000000000LL) >> 56));
}
template<>  int16_t IGL_INLINE endian_swap<int16_t, int16_t>(const int16_t & v) noexcept { uint16_t r = endian_swap<uint16_t, uint16_t>(*(uint16_t*)&v); return *(int16_t*)&r; }
template<>  int32_t IGL_INLINE endian_swap<int32_t, int32_t>(const int32_t & v) noexcept { uint32_t r = endian_swap<uint32_t, uint32_t>(*(uint32_t*)&v); return *(int32_t*)&r; }
template<>  int64_t IGL_INLINE endian_swap<int64_t, int64_t>(const int64_t & v) noexcept { uint64_t r = endian_swap<uint64_t, uint64_t>(*(uint64_t*)&v); return *(int64_t*)&r; }
template<>  float IGL_INLINE endian_swap<uint32_t, float>(const uint32_t & v) noexcept { union { float f; uint32_t i; }; i = endian_swap<uint32_t, uint32_t>(v); return f; }
template<>  double IGL_INLINE endian_swap<uint64_t, double>(const uint64_t & v) noexcept { union { double d; uint64_t i; }; i = endian_swap<uint64_t, uint64_t>(v); return d; }


IGL_INLINE uint32_t hash_fnv1a(const std::string & str) noexcept
{
    static const uint32_t fnv1aBase32 = 0x811C9DC5u;
    static const uint32_t fnv1aPrime32 = 0x01000193u;
    uint32_t result = fnv1aBase32;
    for (auto & c : str) { result ^= static_cast<uint32_t>(c); result *= fnv1aPrime32; }
    return result;
}

IGL_INLINE Type property_type_from_string(const std::string & t) noexcept
{
    if (t == "int8" || t == "char")           return Type::INT8;
    else if (t == "uint8" || t == "uchar")    return Type::UINT8;
    else if (t == "int16" || t == "short")    return Type::INT16;
    else if (t == "uint16" || t == "ushort")  return Type::UINT16;
    else if (t == "int32" || t == "int")      return Type::INT32;
    else if (t == "uint32" || t == "uint")    return Type::UINT32;
    else if (t == "float32" || t == "float")  return Type::FLOAT32;
    else if (t == "float64" || t == "double") return Type::FLOAT64;
    return Type::INVALID;
}

struct PlyFile::PlyFileImpl
{
    struct PlyDataCursor
    {
        size_t byteOffset{ 0 };
        size_t totalSizeBytes{ 0 };
    };

    struct ParsingHelper
    {
        std::shared_ptr<PlyData> data;
        std::shared_ptr<PlyDataCursor> cursor;
        uint32_t list_size_hint;
    };

    struct PropertyLookup
    {
        ParsingHelper * helper{ nullptr };
        bool skip{ false };
        size_t prop_stride{ 0 }; // precomputed
        size_t list_stride{ 0 }; // precomputed
    };

    std::unordered_map<uint32_t, ParsingHelper> userData;

    bool isBinary = false;
    bool isBigEndian = false;
    std::vector<PlyElement> elements;
    std::vector<std::string> comments;
    std::vector<std::string> objInfo;
    uint8_t scratch[64]; // large enough for max list size

    void read(std::istream & is);
    void write(std::ostream & os, bool isBinary);

    std::shared_ptr<PlyData> request_properties_from_element(const std::string & elementKey,
        const std::vector<std::string> propertyKeys,
        const uint32_t list_size_hint);

    void add_properties_to_element(const std::string & elementKey,
        const std::vector<std::string> propertyKeys,
        const Type type, const size_t count, uint8_t * data, const Type listType, const size_t listCount);

    size_t read_property_binary(const size_t & stride, void * dest, size_t & destOffset, std::istream & is) noexcept;
    size_t read_property_ascii(const Type & t, const size_t & stride, void * dest, size_t & destOffset, std::istream & is);

    std::vector<std::vector<PropertyLookup>> make_property_lookup_table();

    bool parse_header(std::istream & is);
    void parse_data(std::istream & is, bool firstPass);
    void read_header_format(std::istream & is);
    void read_header_element(std::istream & is);
    void read_header_property(std::istream & is);
    void read_header_text(std::string line, std::vector<std::string> & place, int erase = 0);

    void write_header(std::ostream & os) noexcept;
    void write_ascii_internal(std::ostream & os) noexcept;
    void write_binary_internal(std::ostream & os) noexcept;
    void write_property_ascii(Type t, std::ostream & os, uint8_t * src, size_t & srcOffset);
    void write_property_binary(std::ostream & os, uint8_t * src, size_t & srcOffset, const size_t & stride) noexcept;
};

IGL_INLINE PlyProperty::PlyProperty(std::istream & is) : isList(false)
{
    std::string type;
    is >> type;
    if (type == "list")
    {
        std::string countType;
        is >> countType >> type;
        listType = property_type_from_string(countType);
        isList = true;
    }
    propertyType = property_type_from_string(type);
    is >> name;
}

IGL_INLINE PlyElement::PlyElement(std::istream & is)
{
    is >> name >> size;
}

template<typename T> IGL_INLINE T ply_read_ascii(std::istream & is)
{
    T data;
    is >> data;
    return data;
}

template<typename T, typename T2>
IGL_INLINE void endian_swap_buffer(uint8_t * data_ptr, const size_t num_bytes, const size_t stride)
{
    for (size_t count = 0; count < num_bytes; count += stride)
    {
        *(reinterpret_cast<T2 *>(data_ptr)) = endian_swap<T, T2>(*(reinterpret_cast<const T *>(data_ptr)));
        data_ptr += stride;
    }
}

template<typename T> void ply_cast_ascii(void * dest, std::istream & is)
{
    *(static_cast<T *>(dest)) = ply_read_ascii<T>(is);
}

IGL_INLINE int64_t find_element(const std::string & key, const std::vector<PlyElement> & list)
{
    for (size_t i = 0; i < list.size(); i++) if (list[i].name == key) return i;
    return -1;
}

IGL_INLINE int64_t find_property(const std::string & key, const std::vector<PlyProperty> & list)
{
    for (size_t i = 0; i < list.size(); ++i) if (list[i].name == key) return i;
    return -1;
}

// The `userData` table is an easy data structure for capturing what data the
// user would like out of the ply file, but an inner-loop hash lookup is non-ideal.
// The property lookup table flattens the table down into a 2D array optimized
// for parsing. The first index is the element, and the second index is the property.
IGL_INLINE std::vector<std::vector<PlyFile::PlyFileImpl::PropertyLookup>> PlyFile::PlyFileImpl::make_property_lookup_table()
{
    std::vector<std::vector<PropertyLookup>> element_property_lookup;

    for (auto & element : elements)
    {
        std::vector<PropertyLookup> lookups;

        for (auto & property : element.properties)
        {
            PropertyLookup f;

            auto cursorIt = userData.find(hash_fnv1a(element.name + property.name));
            if (cursorIt != userData.end()) f.helper = &cursorIt->second;
            else f.skip = true;

            f.prop_stride = PropertyTable[property.propertyType].stride;
            if (property.isList) f.list_stride = PropertyTable[property.listType].stride;

            lookups.push_back(f);
        }

        element_property_lookup.push_back(lookups);
    }

    return element_property_lookup;
}

IGL_INLINE bool PlyFile::PlyFileImpl::parse_header(std::istream & is)
{
    std::string line;
    bool success = true;
    while (std::getline(is, line))
    {
        std::istringstream ls(line);
        std::string token;
        ls >> token;
        if (token == "ply" || token == "PLY" || token == "") continue;
        else if (token == "comment")    read_header_text(line, comments, 8);
        else if (token == "format")     read_header_format(ls);
        else if (token == "element")    read_header_element(ls);
        else if (token == "property")   read_header_property(ls);
        else if (token == "obj_info")   read_header_text(line, objInfo, 9);
        else if (token == "end_header") break;
        else success = false; // unexpected header field
    }
    return success;
}

IGL_INLINE void PlyFile::PlyFileImpl::read_header_text(std::string line, std::vector<std::string>& place, int erase)
{
    place.push_back((erase > 0) ? line.erase(0, erase) : line);
}

IGL_INLINE void PlyFile::PlyFileImpl::read_header_format(std::istream & is)
{
    std::string s;
    (is >> s);
    if (s == "binary_little_endian") isBinary = true;
    else if (s == "binary_big_endian") isBinary = isBigEndian = true;
}

IGL_INLINE void PlyFile::PlyFileImpl::read_header_element(std::istream & is)
{
    elements.emplace_back(is);
}

IGL_INLINE void PlyFile::PlyFileImpl::read_header_property(std::istream & is)
{
    if (!elements.size()) throw std::runtime_error("no elements defined; file is malformed");
    elements.back().properties.emplace_back(is);
}

IGL_INLINE size_t PlyFile::PlyFileImpl::read_property_binary(const size_t & stride, void * dest, size_t & destOffset, std::istream & is) noexcept
{
    destOffset += stride;
    is.read((char*)dest, stride);
    return stride;
}

IGL_INLINE size_t PlyFile::PlyFileImpl::read_property_ascii(const Type & t, const size_t & stride, void * dest, size_t & destOffset, std::istream & is)
{
    destOffset += stride;
    switch (t)
    {
    case Type::INT8:       *((int8_t *)dest)  = static_cast<int8_t>(ply_read_ascii<int32_t>(is));   break;
    case Type::UINT8:      *((uint8_t *)dest) = static_cast<uint8_t>(ply_read_ascii<uint32_t>(is)); break;
    case Type::INT16:      ply_cast_ascii<int16_t>(dest, is);                 break;
    case Type::UINT16:     ply_cast_ascii<uint16_t>(dest, is);                break;
    case Type::INT32:      ply_cast_ascii<int32_t>(dest, is);                 break;
    case Type::UINT32:     ply_cast_ascii<uint32_t>(dest, is);                break;
    case Type::FLOAT32:    ply_cast_ascii<float>(dest, is);                   break;
    case Type::FLOAT64:    ply_cast_ascii<double>(dest, is);                  break;
    case Type::INVALID:    throw std::invalid_argument("invalid ply property");
    }
    return stride;
}

IGL_INLINE void PlyFile::PlyFileImpl::write_property_ascii(Type t, std::ostream & os, uint8_t * src, size_t & srcOffset)
{
    switch (t)
    {
    case Type::INT8:       os << static_cast<int32_t>(*reinterpret_cast<int8_t*>(src));   break;
    case Type::UINT8:      os << static_cast<uint32_t>(*reinterpret_cast<uint8_t*>(src)); break;
    case Type::INT16:      os << *reinterpret_cast<int16_t*>(src);  break;
    case Type::UINT16:     os << *reinterpret_cast<uint16_t*>(src); break;
    case Type::INT32:      os << *reinterpret_cast<int32_t*>(src);  break;
    case Type::UINT32:     os << *reinterpret_cast<uint32_t*>(src); break;
    case Type::FLOAT32:    os << *reinterpret_cast<float*>(src);    break;
    case Type::FLOAT64:    os << *reinterpret_cast<double*>(src);   break;
    case Type::INVALID:    throw std::invalid_argument("invalid ply property");
    }
    os << " ";
    srcOffset += PropertyTable[t].stride;
}

IGL_INLINE void PlyFile::PlyFileImpl::write_property_binary(std::ostream & os, uint8_t * src, size_t & srcOffset, const size_t & stride) noexcept
{
    os.write((char *)src, stride);
    srcOffset += stride;
}

IGL_INLINE void PlyFile::PlyFileImpl::read(std::istream & is)
{
    std::vector<std::shared_ptr<PlyData>> buffers;
    for (auto & entry : userData) buffers.push_back(entry.second.data);

    // Discover if we can allocate up front without parsing the file twice
    uint32_t list_hints = 0;
    for (auto & b : buffers) for (auto & entry : userData) {list_hints += entry.second.list_size_hint;(void)b;}

    // No list hints? Then we need to calculate how much memory to allocate
    if (list_hints == 0)
    {
        parse_data(is, true);
    }

    // Count the number of properties (required for allocation)
    // e.g. if we have properties x y and z requested, we ensure
    // that their buffer points to the same PlyData
    std::unordered_map<PlyData*, int32_t> unique_data_count;
    for (auto & ptr : buffers) unique_data_count[ptr.get()] += 1;

    // Since group-requested properties share the same cursor,
    // we need to find unique cursors so we only allocate once
    std::sort(buffers.begin(), buffers.end());
    buffers.erase(std::unique(buffers.begin(), buffers.end()), buffers.end());

    // We sorted by ptrs on PlyData, need to remap back onto its cursor in the userData table
    for (auto & b : buffers)
    {
        for (auto & entry : userData)
        {
            if (entry.second.data == b && b->buffer.get() == nullptr)
            {
                // If we didn't receive any list hints, it means we did two passes over the
                // file to compute the total length of all (potentially) variable-length lists
                if (list_hints == 0)
                {
                    b->buffer = Buffer(entry.second.cursor->totalSizeBytes);
                }
                else
                {
                    // otherwise, we can allocate up front, skipping the first pass.
                    const size_t list_size_multiplier = (entry.second.data->isList ? entry.second.list_size_hint : 1);
                    auto bytes_per_property = entry.second.data->count * PropertyTable[entry.second.data->t].stride * list_size_multiplier;
                    bytes_per_property *= unique_data_count[b.get()];
                    b->buffer = Buffer(bytes_per_property);
                }

            }
        }
    }

    // Populate the data
    parse_data(is, false);

    // In-place big-endian to little-endian swapping if required
    if (isBigEndian)
    {
        for (auto & b : buffers)
        {
            uint8_t * data_ptr = b->buffer.get();
            const size_t stride = PropertyTable[b->t].stride;
            const size_t buffer_size_bytes = b->buffer.size_bytes();

            switch (b->t)
            {
            case Type::INT16:   endian_swap_buffer<int16_t, int16_t>(data_ptr, buffer_size_bytes, stride);   break;
            case Type::UINT16:  endian_swap_buffer<uint16_t, uint16_t>(data_ptr, buffer_size_bytes, stride); break;
            case Type::INT32:   endian_swap_buffer<int32_t, int32_t>(data_ptr, buffer_size_bytes, stride);   break;
            case Type::UINT32:  endian_swap_buffer<uint32_t, uint32_t>(data_ptr, buffer_size_bytes, stride); break;
            case Type::FLOAT32: endian_swap_buffer<uint32_t, float>(data_ptr, buffer_size_bytes, stride);    break;
            case Type::FLOAT64: endian_swap_buffer<uint64_t, double>(data_ptr, buffer_size_bytes, stride);   break;
            default: break;
            }
        }
    }
}

IGL_INLINE void PlyFile::PlyFileImpl::write(std::ostream & os, bool _isBinary)
{
    for (auto & d : userData) { d.second.cursor->byteOffset = 0; }
    if (_isBinary)
    {
        isBinary = true;
        isBigEndian = false;
        write_binary_internal(os);
    }
    else
    {
        isBinary = false;
        isBigEndian = false;
        write_ascii_internal(os);
    }
}

IGL_INLINE void PlyFile::PlyFileImpl::write_binary_internal(std::ostream & os) noexcept
{
    isBinary = true;

    write_header(os);

    uint8_t listSize[4] = { 0, 0, 0, 0 };
    size_t dummyCount = 0;

    auto element_property_lookup = make_property_lookup_table();

    size_t element_idx = 0;
    for (auto & e : elements)
    {
        for (size_t i = 0; i < e.size; ++i)
        {
            size_t property_index = 0;
            for (auto & p : e.properties)
            {
                auto & f = element_property_lookup[element_idx][property_index];
                auto * helper = f.helper;
                if (f.skip || helper == nullptr) continue;

                if (p.isList)
                {
                    std::memcpy(listSize, &p.listCount, sizeof(uint32_t));
                    write_property_binary(os, listSize, dummyCount, f.list_stride);
                    write_property_binary(os, (helper->data->buffer.get() + helper->cursor->byteOffset), helper->cursor->byteOffset, f.prop_stride * p.listCount);
                }
                else
                {
                    write_property_binary(os, (helper->data->buffer.get() + helper->cursor->byteOffset), helper->cursor->byteOffset, f.prop_stride);
                }
                property_index++;
            }
        }
        element_idx++;
    }
}

IGL_INLINE void PlyFile::PlyFileImpl::write_ascii_internal(std::ostream & os) noexcept
{
    write_header(os);

    auto element_property_lookup = make_property_lookup_table();

    size_t element_idx = 0;
    for (auto & e : elements)
    {
        for (size_t i = 0; i < e.size; ++i)
        {
            size_t property_index = 0;
            for (auto & p : e.properties)
            {
                auto & f = element_property_lookup[element_idx][property_index];
                auto * helper = f.helper;
                if (f.skip || helper == nullptr) continue;

                if (p.isList)
                {
                    os << p.listCount << " ";
                    for (size_t j = 0; j < p.listCount; ++j)
                    {
                        write_property_ascii(p.propertyType, os, (helper->data->buffer.get() + helper->cursor->byteOffset), helper->cursor->byteOffset);
                    }
                }
                else
                {
                    write_property_ascii(p.propertyType, os, (helper->data->buffer.get() + helper->cursor->byteOffset), helper->cursor->byteOffset);
                }
                property_index++;
            }
            os << "\n";
        }
        element_idx++;
    }
}

IGL_INLINE void PlyFile::PlyFileImpl::write_header(std::ostream & os) noexcept
{
    const std::locale & fixLoc = std::locale("C");
    os.imbue(fixLoc);

    os << "ply\n";
    if (isBinary) os << ((isBigEndian) ? "format binary_big_endian 1.0" : "format binary_little_endian 1.0") << "\n";
    else os << "format ascii 1.0\n";

    for (const auto & comment : comments) os << "comment " << comment << "\n";

    auto property_lookup = make_property_lookup_table();

    size_t element_idx = 0;
    for (auto & e : elements)
    {
        os << "element " << e.name << " " << e.size << "\n";
        size_t property_idx = 0;
        for (const auto & p : e.properties)
        {
            PropertyLookup & lookup = property_lookup[element_idx][property_idx];

            if (!lookup.skip)
            {
                if (p.isList)
                {
                    os << "property list " << PropertyTable[p.listType].str << " "
                       << PropertyTable[p.propertyType].str << " " << p.name << "\n";
                }
                else
                {
                    os << "property " << PropertyTable[p.propertyType].str << " " << p.name << "\n";
                }
            }
            property_idx++;
        }
        element_idx++;
    }
    os << "end_header\n";
}

IGL_INLINE std::shared_ptr<PlyData> PlyFile::PlyFileImpl::request_properties_from_element(const std::string & elementKey,
    const std::vector<std::string> propertyKeys,
    const uint32_t list_size_hint)
{
    if (elements.empty()) throw std::runtime_error("header had no elements defined. malformed file?");
    if (elementKey.empty()) throw std::invalid_argument("`elementKey` argument is empty");
    if (propertyKeys.empty()) throw std::invalid_argument("`propertyKeys` argument is empty");

    std::shared_ptr<PlyData> out_data = std::make_shared<PlyData>();

    const int64_t elementIndex = find_element(elementKey, elements);

    std::vector<std::string> keys_not_found;

    // Sanity check if the user requested element is in the pre-parsed header
    if (elementIndex >= 0)
    {
        // We found the element
        const PlyElement & element = elements[elementIndex];

        // Each key in `propertyKey` gets an entry into the userData map (keyed by a hash of
        // element name and property name), but groups of properties (requested from the
        // public api through this function) all share the same `ParsingHelper`. When it comes
        // time to .read(), we check the number of unique PlyData shared pointers
        // and allocate a single buffer that will be used by each property key group.
        // That way, properties like, {"x", "y", "z"} will all be put into the same buffer.

        ParsingHelper helper;
        helper.data = out_data;
        helper.data->count = element.size; // how many items are in the element?
        helper.data->isList = false;
        helper.data->t = Type::INVALID;
        helper.cursor = std::make_shared<PlyDataCursor>();
        helper.list_size_hint = list_size_hint;

        // Find each of the keys
        for (const auto & key : propertyKeys)
        {
            const int64_t propertyIndex = find_property(key, element.properties);
            if (propertyIndex < 0) keys_not_found.push_back(key);
        }

        if (keys_not_found.size())
        {
            std::stringstream ss;
            for (auto & str : keys_not_found) ss << str << ", ";
            throw std::invalid_argument("the following property keys were not found in the header: " + ss.str());
        }

        for (const auto & key : propertyKeys)
        {
            const int64_t propertyIndex = find_property(key, element.properties);
            const PlyProperty & property = element.properties[propertyIndex];
            helper.data->t = property.propertyType;
            helper.data->isList = property.isList;
            auto result = userData.insert(std::pair<uint32_t, ParsingHelper>(hash_fnv1a(element.name + property.name), helper));
            if (result.second == false)
            {
                throw std::invalid_argument("element-property key has already been requested: " + element.name + " " + property.name);
            }
        }

        // Sanity check that all properties share the same type
        std::vector<Type> propertyTypes;
        for (const auto & key : propertyKeys)
        {
            const int64_t propertyIndex = find_property(key, element.properties);
            const PlyProperty & property = element.properties[propertyIndex];
            propertyTypes.push_back(property.propertyType);
        }

        if (std::adjacent_find(propertyTypes.begin(), propertyTypes.end(), std::not_equal_to<Type>()) != propertyTypes.end())
        {
            throw std::invalid_argument("all requested properties must share the same type.");
        }
    }
    else throw std::invalid_argument("the element key was not found in the header: " + elementKey);

    return out_data;
}

IGL_INLINE void PlyFile::PlyFileImpl::add_properties_to_element(const std::string & elementKey,
    const std::vector<std::string> propertyKeys,
    const Type type, const size_t count, uint8_t * data, const Type listType, const size_t listCount)
{
    ParsingHelper helper;
    helper.data = std::make_shared<PlyData>();
    helper.data->count = count;
    helper.data->t = type;
    helper.data->buffer = Buffer(data); // we should also set size for safety reasons
    helper.cursor = std::make_shared<PlyDataCursor>();

    auto create_property_on_element = [&](PlyElement & e)
    {
        for (auto key : propertyKeys)
        {
            PlyProperty newProp = (listType == Type::INVALID) ? PlyProperty(type, key) : PlyProperty(listType, type, key, listCount);
            userData.insert(std::pair<uint32_t, ParsingHelper>(hash_fnv1a(elementKey + key), helper));
            e.properties.push_back(newProp);
        }
    };

    const int64_t idx = find_element(elementKey, elements);
    if (idx >= 0)
    {
        PlyElement & e = elements[idx];
        create_property_on_element(e);
    }
    else
    {
        PlyElement newElement = (listType == Type::INVALID) ? PlyElement(elementKey, count) : PlyElement(elementKey, count);
        create_property_on_element(newElement);
        elements.push_back(newElement);
    }
}

IGL_INLINE void PlyFile::PlyFileImpl::parse_data(std::istream & is, bool firstPass)
{
    std::function<void(PropertyLookup & f, const PlyProperty & p, uint8_t * dest, size_t & destOffset, std::istream & is)> read;
    std::function<size_t(PropertyLookup & f, const PlyProperty & p, std::istream & is)> skip;

    const auto start = is.tellg();

    uint32_t listSize = 0;
    size_t dummyCount = 0;
    std::string skip_ascii_buffer;

    // Special case mirroring read_property_binary but for list types; this
    // has an additional big endian check to flip the data in place immediately
    // after reading. We do this as a performance optimization; endian flipping is
    // done on regular properties as a post-process after reading (also for optimization)
    // but we need the correct little-endian list count as we read the file.
    auto read_list_binary = [this](const Type & t, void * dst, size_t & destOffset, const size_t & stride, std::istream & _is) noexcept
    {
        destOffset += stride;
        _is.read((char*)dst, stride);

        if (isBigEndian)
        {
            switch (t)
            {
            case Type::INT16:  *(int16_t*)dst  = endian_swap<int16_t, int16_t>(*(int16_t*)dst);    break;
            case Type::UINT16: *(uint16_t*)dst = endian_swap<uint16_t, uint16_t>(*(uint16_t*)dst); break;
            case Type::INT32:  *(int32_t*)dst  = endian_swap<int32_t, int32_t>(*(int32_t*)dst);    break;
            case Type::UINT32: *(uint32_t*)dst = endian_swap<uint32_t, uint32_t>(*(uint32_t*)dst); break;
            default: break;
            }
        }

        return stride;
    };

    if (isBinary)
    {
        read = [this, &listSize, &dummyCount, &read_list_binary](PropertyLookup & f, const PlyProperty & p, uint8_t * dest, size_t & destOffset, std::istream & _is) noexcept
        {
            if (!p.isList)
            {
                return read_property_binary(f.prop_stride, dest + destOffset, destOffset, _is);
            }
            read_list_binary(p.listType, &listSize, dummyCount, f.list_stride, _is); // the list size
            return read_property_binary(f.prop_stride * listSize, dest + destOffset, destOffset, _is); // properties in list
        };
        skip = [this, &listSize, &dummyCount, &read_list_binary](PropertyLookup & f, const PlyProperty & p, std::istream & _is) noexcept
        {
            if (!p.isList)
            {
                _is.read((char*)scratch, f.prop_stride);
                return f.prop_stride;
            }
            read_list_binary(p.listType, &listSize, dummyCount, f.list_stride, _is); // the list size (does not count for memory alloc)
            auto bytes_to_skip = f.prop_stride * listSize;
            _is.ignore(bytes_to_skip);
            return bytes_to_skip;
        };
    }
    else
    {
        read = [this, &listSize, &dummyCount](PropertyLookup & f, const PlyProperty & p, uint8_t * dest, size_t & destOffset, std::istream & _is) noexcept
        {
            if (!p.isList)
            {
                read_property_ascii(p.propertyType, f.prop_stride, dest + destOffset, destOffset, _is);
            }
            else
            {
                read_property_ascii(p.listType, f.list_stride, &listSize, dummyCount, _is); // the list size
                for (size_t i = 0; i < listSize; ++i)
                {
                    read_property_ascii(p.propertyType, f.prop_stride, dest + destOffset, destOffset, _is);
                }
            }
        };
        skip = [this, &listSize, &dummyCount, &skip_ascii_buffer](PropertyLookup & f, const PlyProperty & p, std::istream & _is) noexcept
        {
            skip_ascii_buffer.clear();
            if (p.isList)
            {
                read_property_ascii(p.listType, f.list_stride, &listSize, dummyCount, _is); // the list size (does not count for memory alloc)
                for (size_t i = 0; i < listSize; ++i) _is >> skip_ascii_buffer; // properties in list
                return listSize * f.prop_stride;
            }
            _is >> skip_ascii_buffer;
            return f.prop_stride;
        };
    }

    std::vector<std::vector<PropertyLookup>> element_property_lookup = make_property_lookup_table();
    size_t element_idx = 0;
    size_t property_idx = 0;
    ParsingHelper * helper {nullptr};

    // This is the inner import loop
    for (auto & element : elements)
    {
        for (size_t count = 0; count < element.size; ++count)
        {
            property_idx = 0;
            for (auto & property : element.properties)
            {
                PropertyLookup & lookup = element_property_lookup[element_idx][property_idx];

                if (!lookup.skip)
                {
                    helper = lookup.helper;
                    if (firstPass)
                    {
                        helper->cursor->totalSizeBytes += skip(lookup, property, is);

                        // These lines will be changed when tinyply supports
                        // variable length lists. We add it here so our header data structure
                        // contains enough info to write it back out again (e.g. transcoding).
                        if (property.listCount == 0) property.listCount = listSize;
                        if (property.listCount != listSize) throw std::runtime_error("variable length lists are not supported yet.");
                    }
                    else
                    {
                        read(lookup, property, helper->data->buffer.get(), helper->cursor->byteOffset, is);
                    }
                }
                else
                {
                    skip(lookup, property, is);
                }
                property_idx++;
            }
        }
        element_idx++;
    }

    // Reset istream position to the start of the data
    if (firstPass) is.seekg(start, is.beg);
}

// Wrap the public interface:

IGL_INLINE PlyFile::PlyFile() { impl.reset(new PlyFileImpl()); }
IGL_INLINE PlyFile::~PlyFile() { }
IGL_INLINE bool PlyFile::parse_header(std::istream & is) { return impl->parse_header(is); }
IGL_INLINE void PlyFile::read(std::istream & is) { return impl->read(is); }
IGL_INLINE void PlyFile::write(std::ostream & os, bool isBinary) { return impl->write(os, isBinary); }
IGL_INLINE std::vector<PlyElement> PlyFile::get_elements() const { return impl->elements; }
IGL_INLINE std::vector<std::string> & PlyFile::get_comments() { return impl->comments; }
IGL_INLINE std::vector<std::string> PlyFile::get_info() const { return impl->objInfo; }
IGL_INLINE bool PlyFile::is_binary_file() const { return impl->isBinary; }
IGL_INLINE std::shared_ptr<PlyData> PlyFile::request_properties_from_element(const std::string & elementKey,
    const std::vector<std::string> propertyKeys,
    const uint32_t list_size_hint)
{
    return impl->request_properties_from_element(elementKey, propertyKeys, list_size_hint);
}
IGL_INLINE void PlyFile::add_properties_to_element(const std::string & elementKey,
    const std::vector<std::string> propertyKeys,
    const Type type, const size_t count, uint8_t * data, const Type listType, const size_t listCount)
{
    return impl->add_properties_to_element(elementKey, propertyKeys, type, count, data, listType, listCount);
}

} // tinyply
} // igl
