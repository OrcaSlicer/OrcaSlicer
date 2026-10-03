#include "AssembleList.hpp"

#include <algorithm>

#include <boost/filesystem.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <string>
#include <vector>
#include <map>
#include <utility>
#include <exception>

#include "nlohmann/json.hpp"

#define JSON_ASSEMPLE_PLATES                   "plates"
#define JSON_ASSEMPLE_PLATE_PARAMS             "plate_params"
#define JSON_ASSEMPLE_PLATE_NAME               "plate_name"
#define JSON_ASSEMPLE_PLATE_NEED_ARRANGE       "need_arrange"
#define JSON_ASSEMPLE_OBJECTS                  "objects"
#define JSON_ASSEMPLE_OBJECT_PATH              "path"
#define JSON_ASSEMPLE_OBJECT_COUNT             "count"
#define JSON_ASSEMPLE_OBJECT_FILAMENTS         "filaments"
#define JSON_ASSEMPLE_OBJECT_POS_X             "pos_x"
#define JSON_ASSEMPLE_OBJECT_POS_Y             "pos_y"
#define JSON_ASSEMPLE_OBJECT_POS_Z             "pos_z"
#define JSON_ASSEMPLE_OBJECT_ASSEMBLE_INDEX    "assemble_index"
#define JSON_ASSEMPLE_OBJECT_PRINT_PARAMS      "print_params"
#define JSON_ASSEMPLE_ASSEMBLE_PARAMS         "assembled_params"


#define JSON_ASSEMPLE_OBJECT_MIN_Z              "min_z"
#define JSON_ASSEMPLE_OBJECT_MAX_Z              "max_z"
#define JSON_ASSEMPLE_OBJECT_HEIGHT_RANGES      "height_ranges"
#define JSON_ASSEMPLE_OBJECT_RANGE_PARAMS       "range_params"

namespace Slic3r {

using json = nlohmann::json;

AssembleListResult load_assemble_plate_list(const std::string &config_file, std::vector<assemble_plate_info_t> &assemble_plate_info_list, int max_plate_count)
{
    AssembleListResult ret = AssembleListResult::Success;
    boost::filesystem::path directory_path(config_file);

    BOOST_LOG_TRIVIAL(info) << boost::format("%1% enter, file %2%")%__FUNCTION__ % config_file;
    if (!boost::filesystem::exists(directory_path)) {
        BOOST_LOG_TRIVIAL(error) << boost::format("directory %1% not exist.")%config_file;
        return AssembleListResult::FileNotFound;
    }

    try {
        json root_json;
        boost::nowide::ifstream ifs(config_file);
        ifs >> root_json;
        ifs.close();

        int plate_count = root_json[JSON_ASSEMPLE_PLATES].size();
        if ((plate_count <= 0) || (plate_count > max_plate_count)) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< boost::format(": invalid plate count %1%")%plate_count;
            return AssembleListResult::ConfigError;
        }
        assemble_plate_info_list.resize(plate_count);

        for (int plate_index = 0; plate_index < plate_count; plate_index++)
        {
            assemble_plate_info_t &assemble_plate = assemble_plate_info_list[plate_index];
            const json& plate_json = root_json[JSON_ASSEMPLE_PLATES][plate_index];
            assemble_plate.plate_name = plate_json[JSON_ASSEMPLE_PLATE_NAME];
            assemble_plate.need_arrange = plate_json[JSON_ASSEMPLE_PLATE_NEED_ARRANGE];

            if (plate_json.contains(JSON_ASSEMPLE_PLATE_PARAMS)) {
                assemble_plate.plate_params = plate_json[JSON_ASSEMPLE_PLATE_PARAMS].get<std::map<std::string, std::string>>();
                BOOST_LOG_TRIVIAL(debug) << boost::format("Plate %1%, has %2% plate params") % (plate_index + 1)  % assemble_plate.plate_params.size();
            }

            int object_count = plate_json[JSON_ASSEMPLE_OBJECTS].size();
            if (object_count <= 0) {
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< boost::format(": invalid object count %1% in plate %2%")%object_count %(plate_index+1);
                return AssembleListResult::ConfigError;
            }

            assemble_plate.assemble_obj_list.resize(object_count);
            for (int object_index = 0; object_index < object_count; object_index++)
            {
                assemble_object_info_t& assemble_object = assemble_plate.assemble_obj_list[object_index];
                const json& object_json = plate_json[JSON_ASSEMPLE_OBJECTS][object_index];

                assemble_object.path = object_json[JSON_ASSEMPLE_OBJECT_PATH];
                assemble_object.count = object_json[JSON_ASSEMPLE_OBJECT_COUNT];

                if (assemble_object.count <= 0) {
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": invalid object clone count %1% in plate %2% Object %3%") % assemble_object.count % (plate_index + 1) % assemble_object.path;
                    return AssembleListResult::ConfigError;
                }

                assemble_object.filaments = object_json.at(JSON_ASSEMPLE_OBJECT_FILAMENTS).get<std::vector<int>>();
                if (assemble_object.filaments.empty())
                {
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": object %1%'s filaments list is empty") % assemble_object.path;
                    return AssembleListResult::ConfigError;
                }
                if ((assemble_object.filaments.size() != assemble_object.count) && (assemble_object.filaments.size() != 1))
                {
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": object %1%'s filaments count %2% not equal to clone count %3%, also not equal to 1") % assemble_object.path % assemble_object.filaments.size() % assemble_object.count;
                    return AssembleListResult::ConfigError;
                }
                // 0 keeps the default filament, as it does for --load-filament-ids.
                if (std::any_of(assemble_object.filaments.begin(), assemble_object.filaments.end(), [](int id) { return id < 0; }))
                {
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": object %1% has a negative filament id") % assemble_object.path;
                    return AssembleListResult::ConfigError;
                }

                if (object_json.contains(JSON_ASSEMPLE_OBJECT_ASSEMBLE_INDEX)) {
                    assemble_object.assemble_index = object_json[JSON_ASSEMPLE_OBJECT_ASSEMBLE_INDEX].get<std::vector<int>>();
                    if ((assemble_object.assemble_index.size() > 0) && (assemble_object.assemble_index.size() != assemble_object.count) && (assemble_object.assemble_index.size() != 1))
                    {
                        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": object %1%'s assemble_index count %2% not equal to clone count %3%, also not equal to 1") % assemble_object.path % assemble_object.assemble_index.size() % assemble_object.count;
                        return AssembleListResult::ConfigError;
                    }
                }

                if (object_json.contains(JSON_ASSEMPLE_OBJECT_POS_X)) {
                    assemble_object.pos_x = object_json[JSON_ASSEMPLE_OBJECT_POS_X].get<std::vector<float>>();
                    if ((assemble_object.pos_x.size() > 0) && (assemble_object.pos_x.size() != assemble_object.count) && (assemble_object.pos_x.size() != 1))
                    {
                        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": object %1%'s pos_x count %2% not equal to clone count %3%, also not equal to 1") % assemble_object.path % assemble_object.pos_x.size() % assemble_object.count;
                        return AssembleListResult::ConfigError;
                    }
                }
                if (object_json.contains(JSON_ASSEMPLE_OBJECT_POS_Y)) {
                    assemble_object.pos_y = object_json[JSON_ASSEMPLE_OBJECT_POS_Y].get<std::vector<float>>();
                    if ((assemble_object.pos_y.size() > 0) && (assemble_object.pos_y.size() != assemble_object.count) && (assemble_object.pos_y.size() != 1))
                    {
                        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": object %1%'s pos_y count %2% not equal to clone count %3%, also not equal to 1") % assemble_object.path % assemble_object.pos_y.size() % assemble_object.count;
                        return AssembleListResult::ConfigError;
                    }
                }
                if (object_json.contains(JSON_ASSEMPLE_OBJECT_POS_Z)) {
                    assemble_object.pos_z = object_json[JSON_ASSEMPLE_OBJECT_POS_Z].get<std::vector<float>>();
                    if ((assemble_object.pos_z.size() > 0) && (assemble_object.pos_z.size() != assemble_object.count) && (assemble_object.pos_z.size() != 1))
                    {
                        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(": object %1%'s pos_z count %2% not equal to clone count %3%, also not equal to 1") % assemble_object.path % assemble_object.pos_z.size() % assemble_object.count;
                        return AssembleListResult::ConfigError;
                    }
                }
                if (object_json.contains(JSON_ASSEMPLE_OBJECT_PRINT_PARAMS)) {
                    assemble_object.print_params = object_json[JSON_ASSEMPLE_OBJECT_PRINT_PARAMS].get<std::map<std::string, std::string>>();
                    BOOST_LOG_TRIVIAL(debug) << boost::format("Plate %1%, object %2% has %3% print params") % (plate_index + 1) %assemble_object.path % assemble_object.print_params.size();
                }
                if (object_json.contains(JSON_ASSEMPLE_OBJECT_HEIGHT_RANGES)) {
                    json height_range_json = object_json[JSON_ASSEMPLE_OBJECT_HEIGHT_RANGES];
                    int range_count = height_range_json.size();

                    BOOST_LOG_TRIVIAL(debug) << boost::format("Plate %1%, object %2% has %3% height ranges") % (plate_index + 1) %assemble_object.path % range_count;

                    assemble_object.height_ranges.resize(range_count);
                    for (int range_index = 0; range_index < range_count; range_index++)
                    {
                        height_range_info_t& height_range = assemble_object.height_ranges[range_index];
                        height_range.min_z = height_range_json[range_index][JSON_ASSEMPLE_OBJECT_MIN_Z];
                        height_range.max_z = height_range_json[range_index][JSON_ASSEMPLE_OBJECT_MAX_Z];
                        height_range.range_params = height_range_json[range_index][JSON_ASSEMPLE_OBJECT_RANGE_PARAMS].get<std::map<std::string, std::string>>();
                    }
                }
            }
            if (plate_json.contains(JSON_ASSEMPLE_ASSEMBLE_PARAMS)) {
                json assemble_params_json = plate_json[JSON_ASSEMPLE_ASSEMBLE_PARAMS];
                int assemble_count = assemble_params_json.size();
                for (int i = 0; i < assemble_count; i++)
                {
                    assembled_param_info_t assembled_param;
                    int assemble_index = assemble_params_json[i][JSON_ASSEMPLE_OBJECT_ASSEMBLE_INDEX];
                    if (assemble_params_json[i].contains(JSON_ASSEMPLE_OBJECT_PRINT_PARAMS)) {
                        assembled_param.print_params = assemble_params_json[i][JSON_ASSEMPLE_OBJECT_PRINT_PARAMS].get<std::map<std::string, std::string>>();
                        BOOST_LOG_TRIVIAL(debug) << boost::format("Plate %1%, assemble object %2% has %3% print params") % (plate_index + 1) %i % assembled_param.print_params.size();
                    }
                    if (assemble_params_json[i].contains(JSON_ASSEMPLE_OBJECT_HEIGHT_RANGES)) {
                        json height_range_json = assemble_params_json[i][JSON_ASSEMPLE_OBJECT_HEIGHT_RANGES];
                        int range_count = height_range_json.size();

                        BOOST_LOG_TRIVIAL(debug) << boost::format("Plate %1%, assemble object %2% has %3% height ranges") % (plate_index + 1) %i % range_count;

                        assembled_param.height_ranges.resize(range_count);
                        for (int range_index = 0; range_index < range_count; range_index++)
                        {
                            height_range_info_t& height_range = assembled_param.height_ranges[range_index];
                            height_range.min_z = height_range_json[range_index][JSON_ASSEMPLE_OBJECT_MIN_Z];
                            height_range.max_z = height_range_json[range_index][JSON_ASSEMPLE_OBJECT_MAX_Z];
                            height_range.range_params = height_range_json[range_index][JSON_ASSEMPLE_OBJECT_RANGE_PARAMS].get<std::map<std::string, std::string>>();
                        }
                    }
                    assemble_plate.assembled_param_list.emplace(assemble_index, std::move(assembled_param));
                }
                BOOST_LOG_TRIVIAL(debug) << boost::format("Plate %1%, has %2% plate params") % (plate_index + 1)  % assemble_plate.plate_params.size();
            }
        }
    }
    catch(std::exception &err) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__<< ": parse file "<<config_file<<" got a generic exception, reason = " << err.what();
        ret = AssembleListResult::ConfigError;
    }

    return ret;
}

} // namespace Slic3r
