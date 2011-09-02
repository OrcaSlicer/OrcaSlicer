#include "Version.hpp"

#include <cctype>

#include <boost/filesystem/operations.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Config.hpp"
#include "libslic3r/FileParserError.hpp"
#include "libslic3r/Utils.hpp"

namespace Slic3r { 
namespace GUI {
namespace Config {


// Optimized lexicographic compare of two pre-release versions, ignoring the numeric suffix.
static int compare_prerelease(const char *p1, const char *p2)
{
	for (;;) {
		char c1 = *p1 ++;
		char c2 = *p2 ++;
		bool a1 = std::isalpha(c1) && c1 != 0;
		bool a2 = std::isalpha(c2) && c2 != 0;
		if (a1) {
			if (a2) {
				if (c1 != c2)
					return (c1 < c2) ? -1 : 1;
			} else
				return 1;
		} else {
			if (a2)
				return -1;
			else
				return 0;
		}
	}
	// This shall never happen.
	return 0;
}

bool Version::is_slic3r_supported(const Semver &slic3r_version) const
{ 
	if (! slic3r_version.in_range(min_slic3r_version, max_slic3r_version))
		return false;
	// Now verify, whether the configuration pre-release status is compatible with the Slic3r's pre-release status.
	// Alpha Slic3r will happily load any configuration, while beta Slic3r will ignore alpha configurations etc.
	const char *prerelease_slic3r = slic3r_version.prerelease();
	const char *prerelease_config = this->config_version.prerelease();
	if (prerelease_config == nullptr)
		// Released config is always supported.
		return true;
	else if (prerelease_slic3r == nullptr)
		// Released slic3r only supports released configs.
		return false;
	// Compare the pre-release status of Slic3r against the config.
	// If the prerelease status of slic3r is lexicographically lower or equal 
	// to the prerelease status of the config, accept it.
	return compare_prerelease(prerelease_slic3r, prerelease_config) != 1;
}

bool Version::is_current_slic3r_supported() const
{
	return this->is_slic3r_supported(Slic3r::SEMVER);
}

bool Version::is_current_slic3r_downgrade() const
{
	return Slic3r::SEMVER < min_slic3r_version;
}
#if 0
//TODO: This test should be moved to a unit test, once we have C++ unit tests in place.
static int version_test()
{
	Version v;
	v.config_version 	 = *Semver::parse("1.1.2");
	v.min_slic3r_version = *Semver::parse("1.38.0");
	v.max_slic3r_version = Semver::inf();
	assert(v.is_slic3r_supported(*Semver::parse("1.38.0")));
	assert(! v.is_slic3r_supported(*Semver::parse("1.38.0-alpha")));
	assert(! v.is_slic3r_supported(*Semver::parse("1.37.0-alpha")));
	// Test the prerelease status.
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-alpha")));
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-alpha1")));
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-alpha1")));
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-beta")));
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-beta1")));
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-beta1")));
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-rc2")));
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0")));
	v.config_version 	 = *Semver::parse("1.1.2-alpha");
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-alpha")));
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-alpha1")));
	assert(! v.is_slic3r_supported(*Semver::parse("1.39.0-beta")));
	assert(! v.is_slic3r_supported(*Semver::parse("1.39.0-beta1")));
	assert(! v.is_slic3r_supported(*Semver::parse("1.39.0-beta1")));
	assert(! v.is_slic3r_supported(*Semver::parse("1.39.0-rc2")));
	assert(! v.is_slic3r_supported(*Semver::parse("1.39.0")));
	v.config_version 	 = *Semver::parse("1.1.2-alpha1");
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-alpha")));
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-alpha1")));
	assert(! v.is_slic3r_supported(*Semver::parse("1.39.0-beta")));
	assert(! v.is_slic3r_supported(*Semver::parse("1.39.0-beta1")));
	assert(! v.is_slic3r_supported(*Semver::parse("1.39.0-beta1")));
	assert(! v.is_slic3r_supported(*Semver::parse("1.39.0-rc2")));
	assert(! v.is_slic3r_supported(*Semver::parse("1.39.0")));
	v.config_version 	 = *Semver::parse("1.1.2-beta");
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-alpha")));
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-alpha1")));
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-beta")));
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-beta1")));
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-beta1")));
	assert(! v.is_slic3r_supported(*Semver::parse("1.39.0-rc")));
	assert(! v.is_slic3r_supported(*Semver::parse("1.39.0-rc2")));
	assert(! v.is_slic3r_supported(*Semver::parse("1.39.0")));
	v.config_version 	 = *Semver::parse("1.1.2-rc");
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-alpha")));
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-alpha1")));
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-beta")));
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-beta1")));
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-beta1")));
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-rc")));
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-rc2")));
	assert(! v.is_slic3r_supported(*Semver::parse("1.39.0")));
	v.config_version 	 = *Semver::parse("1.1.2-rc2");
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-alpha")));
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-alpha1")));
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-beta")));
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-beta1")));
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-beta1")));
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-rc")));
	assert(v.is_slic3r_supported(*Semver::parse("1.39.0-rc2")));
	assert(! v.is_slic3r_supported(*Semver::parse("1.39.0")));
	// Test the upper boundary.
	v.config_version 	 = *Semver::parse("1.1.2");
	v.max_slic3r_version = *Semver::parse("1.39.3-beta1");
	assert(v.is_slic3r_supported(*Semver::parse("1.38.0")));
	assert(! v.is_slic3r_supported(*Semver::parse("1.38.0-alpha")));
	assert(! v.is_slic3r_supported(*Semver::parse("1.38.0-alpha1")));
	assert(! v.is_slic3r_supported(*Semver::parse("1.37.0-alpha")));
	return 0;
}
static int version_test_run = version_test();
#endif

inline char* left_trim(char *c)
{
	for (; *c == ' ' || *c == '\t'; ++ c);
	return c;
}

inline char* right_trim(char *start)
{
	char *end = start + strlen(start) - 1;
	for (; end >= start && (*end == ' ' || *end == '\t'); -- end);
	*(++ end) = 0;
	return end;
}

inline std::string unquote_value(char *value, char *end, const std::string &path, int idx_line)
{
	std::string svalue;
	if (value == end) {
		// Empty string is a valid string.
	} else if (*value == '"') {
		if (++ value > -- end || *end != '"')
			throw file_parser_error("String not enquoted correctly", path, idx_line);
		*end = 0;
		if (! unescape_string_cstyle(value, svalue))
			throw file_parser_error("Invalid escape sequence inside a quoted string", path, idx_line);
	} else
		svalue.assign(value, end);
	return svalue;
}

inline std::string unquote_version_comment(char *value, char *end, const std::string &path, int idx_line)
{
	std::string svalue;
	if (value == end) {
		// Empty string is a valid string.
	} else if (*value == '"') {
		if (++ value > -- end || *end != '"')
			throw file_parser_error("Version comment not enquoted correctly", path, idx_line);
		*end = 0;
		if (! unescape_string_cstyle(value, svalue))
			throw file_parser_error("Invalid escape sequence inside a quoted version comment", path, idx_line);
	} else
		svalue.assign(value, end);
	return svalue;
}

size_t Index::load(const boost::filesystem::path &path)
{
	m_configs.clear();
	m_vendor = path.stem().string();
	m_path = path;

    boost::nowide::ifstream ifs(path.string());
    std::string line;
    size_t idx_line = 0;
    Version ver;
    while (std::getline(ifs, line)) {
#ifndef _MSVCVER
		// On a Unix system, getline does not remove the trailing carriage returns, if the index is shared over a Windows filesystem. Remove them manually.
		while (! line.empty() && line.back() == '\r')
			line.pop_back();
#endif
    	++ idx_line;
    	// Skip the initial white spaces.
    	char *key = left_trim(line.data());
		if (*key == '#')
			// Skip a comment line.
			continue;
		// Right trim the line.
		char *end = right_trim(key);
        if (key == end)
            // Skip an empty line.
            continue;
		// Keyword may only contain alphanumeric characters. Semantic version may in addition contain "+.-".
    	char *key_end = key;
    	bool  maybe_semver = true;
		for (; *key_end != 0; ++ key_end) {
			if (std::isalnum(*key_end) || strchr("+.-", *key_end) != nullptr) {
				// It may be a semver.
			} else if (*key_end == '_') {
				// Cannot be a semver, but it may be a key.
				maybe_semver = false;
			} else
				// End of semver or keyword.
				break;
    	}
        if (*key_end != 0 && *key_end != ' ' && *key_end != '\t' && *key_end != '=')
    		throw file_parser_error("Invalid keyword or semantic version", path, idx_line);
        char *value = left_trim(key_end);
		bool  key_value_pair = *value == '=';
		if (key_value_pair)
			value = left_trim(value + 1);
		*key_end = 0;
    	boost::optional<Semver> semver;
        if (maybe_semver)
    		semver = Semver::parse(key);
        if (key_value_pair) {
    		if (semver)
    			throw file_parser_error("Key cannot be a semantic version", path, idx_line);\
    		// Verify validity of the key / value pair.
			std::string svalue = unquote_value(value, end, path.string(), idx_line);
    		if (strcmp(key, "min_slic3r_version") == 0 || strcmp(key, "max_slic3r_version") == 0) {
    			if (! svalue.empty())
					semver = Semver::parse(svalue);
                if (! semver)
		    		throw file_parser_error(std::string(key) + " must referece a valid semantic version", path, idx_line);
                if (strcmp(key, "min_slic3r_version") == 0)
    				ver.min_slic3r_version = *semver;
                else
    				ver.max_slic3r_version = *semver;
    		} else {
    			// Ignore unknown keys, as there may come new keys in the future.
    		}
			continue;
    	}
		if (! semver)
			throw file_parser_error("Invalid semantic version", path, idx_line);
		ver.config_version = *semver;
		ver.comment = (end <= key_end) ? "" : unquote_version_comment(value, end, path.string(), idx_line);
		m_configs.emplace_back(ver);
    }

    // Sort the configs by their version.
    std::sort(m_configs.begin(), m_configs.end(), [](const Version &v1, const Version &v2) { return v1.config_version < v2.config_version; });
    return m_configs.size();
}

Semver Index::version() const
{
    Semver ver = Semver::zero();
    for (const Version &cv : m_configs)
        if (cv.config_version >= ver)
            ver = cv.config_version;
    return ver;
}

Index::const_iterator Index::find(const Semver &ver) const
{ 
	Version key;
	key.config_version = ver;
	auto it = std::lower_bound(m_configs.begin(), m_configs.end(), key, 
		[](const Version &v1, const Version &v2) { return v1.config_version < v2.config_version; });
	return (it == m_configs.end() || it->config_version == ver) ? it : m_configs.end();
}

Index::const_iterator Index::recommended(const Semver &slic3r_version) const
{
	const_iterator highest = this->end();
	for (const_iterator it = this->begin(); it != this->end(); ++ it)
		if (it->is_slic3r_supported(slic3r_version) &&
			(highest == this->end() || highest->config_version < it->config_version))
			highest = it;
	return highest;
}

Index::const_iterator Index::recommended() const
{
	return this->recommended(Slic3r::SEMVER);
}

std::vector<Index> Index::load_db()
{
    boost::filesystem::path cache_dir = boost::filesystem::path(Slic3r::data_dir()) / "ota";

    std::vector<Index> index_db;
    std::string errors_cummulative;
	for (auto &dir_entry : boost::filesystem::directory_iterator(cache_dir))
        if (Slic3r::is_idx_file(dir_entry)) {
        	Index idx;
            try {
            	idx.load(dir_entry.path());
            } catch (const std::runtime_error &err) {
                errors_cummulative += err.what();
                errors_cummulative += "\n";
                continue;
			}
            index_db.emplace_back(std::move(idx));
        }

    if (! errors_cummulative.empty())
        throw Slic3r::RuntimeError(errors_cummulative);
    return index_db;
}

} // namespace Config
} // namespace GUI
} // namespace Slic3r
