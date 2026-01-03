#ifndef SLIC3R_SPOOLMAN_HPP
#define SLIC3R_SPOOLMAN_HPP

#include "Http.hpp"
#include <boost/property_tree/ptree.hpp>
#include <map>

namespace pt = boost::property_tree;

namespace Slic3r {
class Preset;
class DynamicConfig;
class DynamicPrintConfig;

class SpoolmanVendor;
class SpoolmanFilament;
class SpoolmanSpool;

typedef std::shared_ptr<SpoolmanVendor>   SpoolmanVendorShrPtr;
typedef std::shared_ptr<SpoolmanFilament> SpoolmanFilamentShrPtr;
typedef std::shared_ptr<SpoolmanSpool>    SpoolmanSpoolShrPtr;

struct SpoolmanResult
{
    SpoolmanResult() = default;
    bool                     has_failed() { return !messages.empty(); }
    std::string build_error_dialog_message() {
        if (!has_failed()) return {};
        std::string message = messages.size() > 1 ? "Multiple errors:\n" : "Error:\n";

        for (const auto& error : messages) {
            message += error + "\n";
        }

        return message;
    }
    std::string  build_single_line_message() {
        if (!has_failed()) return {};
        std::string message = messages.size() > 1 ? "Multiple errors: " : "Error: ";

        for (const auto& error : messages) {
            message += error + ". ";
        }

        return message;
    }
    std::vector<std::string> messages{};
};

/// Contains routines to get the data from the Spoolman server, save as Spoolman data containers, and create presets from them.
/// The Spoolman data classes can only be accessed/instantiated by this class.
/// An instance of this class can only be accessed via the get_instance() function.
class Spoolman
{
    inline static Spoolman* m_instance{nullptr};


    bool m_initialized{false};

    std::map<unsigned int, double> m_use_undo_buffer{};
    std::string                    m_last_usage_type{};

    std::map<unsigned int, SpoolmanVendorShrPtr>   m_vendors{};
    std::map<unsigned int, SpoolmanFilamentShrPtr> m_filaments{};
    std::map<unsigned int, SpoolmanSpoolShrPtr>    m_spools{};

    Spoolman()
    {
        m_instance    = this;
        if (is_server_valid()) {
            on_server_changed();
            m_initialized = pull_spoolman_spools();
        }
    };

    enum HTTPAction
    {
        GET, PUT, POST, PATCH
    };

    /// get an Http instance for the specified HTTPAction
    static Http get_http_instance(HTTPAction action, const std::string& url);

    /// uses the specified HTTPAction to make an API call to the spoolman server
    static pt::ptree spoolman_api_call(HTTPAction http_action, const std::string& api_endpoint, const pt::ptree& data = {});

    /// gets the json response from the specified API endpoint
    /// \returns the json response
    static pt::ptree get_spoolman_json(const std::string& api_endpoint) { return spoolman_api_call(GET, api_endpoint); }

    /// puts the provided data to the specified API endpoint
    /// \returns the json response
    static pt::ptree put_spoolman_json(const std::string& api_endpoint, const pt::ptree& data) { return spoolman_api_call(PUT, api_endpoint, data); }

    /// posts the provided data to the specified API endpoint
    /// \returns the json response
    static pt::ptree post_spoolman_json(const std::string& api_endpoint, const pt::ptree& data) { return spoolman_api_call(POST, api_endpoint, data); }

    /// patches the provided data to the specified API endpoint
    /// \returns the json response
    static pt::ptree patch_spoolman_json(const std::string& api_endpoint, const pt::ptree& data) { return spoolman_api_call(PATCH, api_endpoint, data); }

    /// get all the spools from the api and store them
    /// \returns if succeeded
    bool pull_spoolman_spools();

    /// uses/consumes filament from the specified spool then updates the spool
    /// \param usage_type The consumption metric to be used. Should be "length" or "weight". This will NOT be checked.
    /// \returns if succeeded
    bool use_spoolman_spool(const unsigned int& spool_id, const double& usage, const std::string& usage_type);
public:
    static constexpr auto DEFAULT_PORT = "7912";

    /// uses/consumes filament from multiple specified spools then updates them
    /// \param data a map with the spool ID as the key and the amount to be consumed as the value
    /// \param usage_type The consumption metric to be used. Should be "length" or "weight". This will be checked.
    /// \returns if succeeded
    bool use_spoolman_spools(const std::map<unsigned int, double>& data, const std::string& usage_type);

    /// undo the previous use/consumption
    /// \returns if succeeded
    bool undo_use_spoolman_spools();

    /// Create a filament preset from a Spoolman spool
    /// \param spool Spoolman spool
    /// \param base_preset preset to inherit settings from
    /// \param use_preset_data if the spool has preset data, the SpoolmanFilament's preset_data will be used instead of using the base preset
    /// \param detach create profile without depending
    /// \param force attempt to force past errors
    static SpoolmanResult create_filament_preset_from_spool(const SpoolmanSpoolShrPtr& spool,
                                                            const Preset*              base_preset,
                                                            bool                       use_preset_data = false,
                                                            bool                       detach = false,
                                                            bool                       force = false);
    static SpoolmanResult update_filament_preset_from_spool(Preset* filament_preset,
                                                            bool    update_from_server     = true,
                                                            bool    only_update_statistics = false);

    static SpoolmanResult save_preset_to_spoolman(const Preset* filament_preset);

    /// Update the statistics values for the visible filament profiles with spoolman enabled
    /// clear_cache should be set true if the update is due to a change in printer profile or other change that requires it
    static void update_visible_spool_statistics(bool clear_cache = false);

    /// Update the statistics values for the filament profiles tied to the specified spool IDs
    static void update_specific_spool_statistics(const std::vector<unsigned int>& spool_ids);

    static void on_server_changed();

    /// Check if Spoolman is enabled and the provided host is valid
    static bool is_server_valid(bool force_check = false);

    /// Check if Spoolman is enabled
    static bool is_enabled();

    const std::map<unsigned int, SpoolmanSpoolShrPtr>& get_spoolman_spools(bool update = false)
    {
        if (update || !m_initialized)
            m_initialized = pull_spoolman_spools();
        return m_spools;
    }

    SpoolmanSpoolShrPtr get_spoolman_spool_by_id(unsigned int spool_id, bool update = false)
    {
        if (update || !m_initialized)
            m_initialized = pull_spoolman_spools();
        return m_spools[spool_id];
    }

    void clear()
    {
        m_spools.clear();
        m_filaments.clear();
        m_vendors.clear();
        m_initialized = false;
    }

    static Spoolman* get_instance()
    {
        if (!m_instance)
            new Spoolman();
        return m_instance;
    }

    friend class SpoolmanVendor;
    friend class SpoolmanFilament;
    friend class SpoolmanSpool;
};

/// Vendor: The vendor name
class SpoolmanVendor
{
public:
    int         id;
    std::string name;
    std::string comment;

    void update_from_server();

private:
    Spoolman* m_spoolman;

    explicit SpoolmanVendor(const pt::ptree& json_data) : m_spoolman(Spoolman::m_instance) { update_from_json(json_data); };

    void update_from_json(pt::ptree json_data);
    void apply_to_config(Slic3r::DynamicConfig& config) const;

    friend class Spoolman;
    friend class SpoolmanFilament;
    friend class SpoolmanSpool;
};

/// Filament: Contains data about a type of filament, including the material, weight, price,
/// etc. You can have multiple spools of one type of filament
class SpoolmanFilament
{
public:
    int         id;
    std::string name;
    std::string material;
    double      price;
    double      density;
    double      diameter;
    double      weight;
    std::string article_number;
    int         extruder_temp;
    int         bed_temp;
    std::string color;
    std::string preset_data;
    std::string comment;

    // Can be nullptr
    SpoolmanVendorShrPtr vendor;

    void update_from_server(bool recursive = false);
    bool get_config_from_preset_data(DynamicPrintConfig& config, std::map<std::string, std::string>* additional_values = nullptr) const;

private:
    Spoolman* m_spoolman;

    explicit SpoolmanFilament(const pt::ptree& json_data) : m_spoolman(Spoolman::m_instance)
    {
        if (const auto vendor_id = json_data.get_optional<int>("vendor.id"); vendor_id.has_value())
            vendor = m_spoolman->m_vendors[vendor_id.value()];
        update_from_json(json_data);
    };

    void update_from_json(pt::ptree json_data);
    void apply_to_config(DynamicConfig& config) const;

    friend class Spoolman;
    friend class SpoolmanVendor;
    friend class SpoolmanSpool;
};

/// Spool: Contains data on the used and remaining amounts of filament
class SpoolmanSpool
{
public:
    int         id;
    std::string comment;
    double      remaining_weight;
    double      used_weight;
    double      remaining_length;
    double      used_length;
    bool        archived;

    SpoolmanFilamentShrPtr filament;

    // Can be nullptr
    SpoolmanVendorShrPtr& get_vendor() { return filament->vendor; }

    void update_from_server(bool recursive = false);

    /// builds a preset name based on spool data
    std::string get_preset_name();

    void apply_to_config(DynamicConfig& config) const;
    void apply_to_preset(Preset* preset, bool only_update_statistics = false) const;

private:
    Spoolman* m_spoolman;

    explicit SpoolmanSpool(const pt::ptree& json_data) : m_spoolman(Spoolman::m_instance)
    {
        filament = m_spoolman->m_filaments[json_data.get<int>("filament.id")];
        update_from_json(json_data);
    }

    void update_from_json(pt::ptree json_data);

    friend class Spoolman;
};

} // namespace Slic3r
#endif // SLIC3R_SPOOLMAN_HPP
