#ifndef slic3r_PrinterAgentPluginCapability_hpp_
#define slic3r_PrinterAgentPluginCapability_hpp_

#include "../../PythonPluginInterface.hpp"

#include "IPrinterAgent.hpp"

#include <memory>
#include <string>

namespace Slic3r {

// A printer-agent plugin capability implements IPrinterAgent directly: the host
// drives it through the native IPrinterAgent surface and the Python plugin
// overrides the individual operations. The capability is registered with the
// NetworkAgentFactory and handed out as the live IPrinterAgent for the selected
// printer agent.
class PrinterAgentPluginCapability : public PluginCapabilityInterface, public IPrinterAgent
{
public:
    static void RegisterBindings(pybind11::module_& module);

    PluginCapabilityType get_type() const override { return PluginCapabilityType::PrinterConnection; }

    // set_cloud_agent is the host-managed dependency injection point — the host hands the
    // capability its ICloudServiceAgent — so it is the one operation kept native here. Optional
    // operations inherit IPrinterAgent's defaults; the remaining operations are plugin-defined.
    void set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud) final override { (void) cloud; }

    AgentInfo get_agent_info() override = 0;

    int connect_printer(const PrinterConnectionParams& params) override                                                                 = 0;
    int send_message(std::string dev_id, std::string json_str, int qos, int flag) override                                              = 0;
    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override                                   = 0;
    bool start_discovery(bool start, bool sending) override                                                                             = 0;
    int bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect) override                                            = 0;
    std::string get_user_selected_machine() override                                                                                    = 0;
    int set_user_selected_machine(std::string dev_id) override                                                                          = 0;
    int start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override = 0;
    int start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override                            = 0;

    int set_server_callback(OnServerErrFn fn) override                                                                   = 0;
    int start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override = 0;
    int start_local_print_with_record(PrintParams params,
                                      OnUpdateStatusFn update_fn,
                                      WasCancelledFn cancel_fn,
                                      OnWaitFn wait_fn) override                                                         = 0;
    int start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override            = 0;

    int set_on_ssdp_msg_fn(OnMsgArrivedFn fn) override                 = 0;
    int set_on_printer_connected_fn(OnPrinterConnectedFn fn) override  = 0;
    int set_on_subscribe_failure_fn(GetSubscribeFailureFn fn) override = 0;
    int set_on_message_fn(OnMessageFn fn) override                     = 0;
    int set_on_user_message_fn(OnMessageFn fn) override                = 0;
    int set_on_local_connect_fn(OnLocalConnectedFn fn) override        = 0;
    int set_on_local_message_fn(OnMessageFn fn) override               = 0;
    int set_queue_on_main_fn(QueueOnMainFn fn) override                = 0;

};

} // namespace Slic3r

#endif /* slic3r_PrinterAgentPluginCapability_hpp_ */
