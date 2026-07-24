#ifndef SLIC3R_SPOOLMAN_HPP
#define SLIC3R_SPOOLMAN_HPP

#include "Http.hpp"
#include "WebSocketClient.hpp"
#include "libslic3r/libslic3r.h"
#include <boost/property_tree/ptree.hpp>
#include <map>
#include <optional>

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
    bool        has_failed() { return !messages.empty(); }
    std::string build_error_dialog_message()
    {
        if (!has_failed())
            return {};
        std::string message = messages.size() > 1 ? "Multiple errors:\n" : "Error:\n";

        for (const auto& error : messages) {
            message += error + "\n";
        }

        return message;
    }
    std::string build_single_line_message()
    {
        if (!has_failed())
            return {};
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
    bool m_server_url_changed{true};

    std::map<unsigned int, double> m_use_undo_buffer{};
    std::string                    m_last_usage_type{};

    std::map<unsigned int, SpoolmanVendorShrPtr>   m_vendors{};
    std::map<unsigned int, SpoolmanFilamentShrPtr> m_filaments{};
    std::map<unsigned int, SpoolmanSpoolShrPtr>    m_spools{};

    AsyncWebSocketClient websocket_client;

    Spoolman()
    {
        // Setup websocket handlers
        websocket_client.set_on_connect_fn([&](const beast::error_code& ec) {
            if (ec) {
                BOOST_LOG_TRIVIAL(error) << "Failed to connect to Spoolman websocket: " << ec.message();
                return;
            }
            BOOST_LOG_TRIVIAL(info) << "Websocket client connected to Spoolman server. Listening for changes...";
            websocket_client.async_receive();
        });
        websocket_client.set_on_receive_fn(
            [&](const std::string& message, const beast::error_code& ec, size_t) { this->on_websocket_receive(message, ec); });
        websocket_client.set_on_close_fn([&](const websocket::close_reason& reason, const bool client_requested_disconnect) {
            BOOST_LOG_TRIVIAL(info) << "Spoolman Websocket client closed. Reason: "
                                    << (client_requested_disconnect ? "Requested by client" :
                                        reason.reason.empty()       ? "Normal" :
                                                                      reason.reason);

            // The client only requests a disconnect when changing servers
            // Clearing the instance will be handled by the code closing the server
            if (!client_requested_disconnect)
                this->clear();
        });

        m_instance = this;
        if (is_server_valid())
            pull_spoolman_spools();
    };

    enum HTTPAction { GET, PUT, POST, PATCH };

    /// get an Http instance for the specified HTTPAction
    static Http get_http_instance(HTTPAction action, const std::string& url);

    /// uses the specified HTTPAction to make an API call to the spoolman server
    static pt::ptree spoolman_api_call(HTTPAction http_action, const std::string& api_endpoint, const pt::ptree& data = {});

    /// gets the json response from the specified API endpoint
    /// \returns the json response
    static pt::ptree get_spoolman_json(const std::string& api_endpoint) { return spoolman_api_call(GET, api_endpoint); }

    /// puts the provided data to the specified API endpoint
    /// \returns the json response
    static pt::ptree put_spoolman_json(const std::string& api_endpoint, const pt::ptree& data)
    { return spoolman_api_call(PUT, api_endpoint, data); }

    /// posts the provided data to the specified API endpoint
    /// \returns the json response
    static pt::ptree post_spoolman_json(const std::string& api_endpoint, const pt::ptree& data)
    { return spoolman_api_call(POST, api_endpoint, data); }

    /// patches the provided data to the specified API endpoint
    /// \returns the json response
    static pt::ptree patch_spoolman_json(const std::string& api_endpoint, const pt::ptree& data)
    { return spoolman_api_call(PATCH, api_endpoint, data); }

    /// Setup the websocket client and connect to Spoolman's general change pool
    void setup_websocket_connection();
    /// Called upon a websocket message
    /// Handles the change that was made and begins listening for the next change
    /// \param message The websocket message that was received from the server
    /// \param ec The error generated during receiving the message
    void on_websocket_receive(const std::string& message, beast::error_code ec);

    /// Called by pull_spoolman_spools if the URL has changed and is the first successful connection
    void on_server_first_connect();

    /// get all the spools from the api and store them
    /// \returns if succeeded
    bool pull_spoolman_spools();

    /// uses/consumes filament from the specified spool
    /// \param usage_type The consumption metric to be used. Should be "length" or "weight". This will NOT be checked.
    /// \returns if succeeded
    bool use_spoolman_spool(const unsigned int& spool_id, const double& usage, const std::string& usage_type);

public:
    static constexpr auto DEFAULT_PORT = "7912";

    /// uses/consumes filament from multiple specified spools
    /// \param data a map with the spool ID as the key and the amount to be consumed as the value
    /// \param usage_type The consumption metric to be used. Should be "length" or "weight". This will be checked.
    /// \returns if succeeded
    bool use_spoolman_spools(const std::map<unsigned int, double>& data, const std::string& usage_type);

    /// undo the previous use/consumption
    /// \returns if succeeded
    bool undo_use_spoolman_spools();

    /// Create a filament preset from a Spoolman filament
    /// \param filament Spoolman filament
    /// \param base_preset preset to inherit settings from
    /// \param use_preset_data if the filament has preset data, it will be used instead of using the base preset
    /// \param detach create preset without depending
    /// \param force attempt to force past errors
    static SpoolmanResult create_filament_preset(const SpoolmanFilamentShrPtr& filament,
                                                 const Preset*                 base_preset,
                                                 bool                          use_preset_data = false,
                                                 bool                          detach          = false,
                                                 bool                          force           = false);

    /// Update the preset's config options from the preset's spool/filament
    /// \param filament_preset preset to update
    /// \param only_update_statistics only update the statistics, not the rest of the config options
    /// \returns result
    static SpoolmanResult update_filament_preset(Preset* filament_preset, bool only_update_statistics = false);

    /// Save the preset data to the Spoolman database in an extras field
    /// \param filament_preset the preset to store
    /// \returns result
    static SpoolmanResult save_preset_to_spoolman(const Preset* filament_preset);

    /// Normalize the state of spoolman_filament_id and spoolman_spool_id
    /// \param config config to normalize
    /// \return if succeeded
    static bool normalize_spoolman_ids(DynamicPrintConfig& config);
    /// Normalize the Spoolman ids for all visible filament presets
    static void normalize_visible_spoolman_ids();

    /// Update the statistics values for the visible filament presets with spoolman enabled
    static void update_visible_spool_statistics();

    /// Update the statistics values for the filament presets tied to the specified spool ID
    static void update_specific_spool_statistics(unsigned spool_id);

    /// Should be called whenever the Spoolman URL has been changed
    void server_changed();

    /// Check if Spoolman is enabled and the provided host is valid
    static bool is_server_valid(bool force_check = false);

    /// Check if Spoolman is enabled
    static bool is_enabled();

    const std::map<unsigned int, SpoolmanSpoolShrPtr>& get_spoolman_spools()
    {
        if (!m_initialized)
            pull_spoolman_spools();
        return m_spools;
    }

    const std::map<unsigned int, SpoolmanFilamentShrPtr>& get_spoolman_filaments()
    {
        if (!m_initialized)
            pull_spoolman_spools();
        return m_filaments;
    }

    std::optional<SpoolmanSpoolShrPtr> get_spoolman_spool_by_id(unsigned int spool_id)
    {
        if (spool_id < 1)
            return std::nullopt;

        if (!m_initialized)
            pull_spoolman_spools();

        if (!contains(m_spools, spool_id))
            return std::nullopt;

        return m_spools.at(spool_id);
    }

    std::optional<SpoolmanFilamentShrPtr> get_spoolman_filament_by_id(unsigned int filament_id)
    {
        if (filament_id < 1)
            return std::nullopt;

        if (!m_initialized)
            pull_spoolman_spools();

        // Attempt to pull the filament from the server
        if (!contains(m_filaments, filament_id))
            return std::nullopt;

        return m_filaments.at(filament_id);
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

private:
    Spoolman* m_spoolman;

    SpoolmanVendor() : m_spoolman(Spoolman::m_instance) {}
    explicit SpoolmanVendor(const pt::ptree& json_data) : SpoolmanVendor() { update_from_json(json_data); };

    void update_from_json(const pt::ptree& json_data);
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

    bool get_config_from_preset_data(DynamicPrintConfig& config, std::map<std::string, std::string>* additional_values = nullptr) const;
    std::optional<SpoolmanSpoolShrPtr> get_most_used_spool() const;

    /// builds a preset name based on filament data
    std::string get_preset_name() const;

private:
    Spoolman* m_spoolman;

    SpoolmanFilament() : m_spoolman(Spoolman::m_instance) {}

    explicit SpoolmanFilament(const pt::ptree& json_data) : SpoolmanFilament() { update_from_json(json_data); };

    void update_from_json(const pt::ptree& json_data);
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

    void apply_to_config(DynamicConfig& config) const;
    void apply_to_preset(Preset* preset, bool only_update_statistics = false) const;

private:
    Spoolman* m_spoolman;

    SpoolmanSpool() : m_spoolman(Spoolman::m_instance) {}

    explicit SpoolmanSpool(const pt::ptree& json_data) : SpoolmanSpool() { update_from_json(json_data); }

    void update_from_json(const pt::ptree& json_data);

    friend class Spoolman;
};

} // namespace Slic3r
#endif // SLIC3R_SPOOLMAN_HPP
