#include <catch2/catch_all.hpp>

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#include <memory>
#include <string>

#include "slic3r/Utils/OrcaCloudServiceAgent.hpp"
#include "test_utils.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

// The encrypted token file is the one secret backend a test can observe without a system
// keychain. Every agent pointed at the same directory shares it, like separate app instances
// share the keychain entry.
std::unique_ptr<OrcaCloudServiceAgent> make_file_backed_agent(const fs::path& dir)
{
    auto agent = std::make_unique<OrcaCloudServiceAgent>(dir.string());
    agent->set_use_encrypted_token_file(true);
    agent->set_config_dir(dir.string());
    return agent;
}

fs::path secret_file(const fs::path& dir) { return dir / secret_constants::USER_SECRET_FILENAME; }

nlohmann::json flat_session_json(const nlohmann::json& fields)
{
    nlohmann::json session = {
        {"access_token", "test-token"},
        {"user_id", "test-user-id"}
    };
    session.update(fields);
    return session;
}

nlohmann::json nested_session_json(const nlohmann::json& metadata)
{
    return {
        {"access_token", "test-token"},
        {"user", {
            {"id", "test-user-id"},
            {"user_metadata", metadata}
        }}
    };
}

// set_user_session() persists the session, so it goes to a throwaway token file rather than the
// system keychain of whoever runs the tests.
std::string resolved_display_name(const nlohmann::json& session)
{
    ScopedTemporaryDir dir("orca-secret");
    auto               agent = make_file_backed_agent(dir.path());
    REQUIRE(agent->set_user_session(session, false));
    return agent->get_user_nickname();
}

} // namespace

TEST_CASE("Logging out removes the secret this instance saved", "[OrcaCloudServiceAgent]")
{
    ScopedTemporaryDir dir("orca-secret");
    auto agent = make_file_backed_agent(dir.path());

    agent->persist_user_secret("refresh-token");
    REQUIRE(fs::exists(secret_file(dir.path())));

    agent->user_logout(false);
    CHECK_FALSE(fs::exists(secret_file(dir.path())));
}

TEST_CASE("Logging out removes a secret this instance loaded from the store", "[OrcaCloudServiceAgent]")
{
    ScopedTemporaryDir dir("orca-secret");
    make_file_backed_agent(dir.path())->persist_user_secret("refresh-token");

    auto        agent = make_file_backed_agent(dir.path());
    std::string secret;
    REQUIRE(agent->load_user_secret(secret));
    CHECK(secret == "refresh-token");

    agent->user_logout(false);
    CHECK_FALSE(fs::exists(secret_file(dir.path())));
}

TEST_CASE("Logging out leaves a secret this instance never loaded or saved alone", "[OrcaCloudServiceAgent]")
{
    ScopedTemporaryDir dir("orca-secret");
    make_file_backed_agent(dir.path())->persist_user_secret("refresh-token");

    // A logged-out instance is asked to log out on every login-status poll.
    auto other = make_file_backed_agent(dir.path());
    other->user_logout(false);
    other->user_logout(false);
    CHECK(fs::exists(secret_file(dir.path())));

    std::string secret;
    REQUIRE(make_file_backed_agent(dir.path())->load_user_secret(secret));
    CHECK(secret == "refresh-token");
}

TEST_CASE("Logging out leaves a secret this instance could not read alone", "[OrcaCloudServiceAgent]")
{
    ScopedTemporaryDir dir("orca-secret");
    // Written under another encryption key, e.g. by another OS user sharing the data directory.
    fs::ofstream(secret_file(dir.path())) << "v2:0000:not-a-payload-this-user-can-decrypt";

    auto        agent = make_file_backed_agent(dir.path());
    std::string secret;
    REQUIRE_FALSE(agent->load_user_secret(secret));

    agent->user_logout(false);
    CHECK(fs::exists(secret_file(dir.path())));
}

TEST_CASE("Orca cloud flat session resolves display name consistently", "[OrcaCloudServiceAgent]")
{
    CHECK(resolved_display_name(flat_session_json({
        {"username", "orca_username"},
        {"display_name", "Display Name"},
        {"nickname", "Nickname"}
    })) == "Display Name");

    CHECK(resolved_display_name(flat_session_json({
        {"username", "orca_username"},
        {"nickname", "Nickname"}
    })) == "Nickname");

    CHECK(resolved_display_name(flat_session_json({
        {"username", "orca_username"},
        {"full_name", "Full Name"}
    })) == "Full Name");

    CHECK(resolved_display_name(flat_session_json({
        {"username", "orca_username"},
        {"name", "Provider Name"}
    })) == "Provider Name");

    CHECK(resolved_display_name(flat_session_json({
        {"username", "orca_username"}
    })) == "orca_username");
}

TEST_CASE("Orca cloud nested session resolves display name consistently", "[OrcaCloudServiceAgent]")
{
    CHECK(resolved_display_name(nested_session_json({
        {"username", "orca_username"},
        {"display_name", "Display Name"},
        {"nickname", "Nickname"}
    })) == "Display Name");

    CHECK(resolved_display_name(nested_session_json({
        {"username", "orca_username"},
        {"nickname", "Nickname"}
    })) == "Nickname");

    CHECK(resolved_display_name(nested_session_json({
        {"username", "orca_username"},
        {"full_name", "Full Name"}
    })) == "Full Name");

    CHECK(resolved_display_name(nested_session_json({
        {"username", "orca_username"},
        {"name", "Provider Name"}
    })) == "Provider Name");

    CHECK(resolved_display_name(nested_session_json({
        {"username", "orca_username"}
    })) == "orca_username");
}
