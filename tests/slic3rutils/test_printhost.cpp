#include <catch2/catch_all.hpp>

#include "slic3r/Utils/PrintHost.hpp"

using namespace Slic3r;

namespace {

// PrintHost::format_error is protected and the class is abstract. A minimal concrete host with the pure
// virtuals stubbed lets us drive the base formatter directly, since it is shared by every host type.
class TestPrintHost : public PrintHost
{
public:
    using PrintHost::format_error;

    const char* get_name() const override { return "Test"; }
    bool test(wxString&) const override { return true; }
    wxString get_test_ok_msg() const override { return {}; }
    wxString get_test_failed_msg(wxString&) const override { return {}; }
    bool upload(PrintHostUpload, ProgressFn, ErrorFn, InfoFn) const override { return true; }
    bool has_auto_discovery() const override { return false; }
    bool can_test() const override { return false; }
    PrintHostPostUploadActions get_post_upload_actions() const override { return {}; }
    std::string get_host() const override { return {}; }
};

std::string format_error(const std::string& body, const std::string& error, unsigned status)
{
    return TestPrintHost().format_error(body, error, status).ToStdString();
}

std::string json_escape(const std::string& s)
{
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out += c;
        }
    }
    return out;
}

// Build a Moonraker error envelope from code, message, and traceback, without hand-escaping the JSON.
std::string envelope(int code, const std::string& message, const std::string& traceback)
{
    return R"({"error": {"code": )" + std::to_string(code) + R"(, "message": ")" + json_escape(message) + R"(", "traceback": ")" +
           json_escape(traceback) + R"("}})";
}

// The whole body Moonraker returns for a raised HTTPError: error.message is the reason phrase, and the
// traceback's final line is Tornado's "...: HTTP <code>: <message>[ (<detail>)]".
std::string moonraker_error(int code, const std::string& message, const std::string& detail = {})
{
    std::string line = "tornado.web.HTTPError: HTTP " + std::to_string(code) + ": " + message;
    if (!detail.empty())
        line += " (" + detail + ")";
    return envelope(code, message, "Traceback (most recent call last):\n  ...\n" + line + "\n");
}

// Real body from a Klipper printer reached via the Octo/Klipper host type: the OctoPrint-compatible
// endpoint delegates to Moonraker's file_manager, so a busy printer returns this Python 3.11 traceback
// (nested "During handling", caret markers) rather than an OctoPrint-shaped error.
constexpr const char* k_busy_file_403 =
    R"JSON({"error": {"code": 403, "message": "Forbidden", "traceback": "Traceback (most recent call last):\n\n  File \"/home/lava/moonraker/moonraker/components/file_manager/file_manager.py\", line 1017, in _finish_gcode_upload\n    can_start = self._handle_operation_check(check_path)\n                ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^\n\nmoonraker.utils.exceptions.ServerError: File currently in use\n\nDuring handling of the above exception, another exception occurred:\n\nTraceback (most recent call last):\n\n  File \"/home/lava/moonraker/moonraker/components/application.py\", line 1069, in post\n    raise tornado.web.HTTPError(\ntornado.web.HTTPError: HTTP 403: Forbidden (File is loaded, upload not permitted)\n"}})JSON";

} // namespace

TEST_CASE("A Klipper upload error shows its reason instead of a Python traceback", "[PrintHost][Regression]")
{
    const std::string msg = format_error(k_busy_file_403, "", 403);
    INFO("actual: " << msg);

    CHECK(msg == "HTTP 403: Forbidden (File is loaded, upload not permitted)");
    CHECK_THAT(msg, !Catch::Matchers::ContainsSubstring("Traceback"));
    CHECK_THAT(msg, !Catch::Matchers::ContainsSubstring("file_manager.py"));
}

// The file endpoints report a generic reason phrase in message and the specific cause only in the
// traceback's final line; recover it, verbatim, including when a filename nests parens.
TEST_CASE("The specific cause is recovered from a file endpoint's traceback", "[PrintHost]")
{
    SECTION("a plain detail")
    {
        const std::string body = moonraker_error(403, "Forbidden", "File is loaded, upload not permitted");
        CHECK(format_error(body, "", 403) == "HTTP 403: Forbidden (File is loaded, upload not permitted)");
    }

    SECTION("a detail whose own parentheses nest (a filename)")
    {
        const std::string detail = "Directory does not exist (/home/pi/gcodes/plate (1).gcode)";
        const std::string body   = moonraker_error(400, "Bad Request", detail);
        CHECK(format_error(body, "", 400) == "HTTP 400: Bad Request (" + detail + ")");
    }

    SECTION("the reason word also appears in an earlier stack frame")
    {
        const std::string tail      = "tornado.web.HTTPError: HTTP 403: Forbidden (File is loaded, upload not permitted)\n";
        const std::string traceback = "Traceback (most recent call last):\n"
                                      "  File \"/home/pi/moonraker/Forbidden/handler.py\", line 5, in check\n" +
                                      tail;
        const std::string body = envelope(403, "Forbidden", traceback);
        CHECK(format_error(body, "", 403) == "HTTP 403: Forbidden (File is loaded, upload not permitted)");
    }

    SECTION("the detail itself repeats the reason word")
    {
        const std::string body = moonraker_error(403, "Forbidden", "Forbidden zone: access denied");
        CHECK(format_error(body, "", 403) == "HTTP 403: Forbidden (Forbidden zone: access denied)");
    }
}

// On the other endpoints message already holds the full reason and the final traceback line carries no
// trailing detail; it must be shown as-is, never doubled.
TEST_CASE("A reason already complete in message is shown unchanged", "[PrintHost]")
{
    SECTION("message is the whole reason, no trailing detail")
    {
        const std::string body = moonraker_error(503, "Klippy is not ready");
        CHECK(format_error(body, "", 503) == "HTTP 503: Klippy is not ready");
    }

    SECTION("a message that itself contains parentheses is not duplicated")
    {
        const std::string reason = "Requested blocks (0-5) are unavailable";
        const std::string body   = moonraker_error(400, reason);
        CHECK(format_error(body, "", 400) == "HTTP 400: " + reason);
    }
}

TEST_CASE("A Moonraker error with no usable detail shows just the reason phrase", "[PrintHost]")
{
    struct Case
    {
        const char* name;
        const char* body;
        unsigned status;
        const char* expected;
    };

    const auto c = GENERATE(
        Case{"no traceback field", R"JSON({"error": {"code": 500, "message": "Internal Server Error"}})JSON", 500,
             "HTTP 500: Internal Server Error"},
        Case{"a final line that is not a Tornado HTTPError",
             R"JSON({"error": {"code": 401, "message": "Unauthorized", "traceback": "Traceback (most recent call last):\n\nutils.ServerError: API key required\n"}})JSON",
             401, "HTTP 401: Unauthorized"},
        // The reason is Format()'s %s argument, so a percent sign in it must survive verbatim.
        Case{"a percent sign in the reason is not a format specifier",
             R"JSON({"error": {"code": 507, "message": "Insufficient Storage: disk 100% full"}})JSON", 507,
             "HTTP 507: Insufficient Storage: disk 100% full"});

    DYNAMIC_SECTION(c.name) { CHECK(format_error(c.body, "", c.status) == c.expected); }
}

// The transform must recognize only the Moonraker envelope; every other body echoes through as before.
TEST_CASE("Error bodies that are not a Moonraker envelope are left unchanged", "[PrintHost]")
{
    SECTION("OctoPrint's string-valued error member")
    {
        const std::string body = R"JSON({"error": "File not found"})JSON";
        CHECK(format_error(body, "", 404) == "HTTP 404: " + body);
    }

    SECTION("PrusaLink's top-level message, not under error")
    {
        const std::string body = R"JSON({"title": "Conflict", "message": "Printer is printing"})JSON";
        CHECK(format_error(body, "", 409) == "HTTP 409: " + body);
    }

    SECTION("a body that is not JSON")
    {
        const std::string html = "<html><head><title>502 Bad Gateway</title></head></html>";
        CHECK(format_error(html, "", 502) == "HTTP 502: " + html);
    }

    SECTION("an envelope whose reason phrase is empty")
    {
        const std::string body = R"JSON({"error": {"code": 403, "message": ""}})JSON";
        CHECK(format_error(body, "", 403) == "HTTP 403: " + body);
    }

    SECTION("a transport error carries no HTTP status or body")
    {
        CHECK(format_error("", "curl:Could not connect", 0) == "curl:Could not connect");
    }
}
