#include "shell.hpp"
#include "vga.hpp"
#include "serial.hpp"
#include "printk.hpp"
#include "keyboard.hpp"
#include "pit.hpp"
#include "pmm.hpp"
#include "heap.hpp"
#include "ata.hpp"
#include "aetherfs.hpp"
#include "fat16.hpp"
#include "updater.hpp"
#include "apphub.hpp"
#include "kstring.hpp"
#include "rtc.hpp"
#include "pci.hpp"
#include "speaker.hpp"
#include "ramfs.hpp"
#include "bga.hpp"
#include "vmware_svga.hpp"
#include "gfx_driver.hpp"
#include "gfx.hpp"
#include "gui.hpp"
#include "browser_engine.hpp"
#include "e1000.hpp"
#include "net.hpp"
#include "wifi.hpp"
#include "sound.hpp"
#include "ac97.hpp"
#include "sb16.hpp"
#include "pat.hpp"
#include "js.hpp"
#include "../arch/x86_64/io.hpp"

namespace shell {

// Command History
constexpr size_t HISTORY_MAX = 32;
static char command_history[HISTORY_MAX][128];
static size_t history_count = 0;

static void add_history(const char* line) {
    if (!line || *line == '\0') return;
    if (history_count < HISTORY_MAX) {
        kstd::strncpy(command_history[history_count], line, 127);
        command_history[history_count][127] = '\0';
        history_count++;
    } else {
        for (size_t i = 0; i < HISTORY_MAX - 1; ++i) {
            kstd::strcpy(command_history[i], command_history[i + 1]);
        }
        kstd::strncpy(command_history[HISTORY_MAX - 1], line, 127);
        command_history[HISTORY_MAX - 1][127] = '\0';
    }
}

static void cmd_history() {
    printk("=== Command History ===\n");
    if (history_count == 0) {
        printk("  (No commands recorded yet)\n");
        return;
    }
    for (size_t i = 0; i < history_count; ++i) {
        printk("  %u: %s\n", i + 1, command_history[i]);
    }
}

static void get_cpuid(uint32_t leaf, uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx) {
    asm volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf)
    );
}

static void print_cpu_info() {
    uint32_t eax, ebx, ecx, edx;
    char vendor[13];

    get_cpuid(0, &eax, &ebx, &ecx, &edx);
    *reinterpret_cast<uint32_t*>(vendor + 0) = ebx;
    *reinterpret_cast<uint32_t*>(vendor + 4) = edx;
    *reinterpret_cast<uint32_t*>(vendor + 8) = ecx;
    vendor[12] = '\0';

    printk(" CPU Vendor: %s\n", vendor);

    // Check brand string support
    get_cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
    if (eax >= 0x80000004) {
        char brand[49];
        uint32_t* brand_u32 = reinterpret_cast<uint32_t*>(brand);
        for (uint32_t i = 0; i < 3; ++i) {
            get_cpuid(0x80000002 + i, &eax, &ebx, &ecx, &edx);
            brand_u32[i * 4 + 0] = eax;
            brand_u32[i * 4 + 1] = ebx;
            brand_u32[i * 4 + 2] = ecx;
            brand_u32[i * 4 + 3] = edx;
        }
        brand[48] = '\0';
        char* bptr = brand;
        while (*bptr == ' ') bptr++;
        printk(" CPU Model : %s\n", bptr);
    }
}

void print_banner() {
    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk("     ___      _   _               ____  _  _   \n");
    printk("    / _ \\    | | | |             / ___|| || |  \n");
    printk("   / /_\\ \\___| |_| |__   ___ _ _/ /___ | || |_ \n");
    printk("   |  _  / _ \\ __| '_ \\ / _ \\ '__| ___||__   _|\n");
    printk("   | | | | __/ |_| | | |  __/ | / /___    | |  \n");
    printk("   \\_| |_/\\___|\\__|_| |_|\\___|_| \\____|   |_|  \n\n");

    vga::set_color(vga::Color::LightGreen, vga::Color::Black);
    printk("   AetherOS 64-bit Modern Operating System [v1.0]\n");
    vga::set_color(vga::Color::DarkGrey, vga::Color::Black);
    printk("   Freestanding C++20 | AMD64 Long Mode | SMP Ready\n");
    printk("--------------------------------------------------------------------------------\n");
    vga::set_color(vga::Color::White, vga::Color::Black);
}

// Builtin Commands
static void cmd_help() {
    vga::set_color(vga::Color::Yellow, vga::Color::Black);
    printk("=== Available AetherOS Commands ===\n");
    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk("System & Hardware:\n");
    vga::set_color(vga::Color::White, vga::Color::Black);
    printk("  info                     System architecture & CPU specifications\n");
    printk("  fetch                    Fastfetch / Neofetch system overview & palette\n");
    printk("  mem                      Physical RAM & Kernel Heap memory statistics\n");
    printk("  uptime                   Time elapsed since boot\n");
    printk("  date / rtc               Real-Time Clock date and time from CMOS\n");
    printk("  lspci                    Scan and list PCI hardware bus devices\n");
    printk("  features                 Check CPU feature flags (SSE, AVX, APIC, etc.)\n");
    printk("  bench                    Run CPU and memory performance benchmark\n");
    printk("  pat [status|bench|wc..]  Page Attribute Table (PAT) & Write-Combining cache mode\n");

    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk("Filesystem (RamFS):\n");
    vga::set_color(vga::Color::White, vga::Color::Black);
    printk("  ls                       List files in the RAM filesystem\n");
    printk("  cat <file>               Display file contents\n");
    printk("  touch <file>             Create a new empty file\n");
    printk("  write <file> <text...>   Write or replace text in file\n");
    printk("  rm <file>                Delete a file from ramfs\n");

    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk("Graphics & GUI:\n");
    vga::set_color(vga::Color::White, vga::Color::Black);
    printk("  gpu                      Display graphics hardware driver, 2D FIFO & page-flip status\n");
    printk("  gui [w] [h] [fps]        Launch Desktop GUI (supports 1920x1080 Full HD, 144Hz, 240Hz...)\n");
    printk("  browser [url]            Launch Native Web Browser inside Desktop GUI\n");
    printk("  browser dump <url>       Load a page and print its laid-out text (no GUI)\n");
    printk("  browser links <url>      Show resolved absolute targets of all links\n");
    printk("  browser boxes <url>      Print laid-out box geometry (layout debugging)\n");
    printk("  web [url]                Alias for browser\n");
    printk("  demo [w] [h]             Display 2D graphics engine test pattern\n");

    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk("Network & Internet:\n");
    vga::set_color(vga::Color::White, vga::Color::Black);
    printk("  ifconfig / ip            Show network interface IP, MAC, status & stats\n");
    printk("  wifi [scan|connect..]    Wi-Fi 802.11 scanning, connection & auto-Ethernet\n");
    printk("  wlan                     Alias for wifi\n");
    printk("  route                    Display kernel IP routing table (Ethernet/Wi-Fi metrics)\n");
    printk("  ping <ip|host> [count]   Send ICMP echo requests and measure round-trip time\n");
    printk("  arp                      Display Address Resolution Protocol cache table\n");
    printk("  netstat                  Show interface statistics & protocol counters\n");
    printk("  curl <ip> [port] [path]  HTTP client: Send GET request and print response\n");
    printk("  http <ip> [port] [path]  Alias for curl\n");

    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk("Storage & Apps:\n");
    vga::set_color(vga::Color::White, vga::Color::Black);
    printk("  disk                     ATA drives and partition layout\n");
    printk("  afs [ls|cat|snap|..]     AetherFS persistent filesystem tools\n");
    printk("  fat [verify|reflash]     FAT16 boot partition tools\n");
    printk("  update [check|set ..]    Update the kernel image\n");
    printk("  apps [catalog|install|update]  App Hub: install apps without touching the kernel\n");

    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk("Audio & Sound Subsystem:\n");
    vga::set_color(vga::Color::White, vga::Color::Black);
    printk("  sound [cues on|off]      Display audio hardware status (AC97/SB16/Speaker) or toggle cues\n");
    printk("  volume <0-100>           Set master output volume (0-100%%)\n");
    printk("  mute / unmute            Mute or unmute audio output\n");
    printk("  play <effect>            Play procedural sound effect (beep, chime, laser, coin, fanfare...)\n");
    printk("  wavplay <file.wav>       Play 8-bit or 16-bit PCM WAV audio file from RamFS\n");

    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk("Utilities & Fun:\n");
    vga::set_color(vga::Color::White, vga::Color::Black);
    printk("  echo [text...]           Print arguments to console\n");
    printk("  calc <n1> <op> <n2>      Basic math calculator (+, -, *, /, %%)\n");
    printk("  rand [min] [max]         Generate pseudo-random integer\n");
    printk("  js <javascript...>       Evaluate JavaScript with the AetherScript engine\n");
    printk("  color <fg> [bg]          Change console text and background color\n");
    printk("  memdump <hex_addr> [n]   Hex dump memory at given address\n");
    printk("  history                  Display command history\n");
    printk("  beep [freq] [ms]         Play tone on PC speaker\n");
    printk("  tune                     Play retro 8-bit melody on PC speaker\n");
    printk("  matrix                   Falling green digital rain animation\n");
    printk("  snake                    Play classic Snake arcade game\n");
    printk("  panic <div|page|gp>      Trigger CPU exception to test IDT crash screen\n");
    printk("  clear                    Clear the terminal screen\n");
    printk("  reboot                   Restart system\n");
    printk("  shutdown                 Power off computer / QEMU\n");
}

static void cmd_info() {
    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk("--- System Information ---\n");
    vga::set_color(vga::Color::White, vga::Color::Black);
    printk(" OS Name     : AetherOS 64-bit\n");
    printk(" Kernel Arch : x86_64 (AMD64 Long Mode)\n");
    printk(" Language    : Modern C++20 (Freestanding, no-exceptions, no-rtti)\n");
    printk(" Paging      : 4-Level Paging (PML4, 2MB Huge Pages identity-mapped)\n");
    print_cpu_info();
    uint64_t sec = pit::uptime_seconds();
    printk(" Uptime      : %u seconds (%u ms)\n", sec, pit::uptime_ms());
}

static void cmd_fetch() {
    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk("   ___    ");
    vga::set_color(vga::Color::LightGreen, vga::Color::Black);
    printk("aether");
    vga::set_color(vga::Color::White, vga::Color::Black);
    printk("@");
    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk("x86_64\n");

    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk("  / _ \\   ");
    vga::set_color(vga::Color::DarkGrey, vga::Color::Black);
    printk("------------------------------------\n");

    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk(" / /_\\ \\  ");
    vga::set_color(vga::Color::Yellow, vga::Color::Black);
    printk("OS:      ");
    vga::set_color(vga::Color::White, vga::Color::Black);
    printk("AetherOS 64-bit (Beta)\n");

    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk(" |  _  /  ");
    vga::set_color(vga::Color::Yellow, vga::Color::Black);
    printk("Kernel:  ");
    vga::set_color(vga::Color::White, vga::Color::Black);
    printk("x86_64 Long Mode Freestanding C++20\n");

    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk(" | | | |  ");
    vga::set_color(vga::Color::Yellow, vga::Color::Black);
    printk("Uptime:  ");
    vga::set_color(vga::Color::White, vga::Color::Black);
    uint64_t s = pit::uptime_seconds();
    printk("%um %us\n", s / 60, s % 60);

    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk(" \\_| |_/  ");
    vga::set_color(vga::Color::Yellow, vga::Color::Black);
    printk("Memory:  ");
    vga::set_color(vga::Color::White, vga::Color::Black);
    size_t used_mb = pmm::get_used_memory() / (1024 * 1024);
    size_t total_mb = pmm::get_total_memory() / (1024 * 1024);
    printk("%u MB / %u MB (%u%%)\n", used_mb, total_mb, total_mb ? (used_mb * 100 / total_mb) : 0);

    printk("          ");
    vga::set_color(vga::Color::Yellow, vga::Color::Black);
    printk("Heap:    ");
    vga::set_color(vga::Color::White, vga::Color::Black);
    printk("%u KB Used / %u KB Free\n", heap::get_used_bytes() / 1024, heap::get_free_bytes() / 1024);

    printk("          ");
    vga::set_color(vga::Color::Yellow, vga::Color::Black);
    printk("Storage: ");
    vga::set_color(vga::Color::White, vga::Color::Black);
    printk("RamFS (In-Memory VFS)\n");

    printk("          ");
    vga::set_color(vga::Color::Yellow, vga::Color::Black);
    printk("Palette: ");
    for (uint8_t c = 0; c < 8; ++c) {
        vga::set_color_raw(vga::make_color(static_cast<vga::Color>(c), static_cast<vga::Color>(c)));
        printk("  ");
    }
    printk("\n                   ");
    for (uint8_t c = 8; c < 16; ++c) {
        vga::set_color_raw(vga::make_color(static_cast<vga::Color>(c), static_cast<vga::Color>(c)));
        printk("  ");
    }
    vga::set_color(vga::Color::White, vga::Color::Black);
    printk("\n\n");
}

static void cmd_features() {
    uint32_t eax, ebx, ecx, edx;
    get_cpuid(1, &eax, &ebx, &ecx, &edx);

    printk("=== CPU Feature Capabilities ===\n");
    auto check_feat = [](const char* name, bool supported) {
        if (supported) {
            vga::set_color(vga::Color::LightGreen, vga::Color::Black);
            printk(" [+] %s", name);
        } else {
            vga::set_color(vga::Color::DarkGrey, vga::Color::Black);
            printk(" [-] %s", name);
        }
        size_t len = kstd::strlen(name);
        for (size_t s = len; s < 10; ++s) printk(" ");
        vga::set_color(vga::Color::White, vga::Color::Black);
    };

    check_feat("FPU", edx & (1 << 0));
    check_feat("TSC", edx & (1 << 4));
    check_feat("MSR", edx & (1 << 5));
    check_feat("PAE", edx & (1 << 6));
    printk("\n");
    check_feat("APIC", edx & (1 << 9));
    check_feat("MTRR", edx & (1 << 12));
    check_feat("PAT", edx & (1 << 16));
    check_feat("CMOV", edx & (1 << 15));
    printk("\n");
    check_feat("MMX", edx & (1 << 23));
    check_feat("FXSR", edx & (1 << 24));
    check_feat("SSE", edx & (1 << 25));
    check_feat("SSE2", edx & (1 << 26));
    check_feat("SSE3", ecx & (1 << 0));
    printk("\n");
    check_feat("SSSE3", ecx & (1 << 9));
    check_feat("SSE4.1", ecx & (1 << 19));
    check_feat("SSE4.2", ecx & (1 << 20));
    check_feat("AVX", ecx & (1 << 28));
    printk("\n");
    check_feat("VMX", ecx & (1 << 5));
    check_feat("SMX", ecx & (1 << 6));
    check_feat("RDRAND", ecx & (1 << 30));
    check_feat("x2APIC", ecx & (1 << 21));
    printk("\n");

    // Long mode check via extended leaf
    get_cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
    check_feat("LM(64-bit)", edx & (1 << 29));
    check_feat("NX/XD", edx & (1 << 20));
    check_feat("1GB-Pages", edx & (1 << 26));
    check_feat("SYSCALL", edx & (1 << 11));
    printk("\n");
}

static void cmd_bench() {
    printk("=== Running System Benchmark ===\n");

    // 1. CPU Arithmetic Benchmark
    printk(" 1. Benchmarking CPU 64-bit integer ALU (25,000,000 operations)...\n");
    uint64_t start_ms = pit::uptime_ms();
    volatile uint64_t acc = 0;
    for (uint64_t i = 1; i <= 25000000; ++i) {
        acc += (i ^ (i >> 3)) + (i * 3);
    }
    (void)acc;
    uint64_t duration_ms = pit::uptime_ms() - start_ms;
    if (duration_ms == 0) duration_ms = 1;
    uint64_t mops = (25000000ULL * 1000) / (duration_ms * 1000000);
    printk("    Time: %u ms | Result: %u Mops/sec\n", duration_ms, mops);

    // 2. RAM Throughput Benchmark
    constexpr size_t BUFFER_SIZE = 1 * 1024 * 1024; // 1 MB buffer
    constexpr size_t ITERATIONS = 32;               // 32 MB transferred
    void* buffer = kmalloc(BUFFER_SIZE);

    if (buffer) {
        printk(" 2. Benchmarking RAM Memory Bandwidth (32 MB read/write)...\n");
        start_ms = pit::uptime_ms();
        uint64_t* ptr = static_cast<uint64_t*>(buffer);
        size_t qwords = BUFFER_SIZE / sizeof(uint64_t);

        for (size_t iter = 0; iter < ITERATIONS; ++iter) {
            for (size_t j = 0; j < qwords; ++j) {
                ptr[j] = j ^ iter;
            }
        }

        duration_ms = pit::uptime_ms() - start_ms;
        if (duration_ms == 0) duration_ms = 1;
        uint64_t mb_per_sec = (ITERATIONS * 1000) / duration_ms;
        printk("    Time: %u ms | Throughput: %u MB/s\n", duration_ms, mb_per_sec);
        kfree(buffer);
    } else {
        printk(" 2. Skipped memory benchmark (could not allocate 1MB buffer).\n");
    }

    printk("Benchmark completed.\n");
}

static uintptr_t get_active_fb_addr() {
    uintptr_t fb = gfx_driver::get_driver_info().fb_phys;
    if (fb == 0) {
        if (vmware_svga::is_available()) {
            pci::Device d;
            if (pci::find_device(vmware_svga::PCI_VENDOR_VMWARE, vmware_svga::PCI_DEVICE_SVGA2, &d)) {
                fb = pci::get_bar(d, 1) & ~0xF;
            }
        }
        if (fb == 0) fb = bga::get_framebuffer_addr();
    }
    return fb;
}

static void cmd_pat(int argc, char** argv) {
    if (argc >= 2) {
        if (!kstd::strcmp(argv[1], "bench") || !kstd::strcmp(argv[1], "benchmark")) {
            pat::run_benchmark();
            return;
        } else if (!kstd::strcmp(argv[1], "wc")) {
            uintptr_t fb = get_active_fb_addr();
            if (fb) {
                pat::set_range_wc(fb, 32 * 1024 * 1024);
                vga::set_color(vga::Color::LightGreen, vga::Color::Black);
                printk("[PAT] Framebuffer caching updated to Write-Combining (WC, PAT4).\n");
                vga::set_color(vga::Color::White, vga::Color::Black);
            }
            return;
        } else if (!kstd::strcmp(argv[1], "wb")) {
            uintptr_t fb = get_active_fb_addr();
            if (fb) {
                pat::set_range_wb(fb, 32 * 1024 * 1024);
                vga::set_color(vga::Color::Yellow, vga::Color::Black);
                printk("[PAT] Framebuffer caching updated to Write-Back (WB, PAT0).\n");
                vga::set_color(vga::Color::White, vga::Color::Black);
            }
            return;
        } else if (!kstd::strcmp(argv[1], "uc")) {
            uintptr_t fb = get_active_fb_addr();
            if (fb) {
                pat::set_range_uc(fb, 32 * 1024 * 1024);
                vga::set_color(vga::Color::LightRed, vga::Color::Black);
                printk("[PAT] Framebuffer caching updated to Uncacheable (UC, PAT3).\n");
                vga::set_color(vga::Color::White, vga::Color::Black);
            }
            return;
        } else if (!kstd::strcmp(argv[1], "help") || !kstd::strcmp(argv[1], "?")) {
            vga::set_color(vga::Color::LightCyan, vga::Color::Black);
            printk("=== AetherOS PAT & Write-Combining Controls ===\n");
            vga::set_color(vga::Color::White, vga::Color::Black);
            printk("Usage: pat [status|bench|wc|wb|uc]\n");
            printk("  pat             Display IA32_PAT MSR table, PDE flags, and framebuffer mode\n");
            printk("  pat bench       Benchmark 64MB memory write bandwidth on framebuffer\n");
            printk("  pat wc          Configure framebuffer caching as Write-Combining (WC)\n");
            printk("  pat wb          Configure framebuffer caching as Write-Back (WB)\n");
            printk("  pat uc          Configure framebuffer caching as Uncacheable (UC)\n");
            return;
        }
    }
    pat::print_status();
}

static void cmd_mem() {
    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk("--- Memory Statistics ---\n");
    vga::set_color(vga::Color::White, vga::Color::Black);

    size_t total_ram = pmm::get_total_memory();
    size_t used_ram = pmm::get_used_memory();
    size_t free_ram = pmm::get_free_memory();

    printk(" Physical RAM:\n");
    printk("   Total : %u MB (%u KB)\n", total_ram / (1024 * 1024), total_ram / 1024);
    printk("   Used  : %u MB (%u KB)\n", used_ram / (1024 * 1024), used_ram / 1024);
    printk("   Free  : %u MB (%u KB)\n", free_ram / (1024 * 1024), free_ram / 1024);
    printk("   Frames: %u total, %u used\n\n", pmm::get_total_frames(), pmm::get_used_frames());

    size_t heap_used = heap::get_used_bytes();
    size_t heap_free = heap::get_free_bytes();
    printk(" Kernel Heap Allocator:\n");
    printk("   Used  : %u bytes (%u KB)\n", heap_used, heap_used / 1024);
    printk("   Free  : %u bytes (%u KB)\n", heap_free, heap_free / 1024);
}

static void cmd_disk() {
    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk("--- ATA Storage ---\n");
    vga::set_color(vga::Color::White, vga::Color::Black);

    int n = ata::count();
    if (n == 0) {
        printk(" No ATA drives detected.\n");
        return;
    }
    for (int i = 0; i < n; ++i) {
        const ata::Drive& d = ata::drive(i);
        uint64_t mb = d.sectors * ata::SECTOR_SIZE / (1024 * 1024);
        printk(" hd%u: %s (%u MB, %s)\n",
               static_cast<unsigned>(i),
               d.model[0] ? d.model : "ATA Disk",
               static_cast<unsigned>(mb),
               d.lba48 ? "LBA48" : "LBA28");
    }
}

static void cmd_afs(int argc, char** argv) {
    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk("--- AetherFS ---\n");
    vga::set_color(vga::Color::White, vga::Color::Black);

    if (!aetherfs::mounted()) {
        printk(" AetherFS not mounted.\n");
        return;
    }

    if (argc < 2) {
        printk(" Files: %u  Blocks: %u / %u used  Snapshot: %s\n",
               static_cast<unsigned>(aetherfs::file_count()),
               static_cast<unsigned>(aetherfs::used_blocks()),
               static_cast<unsigned>(aetherfs::total_blocks()),
               aetherfs::snapshot_valid() ? "yes" : "no");
        const aetherfs::Entry* e = aetherfs::entries();
        for (size_t i = 0; i < aetherfs::MAX_FILES; ++i) {
            if (!(e[i].flags & aetherfs::FLAG_USED)) continue;
            printk("   %s  (%u bytes)%s\n", e[i].name,
                   static_cast<unsigned>(e[i].size),
                   (e[i].flags & aetherfs::FLAG_LOCKED) ? " [locked]" : "");
        }
        printk(" Usage: afs write <name> <text> | cat <name> | rm <name> | snap | restore | lock <true|false>\n");
        return;
    }

    if (!kstd::strcmp(argv[1], "write") && argc >= 4) {
        char text[256];
        text[0] = '\0';
        for (int i = 3; i < argc; ++i) {
            kstd::strcpy(text + kstd::strlen(text), argv[i]);
            if (i + 1 < argc) kstd::strcpy(text + kstd::strlen(text), " ");
        }
        bool ok = aetherfs::write_file(argv[2], text, kstd::strlen(text));
        printk(" %s: wrote '%s' (%u bytes)\n", ok ? "OK" : "FAIL", argv[2],
               static_cast<unsigned>(kstd::strlen(text)));
    } else if (!kstd::strcmp(argv[1], "cat") && argc >= 3) {
        char buf[512];
        int n = aetherfs::read_file(argv[2], buf, sizeof(buf) - 1);
        if (n < 0) {
            printk(" no such file: %s\n", argv[2]);
        } else {
            buf[n] = '\0';
            printk(" %s\n", buf);
        }
    } else if (!kstd::strcmp(argv[1], "rm") && argc >= 3) {
        printk(" %s: %s\n", aetherfs::remove_file(argv[2]) ? "removed" : "not found", argv[2]);
    } else if (!kstd::strcmp(argv[1], "snap")) {
        printk(" snapshot: %s\n", aetherfs::snapshot() ? "created" : "failed");
    } else if (!kstd::strcmp(argv[1], "restore")) {
        printk(" restore: %s\n", aetherfs::restore() ? "done" : "failed");
    } else if (!kstd::strcmp(argv[1], "lock") && argc >= 3) {
        bool v = !kstd::strcmp(argv[2], "true");
        printk(" update.lock = %s (%s)\n", v ? "true" : "false",
               aetherfs::set_update_lock(v) ? "ok" : "fail");
    } else {
        printk(" Unknown afs subcommand.\n");
    }
}

static void cmd_fat(int argc, char** argv) {
    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk("--- FAT16 Boot ---\n");
    vga::set_color(vga::Color::White, vga::Color::Black);

    if (!fat16::mounted()) {
        printk(" Boot FAT partition not mounted.\n");
        return;
    }

    if (argc >= 2 && !kstd::strcmp(argv[1], "verify")) {
        uint8_t hdr[64];
        int n = fat16::read_file("AETHEROSBIN", hdr, sizeof(hdr));
        if (n < 4) {
            printk(" AETHEROS.BIN: read failed (%d)\n", n);
            return;
        }
        uint32_t magic;
        kstd::memcpy(&magic, hdr, 4);
        printk(" AETHEROS.BIN: %d bytes, magic 0x%x (%s)\n",
               fat16::file_size("AETHEROSBIN"), magic,
               magic == 0x464C457F ? "ELF OK" : "bad");
        return;
    }

    if (argc >= 2 && !kstd::strcmp(argv[1], "reflash")) {
        int sz = fat16::file_size("AETHEROSBIN");
        if (sz <= 0) {
            printk(" AETHEROS.BIN: missing\n");
            return;
        }
        uint8_t* buf = new uint8_t[sz];
        int n = fat16::read_file("AETHEROSBIN", buf, sz);
        bool ok = (n == sz) && fat16::write_file("AETHEROSBIN", buf, sz);
        delete[] buf;
        printk(" reflash: %s (%d bytes)\n", ok ? "done" : "FAILED", n);
        return;
    }

    printk(" Kernel file : AETHEROS.BIN (%d bytes)\n", fat16::file_size("AETHEROSBIN"));
    printk(" Usage: fat verify | fat reflash\n");
}

static void cmd_update(int argc, char** argv) {
    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk("--- System Update ---\n");
    vga::set_color(vga::Color::White, vga::Color::Black);

    if (argc >= 2 && !kstd::strcmp(argv[1], "info")) {
        const updater::Config& c = updater::config();
        printk(" Source: http://%s:%u%s\n", c.host, static_cast<unsigned>(c.port), c.path);
        printk(" Usage: update | update check | update set <host> <port> <path>\n");
        return;
    }

    if (argc >= 5 && !kstd::strcmp(argv[1], "set")) {
        uint32_t port = 0;
        for (const char* d = argv[3]; *d >= '0' && *d <= '9'; ++d)
            port = port * 10 + static_cast<uint32_t>(*d - '0');
        updater::set_config(argv[2], static_cast<uint16_t>(port), argv[4]);
        printk(" Update source saved.\n");
        return;
    }

    char msg[128];
    msg[0] = '\0';

    if (argc >= 2 && !kstd::strcmp(argv[1], "check")) {
        updater::Result r = updater::fetch_verify(msg, sizeof(msg));
        printk(" Check: %s%s%s\n", updater::result_name(r),
               msg[0] ? " - " : "", msg);
        return;
    }

    printk(" Downloading and installing update...\n");
    updater::Result r = updater::apply(msg, sizeof(msg));
    if (r == updater::Result::Ok) {
        printk(" %s. Reboot to run the new kernel.\n", msg);
    } else if (r == updater::Result::UpToDate) {
        printk(" Already up to date - %s.\n", msg);
    } else {
        printk(" Update failed: %s\n", updater::result_name(r));
    }
}

static void cmd_apps(int argc, char** argv) {
    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk("--- AetherOS App Hub ---\n");
    vga::set_color(vga::Color::White, vga::Color::Black);

    char msg[128];
    msg[0] = '\0';

    if (argc >= 2 && !kstd::strcmp(argv[1], "catalog")) {
        printk(" Fetching catalog from update host...\n");
        if (apphub::refresh_catalog(msg, sizeof(msg)))
            printk(" %s.\n", msg);
        else
            printk(" Catalog refresh failed: %s\n", msg);
    } else if (argc >= 3 && !kstd::strcmp(argv[1], "install")) {
        if (apphub::install_by_name(argv[2], msg, sizeof(msg)))
            printk(" %s.\n", msg);
        else
            printk(" Install failed: %s\n", msg);
    } else if (argc >= 2 && !kstd::strcmp(argv[1], "update")) {
        printk(" Updating installed apps...\n");
        apphub::update_all(msg, sizeof(msg));
        printk(" %s.\n", msg);
    }

    int n = apphub::count();
    if (n == 0) {
        printk(" No apps listed. Run 'apps catalog' to fetch available apps.\n");
        printk(" Usage: apps [catalog | install <name> | update]\n");
        return;
    }

    printk(" %d app(s):\n", n);
    for (int i = 0; i < n; ++i) {
        const apphub::App* a = apphub::get(i);
        if (!a) continue;
        printk("  %s  v%s  [%s]", a->name, a->version[0] ? a->version : "?",
               a->installed ? "installed" : "available");
        if (a->installed && a->version[0] &&
            kstd::strcmp(a->installed_version, a->version) != 0)
            printk(" -> update %s", a->version);
        if (a->desc[0]) printk("  %s", a->desc);
        printk("\n");
    }
    printk(" Launch installed apps from the GUI App Hub, or open app://<name> in the browser.\n");
}

static void cmd_uptime() {
    uint64_t total_sec = pit::uptime_seconds();
    uint64_t hours = total_sec / 3600;
    uint64_t mins = (total_sec % 3600) / 60;
    uint64_t secs = total_sec % 60;
    uint64_t ms = pit::uptime_ms() % 1000;

    printk("System Uptime: %u hours, %u minutes, %u.%u seconds (Ticks: %u)\n",
           hours, mins, secs, ms, pit::get_ticks());
}

static void cmd_color(int argc, char** argv) {
    if (argc < 2) {
        printk("Usage: color <fg> [bg]\n");
        printk("Colors: black, blue, green, cyan, red, magenta, brown, lightgrey,\n");
        printk("        darkgrey, lightblue, lightgreen, lightcyan, lightred, yellow, white\n");
        return;
    }

    auto parse_color = [](const char* name) -> vga::Color {
        if (!kstd::strcmp(name, "black")) return vga::Color::Black;
        if (!kstd::strcmp(name, "blue")) return vga::Color::Blue;
        if (!kstd::strcmp(name, "green")) return vga::Color::Green;
        if (!kstd::strcmp(name, "cyan")) return vga::Color::Cyan;
        if (!kstd::strcmp(name, "red")) return vga::Color::Red;
        if (!kstd::strcmp(name, "magenta")) return vga::Color::Magenta;
        if (!kstd::strcmp(name, "brown")) return vga::Color::Brown;
        if (!kstd::strcmp(name, "lightgrey")) return vga::Color::LightGrey;
        if (!kstd::strcmp(name, "darkgrey")) return vga::Color::DarkGrey;
        if (!kstd::strcmp(name, "lightblue")) return vga::Color::LightBlue;
        if (!kstd::strcmp(name, "lightgreen")) return vga::Color::LightGreen;
        if (!kstd::strcmp(name, "lightcyan")) return vga::Color::LightCyan;
        if (!kstd::strcmp(name, "lightred")) return vga::Color::LightRed;
        if (!kstd::strcmp(name, "yellow")) return vga::Color::Yellow;
        if (!kstd::strcmp(name, "white")) return vga::Color::White;
        return vga::Color::White;
    };

    vga::Color fg = parse_color(argv[1]);
    vga::Color bg = (argc >= 3) ? parse_color(argv[2]) : vga::Color::Black;

    vga::set_color(fg, bg);
    printk("Color set to FG: %s, BG: %s\n", argv[1], (argc >= 3) ? argv[2] : "black");
}

static void cmd_calc(int argc, char** argv) {
    if (argc < 4) {
        printk("Usage: calc <number1> <op> <number2>\n");
        printk("Supported operators: +, -, *, /, %%\n");
        return;
    }

    int64_t a = kstd::parse_int(argv[1]);
    char op = argv[2][0];
    int64_t b = kstd::parse_int(argv[3]);
    int64_t res = 0;

    switch (op) {
        case '+': res = a + b; break;
        case '-': res = a - b; break;
        case '*': res = a * b; break;
        case '/':
            if (b == 0) {
                printk("Error: Division by zero!\n");
                return;
            }
            res = a / b;
            break;
        case '%':
            if (b == 0) {
                printk("Error: Modulo by zero!\n");
                return;
            }
            res = a % b;
            break;
        default:
            printk("Error: Unsupported operator '%c'\n", op);
            return;
    }

    printk("%d %c %d = %d\n", a, op, b, res);
}

static void cmd_rand(int argc, char** argv) {
    uint64_t tsc = 0;
    asm volatile("rdtsc" : "=A"(tsc));
    uint32_t seed = static_cast<uint32_t>(tsc ^ pit::get_ticks());
    seed = seed * 1103515245 + 12345;
    uint32_t r = (seed / 65536) % 32768;

    if (argc >= 3) {
        int64_t min_v = kstd::parse_int(argv[1]);
        int64_t max_v = kstd::parse_int(argv[2]);
        if (max_v > min_v) {
            int64_t val = min_v + (r % (max_v - min_v + 1));
            printk("Random [%d..%d]: %d\n", min_v, max_v, val);
            return;
        }
    }
    printk("Random: %u\n", r);
}

static void js_host_log(void*, const char* msg) {
    printk("[js] %s\n", msg);
}
static void js_host_write(void*, const char* html) {
    serial::write("[js:write] ");
    serial::write(html);
    serial::write("\n");
}

static void cmd_js(int argc, char** argv) {
    char src[1024];
    size_t n = 0;
    for (int i = 1; i < argc && n < sizeof(src) - 2; ++i) {
        if (i > 1 && n < sizeof(src) - 2) src[n++] = ' ';
        const char* a = argv[i];
        while (*a && n < sizeof(src) - 2) src[n++] = *a++;
    }
    src[n] = '\0';

    if (n == 0) {
        printk("Usage: js <javascript...>\n");
        printk("  e.g. js var x=2; console.log(Math.sqrt(x*x+5*5));\n");
        return;
    }

    js::Host host;
    host.log = js_host_log;
    host.write = js_host_write;
    bool ok = js::execute(src, &host);
    if (!ok) printk("[js] (empty or unparseable script)\n");
}

static void cmd_sound(int argc, char** argv) {
    if (argc >= 2) {
        if (!kstd::strcmp(argv[1], "cues")) {
            if (argc >= 3) {
                if (!kstd::strcmp(argv[2], "on") || !kstd::strcmp(argv[2], "enable")) {
                    sound::set_gui_cues_enabled(true);
                    printk("GUI audio cues enabled.\n");
                    return;
                } else if (!kstd::strcmp(argv[2], "off") || !kstd::strcmp(argv[2], "disable")) {
                    sound::set_gui_cues_enabled(false);
                    printk("GUI audio cues disabled.\n");
                    return;
                }
            }
            sound::set_gui_cues_enabled(!sound::are_gui_cues_enabled());
            printk("GUI audio cues %s.\n", sound::are_gui_cues_enabled() ? "enabled" : "disabled");
            return;
        }
    }

    vga::set_color(vga::Color::Yellow, vga::Color::Black);
    printk("=== AetherOS Audio Subsystem Status ===\n");
    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk(" Driver        : ");
    vga::set_color(vga::Color::White, vga::Color::Black);
    printk("%s\n", sound::get_driver_name());

    if (sound::get_driver_type() == sound::DriverType::AC97) {
        vga::set_color(vga::Color::LightCyan, vga::Color::Black);
        printk(" Hardware      : ");
        vga::set_color(vga::Color::White, vga::Color::Black);
        printk("Intel 82801AA AC'97 (PCI %u:%u.%u)\n", ac97::get_pci_bus(), ac97::get_pci_slot(), ac97::get_pci_func());

        vga::set_color(vga::Color::LightCyan, vga::Color::Black);
        printk(" I/O Ports     : ");
        vga::set_color(vga::Color::White, vga::Color::Black);
        printk("Mixer (NAMBAR) 0x%x | Bus Master (NABMBAR) 0x%x\n", ac97::get_nambar(), ac97::get_nabmbar());

        vga::set_color(vga::Color::LightCyan, vga::Color::Black);
        printk(" Stream Config : ");
        vga::set_color(vga::Color::White, vga::Color::Black);
        printk("16-bit Stereo PCM @ %u Hz (Double-Buffered DMA BDL)\n", ac97::get_sample_rate());
    } else if (sound::get_driver_type() == sound::DriverType::SB16) {
        vga::set_color(vga::Color::LightCyan, vga::Color::Black);
        printk(" Hardware      : ");
        vga::set_color(vga::Color::White, vga::Color::Black);
        printk("Creative Sound Blaster 16 (Port 0x%x, DSP v%u.%u)\n", sb16::get_base_port(), sb16::get_version_major(), sb16::get_version_minor());
    }

    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk(" Companion     : ");
    vga::set_color(vga::Color::White, vga::Color::Black);
    printk("PC Speaker (PIT Timer 2 / Port 0x61) [Ready]\n");

    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk(" Master Volume : ");
    vga::set_color(vga::Color::LightGreen, vga::Color::Black);
    printk("%u%%", sound::get_volume());
    vga::set_color(vga::Color::White, vga::Color::Black);
    printk(" [%s]\n", sound::is_muted() ? "MUTED" : "UNMUTED");

    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk(" Playback State: ");
    vga::set_color(vga::Color::White, vga::Color::Black);
    printk("%s\n", sound::is_playing() ? "Playing" : "Idle");

    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk(" GUI Sound Cues: ");
    vga::set_color(vga::Color::White, vga::Color::Black);
    printk("%s\n", sound::are_gui_cues_enabled() ? "Enabled" : "Disabled");
}

static void cmd_volume(int argc, char** argv) {
    if (argc < 2) {
        printk("Master volume: %u%% (%s)\n", sound::get_volume(), sound::is_muted() ? "Muted" : "Active");
        printk("Usage: volume <0-100>\n");
        return;
    }
    int vol = kstd::parse_int(argv[1]);
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    sound::set_volume(static_cast<uint8_t>(vol));
    if (sound::is_muted()) sound::set_mute(false);
    vga::set_color(vga::Color::LightGreen, vga::Color::Black);
    printk("Master output volume set to %d%%.\n", vol);
    vga::set_color(vga::Color::White, vga::Color::Black);
}

static void cmd_mute() {
    sound::set_mute(true);
    vga::set_color(vga::Color::Yellow, vga::Color::Black);
    printk("Audio output muted.\n");
    vga::set_color(vga::Color::White, vga::Color::Black);
}

static void cmd_unmute() {
    sound::set_mute(false);
    vga::set_color(vga::Color::LightGreen, vga::Color::Black);
    printk("Audio output unmuted. Current volume: %u%%.\n", sound::get_volume());
    vga::set_color(vga::Color::White, vga::Color::Black);
}

static void cmd_play(int argc, char** argv) {
    if (argc < 2) {
        vga::set_color(vga::Color::Yellow, vga::Color::Black);
        printk("Usage: play <effect> [options]\n");
        vga::set_color(vga::Color::White, vga::Color::Black);
        printk("Available procedural sound effects:\n");
        printk("  play beep               Play standard alert tone\n");
        printk("  play chime              Play crystalline multi-tone bell chord\n");
        printk("  play laser              Play retro pitch-swept synth effect\n");
        printk("  play coin               Play 8-bit coin pickup arpeggio\n");
        printk("  play fanfare            Play triumphant victory fanfare jingle\n");
        printk("  play tone [freq] [ms]   Play synthesized sine tone (e.g. play tone 440 300)\n");
        printk("  play chord              Play polyphonic C major triad chord\n");
        printk("  play melody             Play multi-note musical melody\n");
        return;
    }

    const char* effect = argv[1];
    if (!kstd::strcmp(effect, "beep")) {
        printk("Playing procedural effect 'beep'...\n");
        sound::play_effect(sound::Effect::Beep);
    } else if (!kstd::strcmp(effect, "chime") || !kstd::strcmp(effect, "bell")) {
        printk("Playing procedural effect 'chime' (multi-frequency bell chord)...\n");
        sound::play_effect(sound::Effect::Chime);
    } else if (!kstd::strcmp(effect, "laser")) {
        printk("Playing procedural effect 'laser' (pitch-swept synth)...\n");
        sound::play_effect(sound::Effect::Laser);
    } else if (!kstd::strcmp(effect, "coin")) {
        printk("Playing procedural effect 'coin' (8-bit coin pickup)...\n");
        sound::play_effect(sound::Effect::Coin);
    } else if (!kstd::strcmp(effect, "fanfare")) {
        printk("Playing procedural effect 'fanfare' (victory jingle)...\n");
        sound::play_effect(sound::Effect::Fanfare);
    } else if (!kstd::strcmp(effect, "tone")) {
        uint32_t freq = 440;
        uint32_t ms = 300;
        if (argc >= 3) freq = static_cast<uint32_t>(kstd::parse_int(argv[2]));
        if (argc >= 4) ms = static_cast<uint32_t>(kstd::parse_int(argv[3]));
        printk("Playing %u Hz sine tone for %u ms...\n", freq, ms);
        sound::play_tone(freq, ms, sound::Waveform::Sine, true);
    } else if (!kstd::strcmp(effect, "chord")) {
        printk("Playing polyphonic C Major chord (C5 + E5 + G5 + C6)...\n");
        uint32_t c_maj[] = { 523, 659, 784, 1046 };
        sound::play_chord(c_maj, 4, 500, true);
    } else if (!kstd::strcmp(effect, "melody")) {
        printk("Playing musical melody...\n");
        sound::Note melody[] = {
            { 523, 150 }, { 587, 150 }, { 659, 150 }, { 698, 150 },
            { 784, 250 }, { 659, 150 }, { 784, 350 }
        };
        sound::play_melody(melody, 7, true);
    } else {
        printk("Unknown sound effect '%s'. Type 'play' for available effects.\n", effect);
    }
}

static void cmd_wavplay(int argc, char** argv) {
    if (argc < 2) {
        printk("Usage: wavplay <filename.wav>\n");
        printk("Available sound files in RamFS:\n");
        const ramfs::File* files = ramfs::get_files();
        for (size_t i = 0; i < ramfs::MAX_FILES; ++i) {
            if (files[i].used && kstd::strstr(files[i].name, ".wav")) {
                printk("  - %s (%u bytes)\n", files[i].name, files[i].size);
            }
        }
        return;
    }

    const char* filename = argv[1];
    size_t file_size = 0;
    const void* data = ramfs::read_bytes(filename, &file_size);
    if (!data || file_size == 0) {
        vga::set_color(vga::Color::LightRed, vga::Color::Black);
        printk("wavplay: file '%s' not found in RamFS.\n", filename);
        vga::set_color(vga::Color::White, vga::Color::Black);
        return;
    }

    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk("Playing '%s' (%u bytes) from RamFS via %s...\n", filename, file_size, sound::get_driver_name());
    vga::set_color(vga::Color::White, vga::Color::Black);

    if (sound::play_wav(data, file_size, true)) {
        vga::set_color(vga::Color::LightGreen, vga::Color::Black);
        printk("Playback finished successfully.\n");
        vga::set_color(vga::Color::White, vga::Color::Black);
    } else {
        vga::set_color(vga::Color::LightRed, vga::Color::Black);
        printk("wavplay: failed to decode/play '%s' (unsupported WAV PCM format).\n", filename);
        vga::set_color(vga::Color::White, vga::Color::Black);
    }
}

static void cmd_beep(int argc, char** argv) {
    uint32_t freq = 750;
    uint32_t ms = 120;
    if (argc >= 2) freq = static_cast<uint32_t>(kstd::parse_int(argv[1]));
    if (argc >= 3) ms = static_cast<uint32_t>(kstd::parse_int(argv[2]));
    printk("Playing tone %u Hz for %u ms...\n", freq, ms);
    speaker::beep(freq, ms);
}

// Filesystem commands
static void cmd_cat(int argc, char** argv) {
    if (argc < 2) {
        printk("Usage: cat <filename>\n");
        return;
    }
    const char* content = ramfs::read(argv[1]);
    if (!content) {
        printk("cat: file '%s' not found\n", argv[1]);
        return;
    }
    printk("%s\n", content);
}

static void cmd_touch(int argc, char** argv) {
    if (argc < 2) {
        printk("Usage: touch <filename>\n");
        return;
    }
    if (ramfs::create(argv[1])) {
        printk("Created file '%s'.\n", argv[1]);
    } else {
        printk("touch: failed to create '%s' (file may already exist or ramfs full)\n", argv[1]);
    }
}

static void cmd_write(int argc, char** argv) {
    if (argc < 3) {
        printk("Usage: write <filename> <text...>\n");
        return;
    }
    char buffer[256];
    buffer[0] = '\0';
    for (int i = 2; i < argc; ++i) {
        kstd::strcpy(buffer + kstd::strlen(buffer), argv[i]);
        if (i < argc - 1) {
            kstd::strcpy(buffer + kstd::strlen(buffer), " ");
        }
    }
    if (ramfs::write(argv[1], buffer, false)) {
        printk("Saved %u bytes to '%s'.\n", kstd::strlen(buffer), argv[1]);
    } else {
        printk("write: failed to write to '%s'\n", argv[1]);
    }
}

static void cmd_rm(int argc, char** argv) {
    if (argc < 2) {
        printk("Usage: rm <filename>\n");
        return;
    }
    if (ramfs::remove(argv[1])) {
        printk("Removed file '%s'.\n", argv[1]);
    } else {
        printk("rm: cannot remove '%s': No such file\n", argv[1]);
    }
}

static void cmd_memdump(int argc, char** argv) {
    if (argc < 2) {
        printk("Usage: memdump <hex_address> [count]\n");
        return;
    }

    uint64_t addr = kstd::parse_hex(argv[1]);
    size_t count = (argc >= 3) ? static_cast<size_t>(kstd::parse_int(argv[2])) : 64;
    if (count > 256) count = 256;

    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(addr);
    for (size_t i = 0; i < count; i += 16) {
        printk("%p: ", addr + i);
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < count) {
                uint8_t byte = ptr[i + j];
                char buf[4];
                kstd::utoa(byte, buf, 16);
                if (byte < 0x10) printk("0");
                printk("%s ", buf);
            } else {
                printk("   ");
            }
        }
        printk(" |");
        for (size_t j = 0; j < 16 && (i + j) < count; ++j) {
            uint8_t c = ptr[i + j];
            if (c >= 32 && c <= 126) printk("%c", static_cast<char>(c));
            else printk(".");
        }
        printk("|\n");
    }
}

static void cmd_panic(int argc, char** argv) {
    if (argc < 2) {
        printk("Usage: panic <div|page|gp>\n");
        return;
    }

    if (!kstd::strcmp(argv[1], "div")) {
        printk("Triggering Divide-By-Zero (#DE)...\n");
        volatile int zero = 0;
        volatile int crash = 100 / zero;
        (void)crash;
    } else if (!kstd::strcmp(argv[1], "page")) {
        printk("Triggering Page Fault (#PF) at 0xDEADBEEF000...\n");
        volatile uint64_t* bad_ptr = reinterpret_cast<volatile uint64_t*>(0xDEADBEEF000ULL);
        *bad_ptr = 42;
    } else if (!kstd::strcmp(argv[1], "gp")) {
        printk("Triggering General Protection Fault (#GP)...\n");
        asm volatile("mov $0x1234, %ax; mov %ax, %ds");
    } else {
        printk("Unknown panic type '%s'. Use div, page, or gp.\n", argv[1]);
    }
}

// Digital Matrix Rain Animation
static void cmd_matrix() {
    vga::clear();
    vga::disable_cursor();

    uint8_t drops[vga::WIDTH];
    for (size_t x = 0; x < vga::WIDTH; ++x) {
        drops[x] = static_cast<uint8_t>((x * 7 + 3) % vga::HEIGHT);
    }

    uint32_t seed = static_cast<uint32_t>(pit::uptime_ms());
    auto rng = [&seed]() -> uint32_t {
        seed = seed * 1103515245 + 12345;
        return (seed / 65536) % 32768;
    };

    const char chars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!@#$%^&*";
    size_t num_chars = sizeof(chars) - 1;

    while (!keyboard::has_char()) {
        for (size_t x = 0; x < vga::WIDTH; ++x) {
            size_t y = drops[x];
            char c = chars[rng() % num_chars];

            // Head of the drop is bright white/green
            vga::put_entry_at(c, vga::make_color(vga::Color::White, vga::Color::Black), x, y);

            // Tail behind is green
            if (y > 0) {
                char tc = chars[rng() % num_chars];
                vga::put_entry_at(tc, vga::make_color(vga::Color::LightGreen, vga::Color::Black), x, y - 1);
            }
            if (y > 3) {
                char tc = chars[rng() % num_chars];
                vga::put_entry_at(tc, vga::make_color(vga::Color::Green, vga::Color::Black), x, y - 3);
            }
            if (y > 8) {
                vga::put_entry_at(' ', vga::make_color(vga::Color::Black, vga::Color::Black), x, y - 8);
            }

            drops[x]++;
            if (drops[x] >= vga::HEIGHT + 8) {
                drops[x] = 0;
            }
        }
        pit::sleep(35);
    }

    keyboard::get_char(); // consume keypress
    vga::enable_cursor(14, 15);
    vga::clear();
    print_banner();
}

// Interactive Snake Game in VGA text mode
static void cmd_snake() {
    vga::clear();
    vga::disable_cursor();

    struct Point {
        int x, y;
    };

    constexpr int MAX_SNAKE = 400;
    Point snake[MAX_SNAKE];
    int snake_len = 5;

    for (int i = 0; i < snake_len; ++i) {
        snake[i].x = 40 - i;
        snake[i].y = 12;
    }

    int dir_x = 1, dir_y = 0;
    Point food = { 20, 10 };
    int score = 0;
    bool game_over = false;

    uint32_t seed = static_cast<uint32_t>(pit::uptime_ms());
    auto rng = [&seed]() -> uint32_t {
        seed = seed * 1103515245 + 12345;
        return (seed / 65536) % 32768;
    };

    auto spawn_food = [&]() {
        food.x = 2 + (rng() % (vga::WIDTH - 4));
        food.y = 2 + (rng() % (vga::HEIGHT - 4));
    };

    uint8_t border_col = vga::make_color(vga::Color::Cyan, vga::Color::Black);
    for (size_t x = 0; x < vga::WIDTH; ++x) {
        vga::put_entry_at('#', border_col, x, 1);
        vga::put_entry_at('#', border_col, x, vga::HEIGHT - 1);
    }
    for (size_t y = 1; y < vga::HEIGHT; ++y) {
        vga::put_entry_at('#', border_col, 0, y);
        vga::put_entry_at('#', border_col, vga::WIDTH - 1, y);
    }

    vga::set_cursor(2, 0);
    vga::set_color(vga::Color::Yellow, vga::Color::Black);
    printk("=== AETHER-OS SNAKE === [Arrows/WASD to move, Q to quit] Score: 0   ");

    while (!game_over) {
        while (keyboard::has_char()) {
            char k = keyboard::get_char();
            if (k == 'w' || k == 'W' || k == static_cast<char>(keyboard::KEY_UP)) { if (dir_y == 0) { dir_x = 0; dir_y = -1; } }
            else if (k == 's' || k == 'S' || k == static_cast<char>(keyboard::KEY_DOWN)) { if (dir_y == 0) { dir_x = 0; dir_y = 1; } }
            else if (k == 'a' || k == 'A' || k == static_cast<char>(keyboard::KEY_LEFT)) { if (dir_x == 0) { dir_x = -1; dir_y = 0; } }
            else if (k == 'd' || k == 'D' || k == static_cast<char>(keyboard::KEY_RIGHT)) { if (dir_x == 0) { dir_x = 1; dir_y = 0; } }
            else if (k == 'q' || k == 'Q' || k == 27) { game_over = true; break; }
        }
        if (game_over) break;

        vga::put_entry_at(' ', vga::make_color(vga::Color::Black, vga::Color::Black),
                          snake[snake_len - 1].x, snake[snake_len - 1].y);

        for (int i = snake_len - 1; i > 0; --i) {
            snake[i] = snake[i - 1];
        }

        snake[0].x += dir_x;
        snake[0].y += dir_y;

        if (snake[0].x <= 0 || snake[0].x >= static_cast<int>(vga::WIDTH - 1) ||
            snake[0].y <= 1 || snake[0].y >= static_cast<int>(vga::HEIGHT - 1)) {
            game_over = true;
            break;
        }

        for (int i = 1; i < snake_len; ++i) {
            if (snake[0].x == snake[i].x && snake[0].y == snake[i].y) {
                game_over = true;
                break;
            }
        }
        if (game_over) break;

        if (snake[0].x == food.x && snake[0].y == food.y) {
            score += 10;
            speaker::beep(1000, 30);
            if (snake_len < MAX_SNAKE - 1) {
                snake[snake_len] = snake[snake_len - 1];
                snake_len++;
            }
            spawn_food();
            vga::set_cursor(60, 0);
            vga::set_color(vga::Color::Yellow, vga::Color::Black);
            printk("Score: %d", score);
        }

        vga::put_entry_at('*', vga::make_color(vga::Color::LightRed, vga::Color::Black), food.x, food.y);
        vga::put_entry_at('@', vga::make_color(vga::Color::Yellow, vga::Color::Black), snake[0].x, snake[0].y);
        for (int i = 1; i < snake_len; ++i) {
            vga::put_entry_at('O', vga::make_color(vga::Color::LightGreen, vga::Color::Black), snake[i].x, snake[i].y);
        }

        pit::sleep(70);
    }

    vga::set_cursor(28, 12);
    vga::set_color(vga::Color::White, vga::Color::Red);
    printk(" *** GAME OVER! Final Score: %d *** ", score);
    vga::set_cursor(26, 14);
    vga::set_color(vga::Color::Yellow, vga::Color::Black);
    printk(" Press any key to return to shell... ");

    speaker::beep(300, 300);

    while (keyboard::has_char()) keyboard::get_char();
    keyboard::get_char_blocking();

    vga::enable_cursor(14, 15);
    vga::set_color(vga::Color::White, vga::Color::Black);
    vga::clear();
    print_banner();
}

static void cmd_reboot() {
    printk("Rebooting system...\n");
    arch::outb(0x64, 0xFE);
    asm volatile("lidt 0; int $3");
}

static void cmd_shutdown() {
    printk("Shutting down system...\n");
    arch::outw(0x604, 0x2000);  // QEMU ACPI shutdown
    arch::outw(0xB004, 0x2000); // Bochs ACPI shutdown
    arch::outw(0x4004, 0x3400); // VirtualBox shutdown
    printk("System halted. It is now safe to turn off your machine.\n");
    arch::cli();
    while (true) arch::hlt();
}

static void cmd_gpu() {
    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    printk("================================================================================\n");
    printk("             AetherOS Hardware Graphics Subsystem & Acceleration               \n");
    printk("================================================================================\n");
    vga::set_color(vga::Color::White, vga::Color::Black);

    gfx_driver::DriverType type = gfx_driver::auto_detect();
    const char* type_str = "None";
    if (type == gfx_driver::DriverType::VMwareSVGA) type_str = "VMware SVGA-II PCI (Vendor 0x15AD, Device 0x0405)";
    else if (type == gfx_driver::DriverType::BochsVBE) type_str = "Bochs/QEMU VBE (BGA) with Hardware Panning Page-Flip";
    else if (type == gfx_driver::DriverType::FallbackVBE) type_str = "Standard VBE Linear Framebuffer";

    printk("Active Driver:          %s\n", type_str);
    printk("Hardware 2D FIFO Accel: %s\n", (type == gfx_driver::DriverType::VMwareSVGA) ? "Enabled (SVGA_CMD_UPDATE, RECT_FILL, RECT_COPY)" : "Not supported on this device");
    printk("Hardware Page Flipping: %s\n", (type == gfx_driver::DriverType::BochsVBE) ? "Managed via FIFO (software backbuffer copy to VRAM)" : "N/A (Managed via FIFO)");
    printk("Hardware Cursor:        %s\n", (type == gfx_driver::DriverType::VMwareSVGA) ? "Supported (Direct hardware cursor bypass)" : "Software Overlay");
    printk("IA32_PAT Write-Combine: Enabled (32 MB burst caching)\n");

    uintptr_t fb = get_active_fb_addr();
    printk("Physical Framebuffer:   %p\n", fb);
    printk("Resolution Supported:   Up to 1920x1080 Full HD (32 bpp TrueColor)\n");
    printk("Status:                 Online & Ready\n");
}

static void cmd_gui(int argc, char** argv) {
    if (!gfx::is_supported()) {
        printk("Error: No supported graphics hardware driver (VMware SVGA-II or Bochs VBE) detected!\n");
        return;
    }

    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t refresh_rate = 144;

    if (argc == 1) {
        vga::set_color(vga::Color::LightCyan, vga::Color::Black);
        printk("================================================================================\n");
        printk("                  AetherOS High-Resolution Desktop GUI Mode                    \n");
        printk("================================================================================\n");
        vga::set_color(vga::Color::Yellow, vga::Color::Black);
        printk("Select Screen Resolution:\n");
        vga::set_color(vga::Color::White, vga::Color::Black);
        printk("  [1] 1920x1080 (1080p Full HD - Default for 1080p monitors)\n");
        printk("  [2] 1600x900  (900p HD+)\n");
        printk("  [3] 1366x768  (WXGA Laptop)\n");
        printk("  [4] 1280x720  (720p HD Standard)\n");
        printk("  [5] 1024x768  (XGA Classic)\n");
        printk("  [6] 800x600   (SVGA Compatibility)\n");
        vga::set_color(vga::Color::LightGreen, vga::Color::Black);
        printk("Desktop Mode:  Fast Boot (3D Engine on-demand, Press [3] to launch)\n");
        printk("Refresh Rates: [144Hz] High Refresh (Press [R] inside GUI to cycle 60-240Hz)\n");
        vga::set_color(vga::Color::LightCyan, vga::Color::Black);
        printk("Enter choice [1-6] or press [Enter] for 1920x1080 @ 144Hz: ");
        vga::set_color(vga::Color::White, vga::Color::Black);

        char sel = keyboard::get_char_blocking();
        printk("%c\n", (sel == '\n' || sel == '\r') ? '1' : sel);

        if (sel == '2') {
            width = 1600; height = 900;
        } else if (sel == '3') {
            width = 1366; height = 768;
        } else if (sel == '4') {
            width = 1280; height = 720;
        } else if (sel == '5') {
            width = 1024; height = 768;
        } else if (sel == '6') {
            width = 800; height = 600;
        } else {
            // Default 1920x1080 Full HD
            width = 1920; height = 1080;
        }
    } else if (argc >= 2) {
        const char* arg1 = argv[1];
        if (!kstd::strcmp(arg1, "1080p") || !kstd::strcmp(arg1, "fhd")) {
            width = 1920; height = 1080;
        } else if (!kstd::strcmp(arg1, "1080p-win") || !kstd::strcmp(arg1, "win1080")) {
            width = 1920; height = 1000;
        } else if (!kstd::strcmp(arg1, "720p") || !kstd::strcmp(arg1, "hd")) {
            width = 1280; height = 720;
        } else if (!kstd::strcmp(arg1, "900p")) {
            width = 1600; height = 900;
        } else if (!kstd::strcmp(arg1, "help") || !kstd::strcmp(arg1, "?")) {
            vga::set_color(vga::Color::LightCyan, vga::Color::Black);
            printk("Usage: gui [width] [height] [fps]\n");
            printk("Examples:\n");
            printk("  gui                     Interactive menu (Fast Boot, 1920x1080 @ 144Hz)\n");
            printk("  gui 1920 1080           1080p Full HD at 144Hz\n");
            printk("  gui 1920 1080 240       1080p Full HD at 240Hz\n");
            printk("  gui 1920 1080 60        1080p Full HD at 60Hz\n");
            printk("  gui 1280 720            720p HD at 144Hz\n");
            printk("  gui 1080p               Quick 1080p preset\n");
            return;
        } else {
            const char* x_pos = kstd::strchr(arg1, 'x');
            if (x_pos) {
                width = static_cast<uint32_t>(kstd::parse_int(arg1));
                height = static_cast<uint32_t>(kstd::parse_int(x_pos + 1));
            } else {
                width = static_cast<uint32_t>(kstd::parse_int(arg1));
                if (argc >= 3) {
                    height = static_cast<uint32_t>(kstd::parse_int(argv[2]));
                }
            }
        }

        if (argc >= 4) {
            refresh_rate = static_cast<uint32_t>(kstd::parse_int(argv[3]));
        } else if (argc == 3 && (kstd::strchr(arg1, 'x') != nullptr || !kstd::strcmp(arg1, "1080p") || !kstd::strcmp(arg1, "720p"))) {
            refresh_rate = static_cast<uint32_t>(kstd::parse_int(argv[2]));
        }
    }

    if (width < 640) width = 640;
    if (height < 480) height = 480;

    printk("Launching Desktop GUI (Fast Boot, 3D Engine on-demand) at %ux%u @ %u Hz...\n", width, height, refresh_rate);
    gui::run_desktop(width, height, refresh_rate);
}

static void cmd_browser(int argc, char** argv) {
    // Headless mode: "browser dump <url>" loads and prints the laid-out text
    // boxes to the console without starting the GUI (handy for debugging).
    if (argc >= 3 && !kstd::strcmp(argv[1], "dump")) {
        browser::Document* doc = browser::create_document();
        if (!doc) { printk("browser: could not allocate document\n"); return; }
        bool ok = browser::load_url(doc, argv[2], 760);
        printk("browser dump '%s' -> %s, title='%s', boxes=%u\n",
               argv[2], ok ? "ok" : "partial", doc->title,
               static_cast<unsigned>(doc->box_count));
        for (size_t i = 0; i < doc->box_count; ++i) {
            const browser::LayoutBox& b = doc->boxes[i];
            if (b.text[0]) printk("  [%u] %s\n", static_cast<unsigned>(i), b.text);
            else if (b.has_bg || b.has_border || b.is_button || b.is_input)
                printk("  [%u] <box %dx%d%s%s%s%s>\n", static_cast<unsigned>(i), b.w, b.h,
                       b.has_bg ? " bg" : "", b.has_gradient ? " grad" : "",
                       b.has_border ? " border" : "",
                       b.is_button ? " button" : (b.is_input ? " input" : ""));
        }
        browser::destroy_document(doc);
        return;
    }

    // "browser links <url>": show each link's resolved absolute target.
    if (argc >= 3 && !kstd::strcmp(argv[1], "links")) {
        browser::Document* doc = browser::create_document();
        if (!doc) { printk("browser: could not allocate document\n"); return; }
        bool ok = browser::load_url(doc, argv[2], 760);
        printk("browser links '%s' -> %s, current='%s'\n",
               argv[2], ok ? "ok" : "partial", doc->current_url);
        for (size_t i = 0; i < doc->box_count; ++i) {
            const browser::LayoutBox& b = doc->boxes[i];
            if (!b.link_url[0]) continue;
            char abs[512];
            browser::resolve_url(doc->current_url, b.link_url, abs, sizeof(abs));
            printk("  [%u] '%s' -> %s\n", static_cast<unsigned>(i), b.text, abs);
        }
        browser::destroy_document(doc);
        return;
    }

    // "browser boxes <url>": print laid-out geometry for layout debugging.
    if (argc >= 3 && !kstd::strcmp(argv[1], "boxes")) {
        browser::Document* doc = browser::create_document();
        if (!doc) { printk("browser: could not allocate document\n"); return; }
        bool ok = browser::load_url(doc, argv[2], 760);
        printk("browser boxes '%s' -> %s, page=%dx%d, boxes=%u\n",
               argv[2], ok ? "ok" : "partial", doc->page_width, doc->page_height,
               static_cast<unsigned>(doc->box_count));
        for (size_t i = 0; i < doc->box_count; ++i) {
            const browser::LayoutBox& b = doc->boxes[i];
            if (!b.text[0] && !b.has_bg && !b.has_border) continue;
            printk("  [%u] x=%d y=%d w=%d h=%d s=%u%s%s%s%s %s\n",
                   static_cast<unsigned>(i), b.x, b.y, b.w, b.h,
                   static_cast<unsigned>(b.scale),
                   b.has_bg ? " bg" : "", b.has_gradient ? " grad" : "",
                   b.has_border ? " border" : "",
                   b.link_url[0] ? " link" : "", b.text);
        }
        browser::destroy_document(doc);
        return;
    }

    if (!gfx::is_supported()) {
        printk("Error: No supported graphics hardware driver (VMware SVGA-II or Bochs VBE) detected!\n");
        return;
    }
    const char* target_url = "about:home";
    if (argc >= 2) {
        target_url = argv[1];
    }
    gui::set_initial_browser_url(target_url);
    printk("Launching Desktop GUI with Aether Web Browser navigating to '%s'...\n", target_url);
    gui::run_desktop(1920, 1080, 144);
}

static void cmd_demo(int argc, char** argv) {
    if (!gfx::is_supported()) {
        printk("Error: No supported graphics hardware driver (VMware SVGA-II or Bochs VBE) detected!\n");
        return;
    }

    uint32_t width = 1920;
    uint32_t height = 1080;

    if (argc >= 3) {
        width = static_cast<uint32_t>(kstd::parse_int(argv[1]));
        height = static_cast<uint32_t>(kstd::parse_int(argv[2]));
    } else if (argc == 2) {
        const char* x_pos = kstd::strchr(argv[1], 'x');
        if (x_pos) {
            width = static_cast<uint32_t>(kstd::parse_int(argv[1]));
            height = static_cast<uint32_t>(kstd::parse_int(x_pos + 1));
        } else if (!kstd::strcmp(argv[1], "720p")) {
            width = 1280; height = 720;
        } else if (!kstd::strcmp(argv[1], "1024")) {
            width = 1024; height = 768;
        }
    }

    if (width < 640) width = 640;
    if (height < 480) height = 480;

    printk("Displaying graphics engine test pattern at %ux%u...\n", width, height);
    gui::run_demo(width, height);
}

static void execute_line(char* line) {
    while (kstd::isspace(*line)) line++;
    if (*line == '\0') return;

    // Record in history
    add_history(line);

    char* argv[16];
    int argc = 0;
    char* p = line;

    while (*p && argc < 16) {
        while (kstd::isspace(*p)) *p++ = '\0';
        if (*p == '\0') break;
        argv[argc++] = p;
        while (*p && !kstd::isspace(*p)) p++;
    }

    if (argc == 0) return;

    const char* cmd = argv[0];
    if (!kstd::strcmp(cmd, "help")) {
        cmd_help();
    } else if (!kstd::strcmp(cmd, "clear")) {
        vga::clear();
    } else if (!kstd::strcmp(cmd, "info")) {
        cmd_info();
    } else if (!kstd::strcmp(cmd, "fetch") || !kstd::strcmp(cmd, "neofetch")) {
        cmd_fetch();
    } else if (!kstd::strcmp(cmd, "features") || !kstd::strcmp(cmd, "cpuid")) {
        cmd_features();
    } else if (!kstd::strcmp(cmd, "bench") || !kstd::strcmp(cmd, "benchmark")) {
        cmd_bench();
    } else if (!kstd::strcmp(cmd, "pat")) {
        cmd_pat(argc, argv);
    } else if (!kstd::strcmp(cmd, "mem")) {
        cmd_mem();
    } else if (!kstd::strcmp(cmd, "disk") || !kstd::strcmp(cmd, "disks") || !kstd::strcmp(cmd, "lsblk")) {
        cmd_disk();
    } else if (!kstd::strcmp(cmd, "afs")) {
        cmd_afs(argc, argv);
    } else if (!kstd::strcmp(cmd, "fat")) {
        cmd_fat(argc, argv);
    } else if (!kstd::strcmp(cmd, "update") || !kstd::strcmp(cmd, "upgrade")) {
        cmd_update(argc, argv);
    } else if (!kstd::strcmp(cmd, "apps") || !kstd::strcmp(cmd, "apphub")) {
        cmd_apps(argc, argv);
    } else if (!kstd::strcmp(cmd, "uptime")) {
        cmd_uptime();
    } else if (!kstd::strcmp(cmd, "date") || !kstd::strcmp(cmd, "rtc") || !kstd::strcmp(cmd, "time")) {
        rtc::print_datetime();
    } else if (!kstd::strcmp(cmd, "lspci")) {
        pci::list_devices();
    } else if (!kstd::strcmp(cmd, "ls") || !kstd::strcmp(cmd, "dir")) {
        ramfs::list();
    } else if (!kstd::strcmp(cmd, "cat")) {
        cmd_cat(argc, argv);
    } else if (!kstd::strcmp(cmd, "touch")) {
        cmd_touch(argc, argv);
    } else if (!kstd::strcmp(cmd, "write")) {
        cmd_write(argc, argv);
    } else if (!kstd::strcmp(cmd, "rm")) {
        cmd_rm(argc, argv);
    } else if (!kstd::strcmp(cmd, "history")) {
        cmd_history();
    } else if (!kstd::strcmp(cmd, "echo")) {
        for (int i = 1; i < argc; ++i) {
            printk("%s%s", argv[i], (i == argc - 1) ? "" : " ");
        }
        printk("\n");
    } else if (!kstd::strcmp(cmd, "color")) {
        cmd_color(argc, argv);
    } else if (!kstd::strcmp(cmd, "calc")) {
        cmd_calc(argc, argv);
    } else if (!kstd::strcmp(cmd, "rand")) {
        cmd_rand(argc, argv);
    } else if (!kstd::strcmp(cmd, "js") || !kstd::strcmp(cmd, "jshell")) {
        cmd_js(argc, argv);
    } else if (!kstd::strcmp(cmd, "sound")) {
        cmd_sound(argc, argv);
    } else if (!kstd::strcmp(cmd, "volume") || !kstd::strcmp(cmd, "vol")) {
        cmd_volume(argc, argv);
    } else if (!kstd::strcmp(cmd, "mute")) {
        cmd_mute();
    } else if (!kstd::strcmp(cmd, "unmute")) {
        cmd_unmute();
    } else if (!kstd::strcmp(cmd, "play")) {
        cmd_play(argc, argv);
    } else if (!kstd::strcmp(cmd, "wavplay")) {
        cmd_wavplay(argc, argv);
    } else if (!kstd::strcmp(cmd, "beep")) {
        cmd_beep(argc, argv);
    } else if (!kstd::strcmp(cmd, "tune")) {
        printk("Playing tune on PC speaker...\n");
        speaker::play_tune();
    } else if (!kstd::strcmp(cmd, "memdump")) {
        cmd_memdump(argc, argv);
    } else if (!kstd::strcmp(cmd, "panic")) {
        cmd_panic(argc, argv);
    } else if (!kstd::strcmp(cmd, "matrix")) {
        cmd_matrix();
    } else if (!kstd::strcmp(cmd, "snake")) {
        cmd_snake();
    } else if (!kstd::strcmp(cmd, "gpu") || !kstd::strcmp(cmd, "driver")) {
        cmd_gpu();
    } else if (!kstd::strcmp(cmd, "gui") || !kstd::strcmp(cmd, "desktop")) {
        cmd_gui(argc, argv);
    } else if (!kstd::strcmp(cmd, "browser") || !kstd::strcmp(cmd, "web")) {
        cmd_browser(argc, argv);
    } else if (!kstd::strcmp(cmd, "demo") || !kstd::strcmp(cmd, "gfx")) {
        cmd_demo(argc, argv);
    } else if (!kstd::strcmp(cmd, "ifconfig") || !kstd::strcmp(cmd, "ip")) {
        net::cmd_ifconfig(argc, argv);
    } else if (!kstd::strcmp(cmd, "wifi") || !kstd::strcmp(cmd, "wlan")) {
        wifi::cmd_wifi(argc, argv);
    } else if (!kstd::strcmp(cmd, "route")) {
        wifi::cmd_route(argc, argv);
    } else if (!kstd::strcmp(cmd, "ping")) {
        net::cmd_ping(argc, argv);
    } else if (!kstd::strcmp(cmd, "arp")) {
        net::cmd_arp(argc, argv);
    } else if (!kstd::strcmp(cmd, "netstat")) {
        net::cmd_netstat(argc, argv);
    } else if (!kstd::strcmp(cmd, "curl") || !kstd::strcmp(cmd, "http")) {
        net::cmd_curl(argc, argv);
    } else if (!kstd::strcmp(cmd, "reboot")) {
        cmd_reboot();
    } else if (!kstd::strcmp(cmd, "shutdown") || !kstd::strcmp(cmd, "poweroff") || !kstd::strcmp(cmd, "exit")) {
        cmd_shutdown();
    } else {
        vga::set_color(vga::Color::LightRed, vga::Color::Black);
        printk("Unknown command '%s'. Type 'help' for available commands.\n", cmd);
        vga::set_color(vga::Color::White, vga::Color::Black);
    }
}

// Block until a character is available from either the PS/2 keyboard or the
// serial console, so the shell can be driven headlessly over a serial link.
static char input_getchar() {
    while (true) {
        if (keyboard::has_char()) return keyboard::get_char();
        if (serial::has_char()) return serial::read_char();
        asm volatile("hlt");
    }
}

static void read_line_with_history(char* buffer, size_t max_len) {
    size_t index = 0;
    int history_browse_idx = static_cast<int>(history_count);
    char current_input_stash[128] = "";

    auto clear_line = [&](size_t count) {
        for (size_t i = 0; i < count; ++i) {
            vga::put_char('\b');
            serial::write("\b \b");
        }
    };

    while (true) {
        char c = input_getchar();

        if (c == '\n' || c == '\r') {
            vga::put_char('\n');
            serial::write("\n");
            buffer[index] = '\0';
            break;
        } else if (c == '\b') {
            if (index > 0) {
                index--;
                vga::put_char('\b');
                serial::write("\b \b");
            }
        } else if (c == static_cast<char>(keyboard::KEY_UP)) {
            if (history_count > 0 && history_browse_idx > 0) {
                if (history_browse_idx == static_cast<int>(history_count)) {
                    buffer[index] = '\0';
                    kstd::strcpy(current_input_stash, buffer);
                }
                history_browse_idx--;
                clear_line(index);
                kstd::strcpy(buffer, command_history[history_browse_idx]);
                index = kstd::strlen(buffer);
                printk("%s", buffer);
                serial::write(buffer);
            }
        } else if (c == static_cast<char>(keyboard::KEY_DOWN)) {
            if (history_browse_idx < static_cast<int>(history_count)) {
                history_browse_idx++;
                clear_line(index);
                if (history_browse_idx == static_cast<int>(history_count)) {
                    kstd::strcpy(buffer, current_input_stash);
                } else {
                    kstd::strcpy(buffer, command_history[history_browse_idx]);
                }
                index = kstd::strlen(buffer);
                printk("%s", buffer);
                serial::write(buffer);
            }
        } else if (c >= 32 && c <= 126) {
            if (index < max_len - 1) {
                buffer[index++] = c;
                vga::put_char(c);
                serial::write_char(c);
            }
        }
    }
}

void run() {
    char input_buf[128];

    while (true) {
        vga::set_color(vga::Color::LightGreen, vga::Color::Black);
        printk("aether");
        vga::set_color(vga::Color::White, vga::Color::Black);
        printk("@");
        vga::set_color(vga::Color::LightCyan, vga::Color::Black);
        printk("x86_64");
        vga::set_color(vga::Color::White, vga::Color::Black);
        printk(":~$ ");

        read_line_with_history(input_buf, sizeof(input_buf));
        execute_line(input_buf);
    }
}

} // namespace shell
