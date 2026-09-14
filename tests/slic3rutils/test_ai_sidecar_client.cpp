#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/AISidecarClient.hpp"
#include "slic3r/GUI/AIServiceManager.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdlib>
#include <iomanip>
#include <sstream>

#include <openssl/evp.h>
#include <openssl/hmac.h>

using Slic3r::GUI::AISidecarClient;
using Slic3r::GUI::AIServiceAvailability;

namespace {

nlohmann::json health_response()
{
    return {
        {"ok", true}, {"protocol_version", 2}, {"sidecar_version", "orcaslicer-ai-sidecar-v9"},
        {"runtime", {{"health_schema_version", 2}, {"session_protected", true}, {"build", nlohmann::json::object()}}},
        {"capabilities", {
            {"config_proposal", {{"available", false}}},
            {"model_generation", {{"available", true}, {"sources", {"text", "image"}}, {"artifact_formats", {"glb", "obj"}}}}
        }}
    };
}

std::string hmac_hex(const std::string& key, const std::string& message)
{
    unsigned int size = 0;
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes {};
    REQUIRE(HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
                 reinterpret_cast<const unsigned char*>(message.data()), message.size(),
                 bytes.data(), &size) != nullptr);
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (unsigned index = 0; index < size; ++index)
        result << std::setw(2) << static_cast<unsigned>(bytes[index]);
    return result.str();
}

} // namespace

TEST_CASE("AI Sidecar discovery accepts supported GLB and legacy OBJ formats", "[AI][Sidecar][ServiceAvailability]")
{
    for (const auto& formats : {nlohmann::json::array({"glb", "obj"}), nlohmann::json::array({"obj", "glb"}),
                               nlohmann::json::array({"obj"}), nlohmann::json::array({"glb"})}) {
        DYNAMIC_SECTION(formats.dump()) {
            auto response = health_response();
            response["capabilities"]["model_generation"]["artifact_formats"] = formats;
            const auto result = AIServiceAvailability::from_health_response(response.dump(), true);
            REQUIRE(result.compatible);
            REQUIRE(result.model_generation_available);
            REQUIRE_FALSE(result.config_proposal_available);
            REQUIRE(result.error.empty());
        }
    }
}

TEST_CASE("AI Sidecar discovery rejects unusable artifact capabilities", "[AI][Sidecar][ServiceAvailability]")
{
    for (const auto& formats : {nlohmann::json(), nlohmann::json("glb"), nlohmann::json::array(),
                               nlohmann::json::array({1}), nlohmann::json::array({"stl"}),
                               nlohmann::json::array({"glb", "stl"})}) {
        DYNAMIC_SECTION(formats.dump()) {
            auto response = health_response();
            response["capabilities"]["model_generation"]["artifact_formats"] = formats;
            const auto result = AIServiceAvailability::from_health_response(response.dump(), true);
            REQUIRE_FALSE(result.compatible);
            REQUIRE_FALSE(result.model_generation_available);
            REQUIRE_FALSE(result.error.empty());
        }
    }
}

TEST_CASE("AI Sidecar discovery keeps GLB compatibility separate from provider availability", "[AI][Sidecar][ServiceAvailability]")
{
    auto response = health_response();
    response["capabilities"]["model_generation"]["available"] = false;
    const auto result = AIServiceAvailability::from_health_response(response.dump(), true);
    REQUIRE(result.compatible);
    REQUIRE_FALSE(result.model_generation_available);
    REQUIRE(result.error.empty());
}

TEST_CASE("AI Sidecar GLB discovery retains protocol and session validation", "[AI][Sidecar][ServiceAvailability][Security]")
{
    auto response = health_response();
    response["runtime"]["session_protected"] = false;
    REQUIRE_FALSE(AIServiceAvailability::from_health_response(response.dump(), true).compatible);
    REQUIRE(AIServiceAvailability::from_health_response(response.dump(), false).compatible);
    response["protocol_version"] = 1;
    REQUIRE_FALSE(AIServiceAvailability::from_health_response(response.dump(), false).compatible);
    response = health_response();
    response["capabilities"]["model_generation"].erase("artifact_formats");
    REQUIRE_FALSE(AIServiceAvailability::from_health_response(response.dump(), true).compatible);
    REQUIRE_FALSE(AIServiceAvailability::from_health_response("not-json", true).compatible);
}

TEST_CASE("AI Sidecar endpoints stay on loopback", "[AI][Sidecar]")
{
    REQUIRE(AISidecarClient::is_loopback_endpoint("http://127.0.0.1:18764"));
    REQUIRE(AISidecarClient::is_loopback_endpoint("http://localhost:18764/"));
    REQUIRE(AISidecarClient::is_loopback_endpoint("http://[::1]:18764"));
    REQUIRE_FALSE(AISidecarClient::is_loopback_endpoint("https://example.com/v1"));
    REQUIRE_FALSE(AISidecarClient::is_loopback_endpoint("not-a-url"));
}

TEST_CASE("AI Sidecar local session gets a process capability", "[AI][Sidecar][Security]")
{
    REQUIRE(AISidecarClient::initialize_local_session());
    REQUIRE(AISidecarClient::session_protection_enabled());
    const char* inherited = std::getenv("ORCASLICER_AI_SESSION_TOKEN");
    REQUIRE((inherited == nullptr || inherited[0] == '\0'));

    const std::string token = AISidecarClient::session_token_for_child();
    const std::string client_nonce = AISidecarClient::create_session_nonce();
    const std::string server_nonce(64, 'a');
    REQUIRE(token.size() == 64);
    REQUIRE(client_nonce.size() == 64);
    REQUIRE_FALSE(AISidecarClient::accept_session_challenge(client_nonce, server_nonce, std::string(64, '0')));
    REQUIRE(AISidecarClient::accept_session_challenge(
        client_nonce,
        server_nonce,
        hmac_hex(token, "server:" + client_nonce + ":" + server_nonce)));
}
