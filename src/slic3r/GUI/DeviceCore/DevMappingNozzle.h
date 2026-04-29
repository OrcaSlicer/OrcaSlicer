/**
* @file  DevMappingNozzle.h
* @brief Support nozzle mapping for hotend rack
*/

#pragma once
#include <string>
#include <unordered_map>
#include <nlohmann/json.hpp>

#include <wx/string.h>

namespace Slic3r
{

namespace GUI
{
class Plater;
}

class MachineObject;
class DevNozzleMappingCtrl
{
    friend class MachineObject;
public:
    enum class NozzleMappingVersion : int {
        Version_V0 = 0, // V0 means support logical fila_id -> physical nozzle_id mapping
        Version_V1 = 1, // V1 means support logical nozzle_id -> physical nozzle_id mapping
    };

    enum class ErrNoV1 : int {
        Success = 0,
        ErrorBadRequest3mf = 1,
        ErrorBadRequestVersion = 2,
        ErrorBadRequestGroupId = 3,
        ErrorNoValidNozzles = 4,
        ErrorBadRequestReadFail = 5,
        ErrorRackCaliFail = 6,
        ErrorRackMoving = 7,
        ErrorRackFull = 8,
        WarningNotEnoughNozzles = 9
    };

public:
    DevNozzleMappingCtrl(MachineObject* obj) : m_obj(obj) {};

public:
    void SetPlater(Slic3r::GUI::Plater* plater) { m_plater = plater; }
    void Clear();

    bool HasResult() const { return !m_result.empty(); }
    std::string GetResultStr() const { return m_result; }

    // mqtt error info
    std::string GetMqttReason() const { return m_mqtt_reason; }

    // command error info
    int GetErrno() const { return m_errno; }
    ErrNoV1 GetErrnoV1() const { return static_cast<ErrNoV1>(m_errno); }
    std::string GetDetailMsg() const { return m_detail_msg; }

    // nozzle mapping
    std::unordered_map<int, int> GetNozzleMappingMap() const { return m_nozzle_mapping; }
    nlohmann::json GetNozzleMappingJson() const { return m_nozzle_mapping_json; }
    void SetManualNozzleMappingByFila(int fila_id, int nozzle_pos_id);

    std::vector<int> GetMappedNozzlePosVecByFilaId(int fila_id) const;// return empy if not mapped
    wxString GetMappedNozzlePosStrByFilaId(int fila_id, const wxString& default_str = "?") const;

    // flush weight
    float  GetFlushWeightBase() const { return m_flush_weight_base; }
    float  GetFlushWeightCurrent() const { return m_flush_weight_current; }

public:
    void ParseAutoNozzleMapping(const nlohmann::json& print_jj);
    int CtrlGetAutoNozzleMappingV0(Slic3r::GUI::Plater* plater, const std::vector<FilamentInfo>& ams_mapping, int flow_cali_opt, int pa_value);
    int CtrlGetAutoNozzleMappingV1(Slic3r::GUI::Plater* plater);

private:
    float  GetFlushWeight(Slic3r::MachineObject* obj) const;

private:
    MachineObject* m_obj;
    Slic3r::GUI::Plater* m_plater = nullptr;

    std::string m_sequence_id;

    std::string m_result;
    std::string m_mqtt_reason;
    std::string m_type; // auto or manual

    int         m_errno;
    std::string m_detail_msg;
    nlohmann::json m_detail_json;

    nlohmann::json m_nozzle_mapping_json;
    std::unordered_map<int, int> m_nozzle_mapping;  // v0: fila_id -> physical_nozzle_id, v1: logical_nozzle_id -> physical_nozzle_id

    float m_flush_weight_base = -1;// the base weight for flush
    float m_flush_weight_current = -1;// the weight current

    NozzleMappingVersion m_req_version = NozzleMappingVersion::Version_V0;
    NozzleMappingVersion m_ack_version = NozzleMappingVersion::Version_V0;
};

}
