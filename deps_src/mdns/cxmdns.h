#ifndef _CX_MDNS_H
#define _CX_MDNS_H
#include<string>
#include<vector>

namespace cxnet
{
	struct machine_info
	{
		std::string machineIp;
		std::string answer;
	};

	std::vector<machine_info> syncDiscoveryService(const std::vector<std::string>& prefix);
}
#endif