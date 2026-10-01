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
