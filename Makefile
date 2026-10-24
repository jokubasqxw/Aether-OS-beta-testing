CXX = g++
CXXFLAGS = -m64 -std=c++20 -ffreestanding -nostdlib -fno-builtin \
           -fno-exceptions -fno-rtti -fno-stack-protector -mno-red-zone \
           -mno-mmx -mno-sse -mno-sse2 -Wall -Wextra -O2 \
           -fno-unwind-tables -fno-asynchronous-unwind-tables -Isrc

CC = gcc
BEARSSL_CFLAGS = -m64 -std=c99 -ffreestanding -nostdlib -fno-builtin \
                 -fno-stack-protector -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
                 -fno-asynchronous-unwind-tables -fno-unwind-tables -Os \
                 -DBR_AES_X86NI=0 -DBR_SSE2=0 -DBR_RDRAND=0 \
                 -DBR_USE_URANDOM=0 -DBR_USE_GETENTROPY=0 -DBR_USE_UNIX_TIME=0 \
                 -Isrc/kernel/bearssl/shim -Isrc/kernel/bearssl/inc -Isrc/kernel/bearssl/src

AS = nasm
ASFLAGS = -f elf64

LD = ld
LDFLAGS = -m elf_x86_64 -T linker.ld -z max-page-size=0x1000 --gc-sections


STRIP = strip --strip-all

BUILD_DIR = build
ISO_DIR = iso

# Source files
ASM_SRCS = src/arch/x86_64/boot.asm \
           src/arch/x86_64/isr.asm

CPP_SRCS = src/kernel/main.cpp \
           src/kernel/kstring.cpp \
           src/kernel/serial.cpp \
           src/kernel/vga.cpp \
           src/kernel/printk.cpp \
           src/kernel/gdt.cpp \
           src/kernel/idt.cpp \
           src/kernel/pic.cpp \
           src/kernel/pit.cpp \
           src/kernel/keyboard.cpp \
           src/kernel/mouse.cpp \
           src/kernel/pmm.cpp \
           src/kernel/ata.cpp \
           src/kernel/part.cpp \
           src/kernel/fat16.cpp \
           src/kernel/updater.cpp \
           src/kernel/aetherfs.cpp \
           src/kernel/apphub.cpp \
           src/kernel/heap.cpp \
           src/kernel/rtc.cpp \
           src/kernel/pci.cpp \
           src/kernel/speaker.cpp \
           src/kernel/ramfs.cpp \
           src/kernel/bga.cpp \
           src/kernel/vmware_svga.cpp \
           src/kernel/gfx_driver.cpp \
           src/kernel/font.cpp \
           src/kernel/gfx.cpp \
           src/kernel/gui.cpp \
           src/kernel/ac97.cpp \
           src/kernel/sb16.cpp \
           src/kernel/sound.cpp \
           src/kernel/e1000.cpp \
           src/kernel/net.cpp \
           src/kernel/wifi.cpp \
           src/kernel/browser_engine.cpp \
           src/kernel/js.cpp \
           src/kernel/crypto/aes/aes.cpp \
           src/kernel/crypto/sha/sha256.cpp \
           src/kernel/crypto/uecc/uecc.cpp \
           src/kernel/crypto/crypto.cpp \
           src/kernel/tls.cpp \
           src/kernel/inflate.cpp \
           src/kernel/pat.cpp \
           src/kernel/shell.cpp

BEARSSL_SRCS = $(shell find src/kernel/bearssl/src -name '*.c' \
                 ! -name 'aes_x86ni*.c' ! -name 'aes_pwr8*.c' \
                 ! -name 'ghash_pclmul.c' ! -name 'ghash_pwr8.c' \
                 ! -name 'chacha20_sse2.c')
C_SRCS = $(BEARSSL_SRCS) src/kernel/bearssl/trust_anchors.c

# Object files
ASM_OBJS = $(patsubst src/%.asm, $(BUILD_DIR)/%.o, $(ASM_SRCS))
CPP_OBJS = $(patsubst src/%.cpp, $(BUILD_DIR)/%.o, $(CPP_SRCS))
C_OBJS = $(patsubst src/%.c, $(BUILD_DIR)/%.o, $(C_SRCS))
ALL_OBJS = $(ASM_OBJS) $(CPP_OBJS) $(C_OBJS)

KERNEL_ELF = $(BUILD_DIR)/aetheros.bin
KERNEL_ELF32 = $(BUILD_DIR)/aetheros32.bin
ISO_IMAGE = $(BUILD_DIR)/aetheros.iso

.PHONY: all clean run run-bga run-vmware run-iso run-vmware-iso run-curses run-nographic

all: $(KERNEL_ELF) $(KERNEL_ELF32) $(ISO_IMAGE)
	@echo "[UPDATE] Build complete. Playing notification..."
	@bash ./ding.sh

$(KERNEL_ELF): $(ALL_OBJS) linker.ld
	@mkdir -p $(dir $@)
	@echo "[LD] Linking 64-bit Kernel: $@"
	$(LD) $(LDFLAGS) $(ALL_OBJS) -o $@
	@echo "[STRIP] Stripping symbols: $@"
	$(STRIP) $@

$(KERNEL_ELF32): $(KERNEL_ELF)
	@echo "[OBJCOPY] Generating 32-bit Multiboot ELF: $@"
	objcopy -O elf32-i386 $< $@

$(ISO_IMAGE): $(KERNEL_ELF) iso/boot/grub/grub.cfg
	@echo "[GRUB] Creating bootable ISO: $@"
	@mkdir -p $(ISO_DIR)/boot/grub
	@cp $(KERNEL_ELF) $(ISO_DIR)/boot/
	@grub-mkrescue -o $@ $(ISO_DIR) 2>/dev/null || echo "[WARN] grub-mkrescue skipped (xorriso/grub package may need mtools)"

$(BUILD_DIR)/%.o: src/%.asm
	@mkdir -p $(dir $@)
	@echo "[AS] $<"
	$(AS) $(ASFLAGS) $< -o $@

# The JavaScript engine uses hardware floating point; enable SSE2 for this TU
# only (the CPU is put into SSE mode early in kernel_main).
$(BUILD_DIR)/kernel/tls.o: src/kernel/tls.cpp
	@mkdir -p $(dir $@)
	@echo "[CXX] $<"
	$(CXX) $(CXXFLAGS) -Isrc/kernel/bearssl/shim -Isrc/kernel/bearssl/inc -c $< -o $@

$(BUILD_DIR)/kernel/js.o: src/kernel/js.cpp
	@mkdir -p $(dir $@)
	@echo "[CXX] $< (+sse2)"
	$(CXX) $(CXXFLAGS) -msse2 -c $< -o $@

$(BUILD_DIR)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	@echo "[CXX] $<"
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	@echo "[CC] $<"
	$(CC) $(BEARSSL_CFLAGS) -c $< -o $@

clean:
	@rm -rf $(BUILD_DIR) $(ISO_DIR)/boot/aetheros.bin
	@echo "[CLEAN] Build directory cleaned."

QEMU_AUDIO ?= -audiodev pa,id=snd0 -device AC97,audiodev=snd0
QEMU_VGA ?= -vga std -global VGA.vgamem_mb=32
QEMU_MEM ?= 4096M

run: $(KERNEL_ELF32)
	qemu-system-x86_64 -kernel $(KERNEL_ELF32) -serial stdio -m $(QEMU_MEM) $(QEMU_VGA) -netdev user,id=net0 -device e1000,netdev=net0 $(QEMU_AUDIO)

run-bga: $(KERNEL_ELF32)
	qemu-system-x86_64 -kernel $(KERNEL_ELF32) -serial stdio -m $(QEMU_MEM) -vga std -global VGA.vgamem_mb=32 -netdev user,id=net0 -device e1000,netdev=net0 $(QEMU_AUDIO)

run-vmware: $(KERNEL_ELF32)
	qemu-system-x86_64 -kernel $(KERNEL_ELF32) -serial stdio -m $(QEMU_MEM) -vga vmware -netdev user,id=net0 -device e1000,netdev=net0 $(QEMU_AUDIO)

run-1080p: $(KERNEL_ELF32)
	qemu-system-x86_64 -kernel $(KERNEL_ELF32) -serial stdio -m $(QEMU_MEM) $(QEMU_VGA) -netdev user,id=net0 -device e1000,netdev=net0 $(QEMU_AUDIO)

run-fullscreen: $(KERNEL_ELF32)
	qemu-system-x86_64 -kernel $(KERNEL_ELF32) -serial stdio -m $(QEMU_MEM) $(QEMU_VGA) -full-screen -netdev user,id=net0 -device e1000,netdev=net0 $(QEMU_AUDIO)

run-iso: $(ISO_IMAGE)
	qemu-system-x86_64 -cdrom $(ISO_IMAGE) -serial stdio -m $(QEMU_MEM) $(QEMU_VGA) -netdev user,id=net0 -device e1000,netdev=net0 $(QEMU_AUDIO)

run-vmware-iso: $(ISO_IMAGE)
	qemu-system-x86_64 -cdrom $(ISO_IMAGE) -serial stdio -m $(QEMU_MEM) -vga vmware -netdev user,id=net0 -device e1000,netdev=net0 $(QEMU_AUDIO)

run-curses: $(KERNEL_ELF32)
	qemu-system-x86_64 -kernel $(KERNEL_ELF32) -serial stdio -display curses -m $(QEMU_MEM) -netdev user,id=net0 -device e1000,netdev=net0 $(QEMU_AUDIO)

run-nographic: $(KERNEL_ELF32)
	qemu-system-x86_64 -kernel $(KERNEL_ELF32) -nographic -m $(QEMU_MEM) -netdev user,id=net0 -device e1000,netdev=net0 $(QEMU_AUDIO)
