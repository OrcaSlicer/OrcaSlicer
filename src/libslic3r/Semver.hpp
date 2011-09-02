#ifndef slic3r_Semver_hpp_
#define slic3r_Semver_hpp_

#include <string>
#include <cstring>
#include <ostream>
#include <stdexcept>
#include <boost/optional.hpp>
#include <boost/format.hpp>

#include "semver/semver.h"

#include "Exception.hpp"

namespace Slic3r {


class Semver
{
public:
	struct Major { const int i;  Major(int i) : i(i) {} };
	struct Minor { const int i;  Minor(int i) : i(i) {} };
	struct Patch { const int i;  Patch(int i) : i(i) {} };

	Semver() : ver(semver_zero()) {}

	Semver(int major, int minor, int patch,
		boost::optional<const std::string&> metadata, boost::optional<const std::string&> prerelease)
		: ver(semver_zero())
	{
		ver.major = major;
		ver.minor = minor;
		ver.patch = patch;
		set_metadata(metadata);
		set_prerelease(prerelease);
	}

	Semver(int major, int minor, int patch, const char *metadata = nullptr, const char *prerelease = nullptr)
		: ver(semver_zero())
	{
		ver.major = major;
		ver.minor = minor;
		ver.patch = patch;
		set_metadata(metadata);
		set_prerelease(prerelease);
	}

	Semver(const std::string &str) : ver(semver_zero())
	{
		auto parsed = parse(str);
		if (! parsed) {
			throw Slic3r::RuntimeError(std::string("Could not parse version string: ") + str);
		}
		ver = parsed->ver;
		parsed->ver = semver_zero();
	}

	static boost::optional<Semver> parse(const std::string &str)
	{
		semver_t ver = semver_zero();
		if (::semver_parse(str.c_str(), &ver) == 0) {
			return Semver(ver);
		} else {
			return boost::none;
		}
	}

	static const Semver zero() { return Semver(semver_zero()); }

	static const Semver inf()
	{
		static semver_t ver = { std::numeric_limits<int>::max(), std::numeric_limits<int>::max(), std::numeric_limits<int>::max(), nullptr, nullptr };
		return Semver(ver);
	}

	static const Semver invalid()
	{
		static semver_t ver = { -1, 0, 0, nullptr, nullptr };
		return Semver(ver);
	}

	Semver(Semver &&other) : ver(other.ver) { other.ver = semver_zero(); }
	Semver(const Semver &other) : ver(::semver_copy(&other.ver)) {}

	Semver &operator=(Semver &&other)
	{
		::semver_free(&ver);
		ver = other.ver;
		other.ver = semver_zero();
		return *this;
	}

	Semver &operator=(const Semver &other)
	{
		::semver_free(&ver);
		ver = ::semver_copy(&other.ver);
		return *this;
	}

	~Semver() { ::semver_free(&ver); }

	// const accessors
	int 		maj()        const { return ver.major; }
	int 		min()        const { return ver.minor; }
	int 		patch() 	 const { return ver.patch; }
	const char*	prerelease() const { return ver.prerelease; }
	const char*	metadata() 	 const { return ver.metadata; }
	
	// Setters
	void set_maj(int maj) { ver.major = maj; }
	void set_min(int min) { ver.minor = min; }
	void set_patch(int patch) { ver.patch = patch; }
    void set_metadata(boost::optional<const std::string &> meta)
    {
        if (ver.metadata)
            free(ver.metadata);
        ver.metadata = meta ? strdup(*meta) : nullptr;
    }
    void set_metadata(const char *meta)
    {
        if (ver.metadata)
            free(ver.metadata);
        ver.metadata = meta ? strdup(meta) : nullptr;
    }
    void set_prerelease(boost::optional<const std::string &> pre)
    {
        if (ver.prerelease)
            free(ver.prerelease);
        ver.prerelease = pre ? strdup(*pre) : nullptr;
    }
    void set_prerelease(const char *pre)
    {
        if (ver.prerelease)
            free(ver.prerelease);
        ver.prerelease = pre ? strdup(pre) : nullptr;
    }

	// Comparison
	bool operator<(const Semver &b)  const { return ::semver_compare(ver, b.ver) == -1; }
	bool operator<=(const Semver &b) const { return ::semver_compare(ver, b.ver) <= 0; }
	bool operator==(const Semver &b) const { return ::semver_compare(ver, b.ver) == 0; }
	bool operator!=(const Semver &b) const { return ::semver_compare(ver, b.ver) != 0; }
	bool operator>=(const Semver &b) const { return ::semver_compare(ver, b.ver) >= 0; }
	bool operator>(const Semver &b)  const { return ::semver_compare(ver, b.ver) == 1; }
	// We're using '&' instead of the '~' operator here as '~' is unary-only:
	// Satisfies patch if Major and minor are equal.
	bool operator&(const Semver &b) const { return ::semver_satisfies_patch(ver, b.ver) != 0; }
	bool operator^(const Semver &b) const { return ::semver_satisfies_caret(ver, b.ver) != 0; }
	bool in_range(const Semver &low, const Semver &high) const { return low <= *this && *this <= high; }
	bool valid()                    const { return *this != zero() && *this != inf() && *this != invalid(); }

	// Conversion
	std::string to_string() const {
		//BBS: version format
		std::string res;
		int patch_1 = ver.patch/100;
		int patch_2 = ver.patch%100;
		res = (boost::format("%1%.%2%.%3%.%4%") % ver.major % ver.minor % patch_1 % patch_2).str();

		if (ver.prerelease != nullptr) { res += '-'; res += ver.prerelease; }
		if (ver.metadata != nullptr)   { res += '+'; res += ver.metadata; }
		return res;
	}
	std::string to_string_sf() const {
		//BBS: version format
		std::string res;
		res = (boost::format("%1%.%2%.%3%") % ver.major % ver.minor % ver.patch).str();

		if (ver.prerelease != nullptr) { res += '-'; res += ver.prerelease; }
		if (ver.metadata != nullptr) { res += '+'; res += ver.metadata; }
		return res;
	}

	// Arithmetics
	Semver& operator+=(const Major &b) { ver.major += b.i; return *this; }
	Semver& operator+=(const Minor &b) { ver.minor += b.i; return *this; }
	Semver& operator+=(const Patch &b) { ver.patch += b.i; return *this; }
	Semver& operator-=(const Major &b) { ver.major -= b.i; return *this; }
	Semver& operator-=(const Minor &b) { ver.minor -= b.i; return *this; }
	Semver& operator-=(const Patch &b) { ver.patch -= b.i; return *this; }
	Semver operator+(const Major &b) const { Semver res(*this); return res += b; }
	Semver operator+(const Minor &b) const { Semver res(*this); return res += b; }
	Semver operator+(const Patch &b) const { Semver res(*this); return res += b; }
	Semver operator-(const Major &b) const { Semver res(*this); return res -= b; }
	Semver operator-(const Minor &b) const { Semver res(*this); return res -= b; }
	Semver operator-(const Patch &b) const { Semver res(*this); return res -= b; }

	// Stream output
	friend std::ostream& operator<<(std::ostream& os, const Semver &self) {
		os << self.to_string();
		return os;
	}
private:
	semver_t ver;

	Semver(semver_t ver) : ver(ver) {}

	static semver_t semver_zero() { return { 0, 0, 0, nullptr, nullptr }; }
	static char * strdup(const std::string &str) { return ::semver_strdup(str.data()); }
};


}
#endif
