#ifndef slic3r_Config_hpp_
#define slic3r_Config_hpp_

#include <assert.h>
#include <map>
#include <climits>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include "libslic3r.h"
#include "clonable_ptr.hpp"
#include "Exception.hpp"
#include "Point.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/format/format_fwd.hpp>
#include <boost/functional/hash.hpp>
#include <boost/property_tree/ptree_fwd.hpp>
#include <boost/log/trivial.hpp>

#include <cereal/access.hpp>
#include <cereal/types/base_class.hpp>

namespace Slic3r {
    struct FloatOrPercent
    {
        double  value;
        bool    percent;
    private:
        friend class cereal::access;
        template<class Archive> void serialize(Archive& ar) { ar(this->value); ar(this->percent); }
    };

    inline bool operator==(const FloatOrPercent& l, const FloatOrPercent& r) throw() { return l.value == r.value && l.percent == r.percent; }
    inline bool operator!=(const FloatOrPercent& l, const FloatOrPercent& r) throw() { return !(l == r); }
    inline bool operator< (const FloatOrPercent& l, const FloatOrPercent& r) throw() { return l.value < r.value || (l.value == r.value && int(l.percent) < int(r.percent)); }
}

namespace std {
    template<> struct hash<Slic3r::FloatOrPercent> {
        std::size_t operator()(const Slic3r::FloatOrPercent& v) const noexcept {
            std::size_t seed = std::hash<double>{}(v.value);
            return v.percent ? seed ^ 0x9e3779b9 : seed;
        }
    };

    template<> struct hash<Slic3r::Vec2d> {
        std::size_t operator()(const Slic3r::Vec2d& v) const noexcept {
            std::size_t seed = std::hash<double>{}(v.x());
            boost::hash_combine(seed, std::hash<double>{}(v.y()));
            return seed;
        }
    };

    template<> struct hash<Slic3r::Vec3d> {
        std::size_t operator()(const Slic3r::Vec3d& v) const noexcept {
            std::size_t seed = std::hash<double>{}(v.x());
            boost::hash_combine(seed, std::hash<double>{}(v.y()));
            boost::hash_combine(seed, std::hash<double>{}(v.z()));
            return seed;
        }
    };
}

namespace Slic3r {

// Name of the configuration option.
typedef std::string                 t_config_option_key;
typedef std::vector<std::string>    t_config_option_keys;

extern std::string  escape_string_cstyle(const std::string &str);
extern std::string  escape_strings_cstyle(const std::vector<std::string> &strs);
extern bool         unescape_string_cstyle(const std::string &str, std::string &out);
extern bool         unescape_strings_cstyle(const std::string &str, std::vector<std::string> &out);

extern std::string  escape_ampersand(const std::string& str);

namespace ConfigHelpers {
	inline bool looks_like_enum_value(std::string value)
	{
		boost::trim(value);
		if (value.empty() || value.size() > 64 || ! isalpha(value.front()))
			return false;
		for (const char c : value)
			if (! (isalnum(c) || c == '_' || c == '-'))
				return false;
		return true;
	}

	inline bool enum_looks_like_true_value(std::string value) {
		boost::trim(value);
		return boost::iequals(value, "enabled") || boost::iequals(value, "on");
	}

	enum class DeserializationSubstitution {
		Disabled,
		DefaultsToFalse,
		DefaultsToTrue
	};

    enum class DeserializationResult {
    	Loaded,
    	Substituted,
    	Failed,
    };
};

// Base for all exceptions thrown by the configuration layer.
class ConfigurationError : public Slic3r::RuntimeError {
public:
    using RuntimeError::RuntimeError;
};

// Specialization of std::exception to indicate that an unknown config option has been encountered.
class UnknownOptionException : public ConfigurationError {
public:
    UnknownOptionException() :
        ConfigurationError("Unknown option exception") {}
    UnknownOptionException(const std::string &opt_key) :
        ConfigurationError(std::string("Unknown option exception: ") + opt_key) {}
};

// Indicate that the ConfigBase derived class does not provide config definition (the method def() returns null).
class NoDefinitionException : public ConfigurationError
{
public:
    NoDefinitionException() :
        ConfigurationError("No definition exception") {}
    NoDefinitionException(const std::string &opt_key) :
        ConfigurationError(std::string("No definition exception: ") + opt_key) {}
};

// Indicate that an unsupported accessor was called on a config option.
class BadOptionTypeException : public ConfigurationError
{
public:
	BadOptionTypeException() : ConfigurationError("Bad option type exception") {}
	BadOptionTypeException(const std::string &message) : ConfigurationError(message) {}
    BadOptionTypeException(const char* message) : ConfigurationError(message) {}
};

// Indicate that an option has been deserialized from an invalid value.
class BadOptionValueException : public ConfigurationError
{
public:
    BadOptionValueException() : ConfigurationError("Bad option value exception") {}
    BadOptionValueException(const std::string &message) : ConfigurationError(message) {}
    BadOptionValueException(const char* message) : ConfigurationError(message) {}
};

// Type of a configuration value.
enum ConfigOptionType {
    coVectorType    = 0x4000,
    coNone          = 0,
    // single float
    coFloat         = 1,
    // vector of floats
    coFloats        = coFloat + coVectorType,
    // single int
    coInt           = 2,
    // vector of ints
    coInts          = coInt + coVectorType,
    // single string
    coString        = 3,
    // vector of strings
    coStrings       = coString + coVectorType,
    // percent value. Currently only used for infill.
    coPercent       = 4,
    // percents value. Currently used for retract before wipe only.
    coPercents      = coPercent + coVectorType,
    // a fraction or an absolute value
    coFloatOrPercent = 5,
    // vector of the above
    coFloatsOrPercents = coFloatOrPercent + coVectorType,
    // single 2d point (Point2f). Currently not used.
    coPoint         = 6,
    // vector of 2d points (Point2f). Currently used for the definition of the print bed and for the extruder offsets.
    coPoints        = coPoint + coVectorType,
    coPoint3        = 7,
//    coPoint3s       = coPoint3 + coVectorType,
    // single boolean value
    coBool          = 8,
    // vector of boolean values
    coBools         = coBool + coVectorType,
    // a generic enum
    coEnum          = 9,
    // BBS: vector of enums
    coEnums         = coEnum + coVectorType,
    coPointsGroups  = 10 + coVectorType,
    coIntsGroups    = 11 + coVectorType
};

enum ConfigOptionMode {
    comSimple = 0,
    comAdvanced,
    comDevelop,
};

enum PrinterTechnology : unsigned char
{
    // Fused Filament Fabrication
    ptFFF,
    // Stereolitography
    ptSLA,
    // Unknown, useful for command line processing
    ptUnknown,
    // Any technology, useful for parameters compatible with both ptFFF and ptSLA
    ptAny
};

enum ForwardCompatibilitySubstitutionRule
{
    // Disable susbtitution, throw exception if an option value is not recognized.
    Disable,
    // Enable substitution of an unknown option value with default. Log the substitution.
    Enable,
    // Enable substitution of an unknown option value with default. Don't log the substitution.
    EnableSilent,
    // Enable substitution of an unknown option value with default. Log substitutions in user profiles, don't log substitutions in system profiles.
    EnableSystemSilent,
    // Enable silent substitution of an unknown option value with default when loading user profiles. Throw on an unknown option value in a system profile.
    EnableSilentDisableSystem,
};

class  ConfigOption;
class  ConfigOptionDef;
// For forward definition of ConfigOption in ConfigOptionUniquePtr, we have to define a custom deleter.
struct ConfigOptionDeleter { void operator()(ConfigOption* p); };
using  ConfigOptionUniquePtr = std::unique_ptr<ConfigOption, ConfigOptionDeleter>;

// When parsing a configuration value, if the old_value is not understood by this OrcaSlicer version,
// it is being substituted with some default value that this OrcaSlicer could work with.
// This structure serves to inform the user about the substitutions having been done during file import.
struct ConfigSubstitution {
    const ConfigOptionDef   *opt_def { nullptr };
    std::string              old_value;
    ConfigOptionUniquePtr    new_value;
};

using  ConfigSubstitutions = std::vector<ConfigSubstitution>;

// Filled in by ConfigBase::set_deserialize_raw(), which based on "rule" either bails out
// or performs substitutions when encountering an unknown configuration value.
struct ConfigSubstitutionContext
{
    ConfigSubstitutionContext(ForwardCompatibilitySubstitutionRule rl) : rule(rl) {}
    bool empty() const throw() { return substitutions.empty(); }

    ForwardCompatibilitySubstitutionRule 	rule;
    ConfigSubstitutions					    substitutions;
    std::vector<std::string>                unrecogized_keys;
};

// A generic value of a configuration option.
class ConfigOption {
public:
    virtual ~ConfigOption() {}

    virtual ConfigOptionType    type() const = 0;
    virtual std::string         serialize() const = 0;
    virtual bool                deserialize(const std::string &str, bool append = false) = 0;
    virtual ConfigOption*       clone() const = 0;
    // Set a value from a ConfigOption. The two options should be compatible.
    virtual void                set(const ConfigOption *option) = 0;
    virtual int                 getInt()        const { throw BadOptionTypeException("Calling ConfigOption::getInt on a non-int ConfigOption"); }
    virtual double              getFloat()      const { throw BadOptionTypeException("Calling ConfigOption::getFloat on a non-float ConfigOption"); }
    virtual bool                getBool()       const { throw BadOptionTypeException("Calling ConfigOption::getBool on a non-boolean ConfigOption");  }
    virtual void                setInt(int /* val */) { throw BadOptionTypeException("Calling ConfigOption::setInt on a non-int ConfigOption"); }
    virtual bool                operator==(const ConfigOption &rhs) const = 0;
    bool                        operator!=(const ConfigOption &rhs) const { return ! (*this == rhs); }
    virtual size_t              hash()          const throw() = 0;
    bool                        is_scalar()     const { return (int(this->type()) & int(coVectorType)) == 0; }
    bool                        is_vector()     const { return ! this->is_scalar(); }
    // If this option is nullable, then it may have its value or values set to nil.
    virtual bool 				nullable()		const { return false; }
    // A scalar is nil, or all values of a vector are nil.
    virtual bool 				is_nil() 		const { return false; }
    // Is this option overridden by another option?
    // An option overrides another option if it is not nil and not equal.
    virtual bool 				overriden_by(const ConfigOption *rhs) const {
    	assert(! this->nullable() && ! rhs->nullable());
    	return *this != *rhs;
    }
    // Apply an override option, possibly a nullable one.
    virtual bool 				apply_override(const ConfigOption *rhs, std::vector<int>& default_index) {
    	if (*this == *rhs)
    		return false;
    	*this = *rhs;
    	return true;
    }
};

typedef ConfigOption*       ConfigOptionPtr;
typedef const ConfigOption* ConfigOptionConstPtr;

// Value of a single valued option (bool, int, float, string, point, enum)
template <class T>
class ConfigOptionSingle : public ConfigOption {
public:
    T value;
    explicit ConfigOptionSingle(T value) : value(std::move(value)) {}
    operator T() const { return this->value; }

    void set(const ConfigOption *rhs) override
    {
        if (rhs->type() != this->type())
            throw ConfigurationError("ConfigOptionSingle: Assigning an incompatible type");
        assert(dynamic_cast<const ConfigOptionSingle<T>*>(rhs));
        this->value = static_cast<const ConfigOptionSingle<T>*>(rhs)->value;
    }

    bool operator==(const ConfigOption &rhs) const override
    {
        if (rhs.type() != this->type())
            throw ConfigurationError("ConfigOptionSingle: Comparing incompatible types");
        assert(dynamic_cast<const ConfigOptionSingle<T>*>(&rhs));
        return this->value == static_cast<const ConfigOptionSingle<T>*>(&rhs)->value;
    }

    bool operator==(const T &rhs) const throw() { return this->value == rhs; }
    bool operator!=(const T &rhs) const throw() { return this->value != rhs; }
    bool operator< (const T &rhs) const throw() { return this->value < rhs; }

    size_t hash() const throw() override { return std::hash<T>{}(this->value); }

private:
	friend class cereal::access;
	template<class Archive> void serialize(Archive & ar) { ar(this->value); }
};

// Value of a vector valued option (bools, ints, floats, strings, points)
class ConfigOptionVectorBase : public ConfigOption {
public:
    // Currently used only to initialize the PlaceholderParser.
    virtual std::vector<std::string> vserialize() const = 0;
    // Set from a vector of ConfigOptions.
    // If the rhs ConfigOption is scalar, then its value is used,
    // otherwise for each of rhs, the first value of a vector is used.
    // This function is useful to collect values for multiple extrder / filament settings.
    virtual void set(const std::vector<const ConfigOption*> &rhs) = 0;
    // Set a single vector item from either a scalar option or the first value of a vector option.vector of ConfigOptions.
    // This function is useful to split values from multiple extrder / filament settings into separate configurations.
    virtual void set_at(const ConfigOption* rhs, size_t i, size_t j) = 0;
    // BBS
    virtual void set_at_to_nil(size_t i)                                                                                    = 0;
    virtual void append(const ConfigOption* rhs)                                                                            = 0;
    virtual void set(const ConfigOption* rhs, size_t start, size_t len)                                                     = 0;
    virtual void set_with_restore(const ConfigOptionVectorBase* rhs, std::vector<int>& restore_index, int stride)           = 0;
    virtual void set_with_restore_2(const ConfigOptionVectorBase* rhs, std::vector<int>& restore_index, int start, int len, bool skip_error = false) = 0;
    virtual void set_only_diff(const ConfigOptionVectorBase* rhs, std::vector<int>& diff_index, int stride)                 = 0;
    virtual void set_with_nil(const ConfigOptionVectorBase* rhs, const ConfigOptionVectorBase* inherits, int stride)        = 0;
    // Resize the vector of values, copy the newly added values from opt_default if provided.
    virtual void resize(size_t n, const ConfigOption *opt_default = nullptr) = 0;
    // Clear the values vector.
    virtual void clear() = 0;

    // Get size of this vector.
    virtual size_t size()  const = 0;
    // Is this vector empty?
    virtual bool   empty() const = 0;
    // Is the value nil? That should only be possible if this->nullable().
    virtual bool   is_nil(size_t idx) const = 0;

    // We just overloaded and hid two base class virtual methods.
    // Let's show it was intentional (warnings).
    using ConfigOption::set;
    using ConfigOption::is_nil;


protected:
    // Used to verify type compatibility when assigning to / from a scalar ConfigOption.
    ConfigOptionType scalar_type() const { return static_cast<ConfigOptionType>(this->type() - coVectorType); }
};

// Value of a vector valued option (bools, ints, floats, strings, points), template
template <class T>
class ConfigOptionVector : public ConfigOptionVectorBase
{
public:
    ConfigOptionVector() {}
    explicit ConfigOptionVector(size_t n, const T &value) : values(n, value) {}
    explicit ConfigOptionVector(std::initializer_list<T> il) : values(std::move(il)) {}
    explicit ConfigOptionVector(const std::vector<T> &values) : values(values) {}
    explicit ConfigOptionVector(std::vector<T> &&values) : values(std::move(values)) {}
    std::vector<T> values;

    void set(const ConfigOption *rhs) override
    {
        if (rhs->type() != this->type())
            throw ConfigurationError("ConfigOptionVector: Assigning an incompatible type");
        assert(dynamic_cast<const ConfigOptionVector<T>*>(rhs));
        this->values = static_cast<const ConfigOptionVector<T>*>(rhs)->values;
    }

    // Set from a vector of ConfigOptions.
    // If the rhs ConfigOption is scalar, then its value is used,
    // otherwise for each of rhs, the first value of a vector is used.
    // This function is useful to collect values for multiple extrder / filament settings.
    void set(const std::vector<const ConfigOption*> &rhs) override
    {
        this->values.clear();
        this->values.reserve(rhs.size());
        for (const ConfigOption *opt : rhs) {
            if (opt->type() == this->type()) {
                auto other = static_cast<const ConfigOptionVector<T>*>(opt);
                if (other->values.empty())
                    throw ConfigurationError("ConfigOptionVector::set(): Assigning from an empty vector");
                this->values.emplace_back(other->values.front());
            } else if (opt->type() == this->scalar_type())
                this->values.emplace_back(static_cast<const ConfigOptionSingle<T>*>(opt)->value);
            else
                throw ConfigurationError("ConfigOptionVector::set():: Assigning an incompatible type");
        }
    }

    // Set a single vector item from either a scalar option or the first value of a vector option.vector of ConfigOptions.
    // This function is useful to split values from multiple extrder / filament settings into separate configurations.
    void set_at(const ConfigOption *rhs, size_t i, size_t j) override
    {
        // It is expected that the vector value has at least one value, which is the default, if not overwritten.
        assert(! this->values.empty());
        if (this->values.size() <= i) {
            // Resize this vector, fill in the new vector fields with the copy of the first field.
            T v = this->values.front();
            this->values.resize(i + 1, v);
        }
        if (rhs->type() == this->type()) {
            // Assign the first value of the rhs vector.
            auto other = static_cast<const ConfigOptionVector<T>*>(rhs);
            if (other->values.empty())
                throw ConfigurationError("ConfigOptionVector::set_at(): Assigning from an empty vector");
            this->values[i] = other->get_at(j);
        } else if (rhs->type() == this->scalar_type())
            this->values[i] = static_cast<const ConfigOptionSingle<T>*>(rhs)->value;
        else
            throw ConfigurationError("ConfigOptionVector::set_at(): Assigning an incompatible type");
    }

    //BBS
    virtual void set_at_to_nil(size_t i) override {}

    void append(const ConfigOption *rhs) override
    {
        if (rhs->type() == this->type()) {
            // Assign the first value of the rhs vector.
            auto other = static_cast<const ConfigOptionVector<T>*>(rhs);
            if (other->values.empty())
                throw ConfigurationError("ConfigOptionVector::append(): append an empty vector");
            this->values.insert(this->values.end(), other->values.begin(), other->values.end());
        } else if (rhs->type() == this->scalar_type())
            this->values.push_back(static_cast<const ConfigOptionSingle<T>*>(rhs)->value);
        else
            throw ConfigurationError("ConfigOptionVector::append(): append an incompatible type");
    }

    // Set a single vector item from a range of another vector option
    // This function is useful to split values from multiple extrder / filament settings into separate configurations.
    void set(const ConfigOption* rhs, size_t start, size_t len) override
    {
        // It is expected that the vector value has at least one value, which is the default, if not overwritten.
        assert(!this->values.empty());
        T v = this->values.front();
        this->values.resize(len, v);
        if (rhs->type() == this->type()) {
            // Assign the first value of the rhs vector.
            auto other = static_cast<const ConfigOptionVector<T>*>(rhs);
            if (other->values.size() < (start+len))
                throw ConfigurationError("ConfigOptionVector::set_with(): Assigning from an vector with invalid size");
            for (size_t i = 0; i < len; i++)
                this->values[i] = other->get_at(start+i);
        }
        else
            throw ConfigurationError("ConfigOptionVector::set_with(): Assigning an incompatible type");
    }

    //set a item related with extruder variants when loading config from 3mf, restore the non change values to system config
    //rhs: item from systemconfig(inherits)
    //keep_index: which index in this vector need to be restored
    virtual void set_with_restore(const ConfigOptionVectorBase* rhs, std::vector<int>& restore_index, int stride) override
    {
        if (rhs->type() == this->type()) {
            //backup original ones
            std::vector<T> backup_values = this->values;
            // Assign the first value of the rhs vector.
            auto other = static_cast<const ConfigOptionVector<T>*>(rhs);
            this->values = other->values;

            if (other->values.size() != (restore_index.size()*stride))
                throw ConfigurationError("ConfigOptionVector::set_with_restore(): Assigning from an vector with invalid restore_index size");

            for (size_t i = 0; i < restore_index.size(); i++) {
                if (restore_index[i] != -1) {
                    for (size_t j = 0; j < stride; j++)
                        this->values[i * stride +j] = backup_values[restore_index[i] * stride +j];
                }
            }
        }
        else
            throw ConfigurationError("ConfigOptionVector::set_with_restore(): Assigning an incompatible type");
    }

    // set a item related with extruder variants when loading config from filament json, replace the original filament items
    // rhs: item from seperate filament config
    // restore_index: which index in this vector need to be restored
    // start: which index in this vector need to be replaced
    // count: how many items in this vector need to be replaced
    virtual void set_with_restore_2(const ConfigOptionVectorBase* rhs, std::vector<int>& restore_index, int start, int len, bool skip_error = false) override
    {
        if (rhs->type() == this->type()) {
            //backup original ones
            std::vector<T> backup_values = this->values;

            if (this->values.size() < start) {
                throw ConfigurationError("ConfigOptionVector::set_with_restore_2(): invalid size found");
            }
            else {
                if (this->values.size() < start + len)
                    len = this->values.size() - start;

                //erase the original ones
                if (len > 0)
                    this->values.erase(this->values.begin() + start, this->values.begin() + start + len);
            }

            // Assign the new value from the rhs vector.
            auto other = const_cast<ConfigOptionVector<T>*>(static_cast<const ConfigOptionVector<T>*>(rhs));

            if (other->values.size() != (restore_index.size())) {
                if (skip_error) {
                    T default_v = other->values.front();
                    other->values.resize(restore_index.size(), default_v);
                }
                else
                    throw ConfigurationError("ConfigOptionVector::set_with_restore_2(): Assigning from an vector with invalid restore_index size");
            }

            for (size_t i = 0; i < restore_index.size(); i++) {
                if ((restore_index[i] != -1)&&(restore_index[i] < backup_values.size())) {
                    this->values.insert(this->values.begin() + start + i, backup_values[restore_index[i]]);
                }
                else
                    this->values.insert(this->values.begin() + start + i, other->values[i]);
            }
        }
        else
            throw ConfigurationError("ConfigOptionVector::set_with_restore_2(): Assigning an incompatible type");
    }

    //set a item related with extruder variants when loading user config, only set the different value of some extruder
    //rhs: item from user config
    //diff_index: which index in this vector need to be set
    virtual void set_only_diff(const ConfigOptionVectorBase* rhs, std::vector<int>& diff_index, int stride) override
    {
        if (rhs->type() == this->type()) {
            // Assign the first value of the rhs vector.
            auto other = static_cast<const ConfigOptionVector<T>*>(rhs);

            if (this->values.size() != (diff_index.size()*stride))
                throw ConfigurationError("ConfigOptionVector::set_only_diff(): Assigning from an vector with invalid diff_index size");

            for (size_t i = 0; i < diff_index.size(); i++) {
                if (diff_index[i] != -1) {
                    for (size_t j = 0; j < stride; j++) {
                        if (!other->is_nil(diff_index[i] * stride))
                            this->values[i * stride + j] = other->values[diff_index[i] * stride + j];
                    }
                }
            }
        }
        else
            throw ConfigurationError("ConfigOptionVector::set_only_diff(): Assigning an incompatible type");
    }

    //set a item related with extruder variants when saving user config, set the non-diff value of some extruder to nill
    //this item has different value with inherit config
    //rhs: item from userconfig
    //inherits: item from inherit config
    virtual void set_with_nil(const ConfigOptionVectorBase* rhs, const ConfigOptionVectorBase* inherits, int stride) override
    {
        if ((rhs->type() == this->type()) && (inherits->type() == this->type())) {
            auto rhs_opt = static_cast<const ConfigOptionVector<T>*>(rhs);
            auto inherits_opt = static_cast<const ConfigOptionVector<T>*>(inherits);

            if (inherits->size() != rhs->size())
                throw ConfigurationError("ConfigOptionVector::set_with_nil(): rhs size different with inherits size");

            this->values.resize(inherits->size(), this->values.front());

            for (size_t i = 0; i < inherits_opt->size(); i= i+stride) {
                bool set_nil = true;
                for (size_t j = 0; j < stride; j++) {
                    if (inherits_opt->values[i +j] != rhs_opt->values[i +j]) {
                        set_nil = false;
                        break;
                    }
                }

                for (size_t j = 0; j < stride; j++) {
                    if (set_nil) {
                        this->set_at_to_nil(i +j);
                    }
                    else
                        this->values[i +j] = rhs_opt->values[i +j];
                }
            }
        }
        else
            throw ConfigurationError("ConfigOptionVector::set_with_nil(): Assigning an incompatible type");
    }

    const T& get_at(size_t i) const
    {
        assert(! this->values.empty());
        return (i < this->values.size()) ? this->values[i] : this->values.front();
    }

    T& get_at(size_t i) { return const_cast<T&>(std::as_const(*this).get_at(i)); }

    // Resize this vector by duplicating the /*last*/first value.
    // If the current vector is empty, the default value is used instead.
    // BBS: support scaler opt_default
    void resize(size_t n, const ConfigOption *opt_default = nullptr) override
    {
        //assert(opt_default == nullptr || opt_default->is_vector());
//        assert(opt_default == nullptr || dynamic_cast<ConfigOptionVector<T>>(opt_default));
        assert(! this->values.empty() || opt_default != nullptr);

        if (n == 0)
            this->values.clear();
        else if (n < this->values.size())
            this->values.erase(this->values.begin() + n, this->values.end());
        else if (n > this->values.size()) {
            if (this->values.empty()) {
                if (opt_default == nullptr) {
                    throw ConfigurationError("ConfigOptionVector::resize(): No default value provided.");
                }
                else if (opt_default->is_vector()) {
                    if (opt_default->type() != this->type())
                        throw ConfigurationError("ConfigOptionVector::resize(): Extending with an incompatible type.");
                    this->values.resize(n, static_cast<const ConfigOptionVector<T>*>(opt_default)->values.front());
                }
                else {
                    if (opt_default->type() != this->scalar_type())
                        throw ConfigurationError("ConfigOptionVector::resize(): Extending with an incompatible type.");
                    this->values.resize(n, static_cast<const ConfigOptionSingle<T>*>(opt_default)->value);
                }
            } else {
                // Resize by duplicating the last value.
                this->values.resize(n, this->values./*back*/front());
            }
        }
    }

    // Clear the values vector.
    void   clear() override { this->values.clear(); }
    size_t size()  const override { return this->values.size(); }
    bool   empty() const override { return this->values.empty(); }

    bool operator==(const ConfigOption &rhs) const override
    {
        if (rhs.type() != this->type())
            throw ConfigurationError("ConfigOptionVector: Comparing incompatible types");
        assert(dynamic_cast<const ConfigOptionVector<T>*>(&rhs));
        return this->values == static_cast<const ConfigOptionVector<T>*>(&rhs)->values;
    }

    bool operator==(const std::vector<T> &rhs) const throw() { return this->values == rhs; }
    bool operator!=(const std::vector<T> &rhs) const throw() { return this->values != rhs; }

    size_t hash() const throw() override {
        std::hash<T> hasher;
        size_t seed = 0;
        for (const auto &v : this->values)
            boost::hash_combine(seed, hasher(v));
        return seed;
    }

    // Is this option overridden by another option?
    // An option overrides another option if it is not nil and not equal.
    bool overriden_by(const ConfigOption *rhs) const override {
        if (this->nullable())
        	throw ConfigurationError("Cannot override a nullable ConfigOption.");
        if (rhs->type() != this->type())
            throw ConfigurationError("ConfigOptionVector.overriden_by() applied to different types.");
    	auto rhs_vec = static_cast<const ConfigOptionVector<T>*>(rhs);
    	if (! rhs->nullable())
    		// Overridding a non-nullable object with another non-nullable object.
    		return this->values != rhs_vec->values;
    	size_t i = 0;
    	size_t cnt = std::min(this->size(), rhs_vec->size());
    	for (; i < cnt; ++ i)
    		if (! rhs_vec->is_nil(i) && this->values[i] != rhs_vec->values[i])
    			return true;
    	for (; i < rhs_vec->size(); ++ i)
    		if (! rhs_vec->is_nil(i))
    			return true;
    	return false;
    }
    // Apply an override option, possibly a nullable one.
    bool apply_override(const ConfigOption *rhs, std::vector<int>& default_index) override {
        if (this->nullable())
        	throw ConfigurationError("Cannot override a nullable ConfigOption.");
        if (rhs->type() != this->type())
			throw ConfigurationError("ConfigOptionVector.apply_override() applied to different types.");
		auto rhs_vec = static_cast<const ConfigOptionVector<T>*>(rhs);
		if (! rhs->nullable()) {
    		// Overridding a non-nullable object with another non-nullable object.
    		if (this->values != rhs_vec->values) {
    			this->values = rhs_vec->values;
    			return true;
    		}
    		return false;
    	}

    	size_t cnt = std::min(this->size(), rhs_vec->size());
        if (cnt < 1)
            return false;

        std::vector<T> default_value = this->values;

        if (this->values.empty())
            this->values.resize(rhs_vec->size());
        else
            this->values.resize(rhs_vec->size(), this->values.front());

        assert(default_index.size() == rhs_vec->size());

        bool modified = false;

        for (size_t i = 0; i < rhs_vec->size(); ++i) {
            if (!rhs_vec->is_nil(i)) {
                this->values[i] = rhs_vec->values[i];
                modified        = true;
            } else {
                if ((i < default_index.size()) && (default_index[i] - 1 < default_value.size()))
                    this->values[i] = default_value[default_index[i] - 1];
                else
                    this->values[i] = default_value[0];
            }
        }
        return modified;
    }

private:
	friend class cereal::access;
	template<class Archive> void serialize(Archive & ar) { ar(this->values); }
};

class ConfigOptionFloat : public ConfigOptionSingle<double>
{
public:
    ConfigOptionFloat() : ConfigOptionSingle<double>(0) {}
    explicit ConfigOptionFloat(double _value) : ConfigOptionSingle<double>(_value) {}

    static ConfigOptionType static_type() { return coFloat; }
    ConfigOptionType        type()      const override { return static_type(); }
    double                  getFloat()  const override { return this->value; }
    ConfigOption*           clone()     const override { return new ConfigOptionFloat(*this); }
    bool                    operator==(const ConfigOptionFloat &rhs) const throw() { return this->value == rhs.value; }
    bool                    operator< (const ConfigOptionFloat &rhs) const throw() { return this->value <  rhs.value; }

    std::string serialize() const override
    {
        std::ostringstream ss;
        ss << this->value;
        return ss.str();
    }

    bool deserialize(const std::string &str, bool append = false) override
    {
        UNUSED(append);
        std::istringstream iss(str);
        iss >> this->value;
        return !iss.fail();
    }

    ConfigOptionFloat& operator=(const ConfigOption *opt)
    {
        this->set(opt);
        return *this;
    }

private:
	friend class cereal::access;
	template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionSingle<double>>(this)); }
};

template<bool NULLABLE>
class ConfigOptionFloatsTempl : public ConfigOptionVector<double>
{
public:
    ConfigOptionFloatsTempl() : ConfigOptionVector<double>() {}
    explicit ConfigOptionFloatsTempl(size_t n, double value) : ConfigOptionVector<double>(n, value) {}
    explicit ConfigOptionFloatsTempl(std::initializer_list<double> il) : ConfigOptionVector<double>(std::move(il)) {}
    explicit ConfigOptionFloatsTempl(const std::vector<double> &vec) : ConfigOptionVector<double>(vec) {}
    explicit ConfigOptionFloatsTempl(std::vector<double> &&vec) : ConfigOptionVector<double>(std::move(vec)) {}

    static ConfigOptionType static_type() { return coFloats; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { return new ConfigOptionFloatsTempl(*this); }
    bool                    operator==(const ConfigOptionFloatsTempl &rhs) const throw() { return vectors_equal(this->values, rhs.values); }
    bool                    operator< (const ConfigOptionFloatsTempl &rhs) const throw() { return vectors_lower(this->values, rhs.values); }
    bool 					operator==(const ConfigOption &rhs) const override {
        if (rhs.type() != this->type())
            throw ConfigurationError("ConfigOptionFloatsTempl: Comparing incompatible types");
        assert(dynamic_cast<const ConfigOptionVector<double>*>(&rhs));
        return vectors_equal(this->values, static_cast<const ConfigOptionVector<double>*>(&rhs)->values);
    }
    // Could a special "nil" value be stored inside the vector, indicating undefined value?
    bool 					nullable() const override { return NULLABLE; }
    // Special "nil" value to be stored into the vector if this->supports_nil().
    static double 			nil_value() { return std::numeric_limits<double>::quiet_NaN(); }
    // A scalar is nil, or all values of a vector are nil.
    bool 					is_nil() const override { for (auto v : this->values) if (! std::isnan(v)) return false; return true; }
    bool 					is_nil(size_t idx) const override { return std::isnan(this->values[idx]); }
    virtual void set_at_to_nil(size_t i) override
    {
        assert(nullable() && (i < this->values.size()));
        this->values[i] = nil_value();
    }

    std::string serialize() const override
    {
        std::ostringstream ss;
        for (const double &v : this->values) {
            if (&v != &this->values.front())
            	ss << ",";
            serialize_single_value(ss, v);
        }
        return ss.str();
    }

    std::vector<std::string> vserialize() const override
    {
        std::vector<std::string> vv;
        vv.reserve(this->values.size());
        for (const double v : this->values) {
            std::ostringstream ss;
        	serialize_single_value(ss, v);
            vv.push_back(ss.str());
        }
        return vv;
    }

    bool deserialize(const std::string &str, bool append = false) override
    {
        if (! append)
            this->values.clear();

        if (str.empty()) {
            this->values.push_back(0);
            return true;
        }
        std::istringstream is(str);
        std::string item_str;
        while (std::getline(is, item_str, ',')) {
        	boost::trim(item_str);
        	if (item_str == "nil") {
        		if (NULLABLE)
        			this->values.push_back(nil_value());
        		else
        			throw ConfigurationError("Deserializing nil into a non-nullable object");
        	} else {
	            std::istringstream iss(item_str);
	            double value;
	            iss >> value;
	            this->values.push_back(value);
	        }
        }
        return true;
    }
    static bool validate_string(const std::string &str)
    {
        // should only have number and commas
        return std::all_of(str.begin(), str.end(), [](char c) {
            return std::isdigit(c) || c == ','|| std::isspace(c);
        });
    }

    ConfigOptionFloatsTempl& operator=(const ConfigOption *opt)
    {
        this->set(opt);
        return *this;
    }

protected:
	void serialize_single_value(std::ostringstream &ss, const double v) const {
        	if (std::isfinite(v))
	            ss << v;
	        else if (std::isnan(v)) {
        		if (NULLABLE)
        			ss << "nil";
        		else
                    throw ConfigurationError("Serializing NaN");
        	} else
                throw ConfigurationError("Serializing invalid number");
	}
    static bool vectors_equal(const std::vector<double> &v1, const std::vector<double> &v2) {
    	if (NULLABLE) {
    		if (v1.size() != v2.size())
    			return false;
    		for (auto it1 = v1.begin(), it2 = v2.begin(); it1 != v1.end(); ++ it1, ++ it2)
	    		if (! ((std::isnan(*it1) && std::isnan(*it2)) || *it1 == *it2))
	    			return false;
    		return true;
    	} else
    		// Not supporting nullable values, the default vector compare is cheaper.
    		return v1 == v2;
    }
    static bool vectors_lower(const std::vector<double> &v1, const std::vector<double> &v2) {
        if (NULLABLE) {
            for (auto it1 = v1.begin(), it2 = v2.begin(); it1 != v1.end() && it2 != v2.end(); ++ it1, ++ it2) {
                auto null1 = int(std::isnan(*it1));
                auto null2 = int(std::isnan(*it2));
                return (null1 < null2) || (null1 == null2 && *it1 < *it2);
            }
            return v1.size() < v2.size();
        } else
            // Not supporting nullable values, the default vector compare is cheaper.
            return v1 < v2;
    }

private:
	friend class cereal::access;
	template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionVector<double>>(this)); }
};

using ConfigOptionFloats 		 = ConfigOptionFloatsTempl<false>;
using ConfigOptionFloatsNullable = ConfigOptionFloatsTempl<true>;

class ConfigOptionInt : public ConfigOptionSingle<int>
{
public:
    ConfigOptionInt() : ConfigOptionSingle<int>(0) {}
    explicit ConfigOptionInt(int value) : ConfigOptionSingle<int>(value) {}
    explicit ConfigOptionInt(double _value) : ConfigOptionSingle<int>(int(floor(_value + 0.5))) {}

    static ConfigOptionType static_type() { return coInt; }
    ConfigOptionType        type()   const override { return static_type(); }
    int                     getInt() const override { return this->value; }
    void                    setInt(int val) override { this->value = val; }
    ConfigOption*           clone()  const override { return new ConfigOptionInt(*this); }
    bool                    operator==(const ConfigOptionInt &rhs) const throw() { return this->value == rhs.value; }

    std::string serialize() const override
    {
        std::ostringstream ss;
        ss << this->value;
        return ss.str();
    }

    bool deserialize(const std::string &str, bool append = false) override
    {
        UNUSED(append);
        std::istringstream iss(str);
        iss >> this->value;
        return !iss.fail();
    }

    ConfigOptionInt& operator=(const ConfigOption *opt)
    {
        this->set(opt);
        return *this;
    }

private:
	friend class cereal::access;
	template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionSingle<int>>(this)); }
};

template<bool NULLABLE>
class ConfigOptionIntsTempl : public ConfigOptionVector<int>
{
public:
    ConfigOptionIntsTempl() : ConfigOptionVector<int>() {}
    explicit ConfigOptionIntsTempl(size_t n, int value) : ConfigOptionVector<int>(n, value) {}
    explicit ConfigOptionIntsTempl(std::initializer_list<int> il) : ConfigOptionVector<int>(std::move(il)) {}
    explicit ConfigOptionIntsTempl(const std::vector<int> &vec) : ConfigOptionVector<int>(vec) {}
    explicit ConfigOptionIntsTempl(std::vector<int> &&vec) : ConfigOptionVector<int>(std::move(vec)) {}

    static ConfigOptionType static_type() { return coInts; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { return new ConfigOptionIntsTempl(*this); }
    ConfigOptionIntsTempl&  operator= (const ConfigOption *opt) { this->set(opt); return *this; }
    bool                    operator==(const ConfigOptionIntsTempl &rhs) const throw() { return this->values == rhs.values; }
    bool                    operator< (const ConfigOptionIntsTempl &rhs) const throw() { return this->values <  rhs.values; }
    // Could a special "nil" value be stored inside the vector, indicating undefined value?
    bool 					nullable() const override { return NULLABLE; }
    // Special "nil" value to be stored into the vector if this->supports_nil().
    static int	 			nil_value() { return std::numeric_limits<int>::max(); }
    // A scalar is nil, or all values of a vector are nil.
    bool 					is_nil() const override { for (auto v : this->values) if (v != nil_value()) return false; return true; }
    bool 					is_nil(size_t idx) const override { return this->values[idx] == nil_value(); }
    virtual void set_at_to_nil(size_t i) override
    {
        assert(nullable() && (i < this->values.size()));
        this->values[i] = nil_value();
    }

    std::string serialize() const override
    {
        std::ostringstream ss;
        for (const int &v : this->values) {
            if (&v != &this->values.front())
            	ss << ",";
            serialize_single_value(ss, v);
        }
        return ss.str();
    }

    std::vector<std::string> vserialize() const override
    {
        std::vector<std::string> vv;
        vv.reserve(this->values.size());
        for (const int v : this->values) {
            std::ostringstream ss;
        	serialize_single_value(ss, v);
            vv.push_back(ss.str());
        }
        return vv;
    }

    bool deserialize(const std::string &str, bool append = false) override
    {
        if (! append)
            this->values.clear();
        std::istringstream is(str);
        std::string item_str;
        while (std::getline(is, item_str, ',')) {
        	boost::trim(item_str);
        	if (item_str == "nil") {
        		if (NULLABLE)
        			this->values.push_back(nil_value());
        		else
                    throw ConfigurationError("Deserializing nil into a non-nullable object");
        	} else {
	            std::istringstream iss(item_str);
	            int value;
	            iss >> value;
	            this->values.push_back(value);
	        }
        }
        return true;
    }

private:
	void serialize_single_value(std::ostringstream &ss, const int v) const {
			if (v == nil_value()) {
        		if (NULLABLE)
        			ss << "nil";
        		else
                    throw ConfigurationError("Serializing NaN");
        	} else
        		ss << v;
	}

	friend class cereal::access;
	template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionVector<int>>(this)); }
};

using ConfigOptionInts   	   = ConfigOptionIntsTempl<false>;
using ConfigOptionIntsNullable = ConfigOptionIntsTempl<true>;

class ConfigOptionString : public ConfigOptionSingle<std::string>
{
public:
    ConfigOptionString() : ConfigOptionSingle<std::string>(std::string{}) {}
    explicit ConfigOptionString(std::string value) : ConfigOptionSingle<std::string>(std::move(value)) {}

    static ConfigOptionType static_type() { return coString; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { return new ConfigOptionString(*this); }
    ConfigOptionString&     operator=(const ConfigOption *opt) { this->set(opt); return *this; }
    bool                    operator==(const ConfigOptionString &rhs) const throw() { return this->value == rhs.value; }
    bool                    operator< (const ConfigOptionString &rhs) const throw() { return this->value <  rhs.value; }
    bool 					empty() const { return this->value.empty(); }

    std::string serialize() const override
    {
        return escape_string_cstyle(this->value);
    }

    bool deserialize(const std::string &str, bool append = false) override
    {
        UNUSED(append);
        return unescape_string_cstyle(str, this->value);
    }

private:
	friend class cereal::access;
	template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionSingle<std::string>>(this)); }
};

// semicolon-separated strings
class ConfigOptionStrings : public ConfigOptionVector<std::string>
{
public:
    ConfigOptionStrings() : ConfigOptionVector<std::string>() {}
    explicit ConfigOptionStrings(size_t n, const std::string &value) : ConfigOptionVector<std::string>(n, value) {}
    explicit ConfigOptionStrings(const std::vector<std::string> &values) : ConfigOptionVector<std::string>(values) {}
    explicit ConfigOptionStrings(std::vector<std::string> &&values) : ConfigOptionVector<std::string>(std::move(values)) {}
    explicit ConfigOptionStrings(std::initializer_list<std::string> il) : ConfigOptionVector<std::string>(std::move(il)) {}

    static ConfigOptionType static_type() { return coStrings; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { return new ConfigOptionStrings(*this); }
    ConfigOptionStrings&    operator=(const ConfigOption *opt) { this->set(opt); return *this; }
    bool                    operator==(const ConfigOptionStrings &rhs) const throw() { return this->values == rhs.values; }
    bool                    operator< (const ConfigOptionStrings &rhs) const throw() { return this->values <  rhs.values; }
    bool					is_nil(size_t) const override { return false; }

    std::string serialize() const override
    {
        return escape_strings_cstyle(this->values);
    }

    std::vector<std::string> vserialize() const override
    {
        //BBS: add serialize
        /*std::vector<std::string> result;
        result.resize(this->values.size());
        for (int i = 0; i < this->values.size(); i++)
        {
            result[i] = escape_string_cstyle(this->values[i]);
        }
        return result;*/
        return this->values;
    }

    bool deserialize(const std::string &str, bool append = false) override
    {
        if (! append)
            this->values.clear();
        return unescape_strings_cstyle(str, this->values);
    }

private:
	friend class cereal::access;
	template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionVector<std::string>>(this)); }
};

class ConfigOptionPercent : public ConfigOptionFloat
{
public:
    ConfigOptionPercent() : ConfigOptionFloat(0) {}
    explicit ConfigOptionPercent(double _value) : ConfigOptionFloat(_value) {}

    static ConfigOptionType static_type() { return coPercent; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { return new ConfigOptionPercent(*this); }
    ConfigOptionPercent&    operator= (const ConfigOption *opt) { this->set(opt); return *this; }
    bool                    operator==(const ConfigOptionPercent &rhs) const throw() { return this->value == rhs.value; }
    bool                    operator< (const ConfigOptionPercent &rhs) const throw() { return this->value <  rhs.value; }

    double                  get_abs_value(double ratio_over) const { return ratio_over * this->value / 100; }

    std::string serialize() const override
    {
        std::ostringstream ss;
        ss << this->value;
        std::string s(ss.str());
        s += "%";
        return s;
    }

    bool deserialize(const std::string &str, bool append = false) override
    {
        UNUSED(append);
        // don't try to parse the trailing % since it's optional
        std::istringstream iss(str);
        iss >> this->value;
        return !iss.fail();
    }

private:
	friend class cereal::access;
	template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionFloat>(this)); }
};

template<bool NULLABLE>
class ConfigOptionPercentsTempl : public ConfigOptionFloatsTempl<NULLABLE>
{
public:
    ConfigOptionPercentsTempl() : ConfigOptionFloatsTempl<NULLABLE>() {}
    explicit ConfigOptionPercentsTempl(size_t n, double value) : ConfigOptionFloatsTempl<NULLABLE>(n, value) {}
    explicit ConfigOptionPercentsTempl(std::initializer_list<double> il) : ConfigOptionFloatsTempl<NULLABLE>(std::move(il)) {}
	explicit ConfigOptionPercentsTempl(const std::vector<double>& vec) : ConfigOptionFloatsTempl<NULLABLE>(vec) {}
	explicit ConfigOptionPercentsTempl(std::vector<double>&& vec) : ConfigOptionFloatsTempl<NULLABLE>(std::move(vec)) {}

    static ConfigOptionType static_type() { return coPercents; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { return new ConfigOptionPercentsTempl(*this); }
    ConfigOptionPercentsTempl& operator=(const ConfigOption *opt) { this->set(opt); return *this; }
    bool                    operator==(const ConfigOptionPercentsTempl &rhs) const throw() { return ConfigOptionFloatsTempl<NULLABLE>::vectors_equal(this->values, rhs.values); }
    bool                    operator< (const ConfigOptionPercentsTempl &rhs) const throw() { return ConfigOptionFloatsTempl<NULLABLE>::vectors_lower(this->values, rhs.values); }

    std::string serialize() const override
    {
        std::ostringstream ss;
        for (const double &v : this->values) {
            if (&v != &this->values.front())
            	ss << ",";
			this->serialize_single_value(ss, v);
			if (! std::isnan(v))
				ss << "%";
        }
        std::string str = ss.str();
        return str;
    }

    std::vector<std::string> vserialize() const override
    {
        std::vector<std::string> vv;
        vv.reserve(this->values.size());
        for (const double v : this->values) {
            std::ostringstream ss;
			this->serialize_single_value(ss, v);
			if (! std::isnan(v))
				ss << "%";
            vv.push_back(ss.str());
        }
        return vv;
    }

    // The float's deserialize function shall ignore the trailing optional %.
    // bool deserialize(const std::string &str, bool append = false) override;

private:
	friend class cereal::access;
	template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionFloatsTempl<NULLABLE>>(this)); }
};

using ConfigOptionPercents 	   		= ConfigOptionPercentsTempl<false>;
using ConfigOptionPercentsNullable 	= ConfigOptionPercentsTempl<true>;

class ConfigOptionFloatOrPercent : public ConfigOptionPercent
{
public:
    bool percent;
    ConfigOptionFloatOrPercent() : ConfigOptionPercent(0), percent(false) {}
    explicit ConfigOptionFloatOrPercent(double _value, bool _percent) : ConfigOptionPercent(_value), percent(_percent) {}

    static ConfigOptionType     static_type() { return coFloatOrPercent; }
    ConfigOptionType            type()  const override { return static_type(); }
    ConfigOption*               clone() const override { return new ConfigOptionFloatOrPercent(*this); }
    ConfigOptionFloatOrPercent& operator=(const ConfigOption *opt) { this->set(opt); return *this; }
    bool                        operator==(const ConfigOption &rhs) const override
    {
        if (rhs.type() != this->type())
            throw ConfigurationError("ConfigOptionFloatOrPercent: Comparing incompatible types");
        assert(dynamic_cast<const ConfigOptionFloatOrPercent*>(&rhs));
        return *this == *static_cast<const ConfigOptionFloatOrPercent*>(&rhs);
    }
    bool                        operator==(const ConfigOptionFloatOrPercent &rhs) const throw()
        { return this->value == rhs.value && this->percent == rhs.percent; }
    size_t                      hash() const throw() override
        { size_t seed = std::hash<double>{}(this->value); return this->percent ? seed ^ 0x9e3779b9 : seed; }
    bool                        operator< (const ConfigOptionFloatOrPercent &rhs) const throw()
        { return this->value < rhs.value || (this->value == rhs.value && int(this->percent) < int(rhs.percent)); }

    double                      get_abs_value(double ratio_over) const
        { return this->percent ? (ratio_over * this->value / 100) : this->value; }

    void set(const ConfigOption *rhs) override {
        if (rhs->type() != this->type())
            throw ConfigurationError("ConfigOptionFloatOrPercent: Assigning an incompatible type");
        assert(dynamic_cast<const ConfigOptionFloatOrPercent*>(rhs));
        *this = *static_cast<const ConfigOptionFloatOrPercent*>(rhs);
    }

    std::string serialize() const override
    {
        std::ostringstream ss;
        ss << this->value;
        std::string s(ss.str());
        if (this->percent) s += "%";
        return s;
    }

    bool deserialize(const std::string &str, bool append = false) override
    {
        UNUSED(append);
        this->percent = str.find_first_of("%") != std::string::npos;
        std::istringstream iss(str);
        iss >> this->value;
        return !iss.fail();
    }

private:
	friend class cereal::access;
	template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionPercent>(this), percent); }
};

template<bool NULLABLE>
class ConfigOptionFloatsOrPercentsTempl : public ConfigOptionVector<FloatOrPercent>
{
public:
    ConfigOptionFloatsOrPercentsTempl() : ConfigOptionVector<FloatOrPercent>() {}
    explicit ConfigOptionFloatsOrPercentsTempl(size_t n, FloatOrPercent value) : ConfigOptionVector<FloatOrPercent>(n, value) {}
    explicit ConfigOptionFloatsOrPercentsTempl(std::initializer_list<FloatOrPercent> il) : ConfigOptionVector<FloatOrPercent>(std::move(il)) {}
    explicit ConfigOptionFloatsOrPercentsTempl(const std::vector<FloatOrPercent> &vec) : ConfigOptionVector<FloatOrPercent>(vec) {}
    explicit ConfigOptionFloatsOrPercentsTempl(std::vector<FloatOrPercent> &&vec) : ConfigOptionVector<FloatOrPercent>(std::move(vec)) {}

    static ConfigOptionType static_type() { return coFloatsOrPercents; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { return new ConfigOptionFloatsOrPercentsTempl(*this); }
    bool                    operator==(const ConfigOptionFloatsOrPercentsTempl &rhs) const throw() { return vectors_equal(this->values, rhs.values); }
    bool                    operator==(const ConfigOption &rhs) const override {
        if (rhs.type() != this->type())
            throw ConfigurationError("ConfigOptionFloatsOrPercentsTempl: Comparing incompatible types");
        assert(dynamic_cast<const ConfigOptionVector<FloatOrPercent>*>(&rhs));
        return vectors_equal(this->values, static_cast<const ConfigOptionVector<FloatOrPercent>*>(&rhs)->values);
    }
    bool                    operator< (const ConfigOptionFloatsOrPercentsTempl &rhs) const throw() { return vectors_lower(this->values, rhs.values); }

    // Could a special "nil" value be stored inside the vector, indicating undefined value?
    bool                    nullable() const override { return NULLABLE; }
    // Special "nil" value to be stored into the vector if this->supports_nil().
    static FloatOrPercent   nil_value() { return { std::numeric_limits<double>::quiet_NaN(), false }; }
    // A scalar is nil, or all values of a vector are nil.
    bool                    is_nil() const override { for (auto v : this->values) if (! std::isnan(v.value)) return false; return true; }
    bool                    is_nil(size_t idx) const override { return std::isnan(this->values[idx].value); }
    virtual void set_at_to_nil(size_t i) override
    {
        assert(nullable() && (i < this->values.size()));
        this->values[i] = nil_value();
    }

    std::string serialize() const override
    {
        std::ostringstream ss;
        for (const FloatOrPercent &v : this->values) {
            if (&v != &this->values.front())
                ss << ",";
            serialize_single_value(ss, v);
        }
        return ss.str();
    }

    std::vector<std::string> vserialize() const override
    {
        std::vector<std::string> vv;
        vv.reserve(this->values.size());
        for (const FloatOrPercent &v : this->values) {
            std::ostringstream ss;
            serialize_single_value(ss, v);
            vv.push_back(ss.str());
        }
        return vv;
    }

    bool deserialize(const std::string &str, bool append = false) override
    {
        if (! append)
            this->values.clear();
        std::istringstream is(str);
        std::string item_str;
        while (std::getline(is, item_str, ',')) {
            boost::trim(item_str);
            if (item_str == "nil") {
                if (NULLABLE)
                    this->values.push_back(nil_value());
                else
                    throw ConfigurationError("Deserializing nil into a non-nullable object");
            } else {
                bool percent = item_str.find_first_of("%") != std::string::npos;
                std::istringstream iss(item_str);
                double value;
                iss >> value;
                this->values.push_back({ value, percent });
            }
        }
        return true;
    }

    ConfigOptionFloatsOrPercentsTempl& operator=(const ConfigOption *opt)
    {
        this->set(opt);
        return *this;
    }

protected:
    void serialize_single_value(std::ostringstream &ss, const FloatOrPercent &v) const {
            if (std::isfinite(v.value)) {
                ss << v.value;
                if (v.percent)
                    ss << "%";
            } else if (std::isnan(v.value)) {
                if (NULLABLE)
                    ss << "nil";
                else
                    throw ConfigurationError("Serializing NaN");
            } else
                throw ConfigurationError("Serializing invalid number");
    }
    static bool vectors_equal(const std::vector<FloatOrPercent> &v1, const std::vector<FloatOrPercent> &v2) {
        if (NULLABLE) {
            if (v1.size() != v2.size())
                return false;
            for (auto it1 = v1.begin(), it2 = v2.begin(); it1 != v1.end(); ++ it1, ++ it2)
                if (! ((std::isnan(it1->value) && std::isnan(it2->value)) || *it1 == *it2))
                    return false;
            return true;
        } else
            // Not supporting nullable values, the default vector compare is cheaper.
            return v1 == v2;
    }
    static bool vectors_lower(const std::vector<FloatOrPercent> &v1, const std::vector<FloatOrPercent> &v2) {
        if (NULLABLE) {
            for (auto it1 = v1.begin(), it2 = v2.begin(); it1 != v1.end() && it2 != v2.end(); ++ it1, ++ it2) {
                auto null1 = int(std::isnan(it1->value));
                auto null2 = int(std::isnan(it2->value));
                return (null1 < null2) || (null1 == null2 && *it1 < *it2);
            }
            return v1.size() < v2.size();
        } else
            // Not supporting nullable values, the default vector compare is cheaper.
            return v1 < v2;
    }

private:
    friend class cereal::access;
    template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionVector<FloatOrPercent>>(this)); }
};

using ConfigOptionFloatsOrPercents          = ConfigOptionFloatsOrPercentsTempl<false>;
using ConfigOptionFloatsOrPercentsNullable  = ConfigOptionFloatsOrPercentsTempl<true>;

class ConfigOptionPoint : public ConfigOptionSingle<Vec2d>
{
public:
    ConfigOptionPoint() : ConfigOptionSingle<Vec2d>(Vec2d(0,0)) {}
    explicit ConfigOptionPoint(const Vec2d &value) : ConfigOptionSingle<Vec2d>(value) {}

    static ConfigOptionType static_type() { return coPoint; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { return new ConfigOptionPoint(*this); }
    ConfigOptionPoint&      operator=(const ConfigOption *opt) { this->set(opt); return *this; }
    bool                    operator==(const ConfigOptionPoint &rhs) const throw() { return this->value == rhs.value; }
    bool                    operator< (const ConfigOptionPoint &rhs) const throw() { return this->value <  rhs.value; }

    std::string serialize() const override
    {
        std::ostringstream ss;
        ss << this->value(0);
        ss << ",";
        ss << this->value(1);
        return ss.str();
    }

    bool deserialize(const std::string &str, bool append = false) override
    {
        UNUSED(append);
        char dummy;
        return sscanf(str.data(), " %lf , %lf %c", &this->value(0), &this->value(1), &dummy) == 2 ||
               sscanf(str.data(), " %lf x %lf %c", &this->value(0), &this->value(1), &dummy) == 2;
    }

private:
	friend class cereal::access;
	template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionSingle<Vec2d>>(this)); }
};

class ConfigOptionPoints : public ConfigOptionVector<Vec2d>
{
public:
    ConfigOptionPoints() : ConfigOptionVector<Vec2d>() {}
    explicit ConfigOptionPoints(size_t n, const Vec2d &value) : ConfigOptionVector<Vec2d>(n, value) {}
    explicit ConfigOptionPoints(std::initializer_list<Vec2d> il) : ConfigOptionVector<Vec2d>(std::move(il)) {}
    explicit ConfigOptionPoints(const std::vector<Vec2d> &values) : ConfigOptionVector<Vec2d>(values) {}

    static ConfigOptionType static_type() { return coPoints; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { return new ConfigOptionPoints(*this); }
    ConfigOptionPoints&     operator= (const ConfigOption *opt) { this->set(opt); return *this; }
    bool                    operator==(const ConfigOptionPoints &rhs) const throw() { return this->values == rhs.values; }
    bool                    operator< (const ConfigOptionPoints &rhs) const throw()
        { return std::lexicographical_compare(this->values.begin(), this->values.end(), rhs.values.begin(), rhs.values.end(), [](const auto &l, const auto &r){ return l < r; }); }
    bool					is_nil(size_t) const override { return false; }

    std::string serialize() const override
    {
        std::ostringstream ss;
        for (Pointfs::const_iterator it = this->values.begin(); it != this->values.end(); ++it) {
            if (it - this->values.begin() != 0) ss << ",";
            ss << (*it)(0);
            ss << "x";
            ss << (*it)(1);
        }
        return ss.str();
    }

    std::vector<std::string> vserialize() const override
    {
        std::vector<std::string> vv;
        for (Pointfs::const_iterator it = this->values.begin(); it != this->values.end(); ++it) {
            std::ostringstream ss;
            //BBS: add json format
            //ss << *it;
            ss << (*it)(0);
            ss << "x";
            ss << (*it)(1);
            vv.push_back(ss.str());
        }
        return vv;
    }

    bool deserialize(const std::string &str, bool append = false) override
    {
        if (! append)
            this->values.clear();
        std::istringstream is(str);
        std::string point_str;
        while (std::getline(is, point_str, ',')) {
            Vec2d point(Vec2d::Zero());
            std::istringstream iss(point_str);
            std::string coord_str;
            if (std::getline(iss, coord_str, 'x')) {
                std::istringstream(coord_str) >> point(0);
                if (std::getline(iss, coord_str, 'x')) {
                    std::istringstream(coord_str) >> point(1);
                }
            }
            this->values.push_back(point);
        }
        return true;
    }

private:
	friend class cereal::access;
	template<class Archive> void save(Archive& archive) const {
		size_t cnt = this->values.size();
		archive(cnt);
		archive.saveBinary((const char*)this->values.data(), sizeof(Vec2d) * cnt);
	}
	template<class Archive> void load(Archive& archive) {
		size_t cnt;
		archive(cnt);
		this->values.assign(cnt, Vec2d());
		archive.loadBinary((char*)this->values.data(), sizeof(Vec2d) * cnt);
	}
};

class ConfigOptionPoint3 : public ConfigOptionSingle<Vec3d>
{
public:
    ConfigOptionPoint3() : ConfigOptionSingle<Vec3d>(Vec3d(0,0,0)) {}
    explicit ConfigOptionPoint3(const Vec3d &value) : ConfigOptionSingle<Vec3d>(value) {}

    static ConfigOptionType static_type() { return coPoint3; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { return new ConfigOptionPoint3(*this); }
    ConfigOptionPoint3&     operator=(const ConfigOption *opt) { this->set(opt); return *this; }
    bool                    operator==(const ConfigOptionPoint3 &rhs) const throw() { return this->value == rhs.value; }
    bool                    operator< (const ConfigOptionPoint3 &rhs) const throw()
        { return this->value.x() < rhs.value.x() || (this->value.x() == rhs.value.x() && (this->value.y() < rhs.value.y() || (this->value.y() == rhs.value.y() && this->value.z() < rhs.value.z()))); }

    std::string serialize() const override
    {
        std::ostringstream ss;
        ss << this->value(0);
        ss << ",";
        ss << this->value(1);
        ss << ",";
        ss << this->value(2);
        return ss.str();
    }

    bool deserialize(const std::string &str, bool append = false) override
    {
        UNUSED(append);
        char dummy;
        return sscanf(str.data(), " %lf , %lf , %lf %c", &this->value(0), &this->value(1), &this->value(2), &dummy) == 3 ||
               sscanf(str.data(), " %lf x %lf x %lf %c", &this->value(0), &this->value(1), &this->value(2), &dummy) == 3;
    }

private:
	friend class cereal::access;
	template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionSingle<Vec3d>>(this)); }
};

class ConfigOptionPointsGroups :public ConfigOptionVector<Vec2ds>
{
public:
    ConfigOptionPointsGroups() :ConfigOptionVector<Vec2ds>() {}
    explicit ConfigOptionPointsGroups(std::initializer_list<Vec2ds> il) :ConfigOptionVector<Vec2ds>(std::move(il)) {}
    explicit ConfigOptionPointsGroups(const std::vector<Vec2ds>& values) :ConfigOptionVector<Vec2ds>(values) {}

    static ConfigOptionType static_type() { return coPointsGroups; }
    ConfigOptionType type()const override { return static_type(); }
    ConfigOption* clone()const override { return new ConfigOptionPointsGroups(*this); }
    ConfigOptionPointsGroups& operator=(const ConfigOption* opt) { this->set(opt); return *this; }
    bool operator == (const ConfigOptionPointsGroups& rhs)const throw() { return this->values == rhs.values; }
    bool operator == (const ConfigOption& rhs) const override {
        if (rhs.type() != this->type())
            throw ConfigurationError("ConfigOptionPointsGroupsTempl: Comparing incompatible types");
        assert(dynamic_cast<const ConfigOptionVector<Vec2ds>*>(&rhs));

        return this->values == static_cast<const ConfigOptionVector<Vec2ds>*>(&rhs)->values;
    }
    bool nullable() const override { return false; }
    bool is_nil(size_t) const override { return false; }

    std::string serialize()const override
    {
        std::ostringstream ss;
        for (auto iter = this->values.begin(); iter != this->values.end(); ++iter) {
            if (iter != this->values.begin())
                ss << "#";
            serialize_single_value(ss, *iter);
        }

        return ss.str();
    }

    std::vector<std::string> vserialize()const override
    {
        std::vector<std::string>ret;
        for (const auto& points : this->values) {
            std::ostringstream ss;
            serialize_single_value(ss, points);
            ret.emplace_back(ss.str());
        }
        return ret;
    }

    bool deserialize(const std::string& str, bool append = false) override
    {
        if (!append)
            this->values.clear();
        std::istringstream is(str);
        std::string group_str;
        while (std::getline(is, group_str, '#')) {
            Vec2ds group;
            std::istringstream iss(group_str);
            std::string point_str;
            while (std::getline(iss, point_str, ',')) {
                Vec2d point(Vec2d::Zero());
                std::istringstream iss(point_str);
                std::string coord_str;
                if (std::getline(iss, coord_str, 'x')) {
                    std::istringstream(coord_str) >> point(0);
                    if (std::getline(iss, coord_str, 'x')) {
                        std::istringstream(coord_str) >> point(1);
                    }
                }
                group.push_back(point);
            }
            this->values.emplace_back(std::move(group));
        }
        return true;
    }
    std::vector<std::string> vserialize_single(int idx) const
    {
        std::vector<std::string>ret;
        assert(idx < this->size());
        for (auto iter = values[idx].begin(); iter != values[idx].end(); ++iter) {
            std::ostringstream ss;
            ss << (*iter)(0);
            ss << "x";
            ss << (*iter)(1);
            ret.emplace_back(ss.str());
        }
        return ret;
    }
protected:
    void serialize_single_value(std::ostringstream& ss, const Vec2ds& v) const {
        for (auto iter = v.begin(); iter != v.end(); ++iter) {
            if (iter - v.begin() != 0)
                ss << ",";
            ss << (*iter)(0);
            ss << "x";
            ss << (*iter)(1);
        }
    }
private:
    friend class cereal::access;
    template<class Archive> void serialize(Archive& ar) { ar(cereal::base_class<ConfigOptionVector>(this)); }
};

class ConfigOptionIntsGroups : public ConfigOptionVector<std::vector<int>>
{
public:
    ConfigOptionIntsGroups() : ConfigOptionVector<std::vector<int>>() {}
    explicit ConfigOptionIntsGroups(std::initializer_list<std::vector<int>> il) : ConfigOptionVector<std::vector<int>>(std::move(il)) {}
    explicit ConfigOptionIntsGroups(const std::vector<std::vector<int>> &values) : ConfigOptionVector<std::vector<int>>(values) {}

    static ConfigOptionType   static_type() { return coIntsGroups; }
    ConfigOptionType          type() const override { return static_type(); }
    ConfigOption             *clone() const override { return new ConfigOptionIntsGroups(*this); }
    ConfigOptionIntsGroups &operator=(const ConfigOption *opt)
    {
        this->set(opt);
        return *this;
    }
    bool operator==(const ConfigOptionIntsGroups &rhs) const throw() { return this->values == rhs.values; }
    bool operator==(const ConfigOption &rhs) const override
    {
        if (rhs.type() != this->type()) throw ConfigurationError("ConfigConfigOptionIntsGroups: Comparing incompatible types");
        assert(dynamic_cast<const ConfigOptionVector<std::vector<int>> *>(&rhs));

        return this->values == static_cast<const ConfigOptionVector<std::vector<int>> *>(&rhs)->values;
    }
    bool operator<(const ConfigOptionIntsGroups &rhs) const throw() {
        bool is_lower = true;
        for (size_t i = 0; i < values.size(); ++i) {
            if (this->values[i] == rhs.values[i])
                continue;

            return (this->values[i] < rhs.values[i]);
        }
        return is_lower;
    }
    bool nullable() const override { return false; }
    bool is_nil(size_t) const override { return false; }

    std::string serialize() const override
    {
        std::ostringstream ss;
        for (auto iter = this->values.begin(); iter != this->values.end(); ++iter) {
            if (iter != this->values.begin())
                ss << "#";
            serialize_single_value(ss, *iter);
        }

        return ss.str();
    }

    std::vector<std::string> vserialize() const override
    {
        std::vector<std::string> ret;
        for (const auto &value : this->values) {
            std::ostringstream ss;
            serialize_single_value(ss, value);
            ret.emplace_back(ss.str());
        }
        return ret;
    }

    bool deserialize(const std::string &str, bool append = false) override
    {
        if (!append) this->values.clear();
        std::istringstream is(str);
        std::string        group_str;
        while (std::getline(is, group_str, '#')) {
            std::vector<int>   group_values;
            std::istringstream iss(group_str);
            std::string        value_str;
            while (std::getline(iss, value_str, ',')) {
                int value;
                std::istringstream(value_str) >> value;
                group_values.push_back(value);
            }
            this->values.emplace_back(std::move(group_values));
        }
        return true;
    }
    std::vector<std::string> vserialize_single(int idx) const
    {
        std::vector<std::string> ret;
        assert(idx < this->size());
        for (auto iter = values[idx].begin(); iter != values[idx].end(); ++iter) {
            std::ostringstream ss;
            ss << (*iter);
            ret.emplace_back(ss.str());
        }
        return ret;
    }

protected:
    void serialize_single_value(std::ostringstream &ss, const std::vector<int> &v) const
    {
        for (auto iter = v.begin(); iter != v.end(); ++iter) {
            if (iter - v.begin() != 0)
                ss << ",";
            ss << (*iter);
        }
    }

private:
    friend class cereal::access;
    template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionVector>(this)); }
};


class ConfigOptionBool : public ConfigOptionSingle<bool>
{
public:
    ConfigOptionBool() : ConfigOptionSingle<bool>(false) {}
    explicit ConfigOptionBool(bool _value) : ConfigOptionSingle<bool>(_value) {}

    static ConfigOptionType static_type() { return coBool; }
    ConfigOptionType        type()      const override { return static_type(); }
    bool                    getBool()   const override { return this->value; }
    ConfigOption*           clone()     const override { return new ConfigOptionBool(*this); }
    ConfigOptionBool&       operator=(const ConfigOption *opt) { this->set(opt); return *this; }
    bool                    operator==(const ConfigOptionBool &rhs) const throw() { return this->value == rhs.value; }
    bool                    operator< (const ConfigOptionBool &rhs) const throw() { return int(this->value) < int(rhs.value); }

    std::string serialize() const override
    {
        return std::string(this->value ? "1" : "0");
    }

    bool deserialize(const std::string &str, bool append = false) override
    {
        UNUSED(append);

        // Orca: take the first value if input is an array
        std::istringstream is(str);
        std::string        item_str;
        if (std::getline(is, item_str, ',')) {
            boost::trim(item_str);

            if (item_str == "1") {
                this->value = true;
                return true;
            }
            if (item_str == "0") {
                this->value = false;
                return true;
            }
        }

        return false;
    }

private:
	friend class cereal::access;
	template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionSingle<bool>>(this)); }
};

template<bool NULLABLE>
class ConfigOptionBoolsTempl : public ConfigOptionVector<unsigned char>
{
public:
    ConfigOptionBoolsTempl() : ConfigOptionVector<unsigned char>() {}
    explicit ConfigOptionBoolsTempl(size_t n, bool value) : ConfigOptionVector<unsigned char>(n, (unsigned char)value) {}
    explicit ConfigOptionBoolsTempl(std::initializer_list<bool> il) { values.reserve(il.size()); for (bool b : il) values.emplace_back((unsigned char)b); }
	explicit ConfigOptionBoolsTempl(std::initializer_list<unsigned char> il) { values.reserve(il.size()); for (unsigned char b : il) values.emplace_back(b); }
	explicit ConfigOptionBoolsTempl(const std::vector<unsigned char>& vec) : ConfigOptionVector<unsigned char>(vec) {}
	explicit ConfigOptionBoolsTempl(std::vector<unsigned char>&& vec) : ConfigOptionVector<unsigned char>(std::move(vec)) {}

    static ConfigOptionType static_type() { return coBools; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { return new ConfigOptionBoolsTempl(*this); }
    ConfigOptionBoolsTempl& operator=(const ConfigOption *opt) { this->set(opt); return *this; }
    bool                    operator==(const ConfigOptionBoolsTempl &rhs) const throw() { return this->values == rhs.values; }
    bool                    operator< (const ConfigOptionBoolsTempl &rhs) const throw() { return this->values <  rhs.values; }
    // Could a special "nil" value be stored inside the vector, indicating undefined value?
    bool 					nullable() const override { return NULLABLE; }
    // Special "nil" value to be stored into the vector if this->supports_nil().
    static unsigned char	nil_value() { return std::numeric_limits<unsigned char>::max(); }
    // A scalar is nil, or all values of a vector are nil.
    bool 					is_nil() const override { for (auto v : this->values) if (v != nil_value()) return false; return true; }
    bool 					is_nil(size_t idx) const override { return this->values[idx] == nil_value(); }
    virtual void set_at_to_nil(size_t i) override
    {
        assert(nullable() && (i < this->values.size()));
        this->values[i] = nil_value();
    }

    bool& get_at(size_t i) {
        assert(! this->values.empty());
        return *reinterpret_cast<bool*>(&((i < this->values.size()) ? this->values[i] : this->values.front()));
    }

    //FIXME this smells, the parent class has the method declared returning (unsigned char&).
    bool get_at(size_t i) const { return ((i < this->values.size()) ? this->values[i] : this->values.front()) != 0; }

    std::string serialize() const override
    {
        std::ostringstream ss;
        for (const unsigned char &v : this->values) {
            if (&v != &this->values.front())
            	ss << ",";
			this->serialize_single_value(ss, v);
		}
        return ss.str();
    }

    std::vector<std::string> vserialize() const override
    {
        std::vector<std::string> vv;
        for (const unsigned char v : this->values) {
			std::ostringstream ss;
			this->serialize_single_value(ss, v);
            vv.push_back(ss.str());
        }
        return vv;
    }

    ConfigHelpers::DeserializationResult deserialize_with_substitutions(const std::string &str, bool append, ConfigHelpers::DeserializationSubstitution substitution)
    {
        if (! append)
            this->values.clear();
        std::istringstream is(str);
        std::string item_str;
        bool substituted = false;
        while (std::getline(is, item_str, ',')) {
        	boost::trim(item_str);
        	unsigned char new_value = 0;
        	if (item_str == "nil") {
        		if (NULLABLE)
                    new_value = nil_value();
        		else
                    throw ConfigurationError("Deserializing nil into a non-nullable object");
        	} else if (item_str == "1") {
        		new_value = true;
        	} else if (item_str == "0") {
        		new_value = false;
        	} else if (substitution != ConfigHelpers::DeserializationSubstitution::Disabled && ConfigHelpers::looks_like_enum_value(item_str)) {
        		new_value = ConfigHelpers::enum_looks_like_true_value(item_str) || substitution == ConfigHelpers::DeserializationSubstitution::DefaultsToTrue;
        		substituted = true;
        	} else
        		return ConfigHelpers::DeserializationResult::Failed;
            this->values.push_back(new_value);
        }
        return substituted ? ConfigHelpers::DeserializationResult::Substituted : ConfigHelpers::DeserializationResult::Loaded;
    }

    bool deserialize(const std::string &str, bool append = false) override
    {
    	return this->deserialize_with_substitutions(str, append, ConfigHelpers::DeserializationSubstitution::Disabled) == ConfigHelpers::DeserializationResult::Loaded;
    }

protected:
	void serialize_single_value(std::ostringstream &ss, const unsigned char v) const {
        	if (v == nil_value()) {
        		if (NULLABLE)
        			ss << "nil";
        		else
                    throw ConfigurationError("Serializing NaN");
        	} else
        		ss << (v ? "1" : "0");
	}

private:
	friend class cereal::access;
	template<class Archive> void serialize(Archive &ar) { ar(cereal::base_class<ConfigOptionVector<unsigned char>>(this)); }
};

using ConfigOptionBools    	    = ConfigOptionBoolsTempl<false>;
using ConfigOptionBoolsNullable = ConfigOptionBoolsTempl<true>;

// Map from an enum integer value to an enum name.
typedef std::vector<std::string>  t_config_enum_names;
// Map from an enum name to an enum integer value.
typedef std::map<std::string,int> t_config_enum_values;

template <class T>
class ConfigOptionEnum : public ConfigOptionSingle<T>
{
public:
    // by default, use the first value (0) of the T enum type
    ConfigOptionEnum() : ConfigOptionSingle<T>(static_cast<T>(0)) {}
    explicit ConfigOptionEnum(T _value) : ConfigOptionSingle<T>(_value) {}

    static ConfigOptionType static_type() { return coEnum; }
    ConfigOptionType        type()  const override { return static_type(); }
    ConfigOption*           clone() const override { return new ConfigOptionEnum<T>(*this); }
    ConfigOptionEnum<T>&    operator=(const ConfigOption *opt) { this->set(opt); return *this; }
    bool                    operator==(const ConfigOptionEnum<T> &rhs) const throw() { return this->value == rhs.value; }
    bool                    operator< (const ConfigOptionEnum<T> &rhs) const throw() { return int(this->value) < int(rhs.value); }
    int                     getInt() const override { return (int)this->value; }
    void                    setInt(int val) override { this->value = T(val); }

    bool operator==(const ConfigOption &rhs) const override
    {
        if (rhs.type() != this->type())
            throw ConfigurationError("ConfigOptionEnum<T>: Comparing incompatible types");
        // rhs could be of the following type: ConfigOptionEnumGeneric or ConfigOptionEnum<T>
        return this->value == (T)rhs.getInt();
    }

    void set(const ConfigOption *rhs) override {
        if (rhs->type() != this->type())
            throw ConfigurationError("ConfigOptionEnum<T>: Assigning an incompatible type");
        // rhs could be of the following type: ConfigOptionEnumGeneric or ConfigOptionEnum<T>
        this->value = (T)rhs->getInt();
    }

    std::string serialize() const override
    {
        const t_config_enum_names& names = ConfigOptionEnum<T>::get_enum_names();
        assert(static_cast<int>(this->value) < int(names.size()));
        return names[static_cast<int>(this->value)];
    }

    bool deserialize(const std::string &str, bool append = false) override
    {
        UNUSED(append);
        return from_string(str, this->value);
    }

    static bool has(T value)
    {
        for (const auto &kvp : ConfigOptionEnum<T>::get_enum_values())
            if (kvp.second == value)
                return true;
        return false;
    }

    // Map from an enum name to an enum integer value.
    static const t_config_enum_names& get_enum_names();
    // Map from an enum name to an enum integer value.
    static const t_config_enum_values& get_enum_values();

    static bool from_string(const std::string &str, T &value)
    {
        const t_config_enum_values &enum_keys_map = ConfigOptionEnum<T>::get_enum_values();
        auto it = enum_keys_map.find(str);
        if (it == enum_keys_map.end())
            return false;
        value = static_cast<T>(it->second);
        return true;
    }
};

// Generic enum configuration value.
// We use this one in DynamicConfig objects when creating a config value object for ConfigOptionType == coEnum.
// In the StaticConfig, it is better to use the specialized ConfigOptionEnum<T> containers.
class ConfigOptionEnumGeneric : public ConfigOptionInt
{
public:
    ConfigOptionEnumGeneric(const t_config_enum_values* keys_map = nullptr) : keys_map(keys_map) {}
    explicit ConfigOptionEnumGeneric(const t_config_enum_values* keys_map, int value) : ConfigOptionInt(value), keys_map(keys_map) {}

    const t_config_enum_values* keys_map;

    static ConfigOptionType     static_type() { return coEnum; }
    ConfigOptionType            type()  const override { return static_type(); }
    ConfigOption*               clone() const override { return new ConfigOptionEnumGeneric(*this); }
    ConfigOptionEnumGeneric&    operator= (const ConfigOption *opt) { this->set(opt); return *this; }
    bool                        operator==(const ConfigOptionEnumGeneric &rhs) const throw() { return this->value == rhs.value; }
    bool                        operator< (const ConfigOptionEnumGeneric &rhs) const throw() { return this->value <  rhs.value; }

    bool operator==(const ConfigOption &rhs) const override
    {
        if (rhs.type() != this->type())
            throw ConfigurationError("ConfigOptionEnumGeneric: Comparing incompatible types");
        // rhs could be of the following type: ConfigOptionEnumGeneric or ConfigOptionEnum<T>
        return this->value == rhs.getInt();
    }

    void set(const ConfigOption *rhs) override {
        if (rhs->type() != this->type())
            throw ConfigurationError("ConfigOptionEnumGeneric: Assigning an incompatible type");
        // rhs could be of the following type: ConfigOptionEnumGeneric or ConfigOptionEnum<T>
        this->value = rhs->getInt();
    }

    std::string serialize() const override
    {
        for (const auto &kvp : *this->keys_map)
            if (kvp.second == this->value)
                return kvp.first;
        return std::string();
    }

    bool deserialize(const std::string &str, bool append = false) override
    {
        UNUSED(append);
        auto it = this->keys_map->find(str);
        if (it == this->keys_map->end())
            return false;
        this->value = it->second;
        return true;
    }

private:
	friend class cereal::access;
	template<class Archive> void serialize(Archive& ar) { ar(cereal::base_class<ConfigOptionInt>(this)); }
};

// BBS
template <bool NULLABLE>
class ConfigOptionEnumsGenericTempl : public ConfigOptionInts
{
public:
    ConfigOptionEnumsGenericTempl(const t_config_enum_values *keys_map = nullptr) : keys_map(keys_map) {}
    explicit ConfigOptionEnumsGenericTempl(const t_config_enum_values *keys_map, size_t size, int value) : ConfigOptionInts(size, value), keys_map(keys_map) {}
    explicit ConfigOptionEnumsGenericTempl(std::initializer_list<int> il) : ConfigOptionInts(std::move(il)), keys_map(keys_map) {}
    explicit ConfigOptionEnumsGenericTempl(const std::vector<int> &vec) : ConfigOptionInts(vec) {}
    explicit ConfigOptionEnumsGenericTempl(std::vector<int> &&vec) : ConfigOptionInts(std::move(vec)) {}

    const t_config_enum_values* keys_map = nullptr;

    static ConfigOptionType     static_type() { return coEnums; }
    ConfigOptionType            type()  const override { return static_type(); }
    ConfigOption* clone() const override { return new ConfigOptionEnumsGenericTempl(*this); }
    ConfigOptionEnumsGenericTempl& operator= (const ConfigOption* opt) { this->set(opt); return *this; }
    bool                        operator< (const ConfigOptionInts& rhs) const throw() { return this->values < rhs.values; }

    bool                        operator==(const ConfigOptionInts& rhs) const
    {
        if (rhs.type() != this->type())
            throw ConfigurationError("ConfigOptionEnumsGeneric: Comparing incompatible types");
        return this->values == rhs.values;
    }
    bool nullable() const override { return NULLABLE; }

    void set(const ConfigOption* rhs) override {
        if (rhs->type() != this->type())
            throw ConfigurationError("ConfigOptionEnumGeneric: Assigning an incompatible type");
        // rhs could be of the following type: ConfigOptionEnumsGeneric
        this->values = dynamic_cast<const ConfigOptionEnumsGenericTempl *>(rhs)->values;
    }

    std::string serialize() const override
    {
        std::ostringstream ss;
        for (const int& v : this->values) {
            if (&v != &this->values.front())
                ss << ",";
            serialize_single_value(ss, v);
        }
        return ss.str();
    }

    std::vector<std::string> vserialize() const override
    {
        std::vector<std::string> vv;
        vv.reserve(this->values.size());
        for (const int v : this->values) {
            std::ostringstream ss;
            serialize_single_value(ss, v);
            vv.push_back(ss.str());
        }
        return vv;
    }

    bool deserialize(const std::string& str, bool append = false) override
    {
        if (!append)
            this->values.clear();
        std::istringstream is(str);
        std::string item_str;
        while (std::getline(is, item_str, ',')) {
            boost::trim(item_str);
            if (item_str == "nil") {
                if (NULLABLE)
                    this->values.push_back(nil_value());
                else
                    throw ConfigurationError("Deserializing nil into a non-nullable object");
            }
            else {
                auto it = this->keys_map->find(item_str);
                if (it == this->keys_map->end())
                    return false;
                this->values.push_back(it->second);
            }
        }
        return true;
    }

private:
    void serialize_single_value(std::ostringstream& ss, const int v) const
    {
        if (v == nil_value()) {
            if (NULLABLE)
                ss << "nil";
            else
                throw ConfigurationError("Serializing NaN");
        }
        else {
            for (const auto& kvp : *this->keys_map)
                if (kvp.second == v)
                    ss << kvp.first;
        }
    }

    friend class cereal::access;
    template<class Archive> void serialize(Archive& ar) { ar(cereal::base_class<ConfigOptionVector<int>>(this)); }
};

using ConfigOptionEnumsGeneric         = ConfigOptionEnumsGenericTempl<false>;
using ConfigOptionEnumsGenericNullable = ConfigOptionEnumsGenericTempl<true>;

// Definition of a configuration value for the purpose of GUI presentation, editing, value mapping and config file handling.
class ConfigOptionDef
{
public:
    enum class GUIType {
        undefined,
        // Open enums, integer value could be one of the enumerated values or something else.
        i_enum_open,
        // Open enums, float value could be one of the enumerated values or something else.
        f_enum_open,
        // Color picker, string value.
        color,
        // ???
        select_open,
        // Currently unused.
        slider,
        // Static text
        legend,
        // Vector value, but edited as a single string.
        one_string,
    };

	// Identifier of this option. It is stored here so that it is accessible through the by_serialization_key_ordinal map.
	t_config_option_key 				opt_key;
    // What type? bool, int, string etc.
    ConfigOptionType                    type            = coNone;
	// If a type is nullable, then it accepts a "nil" value (scalar) or "nil" values (vector).
	bool								nullable		= false;
    // Default value of this option. The default value object is owned by ConfigDef, it is released in its destructor.
    Slic3r::clonable_ptr<const ConfigOption> default_value;
    void 								set_default_value(const ConfigOption* ptr) { this->default_value = Slic3r::clonable_ptr<const ConfigOption>(ptr); }
    template<typename T> const T* 		get_default_value() const { return static_cast<const T*>(this->default_value.get()); }

    // Create an empty option to be used as a base for deserialization of DynamicConfig.
    ConfigOption*						create_empty_option() const;
    // Create a default option to be inserted into a DynamicConfig.
    ConfigOption*						create_default_option() const;

    bool                                is_scalar()     const { return (int(this->type) & int(coVectorType)) == 0; }

    template<class Archive> ConfigOption* load_option_from_archive(Archive& archive) const
    {
        if (this->nullable) {
            switch (this->type) {
            case coFloats: {
                auto opt = new ConfigOptionFloatsNullable();
                archive(*opt);
                return opt;
            }
            case coInts: {
                auto opt = new ConfigOptionIntsNullable();
                archive(*opt);
                return opt;
            }
            case coPercents: {
                auto opt = new ConfigOptionPercentsNullable();
                archive(*opt);
                return opt;
            }
            case coBools: {
                auto opt = new ConfigOptionBoolsNullable();
                archive(*opt);
                return opt;
            }
            case coFloatsOrPercents: {
                auto opt = new ConfigOptionFloatsOrPercentsNullable();
                archive(*opt);
                return opt;
            }
            case coEnums: {
                auto opt = new ConfigOptionEnumsGenericNullable(this->enum_keys_map);
                archive(*opt);
                return opt;
            }
            default:
                throw ConfigurationError(
                    std::string("ConfigOptionDef::load_option_from_archive(): Unknown nullable option type for option ") + this->opt_key);
            }
        } else {
            switch (this->type) {
            case coFloat: {
                auto opt = new ConfigOptionFloat();
                archive(*opt);
                return opt;
            }
            case coFloats: {
                auto opt = new ConfigOptionFloats();
                archive(*opt);
                return opt;
            }
            case coInt: {
                auto opt = new ConfigOptionInt();
                archive(*opt);
                return opt;
            }
            case coInts: {
                auto opt = new ConfigOptionInts();
                archive(*opt);
                return opt;
            }
            case coString: {
                auto opt = new ConfigOptionString();
                archive(*opt);
                return opt;
            }
            case coStrings: {
                auto opt = new ConfigOptionStrings();
                archive(*opt);
                return opt;
            }
            case coPercent: {
                auto opt = new ConfigOptionPercent();
                archive(*opt);
                return opt;
            }
            case coPercents: {
                auto opt = new ConfigOptionPercents();
                archive(*opt);
                return opt;
            }
            case coFloatOrPercent: {
                auto opt = new ConfigOptionFloatOrPercent();
                archive(*opt);
                return opt;
            }
            case coFloatsOrPercents: {
                auto opt = new ConfigOptionFloatsOrPercents();
                archive(*opt);
                return opt;
            }
            case coPoint: {
                auto opt = new ConfigOptionPoint();
                archive(*opt);
                return opt;
            }
            case coPoints: {
                auto opt = new ConfigOptionPoints();
                archive(*opt);
                return opt;
            }
            case coPoint3: {
                auto opt = new ConfigOptionPoint3();
                archive(*opt);
                return opt;
            }
            case coBool: {
                auto opt = new ConfigOptionBool();
                archive(*opt);
                return opt;
            }
            case coBools: {
                auto opt = new ConfigOptionBools();
                archive(*opt);
                return opt;
            }
            case coEnum: {
                auto opt = new ConfigOptionEnumGeneric(this->enum_keys_map);
                archive(*opt);
                return opt;
            }
            // BBS
            case coEnums: {
                auto opt = new ConfigOptionEnumsGeneric(this->enum_keys_map);
                archive(*opt);
                return opt;
            }
            case coIntsGroups: {
                auto opt = new ConfigOptionIntsGroups();
                archive(*opt);
                return opt;
            }
            case coPointsGroups: {
                auto opt = new ConfigOptionPointsGroups();
                archive(*opt);
                return opt;
            }
            default:
                throw ConfigurationError(std::string("ConfigOptionDef::load_option_from_archive(): Unknown option type for option ") +
                                         this->opt_key);
            }
        }
    }

    template<class Archive> ConfigOption* save_option_to_archive(Archive &archive, const ConfigOption *opt) const {
    	if (this->nullable) {
		    switch (this->type) {
		    case coFloats:          archive(*static_cast<const ConfigOptionFloatsNullable*>(opt));  break;
		    case coInts:            archive(*static_cast<const ConfigOptionIntsNullable*>(opt));    break;
		    case coPercents:        archive(*static_cast<const ConfigOptionPercentsNullable*>(opt));break;
		    case coBools:           archive(*static_cast<const ConfigOptionBoolsNullable*>(opt)); 	break;
            case coFloatsOrPercents: archive(*static_cast<const ConfigOptionFloatsOrPercentsNullable*>(opt)); break;
            case coEnums: archive(*static_cast<const ConfigOptionEnumsGenericNullable*>(opt)); break;
            default:
                throw ConfigurationError(
                    std::string("ConfigOptionDef::save_option_to_archive(): Unknown nullable option type for option ") + this->opt_key);
            }
        } else {
            switch (this->type) {
            case coFloat: archive(*static_cast<const ConfigOptionFloat*>(opt)); break;
            case coFloats: archive(*static_cast<const ConfigOptionFloats*>(opt)); break;
            case coInt: archive(*static_cast<const ConfigOptionInt*>(opt)); break;
            case coInts: archive(*static_cast<const ConfigOptionInts*>(opt)); break;
            case coString: archive(*static_cast<const ConfigOptionString*>(opt)); break;
            case coStrings: archive(*static_cast<const ConfigOptionStrings*>(opt)); break;
            case coPercent: archive(*static_cast<const ConfigOptionPercent*>(opt)); break;
            case coPercents: archive(*static_cast<const ConfigOptionPercents*>(opt)); break;
            case coFloatOrPercent: archive(*static_cast<const ConfigOptionFloatOrPercent*>(opt)); break;
            case coFloatsOrPercents: archive(*static_cast<const ConfigOptionFloatsOrPercents*>(opt)); break;
            case coPoint: archive(*static_cast<const ConfigOptionPoint*>(opt)); break;
            case coPoints: archive(*static_cast<const ConfigOptionPoints*>(opt)); break;
            case coPoint3: archive(*static_cast<const ConfigOptionPoint3*>(opt)); break;
            case coBool: archive(*static_cast<const ConfigOptionBool*>(opt)); break;
            case coBools: archive(*static_cast<const ConfigOptionBools*>(opt)); break;
            case coEnum: archive(*static_cast<const ConfigOptionEnumGeneric*>(opt)); break;
            // BBS
            case coEnums: archive(*static_cast<const ConfigOptionEnumsGeneric*>(opt)); break;
            case coIntsGroups: archive(*static_cast<const ConfigOptionIntsGroups*>(opt)); break;
            case coPointsGroups: archive(*static_cast<const ConfigOptionPointsGroups*>(opt)); break;
            default:
                throw ConfigurationError(std::string("ConfigOptionDef::save_option_to_archive(): Unknown option type for option ") +
                                         this->opt_key);
            }
        }
        // Make the compiler happy, shut up the warnings.
        return nullptr;
    }

    // Usually empty.
    // Special values - "i_enum_open", "f_enum_open" to provide combo box for int or float selection,
    // "select_open" - to open a selection dialog (currently only a serial port selection).
    GUIType                             gui_type { GUIType::undefined };
    // Usually empty. Otherwise "serialized" or "show_value"
    // The flags may be combined.
    // "serialized" - vector valued option is entered in a single edit field. Values are separated by a semicolon.
    // "show_value" - even if enum_values / enum_labels are set, still display the value, not the enum label.
    std::string                         gui_flags;
    // Label of the GUI input field.
    // In case the GUI input fields are grouped in some views, the label defines a short label of a grouped value,
    // while full_label contains a label of a stand-alone field.
    // The full label is shown, when adding an override parameter for an object or a modified object.
    std::string                         label;
    std::string                         full_label;
    // With which printer technology is this configuration valid?
    PrinterTechnology                   printer_technology = ptUnknown;
    // Category of a configuration field, from the GUI perspective.
    // One of: "Layers and Perimeters", "Infill", "Support material", "Speed", "Extruders", "Advanced", "Extrusion Width"
    std::string                         category;
    // A tooltip text shown in the GUI.
    std::string                         tooltip;
    // Text right from the input field, usually a unit of measurement.
    std::string                         sidetext;
    // Format of this parameter on a command line.
    std::string                         cli;
    //BBS: add cli command line params
    std::string                         cli_params;
    // Set for type == coFloatOrPercent.
    // It provides a link to a configuration value, of which this option provides a ratio.
    // For example,
    // For example outer_wall_speed may be defined as a fraction of inner_wall_speed.
    t_config_option_key                 ratio_over;
    // True for multiline strings.
    bool                                multiline       = false;
    // For text input: If true, the GUI text box spans the complete page width.
    bool                                full_width      = false;
    // For text input: If true, the GUI formats text as code (fixed-width)
    bool                                is_code         = false;
    // Not editable. Currently only used for the display of the number of threads.
    bool                                readonly        = false;
    // Height of a multiline GUI text box.
    int                                 height          = -1;
    // Optional width of an input field.
    int                                 width           = -1;
    // <min, max> limit of a numeric input.
    // If not set, the <min, max> is set to <-FLT_MAX, FLT_MAX>
    // By setting min=0, only nonnegative input is allowed.
    float                               min = -FLT_MAX;
    float                               max =  FLT_MAX;
    bool                                is_value_valid(double value, int max_precision = 4) const;
    // To check if it's not a typo and a % is missing
    double                              max_literal = 1;
    ConfigOptionMode                    mode = comSimple;
    // Legacy names for this configuration option.
    // Used when parsing legacy configuration file.
    std::vector<t_config_option_key>    aliases;
    // Sometimes a single value may well define multiple values in a "beginner" mode.
    // Currently used for aliasing "solid_layers" to "top_shell_layers", "bottom_shell_layers".
    std::vector<t_config_option_key>    shortcut;
    // Definition of values / labels for a combo box.
    // Mostly used for enums (when type == coEnum), but may be used for ints resp. floats, if gui_type is set to "i_enum_open" resp. "f_enum_open".
    std::vector<std::string>            enum_values;
    std::vector<std::string>            enum_labels;
    // For enums (when type == coEnum). Maps enum_values to enums.
    // Initialized by ConfigOptionEnum<xxx>::get_enum_values()
    const t_config_enum_values         *enum_keys_map   = nullptr;

    bool has_enum_value(const std::string &value) const {
        for (const std::string &v : enum_values)
            if (v == value)
                return true;
        return false;
    }

    // 0 is an invalid key.
    size_t 								serialization_key_ordinal = 0;

    // Returns the alternative CLI arguments for the given option.
    // If there are no cli arguments defined, use the key and replace underscores with dashes.
    std::vector<std::string> cli_args(const std::string &key) const;

    // Assign this key to cli to disable CLI for this option.
    static const constexpr char *nocli =  "~~~noCLI";
};

inline bool operator<(const ConfigSubstitution &lhs, const ConfigSubstitution &rhs) throw() {
    return lhs.opt_def->opt_key < rhs.opt_def->opt_key ||
           (lhs.opt_def->opt_key == rhs.opt_def->opt_key && lhs.old_value < rhs.old_value);
}
inline bool operator==(const ConfigSubstitution &lhs, const ConfigSubstitution &rhs) throw() {
    return lhs.opt_def == rhs.opt_def && lhs.old_value == rhs.old_value;
}

// Map from a config option name to its definition.
// The definition does not carry an actual value of the config option, only its constant default value.
// t_config_option_key is std::string
typedef std::map<t_config_option_key, ConfigOptionDef> t_optiondef_map;

// Definition of configuration values for the purpose of GUI presentation, editing, value mapping and config file handling.
// The configuration definition is static: It does not carry the actual configuration values,
// but it carries the defaults of the configuration values.
class ConfigDef
{
public:
    t_optiondef_map         					options;
    std::map<size_t, const ConfigOptionDef*>	by_serialization_key_ordinal;

    bool                    has(const t_config_option_key &opt_key) const { return this->options.count(opt_key) > 0; }
    const ConfigOptionDef*  get(const t_config_option_key &opt_key) const {
        t_optiondef_map::iterator it = const_cast<ConfigDef*>(this)->options.find(opt_key);
        return (it == this->options.end()) ? nullptr : &it->second;
    }
    std::vector<std::string> keys() const {
        std::vector<std::string> out;
        out.reserve(options.size());
        for(auto const& kvp : options)
            out.push_back(kvp.first);
        return out;
    }
    bool                    empty() { return options.empty(); }

    // Iterate through all of the CLI options and write them to a stream.
    std::ostream&           print_cli_help(
        std::ostream& out, bool show_defaults,
        std::function<bool(const ConfigOptionDef &)> filter = [](const ConfigOptionDef &){ return true; }) const;

protected:
    ConfigOptionDef*        add(const t_config_option_key &opt_key, ConfigOptionType type);
    ConfigOptionDef*        add_nullable(const t_config_option_key &opt_key, ConfigOptionType type);
};

// A pure interface to resolving ConfigOptions.
// This pure interface is useful as a base of ConfigBase, also it may be overriden to combine
// various config sources.
class ConfigOptionResolver
{
public:
    ConfigOptionResolver() {}
    virtual ~ConfigOptionResolver() {}

    // Find a ConfigOption instance for a given name.
    virtual const ConfigOption* optptr(const t_config_option_key &opt_key) const = 0;

    bool 						has(const t_config_option_key &opt_key) const { return this->optptr(opt_key) != nullptr; }

    const ConfigOption* 		option(const t_config_option_key &opt_key) const { return this->optptr(opt_key); }

    template<typename TYPE>
    const TYPE* 				option(const t_config_option_key& opt_key) const
    {
        const ConfigOption* opt = this->optptr(opt_key);
        return (opt == nullptr || opt->type() != TYPE::static_type()) ? nullptr : static_cast<const TYPE*>(opt);
    }

    const ConfigOption* 		option_throw(const t_config_option_key& opt_key) const
    {
        const ConfigOption* opt = this->optptr(opt_key);
        if (opt == nullptr)
            throw UnknownOptionException(opt_key);
        return opt;
    }

    template<typename TYPE>
    const TYPE* 				option_throw(const t_config_option_key& opt_key) const
    {
        const ConfigOption* opt = this->option_throw(opt_key);
        if (opt->type() != TYPE::static_type())
            throw BadOptionTypeException("Conversion to a wrong type");
        return static_cast<TYPE*>(opt);
    }
};



// An abstract configuration store.
class ConfigBase : public ConfigOptionResolver
{
public:
    // Definition of configuration values for the purpose of GUI presentation, editing, value mapping and config file handling.
    // The configuration definition is static: It does not carry the actual configuration values,
    // but it carries the defaults of the configuration values.

    ConfigBase() = default;
    ~ConfigBase() override = default;

    // Virtual overridables:
public:
    // Static configuration definition. Any value stored into this ConfigBase shall have its definition here.
    virtual const ConfigDef*        def() const = 0;
    // Find ando/or create a ConfigOption instance for a given name.
    using ConfigOptionResolver::optptr;
    virtual ConfigOption*           optptr(const t_config_option_key &opt_key, bool create = false) = 0;
    // Collect names of all configuration values maintained by this configuration store.
    virtual t_config_option_keys    keys() const = 0;

protected:
    // Verify whether the opt_key has not been obsoleted or renamed.
    // Both opt_key and value may be modified by handle_legacy().
    // If the opt_key is no more valid in this version of Slic3r, opt_key is cleared by handle_legacy().
    // handle_legacy() is called internally by set_deserialize().
    virtual void                    handle_legacy(t_config_option_key &/*opt_key*/, std::string &/*value*/) const {}
    // Called after a config is loaded as a whole.
    // Perform composite conversions, for example merging multiple keys into one key.
    // For conversion of single options, the handle_legacy() method above is called.
    virtual void                    handle_legacy_composite() {}

public:
	using ConfigOptionResolver::option;
	using ConfigOptionResolver::option_throw;

    // Non-virtual methods:
    ConfigOption* option(const t_config_option_key &opt_key, bool create = false)
        { return this->optptr(opt_key, create); }

    template<typename TYPE>
    TYPE* option(const t_config_option_key &opt_key, bool create = false)
    {
        ConfigOption* opt = this->optptr(opt_key, create);
        if (opt != nullptr && opt->type() != TYPE::static_type()) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": attempt to access option with wrong type: " << opt_key;
            return nullptr;
        }
        return static_cast<TYPE*>(opt);
    }

    ConfigOption* option_throw(const t_config_option_key &opt_key, bool create = false)
    {
        ConfigOption *opt = this->optptr(opt_key, create);
        if (opt == nullptr)
            throw UnknownOptionException(opt_key);
        return opt;
    }

    template<typename TYPE>
    TYPE* option_throw(const t_config_option_key &opt_key, bool create = false)
    {
        ConfigOption *opt = this->option_throw(opt_key, create);
        if (opt->type() != TYPE::static_type())
            throw BadOptionTypeException("Conversion to a wrong type");
        return static_cast<TYPE*>(opt);
    }

    // Apply all keys of other ConfigBase defined by this->def() to this ConfigBase.
    // An UnknownOptionException is thrown in case some option keys of other are not defined by this->def(),
    // or this ConfigBase is of a StaticConfig type and it does not support some of the keys, and ignore_nonexistent is not set.
    void apply(const ConfigBase &other, bool ignore_nonexistent = false) { this->apply_only(other, other.keys(), ignore_nonexistent); }
    // Apply explicitely enumerated keys of other ConfigBase defined by this->def() to this ConfigBase.
    // An UnknownOptionException is thrown in case some option keys are not defined by this->def(),
    // or this ConfigBase is of a StaticConfig type and it does not support some of the keys, and ignore_nonexistent is not set.
    void apply_only(const ConfigBase &other, const t_config_option_keys &keys, bool ignore_nonexistent = false);

    // Are the two configs equal? Ignoring options not present in both configs.
    //BBS: add skipped_keys logic
    bool equals(const ConfigBase &other, const std::set<std::string>* skipped_keys = nullptr) const;
    // Returns options differing in the two configs, ignoring options not present in both configs.
    t_config_option_keys diff(const ConfigBase &other) const;
    // Returns options being equal in the two configs, ignoring options not present in both configs.
    t_config_option_keys equal(const ConfigBase &other) const;
    std::string opt_serialize(const t_config_option_key &opt_key) const;

    // Set a value. Convert numeric types using a C style implicit conversion / promotion model.
    // Throw if option is not avaiable and create is not enabled,
    // or if the conversion is not possible.
    // Conversion to string is always possible.
    void set(const std::string &opt_key, bool  				value, bool create = false)
    	{ this->option_throw<ConfigOptionBool>(opt_key, create)->value = value; }
    void set(const std::string &opt_key, int   				value, bool create = false);
    void set(const std::string &opt_key, double				value, bool create = false);
    void set(const std::string &opt_key, const char		   *value, bool create = false)
    	{ this->option_throw<ConfigOptionString>(opt_key, create)->value = value; }
    void set(const std::string &opt_key, const std::string &value, bool create = false)
    	{ this->option_throw<ConfigOptionString>(opt_key, create)->value = value; }

    // Set a configuration value from a string, it will call an overridable handle_legacy()
    // to resolve renamed and removed configuration keys.
    bool set_deserialize_nothrow(const t_config_option_key &opt_key_src, const std::string &value_src, ConfigSubstitutionContext& substitutions, bool append = false);
	// May throw BadOptionTypeException() if the operation fails.
    void set_deserialize(const t_config_option_key &opt_key, const std::string &str, ConfigSubstitutionContext& config_substitutions, bool append = false);
    void set_deserialize_strict(const t_config_option_key &opt_key, const std::string &str, bool append = false)
        { ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable }; this->set_deserialize(opt_key, str, ctxt, append); }
    struct SetDeserializeItem {
    	SetDeserializeItem(const char *opt_key, const char *opt_value, bool append = false) : opt_key(opt_key), opt_value(opt_value), append(append) {}
    	SetDeserializeItem(const std::string &opt_key, const std::string &opt_value, bool append = false) : opt_key(opt_key), opt_value(opt_value), append(append) {}
    	SetDeserializeItem(const char *opt_key, const bool value, bool append = false) : opt_key(opt_key), opt_value(value ? "1" : "0"), append(append) {}
    	SetDeserializeItem(const std::string &opt_key, const bool value, bool append = false) : opt_key(opt_key), opt_value(value ? "1" : "0"), append(append) {}
    	SetDeserializeItem(const char *opt_key, const int value, bool append = false) : opt_key(opt_key), opt_value(std::to_string(value)), append(append) {}
    	SetDeserializeItem(const std::string &opt_key, const int value, bool append = false) : opt_key(opt_key), opt_value(std::to_string(value)), append(append) {}
        SetDeserializeItem(const char *opt_key, const float value, bool append = false) : opt_key(opt_key), opt_value(float_to_string_decimal_point(value)), append(append) {}
        SetDeserializeItem(const std::string &opt_key, const float value, bool append = false) : opt_key(opt_key), opt_value(float_to_string_decimal_point(value)), append(append) {}
        SetDeserializeItem(const char *opt_key, const double value, bool append = false) : opt_key(opt_key), opt_value(float_to_string_decimal_point(value)), append(append) {}
        SetDeserializeItem(const std::string &opt_key, const double value, bool append = false) : opt_key(opt_key), opt_value(float_to_string_decimal_point(value)), append(append) {}
    	std::string opt_key; std::string opt_value; bool append = false;
    };
	// May throw BadOptionTypeException() if the operation fails.
    void set_deserialize(std::initializer_list<SetDeserializeItem> items, ConfigSubstitutionContext& substitutions);
    void set_deserialize_strict(std::initializer_list<SetDeserializeItem> items)
        { ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable }; this->set_deserialize(items, ctxt); }

    double get_abs_value(const t_config_option_key &opt_key) const;
    double get_abs_value(const t_config_option_key &opt_key, double ratio_over) const;
    void setenv_() const;
    ConfigSubstitutions load(const std::string &file, ForwardCompatibilitySubstitutionRule compatibility_rule);
    //BBS support load from ini string
    ConfigSubstitutions load_string_map(std::map<std::string, std::string> &key_values, ForwardCompatibilitySubstitutionRule compatibility_rule);
    //BBS: add json support
    int load_from_json(const std::string &file, ConfigSubstitutionContext& substitutions, bool load_inherits_in_config, std::map<std::string, std::string>& key_values, std::string& reason);
    ConfigSubstitutions load_from_json(const std::string &file, ForwardCompatibilitySubstitutionRule compatibility_rule, std::map<std::string, std::string>& key_values, std::string& reason);

    ConfigSubstitutions load_from_ini(const std::string &file, ForwardCompatibilitySubstitutionRule compatibility_rule);
    ConfigSubstitutions load_from_ini_string(const std::string &data, ForwardCompatibilitySubstitutionRule compatibility_rule);
    // Loading a "will be one day a legacy format" of configuration stored into 3MF or AMF.
    // Accepts the same data as load_from_ini_string(), only with each configuration line possibly prefixed with a semicolon (G-code comment).
    ConfigSubstitutions load_from_ini_string_commented(std::string &&data, ForwardCompatibilitySubstitutionRule compatibility_rule);
    ConfigSubstitutions load_from_gcode_file(const std::string &file, ForwardCompatibilitySubstitutionRule compatibility_rule);
    ConfigSubstitutions load(const boost::property_tree::ptree &tree, ForwardCompatibilitySubstitutionRule compatibility_rule);
    void save(const std::string &file) const;

    //BBS: add json support
    void save_to_json(const std::string &file, const std::string &name, const std::string &from, const std::string &version) const;

	// Set all the nullable values to nils.
    void null_nullables();

    static size_t load_from_gcode_string_legacy(ConfigBase& config, const char* str, ConfigSubstitutionContext& substitutions);

private:
    // Set a configuration value from a string.
    bool set_deserialize_raw(const t_config_option_key& opt_key_src, const std::string& value, ConfigSubstitutionContext& substitutions, bool append);
};

// Configuration store with dynamic number of configuration values.
// In Slic3r, the dynamic config is mostly used at the user interface layer.
class DynamicConfig : public virtual ConfigBase
{
public:
    DynamicConfig() = default;
    DynamicConfig(const DynamicConfig &rhs) { *this = rhs; }
    DynamicConfig(DynamicConfig &&rhs) noexcept : options(std::move(rhs.options)) { rhs.options.clear(); }
	explicit DynamicConfig(const ConfigBase &rhs, const t_config_option_keys &keys);
	explicit DynamicConfig(const ConfigBase& rhs) : DynamicConfig(rhs, rhs.keys()) {}
	virtual ~DynamicConfig() override = default;

    // Copy a content of one DynamicConfig to another DynamicConfig.
    // If rhs.def() is not null, then it has to be equal to this->def().
    DynamicConfig& operator=(const DynamicConfig &rhs)
    {
        assert(this->def() == nullptr || this->def() == rhs.def());
        this->clear();
        for (const auto &kvp : rhs.options)
            this->options[kvp.first].reset(kvp.second->clone());
        return *this;
    }

    // Move a content of one DynamicConfig to another DynamicConfig.
    // If rhs.def() is not null, then it has to be equal to this->def().
    DynamicConfig& operator=(DynamicConfig &&rhs) noexcept
    {
        assert(this->def() == nullptr || this->def() == rhs.def());
        this->clear();
        this->options = std::move(rhs.options);
        rhs.options.clear();
        return *this;
    }

    // Add a content of one DynamicConfig to another DynamicConfig.
    // If rhs.def() is not null, then it has to be equal to this->def().
    DynamicConfig& operator+=(const DynamicConfig &rhs)
    {
        assert(this->def() == nullptr || this->def() == rhs.def());
        for (const auto &kvp : rhs.options) {
            auto it = this->options.find(kvp.first);
            if (it == this->options.end())
                this->options[kvp.first].reset(kvp.second->clone());
            else {
                assert(it->second->type() == kvp.second->type());
                if (it->second->type() == kvp.second->type())
                    *it->second = *kvp.second;
                else
                    it->second.reset(kvp.second->clone());
            }
        }
        return *this;
    }

    // Move a content of one DynamicConfig to another DynamicConfig.
    // If rhs.def() is not null, then it has to be equal to this->def().
    DynamicConfig& operator+=(DynamicConfig &&rhs)
    {
        assert(this->def() == nullptr || this->def() == rhs.def());
        for (auto &kvp : rhs.options) {
            auto it = this->options.find(kvp.first);
            if (it == this->options.end()) {
                this->options.insert(std::make_pair(kvp.first, std::move(kvp.second)));
            } else {
                assert(it->second->type() == kvp.second->type());
                it->second = std::move(kvp.second);
            }
        }
        rhs.options.clear();
        return *this;
    }

    bool           operator==(const DynamicConfig &rhs) const;
    bool           operator!=(const DynamicConfig &rhs) const { return ! (*this == rhs); }

    void swap(DynamicConfig &other)
    {
        std::swap(this->options, other.options);
    }

    void clear()
    {
        this->options.clear();
    }

    bool erase(const t_config_option_key &opt_key)
    {
        auto it = this->options.find(opt_key);
        if (it == this->options.end())
            return false;
        this->options.erase(it);
        return true;
    }

    // Remove options with all nil values, those are optional and it does not help to hold them.
    size_t remove_nil_options();

    // Allow DynamicConfig to be instantiated on ints own without a definition.
    // If the definition is not defined, the method requiring the definition will throw NoDefinitionException.
    const ConfigDef*        def() const override { return nullptr; }
    template<class T> T*    opt(const t_config_option_key &opt_key, bool create = false)
        { return dynamic_cast<T*>(this->option(opt_key, create)); }
    template<class T> const T* opt(const t_config_option_key &opt_key) const
        { return dynamic_cast<const T*>(this->option(opt_key)); }
    // Overrides ConfigResolver::optptr().
    const ConfigOption*     optptr(const t_config_option_key &opt_key) const override;
    // Overrides ConfigBase::optptr(). Find ando/or create a ConfigOption instance for a given name.
    ConfigOption*           optptr(const t_config_option_key &opt_key, bool create = false) override;
    // Overrides ConfigBase::keys(). Collect names of all configuration values maintained by this configuration store.
    t_config_option_keys    keys() const override;
    bool                    empty() const { return options.empty(); }

    // Set a value for an opt_key. Returns true if the value did not exist yet.
    // This DynamicConfig will take ownership of opt.
    // Be careful, as this method does not test the existence of opt_key in this->def().
    bool                    set_key_value(const std::string &opt_key, ConfigOption *opt)
    {
        auto it = this->options.find(opt_key);
        if (it == this->options.end()) {
            this->options[opt_key].reset(opt);
            return true;
        } else {
            it->second.reset(opt);
            return false;
        }
    }

    // Are the two configs equal? Ignoring options not present in both configs.
    //BBS: add skipped_keys logic
    bool equals(const DynamicConfig &other, const std::set<std::string>* skipped_keys = nullptr) const;
    // Returns options differing in the two configs, ignoring options not present in both configs.
    t_config_option_keys diff(const DynamicConfig &other) const;
    // Returns options being equal in the two configs, ignoring options not present in both configs.
    t_config_option_keys equal(const DynamicConfig &other) const;

    std::string&        opt_string(const t_config_option_key &opt_key, bool create = false)     { return this->option<ConfigOptionString>(opt_key, create)->value; }
    const std::string&  opt_string(const t_config_option_key &opt_key) const                    { return const_cast<DynamicConfig*>(this)->opt_string(opt_key); }
    std::string&        opt_string(const t_config_option_key &opt_key, unsigned int idx)        { return this->option<ConfigOptionStrings>(opt_key)->get_at(idx); }
    const std::string&  opt_string(const t_config_option_key &opt_key, unsigned int idx) const  { return const_cast<DynamicConfig*>(this)->opt_string(opt_key, idx); }

    double&             opt_float(const t_config_option_key &opt_key)                           { return this->option<ConfigOptionFloat>(opt_key)->value; }
    const double&       opt_float(const t_config_option_key &opt_key) const                     { return dynamic_cast<const ConfigOptionFloat*>(this->option(opt_key))->value; }
    double&             opt_float(const t_config_option_key &opt_key, unsigned int idx)         { return this->option<ConfigOptionFloats>(opt_key)->get_at(idx); }
    const double&       opt_float(const t_config_option_key &opt_key, unsigned int idx) const   { return dynamic_cast<const ConfigOptionFloats*>(this->option(opt_key))->get_at(idx); }

    int&                opt_int(const t_config_option_key &opt_key)                             { return this->option<ConfigOptionInt>(opt_key)->value; }
    int                 opt_int(const t_config_option_key &opt_key) const                       { return dynamic_cast<const ConfigOptionInt*>(this->option(opt_key))->value; }
    int&                opt_int(const t_config_option_key &opt_key, unsigned int idx)           { return this->option<ConfigOptionInts>(opt_key)->get_at(idx); }
    int                 opt_int(const t_config_option_key &opt_key, unsigned int idx) const     { return dynamic_cast<const ConfigOptionInts*>(this->option(opt_key))->get_at(idx); }

    // In ConfigManipulation::toggle_print_fff_options, it is called on option with type ConfigOptionEnumGeneric* and also ConfigOptionEnum*.
    // Thus the virtual method getInt() is used to retrieve the enum value.
    template<typename ENUM>
    ENUM                opt_enum(const t_config_option_key &opt_key) const                      { return static_cast<ENUM>(this->option(opt_key)->getInt()); }
    // BBS
    int                 opt_enum(const t_config_option_key &opt_key, unsigned int idx) const    { return dynamic_cast<const ConfigOptionEnumsGeneric*>(this->option(opt_key))->get_at(idx); }

    bool                opt_bool(const t_config_option_key &opt_key) const                      { return this->option<ConfigOptionBool>(opt_key)->value != 0; }
    bool                opt_bool(const t_config_option_key &opt_key, unsigned int idx) const    { return this->option<ConfigOptionBools>(opt_key)->get_at(idx) != 0; }

    // Command line processing
    bool                read_cli(int argc, const char* const argv[], t_config_option_keys* extra, t_config_option_keys* keys = nullptr);

    std::map<t_config_option_key, std::unique_ptr<ConfigOption>>::const_iterator cbegin() const { return options.cbegin(); }
    std::map<t_config_option_key, std::unique_ptr<ConfigOption>>::const_iterator cend()   const { return options.cend(); }
    size_t                        												 size()   const { return options.size(); }

    /**
     * @brief Detailed information about the difference found for a single key.
     */
    struct KeyDifference {
        std::optional<std::string> left_value;
        std::optional<std::string> right_value;

        bool is_missing_key() const {
            return !left_value.has_value() || !right_value.has_value();
        }
        bool is_different_value() const {
            return left_value.has_value() && right_value.has_value() && (left_value.value() != right_value.value());
        }
    };

    /**
     * @brief The full report object containing all detected differences.
     */
    struct DynamicConfigDifference {
        std::map<t_config_option_key, KeyDifference> differences;

        bool is_different() const {
            return !differences.empty();
        }
    };

    /**
     * @brief Computes the symmetric difference between this DynamicConfig (left)
     * and another DynamicConfig (rhs).
     * @param rhs The right-hand side config to compare against.
     * @return DynamicConfigDifference report.
     */
    DynamicConfigDifference diff_report(const DynamicConfig& rhs) const;

private:
    std::map<t_config_option_key, std::unique_ptr<ConfigOption>> options;

	friend class cereal::access;
	template<class Archive> void serialize(Archive &ar) { ar(options); }
};

std::ostream& operator<<(std::ostream& os, const DynamicConfig::DynamicConfigDifference& diff);

// Configuration store with a static definition of configuration values.
// In Slic3r, the static configuration stores are during the slicing / g-code generation for efficiency reasons,
// because the configuration values could be accessed directly.
class StaticConfig : public virtual ConfigBase
{
public:
    StaticConfig() {}
    /// Gets list of config option names for each config option of this->def, which has a static counter-part defined by the derived object
    /// and which could be resolved by this->optptr(key) call.
    t_config_option_keys keys() const;

protected:
    /// Set all statically defined config options to their defaults defined by this->def().
    void set_defaults();
};

}

#endif
