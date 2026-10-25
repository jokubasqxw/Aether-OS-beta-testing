#include "net.hpp"
#include "wifi.hpp"
#include "e1000.hpp"
#include "printk.hpp"
#include "vga.hpp"
#include "kstring.hpp"
#include "pit.hpp"
#include "heap.hpp"
#include "../arch/x86_64/io.hpp"
#include <stdarg.h>

namespace net {

// Standard Colors (ARGB32 for GUI / converted for VGA)
constexpr uint32_t COLOR_WHITE  = 0xFFFFFFFF;
constexpr uint32_t COLOR_GREEN  = 0xFF55FF55;
constexpr uint32_t COLOR_CYAN   = 0xFF55FFFF;
constexpr uint32_t COLOR_YELLOW = 0xFFFFFF55;
constexpr uint32_t COLOR_RED    = 0xFFFF5555;
constexpr uint32_t COLOR_GREY   = 0xFFAAAAAA;

// Interface Configuration
static uint8_t host_ip[4]      = {10, 0, 2, 15};
static uint8_t netmask[4]      = {255, 255, 255, 0};
static uint8_t gateway_ip[4]   = {10, 0, 2, 2};
static uint8_t dns_ip[4]       = {10, 0, 2, 3};
static uint8_t host_mac[6]     = {0, 0, 0, 0, 0, 0};

// Protocol Statistics
static NetStats stats = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static uint16_t ip_id_counter = 0x1000;

// ARP Cache
static ArpCacheEntry arp_cache[ARP_CACHE_CAPACITY];

// Active Ping State
static volatile bool ping_reply_received = false;
static volatile uint16_t ping_reply_seq  = 0;
static volatile uint16_t ping_reply_id   = 0;
static volatile uint8_t  ping_reply_ttl  = 0;
static volatile uint64_t ping_reply_time = 0;

// Active TCP Client State (for HTTP / curl)
enum class TcpState {
    CLOSED,
    SYN_SENT,
    ESTABLISHED,
    FIN_WAIT,
    TIME_WAIT
};

struct ActiveTcpConn {
    bool active;
    TcpState state;
    uint8_t server_ip[4];
    uint16_t local_port;
    uint16_t server_port;
    uint32_t client_seq;
    uint32_t server_seq;
    char* recv_buf;
    size_t recv_capacity;
    size_t recv_len;
    bool reset_received;
    bool fin_received;
};

static ActiveTcpConn tcp_conn = {false, TcpState::CLOSED, {0,0,0,0}, 0, 0, 0, 0, nullptr, 0, 0, false, false};

// Active DNS Query State
static volatile bool dns_reply_received = false;
static volatile uint16_t dns_query_id   = 0;
static uint8_t dns_resolved_ip[4]       = {0, 0, 0, 0};

// Forward Declarations of Packet Handlers
static void handle_ethernet(const uint8_t* frame, size_t len);
static void handle_arp(const uint8_t* data, size_t len);
static void handle_ipv4(const uint8_t* data, size_t len);
static void handle_icmp(const uint8_t* src_ip, const uint8_t* data, size_t len);
static void handle_udp(const uint8_t* src_ip, const uint8_t* data, size_t len);
static void handle_tcp(const uint8_t* src_ip, const uint8_t* data, size_t len);

// Output Formatter for Shell / GUI
static void net_output(PrintFn print, uint32_t color, const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);

    // Simple formatting into local buffer
    size_t pos = 0;
    while (*fmt && pos < sizeof(buf) - 1) {
        if (*fmt != '%') {
            buf[pos++] = *fmt++;
            continue;
        }
        fmt++;
        if (*fmt == '\0') break;

        char nbuf[32];
        switch (*fmt) {
            case 's': {
                const char* s = va_arg(args, const char*);
                if (!s) s = "(null)";
                while (*s && pos < sizeof(buf) - 1) buf[pos++] = *s++;
                break;
            }
            case 'u': {
                uint64_t val = va_arg(args, uint64_t);
                kstd::utoa(val, nbuf, 10);
                for (int i = 0; nbuf[i] && pos < sizeof(buf) - 1; ++i) buf[pos++] = nbuf[i];
                break;
            }
            case 'd':
            case 'i': {
                int64_t val = va_arg(args, int64_t);
                kstd::itoa(val, nbuf, 10);
                for (int i = 0; nbuf[i] && pos < sizeof(buf) - 1; ++i) buf[pos++] = nbuf[i];
                break;
            }
            case 'x': {
                uint64_t val = va_arg(args, uint64_t);
                kstd::utoa(val, nbuf, 16);
                for (int i = 0; nbuf[i] && pos < sizeof(buf) - 1; ++i) buf[pos++] = nbuf[i];
                break;
            }
            case 'X': {
                uint64_t val = va_arg(args, uint64_t);
                kstd::utoa(val, nbuf, 16);
                for (int i = 0; nbuf[i] && pos < sizeof(buf) - 1; ++i) buf[pos++] = kstd::toupper(nbuf[i]);
                break;
            }
            case 'c': {
                char c = static_cast<char>(va_arg(args, int));
                buf[pos++] = c;
                break;
            }
            case '%': {
                buf[pos++] = '%';
                break;
            }
            default: {
                buf[pos++] = '%';
                if (pos < sizeof(buf) - 1) buf[pos++] = *fmt;
                break;
            }
        }
        fmt++;
    }
    va_end(args);
    buf[pos] = '\0';

    if (print) {
        // Strip trailing newline if present, GUI terminal adds line by line
        if (pos > 0 && buf[pos - 1] == '\n') {
            buf[pos - 1] = '\0';
        }
        print(buf, color);
    } else {
        // VGA Text mode output
        vga::Color fg = vga::Color::White;
        if (color == COLOR_GREEN)  fg = vga::Color::LightGreen;
        else if (color == COLOR_CYAN)   fg = vga::Color::LightCyan;
        else if (color == COLOR_YELLOW) fg = vga::Color::Yellow;
        else if (color == COLOR_RED)    fg = vga::Color::LightRed;
        else if (color == COLOR_GREY)   fg = vga::Color::LightGrey;

        vga::set_color(fg, vga::Color::Black);
        printk("%s", buf);
        vga::set_color(vga::Color::White, vga::Color::Black);
    }
}

// Checksum Calculation (RFC 1071)
uint16_t calculate_checksum(const void* data, size_t len) {
    const uint16_t* ptr = reinterpret_cast<const uint16_t*>(data);
    uint32_t sum = 0;

    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }

    if (len > 0) {
        sum += *reinterpret_cast<const uint8_t*>(ptr);
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return static_cast<uint16_t>(~sum);
}

// IP & MAC Formatting / Parsing Helpers
bool parse_ip(const char* str, uint8_t out_ip[4]) {
    if (!str || !out_ip) return false;
    uint32_t octets[4] = {0, 0, 0, 0};
    int octet_idx = 0;

    while (*str) {
        if (octet_idx >= 4) return false;
        if (!kstd::isdigit(*str)) return false;

        uint32_t val = 0;
        while (*str && kstd::isdigit(*str)) {
            val = val * 10 + (*str - '0');
            if (val > 255) return false;
            str++;
        }
        octets[octet_idx++] = val;

        if (*str == '.') {
            str++;
            if (!*str) return false;
        } else if (*str == '\0') {
            break;
        } else {
            return false;
        }
    }

    if (octet_idx != 4) return false;

    for (int i = 0; i < 4; ++i) {
        out_ip[i] = static_cast<uint8_t>(octets[i]);
    }
    return true;
}

void format_ip(const uint8_t ip[4], char* out_str) {
    char nbuf[16];
    out_str[0] = '\0';
    for (int i = 0; i < 4; ++i) {
        kstd::utoa(ip[i], nbuf, 10);
        kstd::strcpy(out_str + kstd::strlen(out_str), nbuf);
        if (i < 3) kstd::strcpy(out_str + kstd::strlen(out_str), ".");
    }
}

void format_mac(const uint8_t mac[6], char* out_str) {
    char nbuf[16];
    out_str[0] = '\0';
    for (int i = 0; i < 6; ++i) {
        if (mac[i] < 16) kstd::strcpy(out_str + kstd::strlen(out_str), "0");
        kstd::utoa(mac[i], nbuf, 16);
        kstd::strcpy(out_str + kstd::strlen(out_str), nbuf);
        if (i < 5) kstd::strcpy(out_str + kstd::strlen(out_str), ":");
    }
}

bool ip_equals(const uint8_t a[4], const uint8_t b[4]) {
    return (a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]);
}

bool mac_equals(const uint8_t a[6], const uint8_t b[6]) {
    for (int i = 0; i < 6; ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

// ARP Subsystem
void arp_cache_insert(const uint8_t ip[4], const uint8_t mac[6]) {
    // 1. Check if already exists, update if found
    for (size_t i = 0; i < ARP_CACHE_CAPACITY; ++i) {
        if (arp_cache[i].valid && ip_equals(arp_cache[i].ip, ip)) {
            memcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].timestamp = pit::uptime_ms();
            return;
        }
    }

    // 2. Insert into first free slot
    for (size_t i = 0; i < ARP_CACHE_CAPACITY; ++i) {
        if (!arp_cache[i].valid) {
            memcpy(arp_cache[i].ip, ip, 4);
            memcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].timestamp = pit::uptime_ms();
            arp_cache[i].valid = true;
            return;
        }
    }

    // 3. Cache full: replace oldest entry (slot 0)
    memcpy(arp_cache[0].ip, ip, 4);
    memcpy(arp_cache[0].mac, mac, 6);
    arp_cache[0].timestamp = pit::uptime_ms();
    arp_cache[0].valid = true;
}

static bool arp_cache_lookup(const uint8_t ip[4], uint8_t out_mac[6]) {
    for (size_t i = 0; i < ARP_CACHE_CAPACITY; ++i) {
        if (arp_cache[i].valid && ip_equals(arp_cache[i].ip, ip)) {
            memcpy(out_mac, arp_cache[i].mac, 6);
            return true;
        }
    }
    return false;
}

bool arp_resolve(const uint8_t ip[4], uint8_t out_mac[6], uint32_t timeout_ms) {
    // Check if broadcast
    if ((ip[0] == 255 && ip[1] == 255 && ip[2] == 255 && ip[3] == 255) ||
        (ip[0] == (host_ip[0] | ~netmask[0]) &&
         ip[1] == (host_ip[1] | ~netmask[1]) &&
         ip[2] == (host_ip[2] | ~netmask[2]) &&
         ip[3] == (host_ip[3] | ~netmask[3]))) {
        memset(out_mac, 0xFF, 6);
        return true;
    }

    // Check ARP cache
    if (arp_cache_lookup(ip, out_mac)) {
        return true;
    }

    // Send ARP Request broadcast
    uint8_t packet[sizeof(EthernetHeader) + sizeof(ArpHeader)];
    EthernetHeader* eth = reinterpret_cast<EthernetHeader*>(packet);
    ArpHeader* arp = reinterpret_cast<ArpHeader*>(packet + sizeof(EthernetHeader));

    memset(eth->dest_mac, 0xFF, 6); // Broadcast
    memcpy(eth->src_mac, host_mac, 6);
    eth->ethertype = htons(ETHERTYPE_ARP);

    arp->hw_type = htons(1); // Ethernet
    arp->proto_type = htons(ETHERTYPE_IPV4);
    arp->hw_len = 6;
    arp->proto_len = 4;
    arp->opcode = htons(ARP_OP_REQUEST);
    memcpy(arp->sender_mac, host_mac, 6);
    memcpy(arp->sender_ip, host_ip, 4);
    memset(arp->target_mac, 0x00, 6);
    memcpy(arp->target_ip, ip, 4);

    stats.arp_tx++;
    e1000::send_packet(packet, sizeof(packet));

    // Wait for reply via polling
    uint64_t start = pit::uptime_ms();
    while (pit::uptime_ms() - start < timeout_ms) {
        poll();
        if (arp_cache_lookup(ip, out_mac)) {
            return true;
        }
        arch::io_wait();
    }

    return false;
}

static void handle_arp(const uint8_t* data, size_t len) {
    if (len < sizeof(ArpHeader)) return;
    const ArpHeader* arp = reinterpret_cast<const ArpHeader*>(data);

    stats.arp_rx++;

    if (ntohs(arp->hw_type) != 1 || ntohs(arp->proto_type) != ETHERTYPE_IPV4) {
        return;
    }

    // Always update sender in our ARP cache
    arp_cache_insert(arp->sender_ip, arp->sender_mac);

    uint16_t op = ntohs(arp->opcode);
    if (op == ARP_OP_REQUEST) {
        // If query is for our IP, send reply
        if (ip_equals(arp->target_ip, host_ip)) {
            uint8_t rep_buf[sizeof(EthernetHeader) + sizeof(ArpHeader)];
            EthernetHeader* eth = reinterpret_cast<EthernetHeader*>(rep_buf);
            ArpHeader* rep_arp = reinterpret_cast<ArpHeader*>(rep_buf + sizeof(EthernetHeader));

            memcpy(eth->dest_mac, arp->sender_mac, 6);
            memcpy(eth->src_mac, host_mac, 6);
            eth->ethertype = htons(ETHERTYPE_ARP);

            rep_arp->hw_type = htons(1);
            rep_arp->proto_type = htons(ETHERTYPE_IPV4);
            rep_arp->hw_len = 6;
            rep_arp->proto_len = 4;
            rep_arp->opcode = htons(ARP_OP_REPLY);
            memcpy(rep_arp->sender_mac, host_mac, 6);
            memcpy(rep_arp->sender_ip, host_ip, 4);
            memcpy(rep_arp->target_mac, arp->sender_mac, 6);
            memcpy(rep_arp->target_ip, arp->sender_ip, 4);

            stats.arp_tx++;
            e1000::send_packet(rep_buf, sizeof(rep_buf));
        }
    }
}

// IPv4 Packet Transmission & Routing
bool send_ipv4(const uint8_t dst_ip[4], uint8_t protocol, const void* payload, size_t payload_len) {
    if (payload_len > 1460) return false; // MTU limitation

    // 1. Determine next hop: check if destination is on local subnet
    bool local = true;
    for (int i = 0; i < 4; ++i) {
        if ((dst_ip[i] & netmask[i]) != (host_ip[i] & netmask[i])) {
            local = false;
            break;
        }
    }

    const uint8_t* next_hop = local ? dst_ip : gateway_ip;

    // 2. Resolve Next Hop MAC address via ARP
    uint8_t next_mac[6];
    if (!arp_resolve(next_hop, next_mac, 2000)) {
        stats.ip_dropped++;
        return false;
    }

    // 3. Assemble Frame
    size_t total_frame_len = sizeof(EthernetHeader) + sizeof(IPv4Header) + payload_len;
    uint8_t buffer[1518];

    EthernetHeader* eth = reinterpret_cast<EthernetHeader*>(buffer);
    memcpy(eth->dest_mac, next_mac, 6);
    memcpy(eth->src_mac, host_mac, 6);
    eth->ethertype = htons(ETHERTYPE_IPV4);

    IPv4Header* ip = reinterpret_cast<IPv4Header*>(buffer + sizeof(EthernetHeader));
    ip->ihl_version = 0x45; // IPv4, 20 bytes header
    ip->tos = 0;
    ip->total_len = htons(static_cast<uint16_t>(sizeof(IPv4Header) + payload_len));
    ip->id = htons(++ip_id_counter);
    ip->flags_offset = htons(0x4000); // DF: Don't Fragment
    ip->ttl = 64;
    ip->protocol = protocol;
    ip->checksum = 0;
    memcpy(ip->src_ip, host_ip, 4);
    memcpy(ip->dst_ip, dst_ip, 4);
    ip->checksum = calculate_checksum(ip, sizeof(IPv4Header));

    if (payload && payload_len > 0) {
        memcpy(buffer + sizeof(EthernetHeader) + sizeof(IPv4Header), payload, payload_len);
    }

    stats.ip_tx++;
    return e1000::send_packet(buffer, total_frame_len);
}

// IPv4 Packet Reception
static void handle_ipv4(const uint8_t* data, size_t len) {
    if (len < sizeof(IPv4Header)) {
        stats.ip_dropped++;
        return;
    }

    const IPv4Header* ip = reinterpret_cast<const IPv4Header*>(data);
    stats.ip_rx++;

    // Verify IPv4 Version
    uint8_t version = (ip->ihl_version >> 4) & 0x0F;
    if (version != 4) {
        stats.ip_dropped++;
        return;
    }

    uint8_t ihl_bytes = (ip->ihl_version & 0x0F) * 4;
    if (ihl_bytes < sizeof(IPv4Header) || ihl_bytes > len) {
        stats.ip_dropped++;
        return;
    }

    // Verify Checksum
    if (calculate_checksum(ip, ihl_bytes) != 0) {
        stats.ip_dropped++;
        return;
    }

    // Verify Destination IP: must be our IP, or broadcast
    bool is_our_ip = ip_equals(ip->dst_ip, host_ip);
    bool is_bcast = (ip->dst_ip[0] == 255 && ip->dst_ip[1] == 255 &&
                     ip->dst_ip[2] == 255 && ip->dst_ip[3] == 255);
    if (!is_our_ip && !is_bcast) {
        return; // Packet not destined for us
    }

    uint16_t total_len = ntohs(ip->total_len);
    if (total_len > len) total_len = static_cast<uint16_t>(len);
    if (total_len <= ihl_bytes) return;

    const uint8_t* payload = data + ihl_bytes;
    size_t payload_len = total_len - ihl_bytes;

    // Dispatch by Protocol
    switch (ip->protocol) {
        case IP_PROTO_ICMP:
            handle_icmp(ip->src_ip, payload, payload_len);
            break;
        case IP_PROTO_UDP:
            handle_udp(ip->src_ip, payload, payload_len);
            break;
        case IP_PROTO_TCP:
            handle_tcp(ip->src_ip, payload, payload_len);
            break;
        default:
            break;
    }
}

// ICMP Subsystem
static void handle_icmp(const uint8_t* src_ip, const uint8_t* data, size_t len) {
    if (len < sizeof(ICMPHeader)) return;
    stats.icmp_rx++;

    const ICMPHeader* icmp = reinterpret_cast<const ICMPHeader*>(data);

    // Verify Checksum
    if (calculate_checksum(data, len) != 0) {
        return;
    }

    if (icmp->type == ICMP_TYPE_ECHO_REQUEST && icmp->code == 0) {
        // Construct ICMP Echo Reply
        uint8_t reply_buf[1400];
        if (len > sizeof(reply_buf)) return;

        memcpy(reply_buf, data, len);
        ICMPHeader* rep_hdr = reinterpret_cast<ICMPHeader*>(reply_buf);
        rep_hdr->type = ICMP_TYPE_ECHO_REPLY;
        rep_hdr->code = 0;
        rep_hdr->checksum = 0;
        rep_hdr->checksum = calculate_checksum(reply_buf, len);

        stats.icmp_tx++;
        send_ipv4(src_ip, IP_PROTO_ICMP, reply_buf, len);
    } else if (icmp->type == ICMP_TYPE_ECHO_REPLY && icmp->code == 0) {
        // Signal Ping reply
        ping_reply_id = ntohs(icmp->id);
        ping_reply_seq = ntohs(icmp->sequence);
        ping_reply_time = pit::uptime_ms();
        ping_reply_ttl = 64; // Default or read from IP
        ping_reply_received = true;
    }
}

bool ping_once(const uint8_t target_ip[4], uint16_t seq, uint32_t timeout_ms, uint32_t* out_rtt, uint8_t* out_ttl) {
    ping_reply_received = false;

    // Payload
    char payload[32];
    kstd::strcpy(payload, "AetherOS ICMP Echo Request #");
    char snum[8];
    kstd::utoa(seq, snum, 10);
    kstd::strcpy(payload + kstd::strlen(payload), snum);
    size_t payload_len = kstd::strlen(payload) + 1;

    size_t icmp_len = sizeof(ICMPHeader) + payload_len;
    uint8_t icmp_buf[64];
    ICMPHeader* icmp = reinterpret_cast<ICMPHeader*>(icmp_buf);

    icmp->type = ICMP_TYPE_ECHO_REQUEST;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->id = htons(0x1337);
    icmp->sequence = htons(seq);
    memcpy(icmp_buf + sizeof(ICMPHeader), payload, payload_len);

    icmp->checksum = calculate_checksum(icmp_buf, icmp_len);

    stats.icmp_tx++;
    uint64_t start_time = pit::uptime_ms();

    // If pinging our own IP, respond directly
    if (ip_equals(target_ip, host_ip)) {
        if (out_rtt) *out_rtt = 0;
        if (out_ttl) *out_ttl = 64;
        return true;
    }

    if (!send_ipv4(target_ip, IP_PROTO_ICMP, icmp_buf, icmp_len)) {
        return false;
    }

    // Wait for matching reply
    while (pit::uptime_ms() - start_time < timeout_ms) {
        poll();
        if (ping_reply_received && ping_reply_id == 0x1337 && ping_reply_seq == seq) {
            uint64_t elapsed = pit::uptime_ms() - start_time;
            if (out_rtt) *out_rtt = static_cast<uint32_t>(elapsed);
            if (out_ttl) *out_ttl = ping_reply_ttl;
            return true;
        }
        arch::io_wait();
    }

    return false;
}

// UDP Subsystem
bool send_udp(const uint8_t dst_ip[4], uint16_t src_port, uint16_t dst_port, const void* data, size_t len) {
    if (len > 1400) return false;

    size_t udp_len = sizeof(UDPHeader) + len;
    uint8_t buffer[1500];

    UDPHeader* udp = reinterpret_cast<UDPHeader*>(buffer);
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length = htons(static_cast<uint16_t>(udp_len));
    udp->checksum = 0; // Checksum optional in IPv4

    if (data && len > 0) {
        memcpy(buffer + sizeof(UDPHeader), data, len);
    }

    stats.udp_tx++;
    return send_ipv4(dst_ip, IP_PROTO_UDP, buffer, udp_len);
}

static void handle_udp(const uint8_t* /*src_ip*/, const uint8_t* data, size_t len) {
    if (len < sizeof(UDPHeader)) return;
    stats.udp_rx++;

    const UDPHeader* udp = reinterpret_cast<const UDPHeader*>(data);
    uint16_t dst_port = ntohs(udp->dst_port);
    const uint8_t* payload = data + sizeof(UDPHeader);
    size_t payload_len = len - sizeof(UDPHeader);

    // Check for DNS response (src port 53 or dst port 53530)
    if (dst_port == 53530 && payload_len >= 12) {
        uint16_t id = (static_cast<uint16_t>(payload[0]) << 8) | payload[1];
        if (id == dns_query_id) {
            uint16_t ancount = (static_cast<uint16_t>(payload[6]) << 8) | payload[7];
            if (ancount > 0) {
                // Skip Header (12 bytes) and Query section
                size_t p = 12;
                while (p < payload_len && payload[p] != 0) {
                    p += payload[p] + 1;
                }
                p += 5; // skip 0 terminator, type (2), class (2)

                // Parse Answer
                if (p + 12 <= payload_len) {
                    if ((payload[p] & 0xC0) == 0xC0) p += 2; // Pointer
                    uint16_t type = (static_cast<uint16_t>(payload[p]) << 8) | payload[p + 1];
                    p += 2; // type
                    p += 2; // class
                    p += 4; // ttl
                    uint16_t rdlen = (static_cast<uint16_t>(payload[p]) << 8) | payload[p + 1];
                    p += 2; // rdlength
                    if (type == 1 && rdlen == 4 && p + 4 <= payload_len) { // Type A
                        dns_resolved_ip[0] = payload[p + 0];
                        dns_resolved_ip[1] = payload[p + 1];
                        dns_resolved_ip[2] = payload[p + 2];
                        dns_resolved_ip[3] = payload[p + 3];
                        dns_reply_received = true;
                    }
                }
            }
        }
    }
}

// DNS Query Resolver
bool dns_resolve(const char* hostname, uint8_t out_ip[4], uint32_t timeout_ms) {
    // If hostname is already quad-dotted IP, parse directly
    if (parse_ip(hostname, out_ip)) {
        return true;
    }

    dns_reply_received = false;
    dns_query_id = static_cast<uint16_t>(pit::uptime_ms() & 0xFFFF);

    uint8_t query_buf[512];
    memset(query_buf, 0, sizeof(query_buf));

    // DNS Header
    query_buf[0] = static_cast<uint8_t>(dns_query_id >> 8);
    query_buf[1] = static_cast<uint8_t>(dns_query_id & 0xFF);
    query_buf[2] = 0x01; // Standard query, recursion desired
    query_buf[3] = 0x00;
    query_buf[4] = 0x00; query_buf[5] = 0x01; // 1 Question

    // Format QNAME (e.g., "google.com" -> "\x06google\x03com\x00")
    size_t pos = 12;
    const char* p = hostname;
    while (*p) {
        const char* next_dot = kstd::strchr(p, '.');
        size_t label_len = next_dot ? static_cast<size_t>(next_dot - p) : kstd::strlen(p);
        if (label_len > 63 || pos + label_len + 1 >= sizeof(query_buf) - 5) return false;

        query_buf[pos++] = static_cast<uint8_t>(label_len);
        for (size_t i = 0; i < label_len; ++i) {
            query_buf[pos++] = p[i];
        }

        if (next_dot) p = next_dot + 1;
        else break;
    }
    query_buf[pos++] = 0; // Terminating zero

    // QTYPE = 1 (A record)
    query_buf[pos++] = 0x00; query_buf[pos++] = 0x01;
    // QCLASS = 1 (IN)
    query_buf[pos++] = 0x00; query_buf[pos++] = 0x01;

    // Send UDP packet to DNS server on port 53
    send_udp(dns_ip, 53530, 53, query_buf, pos);

    // Wait for response
    uint64_t start = pit::uptime_ms();
    while (pit::uptime_ms() - start < timeout_ms) {
        poll();
        if (dns_reply_received) {
            memcpy(out_ip, dns_resolved_ip, 4);
            return true;
        }
        arch::io_wait();
    }

    return false;
}

// TCP Checksum Helper
static uint16_t calculate_tcp_checksum(const uint8_t src[4], const uint8_t dst[4], const TCPHeader* th, const void* payload, size_t payload_len) {
    uint32_t sum = 0;

    // Pseudo-header: Src IP (4), Dst IP (4), Zero (1), Proto 6 (1), TCP Length (2)
    const uint16_t* s = reinterpret_cast<const uint16_t*>(src);
    sum += s[0]; sum += s[1];
    const uint16_t* d = reinterpret_cast<const uint16_t*>(dst);
    sum += d[0]; sum += d[1];
    sum += htons(6); // TCP Protocol
    sum += htons(static_cast<uint16_t>(sizeof(TCPHeader) + payload_len));

    // TCP Header
    const uint16_t* h = reinterpret_cast<const uint16_t*>(th);
    for (size_t i = 0; i < sizeof(TCPHeader) / 2; ++i) {
        sum += h[i];
    }

    // Payload
    const uint16_t* p = reinterpret_cast<const uint16_t*>(payload);
    size_t len = payload_len;
    while (len > 1) {
        sum += *p++;
        len -= 2;
    }
    if (len > 0) {
        sum += *reinterpret_cast<const uint8_t*>(p);
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return static_cast<uint16_t>(~sum);
}

// TCP Packet Sender
static bool send_tcp_packet(const uint8_t dst_ip[4], uint16_t src_port, uint16_t dst_port,
                            uint32_t seq, uint32_t ack, uint16_t flags, uint16_t window,
                            const void* data, size_t data_len) {
    uint8_t buffer[1500];
    TCPHeader* tcp = reinterpret_cast<TCPHeader*>(buffer);

    tcp->src_port = htons(src_port);
    tcp->dst_port = htons(dst_port);
    tcp->seq_num = htonl(seq);
    tcp->ack_num = htonl(ack);
    tcp->flags = htons((5 << 12) | (flags & 0x01FF)); // 20 byte header
    tcp->window_size = htons(window);
    tcp->checksum = 0;
    tcp->urgent_ptr = 0;

    if (data && data_len > 0) {
        memcpy(buffer + sizeof(TCPHeader), data, data_len);
    }

    tcp->checksum = calculate_tcp_checksum(host_ip, dst_ip, tcp, data, data_len);

    stats.tcp_tx++;
    return send_ipv4(dst_ip, IP_PROTO_TCP, buffer, sizeof(TCPHeader) + data_len);
}

// TCP Inbound Handler
static void handle_tcp(const uint8_t* src_ip, const uint8_t* data, size_t len) {
    if (len < sizeof(TCPHeader)) return;
    stats.tcp_rx++;

    const TCPHeader* tcp = reinterpret_cast<const TCPHeader*>(data);
    uint16_t src_port = ntohs(tcp->src_port);
    uint16_t dst_port = ntohs(tcp->dst_port);
    uint16_t dof = ntohs(tcp->flags);
    uint8_t data_offset = ((dof >> 12) & 0x0F) * 4;
    uint16_t flags = dof & 0x01FF;

    if (data_offset < sizeof(TCPHeader) || data_offset > len) return;

    // Check if packet matches active TCP client connection
    if (tcp_conn.active &&
        ip_equals(src_ip, tcp_conn.server_ip) &&
        src_port == tcp_conn.server_port &&
        dst_port == tcp_conn.local_port) {

        if (flags & TCP_FLAG_RST) {
            stats.tcp_resets++;
            tcp_conn.reset_received = true;
            tcp_conn.state = TcpState::CLOSED;
            return;
        }

        uint32_t seq = ntohl(tcp->seq_num);
        uint32_t ack = ntohl(tcp->ack_num);

        if (tcp_conn.state == TcpState::SYN_SENT) {
            if ((flags & TCP_FLAG_SYN) && (flags & TCP_FLAG_ACK) && ack == tcp_conn.client_seq + 1) {
                tcp_conn.server_seq = seq + 1;
                tcp_conn.client_seq++;
                tcp_conn.state = TcpState::ESTABLISHED;

                // Send ACK to complete 3-way handshake
                send_tcp_packet(tcp_conn.server_ip, tcp_conn.local_port, tcp_conn.server_port,
                                tcp_conn.client_seq, tcp_conn.server_seq,
                                TCP_FLAG_ACK, 8192, nullptr, 0);
            }
        } else if (tcp_conn.state == TcpState::ESTABLISHED) {
            const uint8_t* payload = data + data_offset;
            size_t payload_len = len - data_offset;

            if (payload_len > 0) {
                // Append payload to receive buffer
                if (tcp_conn.recv_buf && tcp_conn.recv_len < tcp_conn.recv_capacity - 1) {
                    size_t avail = tcp_conn.recv_capacity - 1 - tcp_conn.recv_len;
                    size_t copy_bytes = (payload_len < avail) ? payload_len : avail;
                    memcpy(tcp_conn.recv_buf + tcp_conn.recv_len, payload, copy_bytes);
                    tcp_conn.recv_len += copy_bytes;
                }
                tcp_conn.server_seq += static_cast<uint32_t>(payload_len);

                // Send ACK for data
                send_tcp_packet(tcp_conn.server_ip, tcp_conn.local_port, tcp_conn.server_port,
                                tcp_conn.client_seq, tcp_conn.server_seq,
                                TCP_FLAG_ACK, 8192, nullptr, 0);
            }

            if (flags & TCP_FLAG_FIN) {
                tcp_conn.server_seq++;
                tcp_conn.fin_received = true;
                tcp_conn.state = TcpState::CLOSED;

                // Send FIN-ACK
                send_tcp_packet(tcp_conn.server_ip, tcp_conn.local_port, tcp_conn.server_port,
                                tcp_conn.client_seq, tcp_conn.server_seq,
                                TCP_FLAG_FIN | TCP_FLAG_ACK, 8192, nullptr, 0);
            }
        }
    }
}

// HTTP GET Request Client
int http_get(const uint8_t server_ip[4], uint16_t port, const char* path, char* response_buf, size_t max_len, uint32_t timeout_ms, const char* host_name, const char* accept_encoding) {
    if (!response_buf || max_len < 32) return -1;
    response_buf[0] = '\0';

    // 1. Setup connection parameters
    tcp_conn.active = true;
    tcp_conn.state = TcpState::SYN_SENT;
    memcpy(tcp_conn.server_ip, server_ip, 4);
    tcp_conn.server_port = port;
    tcp_conn.local_port = static_cast<uint16_t>(49152 + (pit::uptime_ms() % 16384));
    tcp_conn.client_seq = static_cast<uint32_t>(pit::uptime_ms() * 1000 + 1337);
    tcp_conn.server_seq = 0;
    tcp_conn.recv_buf = response_buf;
    tcp_conn.recv_capacity = max_len;
    tcp_conn.recv_len = 0;
    tcp_conn.reset_received = false;
    tcp_conn.fin_received = false;

    stats.tcp_connections++;

    // 2. Send SYN
    send_tcp_packet(server_ip, tcp_conn.local_port, port, tcp_conn.client_seq, 0, TCP_FLAG_SYN, 8192, nullptr, 0);

    // 3. Wait for SYN-ACK
    uint64_t start_time = pit::uptime_ms();
    uint64_t last_syn = start_time;

    while (tcp_conn.state == TcpState::SYN_SENT) {
        if (pit::uptime_ms() - start_time > timeout_ms) {
            tcp_conn.active = false;
            return -1; // Timeout
        }
        if (tcp_conn.reset_received) {
            tcp_conn.active = false;
            return -2; // Connection refused
        }
        // Retransmit SYN if needed
        if (pit::uptime_ms() - last_syn > 1000) {
            send_tcp_packet(server_ip, tcp_conn.local_port, port, tcp_conn.client_seq, 0, TCP_FLAG_SYN, 8192, nullptr, 0);
            last_syn = pit::uptime_ms();
        }
        poll();
        arch::io_wait();
    }

    // 4. Connection ESTABLISHED! Send HTTP GET Request
    char req[512];
    char ip_str[32];
    format_ip(server_ip, ip_str);

    kstd::strcpy(req, "GET ");
    kstd::strcpy(req + kstd::strlen(req), (path && *path) ? path : "/");
    kstd::strcpy(req + kstd::strlen(req), " HTTP/1.0\r\nHost: ");
    kstd::strcpy(req + kstd::strlen(req), (host_name && *host_name) ? host_name : ip_str);
    kstd::strcpy(req + kstd::strlen(req), "\r\nUser-Agent: AetherOS/1.0 (x86_64)\r\nAccept: application/octet-stream\r\nAccept-Encoding: ");
    kstd::strcpy(req + kstd::strlen(req), (accept_encoding && *accept_encoding) ? accept_encoding : "gzip, deflate");
    kstd::strcpy(req + kstd::strlen(req), "\r\nConnection: close\r\n\r\n");

    size_t req_len = kstd::strlen(req);
    send_tcp_packet(server_ip, tcp_conn.local_port, port, tcp_conn.client_seq, tcp_conn.server_seq,
                    TCP_FLAG_PSH | TCP_FLAG_ACK, 8192, req, req_len);
    tcp_conn.client_seq += static_cast<uint32_t>(req_len);

    // 5. Receive HTTP Response
    start_time = pit::uptime_ms();
    size_t prev_len = 0;

    while (tcp_conn.state == TcpState::ESTABLISHED && !tcp_conn.fin_received) {
        if (pit::uptime_ms() - start_time > timeout_ms) {
            break; // Finished receiving or timeout
        }
        poll();

        if (tcp_conn.recv_len != prev_len) {
            start_time = pit::uptime_ms(); // Reset timeout on incoming data
            prev_len = tcp_conn.recv_len;
        }
        arch::io_wait();
    }

    response_buf[tcp_conn.recv_len] = '\0';
    int total = static_cast<int>(tcp_conn.recv_len);
    tcp_conn.active = false;
    return total;
}

bool tcp_connect(const uint8_t server_ip[4], uint16_t port, char* recv_buf, size_t recv_capacity, uint32_t timeout_ms) {
    if (!recv_buf || recv_capacity < 32) return false;
    tcp_conn.active = true;
    tcp_conn.state = TcpState::SYN_SENT;
    memcpy(tcp_conn.server_ip, server_ip, 4);
    tcp_conn.server_port = port;
    tcp_conn.local_port = static_cast<uint16_t>(49152 + (pit::uptime_ms() % 16384));
    tcp_conn.client_seq = static_cast<uint32_t>(pit::uptime_ms() * 1000 + 1337);
    tcp_conn.server_seq = 0;
    tcp_conn.recv_buf = recv_buf;
    tcp_conn.recv_capacity = recv_capacity;
    tcp_conn.recv_len = 0;
    tcp_conn.reset_received = false;
    tcp_conn.fin_received = false;
    stats.tcp_connections++;

    send_tcp_packet(server_ip, tcp_conn.local_port, port, tcp_conn.client_seq, 0, TCP_FLAG_SYN, 8192, nullptr, 0);

    uint64_t start_time = pit::uptime_ms();
    uint64_t last_syn = start_time;
    while (tcp_conn.state == TcpState::SYN_SENT) {
        if (pit::uptime_ms() - start_time > timeout_ms) { tcp_conn.active = false; return false; }
        if (tcp_conn.reset_received) { tcp_conn.active = false; return false; }
        if (pit::uptime_ms() - last_syn > 1000) {
            send_tcp_packet(server_ip, tcp_conn.local_port, port, tcp_conn.client_seq, 0, TCP_FLAG_SYN, 8192, nullptr, 0);
            last_syn = pit::uptime_ms();
        }
        poll();
        arch::io_wait();
    }
    return true;
}

int tcp_send(const void* data, size_t len) {
    if (tcp_conn.state != TcpState::ESTABLISHED || !data) return -1;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    while (sent < len) {
        size_t chunk = len - sent;
        if (chunk > 1400) chunk = 1400;
        bool last = sent + chunk == len;
        uint16_t flags = TCP_FLAG_ACK | (last ? TCP_FLAG_PSH : 0);
        if (!send_tcp_packet(tcp_conn.server_ip, tcp_conn.local_port, tcp_conn.server_port,
                             tcp_conn.client_seq, tcp_conn.server_seq, flags, 8192, p + sent, chunk)) {
            return sent > 0 ? static_cast<int>(sent) : -1;
        }
        tcp_conn.client_seq += static_cast<uint32_t>(chunk);
        sent += chunk;
    }
    return static_cast<int>(sent);
}

int tcp_recv(void* out, size_t max_len, uint32_t timeout_ms) {
    if (!out || max_len == 0) return 0;
    uint64_t start = pit::uptime_ms();
    while (tcp_conn.recv_len == 0) {
        if (tcp_conn.reset_received) return -2;
        if (tcp_conn.fin_received || tcp_conn.state == TcpState::CLOSED || !tcp_conn.active) return -1;
        if (pit::uptime_ms() - start > timeout_ms) return 0;
        poll();
        arch::io_wait();
    }
    size_t n = tcp_conn.recv_len < max_len ? tcp_conn.recv_len : max_len;
    memcpy(out, tcp_conn.recv_buf, n);
    if (n < tcp_conn.recv_len) {
        memmove(tcp_conn.recv_buf, tcp_conn.recv_buf + n, tcp_conn.recv_len - n);
    }
    tcp_conn.recv_len -= n;
    return static_cast<int>(n);
}

void tcp_close() {
    if (tcp_conn.state == TcpState::ESTABLISHED) {
        send_tcp_packet(tcp_conn.server_ip, tcp_conn.local_port, tcp_conn.server_port,
                        tcp_conn.client_seq, tcp_conn.server_seq,
                        TCP_FLAG_FIN | TCP_FLAG_ACK, 8192, nullptr, 0);
        tcp_conn.client_seq++;
    }
    tcp_conn.active = false;
    tcp_conn.state = TcpState::CLOSED;
    tcp_conn.recv_buf = nullptr;
    tcp_conn.recv_capacity = 0;
    tcp_conn.recv_len = 0;
}

// Ethernet Packet Dispatcher
static void handle_ethernet(const uint8_t* frame, size_t len) {
    if (len < sizeof(EthernetHeader)) return;

    const EthernetHeader* eth = reinterpret_cast<const EthernetHeader*>(frame);
    uint16_t ethertype = ntohs(eth->ethertype);

    const uint8_t* payload = frame + sizeof(EthernetHeader);
    size_t payload_len = len - sizeof(EthernetHeader);

    switch (ethertype) {
        case ETHERTYPE_ARP:
            handle_arp(payload, payload_len);
            break;
        case ETHERTYPE_IPV4:
            handle_ipv4(payload, payload_len);
            break;
        default:
            break;
    }
}

// Network Polling
void poll() {
    if (!e1000::is_available()) return;

    uint8_t rx_buffer[2048];
    while (true) {
        size_t len = e1000::receive_packet(rx_buffer, sizeof(rx_buffer));
        if (len == 0) break;
        handle_ethernet(rx_buffer, len);
    }
}

void init() {
    printk("[net] Initializing AetherOS Network Protocol Stack...\n");

    // Copy MAC from e1000 driver
    e1000::get_mac(host_mac);

    // Clear ARP Cache
    memset(arp_cache, 0, sizeof(arp_cache));

    // Pre-populate gateway in ARP cache if QEMU default
    uint8_t qemu_gw_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    arp_cache_insert(gateway_ip, qemu_gw_mac);

    char ip_str[32], gw_str[32], mac_str[32];
    format_ip(host_ip, ip_str);
    format_ip(gateway_ip, gw_str);
    format_mac(host_mac, mac_str);

    printk("[net] eth0: IP=%s Netmask=255.255.255.0 Gateway=%s MAC=%s\n",
           ip_str, gw_str, mac_str);
    printk("[net] Network Stack active. Protocols: Ethernet II, ARP, IPv4, ICMP, UDP, TCP, DNS.\n");
}

bool is_available() {
    return e1000::is_available();
}

const uint8_t* get_mac()     { return host_mac; }
const uint8_t* get_ip()      { return host_ip; }
const uint8_t* get_netmask() { return netmask; }
const uint8_t* get_gateway() { return gateway_ip; }
const uint8_t* get_dns()     { return dns_ip; }

void set_ip(const uint8_t ip[4])          { memcpy(host_ip, ip, 4); }
void set_netmask(const uint8_t mask[4])   { memcpy(netmask, mask, 4); }
void set_gateway(const uint8_t gw[4])     { memcpy(gateway_ip, gw, 4); }
void set_dns(const uint8_t dns[4])         { memcpy(dns_ip, dns, 4); }

const ArpCacheEntry* get_arp_cache() { return arp_cache; }
const NetStats& get_stats()          { return stats; }

// ============================================================================
// Shell & GUI Command Implementations
// ============================================================================

void cmd_ifconfig(int argc, char** argv, PrintFn print) {
    // Handle parameter: ifconfig eth0 <ip>
    if (argc >= 3 && !kstd::strcmp(argv[1], "eth0")) {
        uint8_t new_ip[4];
        if (parse_ip(argv[2], new_ip)) {
            set_ip(new_ip);
            net_output(print, COLOR_GREEN, "eth0: IP address set to %s\n", argv[2]);
            return;
        } else {
            net_output(print, COLOR_RED, "ifconfig: Invalid IPv4 address '%s'\n", argv[2]);
            return;
        }
    }

    // Handle parameter: ifconfig wlan0 <ip>
    if (argc >= 3 && !kstd::strcmp(argv[1], "wlan0")) {
        uint8_t new_ip[4];
        if (parse_ip(argv[2], new_ip)) {
            wifi::set_ip(new_ip);
            net_output(print, COLOR_GREEN, "wlan0: IP address set to %s\n", argv[2]);
            return;
        } else {
            net_output(print, COLOR_RED, "ifconfig: Invalid IPv4 address '%s'\n", argv[2]);
            return;
        }
    }

    bool eth_avail = e1000::is_available();
    bool eth_link = eth_avail && e1000::has_link();
    bool eth_is_primary = wifi::is_ethernet_preferred();
    bool wifi_connected = wifi::is_connected();

    // 1. eth0 (Ethernet Interface)
    if (eth_avail) {
        char ip_s[32], nm_s[32], gw_s[32], dns_s[32], mac_s[32];
        format_ip(host_ip, ip_s);
        format_ip(netmask, nm_s);
        format_ip(gateway_ip, gw_s);
        format_ip(dns_ip, dns_s);
        format_mac(host_mac, mac_s);

        net_output(print, eth_is_primary ? COLOR_GREEN : COLOR_CYAN,
                   "eth0: flags=4163<UP,BROADCAST,%s,MULTICAST> mtu 1500  %s\n",
                   eth_link ? "RUNNING" : "NO-CARRIER",
                   eth_is_primary ? "[PRIMARY DEFAULT ROUTE]" : (eth_link ? "[STANDBY]" : "[NO-CARRIER]"));
        net_output(print, COLOR_WHITE, "        inet %s  netmask %s  broadcast 10.0.2.255\n", ip_s, nm_s);
        net_output(print, COLOR_WHITE, "        gateway %s  dns %s  metric 100\n", gw_s, dns_s);
        net_output(print, COLOR_WHITE, "        ether %s  txqueuelen 1000  (%s)\n",
                   mac_s, e1000::get_device_name());
        net_output(print, COLOR_WHITE, "        RX packets %u  bytes %u  errors %u  dropped %u\n",
                   e1000::get_rx_packets(), e1000::get_rx_bytes(), e1000::get_rx_errors(), e1000::get_rx_dropped());
        net_output(print, COLOR_WHITE, "        TX packets %u  bytes %u  errors %u\n",
                   e1000::get_tx_packets(), e1000::get_tx_bytes(), e1000::get_tx_errors());
        net_output(print, COLOR_GREY,  "        interrupt %u  memory 0x%x\n",
                   e1000::get_irq(), static_cast<uint32_t>(e1000::get_mmio_base()));
    } else {
        net_output(print, COLOR_GREY, "eth0: No active Ethernet NIC detected.\n");
    }

    net_output(print, COLOR_GREY, "\n");

    // 2. wlan0 (Wi-Fi 802.11ax Interface)
    char wmac_s[32], wip_s[32], wnm_s[32], wgw_s[32], wdns_s[32];
    format_mac(wifi::get_mac(), wmac_s);
    format_ip(wifi::get_ip(), wip_s);
    format_ip(wifi::get_netmask(), wnm_s);
    format_ip(wifi::get_gateway(), wgw_s);
    format_ip(wifi::get_dns(), wdns_s);

    bool wifi_is_primary = !eth_is_primary && wifi_connected;

    net_output(print, wifi_is_primary ? COLOR_GREEN : COLOR_CYAN,
               "wlan0: flags=4099<UP,BROADCAST,MULTICAST> mtu 1500  %s\n",
               wifi_is_primary ? "[PRIMARY DEFAULT ROUTE]" : (wifi_connected ? "[WARM STANDBY]" : "[DISCONNECTED]"));
    if (wifi_connected) {
        net_output(print, COLOR_GREEN, "        status: CONNECTED (SSID: %s, Ch %u, %u MHz, Signal: %u%%)\n",
                   wifi::get_connected_ssid(), wifi::get_connected_channel(),
                   wifi::get_connected_freq(), wifi::get_connected_signal());
        net_output(print, COLOR_WHITE, "        inet %s  netmask %s  metric %u\n", wip_s, wnm_s, wifi_is_primary ? 100 : 600);
        net_output(print, COLOR_WHITE, "        gateway %s  dns %s\n", wgw_s, wdns_s);
    } else {
        net_output(print, COLOR_GREY, "        status: DISCONNECTED (Use 'wifi scan' & 'wifi connect' to associate)\n");
    }
    net_output(print, COLOR_WHITE, "        ether %s  (802.11ax Wi-Fi 6 Wireless Controller)\n", wmac_s);

    // 3. Routing & Auto-Ethernet summary
    net_output(print, COLOR_GREY, "-------------------------------------------------------------------------------\n");
    net_output(print, COLOR_CYAN, "Active Default Route: %s | Auto-Ethernet Priority: %s\n",
               wifi::get_active_interface_name(),
               wifi::is_auto_ethernet_enabled() ? "ENABLED (Ethernet Link Takes Priority)" : "DISABLED");
}

void cmd_ping(int argc, char** argv, PrintFn print) {
    if (argc < 2) {
        net_output(print, COLOR_YELLOW, "Usage: ping <ip_address|hostname> [count]\n");
        return;
    }

    if (!e1000::is_available()) {
        net_output(print, COLOR_RED, "ping: Network interface unavailable.\n");
        return;
    }

    uint8_t target_ip[4];
    if (!parse_ip(argv[1], target_ip)) {
        net_output(print, COLOR_CYAN, "Resolving host '%s' via DNS (%u.%u.%u.%u)...\n",
                   argv[1], dns_ip[0], dns_ip[1], dns_ip[2], dns_ip[3]);
        if (!dns_resolve(argv[1], target_ip, 2000)) {
            net_output(print, COLOR_RED, "ping: Cannot resolve '%s': Unknown host\n", argv[1]);
            return;
        }
    }

    int count = 4;
    if (argc >= 3) {
        count = static_cast<int>(kstd::parse_int(argv[2]));
        if (count < 1) count = 1;
        if (count > 20) count = 20;
    }

    char target_str[32];
    format_ip(target_ip, target_str);
    net_output(print, COLOR_CYAN, "PING %s (%s): 56 data bytes\n", argv[1], target_str);

    uint32_t transmitted = 0;
    uint32_t received = 0;
    uint32_t min_rtt = 999999;
    uint32_t max_rtt = 0;
    uint32_t total_rtt = 0;

    for (int seq = 1; seq <= count; ++seq) {
        transmitted++;
        uint32_t rtt = 0;
        uint8_t ttl = 64;

        bool ok = ping_once(target_ip, static_cast<uint16_t>(seq), 1000, &rtt, &ttl);
        if (ok) {
            received++;
            if (rtt < min_rtt) min_rtt = rtt;
            if (rtt > max_rtt) max_rtt = rtt;
            total_rtt += rtt;
            net_output(print, COLOR_GREEN, "64 bytes from %s: icmp_seq=%d ttl=%u time=%u ms\n",
                       target_str, seq, ttl, rtt);
        } else {
            net_output(print, COLOR_RED, "Request timeout for icmp_seq %d\n", seq);
        }

        if (seq < count) {
            pit::sleep(300);
        }
    }

    net_output(print, COLOR_CYAN, "\n--- %s ping statistics ---\n", target_str);
    uint32_t loss = transmitted ? ((transmitted - received) * 100 / transmitted) : 0;
    net_output(print, COLOR_WHITE, "%u packets transmitted, %u received, %u%% packet loss\n",
               transmitted, received, loss);

    if (received > 0) {
        uint32_t avg_rtt = total_rtt / received;
        net_output(print, COLOR_WHITE, "round-trip min/avg/max = %u/%u/%u ms\n",
                   min_rtt, avg_rtt, max_rtt);
    }
}

static void pad_string(char* dest, const char* src, size_t target_len) {
    size_t len = kstd::strlen(src);
    kstd::strcpy(dest, src);
    for (size_t i = len; i < target_len; ++i) {
        dest[i] = ' ';
    }
    dest[target_len] = '\0';
}

void cmd_arp(int /*argc*/, char** /*argv*/, PrintFn print) {
    net_output(print, COLOR_CYAN, "Address          HWtype   HWaddress          Flags   Iface\n");

    int count = 0;
    char ip_s[32], mac_s[32];
    char p_ip[20], p_hw[12], p_mac[22];

    for (size_t i = 0; i < ARP_CACHE_CAPACITY; ++i) {
        if (arp_cache[i].valid) {
            format_ip(arp_cache[i].ip, ip_s);
            format_mac(arp_cache[i].mac, mac_s);
            pad_string(p_ip, ip_s, 16);
            pad_string(p_hw, "ether", 8);
            pad_string(p_mac, mac_s, 18);
            net_output(print, COLOR_WHITE, "%s %s %s %s    %s\n",
                       p_ip, p_hw, p_mac, "C", "eth0");
            count++;
        }
    }

    if (count == 0) {
        net_output(print, COLOR_GREY, "(No entries found in ARP cache)\n");
    } else {
        net_output(print, COLOR_GREY, "Total ARP entries: %d\n", count);
    }
}

void cmd_netstat(int /*argc*/, char** /*argv*/, PrintFn print) {
    net_output(print, COLOR_CYAN, "=== Network Interface Statistics ===\n");
    net_output(print, COLOR_WHITE, "Iface  MTU   RX-OK    RX-ERR   TX-OK    TX-ERR\n");
    net_output(print, COLOR_WHITE, "eth0   1500  %u        %u        %u        %u\n\n",
               e1000::get_rx_packets(), e1000::get_rx_errors(),
               e1000::get_tx_packets(), e1000::get_tx_errors());

    net_output(print, COLOR_CYAN, "=== Network Protocol Statistics ===\n");
    net_output(print, COLOR_WHITE, "Ip:\n");
    net_output(print, COLOR_WHITE, "    %u total packets received\n", stats.ip_rx);
    net_output(print, COLOR_WHITE, "    %u packets sent out\n", stats.ip_tx);
    net_output(print, COLOR_WHITE, "    %u incoming packets dropped\n", stats.ip_dropped);

    net_output(print, COLOR_WHITE, "Icmp:\n");
    net_output(print, COLOR_WHITE, "    %u ICMP messages received\n", stats.icmp_rx);
    net_output(print, COLOR_WHITE, "    %u ICMP messages sent\n", stats.icmp_tx);

    net_output(print, COLOR_WHITE, "Arp:\n");
    net_output(print, COLOR_WHITE, "    %u ARP requests/replies received\n", stats.arp_rx);
    net_output(print, COLOR_WHITE, "    %u ARP requests/replies sent\n", stats.arp_tx);

    net_output(print, COLOR_WHITE, "Tcp:\n");
    net_output(print, COLOR_WHITE, "    %u active connection openings\n", stats.tcp_connections);
    net_output(print, COLOR_WHITE, "    %u segments received\n", stats.tcp_rx);
    net_output(print, COLOR_WHITE, "    %u segments sent out\n", stats.tcp_tx);
    net_output(print, COLOR_WHITE, "    %u connection resets received\n", stats.tcp_resets);

    net_output(print, COLOR_WHITE, "Udp:\n");
    net_output(print, COLOR_WHITE, "    %u UDP packets received\n", stats.udp_rx);
    net_output(print, COLOR_WHITE, "    %u UDP packets sent out\n", stats.udp_tx);
}

void cmd_curl(int argc, char** argv, PrintFn print) {
    if (argc < 2) {
        net_output(print, COLOR_YELLOW, "Usage: curl <ip_or_host> [port] [path]\n");
        net_output(print, COLOR_GREY,   "Example: curl 10.0.2.2 80 /\n");
        return;
    }

    if (!e1000::is_available()) {
        net_output(print, COLOR_RED, "curl: Network interface unavailable.\n");
        return;
    }

    uint8_t server_ip[4];
    if (!parse_ip(argv[1], server_ip)) {
        net_output(print, COLOR_CYAN, "Resolving host '%s' via DNS...\n", argv[1]);
        if (!dns_resolve(argv[1], server_ip, 2500)) {
            net_output(print, COLOR_RED, "curl: Could not resolve host '%s'\n", argv[1]);
            return;
        }
    }

    uint16_t port = 80;
    if (argc >= 3) {
        port = static_cast<uint16_t>(kstd::parse_int(argv[2]));
        if (port == 0) port = 80;
    }

    const char* path = "/";
    if (argc >= 4) {
        path = argv[3];
    }

    char ip_s[32];
    format_ip(server_ip, ip_s);
    net_output(print, COLOR_CYAN, "* Connecting to %s:%u (path: %s)...\n", ip_s, port, path);

    // Allocate 32KB buffer for HTTP response
    constexpr size_t BUF_SIZE = 32768;
    char* buf = reinterpret_cast<char*>(kmalloc(BUF_SIZE));
    if (!buf) {
        net_output(print, COLOR_RED, "curl: Out of memory for response buffer!\n");
        return;
    }

    int bytes = http_get(server_ip, port, path, buf, BUF_SIZE, 5000);

    if (bytes < 0) {
        if (bytes == -2) {
            net_output(print, COLOR_RED, "curl: (7) Failed to connect to %s port %u: Connection refused\n", ip_s, port);
        } else {
            net_output(print, COLOR_RED, "curl: (28) Connection to %s port %u timed out\n", ip_s, port);
        }
    } else {
        net_output(print, COLOR_GREEN, "* Connected successfully! Received %d bytes:\n\n", bytes);
        // Print received response (line by line for print callback or raw for printk)
        if (print) {
            char line_buf[256];
            size_t lpos = 0;
            for (int i = 0; i < bytes; ++i) {
                if (buf[i] == '\r') continue;
                if (buf[i] == '\n') {
                    line_buf[lpos] = '\0';
                    print(line_buf, COLOR_WHITE);
                    lpos = 0;
                } else if (lpos < sizeof(line_buf) - 1) {
                    line_buf[lpos++] = buf[i];
                }
            }
            if (lpos > 0) {
                line_buf[lpos] = '\0';
                print(line_buf, COLOR_WHITE);
            }
        } else {
            printk("%s\n", buf);
        }
    }

    kfree(buf);
}

void cmd_wifi(int argc, char** argv, PrintFn print) {
    wifi::cmd_wifi(argc, argv, print);
}

void cmd_route(int argc, char** argv, PrintFn print) {
    wifi::cmd_route(argc, argv, print);
}

} // namespace net
