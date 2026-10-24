#ifndef KERNEL_UPDATER_HPP
#define KERNEL_UPDATER_HPP

#include "types.hpp"

namespace updater {

// Update source. Defaults to the QEMU SLIRP host so the flow can be tested
// with a local web server; override with `update set <host> <port> <path>`.
struct Config {
    char host[64];
    uint16_t port;
    char path[96];
};

void load_config();
const Config& config();
void set_config(const char* host, uint16_t port, const char* path);

enum class Result {
    Ok,          // a newer kernel is available (fetch/apply)
    UpToDate,    // the remote kernel is identical to the installed one
    NoDisk,
    ResolveFailed,
    DownloadFailed,
    BadImage,
    BackupFailed,
    WriteFailed,
};

const char* result_name(Result r);

// Full update: download the new kernel, back up the current one to AetherFS,
// snapshot, engage the update lock, rewrite the boot kernel, release the lock.
// `stage` receives a human-readable progress string.
Result apply(char* message, size_t message_len);

// Downloads and validates the image without installing it.
Result fetch_verify(char* message, size_t message_len);

// Restore the kernel on the FAT boot partition from the AetherFS backup.
// Used during boot rollback after an interrupted update.
bool restore_kernel_backup();

} // namespace updater

#endif // KERNEL_UPDATER_HPP
