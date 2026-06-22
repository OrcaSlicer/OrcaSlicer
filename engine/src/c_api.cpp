// C ABI implementation for the orca engine.
//
// Declared in orca/c_api.h. This file is the only translation unit that bridges
// the POD C boundary to the C++ orca:: services. Boundary rules (per c_api.h):
//   - No C++ types or exceptions cross the boundary.
//   - Strings are UTF-8 const char*, borrowed unless documented otherwise.
//   - Opaque handles map 1:1 to orca:: classes returned by Session accessors.
//
// Every function catches all exceptions and converts them to an error code (or
// a null / false / 0 sentinel for non-error-code returns), recording the
// message in a thread-local retrievable via orca_last_error_message().

#include "orca/c_api.h"
#include "orca/Session.hpp"
#include "orca/Presets.hpp"
#include "orca/Project.hpp"
#include "orca/Slicer.hpp"
#include "orca/Export.hpp"
#include "orca/Events.hpp"
#include "orca/EventTypes.hpp"
#include "orca/Result.hpp"

#include <libslic3r/Config.hpp>
#include <libslic3r/PresetBundle.hpp>
#include <libslic3r/Preset.hpp>

#include <filesystem>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// §1 Handle casts
// ---------------------------------------------------------------------------
namespace {
inline orca::Session*        S (orca_session_t* h)         { return reinterpret_cast<orca::Session*>(h); }
inline orca::Presets*        P (orca_presets_t* h)         { return reinterpret_cast<orca::Presets*>(h); }
inline const orca::Presets*  P (const orca_presets_t* h)   { return reinterpret_cast<const orca::Presets*>(h); }
inline orca::Project*        PR(orca_project_t* h)         { return reinterpret_cast<orca::Project*>(h); }
inline const orca::Project*  PR(const orca_project_t* h)   { return reinterpret_cast<const orca::Project*>(h); }
inline orca::Slicer*         SL(orca_slicer_t* h)          { return reinterpret_cast<orca::Slicer*>(h); }
inline const orca::Slicer*   SL(const orca_slicer_t* h)    { return reinterpret_cast<const orca::Slicer*>(h); }
inline orca::Exporter*       EX(orca_exporter_t* h)        { return reinterpret_cast<orca::Exporter*>(h); }
inline orca::Events*         EV(orca_events_t* h)          { return reinterpret_cast<orca::Events*>(h); }
} // namespace

// ---------------------------------------------------------------------------
// §2 Error handling
// ---------------------------------------------------------------------------
namespace {
thread_local std::string g_last_error;
thread_local std::string g_str_scratch;     // for orca_presets_opt_string
thread_local std::string g_name_scratch;    // for orca_presets_current
thread_local std::string g_status_msg;      // for orca_slice_status_t.message
thread_local std::string g_status_err;      // for orca_slice_status_t.error

orca_error_code_t map_code(orca::ErrorCode c) {
    using E = orca::ErrorCode;
    switch (c) {
        case E::Unknown:         return ORCA_ERR_UNKNOWN;
        case E::InvalidArgument: return ORCA_ERR_INVALID_ARGUMENT;
        case E::NotFound:        return ORCA_ERR_NOT_FOUND;
        case E::AlreadyExists:   return ORCA_ERR_ALREADY_EXISTS;
        case E::IoError:         return ORCA_ERR_IO;
        case E::ParseError:      return ORCA_ERR_PARSE;
        case E::Cancelled:       return ORCA_ERR_CANCELLED;
        case E::Unsupported:      return ORCA_ERR_UNSUPPORTED;
        case E::NotImplemented:   return ORCA_ERR_UNSUPPORTED;
        case E::InvalidState:     return ORCA_ERR_UNKNOWN;
        case E::PermissionDenied: return ORCA_ERR_PERMISSION_DENIED;
    }
    return ORCA_ERR_UNKNOWN;
}
orca_error_code_t fail(const orca::Error& e) { g_last_error = e.message; return map_code(e.code); }
orca_error_code_t fail(orca_error_code_t code, std::string msg) { g_last_error = std::move(msg); return code; }

// ---------------------------------------------------------------------------
// Scope + preset-type mapping helpers
// ---------------------------------------------------------------------------
orca::ConfigScope to_scope(orca_config_scope_t s) {
    switch (s) {
        case ORCA_SCOPE_FULL:            return orca::ConfigScope::Full;
        case ORCA_SCOPE_PROJECT:         return orca::ConfigScope::Project;
        case ORCA_SCOPE_PRINT_PRESET:    return orca::ConfigScope::PrintPreset;
        case ORCA_SCOPE_PRINTER_PRESET:  return orca::ConfigScope::PrinterPreset;
        case ORCA_SCOPE_FILAMENT_PRESET: return orca::ConfigScope::FilamentPreset;
    }
    return orca::ConfigScope::Full;
}

orca::PresetType to_preset_type(orca_preset_type_t t) {
    switch (t) {
        case ORCA_PRESET_PRINT:            return orca::PresetType::Print;
        case ORCA_PRESET_PRINTER:          return orca::PresetType::Printer;
        case ORCA_PRESET_FILAMENT:         return orca::PresetType::Filament;
        case ORCA_PRESET_SLA_PRINT:        return orca::PresetType::SlaPrint;
        case ORCA_PRESET_SLA_MATERIAL:     return orca::PresetType::SlaMaterial;
        case ORCA_PRESET_PHYSICAL_PRINTER: return orca::PresetType::PhysicalPrinter;
    }
    return orca::PresetType::Print;
}

orca_preset_type_t to_c_preset_type(orca::PresetType t) {
    switch (t) {
        case orca::PresetType::Print:           return ORCA_PRESET_PRINT;
        case orca::PresetType::Printer:         return ORCA_PRESET_PRINTER;
        case orca::PresetType::Filament:        return ORCA_PRESET_FILAMENT;
        case orca::PresetType::SlaPrint:        return ORCA_PRESET_SLA_PRINT;
        case orca::PresetType::SlaMaterial:     return ORCA_PRESET_SLA_MATERIAL;
        case orca::PresetType::PhysicalPrinter: return ORCA_PRESET_PHYSICAL_PRINTER;
    }
    return ORCA_PRESET_PRINT;
}

// READ path. Resolves a scope to a libslic3r config via the bundle, mirroring
// Presets::config_for_scope (which is private). Returns nullptr + sets
// g_last_error if unavailable.
const Slic3r::ConfigBase* read_cfg(const orca::Presets* p, orca_config_scope_t scope) {
    if (!p->has_bundle()) { g_last_error = "no PresetBundle attached"; return nullptr; }
    Slic3r::PresetBundle& b = const_cast<Slic3r::PresetBundle&>(p->raw());
    switch (to_scope(scope)) {
        case orca::ConfigScope::Full: {
            // full_config() returns by value; own it in a thread_local so the
            // returned pointer stays valid for the duration of the C call.
            static thread_local Slic3r::DynamicPrintConfig full;
            full = b.full_config();
            return &full;
        }
        case orca::ConfigScope::Project:        return &b.project_config;
        case orca::ConfigScope::PrintPreset:    return &b.prints.get_edited_preset().config;
        case orca::ConfigScope::PrinterPreset:  return &b.printers.get_edited_preset().config;
        case orca::ConfigScope::FilamentPreset: return &b.filaments.get_edited_preset().config;
    }
    return nullptr;
}

// WRITE path. Full is a derived view and is not writable.
Slic3r::DynamicPrintConfig* write_cfg(orca::Presets* p, orca_config_scope_t scope, const char** err_msg) {
    if (!p->has_bundle()) { *err_msg = "no PresetBundle attached"; return nullptr; }
    Slic3r::PresetBundle& b = p->raw();
    switch (to_scope(scope)) {
        case orca::ConfigScope::Full:           *err_msg = "Full scope is a derived view, not writable"; return nullptr;
        case orca::ConfigScope::Project:        return &b.project_config;
        case orca::ConfigScope::PrintPreset:    return &b.prints.get_edited_preset().config;
        case orca::ConfigScope::PrinterPreset:  return &b.printers.get_edited_preset().config;
        case orca::ConfigScope::FilamentPreset: return &b.filaments.get_edited_preset().config;
    }
    *err_msg = "unknown scope"; return nullptr;
}
} // namespace

// ---------------------------------------------------------------------------
// §3 Definitions (C linkage)
// ---------------------------------------------------------------------------
extern "C" {

const char* orca_last_error_message(void) { return g_last_error.c_str(); }

// ---------- Session ----------

orca_session_t* orca_session_create(void) {
    try {
        auto up = orca::Session::create();
        return reinterpret_cast<orca_session_t*>(up.release());
    } catch (const std::exception& ex) {
        g_last_error = ex.what();
        return nullptr;
    } catch (...) {
        g_last_error = "unknown C++ exception";
        return nullptr;
    }
}

void orca_session_destroy(orca_session_t* session) {
    try {
        if (session) delete S(session);
    } catch (const std::exception& ex) {
        g_last_error = ex.what();
    } catch (...) {
        g_last_error = "unknown C++ exception";
    }
}

orca_presets_t* orca_session_presets(orca_session_t* session) {
    try {
        if (!session) return nullptr;
        return reinterpret_cast<orca_presets_t*>(&S(session)->presets());
    } catch (const std::exception& ex) {
        g_last_error = ex.what();
        return nullptr;
    } catch (...) {
        g_last_error = "unknown C++ exception";
        return nullptr;
    }
}

orca_project_t* orca_session_project(orca_session_t* session) {
    try {
        if (!session) return nullptr;
        return reinterpret_cast<orca_project_t*>(&S(session)->project());
    } catch (const std::exception& ex) {
        g_last_error = ex.what();
        return nullptr;
    } catch (...) {
        g_last_error = "unknown C++ exception";
        return nullptr;
    }
}

orca_slicer_t* orca_session_slicer(orca_session_t* session) {
    try {
        if (!session) return nullptr;
        return reinterpret_cast<orca_slicer_t*>(&S(session)->slicer());
    } catch (const std::exception& ex) {
        g_last_error = ex.what();
        return nullptr;
    } catch (...) {
        g_last_error = "unknown C++ exception";
        return nullptr;
    }
}

orca_exporter_t* orca_session_exporter(orca_session_t* session) {
    try {
        if (!session) return nullptr;
        return reinterpret_cast<orca_exporter_t*>(&S(session)->exporter());
    } catch (const std::exception& ex) {
        g_last_error = ex.what();
        return nullptr;
    } catch (...) {
        g_last_error = "unknown C++ exception";
        return nullptr;
    }
}

orca_events_t* orca_session_events(orca_session_t* session) {
    try {
        if (!session) return nullptr;
        return reinterpret_cast<orca_events_t*>(&S(session)->events());
    } catch (const std::exception& ex) {
        g_last_error = ex.what();
        return nullptr;
    } catch (...) {
        g_last_error = "unknown C++ exception";
        return nullptr;
    }
}

// ---------- Presets ----------

orca_error_code_t orca_presets_opt_int(const orca_presets_t* p, orca_config_scope_t scope, const char* key, int32_t* out) {
    try {
        if (!p || !key || !out) return fail(ORCA_ERR_INVALID_ARGUMENT, "null argument");
        const Slic3r::ConfigBase* cfg = read_cfg(P(p), scope);
        if (!cfg) return fail(ORCA_ERR_UNKNOWN, g_last_error);
        const Slic3r::ConfigOption* o = cfg->option(std::string{key});
        if (!o) return fail(ORCA_ERR_NOT_FOUND, "option not present");
        *out = static_cast<int32_t>(o->getInt());
        return ORCA_OK;
    } catch (const std::exception& ex) {
        return fail(ORCA_ERR_UNKNOWN, ex.what());
    } catch (...) {
        return fail(ORCA_ERR_UNKNOWN, "unknown C++ exception");
    }
}

orca_error_code_t orca_presets_opt_float(const orca_presets_t* p, orca_config_scope_t scope, const char* key, double* out) {
    try {
        if (!p || !key || !out) return fail(ORCA_ERR_INVALID_ARGUMENT, "null argument");
        const Slic3r::ConfigBase* cfg = read_cfg(P(p), scope);
        if (!cfg) return fail(ORCA_ERR_UNKNOWN, g_last_error);
        const Slic3r::ConfigOption* o = cfg->option(std::string{key});
        if (!o) return fail(ORCA_ERR_NOT_FOUND, "option not present");
        *out = o->getFloat();
        return ORCA_OK;
    } catch (const std::exception& ex) {
        return fail(ORCA_ERR_UNKNOWN, ex.what());
    } catch (...) {
        return fail(ORCA_ERR_UNKNOWN, "unknown C++ exception");
    }
}

orca_error_code_t orca_presets_opt_bool(const orca_presets_t* p, orca_config_scope_t scope, const char* key, bool* out) {
    try {
        if (!p || !key || !out) return fail(ORCA_ERR_INVALID_ARGUMENT, "null argument");
        const Slic3r::ConfigBase* cfg = read_cfg(P(p), scope);
        if (!cfg) return fail(ORCA_ERR_UNKNOWN, g_last_error);
        const Slic3r::ConfigOption* o = cfg->option(std::string{key});
        if (!o) return fail(ORCA_ERR_NOT_FOUND, "option not present");
        *out = o->getBool();
        return ORCA_OK;
    } catch (const std::exception& ex) {
        return fail(ORCA_ERR_UNKNOWN, ex.what());
    } catch (...) {
        return fail(ORCA_ERR_UNKNOWN, "unknown C++ exception");
    }
}

orca_error_code_t orca_presets_opt_string(const orca_presets_t* p, orca_config_scope_t scope, const char* key, const char** out_borrowed) {
    try {
        if (!p || !key || !out_borrowed) return fail(ORCA_ERR_INVALID_ARGUMENT, "null argument");
        const Slic3r::ConfigBase* cfg = read_cfg(P(p), scope);
        if (!cfg) return fail(ORCA_ERR_UNKNOWN, g_last_error);
        const Slic3r::ConfigOption* o = cfg->option(std::string{key});
        if (!o) return fail(ORCA_ERR_NOT_FOUND, "option not present");
        g_str_scratch = o->serialize();
        *out_borrowed = g_str_scratch.c_str();
        return ORCA_OK;
    } catch (const std::exception& ex) {
        return fail(ORCA_ERR_UNKNOWN, ex.what());
    } catch (...) {
        return fail(ORCA_ERR_UNKNOWN, "unknown C++ exception");
    }
}

orca_error_code_t orca_presets_set_int(orca_presets_t* p, orca_config_scope_t scope, const char* key, int32_t value) {
    try {
        if (!p || !key) return fail(ORCA_ERR_INVALID_ARGUMENT, "null argument");
        const char* em = nullptr;
        Slic3r::DynamicPrintConfig* cfg = write_cfg(P(p), scope, &em);
        if (!cfg)
            return fail(em && std::string(em).find("not writable") != std::string::npos ? ORCA_ERR_UNSUPPORTED : ORCA_ERR_UNKNOWN,
                        em ? em : "write unavailable");
        cfg->set_key_value(key, new Slic3r::ConfigOptionInt(value));
        return ORCA_OK;
    } catch (const std::exception& ex) {
        return fail(ORCA_ERR_UNKNOWN, ex.what());
    } catch (...) {
        return fail(ORCA_ERR_UNKNOWN, "unknown C++ exception");
    }
}

orca_error_code_t orca_presets_set_float(orca_presets_t* p, orca_config_scope_t scope, const char* key, double value) {
    try {
        if (!p || !key) return fail(ORCA_ERR_INVALID_ARGUMENT, "null argument");
        const char* em = nullptr;
        Slic3r::DynamicPrintConfig* cfg = write_cfg(P(p), scope, &em);
        if (!cfg)
            return fail(em && std::string(em).find("not writable") != std::string::npos ? ORCA_ERR_UNSUPPORTED : ORCA_ERR_UNKNOWN,
                        em ? em : "write unavailable");
        cfg->set_key_value(key, new Slic3r::ConfigOptionFloat(value));
        return ORCA_OK;
    } catch (const std::exception& ex) {
        return fail(ORCA_ERR_UNKNOWN, ex.what());
    } catch (...) {
        return fail(ORCA_ERR_UNKNOWN, "unknown C++ exception");
    }
}

orca_error_code_t orca_presets_set_bool(orca_presets_t* p, orca_config_scope_t scope, const char* key, bool value) {
    try {
        if (!p || !key) return fail(ORCA_ERR_INVALID_ARGUMENT, "null argument");
        const char* em = nullptr;
        Slic3r::DynamicPrintConfig* cfg = write_cfg(P(p), scope, &em);
        if (!cfg)
            return fail(em && std::string(em).find("not writable") != std::string::npos ? ORCA_ERR_UNSUPPORTED : ORCA_ERR_UNKNOWN,
                        em ? em : "write unavailable");
        cfg->set_key_value(key, new Slic3r::ConfigOptionBool(value));
        return ORCA_OK;
    } catch (const std::exception& ex) {
        return fail(ORCA_ERR_UNKNOWN, ex.what());
    } catch (...) {
        return fail(ORCA_ERR_UNKNOWN, "unknown C++ exception");
    }
}

orca_error_code_t orca_presets_set_string(orca_presets_t* p, orca_config_scope_t scope, const char* key, const char* value) {
    try {
        if (!p || !key || !value) return fail(ORCA_ERR_INVALID_ARGUMENT, "null argument");
        const char* em = nullptr;
        Slic3r::DynamicPrintConfig* cfg = write_cfg(P(p), scope, &em);
        if (!cfg)
            return fail(em && std::string(em).find("not writable") != std::string::npos ? ORCA_ERR_UNSUPPORTED : ORCA_ERR_UNKNOWN,
                        em ? em : "write unavailable");
        cfg->set_key_value(key, new Slic3r::ConfigOptionString(std::string{value}));
        return ORCA_OK;
    } catch (const std::exception& ex) {
        return fail(ORCA_ERR_UNKNOWN, ex.what());
    } catch (...) {
        return fail(ORCA_ERR_UNKNOWN, "unknown C++ exception");
    }
}

orca_error_code_t orca_presets_current(const orca_presets_t* p, orca_preset_type_t type, const char** out_name_borrowed) {
    try {
        if (!p || !out_name_borrowed) return fail(ORCA_ERR_INVALID_ARGUMENT, "null argument");
        if (!P(p)->has_bundle()) return fail(ORCA_ERR_UNKNOWN, "no PresetBundle attached");
        Slic3r::PresetBundle& b = const_cast<Slic3r::PresetBundle&>(P(p)->raw());
        switch (type) {
            case ORCA_PRESET_PRINT:        g_name_scratch = b.prints.get_selected_preset_name();        break;
            case ORCA_PRESET_PRINTER:      g_name_scratch = b.printers.get_selected_preset_name();      break;
            case ORCA_PRESET_FILAMENT:     g_name_scratch = b.filaments.get_selected_preset_name();     break;
            case ORCA_PRESET_SLA_PRINT:    g_name_scratch = b.sla_prints.get_selected_preset_name();    break;
            case ORCA_PRESET_SLA_MATERIAL: g_name_scratch = b.sla_materials.get_selected_preset_name(); break;
            case ORCA_PRESET_PHYSICAL_PRINTER:
                g_name_scratch = b.physical_printers.get_selected_full_printer_name();
                break;
            default:
                return fail(ORCA_ERR_INVALID_ARGUMENT, "unknown preset type");
        }
        *out_name_borrowed = g_name_scratch.c_str();
        return ORCA_OK;
    } catch (const std::exception& ex) {
        return fail(ORCA_ERR_UNKNOWN, ex.what());
    } catch (...) {
        return fail(ORCA_ERR_UNKNOWN, "unknown C++ exception");
    }
}

orca_error_code_t orca_presets_set_current(orca_presets_t* p, orca_preset_type_t type, const char* name) {
    try {
        if (!p || !name) return fail(ORCA_ERR_INVALID_ARGUMENT, "null argument");
        auto r = P(p)->set_current(to_preset_type(type), std::string_view{name});
        if (!r) return fail(r.error());
        return ORCA_OK;
    } catch (const std::exception& ex) {
        return fail(ORCA_ERR_UNKNOWN, ex.what());
    } catch (...) {
        return fail(ORCA_ERR_UNKNOWN, "unknown C++ exception");
    }
}

orca_error_code_t orca_presets_set_filament(orca_presets_t* p, int32_t extruder_idx, const char* name) {
    try {
        if (!p || !name) return fail(ORCA_ERR_INVALID_ARGUMENT, "null argument");
        auto r = P(p)->set_filament(static_cast<int>(extruder_idx), std::string_view{name});
        if (!r) return fail(r.error());
        return ORCA_OK;
    } catch (const std::exception& ex) {
        return fail(ORCA_ERR_UNKNOWN, ex.what());
    } catch (...) {
        return fail(ORCA_ERR_UNKNOWN, "unknown C++ exception");
    }
}

int32_t orca_presets_extruder_count(const orca_presets_t* p) {
    try {
        if (!p) return 0;
        return static_cast<int32_t>(P(p)->extruder_count());
    } catch (const std::exception& ex) {
        g_last_error = ex.what();
        return 0;
    } catch (...) {
        g_last_error = "unknown C++ exception";
        return 0;
    }
}

bool orca_presets_is_bbl_vendor(const orca_presets_t* p) {
    try {
        if (!p) return false;
        return P(p)->is_bbl_vendor();
    } catch (const std::exception& ex) {
        g_last_error = ex.what();
        return false;
    } catch (...) {
        g_last_error = "unknown C++ exception";
        return false;
    }
}

// ---------- Project ----------

orca_error_code_t orca_project_load_file(orca_project_t* p, const char* path, const orca_load_options_t* options, orca_load_handle_t* out_handle) {
    try {
        if (!p || !path || !options || !out_handle) return fail(ORCA_ERR_INVALID_ARGUMENT, "null argument");
        orca::LoadOptions opts;
        opts.strategy = static_cast<orca::LoadStrategy>(options->strategy);
        opts.plate_id = options->plate_id;
        std::vector<std::filesystem::path> paths{ std::filesystem::path(path) };
        auto r = PR(p)->load_files(paths, opts);
        if (!r) return fail(r.error());
        *out_handle = r.value();
        return ORCA_OK;
    } catch (const std::exception& ex) {
        return fail(ORCA_ERR_UNKNOWN, ex.what());
    } catch (...) {
        return fail(ORCA_ERR_UNKNOWN, "unknown C++ exception");
    }
}

orca_error_code_t orca_project_save(orca_project_t* p, const char* path) {
    try {
        if (!p || !path) return fail(ORCA_ERR_INVALID_ARGUMENT, "null argument");
        auto r = PR(p)->save(std::filesystem::path(path));
        return r ? ORCA_OK : fail(r.error());
    } catch (const std::exception& ex) {
        return fail(ORCA_ERR_UNKNOWN, ex.what());
    } catch (...) {
        return fail(ORCA_ERR_UNKNOWN, "unknown C++ exception");
    }
}

size_t orca_project_object_count(const orca_project_t* p) {
    try {
        if (!p) return 0;
        return PR(p)->object_count();
    } catch (const std::exception& ex) {
        g_last_error = ex.what();
        return 0;
    } catch (...) {
        g_last_error = "unknown C++ exception";
        return 0;
    }
}

orca_error_code_t orca_project_object_id_at(const orca_project_t* p, size_t idx, orca_object_id_t* out_id) {
    try {
        if (!p || !out_id) return fail(ORCA_ERR_INVALID_ARGUMENT, "null argument");
        // objects() is non-const; the C handle is logically const but the
        // underlying enumeration mutates nothing observable.
        auto objs = const_cast<orca::Project*>(PR(p))->objects();
        if (idx >= objs.size()) return fail(ORCA_ERR_NOT_FOUND, "index out of range");
        *out_id = objs[idx].id();
        return ORCA_OK;
    } catch (const std::exception& ex) {
        return fail(ORCA_ERR_UNKNOWN, ex.what());
    } catch (...) {
        return fail(ORCA_ERR_UNKNOWN, "unknown C++ exception");
    }
}

orca_error_code_t orca_project_remove_object(orca_project_t* p, orca_object_id_t id) {
    try {
        if (!p) return fail(ORCA_ERR_INVALID_ARGUMENT, "null argument");
        auto r = PR(p)->remove_object(id);
        return r ? ORCA_OK : fail(r.error());
    } catch (const std::exception& ex) {
        return fail(ORCA_ERR_UNKNOWN, ex.what());
    } catch (...) {
        return fail(ORCA_ERR_UNKNOWN, "unknown C++ exception");
    }
}

orca_error_code_t orca_project_clear(orca_project_t* p) {
    try {
        if (!p) return fail(ORCA_ERR_INVALID_ARGUMENT, "null argument");
        auto r = PR(p)->clear();
        return r ? ORCA_OK : fail(r.error());
    } catch (const std::exception& ex) {
        return fail(ORCA_ERR_UNKNOWN, ex.what());
    } catch (...) {
        return fail(ORCA_ERR_UNKNOWN, "unknown C++ exception");
    }
}

// ---------- Slicer ----------

orca_error_code_t orca_slicer_request(orca_slicer_t* s, const orca_slice_params_t* params, orca_slice_handle_t* out_handle) {
    try {
        if (!s || !params || !out_handle) return fail(ORCA_ERR_INVALID_ARGUMENT, "null argument");
        orca::SliceParams sp;
        sp.plate_id   = params->plate_id;
        sp.technology = (params->technology == ORCA_TECH_SLA) ? orca::SliceTechnology::SLA : orca::SliceTechnology::FFF;
        auto r = SL(s)->request_slice(sp);
        if (!r) return fail(r.error());
        *out_handle = r.value();
        return ORCA_OK;
    } catch (const std::exception& ex) {
        return fail(ORCA_ERR_UNKNOWN, ex.what());
    } catch (...) {
        return fail(ORCA_ERR_UNKNOWN, "unknown C++ exception");
    }
}

void orca_slicer_cancel(orca_slicer_t* s, orca_slice_handle_t handle) {
    try {
        if (s) SL(s)->cancel(handle);
    } catch (const std::exception& ex) {
        g_last_error = ex.what();
    } catch (...) {
        g_last_error = "unknown C++ exception";
    }
}

void orca_slicer_status(const orca_slicer_t* s, orca_slice_handle_t handle, orca_slice_status_t* out) {
    try {
        if (!s || !out) return;
        orca::SliceStatus st = SL(s)->status(handle);
        switch (st.state) {
            case orca::SliceState::NotStarted: out->state = ORCA_SLICE_NOT_STARTED; break;
            case orca::SliceState::Queued:     out->state = ORCA_SLICE_QUEUED;      break;
            case orca::SliceState::Running:    out->state = ORCA_SLICE_RUNNING;     break;
            case orca::SliceState::Completed:  out->state = ORCA_SLICE_COMPLETED;   break;
            case orca::SliceState::Cancelled:  out->state = ORCA_SLICE_CANCELLED;   break;
            case orca::SliceState::Failed:     out->state = ORCA_SLICE_FAILED;      break;
        }
        out->progress = st.progress;
        g_status_msg  = st.message;
        g_status_err  = st.error;
        out->message  = g_status_msg.c_str();
        out->error    = g_status_err.c_str();
    } catch (const std::exception& ex) {
        g_last_error = ex.what();
    } catch (...) {
        g_last_error = "unknown C++ exception";
    }
}

bool orca_slicer_is_busy(const orca_slicer_t* s) {
    try {
        if (!s) return false;
        return SL(s)->is_busy();
    } catch (const std::exception& ex) {
        g_last_error = ex.what();
        return false;
    } catch (...) {
        g_last_error = "unknown C++ exception";
        return false;
    }
}

// ---------- Exporter ----------

orca_error_code_t orca_exporter_export(orca_exporter_t* e, const orca_export_params_t* params, orca_export_handle_t* out_handle) {
    try {
        if (!e || !params || !params->output_path || !out_handle) return fail(ORCA_ERR_INVALID_ARGUMENT, "null argument");
        orca::ExportParams ep;
        ep.output_path     = std::filesystem::path(params->output_path);
        ep.plate_id        = params->plate_id;
        ep.send_to_printer = params->send_to_printer;
        auto r = EX(e)->export_gcode(ep);
        if (!r) return fail(r.error());
        *out_handle = r.value();
        return ORCA_OK;
    } catch (const std::exception& ex) {
        return fail(ORCA_ERR_UNKNOWN, ex.what());
    } catch (...) {
        return fail(ORCA_ERR_UNKNOWN, "unknown C++ exception");
    }
}

void orca_exporter_cancel(orca_exporter_t* e, orca_export_handle_t handle) {
    try {
        if (e) EX(e)->cancel(handle);
    } catch (const std::exception& ex) {
        g_last_error = ex.what();
    } catch (...) {
        g_last_error = "unknown C++ exception";
    }
}

// ---------- Events ----------

orca_subscription_id_t orca_events_subscribe(orca_events_t* events, orca_event_kind_t kind, orca_event_callback_t cb, void* user_data) {
    try {
        if (!events || !cb) return 0;
        orca::Events* ev = EV(events);
        switch (kind) {
            case ORCA_EVT_SLICING_PROGRESS:
                return ev->subscribe<orca::SlicingProgress>(
                    [cb, user_data](const orca::SlicingProgress& e) {
                        orca_evt_slicing_progress_t payload{ e.handle, e.progress, e.message.c_str() };
                        cb(ORCA_EVT_SLICING_PROGRESS, &payload, user_data);
                    });
            case ORCA_EVT_SLICING_FINISHED:
                return ev->subscribe<orca::SlicingFinished>(
                    [cb, user_data](const orca::SlicingFinished& e) {
                        orca_evt_slicing_finished_t payload{ e.handle, e.success, e.error.c_str() };
                        cb(ORCA_EVT_SLICING_FINISHED, &payload, user_data);
                    });
            case ORCA_EVT_EXPORT_BEGAN:
                return ev->subscribe<orca::ExportBegan>(
                    [cb, user_data](const orca::ExportBegan& e) {
                        std::string sp = e.path.string();
                        orca_evt_export_began_t payload{ e.handle, sp.c_str() };
                        cb(ORCA_EVT_EXPORT_BEGAN, &payload, user_data);
                    });
            case ORCA_EVT_EXPORT_FINISHED:
                return ev->subscribe<orca::ExportFinished>(
                    [cb, user_data](const orca::ExportFinished& e) {
                        orca_evt_export_finished_t payload{ e.handle, e.success, e.line_count, e.error.c_str() };
                        cb(ORCA_EVT_EXPORT_FINISHED, &payload, user_data);
                    });
            case ORCA_EVT_PRESET_CHANGED:
                return ev->subscribe<orca::PresetChanged>(
                    [cb, user_data](const orca::PresetChanged& e) {
                        orca_evt_preset_changed_t payload{ to_c_preset_type(e.type), e.name.c_str() };
                        cb(ORCA_EVT_PRESET_CHANGED, &payload, user_data);
                    });
            case ORCA_EVT_PROJECT_LOADED:
                return ev->subscribe<orca::ProjectLoaded>(
                    [cb, user_data](const orca::ProjectLoaded& e) {
                        std::string sp = e.path.string();
                        orca_evt_project_loaded_t payload{ e.handle, sp.c_str() };
                        cb(ORCA_EVT_PROJECT_LOADED, &payload, user_data);
                    });
            case ORCA_EVT_OBJECT_ADDED:
                return ev->subscribe<orca::ObjectAdded>(
                    [cb, user_data](const orca::ObjectAdded& e) {
                        orca_evt_object_added_t payload{ e.id };
                        cb(ORCA_EVT_OBJECT_ADDED, &payload, user_data);
                    });
            case ORCA_EVT_OBJECT_REMOVED:
                return ev->subscribe<orca::ObjectRemoved>(
                    [cb, user_data](const orca::ObjectRemoved& e) {
                        orca_evt_object_removed_t payload{ e.id };
                        cb(ORCA_EVT_OBJECT_REMOVED, &payload, user_data);
                    });
        }
        return 0; // unknown kind
    } catch (const std::exception& ex) {
        g_last_error = ex.what();
        return 0;
    } catch (...) {
        g_last_error = "unknown C++ exception";
        return 0;
    }
}

void orca_events_unsubscribe(orca_events_t* events, orca_subscription_id_t id) {
    try {
        if (events) EV(events)->unsubscribe(id);
    } catch (const std::exception& ex) {
        g_last_error = ex.what();
    } catch (...) {
        g_last_error = "unknown C++ exception";
    }
}

} // extern "C"
