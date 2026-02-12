#pragma once
#include "DevDefs.h"
#include "DevFirmware.h"

#include "libslic3r/CommonDefs.hpp"
#include "libslic3r/MultiNozzleUtils.hpp"
#include "slic3r/Utils/json_diff.hpp"

#include <wx/string.h>
#include <map>

namespace Slic3r
{
    // Previous definitions
   class MachineObject;
   class DevNozzleRack;

   struct DevNozzle
   {
   private:
       friend class DevNozzleSystemParser;

   public:
       int             m_nozzle_id = -1;
       NozzleFlowType  m_nozzle_flow = NozzleFlowType::S_FLOW;// 0-common 1-high flow
       NozzleType      m_nozzle_type = NozzleType::ntUndefine;// 0-stainless_steel 1-hardened_steel 5-tungsten_carbide
       float           m_diameter = 0.4f;// 0.2mm  0.4mm  0.6mm 0.8mm

    public:
        static wxString             GetNozzleFlowTypeStr(NozzleFlowType type);
        static std::string          GetNozzleFlowTypeString(NozzleFlowType type);// no translation
        static NozzleFlowType       ToNozzleFlowType(const std::string& type);
        static NozzleFlowType       VariantToNozzleFlowType(const std::string& variant);
        static std::string          ToNozzleFlowString(const NozzleFlowType& type);

        static wxString             GetNozzleTypeStr(NozzleType type);
        static std::string          GetNozzleTypeString(NozzleType type);

        static NozzleFlowType       ToNozzleFlowType(const NozzleVolumeType& type);
        static wxString             GetNozzleVolumeTypeStr(const NozzleVolumeType& type);
        static NozzleVolumeType     ToNozzleVolumeType(const NozzleFlowType& type);
        static std::string          ToNozzleVolumeString(const NozzleVolumeType& type);
        static std::string          ToNozzleVolumeShortString(const NozzleVolumeType& type);

        static float                ToNozzleDiameterFloat(const NozzleDiameterType& type);
        static NozzleDiameterType   ToNozzleDiameterType(float diameter);
        static wxString             ToNozzleDiameterStr(const NozzleDiameterType& type);

   public:
       bool     IsEmpty() const { return m_nozzle_id < 0; }

       /**/
       void       SetRack(const std::weak_ptr<DevNozzleRack>& rack) { m_nozzle_rack = rack; };

       /**/
       int            GetNozzlePosId() const;
       int            GetNozzleId() const { return m_nozzle_id; }
       NozzleType     GetNozzleType() const { return m_nozzle_type; }
       NozzleFlowType GetNozzleFlowType() const { return m_nozzle_flow; }
       NozzleDiameterType GetNozzleDiameterType() const;
       float          GetNozzleDiameter() const { return m_diameter; }
       float          GetNozzleWear() const { return m_wear; }

       // display
       wxString GetDisplayId() const;
       wxString GetNozzleDiameterStr() const {  return wxString::Format("%.1f mm", m_diameter);}
       wxString GetNozzleFlowTypeStr() const;
       wxString GetNozzleTypeStr() const;

       // serial number
       wxString GetSerialNumber() const { return GetFirmwareInfo().sn; }
       DevFirmwareVersionInfo GetFirmwareInfo() const;

       // location
       bool AtLeftExtruder() const;
       bool AtRightExtruder() const;
       int  GetLogicExtruderId() const;// warning: logical extruder id

       /* holder nozzle*/
       bool IsOnRack() const { return m_on_rack; }
       bool IsInfoReliable() const;

       bool IsNormal() const;
       bool IsAbnormal() const;
       bool IsUnknown() const;

       std::string GetFilamentId() const { return m_fila_id; }
       std::string GetFilamentColor() const { return m_filament_clr; }

       void SetOnRack(bool on_rack) { m_on_rack = on_rack; };
       void SetStatus(int stat) { m_stat = stat; }
   private:
       int  GetTotalExtruderCount() const;

   private:
       bool m_on_rack = false;

       int m_stat = 0;
       float m_wear;

       std::string m_sn;
       std::string m_fila_id;// main material
       std::string m_filament_clr;// main color

       std::weak_ptr<DevNozzleRack> m_nozzle_rack; // weak pointer to the nozzle rack
   };

   class DevNozzleSystem
   {
       friend class DevNozzleSystemParser;
   public:
       DevNozzleSystem(MachineObject* owner) : m_owner(owner) {}
   private:
       enum Status : int
       {
           NOZZLE_SYSTEM_IDLE = 0,
           NOZZLE_SYSTEM_REFRESHING = 1,
       };

   public:
       bool                            ContainsNozzle(int id) const { return m_nozzles.find(id) != m_nozzles.end(); }
       DevNozzle                       GetNozzle(int id) const;
       const std::map<int, DevNozzle>& GetNozzles() const { return m_nozzles;}
       bool                            IsRefreshing() const { return m_state == 1; }

   private:
       void Reset();
       void ClearNozzles();

   private:
       MachineObject* m_owner = nullptr;

       int                          m_extder_exist = 0;  //0- none exist 1-exist, unused
       int                          m_state = 0; //0-idle 1-checking, unused
       std::map<int, DevNozzle> m_nozzles;

   };

   class DevNozzleSystemParser
   {
   public:
       static void  ParseV1_0(const nlohmann::json& nozzletype_json, const nlohmann::json& diameter_json, DevNozzleSystem* system, std::optional<int> flag_e3d);
       static void  ParseV2_0(const json& nozzle_json, DevNozzleSystem* system);
   };
};