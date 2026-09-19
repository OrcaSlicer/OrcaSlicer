#ifndef slic3r_CrealityMaterialCatalog_hpp_
#define slic3r_CrealityMaterialCatalog_hpp_

#include <map>
#include <string>

namespace Slic3r {

// Maps a Creality CFS `filamentId` to (vendor, product, base type).
//
// The CFS `box` object / MIFARE tag report a numeric filamentId (e.g. "103001").
// Its last 5 digits are the catalogue id (the leading digit is a printer-series
// prefix). Resolving the code to the exact product ("Hyper PLA", "CR-PLA Matte",
// ...) lets auto-assign pick the precise filament preset instead of guessing by
// nearest colour among same-type slots.
//
// Source: Creality's on-printer material database
// (/mnt/UDISK/creality/userdata/box/material_database.json), mirrored by the
// community K2-RFID project (db/{k1,k2,hi}.json). 66 entries, deduped.
struct CrealityMaterial { const char* vendor; const char* product; const char* type; };

inline const std::map<std::string, CrealityMaterial>& creality_material_catalog()
{
    static const std::map<std::string, CrealityMaterial> tbl = {
        {"00001", {"Generic", "Generic PLA", "PLA"}},
        {"00002", {"Generic", "Generic PLA-Silk", "PLA"}},
        {"00003", {"Generic", "Generic PETG", "PETG"}},
        {"00004", {"Generic", "Generic ABS", "ABS"}},
        {"00005", {"Generic", "Generic TPU", "TPU"}},
        {"00006", {"Generic", "Generic PLA-CF", "PLA-CF"}},
        {"00007", {"Generic", "Generic ASA", "ASA"}},
        {"00008", {"Generic", "Generic PA", "PA"}},
        {"00009", {"Generic", "Generic PA-CF", "PA-CF"}},
        {"00010", {"Generic", "Generic BVOH", "BVOH"}},
        {"00011", {"Generic", "Generic PVA", "PVA"}},
        {"00012", {"Generic", "Generic HIPS", "HIPS"}},
        {"00013", {"Generic", "Generic PET-CF", "PET-CF"}},
        {"00014", {"Generic", "Generic PETG-CF", "PETG-CF"}},
        {"00015", {"Generic", "Generic PA6-CF", "PA6-CF"}},
        {"00016", {"Generic", "Generic PAHT-CF", "PAHT-CF"}},
        {"00017", {"Generic", "Generic PPS", "PPS"}},
        {"00018", {"Generic", "Generic PPS-CF", "PPS-CF"}},
        {"00019", {"Generic", "Generic PP", "PP"}},
        {"00020", {"Generic", "Generic PET", "PET"}},
        {"00021", {"Generic", "Generic PC", "PC"}},
        {"00022", {"Generic", "Generic PA612-CF", "PA-CF"}},
        {"00023", {"Generic", "Generic Support for PA", "PA"}},
        {"00024", {"Generic", "Generic Support for PLA", "PLA"}},
        {"00025", {"Generic", "Generic PA12-CF", "PA-CF"}},
        {"00026", {"Generic", "Generic TPU 64D", "TPU"}},
        {"00027", {"Generic", "Generic PETG-GF", "PETG-GF"}},
        {"00031", {"Generic", "Generic PP-CF", "PP-CF"}},
        {"00032", {"Generic", "Generic PCTG", "PCTG"}},
        {"00033", {"Generic", "Generic ASA-CF", "ASA-CF"}},
        {"00034", {"Generic", "Generic PA6-GF", "PA-GF"}},
        {"00035", {"eSUN", "PLA-LW", "PLA"}},
        {"01001", {"Creality", "Hyper PLA", "PLA"}},
        {"01002", {"Creality", "Hyper L-W PLA", "PLA"}},
        {"01004", {"Creality", "Hyper Stardust", "PLA"}},
        {"01601", {"Creality", "Soleyin Ultra PLA", "PLA"}},
        {"02001", {"Creality", "Hyper PLA-CF", "PLA-CF"}},
        {"03001", {"Creality", "Hyper ABS", "ABS"}},
        {"04001", {"Creality", "CR-PLA", "PLA"}},
        {"05001", {"Creality", "CR-Silk", "PLA"}},
        {"06001", {"Creality", "CR-PETG", "PETG"}},
        {"06002", {"Creality", "Hyper PETG", "PETG"}},
        {"06003", {"Creality", "Hyper PETG-CF", "PETG-CF"}},
        {"07001", {"Creality", "CR-ABS", "ABS"}},
        {"07002", {"Creality", "Hyper PC", "PC"}},
        {"08001", {"Creality", "Ender-PLA", "PLA"}},
        {"09001", {"Creality", "EN-PLA+", "PLA"}},
        {"09002", {"Creality", "ENDER FAST PLA", "PLA"}},
        {"10001", {"Creality", "HP-TPU", "TPU"}},
        {"11001", {"Creality", "CR-Nylon", "PA"}},
        {"12002", {"Creality", "Hyper PPA-CF", "PA-CF"}},
        {"12003", {"Creality", "Hyper PAHT-CF", "PA-CF"}},
        {"12004", {"Creality", "Hyper PA612-CF", "PA612-CF"}},
        {"12005", {"Creality", "Hyper PA6-CF", "PA6-CF"}},
        {"13001", {"Creality", "CR-PLA Carbon", "PLA-CF"}},
        {"14001", {"Creality", "CR-PLA Matte", "PLA"}},
        {"15001", {"Creality", "CR-PLA Fluo", "PLA"}},
        {"16001", {"Creality", "CR-TPU", "TPU"}},
        {"17001", {"Creality", "CR-Wood", "PLA"}},
        {"18001", {"Creality", "HP Ultra PLA", "PLA"}},
        {"19001", {"Creality", "HP-ASA", "ASA"}},
        {"29001", {"Creality", "Hyper Marble", "PLA"}},
        {"E1001", {"eSUN", "PLA+", "PLA"}},
        {"P1001", {"Polymaker", "Panchroma PLA Satin", "PLA"}},
        {"P1002", {"Polymaker", "PolySonic PLA Pro", "PLA"}},
        {"P1003", {"Polymaker", "Panchroma PLA Matte", "PLA"}},
    };
    return tbl;
}

// Resolve a CFS filamentId (5- or 6-digit; the last 5 digits are the catalogue id)
// to vendor/product/type. Returns false when the code is unknown.
inline bool creality_cfs_lookup(const std::string& code,
                                std::string&       vendor,
                                std::string&       product,
                                std::string&       type)
{
    const auto& tbl = creality_material_catalog();
    auto try_key = [&](const std::string& k) {
        auto it = tbl.find(k);
        if (it == tbl.end()) return false;
        vendor  = it->second.vendor;
        product = it->second.product;
        type    = it->second.type;
        return true;
    };
    if (try_key(code)) return true;
    if (code.size() > 5 && try_key(code.substr(code.size() - 5))) return true;
    return false;
}

} // namespace Slic3r

#endif // slic3r_CrealityMaterialCatalog_hpp_
