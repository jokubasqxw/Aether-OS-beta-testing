#include "updater.hpp"
#include "net.hpp"
#include "fat16.hpp"
#include "aetherfs.hpp"
#include "kstring.hpp"
#include "printk.hpp"

namespace updater {

static const char* KERNEL_NAME = "AETHEROSBIN";
static const char* BACKUP_NAME = "kernel.bak";
static const char* CONFIG_NAME = "update.conf";
static constexpr size_t DOWNLOAD_CAP = 8u * 1024u * 1024u;

static Config g_cfg = { "raw.githubusercontent.com", 80, "/jokubasqxw/Aether-OS-beta-testing/main/release/aetheros32.bin" };

void load_config() {
    if (!aetherfs::mounted() || !aetherfs::exists(CONFIG_NAME)) return;
    char buf[256];
    int n = aetherfs::read_file(CONFIG_NAME, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = '\0';

    // Format: host\nport\npath\n
    int line = 0;
    char* p = buf;
    while (*p && line < 3) {
        char* start = p;
        while (*p && *p != '\n') p++;
        char saved = *p;
        *p = '\0';
        if (line == 0) {
            kstd::strncpy(g_cfg.host, start, sizeof(g_cfg.host) - 1);
            g_cfg.host[sizeof(g_cfg.host) - 1] = '\0';
        } else if (line == 1) {
            uint16_t port = 0;
            for (char* d = start; *d >= '0' && *d <= '9'; ++d) port = static_cast<uint16_t>(port * 10 + (*d - '0'));
            if (port) g_cfg.port = port;
        } else if (line == 2) {
            kstd::strncpy(g_cfg.path, start, sizeof(g_cfg.path) - 1);
            g_cfg.path[sizeof(g_cfg.path) - 1] = '\0';
        }
        line++;
        if (saved == '\0') break;
        p++;
    }
}

const Config& config() { return g_cfg; }

static const char* KERNEL_VERSION = "1.0.0";

void set_config(const char* host, uint16_t port, const char* path) {
    if (host && *host) {
        kstd::strncpy(g_cfg.host, host, sizeof(g_cfg.host) - 1);
        g_cfg.host[sizeof(g_cfg.host) - 1] = '\0';
    }
    if (port) g_cfg.port = port;
    if (path && *path) {
        kstd::strncpy(g_cfg.path, path, sizeof(g_cfg.path) - 1);
        g_cfg.path[sizeof(g_cfg.path) - 1] = '\0';
    }

    if (!aetherfs::mounted()) return;
    char out[256];
    out[0] = '\0';
    kstd::strcpy(out, g_cfg.host);
    kstd::strcpy(out + kstd::strlen(out), "\n");
    char port_str[8];
    kstd::itoa(static_cast<uint64_t>(g_cfg.port), port_str, 10);
    kstd::strcpy(out + kstd::strlen(out), port_str);
    kstd::strcpy(out + kstd::strlen(out), "\n");
    kstd::strcpy(out + kstd::strlen(out), g_cfg.path);
    kstd::strcpy(out + kstd::strlen(out), "\n");
    aetherfs::write_file(CONFIG_NAME, out, kstd::strlen(out));
}

const char* result_name(Result r) {
    switch (r) {
        case Result::Ok:             return "ok";
        case Result::UpToDate:       return "already up to date";
        case Result::NoDisk:         return "no disk / filesystem";
        case Result::ResolveFailed:  return "cannot resolve update host";
        case Result::DownloadFailed: return "download failed";
        case Result::BadImage:       return "downloaded image is not a valid kernel";
        case Result::BackupFailed:   return "could not back up current kernel";
        case Result::WriteFailed:    return "could not write new kernel";
    }
    return "unknown error";
}

static bool resolve_host(const char* host, uint8_t out[4]) {
    bool numeric = true;
    for (const char* p = host; *p; ++p) {
        if (!((*p >= '0' && *p <= '9') || *p == '.')) { numeric = false; break; }
    }
    if (numeric && net::parse_ip(host, out)) return true;
    return net::dns_resolve(host, out, 3000);
}

// Downloads the update into a heap buffer, strips HTTP headers, validates the
// ELF header. On success `buf` owns the allocation and `body` points inside it;
// the caller must `delete[] buf` (never `body`, which may be offset).
static Result download(char*& buf, char*& body, size_t& body_len,
                       char* message, size_t message_len) {
    buf = nullptr;
    body = nullptr;
    body_len = 0;

    uint8_t ip[4];
    if (!resolve_host(g_cfg.host, ip)) {
        printk("[update] resolve failed for %s\n", g_cfg.host);
        return Result::ResolveFailed;
    }

    buf = new char[DOWNLOAD_CAP];
    if (!buf) return Result::DownloadFailed;

    int total = net::http_get(ip, g_cfg.port, g_cfg.path, buf, DOWNLOAD_CAP, 8000,
                              g_cfg.host, "identity");
    if (total <= 0) {
        delete[] buf;
        buf = nullptr;
        return Result::DownloadFailed;
    }

    char* he = kstd::strstr(buf, "\r\n\r\n");
    if (!he) {
        delete[] buf;
        buf = nullptr;
        return Result::BadImage;
    }
    body = he + 4;
    body_len = static_cast<size_t>(total) - static_cast<size_t>(body - buf);

    uint32_t magic;
    kstd::memcpy(&magic, body, 4);
    // Require ELF32 (the bootloader loads build/aetheros32.bin, an elf32-i386
    // multiboot image) so we never install an incompatible kernel.
    if (body_len < 64 || magic != 0x464C457F || static_cast<uint8_t>(body[4]) != 1) {
        delete[] buf;
        buf = nullptr;
        return Result::BadImage;
    }

    if (message && message_len) {
        const char* s = "downloaded kernel";
        kstd::strncpy(message, s, message_len - 1);
        message[message_len - 1] = '\0';
    }
    return Result::Ok;
}

// True when the downloaded image is byte-identical to the installed kernel.
static bool same_as_installed(const char* body, size_t len) {
    int sz = fat16::file_size(KERNEL_NAME);
    if (sz <= 0 || static_cast<size_t>(sz) != len) return false;

    char* cur = new char[static_cast<size_t>(sz)];
    if (!cur) return false;
    int got = fat16::read_file(KERNEL_NAME, cur, static_cast<size_t>(sz));
    bool same = (got == sz) && kstd::memcmp(cur, body, len) == 0;
    delete[] cur;
    return same;
}

Result fetch_verify(char* message, size_t message_len) {
    if (!fat16::mounted()) return Result::NoDisk;
    char* buf = nullptr;
    char* body = nullptr;
    size_t len = 0;
    Result r = download(buf, body, len, message, message_len);
    if (r != Result::Ok) return r;

    if (same_as_installed(body, len)) {
        // Report version info when kernels match
        if (message && message_len) {
            kstd::strcpy(message, "you are on the latest build (v1.0.0)");
            message[message_len - 1] = '\0';
        }
        delete[] buf;
        return Result::UpToDate;
    }

    if (message && message_len) {
        // "downloaded kernel" -> append size
        size_t used = kstd::strlen(message);
        if (used + 16 < message_len) {
            kstd::strcpy(message + used, " (");
            kstd::itoa(static_cast<uint64_t>(len), message + kstd::strlen(message), 10);
            kstd::strcpy(message + kstd::strlen(message), " bytes)");
        }
    }
    delete[] buf;
    return Result::Ok;
}

Result apply(char* message, size_t message_len) {
    if (!fat16::mounted() || !aetherfs::mounted()) return Result::NoDisk;

    char* buf = nullptr;
    char* body = nullptr;
    size_t len = 0;
    Result r = download(buf, body, len, message, message_len);
    if (r != Result::Ok) return r;

    // No-op if the server is already serving the installed build.
    if (same_as_installed(body, len)) {
        delete[] buf;
        if (message && message_len) {
            const char* s = "already running the latest build";
            kstd::strncpy(message, s, message_len - 1);
            message[message_len - 1] = '\0';
        }
        return Result::UpToDate;
    }

    // 1. Back up the current kernel into AetherFS so a crash can be undone.
    int old_size = fat16::file_size(KERNEL_NAME);
    if (old_size <= 0) {
        delete[] buf;
        return Result::BackupFailed;
    }
    char* oldbuf = new char[static_cast<size_t>(old_size)];
    if (!oldbuf) {
        delete[] buf;
        return Result::BackupFailed;
    }
    int got = fat16::read_file(KERNEL_NAME, oldbuf, static_cast<size_t>(old_size));
    bool backed_up = (got == old_size) &&
                     aetherfs::write_file(BACKUP_NAME, oldbuf, static_cast<size_t>(old_size));
    delete[] oldbuf;
    if (!backed_up) {
        delete[] buf;
        return Result::BackupFailed;
    }

    // 2. Snapshot the filesystem, then engage the crash lock.
    aetherfs::snapshot();
    aetherfs::set_update_lock(true);

    // 3. Install the new kernel image.
    bool ok = fat16::write_file(KERNEL_NAME, body, len);
    delete[] buf;

    // 4. Success: release the lock (keeps the snapshot for manual rollback).
    aetherfs::set_update_lock(false);

    if (!ok) return Result::WriteFailed;

    if (message && message_len) {
        const char* s = "update installed";
        kstd::strncpy(message, s, message_len - 1);
        message[message_len - 1] = '\0';
    }
    return Result::Ok;
}

bool restore_kernel_backup() {
    if (!fat16::mounted() || !aetherfs::mounted()) return false;
    int sz = aetherfs::file_size(BACKUP_NAME);
    if (sz <= 0) return false;

    char* buf = new char[static_cast<size_t>(sz)];
    if (!buf) return false;
    int n = aetherfs::read_file(BACKUP_NAME, buf, static_cast<size_t>(sz));
    bool ok = (n == sz) && fat16::write_file(KERNEL_NAME, buf, static_cast<size_t>(sz));
    delete[] buf;
    return ok;
}

} // namespace updater
