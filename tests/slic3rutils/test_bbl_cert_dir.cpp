#include <catch2/catch_all.hpp>
#include <boost/filesystem.hpp>
#include <ctime>
#include <set>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/BBLPrinterAgent.hpp"
#include "test_utils.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

// Stand-in for the bundled CA list. It is copied as it is, never parsed.
const std::string bundled_pem = "-----BEGIN CERTIFICATE-----\nBUNDLED\n-----END CERTIFICATE-----\n";

// Self-signed, basicConstraints CA:TRUE, valid until 2126.
const std::string ca_pem = "-----BEGIN CERTIFICATE-----\n"
                           "MIIBdDCCARqgAwIBAgIUHgU8NZU4wLWpjhUvuDFYyLC462YwCgYIKoZIzj0EAwIw\n"
                           "FzEVMBMGA1UEAwwMT3JjYSBUZXN0IENBMCAXDTI2MTAzMDE3NTcyNFoYDzIxMjYx\n"
                           "MDA2MTc1NzI0WjAXMRUwEwYDVQQDDAxPcmNhIFRlc3QgQ0EwWTATBgcqhkjOPQIB\n"
                           "BggqhkjOPQMBBwNCAASgb/DwDMUKTWsm+8Mqc9o646Co6OZsgZZUMkj/uZf8ZmcM\n"
                           "hJLS+iciD44/h37HHxw/r9f2l1kqv3mk3RalWJdWo0IwQDAdBgNVHQ4EFgQUYqhd\n"
                           "KI+8k/orAS81GcJ1D14QteIwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC\n"
                           "AgQwCgYIKoZIzj0EAwIDSAAwRQIgXgYJ+qQTNM0Cg0j7QilqsowswWaFX8eM8gLv\n"
                           "Jqa8nWcCIQCnLysLNA0PlKGWcfIqZ093RJg/y4lDq9vn9lp5Ss5NrQ==\n"
                           "-----END CERTIFICATE-----\n";

// Signed by the CA above, basicConstraints CA:FALSE: a server certificate, not a CA.
const std::string leaf_pem = "-----BEGIN CERTIFICATE-----\n"
                             "MIIBRjCB7aADAgECAhQETSnonAw6sZXxIjMMJZJr58MS8TAKBggqhkjOPQQDAjAX\n"
                             "MRUwEwYDVQQDDAxPcmNhIFRlc3QgQ0EwIBcNMjYxMDMwMTc1NzI0WhgPMjEyNjEw\n"
                             "MDYxNzU3MjRaMBwxGjAYBgNVBAMMEW9yY2EtdGVzdC1wcmludGVyMFkwEwYHKoZI\n"
                             "zj0CAQYIKoZIzj0DAQcDQgAEuLIhVucena1P1KJzYyW1i6kWmgqbG0/KrfosMeA+\n"
                             "+9Lt0zxSKwEntR5Gl9B1SuQsdsrBUv61toCyKctXp/lfXqMQMA4wDAYDVR0TAQH/\n"
                             "BAIwADAKBggqhkjOPQQDAgNIADBFAiEA7Qp7rXzT1UfVKSkVqmUICjFzxkTbSrxQ\n"
                             "5JHSKxTn6eICIES+RhZNXmt1D6MX04UYGeIF+LXKcSImuhaDWVDYoAPS\n"
                             "-----END CERTIFICATE-----\n";

std::string read_file(const fs::path& path)
{
    std::string content;
    load_string_file(path, content);
    return content;
}

// A stand-in for resources/cert: the plug-in's CA bundle and the cloud certificate next to it.
struct BundledCertDir
{
    ScopedTemporaryDir dir{"orca-bbl-cert-bundled"};
    BundledCertDir()
    {
        save_string_file(dir.path() / "printer.cer", bundled_pem);
        save_string_file(dir.path() / "slicer_base64.cer", "cloud");
    }
};

} // namespace

TEST_CASE("A custom CA is appended to a copy of the bundled printer.cer", "[BBLCertDir]")
{
    BundledCertDir     bundled;
    ScopedTemporaryDir scratch{"orca-bbl-cert-out"};
    const fs::path     out = scratch.path() / "cert";
    const fs::path     ca  = scratch.path() / "proxy-ca.crt";
    save_string_file(ca, ca_pem + "text after the certificate\n");

    const std::string used = BBLPrinterAgent::cert_dir_with_cas(bundled.dir.path().string(), out.string(), {ca.string()});

    CHECK(used == out.string());
    CHECK(read_file(out / "printer.cer") == bundled_pem + ca_pem); // only the parsed certificate, not the trailing text
    CHECK(read_file(out / "slicer_base64.cer") == "cloud");
    CHECK(read_file(bundled.dir.path() / "printer.cer") == bundled_pem);
}

TEST_CASE("The bundled cert folder is kept when no custom CA is usable", "[BBLCertDir]")
{
    const std::string unusable = GENERATE("none", "missing", "not-pem", "broken-pem", "leaf-only", "ca-and-leaf");

    BundledCertDir     bundled;
    ScopedTemporaryDir scratch{"orca-bbl-cert-out"};
    const fs::path     out  = scratch.path() / "cert";
    const fs::path     file = scratch.path() / "ca.crt";

    std::set<std::string> extra_cas;
    if (unusable != "none")
        extra_cas.insert(file.string());
    if (unusable == "not-pem")
        save_string_file(file, "hello");
    else if (unusable == "broken-pem")
        save_string_file(file, "-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n");
    else if (unusable == "leaf-only")
        save_string_file(file, leaf_pem);
    else if (unusable == "ca-and-leaf")
        save_string_file(file, ca_pem + leaf_pem);

    INFO("unusable CA: " << unusable);
    CHECK(BBLPrinterAgent::cert_dir_with_cas(bundled.dir.path().string(), out.string(), extra_cas) == bundled.dir.path().string());
    CHECK_FALSE(fs::exists(out));
}

TEST_CASE("An unchanged cert folder is not rewritten", "[BBLCertDir]")
{
    BundledCertDir     bundled;
    ScopedTemporaryDir scratch{"orca-bbl-cert-out"};
    const fs::path     out = scratch.path() / "cert";
    const fs::path     ca  = scratch.path() / "proxy-ca.crt";
    save_string_file(ca, ca_pem);

    BBLPrinterAgent::cert_dir_with_cas(bundled.dir.path().string(), out.string(), {ca.string()});
    const std::time_t long_ago = 1000000000;
    fs::last_write_time(out / "printer.cer", long_ago);
    fs::last_write_time(out / "slicer_base64.cer", long_ago);

    BBLPrinterAgent::cert_dir_with_cas(bundled.dir.path().string(), out.string(), {ca.string()});

    CHECK(fs::last_write_time(out / "printer.cer") == long_ago);
    CHECK(fs::last_write_time(out / "slicer_base64.cer") == long_ago);
    CHECK_FALSE(fs::exists(out / "printer.cer.tmp"));
}
