#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <catch2/catch_message.hpp>
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/Widgets/WebViewHostDialog.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <string>
#include <wx/filename.h>
#include <wx/string.h>
#include <wx/uri.h>

using namespace Slic3r::GUI;

#ifndef _WIN32
TEST_CASE("A file URL keeps characters special to URLs in its path", "[FileUrl]")
{
    const std::string path = GENERATE(as<std::string>{},
        "/opt/test#dir/resources/web/homepage/index.html",
        "/opt/test%20x/resources/web/homepage/index.html",
        "/opt/Orca Slicer/resources/web/homepage/index.html",
        "/opt/what?/resources/web/homepage/index.html",
        "/home/Jos\xC3\xA9/\xE8\xB5\x84\xE6\xBA\x90/resources/web/homepage/index.html");

    const wxString url = file_url_from_path(boost::filesystem::path(path));
    CAPTURE(path, url.utf8_string());

    // WebView::CreateWebView() and WebView::LoadUrl() re-parse the URL before loading it.
    const wxURI uri(wxURI(url).BuildURI());
    CHECK(uri.GetScheme() == "file");
    CHECK(!uri.HasQuery());
    CHECK(!uri.HasFragment());
    CHECK(wxURI::Unescape(uri.GetPath()).utf8_string() == path);
}

TEST_CASE("A file URL of a plain path is the path behind file://", "[FileUrl]")
{
    const std::string path = "/opt/OrcaSlicer/resources/web/homepage/index.html";
    CHECK(file_url_from_path(boost::filesystem::path(path)) == "file://" + path);
}

TEST_CASE("A query appended to a file URL stays separate from its path", "[FileUrl]")
{
    const std::string path = "/opt/test#dir%20x/resources/web/guide/0/index.html";
    const wxURI uri(file_url_from_path(boost::filesystem::path(path)) + "?target=21&lang=de");
    CHECK(wxURI::Unescape(uri.GetPath()).utf8_string() == path);
    CHECK(uri.GetQuery() == "target=21&lang=de");
    CHECK(!uri.HasFragment());
}
#else
TEST_CASE("A file URL of a Windows path has a drive letter and forward slashes", "[FileUrl]")
{
    const auto [path, url] = GENERATE(table<std::wstring, std::string>({
        { L"C:\\Program Files\\OrcaSlicer\\resources\\web\\homepage\\index.html",
          "file:///C:/Program%20Files/OrcaSlicer/resources/web/homepage/index.html" },
        { L"D:\\#OneDrive\\OrcaSlicer\\resources\\web\\guide\\0\\index.html",
          "file:///D:/%23OneDrive/OrcaSlicer/resources/web/guide/0/index.html" },
        { L"D:\\100%\\OrcaSlicer\\resources\\web\\homepage\\index.html",
          "file:///D:/100%25/OrcaSlicer/resources/web/homepage/index.html" },
        // Callers join the resources directory with a forward-slash relative path.
        { L"D:\\#OneDrive\\OrcaSlicer\\resources/web/homepage/index.html",
          "file:///D:/%23OneDrive/OrcaSlicer/resources/web/homepage/index.html" },
        { L"\\\\server\\share\\OrcaSlicer\\resources\\web\\homepage\\index.html",
          "file://server/share/OrcaSlicer/resources/web/homepage/index.html" },
    }));
    CHECK(file_url_from_path(boost::filesystem::path(path)).utf8_string() == url);
}

TEST_CASE("A query appended to a Windows file URL stays separate from its path", "[FileUrl]")
{
    const wxURI uri(file_url_from_path(boost::filesystem::path(L"D:\\#OneDrive\\OrcaSlicer\\resources\\web\\guide\\0\\index.html")) +
                    "?target=21&lang=de");
    CHECK(wxURI::Unescape(uri.GetPath()) == "/D:/#OneDrive/OrcaSlicer/resources/web/guide/0/index.html");
    CHECK(uri.GetQuery() == "target=21&lang=de");
    CHECK(!uri.HasFragment());
}
#endif

TEST_CASE("A page's own loads are told from a dropped file and from other pages", "[FileUrl]")
{
    const boost::filesystem::path dir     = boost::filesystem::temp_directory_path() / "orca page";
    const boost::filesystem::path page    = dir / "index.html";
    const boost::filesystem::path dropped = dir / "replay.json";
    const wxFileName              page_name(from_path(page));

    CHECK(WebViewHostDialog::own_page_navigation(file_url_from_path(page) + "?lang=de", page_name));
    CHECK(WebViewHostDialog::own_page_navigation("about:blank", page_name));
    CHECK_FALSE(WebViewHostDialog::own_page_navigation("https://example.com/", page_name));
    CHECK_FALSE(WebViewHostDialog::own_page_navigation(file_url_from_path(dropped), page_name));

    const boost::filesystem::path file = WebViewHostDialog::dropped_file(file_url_from_path(dropped));
    CHECK(wxFileName(from_path(file)).SameAs(wxFileName(from_path(dropped))));
    CHECK(WebViewHostDialog::dropped_file("https://example.com/replay.json").empty());
}
