#include "CreatePresetsDialog.hpp"
#include <boost/log/trivial.hpp>
#include <vector>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <openssl/md5.h>
#include <openssl/evp.h>
#include <wx/dcgraph.h>
#include <wx/tooltip.h>
#include <boost/nowide/cstdio.hpp>
#include "libslic3r/PresetBundle.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "MsgDialog.hpp"
#include "FileHelp.hpp"
#include "Tab.hpp"
#include "MainFrame.hpp"
#include "libslic3r_version.h"

#define NAME_OPTION_COMBOBOX_SIZE wxSize(FromDIP(200), FromDIP(24))
#define FILAMENT_PRESET_COMBOBOX_SIZE wxSize(FromDIP(300), FromDIP(24))
#define OPTION_SIZE wxSize(FromDIP(100), FromDIP(24))
#define PRINTER_LIST_SIZE wxSize(-1, FromDIP(100))
#define FILAMENT_LIST_SIZE wxSize(FromDIP(560), FromDIP(100))
#define FILAMENT_OPTION_SIZE wxSize(FromDIP(-1), FromDIP(30))
#define PRESET_TEMPLATE_SIZE wxSize(FromDIP(-1), FromDIP(100))
#define PRINTER_SPACE_SIZE wxSize(FromDIP(100), FromDIP(24)) // ORCA Match size with other components
#define ORIGIN_TEXT_SIZE wxSize(FromDIP(10), FromDIP(24))
#define PRINTER_PRESET_VENDOR_SIZE wxSize(FromDIP(150), FromDIP(24))
#define PRINTER_PRESET_MODEL_SIZE wxSize(FromDIP(280), FromDIP(24))
#define STATIC_TEXT_COLOUR wxColour("#363636")
#define PRINTER_LIST_COLOUR wxColour("#EEEEEE")
#define FILAMENT_OPTION_COLOUR wxColour("#D9D9D9")
#define SELECT_ALL_OPTION_COLOUR wxColour("#009688")
#define DEFAULT_PROMPT_TEXT_COLOUR wxColour("#ACACAC")

namespace Slic3r {
namespace GUI {

  static const std::vector<std::string> filament_vendors = 
    {"3Dgenius",               "3DJake",                 "3DXTECH",                "3D BEST-Q",              "3D Hero",
     "3D-Fuel",                "Aceaddity",              "AddNorth",               "Amazon Basics",          "AMOLEN",
     "Ankermake",              "Anycubic",               "Atomic",                 "AzureFilm",              "BASF",
     "Bblife",                 "BCN3D",                  "Beyond Plastic",         "California Filament",    "Capricorn",
     "CC3D",                   "colorFabb",              "Comgrow",                "Cookiecad",              "Creality",
     "CERPRiSE",               "Das Filament",           "DO3D",                   "DOW",                    "DREMC",
     "DSM",                    "Duramic",                "ELEGOO",                 "Eryone",                 "Essentium",
     "eSUN",                   "Extrudr",                "Fiberforce",             "Fiberlogy",              "FilaCube",
     "Filamentive",            "Fillamentum",            "FLASHFORGE",            "Formfutura",             "Francofil",
     "FusRock",                "FilamentOne",            "Fil X",                  "GEEETECH",               "Giantarm",
     "Gizmo Dorks",            "GreenGate3D",            "HATCHBOX",               "Hello3D",                "IC3D",
     "IEMAI",                  "IIID Max",               "INLAND",                 "iProspect",              "iSANMATE",
     "Justmaker",             "Keene Village Plastics",  "Kexcelled",              "LDO",                    "MakerBot",
     "MatterHackers",         "MIKA3D",                  "NinjaTek",               "Nobufil",                "Novamaker",
     "OVERTURE",              "OVVNYXE",                 "Polymaker",              "Priline",                "Printed Solid",
     "Protopasta",            "Prusament",               "Push Plastic",           "R3D",                    "Re-pet3D",
     "Recreus",               "Regen",                   "RatRig",                 "Sain SMART",             "SliceWorx",
     "Snapmaker",             "SnoLabs",                 "Spectrum",               "SUNLU",                  "TTYT3D",
     "Tianse",                "UltiMaker",               "Valment",                "Verbatim",               "VO3D",
     "Voxelab",               "VOXELPLA",                "YOOPAI",                 "Yousu",                  "Ziro", 
     "Zyltech"};
     
static const std::vector<std::string> filament_types = {"PLA",    "rPLA",  "PLA+",      "PLA Tough", "PETG",  "ABS",    "ASA",    "FLEX",   "HIPS",   "PA",     "PACF",
                                                        "NYLON",  "PVA",   "PVB",       "PC",        "PCABS", "PCTG",   "PCCF",   "PHA",    "PP",     "PEI",    "PET",
                                                        "PETGCF", "PTBA",  "PTBA90A",   "PEEK",  "TPU93A", "TPU75D", "TPU",       "TPU92A", "TPU98A", "Misc",
                                                        "TPE",    "GLAZE", "Nylon",     "CPE",   "METAL",  "ABST",   "Carbon Fiber", "SBS"};

static const std::vector<std::string> printer_vendors = 
    {"Anker",              "Anycubic",           "Artillery",          "Bambulab",           "BIQU",
     "Blocks",             "Chuanying",          "Co Print",           "Comgrow",            "CONSTRUCT3D",
     "Creality",           "DeltaMaker",         "Dremel",             "Elegoo",             "Flashforge",
     "FLSun",              "FlyingBear",         "Folgertech",         "Geeetech",           "Ginger Additive",
     "InfiMech",           "Kingroon",           "Lulzbot",            "MagicMaker",         "Mellow",
     "Orca Arena Printer", "Peopoly",            "Positron 3D",        "Prusa",              "Qidi",
     "Raise3D",            "RatRig",             "RolohaunDesign",     "SecKit",             "Snapmaker",
     "Sovol",              "Thinker X400",       "Tronxy",             "TwoTrees",           "UltiMaker",
     "Vivedino",           "Volumic",            "Voron",              "Voxelab",            "Vzbot",
     "Wanhao",             "Z-Bolt"};

static const std::unordered_map<std::string, std::vector<std::string>> printer_model_map =
    {{"Anker",             {"Anker M5",                   "Anker M5 All-Metal Hot End", "Anker M5C"}},
     {"Anycubic",          {"Anycubic i3 Mega S",    "Anycubic Chiron",       "Anycubic Vyper",        "Anycubic Kobra",        "Anycubic Kobra Max",
                            "Anycubic Kobra Plus",   "Anycubic 4Max Pro",     "Anycubic 4Max Pro 2",   "Anycubic Kobra 2",      "Anycubic Kobra 2 Plus",
                            "Anycubic Kobra 2 Max",  "Anycubic Kobra 2 Pro",  "Anycubic Kobra 2 Neo",  "Anycubic Kobra 3",      "Anycubic Kobra S1", "Anycubic Predator", }},
     {"Artillery",         {"Artillery Sidewinder X1",      "Artillery Genius",             "Artillery Genius Pro",         "Artillery Sidewinder X2",      "Artillery Hornet",
                            "Artillery Sidewinder X3 Pro",  "Artillery Sidewinder X3 Plus", "Artillery Sidewinder X4 Pro",  "Artillery Sidewinder X4 Plus"}},
     {"Bambulab",          {"Bambu Lab X1 Carbon", "Bambu Lab X1",        "Bambu Lab X1E",       "Bambu Lab P1P",       "Bambu Lab P1S",
                            "Bambu Lab A1 mini",   "Bambu Lab A1"}},
     {"BIQU",              {"BIQU B1",      "BIQU BX",      "BIQU Hurakan"}},
     {"Blocks",            {"BLOCKS Pro S100", "BLOCKS RD50 V2",  "BLOCKS RF50"}},
     {"Chuanying",         {"Chuanying X1"}},
     {"Co Print",          {"Co Print ChromaSet"}},
     {"Comgrow",           {"Comgrow T300", "Comgrow T500"}},
     {"CONSTRUCT3D",       {"Construct 1 XL", "Construct 1"}},
     {"Creality",          {"Creality CR-10 V2",           "Creality CR-10 Max",          "Creality CR-10 SE",           "Creality CR-6 SE",            "Creality CR-6 Max",
                            "Creality CR-M4",              "Creality Ender-3 V2",         "Creality Ender-3 V2 Neo",     "Creality Ender-3 S1",         "Creality Ender-3",
                            "Creality Ender-3 Pro",        "Creality Ender-3 S1 Pro",     "Creality Ender-3 S1 Plus",    "Creality Ender-3 V3 SE",      "Creality Ender-3 V3 KE",
                            "Creality Ender-3 V3",         "Creality Ender-3 V3 Plus",    "Creality Ender-5",            "Creality Ender-5 Max",        "Creality Ender-5 Plus",
                            "Creality Ender-5 Pro (2019)", "Creality Ender-5S",           "Creality Ender-5 S1",         "Creality Ender-6",            "Creality Sermoon V1",
                            "Creality K1",                 "Creality K1C",                "Creality K1 Max",             "Creality K1 SE",              "Creality K2 Plus",
                            "Creality Hi"}},
     {"DeltaMaker",        {"DeltaMaker 2",   "DeltaMaker 2T",  "DeltaMaker 2XT"}},
     {"Dremel",            {"Dremel 3D20", "Dremel 3D40", "Dremel 3D45"}},
     {"Elegoo",            {"Elegoo Centauri Carbon",  "Elegoo Centauri",         "Elegoo Neptune",          "Elegoo Neptune X",        "Elegoo Neptune 2",
                            "Elegoo Neptune 2S",       "Elegoo Neptune 2D",       "Elegoo Neptune 3",        "Elegoo Neptune 3 Pro",    "Elegoo Neptune 3 Plus",
                            "Elegoo Neptune 3 Max",    "Elegoo Neptune 4 Pro",    "Elegoo Neptune 4",        "Elegoo Neptune 4 Max",    "Elegoo Neptune 4 Plus",
                            "Elegoo OrangeStorm Giga"}},
     {"Flashforge",        {"Flashforge Adventurer 5M",       "Flashforge Adventurer 5M Pro",   "Flashforge AD5X",                "Flashforge Adventurer 3 Series", "Flashforge Adventurer 4 Series",
                            "Flashforge Guider 3 Ultra",      "Flashforge Guider 2s"}},
     {"FLSun",             {"FLSun Q5",               "FLSun QQ-S Pro",         "FLSun Super Racer (SR)", "FLSun V400",             "FLSun T1",
                            "FLSun S1"}},
     {"FlyingBear",        {"FlyingBear Reborn3", "FlyingBear S1",      "FlyingBear Ghost 6"}},
     {"Folgertech",        {"Folgertech i3",   "Folgertech FT-5", "Folgertech FT-6"}},
     {"Geeetech",          {"Geeetech Thunder",   "Geeetech Mizar M",   "Geeetech Mizar S",   "Geeetech Mizar Pro", "Geeetech Mizar Max",
                            "Geeetech Mizar",     "Geeetech A10 Pro",   "Geeetech A10 M",     "Geeetech A10 T",     "Geeetech A20",
                            "Geeetech A20 M",     "Geeetech A20 T",     "Geeetech A30 Pro",   "Geeetech A30 M",     "Geeetech A30 T",
                            "Geeetech M1"}},
     {"Ginger Additive",   {"ginger G1"}},
     {"InfiMech",          {"InfiMech TX",                       "InfiMech TX Hardened Steel Nozzle"}},
     {"Kingroon",          {"Kingroon KP3S PRO S1", "Kingroon KP3S PRO V2", "Kingroon KP3S 3.0",    "Kingroon KP3S V1",     "Kingroon KLP1"}},
     {"Lulzbot",           {"Lulzbot Taz 6",        "Lulzbot Taz 4 or 5",   "Lulzbot Taz Pro Dual", "Lulzbot Taz Pro S"}},
     {"MagicMaker",        {"MM hqs hj",   "MM hqs SF",   "MM hj SK",    "MM BoneKing", "MM slb"}},
     {"Mellow",            {"M1"}},
     {"Orca Arena Printer",{"Orca Arena X1 Carbon"}},
     {"Peopoly",           {"Peopoly Magneto X"}},
     {"Positron 3D",       {"The Positron"}},
     {"Prusa",             {"Prusa CORE One", "Prusa CORE One HF", "MK4IS", "MK4S", "MK4S HF",
                            "Prusa XL", "Prusa XL 5T", "MK3.5", "MK3S", "MINI", "MINIIS"}},
     {"Qidi",              {"Qidi X-Plus 4",  "Qidi Q1 Pro",    "Qidi X-Max 3",   "Qidi X-Plus 3",  "Qidi X-Smart 3",
                            "Qidi X-Plus",    "Qidi X-Max",     "Qidi X-CF Pro"}},
     {"Raise3D",           {"Raise3D Pro3",      "Raise3D Pro3 Plus"}},
     {"RatRig",            {"RatRig V-Core 3 200",                  "RatRig V-Core 3 300",                  "RatRig V-Core 3 400",                  "RatRig V-Core 3 500",                  "RatRig V-Minion",
                            "RatRig V-Cast",                        "RatRig V-Core 4 300",                  "RatRig V-Core 4 400",                  "RatRig V-Core 4 500",                  "RatRig V-Core 4 HYBRID 300",
                            "RatRig V-Core 4 HYBRID 400",           "RatRig V-Core 4 HYBRID 500",           "RatRig V-Core 4 IDEX 300",             "RatRig V-Core 4 IDEX 300 COPY MODE",   "RatRig V-Core 4 IDEX 300 MIRROR MODE",
                            "RatRig V-Core 4 IDEX 400",             "RatRig V-Core 4 IDEX 400 COPY MODE",   "RatRig V-Core 4 IDEX 400 MIRROR MODE", "RatRig V-Core 4 IDEX 500",             "RatRig V-Core 4 IDEX 500 COPY MODE",
                            "RatRig V-Core 4 IDEX 500 MIRROR MODE"}},
     {"RolohaunDesign",    {"Rook MK1 LDO"}},
     {"SecKit",            {"SecKit SK-Tank", "Seckit Go3"}},
     {"Snapmaker",         {"Snapmaker J1",                 "Snapmaker A250",               "Snapmaker A350",               "Snapmaker A250 Dual",          "Snapmaker A350 Dual",
                            "Snapmaker A250 QSKit",         "Snapmaker A350 QSKit",         "Snapmaker A250 BKit",          "Snapmaker A350 BKit",          "Snapmaker A250 QS+B Kit",
                            "Snapmaker A350 QS+B Kit",      "Snapmaker A250 Dual QSKit",    "Snapmaker A350 Dual QSKit",    "Snapmaker A250 Dual BKit",     "Snapmaker A350 Dual BKit",
                            "Snapmaker A250 Dual QS+B Kit", "Snapmaker A350 Dual QS+B Kit", "Snapmaker Artisan"}},
     {"Sovol",             {"Sovol SV01 Pro",      "Sovol SV02",          "Sovol SV05",          "Sovol SV06",          "Sovol SV06 Plus",
                            "Sovol SV06 ACE",      "Sovol SV06 Plus ACE", "Sovol SV07",          "Sovol SV07 Plus",     "Sovol SV08"}},
     {"Thinker X400",      {"Thinker X400"}},
     {"Tronxy",            {"Tronxy X5SA 400 Marlin Firmware"}},
     {"TwoTrees",          {"TwoTrees SP-5 Klipper", "TwoTrees SK1"}},
     {"UltiMaker",         {"UltiMaker 2"}},
     {"Vivedino",          {"Troodon 2.0 - RRF",     "Troodon 2.0 - Klipper"}},
     {"Volumic",           {"EXO42 Performance", "EXO65 Performance", "SH65 Performance",  "EXO42",             "EXO65",           
                            "SH65",              "VS30SC2",           "VS30SC",            "VS30ULTRA",         "VS30MK3",         
                            "VS30MK2",           "VS20MK2"}},
     {"Voron",             {"Voron 2.4 250",        "Voron 2.4 300",        "Voron 2.4 350",        "Voron Trident 250",    "Voron Trident 300",
                            "Voron Trident 350",    "Voron 0.1",            "Voron Switchwire 250"}},
     {"Voxelab",           {"Voxelab Aquila X2"}},
     {"Vzbot",             {"Vzbot 235 AWD", "Vzbot 330 AWD"}},
     {"Wanhao",            {"Wanhao D12-300"}},
     {"Z-Bolt",            {"Z-Bolt S300",      "Z-Bolt S300 Dual", "Z-Bolt S400",      "Z-Bolt S400 Dual", "Z-Bolt S600",
                            "Z-Bolt S600 Dual"}}};

static std::vector<std::string>               nozzle_diameter_vec = {"0.4", "0.15", "0.2", "0.25", "0.3", "0.35", "0.5", "0.6", "0.75", "0.8", "1.0", "1.2"};
static std::unordered_map<std::string, float> nozzle_diameter_map = {{"0.15", 0.15}, {"0.2", 0.2},   {"0.25", 0.25}, {"0.3", 0.3},
                                                                     {"0.35", 0.35}, {"0.4", 0.4},   {"0.5", 0.5},   {"0.6", 0.6},
                                                                     {"0.75", 0.75}, {"0.8", 0.8},   {"1.0", 1.0},   {"1.2", 1.2}};

static std::set<int> cannot_input_key = {9, 10, 13, 33, 35, 36, 37, 38, 40, 41, 42, 44, 46, 47, 59, 60, 62, 63, 64, 92, 94, 95, 124, 126};

static std::set<char> special_key = {'\n', '\t', '\r', '\v', '@', ';'};

static std::string remove_special_key(const std::string &str)
{
    std::string res_str;
    for (char c : str) {
        if (special_key.find(c) == special_key.end()) {
            res_str.push_back(c);
        }
    }
    return res_str;
}

static bool str_is_all_digit(const std::string &str) {
    for (const char &c : str) {
        if (!std::isdigit(c)) return false;
    }
    return true;
}

// Custom comparator for case-insensitive sorting
static bool caseInsensitiveCompare(const std::string& a, const std::string& b) {
    std::string lowerA = a;
    std::string lowerB = b;
    std::transform(lowerA.begin(), lowerA.end(), lowerA.begin(), ::tolower);
    std::transform(lowerB.begin(), lowerB.end(), lowerB.begin(), ::tolower);
    return lowerA < lowerB;
}

static float my_stof(std::string str) {

    const char dec_sep     = is_decimal_separator_point() ? '.' : ',';
    const char dec_sep_alt = dec_sep == '.' ? ',' : '.';

    size_t alt_pos = str.find(dec_sep_alt);
    if (alt_pos != std::string::npos) { str.replace(alt_pos, 1, 1, dec_sep); }

    if (str == std::string(1, dec_sep)) { return 0.0f; }

    try {
        return static_cast<float>(std::stod(str));
    } catch (...) {
        return 0.f;
    }
}

static bool delete_filament_preset_by_name(std::string delete_preset_name, std::string &selected_preset_name)
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format("select preset, name %1%") % delete_preset_name;
    if (delete_preset_name.empty()) return false;

    // Find an alternate preset to be selected after the current preset is deleted.
    PresetCollection &m_presets = wxGetApp().preset_bundle->filaments;
    if (delete_preset_name == selected_preset_name) {
        const std::deque<Preset> &presets     = m_presets.get_presets();
        size_t                    idx_current = m_presets.get_idx_selected();

        // Find the visible preset.
        size_t idx_new = idx_current;
        if (idx_current > presets.size()) idx_current = presets.size();
        if (idx_current < 0) idx_current = 0;
        if (idx_new < presets.size())
            for (; idx_new < presets.size() && (presets[idx_new].name == delete_preset_name || !presets[idx_new].is_visible); ++idx_new)
                ;
        if (idx_new == presets.size())
            for (idx_new = idx_current - 1; idx_new > 0 && (presets[idx_new].name == delete_preset_name || !presets[idx_new].is_visible); --idx_new)
                ;
        selected_preset_name = presets[idx_new].name;
        BOOST_LOG_TRIVIAL(info) << boost::format("cause by delete current ,choose the next visible, idx %1%, name %2%") % idx_new % selected_preset_name;
    }

    try {
        // BBS delete preset
        Preset *need_delete_preset = m_presets.find_preset(delete_preset_name);
        if (!need_delete_preset) BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" can't find delete preset and name: %1%") % delete_preset_name;
        if (!need_delete_preset->setting_id.empty()) {
            BOOST_LOG_TRIVIAL(info) << "delete preset = " << need_delete_preset->name << ", setting_id = " << need_delete_preset->setting_id;
            m_presets.set_sync_info_and_save(need_delete_preset->name, need_delete_preset->setting_id, "delete", 0);
            wxGetApp().delete_preset_from_cloud(need_delete_preset->setting_id);
        } else {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" can't preset setting id is empty and name: %1%") % delete_preset_name;
        }
        if (m_presets.get_edited_preset().name == delete_preset_name) {
            m_presets.discard_current_changes();
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format("delete preset dirty and cancelled");
        }
        m_presets.delete_preset(need_delete_preset->name);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " preset has been delete from filaments, and preset name is: " << delete_preset_name;
    } catch (const std::exception &ex) {
        // FIXME add some error reporting!
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format("found exception when delete: %1% and preset name: %%") % ex.what() % delete_preset_name;
        return false;
    }

    return true;
}

static std::string get_curr_time(const char* format = "%Y_%m_%d_%H_%M_%S")
{
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();

    std::time_t time = std::chrono::system_clock::to_time_t(now);

    std::tm            local_time = *std::localtime(&time);
    std::ostringstream time_stream;
    time_stream << std::put_time(&local_time, format);

    std::string current_time = time_stream.str();
    return current_time;
}

static std::string get_curr_timestmp()
{
    return get_curr_time("%Y%m%d%H%M%S");
    // std::time_t currentTime = std::time(nullptr);
    // std::ostringstream oss;
    // oss << currentTime;
    // std::string timestampString = oss.str();
    // return timestampString;
}

static void get_filament_compatible_printer(Preset* preset, vector<std::string>& printers)
{
    auto compatible_printers = dynamic_cast<ConfigOptionStrings *>(preset->config.option("compatible_printers"));
    if (compatible_printers == nullptr) return;
    for (const std::string &printer_name : compatible_printers->values) {
        printers.push_back(printer_name);
    }
}

static wxBoxSizer* create_checkbox(wxWindow* parent, Preset* preset, wxString& preset_name, std::vector<std::pair<::CheckBox*, Preset*>>& preset_checkbox)
{
    wxBoxSizer *sizer    = new wxBoxSizer(wxHORIZONTAL);
    ::CheckBox *  checkbox = new ::CheckBox(parent);
    sizer->Add(checkbox, 0, 0, 0);
    preset_checkbox.push_back(std::make_pair(checkbox, preset));
    wxStaticText *preset_name_str = new wxStaticText(parent, wxID_ANY, preset_name);
    wxToolTip *   toolTip         = new wxToolTip(preset_name);
    preset_name_str->SetToolTip(toolTip);
    sizer->Add(preset_name_str, 0, wxLEFT, 5);
    return sizer;
}

static wxBoxSizer *create_checkbox(wxWindow *parent, std::string &compatible_printer, Preset* preset, std::unordered_map<::CheckBox *, std::pair<std::string, Preset *>> &ptinter_compatible_filament_preset)
{
    wxBoxSizer *sizer    = new wxBoxSizer(wxHORIZONTAL);
    ::CheckBox *checkbox = new ::CheckBox(parent);
    sizer->Add(checkbox, 0, 0, 0);
    ptinter_compatible_filament_preset[checkbox] = std::make_pair(compatible_printer, preset);
    wxStaticText *preset_name_str = new wxStaticText(parent, wxID_ANY, wxString::FromUTF8(compatible_printer));
    sizer->Add(preset_name_str, 0, wxLEFT, 5);
    return sizer;
}

static wxBoxSizer *create_checkbox(wxWindow *parent, wxString &preset_name, std::vector<std::pair<::CheckBox *, std::string>> &preset_checkbox)
{
    wxBoxSizer *sizer    = new wxBoxSizer(wxHORIZONTAL);
    ::CheckBox *checkbox = new ::CheckBox(parent);
    sizer->Add(checkbox, 0, 0, 0);
    preset_checkbox.push_back(std::make_pair(checkbox, into_u8(preset_name)));
    wxStaticText *preset_name_str = new wxStaticText(parent, wxID_ANY, preset_name);
    sizer->Add(preset_name_str, 0, wxLEFT, 5);
    return sizer;
}

static wxArrayString get_exist_vendor_choices(VendorMap& vendors)
{
    wxArrayString choices;
    PresetBundle  temp_preset_bundle;
    temp_preset_bundle.load_system_models_from_json(ForwardCompatibilitySubstitutionRule::EnableSystemSilent);
    PresetBundle *preset_bundle = wxGetApp().preset_bundle;

    VendorProfile users_models  = preset_bundle->get_custom_vendor_models();

    vendors = temp_preset_bundle.vendors;

    if (!users_models.models.empty()) {
        vendors[users_models.name] = users_models;
    }

    for (const auto& vendor : vendors) {
        if (vendor.second.models.empty() || vendor.second.id.empty()) continue;
        choices.Add(vendor.first);
    }
    return choices;
}

static std::string get_machine_name(const std::string &preset_name)
{
    size_t index_at = preset_name.find_last_of("@");
    if (std::string::npos == index_at) {
        return "";
    } else {
        return preset_name.substr(index_at + 1);
    }
}

static std::string get_filament_name(std::string &preset_name)
{
    size_t index_at = preset_name.find_last_of("@");
    if (std::string::npos == index_at) {
        return preset_name;
    } else {
        return preset_name.substr(0, index_at - 1);
    }
}

static wxBoxSizer *create_preset_tree(wxWindow *parent, std::pair<std::string, std::vector<std::shared_ptr<Preset>>> printer_and_preset)
{
    wxTreeCtrl *treeCtrl = new wxTreeCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTR_DEFAULT_STYLE | wxNO_BORDER);
    wxColour    backgroundColor = parent->GetBackgroundColour();
    treeCtrl->SetBackgroundColour(backgroundColor);

    wxString     printer_name = wxString::FromUTF8(printer_and_preset.first);
    wxTreeItemId rootId       = treeCtrl->AddRoot(printer_name);
    int          row          = 1;
    for (std::shared_ptr<Preset> preset : printer_and_preset.second) {
        wxString     preset_name = wxString::FromUTF8(preset->name);
        wxTreeItemId childId1    = treeCtrl->AppendItem(rootId, preset_name);
        row++;
    }

    treeCtrl->Expand(rootId);
    wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
    treeCtrl->SetMinSize(wxSize(-1, row * 22));
    treeCtrl->SetMaxSize(wxSize(-1, row * 22));
    sizer->Add(treeCtrl, 0, wxEXPAND | wxALL, 0);

    return sizer;
}

static std::string get_vendor_name(std::string& preset_name)
{
    if (preset_name.empty()) return "";
    std::string vendor_name = preset_name.substr(preset_name.find_first_not_of(' ')); //remove the name prefix space
    size_t index_at = vendor_name.find(" ");
    if (std::string::npos == index_at) {
        return vendor_name;
    } else {
        vendor_name = vendor_name.substr(0, index_at);
        return vendor_name;
    }
}

static wxBoxSizer *create_select_filament_preset_checkbox(wxWindow *                                    parent,
                                                          std::string &                                 compatible_printer,
                                                          std::vector<Preset *>                         presets,
                                                          std::unordered_map<::CheckBox *, std::pair<std::string, Preset *>> &machine_filament_preset)
{
    wxBoxSizer *horizontal_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer *checkbox_sizer   = new wxBoxSizer(wxVERTICAL);
    ::CheckBox *checkbox         = new ::CheckBox(parent);
    checkbox_sizer->Add(checkbox, 0, wxEXPAND | wxRIGHT, 5);

    wxBoxSizer *combobox_sizer = new wxBoxSizer(wxVERTICAL);
    wxStaticText *machine_name_str = new wxStaticText(parent, wxID_ANY, wxString::FromUTF8(compatible_printer));
    ComboBox *    combobox        = new ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(200, 24), 0, nullptr, wxCB_READONLY);
    combobox->SetBackgroundColor(PRINTER_LIST_COLOUR);
    combobox->SetBorderColor(*wxWHITE);
    combobox->SetLabel(_L("Select filament preset"));
    combobox->Bind(wxEVT_COMBOBOX, [combobox, checkbox, presets, &machine_filament_preset, compatible_printer](wxCommandEvent &e) {
        combobox->SetLabelColor(*wxBLACK);
        wxString preset_name = combobox->GetStringSelection();
        checkbox->SetValue(true);
        for (Preset *preset : presets) {
            if (preset_name == wxString::FromUTF8(preset->name)) {
                machine_filament_preset[checkbox] = std::make_pair(compatible_printer, preset);
            }
        }
        e.Skip();
    });
    combobox_sizer->Add(machine_name_str, 0, wxEXPAND, 0);
    combobox_sizer->Add(combobox, 0, wxEXPAND | wxTOP, 5);

    wxArrayString choices;
    for (Preset *preset : presets) {
        choices.Add(wxString::FromUTF8(preset->name));
    }
    combobox->Set(choices);

    horizontal_sizer->Add(checkbox_sizer);
    horizontal_sizer->Add(combobox_sizer);
    return horizontal_sizer;
}

static wxString get_curr_radio_type(std::vector<std::pair<RadioBox *, wxString>> &radio_btns)
{
    for (std::pair<RadioBox *, wxString> radio_string : radio_btns) {
        if (radio_string.first->GetValue()) {
            return radio_string.second;
        }
    }
    return "";
}

static std::string calculate_md5(const std::string &input)
{
    unsigned char digest[MD5_DIGEST_LENGTH];
    std::string   md5;

    EVP_MD_CTX *mdContext = EVP_MD_CTX_new();
    EVP_DigestInit(mdContext, EVP_md5());
    EVP_DigestUpdate(mdContext, input.c_str(), input.length());
    EVP_DigestFinal(mdContext, digest, nullptr);
    EVP_MD_CTX_free(mdContext);

    char hexDigest[MD5_DIGEST_LENGTH * 2 + 1];
    for (int i = 0; i < MD5_DIGEST_LENGTH; ++i) { sprintf(hexDigest + (i * 2), "%02x", digest[i]); }
    hexDigest[MD5_DIGEST_LENGTH * 2] = '\0';

    md5 = std::string(hexDigest);
    return md5;
}

static std::string get_filament_id(std::string vendor_typr_serial)
{
    std::unordered_map<std::string, std::set<std::string>> filament_id_to_filament_name;

    // temp filament presets
    PresetBundle temp_preset_bundle;
    temp_preset_bundle.load_system_filaments_json(Slic3r::ForwardCompatibilitySubstitutionRule::EnableSilent);
    std::string dir_user_presets = wxGetApp().app_config->get("preset_folder");
    if (dir_user_presets.empty()) {
        temp_preset_bundle.load_user_presets(DEFAULT_USER_FOLDER_NAME, ForwardCompatibilitySubstitutionRule::EnableSilent);
    } else {
        temp_preset_bundle.load_user_presets(dir_user_presets, ForwardCompatibilitySubstitutionRule::EnableSilent);
    }
    const std::deque<Preset> &filament_presets = temp_preset_bundle.filaments.get_presets();

    for (const Preset &preset : filament_presets) {
        std::string preset_name = preset.name;
        size_t      index_at    = preset_name.find_first_of('@');
        if (index_at == std::string::npos) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " filament preset name has no @ and name is: " << preset_name;
            continue;
        }
        std::string filament_name = preset_name.substr(0, index_at - 1);
        if (filament_name == vendor_typr_serial && preset.filament_id != "null")
            return preset.filament_id;
        filament_id_to_filament_name[preset.filament_id].insert(filament_name);
    }
    // global filament presets
    PresetBundle *                                     preset_bundle               = wxGetApp().preset_bundle;
    std::map<std::string, std::vector<Preset const *>> temp_filament_id_to_presets = preset_bundle->filaments.get_filament_presets();
    for (std::pair<std::string, std::vector<Preset const *>> filament_id_to_presets : temp_filament_id_to_presets) {
        if (filament_id_to_presets.first.empty()) continue;
        for (const Preset *preset : filament_id_to_presets.second) {
            std::string preset_name = preset->name;
            size_t      index_at    = preset_name.find_first_of('@');
            if (index_at == std::string::npos) {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " filament preset name has no @ and name is: " << preset_name;
                continue;
            }
            std::string filament_name = preset_name.substr(0, index_at - 1);
            if (filament_name == vendor_typr_serial && preset->filament_id != "null")
                return preset->filament_id;
            filament_id_to_filament_name[preset->filament_id].insert(filament_name);
        }
    }

    std::string user_filament_id = "P" + calculate_md5(vendor_typr_serial).substr(0, 7);

    while (filament_id_to_filament_name.find(user_filament_id) != filament_id_to_filament_name.end()) {//find same filament id
        bool have_same_filament_name = false;
        for (const std::string &name : filament_id_to_filament_name.find(user_filament_id)->second) {
            if (name == vendor_typr_serial) {
                have_same_filament_name = true;
                break;
            }
        }
        if (have_same_filament_name) {
            break;
        }
        else { //Different names correspond to the same filament id
            user_filament_id = "P" + calculate_md5(vendor_typr_serial + get_curr_time()).substr(0, 7);
        }
    }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " filament name is: " << vendor_typr_serial << "and create filament_id is: " << user_filament_id;
    return user_filament_id;
}

static json get_config_json(const Preset* preset) {
    json j;
    // record the headers
    j[BBL_JSON_KEY_VERSION] = preset->version.to_string();
    j[BBL_JSON_KEY_NAME]    = preset->name;
    j[BBL_JSON_KEY_FROM]    = "";

   DynamicPrintConfig config = preset->config;

    // record all the key-values
    for (const std::string &opt_key : config.keys()) {
        const ConfigOption *opt = config.option(opt_key);
        if (opt->is_scalar()) {
            if (opt->type() == coString)
                // keep \n, \r, \t
                j[opt_key] = (dynamic_cast<const ConfigOptionString *>(opt))->value;
            else
                j[opt_key] = opt->serialize();
        } else {
            const ConfigOptionVectorBase *vec = static_cast<const ConfigOptionVectorBase *>(opt);
            std::vector<std::string> string_values = vec->vserialize();

            json j_array(string_values);
            j[opt_key] = j_array;
        }
    }

    return j;
}

static char* read_json_file(const std::string &preset_path)
{
    FILE *json_file = boost::nowide::fopen(boost::filesystem::path(preset_path).make_preferred().string().c_str(), "rb");
    if (json_file == NULL) {
        BOOST_LOG_TRIVIAL(info) << "Failed to open JSON file: " << preset_path;
        return NULL;
    }
    fseek(json_file, 0, SEEK_END);     // seek to end
    long file_size = ftell(json_file); // get file size
    fseek(json_file, 0, SEEK_SET);     // seek to start

    char * json_contents = (char *) malloc(file_size);
    if (json_contents == NULL) {
        BOOST_LOG_TRIVIAL(info) << "Failed to allocate memory for JSON file ";
        fclose(json_file);
        return NULL;
    }

    fread(json_contents, 1, file_size, json_file);
    fclose(json_file);

    return json_contents;
}

static std::string get_printer_nozzle_diameter(std::string printer_name) {
    // Create a lowercase version of the printer_name for case-insensitive search
    std::string printer_name_lower = printer_name;
    std::transform(printer_name_lower.begin(), printer_name_lower.end(), printer_name_lower.begin(), ::tolower);

    size_t index = printer_name_lower.find(" nozzle)");
    if (std::string::npos == index) {
        size_t index = printer_name_lower.find(" nozzle");
        if (std::string::npos == index) {
            return "";
        }
        std::string nozzle = printer_name_lower.substr(0, index);
        size_t      last_space_index = nozzle.find_last_of(" ");
        if (std::string::npos == index) {
            return "";
        }
        return nozzle.substr(last_space_index + 1);
    } else {
        std::string nozzle = printer_name_lower.substr(0, index);
        size_t      last_bracket_index = nozzle.find_last_of("(");
        if (std::string::npos == index) {
            return "";
        }
        return nozzle.substr(last_bracket_index + 1);
    }
}

static void adjust_dialog_in_screen(DPIDialog* dialog) {
    wxSize screen_size = wxGetDisplaySize();
    int    pos_x, pos_y, size_x, size_y, screen_width, screen_height, dialog_x, dialog_y;
    pos_x         = dialog->GetPosition().x;
    pos_y         = dialog->GetPosition().y;
    size_x        = dialog->GetSize().x;
    size_y        = dialog->GetSize().y;
    screen_width  = screen_size.GetWidth();
    screen_height = screen_size.GetHeight();
    dialog_x      = pos_x;
    dialog_y      = pos_y;
    if (pos_x + size_x > screen_width) {
        int exceed_x = pos_x + size_x - screen_width;
        dialog_x -= exceed_x;
    }
    if (pos_y + size_y > screen_height - 50) {
        int exceed_y = pos_y + size_y - screen_height + 50;
        dialog_y -= exceed_y;
    }
    if (pos_x != dialog_x || pos_y != dialog_y) { dialog->SetPosition(wxPoint(dialog_x, dialog_y)); }
}

CreateFilamentPresetDialog::CreateFilamentPresetDialog(wxWindow *parent)
	: DPIDialog(parent ? parent : nullptr, wxID_ANY, _L("Create Filament"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX | wxCENTRE | wxRESIZE_BORDER)
{
    m_create_type.base_filament = _L("Create Based on Current Filament");
    m_create_type.base_filament_preset = _L("Copy Current Filament Preset ");
    get_all_filament_presets();

	this->SetBackgroundColour(*wxWHITE);
    this->SetSize(wxSize(FromDIP(600), FromDIP(480)));

	wxBoxSizer *m_main_sizer = new wxBoxSizer(wxVERTICAL);
    // top line
    auto m_line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    m_line_top->SetBackgroundColour(wxColour(0xA6, 0xa9, 0xAA));
    m_main_sizer->Add(m_line_top, 0, wxEXPAND, 0);
    m_main_sizer->Add(0, 0, 0, wxTOP, FromDIP(5));

    wxStaticText *basic_information = new wxStaticText(this, wxID_ANY, _L("Basic Information"));
    basic_information->SetFont(Label::Head_16);
    m_main_sizer->Add(basic_information, 0, wxLEFT, FromDIP(10));

    m_main_sizer->Add(create_item(FilamentOptionType::VENDOR), 0, wxEXPAND | wxALL, FromDIP(5));
    m_main_sizer->Add(create_item(FilamentOptionType::TYPE), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(5));
    m_main_sizer->Add(create_item(FilamentOptionType::SERIAL), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(5));

    // divider line
    auto line_divider = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    line_divider->SetBackgroundColour(wxColour(0xA6, 0xa9, 0xAA));
    m_main_sizer->Add(line_divider, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));
    m_main_sizer->Add(0, 0, 0, wxTOP, FromDIP(5));

    wxStaticText *presets_information = new wxStaticText(this, wxID_ANY, _L("Add Filament Preset under this filament"));
    presets_information->SetFont(Label::Head_16);
    m_main_sizer->Add(presets_information, 0, wxLEFT | wxRIGHT, FromDIP(15));

    m_main_sizer->Add(create_item(FilamentOptionType::FILAMENT_PRESET), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(5));

    m_filament_preset_text = new wxStaticText(this, wxID_ANY, _L("We could create the filament presets for your following printer:"), wxDefaultPosition, wxDefaultSize);
    m_main_sizer->Add(m_filament_preset_text, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(15));

    m_scrolled_preset_panel = new wxScrolledWindow(this, wxID_ANY);
    m_scrolled_preset_panel->SetMaxSize(wxSize(-1, FromDIP(350)));
    m_scrolled_preset_panel->SetBackgroundColour(*wxWHITE);
    m_scrolled_preset_panel->SetScrollRate(5, 5);
    m_scrolled_sizer = new wxBoxSizer(wxVERTICAL);
    m_scrolled_sizer->Add(create_item(FilamentOptionType::PRESET_FOR_PRINTER), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(5));
    m_scrolled_sizer->Add(0, 0, 0, wxTOP, FromDIP(5));
    m_scrolled_preset_panel->SetSizerAndFit(m_scrolled_sizer);
    m_main_sizer->Add(m_scrolled_preset_panel, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));
    m_main_sizer->Add(create_dialog_buttons(), 0, wxEXPAND);

    get_all_visible_printer_name();
    select_curr_radiobox(m_create_type_btns, 0);

    this->SetSizer(m_main_sizer);

    Layout();
    Fit();

    this->Bind(wxEVT_SIZE, [this](wxSizeEvent &event) {
        this->Refresh();
        event.Skip();
    });

	wxGetApp().UpdateDlgDarkUI(this);
}

CreateFilamentPresetDialog::~CreateFilamentPresetDialog()
{
    for (std::pair<std::string, Preset *> preset : m_all_presets_map) {
        Preset *p = preset.second;
        if (p) {
            delete p;
            p = nullptr;
        }
    }
}

void CreateFilamentPresetDialog::on_dpi_changed(const wxRect &suggested_rect) {
    Layout();
}

bool CreateFilamentPresetDialog::is_check_box_selected()
{
    for (const auto& checkbox_preset : m_filament_preset) {
        if (checkbox_preset.first->GetValue()) { return true; }
    }

    for (const auto& checkbox_preset : m_machint_filament_preset) {
        if (checkbox_preset.first->GetValue()) { return true; }
    }

    return false;
}

wxBoxSizer *CreateFilamentPresetDialog::create_item(FilamentOptionType option_type)
{

    wxSizer *item = nullptr;
    switch (option_type) {
        case VENDOR:             return create_vendor_item();
        case TYPE:               return create_type_item();
        case SERIAL:             return create_serial_item();
        case FILAMENT_PRESET:    return create_filament_preset_item();
        case PRESET_FOR_PRINTER: return create_filament_preset_for_printer_item();
        default:                 return nullptr;
    }
}
wxBoxSizer *CreateFilamentPresetDialog::create_vendor_item()
{
    wxBoxSizer *horizontal_sizer = new wxBoxSizer(wxHORIZONTAL);

    wxBoxSizer *  optionSizer = new wxBoxSizer(wxVERTICAL);
    wxStaticText *static_vendor_text = new wxStaticText(this, wxID_ANY, _L("Vendor"), wxDefaultPosition, wxDefaultSize);
    optionSizer->Add(static_vendor_text, 0, wxEXPAND | wxALL, 0);
    optionSizer->SetMinSize(OPTION_SIZE);
    horizontal_sizer->Add(optionSizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));

    // Convert all std::any to std::string
    std::vector<std::string> string_vendors;
    for (const auto& vendor_any : filament_vendors) {
        string_vendors.push_back(std::any_cast<std::string>(vendor_any));
    }

    // Sort the vendors alphabetically
    std::sort(string_vendors.begin(), string_vendors.end(), caseInsensitiveCompare);

    wxArrayString choices;
    for (const std::string &vendor : string_vendors) {
        choices.push_back(wxString(vendor)); // Convert std::string to wxString before adding
    }

    wxBoxSizer *vendor_sizer   = new wxBoxSizer(wxHORIZONTAL);
    m_filament_vendor_combobox = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, NAME_OPTION_COMBOBOX_SIZE, 0, nullptr, wxCB_READONLY);
    m_filament_vendor_combobox->SetLabel(_L("Select Vendor"));
    m_filament_vendor_combobox->SetLabelColor(DEFAULT_PROMPT_TEXT_COLOUR);
    m_filament_vendor_combobox->Set(choices);
    m_filament_vendor_combobox->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &e) {
        m_filament_vendor_combobox->SetLabelColor(*wxBLACK);
        e.Skip();
    });
    vendor_sizer->Add(m_filament_vendor_combobox, 0, wxEXPAND | wxALL, 0);
    wxBoxSizer *textInputSizer = new wxBoxSizer(wxVERTICAL);
    m_filament_custom_vendor_input = new TextInput(this, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, NAME_OPTION_COMBOBOX_SIZE, wxTE_PROCESS_ENTER);
    m_filament_custom_vendor_input->GetTextCtrl()->SetMaxLength(50);
    m_filament_custom_vendor_input->SetSize(NAME_OPTION_COMBOBOX_SIZE);
    textInputSizer->Add(m_filament_custom_vendor_input, 0, wxEXPAND | wxALL, 0);
    m_filament_custom_vendor_input->GetTextCtrl()->SetHint(_L("Input Custom Vendor"));
    m_filament_custom_vendor_input->GetTextCtrl()->Bind(wxEVT_CHAR, [](wxKeyEvent &event) {
        int key = event.GetKeyCode();
        if (cannot_input_key.find(key) != cannot_input_key.end()) {
            event.Skip(false);
            return;
        }
        event.Skip();
    });
    m_filament_custom_vendor_input->Hide();
    vendor_sizer->Add(textInputSizer, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    wxBoxSizer *comboBoxSizer      = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer *checkbox_sizer     = new wxBoxSizer(wxHORIZONTAL);
    m_can_not_find_vendor_checkbox = new ::CheckBox(this);

    checkbox_sizer->Add(m_can_not_find_vendor_checkbox, 0, wxALIGN_CENTER, 0);
    checkbox_sizer->Add(0, 0, 0, wxEXPAND | wxRIGHT, FromDIP(5));

    wxStaticText *m_can_not_find_vendor_text = new wxStaticText(this, wxID_ANY, _L("Can't find vendor I want"), wxDefaultPosition, wxDefaultSize, 0);
    m_can_not_find_vendor_text->SetFont(::Label::Body_13);

    wxSize size = m_can_not_find_vendor_text->GetTextExtent(_L("Can't find vendor I want"));
    m_can_not_find_vendor_text->SetMinSize(wxSize(size.x + FromDIP(4), -1));
    m_can_not_find_vendor_text->Wrap(-1);
    checkbox_sizer->Add(m_can_not_find_vendor_text, 0, wxALIGN_CENTER, 0);

    m_can_not_find_vendor_checkbox->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &e) {
        bool value = m_can_not_find_vendor_checkbox->GetValue();
        if (value) {
            m_can_not_find_vendor_checkbox->SetValue(true);
            m_filament_vendor_combobox->Hide();
            m_filament_custom_vendor_input->Show();
        } else {
            m_can_not_find_vendor_checkbox->SetValue(false);
            m_filament_vendor_combobox->Show();
            m_filament_custom_vendor_input->Hide();
        }
        Refresh();
        Layout();
        Fit();

        e.Skip();
    });

    comboBoxSizer->Add(vendor_sizer, 0, wxEXPAND | wxTOP, FromDIP(5));
    comboBoxSizer->Add(checkbox_sizer, 0, wxEXPAND | wxTOP, FromDIP(5));
    horizontal_sizer->Add(comboBoxSizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));

    return horizontal_sizer;

}

wxBoxSizer *CreateFilamentPresetDialog::create_type_item()
{
    wxBoxSizer *horizontal_sizer = new wxBoxSizer(wxHORIZONTAL);

    wxBoxSizer *  optionSizer        = new wxBoxSizer(wxVERTICAL);
    wxStaticText *static_type_text = new wxStaticText(this, wxID_ANY, _L("Type"), wxDefaultPosition, wxDefaultSize);
    optionSizer->Add(static_type_text, 0, wxEXPAND | wxALL, 0);
    optionSizer->SetMinSize(OPTION_SIZE);
    horizontal_sizer->Add(optionSizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));

    wxArrayString filament_type;
    for (const wxString filament : m_system_filament_types_set) {
        filament_type.Add(filament);
    }
    filament_type.Sort();

    wxBoxSizer *comboBoxSizer = new wxBoxSizer(wxVERTICAL);
    m_filament_type_combobox  = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, NAME_OPTION_COMBOBOX_SIZE, 0, nullptr, wxCB_READONLY);
    m_filament_type_combobox->SetLabel(_L("Select Type"));
    m_filament_type_combobox->SetLabelColor(DEFAULT_PROMPT_TEXT_COLOUR);
    m_filament_type_combobox->Set(filament_type);
    comboBoxSizer->Add(m_filament_type_combobox, 0, wxEXPAND | wxALL, 0);
    horizontal_sizer->Add(comboBoxSizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));

    m_filament_type_combobox->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &e) {
        m_filament_type_combobox->SetLabelColor(*wxBLACK);
        const wxString &curr_create_type = curr_create_filament_type();
        clear_filament_preset_map();
        if (curr_create_type == m_create_type.base_filament) {
            wxArrayString filament_preset_choice = get_filament_preset_choices();
            m_filament_preset_combobox->Set(filament_preset_choice);
            m_filament_preset_combobox->SetLabel(_L("Select Filament Preset"));
            m_filament_preset_combobox->SetLabelColor(DEFAULT_PROMPT_TEXT_COLOUR);

        } else if (curr_create_type == m_create_type.base_filament_preset) {
            get_filament_presets_by_machine();
        }
        m_scrolled_preset_panel->SetSizerAndFit(m_scrolled_sizer);

        update_dialog_size();
        e.Skip();
    });

    return horizontal_sizer;
}

wxBoxSizer *CreateFilamentPresetDialog::create_serial_item()
{
    wxBoxSizer *horizontal_sizer = new wxBoxSizer(wxHORIZONTAL);

    wxBoxSizer *  optionSizer        = new wxBoxSizer(wxVERTICAL);
    wxStaticText *static_serial_text = new wxStaticText(this, wxID_ANY, _L("Serial"), wxDefaultPosition, wxDefaultSize);
    optionSizer->Add(static_serial_text, 0, wxEXPAND | wxALL, 0);
    optionSizer->SetMinSize(OPTION_SIZE);
    horizontal_sizer->Add(optionSizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));

    wxBoxSizer *comboBoxSizer = new wxBoxSizer(wxVERTICAL);
    m_filament_serial_input   = new TextInput(this, "", "", "", wxDefaultPosition, NAME_OPTION_COMBOBOX_SIZE, wxTE_PROCESS_ENTER);
    m_filament_serial_input->GetTextCtrl()->SetMaxLength(50);
    comboBoxSizer->Add(m_filament_serial_input, 0, wxEXPAND | wxALL, 0);
    m_filament_serial_input->GetTextCtrl()->Bind(wxEVT_CHAR, [](wxKeyEvent &event) {
        int key = event.GetKeyCode();
        if (cannot_input_key.find(key) != cannot_input_key.end()) {
            event.Skip(false);
            return;
        }
        event.Skip();
        });

    wxStaticText *static_eg_text = new wxStaticText(this, wxID_ANY, _L("e.g. Basic, Matte, Silk, Marble"), wxDefaultPosition, wxDefaultSize);
    static_eg_text->SetForegroundColour(wxColour("#6B6B6B"));
    static_eg_text->SetFont(::Label::Body_12);
    comboBoxSizer->Add(static_eg_text, 0, wxEXPAND | wxTOP, FromDIP(5));
    horizontal_sizer->Add(comboBoxSizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));

    return horizontal_sizer;
}

wxBoxSizer *CreateFilamentPresetDialog::create_filament_preset_item()
{
    wxBoxSizer *horizontal_sizer = new wxBoxSizer(wxHORIZONTAL);

    wxBoxSizer *  optionSizer        = new wxBoxSizer(wxVERTICAL);
    wxStaticText *static_filament_preset_text = new wxStaticText(this, wxID_ANY, _L("Filament Preset"), wxDefaultPosition, wxDefaultSize);
    optionSizer->Add(static_filament_preset_text, 0, wxEXPAND | wxALL, 0);
    optionSizer->SetMinSize(OPTION_SIZE);
    horizontal_sizer->Add(optionSizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    wxBoxSizer *  comboBoxSizer  = new wxBoxSizer(wxVERTICAL);
    comboBoxSizer->Add(create_radio_item(m_create_type.base_filament, this, wxEmptyString, m_create_type_btns), 0, wxEXPAND | wxALL, 0);

    m_filament_preset_combobox = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, FILAMENT_PRESET_COMBOBOX_SIZE, 0, nullptr, wxCB_READONLY);
    m_filament_preset_combobox->SetLabel(_L("Select Filament Preset"));
    m_filament_preset_combobox->SetLabelColor(DEFAULT_PROMPT_TEXT_COLOUR);


    m_filament_preset_combobox->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &e) {
        m_filament_preset_combobox->SetLabelColor(*wxBLACK);
        wxString filament_type = m_filament_preset_combobox->GetStringSelection();
        std::unordered_map<std::string, std::vector<Preset *>>::iterator iter = m_filament_choice_map.find(m_public_name_to_filament_id_map[filament_type]);

        m_scrolled_preset_panel->Freeze();
        m_filament_presets_sizer->Clear(true);
        m_filament_preset.clear();

        std::vector<std::pair<std::string, Preset *>> printer_name_to_filament_preset;
        if (iter != m_filament_choice_map.end()) {
            std::unordered_map<std::string, float> nozzle_diameter = nozzle_diameter_map;
            for (Preset* preset : iter->second) {
                auto compatible_printers = preset->config.option<ConfigOptionStrings>("compatible_printers", true);
                if (!compatible_printers || compatible_printers->values.empty()) {
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "there is a preset has no compatible printers and the preset name is: " << preset->name;
                    // If no compatible printers are defined, add all visible printers
                    for (const std::string& visible_printer : m_visible_printers) {
                        std::string nozzle = get_printer_nozzle_diameter(visible_printer);
                        if (nozzle_diameter[nozzle] == 0) {
                            BOOST_LOG_TRIVIAL(info)
                                << __FUNCTION__ << " compatible printer nozzle encounter exception and name is: " << visible_printer;
                            continue;
                        }
                        // Add to the list of available printer-preset pairs
                        printer_name_to_filament_preset.push_back(std::make_pair(visible_printer, preset));
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "show compatible printer name: " << visible_printer
                                                << " and preset name is: " << preset->name;
                    }
                    
                    continue;
                }
                for (std::string &compatible_printer_name : compatible_printers->values) {
                    if (m_visible_printers.find(compatible_printer_name) == m_visible_printers.end()) {
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "there is a comppatible printer no exist: " << compatible_printer_name
                                                << "and the preset name is: " << preset->name;
                        continue;
                    }
                    std::string nozzle = get_printer_nozzle_diameter(compatible_printer_name);
                    if (nozzle_diameter[nozzle] == 0) {
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " compatible printer nozzle encounter exception and name is: " << compatible_printer_name;
                        continue;
                    }
                    printer_name_to_filament_preset.push_back(std::make_pair(compatible_printer_name,preset));
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "show compatible printer name: " << compatible_printer_name << "and preset name is: " << preset;
                }
            }
        } else {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " not find filament_id corresponding to the type: and the type is" << filament_type;
        }
        sort_printer_by_nozzle(printer_name_to_filament_preset);
        for (std::pair<std::string, Preset *> printer_to_preset : printer_name_to_filament_preset)
            m_filament_presets_sizer->Add(create_checkbox(m_filament_preset_panel, printer_to_preset.first, printer_to_preset.second, m_filament_preset), 0,
                                          wxEXPAND | wxTOP | wxLEFT, FromDIP(5));
        m_scrolled_preset_panel->SetSizerAndFit(m_scrolled_sizer);
        m_scrolled_preset_panel->Thaw();

        update_dialog_size();
        e.Skip();
    });

    comboBoxSizer->Add(m_filament_preset_combobox, 0, wxEXPAND | wxTOP, FromDIP(5));

    comboBoxSizer->Add(create_radio_item(m_create_type.base_filament_preset, this, wxEmptyString, m_create_type_btns), 0, wxEXPAND | wxTOP, FromDIP(10));

    horizontal_sizer->Add(comboBoxSizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    horizontal_sizer->Add(0, 0, 0, wxLEFT, FromDIP(30));

    return horizontal_sizer;

}

wxBoxSizer *CreateFilamentPresetDialog::create_filament_preset_for_printer_item()
{
    wxBoxSizer *vertical_sizer = new wxBoxSizer(wxVERTICAL);
    m_filament_preset_panel = new wxPanel(m_scrolled_preset_panel, wxID_ANY);
    m_filament_preset_panel->SetBackgroundColour(PRINTER_LIST_COLOUR);
    m_filament_preset_panel->SetSize(PRINTER_LIST_SIZE);
    m_filament_presets_sizer = new wxGridSizer(3, FromDIP(5), FromDIP(5));
    m_filament_preset_panel->SetSizer(m_filament_presets_sizer);
    vertical_sizer->Add(m_filament_preset_panel, 0, wxEXPAND | wxTOP | wxALIGN_CENTER_HORIZONTAL, FromDIP(5));

    return vertical_sizer;
}

wxWindow *CreateFilamentPresetDialog::create_dialog_buttons()
{
    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});

    auto btn_ok = dlg_btns->GetOK();
    btn_ok->SetLabel(_L("Create"));
    btn_ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        //get vendor name
        wxString vendor_str = m_filament_vendor_combobox->GetLabel();
        std::string vendor_name;

        if (!m_can_not_find_vendor_checkbox->GetValue()) {
            if (_L("Select Vendor") == vendor_str) {
                MessageDialog dlg(this, _L("Vendor is not selected, please reselect vendor."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"),
                                  wxYES | wxYES_DEFAULT | wxCENTRE);
                dlg.ShowModal();
                return;
            } else {
                vendor_name = into_u8(vendor_str);
            }
        } else {
            if (m_filament_custom_vendor_input->GetTextCtrl()->GetValue().empty()) {
                MessageDialog dlg(this, _L("Custom vendor is not input, please input custom vendor."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"),
                                  wxYES | wxYES_DEFAULT | wxCENTRE);
                dlg.ShowModal();
                return;
            } else {
                vendor_name = into_u8(m_filament_custom_vendor_input->GetTextCtrl()->GetValue());
                if (vendor_name == "Bambu" || vendor_name == "Generic") {
                    MessageDialog dlg(this, _L("\"Bambu\" or \"Generic\" cannot be used as a Vendor for custom filaments."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"),
                                      wxYES | wxYES_DEFAULT | wxCENTRE);
                    dlg.ShowModal();
                    return;
                }
            }
        }

        //get fialment type name
        wxString type_str = m_filament_type_combobox->GetLabel();
        std::string type_name;
        if (_L("Select Type") == type_str) {
            MessageDialog dlg(this, _L("Filament type is not selected, please reselect type."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES | wxYES_DEFAULT | wxCENTRE);
            dlg.ShowModal();
            return;
        } else {
            type_name = into_u8(type_str);
        }
        //get filament serial
        wxString    serial_str = m_filament_serial_input->GetTextCtrl()->GetValue();
        std::string serial_name;
        if (serial_str.empty()) {
            MessageDialog dlg(this, _L("Filament serial is not entered, please enter serial."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"),
                              wxYES | wxYES_DEFAULT | wxCENTRE);
            dlg.ShowModal();
            return;
        } else {
            serial_name = into_u8(serial_str);
        }
        vendor_name = remove_special_key(vendor_name);
        serial_name = remove_special_key(serial_name);

        if (vendor_name.empty() || serial_name.empty()) {
            MessageDialog dlg(this, _L("There may be escape characters in the vendor or serial input of filament. Please delete and re-enter."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"),
                              wxYES | wxYES_DEFAULT | wxCENTRE);
            dlg.ShowModal();
            return;
        }
        boost::algorithm::trim(vendor_name);
        boost::algorithm::trim(serial_name);
        if (vendor_name.empty() || serial_name.empty()) {
            MessageDialog dlg(this, _L("All inputs in the custom vendor or serial are spaces. Please re-enter."),
                              wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES | wxYES_DEFAULT | wxCENTRE);
            dlg.ShowModal();
            return;
        }
        if (m_can_not_find_vendor_checkbox->GetValue() && str_is_all_digit(vendor_name)) {
            MessageDialog dlg(this, _L("The vendor cannot be a number. Please re-enter."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"),
                              wxYES | wxYES_DEFAULT | wxCENTRE);
            dlg.ShowModal();
            return;
        }

        if (!is_check_box_selected()) {
            MessageDialog dlg(this, _L("You have not selected a printer or preset yet. Please select at least one."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"),
                              wxYES | wxYES_DEFAULT | wxCENTRE);
            dlg.ShowModal();
            return;
        }

        std::string filament_preset_name = vendor_name + " " + (type_name == "PLA-AERO" ? "PLA Aero" : type_name) + " " + serial_name;
        PresetBundle *preset_bundle        = wxGetApp().preset_bundle;
        if (preset_bundle->filaments.is_alias_exist(filament_preset_name)) {
            MessageDialog dlg(this,
                              wxString::Format(_L("The Filament name %s you created already exists.\n"
                                                  "If you continue creating, the preset created will be displayed with its full name. Do you want to continue?"),
                                               from_u8(filament_preset_name)),
                              wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES_NO | wxYES_DEFAULT | wxCENTRE);
            if (wxID_YES != dlg.ShowModal()) { return; }
        }

        std::string user_filament_id     = get_filament_id(filament_preset_name);

        const wxString &curr_create_type = curr_create_filament_type();

        if (curr_create_type == m_create_type.base_filament) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":clone filament  create type  filament ";
            for (const auto& checkbox_preset : m_filament_preset) {
                if (checkbox_preset.first->GetValue()) {
                    std::string compatible_printer_name = checkbox_preset.second.first;
                    std::vector<std::string> failures;
                    Preset const *const      checked_preset = checkbox_preset.second.second;
                    DynamicConfig            dynamic_config;
                    dynamic_config.set_key_value("filament_vendor", new ConfigOptionStrings({vendor_name}));
                    dynamic_config.set_key_value("compatible_printers", new ConfigOptionStrings({compatible_printer_name}));
                    dynamic_config.set_key_value("filament_type", new ConfigOptionStrings({type_name}));
                    bool res = preset_bundle->filaments.clone_presets_for_filament(checked_preset, failures, filament_preset_name, user_filament_id, dynamic_config,
                                                                                   compatible_printer_name);
                    if (!res) {
                        std::string failure_names;
                        for (std::string &failure : failures) { failure_names += failure + "\n"; }
                        MessageDialog dlg(this, _L("Some existing presets have failed to be created, as follows:\n") + from_u8(failure_names) + _L("\nDo you want to rewrite it?"),
                                          wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES_NO | wxYES_DEFAULT | wxCENTRE);
                        if (dlg.ShowModal() == wxID_YES) {
                            res = preset_bundle->filaments.clone_presets_for_filament(checked_preset, failures, filament_preset_name, user_filament_id, dynamic_config,
                                                                                      compatible_printer_name, true);
                            BOOST_LOG_TRIVIAL(info) << "clone filament  have failures  rewritten  is successful? " << res;
                        }
                    }
                    BOOST_LOG_TRIVIAL(info) << "clone filament  no failures  is successful? " << res;
                }
            }
        } else if (curr_create_type == m_create_type.base_filament_preset) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":clone filament presets  create type  filament preset";
            for (const auto& checkbox_preset : m_machint_filament_preset) {
                if (checkbox_preset.first->GetValue()) {
                    std::string compatible_printer_name = checkbox_preset.second.first;
                    std::vector<std::string> failures;
                    Preset const *const      checked_preset = checkbox_preset.second.second;
                    DynamicConfig            dynamic_config;
                    dynamic_config.set_key_value("filament_vendor", new ConfigOptionStrings({vendor_name}));
                    dynamic_config.set_key_value("compatible_printers", new ConfigOptionStrings({compatible_printer_name}));
                    dynamic_config.set_key_value("filament_type", new ConfigOptionStrings({type_name}));
                    bool res = preset_bundle->filaments.clone_presets_for_filament(checked_preset, failures, filament_preset_name, user_filament_id, dynamic_config,
                                                                                   compatible_printer_name);
                    if (!res) {
                        std::string failure_names;
                        for (std::string &failure : failures) { failure_names += failure + "\n"; }
                        MessageDialog dlg(this, _L("Some existing presets have failed to be created, as follows:\n") + from_u8(failure_names) + _L("\nDo you want to rewrite it?"),
                                          wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES_NO | wxYES_DEFAULT | wxCENTRE);
                        if (wxID_YES == dlg.ShowModal()) {
                            res = preset_bundle->filaments.clone_presets_for_filament(checked_preset, failures, filament_preset_name, user_filament_id, dynamic_config,
                                                                                      compatible_printer_name, true);
                            BOOST_LOG_TRIVIAL(info) << "clone filament presets  have failures  rewritten  is successful? " << res;
                        }
                    }
                    BOOST_LOG_TRIVIAL(info) << "clone filament presets  no failures  is successful? " << res << " old preset is: " << checked_preset->name
                                            << " compatible_printer_name is: " << compatible_printer_name;
                }
            }
        }
        preset_bundle->update_compatible(PresetSelectCompatibleType::Always);
        EndModal(wxID_OK);
        });

    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) { 
        EndModal(wxID_CANCEL);
    });

    return dlg_btns;
}

wxArrayString CreateFilamentPresetDialog::get_filament_preset_choices()
{
    wxArrayString choices;
    // get fialment type name
    wxString    type_str = m_filament_type_combobox->GetLabel();
    std::string type_name;
    if (_L("Select Type") == type_str) {
        /*MessageDialog dlg(this, _L("Filament type is not selected, please reselect type."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES | wxYES_DEFAULT | wxCENTRE);
        dlg.ShowModal();*/
        return choices;
    } else {
        type_name = into_u8(type_str);
    }

    for (std::pair<std::string, Preset*> filament_presets : m_all_presets_map) {
        Preset *preset = filament_presets.second;
        auto    inherit = preset->config.option<ConfigOptionString>("inherits");
        if (inherit && !inherit->value.empty()) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " inherit user preset is:" << preset->name << " and inherits is: " << inherit->value;
            continue;
        }
        auto fila_type = preset->config.option<ConfigOptionStrings>("filament_type");
        if (!fila_type || fila_type->values.empty() || type_name != fila_type->values[0]) continue;
        m_filament_choice_map[preset->filament_id].push_back(preset);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " base user preset is:" << preset->name;
    }

    int suffix = 0;
    for (const auto& preset : m_filament_choice_map) {
        if (preset.second.empty()) continue;
        std::set<wxString> preset_name_set;
        for (Preset* filament_preset : preset.second) {
            std::string preset_name = filament_preset->name;
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " filament_id: " << filament_preset->filament_id << " preset name: " << filament_preset->name;
            size_t      index_at    = preset_name.find(" @");
            std::string cur_preset_name = preset_name;
            if (std::string::npos != index_at) {
                cur_preset_name = preset_name.substr(0, index_at);
            }
            preset_name_set.insert(from_u8(cur_preset_name));
        }
        assert(1 == preset_name_set.size());
        if (preset_name_set.size() > 1) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " the same filament has different filament(vendor type serial)";
        }
        for (const wxString& public_name : preset_name_set) {
            if (m_public_name_to_filament_id_map.find(public_name) != m_public_name_to_filament_id_map.end()) {
                suffix++;
                m_public_name_to_filament_id_map[public_name + "_" + std::to_string(suffix)] = preset.first;
                choices.Add(public_name + "_" + std::to_string(suffix));
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " add filament choice: " << choices.back();
            } else {
                m_public_name_to_filament_id_map[public_name] = preset.first;
                choices.Add(public_name);
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " add filament choice: " << choices.back();
            }
        }
    }

    return choices;
}

wxBoxSizer *CreateFilamentPresetDialog::create_radio_item(wxString title, wxWindow *parent, wxString tooltip, std::vector<std::pair<RadioBox *, wxString>> &radiobox_list)
{
    wxBoxSizer *horizontal_sizer = new wxBoxSizer(wxHORIZONTAL);
    RadioBox *  radiobox         = new RadioBox(parent);
    horizontal_sizer->Add(radiobox, 0, wxEXPAND | wxALL, 0);
    horizontal_sizer->Add(0, 0, 0, wxEXPAND | wxLEFT, FromDIP(5));
    radiobox_list.push_back(std::make_pair(radiobox, title));
    int btn_idx = radiobox_list.size() - 1;
    radiobox->Bind(wxEVT_LEFT_DOWN, [this, &radiobox_list, btn_idx](wxMouseEvent &e) { select_curr_radiobox(radiobox_list, btn_idx); });

    wxStaticText *text = new wxStaticText(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize);
    text->Bind(wxEVT_LEFT_DOWN, [this, &radiobox_list, btn_idx](wxMouseEvent &e) { select_curr_radiobox(radiobox_list, btn_idx); });
    horizontal_sizer->Add(text, 0, wxEXPAND | wxLEFT, 0);

    radiobox->SetToolTip(tooltip);
    text->SetToolTip(tooltip);
    return horizontal_sizer;
}

void CreateFilamentPresetDialog::select_curr_radiobox(std::vector<std::pair<RadioBox *, wxString>> &radiobox_list, int btn_idx)
{
    int len = radiobox_list.size();
    for (int i = 0; i < len; ++i) {
        if (i == btn_idx) {
            radiobox_list[i].first->SetValue(true);
            const wxString &curr_selected_type = radiobox_list[i].second;
            this->Freeze();
            if (curr_selected_type == m_create_type.base_filament) {
                m_filament_preset_text->SetLabel(_L("We could create the filament presets for your following printer:"));
                m_filament_preset_combobox->Show();
                if (_L("Select Type") != m_filament_type_combobox->GetLabel()) {
                    clear_filament_preset_map();
                    wxArrayString filament_preset_choice = get_filament_preset_choices();
                    m_filament_preset_combobox->Set(filament_preset_choice);
                    m_filament_preset_combobox->SetLabel(_L("Select Filament Preset"));
                    m_filament_preset_combobox->SetLabelColor(DEFAULT_PROMPT_TEXT_COLOUR);
                }
            } else if (curr_selected_type == m_create_type.base_filament_preset) {
                m_filament_preset_text->SetLabel(_L("We would rename the presets as \"Vendor Type Serial @printer you selected\".\n"
                                                    "To add preset for more printers, please go to printer selection"));
                m_filament_preset_combobox->Hide();
                if (_L("Select Type") != m_filament_type_combobox->GetLabel()) {

                    clear_filament_preset_map();
                    get_filament_presets_by_machine();

                }
            }
            m_scrolled_preset_panel->SetSizerAndFit(m_scrolled_sizer);
            this->Thaw();
        } else {
            radiobox_list[i].first->SetValue(false);
        }
    }
    update_dialog_size();
}

wxString CreateFilamentPresetDialog::curr_create_filament_type()
{
    wxString curr_filament_type;
    for (const std::pair<RadioBox *, wxString> &printer_radio : m_create_type_btns) {
        if (printer_radio.first->GetValue()) {
            curr_filament_type = printer_radio.second;
        }
    }
    return curr_filament_type;
}

void CreateFilamentPresetDialog::get_filament_presets_by_machine()
{
    wxArrayString choices;
    // get fialment type name
    wxString    type_str = m_filament_type_combobox->GetLabel();
    std::string type_name;
    if (_L("Select Type") == type_str) {
        /*MessageDialog dlg(this, _L("Filament type is not selected, please reselect type."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES | wxYES_DEFAULT |
        wxCENTRE); dlg.ShowModal();*/
        return;
    } else {
        type_name = into_u8(type_str);
    }

    std::unordered_map<std::string, float>                 nozzle_diameter = nozzle_diameter_map;
    std::unordered_map<std::string, std::vector<Preset *>> machine_name_to_presets;
    PresetBundle *                                         preset_bundle = wxGetApp().preset_bundle;
    for (std::pair<std::string, Preset*> filament_preset : m_all_presets_map) {
        Preset *    preset      = filament_preset.second;
        auto    compatible_printers = preset->config.option<ConfigOptionStrings>("compatible_printers", true);
        if (!compatible_printers || compatible_printers->values.empty()) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "there is a preset has no compatible printers and the preset name is: " << preset->name;
            // If no compatible printers are defined, add all visible printers
            for (const std::string& visible_printer : m_visible_printers) {
                Preset* inherit_preset = nullptr;
                auto    inherit        = dynamic_cast<ConfigOptionString*>(preset->config.option(BBL_JSON_KEY_INHERITS, false));
                if (inherit && !inherit->value.empty()) {
                    std::string inherits_value = inherit->value;
                    inherit_preset             = preset_bundle->filaments.find_preset(inherits_value, false, true);
                }

                ConfigOptionStrings* filament_types;
                if (!inherit_preset) {
                    filament_types = dynamic_cast<ConfigOptionStrings*>(preset->config.option("filament_type"));
                } else {
                    filament_types = dynamic_cast<ConfigOptionStrings*>(inherit_preset->config.option("filament_type"));
                }

                if (filament_types && filament_types->values.empty())
                    continue;
                const std::string filament_type = filament_types->values[0];
                if (filament_type != type_name) {
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " preset type is not selected type and preset name is: " << preset->name;
                    continue;
                }

                std::string nozzle = get_printer_nozzle_diameter(visible_printer);
                if (nozzle_diameter[nozzle] == 0) {
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__
                                            << " compatible printer nozzle encounter exception and name is: " << visible_printer;
                    continue;
                }

                // Add all visible printers as compatible printers
                machine_name_to_presets[visible_printer].push_back(preset);
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "show compatible printer name: " << visible_printer
                                        << " and preset name is: " << preset->name;
            }
            
            continue;
        }
        for (std::string &compatible_printer_name : compatible_printers->values) {
            if (m_visible_printers.find(compatible_printer_name) == m_visible_printers.end()) {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " compatable printer is not visible and preset name is: " << preset->name;
                continue;
            }
            Preset *             inherit_preset = nullptr;
            auto inherit = dynamic_cast<ConfigOptionString*>(preset->config.option(BBL_JSON_KEY_INHERITS,false));
            if (inherit && !inherit->value.empty()) {
                std::string inherits_value = inherit->value;
                inherit_preset             = preset_bundle->filaments.find_preset(inherits_value, false, true);
            }
            ConfigOptionStrings *filament_types;
            if (!inherit_preset) {
                filament_types = dynamic_cast<ConfigOptionStrings *>(preset->config.option("filament_type"));
            } else {
                filament_types = dynamic_cast<ConfigOptionStrings *>(inherit_preset->config.option("filament_type"));
            }

            if (filament_types && filament_types->values.empty()) continue;
            const std::string filament_type = filament_types->values[0];
            if (filament_type != type_name) {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " preset type is not selected type and preset name is: " << preset->name;
                continue;
            }
            std::string nozzle = get_printer_nozzle_diameter(compatible_printer_name);
            if (nozzle_diameter[nozzle] == 0) {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " compatible printer nozzle encounter exception and name is: " << compatible_printer_name;
                continue;
            }
            machine_name_to_presets[compatible_printer_name].push_back(preset);
        }
    }
    std::vector<std::pair<std::string, std::vector<Preset *>>> printer_name_to_filament_presets;
    for (std::pair<std::string, std::vector<Preset *>> machine_filament_presets : machine_name_to_presets) {
        printer_name_to_filament_presets.push_back(machine_filament_presets);
    }
    sort_printer_by_nozzle(printer_name_to_filament_presets);
    m_filament_preset_panel->Freeze();
    for (std::pair<std::string, std::vector<Preset *>> machine_filament_presets : printer_name_to_filament_presets) {
        std::string            compatible_printer = machine_filament_presets.first;
        std::vector<Preset *> &presets      = machine_filament_presets.second;
        m_filament_presets_sizer->Add(create_select_filament_preset_checkbox(m_filament_preset_panel, compatible_printer, presets, m_machint_filament_preset), 0, wxEXPAND | wxALL, FromDIP(5));
    }
    m_filament_preset_panel->Thaw();
}

void CreateFilamentPresetDialog::get_all_filament_presets()
{
    // temp filament presets
    PresetBundle temp_preset_bundle;
    std::string dir_user_presets = wxGetApp().app_config->get("preset_folder");
    if (dir_user_presets.empty()) {
        temp_preset_bundle.load_user_presets(DEFAULT_USER_FOLDER_NAME, ForwardCompatibilitySubstitutionRule::EnableSilent);
    } else {
        temp_preset_bundle.load_user_presets(dir_user_presets, ForwardCompatibilitySubstitutionRule::EnableSilent);
    }
    const std::deque<Preset> &filament_presets = temp_preset_bundle.filaments.get_presets();

    for (const Preset &preset : filament_presets) {
        if (preset.filament_id.empty() || "null" == preset.filament_id) continue;
        std::string filament_preset_name = preset.name;
        Preset *filament_preset = new Preset(preset);
        m_all_presets_map[filament_preset_name] = filament_preset;
    }
    // global filament presets
    PresetBundle * preset_bundle = wxGetApp().preset_bundle;
    const std::deque<Preset> &temp_filament_presets = preset_bundle->filaments.get_presets();
    for (const Preset& preset : temp_filament_presets) {
        if (preset.filament_id.empty() || "null" == preset.filament_id) continue;
        auto filament_type = preset.config.option<ConfigOptionStrings>("filament_type");
        if (filament_type && filament_type->values.size())
            m_system_filament_types_set.insert(filament_type->values[0]);
        if (!preset.is_visible) continue;
        std::string filament_preset_name        = preset.name;
        Preset *filament_preset                 = new Preset(preset);
        m_all_presets_map[filament_preset_name] = filament_preset;
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " loaded preset name is: " << filament_preset->name;
    }
}

void CreateFilamentPresetDialog::get_all_visible_printer_name()
{
    PresetBundle *preset_bundle = wxGetApp().preset_bundle;
    for (const Preset &printer_preset : preset_bundle->printers.get_presets()) {
        if (!printer_preset.is_visible) continue;
        assert(m_visible_printers.find(printer_preset.name) == m_visible_printers.end());
        m_visible_printers.insert(printer_preset.name);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " entry, and visible printer is: " << printer_preset.name;
    }

}

void CreateFilamentPresetDialog::update_dialog_size()
{
    this->Freeze();
    int height_before = m_filament_preset_panel->GetSize().GetHeight();

    m_filament_preset_panel->SetSizerAndFit(m_filament_presets_sizer);

    int width  = m_filament_preset_panel->GetSize().GetWidth();
    int height = m_filament_preset_panel->GetSize().GetHeight();

    int    screen_height          = wxGetDisplaySize().GetHeight();
    wxSize dialog_size            = this->GetSize();
    int    max_available_height   = screen_height - FromDIP(100);
    int    ideal_scroll_height    = height + FromDIP(26);
    int    other_parts_height     = dialog_size.GetHeight() - m_scrolled_preset_panel->GetSize().GetHeight() + FromDIP(12);
    int    max_safe_scroll_height = max_available_height - other_parts_height;
    int    final_scroll_height    = std::min(ideal_scroll_height, max_safe_scroll_height);

    m_scrolled_preset_panel->SetMinSize(wxSize(std::min(1400, width + FromDIP(26)), final_scroll_height));
    m_scrolled_preset_panel->SetMaxSize(wxSize(std::min(1400, width + FromDIP(26)), final_scroll_height));
    m_scrolled_preset_panel->SetSize(wxSize(std::min(1500, width + FromDIP(26)), final_scroll_height));

    Layout();
    Fit();
    Refresh();
    adjust_dialog_in_screen(this);
    this->Thaw();
}

template<typename T>
void CreateFilamentPresetDialog::sort_printer_by_nozzle(std::vector<std::pair<std::string, T>> &printer_name_to_filament_preset)
{
    std::unordered_map<std::string, float> nozzle_diameter = nozzle_diameter_map;
    std::sort(printer_name_to_filament_preset.begin(), printer_name_to_filament_preset.end(),
              [&nozzle_diameter](const std::pair<string, T> &a, const std::pair<string, T> &b) {
                  size_t nozzle_index_a = a.first.find(" nozzle");
                  size_t nozzle_index_b = b.first.find(" nozzle");
                  if (nozzle_index_a == std::string::npos || nozzle_index_b == std::string::npos) return a.first < b.first;
                  std::string nozzle_str_a;
                  std::string nozzle_str_b;
                  try {
                      nozzle_str_a = a.first.substr(0, nozzle_index_a);
                      nozzle_str_b = b.first.substr(0, nozzle_index_b);
                      size_t last_space_index = nozzle_str_a.find_last_of(" ");
                      nozzle_str_a            = nozzle_str_a.substr(last_space_index + 1);
                      last_space_index        = nozzle_str_b.find_last_of(" ");
                      nozzle_str_b            = nozzle_str_b.substr(last_space_index + 1);
                  } catch (...) {
                      BOOST_LOG_TRIVIAL(info) << "substr filed, and printer name is: " << a.first << " and " << b.first;
                      return a.first < b.first;
                  }
                  float nozzle_a, nozzle_b;
                  try {
                      nozzle_a = nozzle_diameter[nozzle_str_a];
                      nozzle_b = nozzle_diameter[nozzle_str_b];
                      assert(nozzle_a != 0 && nozzle_b != 0);
                  } catch (...) {
                      BOOST_LOG_TRIVIAL(info) << "find nozzle filed, and nozzle is: " << nozzle_str_a << "mm and " << nozzle_str_b << "mm";
                      return a.first < b.first;
                  }
                  float diff_nozzle_a = std::abs(nozzle_a - 0.4);
                  float diff_nozzle_b = std::abs(nozzle_b - 0.4);
                  if (nozzle_a == nozzle_b) return a.first < b.first;
                  if (diff_nozzle_a == diff_nozzle_b) return nozzle_a < nozzle_b;

                  return diff_nozzle_a < diff_nozzle_b;
              });
}

void CreateFilamentPresetDialog::clear_filament_preset_map()
{
    m_filament_choice_map.clear();
    m_filament_preset.clear();
    m_machint_filament_preset.clear();
    m_public_name_to_filament_id_map.clear();
    m_filament_preset_panel->Freeze();
    m_filament_presets_sizer->Clear(true);
    m_filament_preset_panel->Thaw();
}

CreatePrinterPresetDialog::CreatePrinterPresetDialog(wxWindow *parent)
: DPIDialog(parent ? parent : nullptr, wxID_ANY, _L("Create Printer/Nozzle"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX | wxCENTER)
{
    m_create_type.create_printer    = _L("Create Printer");
    m_create_type.create_nozzle     = _L("Create Nozzle for Existing Printer");
    m_create_type.base_template     = _L("Create from Template");
    m_create_type.base_curr_printer = _L("Create Based on Current Printer");
    this->SetBackgroundColour(*wxWHITE);
    SetSizeHints(wxDefaultSize, wxDefaultSize);

    wxBoxSizer *m_main_sizer = new wxBoxSizer(wxVERTICAL);
    // top line
    auto m_line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 2), wxTAB_TRAVERSAL);
    m_line_top->SetBackgroundColour(wxColour(0xA6, 0xa9, 0xAA));
    m_main_sizer->Add(m_line_top, 0, wxEXPAND, 0);
    m_main_sizer->Add(0, 0, 0, wxTOP, FromDIP(5));
    m_main_sizer->Add(create_step_switch_item(), 0, wxEXPAND | wxALL, FromDIP(5));

    wxBoxSizer *page_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_page1 = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    m_page1->SetBackgroundColour(*wxWHITE);
    m_page1->SetScrollRate(5, 5);
    m_page2 = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize);\
    m_page2->SetBackgroundColour(*wxWHITE);

    create_printer_page1(m_page1);
    create_printer_page2(m_page2);
    m_page2->Hide();

    page_sizer->Add(m_page1, 1, wxEXPAND, 0);
    page_sizer->Add(m_page2, 1, wxEXPAND, 0);
    m_main_sizer->Add(page_sizer, 0, wxEXPAND | wxRIGHT | wxLEFT, FromDIP(5)); // ORCA use equal border for both sides
    select_curr_radiobox(m_create_type_btns, 0);
    select_curr_radiobox(m_create_presets_btns, 0);

    m_main_sizer->Add(0, 0, 0, wxTOP, FromDIP(10));

    this->SetSizer(m_main_sizer);

    Layout();
    Fit();

    wxSize screen_size = wxGetDisplaySize();
    int    dialogX     = (screen_size.GetWidth() - GetSize().GetWidth()) / 2;
    int    dialogY     = (screen_size.GetHeight() - GetSize().GetHeight()) / 2;
    SetPosition(wxPoint(dialogX, dialogY));

    wxGetApp().UpdateDlgDarkUI(this);
}

CreatePrinterPresetDialog::~CreatePrinterPresetDialog()
{
    clear_preset_combobox();
    if (m_printer_preset) {
        delete m_printer_preset;
        m_printer_preset = nullptr;
    }
}

void CreatePrinterPresetDialog::on_dpi_changed(const wxRect &suggested_rect) {
    Layout();
}

wxBoxSizer *CreatePrinterPresetDialog::create_step_switch_item()
{
    wxBoxSizer *step_switch_sizer = new wxBoxSizer(wxVERTICAL);

    // std::string      wiki_url             = "https://wiki.bambulab.com/en/software/bambu-studio/3rd-party-printer-profile";
    // wxHyperlinkCtrl *m_download_hyperlink = new wxHyperlinkCtrl(this, wxID_ANY, _L("wiki"), wiki_url, wxDefaultPosition, wxDefaultSize, wxHL_DEFAULT_STYLE);
    // step_switch_sizer->Add(m_download_hyperlink, 0,  wxRIGHT | wxALIGN_RIGHT, FromDIP(5));

    wxBoxSizer *horizontal_sizer  = new wxBoxSizer(wxHORIZONTAL);
    wxPanel *   step_switch_panel = new wxPanel(this);
    step_switch_panel->SetBackgroundColour(*wxWHITE);
    horizontal_sizer->Add(0, 0, 1, wxEXPAND,0);
    m_step_1 = new wxStaticBitmap(step_switch_panel, wxID_ANY, create_scaled_bitmap("step_1", nullptr, FromDIP(20)), wxDefaultPosition, wxDefaultSize);
    horizontal_sizer->Add(m_step_1, 0, wxEXPAND | wxLEFT | wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(3));
    wxStaticText *static_create_printer_text = new wxStaticText(step_switch_panel, wxID_ANY, m_create_type.create_printer, wxDefaultPosition, wxDefaultSize);
    horizontal_sizer->Add(static_create_printer_text, 0, wxEXPAND | wxLEFT | wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(3));
    auto divider_line = new wxPanel(step_switch_panel, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(50), 1));
    divider_line->SetBackgroundColour(PRINTER_LIST_COLOUR);
    horizontal_sizer->Add(divider_line, 0, wxLEFT | wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(3));
    m_step_2 = new wxStaticBitmap(step_switch_panel, wxID_ANY, create_scaled_bitmap("step_2_ready", nullptr, FromDIP(20)), wxDefaultPosition, wxDefaultSize);
    horizontal_sizer->Add(m_step_2, 0, wxEXPAND | wxLEFT | wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(3));
    wxStaticText *static_import_presets_text = new wxStaticText(step_switch_panel, wxID_ANY, _L("Import Preset"), wxDefaultPosition, wxDefaultSize);
    horizontal_sizer->Add(static_import_presets_text, 0, wxEXPAND | wxLEFT | wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(3));
    horizontal_sizer->Add(0, 0, 1, wxEXPAND, 0);

    step_switch_panel->SetSizer(horizontal_sizer);

    step_switch_sizer->Add(step_switch_panel, 0, wxBOTTOM | wxALIGN_CENTER_HORIZONTAL, FromDIP(10));

    auto line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    line_top->SetBackgroundColour(PRINTER_LIST_COLOUR);

    step_switch_sizer->Add(line_top, 0, wxEXPAND | wxALL, FromDIP(10));

    return step_switch_sizer;
}

void CreatePrinterPresetDialog::create_printer_page1(wxWindow *parent)
{
    this->SetBackgroundColour(*wxWHITE);

    m_page1_sizer = new wxBoxSizer(wxVERTICAL);

    m_page1_sizer->Add(create_type_item(parent), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(5));
    m_page1_sizer->Add(create_printer_item(parent), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(5));
    m_page1_sizer->Add(create_nozzle_diameter_item(parent), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(5));
    m_printer_info_panel = new wxPanel(parent);
    m_printer_info_panel->SetBackgroundColour(*wxWHITE);
    m_printer_info_sizer = new wxBoxSizer(wxVERTICAL);
    m_printer_info_sizer->Add(create_bed_shape_item(m_printer_info_panel), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(5));
    m_printer_info_sizer->Add(create_bed_size_item(m_printer_info_panel), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(5));
    m_printer_info_sizer->Add(create_origin_item(m_printer_info_panel), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(5));
    m_printer_info_sizer->Add(create_hot_bed_stl_item(m_printer_info_panel), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(5));
    m_printer_info_sizer->Add(create_hot_bed_svg_item(m_printer_info_panel), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(5));
    m_printer_info_sizer->Add(create_max_print_height_item(m_printer_info_panel), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(5));
    m_printer_info_panel->SetSizer(m_printer_info_sizer);
    m_page1_sizer->Add(m_printer_info_panel, 0, wxEXPAND, 0);
    m_page1_sizer->Add(create_page1_dialog_buttons(parent), 0, wxEXPAND);

    parent->SetSizerAndFit(m_page1_sizer);
    Layout();

    wxGetApp().UpdateDlgDarkUI(this);
}

wxBoxSizer *CreatePrinterPresetDialog::create_type_item(wxWindow *parent)
{
    wxBoxSizer *horizontal_sizer = new wxBoxSizer(wxHORIZONTAL);

    wxBoxSizer *  optionSizer        = new wxBoxSizer(wxVERTICAL);
    wxStaticText *static_serial_text = new wxStaticText(parent, wxID_ANY, _L("Create Type"), wxDefaultPosition, wxDefaultSize);
    optionSizer->Add(static_serial_text, 0, wxEXPAND | wxALL, 0);
    optionSizer->SetMinSize(OPTION_SIZE);
    horizontal_sizer->Add(optionSizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    wxBoxSizer *radioBoxSizer = new wxBoxSizer(wxVERTICAL);

    radioBoxSizer->Add(create_radio_item(m_create_type.create_printer, parent, wxEmptyString, m_create_type_btns), 0, wxEXPAND | wxALL, 0);
    radioBoxSizer->Add(create_radio_item(m_create_type.create_nozzle, parent, wxEmptyString, m_create_type_btns), 0, wxEXPAND | wxTOP, FromDIP(10));
    horizontal_sizer->Add(radioBoxSizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    return horizontal_sizer;
}

wxBoxSizer *CreatePrinterPresetDialog::create_printer_item(wxWindow *parent)
{
    wxBoxSizer *horizontal_sizer = new wxBoxSizer(wxHORIZONTAL);

    wxBoxSizer *  optionSizer        = new wxBoxSizer(wxVERTICAL);
    wxStaticText *static_vendor_text = new wxStaticText(parent, wxID_ANY, _L("Printer"), wxDefaultPosition, wxDefaultSize);
    optionSizer->Add(static_vendor_text, 0, wxEXPAND | wxALL, 0);
    optionSizer->SetMinSize(OPTION_SIZE);
    horizontal_sizer->Add(optionSizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    wxBoxSizer *vertical_sizer = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer *comboBoxSizer = new wxBoxSizer(wxHORIZONTAL);
    m_select_vendor            = new ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, NAME_OPTION_COMBOBOX_SIZE, 0, nullptr, wxCB_READONLY);
    m_select_vendor->SetValue(_L("Select Vendor"));
    m_select_vendor->SetLabelColor(DEFAULT_PROMPT_TEXT_COLOUR);
    wxArrayString printer_vendor;
    for (const std::string &vendor : printer_vendors) {
        assert(printer_model_map.find(vendor) != printer_model_map.end());
        printer_vendor.Add(vendor);
    }
    m_select_vendor->Set(printer_vendor);
    m_select_vendor->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent e) {
        m_select_vendor->SetLabelColor(*wxBLACK);
        std::string curr_selected_vendor = into_u8(m_select_vendor->GetStringSelection());
        std::unordered_map<std::string,std::vector<std::string>>::const_iterator iter  = printer_model_map.find(curr_selected_vendor);
        if (iter != printer_model_map.end())
        {
            std::vector<std::string> vendor_model = iter->second;
            wxArrayString            model_choice;
            for (const std::string &model : vendor_model) {
                model_choice.Add(model);
            }
            m_select_model->Set(model_choice);
            if (!model_choice.empty()) {
                m_select_model->SetSelection(0);
                m_select_model->SetLabelColor(*wxBLACK);
            }
        } else {
            MessageDialog dlg(this, _L("The model was not found, please reselect vendor."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES | wxYES_DEFAULT | wxCENTRE);
            dlg.ShowModal();
        }

        m_select_printer->SetSelection(-1);
        m_select_printer->SetValue(_L("Select Printer"));
        m_select_printer->SetLabelColor(DEFAULT_PROMPT_TEXT_COLOUR);
        e.Skip();
    });

    comboBoxSizer->Add(m_select_vendor, 0, wxEXPAND | wxALL, 0);

    m_select_model = new ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, NAME_OPTION_COMBOBOX_SIZE, 0, nullptr, wxCB_READONLY);
    comboBoxSizer->Add(m_select_model, 0, wxEXPAND | wxLEFT, FromDIP(5));
    m_select_model->SetValue(_L("Select Model"));
    m_select_model->SetLabelColor(DEFAULT_PROMPT_TEXT_COLOUR);
    m_select_model->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent e) {
        m_select_model->SetLabelColor(*wxBLACK);
        e.Skip();
    });

    m_select_printer = new ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, PRINTER_PRESET_MODEL_SIZE, 0, nullptr, wxCB_READONLY);
    comboBoxSizer->Add(m_select_printer, 0, wxEXPAND | wxALL, 0);
    m_select_printer->SetValue(_L("Select Printer"));
    m_select_printer->SetLabelColor(DEFAULT_PROMPT_TEXT_COLOUR);
    m_select_printer->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent e) {
        m_select_printer->SetLabelColor(*wxBLACK);

        e.Skip();
    });
    m_select_printer->Hide();

    m_custom_vendor_text_ctrl                      = new wxTextCtrl(parent, wxID_ANY, "", wxDefaultPosition, NAME_OPTION_COMBOBOX_SIZE);
    m_custom_vendor_text_ctrl->SetHint(_L("Input Custom Vendor"));
    m_custom_vendor_text_ctrl->Bind(wxEVT_CHAR, [](wxKeyEvent &event) {
        int key = event.GetKeyCode();
        if (cannot_input_key.find(key) != cannot_input_key.end()) { // "@" can not be inputed
            event.Skip(false);
            return;
        }
        event.Skip();
    });
    comboBoxSizer->Add(m_custom_vendor_text_ctrl, 0, wxEXPAND | wxALL, 0);
    m_custom_vendor_text_ctrl->Hide();
    m_custom_model_text_ctrl = new wxTextCtrl(parent, wxID_ANY, "", wxDefaultPosition, NAME_OPTION_COMBOBOX_SIZE);
    m_custom_model_text_ctrl->SetHint(_L("Input Custom Model"));
    m_custom_model_text_ctrl->Bind(wxEVT_CHAR, [](wxKeyEvent &event) {
        int key = event.GetKeyCode();
        if (cannot_input_key.find(key) != cannot_input_key.end()) { // "@" can not be inputed
            event.Skip(false);
            return;
        }
        event.Skip();
    });
    comboBoxSizer->Add(m_custom_model_text_ctrl, 0, wxEXPAND | wxLEFT, FromDIP(5));
    m_custom_model_text_ctrl->Hide();

    vertical_sizer->Add(comboBoxSizer, 0, wxEXPAND, 0);

    wxBoxSizer *checkbox_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_can_not_find_vendor_combox = new ::CheckBox(parent);

    checkbox_sizer->Add(m_can_not_find_vendor_combox, 0, wxALIGN_CENTER, 0);
    checkbox_sizer->Add(0, 0, 0, wxEXPAND | wxRIGHT, FromDIP(5));

    m_can_not_find_vendor_text = new wxStaticText(parent, wxID_ANY, _L("Can't find my printer model"), wxDefaultPosition, wxDefaultSize, 0);
    m_can_not_find_vendor_text->SetFont(::Label::Body_13);

    wxSize size = m_can_not_find_vendor_text->GetTextExtent(_L("Can't find my printer model"));
    m_can_not_find_vendor_text->SetMinSize(wxSize(size.x + FromDIP(4), -1));
    m_can_not_find_vendor_text->Wrap(-1);
    checkbox_sizer->Add(m_can_not_find_vendor_text, 0, wxALIGN_CENTER, 0);

    m_can_not_find_vendor_combox->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &e) {
        bool value = m_can_not_find_vendor_combox->GetValue();
        if (value) {
            m_can_not_find_vendor_combox->SetValue(true);
            m_custom_vendor_text_ctrl->Show();
            m_custom_model_text_ctrl->Show();
            m_select_vendor->Hide();
            m_select_model->Hide();
        } else {
            m_can_not_find_vendor_combox->SetValue(false);
            m_custom_vendor_text_ctrl->Hide();
            m_custom_model_text_ctrl->Hide();
            m_select_vendor->Show();
            m_select_model->Show();
        }
        Refresh();
        Layout();
        m_page1->SetSizerAndFit(m_page1_sizer);
        Fit();

        e.Skip();
    });

    vertical_sizer->Add(checkbox_sizer, 0, wxEXPAND | wxTOP, FromDIP(5));

    horizontal_sizer->Add(vertical_sizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    return horizontal_sizer;

}

wxBoxSizer *CreatePrinterPresetDialog::create_nozzle_diameter_item(wxWindow *parent)
{
    wxBoxSizer *horizontal_sizer = new wxBoxSizer(wxHORIZONTAL);

    wxBoxSizer *  optionSizer      = new wxBoxSizer(wxVERTICAL);
    wxStaticText *static_type_text = new wxStaticText(parent, wxID_ANY, _L("Nozzle Diameter"), wxDefaultPosition, wxDefaultSize);
    optionSizer->Add(static_type_text, 0, wxEXPAND | wxALL, 0);
    optionSizer->SetMinSize(OPTION_SIZE);
    horizontal_sizer->Add(optionSizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    wxBoxSizer *vertical_sizer = new wxBoxSizer(wxVERTICAL);
    wxBoxSizer *comboBoxSizer = new wxBoxSizer(wxHORIZONTAL);
    m_nozzle_diameter         = new ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, OPTION_SIZE, 0, nullptr, wxCB_READONLY);
    wxArrayString nozzle_diameters;
    const char    dec_sep = is_decimal_separator_point() ? '.' : ',';
    for (const std::string& nozzle : nozzle_diameter_vec) {
        std::string display_nozzle = nozzle;
        size_t pos = display_nozzle.find('.');
        if (pos != std::string::npos) { display_nozzle.replace(pos, 1, 1, dec_sep); }
        nozzle_diameters.Add(display_nozzle + " mm");
    }
    m_nozzle_diameter->Set(nozzle_diameters);
    m_nozzle_diameter->SetSelection(0);
    comboBoxSizer->Add(m_nozzle_diameter, 0, wxEXPAND | wxALL, 0);

    m_custom_nozzle_diameter_ctrl = new wxTextCtrl(parent, wxID_ANY, "", wxDefaultPosition, NAME_OPTION_COMBOBOX_SIZE);
    m_custom_nozzle_diameter_ctrl->SetHint(_L("Input Custom Nozzle Diameter"));
    m_custom_nozzle_diameter_ctrl->Bind(wxEVT_CHAR, [this](wxKeyEvent &event) {
        int key = event.GetKeyCode();
        if (key != 44 && key != 46 && cannot_input_key.find(key) != cannot_input_key.end()) { // "@" can not be inputed
            event.Skip(false);
            return;
        }
        event.Skip();
    });
    comboBoxSizer->Add(m_custom_nozzle_diameter_ctrl, 0, wxEXPAND | wxALL, 0);
    m_custom_nozzle_diameter_ctrl->Hide();
    vertical_sizer->Add(comboBoxSizer, 0, wxEXPAND, 0);

    wxBoxSizer *checkbox_sizer   = new wxBoxSizer(wxHORIZONTAL);
    m_can_not_find_nozzle_checkbox = new ::CheckBox(parent);

    checkbox_sizer->Add(m_can_not_find_nozzle_checkbox, 0, wxALIGN_CENTER, 0);
    checkbox_sizer->Add(0, 0, 0, wxEXPAND | wxRIGHT, FromDIP(5));

    auto can_not_find_nozzle_diameter = new wxStaticText(parent, wxID_ANY, _L("Can't find my nozzle diameter"), wxDefaultPosition, wxDefaultSize, 0);
    can_not_find_nozzle_diameter->SetFont(::Label::Body_13);

    wxSize size = can_not_find_nozzle_diameter->GetTextExtent(_L("Can't find my printer model"));
    can_not_find_nozzle_diameter->SetMinSize(wxSize(size.x + FromDIP(4), -1));
    can_not_find_nozzle_diameter->Wrap(-1);
    checkbox_sizer->Add(can_not_find_nozzle_diameter, 0, wxALIGN_CENTER, 0);

    m_can_not_find_nozzle_checkbox->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &e) {
        bool value = m_can_not_find_nozzle_checkbox->GetValue();
        if (value) {
            m_can_not_find_nozzle_checkbox->SetValue(true);
            m_custom_nozzle_diameter_ctrl->Show();
            m_nozzle_diameter->Hide();
        } else {
            m_can_not_find_nozzle_checkbox->SetValue(false);
            m_custom_nozzle_diameter_ctrl->Hide();
            m_nozzle_diameter->Show();
        }
        Refresh();
        Layout();
        m_page1->SetSizerAndFit(m_page1_sizer);
        Fit();

        e.Skip();
    });

    vertical_sizer->Add(checkbox_sizer, 0, wxEXPAND | wxTOP, FromDIP(5));
    horizontal_sizer->Add(vertical_sizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(10));
    horizontal_sizer->Add(0, 0, 0, wxEXPAND | wxLEFT, FromDIP(200));

    return horizontal_sizer;
}

wxBoxSizer *CreatePrinterPresetDialog::create_bed_shape_item(wxWindow *parent)
{
    wxBoxSizer *horizontal_sizer = new wxBoxSizer(wxHORIZONTAL);

    wxBoxSizer *  optionSizer      = new wxBoxSizer(wxVERTICAL);
    wxStaticText *static_type_text = new wxStaticText(parent, wxID_ANY, _L("Bed Shape"), wxDefaultPosition, wxDefaultSize);
    optionSizer->Add(static_type_text, 0, wxEXPAND | wxALL, 0);
    optionSizer->SetMinSize(OPTION_SIZE);
    horizontal_sizer->Add(optionSizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    wxBoxSizer *  bed_shape_sizer       = new wxBoxSizer(wxVERTICAL);
    wxStaticText *static_bed_shape_text = new wxStaticText(parent, wxID_ANY, _L("Rectangle"), wxDefaultPosition, wxDefaultSize);
    bed_shape_sizer->Add(static_bed_shape_text, 0, wxEXPAND | wxALL, 0);
    horizontal_sizer->Add(bed_shape_sizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    return horizontal_sizer;
}

wxBoxSizer *CreatePrinterPresetDialog::create_bed_size_item(wxWindow *parent)
{
    wxBoxSizer *horizontal_sizer = new wxBoxSizer(wxHORIZONTAL);

    wxBoxSizer *  optionSizer      = new wxBoxSizer(wxVERTICAL);
    wxStaticText *static_type_text = new wxStaticText(parent, wxID_ANY, _L("Printable Space"), wxDefaultPosition, wxDefaultSize);
    optionSizer->Add(static_type_text, 0, wxEXPAND | wxALL, 0);
    optionSizer->SetMinSize(OPTION_SIZE);
    horizontal_sizer->Add(optionSizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    wxBoxSizer *  length_sizer          = new wxBoxSizer(wxVERTICAL);
     // ORCA use icon on input box to match style with other Point fields
    horizontal_sizer->Add(length_sizer, 0, wxEXPAND | wxLEFT | wxTOP | wxALIGN_CENTER_VERTICAL, FromDIP(10));
    wxBoxSizer *length_input_sizer      = new wxBoxSizer(wxVERTICAL);
    m_bed_size_x_input = new TextInput(parent, "200", _L("mm"), "inputbox_x", wxDefaultPosition, PRINTER_SPACE_SIZE, wxTE_PROCESS_ENTER);
    wxTextValidator validator(wxFILTER_DIGITS);
    m_bed_size_x_input->GetTextCtrl()->SetValidator(validator);
    length_input_sizer->Add(m_bed_size_x_input, 0, wxEXPAND | wxLEFT, FromDIP(5));
    horizontal_sizer->Add(length_input_sizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));

    wxBoxSizer *  width_sizer      = new wxBoxSizer(wxVERTICAL);
    // ORCA use icon on input box to match style with other Point fields
    horizontal_sizer->Add(width_sizer, 0, wxEXPAND | wxLEFT | wxTOP | wxALIGN_CENTER_VERTICAL, FromDIP(10));
    wxBoxSizer *width_input_sizer      = new wxBoxSizer(wxVERTICAL);
    m_bed_size_y_input            = new TextInput(parent, "200", _L("mm"), "inputbox_y", wxDefaultPosition, PRINTER_SPACE_SIZE, wxTE_PROCESS_ENTER);
    m_bed_size_y_input->GetTextCtrl()->SetValidator(validator);
    width_input_sizer->Add(m_bed_size_y_input, 0, wxEXPAND | wxALL, 0);
    horizontal_sizer->Add(width_input_sizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));

    return horizontal_sizer;

}

wxBoxSizer *CreatePrinterPresetDialog::create_origin_item(wxWindow *parent)
{
    wxBoxSizer *horizontal_sizer = new wxBoxSizer(wxHORIZONTAL);

    wxBoxSizer *  optionSizer      = new wxBoxSizer(wxVERTICAL);
    wxStaticText *static_type_text = new wxStaticText(parent, wxID_ANY, _L("Origin"), wxDefaultPosition, wxDefaultSize);
    optionSizer->Add(static_type_text, 0, wxEXPAND | wxALL, 0);
    optionSizer->SetMinSize(OPTION_SIZE);
    horizontal_sizer->Add(optionSizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    wxBoxSizer *  length_sizer       = new wxBoxSizer(wxVERTICAL);
    // ORCA use icon on input box to match style with other Point fields
    horizontal_sizer->Add(length_sizer, 0, wxEXPAND | wxLEFT | wxTOP | wxALIGN_CENTER_VERTICAL, FromDIP(10));
    wxBoxSizer *length_input_sizer = new wxBoxSizer(wxVERTICAL);
    m_bed_origin_x_input           = new TextInput(parent, "0", _L("mm"), "inputbox_x", wxDefaultPosition, PRINTER_SPACE_SIZE, wxTE_PROCESS_ENTER);
    wxTextValidator validator(wxFILTER_DIGITS);
    m_bed_origin_x_input->GetTextCtrl()->SetValidator(validator);
    length_input_sizer->Add(m_bed_origin_x_input, 0, wxEXPAND | wxLEFT, FromDIP(5)); // Align with other
    horizontal_sizer->Add(length_input_sizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));

    wxBoxSizer *  width_sizer       = new wxBoxSizer(wxVERTICAL);
    // ORCA use icon on input box to match style with other Point fields
    horizontal_sizer->Add(width_sizer, 0, wxEXPAND | wxLEFT | wxTOP | wxALIGN_CENTER_VERTICAL, FromDIP(10));
    wxBoxSizer *width_input_sizer = new wxBoxSizer(wxVERTICAL);
    m_bed_origin_y_input          = new TextInput(parent, "0", _L("mm"), "inputbox_y", wxDefaultPosition, PRINTER_SPACE_SIZE, wxTE_PROCESS_ENTER);
    m_bed_origin_y_input->GetTextCtrl()->SetValidator(validator);
    width_input_sizer->Add(m_bed_origin_y_input, 0, wxEXPAND | wxALL, 0);
    horizontal_sizer->Add(width_input_sizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));

    return horizontal_sizer;
}

wxBoxSizer *CreatePrinterPresetDialog::create_hot_bed_stl_item(wxWindow *parent)
{
    wxBoxSizer *horizontal_sizer = new wxBoxSizer(wxHORIZONTAL);

    wxBoxSizer *  optionSizer      = new wxBoxSizer(wxVERTICAL);
    wxStaticText *static_type_text = new wxStaticText(parent, wxID_ANY, _L("Hot Bed STL"), wxDefaultPosition, wxDefaultSize);
    optionSizer->Add(static_type_text, 0, wxEXPAND | wxALL, 0);
    optionSizer->SetMinSize(OPTION_SIZE);
    horizontal_sizer->Add(optionSizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    wxBoxSizer *hot_bed_stl_sizer = new wxBoxSizer(wxVERTICAL);

    m_button_bed_stl = new Button(parent, _L("Load..."));
    m_button_bed_stl->SetStyle(ButtonStyle::Regular, ButtonType::Parameter);
    m_button_bed_stl->Bind(wxEVT_BUTTON, ([this](wxCommandEvent& e) { load_model_stl(); }));

    hot_bed_stl_sizer->Add(m_button_bed_stl, 0, wxEXPAND | wxALL, 0);

    horizontal_sizer->Add(hot_bed_stl_sizer, 0, wxEXPAND | wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    m_upload_stl_tip_text = new wxStaticText(parent, wxID_ANY, "", wxDefaultPosition, wxDefaultSize);
    m_upload_stl_tip_text->SetLabelText(_L("Empty"));
    horizontal_sizer->Add(m_upload_stl_tip_text, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(10));
    return horizontal_sizer;
}

wxBoxSizer *CreatePrinterPresetDialog::create_hot_bed_svg_item(wxWindow *parent)
{
    wxBoxSizer *horizontal_sizer = new wxBoxSizer(wxHORIZONTAL);

    wxBoxSizer *  optionSizer      = new wxBoxSizer(wxVERTICAL);
    wxStaticText *static_type_text = new wxStaticText(parent, wxID_ANY, _L("Hot Bed SVG"), wxDefaultPosition, wxDefaultSize);
    optionSizer->Add(static_type_text, 0, wxEXPAND | wxALL, 0);
    optionSizer->SetMinSize(OPTION_SIZE);
    horizontal_sizer->Add(optionSizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    wxBoxSizer *hot_bed_stl_sizer = new wxBoxSizer(wxVERTICAL);

    m_button_bed_svg = new Button(parent, _L("Load..."));
    m_button_bed_svg->SetStyle(ButtonStyle::Regular, ButtonType::Parameter);
    m_button_bed_svg->Bind(wxEVT_BUTTON, ([this](wxCommandEvent &e) { load_texture(); }));

    hot_bed_stl_sizer->Add(m_button_bed_svg, 0, wxEXPAND | wxALL, 0);

    horizontal_sizer->Add(hot_bed_stl_sizer, 0, wxEXPAND | wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    m_upload_svg_tip_text = new wxStaticText(parent, wxID_ANY, "", wxDefaultPosition, wxDefaultSize);
    m_upload_svg_tip_text->SetLabelText(_L("Empty"));
    horizontal_sizer->Add(m_upload_svg_tip_text, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(10));
    return horizontal_sizer;
}

wxBoxSizer *CreatePrinterPresetDialog::create_max_print_height_item(wxWindow *parent)
{
    wxBoxSizer *  horizontal_sizer  = new wxBoxSizer(wxHORIZONTAL);

    wxBoxSizer *  optionSizer      = new wxBoxSizer(wxVERTICAL);
    wxStaticText *static_type_text = new wxStaticText(parent, wxID_ANY, _L("Max Print Height"), wxDefaultPosition, wxDefaultSize);
    optionSizer->Add(static_type_text, 0, wxEXPAND | wxALL, 0);
    optionSizer->SetMinSize(OPTION_SIZE);
    horizontal_sizer->Add(optionSizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    wxBoxSizer *hight_input_sizer = new wxBoxSizer(wxVERTICAL);
    m_print_height_input          = new TextInput(parent, "200", _L("mm"), wxEmptyString, wxDefaultPosition, PRINTER_SPACE_SIZE, wxTE_PROCESS_ENTER); // Use same alignment with all other input boxes
    wxTextValidator validator(wxFILTER_DIGITS);
    m_print_height_input->GetTextCtrl()->SetValidator(validator);
    hight_input_sizer->Add(m_print_height_input, 0, wxEXPAND | wxLEFT, FromDIP(5));
    horizontal_sizer->Add(hight_input_sizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(5));

    return horizontal_sizer;
}

wxWindow *CreatePrinterPresetDialog::create_page1_dialog_buttons(wxWindow *parent)
{
    auto dlg_btns = new DialogButtons(parent, {"OK", "Cancel"});
    
    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        if (!validate_input_valid()) return;
        data_init();
        show_page2();
    });

    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) { EndModal(wxID_CANCEL); });

    return dlg_btns;
}
static std::string last_directory = "";
void CreatePrinterPresetDialog::load_texture() {
    wxFileDialog       dialog(this, _L("Choose a file to import bed texture from (PNG/SVG):"), last_directory, "", file_wildcards(FT_TEX), wxFD_OPEN | wxFD_FILE_MUST_EXIST);

    if (dialog.ShowModal() != wxID_OK)
        return;

    m_custom_texture = "";
    m_upload_svg_tip_text->SetLabelText(_L("Empty"));
    last_directory        = dialog.GetDirectory().ToUTF8().data();
    std::string file_name = dialog.GetPath().ToUTF8().data();
    if (!boost::algorithm::iends_with(file_name, ".png") && !boost::algorithm::iends_with(file_name, ".svg")) {
        show_error(this, _L("Invalid file format."));
        return;
    }
    bool try_ok;
    if (Utils::is_file_too_large(file_name, try_ok)) {
        if (try_ok) {
            m_upload_svg_tip_text->SetLabelText(wxString::Format(_L("The file exceeds %d MB, please import again."), STL_SVG_MAX_FILE_SIZE_MB));
        } else {
            m_upload_svg_tip_text->SetLabelText(_L("Exception in obtaining file size, please import again."));
        }
        return;
    }
    m_custom_texture = file_name;
    wxGCDC dc;
    auto text = wxControl::Ellipsize(_L(boost::filesystem::path(file_name).filename().string()), dc, wxELLIPSIZE_END, FromDIP(200));
    m_upload_svg_tip_text->SetLabelText(text);
}

void CreatePrinterPresetDialog::load_model_stl()
{
    wxFileDialog dialog(this, _L("Choose an STL file to import bed model from:"), last_directory, "", file_wildcards(FT_STL), wxFD_OPEN | wxFD_FILE_MUST_EXIST);

    if (dialog.ShowModal() != wxID_OK)
        return;

    m_custom_model = "";
    m_upload_stl_tip_text->SetLabelText(_L("Empty"));
    last_directory        = dialog.GetDirectory().ToUTF8().data();
    std::string file_name = dialog.GetPath().ToUTF8().data();
    if (!boost::algorithm::iends_with(file_name, ".stl")) {
        show_error(this, _L("Invalid file format."));
        return;
    }
    bool try_ok;
    if (Utils::is_file_too_large(file_name, try_ok)) {
        if (try_ok) {
            m_upload_stl_tip_text->SetLabelText(wxString::Format(_L("The file exceeds %d MB, please import again."), STL_SVG_MAX_FILE_SIZE_MB));
        }
        else {
            m_upload_stl_tip_text->SetLabelText(_L("Exception in obtaining file size, please import again."));
        }
        return;
    }
    m_custom_model = file_name;
    wxGCDC dc;
    auto text      = wxControl::Ellipsize(_L(boost::filesystem::path(file_name).filename().string()), dc, wxELLIPSIZE_END, FromDIP(200));
    m_upload_stl_tip_text->SetLabelText(text);
}

bool CreatePrinterPresetDialog::load_system_and_user_presets_with_curr_model(PresetBundle &temp_preset_bundle, bool just_template)
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " is load template: "<< just_template;
    std::string selected_vendor_id;
    std::string preset_path;
    if (m_printer_preset) {
        delete m_printer_preset;
        m_printer_preset = nullptr;
    }

    std::string curr_selected_model = into_u8(m_printer_model->GetStringSelection());
    int         nozzle_index        = curr_selected_model.find_first_of("@");
    std::string select_model        = curr_selected_model.substr(0, nozzle_index - 1);
    for (const Slic3r::VendorProfile::PrinterModel &model : m_printer_preset_vendor_selected.models) {
        if (model.name == select_model) {
            m_printer_preset_model_selected = model;
            break;
        }
    }
    if (m_printer_preset_vendor_selected.id.empty() || m_printer_preset_model_selected.id.empty()) {
        BOOST_LOG_TRIVIAL(info) << "selected id was not found";
        MessageDialog dlg(this, _L("Preset path was not found, please reselect vendor."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES_NO | wxYES_DEFAULT | wxCENTRE);
        dlg.ShowModal();
        return false;
    }

    bool is_custom_vendor = false;
    if (PRESET_CUSTOM_VENDOR == m_printer_preset_vendor_selected.name || PRESET_CUSTOM_VENDOR == m_printer_preset_vendor_selected.id) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " select custom vendor ";
        is_custom_vendor   = true;
        temp_preset_bundle = *(wxGetApp().preset_bundle);
    } else {
        selected_vendor_id = m_printer_preset_vendor_selected.id;

        if (boost::filesystem::exists(boost::filesystem::path(Slic3r::data_dir()) / PRESET_SYSTEM_DIR / selected_vendor_id)) {
            preset_path = (boost::filesystem::path(Slic3r::data_dir()) / PRESET_SYSTEM_DIR).string();
        } else if (boost::filesystem::exists(boost::filesystem::path(Slic3r::resources_dir()) / "profiles" / selected_vendor_id)) {
            preset_path = (boost::filesystem::path(Slic3r::resources_dir()) / "profiles").string();
        }

        if (preset_path.empty()) {
            BOOST_LOG_TRIVIAL(info) << "Preset path was not found";
            MessageDialog dlg(this, _L("Preset path was not found, please reselect vendor."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"),
                              wxYES_NO | wxYES_DEFAULT | wxCENTRE);
            dlg.ShowModal();
            return false;
        }

        try {
            temp_preset_bundle.load_vendor_configs_from_json(preset_path, selected_vendor_id, PresetBundle::LoadConfigBundleAttribute::LoadSystem,
                                                             ForwardCompatibilitySubstitutionRule::EnableSilent);
        } catch (...) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "load vendor fonfigs form json failed";
            MessageDialog dlg(this, _L("The printer model was not found, please reselect."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"),
                              wxYES_NO | wxYES_DEFAULT | wxCENTRE);
            dlg.ShowModal();
            return false;
        }

        if (!just_template) {
            std::string dir_user_presets = wxGetApp().app_config->get("preset_folder");
            if (dir_user_presets.empty()) {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "default user presets path";
                temp_preset_bundle.load_user_presets(DEFAULT_USER_FOLDER_NAME, ForwardCompatibilitySubstitutionRule::EnableSilent);
            } else {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "user presets path";
                temp_preset_bundle.load_user_presets(dir_user_presets, ForwardCompatibilitySubstitutionRule::EnableSilent);
            }
        }
    }
    //get model varient
    std::string model_varient = into_u8(m_printer_model->GetStringSelection());
    size_t      index_at      = model_varient.find(" @ ");
    size_t      index_nozzle  = model_varient.find("nozzle");
    std::string varient;
    if (index_at != std::string::npos && index_nozzle != std::string::npos) {
        varient = model_varient.substr(index_at + 3, index_nozzle - index_at - 4);
    } else {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "get nozzle failed";
        MessageDialog dlg(this, _L("The nozzle diameter was not found, please reselect."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES_NO | wxYES_DEFAULT | wxCENTRE);
        dlg.ShowModal();
        return false;
    }

    const Preset *temp_printer_preset = is_custom_vendor ? temp_preset_bundle.printers.find_custom_preset_by_model_and_variant(m_printer_preset_model_selected.id, varient) :
                                                           temp_preset_bundle.printers.find_system_preset_by_model_and_variant(m_printer_preset_model_selected.id, varient);

    if (temp_printer_preset) {
        m_printer_preset = new Preset(*temp_printer_preset);
    } else {
        MessageDialog dlg(this, _L("The printer preset was not found, please reselect."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES_NO | wxYES_DEFAULT | wxCENTRE);
        dlg.ShowModal();
        return false;
    }

    if (!just_template) {
        temp_preset_bundle.printers.select_preset_by_name(m_printer_preset->name, true);
        temp_preset_bundle.update_compatible(PresetSelectCompatibleType::Always);
    } else {
        selected_vendor_id = PRESET_TEMPLATE_DIR;
        preset_path.clear();
        if (boost::filesystem::exists(boost::filesystem::path(Slic3r::resources_dir()) / PRESET_PROFILES_TEMOLATE_DIR / selected_vendor_id)) {
            preset_path = (boost::filesystem::path(Slic3r::resources_dir()) / PRESET_PROFILES_TEMOLATE_DIR).string();
        }
        if (preset_path.empty()) {
            BOOST_LOG_TRIVIAL(info) << "Preset path was not found";
            MessageDialog dlg(this, _L("Preset path was not found, please reselect vendor."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"),
                              wxYES_NO | wxYES_DEFAULT | wxCENTRE);
            dlg.ShowModal();
            return false;
        }
        try {
            temp_preset_bundle.load_vendor_configs_from_json(preset_path, selected_vendor_id, PresetBundle::LoadConfigBundleAttribute::LoadSystem,
                                                             ForwardCompatibilitySubstitutionRule::EnableSilent);
        } catch (...) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "load template vendor configs form json failed";
            MessageDialog dlg(this, _L("The printer model was not found, please reselect."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"),
                              wxYES_NO | wxYES_DEFAULT | wxCENTRE);
            dlg.ShowModal();
            return false;
        }
    }

    return true;
}

void CreatePrinterPresetDialog::generate_process_presets_data(std::vector<Preset const *> presets, std::string nozzle)
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " entry, and nozzle is: " << nozzle;
    std::unordered_map<std::string, float> nozzle_diameter_map_ = nozzle_diameter_map;
    float                                  nozzle_dia           = my_stof(get_nozzle_diameter());
    for (const Preset *preset : presets) {
        auto layer_height = dynamic_cast<ConfigOptionFloat *>(const_cast<Preset *>(preset)->config.option("layer_height", true));
        if (layer_height)
            layer_height->value = nozzle_dia / 2;
        else
            BOOST_LOG_TRIVIAL(info) << "process template has no layer_height";

        auto initial_layer_print_height = dynamic_cast<ConfigOptionFloat *>(const_cast<Preset *>(preset)->config.option("initial_layer_print_height", true));
        if (initial_layer_print_height)
            initial_layer_print_height->value = nozzle_dia / 2;
        else
            BOOST_LOG_TRIVIAL(info) << "process template has no initial_layer_print_height";

        auto line_width = dynamic_cast<ConfigOptionFloat *>(const_cast<Preset *>(preset)->config.option("line_width", true));
        if (line_width)
            line_width->value = nozzle_dia;
        else
            BOOST_LOG_TRIVIAL(info) << "process template has no line_width";

        auto initial_layer_line_width = dynamic_cast<ConfigOptionFloat *>(const_cast<Preset *>(preset)->config.option("initial_layer_line_width", true));
        if (initial_layer_line_width)
            initial_layer_line_width->value = nozzle_dia;
        else
            BOOST_LOG_TRIVIAL(info) << "process template has no initial_layer_line_width";

        auto outer_wall_line_width = dynamic_cast<ConfigOptionFloat *>(const_cast<Preset *>(preset)->config.option("outer_wall_line_width", true));
        if (outer_wall_line_width)
            outer_wall_line_width->value = nozzle_dia;
        else
            BOOST_LOG_TRIVIAL(info) << "process template has no outer_wall_line_width";

        auto inner_wall_line_width = dynamic_cast<ConfigOptionFloat *>(const_cast<Preset *>(preset)->config.option("inner_wall_line_width", true));
        if (inner_wall_line_width)
            inner_wall_line_width->value = nozzle_dia;
        else
            BOOST_LOG_TRIVIAL(info) << "process template has no inner_wall_line_width";

        auto top_surface_line_width = dynamic_cast<ConfigOptionFloat *>(const_cast<Preset *>(preset)->config.option("top_surface_line_width", true));
        if (top_surface_line_width)
            top_surface_line_width->value = nozzle_dia;
        else
            BOOST_LOG_TRIVIAL(info) << "process template has no top_surface_line_width";

        auto sparse_infill_line_width = dynamic_cast<ConfigOptionFloat *>(const_cast<Preset *>(preset)->config.option("sparse_infill_line_width", true));
        if (sparse_infill_line_width)
            sparse_infill_line_width->value = nozzle_dia;
        else
            BOOST_LOG_TRIVIAL(info) << "process template has no sparse_infill_line_width";

        auto internal_solid_infill_line_width = dynamic_cast<ConfigOptionFloat *>(const_cast<Preset *>(preset)->config.option("internal_solid_infill_line_width", true));
        if (internal_solid_infill_line_width)
            internal_solid_infill_line_width->value = nozzle_dia;
        else
            BOOST_LOG_TRIVIAL(info) << "process template has no internal_solid_infill_line_width";

        auto support_line_width = dynamic_cast<ConfigOptionFloat *>(const_cast<Preset *>(preset)->config.option("support_line_width", true));
        if (support_line_width)
            support_line_width->value = nozzle_dia;
        else
            BOOST_LOG_TRIVIAL(info) << "process template has no support_line_width";

        auto wall_loops = dynamic_cast<ConfigOptionInt *>(const_cast<Preset *>(preset)->config.option("wall_loops", true));
        if (wall_loops)
            wall_loops->value = std::max(2, (int) std::ceil(2 * 0.4 / nozzle_dia));
        else
            BOOST_LOG_TRIVIAL(info) << "process template has no wall_loops";

        auto top_shell_layers = dynamic_cast<ConfigOptionInt *>(const_cast<Preset *>(preset)->config.option("top_shell_layers", true));
        if (top_shell_layers)
            top_shell_layers->value = std::max(5, (int) std::ceil(5 * 0.4 / nozzle_dia));
        else
            BOOST_LOG_TRIVIAL(info) << "process template has no top_shell_layers";

        auto bottom_shell_layers = dynamic_cast<ConfigOptionInt *>(const_cast<Preset *>(preset)->config.option("bottom_shell_layers", true));
        if (bottom_shell_layers)
            bottom_shell_layers->value = std::max(3, (int) std::ceil(3 * 0.4 / nozzle_dia));
        else
            BOOST_LOG_TRIVIAL(info) << "process template has no bottom_shell_layers";
    }
}

void CreatePrinterPresetDialog::update_preset_list_size()
{
    m_scrolled_preset_window->Freeze();
    m_preset_template_panel->SetSizerAndFit(m_filament_sizer);
    m_preset_template_panel->SetMinSize(wxSize(FromDIP(660), -1));
    m_preset_template_panel->SetSize(wxSize(FromDIP(660), -1));
    int width = m_preset_template_panel->GetSize().GetWidth();
    int height = m_preset_template_panel->GetSize().GetHeight();
    m_scrolled_preset_window->SetMinSize(wxSize(std::min(1500, width + FromDIP(26)), std::min(600, height)));
    m_scrolled_preset_window->SetMaxSize(wxSize(std::min(1500, width + FromDIP(26)), std::min(600, height)));
    m_scrolled_preset_window->SetSize(wxSize(std::min(1500, width + FromDIP(26)), std::min(600, height)));
    m_page2->SetSizerAndFit(m_page2_sizer);
    Layout();
    Fit();
    Refresh();
    adjust_dialog_in_screen(this);
    m_scrolled_preset_window->Thaw();
}

std::string CreatePrinterPresetDialog::get_printer_vendor() const
{
    assert(curr_create_printer_type() == m_create_type.create_printer);
    std::string custom_vendor;
    if (m_can_not_find_vendor_combox->GetValue()) {
        custom_vendor = into_u8(m_custom_vendor_text_ctrl->GetValue());
        custom_vendor             = remove_special_key(custom_vendor);
        boost::algorithm::trim(custom_vendor);
    } else {
        custom_vendor = into_u8(m_select_vendor->GetStringSelection());
    }
    return custom_vendor;
}

std::string CreatePrinterPresetDialog::get_printer_model() const
{
    assert(curr_create_printer_type() == m_create_type.create_printer);
    std::string custom_model;
    if (m_can_not_find_vendor_combox->GetValue()) {
        custom_model  = into_u8(m_custom_model_text_ctrl->GetValue());
        custom_model              = remove_special_key(custom_model);
        boost::algorithm::trim(custom_model);
    } else {
        custom_model = into_u8(m_select_model->GetStringSelection());
    }
    return custom_model;
}

std::string CreatePrinterPresetDialog::get_nozzle_diameter() const
{
    std::string diameter;
    if (m_can_not_find_nozzle_checkbox->GetValue()) {
        diameter = into_u8(m_custom_nozzle_diameter_ctrl->GetValue());
    } else {
        diameter = into_u8(m_nozzle_diameter->GetStringSelection());
        size_t index_mm = diameter.find(" mm");
        if (std::string::npos != index_mm) { diameter = diameter.substr(0, index_mm); }
    }
    float nozzle = 0;
    try {
        nozzle = my_stof(diameter);
    }
    catch (...) { }
    if (nozzle == 0) diameter = "0.4";
    return diameter;
}

std::string CreatePrinterPresetDialog::get_custom_printer_model() const
{
    const wxString curr_selected_printer_type = curr_create_printer_type();
    std::string    printer_model_name;
    if (curr_selected_printer_type == m_create_type.create_printer) {
        std::string custom_vendor = get_printer_vendor();
        std::string custom_model  = get_printer_model();
        printer_model_name        = custom_vendor + " " + custom_model;
    } else if (curr_selected_printer_type == m_create_type.create_nozzle) {
        std::string selected_printer_preset_name = into_u8(m_select_printer->GetStringSelection());
        std::unordered_map<std::string, std::shared_ptr<Preset>>::const_iterator itor = m_printer_name_to_preset.find(selected_printer_preset_name);
        assert(m_printer_name_to_preset.end() != itor);
        if (m_printer_name_to_preset.end() != itor) {
            std::shared_ptr<Preset> printer_preset = itor->second;
            try {
                printer_model_name  = printer_preset->config.opt_string("printer_model", true);
            } catch (...) {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " get config printer_model or , and the name is: " << selected_printer_preset_name;
            }

        } else {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " don't get printer preset, and the name is: " << selected_printer_preset_name;
        }
    }
    return printer_model_name;
}

std::string CreatePrinterPresetDialog::get_custom_printer_name() const
{
    return get_custom_printer_model() + " " + get_nozzle_diameter() + " nozzle";
}

wxBoxSizer *CreatePrinterPresetDialog::create_radio_item(wxString title, wxWindow *parent, wxString tooltip, std::vector<std::pair<RadioBox *, wxString>> &radiobox_list)
{
    wxBoxSizer *horizontal_sizer = new wxBoxSizer(wxHORIZONTAL);
    RadioBox *  radiobox         = new RadioBox(parent);
    horizontal_sizer->Add(radiobox, 0, wxEXPAND | wxALL, 0);
    horizontal_sizer->Add(0, 0, 0, wxEXPAND | wxLEFT, FromDIP(5));
    radiobox_list.push_back(std::make_pair(radiobox,title));
    int btn_idx = radiobox_list.size() - 1;
    radiobox->Bind(wxEVT_LEFT_DOWN, [this, &radiobox_list, btn_idx](wxMouseEvent &e) {
        select_curr_radiobox(radiobox_list, btn_idx);
    });

    wxStaticText *text = new wxStaticText(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize);
    text->Bind(wxEVT_LEFT_DOWN, [this, &radiobox_list, btn_idx](wxMouseEvent &e) {
        select_curr_radiobox(radiobox_list, btn_idx);
    });
    horizontal_sizer->Add(text, 0, wxEXPAND | wxLEFT, 0);

    radiobox->SetToolTip(tooltip);
    text->SetToolTip(tooltip);
    return horizontal_sizer;

}

void CreatePrinterPresetDialog::select_curr_radiobox(std::vector<std::pair<RadioBox *, wxString>> &radiobox_list, int btn_idx)
{
    int len = radiobox_list.size();
    for (int i = 0; i < len; ++i) {
        if (i == btn_idx) {
            if (!radiobox_list[i].first->IsEnabled())
                return;
            radiobox_list[i].first->SetValue(true);
            wxString curr_selected_type = radiobox_list[i].second;
            this->Freeze();
            if (curr_selected_type == m_create_type.base_template) {
                if (m_printer_model->GetValue() == _L("Select Model")) {
                    m_filament_preset_template_sizer->Clear(true);
                    m_filament_preset.clear();
                    m_process_preset_template_sizer->Clear(true);
                    m_process_preset.clear();
                } else {
                    update_presets_list(true);
                }
                m_page2->SetSizerAndFit(m_page2_sizer);
            } else if (curr_selected_type == m_create_type.base_curr_printer) {
                if (m_printer_model->GetValue() == _L("Select Model")) {
                    m_filament_preset_template_sizer->Clear(true);
                    m_filament_preset.clear();
                    m_process_preset_template_sizer->Clear(true);
                    m_process_preset.clear();
                } else {
                    update_presets_list();
                }
                m_page2->SetSizerAndFit(m_page2_sizer);
            } else if (curr_selected_type == m_create_type.create_printer) {
                m_select_printer->Hide();
                m_can_not_find_vendor_combox->Show();
                m_can_not_find_vendor_text->Show();
                m_printer_info_panel->Show();
                if (m_can_not_find_vendor_combox->GetValue()) {
                    m_custom_vendor_text_ctrl->Show();
                    m_custom_model_text_ctrl->Show();
                    m_select_vendor->Hide();
                    m_select_model->Hide();
                } else {
                    m_select_vendor->Show();
                    m_select_model->Show();
                }
                m_page1->SetSizerAndFit(m_page1_sizer);
            } else if (curr_selected_type == m_create_type.create_nozzle) {
                set_current_visible_printer();
                m_select_vendor->Hide();
                m_select_model->Hide();
                m_can_not_find_vendor_combox->Hide();
                m_can_not_find_vendor_text->Hide();
                m_custom_vendor_text_ctrl->Hide();
                m_custom_model_text_ctrl->Hide();
                m_printer_info_panel->Hide();
                m_select_printer->Show();
                m_page1->SetSizerAndFit(m_page1_sizer);
            }
            this->Thaw();
        } else {
            radiobox_list[i].first->SetValue(false);
        }
    }

    update_preset_list_size();
}

void CreatePrinterPresetDialog::create_printer_page2(wxWindow *parent)
{
    this->SetBackgroundColour(*wxWHITE);

    m_page2_sizer = new wxBoxSizer(wxVERTICAL);

    m_page2_sizer->Add(create_printer_preset_item(parent), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(5));
    m_page2_sizer->Add(create_presets_item(parent), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(5));
    m_page2_sizer->Add(create_presets_template_item(parent), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(5));
    m_page2_sizer->Add(create_page2_dialog_buttons(parent), 0, wxEXPAND);

    parent->SetSizerAndFit(m_page2_sizer);
    Layout();
    Fit();

    wxGetApp().UpdateDlgDarkUI(this);
}

wxBoxSizer *CreatePrinterPresetDialog::create_printer_preset_item(wxWindow *parent)
{
    wxBoxSizer *horizontal_sizer = new wxBoxSizer(wxHORIZONTAL);

    wxBoxSizer *  optionSizer        = new wxBoxSizer(wxVERTICAL);
    wxStaticText *static_vendor_text = new wxStaticText(parent, wxID_ANY, _L("Printer Preset"), wxDefaultPosition, wxDefaultSize);
    optionSizer->Add(static_vendor_text, 0, wxEXPAND | wxALL, 0);
    optionSizer->SetMinSize(OPTION_SIZE);
    horizontal_sizer->Add(optionSizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    wxBoxSizer *  vertical_sizer = new wxBoxSizer(wxVERTICAL);
    wxStaticText *combobox_title = new wxStaticText(parent, wxID_ANY, m_create_type.base_curr_printer, wxDefaultPosition, wxDefaultSize, 0);
    combobox_title->SetFont(::Label::Body_13);
    auto size = combobox_title->GetTextExtent(m_create_type.base_curr_printer);
    combobox_title->SetMinSize(wxSize(size.x + FromDIP(4), -1));
    combobox_title->Wrap(-1);
    vertical_sizer->Add(combobox_title, 0, wxEXPAND | wxALL, 0);

    wxBoxSizer *comboBox_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_printer_vendor           = new ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, PRINTER_PRESET_VENDOR_SIZE, 0, nullptr, wxCB_READONLY);
    m_printer_vendor->SetValue(_L("Select Vendor"));
    m_printer_vendor->SetLabelColor(DEFAULT_PROMPT_TEXT_COLOUR);

    VendorMap     vendors;
    wxArrayString exist_vendor_choice = get_exist_vendor_choices(vendors);
    m_printer_vendor->Set(exist_vendor_choice);
    m_printer_vendor->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &e) {
        e.SetExtraLong(1);  // 0 means form last page,  1 means form cur combobox
        on_select_printer_model(e);
    });

    comboBox_sizer->Add(m_printer_vendor, 0, wxEXPAND, 0);
    m_printer_model = new ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, PRINTER_PRESET_MODEL_SIZE, 0, nullptr, wxCB_READONLY);
    m_printer_model->SetLabelColor(DEFAULT_PROMPT_TEXT_COLOUR);
    m_printer_model->SetValue(_L("Select Model"));

    m_printer_model->Bind(wxEVT_COMBOBOX, &CreatePrinterPresetDialog::on_preset_model_value_change, this);

    comboBox_sizer->Add(m_printer_model, 0, wxEXPAND | wxLEFT, FromDIP(10));
    vertical_sizer->Add(comboBox_sizer, 0, wxEXPAND | wxTOP, FromDIP(5));

    horizontal_sizer->Add(vertical_sizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    return horizontal_sizer;

}

wxBoxSizer *CreatePrinterPresetDialog::create_presets_item(wxWindow *parent)
{
    wxBoxSizer *horizontal_sizer = new wxBoxSizer(wxHORIZONTAL);

    wxBoxSizer *  optionSizer        = new wxBoxSizer(wxVERTICAL);
    wxStaticText *static_serial_text = new wxStaticText(parent, wxID_ANY, _L("Presets"), wxDefaultPosition, wxDefaultSize);
    optionSizer->Add(static_serial_text, 0, wxEXPAND | wxALL, 0);
    optionSizer->SetMinSize(OPTION_SIZE);
    horizontal_sizer->Add(optionSizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    wxBoxSizer *radioBoxSizer = new wxBoxSizer(wxVERTICAL);

    radioBoxSizer->Add(create_radio_item(m_create_type.base_template, parent, wxEmptyString, m_create_presets_btns), 0, wxEXPAND | wxALL, 0);
    radioBoxSizer->Add(create_radio_item(m_create_type.base_curr_printer, parent, wxEmptyString, m_create_presets_btns), 0, wxEXPAND | wxTOP, FromDIP(10));
    horizontal_sizer->Add(radioBoxSizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    return horizontal_sizer;
}

wxBoxSizer *CreatePrinterPresetDialog::create_presets_template_item(wxWindow *parent)
{
    wxBoxSizer *vertical_sizer = new wxBoxSizer(wxVERTICAL);

    m_scrolled_preset_window = new wxScrolledWindow(parent);
    m_scrolled_preset_window->SetScrollRate(5, 5);
    m_scrolled_preset_window->SetBackgroundColour(*wxWHITE);
    //m_scrolled_preset_window->SetMinSize(wxSize(FromDIP(1500), FromDIP(-1)));
    m_scrolled_preset_window->SetMaxSize(wxSize(FromDIP(1500), FromDIP(-1)));
    m_scrolled_preset_window->SetSize(wxSize(FromDIP(1500), FromDIP(-1)));
    m_scrooled_preset_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_preset_template_panel = new wxPanel(m_scrolled_preset_window);
    m_preset_template_panel->SetSize(wxSize(-1, -1));
    m_preset_template_panel->SetBackgroundColour(PRINTER_LIST_COLOUR);
    m_preset_template_panel->SetMinSize(wxSize(FromDIP(660), -1));
    m_filament_sizer              = new wxBoxSizer(wxVERTICAL);
    wxStaticText *static_filament_preset_text = new wxStaticText(m_preset_template_panel, wxID_ANY, _L("Filament Preset Template"), wxDefaultPosition, wxDefaultSize);
    m_filament_sizer->Add(static_filament_preset_text, 0, wxEXPAND | wxALL, FromDIP(5));
    m_filament_preset_panel          = new wxPanel(m_preset_template_panel);
    m_filament_preset_template_sizer = new wxGridSizer(3, FromDIP(5), FromDIP(5));
    m_filament_preset_panel->SetSize(PRESET_TEMPLATE_SIZE);
    m_filament_preset_panel->SetSizer(m_filament_preset_template_sizer);
    m_filament_sizer->Add(m_filament_preset_panel, 0, wxEXPAND | wxALL, FromDIP(5));

    wxBoxSizer *hori_filament_btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxPanel *   filament_btn_panel      = new wxPanel(m_preset_template_panel);
    filament_btn_panel->SetBackgroundColour(FILAMENT_OPTION_COLOUR);
    wxStaticText *filament_sel_all_text = new wxStaticText(filament_btn_panel, wxID_ANY, _L("Select All"), wxDefaultPosition, wxDefaultSize);
    filament_sel_all_text->SetForegroundColour(SELECT_ALL_OPTION_COLOUR);
    filament_sel_all_text->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &e) {
        select_all_preset_template(m_filament_preset);
        e.Skip();
        });
    wxStaticText *filament_desel_all_text = new wxStaticText(filament_btn_panel, wxID_ANY, _L("Deselect All"), wxDefaultPosition, wxDefaultSize);
    filament_desel_all_text->SetForegroundColour(SELECT_ALL_OPTION_COLOUR);
    filament_desel_all_text->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &e) {
        deselect_all_preset_template(m_filament_preset);
        e.Skip();
    });
    hori_filament_btn_sizer->Add(filament_sel_all_text, 0, wxEXPAND | wxALL, FromDIP(5));
    hori_filament_btn_sizer->Add(filament_desel_all_text, 0, wxEXPAND | wxALL, FromDIP(5));
    filament_btn_panel->SetSizer(hori_filament_btn_sizer);
    m_filament_sizer->Add(filament_btn_panel, 0, wxEXPAND, 0);

    wxPanel *split_panel = new wxPanel(m_preset_template_panel, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(10)));
    split_panel->SetBackgroundColour(wxColour(*wxWHITE));
    m_filament_sizer->Add(split_panel, 0, wxEXPAND, 0);

    wxStaticText *static_process_preset_text = new wxStaticText(m_preset_template_panel, wxID_ANY, _L("Process Preset Template"), wxDefaultPosition, wxDefaultSize);
    m_filament_sizer->Add(static_process_preset_text, 0, wxEXPAND | wxALL, FromDIP(5));
    m_process_preset_panel = new wxPanel(m_preset_template_panel);
    m_process_preset_panel->SetSize(PRESET_TEMPLATE_SIZE);
    m_process_preset_template_sizer = new wxGridSizer(3, FromDIP(5), FromDIP(5));
    m_process_preset_panel->SetSizer(m_process_preset_template_sizer);
    m_filament_sizer->Add(m_process_preset_panel, 0, wxEXPAND | wxALL, FromDIP(5));


    wxBoxSizer *hori_process_btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxPanel *   process_btn_panel      = new wxPanel(m_preset_template_panel);
    process_btn_panel->SetBackgroundColour(FILAMENT_OPTION_COLOUR);
    wxStaticText *process_sel_all_text = new wxStaticText(process_btn_panel, wxID_ANY, _L("Select All"), wxDefaultPosition, wxDefaultSize);
    process_sel_all_text->SetForegroundColour(SELECT_ALL_OPTION_COLOUR);
    process_sel_all_text->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &e) {
        select_all_preset_template(m_process_preset);
        e.Skip();
    });
    wxStaticText *process_desel_all_text = new wxStaticText(process_btn_panel, wxID_ANY, _L("Deselect All"), wxDefaultPosition, wxDefaultSize);
    process_desel_all_text->SetForegroundColour(SELECT_ALL_OPTION_COLOUR);
    process_desel_all_text->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &e) {
        deselect_all_preset_template(m_process_preset);
        e.Skip();
    });
    hori_process_btn_sizer->Add(process_sel_all_text, 0, wxEXPAND | wxALL, FromDIP(5));
    hori_process_btn_sizer->Add(process_desel_all_text, 0, wxEXPAND | wxALL, FromDIP(5));
    process_btn_panel->SetSizer(hori_process_btn_sizer);
    m_filament_sizer->Add(process_btn_panel, 0, wxEXPAND, 0);

    m_preset_template_panel->SetSizer(m_filament_sizer);
    m_scrooled_preset_sizer->Add(m_preset_template_panel, 0, wxEXPAND | wxALL, 0);
    m_scrolled_preset_window->SetSizerAndFit(m_scrooled_preset_sizer);
    vertical_sizer->Add(m_scrolled_preset_window, 0, wxEXPAND | wxLEFT | wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    return vertical_sizer;
}

wxWindow *CreatePrinterPresetDialog::create_page2_dialog_buttons(wxWindow *parent)
{
    auto dlg_btns = new DialogButtons(parent, {"Return", "OK", "Cancel"}, "", 1 /*left_aligned*/);

    dlg_btns->GetRETURN()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) { show_page1(); });

    auto btn_ok = dlg_btns->GetOK();
    btn_ok->SetLabel(_L("Create"));
    btn_ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {

        PresetBundle *preset_bundle = wxGetApp().preset_bundle;
        const wxString curr_selected_printer_type = curr_create_printer_type();
        const wxString curr_selected_preset_type  = curr_create_preset_type();

        // Confirm if the printer preset exists
        if (!m_printer_preset) {
            MessageDialog dlg(this, _L("You have not yet chosen which printer preset to create based on. Please choose the vendor and model of the printer"),
                              wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES | wxYES_DEFAULT | wxCENTRE);
            dlg.ShowModal();
            return;
        }

        if (!save_printable_area_config(m_printer_preset)) {
            MessageDialog dlg(this, _L("You have entered an illegal input in the printable area section on the first page. Please check before creating it."),
                              wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES | wxYES_DEFAULT | wxCENTRE);
            dlg.ShowModal();
            show_page1();
            return;
        }

        // create preset name
        std::string printer_model_name = get_custom_printer_model();
        std::string printer_nozzle_name = get_nozzle_diameter();
        // Replace comma with period in nozzle diameter for consistency
        size_t comma_pos = printer_nozzle_name.find(',');
        if (comma_pos != std::string::npos) {
            printer_nozzle_name.replace(comma_pos, 1, ".");
        }
        std::string nozzle_diameter     = printer_nozzle_name + " nozzle";
        std::string printer_preset_name = printer_model_name + " " + nozzle_diameter;

        // Confirm if the printer preset has a duplicate name
        if (!rewritten && preset_bundle->printers.find_preset(printer_preset_name)) {
            MessageDialog dlg(this,
                              _L("The printer preset you created already has a preset with the same name. Do you want to overwrite it?\n\tYes: Overwrite the printer preset with the "
                                 "same name, and filament and process presets with the same preset name will be recreated \nand filament and process presets without the same preset name will be reserve.\n\tCancel: Do not create a preset, return to the "
                                 "creation interface."),
                              wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES | wxCANCEL | wxYES_DEFAULT | wxCENTRE);
            int           res = dlg.ShowModal();
            if (res == wxID_YES) {
                rewritten = true;
            } else {
                return;
            }
        }

        // Confirm if the filament preset is exist
        bool                        filament_preset_is_exist = false;
        std::vector<Preset const *> selected_filament_presets;
        for (std::pair<::CheckBox *, Preset const *> filament_preset : m_filament_preset) {
            if (filament_preset.first->GetValue()) { selected_filament_presets.push_back(filament_preset.second); }
            if (!filament_preset_is_exist && preset_bundle->filaments.find_preset(filament_preset.second->alias + " @ " + printer_preset_name) != nullptr) {
                filament_preset_is_exist = true;
            }
        }
        if (selected_filament_presets.empty() && !filament_preset_is_exist) {
            MessageDialog dlg(this, _L("You need to select at least one filament preset."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES | wxYES_DEFAULT | wxCENTRE);
            dlg.ShowModal();
            return;
        }

        // Confirm if the process preset is exist
        bool                        process_preset_is_exist = false;
        std::vector<Preset const *> selected_process_presets;
        for (std::pair<::CheckBox *, Preset const *> process_preset : m_process_preset) {
            if (process_preset.first->GetValue()) { selected_process_presets.push_back(process_preset.second); }
            if (!process_preset_is_exist && preset_bundle->prints.find_preset(process_preset.second->alias + " @" + printer_preset_name) != nullptr) {
                process_preset_is_exist = true;
            }
        }
        if (selected_process_presets.empty() && !process_preset_is_exist) {
            MessageDialog dlg(this, _L("You need to select at least one process preset."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES | wxYES_DEFAULT | wxCENTRE);
            dlg.ShowModal();
            return;
        }

        std::vector<std::string> successful_preset_names;
        if (curr_selected_preset_type == m_create_type.base_template) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " base template";
            /******************************   clone filament preset    ********************************/
            std::vector<std::string> failures;
            if (!selected_filament_presets.empty()) {
                bool create_preset_result = preset_bundle->filaments.clone_presets_for_printer(selected_filament_presets, failures, printer_preset_name, get_filament_id, rewritten);
                if (!create_preset_result) {
                    std::string message;
                    for (const std::string &failure : failures) { message += "\t" + failure + "\n"; }
                    MessageDialog dlg(this, _L("Create filament presets failed. As follows:\n") + from_u8(message) + _L("\nDo you want to rewrite it?"),
                                      wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"),
                                      wxYES | wxYES_DEFAULT | wxCENTRE);
                    int res = dlg.ShowModal();
                    if (wxID_YES == res) {
                        create_preset_result = preset_bundle->filaments.clone_presets_for_printer(selected_filament_presets, failures, printer_preset_name,
                                                                                                              get_filament_id, true);
                    } else {
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " printer preset no same preset but filament has same preset, user cancel create the printer preset";
                        return;
                    }
                }
                // save created successfully preset name
                for (Preset const *sucessful_preset : selected_filament_presets)
                    successful_preset_names.push_back(sucessful_preset->name.substr(0, sucessful_preset->name.find(" @")) + " @" + printer_preset_name);
            }

            /******************************   clone process preset    ********************************/
            failures.clear();
            if (!selected_process_presets.empty()) {
                generate_process_presets_data(selected_process_presets, printer_nozzle_name);
                bool create_preset_result = preset_bundle->prints.clone_presets_for_printer(selected_process_presets, failures, printer_preset_name,
                                                                                                           get_filament_id, rewritten);
                if (!create_preset_result) {
                    std::string message;
                    for (const std::string &failure : failures) { message += "\t" + failure + "\n"; }
                    MessageDialog dlg(this, _L("Create process presets failed. As follows:\n") + from_u8(message) + _L("\nDo you want to rewrite it?"),
                                      wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"),
                                      wxYES | wxYES_DEFAULT | wxCENTRE);
                    int res = dlg.ShowModal();
                    if (wxID_YES == res) {
                        create_preset_result = preset_bundle->prints.clone_presets_for_printer(selected_process_presets, failures, printer_preset_name, get_filament_id, true);
                    } else {
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " printer preset no same preset but process has same preset, user cancel create the printer preset";
                        return;
                    }
                }
            }
        } else if (curr_selected_preset_type == m_create_type.base_curr_printer) { // create printer and based on printer
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " base curr printer";
            /******************************   clone filament preset    ********************************/
            std::vector<std::string> failures;
            if (!selected_filament_presets.empty()) {
                bool create_preset_result = preset_bundle->filaments.clone_presets_for_printer(selected_filament_presets, failures, printer_preset_name, get_filament_id, rewritten);
                if (!create_preset_result) {
                    std::string message;
                    for (const std::string& failure : failures) {
                        message += "\t" + failure + "\n";
                    }
                    MessageDialog dlg(this, _L("Create filament presets failed. As follows:\n") + from_u8(message) + _L("\nDo you want to rewrite it?"),
                                      wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"),
                                      wxYES | wxYES_DEFAULT | wxCENTRE);
                    int           res = dlg.ShowModal();
                    if (wxID_YES == res) {
                        create_preset_result = preset_bundle->filaments.clone_presets_for_printer(selected_filament_presets, failures, printer_preset_name, get_filament_id, true);
                    } else {
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " printer preset no same preset but filament has same preset, user cancel create the printer preset";
                        return;
                    }
                }
            }

            /******************************   clone process preset    ********************************/
            failures.clear();
            if (!selected_process_presets.empty()) {
                bool create_preset_result = preset_bundle->prints.clone_presets_for_printer(selected_process_presets, failures, printer_preset_name, get_filament_id, rewritten);
                if (!create_preset_result) {
                    std::string message;
                    for (const std::string& failure : failures) {
                        message += "\t" + failure + "\n";
                    }
                    MessageDialog dlg(this, _L("Create process presets failed. As follows:\n") + from_u8(message) + _L("\nDo you want to rewrite it?"), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES | wxYES_DEFAULT | wxCENTRE);
                    int           res = dlg.ShowModal();
                    if (wxID_YES == res) {
                        create_preset_result = preset_bundle->prints.clone_presets_for_printer(selected_process_presets, failures, printer_preset_name, get_filament_id, true);
                    } else {
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " printer preset no same preset but filament has same preset, user cancel create the printer preset";
                        return;
                    }
                }
                // save created successfully preset name
                for (Preset const *sucessful_preset : selected_filament_presets)
                    successful_preset_names.push_back(sucessful_preset->name.substr(0, sucessful_preset->name.find(" @")) + " @" + printer_preset_name);
            }
        }

        /******************************   clone printer preset     ********************************/
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ":creater printer ";
        try {
            auto printer_model = dynamic_cast<ConfigOptionString *>(m_printer_preset->config.option("printer_model", true));
            if (printer_model)
                printer_model->value = printer_model_name;

            auto printer_variant = dynamic_cast<ConfigOptionString *>(m_printer_preset->config.option("printer_variant", true));
            if (printer_variant)
                printer_variant->value = printer_nozzle_name;

            auto nozzle_diameter = dynamic_cast<ConfigOptionFloats *>(m_printer_preset->config.option("nozzle_diameter", true));
            if (nozzle_diameter) {
                std::unordered_map<std::string, float>::const_iterator iter = nozzle_diameter_map.find(printer_nozzle_name);
                if (nozzle_diameter_map.end() != iter) {
                    std::fill(nozzle_diameter->values.begin(), nozzle_diameter->values.end(), iter->second);
                } else {
                    std::fill(nozzle_diameter->values.begin(), nozzle_diameter->values.end(), my_stof(get_nozzle_diameter()));
                }
            }
        }
        catch (...) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " bisic info is not rewritten, may be printer_model, printer_variant, or nozzle_diameter";
        }
        preset_bundle->printers.save_current_preset(printer_preset_name, true, false, m_printer_preset);
        preset_bundle->update_compatible(PresetSelectCompatibleType::Always);
        EndModal(wxID_OK);

        });

    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) { EndModal(wxID_CANCEL); });

    return dlg_btns;
}

void CreatePrinterPresetDialog::show_page1()
{
    m_step_1->SetBitmap(create_scaled_bitmap("step_1", nullptr, FromDIP(20)));
    m_step_2->SetBitmap(create_scaled_bitmap("step_2_ready", nullptr, FromDIP(20)));
    m_page1->Show();
    m_page2->Hide();
    Refresh();
    Layout();
    Fit();
}

void CreatePrinterPresetDialog::show_page2()
{
    m_step_1->SetBitmap(create_scaled_bitmap("step_is_ok", nullptr, FromDIP(20)));
    m_step_2->SetBitmap(create_scaled_bitmap("step_2", nullptr, FromDIP(20)));
    m_page2->Show();
    m_page1->Hide();
    Refresh();
    Layout();
    Fit();
}

bool CreatePrinterPresetDialog::data_init()
{
    wxCommandEvent e;
    e.SetExtraLong(0);  // 0 means form last page,  1 means form cur combobox
    on_select_printer_model(e);

    auto get_nozzle_size_for_printer_model = [this](const std::string &model_name) -> size_t {
        auto iter = m_printer_name_to_preset.find(model_name);
        if (iter != m_printer_name_to_preset.end()) {
            std::shared_ptr<Preset> printer_preset = iter->second;
            if (printer_preset) {
                auto nozzle_diameter = dynamic_cast<ConfigOptionFloats *>(printer_preset->config.option("nozzle_diameter", true));
                return nozzle_diameter->values.size();
            }
        }
        return 1; // default nozzle size
    };

    size_t selected_nozzle_size = get_nozzle_size_for_printer_model(into_u8(m_select_printer->GetStringSelection()));

    bool has_set_value = false;
    for (size_t i = 0; i < m_create_presets_btns.size(); ++i) {
        auto &item = m_create_presets_btns[i];
        if (item.second == m_create_type.base_template) {
            if (selected_nozzle_size > 1) {
                item.first->Disable();
                item.first->SetValue(false);
            }
            else {
                item.first->Enable();
                if (!has_set_value) {
                    select_curr_radiobox(m_create_presets_btns, i);
                    has_set_value = true;
                }
            }
        }
        else {
            if (!has_set_value) {
                select_curr_radiobox(m_create_presets_btns, i);
                has_set_value = true;
            } else {
                item.first->SetValue(false);
            }
        }
    }

    m_page2->SetSizerAndFit(m_page2_sizer);
    return true;
}

void CreatePrinterPresetDialog::on_select_printer_model(wxCommandEvent &e)
{
    bool is_from_last_page = e.GetExtraLong() == 0; // 0 means form last page,  1 means form cur combobox
    m_printer_vendor->SetLabelColor(*wxBLACK);
    VendorMap     vendors;
    wxArrayString exist_vendor_choice  = get_exist_vendor_choices(vendors);
    std::string curr_selected_vendor = into_u8(m_printer_vendor->GetStringSelection());
    auto        iterator             = vendors.find(curr_selected_vendor);
    if (iterator != vendors.end()) {
        m_printer_preset_vendor_selected = iterator->second;
    } else {
        if (is_from_last_page) {
            m_printer_vendor->SetLabelColor(DEFAULT_PROMPT_TEXT_COLOUR);
            return;
        }

        MessageDialog dlg(this, _L("Vendor was not found, please reselect."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES_NO | wxYES_DEFAULT | wxCENTRE);
        dlg.ShowModal();
        return;
    }

    std::string nozzle_type = into_u8(m_nozzle_diameter->GetStringSelection());
    size_t      index_mm    = nozzle_type.find(" mm");
    if (std::string::npos != index_mm) { nozzle_type = nozzle_type.substr(0, index_mm); }
    float nozzle = nozzle_diameter_map[nozzle_type];
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " entry and nozzle type is: " << nozzle_type << " and nozzle is: " << nozzle;

    wxArrayString printer_preset_model = printer_preset_sort_with_nozzle_diameter(m_printer_preset_vendor_selected, nozzle);
    if (printer_preset_model.size() == 0) {
        MessageDialog dlg(this, _L("Current vendor has no models, please reselect."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES | wxYES_DEFAULT | wxCENTRE);
        dlg.ShowModal();
        return;
    }
    m_printer_model->Set(printer_preset_model);
    if (!printer_preset_model.empty()) {
        m_printer_model->SetSelection(0);
        wxCommandEvent e;
        on_preset_model_value_change(e);
        update_preset_list_size();
    }
    rewritten = false;
    e.Skip();
}

void CreatePrinterPresetDialog::set_current_visible_printer()
{
    //The entire process of creating a custom printer only needs to be done once
    if (m_printer_name_to_preset.size() > 0) return;
    PresetBundle *preset_bundle = wxGetApp().preset_bundle;
    const std::deque<Preset> &printer_presets =  preset_bundle->printers.get_presets();
    wxArrayString             printer_choice;
    m_printer_name_to_preset.clear();
    for (const Preset &printer_preset : printer_presets) {
        if (!printer_preset.is_visible) continue;
        if (preset_bundle->printers.get_preset_base(printer_preset)->name != printer_preset.name) continue;
        if (auto printer_model = dynamic_cast<ConfigOptionString*>(const_cast<Preset&>(printer_preset).config.option("printer_model", false))) {
            if (m_printer_name_to_preset.find(printer_model->value) == m_printer_name_to_preset.end()) {
                printer_choice.push_back(from_u8(printer_model->value));
                m_printer_name_to_preset[printer_model->value] = std::make_shared<Preset>(printer_preset);
            }
        }
    }
    m_select_printer->Set(printer_choice);
}

wxArrayString CreatePrinterPresetDialog::printer_preset_sort_with_nozzle_diameter(const VendorProfile &vendor_profile, float nozzle_diameter)
{
    std::vector<pair<float, std::string>> preset_sort;

    auto get_nozzle_size_for_printer_model = [this](const std::string & model_name) -> size_t {
        auto iter = m_printer_name_to_preset.find(model_name);
        if (iter != m_printer_name_to_preset.end()) {
            std::shared_ptr<Preset> printer_preset  = iter->second;
            if (printer_preset) {
                auto nozzle_diameter = dynamic_cast<ConfigOptionFloats *>(printer_preset->config.option("nozzle_diameter", true));
                return nozzle_diameter->values.size();
            }
        }
        return 1;  // default nozzle size
    };

    size_t selected_nozzle_size = get_nozzle_size_for_printer_model(into_u8(m_select_printer->GetStringSelection()));
    for (const Slic3r::VendorProfile::PrinterModel &model : vendor_profile.models) {
        std::string model_name = model.name;
        size_t      nozzle_size = get_nozzle_size_for_printer_model(model_name);
        if (nozzle_size != selected_nozzle_size)
            continue;

        for (const Slic3r::VendorProfile::PrinterVariant &variant : model.variants) {
            try {
                float variant_diameter = my_stof(variant.name);
                preset_sort.push_back(std::make_pair(variant_diameter, model_name + " @ " + variant.name + " nozzle"));
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "nozzle: " << variant_diameter << "model: " << preset_sort.back().second;
            }
            catch (...) {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " prase varient fialed and the model_name is: " << model_name;
                continue;
            }
        }
    }

    std::sort(preset_sort.begin(), preset_sort.end(), [](const std::pair<float, std::string> &a, const std::pair<float, std::string> &b) { return a.first < b.first; });

    int index_nearest_nozzle = -1;
    float nozzle_diameter_diff = 1;
    for (int i = 0; i < preset_sort.size(); ++i) {
        float curr_nozzle_diameter_diff = std::abs(nozzle_diameter - preset_sort[i].first);
        if (curr_nozzle_diameter_diff < nozzle_diameter_diff) {
            index_nearest_nozzle = i;
            nozzle_diameter_diff = curr_nozzle_diameter_diff;
            if (curr_nozzle_diameter_diff == 0) break;
        }
    }
    wxArrayString printer_preset_model_selection;
    int right_index = index_nearest_nozzle + 1;
    while (index_nearest_nozzle >= 0 || right_index < preset_sort.size()) {
        if (index_nearest_nozzle >= 0 && right_index < preset_sort.size()) {
            float left_nozzle_diff  = std::abs(nozzle_diameter - preset_sort[index_nearest_nozzle].first);
            float right_nozzle_diff = std::abs(nozzle_diameter - preset_sort[right_index].first);
            bool  left_is_little    = left_nozzle_diff < right_nozzle_diff;
            if (left_is_little) {
                printer_preset_model_selection.Add(from_u8(preset_sort[index_nearest_nozzle].second));
                index_nearest_nozzle--;
            } else {
                printer_preset_model_selection.Add(from_u8(preset_sort[right_index].second));
                right_index++;
            }
        } else if (index_nearest_nozzle >= 0) {
            printer_preset_model_selection.Add(from_u8(preset_sort[index_nearest_nozzle].second));
            index_nearest_nozzle--;
        } else if (right_index < preset_sort.size()) {
            printer_preset_model_selection.Add(from_u8(preset_sort[right_index].second));
            right_index++;
        }
    }
    return printer_preset_model_selection;
}

void CreatePrinterPresetDialog::select_all_preset_template(std::vector<std::pair<::CheckBox *, Preset *>> &preset_templates)
{
    for (std::pair<::CheckBox *, Preset const *> filament_preset : preset_templates) {
        filament_preset.first->SetValue(true);
    }
}

void CreatePrinterPresetDialog::deselect_all_preset_template(std::vector<std::pair<::CheckBox *, Preset *>> &preset_templates)
{
    for (std::pair<::CheckBox *, Preset const *> filament_preset : preset_templates) {
        filament_preset.first->SetValue(false);
    }
}

void CreatePrinterPresetDialog::update_presets_list(bool just_template)
{
    PresetBundle temp_preset_bundle;
    if (!load_system_and_user_presets_with_curr_model(temp_preset_bundle, just_template)) return;

    const std::deque<Preset> &filament_presets = temp_preset_bundle.filaments.get_presets();
    const std::deque<Preset> &process_presets  = temp_preset_bundle.prints.get_presets();

    // clear filament preset window sizer
    m_preset_template_panel->Freeze();
    clear_preset_combobox();

    // update filament preset window sizer
    for (const Preset &filament_preset : filament_presets) {
        if (filament_preset.is_compatible) {
            if (filament_preset.is_default) continue;
            Preset *temp_filament = new Preset(filament_preset);
            wxString filament_name = wxString::FromUTF8(temp_filament->name);
            m_filament_preset_template_sizer->Add(create_checkbox(m_filament_preset_panel, temp_filament, filament_name, m_filament_preset), 0,
                                                  wxEXPAND, FromDIP(5));
        }
    }

    for (const Preset &process_preset : process_presets) {
        if (process_preset.is_compatible) {
            if (process_preset.is_default) continue;

            Preset *temp_process = new Preset(process_preset);
            wxString process_name = wxString::FromUTF8(temp_process->name);
            m_process_preset_template_sizer->Add(create_checkbox(m_process_preset_panel, temp_process, process_name, m_process_preset), 0, wxEXPAND,
                                                 FromDIP(5));
        }
    }
    m_preset_template_panel->Thaw();
}

void CreatePrinterPresetDialog::clear_preset_combobox()
{
    for (std::pair<::CheckBox *, Preset *> preset : m_filament_preset) {
        if (preset.second) {
            delete preset.second;
            preset.second = nullptr;
        }
    }
    m_filament_preset.clear();
    m_filament_preset_template_sizer->Clear(true);

    for (std::pair<::CheckBox *, Preset *> preset : m_process_preset) {
        if (preset.second) {
            delete preset.second;
            preset.second = nullptr;
        }
    }
    m_process_preset.clear();
    m_process_preset_template_sizer->Clear(true);
}

bool CreatePrinterPresetDialog::save_printable_area_config(Preset *preset)
{
    const wxString      curr_selected_printer_type = curr_create_printer_type();
    DynamicPrintConfig &config                     = preset->config;

    if (curr_selected_printer_type == m_create_type.create_printer) {
        double x = 0;
        m_bed_size_x_input->GetTextCtrl()->GetValue().ToDouble(&x);
        double y = 0;
        m_bed_size_y_input->GetTextCtrl()->GetValue().ToDouble(&y);
        double dx = 0;
        m_bed_origin_x_input->GetTextCtrl()->GetValue().ToDouble(&dx);
        double dy = 0;
        m_bed_origin_y_input->GetTextCtrl()->GetValue().ToDouble(&dy);
        // range check begin
        if (x == 0 || y == 0) { return false; }
        double x0 = 0.0;
        double y0 = 0.0;
        double x1 = x;
        double y1 = y;
        if (dx >= x || dy >= y) { return false; }
        x0 -= dx;
        x1 -= dx;
        y0 -= dy;
        y1 -= dy;
        // range check end
        std::vector<Vec2d> points = {Vec2d(x0, y0), Vec2d(x1, y0), Vec2d(x1, y1), Vec2d(x0, y1)};
        config.set_key_value("printable_area", new ConfigOptionPoints(points));

        double max_print_height = 0;
        m_print_height_input->GetTextCtrl()->GetValue().ToDouble(&max_print_height);
        config.set("printable_height", max_print_height);

        Utils::slash_to_back_slash(m_custom_texture);
        Utils::slash_to_back_slash(m_custom_model);
        config.set("bed_custom_model", m_custom_model);
        config.set("bed_custom_texture", m_custom_texture);
    } else if(m_create_type.create_nozzle){
        std::string selected_printer_preset_name = into_u8(m_select_printer->GetStringSelection());
        std::unordered_map<std::string, std::shared_ptr<Preset>>::iterator itor = m_printer_name_to_preset.find(selected_printer_preset_name);
        assert(m_printer_name_to_preset.end() != itor);
        if (m_printer_name_to_preset.end() != itor) {
            std::shared_ptr<Preset> printer_preset = itor->second;
            std::vector<std::string> keys = {"printable_area", "printable_height", "bed_custom_model", "bed_custom_texture"};
            config.apply_only(printer_preset->config, keys, true);
        }
    }
    return true;
}

bool CreatePrinterPresetDialog::check_printable_area() {
    double x = 0;
    m_bed_size_x_input->GetTextCtrl()->GetValue().ToDouble(&x);
    double y = 0;
    m_bed_size_y_input->GetTextCtrl()->GetValue().ToDouble(&y);
    double dx = 0;
    m_bed_origin_x_input->GetTextCtrl()->GetValue().ToDouble(&dx);
    double dy = 0;
    m_bed_origin_y_input->GetTextCtrl()->GetValue().ToDouble(&dy);
    // range check begin
    if (x == 0 || y == 0) {
        return false;
    }
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = x;
    double y1 = y;
    if (dx >= x || dy >= y) {
        return false;
    }
    return true;
}

bool CreatePrinterPresetDialog::validate_input_valid()
{
    const wxString curr_selected_printer_type = curr_create_printer_type();
    if (curr_selected_printer_type == m_create_type.create_printer) {
        std::string vendor_name = get_printer_vendor();
        std::string model_name  = get_printer_model();
        if ((vendor_name.empty() || model_name.empty())) {
            MessageDialog dlg(this, _L("You have not selected the vendor and model or entered the custom vendor and model."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"),
                              wxYES | wxYES_DEFAULT | wxCENTRE);
            dlg.ShowModal();
            return false;
        }

        vendor_name = remove_special_key(vendor_name);
        model_name  = remove_special_key(model_name);
        if (vendor_name.empty() || model_name.empty()) {
            MessageDialog dlg(this, _L("There may be escape characters in the custom printer vendor or model. Please delete and re-enter."),
                              wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES | wxYES_DEFAULT | wxCENTRE);
            dlg.ShowModal();
            return false;
        }
        boost::algorithm::trim(vendor_name);
        boost::algorithm::trim(model_name);
        if (vendor_name.empty() || model_name.empty()) {
            MessageDialog dlg(this, _L("All inputs in the custom printer vendor or model are spaces. Please re-enter."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"),
                              wxYES | wxYES_DEFAULT | wxCENTRE);
            dlg.ShowModal();
            return false;
        }

        if (check_printable_area() == false) {
            MessageDialog dlg(this, _L("Please check bed printable shape and origin input."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES | wxYES_DEFAULT | wxCENTRE);
            dlg.ShowModal();
            return false;
        }
    } else if (curr_selected_printer_type == m_create_type.create_nozzle) {
        wxString printer_name = m_select_printer->GetStringSelection();
        if (printer_name.empty()) {
            MessageDialog dlg(this, _L("You have not yet selected the printer to replace the nozzle, please choose."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"),
                              wxYES | wxYES_DEFAULT | wxCENTRE);
            dlg.ShowModal();
            return false;
        }
    }

    std::string nozzle_diameter;
    if (m_can_not_find_nozzle_checkbox->GetValue()) {
        nozzle_diameter = into_u8(m_custom_nozzle_diameter_ctrl->GetValue());
    } else {
        nozzle_diameter = into_u8(m_nozzle_diameter->GetStringSelection());
        size_t index_mm = nozzle_diameter.find(" mm");
        if (std::string::npos != index_mm) { nozzle_diameter = nozzle_diameter.substr(0, index_mm); }
    }
    float nozzle_dia = 0;
    try {
        nozzle_dia = my_stof(nozzle_diameter);
    } catch (...) { }
    if (nozzle_dia == 0) {
        MessageDialog dlg(this, _L("The entered nozzle diameter is invalid, please re-enter:\n"), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"),
                          wxOK | wxYES_DEFAULT | wxCENTRE);
        int           res = dlg.ShowModal();
        return false;
    }

    std::string custom_printer_name = get_custom_printer_name();

    if (auto preset = wxGetApp().preset_bundle->printers.find_preset(custom_printer_name)) {
        if (preset->is_system) {
            MessageDialog dlg(this, _L("The system preset does not allow creation. \nPlease re-enter the printer model or nozzle diameter."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"),
                              wxYES | wxYES_DEFAULT | wxCENTRE);
            dlg.ShowModal();
            return false;
        }
    }

    return true;
}

void CreatePrinterPresetDialog::on_preset_model_value_change(wxCommandEvent &e)
{
    m_printer_model->SetLabelColor(*wxBLACK);
    if (m_printer_preset_vendor_selected.models.empty()) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " selected vendor has no models, and the vendor is: " << m_printer_preset_vendor_selected.id;
        return;
    }

    wxString curr_selected_preset_type = curr_create_preset_type();
    if (curr_selected_preset_type == m_create_type.base_curr_printer) {
        update_presets_list();
    } else if (curr_selected_preset_type == m_create_type.base_template) {
        update_presets_list(true);
    }
    rewritten = false;

    update_preset_list_size();

    e.Skip();
}

wxString CreatePrinterPresetDialog::curr_create_preset_type() const
{
    wxString curr_selected_preset_type;
    for (const std::pair<RadioBox *, wxString> &presets_radio : m_create_presets_btns) {
        if (presets_radio.first->GetValue()) {
            curr_selected_preset_type = presets_radio.second;
        }
    }
    return curr_selected_preset_type;
}

wxString CreatePrinterPresetDialog::curr_create_printer_type() const
{
    wxString curr_selected_printer_type;
    for (const std::pair<RadioBox *, wxString> &printer_radio : m_create_type_btns) {
        if (printer_radio.first->GetValue()) { curr_selected_printer_type = printer_radio.second; }
    }
    return curr_selected_printer_type;
}

CreatePresetSuccessfulDialog::CreatePresetSuccessfulDialog(wxWindow *parent, const SuccessType &create_success_type)
    : DPIDialog(parent ? parent : nullptr, wxID_ANY, PRINTER == create_success_type ? _L("Printer Created Successfully") : _L("Filament Created Successfully"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    this->SetBackgroundColour(*wxWHITE);
    this->SetSize(wxSize(FromDIP(450), FromDIP(200)));

    wxBoxSizer *m_main_sizer = new wxBoxSizer(wxVERTICAL);
    // top line
    auto m_line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    m_line_top->SetBackgroundColour(wxColour(0xA6, 0xa9, 0xAA));
    m_main_sizer->Add(m_line_top, 0, wxEXPAND, 0);
    m_main_sizer->Add(0, 0, 0, wxTOP, FromDIP(5));

    wxBoxSizer *horizontal_sizer = new wxBoxSizer(wxHORIZONTAL);
    horizontal_sizer->Add(0, 0, 0, wxLEFT, FromDIP(30));

    wxBoxSizer *success_bitmap_sizer = new wxBoxSizer(wxVERTICAL);
    wxStaticBitmap *success_bitmap       = new wxStaticBitmap(this,wxID_ANY, create_scaled_bitmap("create_success", nullptr, FromDIP(24)));
    success_bitmap_sizer->Add(success_bitmap, 0, wxEXPAND, 0);
    horizontal_sizer->Add(success_bitmap_sizer, 0, wxEXPAND | wxALL, FromDIP(5));

    wxBoxSizer *success_text_sizer = new wxBoxSizer(wxVERTICAL);
    wxStaticText *success_text = nullptr;
    wxStaticText *next_step_text = nullptr;
    bool          sync_user_preset_need_enabled = wxGetApp().getAgent() && wxGetApp().app_config->get("sync_user_preset") == "false";
    switch (create_success_type) {
    case PRINTER:
        success_text = new wxStaticText(this, wxID_ANY, _L("Printer Created"));
        next_step_text = new wxStaticText(this, wxID_ANY, _L("Please go to printer settings to edit your presets"));
        break;
    case FILAMENT:
        success_text = new wxStaticText(this, wxID_ANY, _L("Filament Created"));
        wxString prompt_text = _L("Please go to filament setting to edit your presets if you need.\nPlease note that nozzle temperature, hot bed temperature, and maximum "
                                  "volumetric speed has a significant impact on printing quality. Please set them carefully.");
        wxString sync_text = sync_user_preset_need_enabled ? _L("\n\nOrca has detected that your user presets synchronization function is not enabled, "
                                                                "which may result in unsuccessful Filament settings on the Device page.\n"
                                                                "Click \"Sync user presets\" to enable the synchronization function.") : "";
        next_step_text = new wxStaticText(this, wxID_ANY, prompt_text + sync_text);
        break;
    }
    success_text->SetFont(Label::Head_18);
    success_text_sizer->Add(success_text, 0, wxEXPAND, 0);
    success_text_sizer->Add(next_step_text, 0, wxEXPAND | wxTOP, FromDIP(5));
    horizontal_sizer->Add(success_text_sizer, 0, wxEXPAND | wxALL, FromDIP(5));
    horizontal_sizer->Add(0, 0, 0, wxLEFT, FromDIP(60));

    m_main_sizer->Add(horizontal_sizer, 0, wxALL, FromDIP(5));

    bool is_cancel_needed = PRINTER == create_success_type || sync_user_preset_need_enabled;

    auto dlg_btns = new DialogButtons(this, is_cancel_needed ? std::vector<wxString>{"OK", "Cancel"} : std::vector<wxString>{"OK"});

    if      (create_success_type == PRINTER) 
        dlg_btns->GetOK()->SetLabel(_L("Printer Setting"));
    else if (create_success_type == FILAMENT && sync_user_preset_need_enabled)
        dlg_btns->GetOK()->SetLabel(_L("Sync user presets"));

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this, sync_user_preset_need_enabled](wxCommandEvent &e) {
        if (sync_user_preset_need_enabled) {
            wxGetApp().app_config->set("sync_user_preset", "true");
            wxGetApp().start_sync_user_preset();
        }
        EndModal(wxID_OK);
    });

    if (is_cancel_needed)
        dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) { EndModal(wxID_CANCEL); });

    m_main_sizer->Add(dlg_btns, 0, wxEXPAND);

    SetSizer(m_main_sizer);
    Layout();
    Fit();
    wxGetApp().UpdateDlgDarkUI(this);
}

CreatePresetSuccessfulDialog::~CreatePresetSuccessfulDialog() {}

void CreatePresetSuccessfulDialog::on_dpi_changed(const wxRect &suggested_rect) {
    Layout();
}

ExportConfigsDialog::ExportConfigsDialog(wxWindow *parent)
    : DPIDialog(parent ? parent : nullptr, wxID_ANY, _L("Export Preset Bundle"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    m_exprot_type.preset_bundle   = _L("Printer config bundle(.orca_printer)");
    m_exprot_type.filament_bundle = _L("Filament bundle(.orca_filament)");
    m_exprot_type.printer_preset  = _L("Printer presets(.zip)");
    m_exprot_type.filament_preset = _L("Filament presets(.zip)");
    m_exprot_type.process_preset  = _L("Process presets(.zip)");

    this->SetBackgroundColour(*wxWHITE);
    this->SetSize(wxSize(FromDIP(600), FromDIP(600)));

    m_main_sizer = new wxBoxSizer(wxVERTICAL);
    // top line
    auto m_line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    m_line_top->SetBackgroundColour(wxColour(0xA6, 0xa9, 0xAA));
    m_main_sizer->Add(m_line_top, 0, wxEXPAND, 0);
    m_main_sizer->Add(0, 0, 0, wxTOP, FromDIP(5));

    m_main_sizer->Add(create_export_config_item(this), 0, wxEXPAND | wxALL, FromDIP(5));
    m_main_sizer->Add(create_select_printer(this), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(5));
    m_main_sizer->Add(create_dialog_buttons(this), 0, wxEXPAND);

    data_init();

    this->SetSizer(m_main_sizer);

    this->Layout();
    this->Fit();

    wxGetApp().UpdateDlgDarkUI(this);

}

ExportConfigsDialog::~ExportConfigsDialog()
{
    for (std::pair<std::string, Preset *> printer_preset : m_printer_presets) {
        Preset *preset = printer_preset.second;
        if (preset) {
            delete preset;
            preset = nullptr;
        }
    }
    for (std::pair<std::string, std::vector<Preset *>> filament_presets : m_filament_presets) {
        for (Preset* preset : filament_presets.second) {
            if (preset) {
                delete preset;
                preset = nullptr;
            }
        }
    }
    for (std::pair<std::string, std::vector<Preset *>> filament_presets : m_process_presets) {
        for (Preset *preset : filament_presets.second) {
            if (preset) {
                delete preset;
                preset = nullptr;
            }
        }
    }
    for (std::pair<std::string, std::vector<std::pair<std::string, Preset *>>> filament_preset : m_filament_name_to_presets) {
        for (std::pair<std::string, Preset*> printer_name_preset : filament_preset.second) {
            Preset *preset = printer_name_preset.second;
            if (preset) {
                delete preset;
                preset = nullptr;
            }
        }
    }

    // Delete the Temp folder
    boost::filesystem::path temp_folder(data_dir() + "/" + PRESET_USER_DIR + "/" + "Temp");
    if (boost::filesystem::exists(temp_folder)) boost::filesystem::remove_all(temp_folder);
}

void ExportConfigsDialog::on_dpi_changed(const wxRect &suggested_rect) {
    Layout();
}

void ExportConfigsDialog::show_export_result(const ExportCase &export_case)
{
    MessageDialog *msg_dlg = nullptr;
    switch (export_case) {
    case ExportCase::INITIALIZE_FAIL:
        msg_dlg = new MessageDialog(this, _L("initialize fail"), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES | wxYES_DEFAULT | wxCENTRE);
        break;
    case ExportCase::ADD_FILE_FAIL:
        msg_dlg = new MessageDialog(this, _L("add file fail"), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES | wxYES_DEFAULT | wxCENTRE);
        break;
    case ExportCase::ADD_BUNDLE_STRUCTURE_FAIL:
        msg_dlg = new MessageDialog(this, _L("add bundle structure file fail"), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES | wxYES_DEFAULT | wxCENTRE);
        break;
    case ExportCase::FINALIZE_FAIL:
        msg_dlg = new MessageDialog(this, _L("finalize fail"), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES | wxYES_DEFAULT | wxCENTRE);
        break;
    case ExportCase::OPEN_ZIP_WRITTEN_FILE:
        msg_dlg = new MessageDialog(this, _L("open zip written fail"), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES | wxYES_DEFAULT | wxCENTRE);
        break;
    case ExportCase::EXPORT_SUCCESS:
        msg_dlg = new MessageDialog(this, _L("Export successful"), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES | wxYES_DEFAULT | wxCENTRE);
        break;
    }

    if (msg_dlg) {
        msg_dlg->ShowModal();
        delete msg_dlg;
        msg_dlg = nullptr;
    }
}

bool ExportConfigsDialog::has_check_box_selected()
{
    for (std::pair<::CheckBox *, Preset *> checkbox_preset : m_preset) {
        if (checkbox_preset.first->GetValue()) return true;
    }
    for (std::pair<::CheckBox *, std::string> checkbox_filament_name : m_printer_name) {
        if (checkbox_filament_name.first->GetValue()) return true;
    }

    return false;
}

bool ExportConfigsDialog::earse_preset_fields_for_safe(Preset *preset)
{
    if (preset->type != Preset::Type::TYPE_PRINTER) return true;

    boost::filesystem::path file_path(data_dir() + "/" + PRESET_USER_DIR + "/" + "Temp" + "/" + (preset->name + ".json"));
    preset->file = file_path.make_preferred().string();

    DynamicPrintConfig &config = preset->config;
    config.erase("print_host");
    config.erase("print_host_webui");
    config.erase("printhost_apikey");
    config.erase("printhost_cafile");
    config.erase("printhost_user");
    config.erase("printhost_password");
    config.erase("printhost_port");

    preset->save(nullptr);
    return true;
}

std::string ExportConfigsDialog::initial_file_path(const wxString &path, const std::string &sub_file_path)
{
    std::string             export_path         = into_u8(path);
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "initial file path and path is:" << export_path << " and sub path is: " << sub_file_path;
    boost::filesystem::path printer_export_path = (boost::filesystem::path(export_path) / sub_file_path).make_preferred();
    if (!boost::filesystem::exists(printer_export_path)) {
        boost::filesystem::create_directories(printer_export_path);
        export_path = printer_export_path.string();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "Same path exists, delete and rebuild, and path is: " << export_path;
    }
    return export_path;
}

std::string ExportConfigsDialog::initial_file_name(const wxString &path, const std::string file_name)
{
    std::string             export_path         = into_u8(path);
    boost::filesystem::path printer_export_path = (boost::filesystem::path(export_path) / file_name).make_preferred();
    if (boost::filesystem::exists(printer_export_path)) {
        MessageDialog dlg(this, wxString::Format(_L("The '%s' folder already exists in the current directory. Do you want to clear it and rebuild it.\nIf not, a time suffix will be "
                             "added, and you can modify the name after creation."), file_name), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES_NO | wxYES_DEFAULT | wxCENTRE);
        int           res = dlg.ShowModal();
        if (wxID_YES == res) {
            try {
                boost::filesystem::remove_all(printer_export_path);
            }
            catch(...) {
                MessageDialog dlg(this,
                                  wxString::Format(_L("The file: %s\nmay have been opened by another program.\nPlease close it and try again."),
                                                      encode_path(printer_export_path.string().c_str())),
                                  wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES | wxYES_DEFAULT | wxCENTRE);
                dlg.ShowModal();
                return "initial_failed";
            }
            export_path = printer_export_path.string();
        } else if (wxID_NO == res) {
            export_path = printer_export_path.string();
            export_path = export_path.substr(0, export_path.find(".zip"));
            std::string              export_path_with_time;
            boost::filesystem::path *printer_export_path_with_time = nullptr;
            do {
                if (printer_export_path_with_time) {
                    delete printer_export_path_with_time;
                    printer_export_path_with_time = nullptr;
                }
                export_path_with_time         = export_path + " " + get_curr_time() + ".zip";
                printer_export_path_with_time = new boost::filesystem::path(export_path_with_time);
            } while (boost::filesystem::exists(*printer_export_path_with_time));
            export_path = export_path_with_time;
            if (printer_export_path_with_time) {
                delete printer_export_path_with_time;
                printer_export_path_with_time = nullptr;
            }
        } else {
            return "";
        }
    } else {
        export_path = printer_export_path.string();
    }
    return export_path;
}

wxBoxSizer *ExportConfigsDialog::create_export_config_item(wxWindow *parent)
{
    wxBoxSizer *horizontal_sizer = new wxBoxSizer(wxHORIZONTAL);

    wxBoxSizer *  optionSizer        = new wxBoxSizer(wxVERTICAL);
    wxStaticText *static_serial_text = new wxStaticText(parent, wxID_ANY, _L("Presets"), wxDefaultPosition, wxDefaultSize);
    optionSizer->Add(static_serial_text, 0, wxEXPAND | wxALL, 0);
    optionSizer->SetMinSize(OPTION_SIZE);
    horizontal_sizer->Add(optionSizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    wxBoxSizer *radioBoxSizer = new wxBoxSizer(wxVERTICAL);

    radioBoxSizer->Add(create_radio_item(m_exprot_type.preset_bundle, parent, wxEmptyString, m_export_type_btns), 0, wxEXPAND | wxALL, 0);
    radioBoxSizer->Add(0, 0, 0, wxTOP, FromDIP(6));
    wxStaticText *static_export_printer_preset_bundle_text = new wxStaticText(parent, wxID_ANY, _L("Printer and all the filament&&process presets that belongs to the printer.\n"
                                                                                                   "Can be shared with others."), wxDefaultPosition, wxDefaultSize);
    static_export_printer_preset_bundle_text->SetFont(Label::Body_12);
    static_export_printer_preset_bundle_text->SetForegroundColour(wxColour("#6B6B6B"));
    radioBoxSizer->Add(static_export_printer_preset_bundle_text, 0, wxEXPAND | wxLEFT, FromDIP(22));
    radioBoxSizer->Add(create_radio_item(m_exprot_type.filament_bundle, parent, wxEmptyString, m_export_type_btns), 0, wxEXPAND | wxTOP, FromDIP(10));
    wxStaticText *static_export_filament_preset_bundle_text = new wxStaticText(parent, wxID_ANY, _L("User's filament preset set.\nCan be shared with others."),
                                                                                                    wxDefaultPosition, wxDefaultSize);
    static_export_filament_preset_bundle_text->SetFont(Label::Body_12);
    static_export_filament_preset_bundle_text->SetForegroundColour(wxColour("#6B6B6B"));
    radioBoxSizer->Add(static_export_filament_preset_bundle_text, 0, wxEXPAND | wxLEFT, FromDIP(22));
    radioBoxSizer->Add(create_radio_item(m_exprot_type.printer_preset, parent, wxEmptyString, m_export_type_btns), 0, wxEXPAND | wxTOP, FromDIP(10));
    radioBoxSizer->Add(create_radio_item(m_exprot_type.filament_preset, parent, wxEmptyString, m_export_type_btns), 0, wxEXPAND | wxTOP, FromDIP(10));
    radioBoxSizer->Add(create_radio_item(m_exprot_type.process_preset, parent, wxEmptyString, m_export_type_btns), 0, wxEXPAND | wxTOP, FromDIP(10));
    horizontal_sizer->Add(radioBoxSizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    return horizontal_sizer;
}

wxBoxSizer *ExportConfigsDialog::create_radio_item(wxString title, wxWindow *parent, wxString tooltip, std::vector<std::pair<RadioBox *, wxString>> &radiobox_list)
{
    wxBoxSizer *horizontal_sizer = new wxBoxSizer(wxHORIZONTAL);
    RadioBox *  radiobox         = new RadioBox(parent);
    horizontal_sizer->Add(radiobox, 0, wxEXPAND | wxALL, 0);
    horizontal_sizer->Add(0, 0, 0, wxEXPAND | wxLEFT, FromDIP(5));
    radiobox_list.push_back(std::make_pair(radiobox, title));
    int btn_idx = radiobox_list.size() - 1;
    radiobox->Bind(wxEVT_LEFT_DOWN, [this, &radiobox_list, btn_idx](wxMouseEvent &e) {
        select_curr_radiobox(radiobox_list, btn_idx);
        });

    wxStaticText *text = new wxStaticText(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize);
    text->Bind(wxEVT_LEFT_DOWN, [this, &radiobox_list, btn_idx](wxMouseEvent &e) {
        select_curr_radiobox(radiobox_list, btn_idx);
        });
    horizontal_sizer->Add(text, 0, wxEXPAND | wxLEFT, 0);

    radiobox->SetToolTip(tooltip);
    text->SetToolTip(tooltip);
    return horizontal_sizer;
}

mz_bool ExportConfigsDialog::initial_zip_archive(mz_zip_archive &zip_archive, const std::string &file_path)
{
    mz_zip_zero_struct(&zip_archive);
    mz_bool status;

    // Initialize the ZIP file to write to the structure, using memory storage

    std::string export_dir = encode_path(file_path.c_str());
    status                 = mz_zip_writer_init_file(&zip_archive, export_dir.c_str(), 0);
    return status;
}

ExportConfigsDialog::ExportCase ExportConfigsDialog::save_zip_archive_to_file(mz_zip_archive &zip_archive)
{
    // Complete writing of ZIP file
    mz_bool status = mz_zip_writer_finalize_archive(&zip_archive);
    if (MZ_FALSE == status) {
        BOOST_LOG_TRIVIAL(info) << "Failed to finalize ZIP archive";
        mz_zip_writer_end(&zip_archive);
        return ExportCase::FINALIZE_FAIL;
    }

    // Release ZIP file to write structure and related resources
    mz_zip_writer_end(&zip_archive);

    return ExportCase::CASE_COUNT;
}

ExportConfigsDialog::ExportCase ExportConfigsDialog::save_presets_to_zip(const std::string &export_file, const std::vector<std::pair<std::string, std::string>> &config_paths)
{
    mz_zip_archive zip_archive;
    mz_bool        status = initial_zip_archive(zip_archive, export_file);

    if (MZ_FALSE == status) {
        BOOST_LOG_TRIVIAL(info) << "Failed to initialize ZIP archive";
        return ExportCase::INITIALIZE_FAIL;
    }

    for (std::pair<std::string, std::string> config_path : config_paths) {
        std::string preset_name = config_path.first;

        // Add a file to the ZIP file
        status = mz_zip_writer_add_file(&zip_archive, (preset_name).c_str(), encode_path(config_path.second.c_str()).c_str(), NULL, 0, MZ_DEFAULT_COMPRESSION);
        // status = mz_zip_writer_add_mem(&zip_archive, ("printer/" + printer_preset->name + ".json").c_str(), json_contents, strlen(json_contents), MZ_DEFAULT_COMPRESSION);
        if (MZ_FALSE == status) {
            BOOST_LOG_TRIVIAL(info) << preset_name << " Filament preset failed to add file to ZIP archive";
            mz_zip_writer_end(&zip_archive);
            return ExportCase::ADD_FILE_FAIL;
        }
        BOOST_LOG_TRIVIAL(info) << "Printer preset json add successful: " << preset_name;
    }
    return save_zip_archive_to_file(zip_archive);
}

void ExportConfigsDialog::select_curr_radiobox(std::vector<std::pair<RadioBox *, wxString>> &radiobox_list, int btn_idx)
{
    int len = radiobox_list.size();
    for (int i = 0; i < len; ++i) {
        if (i == btn_idx) {
            radiobox_list[i].first->SetValue(true);
            const wxString &export_type = radiobox_list[i].second;
            m_preset_sizer->Clear(true);
            m_printer_name.clear();
            m_preset.clear();
            PresetBundle *preset_bundle = wxGetApp().preset_bundle;
            this->Freeze();
            if (export_type == m_exprot_type.preset_bundle) {
                for (std::pair<std::string, Preset *> preset : m_printer_presets) {
                    std::string preset_name = preset.first;
                    //printer preset mast have user's filament or process preset or printer preset is user preset
                    if (m_filament_presets.find(preset_name) == m_filament_presets.end() && m_process_presets.find(preset_name) == m_process_presets.end() && preset.second->is_system) continue;
                    wxString printer_name = wxString::FromUTF8(preset_name);
                    m_preset_sizer->Add(create_checkbox(m_presets_window, preset.second, printer_name, m_preset), 0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, FromDIP(5));
                }
                m_serial_text->SetLabel(_L("Only display printer names with changes to printer, filament, and process presets."));
            }else if (export_type == m_exprot_type.filament_bundle) {
                for (std::pair<std::string, std::vector<std::pair<std::string, Preset*>>> filament_name_to_preset : m_filament_name_to_presets) {
                    if (filament_name_to_preset.second.empty()) continue;
                    wxString filament_name = wxString::FromUTF8(filament_name_to_preset.first);
                    m_preset_sizer->Add(create_checkbox(m_presets_window, filament_name, m_printer_name), 0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, FromDIP(5));
                }
                m_serial_text->SetLabel(_L("Only display the filament names with changes to filament presets."));
            } else if (export_type == m_exprot_type.printer_preset) {
                for (std::pair<std::string, Preset *> preset : m_printer_presets) {
                    if (preset.second->is_system) continue;
                    wxString printer_name = wxString::FromUTF8(preset.first);
                    m_preset_sizer->Add(create_checkbox(m_presets_window, preset.second, printer_name, m_preset), 0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT,
                                        FromDIP(5));
                }
                m_serial_text->SetLabel(_L("Only printer names with user printer presets will be displayed, and each preset you choose will be exported as a zip."));
            } else if (export_type == m_exprot_type.filament_preset) {
                for (std::pair<std::string, std::vector<std::pair<std::string, Preset *>>> filament_name_to_preset : m_filament_name_to_presets) {
                    if (filament_name_to_preset.second.empty()) continue;
                    wxString filament_name = wxString::FromUTF8(filament_name_to_preset.first);
                    m_preset_sizer->Add(create_checkbox(m_presets_window, filament_name, m_printer_name), 0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, FromDIP(5));
                }
                m_serial_text->SetLabel(_L("Only the filament names with user filament presets will be displayed, \nand all user filament presets in each filament name you select will be exported as a zip."));
            } else if (export_type == m_exprot_type.process_preset) {
                for (std::pair<std::string, std::vector<Preset *>> presets : m_process_presets) {
                    Preset *      printer_preset = preset_bundle->printers.find_preset(presets.first, false);
                    if (!printer_preset) continue;
                    if (!printer_preset->is_system) continue;
                    if (preset_bundle->printers.get_preset_base(*printer_preset) != printer_preset) continue;
                    for (Preset *preset : presets.second) {
                        if (!preset->is_system) {
                            wxString printer_name = wxString::FromUTF8(presets.first);
                            m_preset_sizer->Add(create_checkbox(m_presets_window, printer_name, m_printer_name), 0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, FromDIP(5));
                            break;
                        }
                    }

                }
                m_serial_text->SetLabel(_L("Only printer names with changed process presets will be displayed, \nand all user process presets in each printer name you select will be exported as a zip."));
            }
            //m_presets_window->SetSizerAndFit(m_preset_sizer);
            m_presets_window->Layout();
            m_presets_window->Fit();
            int width  = m_presets_window->GetSize().GetWidth();
            int height = m_presets_window->GetSize().GetHeight();
            m_scrolled_preset_window->SetMinSize(wxSize(std::min(1200, width), std::min(600, height)));
            m_scrolled_preset_window->SetMaxSize(wxSize(std::min(1200, width), std::min(600, height)));
            m_scrolled_preset_window->SetSize(wxSize(std::min(1200, width), std::min(600, height)));
            this->SetSizerAndFit(m_main_sizer);
            Layout();
            Fit();
            Refresh();
            adjust_dialog_in_screen(this);
            this->Thaw();
        } else {
            radiobox_list[i].first->SetValue(false);
        }
    }
}

ExportConfigsDialog::ExportCase ExportConfigsDialog::archive_preset_bundle_to_file(const wxString &path)
{
    std::string export_path = initial_file_path(path, "");
    if (export_path.empty() || "initial_failed" == export_path) return ExportCase::EXPORT_CANCEL;
    BOOST_LOG_TRIVIAL(info) << "Export printer preset bundle";

    for (std::pair<::CheckBox *, Preset *> checkbox_preset : m_preset) {
        if (checkbox_preset.first->GetValue()) {
            Preset *printer_preset = checkbox_preset.second;
            std::string printer_preset_name_ = printer_preset->name;

            json          bundle_structure;
            NetworkAgent *agent = wxGetApp().getAgent();
            std::string   clock = get_curr_timestmp();
            if (agent) {
                bundle_structure["version"]   = agent->get_version();
                bundle_structure["bundle_id"] = agent->get_user_id() + "_" + printer_preset_name_ + "_" + clock;
            } else {
                bundle_structure["version"]   = "";
                bundle_structure["bundle_id"] = "offline_" + printer_preset_name_ + "_" + clock;
            }
            bundle_structure["bundle_type"] = "printer config bundle";
            bundle_structure["printer_preset_name"] = printer_preset_name_;
            json printer_config   = json::array();
            json filament_configs = json::array();
            json process_configs  = json::array();

            mz_zip_archive zip_archive;
            mz_bool        status = initial_zip_archive(zip_archive, export_path + "/" + printer_preset->name + ".orca_printer");
            if (MZ_FALSE == status) {
                BOOST_LOG_TRIVIAL(info) << "Failed to initialize ZIP archive";
                return ExportCase::INITIALIZE_FAIL;
            }

            boost::filesystem::path printer_file_path      = boost::filesystem::path(printer_preset->file);
            std::string             preset_path       = printer_file_path.make_preferred().string();
            if (preset_path.empty()) {
                BOOST_LOG_TRIVIAL(info) << "Export printer preset: " << printer_preset->name << " skip because of the preset file path is empty.";
                continue;
            }

            // Add a file to the ZIP file
            std::string printer_config_file_name = "printer/" + printer_file_path.filename().string();
            status = mz_zip_writer_add_file(&zip_archive, printer_config_file_name.c_str(), encode_path(preset_path.c_str()).c_str(), NULL, 0, MZ_DEFAULT_COMPRESSION);
            //status = mz_zip_writer_add_mem(&zip_archive, ("printer/" + printer_preset->name + ".json").c_str(), json_contents, strlen(json_contents), MZ_DEFAULT_COMPRESSION);
            if (MZ_FALSE == status) {
                BOOST_LOG_TRIVIAL(info) << printer_preset->name << " Failed to add file to ZIP archive";
                mz_zip_writer_end(&zip_archive);
                return ExportCase::ADD_FILE_FAIL;
            }
            printer_config.push_back(printer_config_file_name);
            BOOST_LOG_TRIVIAL(info) << "Printer preset json add successful: " << printer_preset->name;

            const std::string printer_preset_name = printer_preset->name;
            std::unordered_map<std::string, std::vector<Preset *>>::iterator iter = m_filament_presets.find(printer_preset_name);
            if (m_filament_presets.end() != iter) {
                for (Preset *preset : iter->second) {
                    boost::filesystem::path filament_file_path   = boost::filesystem::path(preset->file);
                    std::string             filament_preset_path = filament_file_path.make_preferred().string();
                    if (filament_preset_path.empty()) {
                        BOOST_LOG_TRIVIAL(info) << "Export filament preset: " << preset->name << " skip because of the preset file path is empty.";
                        continue;
                    }

                    std::string filament_config_file_name = "filament/" + filament_file_path.filename().string();
                    status = mz_zip_writer_add_file(&zip_archive, filament_config_file_name.c_str(), encode_path(filament_preset_path.c_str()).c_str(), NULL, 0, MZ_DEFAULT_COMPRESSION);
                    if (MZ_FALSE == status) {
                        BOOST_LOG_TRIVIAL(info) << preset->name << " Failed to add file to ZIP archive";
                        mz_zip_writer_end(&zip_archive);
                        return ExportCase::ADD_FILE_FAIL;
                    }
                    filament_configs.push_back(filament_config_file_name);
                    BOOST_LOG_TRIVIAL(info) << "Filament preset json add successful.";
                }
            }

            iter = m_process_presets.find(printer_preset_name);
            if (m_process_presets.end() != iter) {
                for (Preset *preset : iter->second) {
                    boost::filesystem::path process_file_path   = boost::filesystem::path(preset->file);
                    std::string             process_preset_path = process_file_path.make_preferred().string();
                    if (process_preset_path.empty()) {
                        BOOST_LOG_TRIVIAL(info) << "Export process preset: " << preset->name << " skip because of the preset file path is empty.";
                        continue;
                    }

                    std::string process_config_file_name = "process/" + process_file_path.filename().string();
                    status = mz_zip_writer_add_file(&zip_archive, process_config_file_name.c_str(), encode_path(process_preset_path.c_str()).c_str(), NULL, 0, MZ_DEFAULT_COMPRESSION);
                    if (MZ_FALSE == status) {
                        BOOST_LOG_TRIVIAL(info) << preset->name << " Failed to add file to ZIP archive";
                        mz_zip_writer_end(&zip_archive);
                        return ExportCase::ADD_FILE_FAIL;
                    }
                    process_configs.push_back(process_config_file_name);
                    BOOST_LOG_TRIVIAL(info) << "Process preset json add successful: ";
                }
            }

            bundle_structure["printer_config"]  = printer_config;
            bundle_structure["filament_config"] = filament_configs;
            bundle_structure["process_config"]  = process_configs;

            std::string bundle_structure_str = bundle_structure.dump();
            status = mz_zip_writer_add_mem(&zip_archive, BUNDLE_STRUCTURE_JSON_NAME, bundle_structure_str.data(), bundle_structure_str.size(), MZ_DEFAULT_COMPRESSION);
            if (MZ_FALSE == status) {
                BOOST_LOG_TRIVIAL(info) << " Failed to add file: " << BUNDLE_STRUCTURE_JSON_NAME;
                mz_zip_writer_end(&zip_archive);
                return ExportCase::ADD_BUNDLE_STRUCTURE_FAIL;
            }
            BOOST_LOG_TRIVIAL(info) << " Success to add file: " << BUNDLE_STRUCTURE_JSON_NAME;

            ExportCase save_result = save_zip_archive_to_file(zip_archive);
            if (ExportCase::CASE_COUNT != save_result) return save_result;
        }
    }
    BOOST_LOG_TRIVIAL(info) << "ZIP archive created successfully";

    return ExportCase::EXPORT_SUCCESS;
}

ExportConfigsDialog::ExportCase ExportConfigsDialog::archive_filament_bundle_to_file(const wxString &path)
{
    std::string export_path = initial_file_path(path, "");
    if (export_path.empty() || "initial_failed" == export_path) return ExportCase::EXPORT_CANCEL;
    BOOST_LOG_TRIVIAL(info) << "Export filament preset bundle";

    for (std::pair<::CheckBox *, std::string> checkbox_filament_name : m_printer_name) {
        if (checkbox_filament_name.first->GetValue()) {
            std::string filament_name = checkbox_filament_name.second;

            json          bundle_structure;
            NetworkAgent *agent = wxGetApp().getAgent();
            std::string   clock = get_curr_timestmp();
            if (agent) {
                bundle_structure["version"]   = agent->get_version();
                bundle_structure["bundle_id"] = agent->get_user_id() + "_" + filament_name + "_" + clock;
            } else {
                bundle_structure["version"]   = "";
                bundle_structure["bundle_id"] = "offline_" + filament_name + "_" + clock;
            }
            bundle_structure["bundle_type"] = "filament config bundle";
            bundle_structure["filament_name"] = filament_name;
            std::unordered_map<std::string, json> vendor_structure;

            mz_zip_archive zip_archive;
            mz_bool        status = initial_zip_archive(zip_archive, export_path + "/" + filament_name + ".orca_filament");
            if (MZ_FALSE == status) {
                BOOST_LOG_TRIVIAL(info) << "Failed to initialize ZIP archive";
                return ExportCase::INITIALIZE_FAIL;
            }

            std::unordered_map<std::string, std::vector<std::pair<std::string, Preset *>>>::iterator iter = m_filament_name_to_presets.find(filament_name);
            if (m_filament_name_to_presets.end() == iter) {
                BOOST_LOG_TRIVIAL(info) << "Filament name do not find, filament name:" << filament_name;
                continue;
            }
            std::set<std::pair<std::string, std::string>> vendor_to_filament_name;
            for (std::pair<std::string, Preset *> printer_name_to_preset : iter->second) {
                std::string printer_vendor = printer_name_to_preset.first;
                if (printer_vendor.empty()) continue;
                Preset *    filament_preset = printer_name_to_preset.second;
                if (vendor_to_filament_name.find(std::make_pair(printer_vendor, filament_preset->name)) != vendor_to_filament_name.end()) continue;
                vendor_to_filament_name.insert(std::make_pair(printer_vendor, filament_preset->name));
                std::string preset_path     = boost::filesystem::path(filament_preset->file).make_preferred().string();
                if (preset_path.empty()) {
                    BOOST_LOG_TRIVIAL(info) << "Export printer preset: " << filament_preset->name << " skip because of the preset file path is empty.";
                    continue;
                }
                // Add a file to the ZIP file
                std::string file_name = printer_vendor + "/" + filament_preset->name + ".json";
                status                = mz_zip_writer_add_file(&zip_archive, file_name.c_str(), encode_path(preset_path.c_str()).c_str(), NULL, 0, MZ_DEFAULT_COMPRESSION);
                // status = mz_zip_writer_add_mem(&zip_archive, ("printer/" + printer_preset->name + ".json").c_str(), json_contents, strlen(json_contents), MZ_DEFAULT_COMPRESSION);
                if (MZ_FALSE == status) {
                    BOOST_LOG_TRIVIAL(info) << filament_preset->name << " Failed to add file to ZIP archive";
                    mz_zip_writer_end(&zip_archive);
                    return ExportCase::ADD_FILE_FAIL;
                }
                std::unordered_map<std::string, json>::iterator iter = vendor_structure.find(printer_vendor);
                if (vendor_structure.end() == iter) {
                    json j = json::array();
                    j.push_back(file_name);
                    vendor_structure[printer_vendor] = j;
                } else {
                    iter->second.push_back(file_name);
                }
                BOOST_LOG_TRIVIAL(info) << "Filament preset json add successful: " << filament_preset->name;
            }

            for (const auto& vendor_name_to_json : vendor_structure) {
                json j;
                std::string printer_vendor = vendor_name_to_json.first;
                j["vendor"]                = printer_vendor;
                j["filament_path"]         = vendor_name_to_json.second;
                bundle_structure["printer_vendor"].push_back(j);
            }

            std::string bundle_structure_str = bundle_structure.dump();
            status = mz_zip_writer_add_mem(&zip_archive, BUNDLE_STRUCTURE_JSON_NAME, bundle_structure_str.data(), bundle_structure_str.size(), MZ_DEFAULT_COMPRESSION);
            if (MZ_FALSE == status) {
                BOOST_LOG_TRIVIAL(info) << " Failed to add file: " << BUNDLE_STRUCTURE_JSON_NAME;
                mz_zip_writer_end(&zip_archive);
                return ExportCase::ADD_BUNDLE_STRUCTURE_FAIL;
            }
            BOOST_LOG_TRIVIAL(info) << " Success to add file: " << BUNDLE_STRUCTURE_JSON_NAME;

            // Complete writing of ZIP file
            ExportCase save_result = save_zip_archive_to_file(zip_archive);
            if (ExportCase::CASE_COUNT != save_result) return save_result;
        }
    }
    BOOST_LOG_TRIVIAL(info) << "ZIP archive created successfully";

    return ExportCase::EXPORT_SUCCESS;
}

ExportConfigsDialog::ExportCase ExportConfigsDialog::archive_printer_preset_to_file(const wxString &path)
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "start exprot printer presets";
    std::string export_file = "Printer presets.zip";
    export_file             = initial_file_name(path, export_file);
    if (export_file.empty() || "initial_failed" == export_file) return ExportCase::EXPORT_CANCEL;

    std::vector<std::pair<std::string, std::string>> config_paths;

    for (std::pair<::CheckBox *, Preset *> checkbox_preset : m_preset) {
        if (checkbox_preset.first->GetValue()) {
            Preset *    printer_preset = checkbox_preset.second;
            std::string preset_path    = boost::filesystem::path(printer_preset->file).make_preferred().string();
            if (preset_path.empty()) {
                BOOST_LOG_TRIVIAL(info) << "Export printer preset: " << printer_preset->name << " skip because of the preset file path is empty.";
                continue;
            }
            std::string preset_name = printer_preset->name + ".json";
            config_paths.push_back(std::make_pair(preset_name, preset_path));
        }
    }

    ExportCase save_result = save_presets_to_zip(export_file, config_paths);
    if (ExportCase::CASE_COUNT != save_result) return save_result;

    BOOST_LOG_TRIVIAL(info) << "ZIP archive created successfully";

    return ExportCase::EXPORT_SUCCESS;

}

ExportConfigsDialog::ExportCase ExportConfigsDialog::archive_filament_preset_to_file(const wxString &path)
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "start exprot filament presets";
    std::string export_file = "Filament presets.zip";
    export_file             = initial_file_name(path, export_file);
    if (export_file.empty() || "initial_failed" == export_file) return ExportCase::EXPORT_CANCEL;

    std::vector<std::pair<std::string, std::string>> config_paths;

    std::set<std::string> filament_presets;
    for (std::pair<::CheckBox *, std::string> checkbox_preset : m_printer_name) {
        if (checkbox_preset.first->GetValue()) {
            std::string filament_name = checkbox_preset.second;

            std::unordered_map<std::string, std::vector<std::pair<std::string, Preset *>>>::iterator iter = m_filament_name_to_presets.find(filament_name);
            if (m_filament_name_to_presets.end() == iter) {
                BOOST_LOG_TRIVIAL(info) << "Filament name do not find, filament name:" << filament_name;
                continue;
            }
            for (std::pair<std::string, Preset*> printer_name_preset : iter->second) {
                Preset *    filament_preset = printer_name_preset.second;
                if (filament_presets.find(filament_preset->name) != filament_presets.end()) continue;
                filament_presets.insert(filament_preset->name);
                std::string preset_path     = boost::filesystem::path(filament_preset->file).make_preferred().string();
                if (preset_path.empty()) {
                    BOOST_LOG_TRIVIAL(info) << "Export filament preset: " << filament_preset->name << " skip because of the filament file path is empty.";
                    continue;
                }

                std::string preset_name = filament_preset->name + ".json";
                config_paths.push_back(std::make_pair(preset_name, preset_path));
            }
        }
    }

    ExportCase save_result = save_presets_to_zip(export_file, config_paths);
    if (ExportCase::CASE_COUNT != save_result) return save_result;

    BOOST_LOG_TRIVIAL(info) << "ZIP archive created successfully";

    return ExportCase::EXPORT_SUCCESS;
}

ExportConfigsDialog::ExportCase ExportConfigsDialog::archive_process_preset_to_file(const wxString &path)
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "start exprot process presets";
    std::string export_file = "Process presets.zip";
    export_file             = initial_file_name(path, export_file);
    if (export_file.empty() || "initial_failed" == export_file) return ExportCase::EXPORT_CANCEL;

    std::vector<std::pair<std::string, std::string>> config_paths;

    std::set<std::string> process_presets;
    for (std::pair<::CheckBox *, std::string> checkbox_preset : m_printer_name) {
        if (checkbox_preset.first->GetValue()) {
            std::string printer_name = checkbox_preset.second;
            std::unordered_map<std::string, std::vector<Preset *>>::iterator iter = m_process_presets.find(printer_name);
            if (m_process_presets.end() != iter) {
                for (Preset *process_preset : iter->second) {
                    if (process_presets.find(process_preset->name) != process_presets.end()) continue;
                    process_presets.insert(process_preset->name);
                    std::string preset_path = boost::filesystem::path(process_preset->file).make_preferred().string();
                    if (preset_path.empty()) {
                        BOOST_LOG_TRIVIAL(info) << "Export process preset: " << process_preset->name << " skip because of the preset file path is empty.";
                        continue;
                    }

                    std::string preset_name = process_preset->name + ".json";
                    config_paths.push_back(std::make_pair(preset_name, preset_path));
                }
            }
        }
    }

    ExportCase save_result = save_presets_to_zip(export_file, config_paths);
    if (ExportCase::CASE_COUNT != save_result) return save_result;

    BOOST_LOG_TRIVIAL(info) << "ZIP archive created successfully";

    return ExportCase::EXPORT_SUCCESS;
}

wxWindow *ExportConfigsDialog::create_dialog_buttons(wxWindow* parent)
{
    auto dlg_btns = new DialogButtons(parent, {"OK", "Cancel"});
    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        if (!has_check_box_selected()) {
            MessageDialog dlg(this, _L("Please select at least one printer or filament."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"),
                              wxYES | wxYES_DEFAULT | wxCENTRE);
            dlg.ShowModal();
            return;
        }

        wxDirDialog dlg(this, _L("Choose a directory"), "", wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
        wxString    path;
        if (dlg.ShowModal() == wxID_OK) path = dlg.GetPath();
        ExportCase export_case = ExportCase::EXPORT_CANCEL;
        if (!path.IsEmpty()) {
            wxGetApp().app_config->update_config_dir(into_u8(path));
            wxGetApp().app_config->save();
            const wxString curr_radio_type = get_curr_radio_type(m_export_type_btns);

            if (curr_radio_type == m_exprot_type.preset_bundle) {
                export_case = archive_preset_bundle_to_file(path);
            } else if (curr_radio_type == m_exprot_type.filament_bundle) {
                export_case = archive_filament_bundle_to_file(path);
            } else if (curr_radio_type == m_exprot_type.printer_preset) {
                export_case = archive_printer_preset_to_file(path);
            } else if (curr_radio_type == m_exprot_type.filament_preset) {
                export_case = archive_filament_preset_to_file(path);
            } else if (curr_radio_type == m_exprot_type.process_preset) {
                export_case = archive_process_preset_to_file(path);
            }
        } else {
            return;
        }
        show_export_result(export_case);
        if (ExportCase::EXPORT_SUCCESS != export_case) return;

        EndModal(wxID_OK);
        });

    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) { EndModal(wxID_CANCEL); });

    return dlg_btns;
}

wxBoxSizer *ExportConfigsDialog::create_select_printer(wxWindow *parent)
{
    wxBoxSizer *horizontal_sizer = new wxBoxSizer(wxVERTICAL);

    wxBoxSizer *  optionSizer        = new wxBoxSizer(wxVERTICAL);
    m_serial_text           = new wxStaticText(parent, wxID_ANY, _L("Please select a type you want to export"), wxDefaultPosition, wxDefaultSize);
    optionSizer->Add(m_serial_text, 0, wxEXPAND | wxALL, 0);
    optionSizer->SetMinSize(OPTION_SIZE);
    horizontal_sizer->Add(optionSizer, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(10));
    m_scrolled_preset_window = new wxScrolledWindow(parent);
    m_scrolled_preset_window->SetScrollRate(5, 5);
    m_scrolled_preset_window->SetBackgroundColour(*wxWHITE);
    m_scrolled_preset_window->SetMaxSize(wxSize(FromDIP(660), FromDIP(400)));
    m_scrolled_preset_window->SetSize(wxSize(FromDIP(660), FromDIP(400)));
    wxBoxSizer *scrolled_window = new wxBoxSizer(wxHORIZONTAL);

    m_presets_window = new wxPanel(m_scrolled_preset_window, wxID_ANY);
    m_presets_window->SetBackgroundColour(*wxWHITE);
    wxBoxSizer *select_printer_sizer  = new wxBoxSizer(wxVERTICAL);

    m_preset_sizer = new wxGridSizer(3, FromDIP(5), FromDIP(5));
    select_printer_sizer->Add(m_preset_sizer, 0, wxEXPAND, FromDIP(5));
    m_presets_window->SetSizer(select_printer_sizer);
    scrolled_window->Add(m_presets_window, 0, wxEXPAND, 0);
    m_scrolled_preset_window->SetSizerAndFit(scrolled_window);

    horizontal_sizer->Add(m_scrolled_preset_window, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));

    return horizontal_sizer;
}

void ExportConfigsDialog::data_init()
{
    // Delete the Temp folder
    boost::filesystem::path folder(data_dir() + "/" + PRESET_USER_DIR + "/" + "Temp");
    if (boost::filesystem::exists(folder)) boost::filesystem::remove_all(folder);

    boost::system::error_code ec;
    boost::filesystem::path user_folder(data_dir() + "/" + PRESET_USER_DIR);
    bool                      temp_folder_exist = true;
    if (!boost::filesystem::exists(user_folder)) {
        if (!boost::filesystem::create_directories(user_folder, ec)) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " create directory failed: " << user_folder << " "<<ec.message();
            temp_folder_exist = false;
        }
    }
    boost::filesystem::path temp_folder(user_folder / "Temp");
    if (!boost::filesystem::exists(temp_folder)) {
        if (!boost::filesystem::create_directories(temp_folder, ec)) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " create directory failed: " << temp_folder << " " << ec.message();
            temp_folder_exist = false;
        }
    }
    if (!temp_folder_exist) {
        MessageDialog dlg(this, _L("Failed to create temporary folder, please try Export Configs again."), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES_NO | wxYES_DEFAULT | wxCENTRE);
        dlg.ShowModal();
        EndModal(wxCANCEL);
    }

    PresetBundle preset_bundle(*wxGetApp().preset_bundle);

    const std::deque<Preset> & printer_presets = preset_bundle.printers.get_presets();
    for (const Preset &printer_preset : printer_presets) {

        std::string preset_name        = printer_preset.name;
        if (!printer_preset.is_visible || printer_preset.is_default || printer_preset.is_project_embedded) continue;
        if (preset_bundle.printers.select_preset_by_name(preset_name, true)) {
            preset_bundle.update_compatible(PresetSelectCompatibleType::Always);

            const std::deque<Preset> &filament_presets = preset_bundle.filaments.get_presets();
            for (const Preset &filament_preset : filament_presets) {
                if (filament_preset.is_system || filament_preset.is_default || filament_preset.is_project_embedded) continue;
                if (filament_preset.is_compatible) {
                    Preset *new_filament_preset = new Preset(filament_preset);
                    m_filament_presets[preset_name].push_back(new_filament_preset);
                }
            }

            const std::deque<Preset> &process_presets = preset_bundle.prints.get_presets();
            for (const Preset &process_preset : process_presets) {
                if (process_preset.is_system || process_preset.is_default || process_preset.is_project_embedded) continue;
                if (process_preset.is_compatible) {
                    Preset *new_prpcess_preset = new Preset(process_preset);
                    m_process_presets[preset_name].push_back(new_prpcess_preset);
                }
            }

            Preset *new_printer_preset     = new Preset(printer_preset);
            earse_preset_fields_for_safe(new_printer_preset);
            m_printer_presets[preset_name] = new_printer_preset;
        }
    }
    const std::deque<Preset> &filament_presets = preset_bundle.filaments.get_presets();
    for (const Preset &filament_preset : filament_presets) {
        if (filament_preset.is_system || filament_preset.is_default) continue;
        Preset *new_filament_preset = new Preset(filament_preset);
        const Preset *base_filament_preset = preset_bundle.filaments.get_preset_base(*new_filament_preset);

        if (base_filament_preset == nullptr) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " Failed to find base preset";
            continue;
        }
        std::string filament_preset_name = base_filament_preset->name;
        std::string machine_name         = get_machine_name(filament_preset_name);
        m_filament_name_to_presets[get_filament_name(filament_preset_name)].push_back(std::make_pair(get_vendor_name(machine_name), new_filament_preset));
    }
}

EditFilamentPresetDialog::EditFilamentPresetDialog(wxWindow *parent, Filamentinformation *filament_info)
    : DPIDialog(parent ? parent : nullptr, wxID_ANY, _L("Edit Filament"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    , m_filament_id("")
    , m_filament_name("")
    , m_vendor_name("")
    , m_filament_type("")
    , m_filament_serial("")
{
    m_preset_tree_creater = new PresetTree(this);

    this->SetBackgroundColour(*wxWHITE);
    this->SetMinSize(wxSize(FromDIP(600), -1));

    m_main_sizer = new wxBoxSizer(wxVERTICAL);
    // top line
    auto m_line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    m_line_top->SetBackgroundColour(wxColour(0xA6, 0xa9, 0xAA));
    m_main_sizer->Add(m_line_top, 0, wxEXPAND, 0);
    m_main_sizer->Add(0, 0, 0, wxTOP, FromDIP(5));

    wxStaticText* basic_information = new wxStaticText(this, wxID_ANY, _L("Basic Information")); 
    basic_information->SetFont(Label::Head_16);

    m_main_sizer->Add(basic_information, 0, wxALL, FromDIP(10));
    m_filament_id = filament_info->filament_id;
    //std::string filament_name = filament_info->filament_name;
    bool get_filament_presets = get_same_filament_id_presets(m_filament_id);
    // get filament vendor, type, serial, and name
    if (get_filament_presets && !m_printer_compatible_presets.empty()) {
        std::shared_ptr<Preset> preset;
        for (std::pair<std::string, std::vector<std::shared_ptr<Preset>>> pair : m_printer_compatible_presets) {
            for (std::shared_ptr<Preset> fialment_preset : pair.second) {
                if (fialment_preset->inherits().empty()) {
                    preset = fialment_preset;
                    break;
                }
            }
        }
        if (!preset.get()) preset = m_printer_compatible_presets.begin()->second[0];
        m_filament_name                = get_filament_name(preset->name);
        auto vendor_names              = dynamic_cast<ConfigOptionStrings *>(preset->config.option("filament_vendor"));
        if (vendor_names  && !vendor_names->values.empty()) m_vendor_name = vendor_names->values[0];
        auto filament_types = dynamic_cast<ConfigOptionStrings *>(preset->config.option("filament_type"));
        if (filament_types && !filament_types->values.empty()) m_filament_type = filament_types->values[0];
        std::string filament_type = m_filament_type == "PLA-AERO" ? "PLA Aero" : m_filament_type;
        size_t      index         = m_filament_name.find(filament_type);
        if (std::string::npos != index && index + filament_type.size() < m_filament_name.size()) {
            m_filament_serial = m_filament_name.substr(index + filament_type.size());
            if (m_filament_serial.size() > 2 && m_filament_serial[0] == ' ') {
                m_filament_serial = m_filament_serial.substr(1);
            }
        }
    }

    m_main_sizer->Add(create_filament_basic_info(), 0, wxEXPAND | wxALL, 0);

    // divider line
    auto line_divider = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    line_divider->SetBackgroundColour(wxColour(0xA6, 0xa9, 0xAA));
    m_main_sizer->Add(line_divider, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));
    m_main_sizer->Add(0, 0, 0, wxTOP, FromDIP(5));

    wxStaticText *presets_information = new wxStaticText(this, wxID_ANY, _L("Filament presets under this filament"));
    presets_information->SetFont(Label::Head_16);
    m_main_sizer->Add(presets_information, 0, wxLEFT | wxRIGHT, FromDIP(10));

    m_main_sizer->Add(create_add_filament_btn(), 0, wxEXPAND | wxALL, 0);
    m_main_sizer->Add(create_preset_tree_sizer(), 0, wxEXPAND | wxALL, 0);
    m_note_text = new wxStaticText(this, wxID_ANY, _L("Note: If the only preset under this filament is deleted, the filament will be deleted after exiting the dialog."));
    m_main_sizer->Add(m_note_text, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM | wxALIGN_CENTER_VERTICAL, FromDIP(10));
    m_note_text->Hide();
    m_main_sizer->Add(create_dialog_buttons(), 0, wxEXPAND);

    update_preset_tree();

    this->SetSizer(m_main_sizer);
    this->Layout();
    this->Fit();
    wxGetApp().UpdateDlgDarkUI(this);
}
EditFilamentPresetDialog::~EditFilamentPresetDialog() {}

void EditFilamentPresetDialog::on_dpi_changed(const wxRect &suggested_rect) {
    Layout();
}

bool EditFilamentPresetDialog::get_same_filament_id_presets(std::string filament_id)
{
    PresetBundle *preset_bundle = wxGetApp().preset_bundle;
    const std::deque<Preset> &filament_presets = preset_bundle->filaments.get_presets();

    m_printer_compatible_presets.clear();
    for (Preset const &preset : filament_presets) {
        if (preset.is_system || preset.filament_id != filament_id) continue;
        std::shared_ptr<Preset> new_preset = std::make_shared<Preset>(preset);
        std::vector<std::string> printers;
        get_filament_compatible_printer(new_preset.get(), printers);
        for (const std::string &printer_name : printers) {
            m_printer_compatible_presets[printer_name].push_back(new_preset);
        }
    }
    if (m_printer_compatible_presets.empty()) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " no filament presets ";
        return false;
    }

    return true;
}

void EditFilamentPresetDialog::update_preset_tree()
{
    this->Freeze();
    m_preset_tree_sizer->Clear(true);
    for (std::pair<std::string, std::vector<std::shared_ptr<Preset>>> printer_and_presets : m_printer_compatible_presets) {
        m_preset_tree_sizer->Add(m_preset_tree_creater->get_preset_tree(printer_and_presets), 0, wxEXPAND | wxLEFT | wxTOP | wxRIGHT, 5);
    }
    if (m_printer_compatible_presets.size() == 1 && m_printer_compatible_presets.begin()->second.size() == 1) {
        m_note_text->Show();
    } else {
        m_note_text->Hide();
    }

    m_preset_tree_panel->SetSizerAndFit(m_preset_tree_sizer);
    int width  = m_preset_tree_panel->GetSize().GetWidth();
    int height = m_preset_tree_panel->GetSize().GetHeight();
    if (width < m_note_text->GetSize().GetWidth()) {
        width = m_note_text->GetSize().GetWidth();
        m_preset_tree_panel->SetMinSize(wxSize(width, -1));
    }
    int width_extend = 0;
    int height_extend = 0;
    if (width > 1000) height_extend = 22;
    if (height > 400) width_extend = 22;
    m_preset_tree_window->SetMinSize(wxSize(std::min(1000, width + width_extend), std::min(400, height + height_extend)));
    m_preset_tree_window->SetMaxSize(wxSize(std::min(1000, width + width_extend), std::min(400, height + height_extend)));
    m_preset_tree_window->SetSize(wxSize(std::min(1000, width + width_extend), std::min(400, height + height_extend)));
    this->SetSizerAndFit(m_main_sizer);

    this->Layout();
    this->Fit();
    this->Refresh();
    wxGetApp().UpdateDlgDarkUI(this);
    adjust_dialog_in_screen(this);
    this->Thaw();
}

void EditFilamentPresetDialog::delete_preset()
{
    if (m_selected_printer.empty()) return;
    if (m_need_delete_preset_index < 0) return;
    std::unordered_map<std::string, std::vector<std::shared_ptr<Preset>>>::iterator iter = m_printer_compatible_presets.find(m_selected_printer);
    if (m_printer_compatible_presets.end() == iter) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " can not find printer and printer name is: " << m_selected_printer;
        return;
    }
    std::vector<std::shared_ptr<Preset>>& filament_presets = iter->second;
    if (m_need_delete_preset_index >= filament_presets.size()) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " index error and selected printer is: " << m_selected_printer << " and index: " << m_need_delete_preset_index;
        return;
    }
    std::shared_ptr<Preset> need_delete_preset = filament_presets[m_need_delete_preset_index];
    // is selecetd filament preset
    if (need_delete_preset->name == wxGetApp().preset_bundle->filaments.get_selected_preset_name()) {
        wxGetApp().get_tab(need_delete_preset->type)->delete_preset();
        // is preset exist? exist: not delete
        Preset *delete_preset = wxGetApp().preset_bundle->filaments.find_preset(need_delete_preset->name, false);
        if (delete_preset) {
            m_selected_printer.clear();
            m_need_delete_preset_index = -1;
            return;
        }
    } else {
        Preset *filament_preset = wxGetApp().preset_bundle->filaments.find_preset(need_delete_preset->name);

        // is root preset ?
        bool is_base_preset = false;
        if (filament_preset && wxGetApp().preset_bundle->filaments.get_preset_base(*filament_preset) == filament_preset) {
            is_base_preset = true;
            int      count = 0;
            wxString presets;
            for (auto &preset2 : wxGetApp().preset_bundle->filaments)
                if (preset2.inherits() == filament_preset->name) {
                    ++count;
                    presets += "\n - " + from_u8(preset2.name);
                }
            wxString msg;
            if (count > 0) {
                msg = _L("Presets inherited by other presets cannot be deleted");
                msg += "\n";
                msg += _L_PLURAL("The following presets inherits this preset.", "The following preset inherits this preset.", count);
                wxString title = _L("Delete Preset");
                MessageDialog(this, msg + presets, title, wxOK | wxICON_ERROR).ShowModal();
                m_selected_printer.clear();
                m_need_delete_preset_index = -1;
                return;
            }
        }
        wxString msg;
        if (is_base_preset) {
            msg = _L("Are you sure to delete the selected preset?\n"
                     "If the preset corresponds to a filament currently in use on your printer, please reset the filament information for that slot.");
        } else {
            msg = _L("Are you sure to delete the selected preset?");
        }
        if (wxID_YES != MessageDialog(this, msg, _L("Delete preset"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION).ShowModal()) {
            m_selected_printer.clear();
            m_need_delete_preset_index = -1;
            return;
        }

        // delete preset
        std::string next_selected_preset_name = wxGetApp().preset_bundle->filaments.get_selected_preset().name;
        bool        delete_result             = delete_filament_preset_by_name(need_delete_preset->name, next_selected_preset_name);
        BOOST_LOG_TRIVIAL(info) << __LINE__ << " filament preset name: " << need_delete_preset->name << (delete_result ? " delete successful" : " delete failed");

        wxGetApp().preset_bundle->filaments.select_preset_by_name(next_selected_preset_name, true);
        for (size_t i = 0; i < wxGetApp().preset_bundle->filament_presets.size(); ++i) {
            auto preset = wxGetApp().preset_bundle->filaments.find_preset(wxGetApp().preset_bundle->filament_presets[i]);
            if (preset == nullptr) wxGetApp().preset_bundle->filament_presets[i] = wxGetApp().preset_bundle->filaments.get_selected_preset_name();
        }
    }

    // remove preset shared_ptr from m_printer_compatible_presets
    int                     last_index         = filament_presets.size() - 1;
    if (m_need_delete_preset_index != last_index) {
        std::swap(filament_presets[m_need_delete_preset_index], filament_presets[last_index]);
    }
    filament_presets.pop_back();
    if (filament_presets.empty()) m_printer_compatible_presets.erase(iter);

    update_preset_tree();

    m_selected_printer.clear();
    m_need_delete_preset_index = -1;
}

void EditFilamentPresetDialog::edit_preset()
{
    if (m_selected_printer.empty()) return;
    if (m_need_edit_preset_index < 0) return;
    std::unordered_map<std::string, std::vector<std::shared_ptr<Preset>>>::iterator iter = m_printer_compatible_presets.find(m_selected_printer);
    if (m_printer_compatible_presets.end() == iter) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " can not find printer and printer name is: " << m_selected_printer;
        return;
    }
    std::vector<std::shared_ptr<Preset>> &filament_presets = iter->second;
    if (m_need_edit_preset_index >= filament_presets.size()) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " index error and selected printer is: " << m_selected_printer << " and index: " << m_need_edit_preset_index;
        return;
    }

    // edit preset
    m_need_edit_preset = filament_presets[m_need_edit_preset_index];
    wxGetApp().params_dialog()->set_editing_filament_id(m_filament_id);

    EndModal(wxID_EDIT);
}

wxBoxSizer *EditFilamentPresetDialog::create_filament_basic_info()
{
    wxBoxSizer *basic_info_sizer = new wxBoxSizer(wxVERTICAL);

    wxBoxSizer *vendor_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer *type_sizer = new wxBoxSizer(wxHORIZONTAL);
    wxBoxSizer *serial_sizer = new wxBoxSizer(wxHORIZONTAL);

    //vendor
    wxBoxSizer *  vendor_key_sizer        = new wxBoxSizer(wxVERTICAL);
    wxStaticText *static_vendor_text = new wxStaticText(this, wxID_ANY, _L("Vendor"), wxDefaultPosition, wxDefaultSize);
    vendor_key_sizer->Add(static_vendor_text, 0, wxEXPAND | wxALL, 0);
    vendor_key_sizer->SetMinSize(OPTION_SIZE);
    vendor_sizer->Add(vendor_key_sizer, 0, wxEXPAND | wxLEFT | wxBOTTOM | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    wxBoxSizer *vendor_value_sizer = new wxBoxSizer(wxVERTICAL);
    wxStaticText *vendor_text = new wxStaticText(this, wxID_ANY, from_u8(m_vendor_name), wxDefaultPosition, wxDefaultSize);
    vendor_value_sizer->Add(vendor_text, 0, wxEXPAND | wxALL, 0);
    vendor_sizer->Add(vendor_value_sizer, 0, wxEXPAND | wxLEFT | wxBOTTOM | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    //type
    wxBoxSizer *  type_key_sizer   = new wxBoxSizer(wxVERTICAL);
    wxStaticText *static_type_text = new wxStaticText(this, wxID_ANY, _L("Type"), wxDefaultPosition, wxDefaultSize);
    type_key_sizer->Add(static_type_text, 0, wxEXPAND | wxALL, 0);
    type_key_sizer->SetMinSize(OPTION_SIZE);
    type_sizer->Add(type_key_sizer, 0, wxEXPAND | wxLEFT | wxBOTTOM | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    wxBoxSizer *  type_value_sizer = new wxBoxSizer(wxVERTICAL);
    wxStaticText *type_text        = new wxStaticText(this, wxID_ANY, from_u8(m_filament_type), wxDefaultPosition, wxDefaultSize);
    type_value_sizer->Add(type_text, 0, wxEXPAND | wxALL, 0);
    type_sizer->Add(type_value_sizer, 0, wxEXPAND | wxLEFT | wxBOTTOM | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    //serial
    wxBoxSizer *  serial_key_sizer   = new wxBoxSizer(wxVERTICAL);
    wxStaticText *static_serial_text = new wxStaticText(this, wxID_ANY, _L("Serial"), wxDefaultPosition, wxDefaultSize);
    serial_key_sizer->Add(static_serial_text, 0, wxEXPAND | wxALL, 0);
    serial_key_sizer->SetMinSize(OPTION_SIZE);
    serial_sizer->Add(serial_key_sizer, 0, wxEXPAND | wxLEFT | wxBOTTOM | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    wxBoxSizer *  serial_value_sizer = new wxBoxSizer(wxVERTICAL);
    wxString      full_filamnet_serial = from_u8(m_filament_serial);
    wxString      show_filament_serial = full_filamnet_serial;
    if (m_filament_serial.size() > 40) {
        show_filament_serial = from_u8(m_filament_serial.substr(0, 20)) + "...";
    }
    wxStaticText *serial_text = new wxStaticText(this, wxID_ANY, show_filament_serial, wxDefaultPosition, wxDefaultSize);
    wxToolTip *   toolTip     = new wxToolTip(full_filamnet_serial);
    serial_text->SetToolTip(toolTip);
    serial_value_sizer->Add(serial_text, 0, wxEXPAND | wxALL, 0);
    serial_sizer->Add(serial_value_sizer, 0, wxEXPAND | wxLEFT | wxBOTTOM | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    basic_info_sizer->Add(vendor_sizer, 0, wxEXPAND | wxALL, 0);
    basic_info_sizer->Add(type_sizer, 0, wxEXPAND | wxALL, 0);
    basic_info_sizer->Add(serial_sizer, 0, wxEXPAND | wxALL, 0);

    return basic_info_sizer;
}

wxBoxSizer *EditFilamentPresetDialog::create_add_filament_btn()
{
    wxBoxSizer *add_filament_btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_add_filament_btn                 = new Button(this, _L("+ Add Preset"));
    m_add_filament_btn->SetStyle(ButtonStyle::Regular, ButtonType::Window);

    add_filament_btn_sizer->Add(m_add_filament_btn, 0, wxEXPAND | wxALL, FromDIP(10));

    m_add_filament_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        CreatePresetForPrinterDialog dlg(nullptr, m_filament_type, m_filament_id, m_vendor_name, m_filament_name);
        int res = dlg.ShowModal();
        if (res == wxID_OK) {
            if (get_same_filament_id_presets(m_filament_id)) {
                update_preset_tree();
            }
        }
    });

    return add_filament_btn_sizer;
}

wxBoxSizer *EditFilamentPresetDialog::create_preset_tree_sizer()
{
    wxBoxSizer *filament_preset_tree_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_preset_tree_window = new wxScrolledWindow(this);
    m_preset_tree_window->SetScrollRate(5, 5);
    m_preset_tree_window->SetBackgroundColour(PRINTER_LIST_COLOUR);
    m_preset_tree_window->SetMinSize(wxSize(-1, FromDIP(400)));
    m_preset_tree_window->SetMaxSize(wxSize(-1, FromDIP(300)));
    m_preset_tree_window->SetSize(wxSize(-1, FromDIP(300)));
    m_preset_tree_panel = new wxPanel(m_preset_tree_window);
    m_preset_tree_sizer = new wxBoxSizer(wxVERTICAL);
    m_preset_tree_panel->SetSizer(m_preset_tree_sizer);
    m_preset_tree_panel->SetMinSize(wxSize(580, -1));
    m_preset_tree_panel->SetBackgroundColour(PRINTER_LIST_COLOUR);
    wxBoxSizer* m_preset_tree_window_sizer = new wxBoxSizer(wxVERTICAL);
    m_preset_tree_window_sizer->Add(m_preset_tree_panel, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));
    m_preset_tree_window->SetSizerAndFit(m_preset_tree_window_sizer);
    filament_preset_tree_sizer->Add(m_preset_tree_window, 0, wxEXPAND | wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    return filament_preset_tree_sizer;
}

wxWindow *EditFilamentPresetDialog::create_dialog_buttons()
{
    auto dlg_btns = new DialogButtons(this, {"Delete", "OK"}, "", 1 /*left_aligned*/);

    dlg_btns->GetFIRST()->Bind(wxEVT_BUTTON, ([this](wxCommandEvent &e) {
        WarningDialog dlg(this, _L("All the filament presets belong to this filament would be deleted.\n"
                                   "If you are using this filament on your printer, please reset the filament information for that slot."),
                          _L("Delete filament"), wxYES | wxCANCEL | wxCANCEL_DEFAULT | wxCENTRE);
        int res = dlg.ShowModal();
        if (wxID_YES == res) {
            PresetBundle *preset_bundle = wxGetApp().preset_bundle;
            std::set<std::shared_ptr<Preset>> inherit_preset_names;
            std::set<std::shared_ptr<Preset>> root_preset_names;
            for (std::pair<std::string, std::vector<std::shared_ptr<Preset>>> printer_and_preset : m_printer_compatible_presets) {
                for (std::shared_ptr<Preset> preset : printer_and_preset.second) {
                    if (preset->inherits().empty()) {
                        root_preset_names.insert(preset);
                    } else {
                        inherit_preset_names.insert(preset);
                    }
                }
            }
            // delete inherit preset first
            std::string next_selected_preset_name = wxGetApp().preset_bundle->filaments.get_selected_preset().name;
            for (std::shared_ptr<Preset> preset : inherit_preset_names) {
                bool delete_result = delete_filament_preset_by_name(preset->name, next_selected_preset_name);
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " inherit filament name: " << preset->name << (delete_result ? " delete successful" : " delete failed");
            }
            for (std::shared_ptr<Preset> preset : root_preset_names) {
                bool delete_result = delete_filament_preset_by_name(preset->name, next_selected_preset_name);
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " root filament name: " << preset->name << (delete_result ? " delete successful" : " delete failed");
            }
            m_printer_compatible_presets.clear();
            wxGetApp().preset_bundle->filaments.select_preset_by_name(next_selected_preset_name,true);

            for (size_t i = 0; i < wxGetApp().preset_bundle->filament_presets.size(); ++i) {
                auto preset = wxGetApp().preset_bundle->filaments.find_preset(wxGetApp().preset_bundle->filament_presets[i]);
                if (preset == nullptr) wxGetApp().preset_bundle->filament_presets[i] = wxGetApp().preset_bundle->filaments.get_selected_preset_name();
            }
            EndModal(wxID_OK);
        }
        e.Skip();
        }));

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) { EndModal(wxID_OK); });

    return dlg_btns;
}

CreatePresetForPrinterDialog::CreatePresetForPrinterDialog(wxWindow *parent, std::string filament_type, std::string filament_id, std::string filament_vendor, std::string filament_name)
    : DPIDialog(parent ? parent : nullptr, wxID_ANY, _L("Add Preset"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    , m_filament_id(filament_id)
    , m_filament_name(filament_name)
    , m_filament_vendor(filament_vendor)
    , m_filament_type(filament_type)
{
    m_preset_bundle = std::make_shared<PresetBundle>(*(wxGetApp().preset_bundle));
    get_visible_printer_and_compatible_filament_presets();

    this->SetBackgroundColour(*wxWHITE);

    wxBoxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);
    // top line
    auto m_line_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    m_line_top->SetBackgroundColour(wxColour(0xA6, 0xa9, 0xAA));
    main_sizer->Add(m_line_top, 0, wxEXPAND, 0);
    main_sizer->Add(0, 0, 0, wxTOP, FromDIP(5));

    wxStaticText *basic_information = new wxStaticText(this, wxID_ANY, _L("Add preset for new printer"));
    basic_information->SetFont(Label::Head_16);
    main_sizer->Add(basic_information, 0, wxALL, FromDIP(10));

    main_sizer->Add(create_selected_printer_preset_sizer(), 0, wxALL, FromDIP(10));
    main_sizer->Add(create_selected_filament_preset_sizer(), 0, wxALL, FromDIP(10));
    main_sizer->Add(create_dialog_buttons(), 0, wxEXPAND);

    this->SetSizer(main_sizer);
    this->Layout();
    this->Fit();
    wxGetApp().UpdateDlgDarkUI(this);
}

CreatePresetForPrinterDialog::~CreatePresetForPrinterDialog() {}

void CreatePresetForPrinterDialog::on_dpi_changed(const wxRect &suggested_rect) {
    Layout();
}

void CreatePresetForPrinterDialog::get_visible_printer_and_compatible_filament_presets()
{
    const std::deque<Preset> &printer_presets = m_preset_bundle->printers.get_presets();
    m_printer_compatible_filament_presets.clear();
    for (const Preset &printer_preset : printer_presets) {
        if (printer_preset.is_visible) {
            if (m_preset_bundle->printers.get_preset_base(printer_preset) != &printer_preset) continue;
            if (m_preset_bundle->printers.select_preset_by_name(printer_preset.name, true)) {
                m_preset_bundle->update_compatible(PresetSelectCompatibleType::Always);
                const std::deque<Preset> &filament_presets = m_preset_bundle->filaments.get_presets();
                for (const Preset &filament_preset : filament_presets) {
                    if (filament_preset.is_default || !filament_preset.is_compatible || filament_preset.is_project_embedded) continue;
                    ConfigOptionStrings *filament_types;
                    const Preset *       filament_preset_base = m_preset_bundle->filaments.get_preset_base(filament_preset);
                    if (filament_preset_base == &filament_preset) {
                        filament_types = dynamic_cast<ConfigOptionStrings *>(const_cast<Preset *>(&filament_preset)->config.option("filament_type"));
                    } else {
                        filament_types = dynamic_cast<ConfigOptionStrings *>(const_cast<Preset *>(filament_preset_base)->config.option("filament_type"));
                    }

                    if (filament_types && filament_types->values.empty()) continue;
                    const std::string filament_type = filament_types->values[0];
                    std::string filament_type_ = m_filament_type == "PLA Aero" ? "PLA-AERO" : m_filament_type;
                    if (filament_type == filament_type_) {
                        m_printer_compatible_filament_presets[printer_preset.name].push_back(std::make_shared<Preset>(filament_preset));
                    }
                }
            }
        }
    }
}

wxBoxSizer *CreatePresetForPrinterDialog::create_selected_printer_preset_sizer()
{
    wxBoxSizer *select_preseter_preset_sizer = new wxBoxSizer(wxVERTICAL);
    wxStaticText *printer_text = new wxStaticText(this, wxID_ANY, _L("Printer"), wxDefaultPosition, wxDefaultSize);
    select_preseter_preset_sizer->Add(printer_text, 0, wxEXPAND | wxALL, 0);
    m_selected_printer = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, PRINTER_PRESET_MODEL_SIZE, 0, nullptr, wxCB_READONLY);
    select_preseter_preset_sizer->Add(m_selected_printer, 0, wxEXPAND | wxTOP, FromDIP(5));

    wxArrayString printer_choices;
    for (std::pair<std::string, std::vector<std::shared_ptr<Preset>>> printer_to_filament_presets : m_printer_compatible_filament_presets) {
        auto compatible_printer_name = printer_to_filament_presets.first;
        if (compatible_printer_name.empty()) {
            BOOST_LOG_TRIVIAL(info)<<__FUNCTION__ << " a printer has no name";
            continue;
        }
        wxString printer_name              = from_u8(compatible_printer_name);
        printer_choices.push_back(printer_name);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " entry, and visible printer is: " << compatible_printer_name;
    }
    m_selected_printer->Set(printer_choices);

    return select_preseter_preset_sizer;
}

wxBoxSizer *CreatePresetForPrinterDialog::create_selected_filament_preset_sizer()
{
    wxBoxSizer *  select_filament_preset_sizer = new wxBoxSizer(wxVERTICAL);
    wxStaticText *printer_text                 = new wxStaticText(this, wxID_ANY, _L("Copy preset from filament"), wxDefaultPosition, wxDefaultSize);
    select_filament_preset_sizer->Add(printer_text, 0, wxEXPAND | wxALL, 0);
    m_selected_filament = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, PRINTER_PRESET_MODEL_SIZE, 0, nullptr, wxCB_READONLY);
    select_filament_preset_sizer->Add(m_selected_filament, 0, wxEXPAND | wxTOP, FromDIP(5));

    m_selected_printer->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &e) {
        wxString printer_name = m_selected_printer->GetStringSelection();
        std::unordered_map<string, std::vector<std::shared_ptr<Preset>>>::iterator filament_iter = m_printer_compatible_filament_presets.find(into_u8(printer_name));
        if (m_printer_compatible_filament_presets.end() != filament_iter) {
            filament_choice_to_filament_preset.clear();
            wxArrayString filament_choices;
            for (std::shared_ptr<Preset> filament_preset : filament_iter->second) {
                wxString filament_name                            = wxString::FromUTF8(filament_preset->name);
                filament_choice_to_filament_preset[filament_name] = filament_preset;
                filament_choices.push_back(filament_name);
            }
            m_selected_filament->Set(filament_choices);
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " count of compatible filament presets :" << filament_choices.size();
            if (filament_choices.size()) { m_selected_filament->SetSelection(0); }
        } else {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "printer preset not find compatible filament presets";
        }
    });

    return select_filament_preset_sizer;
}

wxWindow *CreatePresetForPrinterDialog::create_dialog_buttons()
{
    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});
    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        wxString selected_printer_name  = m_selected_printer->GetStringSelection();
        std::string printer_name = into_u8(selected_printer_name);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " add preset: get compatible printer name:";

        wxString filament_preset_name = m_selected_filament->GetStringSelection();
        std::unordered_map<wxString, std::shared_ptr<Preset>>::iterator iter = filament_choice_to_filament_preset.find(filament_preset_name);
        if (filament_choice_to_filament_preset.end() != iter) {
            std::shared_ptr<Preset>  filament_preset = iter->second;
            PresetBundle *           preset_bundle   = wxGetApp().preset_bundle;
            std::vector<std::string> failures;
            DynamicConfig            dynamic_config;
            dynamic_config.set_key_value("filament_vendor", new ConfigOptionStrings({m_filament_vendor}));
            dynamic_config.set_key_value("compatible_printers", new ConfigOptionStrings({printer_name}));
            dynamic_config.set_key_value("filament_type", new ConfigOptionStrings({m_filament_type}));
            bool res = preset_bundle->filaments.clone_presets_for_filament(filament_preset.get(), failures, m_filament_name, m_filament_id, dynamic_config, printer_name);
            if (!res) {
                std::string failure_names;
                for (std::string &failure : failures) { failure_names += failure + "\n"; }
                MessageDialog dlg(this, _L("Some existing presets have failed to be created, as follows:\n") + from_u8(failure_names) + _L("\nDo you want to rewrite it?"),
                                  wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES_NO | wxYES_DEFAULT | wxCENTRE);
                if (dlg.ShowModal() == wxID_YES) {
                    res = preset_bundle->filaments.clone_presets_for_filament(filament_preset.get(), failures, m_filament_name, m_filament_id, dynamic_config, printer_name, true);
                    BOOST_LOG_TRIVIAL(info) << "clone filament  have failures  rewritten  is successful? " << res;
                } else {
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "have same name preset and not rewritten";
                    return;
                }
            } else {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "create filament preset successful and name is:" << m_filament_name + " @" + printer_name;
            }

        } else {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "filament choice not find filament preset and choice is:" << filament_preset_name;
            MessageDialog dlg(this, _L("The filament choice not find filament preset, please reselect it"), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"),
                              wxYES | wxYES_DEFAULT | wxCENTRE);
            dlg.ShowModal();
            return;
        }

        EndModal(wxID_OK);
    });
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        EndModal(wxID_CANCEL);
    });

    return dlg_btns;
}

PresetTree::PresetTree(EditFilamentPresetDialog * dialog)
: m_parent_dialog(dialog)
{}

wxPanel *PresetTree::get_preset_tree(std::pair<std::string, std::vector<std::shared_ptr<Preset>>> printer_and_presets)
{
    wxBoxSizer *sizer           = new wxBoxSizer(wxVERTICAL);
    wxPanel *   parent          = m_parent_dialog->get_preset_tree_panel();
    wxPanel *   panel           = new wxPanel(parent);
    wxColour    backgroundColor = parent->GetBackgroundColour();
    panel->SetBackgroundColour(backgroundColor);
    const std::string &printer_name = printer_and_presets.first;
    sizer->Add(get_root_item(panel, printer_name), 0, wxEXPAND, 0);
    int child_count = printer_and_presets.second.size();
    for (int i = 0; i < child_count; i++) {
        if (i == child_count - 1) {
            sizer->Add(get_child_item(panel, printer_and_presets.second[i], printer_name, i, true), 0, wxEXPAND, 0);
        } else {
            sizer->Add(get_child_item(panel, printer_and_presets.second[i], printer_name, i, false), 0, wxEXPAND, 0);
        }
    }
    panel->SetSizerAndFit(sizer);
    return panel;
}

wxPanel *PresetTree::get_root_item(wxPanel *parent, const std::string &printer_name)
{
    wxBoxSizer *sizer           = new wxBoxSizer(wxHORIZONTAL);
    wxPanel *   panel           = new wxPanel(parent);
    wxColour    backgroundColor = parent->GetBackgroundColour();
    panel->SetBackgroundColour(backgroundColor);
    wxStaticText *preset_name = new wxStaticText(panel, wxID_ANY, from_u8(printer_name));
    preset_name->SetFont(Label::Body_11);
    preset_name->SetForegroundColour(*wxBLACK);
    sizer->Add(preset_name, 0, wxEXPAND | wxALL, 5);
    panel->SetSizer(sizer);

    return panel;
}

wxPanel *PresetTree::get_child_item(wxPanel *parent, std::shared_ptr<Preset> preset, std::string printer_name, int preset_index, bool is_last)
{
    wxBoxSizer *sizer           = new wxBoxSizer(wxHORIZONTAL);
    wxPanel *   panel           = new wxPanel(parent);
    wxColour    backgroundColor = parent->GetBackgroundColour();
    panel->SetBackgroundColour(backgroundColor);
    sizer->Add(0, 0, 0, wxLEFT, 10);
    wxPanel *line_left = new wxPanel(panel, wxID_ANY, wxDefaultPosition, is_last ? wxSize(1, 12) : wxSize(1, -1));
    line_left->SetBackgroundColour(*wxBLACK);
    sizer->Add(line_left, 0, is_last ? wxALL : wxEXPAND | wxALL, 0);
    wxPanel *line_right = new wxPanel(panel, wxID_ANY, wxDefaultPosition, wxSize(10, 1));
    line_right->SetBackgroundColour(*wxBLACK);
    sizer->Add(line_right, 0, wxALL | wxALIGN_CENTER_VERTICAL, 0);
    sizer->Add(0, 0, 0, wxLEFT, 5);
    wxStaticText *preset_name = new wxStaticText(panel, wxID_ANY, from_u8(preset->name));
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " create child item: " << preset->name;
    preset_name->SetFont(Label::Body_10);
    preset_name->SetForegroundColour(*wxBLACK);
    sizer->Add(preset_name, 0, wxEXPAND | wxALL, 5);
    bool base_id_error = false;
    if (preset->inherits() == "" && preset->base_id != "") base_id_error = true;
    if (base_id_error) {
        std::string      wiki_url             = "https://wiki.bambulab.com/en/software/bambu-studio/custom-filament-issue";
        wxHyperlinkCtrl *m_download_hyperlink = new wxHyperlinkCtrl(panel, wxID_ANY, _L("[Delete Required]"), wiki_url, wxDefaultPosition, wxDefaultSize, wxHL_DEFAULT_STYLE);
        m_download_hyperlink->SetFont(Label::Body_10);
        sizer->Add(m_download_hyperlink, 0, wxEXPAND | wxALL, 5);
    }
    sizer->Add(0, 0, 1, wxEXPAND, 0);

    Button *edit_preset_btn = new Button(panel, _L("Edit Preset")); 
    edit_preset_btn->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    //edit_preset_btn->Hide();
    sizer->Add(edit_preset_btn, 0, wxALL | wxALIGN_CENTER_VERTICAL, 0);
    sizer->Add(0, 0, 0, wxLEFT, 5);

    Button *del_preset_btn = new Button(panel, _L("Delete Preset"));
    if (base_id_error) {
        del_preset_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Compact);
    } else {
        del_preset_btn->SetStyle(ButtonStyle::Alert,   ButtonType::Compact);
    }

    //del_preset_btn->Hide();
    sizer->Add(del_preset_btn, 0, wxALL | wxALIGN_CENTER_VERTICAL, 0);

    edit_preset_btn->Bind(wxEVT_BUTTON, [this, printer_name, preset_index](wxCommandEvent &e) {
        wxGetApp().CallAfter([this, printer_name, preset_index]() { edit_preset(printer_name, preset_index); });
    });
    del_preset_btn->Bind(wxEVT_BUTTON, [this, printer_name, preset_index](wxCommandEvent &e) {
        wxGetApp().CallAfter([this, printer_name, preset_index]() { delete_preset(printer_name, preset_index); });
    });

    panel->SetSizer(sizer);

    return panel;
}

void PresetTree::delete_preset(std::string printer_name, int need_delete_preset_index)
{
    m_parent_dialog->set_printer_name(printer_name);
    m_parent_dialog->set_need_delete_preset_index(need_delete_preset_index);
    m_parent_dialog->delete_preset();
}

void PresetTree::edit_preset(std::string printer_name, int need_edit_preset_index)
{
    m_parent_dialog->set_printer_name(printer_name);
    m_parent_dialog->set_need_edit_preset_index(need_edit_preset_index);
    m_parent_dialog->edit_preset();
}

} // namespace GUI

} // namespace Slic3r
