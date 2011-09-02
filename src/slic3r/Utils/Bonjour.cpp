#include "Bonjour.hpp"

#include <cstdint>
#include <algorithm>
#include <array>
#include <vector>
#include <string>
#include <map>
#include <thread>
#include <boost/endian/conversion.hpp>
#include <boost/date_time/posix_time/posix_time_duration.hpp>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/bind/bind.hpp>

using boost::optional;
using boost::system::error_code;
namespace endian = boost::endian;
namespace asio = boost::asio;
using boost::asio::ip::udp;


namespace Slic3r {


// Minimal implementation of a MDNS/DNS-SD client
// This implementation is extremely simple, only the bits that are useful
// for basic MDNS discovery of OctoPi devices are present.
// However, the bits that are present are implemented with security in mind.
// Only fully correct DNS replies are allowed through.
// While decoding the decoder will bail the moment it encounters anything fishy.
// At least that's the idea. To help prove this is actually the case,
// the implementations has been tested with AFL.


// Relevant RFCs:
//    https://tools.ietf.org/html/rfc6762.txt
//    https://tools.ietf.org/html/rfc6763.txt


struct DnsName: public std::string
{
	enum
	{
		MAX_RECURSION = 10,     // Keep this low
	};

	static optional<DnsName> decode(const std::vector<char> &buffer, size_t &offset, unsigned depth = 0)
	{
		// Check offset sanity:
		if (offset + 1 >= buffer.size()) {
			return boost::none;
		}

		// Check for recursion depth to prevent parsing names that are nested too deeply or end up cyclic:
		if (depth >= MAX_RECURSION) {
			return boost::none;
		}

		DnsName res;
		const size_t bsize = buffer.size();

		while (true) {
			const char* ptr = buffer.data() + offset;
			unsigned len = static_cast<unsigned char>(*ptr);
			if (len & 0xc0) {
				// This is a recursive label
				unsigned len_2 = static_cast<unsigned char>(ptr[1]);
				size_t pointer = (len & 0x3f) << 8 | len_2;
				const auto nested = decode(buffer, pointer, depth + 1);
				if (!nested) {
					return boost::none;
				} else {
					if (res.size() > 0) {
						res.push_back('.');
					}
					res.append(*nested);
					offset += 2;
					return std::move(res);
				}
			} else if (len == 0) {
				// This is a name terminator
				offset++;
				break;
			} else {
				// This is a regular label
				len &= 0x3f;
				if (len + offset + 1 >= bsize) {
					return boost::none;
				}

				res.reserve(len);
				if (res.size() > 0) {
					res.push_back('.');
				}

				ptr++;
				for (const auto end = ptr + len; ptr < end; ptr++) {
					char c = *ptr;
					if (c >= 0x20 && c <= 0x7f) {
						res.push_back(c);
					} else {
						return boost::none;
					}
				}

				offset += len + 1;
			}
		}

		if (res.size() > 0) {
			return std::move(res);
		} else {
			return boost::none;
		}
	}
};

struct DnsHeader
{
	uint16_t id;
	uint16_t flags;
	uint16_t qdcount;
	uint16_t ancount;
	uint16_t nscount;
	uint16_t arcount;

	enum
	{
		SIZE = 12,
	};

	static DnsHeader decode(const std::vector<char> &buffer) {
		DnsHeader res;
		const uint16_t *data_16 = reinterpret_cast<const uint16_t*>(buffer.data());
		res.id = endian::big_to_native(data_16[0]);
		res.flags = endian::big_to_native(data_16[1]);
		res.qdcount = endian::big_to_native(data_16[2]);
		res.ancount = endian::big_to_native(data_16[3]);
		res.nscount = endian::big_to_native(data_16[4]);
		res.arcount = endian::big_to_native(data_16[5]);
		return res;
	}

	uint32_t rrcount() const {
		return ancount + nscount + arcount;
	}
};

struct DnsQuestion
{
	enum
	{
		MIN_SIZE = 5,
	};

	DnsName name;
	uint16_t type;
	uint16_t qclass;

	DnsQuestion()
		: type(0)
		, qclass(0)
	{}

	static optional<DnsQuestion> decode(const std::vector<char> &buffer, size_t &offset)
	{
		auto qname = DnsName::decode(buffer, offset);
		if (!qname) {
			return boost::none;
		}

		DnsQuestion res;
		res.name = std::move(*qname);
		const uint16_t *data_16 = reinterpret_cast<const uint16_t*>(buffer.data() + offset);
		res.type = endian::big_to_native(data_16[0]);
		res.qclass = endian::big_to_native(data_16[1]);

		offset += 4;
		return std::move(res);
	}
};

struct DnsResource
{
	DnsName name;
	uint16_t type;
	uint16_t rclass;
	uint32_t ttl;
	std::vector<char> data;

	DnsResource()
		: type(0)
		, rclass(0)
		, ttl(0)
	{}

	static optional<DnsResource> decode(const std::vector<char> &buffer, size_t &offset, size_t &dataoffset)
	{
		const size_t bsize = buffer.size();
		if (offset + 1 >= bsize) {
			return boost::none;
		}

		auto rname = DnsName::decode(buffer, offset);
		if (!rname) {
			return boost::none;
		}

		if (offset + 10 >= bsize) {
			return boost::none;
		}

		DnsResource res;
		res.name = std::move(*rname);
		const uint16_t *data_16 = reinterpret_cast<const uint16_t*>(buffer.data() + offset);
		res.type = endian::big_to_native(data_16[0]);
		res.rclass = endian::big_to_native(data_16[1]);
		res.ttl = endian::big_to_native(*reinterpret_cast<const uint32_t*>(data_16 + 2));
		uint16_t rdlength = endian::big_to_native(data_16[4]);

		offset += 10;
		if (offset + rdlength > bsize) {
			return boost::none;
		}

		dataoffset = offset;
        res.data = std::vector<char>(buffer.begin() + offset, buffer.begin() + offset + rdlength);
		offset += rdlength;

		return std::move(res);
	}
};

struct DnsRR_A
{
	enum { TAG = 0x1 };

	asio::ip::address_v4 ip;
	std::string name;

	static void decode(optional<DnsRR_A> &result, const DnsResource &rr)
	{
		if (rr.data.size() == 4) {
			DnsRR_A res;
			const uint32_t ip = endian::big_to_native(*reinterpret_cast<const uint32_t*>(rr.data.data()));
			res.ip = asio::ip::address_v4(ip);
			result = std::move(res);
		}
	}
};

struct DnsRR_AAAA
{
	enum { TAG = 0x1c };

	asio::ip::address_v6 ip;
	std::string name;

	static void decode(optional<DnsRR_AAAA> &result, const DnsResource &rr)
	{
		if (rr.data.size() == 16) {
			DnsRR_AAAA res;
			std::array<unsigned char, 16> ip;
			std::copy_n(rr.data.begin(), 16, ip.begin());
			res.ip = asio::ip::address_v6(ip);
			result = std::move(res);
		}
	}
};

struct DnsRR_SRV
{
	enum
	{
		TAG = 0x21,
		MIN_SIZE = 8,
	};

	uint16_t priority;
	uint16_t weight;
	uint16_t port;
	DnsName hostname;

	static optional<DnsRR_SRV> decode(const std::vector<char> &buffer, const DnsResource &rr, size_t dataoffset)
	{
		if (rr.data.size() < MIN_SIZE) {
			return boost::none;
		}

		DnsRR_SRV res;

		const uint16_t *data_16 = reinterpret_cast<const uint16_t*>(rr.data.data());
		res.priority = endian::big_to_native(data_16[0]);
		res.weight = endian::big_to_native(data_16[1]);
		res.port = endian::big_to_native(data_16[2]);

		size_t offset = dataoffset + 6;
		auto hostname = DnsName::decode(buffer, offset);

		if (hostname) {
			res.hostname = std::move(*hostname);
			return std::move(res);
		} else {
			return boost::none;
		}
	}
};

struct DnsRR_TXT
{
	enum
	{
		TAG = 0x10,
	};

	BonjourReply::TxtData data;

	static optional<DnsRR_TXT> decode(const DnsResource &rr, const Bonjour::TxtKeys &txt_keys)
	{
		const size_t size = rr.data.size();
		if (size < 2) {
			return boost::none;
		}

		DnsRR_TXT res;

		for (auto it = rr.data.begin(); it != rr.data.end(); ) {
			unsigned val_size = static_cast<unsigned char>(*it);
			if (val_size == 0 || it + val_size >= rr.data.end()) {
				return boost::none;
			}
			++it;

			const auto it_end = it + val_size;
			const auto it_eq = std::find(it, it_end, '=');
			if (it_eq > it && it_eq < it_end - 1) {
				std::string key(it_eq - it, ' ');
				std::copy(it, it_eq, key.begin());

				if (txt_keys.find(key) != txt_keys.end() || key == "path") {
					// This key-value has been requested for
					std::string value(it_end - it_eq - 1, ' ');
					std::copy(it_eq + 1, it_end, value.begin());
					res.data.insert(std::make_pair(std::move(key), std::move(value)));
				}
			}

			it = it_end;
		}

		return std::move(res);
	}
};

struct DnsSDPair
{
	optional<DnsRR_SRV> srv;
	optional<DnsRR_TXT> txt;
};

struct DnsSDMap : public std::map<std::string, DnsSDPair>
{
	void insert_srv(std::string &&name, DnsRR_SRV &&srv)
	{
		auto hit = this->find(name);
		if (hit != this->end()) {
			hit->second.srv = std::move(srv);
		} else {
			DnsSDPair pair;
			pair.srv = std::move(srv);
			this->insert(std::make_pair(std::move(name), std::move(pair)));
		}
	}

	void insert_txt(std::string &&name, DnsRR_TXT &&txt)
	{
		auto hit = this->find(name);
		if (hit != this->end()) {
			hit->second.txt = std::move(txt);
		} else {
			DnsSDPair pair;
			pair.txt = std::move(txt);
			this->insert(std::make_pair(std::move(name), std::move(pair)));
		}
	}
};

struct DnsMessage
{
	enum
	{
		MAX_SIZE = 4096,
		MAX_ANS = 30,
	};

	DnsHeader header;
	optional<DnsQuestion> question;

	optional<DnsRR_A> rr_a;
	optional<DnsRR_AAAA> rr_aaaa;
	std::vector<DnsRR_SRV> rr_srv;

	DnsSDMap sdmap;

	static optional<DnsMessage> decode(const std::vector<char>& buffer, const Bonjour::TxtKeys& txt_keys)
	{
		const auto size = buffer.size();
		if (size < DnsHeader::SIZE + DnsQuestion::MIN_SIZE || size > MAX_SIZE) {
			return boost::none;
		}

		DnsMessage res;
		res.header = DnsHeader::decode(buffer);

		if (res.header.qdcount > 1 || res.header.ancount > MAX_ANS) {
			return boost::none;
		}

		size_t offset = DnsHeader::SIZE;
		if (res.header.qdcount == 1) {
			res.question = DnsQuestion::decode(buffer, offset);
		}

		for (unsigned i = 0; i < res.header.rrcount(); i++) {
			size_t dataoffset = 0;
			auto rr = DnsResource::decode(buffer, offset, dataoffset);
			if (!rr) {
				return boost::none;
			}
			else {
				res.parse_rr(buffer, std::move(*rr), dataoffset, txt_keys);
			}
		}

		return std::move(res);
	}
private:
	void parse_rr(const std::vector<char>& buffer, DnsResource&& rr, size_t dataoffset, const Bonjour::TxtKeys& txt_keys)
	{
		switch (rr.type) {
		case DnsRR_A::TAG:
			DnsRR_A::decode(this->rr_a, rr);
			this->rr_a->name = rr.name;
			break;
		case DnsRR_AAAA::TAG:
			DnsRR_AAAA::decode(this->rr_aaaa, rr);
			this->rr_aaaa->name = rr.name;
			break;
		case DnsRR_SRV::TAG: {
			auto srv = DnsRR_SRV::decode(buffer, rr, dataoffset);
			if (srv) { this->sdmap.insert_srv(std::move(rr.name), std::move(*srv)); }
			break;
		}
		case DnsRR_TXT::TAG: {
			auto txt = DnsRR_TXT::decode(rr, txt_keys);
			if (txt) { this->sdmap.insert_txt(std::move(rr.name), std::move(*txt)); }
			break;
		}
		}
	}
};

std::ostream& operator<<(std::ostream &os, const DnsMessage &msg)
{
	os << boost::format("DnsMessage(ID: %1%, Q: %2%, A: %3%, AAAA: %4%, services: [")
		% msg.header.id
		% (msg.question ? msg.question->name.c_str() : "none")
		% (msg.rr_a ? msg.rr_a->ip.to_string() : "none")
		% (msg.rr_aaaa ? msg.rr_aaaa->ip.to_string() : "none");

	enum { SRV_PRINT_MAX = 3 };
	unsigned i = 0;
	for (const auto &sdpair : msg.sdmap) {
		if (i > 0) { os << ", "; }

		if (i < SRV_PRINT_MAX) {
			os << sdpair.first;
		} else {
			os << "...";
			break;
		}

		i++;
	}

	return os << "])";
}

const asio::ip::address_v4 BonjourRequest::MCAST_IP4{ 0xe00000fb };
const asio::ip::address_v6 BonjourRequest::MCAST_IP6 = asio::ip::make_address_v6("ff02::fb");
const uint16_t BonjourRequest::MCAST_PORT = 5353;

optional<BonjourRequest> BonjourRequest::make_PTR(const std::string &service, const std::string &protocol)
{
	if (service.size() > 15 || protocol.size() > 15) {
		return boost::none;
	}

	std::vector<char> data;
	data.reserve(service.size() + 18);

	// Add metadata
	static const unsigned char rq_meta[] = {
		0x00, 0x00, // Query ID (zero for mDNS)
		0x00, 0x00, // Flags
		0x00, 0x01, // One query
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00   // Zero Answer, Authority, and Additional RRs
	};
	std::copy(rq_meta, rq_meta + sizeof(rq_meta), std::back_inserter(data));

	// Add PTR query name
	data.push_back(service.size() + 1);
	data.push_back('_');
	data.insert(data.end(), service.begin(), service.end());
	data.push_back(protocol.size() + 1);
	data.push_back('_');
	data.insert(data.end(), protocol.begin(), protocol.end());

	// Add the rest of PTR record
	static const unsigned char ptr_tail[] = {
		0x05, // length of "label"
		0x6c, 0x6f, 0x63, 0x61, 0x6c, 0x00, // "label" string and terminator
		0x00, 0x0c, // Type PTR
		0x00, 0xff, // Class ANY
	};
	std::copy(ptr_tail, ptr_tail + sizeof(ptr_tail), std::back_inserter(data));

	return BonjourRequest(std::move(data));
}

optional<BonjourRequest> BonjourRequest::make_A(const std::string& hostname)
{
	// todo: why is this and what is real max
	if (hostname.size() > 30) {
		return boost::none;
	}

	std::vector<char> data;
	data.reserve(hostname.size() + 18);

	// Add metadata
	static const unsigned char rq_meta[] = {
		0x00, 0x00, // Query ID (zero for mDNS)
		0x00, 0x00, // Flags
		0x00, 0x01, // One query
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00   // Zero Answer, Authority, and Additional RRs
	};
	std::copy(rq_meta, rq_meta + sizeof(rq_meta), std::back_inserter(data));

	// Add hostname without .local
	data.push_back(hostname.size());
	data.insert(data.end(), hostname.begin(), hostname.end());
	
	// Add the rest of A record
	static const unsigned char ptr_tail[] = {
		0x05, // length of "local"
		0x6c, 0x6f, 0x63, 0x61, 0x6c, 0x00,// "local" string and terminator
		0x00, 0x01, // Type A
		0x00, 0xff, // Class - 01 is internet 0xff is any
	};
	std::copy(ptr_tail, ptr_tail + sizeof(ptr_tail), std::back_inserter(data));

	return BonjourRequest(std::move(data));
}

optional<BonjourRequest> BonjourRequest::make_AAAA(const std::string& hostname)
{
	// todo: why is this and what is real max
	if (hostname.size() > 30) {
		return boost::none;
	}

	std::vector<char> data;
	data.reserve(hostname.size() + 18);

	// Add metadata
	static const unsigned char rq_meta[] = {
		0x00, 0x00, // Query ID (zero for mDNS)
		0x00, 0x00, // Flags
		0x00, 0x01, // One query
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00   // Zero Answer, Authority, and Additional RRs
	};
	std::copy(rq_meta, rq_meta + sizeof(rq_meta), std::back_inserter(data));

	// Add hostname without .local
	data.push_back(hostname.size());
	data.insert(data.end(), hostname.begin(), hostname.end());

	// Add the rest of A record
	static const unsigned char ptr_tail[] = {
		0x05, // length of "local"
		0x6c, 0x6f, 0x63, 0x61, 0x6c, 0x00, // "local" string and terminator
		0x00, 0x1c, // Type AAAA
		0x00, 0xff, // Class - 01 is internet 0xff is any 
	};
	std::copy(ptr_tail, ptr_tail + sizeof(ptr_tail), std::back_inserter(data));

	return BonjourRequest(std::move(data));
}

namespace {
std::string strip_service_dn(const std::string& service_name, const std::string& service_dn)
{
	if (service_name.size() <= service_dn.size()) {
		return std::string();
	}

	auto needle = service_name.rfind(service_dn);
	if (needle == service_name.size() - service_dn.size()) {
		return service_name.substr(0, needle - 1);
	} else {
		return std::string();
	}
}
} // namespace

UdpSession::UdpSession(Bonjour::ReplyFn rfn) : replyfn(rfn)
{
	buffer.resize(DnsMessage::MAX_SIZE);
}

UdpSocket::UdpSocket( Bonjour::ReplyFn replyfn, const asio::ip::address& multicast_address, const asio::ip::address& interface_address, std::shared_ptr< boost::asio::io_service > io_service)
	: replyfn(replyfn)
	, multicast_address(multicast_address)
	, socket(*io_service)
	, io_service(io_service)
{
	try {
		// open socket
		boost::asio::ip::udp::endpoint listen_endpoint(multicast_address.is_v4() ? udp::v4() : udp::v6(), BonjourRequest::MCAST_PORT);
		socket.open(listen_endpoint.protocol());
		// set socket to listen
		socket.set_option(udp::socket::reuse_address(true));
		socket.bind(listen_endpoint);
		if (interface_address.is_v4()) {
			// listen for multicast on given interface
			socket.set_option(boost::asio::ip::multicast::join_group(multicast_address.to_v4(), interface_address.to_v4()));
			// send to interface
			socket.set_option(asio::ip::multicast::outbound_interface(interface_address.to_v4()));
		} else {
			// listen for multicast on given interface
			socket.set_option(boost::asio::ip::multicast::join_group(multicast_address.to_v6(), interface_address.to_v6().scope_id()));
			// send to interface
			socket.set_option(asio::ip::multicast::outbound_interface(interface_address.to_v6().scope_id()));
		}
		mcast_endpoint = udp::endpoint(multicast_address, BonjourRequest::MCAST_PORT);
		
		BOOST_LOG_TRIVIAL(info) << "Socket created. Multicast: " << multicast_address << ". Interface: " << interface_address;
	}
	catch (std::exception& e) {
		BOOST_LOG_TRIVIAL(error) << e.what();
	}
}


UdpSocket::UdpSocket( Bonjour::ReplyFn replyfn, const asio::ip::address& multicast_address, std::shared_ptr< boost::asio::io_service > io_service)
	: replyfn(replyfn)
	, multicast_address(multicast_address)
	, socket(*io_service)
	, io_service(io_service)
{
	try {
		// open socket
		boost::asio::ip::udp::endpoint listen_endpoint(multicast_address.is_v4() ? udp::v4() : udp::v6(), BonjourRequest::MCAST_PORT);
		socket.open(listen_endpoint.protocol());
		// set socket to listen
		socket.set_option(udp::socket::reuse_address(true));
		socket.bind(listen_endpoint);
		socket.set_option(boost::asio::ip::multicast::join_group(multicast_address));
		mcast_endpoint = udp::endpoint(multicast_address, BonjourRequest::MCAST_PORT);
		
		BOOST_LOG_TRIVIAL(info) << "Socket created. Multicast: " << multicast_address;
	}
	catch (std::exception& e) {
		BOOST_LOG_TRIVIAL(error) << e.what();
	}
}

void UdpSocket::send()
{
	try {
		for (const auto& request : requests)
			socket.send_to(asio::buffer(request.m_data), mcast_endpoint);
		
		// Should we care if this is called while already receiving? (async_receive call from receive_handler)
		async_receive();
	}
	catch (std::exception& e) {
		BOOST_LOG_TRIVIAL(error) << e.what();
	}
}

void UdpSocket::async_receive()
{
	try {
		// our session to hold the buffer + endpoint
		auto session = create_session();
		socket.async_receive_from(asio::buffer(session->buffer, session->buffer.size())
			, session->remote_endpoint
			, boost::bind(&UdpSocket::receive_handler, this, session, asio::placeholders::error, asio::placeholders::bytes_transferred));
	}
	catch (std::exception& e) {
		BOOST_LOG_TRIVIAL(error) << e.what();
	}
}

void UdpSocket::receive_handler(SharedSession session, const boost::system::error_code& error, size_t bytes)
{
	// let io_service to handle the datagram on session
	// from boost documentation io_service::post:
	// The io_service guarantees that the handler will only be called in a thread in which the run(), run_one(), poll() or poll_one() member functions is currently being invoked.
	io_service->post(boost::bind(&UdpSession::handle_receive, session, error, bytes));
	// immediately accept new datagrams
	async_receive();
}

SharedSession LookupSocket::create_session() const
{ 
	return std::shared_ptr< LookupSession >(new LookupSession(this, replyfn));
}


void LookupSession::handle_receive(const error_code& error, size_t bytes)
{
	assert(socket);

	if (error) {
		BOOST_LOG_TRIVIAL(error) << error.message();
		return;
	}
	if (bytes == 0 || !replyfn) {
		return;
	}

	buffer.resize(bytes);
	auto dns_msg = DnsMessage::decode(buffer, socket->get_txt_keys());
	if (dns_msg) {
		asio::ip::address ip = remote_endpoint.address();
		if (dns_msg->rr_a) { ip = dns_msg->rr_a->ip; }
		else if (dns_msg->rr_aaaa) { ip = dns_msg->rr_aaaa->ip; }

		for (auto& sdpair : dns_msg->sdmap) {
			if (!sdpair.second.srv) {
				continue;
			}

			const auto& srv = *sdpair.second.srv;

			auto service_name = strip_service_dn(sdpair.first, socket->get_service_dn());
			if (service_name.empty())
				continue;

			std::string path;
			std::string version;

			BonjourReply::TxtData txt_data;
			if (sdpair.second.txt) {
				txt_data = std::move(sdpair.second.txt->data);
			}

			BonjourReply reply(ip, srv.port, std::move(service_name), srv.hostname, std::move(txt_data));
			replyfn(std::move(reply));
		}
	}
}

SharedSession ResolveSocket::create_session() const 
{ 
	return std::shared_ptr< ResolveSession > (new ResolveSession(this, replyfn));
}


void ResolveSession::handle_receive(const error_code& error, size_t bytes)
{
	assert(socket);
	if (error) {
		// todo: what level? do we even log? There might be callbacks when timer runs out
		BOOST_LOG_TRIVIAL(info) << error.message();
		return;
	}
	if (bytes == 0 || !replyfn) {
		// todo: log something?
		return;
	}

	buffer.resize(bytes);
#if 0
	std::string str;
	char const hex_chars[16] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F' };
	for (size_t i = 0; i < buffer.size(); i++) {
		const char ch = buffer[i];
		str += hex_chars[(ch & 0xF0) >> 4];
		str += hex_chars[(ch & 0x0F) >> 0];
	}
	BOOST_LOG_TRIVIAL(debug) << remote_endpoint.address()<< " " << str;
#endif
	// decode buffer, txt keys are not needed for A / AAAA answer
	auto dns_msg = DnsMessage::decode(buffer, Bonjour::TxtKeys());
	if (dns_msg) {
		asio::ip::address ip;
		std::string answer_name;
		if (dns_msg->rr_a) { 
			ip = dns_msg->rr_a->ip; 
			answer_name = dns_msg->rr_a->name;
		}
		else if (dns_msg->rr_aaaa) { 
			ip = dns_msg->rr_aaaa->ip; 
			answer_name = dns_msg->rr_aaaa->name;
		}
		else 
			return; // not matching query type with answer type

		if (!answer_name.empty()) {
			// transform both strings to lower. Should we really do it?
			std::string name_tolower = answer_name;
			std::transform(name_tolower.begin(), name_tolower.end(), name_tolower.begin(),
				[](unsigned char c) { return std::tolower(c); });
			std::string hostname_tolower = socket->get_hostname();
			std::transform(hostname_tolower.begin(), hostname_tolower.end(), hostname_tolower.begin(),
				[](unsigned char c) { return std::tolower(c); });
			if (name_tolower == hostname_tolower) {
				BonjourReply reply(ip, 0, std::string(), answer_name, BonjourReply::TxtData());
				replyfn(std::move(reply));
			}
		}
	}
}

// API - private part

struct Bonjour::priv
{
	const std::string service;
	std::string protocol;
	std::string service_dn;
	TxtKeys txt_keys;
	unsigned timeout;
	unsigned retries;
	std::string hostname;

//	std::vector<BonjourReply> replies;

	std::vector<char> buffer;
	std::thread io_thread;
	Bonjour::ReplyFn replyfn;
	Bonjour::CompleteFn completefn;
	Bonjour::ResolveFn resolvefn;

	priv(std::string&& service);

	//	void udp_receive_lookup(udp::endpoint from, size_t bytes);
	void lookup_perform();
	void resolve_perform();
};

Bonjour::priv::priv(std::string&& service)
	: service(std::move(service))
	, protocol("tcp")
	, timeout(10)
	, retries(1)
{
	buffer.resize(DnsMessage::MAX_SIZE);
}

void Bonjour::priv::lookup_perform()
{
	service_dn = (boost::format("_%1%._%2%.local") % service % protocol).str();

	std::shared_ptr< boost::asio::io_service > io_service(new boost::asio::io_service);

	std::vector<LookupSocket*> sockets;

	// resolve intefaces - from PR#6646
	std::vector<boost::asio::ip::address> interfaces;
	asio::ip::udp::resolver resolver(*io_service);
	boost::system::error_code ec;
	// ipv4 interfaces
	auto results = resolver.resolve(udp::v4(), asio::ip::host_name(), "", ec);
	if (!ec) {	
		for (const auto & r : results) {
			const auto addr = r.endpoint().address();
			if (addr.is_loopback()) continue;
			interfaces.emplace_back(std::move(addr));
		}
		// create ipv4 socket for each interface
		// each will send to querry to for both ipv4 and ipv6
		for (const auto& intrfc : interfaces) 		
			sockets.emplace_back(new LookupSocket(txt_keys, service, service_dn, protocol, replyfn, BonjourRequest::MCAST_IP4, intrfc, io_service));
	} else {
		BOOST_LOG_TRIVIAL(info) << "Failed to resolve ipv4 interfaces: " << ec.message();
	}
	if (sockets.empty())
		sockets.emplace_back(new LookupSocket(txt_keys, service, service_dn, protocol, replyfn, BonjourRequest::MCAST_IP4, io_service));
	// ipv6 interfaces
	interfaces.clear();
	//udp::resolver::query query(host, PORT, boost::asio::ip::resolver_query_base::numeric_service);
	results = resolver.resolve(udp::v6(), asio::ip::host_name(), "", ec);
	if (!ec)
	{
		for (const auto& r : results) {
			const auto addr = r.endpoint().address();
			if (addr.is_loopback()) continue;
			interfaces.emplace_back(std::move(addr));
		}
		// create ipv6 socket for each interface
		// each will send to querry to for both ipv4 and ipv6
		for (const auto& intrfc : interfaces)
			sockets.emplace_back(new LookupSocket(txt_keys, service, service_dn, protocol, replyfn, BonjourRequest::MCAST_IP6, intrfc, io_service));
		if (interfaces.empty())
			sockets.emplace_back(new LookupSocket(txt_keys, service, service_dn, protocol, replyfn, BonjourRequest::MCAST_IP6, io_service));
	} else {
		BOOST_LOG_TRIVIAL(info)<< "Failed to resolve ipv6 interfaces: " << ec.message();
	}
	
	try {
		// send first queries
		for (auto * socket : sockets)
			socket->send();

		// timer settings
		asio::deadline_timer timer(*io_service);
		retries--;
		std::function<void(const error_code&)> timer_handler = [&](const error_code& error) {
			// end 
			if (retries == 0 || error) {
				// is this correct ending?
				io_service->stop();
				if (completefn) {
					completefn();
				}
			// restart timer
			} else {
				retries--;
				timer.expires_from_now(boost::posix_time::seconds(timeout));
				timer.async_wait(timer_handler);
				// trigger another round of queries
				for (auto * socket : sockets)
					socket->send();
			}
		};
		// start timer
		timer.expires_from_now(boost::posix_time::seconds(timeout));
		timer.async_wait(timer_handler);
		// start io_service, it will run until it has something to do - so in this case until stop is called in timer
		io_service->run();
	}
	catch (std::exception& e) {
		BOOST_LOG_TRIVIAL(error) << e.what();
	}
}

void Bonjour::priv::resolve_perform()
{
	// reply callback is shared to every UDPSession which is called on same thread as io_service->run();
	// thus no need to mutex replies in reply_callback, same should go with the timer
	std::vector<BonjourReply> replies;
	// examples would store [self] to the lambda (and the timer one), is it ok not to do it? (Should be c++03)
	const auto reply_callback = [&rpls = replies](BonjourReply&& reply)
	{
		if (std::find(rpls.begin(), rpls.end(), reply) == rpls.end())
			rpls.push_back(reply);
	};

	std::shared_ptr< boost::asio::io_service > io_service(new boost::asio::io_service);
	std::vector<ResolveSocket*> sockets;

	// resolve interfaces - from PR#6646
	std::vector<boost::asio::ip::address> interfaces;
	asio::ip::udp::resolver resolver(*io_service);
	boost::system::error_code ec;
	// ipv4 interfaces
	auto results = resolver.resolve(udp::v4(), asio::ip::host_name(), "", ec);
	if (!ec) {
		for (auto const& r : results) {
			auto const addr = r.endpoint().address();
			if (addr.is_loopback()) continue;
			interfaces.emplace_back(addr);
		}
		// create ipv4 socket for each interface
		// each will send to querry to for both ipv4 and ipv6
		for (const auto& intrfc : interfaces)
			sockets.emplace_back(new ResolveSocket(hostname, reply_callback, BonjourRequest::MCAST_IP4, intrfc, io_service));
	} else {
		BOOST_LOG_TRIVIAL(info) << "Failed to resolve ipv4 interfaces: " << ec.message();
	}
	if (sockets.empty())
		sockets.emplace_back(new ResolveSocket(hostname, reply_callback, BonjourRequest::MCAST_IP4, io_service));

	// ipv6 interfaces
	interfaces.clear();
	results = resolver.resolve(udp::v6(), asio::ip::host_name(), "", ec);
	if (!ec) {
		for (auto const& r : results) {
			auto const addr = r.endpoint().address();
			if (addr.is_loopback()) continue;
			interfaces.emplace_back(addr);
		}
		// create ipv6 socket for each interface
		// each will send to querry to for both ipv4 and ipv6
		for (const auto& intrfc : interfaces) 
			sockets.emplace_back(new ResolveSocket(hostname, reply_callback, BonjourRequest::MCAST_IP6, intrfc, io_service));
		if (interfaces.empty())
			sockets.emplace_back(new ResolveSocket(hostname, reply_callback, BonjourRequest::MCAST_IP6, io_service));
	} else {
		BOOST_LOG_TRIVIAL(info) << "Failed to resolve ipv6 interfaces: " << ec.message();
	}

	try {
		// send first queries
		for (auto * socket : sockets)
			socket->send();

		// timer settings
		asio::deadline_timer timer(*io_service);
		retries--;
		std::function<void(const error_code&)> timer_handler = [&](const error_code& error) {
			int replies_count = replies.size();
			// end 
			if (retries == 0 || error || replies_count > 0) {
				// is this correct ending?
				io_service->stop();
				if (replies_count > 0 && resolvefn) {
					resolvefn(replies);
				}
			// restart timer
			} else {
				retries--;
				timer.expires_from_now(boost::posix_time::seconds(timeout));
				timer.async_wait(timer_handler);
				// trigger another round of queries
				for (auto * socket : sockets)
					socket->send();
			}
		};
		// start timer
		timer.expires_from_now(boost::posix_time::seconds(timeout));
		timer.async_wait(timer_handler);
		// start io_service, it will run until it has something to do - so in this case until stop is called in timer
		io_service->run();
	}
	catch (std::exception& e) {
		BOOST_LOG_TRIVIAL(error) << e.what();
	}
}


// API - public part

BonjourReply::BonjourReply(boost::asio::ip::address ip, uint16_t port, std::string service_name, std::string hostname, BonjourReply::TxtData txt_data)
	: ip(std::move(ip))
	, port(port)
	, service_name(std::move(service_name))
	, hostname(std::move(hostname))
	, txt_data(std::move(txt_data))
{
	std::string proto;
	std::string port_suffix;
	if (port == 443) { proto = "https://"; }
	if (port != 443 && port != 80) { port_suffix = std::to_string(port).insert(0, 1, ':'); }

	std::string path = this->path();
	if (path[0] != '/') { path.insert(0, 1, '/'); }
	full_address = proto + ip.to_string() + port_suffix;
	if (path != "/") { full_address += path; }
	txt_data["path"] = std::move(path);
}

std::string BonjourReply::path() const
{
	const auto it = txt_data.find("path");
	return it != txt_data.end() ? it->second : std::string("/");
}

bool BonjourReply::operator==(const BonjourReply &other) const
{
	return this->full_address == other.full_address
		&& this->service_name == other.service_name;
}

bool BonjourReply::operator<(const BonjourReply &other) const
{
	if (this->ip != other.ip) {
		// So that the common case doesn't involve string comparison
		return this->ip < other.ip;
	} else {
		auto cmp = this->full_address.compare(other.full_address);
		return cmp != 0 ? cmp < 0 : this->service_name < other.service_name;
	}
}

std::ostream& operator<<(std::ostream &os, const BonjourReply &reply)
{
	os << boost::format("BonjourReply(%1%, %2%, %3%, %4%")
		% reply.ip.to_string()
		% reply.service_name
		% reply.hostname
		% reply.full_address;

	for (const auto &kv : reply.txt_data) {
		os << boost::format(", %1%=%2%") % kv.first % kv.second;
	}

	return os << ')';
}


Bonjour::Bonjour(std::string service)
	: p(new priv(std::move(service)))
{}

Bonjour::Bonjour(Bonjour &&other) : p(std::move(other.p)) {}

Bonjour::~Bonjour()
{
	if (p && p->io_thread.joinable()) {
		p->io_thread.detach();
	}
}

Bonjour& Bonjour::set_protocol(std::string protocol)
{
	if (p) { p->protocol = std::move(protocol); }
	return *this;
}

Bonjour& Bonjour::set_txt_keys(TxtKeys txt_keys)
{
	if (p) { p->txt_keys = std::move(txt_keys); }
	return *this;
}

Bonjour& Bonjour::set_timeout(unsigned timeout)
{
	if (p) { p->timeout = timeout; }
	return *this;
}

Bonjour& Bonjour::set_hostname(const std::string& hostname)
{
	if (p) { p->hostname = hostname; }
	return *this;
}

Bonjour& Bonjour::set_retries(unsigned retries)
{
	if (p && retries > 0) { p->retries = retries; }
	return *this;
}

Bonjour& Bonjour::on_reply(ReplyFn fn)
{
	if (p) { p->replyfn = std::move(fn); }
	return *this;
}

Bonjour& Bonjour::on_complete(CompleteFn fn)
{
	if (p) { p->completefn = std::move(fn); }
	return *this;
}

Bonjour& Bonjour::on_resolve(ResolveFn fn)
{
	if (p) { p->resolvefn = std::move(fn); }
	return *this;
}

Bonjour::Ptr Bonjour::lookup()
{
	auto self = std::make_shared<Bonjour>(std::move(*this));

	if (self->p) {
		auto io_thread = std::thread([self]() {
				self->p->lookup_perform();
			});
		self->p->io_thread = std::move(io_thread);
	}

	return self;
}


Bonjour::Ptr Bonjour::resolve()
{
	auto self = std::make_shared<Bonjour>(std::move(*this));

	if (self->p) {
		auto io_thread = std::thread([self]() {
			self->p->resolve_perform();
			});
		self->p->io_thread = std::move(io_thread);
	}

	return self;
}

void Bonjour::resolve_sync()
{
	if (p)
		p->resolve_perform();
}


}

