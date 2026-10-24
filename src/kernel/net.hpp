#ifndef KERNEL_NET_HPP
#define KERNEL_NET_HPP

#include "types.hpp"

namespace net {

// Byte-order utilities (big-endian <-> little-endian)
inline uint16_t htons(uint16_t v) {
    return static_cast<uint16_t>((v << 8) | (v >> 8));
}

inline uint16_t ntohs(uint16_t v) {
    return htons(v);
}

inline uint32_t htonl(uint32_t v) {
    return ((v & 0x000000FFU) << 24) |
           ((v & 0x0000FF00U) << 8)  |
           ((v & 0x00FF0000U) >> 8)  |
           ((v & 0xFF000000U) >> 24);
}

inline uint32_t ntohl(uint32_t v) {
    return htonl(v);
}

// Checksum calculation (RFC 1071)
uint16_t calculate_checksum(const void* data, size_t len);

// IPv4 & MAC Address Helpers
bool parse_ip(const char* str, uint8_t out_ip[4]);
void format_ip(const uint8_t ip[4], char* out_str);
void format_mac(const uint8_t mac[6], char* out_str);
bool ip_equals(const uint8_t a[4], const uint8_t b[4]);
bool mac_equals(const uint8_t a[6], const uint8_t b[6]);

// Protocol Headers
struct EthernetHeader {
    uint8_t  dest_mac[6];
    uint8_t  src_mac[6];
    uint16_t ethertype;
} __attribute__((packed));

constexpr uint16_t ETHERTYPE_IPV4 = 0x0800;
constexpr uint16_t ETHERTYPE_ARP  = 0x0806;

struct ArpHeader {
    uint16_t hw_type;
    uint16_t proto_type;
    uint8_t  hw_len;
    uint8_t  proto_len;
    uint16_t opcode;
    uint8_t  sender_mac[6];
    uint8_t  sender_ip[4];
    uint8_t  target_mac[6];
    uint8_t  target_ip[4];
} __attribute__((packed));

constexpr uint16_t ARP_OP_REQUEST = 1;
constexpr uint16_t ARP_OP_REPLY   = 2;

struct IPv4Header {
    uint8_t  ihl_version; // Version: 4 bits, IHL: 4 bits
    uint8_t  tos;         // Type of Service
    uint16_t total_len;   // Total length of IP packet
    uint16_t id;          // Packet ID
    uint16_t flags_offset;// Flags (3 bits) & Fragment Offset (13 bits)
    uint8_t  ttl;         // Time to Live
    uint8_t  protocol;    // Transport protocol
    uint16_t checksum;    // Header Checksum
    uint8_t  src_ip[4];
    uint8_t  dst_ip[4];
} __attribute__((packed));

constexpr uint8_t IP_PROTO_ICMP = 1;
constexpr uint8_t IP_PROTO_TCP  = 6;
constexpr uint8_t IP_PROTO_UDP  = 17;

struct ICMPHeader {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t sequence;
} __attribute__((packed));

constexpr uint8_t ICMP_TYPE_ECHO_REPLY   = 0;
constexpr uint8_t ICMP_TYPE_ECHO_REQUEST = 8;

struct UDPHeader {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed));

struct TCPHeader {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint16_t flags; // Data offset (4) + Reserved (3) + Flags (9)
    uint16_t window_size;
    uint16_t checksum;
    uint16_t urgent_ptr;
} __attribute__((packed));

constexpr uint16_t TCP_FLAG_FIN = (1 << 0);
constexpr uint16_t TCP_FLAG_SYN = (1 << 1);
constexpr uint16_t TCP_FLAG_RST = (1 << 2);
constexpr uint16_t TCP_FLAG_PSH = (1 << 3);
constexpr uint16_t TCP_FLAG_ACK = (1 << 4);
constexpr uint16_t TCP_FLAG_URG = (1 << 5);

// ARP Cache Entry
struct ArpCacheEntry {
    uint8_t  ip[4];
    uint8_t  mac[6];
    uint64_t timestamp;
    bool     valid;
};
constexpr size_t ARP_CACHE_CAPACITY = 32;

// Protocol Statistics
struct NetStats {
    uint64_t ip_rx;
    uint64_t ip_tx;
    uint64_t ip_dropped;
    uint64_t arp_rx;
    uint64_t arp_tx;
    uint64_t icmp_rx;
    uint64_t icmp_tx;
    uint64_t udp_rx;
    uint64_t udp_tx;
    uint64_t tcp_rx;
    uint64_t tcp_tx;
    uint64_t tcp_connections;
    uint64_t tcp_resets;
};

// Unified Printer callback for Shell and GUI terminal
typedef void (*PrintFn)(const char* line, uint32_t color);

// Initialization & Packet Polling
void init();
void poll();
bool is_available();

// Interface Configuration
const uint8_t* get_mac();
const uint8_t* get_ip();
const uint8_t* get_netmask();
const uint8_t* get_gateway();
const uint8_t* get_dns();

void set_ip(const uint8_t ip[4]);
void set_netmask(const uint8_t mask[4]);
void set_gateway(const uint8_t gw[4]);
void set_dns(const uint8_t dns[4]);

// ARP Subsystem
bool arp_resolve(const uint8_t ip[4], uint8_t out_mac[6], uint32_t timeout_ms = 1000);
void arp_cache_insert(const uint8_t ip[4], const uint8_t mac[6]);
const ArpCacheEntry* get_arp_cache();

// IPv4 Send & Route
bool send_ipv4(const uint8_t dst_ip[4], uint8_t protocol, const void* payload, size_t payload_len);

// ICMP / Ping
bool ping_once(const uint8_t target_ip[4], uint16_t seq, uint32_t timeout_ms, uint32_t* out_rtt, uint8_t* out_ttl);

// UDP Send
bool send_udp(const uint8_t dst_ip[4], uint16_t src_port, uint16_t dst_port, const void* data, size_t len);

// DNS Resolve
bool dns_resolve(const char* hostname, uint8_t out_ip[4], uint32_t timeout_ms = 2000);

// TCP / HTTP Client
int http_get(const uint8_t server_ip[4], uint16_t port, const char* path, char* response_buf, size_t max_len, uint32_t timeout_ms = 5000, const char* host_name = nullptr, const char* accept_encoding = nullptr);

// Raw TCP client (used by the TLS layer). A single connection is active at a time.
bool tcp_connect(const uint8_t server_ip[4], uint16_t port, char* recv_buf, size_t recv_capacity, uint32_t timeout_ms = 5000);
int tcp_send(const void* data, size_t len);
int tcp_recv(void* out, size_t max_len, uint32_t timeout_ms = 5000);
void tcp_close();

// Statistics
const NetStats& get_stats();

// Shell & GUI Commands
void cmd_ifconfig(int argc, char** argv, PrintFn print = nullptr);
void cmd_ping(int argc, char** argv, PrintFn print = nullptr);
void cmd_arp(int argc, char** argv, PrintFn print = nullptr);
void cmd_netstat(int argc, char** argv, PrintFn print = nullptr);
void cmd_curl(int argc, char** argv, PrintFn print = nullptr);
void cmd_wifi(int argc, char** argv, PrintFn print = nullptr);
void cmd_route(int argc, char** argv, PrintFn print = nullptr);

} // namespace net

#endif // KERNEL_NET_HPP
