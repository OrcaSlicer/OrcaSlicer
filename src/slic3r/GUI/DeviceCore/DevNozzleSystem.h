#pragma once
#include "DevDefs.h"
#include "DevFirmware.h"

#include "libslic3r/CommonDefs.hpp"
#include "libslic3r/MultiNozzleUtils.hpp"
#include "slic3r/Utils/json_diff.hpp"

#include <wx/string.h>
#include <map>
#include <memory>

// Device flow-type mapping note (nozzle rack): the device U_FLOW value maps to nvtTPUHighFlow
// (TPU High Flow), so a device-synced TPU-HF rack resolves to nvtTPUHighFlow and the H2C
// change_filament_gcode TPU-kit branch can activate. nvtHybrid is a slicer-only sentinel with no
// device representation, so it stays out of these device flow-type conversions.
//
// GetExtruderNozzleInfo (and its ExtruderNozzleInfos aggregate) is intentionally omitted here;
// the SelectMachine slicing-vs-installed nozzle comparison collects its per-extruder NozzleDef
// entries itself (s_get_slicing_extuder_nozzles).

namespace Slic3r
{
    // Previous definitions
   class MachineObject;
   class DevNozzleRack;

   struct DevNozzle
   {
       friend class DevNozzleSystemParser;

   public:
       int             m_nozzle_id = -1;
       NozzleFlowType  m_nozzle_flow = NozzleFlowType::S_FLOW;// 0-common 1-high flow
       NozzleType      m_nozzle_type = NozzleType::ntUndefine;// 0-stainless_steel 1-hardened_steel 5-tungsten_carbide
       float           m_diameter = 0.0f;// unknown until reported by the printer

   public:
       // flow/volume conversions (Standard / High Flow / TPU High Flow)
       static NozzleFlowType       ToNozzleFlowType(const NozzleVolumeType& type);
       static NozzleVolumeType     ToNozzleVolumeType(const NozzleFlowType& type);

       static wxString             GetNozzleFlowTypeStr(NozzleFlowType type);
       static std::string          GetNozzleFlowTypeString(NozzleFlowType type);// untranslated literal ("High Flow"/"Standard"/"TPU High Flow") — the filament blacklist JSON matches these raw strings, so it must NOT use the translated GetNozzleFlowTypeStr (would break non-English locales)
       static std::string          ToNozzleFlowString(const NozzleFlowType& type);// untranslated "Standard"/"High Flow"/"TPU High Flow" ("" for none) — the raw literal serialized into the get_auto_nozzle_mapping payload
       static wxString             GetNozzleTypeStr(NozzleType type);

   public:
       bool     IsEmpty() const { return m_nozzle_id < 0; }

       void     SetRack(const std::weak_ptr<DevNozzleRack>& rack) { m_nozzle_rack = rack; }

       int            GetNozzleId() const { return m_nozzle_id; }
       int            GetNozzlePosId() const;// physical position id: rack nozzle -> id + 0x10, else id
       NozzleType     GetNozzleType() const { return m_nozzle_type; }
       NozzleFlowType GetNozzleFlowType() const { return m_nozzle_flow; }
       NozzleDiameterType GetNozzleDiameterType() const;
       float          GetNozzleDiameter() const { return m_diameter; }
       float          GetNozzleWear() const { return m_wear; }
       int            GetNozzlePrintTime() const { return m_nozzle_print_time; }

       // firmware (rack hotend WTM / extruder nozzle firmware)
       DevFirmwareVersionInfo GetFirmwareInfo() const;

       // display
       wxString GetNozzleDiameterStr() const { return wxString::Format("%.1f mm", m_diameter); }
       wxString GetNozzleFlowTypeStr() const { return GetNozzleFlowTypeStr(m_nozzle_flow); }
       wxString GetNozzleTypeStr() const { return GetNozzleTypeStr(m_nozzle_type); }

       std::string GetFilamentId() const { return m_fila_id; }
       std::string GetFilamentColor() const { return m_filament_clr; }

       // location
       bool AtLeftExtruder() const;
       bool AtRightExtruder() const;

       int  GetLogicExtruderId() const;// warning: logical extruder id
       int  GetExtruderId() const;// warning: physical extruder id

       /* holder nozzle */
       bool IsOnRack() const { return m_on_rack; }
       bool IsInfoReliable() const;

       bool IsNormal() const;
       bool IsAbnormal() const;
       bool IsUnknown() const;

       void SetOnRack(bool on_rack) { m_on_rack = on_rack; }
       void SetStatus(int stat) { m_stat = stat; }

   private:
       int  GetTotalExtruderCount() const;

   private:
       bool m_on_rack = false;

       int   m_stat = 0;
       float m_wear = 0.0f;

       std::string m_fila_id;      // main material
       std::string m_filament_clr; // main color

       std::weak_ptr<DevNozzleRack> m_nozzle_rack; // weak pointer to the nozzle rack
       int m_nozzle_print_time{0};
   };

   class DevNozzleSystem
   {
       friend class DevNozzleSystemParser;
   private:
       enum Status : int
       {
           NOZZLE_SYSTEM_IDLE = 0,
           NOZZLE_SYSTEM_REFRESHING = 1,
       };

   public:
       DevNozzleSystem(MachineObject* owner);
       ~DevNozzleSystem() = default;

   public:
       MachineObject* GetOwner() const { return m_owner; }

       // nozzle by position id: pos_id < 0x10 -> extruder nozzle, else rack nozzle at (pos_id - 0x10)
       DevNozzle                       GetNozzleByPosId(int pos_id) const { return pos_id < 0x10 ? GetExtNozzle(pos_id) : GetRackNozzle(pos_id - 0x10); }

       // nozzles on extruder
       bool                            ContainsExtNozzle(int id) const { return m_ext_nozzles.find(id) != m_ext_nozzles.end(); }
       DevNozzle                       GetExtNozzle(int id) const;
       const std::map<int, DevNozzle>& GetExtNozzles() const { return m_ext_nozzles; }
       int                             GetExtNozzleCount() const { return (int) m_ext_nozzles.size(); }

       // nozzles on rack
       void  SetSupportNozzleRack(bool supported);
       std::shared_ptr<DevNozzleRack>  GetNozzleRack() const { return m_nozzle_rack; }
       DevNozzle                       GetRackNozzle(int idx) const;
       const std::map<int, DevNozzle>& GetRackNozzles() const;

       // nozzles on extruder and rack
       bool IsRackMaximumInstalled() const;// true when the main extruder + all 6 rack slots hold nozzles

       // grouping (drives the MultiNozzleSyncDialog options)
       const std::vector<DevNozzle> CollectNozzles(int ext_loc, NozzleFlowType flow_type, float diameter = -1.0f) const;
       std::vector<MultiNozzleUtils::NozzleGroupInfo> GetNozzleGroups() const;

       bool  IsIdle() const { return m_state_0_4 == NOZZLE_SYSTEM_IDLE; }
       bool  IsRefreshing() const { return m_state_0_4 == NOZZLE_SYSTEM_REFRESHING; }

       bool  HasUnreliableNozzles() const;// any extruder or rack nozzle whose reported info is not reliable
       bool  HasUnknownNozzles() const;   // any extruder or rack nozzle of unknown state

       /* reading */
       int GetReadingIdx() const { return m_reading_idx; }
       int GetReadingCount() const { return m_reading_count; }

       /* firmware */
       void AddFirmwareInfoWTM(const DevFirmwareVersionInfo& info);// route a "wtm/<id>" module version to the rack nozzle, else to the extruder nozzle
       void ClearFirmwareInfoWTM();
       DevFirmwareVersionInfo GetExtruderNozzleFirmware() const { return m_ext_nozzle_firmware_info; }

       /* replace nozzle (device reports src/tar position while a rack nozzle stands in for the toolhead) */
       std::optional<int> GetReplaceNozzleSrc() const { return m_replace_nozzle_src; }
       std::optional<int> GetReplaceNozzleTar() const { return m_replace_nozzle_tar; }

   private:
       void ClearNozzles();

   private:
       MachineObject* m_owner = nullptr;

       int m_extder_exist = 0;  //0- none exist 1-exist, unused
       int m_state_0_4 = 0;     //0-idle 1-refreshing

       std::optional<int> m_replace_nozzle_src; // replace nozzle source position (device-reported)
       std::optional<int> m_replace_nozzle_tar; // replace nozzle target position (device-reported)

       /* refreshing */
       int m_reading_idx = 0;
       int m_reading_count = 0;

       // nozzles on extruder
       std::map<int, DevNozzle> m_ext_nozzles;
       DevFirmwareVersionInfo m_ext_nozzle_firmware_info;

       // nozzles on rack
       std::shared_ptr<DevNozzleRack> m_nozzle_rack;
   };

   class DevNozzleSystemParser
   {
   public:
       static void  ParseV1_0(const nlohmann::json& nozzletype_json, const nlohmann::json& diameter_json, DevNozzleSystem* system, std::optional<int> flag_e3d);
       static void  ParseV2_0(const json& device_json, DevNozzleSystem* system);
   };
};
