#include "tls.hpp"
#include "net.hpp"
#include "heap.hpp"
#include "kstring.hpp"
#include "pit.hpp"
#include "printk.hpp"
#include "rtc.hpp"
#include "crypto/crypto.hpp"
#include "bearssl/inc/bearssl.h"

extern "C" const br_x509_trust_anchor *aether_tls_trust_anchors(void);
extern "C" size_t aether_tls_trust_anchors_num(void);

namespace tls {

namespace {

constexpr size_t kTcpBufferSize = 65536;
constexpr size_t kEntropySize = 64;

br_ssl_client_context g_client;
br_x509_minimal_context g_x509;
unsigned char g_iobuf[BR_SSL_BUFSIZE_BIDI];
char g_last_error[96] = "secure handshake failed";

struct IoState {
    uint32_t timeout_ms;
};

int low_read(void* ctx, unsigned char* data, size_t len) {
    IoState* io = static_cast<IoState*>(ctx);
    uint64_t deadline = pit::uptime_ms() + io->timeout_ms;
    for (;;) {
        uint64_t now = pit::uptime_ms();
        if (now >= deadline) return -1;
        int r = net::tcp_recv(data, len, static_cast<uint32_t>(deadline - now));
        if (r > 0) return r;
        if (r < 0) return -1;
    }
}

int low_write(void* ctx, const unsigned char* data, size_t len) {
    (void)ctx;
    return net::tcp_send(data, len);
}

uint32_t days_from_civil(int32_t y, uint32_t m, uint32_t d) {
    y -= m <= 2;
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = static_cast<uint32_t>(y - era * 400);
    const uint32_t mp = (m > 2) ? (m - 3) : (m + 9);
    const uint32_t doy = (153 * mp + 2) / 5 + d - 1;
    const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<uint32_t>(era * 146097 + static_cast<int32_t>(doe) - 719468);
}

void set_validation_time() {
    rtc::DateTime dt = rtc::get_datetime();
    uint32_t days = days_from_civil(dt.year, dt.month, dt.day) + 719528;
    uint32_t seconds = static_cast<uint32_t>(dt.hour) * 3600 +
                       static_cast<uint32_t>(dt.minute) * 60 +
                       static_cast<uint32_t>(dt.second);
    br_x509_minimal_set_time(&g_x509, days, seconds);
}

const char* error_text(int err) {
    if (err == BR_ERR_X509_NOT_TRUSTED) return "certificate is not trusted";
    if (err == BR_ERR_X509_EXPIRED) return "certificate is expired or not yet valid";
    if (err == BR_ERR_X509_BAD_SERVER_NAME) return "certificate does not match the host name";
    if (err == BR_ERR_X509_BAD_SIGNATURE) return "certificate signature is invalid";
    if (err == BR_ERR_X509_TIME_UNKNOWN) return "certificate time could not be verified";
    if (err == BR_ERR_X509_WEAK_PUBLIC_KEY) return "certificate key is too weak";
    if (err == BR_ERR_BAD_SIGNATURE) return "handshake signature is invalid";
    if (err == BR_ERR_UNSUPPORTED_VERSION || err == BR_ERR_BAD_VERSION)
        return "server does not support TLS 1.2";
    if (err == BR_ERR_BAD_CIPHER_SUITE) return "no shared cipher suite";
    if (err == BR_ERR_NO_RANDOM) return "secure random generator failed";
    if (err == BR_ERR_IO) return "network connection failed";
    if (err >= BR_ERR_RECV_FATAL_ALERT) return "server rejected the secure connection";
    return "secure handshake failed";
}

int fail(const char* what) {
    int err = br_ssl_engine_last_error(&g_client.eng);
    kstd::strncpy(g_last_error, error_text(err), sizeof(g_last_error) - 1);
    g_last_error[sizeof(g_last_error) - 1] = '\0';
    printk("[tls] %s: %s (err %d)\n", what, g_last_error, err);
    net::tcp_close();
    return -1;
}

void build_request(const char* host, uint16_t port, const char* path,
                   char* req, size_t cap) {
    size_t n = 0;
    auto add = [&](const char* s) {
        while (*s && n + 1 < cap) req[n++] = *s++;
    };
    add("GET ");
    add((path && *path) ? path : "/");
    add(" HTTP/1.1\r\nHost: ");
    add(host);
    if (port != 443) {
        char pt[8];
        kstd::utoa(port, pt, 10);
        add(":");
        add(pt);
    }
    add("\r\nUser-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/120.0 Safari/537.36\r\n");
    add("Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n");
    add("Accept-Language: en-US,en;q=0.9\r\n");
    add("Accept-Encoding: gzip, deflate\r\nConnection: close\r\n\r\n");
    req[n] = '\0';
}

} // namespace

const char* last_error() {
    return g_last_error;
}

int https_get(const uint8_t server_ip[4], uint16_t port, const char* host,
              const char* path, char* response, size_t max_len,
              uint32_t timeout_ms) {
    if (!response || max_len < 64 || !host || !*host) return -1;
    response[0] = '\0';
    kstd::strcpy(g_last_error, "secure handshake failed");

    char* tcp_buf = static_cast<char*>(kmalloc(kTcpBufferSize));
    if (!tcp_buf) {
        kstd::strcpy(g_last_error, "out of memory");
        return -1;
    }
    if (!net::tcp_connect(server_ip, port, tcp_buf, kTcpBufferSize, timeout_ms)) {
        kstd::strcpy(g_last_error, "network connection failed");
        printk("[tls] tcp connect failed\n");
        kfree(tcp_buf);
        return -1;
    }

    br_ssl_client_init_full(&g_client, &g_x509, aether_tls_trust_anchors(),
                            aether_tls_trust_anchors_num());
    br_x509_minimal_set_minrsa(&g_x509, 256);
    set_validation_time();

    static const char* protocols[] = { "http/1.1" };
    br_ssl_engine_set_protocol_names(&g_client.eng, protocols, 1);
    br_ssl_engine_set_buffer(&g_client.eng, g_iobuf, sizeof(g_iobuf), 1);

    uint8_t entropy[kEntropySize];
    if (!crypto::random_bytes(entropy, sizeof(entropy))) {
        kfree(tcp_buf);
        g_last_error[0] = '\0';
        kstd::strcpy(g_last_error, "secure random generator failed");
        printk("[tls] entropy failed\n");
        net::tcp_close();
        return -1;
    }
    br_ssl_engine_inject_entropy(&g_client.eng, entropy, sizeof(entropy));
    kstd::memset(entropy, 0, sizeof(entropy));

    if (!br_ssl_client_reset(&g_client, host, 0)) {
        kfree(tcp_buf);
        return fail("reset failed");
    }

    IoState io = { timeout_ms };
    br_sslio_context ioc;
    br_sslio_init(&ioc, &g_client.eng, low_read, &io, low_write, &io);

    char req[768];
    build_request(host, port, path, req, sizeof(req));
    if (br_sslio_write_all(&ioc, req, kstd::strlen(req)) < 0) {
        kfree(tcp_buf);
        return fail("request failed");
    }
    if (br_sslio_flush(&ioc) < 0) {
        kfree(tcp_buf);
        return fail("flush failed");
    }

    size_t total = 0;
    while (total < max_len - 1) {
        size_t room = max_len - 1 - total;
        int r = br_sslio_read(&ioc, response + total, room);
        if (r <= 0) break;
        total += static_cast<size_t>(r);
    }
    response[total] = '\0';

    if (total == 0) {
        kfree(tcp_buf);
        return fail("empty response");
    }

    kfree(tcp_buf);
    net::tcp_close();
    return static_cast<int>(total);
}

} // namespace tls
