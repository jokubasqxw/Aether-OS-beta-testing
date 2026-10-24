#!/usr/bin/env bash
# Build a bootable raw disk image (MBR + GRUB BIOS + FAT16 boot partition).
#
#   build/disk.img  ->  boots AetherOS from real storage (no -kernel live boot)
#
# Partition 1 (FAT16, active): GRUB + /AETHEROS.BIN (the boot kernel).
# Partition 2 (AetherFS): reserved for the OS to format on first boot.
#
# Requires root for loop devices (uses sudo).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMG="${1:-$ROOT/build/disk.img}"
KERNEL="$ROOT/build/aetheros32.bin"
SIZE_MB="${DISK_SIZE_MB:-128}"
BOOT_MB=64

if [[ ! -f "$KERNEL" ]]; then
    echo "error: $KERNEL not found (run 'make' first)" >&2
    exit 1
fi

echo "[disk] creating ${SIZE_MB}MB image: $IMG"
rm -f "$IMG"
truncate -s "${SIZE_MB}M" "$IMG"

# MBR with two partitions: p1 FAT32 boot (active), p2 AetherFS (raw).
parted -s "$IMG" mklabel msdos
parted -s "$IMG" mkpart primary fat32 1MiB "${BOOT_MB}MiB"
parted -s "$IMG" mkpart primary "${BOOT_MB}MiB" 100%
parted -s "$IMG" set 1 boot on

LOOP="$(sudo losetup --show -fP "$IMG")"
echo "[disk] loop device: $LOOP"
cleanup() {
    sudo umount "$MNT" 2>/dev/null || true
    sudo losetup -d "$LOOP" 2>/dev/null || true
}
trap cleanup EXIT

sudo mkfs.vfat -F 16 -n AETHERBOOT "${LOOP}p1" >/dev/null

MNT="$(mktemp -d)"
sudo mount "${LOOP}p1" "$MNT"
sudo mkdir -p "$MNT/boot/grub"

# The kernel lives at the FAT root as AETHEROS.BIN. The OS updater rewrites
# this file in place (the FAT16 writer extends the cluster chain as needed).
sudo cp "$KERNEL" "$MNT/AETHEROS.BIN"

sudo tee "$MNT/boot/grub/grub.cfg" >/dev/null <<'EOF'
set timeout=1
set default=0

menuentry "AetherOS 64-bit" {
    multiboot /AETHEROS.BIN
    boot
}
EOF

echo "[disk] installing GRUB (i386-pc)"
sudo grub-install --target=i386-pc --boot-directory="$MNT/boot" "$LOOP" >/dev/null 2>&1

sudo umount "$MNT"
rmdir "$MNT" 2>/dev/null || true
sudo losetup -d "$LOOP"
trap - EXIT

echo "[disk] done: $IMG"
