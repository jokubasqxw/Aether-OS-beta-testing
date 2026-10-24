#ifndef KERNEL_APPHUB_HPP
#define KERNEL_APPHUB_HPP

#include "types.hpp"

// Lightweight application distribution for AetherOS.
//
// Apps are self-contained HTML/JS bundles stored in AetherFS. They run inside
// the native browser engine and are updated independently of the kernel, so an
// app update never requires reflashing the boot image.
//
// Catalog format (served as text/plain from <update host>/apps/catalog.txt):
//     name|version|path|description
// One app per line; '#' starts a comment.
namespace apphub {

constexpr size_t MAX_APPS = 16;

struct App {
    char name[24];
    char version[16];
    char path[80];
    char desc[64];
    char installed_version[16];
    bool installed;
};

// Scan AetherFS for already-installed apps and populate the catalogue with
// them (so the hub is useful before the network catalogue is refreshed).
void init();

// Replace the catalogue with the one fetched from the update host.
// `msg` receives a human-readable status string.
bool refresh_catalog(char* msg, size_t msg_len);

int count();
const App* get(int idx);
int find(const char* name);
bool is_installed(const char* name);

// Download and install the selected catalogue entry. If the installed version
// already matches, no download is performed.
bool install(int idx, char* msg, size_t msg_len);
bool install_by_name(const char* name, char* msg, size_t msg_len);
bool update_all(char* msg, size_t msg_len);

// Read an installed app bundle (for app:// URLs in the browser).
int app_size(const char* name);
int read_app(const char* name, void* buf, size_t max);

} // namespace apphub

#endif // KERNEL_APPHUB_HPP
