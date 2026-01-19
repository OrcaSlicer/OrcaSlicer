#ifndef slic3r_Format_AMF_hpp_
#define slic3r_Format_AMF_hpp_

namespace Slic3r {

class Model;
class DynamicPrintConfig;

// Load the content of an amf file into the given model and configuration.
extern bool load_amf(const char* path, DynamicPrintConfig* config, ConfigSubstitutionContext* config_substitutions, Model* model, bool* use_inches);

//BBS: remove amf export
// Save the given model and the config data into an amf file.
// The model could be modified during the export process if meshes are not repaired or have no shared vertices
//extern bool store_amf(const char* path, Model* model, const DynamicPrintConfig* config, bool fullpath_sources);

} // namespace Slic3r

#endif /* slic3r_Format_AMF_hpp_ */
