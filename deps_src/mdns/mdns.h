/* mdns.h  -  mDNS/DNS-SD library  -  Public Domain  -  2017 Mattias Jansson
 *
 * This library provides a cross-platform mDNS and DNS-SD library in C.
 * The implementation is based on RFC 6762 and RFC 6763.
 *
 * The latest source code is always available at
 *
 * https://github.com/mjansson/mdns
 *
 * This library is put in the public domain; you can redistribute it and/or modify it without any
 * restrictions.
 *
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#ifdef _WIN32
#include <Winsock2.h>
#include <Ws2tcpip.h>
#define strncasecmp _strnicmp
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MDNS_INVALID_POS ((size_t)-1)

#define MDNS_STRING_CONST(s) (s), (sizeof((s)) - 1)
#define MDNS_STRING_ARGS(s) s.str, s.length
#define MDNS_STRING_FORMAT(s) (int)((s).length), s.str

#define MDNS_POINTER_OFFSET(p, ofs) ((void*)((char*)(p) + (ptrdiff_t)(ofs)))
#define MDNS_POINTER_OFFSET_CONST(p, ofs) ((const void*)((const char*)(p) + (ptrdiff_t)(ofs)))
#define MDNS_POINTER_DIFF(a, b) ((size_t)((const char*)(a) - (const char*)(b)))

#define MDNS_PORT 5353
#define MDNS_UNICAST_RESPONSE 0x8000U
#define MDNS_CACHE_FLUSH 0x8000U
#define MDNS_MAX_SUBSTRINGS 64

enum mdns_record_type {
	MDNS_RECORDTYPE_IGNORE = 0,
	// Address
	MDNS_RECORDTYPE_A = 1,
	// Domain Name pointer
	MDNS_RECORDTYPE_PTR = 12,
	// Arbitrary text string
	MDNS_RECORDTYPE_TXT = 16,
	// IP6 Address [Thomson]
	MDNS_RECORDTYPE_AAAA = 28,
	// Server Selection [RFC2782]
	MDNS_RECORDTYPE_SRV = 33,
	// Any available records
	MDNS_RECORDTYPE_ANY = 255
};

enum mdns_entry_type {
	MDNS_ENTRYTYPE_QUESTION = 0,
	MDNS_ENTRYTYPE_ANSWER = 1,
	MDNS_ENTRYTYPE_AUTHORITY = 2,
	MDNS_ENTRYTYPE_ADDITIONAL = 3
};

enum mdns_class { MDNS_CLASS_IN = 1, MDNS_CLASS_ANY = 255 };

typedef enum mdns_record_type mdns_record_type_t;
typedef enum mdns_entry_type mdns_entry_type_t;
typedef enum mdns_class mdns_class_t;

typedef int (*mdns_record_callback_fn)(int sock, const struct sockaddr* from, size_t addrlen,
                                       mdns_entry_type_t entry, uint16_t query_id, uint16_t rtype,
                                       uint16_t rclass, uint32_t ttl, const void* data, size_t size,
                                       size_t name_offset, size_t name_length, size_t record_offset,
                                       size_t record_length, void* user_data);

typedef struct mdns_string_t mdns_string_t;
typedef struct mdns_string_pair_t mdns_string_pair_t;
typedef struct mdns_string_table_item_t mdns_string_table_item_t;
typedef struct mdns_string_table_t mdns_string_table_t;
typedef struct mdns_record_t mdns_record_t;
typedef struct mdns_record_srv_t mdns_record_srv_t;
typedef struct mdns_record_ptr_t mdns_record_ptr_t;
typedef struct mdns_record_a_t mdns_record_a_t;
typedef struct mdns_record_aaaa_t mdns_record_aaaa_t;
typedef struct mdns_record_txt_t mdns_record_txt_t;
typedef struct mdns_query_t mdns_query_t;

#ifdef _WIN32
typedef int mdns_size_t;
typedef int mdns_ssize_t;
#else
typedef size_t mdns_size_t;
typedef ssize_t mdns_ssize_t;
#endif

struct mdns_string_t {
	const char* str;
	size_t length;
};

struct mdns_ip_mac_str {
	char ip[16] ;
	char mac[12];
};

struct mdns_string_pair_t {
	size_t offset;
	size_t length;
	int ref;
};

struct mdns_string_table_t {
	size_t offset[16];
	size_t count;
	size_t next;
};

struct mdns_record_srv_t {
	uint16_t priority;
	uint16_t weight;
	uint16_t port;
	mdns_string_t name;
};

struct mdns_record_ptr_t {
	mdns_string_t name;
};

struct mdns_record_a_t {
	struct sockaddr_in addr;
};

struct mdns_record_aaaa_t {
	struct sockaddr_in6 addr;
};

struct mdns_record_txt_t {
	mdns_string_t key;
	mdns_string_t value;
};

struct mdns_record_t {
	mdns_string_t name;
	mdns_record_type_t type;
	union mdns_record_data {
		mdns_record_ptr_t ptr;
		mdns_record_srv_t srv;
		mdns_record_a_t a;
		mdns_record_aaaa_t aaaa;
		mdns_record_txt_t txt;
	} data;
	uint16_t rclass;
	uint32_t ttl;
};

struct mdns_header_t {
	uint16_t query_id;
	uint16_t flags;
	uint16_t questions;
	uint16_t answer_rrs;
	uint16_t authority_rrs;
	uint16_t additional_rrs;
};

struct mdns_query_t {
	mdns_record_type_t type;
	const char* name;
	size_t length;
};

void  discoveryServiceC();

// mDNS/DNS-SD public API

//! Open and setup a IPv4 socket for mDNS/DNS-SD. To bind the socket to a specific interface, pass
//! in the appropriate socket address in saddr, otherwise pass a null pointer for INADDR_ANY. To
//! send one-shot discovery requests and queries pass a null pointer or set 0 as port to assign a
//! random user level ephemeral port. To run discovery service listening for incoming discoveries
//! and queries, you must set MDNS_PORT as port.
static inline int
mdns_socket_open_ipv4(const struct sockaddr_in* saddr);

//! Setup an already opened IPv4 socket for mDNS/DNS-SD. To bind the socket to a specific interface,
//! pass in the appropriate socket address in saddr, otherwise pass a null pointer for INADDR_ANY.
//! To send one-shot discovery requests and queries pass a null pointer or set 0 as port to assign a
//! random user level ephemeral port. To run discovery service listening for incoming discoveries
//! and queries, you must set MDNS_PORT as port.
static inline int
mdns_socket_setup_ipv4(int sock, const struct sockaddr_in* saddr);

//! Open and setup a IPv6 socket for mDNS/DNS-SD. To bind the socket to a specific interface, pass
//! in the appropriate socket address in saddr, otherwise pass a null pointer for in6addr_any. To
//! send one-shot discovery requests and queries pass a null pointer or set 0 as port to assign a
//! random user level ephemeral port. To run discovery service listening for incoming discoveries
//! and queries, you must set MDNS_PORT as port.
static inline int
mdns_socket_open_ipv6(const struct sockaddr_in6* saddr);

//! Setup an already opened IPv6 socket for mDNS/DNS-SD. To bind the socket to a specific interface,
//! pass in the appropriate socket address in saddr, otherwise pass a null pointer for in6addr_any.
//! To send one-shot discovery requests and queries pass a null pointer or set 0 as port to assign a
//! random user level ephemeral port. To run discovery service listening for incoming discoveries
//! and queries, you must set MDNS_PORT as port.
static inline int
mdns_socket_setup_ipv6(int sock, const struct sockaddr_in6* saddr);

//! Close a socket opened with mdns_socket_open_ipv4 and mdns_socket_open_ipv6.
static inline void
mdns_socket_close(int sock);

//! Listen for incoming multicast DNS-SD and mDNS query requests. The socket should have been opened
//! on port MDNS_PORT using one of the mdns open or setup socket functions. Buffer must be 32 bit
//! aligned. Parsing is stopped when callback function returns non-zero. Returns the number of
//! queries parsed.
static inline size_t
mdns_socket_listen(int sock, void* buffer, size_t capacity, mdns_record_callback_fn callback,
                   void* user_data);

//! Send a multicast DNS-SD reqeuest on the given socket to discover available services. Returns 0
//! on success, or <0 if error.
static inline int
mdns_discovery_send(int sock);

//! Recieve unicast responses to a DNS-SD sent with mdns_discovery_send. Any data will be piped to
//! the given callback for parsing. Buffer must be 32 bit aligned. Parsing is stopped when callback
//! function returns non-zero. Returns the number of responses parsed.
static inline size_t
mdns_discovery_recv(int sock, void* buffer, size_t capacity, mdns_record_callback_fn callback,
                    void* user_data);


//! Send a multicast mDNS query on the given socket for the given service name. The supplied buffer
//! will be used to build the query packet and must be 32 bit aligned. The query ID can be set to
//! non-zero to filter responses, however the RFC states that the query ID SHOULD be set to 0 for
//! multicast queries. The query will request a unicast response if the socket is bound to an
//! ephemeral port, or a multicast response if the socket is bound to mDNS port 5353. Returns the
//! used query ID, or <0 if error.
static inline int
mdns_query_send(int sock, mdns_record_type_t type, const char* name, size_t length, void* buffer,
                size_t capacity, uint16_t query_id);

//! Send a multicast mDNS query on the given socket for the given service names. The supplied buffer
//! will be used to build the query packet and must be 32 bit aligned. The query ID can be set to
//! non-zero to filter responses, however the RFC states that the query ID SHOULD be set to 0 for
//! multicast queries. Each additional service name query consists of a triplet - a record type
//! (mdns_record_type_t), a name string pointer (const char*) and a name length (size_t). The list
//! of variable arguments should be terminated with a record type of 0. The query will request a
//! unicast response if the socket is bound to an ephemeral port, or a multicast response if the
//! socket is bound to mDNS port 5353. Returns the used query ID, or <0 if error.
static inline int
mdns_multiquery_send(int sock, const mdns_query_t* query, size_t count, void* buffer,
                     size_t capacity, uint16_t query_id);

//! Receive unicast responses to a mDNS query sent with mdns_discovery_recv, optionally filtering
//! out any responses not matching the given query ID. Set the query ID to 0 to parse all responses,
//! even if it is not matching the query ID set in a specific query. Any data will be piped to the
//! given callback for parsing. Buffer must be 32 bit aligned. Parsing is stopped when callback
//! function returns non-zero. Returns the number of responses parsed.
static inline size_t
mdns_query_recv(int sock, void* buffer, size_t capacity, mdns_record_callback_fn callback,
                void* user_data, int query_id);

//! Send a variable unicast mDNS query answer to any question with variable number of records to the
//! given address. Use the top bit of the query class field (MDNS_UNICAST_RESPONSE) in the query
//! recieved to determine if the answer should be sent unicast (bit set) or multicast (bit not set).
//! Buffer must be 32 bit aligned. The record type and name should match the data from the query
//! recieved. Returns 0 if success, or <0 if error.
static inline int
mdns_query_answer_unicast(int sock, const void* address, size_t address_size, void* buffer,
                          size_t capacity, uint16_t query_id, mdns_record_type_t record_type,
                          const char* name, size_t name_length, mdns_record_t answer,
                          const mdns_record_t* authority, size_t authority_count,
                          const mdns_record_t* additional, size_t additional_count);

//! Send a variable multicast mDNS query answer to any question with variable number of records. Use
//! the top bit of the query class field (MDNS_UNICAST_RESPONSE) in the query recieved to determine
//! if the answer should be sent unicast (bit set) or multicast (bit not set). Buffer must be 32 bit
//! aligned. Returns 0 if success, or <0 if error.
static inline int
mdns_query_answer_multicast(int sock, void* buffer, size_t capacity, mdns_record_t answer,
                            const mdns_record_t* authority, size_t authority_count,
                            const mdns_record_t* additional, size_t additional_count);

//! Send a variable multicast mDNS announcement (as an unsolicited answer) with variable number of
//! records.Buffer must be 32 bit aligned. Returns 0 if success, or <0 if error. Use this on service
//! startup to announce your instance to the local network.
static inline int
mdns_announce_multicast(int sock, void* buffer, size_t capacity, mdns_record_t answer,
                        const mdns_record_t* authority, size_t authority_count,
                        const mdns_record_t* additional, size_t additional_count);

//! Send a variable multicast mDNS announcement. Use this on service end for removing the resource
//! from the local network. The records must be identical to the according announcement.
static inline int
mdns_goodbye_multicast(int sock, void* buffer, size_t capacity, mdns_record_t answer,
                       const mdns_record_t* authority, size_t authority_count,
                       const mdns_record_t* additional, size_t additional_count);

// Parse records functions

//! Parse a PTR record, returns the name in the record
static inline mdns_string_t
mdns_record_parse_ptr(const void* buffer, size_t size, size_t offset, size_t length,
                      char* strbuffer, size_t capacity);

//! Parse a SRV record, returns the priority, weight, port and name in the record
static inline mdns_record_srv_t
mdns_record_parse_srv(const void* buffer, size_t size, size_t offset, size_t length,
                      char* strbuffer, size_t capacity);

//! Parse an A record, returns the IPv4 address in the record
static inline struct sockaddr_in*
mdns_record_parse_a(const void* buffer, size_t size, size_t offset, size_t length,
                    struct sockaddr_in* addr);

//! Parse an AAAA record, returns the IPv6 address in the record
static inline struct sockaddr_in6*
mdns_record_parse_aaaa(const void* buffer, size_t size, size_t offset, size_t length,
                       struct sockaddr_in6* addr);

//! Parse a TXT record, returns the number of key=value records parsed and stores the key-value
//! pairs in the supplied buffer
static inline size_t
mdns_record_parse_txt(const void* buffer, size_t size, size_t offset, size_t length,
                      mdns_record_txt_t* records, size_t capacity);

// Internal functions

static inline mdns_string_t
mdns_string_extract(const void* buffer, size_t size, size_t* offset, char* str, size_t capacity);

static inline int
mdns_string_skip(const void* buffer, size_t size, size_t* offset);

static inline size_t
mdns_string_find(const char* str, size_t length, char c, size_t offset);

//! Compare if two strings are equal. If the strings are equal it returns >0 and the offset variables are
//! updated to the end of the corresponding strings. If the strings are not equal it returns 0 and 
//! the offset variables are NOT updated.
static inline int
mdns_string_equal(const void* buffer_lhs, size_t size_lhs, size_t* ofs_lhs, const void* buffer_rhs,
                  size_t size_rhs, size_t* ofs_rhs);

static inline void*
mdns_string_make(void* buffer, size_t capacity, void* data, const char* name, size_t length,
                 mdns_string_table_t* string_table);

static inline size_t
mdns_string_table_find(mdns_string_table_t* string_table, const void* buffer, size_t capacity,
                       const char* str, size_t first_length, size_t total_length);

// Implementations

static inline uint16_t
mdns_ntohs(const void* data) {
	uint16_t aligned;
	memcpy(&aligned, data, sizeof(uint16_t));
	return ntohs(aligned);
}

static inline uint32_t
mdns_ntohl(const void* data) {
	uint32_t aligned;
	memcpy(&aligned, data, sizeof(uint32_t));
	return ntohl(aligned);
}

static inline void*
mdns_htons(void* data, uint16_t val) {
	val = htons(val);
	memcpy(data, &val, sizeof(uint16_t));
	return MDNS_POINTER_OFFSET(data, sizeof(uint16_t));
}

static inline void*
mdns_htonl(void* data, uint32_t val) {
	val = htonl(val);
	memcpy(data, &val, sizeof(uint32_t));
	return MDNS_POINTER_OFFSET(data, sizeof(uint32_t));
}

static inline int
mdns_socket_open_ipv4(const struct sockaddr_in* saddr) {
	int sock = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0)
		return -1;
	if (mdns_socket_setup_ipv4(sock, saddr)) {
		mdns_socket_close(sock);
		return -1;
	}
	return sock;
}
//==========================add 
int
send_dns_sd(void);
int
open_client_sockets(int* sockets, int max_sockets, int port);

mdns_string_t
ipv4_address_to_string(char* buffer, size_t capacity, const struct sockaddr_in* addr,
	size_t addrlen);
mdns_string_t
ip_address_to_string(char* buffer, size_t capacity, const struct sockaddr* addr, size_t addrlen);

int
query_callback(int sock, const struct sockaddr* from, size_t addrlen, mdns_entry_type_t entry,
	uint16_t query_id, uint16_t rtype, uint16_t rclass, uint32_t ttl, const void* data,
	size_t size, size_t name_offset, size_t name_length, size_t record_offset,
	size_t record_length, void* user_data);


static inline int
mdns_socket_setup_ipv4(int sock, const struct sockaddr_in* saddr) {
	unsigned char ttl = 1;
	unsigned char loopback = 1;
	unsigned int reuseaddr = 1;
	struct ip_mreq req;

	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuseaddr, sizeof(reuseaddr));
#ifdef SO_REUSEPORT
	setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (const char*)&reuseaddr, sizeof(reuseaddr));
#endif
	setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof(ttl));
	setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, (const char*)&loopback, sizeof(loopback));

	memset(&req, 0, sizeof(req));
	req.imr_multiaddr.s_addr = htonl((((uint32_t)224U) << 24U) | ((uint32_t)251U));
	if (saddr)
		req.imr_interface = saddr->sin_addr;
	if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*)&req, sizeof(req)))
		return -1;

	struct sockaddr_in sock_addr;
	if (!saddr) {
		memset(&sock_addr, 0, sizeof(struct sockaddr_in));
		sock_addr.sin_family = AF_INET;
		sock_addr.sin_addr.s_addr = INADDR_ANY;
#ifdef __APPLE__
		sock_addr.sin_len = sizeof(struct sockaddr_in);
#endif
	} else {
		memcpy(&sock_addr, saddr, sizeof(struct sockaddr_in));
		setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, (const char*)&sock_addr.sin_addr,
		           sizeof(sock_addr.sin_addr));
#ifndef _WIN32
		sock_addr.sin_addr.s_addr = INADDR_ANY;
#endif
	}

	if (bind(sock, (struct sockaddr*)&sock_addr, sizeof(struct sockaddr_in)))
		return -1;

#ifdef _WIN32
	unsigned long param = 1;
	ioctlsocket(sock, FIONBIO, &param);
#else
	const int flags = fcntl(sock, F_GETFL, 0);
	fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif

	return 0;
}

static inline int
mdns_socket_open_ipv6(const struct sockaddr_in6* saddr) {
	int sock = (int)socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0)
		return -1;
	if (mdns_socket_setup_ipv6(sock, saddr)) {
		mdns_socket_close(sock);
		return -1;
	}
	return sock;
}

static inline int
mdns_socket_setup_ipv6(int sock, const struct sockaddr_in6* saddr) {
	int hops = 1;
	unsigned int loopback = 1;
	unsigned int reuseaddr = 1;
	struct ipv6_mreq req;

	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuseaddr, sizeof(reuseaddr));
#ifdef SO_REUSEPORT
	setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, (const char*)&reuseaddr, sizeof(reuseaddr));
#endif
	setsockopt(sock, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, (const char*)&hops, sizeof(hops));
	setsockopt(sock, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, (const char*)&loopback, sizeof(loopback));

	memset(&req, 0, sizeof(req));
	req.ipv6mr_multiaddr.s6_addr[0] = 0xFF;
	req.ipv6mr_multiaddr.s6_addr[1] = 0x02;
	req.ipv6mr_multiaddr.s6_addr[15] = 0xFB;
	if (setsockopt(sock, IPPROTO_IPV6, IPV6_JOIN_GROUP, (char*)&req, sizeof(req)))
		return -1;

	struct sockaddr_in6 sock_addr;
	if (!saddr) {
		memset(&sock_addr, 0, sizeof(struct sockaddr_in6));
		sock_addr.sin6_family = AF_INET6;
		sock_addr.sin6_addr = in6addr_any;
#ifdef __APPLE__
		sock_addr.sin6_len = sizeof(struct sockaddr_in6);
#endif
	} else {
		memcpy(&sock_addr, saddr, sizeof(struct sockaddr_in6));
		unsigned int ifindex = 0;
		setsockopt(sock, IPPROTO_IPV6, IPV6_MULTICAST_IF, (const char*)&ifindex, sizeof(ifindex));
#ifndef _WIN32
		sock_addr.sin6_addr = in6addr_any;
#endif
	}

	if (bind(sock, (struct sockaddr*)&sock_addr, sizeof(struct sockaddr_in6)))
		return -1;

#ifdef _WIN32
	unsigned long param = 1;
	ioctlsocket(sock, FIONBIO, &param);
#else
	const int flags = fcntl(sock, F_GETFL, 0);
	fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif

	return 0;
}

static inline void
mdns_socket_close(int sock) {
#ifdef _WIN32
	closesocket(sock);
#else
	close(sock);
#endif
}

static inline int
mdns_is_string_ref(uint8_t val) {
	return (0xC0 == (val & 0xC0));
}

static inline mdns_string_pair_t
mdns_get_next_substring(const void* rawdata, size_t size, size_t offset) {
	const uint8_t* buffer = (const uint8_t*)rawdata;
	mdns_string_pair_t pair = {MDNS_INVALID_POS, 0, 0};
	if (offset >= size)
		return pair;
	if (!buffer[offset]) {
		pair.offset = offset;
		return pair;
	}
	int recursion = 0;
	while (mdns_is_string_ref(buffer[offset])) {
		if (size < offset + 2)
			return pair;

		offset = mdns_ntohs(MDNS_POINTER_OFFSET(buffer, offset)) & 0x3fff;
		if (offset >= size)
			return pair;

		pair.ref = 1;
		if (++recursion > 16)
			return pair;
	}

	size_t length = (size_t)buffer[offset++];
	if (size < offset + length)
		return pair;

	pair.offset = offset;
	pair.length = length;

	return pair;
}

static inline int
mdns_string_skip(const void* buffer, size_t size, size_t* offset) {
	size_t cur = *offset;
	mdns_string_pair_t substr;
	unsigned int counter = 0;
	do {
		substr = mdns_get_next_substring(buffer, size, cur);
		if ((substr.offset == MDNS_INVALID_POS) || (counter++ > MDNS_MAX_SUBSTRINGS))
			return 0;
		if (substr.ref) {
			*offset = cur + 2;
			return 1;
		}
		cur = substr.offset + substr.length;
	} while (substr.length);

	*offset = cur + 1;
	return 1;
}

static inline int
mdns_string_equal(const void* buffer_lhs, size_t size_lhs, size_t* ofs_lhs, const void* buffer_rhs,
                  size_t size_rhs, size_t* ofs_rhs) {
	size_t lhs_cur = *ofs_lhs;
	size_t rhs_cur = *ofs_rhs;
	size_t lhs_end = MDNS_INVALID_POS;
	size_t rhs_end = MDNS_INVALID_POS;
	mdns_string_pair_t lhs_substr;
	mdns_string_pair_t rhs_substr;
	unsigned int counter = 0;
	do {
		lhs_substr = mdns_get_next_substring(buffer_lhs, size_lhs, lhs_cur);
		rhs_substr = mdns_get_next_substring(buffer_rhs, size_rhs, rhs_cur);
		if ((lhs_substr.offset == MDNS_INVALID_POS) || (rhs_substr.offset == MDNS_INVALID_POS) ||
		    (counter++ > MDNS_MAX_SUBSTRINGS))
			return 0;
		if (lhs_substr.length != rhs_substr.length)
			return 0;
		if (strncasecmp((const char*)MDNS_POINTER_OFFSET_CONST(buffer_rhs, rhs_substr.offset),
		                (const char*)MDNS_POINTER_OFFSET_CONST(buffer_lhs, lhs_substr.offset),
		                rhs_substr.length))
			return 0;
		if (lhs_substr.ref && (lhs_end == MDNS_INVALID_POS))
			lhs_end = lhs_cur + 2;
		if (rhs_substr.ref && (rhs_end == MDNS_INVALID_POS))
			rhs_end = rhs_cur + 2;
		lhs_cur = lhs_substr.offset + lhs_substr.length;
		rhs_cur = rhs_substr.offset + rhs_substr.length;
	} while (lhs_substr.length);

	if (lhs_end == MDNS_INVALID_POS)
		lhs_end = lhs_cur + 1;
	*ofs_lhs = lhs_end;

	if (rhs_end == MDNS_INVALID_POS)
		rhs_end = rhs_cur + 1;
	*ofs_rhs = rhs_end;

	return 1;
}

static inline mdns_string_t
mdns_string_extract(const void* buffer, size_t size, size_t* offset, char* str, size_t capacity) {
	size_t cur = *offset;
	size_t end = MDNS_INVALID_POS;
	mdns_string_pair_t substr;
	mdns_string_t result;
	result.str = str;
	result.length = 0;
	char* dst = str;
	unsigned int counter = 0;
	size_t remain = capacity;
	do {
		substr = mdns_get_next_substring(buffer, size, cur);
		if ((substr.offset == MDNS_INVALID_POS) || (counter++ > MDNS_MAX_SUBSTRINGS))
			return result;
		if (substr.ref && (end == MDNS_INVALID_POS))
			end = cur + 2;
		if (substr.length) {
			size_t to_copy = (substr.length < remain) ? substr.length : remain;
			memcpy(dst, (const char*)buffer + substr.offset, to_copy);
			dst += to_copy;
			remain -= to_copy;
			if (remain) {
				*dst++ = '.';
				--remain;
			}
		}
		cur = substr.offset + substr.length;
	} while (substr.length);

	if (end == MDNS_INVALID_POS)
		end = cur + 1;
	*offset = end;

	result.length = capacity - remain;
	return result;
}

static inline size_t
mdns_string_table_find(mdns_string_table_t* string_table, const void* buffer, size_t capacity,
                       const char* str, size_t first_length, size_t total_length) {
	if (!string_table)
		return MDNS_INVALID_POS;

	for (size_t istr = 0; istr < string_table->count; ++istr) {
		if (string_table->offset[istr] >= capacity)
			continue;
		size_t offset = 0;
		mdns_string_pair_t sub_string =
		    mdns_get_next_substring(buffer, capacity, string_table->offset[istr]);
		if (!sub_string.length || (sub_string.length != first_length))
			continue;
		if (memcmp(str, MDNS_POINTER_OFFSET(buffer, sub_string.offset), sub_string.length))
			continue;

		// Initial substring matches, now match all remaining substrings
		offset += first_length + 1;
		while (offset < total_length) {
			size_t dot_pos = mdns_string_find(str, total_length, '.', offset);
			if (dot_pos == MDNS_INVALID_POS)
				dot_pos = total_length;
			size_t current_length = dot_pos - offset;

			sub_string =
			    mdns_get_next_substring(buffer, capacity, sub_string.offset + sub_string.length);
			if (!sub_string.length || (sub_string.length != current_length))
				break;
			if (memcmp(str + offset, MDNS_POINTER_OFFSET(buffer, sub_string.offset),
			           sub_string.length))
				break;

			offset = dot_pos + 1;
		}

		// Return reference offset if entire string matches
		if (offset >= total_length)
			return string_table->offset[istr];
	}

	return MDNS_INVALID_POS;
}

static inline void
mdns_string_table_add(mdns_string_table_t* string_table, size_t offset) {
	if (!string_table)
		return;

	string_table->offset[string_table->next] = offset;

	size_t table_capacity = sizeof(string_table->offset) / sizeof(string_table->offset[0]);
	if (++string_table->count > table_capacity)
		string_table->count = table_capacity;
	if (++string_table->next >= table_capacity)
		string_table->next = 0;
}

static inline size_t
mdns_string_find(const char* str, size_t length, char c, size_t offset) {
	const void* found;
	if (offset >= length)
		return MDNS_INVALID_POS;
	found = memchr(str + offset, c, length - offset);
	if (found)
		return (size_t)MDNS_POINTER_DIFF(found, str);
	return MDNS_INVALID_POS;
}

static inline void*
mdns_string_make_ref(void* data, size_t capacity, size_t ref_offset) {
	if (capacity < 2)
		return 0;
	return mdns_htons(data, 0xC000 | (uint16_t)ref_offset);
}

static inline void*
mdns_string_make(void* buffer, size_t capacity, void* data, const char* name, size_t length,
                 mdns_string_table_t* string_table) {
	size_t last_pos = 0;
	size_t remain = capacity - MDNS_POINTER_DIFF(data, buffer);
	if (name[length - 1] == '.')
		--length;
	while (last_pos < length) {
		size_t pos = mdns_string_find(name, length, '.', last_pos);
		size_t sub_length = ((pos != MDNS_INVALID_POS) ? pos : length) - last_pos;
		size_t total_length = length - last_pos;

		size_t ref_offset =
		    mdns_string_table_find(string_table, buffer, capacity,
		                           (char*)MDNS_POINTER_OFFSET(name, last_pos), sub_length,
		                           total_length);
		if (ref_offset != MDNS_INVALID_POS)
			return mdns_string_make_ref(data, remain, ref_offset);

		if (remain <= (sub_length + 1))
			return 0;

		*(unsigned char*)data = (unsigned char)sub_length;
		memcpy(MDNS_POINTER_OFFSET(data, 1), name + last_pos, sub_length);
		mdns_string_table_add(string_table, MDNS_POINTER_DIFF(data, buffer));

		data = MDNS_POINTER_OFFSET(data, sub_length + 1);
		last_pos = ((pos != MDNS_INVALID_POS) ? pos + 1 : length);
		remain = capacity - MDNS_POINTER_DIFF(data, buffer);
	}

	if (!remain)
		return 0;

	*(unsigned char*)data = 0;
	return MDNS_POINTER_OFFSET(data, 1);
}

static inline size_t
mdns_records_parse(int sock, const struct sockaddr* from, size_t addrlen, const void* buffer,
                   size_t size, size_t* offset, mdns_entry_type_t type, uint16_t query_id,
                   size_t records, mdns_record_callback_fn callback, void* user_data) {
	size_t parsed = 0;
	for (size_t i = 0; i < records; ++i) {
		size_t name_offset = *offset;
		mdns_string_skip(buffer, size, offset);
		if (((*offset) + 10) > size)
			return parsed;
		size_t name_length = (*offset) - name_offset;
		const uint16_t* data = (const uint16_t*)MDNS_POINTER_OFFSET(buffer, *offset);

		uint16_t rtype = mdns_ntohs(data++);
		uint16_t rclass = mdns_ntohs(data++);
		uint32_t ttl = mdns_ntohl(data);
		data += 2;
		uint16_t length = mdns_ntohs(data++);

		*offset += 10;

		if (length <= (size - (*offset))) {
			++parsed;
			if (callback &&
			    callback(sock, from, addrlen, type, query_id, rtype, rclass, ttl, buffer, size,
			             name_offset, name_length, *offset, length, user_data))
				break;
		}

		*offset += length;
	}
	return parsed;
}

static inline int
mdns_unicast_send(int sock, const void* address, size_t address_size, const void* buffer,
                  size_t size) {
	if (sendto(sock, (const char*)buffer, (mdns_size_t)size, 0, (const struct sockaddr*)address,
	           (socklen_t)address_size) < 0)
		return -1;
	return 0;
}

static inline int
mdns_multicast_send(int sock, const void* buffer, size_t size) {
	struct sockaddr_storage addr_storage;
	struct sockaddr_in addr;
	struct sockaddr_in6 addr6;
	struct sockaddr* saddr = (struct sockaddr*)&addr_storage;
	socklen_t saddrlen = sizeof(struct sockaddr_storage);
	if (getsockname(sock, saddr, &saddrlen))
		return -1;
	if (saddr->sa_family == AF_INET6) {
		memset(&addr6, 0, sizeof(addr6));
		addr6.sin6_family = AF_INET6;
#ifdef __APPLE__
		addr6.sin6_len = sizeof(addr6);
#endif
		addr6.sin6_addr.s6_addr[0] = 0xFF;
		addr6.sin6_addr.s6_addr[1] = 0x02;
		addr6.sin6_addr.s6_addr[15] = 0xFB;
		addr6.sin6_port = htons((unsigned short)MDNS_PORT);
		saddr = (struct sockaddr*)&addr6;
		saddrlen = sizeof(addr6);
	} else {
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
#ifdef __APPLE__
		addr.sin_len = sizeof(addr);
#endif
		addr.sin_addr.s_addr = htonl((((uint32_t)224U) << 24U) | ((uint32_t)251U));
		addr.sin_port = htons((unsigned short)MDNS_PORT);
		saddr = (struct sockaddr*)&addr;
		saddrlen = sizeof(addr);
	}

	if (sendto(sock, (const char*)buffer, (mdns_size_t)size, 0, saddr, saddrlen) < 0)
		return -1;
	return 0;
}

static const uint8_t mdns_services_query[] = {
    // Query ID
    0x00, 0x00,
    // Flags
    0x00, 0x00,
    // 1 question
    0x00, 0x01,
    // No answer, authority or additional RRs
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // _services._dns-sd._udp.local.
    0x09, '_', 's', 'e', 'r', 'v', 'i', 'c', 'e', 's', 0x07, '_', 'd', 'n', 's', '-', 's', 'd',
    0x04, '_', 'u', 'd', 'p', 0x05, 'l', 'o', 'c', 'a', 'l', 0x00,
    // PTR record
    0x00, MDNS_RECORDTYPE_PTR,
    // QU (unicast response) and class IN
    0x80, MDNS_CLASS_IN};

static inline int
mdns_discovery_send(int sock) {
	return mdns_multicast_send(sock, mdns_services_query, sizeof(mdns_services_query));
}


static inline size_t
mdns_discovery_recv(int sock, void* buffer, size_t capacity, mdns_record_callback_fn callback,
                    void* user_data) {
	struct sockaddr_in6 addr;
	struct sockaddr* saddr = (struct sockaddr*)&addr;
	socklen_t addrlen = sizeof(addr);
	memset(&addr, 0, sizeof(addr));
#ifdef __APPLE__
	saddr->sa_len = sizeof(addr);
#endif
	mdns_ssize_t ret = recvfrom(sock, (char*)buffer, (mdns_size_t)capacity, 0, saddr, &addrlen);
	if (ret <= 0)
		return 0;

	size_t data_size = (size_t)ret;
	size_t records = 0;
	const uint16_t* data = (uint16_t*)buffer;

	uint16_t query_id = mdns_ntohs(data++);
	uint16_t flags = mdns_ntohs(data++);
	uint16_t questions = mdns_ntohs(data++);
	uint16_t answer_rrs = mdns_ntohs(data++);
	uint16_t authority_rrs = mdns_ntohs(data++);
	uint16_t additional_rrs = mdns_ntohs(data++);

	// According to RFC 6762 the query ID MUST match the sent query ID (which is 0 in our case)
	if (query_id || (flags != 0x8400))
		return 0;  // Not a reply to our question

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
			return 0;
		data = (const uint16_t*)MDNS_POINTER_OFFSET(buffer, offset);

		uint16_t rtype = mdns_ntohs(data++);
		uint16_t rclass = mdns_ntohs(data++);

		// Make sure we get a reply based on our PTR question for class IN
		if ((rtype != MDNS_RECORDTYPE_PTR) || ((rclass & 0x7FFF) != MDNS_CLASS_IN))
			return 0;
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
			return records;
		data = (const uint16_t*)MDNS_POINTER_OFFSET(buffer, offset);

		uint16_t rtype = mdns_ntohs(data++);
		uint16_t rclass = mdns_ntohs(data++);
		uint32_t ttl = mdns_ntohl(data);
		data += 2;
		uint16_t length = mdns_ntohs(data++);
		if (length > (data_size - offset))
			return 0;

		if (is_answer) {
			++records;
			offset = MDNS_POINTER_DIFF(data, buffer);
			if (callback &&
			    callback(sock, saddr, addrlen, MDNS_ENTRYTYPE_ANSWER, query_id, rtype, rclass, ttl,
			             buffer, data_size, name_offset, name_length, offset, length, user_data))
				return records;
		}
		data = (const uint16_t*)MDNS_POINTER_OFFSET_CONST(data, length);
	}

	size_t total_records = records;
	size_t offset = MDNS_POINTER_DIFF(data, buffer);
	records =
	    mdns_records_parse(sock, saddr, addrlen, buffer, data_size, &offset,
	                       MDNS_ENTRYTYPE_AUTHORITY, query_id, authority_rrs, callback, user_data);
	total_records += records;
	if (records != authority_rrs)
		return total_records;

	records = mdns_records_parse(sock, saddr, addrlen, buffer, data_size, &offset,
	                             MDNS_ENTRYTYPE_ADDITIONAL, query_id, additional_rrs, callback,
	                             user_data);
	total_records += records;
	if (records != additional_rrs)
		return total_records;

	return total_records;
}

static inline size_t
mdns_socket_listen(int sock, void* buffer, size_t capacity, mdns_record_callback_fn callback,
                   void* user_data) {
	struct sockaddr_in6 addr;
	struct sockaddr* saddr = (struct sockaddr*)&addr;
	socklen_t addrlen = sizeof(addr);
	memset(&addr, 0, sizeof(addr));
#ifdef __APPLE__
	saddr->sa_len = sizeof(addr);
#endif
	mdns_ssize_t ret = recvfrom(sock, (char*)buffer, (mdns_size_t)capacity, 0, saddr, &addrlen);
	if (ret <= 0)
		return 0;

	size_t data_size = (size_t)ret;
	const uint16_t* data = (const uint16_t*)buffer;

	uint16_t query_id = mdns_ntohs(data++);
	uint16_t flags = mdns_ntohs(data++);
	uint16_t questions = mdns_ntohs(data++);
	uint16_t answer_rrs = mdns_ntohs(data++);
	uint16_t authority_rrs = mdns_ntohs(data++);
	uint16_t additional_rrs = mdns_ntohs(data++);

	size_t records;
	size_t total_records = 0;
	for (int iquestion = 0; iquestion < questions; ++iquestion) {
		size_t question_offset = MDNS_POINTER_DIFF(data, buffer);
		size_t offset = question_offset;
		size_t verify_offset = 12;
		int dns_sd = 0;
		if (mdns_string_equal(buffer, data_size, &offset, mdns_services_query,
		                      sizeof(mdns_services_query), &verify_offset)) {
			dns_sd = 1;
		} else if (!mdns_string_skip(buffer, data_size, &offset)) {
			break;
		}
		size_t length = offset - question_offset;
		data = (const uint16_t*)MDNS_POINTER_OFFSET_CONST(buffer, offset);

		uint16_t rtype = mdns_ntohs(data++);
		uint16_t rclass = mdns_ntohs(data++);
		uint16_t class_without_flushbit = rclass & ~MDNS_CACHE_FLUSH;

		// Make sure we get a question of class IN or ANY
		if (!((class_without_flushbit == MDNS_CLASS_IN) ||
		      (class_without_flushbit == MDNS_CLASS_ANY))) {
			break;
		}

		if (dns_sd && flags)
			continue;

		++total_records;
		if (callback && callback(sock, saddr, addrlen, MDNS_ENTRYTYPE_QUESTION, query_id, rtype,
		                         rclass, 0, buffer, data_size, question_offset, length,
		                         question_offset, length, user_data))
			return total_records;
	}

	size_t offset = MDNS_POINTER_DIFF(data, buffer);
	records = mdns_records_parse(sock, saddr, addrlen, buffer, data_size, &offset,
	                             MDNS_ENTRYTYPE_ANSWER, query_id, answer_rrs, callback, user_data);
	total_records += records;
	if (records != answer_rrs)
		return total_records;

	records =
	    mdns_records_parse(sock, saddr, addrlen, buffer, data_size, &offset,
	                       MDNS_ENTRYTYPE_AUTHORITY, query_id, authority_rrs, callback, user_data);
	total_records += records;
	if (records != authority_rrs)
		return total_records;

	records = mdns_records_parse(sock, saddr, addrlen, buffer, data_size, &offset,
	                             MDNS_ENTRYTYPE_ADDITIONAL, query_id, additional_rrs, callback,
	                             user_data);

	return total_records;
}

static inline int
mdns_query_send(int sock, mdns_record_type_t type, const char* name, size_t length, void* buffer,
                size_t capacity, uint16_t query_id) {
	mdns_query_t query;
	query.type = type;
	query.name = name;
	query.length = length;
	return mdns_multiquery_send(sock, &query, 1, buffer, capacity, query_id);
}

static inline int
mdns_multiquery_send(int sock, const mdns_query_t* query, size_t count, void* buffer, size_t capacity,
                     uint16_t query_id) {
	if (!count || (capacity < (sizeof(struct mdns_header_t) + (6 * count))))
		return -1;

	// Ask for a unicast response since it's a one-shot query
	uint16_t rclass = MDNS_CLASS_IN | MDNS_UNICAST_RESPONSE;

	struct sockaddr_storage addr_storage;
	struct sockaddr* saddr = (struct sockaddr*)&addr_storage;
	socklen_t saddrlen = sizeof(addr_storage);
	if (getsockname(sock, saddr, &saddrlen) == 0) {
		if ((saddr->sa_family == AF_INET) &&
		    (ntohs(((struct sockaddr_in*)saddr)->sin_port) == MDNS_PORT))
			rclass &= ~MDNS_UNICAST_RESPONSE;
		else if ((saddr->sa_family == AF_INET6) &&
		         (ntohs(((struct sockaddr_in6*)saddr)->sin6_port) == MDNS_PORT))
			rclass &= ~MDNS_UNICAST_RESPONSE;
	}

	struct mdns_header_t* header = (struct mdns_header_t*)buffer;
	// Query ID
	header->query_id = htons((unsigned short)query_id);
	// Flags
	header->flags = 0;
	// Questions
	header->questions = htons((unsigned short)count);
	// No answer, authority or additional RRs
	header->answer_rrs = 0;
	header->authority_rrs = 0;
	header->additional_rrs = 0;
	// Fill in questions
	void* data = MDNS_POINTER_OFFSET(buffer, sizeof(struct mdns_header_t));
	for (size_t iq = 0; iq < count; ++iq) {
		// Name string
		data = mdns_string_make(buffer, capacity, data, query[iq].name, query[iq].length, 0);
		if (!data)
			return -1;
		// Record type
		data = mdns_htons(data, query[iq].type);
		//! Optional unicast response based on local port, class IN
		data = mdns_htons(data, rclass);
	}

	size_t tosend = MDNS_POINTER_DIFF(data, buffer);
	if (mdns_multicast_send(sock, buffer, (size_t)tosend))
		return -1;
	return query_id;
}

static inline size_t
mdns_query_recv(int sock, void* buffer, size_t capacity, mdns_record_callback_fn callback,
                void* user_data, int only_query_id) {
	struct sockaddr_in6 addr;
	struct sockaddr* saddr = (struct sockaddr*)&addr;
	socklen_t addrlen = sizeof(addr);
	memset(&addr, 0, sizeof(addr));
#ifdef __APPLE__
	saddr->sa_len = sizeof(addr);
#endif
	mdns_ssize_t ret = recvfrom(sock, (char*)buffer, (mdns_size_t)capacity, 0, saddr, &addrlen);
	if (ret <= 0)
		return 0;

	size_t data_size = (size_t)ret;
	const uint16_t* data = (const uint16_t*)buffer;

	uint16_t query_id = mdns_ntohs(data++);
	uint16_t flags = mdns_ntohs(data++);
	uint16_t questions = mdns_ntohs(data++);
	uint16_t answer_rrs = mdns_ntohs(data++);
	uint16_t authority_rrs = mdns_ntohs(data++);
	uint16_t additional_rrs = mdns_ntohs(data++);
	(void)sizeof(flags);

	if ((only_query_id > 0) && (query_id != only_query_id))
		return 0;  // Not a reply to the wanted one-shot query

	if (questions > 1)
		return 0;

	// Skip questions part
	int i;
	for (i = 0; i < questions; ++i) {
		size_t offset = MDNS_POINTER_DIFF(data, buffer);
		if (!mdns_string_skip(buffer, data_size, &offset))
			return 0;
		data = (const uint16_t*)MDNS_POINTER_OFFSET_CONST(buffer, offset);
		// Record type and class not used, skip
		// uint16_t rtype = mdns_ntohs(data++);
		// uint16_t rclass = mdns_ntohs(data++);
		data += 2;
	}

	size_t records = 0;
	size_t total_records = 0;
	size_t offset = MDNS_POINTER_DIFF(data, buffer);
	records = mdns_records_parse(sock, saddr, addrlen, buffer, data_size, &offset,
	                             MDNS_ENTRYTYPE_ANSWER, query_id, answer_rrs, callback, user_data);
	total_records += records;
	if (records != answer_rrs)
		return total_records;

	records =
	    mdns_records_parse(sock, saddr, addrlen, buffer, data_size, &offset,
	                       MDNS_ENTRYTYPE_AUTHORITY, query_id, authority_rrs, callback, user_data);
	total_records += records;
	if (records != authority_rrs)
		return total_records;

	records = mdns_records_parse(sock, saddr, addrlen, buffer, data_size, &offset,
	                             MDNS_ENTRYTYPE_ADDITIONAL, query_id, additional_rrs, callback,
	                             user_data);
	total_records += records;
	if (records != additional_rrs)
		return total_records;

	return total_records;
}

static inline void*
mdns_answer_add_question_unicast(void* buffer, size_t capacity, void* data,
                                 mdns_record_type_t record_type, const char* name,
                                 size_t name_length, mdns_string_table_t* string_table) {
	data = mdns_string_make(buffer, capacity, data, name, name_length, string_table);
	if (!data)
		return 0;
	size_t remain = capacity - MDNS_POINTER_DIFF(data, buffer);
	if (remain < 4)
		return 0;

	data = mdns_htons(data, record_type);
	data = mdns_htons(data, MDNS_UNICAST_RESPONSE | MDNS_CLASS_IN);

	return data;
}

static inline void*
mdns_answer_add_record_header(void* buffer, size_t capacity, void* data, mdns_record_t record,
                              mdns_string_table_t* string_table) {
	data = mdns_string_make(buffer, capacity, data, record.name.str, record.name.length, string_table);
	if (!data)
		return 0;
	size_t remain = capacity - MDNS_POINTER_DIFF(data, buffer);
	if (remain < 10)
		return 0;

	data = mdns_htons(data, record.type);
	data = mdns_htons(data, record.rclass);
	data = mdns_htonl(data, record.ttl);
	data = mdns_htons(data, 0);  // Length, to be filled later
	return data;
}

static inline void*
mdns_answer_add_record(void* buffer, size_t capacity, void* data, mdns_record_t record,
                       mdns_string_table_t* string_table) {
	// TXT records will be coalesced into one record later
	if (!data || (record.type == MDNS_RECORDTYPE_TXT))
		return data;

	data = mdns_answer_add_record_header(buffer, capacity, data, record, string_table);
	if (!data)
		return 0;

	// Pointer to length of record to be filled at end
	void* record_length = MDNS_POINTER_OFFSET(data, -2);
	void* record_data = data;

	size_t remain = capacity - MDNS_POINTER_DIFF(data, buffer);
	switch (record.type) {
		case MDNS_RECORDTYPE_PTR:
			data = mdns_string_make(buffer, capacity, data, record.data.ptr.name.str,
			                        record.data.ptr.name.length, string_table);
			break;

		case MDNS_RECORDTYPE_SRV:
			if (remain <= 6)
				return 0;
			data = mdns_htons(data, record.data.srv.priority);
			data = mdns_htons(data, record.data.srv.weight);
			data = mdns_htons(data, record.data.srv.port);
			data = mdns_string_make(buffer, capacity, data, record.data.srv.name.str,
			                        record.data.srv.name.length, string_table);
			break;

		case MDNS_RECORDTYPE_A:
			if (remain < 4)
				return 0;
			memcpy(data, &record.data.a.addr.sin_addr.s_addr, 4);
			data = MDNS_POINTER_OFFSET(data, 4);
			break;

		case MDNS_RECORDTYPE_AAAA:
			if (remain < 16)
				return 0;
			memcpy(data, &record.data.aaaa.addr.sin6_addr, 16);  // ipv6 address
			data = MDNS_POINTER_OFFSET(data, 16);
			break;

		default:
			break;
	}

	if (!data)
		return 0;

	// Fill record length
	mdns_htons(record_length, (uint16_t)MDNS_POINTER_DIFF(data, record_data));
	return data;
}

static inline void
mdns_record_update_rclass_ttl(mdns_record_t* record, uint16_t rclass, uint32_t ttl) {
	if (!record->rclass)
		record->rclass = rclass;
	if (!record->ttl || !ttl)
		record->ttl = ttl;
	record->rclass &= (uint16_t)(MDNS_CLASS_IN | MDNS_CACHE_FLUSH);
	// Never flush PTR record
	if (record->type == MDNS_RECORDTYPE_PTR)
		record->rclass &= ~(uint16_t)MDNS_CACHE_FLUSH;
}

static inline void*
mdns_answer_add_txt_record(void* buffer, size_t capacity, void* data, const mdns_record_t* records,
                           size_t record_count, uint16_t rclass, uint32_t ttl,
                           mdns_string_table_t* string_table) {
	// Pointer to length of record to be filled at end
	void* record_length = 0;
	void* record_data = 0;

	size_t remain = 0;
	for (size_t irec = 0; data && (irec < record_count); ++irec) {
		if (records[irec].type != MDNS_RECORDTYPE_TXT)
			continue;

		mdns_record_t record = records[irec];
		mdns_record_update_rclass_ttl(&record, rclass, ttl);
		if (!record_data) {
			data = mdns_answer_add_record_header(buffer, capacity, data, record, string_table);
			if (!data)
				return data;
			record_length = MDNS_POINTER_OFFSET(data, -2);
			record_data = data;
		}

		// TXT strings are unlikely to be shared, just make then raw. Also need one byte for
		// termination, thus the <= check
		size_t string_length = record.data.txt.key.length + record.data.txt.value.length + 1;
		if (!data)
			return 0;
		remain = capacity - MDNS_POINTER_DIFF(data, buffer);
		if ((remain <= string_length) || (string_length > 0x3FFF))
			return 0;

		unsigned char* strdata = (unsigned char*)data;
		*strdata++ = (unsigned char)string_length;
		memcpy(strdata, record.data.txt.key.str, record.data.txt.key.length);
		strdata += record.data.txt.key.length;
		*strdata++ = '=';
		memcpy(strdata, record.data.txt.value.str, record.data.txt.value.length);
		strdata += record.data.txt.value.length;

		data = strdata;
	}

	// Fill record length
	if (record_data)
		mdns_htons(record_length, (uint16_t)MDNS_POINTER_DIFF(data, record_data));

	return data;
}

static inline uint16_t
mdns_answer_get_record_count(const mdns_record_t* records, size_t record_count) {
	// TXT records will be coalesced into one record
	uint16_t total_count = 0;
	uint16_t txt_record = 0;
	for (size_t irec = 0; irec < record_count; ++irec) {
		if (records[irec].type == MDNS_RECORDTYPE_TXT)
			txt_record = 1;
		else
			++total_count;
	}
	return total_count + txt_record;
}

static inline int
mdns_query_answer_unicast(int sock, const void* address, size_t address_size, void* buffer,
                          size_t capacity, uint16_t query_id, mdns_record_type_t record_type,
                          const char* name, size_t name_length, mdns_record_t answer,
                          const mdns_record_t* authority, size_t authority_count,
                          const mdns_record_t* additional, size_t additional_count) {
	if (capacity < (sizeof(struct mdns_header_t) + 32 + 4))
		return -1;

	// According to RFC 6762:
	// The cache-flush bit MUST NOT be set in any resource records in a response message
	// sent in legacy unicast responses to UDP ports other than 5353.
	uint16_t rclass = MDNS_CLASS_IN;
	uint32_t ttl = 10;

	// Basic answer structure
	struct mdns_header_t* header = (struct mdns_header_t*)buffer;
	header->query_id = htons(query_id);
	header->flags = htons(0x8400);
	header->questions = htons(1);
	header->answer_rrs = htons(1);
	header->authority_rrs = htons(mdns_answer_get_record_count(authority, authority_count));
	header->additional_rrs = htons(mdns_answer_get_record_count(additional, additional_count));

	mdns_string_table_t string_table = {{0}, 0, 0};
	void* data = MDNS_POINTER_OFFSET(buffer, sizeof(struct mdns_header_t));

	// Fill in question
	data = mdns_answer_add_question_unicast(buffer, capacity, data, record_type, name, name_length,
	                                        &string_table);

	// Fill in answer
	answer.rclass = rclass;
	answer.ttl = ttl;
	data = mdns_answer_add_record(buffer, capacity, data, answer, &string_table);

	// Fill in authority records
	for (size_t irec = 0; data && (irec < authority_count); ++irec) {
		mdns_record_t record = authority[irec];
		record.rclass = rclass;
		if (!record.ttl)
			record.ttl = ttl;
		data = mdns_answer_add_record(buffer, capacity, data, record, &string_table);
	}
	data = mdns_answer_add_txt_record(buffer, capacity, data, authority, authority_count,
	                                  rclass, ttl, &string_table);

	// Fill in additional records
	for (size_t irec = 0; data && (irec < additional_count); ++irec) {
		mdns_record_t record = additional[irec];
		record.rclass = rclass;
		if (!record.ttl)
			record.ttl = ttl;
		data = mdns_answer_add_record(buffer, capacity, data, record, &string_table);
	}
	data = mdns_answer_add_txt_record(buffer, capacity, data, additional, additional_count,
	                                  rclass, ttl, &string_table);
	if (!data)
		return -1;

	size_t tosend = MDNS_POINTER_DIFF(data, buffer);
	return mdns_unicast_send(sock, address, address_size, buffer, tosend);
}

static inline int
mdns_answer_multicast_rclass_ttl(int sock, void* buffer, size_t capacity, mdns_record_t answer,
                                 const mdns_record_t* authority, size_t authority_count,
                                 const mdns_record_t* additional, size_t additional_count,
                                 uint16_t rclass, uint32_t ttl) {
	if (capacity < (sizeof(struct mdns_header_t) + 32 + 4))
		return -1;

	// Basic answer structure
	struct mdns_header_t* header = (struct mdns_header_t*)buffer;
	header->query_id = 0;
	header->flags = htons(0x8400);
	header->questions = 0;
	header->answer_rrs = htons(1);
	header->authority_rrs = htons(mdns_answer_get_record_count(authority, authority_count));
	header->additional_rrs = htons(mdns_answer_get_record_count(additional, additional_count));

	mdns_string_table_t string_table = {{0}, 0, 0};
	void* data = MDNS_POINTER_OFFSET(buffer, sizeof(struct mdns_header_t));

	// Fill in answer
	mdns_record_t record = answer;
	mdns_record_update_rclass_ttl(&record, rclass, ttl);
	data = mdns_answer_add_record(buffer, capacity, data, record, &string_table);

	// Fill in authority records
	for (size_t irec = 0; data && (irec < authority_count); ++irec) {
		record = authority[irec];
		mdns_record_update_rclass_ttl(&record, rclass, ttl);
		data = mdns_answer_add_record(buffer, capacity, data, record, &string_table);
	}
	data = mdns_answer_add_txt_record(buffer, capacity, data, authority, authority_count,
	                                  rclass, ttl, &string_table);

	// Fill in additional records
	for (size_t irec = 0; data && (irec < additional_count); ++irec) {
		record = additional[irec];
		mdns_record_update_rclass_ttl(&record, rclass, ttl);
		data = mdns_answer_add_record(buffer, capacity, data, record, &string_table);
	}
	data = mdns_answer_add_txt_record(buffer, capacity, data, additional, additional_count,
	                                  rclass, ttl, &string_table);
	if (!data)
		return -1;

	size_t tosend = MDNS_POINTER_DIFF(data, buffer);
	return mdns_multicast_send(sock, buffer, tosend);
}

static inline int
mdns_query_answer_multicast(int sock, void* buffer, size_t capacity, mdns_record_t answer,
                            const mdns_record_t* authority, size_t authority_count,
                            const mdns_record_t* additional, size_t additional_count) {
	return mdns_answer_multicast_rclass_ttl(sock, buffer, capacity, answer, authority,
	                                        authority_count, additional, additional_count,
	                                        MDNS_CLASS_IN, 60);
}

static inline int
mdns_announce_multicast(int sock, void* buffer, size_t capacity, mdns_record_t answer,
                        const mdns_record_t* authority, size_t authority_count,
                        const  mdns_record_t* additional, size_t additional_count) {
	return mdns_answer_multicast_rclass_ttl(sock, buffer, capacity, answer, authority,
	                                        authority_count, additional, additional_count,
	                                        MDNS_CLASS_IN | MDNS_CACHE_FLUSH, 60);
}

static inline int
mdns_goodbye_multicast(int sock, void* buffer, size_t capacity, mdns_record_t answer,
                       const mdns_record_t* authority, size_t authority_count,
                       const mdns_record_t* additional, size_t additional_count) {
	// Goodbye should have ttl of 0
	return mdns_answer_multicast_rclass_ttl(sock, buffer, capacity, answer, authority,
	                                        authority_count, additional, additional_count,
	                                        MDNS_CLASS_IN, 0);
}

static inline mdns_string_t
mdns_record_parse_ptr(const void* buffer, size_t size, size_t offset, size_t length,
                      char* strbuffer, size_t capacity) {
	// PTR record is just a string
	if ((size >= offset + length) && (length >= 2))
		return mdns_string_extract(buffer, size, &offset, strbuffer, capacity);
	mdns_string_t empty = {0, 0};
	return empty;
}

static inline mdns_record_srv_t
mdns_record_parse_srv(const void* buffer, size_t size, size_t offset, size_t length,
                      char* strbuffer, size_t capacity) {
	mdns_record_srv_t srv;
	memset(&srv, 0, sizeof(mdns_record_srv_t));
	// Read the service priority, weight, port number and the discovery name
	// SRV record format (http://www.ietf.org/rfc/rfc2782.txt):
	// 2 bytes network-order unsigned priority
	// 2 bytes network-order unsigned weight
	// 2 bytes network-order unsigned port
	// string: discovery (domain) name, minimum 2 bytes when compressed
	if ((size >= offset + length) && (length >= 8)) {
		const uint16_t* recorddata = (const uint16_t*)MDNS_POINTER_OFFSET_CONST(buffer, offset);
		srv.priority = mdns_ntohs(recorddata++);
		srv.weight = mdns_ntohs(recorddata++);
		srv.port = mdns_ntohs(recorddata++);
		offset += 6;
		srv.name = mdns_string_extract(buffer, size, &offset, strbuffer, capacity);
	}
	return srv;
}

static inline struct sockaddr_in*
mdns_record_parse_a(const void* buffer, size_t size, size_t offset, size_t length,
                    struct sockaddr_in* addr) {
	memset(addr, 0, sizeof(struct sockaddr_in));
	addr->sin_family = AF_INET;
#ifdef __APPLE__
	addr->sin_len = sizeof(struct sockaddr_in);
#endif
	if ((size >= offset + length) && (length == 4))
		memcpy(&addr->sin_addr.s_addr, MDNS_POINTER_OFFSET(buffer, offset), 4);
	return addr;
}

static inline struct sockaddr_in6*
mdns_record_parse_aaaa(const void* buffer, size_t size, size_t offset, size_t length,
                       struct sockaddr_in6* addr) {
	memset(addr, 0, sizeof(struct sockaddr_in6));
	addr->sin6_family = AF_INET6;
#ifdef __APPLE__
	addr->sin6_len = sizeof(struct sockaddr_in6);
#endif
	if ((size >= offset + length) && (length == 16))
		memcpy(&addr->sin6_addr, MDNS_POINTER_OFFSET(buffer, offset), 16);
	return addr;
}

static inline size_t
mdns_record_parse_txt(const void* buffer, size_t size, size_t offset, size_t length,
                      mdns_record_txt_t* records, size_t capacity) {
	size_t parsed = 0;
	const char* strdata;
	size_t end = offset + length;

	if (size < end)
		end = size;

	while ((offset < end) && (parsed < capacity)) {
		strdata = (const char*)MDNS_POINTER_OFFSET(buffer, offset);
		size_t sublength = *(const unsigned char*)strdata;

		++strdata;
		offset += sublength + 1;

		size_t separator = 0;
		for (size_t c = 0; c < sublength; ++c) {
			// DNS-SD TXT record keys MUST be printable US-ASCII, [0x20, 0x7E]
			if ((strdata[c] < 0x20) || (strdata[c] > 0x7E))
				break;
			if (strdata[c] == '=') {
				separator = c;
				break;
			}
		}

		if (!separator)
			continue;

		if (separator < sublength) {
			records[parsed].key.str = strdata;
			records[parsed].key.length = separator;
			records[parsed].value.str = strdata + separator + 1;
			records[parsed].value.length = sublength - (separator + 1);
		} else {
			records[parsed].key.str = strdata;
			records[parsed].key.length = sublength;
		}

		++parsed;
	}

	return parsed;
}

#ifdef _WIN32
#undef strncasecmp
#endif

#ifdef __cplusplus
}
#endif
