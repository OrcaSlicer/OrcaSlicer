#ifndef slic3r_Format_3mf_hpp_
#define slic3r_Format_3mf_hpp_
#include <expat.h>

namespace Slic3r {
// PrusaFileParser is used to check 3mf file is from Prusa
class PrusaFileParser
{
public:
    PrusaFileParser() {}
    ~PrusaFileParser() {}

    bool check_3mf_from_prusa(const std::string filename);
    void _start_element_handler(const char *name, const char **attributes);
    void _characters_handler(const XML_Char *s, int len);

private:
    const char *get_attribute_value_charptr(const char **attributes, unsigned int attributes_size, const char *attribute_key);
    std::string get_attribute_value_string(const char **attributes, unsigned int attributes_size, const char *attribute_key);

    static void XMLCALL start_element_handler(void *userData, const char *name, const char **attributes);
    static void XMLCALL characters_handler(void *userData, const XML_Char *s, int len);
private:
    bool       m_from_prusa         = false;
    bool       m_is_application_key = false;
    XML_Parser m_parser;
};

    /* The format for saving the SLA points was changing in the past. This enum holds the latest version that is being currently used.
     * Examples of the Slic3r_PE_sla_support_points.txt for historically used versions:

     *  version 0 : object_id=1|-12.055421 -2.658771 10.000000
                    object_id=2|-14.051745 -3.570338 5.000000
        // no header and x,y,z positions of the points)

     * version 1 :  ThreeMF_support_points_version=1
                    object_id=1|-12.055421 -2.658771 10.000000 0.4 0.0
                    object_id=2|-14.051745 -3.570338 5.000000 0.6 1.0
        // introduced header with version number; x,y,z,head_size,is_new_island)
    */

    enum {
        support_points_format_version = 1
    };
    
    enum {
        drain_holes_format_version = 1
    };

    class Model;
    struct ConfigSubstitutionContext;
    class DynamicPrintConfig;
    struct ThumbnailData;

    // Load the content of a 3mf file into the given model and preset bundle.
    extern bool load_3mf(const char* path, DynamicPrintConfig& config, ConfigSubstitutionContext& config_substitutions, Model* model, bool check_version);

    // Save the given model and the config data contained in the given Print into a 3mf file.
    // The model could be modified during the export process if meshes are not repaired or have no shared vertices
    extern bool store_3mf(const char* path, Model* model, const DynamicPrintConfig* config, bool fullpath_sources, const ThumbnailData* thumbnail_data = nullptr, bool zip64 = true);

} // namespace Slic3r

#endif /* slic3r_Format_3mf_hpp_ */
