#ifndef ARCHIVETRAITS_HPP
#define ARCHIVETRAITS_HPP

#include <string>

#include "libslic3r/Zipper.hpp"
#include "libslic3r/SLAPrint.hpp"

namespace Slic3r {

class SL1Archive: public SLAArchive {
    SLAPrinterConfig m_cfg;
    
protected:
    std::unique_ptr<sla::RasterBase> create_raster() const override;
    sla::RasterEncoder get_encoder() const override;

public:
    
    SL1Archive() = default;
    explicit SL1Archive(const SLAPrinterConfig &cfg): m_cfg(cfg) {}
    explicit SL1Archive(SLAPrinterConfig &&cfg): m_cfg(std::move(cfg)) {}
    
    void export_print(Zipper &zipper, const SLAPrint &print, const std::string &projectname = "");
    void export_print(const std::string &fname, const SLAPrint &print, const std::string &projectname = "")
    {
        Zipper zipper(fname);
        export_print(zipper, print, projectname);
    }
    
    void apply(const SLAPrinterConfig &cfg) override
    {
        auto diff = m_cfg.diff(cfg);
        if (!diff.empty()) {
            m_cfg.apply_only(cfg, diff);
            m_layers = {};
        }
    }
};
    
ConfigSubstitutions import_sla_archive(const std::string &zipfname, DynamicPrintConfig &out);

ConfigSubstitutions import_sla_archive(
    const std::string &      zipfname,
    Vec2i32                    windowsize,
    indexed_triangle_set &   out,
    DynamicPrintConfig &     profile,
    std::function<bool(int)> progr = [](int) { return true; });

inline ConfigSubstitutions import_sla_archive(
    const std::string &      zipfname,
    Vec2i32                    windowsize,
    indexed_triangle_set &   out,
    std::function<bool(int)> progr = [](int) { return true; })
{
    DynamicPrintConfig profile;
    return import_sla_archive(zipfname, windowsize, out, profile, progr);
}

class MissingProfileError : public RuntimeError { using RuntimeError::RuntimeError; };

} // namespace Slic3r::sla

#endif // ARCHIVETRAITS_HPP
