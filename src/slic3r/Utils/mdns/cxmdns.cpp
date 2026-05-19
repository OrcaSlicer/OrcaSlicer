#include"cxmdns.h"
#include"mdns.h"
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include <stdio.h>
#include<string.h>
#include <errno.h>
#include <signal.h>

#ifdef _WIN32
#include <winsock2.h>
#include <iphlpapi.h>
#define sleep(x) Sleep(x * 1000)
#else
#include <netdb.h>
#include <ifaddrs.h>
#include <net/if.h>
#endif

// Alias some things to simulate recieving data to fuzz library
#if defined(MDNS_FUZZING)
#define recvfrom(sock, buffer, capacity, flags, src_addr, addrlen) ((mdns_ssize_t)capacity)
#define printf
#endif

#include "mdns.h"

#if defined(MDNS_FUZZING)
#undef recvfrom
#endif

namespace cxnet
{
	template <typename F>
	mdns_record_callback_fn lambda2function(F lambda)
	{
		static auto lambdabak = lambda;
		return [](int sock, const struct sockaddr* from, size_t addrlen,
			mdns_entry_type_t entry, uint16_t query_id, uint16_t rtype,
			uint16_t rclass, uint32_t ttl, const void* data, size_t size,
			size_t name_offset, size_t name_length, size_t record_offset,
			size_t record_length, void* user_data)->int {lambdabak(sock, from, addrlen, entry, query_id, rtype, rclass, ttl, data, size, name_offset, name_length, record_offset, record_length, user_data); return 0; };
	}

	volatile sig_atomic_t running = 1;
#ifdef _WIN32
	BOOL console_handler(DWORD signal) {
		if (signal == CTRL_C_EVENT) {
			running = 0;
		}
		return TRUE;
	}
#else
	void signal_handler(int signal) {
		running = 0;
	}
#endif

	void recvMachineInfoFromSocket(int sock, void* buffer, size_t capacity, const std::vector<std::string>& prefix, std::vector<machine_info>& retmachineInfos, int recIndex)
	{
		struct sockaddr_in6 addr;
		struct sockaddr* saddr = (struct sockaddr*)&addr;
		socklen_t addrlen = sizeof(addr);
		memset(&addr, 0, sizeof(addr));
#ifdef __APPLE__
		saddr->sa_len = sizeof(addr);
#endif
		mdns_ssize_t ret = recvfrom(sock, (char*)buffer, (mdns_size_t)capacity, 0, saddr, &addrlen);
		if (ret <= 0)
			return;

		size_t data_size = (size_t)ret;
		//size_t records = 0;
		const uint16_t* data = (uint16_t*)buffer;

		uint16_t query_id = mdns_ntohs(data++);
		uint16_t flags = mdns_ntohs(data++);
		uint16_t questions = mdns_ntohs(data++);
		uint16_t answer_rrs = mdns_ntohs(data++);
		uint16_t authority_rrs = mdns_ntohs(data++);
		uint16_t additional_rrs = mdns_ntohs(data++);

		// According to RFC 6762 the query ID MUST match the sent query ID (which is 0 in our case)
		if (query_id || (flags != 0x8400))
			return;  // Not a reply to our question

		// It seems some implementations do not fill the correct questions field,
		// so ignore this check for now and only validate answer string
		// if (questions != 1)
		// 	return 0;

		int i;
		for (i = 0; i < questions; ++i) {
			size_t offset = MDNS_POINTER_DIFF(data, buffer);
			size_t verify_offset = 12;
			// Verify it's our question, _services._dns-sd._udp.local.
			if (!mdns_string_equal(buffer, data_size, &offset, mdns_services_query,
				sizeof(mdns_services_query), &verify_offset))
				return;
			data = (const uint16_t*)MDNS_POINTER_OFFSET(buffer, offset);

			uint16_t rtype = mdns_ntohs(data++);
			uint16_t rclass = mdns_ntohs(data++);

			// Make sure we get a reply based on our PTR question for class IN
			if ((rtype != MDNS_RECORDTYPE_PTR) || ((rclass & 0x7FFF) != MDNS_CLASS_IN))
				return;
		}

		for (i = 0; i < answer_rrs; ++i) {
			size_t offset = MDNS_POINTER_DIFF(data, buffer);
			size_t verify_offset = 12;
			// Verify it's an answer to our question, _services._dns-sd._udp.local.
			size_t name_offset = offset;
			int is_answer = mdns_string_equal(buffer, data_size, &offset, mdns_services_query,
				sizeof(mdns_services_query), &verify_offset);
			if (!is_answer && !mdns_string_skip(buffer, data_size, &offset))
				break;
			size_t name_length = offset - name_offset;
			if ((offset + 10) > data_size)
				return;
			data = (const uint16_t*)MDNS_POINTER_OFFSET(buffer, offset);

			uint16_t rtype = mdns_ntohs(data++);
			uint16_t rclass = mdns_ntohs(data++);
			uint32_t ttl = mdns_ntohl(data);
			data += 2;
			uint16_t length = mdns_ntohs(data++);
			if (length > (data_size - offset))
				return;

			static char addrbuf[64];
			static char entrybuf[256];
			static char namebuf[256];

			if (is_answer) {
				offset = MDNS_POINTER_DIFF(data, buffer);
				(void)sizeof(sock);
				(void)sizeof(query_id);
				(void)sizeof(name_length);
				//(void)sizeof(0);
				mdns_string_t fromaddrstr = ip_address_to_string(addrbuf, sizeof(addrbuf), saddr, addrlen);
				mdns_string_t entrystr =
					mdns_string_extract(buffer, data_size, &name_offset, entrybuf, sizeof(entrybuf));
				if (rtype == MDNS_RECORDTYPE_PTR) {
					mdns_string_t namestr = mdns_record_parse_ptr(buffer, data_size, offset, length,
						namebuf, sizeof(namebuf));
					if (!namestr.str || namestr.length == 0) {
						return;
					}
					const std::string answer_name(namestr.str, namestr.length);
					bool bFound = false;
					for (const auto& item : prefix)
					{
						if (answer_name.find(item) != std::string::npos)
							bFound = true;
					}
					if (!bFound)
					{
						return;
					}

					char ip[16] = { 0 };
					sscanf(fromaddrstr.str, "%[^:]", ip);
					retmachineInfos.push_back({ ip, answer_name });
				}
			}
		}
	}

	std::vector<machine_info> syncDiscoveryService(const std::vector<std::string>& prefix)
	{
		std::vector<machine_info> retmachineInfos;
		const char* hostname = "cxslice-host";
		// Initialize network environment
#ifdef _WIN32
		WORD versionWanted = MAKEWORD(1, 1);
		WSADATA wsaData;
		if (WSAStartup(versionWanted, &wsaData)) {
			printf("Failed to initialize WinSock\n");
			return retmachineInfos;
		}
		char hostname_buffer[256];
		DWORD hostname_size = (DWORD)sizeof(hostname_buffer);
		if (GetComputerNameA(hostname_buffer, &hostname_size))
			hostname = hostname_buffer;
		SetConsoleCtrlHandler(console_handler, TRUE);
#else
		char hostname_buffer[256];
		size_t hostname_size = sizeof(hostname_buffer);
		if (gethostname(hostname_buffer, hostname_size) == 0)
			hostname = hostname_buffer;
		signal(SIGINT, signal_handler);
#endif
		int sockets[32];
		int num_sockets = open_client_sockets(sockets, sizeof(sockets) / sizeof(sockets[0]), 0);
		if (num_sockets <= 0) {
			printf("Failed to open any client sockets\n");
#ifdef _WIN32
			WSACleanup();
#endif
			return retmachineInfos;
		}
		printf("Opened %d socket%s for DNS-SD\n", num_sockets, num_sockets > 1 ? "s" : "");
		printf("Sending DNS-SD discovery\n");
		for (int isock = 0; isock < num_sockets; ++isock) {
			if (mdns_discovery_send(sockets[isock]))
				printf("Failed to send DNS-DS discovery: %s\n", strerror(errno));
		}
		size_t capacity = 2048;
		void* buffer = malloc(capacity);
		size_t recordNum = 0;
		void* user_data = 0;

		// This is a simple implementation that loops for 5 seconds or as long as we get replies
		int res;
		printf("Reading DNS-SD replies\n");
		do {
			struct timeval timeout;
			timeout.tv_sec = 5;
			timeout.tv_usec = 0;

			int nfds = 0;
			fd_set readfs;
			FD_ZERO(&readfs);
			for (int isock = 0; isock < num_sockets; ++isock) {
				if (sockets[isock] >= nfds)
					nfds = sockets[isock] + 1;
				FD_SET(sockets[isock], &readfs);
			}
			res = select(nfds, &readfs, 0, 0, &timeout);
			if (res > 0) {
				for (int isock = 0; isock < num_sockets; ++isock) {
					if (FD_ISSET(sockets[isock], &readfs)) {
						//	records += mdns_discovery_recv(sockets[isock], buffer, capacity, query_callback,
						//		0);
						recvMachineInfoFromSocket(sockets[isock], buffer, capacity, prefix, retmachineInfos, isock);
						recordNum++;
					}
				}
			}
		} while (res > 0);

		free(buffer);
		//
		for (int isock = 0; isock < num_sockets; ++isock)
			mdns_socket_close(sockets[isock]);
		printf("Closed socket%s\n", num_sockets ? "s" : "");
#ifdef _WIN32
		WSACleanup();
#endif
		return std::move(retmachineInfos);
	}
}
