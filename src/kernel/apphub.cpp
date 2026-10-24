#include "apphub.hpp"
#include "aetherfs.hpp"
#include "updater.hpp"
#include "net.hpp"
#include "kstring.hpp"
#include "printk.hpp"

namespace apphub {

static constexpr size_t CATALOG_CAP = 64 * 1024;
static constexpr size_t APP_CAP = 256 * 1024;
static constexpr const char* CATALOG_PATH = "/apps/catalog.txt";

static App g_apps[MAX_APPS];
static int g_count = 0;

static void append(char* out, size_t cap, size_t& o, const char* s) {
    while (*s && o + 1 < cap) out[o++] = *s++;
    out[o] = '\0';
}

static void set_msg(char* msg, size_t len, const char* s) {
    if (!msg || len == 0) return;
    kstd::strncpy(msg, s, len - 1);
    msg[len - 1] = '\0';
}

static void sanitize(const char* in, char* out, size_t cap) {
    size_t o = 0;
    for (const char* p = in; p && *p && o + 1 < cap; ++p) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_';
        out[o++] = ok ? c : '_';
    }
    out[o] = '\0';
}

static void html_name(const char* name, char* out, size_t cap) {
    char id[24];
    sanitize(name, id, sizeof(id));
    size_t o = 0;
    out[0] = '\0';
    append(out, cap, o, "app.");
    append(out, cap, o, id);
    append(out, cap, o, ".html");
}

static void ver_name(const char* name, char* out, size_t cap) {
    char id[24];
    sanitize(name, id, sizeof(id));
    size_t o = 0;
    out[0] = '\0';
    append(out, cap, o, "app.");
    append(out, cap, o, id);
    append(out, cap, o, ".ver");
}

static App* ensure(const char* name) {
    char id[24];
    sanitize(name, id, sizeof(id));
    if (id[0] == '\0') return nullptr;

    for (int i = 0; i < g_count; ++i) {
        if (!kstd::strcmp(g_apps[i].name, id)) return &g_apps[i];
    }
    if (g_count >= static_cast<int>(MAX_APPS)) return nullptr;

    App* a = &g_apps[g_count++];
    kstd::memset(a, 0, sizeof(*a));
    kstd::strncpy(a->name, id, sizeof(a->name) - 1);
    return a;
}

static void read_installed(App& a) {
    char vf[48];
    char hf[48];
    ver_name(a.name, vf, sizeof(vf));
    html_name(a.name, hf, sizeof(hf));

    a.installed_version[0] = '\0';
    if (!aetherfs::exists(hf)) { a.installed = false; return; }

    int n = aetherfs::file_size(vf);
    if (n <= 0) { a.installed = false; return; }
    size_t cap = sizeof(a.installed_version);
    if (static_cast<size_t>(n) >= cap) n = static_cast<int>(cap - 1);
    if (n <= 0) { a.installed = false; return; }

    int got = aetherfs::read_file(vf, a.installed_version, static_cast<size_t>(n));
    if (got <= 0) { a.installed_version[0] = '\0'; a.installed = false; return; }
    a.installed_version[got] = '\0';
    a.installed = true;
}

static void scan_installed() {
    const aetherfs::Entry* e = aetherfs::entries();
    for (size_t i = 0; i < aetherfs::MAX_FILES; ++i) {
        if (!(e[i].flags & aetherfs::FLAG_USED)) continue;
        const char* n = e[i].name;
        size_t len = kstd::strlen(n);
        if (len < 9) continue;
        if (kstd::strncmp(n, "app.", 4) != 0) continue;
        if (kstd::strcmp(n + len - 4, ".ver") != 0) continue;

        size_t id_len = len - 8; // strip "app." and ".ver"
        if (id_len == 0 || id_len >= 24) continue;

        char id[24];
        kstd::strncpy(id, n + 4, id_len);
        id[id_len] = '\0';

        App* a = ensure(id);
        if (!a) break;
        read_installed(*a);
        if (!a->version[0] && a->installed) {
            kstd::strncpy(a->version, a->installed_version, sizeof(a->version) - 1);
        }
        if (!a->desc[0]) kstd::strncpy(a->desc, "Installed app", sizeof(a->desc) - 1);
    }

    for (int i = 0; i < g_count; ++i) read_installed(g_apps[i]);
}

void init() {
    g_count = 0;
    kstd::memset(g_apps, 0, sizeof(g_apps));
    if (aetherfs::mounted()) scan_installed();
}

int count() { return g_count; }

const App* get(int idx) {
    if (idx < 0 || idx >= g_count) return nullptr;
    return &g_apps[idx];
}

int find(const char* name) {
    char id[24];
    sanitize(name, id, sizeof(id));
    for (int i = 0; i < g_count; ++i) {
        if (!kstd::strcmp(g_apps[i].name, id)) return i;
    }
    return -1;
}

bool is_installed(const char* name) {
    char hf[48];
    char vf[48];
    html_name(name, hf, sizeof(hf));
    ver_name(name, vf, sizeof(vf));
    return aetherfs::exists(hf) && aetherfs::exists(vf);
}

// Fetch `path` from the configured update host. On success `buf` owns the
// allocation and `body` points inside it; the caller must `delete[] buf`.
static int http_fetch(const char* path, char*& buf, char*& body, size_t& body_len,
                      size_t cap) {
    buf = nullptr;
    body = nullptr;
    body_len = 0;
    if (!path || !*path) return -1;

    const updater::Config& cfg = updater::config();
    uint8_t ip[4];
    bool numeric = true;
    for (const char* p = cfg.host; *p; ++p) {
        if (!((*p >= '0' && *p <= '9') || *p == '.')) { numeric = false; break; }
    }
    bool resolved = numeric && net::parse_ip(cfg.host, ip);
    if (!resolved) resolved = net::dns_resolve(cfg.host, ip, 3000);
    if (!resolved) return -1;

    buf = new char[cap];
    if (!buf) return -1;

    int total = net::http_get(ip, cfg.port, path, buf, cap, 8000, cfg.host, "identity");
    if (total <= 0) {
        delete[] buf;
        buf = nullptr;
        return -1;
    }

    if (kstd::strncmp(buf, "HTTP/", 5) == 0) {
        char* sp = kstd::strchr(buf, ' ');
        if (!sp || sp[1] != '2') {
            delete[] buf;
            buf = nullptr;
            return -2;
        }
    }

    char* he = kstd::strstr(buf, "\r\n\r\n");
    if (!he) {
        delete[] buf;
        buf = nullptr;
        return -3;
    }
    body = he + 4;
    body_len = static_cast<size_t>(total) - static_cast<size_t>(body - buf);
    return 0;
}

bool refresh_catalog(char* msg, size_t msg_len) {
    char* buf = nullptr;
    char* body = nullptr;
    size_t len = 0;
    int r = http_fetch(CATALOG_PATH, buf, body, len, CATALOG_CAP);
    if (r != 0) {
        set_msg(msg, msg_len, "Catalog download failed");
        return false;
    }

    g_count = 0;
    kstd::memset(g_apps, 0, sizeof(g_apps));

    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && body[i] != '\n') ++i;
        size_t line_len = i - start;
        if (i < len) ++i;
        if (line_len > 0 && body[start + line_len - 1] == '\r') --line_len;

        size_t p = start;
        while (p < start + line_len && (body[p] == ' ' || body[p] == '\t')) ++p;
        if (p >= start + line_len || body[p] == '#') continue;

        // Split into up to four '|' separated fields.
        const char* fields[4] = {nullptr, nullptr, nullptr, nullptr};
        size_t lens[4] = {0, 0, 0, 0};
        int nf = 0;
        size_t fs = p;
        for (size_t k = p; k <= start + line_len && nf < 4; ++k) {
            if (k == start + line_len || body[k] == '|') {
                fields[nf] = body + fs;
                lens[nf] = k - fs;
                ++nf;
                fs = k + 1;
            }
        }
        if (nf < 2 || lens[0] == 0) continue;

        char name[24];
        size_t nl = lens[0] < sizeof(name) - 1 ? lens[0] : sizeof(name) - 1;
        kstd::strncpy(name, fields[0], nl);
        name[nl] = '\0';

        App* a = ensure(name);
        if (!a) break;

        size_t vl = lens[1] < sizeof(a->version) - 1 ? lens[1] : sizeof(a->version) - 1;
        kstd::strncpy(a->version, fields[1], vl);
        a->version[vl] = '\0';

        if (nf >= 3 && lens[2] > 0) {
            size_t pl = lens[2] < sizeof(a->path) - 1 ? lens[2] : sizeof(a->path) - 1;
            kstd::strncpy(a->path, fields[2], pl);
            a->path[pl] = '\0';
        }

        if (nf >= 4 && lens[3] > 0) {
            size_t dl = lens[3] < sizeof(a->desc) - 1 ? lens[3] : sizeof(a->desc) - 1;
            kstd::strncpy(a->desc, fields[3], dl);
            a->desc[dl] = '\0';
        }
        if (!a->desc[0]) kstd::strncpy(a->desc, "Available app", sizeof(a->desc) - 1);
    }

    delete[] buf;

    // Keep installed apps that are not in the server catalog visible.
    scan_installed();

    for (int k = 0; k < g_count; ++k) read_installed(g_apps[k]);

    char out[64];
    kstd::strcpy(out, "Catalog loaded - ");
    char num[8];
    kstd::utoa(static_cast<uint64_t>(g_count), num, 10);
    kstd::strcpy(out + kstd::strlen(out), num);
    kstd::strcpy(out + kstd::strlen(out), " apps");
    set_msg(msg, msg_len, out);
    return true;
}

bool install(int idx, char* msg, size_t msg_len) {
    if (!aetherfs::mounted()) {
        set_msg(msg, msg_len, "AetherFS is not mounted");
        return false;
    }
    const App* cp = get(idx);
    if (!cp) {
        set_msg(msg, msg_len, "No app selected");
        return false;
    }

    // Copy out the fields: writing the version file mutates the catalogue.
    char name[24];
    char version[16];
    char path[80];
    kstd::strncpy(name, cp->name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    kstd::strncpy(version, cp->version, sizeof(version) - 1);
    version[sizeof(version) - 1] = '\0';
    kstd::strncpy(path, cp->path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    char hf[48];
    char vf[48];
    html_name(name, hf, sizeof(hf));
    ver_name(name, vf, sizeof(vf));

    App* a = &g_apps[idx];
    read_installed(*a);
    if (a->installed && version[0] && !kstd::strcmp(a->installed_version, version)) {
        char out[96];
        size_t o = 0;
        out[0] = '\0';
        append(out, sizeof(out), o, "Already up to date - ");
        append(out, sizeof(out), o, name);
        append(out, sizeof(out), o, " ");
        append(out, sizeof(out), o, version);
        set_msg(msg, msg_len, out);
        return true;
    }

    if (!path[0]) {
        set_msg(msg, msg_len, "No download path for this app");
        return false;
    }

    char* buf = nullptr;
    char* body = nullptr;
    size_t len = 0;
    if (http_fetch(path, buf, body, len, APP_CAP) != 0) {
        set_msg(msg, msg_len, "App download failed");
        return false;
    }
    if (len == 0) {
        delete[] buf;
        set_msg(msg, msg_len, "Downloaded app is empty");
        return false;
    }

    if (!aetherfs::write_file(hf, body, len)) {
        delete[] buf;
        set_msg(msg, msg_len, "Could not write app to AetherFS");
        return false;
    }

    if (version[0]) {
        aetherfs::write_file(vf, version, kstd::strlen(version));
    }

    delete[] buf;
    read_installed(*a);

    char out[112];
    size_t o = 0;
    out[0] = '\0';
    append(out, sizeof(out), o, "Installed ");
    append(out, sizeof(out), o, name);
    if (version[0]) {
        append(out, sizeof(out), o, " ");
        append(out, sizeof(out), o, version);
    }
    set_msg(msg, msg_len, out);
    return true;
}

bool install_by_name(const char* name, char* msg, size_t msg_len) {
    int idx = find(name);
    if (idx < 0) {
        set_msg(msg, msg_len, "App not found in catalog");
        return false;
    }
    return install(idx, msg, msg_len);
}

bool update_all(char* msg, size_t msg_len) {
    int updated = 0;
    int failed = 0;
    int total = g_count;
    for (int i = 0; i < total; ++i) {
        App* a = &g_apps[i];
        read_installed(*a);
        if (a->installed && a->version[0] &&
            !kstd::strcmp(a->installed_version, a->version)) {
            continue;
        }
        char m[96];
        m[0] = '\0';
        if (install(i, m, sizeof(m))) ++updated;
        else ++failed;
    }

    char out[80];
    size_t o = 0;
    out[0] = '\0';
    append(out, sizeof(out), o, "Updated ");
    char num[8];
    kstd::utoa(static_cast<uint64_t>(updated), num, 10);
    append(out, sizeof(out), o, num);
    if (failed) {
        append(out, sizeof(out), o, ", failed ");
        kstd::utoa(static_cast<uint64_t>(failed), num, 10);
        append(out, sizeof(out), o, num);
    }
    set_msg(msg, msg_len, out);
    return failed == 0;
}

int app_size(const char* name) {
    char hf[48];
    html_name(name, hf, sizeof(hf));
    return aetherfs::file_size(hf);
}

int read_app(const char* name, void* buf, size_t max) {
    char hf[48];
    html_name(name, hf, sizeof(hf));
    return aetherfs::read_file(hf, buf, max);
}

} // namespace apphub
