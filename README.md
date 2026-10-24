# AetherOS 64-bit Modern Bare-Metal Kernel in C++

AetherOS is a modern, modular 64-bit operating system kernel written from scratch in freestanding **C++20** and **x86_64 Assembly**. It transitions the CPU from 32-bit Multiboot into 64-bit Long Mode with 4-level paging, sets up modern hardware abstractions (GDT, TSS, IDT, PIC, PIT, PS/2 Keyboard, Physical Memory Manager, Dynamic Kernel Heap), and provides an interactive terminal shell with diagnostic utilities, memory viewers, and games.

---

## Architecture & Features

```
+-------------------------------------------------------------------------+
|                  Interactive Kernel Shell (CLI & Games)                 |
|             (help, info, mem, calc, memdump, snake, matrix, panic)       |
+------------------------------------+------------------------------------+
|        Kernel Heap Allocator       |      Physical Memory Manager       |
|    (malloc/free, new/delete, 16MB) |     (4KB Frame Bitmap Allocator)   |
+------------------------------------+------------------------------------+
|            8254 PIT Timer          |       PS/2 Keyboard & UART Serial  |
|         (IRQ0, 100Hz Ticks)        |         (IRQ1 & COM1 Debug Log)    |
+------------------------------------+------------------------------------+
|         8259 PIC Remapper          |     Interrupt Descriptor Table     |
|      (IRQ 0..15 -> INT 32..47)     |    (64-bit IDT + Panic Dump)       |
+------------------------------------+------------------------------------+
|    Global Descriptor Table (GDT)   |        4-Level Paging (PML4)       |
|     (64-bit Code, Data, TSS)       |    (1GB Identity Map via 2MB Pages)|
+------------------------------------+------------------------------------+
|                     x86_64 Long Mode Bootstrap                          |
|             (Multiboot 1 / GRUB / CPUID / EFER.LME / CR0.PG)            |
+-------------------------------------------------------------------------+
```

### Key Subsystems:
1. **Bootstrapping & Long Mode Switch**:
   - Multiboot 1 compatible header (bootable directly by QEMU `-kernel` or GRUB).
   - Dynamic CPUID validation and Long Mode (`LM`) detection.
   - 4-level paging initialization (PML4, PDPT, Page Directories with 2MB huge pages) identity mapping the first 4 GB of physical memory.
   - Enabling PAE, EFER.LME, CR0 Paging & Protected Mode, and jumping to 64-bit Long Mode.
2. **64-bit GDT & TSS**:
   - 64-bit Kernel Code (`0x08`), Kernel Data (`0x10`), User Data (`0x18`), User Code (`0x20`), and 16-byte Task State Segment (TSS, `0x28`).
3. **64-bit IDT & Exception Handling**:
   - 256 interrupt gates with assembly ISR stubs saving all 64-bit registers.
   - Comprehensive crash screen / register dump on CPU exceptions (Page Fault `#PF` with CR2, Divide by Zero `#DE`, General Protection `#GP`, etc.).
4. **Hardware Drivers**:
   - **Dual Console**: Simultaneous output to colorful VGA 80x25 text buffer (`0xB8000`) and 16550 UART Serial (`0x3F8`).
   - **Bochs/QEMU Graphics Adapter (BGA)**: High-resolution hardware graphics driver accessing I/O ports `0x1CE`/`0x1CF` and resolving PCI BAR0 linear framebuffer. Supports up to 1280x720 and 1024x768 32-bit TrueColor (ARGB8888).
   - **2D Graphics & Font Engine**: Double-buffered framebuffer engine with Bresenham lines, rectangles, vertical gradients, circles, and 8x16 bitmap text typography.
   - **8259 PIC**: Remapped IRQs 0-15 to vectors 32-47.
   - **8254 PIT**: Configured at 100 Hz for system ticks, uptime calculation, and sub-millisecond sleeps.
   - **CMOS RTC**: Real-Time Clock reader via ports `0x70`/`0x71` with BCD conversion.
   - **PC Speaker Driver**: Square-wave frequency synthesis using PIT channel 2 and port `0x61`.
   - **PS/2 Keyboard Driver**: Interrupt-driven ring buffer with full US QWERTY keymap (Shift, CapsLock, Ctrl, Alt, Enter, Backspace).
5. **Memory Management**:
   - **4GB Identity Paging**: 4-level paging (PML4 -> PDPT -> 4 Page Directories) mapping the full 4GB 32-bit physical address space for PCI linear framebuffers and MMIO.
   - **Physical Memory Manager (PMM)**: Bitmap allocator tracking 4KB frames parsed directly from the Multiboot memory map.
   - **Kernel Heap Allocator**: Freelist allocator with block headers, magic validation, and block coalescing. Provides standard `malloc`/`free` and C++ `new`/`delete`/`new[]`/`delete[]`.
6. **Audio & Sound Driver Subsystem**:
   - **Intel AC97 PCI Audio Controller (Vendor 0x8086, Device 0x2415)**: Bus Master DMA audio playback with aligned 32-entry Buffer Descriptor Lists (BDL), 16-bit stereo PCM streaming @ 48000 Hz, master/PCM volume attenuation, and hardware channel muting.
   - **Creative Sound Blaster 16 & PC Speaker Fallback**: Multi-tier fallback architecture ensuring audio support across modern AC97 hardware, legacy Sound Blaster 16 DSPs (port `0x220`), and retro square-wave PC speaker tones (`0x61`).
   - **Sound Engine & Synthesizer**: Procedural sound effects generator (`beep`, crystalline bell `chime`, pitch-swept `laser`, retro 8-bit `coin` pickup, victory `fanfare`), pure sine/square/triangle/sawtooth wave synthesis, and polyphonic chords.
   - **WAV Audio Player & RamFS Sample Library**: Embedded RIFF WAVE parser capable of decoding and playing 8-bit/16-bit mono/stereo PCM WAV files resampled to 48kHz, pre-seeded with `chime.wav` and `beep.wav` in RamFS.
   - **Desktop GUI Audio Feedback Cues**: Subtle, non-blocking auditory cues triggered on window minimize, maximize/restore, and terminal command execution.
7. **Wi-Fi Subsystem & Automatic Ethernet Priority Engine**:
   - **802.11ax Wi-Fi 6 Wireless Subsystem (`wlan0`)**: Active 2.4 GHz / 5.0 GHz channel scanning (`wifi scan`) discovering nearby BSSIDs, signal levels (`[||||]` meters and RSSI in dBm), frequency bands, and security standards (WPA3-SAE, WPA2-PSK, OPEN).
   - **Wireless Connection & 4-Way Handshake (`wifi connect <ssid> [pass]`)**: Association, EAPOL 4-way key handshake (PTK/GTK derivation), and DHCP leasing (`192.168.1.105/24`).
   - **Automatic Ethernet Priority & Warm Standby Failover**: Carrier auto-detection via Intel e1000 PHY link register (`e1000::has_link()`). When an Ethernet cable is plugged in, the kernel automatically assigns `eth0` as the primary default route (Metric 100), placing Wi-Fi on warm standby (Metric 600). If the Ethernet cable is unplugged, the kernel seamlessly and automatically fails over to `wlan0`.
   - **Routing & Dual-Interface Status (`route`, `ifconfig`)**: Displays kernel routing table with interface metrics, carrier detection status, and live taskbar network badge (`[ ETH: 1Gbps ]` / `[ WiFi: <SSID> ]`).
8. **Graphical Desktop & Interactive Shell**:
   - **Desktop Environment (`gui`)**: High-performance Full HD (1920x1080) desktop window manager with sub-millisecond dirty-rect redraws, high refresh rate frame pacing (60Hz..240Hz..MAX FPS), interactive terminal, and 3D rotating wireframe engine.
   - **Rich CLI Shell**: Interactive command parser with files (`ls`, `cat`, `touch`, `write`, `rm`), audio controls (`sound`, `volume`, `play`, `wavplay`, `mute`), networking (`ifconfig`, `wifi`, `route`, `ping`, `curl`), and games.
9. **Native Web Browser Engine (`browser`)**:
   - Streaming HTML tokenizer and layout builder: headings, paragraphs, lists, tables, forms, buttons, images (placeholders), `center`, `blockquote`, `font` and `app://` bundles.
   - CSS 2.1 subset: tag/class/id selectors, colours, borders, padding, percentage and `max-width` sizing, `margin: auto` centring, `text-align`, font weight/style and display.
   - Scaled type rendering (1x-6x from `font-size` and heading levels) plus vertical `linear-gradient()` backgrounds for elements and pages.
   - Legacy presentational HTML (`align`, `bgcolor`, `<font color>`) still honoured for older pages.
   - Inline JavaScript via the compact AetherScript engine: arrow functions, closures, `switch`, array iteration/`map`/`filter`/`reduce`/`sort`/`splice`, `Object`/`Array`/`JSON` helpers, Math, string padding/repeat methods, and `document.write`/`console` integration.
   - Relative URL resolution (`../x`, `sub/x`, `/x`, `//host/x`, query/fragment) for links and HTTP redirects, plus back/forward history.
   - UTF-8 source folding to ASCII (smart quotes, dashes, accents) with HTML entity decoding.
   - HTTP/1.1 client with redirects, chunked transfer decoding and gzip/deflate decompression.
   - BearSSL HTTPS/TLS 1.2 client with SNI, ALPN (`http/1.1`), full X.509 chain validation against 121 bundled CA roots, hostname checking and certificate validity/expiry checks, plus HTTPS redirect following.
   - Real Google homepage via its static HTML fallback (`?gbv=1&hl=en`), served over validated HTTPS.
   - Headless debugging: `browser dump <url>`, `browser links <url>` and `browser boxes <url>` (prints scale and gradient flags).

---

## Directory Structure

```
aether-os/
├── Makefile                        # Build automation (bin, elf32, iso, qemu targets)
├── linker.ld                       # 64-bit ELF linker script (1MB base, 4KB page align)
├── iso/
│   └── boot/grub/grub.cfg          # GRUB configuration for bootable ISO
└── src/
    ├── arch/x86_64/
    │   ├── boot.asm                # 32-bit bootstrap, paging setup & 64-bit long jump
    │   ├── isr.asm                 # 64-bit interrupt stubs and iretq
    │   └── io.hpp                  # Inline port I/O (inb, outb, inw, outw, inl, outl)
    └── kernel/
        ├── types.hpp               # Freestanding C++ fixed-width types & placement new
        ├── multiboot.hpp           # Multiboot specification data structures
        ├── kstring.hpp / .cpp      # Freestanding string & memory functions (memset, memcpy, itoa...)
        ├── serial.hpp / .cpp       # 16550 UART COM1 serial driver
        ├── vga.hpp / .cpp          # VGA 80x25 text mode driver with color and scrolling
        ├── printk.hpp / .cpp       # Dual VGA + Serial formatted printer
        ├── gdt.hpp / .cpp          # 64-bit GDT & TSS management
        ├── idt.hpp / .cpp          # 64-bit IDT & exception dump handlers
        ├── pic.hpp / .cpp          # 8259 PIC remapping and EOI
        ├── pit.hpp / .cpp          # 8254 PIT timer driver (ticks, sleep, uptime)
        ├── keyboard.hpp / .cpp     # PS/2 keyboard driver & circular buffer
        ├── mouse.hpp / .cpp        # PS/2 mouse driver on IRQ12 with packet decoder
        ├── pmm.hpp / .cpp          # 4KB Physical frame allocator
        ├── heap.hpp / .cpp         # Dynamic heap & C++ operator new/delete
        ├── rtc.hpp / .cpp          # CMOS Real-Time Clock
        ├── pci.hpp / .cpp          # PCI Bus scanner & configuration reader
        ├── ac97.hpp / .cpp         # Intel AC97 PCI Audio Controller (Bus Master DMA)
        ├── sb16.hpp / .cpp         # Sound Blaster 16 DSP & Mixer driver
        ├── speaker.hpp / .cpp      # PC Speaker frequency synthesizer
        ├── sound.hpp / .cpp        # Audio subsystem manager, synthesizer & WAV player
        ├── ramfs.hpp / .cpp        # Virtual in-memory filesystem (with sample WAVs)
        ├── bga.hpp / .cpp          # Bochs/QEMU BGA linear framebuffer driver
        ├── font.hpp / .cpp         # Embedded 8x16 VGA bitmap typography
        ├── gfx.hpp / .cpp          # Double-buffered 2D graphics primitive engine
        ├── gui.hpp / .cpp          # 1024x768 desktop GUI, cursor pointer & 3D wireframe
        ├── e1000.hpp / .cpp        # Intel 82540EM Gigabit Ethernet NIC driver
        ├── net.hpp / .cpp          # AetherOS Network Protocol Stack
        ├── wifi.hpp / .cpp         # 802.11ax Wi-Fi scanner, connection & auto-Ethernet failover
        ├── shell.hpp / .cpp        # Interactive shell, diagnostic tools & Snake game
        └── main.cpp                # Kernel entry point and initialization sequence
```

---

## Building and Running

### Prerequisites
On Arch Linux or Ubuntu/Debian, ensure you have:
- `g++` (GCC C++ compiler with x86_64 support)
- `nasm` (Netwide Assembler)
- `qemu-system-x86_64` (QEMU emulator)
- `xorriso` and `grub-mkrescue` (optional, for ISO generation)

### Compilation
From the project directory:
```bash
make
```
This produces:
- `build/aetheros.bin` - Pure 64-bit ELF kernel
- `build/aetheros32.bin` - Multiboot 32-bit wrapped ELF for fast direct QEMU booting
- `build/aetheros.iso` - Bootable CD-ROM ISO image created with GRUB

### Running in QEMU

1. **Standard Graphical Window (with Serial in Terminal)**:
   ```bash
   make run
   ```
2. **Boot from ISO Image**:
   ```bash
   make run-iso
   ```
3. **Headless / Serial-only Console (ideal for SSH or terminal sessions)**:
   ```bash
   make run-nographic
   ```
4. **Terminal Curses Display**:
   ```bash
   make run-curses
   ```

### Resolution, Refresh Rate & Window Management

AetherOS features a professional, high-performance desktop window manager optimized for Full HD (1920x1080) and high refresh rate displays (up to 240Hz and unlocked MAX FPS):

- **Interactive Selection**: Type `gui` or `desktop` without arguments to open an interactive resolution selector. Press <kbd>Enter</kbd> to launch **1920x1080 @ 144Hz** by default.
- **Direct 1080p Launch**:
  ```bash
  gui 1920 1080 144     # 1080p Full HD at 144 Hz
  gui 1920 1080 240     # 1080p Full HD at 240 Hz
  gui 1920 1080 60      # 1080p Full HD at 60 Hz
  desktop 1080p         # Quick 1080p preset
  desktop max           # Unlocked MAX FPS preset (300-500+ FPS)
  gui 1280 720          # 720p HD
  ```
- **Dual-Style Window Controls (macOS & Windows)**:
  - **Left Traffic Lights (macOS style)**:
    - Red Circle `[×]`: **Close Window** (hides window, marks taskbar as `x <Name>`)
    - Yellow Circle `[−]`: **Minimize Window** (minimizes to taskbar as `_ <Name>`)
    - Green Circle `[+]`: **Maximize / Restore Toggle** (expands window to full workspace or restores tiled geometry)
  - **Right Controls (Windows style)**:
    - Minimize Button `[_]`: Minimizes window to taskbar
    - Maximize Button `[□]`: Toggles full workspace maximize or restore
    - Close Button `[✕]`: Closes window
  - **Double-Click Titlebar**: Double-clicking any window's titlebar toggles between maximized full workspace and restored tiled view.
  - **Z-Order Stacking**: Clicking anywhere on an inactive window or its titlebar brings it immediately to the front of the focus stack with active titlebar glow.
  - **Interactive Taskbar**:
    - `* <Name>` (Active / Front): Clicking minimizes the window.
    - `  <Name>` (Background): Clicking brings it to the front.
    - `_ <Name>` (Minimized): Clicking restores the window to the front.
    - `x <Name>` (Closed): Clicking reopens and focuses the window.

- **Fully Interactive GUI Terminal (Window 2)**:
  - When Window 2 is focused, keyboard keystrokes are routed directly into the terminal window.
  - Interactive line editing with printable ASCII, Backspace, Enter, and blinking cursor (`_`) at `aether@x86_64:~$ `.
  - 12 supported in-GUI commands:
    - `help`: Display help and command summary
    - `fetch` / `fastfetch`: Display ASCII system logo, kernel version, RAM & Heap meters
    - `ls`: List files in RAM filesystem with sizes
    - `cat <file>`: View file contents
    - `mem`: Physical RAM and kernel heap allocation statistics
    - `uptime`: System uptime, elapsed seconds, and timer ticks
    - `date` / `rtc`: Real-Time Clock UTC timestamp
    - `bench`: Run 64-bit integer ALU compute benchmark (Mops/sec)
    - `calc <expr>`: Multi-operator integer calculator (`+`, `-`, `*`, `/`, `%`)
    - `echo <text...>`: Print text to terminal
    - `clear`: Clear the terminal screen buffer
    - `exit`: Close/minimize the terminal window
  - Press <kbd>ESC</kbd> when input buffer is empty to release focus back to the desktop.

- **Sub-Millisecond Dirty-Rectangle Graphics Engine & 3D Bounding-Box Tracking**:
  - **3D Engine Optimization**: Eliminates the 5MB per-frame transfer bottleneck on Window 3 ($1230\times 1010$ px). Projects 3D vertices into a dynamic bounding box ($\approx 350\times 350$ px), erasing only the union bounding box between frames and copying only the dirty box to VRAM. Reduces MMIO bus bandwidth by **over 90%** ($490\text{ KB}$ vs $4.97\text{ MB}$ per frame), maintaining rock-solid **144Hz+ / 240Hz / 300+ MAX FPS** without any framerate drop.
  - **Decoupled HUD Updates**: Static window elements and bottom HUD text (`Pipeline: ...`) only redraw and flush when measured FPS changes (2 Hz vs 144 Hz), eliminating 98.6% of HUD bus writes.
  - **Clean Real-Time Clock**: Fixed digit smudging by rendering the RTC badge with background fill and `transparent_bg = false`, flushing only when the second advances.
  - **Zero-Overhead Mouse Movement**: Cursor background (28x22 pixels) is saved in RAM and restored in < 1 µs, completely bypassing full-screen redraws while moving the mouse.
  - **Frame Pacing**: Hardware PIT timer pacer maintains steady 60Hz, 120Hz, 144Hz, 240Hz, or unthrottled MAX FPS (reaching 300-500+ FPS).
  - Press <kbd>B</kbd> to toggle Menu Bar position (**Top** $\leftrightarrow$ **Bottom**)
  - Press <kbd>T</kbd> to toggle Button Style (**Circles** $\leftrightarrow$ **Squares**)
  - Press <kbd>R</kbd> or <kbd>Tab</kbd> to cycle refresh rate: `60 Hz -> 120 Hz -> 144 Hz -> 240 Hz -> MAX FPS`
  - Press <kbd>1</kbd>, <kbd>2</kbd>, <kbd>3</kbd>, <kbd>4</kbd>, <kbd>0</kbd> for direct refresh rate presets
  - Drag window titlebars across the full 1920x1080 screen with your mouse
  - Press <kbd>Q</kbd>, <kbd>ESC</kbd>, or click `> Exit to Console` in the Start Menu to cleanly return to VGA text mode

---

## Interactive Shell Commands

Once booted, AetherOS presents an interactive command line `aether@x86_64:~$ `:

| Command | Description |
| :--- | :--- |
| `help` | Displays the full list of available commands and instructions |
| `fetch` / `neofetch` | Fastfetch / Neofetch system overview with ASCII art, hardware specs, and color palette |
| `info` | Prints CPU Vendor & Model (CPUID), 64-bit Long Mode status, and uptime |
| `date` / `rtc` | Reads CMOS hardware clock to display current UTC date & time |
| `lspci` | Scans PCI bus configuration space and lists all PCI hardware devices |
| `features` | Tests CPU capabilities via CPUID (FPU, SSE, AVX, APIC, RDRAND, etc.) |
| `bench` | Runs CPU integer compute benchmark and memory bandwidth speed test (MB/s) |
| `mem` | Displays Physical RAM frames (total/used/free) and Kernel Heap statistics |
| `uptime` | Shows time elapsed since boot in hours, minutes, seconds, and milliseconds |
| `ls` | Lists files in the in-memory RAM filesystem (ramfs) |
| `cat <file>` | Displays file contents from ramfs |
| `touch <file>` | Creates a new file in ramfs |
| `write <file> <text...>` | Writes or updates text in a file |
| `rm <file>` | Deletes a file from ramfs |
| `history` | Displays list of previously executed shell commands |
| `calc <a> <op> <b>` | Evaluates arithmetic expressions (`+`, `-`, `*`, `/`, `%`) |
| `rand [min] [max]` | Generates hardware/TSC pseudo-random numbers |
| `echo [text...]` | Prints text back to console |
| `color <fg> [bg]` | Changes console colors (e.g. `color green black`, `color cyan blue`) |
| `memdump <addr> [n]`| Hex dumps memory contents at given physical/virtual address |
| `beep [freq] [ms]` | Emits tone on the PC speaker using PIT channel 2 |
| `tune` | Plays an 8-bit retro victory melody on the PC speaker |
| `sound [cues on|off]` | Shows sound card hardware status (AC97 / SB16 / PC Speaker), volume, and audio cues |
| `volume <0-100>` | Sets or queries master output volume (0-100%) |
| `mute` / `unmute` | Mutes or unmutes master audio output |
| `play <effect>` | Plays procedural sound effects (`beep`, `chime`, `laser`, `coin`, `fanfare`, `tone`, `chord`, `melody`) |
| `wavplay <file.wav>` | Parses and plays 8-bit or 16-bit PCM WAV audio file from RamFS (e.g. `chime.wav`, `beep.wav`) |
| `snake` | Launches the interactive arcade Snake game in VGA text mode |
| `matrix` | Launches digital falling green code rain (press any key to exit) |
| `panic <type>` | Safely triggers a CPU exception (`div`, `page`, `gp`) to test IDT crash screen |
| `gui [w] [h] [fps]` | Launches Desktop GUI (supports 1920x1080 Full HD, 144Hz/240Hz, mouse cursor, window dragging & 3D engine) |
| `demo [w] [h]` | Displays high-resolution 2D graphics primitive engine test pattern |
| `reboot` | Restarts the machine via keyboard controller reset |
| `shutdown` | Powers down the system / QEMU cleanly via ACPI |

---

## App Hub & Independent App Updates

The native desktop **App Hub** (dock / Start menu) is the single launcher for
all applications. Its built-in section lists Terminal, Browser, 3D Engine,
Monitor, Task Manager, Settings and App Hub itself; the catalogue section lists
installable HTML/CSS/JS bundles stored in **AetherFS** and run by the native
browser engine. Bundles install and update without reflashing or updating the
kernel, and can also be opened directly with `app://<name>`.

The hub fetches its catalogue from the same plain-HTTP update host configured
with `update set <host> <port> <path>`:

```
<host>/apps/catalog.txt

# name|version|path|description
notes|1.0.0|/apps/notes.html|Notes and scratchpad
calc|1.0.0|/apps/calc.html|Simple calculator
```

Installed bundles are stored as `app.<name>.html` with the version recorded in
`app.<name>.ver`, so the hub can offer targeted app updates and the files
survive reboots.

| Command | Description |
| :--- | :--- |
| `apps` | List known apps, versions and installed/update status |
| `apps catalog` | Download and parse the app catalogue from the update host |
| `apps install <name>` | Download and install/update a single app |
| `apps update` | Install or update every app in the catalogue |
| `browser app://<name>` | Open an installed app in the browser (GUI) |

---

## Launch & Development

### Boot the OS (QEMU)

```bash
# Simple launch (default: 4GB RAM, std VGA, serial console)
./run

# Custom memory
AETHEROS_MEM=8192M ./run

# Fullscreen
AETHEROS_FULLSCREEN=true ./run
```

Or with `make`:

```bash
make run          # Standard launch
make run-bga      # Standard VGA with VRAM override
make run-vmware   # VMware SVGA adapter
make run-curses   # Text-based curses UI
make run-iso      # Boot from ISO image
```

### Updating the OS

```bash
# Build new kernel
make all

# Push to GitHub (for updates)
git add -A && git commit -m "update" && git push origin main

# Configure updater (in OS shell)
update github /release/aetheros.bin

# Check for updates
update check

# Apply update (downloads + installs new kernel)
update submit
```

### Files

| Path | Description |
| --- | --- |
| `release/aetheros.bin` | 64-bit multiboot kernel |
| `release/aetheros32.bin` | 32-bit multiboot kernel |
| `release/aetheros.iso` | Bootable ISO image |
| `tools/mkdisk.sh` | Creates `build/disk.img` (128MB) with GRUB + FAT16 |

---

## Contributing

...
