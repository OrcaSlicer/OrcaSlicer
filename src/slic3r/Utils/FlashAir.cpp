#include "FlashAir.hpp"

#include <algorithm>
#include <ctime>
#include <boost/filesystem/path.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <wx/frame.h>
#include <wx/event.h>
#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/checkbox.h>

#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "Http.hpp"

namespace fs = boost::filesystem;
namespace pt = boost::property_tree;

namespace Slic3r {

FlashAir::FlashAir(DynamicPrintConfig *config) :
	host(config->opt_string("print_host"))
{}

const char* FlashAir::get_name() const { return "FlashAir"; }

bool FlashAir::test(wxString &msg) const
{
	// Since the request is performed synchronously here,
	// it is ok to refer to `msg` from within the closure

	const char *name = get_name();

	bool res = false;
	auto url = make_url("command.cgi", "op", "118");

	BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Get upload enabled at: %2%") % name % url;

	auto http = Http::get(std::move(url));
	http.on_error([&](std::string body, std::string error, unsigned status) {
			BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting upload enabled: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
			res = false;
			msg = format_error(body, error, status);
		})
        .on_complete([&](std::string body, unsigned) {
			BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: Got upload enabled: %2%") % name % body;

			res = boost::starts_with(body, "1");
			if (! res) {
				msg = _(L("Upload not enabled on FlashAir card."));
			}
		})
		.perform_sync();

	return res;
}

wxString FlashAir::get_test_ok_msg () const
{
	return _(L("Connection to FlashAir is working correctly and upload is enabled."));
}

wxString FlashAir::get_test_failed_msg (wxString &msg) const
{
    return GUI::from_u8((boost::format("%s: %s\n%s")
                    % _utf8(L("Could not connect to FlashAir"))
                    % std::string(msg.ToUTF8())
                    % _utf8(L("Note: FlashAir with firmware 2.00.02 or newer and activated upload function is required."))).str());
}

bool FlashAir::upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
{
	const char *name = get_name();

	const auto upload_filename = upload_data.upload_path.filename();
	const auto upload_parent_path = upload_data.upload_path.parent_path();
	wxString test_msg;
	if (! test(test_msg)) {
		error_fn(std::move(test_msg));
		return false;
	}

	bool res = false;

    std::string strDest = upload_parent_path.string();
    if (strDest.front()!='/') // Needs a leading / else root uploads fail.
    {
        strDest.insert(0,"/");
    }

	auto urlPrepare = make_url("upload.cgi", "WRITEPROTECT=ON&FTIME", timestamp_str());
    auto urlSetDir = make_url("upload.cgi","UPDIR",strDest);
	auto urlUpload = make_url("upload.cgi");

	BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Uploading file %2% at %3% / %4%, filename: %5%")
		% name
		% upload_data.source_path
		% urlPrepare
		% urlUpload
		% upload_filename.string();

	// set filetime for upload and make card writeprotect to prevent filesystem damage
	auto httpPrepare = Http::get(std::move(urlPrepare));
	httpPrepare.on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error preparing upload: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
			error_fn(format_error(body, error, status));
			res = false;
		})
		.on_complete([&, this](std::string body, unsigned) {
			BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: Got prepare result: %2%") % name % body;
			res = boost::icontains(body, "SUCCESS");
			if (! res) {
				BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Request completed but no SUCCESS message was received.") % name;
				error_fn(format_error(body, L("Unknown error occurred"), 0));
			}
		})
		.perform_sync();
	
	if(! res ) {
		return res;
	}
	
	// start file upload
    auto httpDir = Http::get(std::move(urlSetDir));
    httpDir.on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error setting upload dir: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
            error_fn(format_error(body, error, status));
            res = false;
        })
        .on_complete([&, this](std::string body, unsigned) {
            BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: Got dir select result: %2%") % name % body;
            res = boost::icontains(body, "SUCCESS");
            if (! res) {
                BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Request completed but no SUCCESS message was received.") % name;
                error_fn(format_error(body, L("Unknown error occurred"), 0));
            }
        })
        .perform_sync();

    if(! res ) {
        return res;
    }

	auto http = Http::post(std::move(urlUpload));
	http.form_add_file("file", upload_data.source_path.string(), upload_filename.string())
		.on_complete([&](std::string body, unsigned status) {
			BOOST_LOG_TRIVIAL(debug) << boost::format("%1%: File uploaded: HTTP %2%: %3%") % name % status % body;
			res = boost::icontains(body, "SUCCESS");
			if (! res) {
				BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Request completed but no SUCCESS message was received.") % name;
				error_fn(format_error(body, L("Unknown error occurred"), 0));
			}
		})
		.on_error([&](std::string body, std::string error, unsigned status) {
			BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error uploading file: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
			error_fn(format_error(body, error, status));
			res = false;
		})
		.on_progress([&](Http::Progress progress, bool &cancel) {
			prorgess_fn(std::move(progress), cancel);
			if (cancel) {
				// Upload was canceled
				BOOST_LOG_TRIVIAL(info) << boost::format("%1%: Upload canceled") % name;
				res = false;
			}
		})
		.perform_sync();

	return res;
}

std::string FlashAir::timestamp_str() const
{
	auto t = std::time(nullptr);
	auto tm = *std::localtime(&t);

	unsigned long fattime = ((tm.tm_year - 80) << 25) | 
							((tm.tm_mon + 1) << 21) |
							(tm.tm_mday << 16) |
							(tm.tm_hour << 11) |
							(tm.tm_min << 5) |
							(tm.tm_sec >> 1);

	return (boost::format("%1$#x") % fattime).str();
}

std::string FlashAir::make_url(const std::string &path) const
{
	if (host.find("http://") == 0 || host.find("https://") == 0) {
		if (host.back() == '/') {
			return (boost::format("%1%%2%") % host % path).str();
		} else {
			return (boost::format("%1%/%2%") % host % path).str();
		}
	} else {
		if (host.back() == '/') {
			return (boost::format("http://%1%%2%") % host % path).str();
		} else {
			return (boost::format("http://%1%/%2%") % host % path).str();
		}
	}
}

std::string FlashAir::make_url(const std::string &path, const std::string &arg, const std::string &val) const
{
	if (host.find("http://") == 0 || host.find("https://") == 0) {
		if (host.back() == '/') {
			return (boost::format("%1%%2%?%3%=%4%") % host % path % arg % val).str();
		} else {
			return (boost::format("%1%/%2%?%3%=%4%") % host % path % arg % val).str();
		}
	} else {
		if (host.back() == '/') {
			return (boost::format("http://%1%%2%?%3%=%4%") % host % path % arg % val).str();
		} else {
			return (boost::format("http://%1%/%2%?%3%=%4%") % host % path % arg % val).str();
		}
	}
}

}
