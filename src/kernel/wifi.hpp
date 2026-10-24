#ifndef KERNEL_WIFI_HPP
#define KERNEL_WIFI_HPP

#include "types.hpp"
#include "net.hpp"

namespace wifi {

enum class SecurityType {
    OPEN,
    WEP,
    WPA2_PSK,
    WPA3_SAE
};

enum class WifiState {
    DISCONNECTED,
    SCANNING,
    ASSOCIATING,
    AUTHENTICATING,
    CONNECTED
};

struct AccessPoint {
    char ssid[33];
    uint8_t bssid[6];
    int8_t rssi;          // dBm (-30 to -95)
    uint8_t signal_pct;   // 0 - 100%
    uint8_t channel;      // 1-14 (2.4GHz) or 36-165 (5GHz)
    uint16_t freq_mhz;    // e.g. 2412, 5180
    SecurityType security;
    bool is_5ghz;
};

constexpr size_t MAX_SCAN_RESULTS = 16;

// Subsystem Lifecycle
void init();

// Scanning & Connection
size_t scan();
size_t get_scan_count();
const AccessPoint* get_scanned_ap(size_t index);
const AccessPoint* find_ap(const char* ssid);

bool connect(const char* ssid, const char* password = nullptr, net::PrintFn print = nullptr);
bool disconnect(net::PrintFn print = nullptr);

// Connection State & Info
WifiState get_state();
bool is_connected();
const char* get_state_name();
const char* get_connected_ssid();
const uint8_t* get_connected_bssid();
int8_t get_connected_rssi();
uint8_t get_connected_signal();
uint8_t get_connected_channel();
uint16_t get_connected_freq();
uint16_t get_link_speed_mbps();
const char* get_security_name(SecurityType sec);

// wlan0 Interface Info
const uint8_t* get_mac();
const uint8_t* get_ip();
const uint8_t* get_netmask();
const uint8_t* get_gateway();
const uint8_t* get_dns();

void set_ip(const uint8_t ip[4]);
void set_netmask(const uint8_t mask[4]);
void set_gateway(const uint8_t gw[4]);
void set_dns(const uint8_t dns[4]);

// Auto-Ethernet Priority & Carrier Detection
bool is_auto_ethernet_enabled();
void set_auto_ethernet(bool enabled);
const char* get_active_interface_name(); // "eth0" or "wlan0" or "none"
bool is_ethernet_preferred();

// Shell & GUI Commands
void cmd_wifi(int argc, char** argv, net::PrintFn print = nullptr);
void cmd_route(int argc, char** argv, net::PrintFn print = nullptr);

} // namespace wifi

#endif // KERNEL_WIFI_HPP
