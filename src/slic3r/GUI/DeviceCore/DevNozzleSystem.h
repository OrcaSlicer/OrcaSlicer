#pragma once
#include "libslic3r/CommonDefs.hpp"
#include "slic3r/Utils/json_diff.hpp"

#include <wx/string.h>
#include <map>

namespace Slic3r
{
    // Previous definitions
   class MachineObject;

   struct DevNozzle
   {
       int             m_nozzle_id = -1;
       NozzleFlowType  m_nozzle_flow = NozzleFlowType::S_FLOW;// 0-common 1-high flow
       NozzleType      m_nozzle_type = NozzleType::ntUndefine;// 0-stainless_steel 1-hardened_steel 5-tungsten_carbide
       float           m_diameter = 0.4f;// 0.2mm  0.4mm  0.6mm 0.8mm
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