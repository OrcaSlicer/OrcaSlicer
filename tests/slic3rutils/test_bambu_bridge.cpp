#include <mutex>
#include <condition_variable>
#include <deque>
#include <cstddef>
#include <stdexcept>
#include "slic3r/Utils/BambuBridge/Rpc.hpp"
#include <utility>
#include <string>
#include "slic3r/Utils/BambuNetworkTypes.hpp"
#include <vector>
#include <cstdint>
#include <exception>
#include <thread>
#ifdef BAMBU_BRIDGE_STANDALONE_TESTS
#include "catch_amalgamated.hpp"
#else
#include <catch2/catch_test_macros.hpp>
#endif
#include "slic3r/Utils/BambuBridge/Bindings.hpp"
#include "slic3r/Utils/BBLNetworkFunctions.hpp"
#include <future>

using namespace Slic3r;
using namespace Slic3r::BambuBridge;
using namespace std::chrono_literals;

namespace {

// In-memory byte streams exercise production framing and dispatcher code on every platform.
struct Stream {
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<unsigned char> bytes;
    bool closed = false;
    void read(void* target, std::size_t count)
    {
        auto* data = static_cast<unsigned char*>(target);
        std::unique_lock<std::mutex> lock(mutex);
        for (std::size_t i = 0; i < count; ++i) {
            condition.wait(lock, [this] { return closed || !bytes.empty(); });
            if (closed) throw std::runtime_error("stream closed");
            data[i] = bytes.front();
            bytes.pop_front();
        }
    }
    void write(void* source, std::size_t count)
    {
        auto* data = static_cast<unsigned char*>(source);
        std::lock_guard<std::mutex> lock(mutex);
        if (closed) throw std::runtime_error("stream closed");
        bytes.insert(bytes.end(), data, data + count);
        condition.notify_all();
    }
    void close()
    {
        std::lock_guard<std::mutex> lock(mutex);
        closed = true;
        condition.notify_all();
    }
};

struct Pair {
    Stream client_input, server_input;
    Rpc client, server;
    Pair(Rpc::Handler client_handler, Rpc::Handler server_handler, Rpc::Background background = {})
        : client([this](void* p, std::size_t n) { client_input.read(p, n); },
                 [this](void* p, std::size_t n) { server_input.write(p, n); },
                 [this] { close(); }, std::move(client_handler)),
          server([this](void* p, std::size_t n) { server_input.read(p, n); },
                 [this](void* p, std::size_t n) { client_input.write(p, n); },
                 [this] { close(); }, std::move(server_handler), std::move(background)) {}
    void start() { client.start(); server.start(); }
    void close() { client_input.close(); server_input.close(); }
    ~Pair() { client.stop(); server.stop(); }
};

int output_values(void*, std::string* ticket, detectResult& detect)
{
    if (ticket) *ticket = "ticket-\xe2\x98\x83";
    detect.dev_id = "printer";
    detect.result_msg = "found";
    return 7;
}

int fake_print(void*, PrintParams params, OnUpdateStatusFn update, WasCancelledFn cancel, OnWaitFn wait)
{
    update(PrintingStageUpload, 0, params.filename);
    if (!wait(PrintingStageWaitPrinter, "ready")) return -2;
    return cancel() ? BAMBU_NETWORK_ERR_CANCELED : 0;
}

// Instantiate every server binding in the allow-list, including its argument decoders.
// This catches ABI/wire-schema drift even when an export is absent in a test DLL.
#define BAMBU_BRIDGE_API(name, type, symbol) \
    [[maybe_unused]] Json compile_##name(type fn, const Json& args, Rpc& rpc, void* agent) \
    { return Binding<type>::serve(fn, args, rpc, agent); }
#include "slic3r/Utils/BambuBridge/Api.def"
#undef BAMBU_BRIDGE_API

} // namespace

TEST_CASE("Bambu RPC preserves Unicode and updates output references and pointers", "[BambuBridge]")
{
    Callbacks callbacks;
    Pair* pair_ptr = nullptr;
    int helper_agent;
    Pair pair([](const std::string&, const Json&) { return Json(); },
              [&](const std::string&, const Json& args) {
                  return Binding<decltype(&output_values)>::serve(&output_values, args, pair_ptr->server, &helper_agent);
              });
    pair_ptr = &pair;
    pair.start();
    std::vector<std::uint64_t> ids;
    std::string ticket;
    detectResult detect{};
    int result = Binding<decltype(&output_values)>::call(pair.client, callbacks, "outputs", ids, agent_token(), &ticket, detect);
    REQUIRE(result == 7);
    REQUIRE(ticket == "ticket-\xe2\x98\x83");
    REQUIRE(detect.dev_id == "printer");
    REQUIRE(detect.result_msg == "found");
    REQUIRE(ids.empty());
}

TEST_CASE("Bambu print callbacks return cancellation and pump nested printer requests", "[BambuBridge]")
{
    Callbacks callbacks;
    Pair* pair_ptr = nullptr;
    int helper_agent;
    std::string progress_file;
    int nested_result = 0;
    Pair pair([&](const std::string& method, const Json& args) {
                  if (method != "callback") throw std::runtime_error("unexpected callback");
                  return callbacks.invoke(args);
              },
              [&](const std::string& method, const Json& args) -> Json {
                  if (method == "nested") return 42;
                  return Binding<func_start_print>::serve(&fake_print, args, pair_ptr->server, &helper_agent);
              });
    pair_ptr = &pair;
    pair.start();
    PrintParams params{};
    params.filename = "C:/prints/\xe2\x98\x83.3mf";
    params.task_use_ams = true;
    params.ams_mapping_info = R"({"mapping":[1,2]})";
    params.svc_context = "context";
    params.queue_plate_id = "plate-123";
    // Also exercise the full field schema used for by-value PrintParams.
    const Json encoded = params;
    REQUIRE(Json(encoded.get<PrintParams>()) == encoded);
    REQUIRE(encoded.get<PrintParams>().queue_plate_id == params.queue_plate_id);
    OnUpdateStatusFn update = [&](int, int, std::string file) { progress_file = std::move(file); };
    WasCancelledFn cancel = [] { return true; };
    OnWaitFn wait = [&](int, std::string) {
        nested_result = pair.client.request("nested", Json::array()).get<int>();
        return true;
    };
    std::vector<std::uint64_t> ids;
    const int result = Binding<func_start_print>::call(pair.client, callbacks, "bambu_network_start_print", ids,
                                                       agent_token(), params, update, cancel, wait);
    callbacks.erase(ids);
    REQUIRE(result == BAMBU_NETWORK_ERR_CANCELED);
    REQUIRE(progress_file == params.filename);
    REQUIRE(nested_result == 42);
    REQUIRE(ids.size() == 3);
    REQUIRE_THROWS(callbacks.invoke({{"callback", ids.front()}, {"args", Json::array()}}));
}

TEST_CASE("Bambu persistent callbacks survive registration and can be revoked", "[BambuBridge]")
{
    Callbacks callbacks;
    Pair* pair_ptr = nullptr;
    OnMessageFn retained;
    std::string message;
    Pair pair([&](const std::string&, const Json& args) { return callbacks.invoke(args); },
              [&](const std::string& method, const Json& args) -> Json {
                  if (method == "register") {
                      ServerValue<OnMessageFn> decoded(args, pair_ptr->server, nullptr);
                      retained = decoded.get();
                  } else if (retained) retained("printer", "message");
                  return nullptr;
              });
    pair_ptr = &pair;
    pair.start();
    std::vector<std::uint64_t> ids;
    const Json id = ClientValue<OnMessageFn>::encode([&](std::string, std::string text) { message = std::move(text); }, callbacks, ids);
    pair.client.request("register", id);
    pair.client.request("emit", nullptr);
    REQUIRE(message == "message");
    pair.client.request("register", nullptr);
    callbacks.erase(ids);
    message.clear();
    pair.client.request("emit", nullptr);
    REQUIRE(message.empty());
}

TEST_CASE("Bambu helper disconnection fails outstanding calls without retrying", "[BambuBridge]")
{
    std::promise<void> entered, release;
    auto released = release.get_future();
    unsigned calls = 0;
    Pair pair([](const std::string&, const Json&) { return Json(); },
              [&](const std::string&, const Json&) {
                  ++calls;
                  entered.set_value();
                  released.wait();
                  return Json();
              });
    pair.start();
    auto outcome = std::async(std::launch::async, [&] {
        try { pair.client.request("print", nullptr, 5s); return false; }
        catch (const std::exception&) { return true; }
    });
    entered.get_future().wait();
    pair.close();
    const auto ready = outcome.wait_for(1s);
    release.set_value();
    REQUIRE(ready == std::future_status::ready);
    REQUIRE(outcome.get());
    pair.server.stop();
    REQUIRE(calls == 1);
}

TEST_CASE("Bambu RPC rejects oversized frames before allocating payloads", "[BambuBridge]")
{
    Stream input;
    Rpc rpc([&](void* p, std::size_t n) { input.read(p, n); }, [](void*, std::size_t) {},
            [&] { input.close(); }, [](const std::string&, const Json&) { return Json(); });
    rpc.start();
    unsigned char bad_header[4] = {0xff, 0xff, 0xff, 0x7f};
    input.write(bad_header, sizeof(bad_header));
    // An outstanding request observes the malformed-frame disconnect, not a huge allocation.
    REQUIRE_THROWS(rpc.request("hello", nullptr, 1s));
    rpc.stop();
    REQUIRE_FALSE(rpc.running());
}

TEST_CASE("Bambu callbacks from DLL threads can query printers during a background job", "[BambuBridge]")
{
    Pair* pair_ptr = nullptr;
    int nested = 0;
    Pair pair([&](const std::string&, const Json&) {
                  nested = pair_ptr->client.request("status", nullptr).get<int>();
                  return Json();
              },
              [&](const std::string& method, const Json&) -> Json {
                  if (method == "status") return 19;
                  // Model an exported function that waits for the DLL's internal worker.
                  std::thread dll_thread([&] { pair_ptr->server.request("callback", nullptr); });
                  dll_thread.join();
                  return 0;
              }, [](const std::string& method) { return method == "print"; });
    pair_ptr = &pair;
    pair.start();
    REQUIRE(pair.client.request("print", nullptr, 2s) == 0);
    REQUIRE(nested == 19);
    REQUIRE_FALSE(pair.server.has_jobs());
}

TEST_CASE("Bambu upload parameters have defined defaults before serialization", "[BambuBridge]")
{
    // Application verification uploads construct this without aggregate initialization.
    PrintParams params;
    const Json wire = params;
    for (const auto* field : {"use_ssl_for_ftp", "use_ssl_for_mqtt", "task_bed_leveling", "task_flow_cali",
                             "task_vibration_cali", "task_layer_inspect", "task_record_timelapse",
                             "task_timelapse_use_internal", "task_use_ams", "task_ext_change_assist", "try_emmc_print"})
        REQUIRE(wire.at(field) == false);
    REQUIRE(wire.at("plate_index") == 0);
    REQUIRE(wire.at("extruder_cali_manual_mode") == -1);
}

TEST_CASE("Bambu background uploads block their caller until the export returns", "[BambuBridge]")
{
    std::promise<void> entered, release;
    auto released = release.get_future();
    Pair pair([](const std::string&, const Json&) { return Json(); },
              [&](const std::string& method, const Json&) {
                  if (method == "status") return Json(19);
                  entered.set_value();
                  released.wait();
                  return Json(23);
              }, [](const std::string& method) { return method == "upload"; });
    pair.start();
    auto upload = std::async(std::launch::async, [&] { return pair.client.request("upload", nullptr, 5s); });
    entered.get_future().wait();
    const bool active = pair.server.has_jobs();
    const auto before_release = upload.wait_for(0s);
    // A separate request remains responsive while the upload caller is suspended.
    Json status;
    try { status = pair.client.request("status", nullptr, 1s); }
    catch (...) { release.set_value(); throw; }
    release.set_value();
    REQUIRE(active);
    REQUIRE(before_release == std::future_status::timeout);
    REQUIRE(status == 19);
    REQUIRE(upload.get() == 23);
}

TEST_CASE("Bambu callback failures cancel jobs and refuse wait decisions", "[BambuBridge]")
{
    Pair pair([](const std::string&, const Json&) -> Json { throw std::runtime_error("callback failed"); },
              [](const std::string&, const Json&) { return Json(); });
    pair.start();
    ServerValue<WasCancelledFn> cancel(1, pair.server, nullptr);
    ServerValue<OnWaitFn> wait(2, pair.server, nullptr);
    REQUIRE(cancel.get()());
    REQUIRE_FALSE(wait.get()(PrintingStageWaitPrinter, "ready"));
    pair.close();
    REQUIRE(cancel.get()());
    REQUIRE_FALSE(wait.get()(PrintingStageWaitPrinter, "ready"));
}
