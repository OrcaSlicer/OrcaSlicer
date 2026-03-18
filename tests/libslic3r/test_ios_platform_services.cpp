#include <catch2/catch_all.hpp>

#include "portability/platform/ios/IOSKeychainCredentialStore.hpp"
#include "portability/platform/ios/IOSPlatformServices.hpp"

#include <utility>

namespace {

using namespace Slic3r::portability::platform::ios;

class FakeKeychainBackend final : public IOSKeychainCredentialBackend
{
public:
    IOSKeychainReadResult read_result{IOSKeychainStatus::item_not_found, std::nullopt};
    IOSKeychainStatus     update_status{IOSKeychainStatus::item_not_found};
    IOSKeychainStatus     add_status{IOSKeychainStatus::success};
    IOSKeychainStatus     remove_status{IOSKeychainStatus::success};

    mutable int read_calls{0};
    int         update_calls{0};
    int         add_calls{0};
    int         remove_calls{0};

    IOSKeychainReadResult read(const std::string&, const std::string&) const override
    {
        ++read_calls;
        return read_result;
    }

    IOSKeychainStatus update(const std::string&, const std::string&, const std::string&) override
    {
        ++update_calls;
        return update_status;
    }

    IOSKeychainStatus add(const std::string&, const std::string&, const std::string&) override
    {
        ++add_calls;
        return add_status;
    }

    IOSKeychainStatus remove(const std::string&, const std::string&) override
    {
        ++remove_calls;
        return remove_status;
    }
};

class FakePathProvider final : public IOSPathProvider
{
public:
    std::string writable_path{"/app/data"};
    std::string temp_path{"/tmp/app"};

    std::string writable_app_data_path() const override { return writable_path; }
    std::string temporary_path() const override { return temp_path; }
};

class FakeDispatchQueue final : public IOSDispatchQueue
{
public:
    int main_posts{0};
    int background_posts{0};

    void post_to_main_thread(std::function<void()> task) override
    {
        ++main_posts;
        if (task)
            task();
    }

    void post_background(std::function<void()> task) override
    {
        ++background_posts;
        if (task)
            task();
    }
};

class DummyCredentialStore final : public Slic3r::portability::ICredentialStore
{
public:
    std::optional<std::string> read(const std::string&, const std::string&) const override { return std::nullopt; }
    bool                       write(const std::string&, const std::string&, const std::string&) override { return true; }
    bool                       remove(const std::string&, const std::string&) override { return true; }
};

} // namespace

TEST_CASE("IOSKeychainCredentialStore reads only successful backend responses", "[platform][ios][keychain]")
{
    auto                       backend = std::make_shared<FakeKeychainBackend>();
    IOSKeychainCredentialStore store(backend);

    backend->read_result = {IOSKeychainStatus::success, std::string("token")};
    REQUIRE(store.read("svc", "acct") == std::optional<std::string>("token"));

    backend->read_result = {IOSKeychainStatus::item_not_found, std::nullopt};
    REQUIRE_FALSE(store.read("svc", "acct").has_value());

    backend->read_result = {IOSKeychainStatus::error, std::optional<std::string>("stale")};
    REQUIRE_FALSE(store.read("svc", "acct").has_value());
}

TEST_CASE("IOSKeychainCredentialStore write falls back to add only when item is missing", "[platform][ios][keychain]")
{
    auto                       backend = std::make_shared<FakeKeychainBackend>();
    IOSKeychainCredentialStore store(backend);

    backend->update_status = IOSKeychainStatus::success;
    backend->add_status    = IOSKeychainStatus::error;
    REQUIRE(store.write("svc", "acct", "secret"));
    REQUIRE(backend->update_calls == 1);
    REQUIRE(backend->add_calls == 0);

    backend->update_status = IOSKeychainStatus::item_not_found;
    backend->add_status    = IOSKeychainStatus::success;
    REQUIRE(store.write("svc", "acct", "secret"));
    REQUIRE(backend->update_calls == 2);
    REQUIRE(backend->add_calls == 1);

    backend->update_status = IOSKeychainStatus::item_not_found;
    backend->add_status    = IOSKeychainStatus::error;
    REQUIRE_FALSE(store.write("svc", "acct", "secret"));
    REQUIRE(backend->update_calls == 3);
    REQUIRE(backend->add_calls == 2);

    backend->update_status = IOSKeychainStatus::error;
    REQUIRE_FALSE(store.write("svc", "acct", "secret"));
    REQUIRE(backend->update_calls == 4);
    REQUIRE(backend->add_calls == 2);
}

TEST_CASE("IOSKeychainCredentialStore remove accepts missing items", "[platform][ios][keychain]")
{
    auto                       backend = std::make_shared<FakeKeychainBackend>();
    IOSKeychainCredentialStore store(backend);

    backend->remove_status = IOSKeychainStatus::success;
    REQUIRE(store.remove("svc", "acct"));

    backend->remove_status = IOSKeychainStatus::item_not_found;
    REQUIRE(store.remove("svc", "acct"));

    backend->remove_status = IOSKeychainStatus::error;
    REQUIRE_FALSE(store.remove("svc", "acct"));
}

TEST_CASE("IOSPlatformServices delegates to injected collaborators", "[platform][ios]")
{
    auto path_provider    = std::make_shared<FakePathProvider>();
    auto dispatch_queue   = std::make_shared<FakeDispatchQueue>();
    auto credential_store = std::make_shared<DummyCredentialStore>();

    IOSPlatformServices services(path_provider, dispatch_queue, credential_store);

    bool main_task_called       = false;
    bool background_task_called = false;
    services.post_to_main_thread([&main_task_called] { main_task_called = true; });
    services.post_background([&background_task_called] { background_task_called = true; });

    REQUIRE(services.platform_name() == "ios");
    REQUIRE(services.writable_app_data_path() == "/app/data");
    REQUIRE(services.temporary_path() == "/tmp/app");
    REQUIRE(main_task_called);
    REQUIRE(background_task_called);
    REQUIRE(dispatch_queue->main_posts == 1);
    REQUIRE(dispatch_queue->background_posts == 1);
    REQUIRE(&services.credential_store() == credential_store.get());

    const IOSPlatformServices& const_services = services;
    REQUIRE(&const_services.credential_store() == credential_store.get());
}
