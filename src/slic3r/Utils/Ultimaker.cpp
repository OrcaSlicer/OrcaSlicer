#include "Ultimaker.hpp"

#include <algorithm>
#include <ctime>
#include <boost/filesystem/path.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>

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

Ultimaker::Ultimaker(DynamicPrintConfig *config) :
	host(config->opt_string("print_host")),
	password(config->opt_string("printhost_apikey"))
{}

const char* Ultimaker::get_name() const { return "Ultimaker"; }

// Modified from OctoPrint::test in OctoPrint.cpp
bool Ultimaker::test(wxString &msg) const
{
	// auto connectionType = connect(msg);
	// disconnect(connectionType);

	// return connectionType != ConnectionType::error;

	// Since the request is performed synchronously here,
    // it is ok to refer to `msg` from within the closure
	BOOST_LOG_TRIVIAL(warning) << boost::format("Ultimaker: Attempting to test machine");

    const char* name = get_name();

    bool res = true;
    auto url = (boost::format("http://%1%/api/v1/system/variant") % host).str();

	BOOST_LOG_TRIVIAL(warning) << boost::format("Ultimaker: name %1% url %2%") % name % url;
    BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Get system variant at: %2%") % name % url;

    auto http = Http::get(std::move(url));
    set_auth(http);
    http.on_error([&](std::string body, std::string error, unsigned status) {
        BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Error getting system variant: %2%, HTTP %3%, body: `%4%`") % name % error % status % body;
        res = false;
        msg = format_error(body, error, status);
        })
        .on_complete([&, this](std::string body, unsigned) {
            BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Got system variant: %2%") % name % body;
            BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: res=%2%") % name % res; // DEBUG

            try {
                /*std::stringstream ss(body);
                pt::ptree ptree;
                pt::read_json(ss, ptree);

                if (!ptree.get_optional<std::string>("api")) {
                    res = false;
                    return;
                }

				const auto text = ptree.get_optional<std::string>("text");*/

				// Validate that response is correct ("Ultimaker 3", "Ultimaker 3 extended" or "Ultimaker S5")
				res = (boost::starts_with(body, "\"Ultimaker 3") || body == "\"Ultimaker S5\"");
            }
            catch (const std::exception &) {
                res = false;
                msg = "Could not parse server response";
				BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Caught error") % name;
				// BOOST_LOG_TRIVIAL(error) << boost::format("%1%: Caught error: %2%") % name % e;
            }
        })
#ifdef WIN32
        .ssl_revoke_best_effort(m_ssl_revoke_best_effort)
        .on_ip_resolve([&](std::string address) {
            // Workaround for Windows 10/11 mDNS resolve issue, where two mDNS resolves in succession fail.
            // Remember resolved address to be reused at successive REST API call.
            msg = GUI::from_u8(address);
        })
#endif // WIN32
        .perform_sync();


	BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: res=%2%") % name % res; //DEBUG
    return res;
}


wxString Ultimaker::get_test_ok_msg () const
{
	return _("Connection to Ultimaker works correctly.");
}

wxString Ultimaker::get_test_failed_msg (wxString &msg) const
{
    return GUI::from_u8((boost::format("%s: %s")
                    // % _utf8(L("Could not connect to Ultimaker"))
                    % _utf8("Could not connect to Ultimaker")
                    % std::string(msg.ToUTF8())).str());
}


#pragma region Auth
// Copied over from PrusaLink::set_auth in OctoPrint.cpp
void Ultimaker::set_auth(Http& http) const
{
    switch (m_authorization_type) {
    case atKeyPassword:
        http.header("X-Api-Key", get_apikey());
        break;
    case atUserPassword:
        http.auth_digest(m_username, m_password);
        break;
    };

    // if (!get_cafile().empty()) {
    //     http.ca_file(get_cafile());
    // }
}


bool Ultimaker::hasAuthCreds() {
	// TODO: implement. True if has authentication credentials, false otherwise
	return false;
}


bool Ultimaker::isAuthorized() {
	//auto url = get_connect_url(false);
	//auto http = Http::get(std::move(url));

	// TODO: Implement this
	return false;
	

}


std::string Ultimaker::generateAuthCreds() {
	//TODO: Implement.
	// Send POST request to generate creds
	// Get the ID and key from the request
	// Wait for user to authorize on the physical machine
	// Return the result.
	return "Unimplemeneted.";
}



std::string Ultimaker::testAuth() {
	/* 
	Used in Ultimaker::test(), creates creds if they don't already
	exist, and returns various error strings if errors.
	Returns "OK" if no errors and creds are valid.
	*/
	const char* name = get_name();
	BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Testing auth.") % name;

	// Auth credentials are alreay existing (user-entered)
	if (hasAuthCreds()) {
		BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Auth credentials found.") % name;

		if (isAuthorized()) {
			BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Auth credentials are valid. Returning OK") % name;
			return "OK";

		} else {
			BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Auth credentials are INVALID.") % name;
			return "Invalid Credentials";
		}
	} else { // Auth credentials do not exist
		BOOST_LOG_TRIVIAL(warning) << boost::format("%1%: Auth credentials do NOT already exist.") % name;
		//TODO create the auth creds
		generateAuthCreds();

		return "OK";
	}

}

#pragma endregion Auth


bool Ultimaker::upload(PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn) const
{
	wxString connect_msg;
	auto connectionType = connect(connect_msg);
	if (connectionType == ConnectionType::error) {
		error_fn(std::move(connect_msg));
		return false;
	}

	bool res = true;
	bool dsf = (connectionType == ConnectionType::dsf);

	auto upload_cmd = get_upload_url(upload_data.upload_path.string(), connectionType);
	BOOST_LOG_TRIVIAL(info) << boost::format("Ultimaker: Uploading file %1%, filepath: %2%, post_action: %3%, command: %4%")
		% upload_data.source_path
		% upload_data.upload_path
		% int(upload_data.post_action)
		% upload_cmd;

	auto http = (dsf ? Http::put(std::move(upload_cmd)) : Http::post(std::move(upload_cmd)));
	if (dsf) {
		http.set_put_body(upload_data.source_path);
	} else {
		http.set_post_body(upload_data.source_path);
	}
	http.on_complete([&](std::string body, unsigned status) {
			BOOST_LOG_TRIVIAL(debug) << boost::format("Ultimaker: File uploaded: HTTP %1%: %2%") % status % body;

			int err_code = dsf ? (status == 201 ? 0 : 1) : get_err_code_from_body(body);
			if (err_code != 0) {
				BOOST_LOG_TRIVIAL(error) << boost::format("Ultimaker: Request completed but error code was received: %1%") % err_code;
				error_fn(format_error(body, L("Unknown error occurred"), 0));
				res = false;
			} else if (upload_data.post_action == PrintHostPostUploadAction::StartPrint) {
				wxString errormsg;
				res = start_print(errormsg, upload_data.upload_path.string(), connectionType, false);
				if (! res) {
					error_fn(std::move(errormsg));
				}
			} else if (upload_data.post_action == PrintHostPostUploadAction::StartSimulation) {
				wxString errormsg;
				res = start_print(errormsg, upload_data.upload_path.string(), connectionType, true);
				if (! res) {
					error_fn(std::move(errormsg));
				}
			}
		})
		.on_error([&](std::string body, std::string error, unsigned status) {
			BOOST_LOG_TRIVIAL(error) << boost::format("Ultimaker: Error uploading file: %1%, HTTP %2%, body: `%3%`") % error % status % body;
			error_fn(format_error(body, error, status));
			res = false;
		})
		.on_progress([&](Http::Progress progress, bool &cancel) {
			prorgess_fn(std::move(progress), cancel);
			if (cancel) {
				// Upload was canceled
				BOOST_LOG_TRIVIAL(info) << "Ultimaker: Upload canceled";
				res = false;
			}
		})
		.perform_sync();

	disconnect(connectionType);

	return res;
}

Ultimaker::ConnectionType Ultimaker::connect(wxString &msg) const
{
	BOOST_LOG_TRIVIAL(warning) << "Ultimaker: Attempting to connect"; // TODO: Remove this. Testing to see if var substitution is the issue
	BOOST_LOG_TRIVIAL(warning) << boost::format("Ultimaker: Attempting to connect"); // TODO: Remove this. Testing to see if var substitution is the issue
	
	auto res = ConnectionType::error;
	auto url = get_connect_url(false);

	auto http = Http::get(std::move(url));
	set_auth(http);

	BOOST_LOG_TRIVIAL(warning) << boost::format("Ultimaker: Attempting to connect to url %1%") % url;

	http.on_error([&](std::string body, std::string error, unsigned status) {
			auto dsfUrl = get_connect_url(true);
			auto dsfHttp = Http::get(std::move(dsfUrl));
			dsfHttp.on_error([&](std::string body, std::string error, unsigned status) {
					BOOST_LOG_TRIVIAL(error) << boost::format("Ultimaker: Error connecting: %1%, HTTP %2%, body: `%3%`") % error % status % body;
					msg = format_error(body, error, status);
				})
				.on_complete([&](std::string body, unsigned) {
					res = ConnectionType::dsf;
				})
				.perform_sync();
		})
		.on_complete([&](std::string body, unsigned) {
			BOOST_LOG_TRIVIAL(warning) << boost::format("Ultimaker: Got: %1%") % body;

			int err_code = get_err_code_from_body(body);
			switch (err_code) {
				case 0:
					res = ConnectionType::rrf;
					break;
				case 1:
					msg = format_error(body, L("Wrong password"), 0);
					break;
				case 2:
					msg = format_error(body, L("Could not get resources to create a new connection"), 0);
					break;
				default:
					msg = format_error(body, L("Unknown error occurred"), 0);
					break;
			}

		})
		.perform_sync();

	return res;
}

void Ultimaker::disconnect(ConnectionType connectionType) const
{
	// we don't need to disconnect from DSF or if it failed anyway
	if (connectionType != ConnectionType::rrf) {
		return;
	}
	auto url =  (boost::format("%1%rr_disconnect")
			% get_base_url()).str();

	auto http = Http::get(std::move(url));
	http.on_error([&](std::string body, std::string error, unsigned status) {
		// we don't care about it, if disconnect is not working Ultimaker will disconnect automatically after some time
		BOOST_LOG_TRIVIAL(error) << boost::format("Ultimaker: Error disconnecting: %1%, HTTP %2%, body: `%3%`") % error % status % body;
	})
	.perform_sync();
}


#pragma region Get Constants
std::string Ultimaker::get_upload_url(const std::string &filename, ConnectionType connectionType) const
{
	// TODO: fix
    assert(connectionType != ConnectionType::error);

	if (connectionType == ConnectionType::dsf) {
		return (boost::format("%1%machine/file/gcodes/%2%")
				% get_base_url()
				% Http::url_encode(filename)).str();
	} else {
		return (boost::format("%1%rr_upload?name=0:/gcodes/%2%&%3%")
				% get_base_url()
				% Http::url_encode(filename)
				% timestamp_str()).str();
	}
}

std::string Ultimaker::get_status_url() const
{
	return (boost::format("%1%/printer/status") % get_base_url()).str();
}

std::string Ultimaker::get_connect_url(const bool dsfUrl) const
{
	if (dsfUrl)	{
		return (boost::format("%1%/printer/status")
				% get_base_url()).str();
	} else {
		return (boost::format("%1%/printer/status")
				% get_base_url()
				// % Http::url_encode(password.empty() ? "reprap" : password) // url_encode is needed because password can contain special characters like `&`, "#", etc.
				// % timestamp_str()
				).str();
	}
}

std::string Ultimaker::get_base_url() const
{
	if (host.find("http://") == 0 || host.find("https://") == 0) {
		if (host.back() == '/') {
			return host;
		} else {
			return (boost::format("%1%/api/v1") % host).str();
		}
	} else {
		return (boost::format("http://%1%/api/v1") % host).str();
	}
}

std::string Ultimaker::timestamp_str() const
{
	enum { BUFFER_SIZE = 32 };

	auto t = std::time(nullptr);
	auto tm = *std::localtime(&t);

	char buffer[BUFFER_SIZE];
	std::strftime(buffer, BUFFER_SIZE, "time=%Y-%m-%dT%H:%M:%S", &tm);

	return std::string(buffer);
}

#pragma endregion Get Cconstants

bool Ultimaker::start_print(wxString &msg, const std::string &filename, ConnectionType connectionType, bool simulationMode) const
{
	// TODO: Fix
    assert(connectionType != ConnectionType::error);

	bool res = false;
	bool dsf = (connectionType == ConnectionType::dsf);

	auto url = dsf
		? (boost::format("%1%machine/code")
			% get_base_url()).str()
		: (boost::format(simulationMode
				? "%1%rr_gcode?gcode=M37%%20P\"0:/gcodes/%2%\""
				: "%1%rr_gcode?gcode=M32%%20\"0:/gcodes/%2%\"")
			% get_base_url()
			% Http::url_encode(filename)).str();

	auto http = (dsf ? Http::post(std::move(url)) : Http::get(std::move(url)));
	set_auth(http);
	if (dsf) {
		http.set_post_body(
				(boost::format(simulationMode
						? "M37 P\"0:/gcodes/%1%\""
						: "M32 \"0:/gcodes/%1%\"")
					% filename).str()
				);
	}
	http.on_error([&](std::string body, std::string error, unsigned status) {
			BOOST_LOG_TRIVIAL(error) << boost::format("Ultimaker: Error starting print: %1%, HTTP %2%, body: `%3%`") % error % status % body;
			msg = format_error(body, error, status);
		})
		.on_complete([&](std::string body, unsigned) {
			BOOST_LOG_TRIVIAL(debug) << boost::format("Ultimaker: Got: %1%") % body;
			res = true;
		})
		.perform_sync();

	return res;
}

int Ultimaker::get_err_code_from_body(const std::string &body) const
{
	pt::ptree root;
	std::istringstream iss (body); // wrap returned json to istringstream
	pt::read_json(iss, root);

	return root.get<int>("err", 0);
}

}
