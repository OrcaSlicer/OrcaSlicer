#include "ProfileTranslator.hpp"
#include "nlohmann/json.hpp"

#include <boost/nowide/fstream.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

using json = nlohmann::json;
namespace fs  = boost::filesystem;

namespace Slic3r {

ProfileTranslator& ProfileTranslator::instance()
{
    static ProfileTranslator s_instance;
    return s_instance;
}

void ProfileTranslator::clear()
{
    m_translations.clear();
    m_current_language.clear();
}

const std::string& ProfileTranslator::translate(const std::string& source) const
{
    auto it = m_translations.find(source);
    if (it != m_translations.end())
        return it->second;
    return source;
}

bool ProfileTranslator::try_load_file(const std::string& filepath)
{
    if (!fs::exists(filepath))
        return false;

    try {
        boost::nowide::ifstream ifs(filepath);
        json j;
        ifs >> j;

        if (!j.is_array()) {
            BOOST_LOG_TRIVIAL(warning) << "ProfileTranslator: expected JSON array in " << filepath;
            return false;
        }

        for (const auto& entry : j) {
            if (entry.is_object() &&
                entry.contains("source") && entry["source"].is_string() &&
                entry.contains("translation") && entry["translation"].is_string()) {
                std::string src = entry["source"].get<std::string>();
                std::string trl = entry["translation"].get<std::string>();
                if (!src.empty() && !trl.empty())
                    m_translations[std::move(src)] = std::move(trl);
            }
        }

        BOOST_LOG_TRIVIAL(info) << "ProfileTranslator: loaded translations from " << filepath;
        return true;
    }
    catch (const std::exception& ex) {
        BOOST_LOG_TRIVIAL(error) << "ProfileTranslator: failed to parse " << filepath << ": " << ex.what();
        return false;
    }
}

void ProfileTranslator::load_vendor_translations(const std::string& vendor_dir, const std::string& language_code)
{
    std::string i18n_dir = vendor_dir + "/I18N";
    if (!fs::exists(i18n_dir) || !fs::is_directory(i18n_dir))
        return;

    // Try exact match first (e.g. "fr_FR.json")
    std::string exact_file = i18n_dir + "/" + language_code + ".json";
    if (try_load_file(exact_file))
        return;

    // Fallback: try base language code (e.g. "fr.json" from "fr_FR")
    auto underscore_pos = language_code.find('_');
    if (underscore_pos != std::string::npos) {
        std::string base_lang = language_code.substr(0, underscore_pos);
        std::string base_file = i18n_dir + "/" + base_lang + ".json";
        try_load_file(base_file);
    }
}

void ProfileTranslator::load_translations(const std::string& profiles_dir, const std::string& language_code)
{
    m_current_language = language_code;

    if (language_code.empty())
        return;

    if (!fs::exists(profiles_dir) || !fs::is_directory(profiles_dir))
        return;

    for (auto& dir_entry : fs::directory_iterator(profiles_dir)) {
        if (!fs::is_directory(dir_entry.path()))
            continue;
        load_vendor_translations(dir_entry.path().string(), language_code);
    }

    BOOST_LOG_TRIVIAL(info) << "ProfileTranslator: total " << m_translations.size()
                            << " translations loaded for language '" << language_code << "'";
}

} // namespace Slic3r
