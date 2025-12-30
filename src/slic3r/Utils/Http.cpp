#include "Http.hpp"

#include <cstdlib>
#include <functional>
#include <thread>
#include <deque>
#include <sstream>
#include <exception>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include <curl/curl.h>

#ifdef OPENSSL_CERT_OVERRIDE
#include <openssl/x509.h>
#endif

namespace fs = boost::filesystem;

namespace Slic3r {

// Private

struct CurlGlobalInit
{
    static std::unique_ptr<CurlGlobalInit> instance;
    std::string message;

	CurlGlobalInit()
    {
#ifdef OPENSSL_CERT_OVERRIDE // defined if SLIC3R_STATIC=ON

        // Look for a set of distro specific directories. Don't change the
        // order: https://bugzilla.redhat.com/show_bug.cgi?id=1053882
        static const char * CA_BUNDLES[] = {
            "/etc/pki/tls/certs/ca-bundle.crt",   // Fedora/RHEL 6
            "/etc/ssl/certs/ca-certificates.crt", // Debian/Ubuntu/Gentoo etc.
            "/usr/share/ssl/certs/ca-bundle.crt",
            "/usr/local/share/certs/ca-root-nss.crt", // FreeBSD
            "/etc/ssl/cert.pem",
            "/etc/ssl/ca-bundle.pem"              // OpenSUSE Tumbleweed
        };

        namespace fs = boost::filesystem;
        // Env var name for the OpenSSL CA bundle (SSL_CERT_FILE nomally)
        const char *const SSL_CA_FILE = X509_get_default_cert_file_env();
        const char * ssl_cafile = ::getenv(SSL_CA_FILE);

        if (!ssl_cafile)
            ssl_cafile = X509_get_default_cert_file();

        int replace = true;
        if (!ssl_cafile || !fs::exists(fs::path(ssl_cafile))) {
            const char * bundle = nullptr;
            for (const char * b : CA_BUNDLES) {
                if (fs::exists(fs::path(b))) {
                    ::setenv(SSL_CA_FILE, bundle = b, replace);
                    break;
                }
            }

            if (!bundle)
                message = "Unable to get system certificate.";
            else
                message = (boost::format("use system SSL certificate: %1%") % bundle).str();

             message += "\n" + (boost::format("To manually specify the system certificate store, "
                                                   "set the %1% environment variable to the correct CA and restart the application") % SSL_CA_FILE).str();
        }
#endif // OPENSSL_CERT_OVERRIDE

        if (CURLcode ec = ::curl_global_init(CURL_GLOBAL_DEFAULT)) {
            message += "CURL initialization failed. See the log for additional details.";
            BOOST_LOG_TRIVIAL(error) << ::curl_easy_strerror(ec);
        }
    }

	~CurlGlobalInit() { ::curl_global_cleanup(); }
};

std::unique_ptr<CurlGlobalInit> CurlGlobalInit::instance;

std::map<std::string, std::string> extra_headers;
std::mutex g_mutex;

struct form_file
{
    fs::ifstream                          ifs;
    boost::filesystem::ifstream::off_type init_offset;
    size_t                                content_length;

    form_file(fs::path const& p, const boost::filesystem::ifstream::off_type offset, const size_t content_length)
        : ifs(p, std::ios::in | std::ios::binary), init_offset(offset), content_length(content_length)
    {}
};

struct Http::priv
{
	enum {
		DEFAULT_TIMEOUT_CONNECT = 10,
        DEFAULT_TIMEOUT_MAX = 0,
		DEFAULT_SIZE_LIMIT = 1024 * 1024 * 1024,
	};

	::CURL *curl;
	::curl_httppost *form;
	::curl_httppost *form_end;
	::curl_mime* mime;
	::curl_slist *headerlist;
	// Used for reading the body
	std::string buffer;
	// Used for storing file streams added as multipart form parts
	// Using a deque here because unlike vector it doesn't ivalidate pointers on insertion
	std::deque<form_file> form_files;
	std::string postfields;
	std::string error_buffer;    // Used for CURLOPT_ERRORBUFFER
    std::string headers;
	size_t limit;
	bool cancel;
    std::unique_ptr<form_file> putFile;

	std::thread io_thread;
	Http::CompleteFn completefn;
	Http::ErrorFn errorfn;
	Http::ProgressFn progressfn;
	Http::IPResolveFn ipresolvefn;
	Http::HeaderCallbackFn headerfn;

	priv(const std::string &url);
	~priv();

	static bool ca_file_supported(::CURL *curl);
	static size_t writecb(void *data, size_t size, size_t nmemb, void *userp);
	static int xfercb(void *userp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow);
	static int xfercb_legacy(void *userp, double dltotal, double dlnow, double ultotal, double ulnow);
	static size_t form_file_read_cb(char *buffer, size_t size, size_t nitems, void *userp);
    static size_t headers_cb(char *buffer, size_t size, size_t nitems, void *userp);

	void set_timeout_connect(long timeout);
    void set_timeout_max(long timeout);
	void form_add_file(const char *name, const fs::path &path, const char* filename, boost::filesystem::ifstream::off_type offset, size_t length);
	/* mime */
	void mime_form_add_text(const char* name, const char* value);
	void mime_form_add_file(const char* name, const char* path);
	void set_post_body(const fs::path &path);
	void set_post_body(const std::string &body);
	void set_put_body(const fs::path &path);
	void set_del_body(const std::string& body);
    void set_range(const std::string &range);

	std::string curl_error(CURLcode curlcode);
	std::string body_size_error();
	void http_perform();
};

// add a dummy log callback
static int log_trace(CURL* handle, curl_infotype type,
	char* data, size_t size,
	void* userp)
{
	return 0;
}

Http::priv::priv(const std::string &url)
	: curl(::curl_easy_init())
	, form(nullptr)
	, form_end(nullptr)
	, mime(nullptr)
	, headerlist(nullptr)
	, error_buffer(CURL_ERROR_SIZE + 1, '\0')
	, limit(0)
	, cancel(false)
{
    Http::tls_global_init();

	if (curl == nullptr) {
		throw Slic3r::RuntimeError(std::string("Could not construct Curl object"));
	}

	set_timeout_connect(DEFAULT_TIMEOUT_CONNECT);
    set_timeout_max(DEFAULT_TIMEOUT_MAX);
	::curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, log_trace);
	::curl_easy_setopt(curl, CURLOPT_URL, url.c_str());   // curl makes a copy internally
	::curl_easy_setopt(curl, CURLOPT_USERAGENT, SLIC3R_APP_NAME "/" SoftFever_VERSION);
	::curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, &error_buffer.front());
#ifdef __WINDOWS__
	::curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_MAX_TLSv1_2);
#endif
	::curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
	::curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

	// https://everything.curl.dev/http/post/expect100.html
	// remove the Expect: header, it will add a second delay to each request,
	// if the file is uploaded in packets, it will cause the upload time to be longer
	headerlist = curl_slist_append(headerlist, "Expect:");
}

Http::priv::~priv()
{
	::curl_easy_cleanup(curl);
	::curl_formfree(form);
	::curl_mime_free(mime);
	::curl_slist_free_all(headerlist);
}

bool Http::priv::ca_file_supported(::CURL *curl)
{
	//BBS support set ca file by default
	bool res = true;

	if (curl == nullptr) { return res; }

#if LIBCURL_VERSION_MAJOR >= 7 && LIBCURL_VERSION_MINOR >= 48
	::curl_tlssessioninfo *tls;
	if (::curl_easy_getinfo(curl, CURLINFO_TLS_SSL_PTR, &tls) == CURLE_OK) {
		if (tls->backend == CURLSSLBACKEND_SCHANNEL || tls->backend == CURLSSLBACKEND_DARWINSSL) {
			// With Windows and OS X native SSL support, cert files cannot be set
			res = false;
		}
	}
#endif

	return res;
}

size_t Http::priv::writecb(void *data, size_t size, size_t nmemb, void *userp)
{
	auto self = static_cast<priv*>(userp);
	const char *cdata = static_cast<char*>(data);
	const size_t realsize = size * nmemb;

	const size_t limit = self->limit > 0 ? self->limit : DEFAULT_SIZE_LIMIT;
	if (self->buffer.size() + realsize > limit) {
		// This makes curl_easy_perform return CURLE_WRITE_ERROR
		return 0;
	}

	self->buffer.append(cdata, realsize);

	return realsize;
}

int Http::priv::xfercb(void *userp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
	auto self = static_cast<priv*>(userp);
	bool cb_cancel = false;

	if (self->progressfn) {
		double speed;
        curl_easy_getinfo(self->curl, CURLINFO_SPEED_UPLOAD, &speed);
		if (speed > 0.01)
			speed = speed;
		Progress progress(dltotal, dlnow, ultotal, ulnow, self->buffer, speed);
		self->progressfn(progress, cb_cancel);
	}

	if (cb_cancel) { self->cancel = true; }

	return self->cancel;
}

int Http::priv::xfercb_legacy(void *userp, double dltotal, double dlnow, double ultotal, double ulnow)
{
	return xfercb(userp, dltotal, dlnow, ultotal, ulnow);
}

size_t Http::priv::form_file_read_cb(char *buffer, size_t size, size_t nitems, void *userp)
{
    auto f = reinterpret_cast<form_file*>(userp);

	try {
	    size_t max_read_size = size * nitems;
        if (f->content_length == 0) {
			// Unlimited
            f->ifs.read(buffer, max_read_size);
        } else {
            unsigned long long read_size = f->ifs.tellg() - f->init_offset;
            if (read_size >= f->content_length) {
                return 0;
            }

            max_read_size = std::min(max_read_size, size_t(f->content_length - read_size));
            f->ifs.read(buffer, max_read_size);
        }
	} catch (const std::exception &) {
		return CURL_READFUNC_ABORT;
	}

	return f->ifs.gcount();
}

size_t Http::priv::headers_cb(char *buffer, size_t size, size_t nitems, void *userp)
{
	auto self = static_cast<priv*>(userp);

	if (self->headerfn) {
        self->headers.append(buffer, nitems * size);
		self->headerfn(self->headers);
	}
	return nitems * size;
}

void Http::priv::set_timeout_connect(long timeout)
{
	::curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout);
}

void Http::priv::set_timeout_max(long timeout)
{
    ::curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
}

void Http::priv::form_add_file(const char *name, const fs::path &path, const char* filename, boost::filesystem::ifstream::off_type offset, size_t length)
{
	// We can't use CURLFORM_FILECONTENT, because curl doesn't support Unicode filenames on Windows
	// and so we use CURLFORM_STREAM with boost ifstream to read the file.

	if (filename == nullptr) {
		filename = path.string().c_str();
	}

	form_files.emplace_back(path, offset, length);
	auto &f = form_files.back();
    size_t size = length;
    if (length == 0) {
        f.ifs.seekg(0, std::ios::end);
        size = f.ifs.tellg() - offset;
    }
    f.ifs.seekg(offset);

	if (filename != nullptr) {
		::curl_formadd(&form, &form_end,
			CURLFORM_COPYNAME, name,
			CURLFORM_FILENAME, filename,
			CURLFORM_CONTENTTYPE, "application/octet-stream",
			CURLFORM_STREAM, static_cast<void*>(&f),
			CURLFORM_CONTENTSLENGTH, static_cast<long>(size),
			CURLFORM_END
		);
	}
}

void Http::priv::mime_form_add_text(const char* name, const char* value)
{
	if (!mime) {
		mime = curl_mime_init(curl);
	}

	curl_mimepart *part;
	part = curl_mime_addpart(mime);
	curl_mime_name(part, name);
	curl_mime_type(part, "multipart/form-data");
	curl_mime_data(part, value, CURL_ZERO_TERMINATED);
}

void Http::priv::mime_form_add_file(const char* name, const char* path)
{
	if (!mime) {
		mime = curl_mime_init(curl);
	}

	curl_mimepart* part;
	part = curl_mime_addpart(mime);
	curl_mime_name(part, "file");
	curl_mime_type(part, "multipart/form-data");
	curl_mime_filedata(part, path);
	// BBS specify filename after filedata
	curl_mime_filename(part, name);
}

//FIXME may throw! Is the caller aware of it?
void Http::priv::set_post_body(const fs::path &path)
{
	std::ifstream file(path.string());
	std::string file_content { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
	postfields = std::move(file_content);
}

void Http::priv::set_post_body(const std::string &body)
{
	postfields = body;
}

void Http::priv::set_put_body(const fs::path &path)
{
	boost::system::error_code ec;
	boost::uintmax_t filesize = file_size(path, ec);
	if (!ec) {
        putFile = std::make_unique<form_file>(path, 0, 0);
		::curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
		::curl_easy_setopt(curl, CURLOPT_READDATA, (void *) (putFile.get()));
		::curl_easy_setopt(curl, CURLOPT_INFILESIZE, filesize);
	}
}

void Http::priv::set_del_body(const std::string& body)
{
	postfields = body;
}

void Http::priv::set_range(const std::string& range)
{
	::curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
}

std::string Http::priv::curl_error(CURLcode curlcode)
{
	return (boost::format("curl:%1%:\n%2%\n[Error %3%]")
		% ::curl_easy_strerror(curlcode)
		% error_buffer.c_str()
		% curlcode
	).str();
}

std::string Http::priv::body_size_error()
{
	return (boost::format("HTTP body data size exceeded limit (%1% bytes)") % limit).str();
}

void Http::priv::http_perform()
{
	::curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	::curl_easy_setopt(curl, CURLOPT_POSTREDIR, CURL_REDIR_POST_ALL);
	::curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writecb);
	::curl_easy_setopt(curl, CURLOPT_WRITEDATA, static_cast<void*>(this));
	::curl_easy_setopt(curl, CURLOPT_READFUNCTION, form_file_read_cb);
	//BBS set header functions
	::curl_easy_setopt(curl, CURLOPT_HEADERDATA, static_cast<void *>(this));
	::curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headers_cb);

	::curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
#if LIBCURL_VERSION_MAJOR >= 7 && LIBCURL_VERSION_MINOR >= 32
	::curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xfercb);
	::curl_easy_setopt(curl, CURLOPT_XFERINFODATA, static_cast<void*>(this));
#ifndef _WIN32
	(void)xfercb_legacy;   // prevent unused function warning
#endif
#else
	::curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, xfercb);
	::curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, static_cast<void*>(this));
#endif

	::curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

	if (headerlist != nullptr) {
		::curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerlist);
	}

	if (form != nullptr) {
		::curl_easy_setopt(curl, CURLOPT_HTTPPOST, form);
	}

	if (mime != nullptr) {
		::curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
	}

	if (!postfields.empty()) {
		::curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postfields.c_str());
		::curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, postfields.size());
	}

	CURLcode res = ::curl_easy_perform(curl);

    putFile.reset();

	if (res != CURLE_OK) {
		if (res == CURLE_ABORTED_BY_CALLBACK) {
			if (cancel) {
				// The abort comes from the request being cancelled programatically
				Progress dummyprogress(0, 0, 0, 0, std::string());
				bool cancel = true;
				if (progressfn) { progressfn(dummyprogress, cancel); }
			} else {
				// The abort comes from the CURLOPT_READFUNCTION callback, which means reading file failed
				if (errorfn) { errorfn(std::move(buffer), "Error reading file for file upload", 0); }
			}
		}
		else if (res == CURLE_WRITE_ERROR) {
			if (errorfn) { errorfn(std::move(buffer), body_size_error(), 0); }
		} else {
			if (errorfn) { errorfn(std::move(buffer), curl_error(res), 0); }
		};
	} else {
		long http_status = 0;
		::curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);

		//BBS check success http status code
		if (http_status >= 200 && http_status < 300) {
			if (completefn) { completefn(std::move(buffer), http_status); }
			if (ipresolvefn) {
				char* ct;
				res = curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &ct);
				if ((CURLE_OK == res) && ct) {
					ipresolvefn(ct);
				}
			}
		}
		//BBS check error http status code
		else if (http_status >= 400) {
			if (errorfn) { errorfn(std::move(buffer), std::string(), http_status); }
		}
	}
}

Http::Http(const std::string &url) : p(new priv(url)) {

    std::lock_guard<std::mutex> l(g_mutex);
	for (auto it = extra_headers.begin(); it != extra_headers.end(); it++)
		this->header(it->first, it->second);
}


// Public

Http::Http(Http &&other) : p(std::move(other.p)) {}

Http::~Http()
{
    assert(! p || ! p->putFile);
	if (p && p->io_thread.joinable()) {
		p->io_thread.detach();
	}
}


Http& Http::timeout_connect(long timeout)
{
	if (timeout < 1) { timeout = priv::DEFAULT_TIMEOUT_CONNECT; }
	if (p) { p->set_timeout_connect(timeout); }
	return *this;
}

Http& Http::timeout_max(long timeout)
{
    if (timeout < 1) { timeout = priv::DEFAULT_TIMEOUT_MAX; }
    if (p) { p->set_timeout_max(timeout); }
    return *this;
}

Http& Http::size_limit(size_t sizeLimit)
{
	if (p) { p->limit = sizeLimit; }
	return *this;
}

Http& Http::set_range(const std::string& range)
{
	if (p) { p->set_range(range); }
	return *this;
}

Http& Http::header(std::string name, const std::string &value)
{
	if (!p) { return * this; }

	if (name.size() > 0) {
		name.append(": ").append(value);
	} else {
		name.push_back(':');
	}
	p->headerlist = curl_slist_append(p->headerlist, name.c_str());
	return *this;
}

Http& Http::remove_header(std::string name)
{
	if (p) {
		name.push_back(':');
		p->headerlist = curl_slist_append(p->headerlist, name.c_str());
	}

	return *this;
}

// Authorization by HTTP digest, based on RFC2617.
Http& Http::auth_digest(const std::string &user, const std::string &password)
{
	curl_easy_setopt(p->curl, CURLOPT_USERNAME, user.c_str());
	curl_easy_setopt(p->curl, CURLOPT_PASSWORD, password.c_str());
	curl_easy_setopt(p->curl, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);

	return *this;
}

Http& Http::auth_basic(const std::string &user, const std::string &password)
{
    curl_easy_setopt(p->curl, CURLOPT_USERNAME, user.c_str());
    curl_easy_setopt(p->curl, CURLOPT_PASSWORD, password.c_str());
    curl_easy_setopt(p->curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);

    return *this;
}

Http& Http::ca_file(const std::string &name)
{
	if (p && priv::ca_file_supported(p->curl)) {
		::curl_easy_setopt(p->curl, CURLOPT_CAINFO, name.c_str());
	}

	return *this;
}

Http& Http::form_clear() {
	if (p) {
        if (p->form) {
            ::curl_formfree(p->form);
            p->form     = nullptr;
            p->form_end = nullptr;
        }
		for (auto &f : p->form_files) {
			f.ifs.close();
		}
		p->form_files.clear();

	}
	return *this;
}

Http& Http::form_add(const std::string &name, const std::string &contents)
{
	if (p) {
		::curl_formadd(&p->form, &p->form_end,
			CURLFORM_COPYNAME, name.c_str(),
			CURLFORM_COPYCONTENTS, contents.c_str(),
			CURLFORM_END
		);
	}

	return *this;
}

Http& Http::form_add_file(const std::string &name, const fs::path &path, boost::filesystem::ifstream::off_type offset, size_t length)
{
	if (p) { p->form_add_file(name.c_str(), path.c_str(), nullptr, offset, length); }
	return *this;
}


Http& Http::mime_form_add_text(std::string &name, std::string &value)
{
	if (p) { p->mime_form_add_text(name.c_str(), value.c_str()); }
	return *this;
}

Http& Http::mime_form_add_file(std::string &name, const char* path)
{
	if (p) { p->mime_form_add_file(name.c_str(), path); }
	return *this;
}


Http& Http::form_add_file(const std::wstring& name, const fs::path& path, boost::filesystem::ifstream::off_type offset, size_t length)
{
	if (p) { p->form_add_file((char*)name.c_str(), path.c_str(), nullptr, offset, length); }
	return *this;
}

Http& Http::form_add_file(const std::string &name, const fs::path &path, const std::string &filename, boost::filesystem::ifstream::off_type offset, size_t length)
{
	if (p) { p->form_add_file(name.c_str(), path.c_str(), filename.c_str(), offset, length); }
	return *this;
}

#ifdef WIN32
// Tells libcurl to ignore certificate revocation checks in case of missing or offline distribution points for those SSL backends where such behavior is present.
// This option is only supported for Schannel (the native Windows SSL library).
Http& Http::ssl_revoke_best_effort(bool set)
{
	// BBS
#if 0
	if(p && set){
		::curl_easy_setopt(p->curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_REVOKE_BEST_EFFORT);
	}
#endif
	return *this;
}
#endif // WIN32

Http& Http::set_post_body(const fs::path &path)
{
	if (p) { p->set_post_body(path);}
	return *this;
}

Http& Http::set_post_body(const std::string &body)
{
	if (p) { p->set_post_body(body); }
	return *this;
}

Http& Http::set_put_body(const fs::path &path)
{
	if (p) { p->set_put_body(path);}
	return *this;
}

Http& Http::set_del_body(const std::string &body)
{
	if (p) { p->set_del_body(body); }
	return *this;
}

Http& Http::on_complete(CompleteFn fn)
{
	if (p) { p->completefn = std::move(fn); }
	return *this;
}

Http& Http::on_error(ErrorFn fn)
{
	if (p) { p->errorfn = std::move(fn); }
	return *this;
}

Http& Http::on_progress(ProgressFn fn)
{
	if (p) { p->progressfn = std::move(fn); }
	return *this;
}

Http& Http::on_ip_resolve(IPResolveFn fn)
{
	if (p) { p->ipresolvefn = std::move(fn); }
	return *this;
}

Http &Http::on_header_callback(HeaderCallbackFn fn)
{
	if (p) { p->headerfn = std::move(fn); }
	return *this;
}

Http::Ptr Http::perform()
{
	auto self = std::make_shared<Http>(std::move(*this));

	if (self->p) {
		auto io_thread = std::thread([self](){
				self->p->http_perform();
			});
		self->p->io_thread = std::move(io_thread);
	}

	return self;
}

void Http::perform_sync()
{
	if (p) { p->http_perform(); }
}

void Http::cancel()
{
	if (p) { p->cancel = true; }
}

Http Http::get(std::string url)
{
    return Http{std::move(url)};
}

Http Http::post(std::string url)
{
	Http http{std::move(url)};
	curl_easy_setopt(http.p->curl, CURLOPT_POST, 1L);
	return http;
}

Http Http::put(std::string url)
{
	Http http{std::move(url)};
	curl_easy_setopt(http.p->curl, CURLOPT_UPLOAD, 1L);
	return http;
}

Http Http::put2(std::string url)
{
	Http http{ std::move(url) };
	curl_easy_setopt(http.p->curl, CURLOPT_CUSTOMREQUEST, "PUT");
	return http;
}

Http Http::patch(std::string url)
{
	Http http{ std::move(url) };
	curl_easy_setopt(http.p->curl, CURLOPT_CUSTOMREQUEST, "PATCH");
	return http;
}

Http Http::del(std::string url)
{
	Http http{ std::move(url) };
	curl_easy_setopt(http.p->curl, CURLOPT_CUSTOMREQUEST, "DELETE");
	return http;
}

void Http::set_extra_headers(std::map<std::string, std::string> headers)
{
    std::lock_guard<std::mutex> l(g_mutex);
	extra_headers.swap(headers);
}

std::map<std::string, std::string> Http::get_extra_headers()
{
    std::lock_guard<std::mutex> l(g_mutex);
    return extra_headers;
}

bool Http::ca_file_supported()
{
	::CURL *curl = ::curl_easy_init();
	bool res = priv::ca_file_supported(curl);
	if (curl != nullptr) { ::curl_easy_cleanup(curl); }
    return res;
}

std::string Http::tls_global_init()
{
    if (!CurlGlobalInit::instance)
        CurlGlobalInit::instance = std::make_unique<CurlGlobalInit>();

    return CurlGlobalInit::instance->message;
}

std::string Http::tls_system_cert_store()
{
    std::string ret;

#ifdef OPENSSL_CERT_OVERRIDE
    ret = ::getenv(X509_get_default_cert_file_env());
#endif

    return ret;
}

std::string Http::url_encode(const std::string &str)
{
	::CURL *curl = ::curl_easy_init();
	if (curl == nullptr) {
		return str;
	}
	char *ce = ::curl_easy_escape(curl, str.c_str(), str.length());
	std::string encoded = std::string(ce);

	::curl_free(ce);
	::curl_easy_cleanup(curl);

	return encoded;
}

std::string Http::url_decode(const std::string &str)
{
    ::CURL *curl = ::curl_easy_init();
    if (curl == nullptr) { return str; }
    int outlen = 0;
    char *ce = ::curl_easy_unescape(curl, str.c_str(), str.length(), &outlen);
    std::string dencoded = std::string(ce, outlen);

    ::curl_free(ce);
    ::curl_easy_cleanup(curl);

    return dencoded;
}

std::string Http::get_filename_from_url(const std::string &url)
{
    int end_pos = url.find_first_of('?');
	if (end_pos <= 0) return "";
	std::string path_url = url.substr(0, end_pos);
	int start_pos = path_url.find_last_of("/");
	if (start_pos < 0) return "";
	return path_url.substr(start_pos + 1, path_url.length() - start_pos - 1);
}

std::ostream& operator<<(std::ostream &os, const Http::Progress &progress)
{
	os << "Http::Progress("
		<< "dltotal = " << progress.dltotal
		<< ", dlnow = " << progress.dlnow
		<< ", ultotal = " << progress.ultotal
		<< ", ulnow = " << progress.ulnow
		<< ")";
	return os;
}


}
