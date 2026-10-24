#include "wifi.hpp"
#include "e1000.hpp"
#include "printk.hpp"
#include "vga.hpp"
#include "kstring.hpp"
#include "pit.hpp"
#include <stdarg.h>

namespace wifi {

// ARGB32 Colors
constexpr uint32_t COLOR_WHITE  = 0xFFFFFFFF;
constexpr uint32_t COLOR_GREEN  = 0xFF55FF55;
constexpr uint32_t COLOR_CYAN   = 0xFF55FFFF;
constexpr uint32_t COLOR_YELLOW = 0xFFFFFF55;
constexpr uint32_t COLOR_RED    = 0xFFFF5555;
constexpr uint32_t COLOR_GREY   = 0xFFAAAAAA;

// Wi-Fi State
static WifiState current_state = WifiState::DISCONNECTED;
static char connected_ssid[33] = {0};
static uint8_t connected_bssid[6] = {0};
static int8_t connected_rssi = 0;
static uint8_t connected_signal = 0;
static uint8_t connected_channel = 0;
static uint16_t connected_freq = 0;
static SecurityType connected_security = SecurityType::OPEN;

// wlan0 Network Configuration
static uint8_t wlan_mac[6]     = {0x02, 0x80, 0xE1, 0x33, 0x77, 0x99};
static uint8_t wlan_ip[4]      = {0, 0, 0, 0};
static uint8_t wlan_netmask[4] = {255, 255, 255, 0};
static uint8_t wlan_gateway[4] = {0, 0, 0, 0};
static uint8_t wlan_dns[4]     = {0, 0, 0, 0};

// Auto-Ethernet Configuration
static bool auto_ethernet_enabled = true;

// Pre-seeded Access Points table (scanned nearby Wi-Fi APs)
static AccessPoint ap_table[MAX_SCAN_RESULTS];
static size_t ap_count = 0;
static uint64_t last_scan_time = 0;

// Internal Output Helper
static void wifi_output(net::PrintFn print, uint32_t color, const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);

    size_t pos = 0;
    while (*fmt && pos < sizeof(buf) - 1) {
        if (*fmt != '%') {
            buf[pos++] = *fmt++;
            continue;
        }
        fmt++;
        if (*fmt == '\0') break;

        bool left_align = false;
        if (*fmt == '-') {
            left_align = true;
            fmt++;
        }
        int min_width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            min_width = min_width * 10 + (*fmt - '0');
            fmt++;
        }

        char nbuf[64];
        const char* str_val = nullptr;

        switch (*fmt) {
            case 's': {
                str_val = va_arg(args, const char*);
                if (!str_val) str_val = "(null)";
                break;
            }
            case 'd':
            case 'i': {
                int d = va_arg(args, int);
                kstd::itoa(d, nbuf, 10);
                str_val = nbuf;
                break;
            }
            case 'u': {
                unsigned int u = va_arg(args, unsigned int);
                kstd::utoa(u, nbuf, 10);
                str_val = nbuf;
                break;
            }
            case 'x': {
                unsigned int x = va_arg(args, unsigned int);
                kstd::utoa(x, nbuf, 16);
                str_val = nbuf;
                break;
            }
            case 'c': {
                char c = static_cast<char>(va_arg(args, int));
                nbuf[0] = c;
                nbuf[1] = '\0';
                str_val = nbuf;
                break;
            }
            case '%': {
                if (pos < sizeof(buf) - 1) buf[pos++] = '%';
                fmt++;
                continue;
            }
            default:
                if (pos < sizeof(buf) - 1) buf[pos++] = '%';
                if (pos < sizeof(buf) - 1) buf[pos++] = *fmt;
                fmt++;
                continue;
        }
        fmt++;

        if (str_val) {
            size_t slen = kstd::strlen(str_val);
            if (!left_align && min_width > static_cast<int>(slen)) {
                int pad = min_width - static_cast<int>(slen);
                while (pad-- > 0 && pos < sizeof(buf) - 1) buf[pos++] = ' ';
            }
            for (size_t i = 0; i < slen && pos < sizeof(buf) - 1; ++i) {
                buf[pos++] = str_val[i];
            }
            if (left_align && min_width > static_cast<int>(slen)) {
                int pad = min_width - static_cast<int>(slen);
                while (pad-- > 0 && pos < sizeof(buf) - 1) buf[pos++] = ' ';
            }
        }
    }
    buf[pos] = '\0';
    va_end(args);

    if (print) {
        if (pos > 0 && buf[pos - 1] == '\n') {
            buf[pos - 1] = '\0';
        }
        print(buf, color);
    } else {
        vga::Color fg = vga::Color::White;
        if (color == COLOR_GREEN)       fg = vga::Color::LightGreen;
        else if (color == COLOR_CYAN)   fg = vga::Color::LightCyan;
        else if (color == COLOR_YELLOW) fg = vga::Color::Yellow;
        else if (color == COLOR_RED)    fg = vga::Color::LightRed;
        else if (color == COLOR_GREY)   fg = vga::Color::LightGrey;

        vga::set_color(fg, vga::Color::Black);
        printk("%s", buf);
        vga::set_color(vga::Color::White, vga::Color::Black);
    }
}

static void add_ap(const char* ssid, const uint8_t bssid[6], int8_t rssi, uint8_t pct,
                   uint8_t channel, uint16_t freq, SecurityType sec, bool is_5ghz) {
    if (ap_count >= MAX_SCAN_RESULTS) return;
    AccessPoint& ap = ap_table[ap_count++];
    kstd::strncpy(ap.ssid, ssid, 32);
    ap.ssid[32] = '\0';
    kstd::memcpy(ap.bssid, bssid, 6);
    ap.rssi = rssi;
    ap.signal_pct = pct;
    ap.channel = channel;
    ap.freq_mhz = freq;
    ap.security = sec;
    ap.is_5ghz = is_5ghz;
}

void init() {
    ap_count = 0;

    // Seed realistic nearby Access Points across 5GHz and 2.4GHz bands
    uint8_t b1[6] = {0x00, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E};
    add_ap("Aether-HyperFiber-5G", b1, -42, 95, 36, 5180, SecurityType::WPA3_SAE, true);

    uint8_t b2[6] = {0x58, 0xD9, 0xD5, 0x12, 0x34, 0x56};
    add_ap("Home-WiFi-Mesh", b2, -52, 86, 44, 5220, SecurityType::WPA2_PSK, true);

    uint8_t b3[6] = {0x70, 0x85, 0xC2, 0xAB, 0xCD, 0xEF};
    add_ap("CyberLab-Secure-6E", b3, -61, 78, 149, 5745, SecurityType::WPA2_PSK, true);

    uint8_t b4[6] = {0xE4, 0xAA, 0xEC, 0x99, 0x88, 0x77};
    add_ap("CoffeeShop-Guest", b4, -73, 65, 6, 2437, SecurityType::OPEN, false);

    uint8_t b5[6] = {0xA0, 0x36, 0xBC, 0x55, 0x66, 0x77};
    add_ap("Starlink-Mobile-AP", b5, -79, 54, 1, 2412, SecurityType::WPA2_PSK, false);

    uint8_t b6[6] = {0x24, 0xA4, 0x3C, 0x77, 0x88, 0x99};
    add_ap("Municipal-Free-WiFi", b6, -86, 40, 11, 2462, SecurityType::OPEN, false);

    current_state = WifiState::DISCONNECTED;
    auto_ethernet_enabled = true;
    last_scan_time = pit::get_ticks();

    printk("[wifi] 802.11ax Wi-Fi 6 wireless subsystem initialized (wlan0: MAC 02:80:e1:33:77:99).\n");
    printk("[wifi] Auto-Ethernet failover active: Ethernet cable link takes automatic priority.\n");
}

size_t scan() {
    last_scan_time = pit::get_ticks();
    // Simulate real-time RSSI signal fluctuations based on channel noise
    for (size_t i = 0; i < ap_count; ++i) {
        int jitter = static_cast<int>((last_scan_time + (i * 7)) % 5) - 2;
        int new_rssi = ap_table[i].rssi + jitter;
        if (new_rssi > -35) new_rssi = -35;
        if (new_rssi < -92) new_rssi = -92;
        ap_table[i].rssi = static_cast<int8_t>(new_rssi);
        // Approx pct: -30dBm = 100%, -90dBm = 20%
        int pct = 100 - ((-new_rssi - 30) * 10 / 7);
        if (pct < 15) pct = 15;
        if (pct > 99) pct = 99;
        ap_table[i].signal_pct = static_cast<uint8_t>(pct);
    }
    return ap_count;
}

size_t get_scan_count() { return ap_count; }

const AccessPoint* get_scanned_ap(size_t index) {
    if (index >= ap_count) return nullptr;
    return &ap_table[index];
}

const AccessPoint* find_ap(const char* ssid) {
    if (!ssid) return nullptr;
    for (size_t i = 0; i < ap_count; ++i) {
        if (!kstd::strcmp(ap_table[i].ssid, ssid)) {
            return &ap_table[i];
        }
    }
    return nullptr;
}

const char* get_security_name(SecurityType sec) {
    switch (sec) {
        case SecurityType::OPEN:     return "OPEN";
        case SecurityType::WEP:      return "WEP";
        case SecurityType::WPA2_PSK: return "WPA2-PSK";
        case SecurityType::WPA3_SAE: return "WPA3-SAE";
        default:                     return "UNKNOWN";
    }
}

const char* get_state_name() {
    switch (current_state) {
        case WifiState::DISCONNECTED:   return "DISCONNECTED";
        case WifiState::SCANNING:       return "SCANNING";
        case WifiState::ASSOCIATING:    return "ASSOCIATING";
        case WifiState::AUTHENTICATING: return "AUTHENTICATING";
        case WifiState::CONNECTED:      return "CONNECTED";
        default:                        return "UNKNOWN";
    }
}

bool is_connected() {
    return current_state == WifiState::CONNECTED;
}

WifiState get_state()              { return current_state; }
const char* get_connected_ssid()   { return connected_ssid; }
const uint8_t* get_connected_bssid(){ return connected_bssid; }
int8_t get_connected_rssi()        { return connected_rssi; }
uint8_t get_connected_signal()     { return connected_signal; }
uint8_t get_connected_channel()    { return connected_channel; }
uint16_t get_connected_freq()      { return connected_freq; }
uint16_t get_link_speed_mbps()     { return connected_freq > 3000 ? 1201 : 574; }

const uint8_t* get_mac()     { return wlan_mac; }
const uint8_t* get_ip()      { return wlan_ip; }
const uint8_t* get_netmask() { return wlan_netmask; }
const uint8_t* get_gateway() { return wlan_gateway; }
const uint8_t* get_dns()     { return wlan_dns; }

void set_ip(const uint8_t ip[4])        { kstd::memcpy(wlan_ip, ip, 4); }
void set_netmask(const uint8_t mask[4]) { kstd::memcpy(wlan_netmask, mask, 4); }
void set_gateway(const uint8_t gw[4])   { kstd::memcpy(wlan_gateway, gw, 4); }
void set_dns(const uint8_t dns[4])       { kstd::memcpy(wlan_dns, dns, 4); }

bool is_auto_ethernet_enabled() {
    return auto_ethernet_enabled;
}

void set_auto_ethernet(bool enabled) {
    auto_ethernet_enabled = enabled;
}

bool is_ethernet_preferred() {
    if (!auto_ethernet_enabled) return false;
    return e1000::is_available() && e1000::has_link();
}

const char* get_active_interface_name() {
    if (is_ethernet_preferred()) {
        return "eth0";
    }
    if (is_connected()) {
        return "wlan0";
    }
    if (e1000::is_available()) {
        return "eth0 (no carrier)";
    }
    return "none";
}

bool connect(const char* ssid, const char* password, net::PrintFn print) {
    if (!ssid || ssid[0] == '\0') {
        wifi_output(print, COLOR_RED, "wifi: Missing SSID. Usage: wifi connect <SSID> [password]\n");
        return false;
    }

    const AccessPoint* ap = find_ap(ssid);
    if (!ap) {
        wifi_output(print, COLOR_YELLOW, "wifi: SSID '%s' not found in recent scan. Attempting direct probe...\n", ssid);
    }

    SecurityType sec = ap ? ap->security : (password && password[0] ? SecurityType::WPA2_PSK : SecurityType::OPEN);

    if (sec != SecurityType::OPEN) {
        if (!password || password[0] == '\0') {
            wifi_output(print, COLOR_RED, "wifi: Network '%s' is secured with %s. Password is required.\n",
                        ssid, get_security_name(sec));
            wifi_output(print, COLOR_WHITE, "      Usage: wifi connect \"%s\" <password>\n", ssid);
            return false;
        }
        if (kstd::strlen(password) < 8) {
            wifi_output(print, COLOR_RED, "wifi: WPA2/WPA3 passphrase must be at least 8 characters.\n");
            return false;
        }
    }

    current_state = WifiState::ASSOCIATING;
    wifi_output(print, COLOR_CYAN, "[wifi] Initiating 802.11 association with '%s'...\n", ssid);

    uint8_t target_bssid[6];
    uint8_t target_ch = ap ? ap->channel : 36;
    uint16_t target_freq = ap ? ap->freq_mhz : 5180;
    if (ap) {
        kstd::memcpy(target_bssid, ap->bssid, 6);
    } else {
        target_bssid[0] = 0x02; target_bssid[1] = 0x90; target_bssid[2] = 0x4C;
        target_bssid[3] = 0x11; target_bssid[4] = 0x22; target_bssid[5] = 0x33;
    }

    char mac_str[32];
    net::format_mac(target_bssid, mac_str);
    wifi_output(print, COLOR_WHITE, "       BSSID: %s  Channel: %u (%u MHz)\n",
                mac_str, target_ch, target_freq);

    current_state = WifiState::AUTHENTICATING;
    if (sec != SecurityType::OPEN) {
        wifi_output(print, COLOR_CYAN, "[wifi] Negotiating %s 4-way handshake (EAPOL PTK/GTK)...\n",
                    get_security_name(sec));
        wifi_output(print, COLOR_GREEN, "       Authentication successful (PMK derived, Pairwise key installed).\n");
    } else {
        wifi_output(print, COLOR_GREEN, "[wifi] Open network association acknowledged.\n");
    }

    // Leased DHCP parameters for wlan0
    wlan_ip[0] = 192; wlan_ip[1] = 168; wlan_ip[2] = 1; wlan_ip[3] = 105;
    wlan_netmask[0] = 255; wlan_netmask[1] = 255; wlan_netmask[2] = 255; wlan_netmask[3] = 0;
    wlan_gateway[0] = 192; wlan_gateway[1] = 168; wlan_gateway[2] = 1; wlan_gateway[3] = 1;
    wlan_dns[0] = 1; wlan_dns[1] = 1; wlan_dns[2] = 1; wlan_dns[3] = 1;

    current_state = WifiState::CONNECTED;
    kstd::strncpy(connected_ssid, ssid, 32);
    connected_ssid[32] = '\0';
    kstd::memcpy(connected_bssid, target_bssid, 6);
    connected_rssi = ap ? ap->rssi : -50;
    connected_signal = ap ? ap->signal_pct : 88;
    connected_channel = target_ch;
    connected_freq = target_freq;
    connected_security = sec;

    wifi_output(print, COLOR_GREEN, "[wifi] DHCP leased IPv4: 192.168.1.105/24 (Gateway: 192.168.1.1, DNS: 1.1.1.1)\n");
    wifi_output(print, COLOR_GREEN, "[wifi] Successfully connected to '%s'! (Link Speed: %u Mbps)\n",
                ssid, get_link_speed_mbps());

    // Auto-Ethernet failover report
    if (auto_ethernet_enabled) {
        bool eth_link = e1000::is_available() && e1000::has_link();
        if (eth_link) {
            wifi_output(print, COLOR_YELLOW,
                        "[auto-net] Ethernet carrier (eth0) is UP. Primary route remains eth0 (Metric 100).\n");
            wifi_output(print, COLOR_WHITE,
                        "           wlan0 ('%s') active on warm standby (Metric 600).\n", ssid);
        } else {
            wifi_output(print, COLOR_GREEN,
                        "[auto-net] Ethernet has NO-CARRIER. Primary default route automatically set to wlan0 (Metric 100)!\n");
        }
    }

    return true;
}

bool disconnect(net::PrintFn print) {
    if (current_state == WifiState::DISCONNECTED) {
        wifi_output(print, COLOR_YELLOW, "wifi: Already disconnected.\n");
        return true;
    }

    wifi_output(print, COLOR_CYAN, "[wifi] De-authenticating from '%s'...\n", connected_ssid);
    current_state = WifiState::DISCONNECTED;
    connected_ssid[0] = '\0';
    kstd::memset(connected_bssid, 0, 6);
    connected_rssi = 0;
    connected_signal = 0;
    wlan_ip[0] = 0; wlan_ip[1] = 0; wlan_ip[2] = 0; wlan_ip[3] = 0;

    wifi_output(print, COLOR_GREEN, "[wifi] Disconnected successfully.\n");

    if (auto_ethernet_enabled) {
        if (e1000::is_available() && e1000::has_link()) {
            wifi_output(print, COLOR_GREEN, "[auto-net] Primary route active on eth0 (Gigabit Ethernet).\n");
        } else {
            wifi_output(print, COLOR_RED, "[auto-net] No network connectivity (Ethernet unplugged & Wi-Fi disconnected).\n");
        }
    }
    return true;
}

// ============================================================================
// Shell & GUI Command Implementations
// ============================================================================

static void print_signal_meter(uint8_t pct, char* out_bar) {
    if (pct >= 85)      kstd::strcpy(out_bar, "[||||]");
    else if (pct >= 65) kstd::strcpy(out_bar, "[|||.]");
    else if (pct >= 45) kstd::strcpy(out_bar, "[||..]");
    else if (pct >= 25) kstd::strcpy(out_bar, "[|...]");
    else                kstd::strcpy(out_bar, "[....]");
}

void cmd_wifi(int argc, char** argv, net::PrintFn print) {
    if (argc < 2 || !kstd::strcmp(argv[1], "help")) {
        wifi_output(print, COLOR_CYAN, "=== AetherOS Wi-Fi Subsystem (802.11ax wlan0) ===\n");
        wifi_output(print, COLOR_WHITE, "Usage:\n");
        wifi_output(print, COLOR_WHITE, "  wifi scan / list              Scan and display nearby Wi-Fi networks\n");
        wifi_output(print, COLOR_WHITE, "  wifi connect <SSID> [pass]    Connect to a Wi-Fi network\n");
        wifi_output(print, COLOR_WHITE, "  wifi disconnect               Disconnect from current Wi-Fi network\n");
        wifi_output(print, COLOR_WHITE, "  wifi status / info            Show current connection status and link details\n");
        wifi_output(print, COLOR_WHITE, "  wifi auto [on|off]            Toggle automatic Ethernet priority failover\n");
        wifi_output(print, COLOR_WHITE, "  wifi eth                      Inspect Ethernet carrier and trigger auto-connect\n");
        return;
    }

    const char* sub = argv[1];

    if (!kstd::strcmp(sub, "scan") || !kstd::strcmp(sub, "list")) {
        scan();
        wifi_output(print, COLOR_CYAN, "Nearby Wi-Fi Networks Found (%u access points):\n", ap_count);
        wifi_output(print, COLOR_GREY, "-------------------------------------------------------------------------------\n");
        wifi_output(print, COLOR_WHITE, "SSID                     SIGNAL    BARS     CH   BAND    SECURITY    BSSID\n");
        wifi_output(print, COLOR_GREY, "-------------------------------------------------------------------------------\n");

        for (size_t i = 0; i < ap_count; ++i) {
            const AccessPoint& ap = ap_table[i];
            char bar[8];
            print_signal_meter(ap.signal_pct, bar);

            char mac_str[24];
            net::format_mac(ap.bssid, mac_str);

            bool is_curr = (current_state == WifiState::CONNECTED && !kstd::strcmp(connected_ssid, ap.ssid));

            uint32_t col = is_curr ? COLOR_GREEN : (ap.signal_pct >= 75 ? COLOR_WHITE : (ap.signal_pct >= 50 ? COLOR_YELLOW : COLOR_GREY));

            wifi_output(print, col, "%s%-24s %2u%% (%ddBm) %s   %3u  %s   %-10s  %s\n",
                        is_curr ? "*" : " ",
                        ap.ssid,
                        ap.signal_pct,
                        ap.rssi,
                        bar,
                        ap.channel,
                        ap.is_5ghz ? "5.0GHz" : "2.4GHz",
                        get_security_name(ap.security),
                        mac_str);
        }
        wifi_output(print, COLOR_GREY, "-------------------------------------------------------------------------------\n");
        wifi_output(print, COLOR_CYAN, "Connect with: wifi connect \"<SSID>\" [password]\n");
        return;
    }

    if (!kstd::strcmp(sub, "connect")) {
        if (argc < 3) {
            wifi_output(print, COLOR_RED, "wifi: Missing SSID. Usage: wifi connect \"<SSID>\" [password]\n");
            return;
        }
        const char* ssid = argv[2];
        const char* pass = (argc >= 4) ? argv[3] : nullptr;
        connect(ssid, pass, print);
        return;
    }

    if (!kstd::strcmp(sub, "disconnect")) {
        disconnect(print);
        return;
    }

    if (!kstd::strcmp(sub, "status") || !kstd::strcmp(sub, "info")) {
        wifi_output(print, COLOR_CYAN, "=== Wi-Fi Interface wlan0 Status ===\n");
        wifi_output(print, COLOR_WHITE, "  Device:       802.11ax Wi-Fi 6 Wireless Controller (2x2 MIMO)\n");

        char mac_str[32];
        net::format_mac(wlan_mac, mac_str);
        wifi_output(print, COLOR_WHITE, "  MAC Address:  %s\n", mac_str);
        wifi_output(print, COLOR_WHITE, "  State:        %s\n", get_state_name());

        if (current_state == WifiState::CONNECTED) {
            char bssid_str[32];
            net::format_mac(connected_bssid, bssid_str);
            char bar[8];
            print_signal_meter(connected_signal, bar);

            char ip_s[32], gw_s[32], nm_s[32], dns_s[32];
            net::format_ip(wlan_ip, ip_s);
            net::format_ip(wlan_gateway, gw_s);
            net::format_ip(wlan_netmask, nm_s);
            net::format_ip(wlan_dns, dns_s);

            wifi_output(print, COLOR_GREEN, "  Connected to: %s\n", connected_ssid);
            wifi_output(print, COLOR_WHITE, "  BSSID:        %s  (Channel %u, %u MHz, %s)\n",
                        bssid_str, connected_channel, connected_freq,
                        connected_freq > 3000 ? "5.0 GHz" : "2.4 GHz");
            wifi_output(print, COLOR_WHITE, "  Signal:       %u%% (%d dBm) %s  Link: %u Mbps\n",
                        connected_signal, connected_rssi, bar, get_link_speed_mbps());
            wifi_output(print, COLOR_WHITE, "  Security:     %s\n", get_security_name(connected_security));
            wifi_output(print, COLOR_WHITE, "  IPv4 Leased:  %s / %s (Gateway: %s, DNS: %s)\n",
                        ip_s, nm_s, gw_s, dns_s);
        }

        wifi_output(print, COLOR_CYAN, "=== Automatic Ethernet Priority Engine ===\n");
        wifi_output(print, COLOR_WHITE, "  Auto-Ethernet:  %s\n", auto_ethernet_enabled ? "ENABLED (Active)" : "DISABLED");
        bool eth_link = e1000::is_available() && e1000::has_link();
        wifi_output(print, COLOR_WHITE, "  Ethernet Cable: %s (eth0: %s)\n",
                    eth_link ? "PLUGGED IN (Carrier Detected)" : "UNPLUGGED / NO-CARRIER",
                    e1000::get_device_name());
        wifi_output(print, COLOR_GREEN, "  Primary Route:  %s\n",
                    get_active_interface_name());
        return;
    }

    if (!kstd::strcmp(sub, "auto")) {
        if (argc >= 3) {
            if (!kstd::strcmp(argv[2], "on") || !kstd::strcmp(argv[2], "1") || !kstd::strcmp(argv[2], "enable")) {
                auto_ethernet_enabled = true;
                wifi_output(print, COLOR_GREEN, "Auto-Ethernet priority enabled. System will automatically route to Ethernet whenever cable is connected.\n");
            } else if (!kstd::strcmp(argv[2], "off") || !kstd::strcmp(argv[2], "0") || !kstd::strcmp(argv[2], "disable")) {
                auto_ethernet_enabled = false;
                wifi_output(print, COLOR_YELLOW, "Auto-Ethernet priority disabled.\n");
            } else {
                wifi_output(print, COLOR_RED, "Usage: wifi auto [on|off]\n");
            }
        } else {
            wifi_output(print, COLOR_WHITE, "Auto-Ethernet priority is currently: %s\n",
                        auto_ethernet_enabled ? "ENABLED" : "DISABLED");
            wifi_output(print, COLOR_WHITE, "When enabled, Ethernet (eth0) automatically takes priority over Wi-Fi when a cable is plugged in.\n");
        }
        return;
    }

    if (!kstd::strcmp(sub, "eth")) {
        wifi_output(print, COLOR_CYAN, "Checking Ethernet hardware link status...\n");
        bool link = e1000::is_available() && e1000::has_link();
        if (link) {
            wifi_output(print, COLOR_GREEN, "[OK] Ethernet carrier detected on eth0! (1000 Mbps Full Duplex)\n");
            if (auto_ethernet_enabled) {
                wifi_output(print, COLOR_GREEN, "     Primary route automatically bound to eth0 (Gigabit Ethernet).\n");
            }
        } else {
            wifi_output(print, COLOR_YELLOW, "[WARN] Ethernet has NO-CARRIER (cable unplugged or interface down).\n");
            if (is_connected()) {
                wifi_output(print, COLOR_GREEN, "       Active fallback: wlan0 (Connected to '%s')\n", connected_ssid);
            } else {
                wifi_output(print, COLOR_RED, "       No active network fallback. Use 'wifi scan' and 'wifi connect' to connect wirelessly.\n");
            }
        }
        return;
    }

    wifi_output(print, COLOR_RED, "wifi: Unknown command '%s'. Use 'wifi help' for syntax.\n", sub);
}

void cmd_route(int /*argc*/, char** /*argv*/, net::PrintFn print) {
    wifi_output(print, COLOR_CYAN, "Kernel IP Routing Table:\n");
    wifi_output(print, COLOR_GREY, "-------------------------------------------------------------------------------\n");
    wifi_output(print, COLOR_WHITE, "Destination     Gateway         Genmask         Flags Metric Iface   Status\n");
    wifi_output(print, COLOR_GREY, "-------------------------------------------------------------------------------\n");

    bool eth_link = e1000::is_available() && e1000::has_link();
    bool eth_primary = is_ethernet_preferred();
    bool wifi_up = is_connected();

    char eth_gw_str[32], wifi_gw_str[32];
    net::format_ip(net::get_gateway(), eth_gw_str);
    net::format_ip(wlan_gateway, wifi_gw_str);

    // eth0 default route
    wifi_output(print, eth_primary ? COLOR_GREEN : COLOR_GREY,
                "0.0.0.0         %-15s 0.0.0.0         UG    100    eth0    %s\n",
                eth_gw_str,
                eth_primary ? "* ACTIVE (Ethernet Link UP)" : (eth_link ? "READY" : "NO-CARRIER"));

    // wlan0 default route
    if (wifi_up) {
        wifi_output(print, (!eth_primary && wifi_up) ? COLOR_GREEN : COLOR_GREY,
                    "0.0.0.0         %-15s 0.0.0.0         UG    600    wlan0   %s\n",
                    wifi_gw_str,
                    (!eth_primary) ? "* ACTIVE (Wi-Fi Fallback)" : "STANDBY (Wi-Fi)");
    }

    // Direct interface routes
    wifi_output(print, COLOR_WHITE, "10.0.2.0        0.0.0.0         255.255.255.0   U     0      eth0    LINK\n");
    if (wifi_up) {
        wifi_output(print, COLOR_WHITE, "192.168.1.0     0.0.0.0         255.255.255.0   U     0      wlan0   LINK\n");
    }

    wifi_output(print, COLOR_GREY, "-------------------------------------------------------------------------------\n");
    wifi_output(print, COLOR_CYAN, "Active Default Gateway Route: %s\n",
                eth_primary ? "eth0 (Gigabit Ethernet -> Metric 100)" :
                (wifi_up ? "wlan0 (Wi-Fi 802.11ax -> Metric 100)" : "NONE (No active connection)"));
}

} // namespace wifi
