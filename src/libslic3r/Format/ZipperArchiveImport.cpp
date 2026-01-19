#include "ZipperArchiveImport.hpp"

#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/property_tree/ini_parser.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/algorithm/string.hpp>

namespace Slic3r {

namespace {

// Read an ini file into boost property tree
boost::property_tree::ptree read_ini(const mz_zip_archive_file_stat &entry,
                                     MZ_Archive                     &zip)
{
    std::string buf(size_t(entry.m_uncomp_size), '\0');

    if (!mz_zip_reader_extract_to_mem(&zip.arch, entry.m_file_index,
                                           buf.data(), buf.size(), 0))
        throw Slic3r::FileIOError(zip.get_errorstr());

    boost::property_tree::ptree tree;
    std::stringstream           ss(buf);
    boost::property_tree::read_ini(ss, tree);
    return tree;
}

// Read an arbitrary file into EntryBuffer
EntryBuffer read_entry(const mz_zip_archive_file_stat &entry,
                       MZ_Archive                     &zip,
                       const std::string              &name)
{
    std::vector<uint8_t> buf(entry.m_uncomp_size);

    if (!mz_zip_reader_extract_to_mem(&zip.arch, entry.m_file_index,
                                           buf.data(), buf.size(), 0))
        throw Slic3r::FileIOError(zip.get_errorstr());

    return {std::move(buf), (name.empty() ? entry.m_filename : name)};
}

} // namespace

ZipperArchive read_zipper_archive(const std::string &zipfname,
                                  const std::vector<std::string> &includes,
                                  const std::vector<std::string> &excludes)
{
    ZipperArchive arch;

    // Little RAII
    struct Arch : public MZ_Archive
    {
        Arch(const std::string &fname)
        {
            if (!open_zip_reader(&arch, fname))
                throw Slic3r::FileIOError(get_errorstr());
        }

        ~Arch() { close_zip_reader(&arch); }
    } zip(zipfname);

    mz_uint num_entries = mz_zip_reader_get_num_files(&zip.arch);

    for (mz_uint i = 0; i < num_entries; ++i) {
        mz_zip_archive_file_stat entry;

        if (mz_zip_reader_file_stat(&zip.arch, i, &entry)) {
            std::string name = entry.m_filename;
            boost::algorithm::to_lower(name);

            if (!std::any_of(includes.begin(), includes.end(),
                            [&name](const std::string &incl) {
                                return boost::algorithm::contains(name, incl);
                            }))
                continue;

            if (std::any_of(excludes.begin(), excludes.end(),
                            [&name](const std::string &excl) {
                                return boost::algorithm::contains(name, excl);
                            }))
                continue;

            if (name == CONFIG_FNAME)  {
                arch.config = read_ini(entry, zip);
                continue;
            }

            if (name == PROFILE_FNAME) {
                arch.profile = read_ini(entry, zip);
                continue;
            }

            auto it = std::lower_bound(
                arch.entries.begin(), arch.entries.end(),
                EntryBuffer{{}, name},
                [](const EntryBuffer &r1, const EntryBuffer &r2) {
                    return std::less<std::string>()(r1.fname, r2.fname);
                });

            arch.entries.insert(it, read_entry(entry, zip, name));
        }
    }

    return arch;
}

std::pair<DynamicPrintConfig, ConfigSubstitutions> extract_profile(
    const ZipperArchive &arch, DynamicPrintConfig &profile_out)
{
    DynamicPrintConfig profile_in, profile_use;
    ConfigSubstitutions config_substitutions =
        profile_in.load(arch.profile,
                        ForwardCompatibilitySubstitutionRule::Enable);

    if (profile_in.empty()) { // missing profile... do guess work
        // try to recover the layer height from the config.ini which was
        // present in all versions of sl1 files.
        if (auto lh_opt = arch.config.find("layerHeight");
            lh_opt != arch.config.not_found()) {
            auto lh_str = lh_opt->second.data();

            size_t pos = 0;
            double lh = string_to_double_decimal_point(lh_str, &pos);
            if (pos) { // TODO: verify that pos is 0 when parsing fails
                profile_out.set("layer_height", lh);
                profile_out.set("initial_layer_height", lh);
            }
        }
    }

    // If the archive contains an empty profile, use the one that was passed
    // as output argument then replace it with the readed profile to report
    // that it was empty.
    profile_use = profile_in.empty() ? profile_out : profile_in;
    profile_out = profile_in;

    return {profile_use, std::move(config_substitutions)};
}

} // namespace Slic3r
