#include "types.hpp"
#include "multiboot.hpp"
#include "serial.hpp"
#include "vga.hpp"
#include "printk.hpp"
#include "gdt.hpp"
#include "idt.hpp"
#include "pic.hpp"
#include "pit.hpp"
#include "keyboard.hpp"
#include "mouse.hpp"
#include "pmm.hpp"
#include "heap.hpp"
#include "ata.hpp"
#include "aetherfs.hpp"
#include "apphub.hpp"
#include "fat16.hpp"
#include "updater.hpp"
#include "ramfs.hpp"
#include "rtc.hpp"
#include "font_ttf.hpp"
#include "e1000.hpp"
#include "net.hpp"
#include "wifi.hpp"
#include "sound.hpp"
#include "pat.hpp"
#include "bga.hpp"
#include "vmware_svga.hpp"
#include "gfx_driver.hpp"
#include "shell.hpp"
#include "../arch/x86_64/io.hpp"

extern "C" uint8_t __kernel_end[];

// Enable SSE/SSE2 so the JavaScript engine (js.cpp, compiled with -msse2) can
// use hardware floating point. The rest of the kernel is built with -mno-sse
// and therefore never touches the XMM registers, so interrupt handlers do not
// need to save/restore them.
static void cpu_enable_sse() {
    uint32_t eax = 1, ebx = 0, ecx = 0, edx = 0;
    asm volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
    if (!(edx & (1u << 25))) return; // no SSE: leave FP disabled

    uint64_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2); // clear EM (no FPU emulation)
    cr0 &= ~(1ULL << 3); // clear TS (no lazy FPU switching)
    cr0 |=  (1ULL << 1); // set MP (monitor coprocessor)
    asm volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");

    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9);  // OSFXSR
    cr4 |= (1ULL << 10); // OSXMMEXCPT
    asm volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");

    uint32_t mxcsr = 0x1F80; // mask all SIMD exceptions
    asm volatile("ldmxcsr %0" :: "m"(mxcsr));
}

extern "C" void kernel_main(uint64_t magic, uint64_t mbi_addr) {
    // 1. Early serial port initialization
    serial::init();
    serial::write("\n[AetherOS] Booting 64-bit C++ Kernel...\n");

    // 2. VGA text console initialization
    vga::init();

    // 3. Early boot logging
    printk("[OK] Serial COM1 UART initialized at 38400 baud.\n");
    printk("[OK] VGA 80x25 Text console initialized.\n");

    // Check Multiboot bootloader magic
    if (magic != MULTIBOOT_BOOTLOADER_MAGIC) {
        printk("[WARN] Invalid Multiboot magic: 0x%x (expected 0x%x)\n", magic, MULTIBOOT_BOOTLOADER_MAGIC);
    } else {
        printk("[OK] Multiboot 1 standard verified (Magic: 0x%x).\n", magic);
    }

    // 4. Global Descriptor Table (GDT) & Task State Segment (TSS)
    gdt::init();
    printk("[OK] 64-bit GDT and TSS initialized.\n");

    // 4b. Enable SSE registers for the JavaScript engine (hardware FP).
    cpu_enable_sse();
    printk("[OK] SSE/SSE2 enabled for hardware floating point.\n");

    // 5. Interrupt Descriptor Table (IDT)
    idt::init();
    printk("[OK] 64-bit IDT initialized (256 gates configured).\n");

    // 6. 8259 Programmable Interrupt Controller (PIC)
    pic::init();
    printk("[OK] 8259 PIC remapped (Master: 0x20-0x27, Slave: 0x28-0x2F).\n");

    // 7. 8254 Programmable Interval Timer (PIT)
    pit::init(1000); // 1000 Hz = 1ms per tick (high refresh rate support)
    printk("[OK] 8254 PIT initialized at 1000 Hz (1ms resolution).\n");

    // 8. PS/2 Keyboard & Mouse Drivers
    keyboard::init();
    printk("[OK] PS/2 Keyboard driver active on IRQ1.\n");
    mouse::init();
    printk("[OK] PS/2 Mouse driver active on IRQ12.\n");

    // 9. Enable CPU Interrupts
    arch::sti();
    printk("[OK] CPU Hardware Interrupts enabled (STI).\n");

    // 10. Physical Memory Manager (PMM)
    multiboot_info* mbi = reinterpret_cast<multiboot_info*>(mbi_addr);
    pmm::init(mbi);
    printk("[OK] Physical Memory Manager active (%u MB usable RAM).\n", pmm::get_total_memory() / (1024 * 1024));

    // 11. Kernel Dynamic Heap Allocator
    // Position the heap after kernel_end rounded up to a 2MB boundary and size
    // it from the amount of RAM the firmware actually reported, so the OS
    // automatically scales its program memory with the installed RAM instead
    // of being capped at a fixed 64MB.
    uintptr_t heap_start = (reinterpret_cast<uintptr_t>(__kernel_end) + 0x1FFFFF) & ~0x1FFFFFULL;
    size_t total_ram = pmm::get_total_memory();
    // Grow the heap into the contiguous, identity-mapped RAM region that follows
    // the kernel image. This is what makes a machine with 4 GB usable (its RAM
    // below the PCI hole), rather than assuming all installed RAM is contiguous.
    uintptr_t region_end = pmm::get_heap_region_end();
    size_t heap_avail = region_end > heap_start ? region_end - heap_start : 0;

    constexpr size_t PMM_RESERVE = 48 * 1024 * 1024; // keep frames for drivers
    constexpr size_t HEAP_MAX    = static_cast<size_t>(4096) * 1024 * 1024; // 4 GB ceiling
    constexpr size_t MIN_HEAP    = 64 * 1024 * 1024;

    size_t HEAP_SIZE;
    if (heap_avail > PMM_RESERVE + MIN_HEAP) {
        HEAP_SIZE = heap_avail - PMM_RESERVE;
    } else if (heap_avail > MIN_HEAP) {
        HEAP_SIZE = heap_avail - (heap_avail / 4);
    } else {
        HEAP_SIZE = MIN_HEAP; // fallback when RAM detection is unavailable
    }
    if (HEAP_SIZE > HEAP_MAX) HEAP_SIZE = HEAP_MAX;
    HEAP_SIZE &= ~static_cast<size_t>(0x1FFFFF); // round down to 2MB
    if (HEAP_SIZE < 16 * 1024 * 1024) HEAP_SIZE = 16 * 1024 * 1024;

    heap::init(heap_start, HEAP_SIZE);
    pmm::reserve_region(heap_start, HEAP_SIZE);
    printk("[OK] Kernel Heap Allocator initialized at %p (Size: %u MB of %u MB RAM).\n",
           heap_start,
           static_cast<unsigned>(HEAP_SIZE / (1024 * 1024)),
           static_cast<unsigned>(total_ram / (1024 * 1024)));

    // 11b. ATA/IDE block storage (persistent disk)
    ata::init();
    if (ata::count() > 0) {
        const ata::Drive& d0 = ata::drive(0);
        printk("[OK] ATA storage online (%d drive(s), %u MB on drive 0).\n",
               ata::count(),
               static_cast<unsigned>(d0.sectors * ata::SECTOR_SIZE / (1024 * 1024)));
    } else {
        printk("[WARN] No ATA storage detected.\n");
    }

    // 11c. FAT16 boot partition (holds the kernel image the bootloader loads)
    if (ata::count() > 0) {
        fat16::init();
    }

    // 11d. Persistent AetherFS (mount, or format on first boot)
    if (ata::count() > 0) {
        if (aetherfs::init()) {
            // Crash-safe updates: if the lock is still set, the previous update
            // was interrupted, so roll back to the pre-update snapshot.
            if (aetherfs::update_lock_engaged()) {
                printk("[update] Interrupted update detected - rolling back to snapshot...\n");
                if (aetherfs::restore()) {
                    if (updater::restore_kernel_backup())
                        printk("[update] Kernel restored from backup.\n");
                    else
                        printk("[update] WARN: kernel backup unavailable.\n");
                    aetherfs::set_update_lock(false);
                }
            }
            updater::load_config();
            apphub::init();
        }
    }

    // 12. Quick C++ dynamic memory sanity test
    int* test_alloc = new int(1337);
    if (test_alloc && *test_alloc == 1337) {
        printk("[OK] C++ operator new/delete verified successfully.\n");
        delete test_alloc;
    }

    // 13. Virtual In-Memory RAM Filesystem (RamFS)
    ramfs::init();
    printk("[OK] Virtual RAM Filesystem (RamFS) mounted with default files.\n");

    // 14. Real-Time Clock (RTC)
    rtc::init();
    printk("[OK] CMOS Real-Time Clock active.\n");

    // 15. Intel e1000 Network Driver, Protocol Stack & Wi-Fi Subsystem
    if (e1000::init()) {
        net::init();
        wifi::init();
        printk("[OK] Intel e1000 PCI Gigabit Network Subsystem online.\n");
    } else {
        wifi::init();
        printk("[WARN] Ethernet offline (no supported NIC detected). Wi-Fi active.\n");
    }

    // 16. Audio & Sound Driver Subsystem
    sound::init();
    printk("[OK] Sound Subsystem active (%s).\n", sound::get_driver_name());

    // 17. Page Attribute Table (PAT) & Unified Hardware Graphics Subsystem
    if (pat::init()) {
        gfx_driver::DriverType best_gfx = gfx_driver::auto_detect();
        uintptr_t fb_phys = 0;
        const char* gfx_name = "Unknown Display Adapter";

        if (best_gfx == gfx_driver::DriverType::VMwareSVGA) {
            gfx_name = "VMware SVGA-II Hardware 2D Accelerator";
            pci::Device svga_dev;
            if (pci::find_device(vmware_svga::PCI_VENDOR_VMWARE, vmware_svga::PCI_DEVICE_SVGA2, &svga_dev)) {
                fb_phys = pci::get_bar(svga_dev, 1) & ~0xF;
            }
        } else if (best_gfx == gfx_driver::DriverType::BochsVBE) {
            gfx_name = "Bochs/QEMU VBE with hardware panning and software backbuffer";
            fb_phys = bga::get_framebuffer_addr();
        }

        if (fb_phys != 0) {
            constexpr size_t FB_SIZE = 32 * 1024 * 1024; // 32 MB VRAM mapping
            pat::set_range_wc(fb_phys, FB_SIZE);
            printk("[OK] IA32_PAT active (Write-Combining enabled for Framebuffer at %p, 32 MB).\n", fb_phys);
            printk("[OK] Graphics Hardware Driver: %s\n", gfx_name);
        } else {
            printk("[OK] IA32_PAT active (Default caching modes initialized).\n");
        }
    } else {
        printk("[WARN] IA32_PAT subsystem not available on this CPU.\n");
    }

    // Initialize TrueType font system
    if (font::ttf::init()) {
        printk("[OK] TrueType font system initialized (Vera Sans).\n");
    } else {
        printk("[WARN] TrueType font init failed, using bitmap fallback.\n");
    }

    pit::sleep(300); // Brief pause to view boot logs

    // 13. Clear screen and display interactive AetherOS shell
    vga::clear();
    shell::print_banner();
    shell::run();

    // In case shell terminates, halt gracefully
    arch::cli();
    while (true) {
        arch::hlt();
    }
}
