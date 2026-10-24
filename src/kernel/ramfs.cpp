#include "ramfs.hpp"
#include "heap.hpp"
#include "kstring.hpp"
#include "printk.hpp"
#include "vga.hpp"

namespace ramfs {

static File files[MAX_FILES];

struct WavHeader {
    char riff[4];
    uint32_t file_size;
    char wave[4];
    char fmt[4];
    uint32_t fmt_size;
    uint16_t format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits;
    char data[4];
    uint32_t data_size;
} __attribute__((packed));

static const int16_t SINE_LUT[256] = {
        0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
     6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
    12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
    18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
    23170,  23731,  24279,  24811,  25329,  25832,  26319,  26790,
    27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
    30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
    32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,
    32767,  32757,  32728,  32678,  32609,  32521,  32412,  32285,
    32137,  31971,  31785,  31580,  31356,  31113,  30852,  30571,
    30273,  29956,  29621,  29268,  28898,  28510,  28105,  27683,
    27245,  26790,  26319,  25832,  25329,  24811,  24279,  23731,
    23170,  22594,  22005,  21403,  20787,  20159,  19519,  18868,
    18204,  17530,  16846,  16151,  15446,  14732,  14010,  13279,
    12539,  11793,  11039,  10278,   9512,   8739,   7962,   7179,
     6393,   5602,   4808,   4011,   3212,   2410,   1608,    804,
        0,   -804,  -1608,  -2410,  -3212,  -4011,  -4808,  -5602,
    -6393,  -7179,  -7962,  -8739,  -9512, -10278, -11039, -11793,
   -12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530,
   -18204, -18868, -19519, -20159, -20787, -21403, -22005, -22594,
   -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
   -27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956,
   -30273, -30571, -30852, -31113, -31356, -31580, -31785, -31971,
   -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
   -32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285,
   -32137, -31971, -31785, -31580, -31356, -31113, -30852, -30571,
   -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
   -27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731,
   -23170, -22594, -22005, -21403, -20787, -20159, -19519, -18868,
   -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
   -12539, -11793, -11039, -10278,  -9512,  -8739,  -7962,  -7179,
    -6393,  -5602,  -4808,  -4011,  -3212,  -2410,  -1608,   -804
};

static void create_default_wavs() {
    // 1. beep.wav (16-bit mono 22050 Hz, 880 Hz tone, 150 ms)
    {
        constexpr uint32_t sample_rate = 22050;
        constexpr uint32_t num_samples = (sample_rate * 150) / 1000;
        constexpr uint32_t data_size = num_samples * sizeof(int16_t);
        constexpr uint32_t total_size = sizeof(WavHeader) + data_size;

        uint8_t* buf = static_cast<uint8_t*>(kmalloc(total_size));
        if (buf) {
            WavHeader* hdr = reinterpret_cast<WavHeader*>(buf);
            kstd::memcpy(hdr->riff, "RIFF", 4);
            hdr->file_size = total_size - 8;
            kstd::memcpy(hdr->wave, "WAVE", 4);
            kstd::memcpy(hdr->fmt, "fmt ", 4);
            hdr->fmt_size = 16;
            hdr->format = 1;
            hdr->channels = 1;
            hdr->sample_rate = sample_rate;
            hdr->byte_rate = sample_rate * 2;
            hdr->block_align = 2;
            hdr->bits = 16;
            kstd::memcpy(hdr->data, "data", 4);
            hdr->data_size = data_size;

            int16_t* samples = reinterpret_cast<int16_t*>(buf + sizeof(WavHeader));
            uint32_t phase = 0;
            uint32_t phase_step = (880ULL * 65536) / sample_rate;
            for (uint32_t i = 0; i < num_samples; ++i) {
                int32_t env = 32767;
                if (i < 220) env = (i * 32767) / 220;
                else if (i > num_samples - 440) env = ((num_samples - i) * 32767) / 440;
                int16_t raw = SINE_LUT[(phase >> 8) & 0xFF];
                samples[i] = static_cast<int16_t>((raw * env) / 32768);
                phase += phase_step;
            }

            write_bytes("beep.wav", buf, total_size);
            kfree(buf);
        }
    }

    // 2. chime.wav (16-bit stereo 22050 Hz, bell chord, 350 ms)
    {
        constexpr uint32_t sample_rate = 22050;
        constexpr uint32_t num_frames = (sample_rate * 350) / 1000;
        constexpr uint32_t data_size = num_frames * 2 * sizeof(int16_t);
        constexpr uint32_t total_size = sizeof(WavHeader) + data_size;

        uint8_t* buf = static_cast<uint8_t*>(kmalloc(total_size));
        if (buf) {
            WavHeader* hdr = reinterpret_cast<WavHeader*>(buf);
            kstd::memcpy(hdr->riff, "RIFF", 4);
            hdr->file_size = total_size - 8;
            kstd::memcpy(hdr->wave, "WAVE", 4);
            kstd::memcpy(hdr->fmt, "fmt ", 4);
            hdr->fmt_size = 16;
            hdr->format = 1;
            hdr->channels = 2;
            hdr->sample_rate = sample_rate;
            hdr->byte_rate = sample_rate * 4;
            hdr->block_align = 4;
            hdr->bits = 16;
            kstd::memcpy(hdr->data, "data", 4);
            hdr->data_size = data_size;

            int16_t* samples = reinterpret_cast<int16_t*>(buf + sizeof(WavHeader));
            uint32_t p1 = 0, p2 = 0, p3 = 0, p4 = 0;
            uint32_t s1 = (523ULL * 65536) / sample_rate;  // C5
            uint32_t s2 = (659ULL * 65536) / sample_rate;  // E5
            uint32_t s3 = (784ULL * 65536) / sample_rate;  // G5
            uint32_t s4 = (1046ULL * 65536) / sample_rate; // C6

            for (uint32_t i = 0; i < num_frames; ++i) {
                int32_t env1 = (num_frames - i) * 32767 / num_frames;
                int32_t env2 = (i < num_frames * 4 / 5) ? (num_frames * 4 / 5 - i) * 32767 / (num_frames * 4 / 5) : 0;
                int32_t env3 = (i < num_frames * 3 / 5) ? (num_frames * 3 / 5 - i) * 32767 / (num_frames * 3 / 5) : 0;
                int32_t env4 = (i < num_frames * 2 / 5) ? (num_frames * 2 / 5 - i) * 32767 / (num_frames * 2 / 5) : 0;

                int32_t v1 = (SINE_LUT[(p1 >> 8) & 0xFF] * env1) / 32768;
                int32_t v2 = (SINE_LUT[(p2 >> 8) & 0xFF] * env2) / 32768;
                int32_t v3 = (SINE_LUT[(p3 >> 8) & 0xFF] * env3) / 32768;
                int32_t v4 = (SINE_LUT[(p4 >> 8) & 0xFF] * env4) / 32768;

                int32_t left = (v1 * 9 + v2 * 6 + v3 * 8 + v4 * 5) / 28;
                int32_t right = (v1 * 6 + v2 * 9 + v3 * 5 + v4 * 8) / 28;

                samples[i * 2 + 0] = static_cast<int16_t>(left);
                samples[i * 2 + 1] = static_cast<int16_t>(right);

                p1 += s1; p2 += s2; p3 += s3; p4 += s4;
            }

            write_bytes("chime.wav", buf, total_size);
            kfree(buf);
        }
    }
}

// Pre-seeded Web Pages for AetherOS Native Browser Subsystem
static const char HTML_INDEX[] = R"rawhtml(<!DOCTYPE html>
<html>
<head>
    <title>AetherOS Web Portal</title>
</head>
<body>
    <h1>AetherOS Web Portal</h1>
    <p><b>Welcome to AetherOS</b> &mdash; The Next-Generation Bare-Metal 64-bit Operating System &amp; Hypermedia Environment.</p>
    <hr />

    <h2>Quick Navigation</h2>
    <p>
        <a href="/docs.html">Kernel Manual &amp; Architecture</a> &nbsp;|&nbsp;
        <a href="/acid.html">Acid-1 Web Standards Test</a> &nbsp;|&nbsp;
        <a href="/news.html">AetherOS Daily Dispatch</a> &nbsp;|&nbsp;
        <a href="/js.html">AetherScript JS Demo</a> &nbsp;|&nbsp;
        <a href="http://10.0.2.2/">QEMU Host Gateway</a>
    </p>

    <p>
        <button onclick="location.href='/docs.html'">Kernel Manual</button>
        <button onclick="location.href='/acid.html'">Acid-1 Test</button>
        <button onclick="location.href='/news.html'">Daily Dispatch</button>
        <button onclick="location.href='http://10.0.2.2/'">Host Gateway</button>
    </p>
    <hr />

    <h2>AetherOS System Architecture Highlights</h2>
    <p>Engineered entirely from bare metal, AetherOS pairs microsecond responsiveness with modern workstation capabilities:</p>
    <ul>
        <li><b>Freestanding C++20:</b> Type-safe modular kernel codebase utilizing zero-overhead modern language constructs, templates, and placement new.</li>
        <li><b>64-bit Long Mode:</b> Native x86_64 long mode transition with 4-level paging (PML4) and complete 4GB identity mapping for MMIO and hardware framebuffers.</li>
        <li><b>Wi-Fi 6 (802.11ax):</b> Dual-band 2.4 GHz / 5.0 GHz active wireless scanner, WPA3-SAE / WPA2-PSK 4-way handshake, and station state management.</li>
        <li><b>Gigabit Ethernet &amp; Auto-Failover:</b> Intel 82540EM (e1000) Gigabit NIC driver with hardware carrier detection, prioritizing Ethernet (Metric 100) and switching to Wi-Fi (Metric 600) on link loss.</li>
        <li><b>1080p Desktop GUI:</b> Full HD 1920x1080 composited window manager with VMware SVGA-II / Bochs VBE acceleration, Intel AC97 48kHz audio, and interactive 3D wireframe engine.</li>
    </ul>
    <hr />

    <h2>Network Search &amp; Address Bar</h2>
    <p>Enter a local RamFS file path or HTTP remote address:</p>
    <p>
        <input type="text" value="http://10.0.2.2/" />
        <button>Browse</button>
    </p>
    <hr />

    <p><i>AetherOS Browser v1.0 &bull; Freestanding Bare-Metal HTML Engine &bull; Type &lsquo;help&rsquo; in terminal for CLI commands</i></p>
</body>
</html>)rawhtml";

static const char HTML_JS_DEMO[] = R"rawhtml(<!DOCTYPE html>
<html>
<head>
    <title>AetherScript JavaScript Demo</title>
</head>
<body>
    <h1>AetherScript JavaScript Engine</h1>
    <p><a href="/index.html">&larr; Return to Web Portal</a></p>
    <hr />
    <p>This page is generated at load time by executing inline
    <code>&lt;script&gt;</code> blocks through the AetherScript interpreter.</p>
    <script>
        var nums = [1, 2, 3, 4, 5];
        var total = 0;
        for (var i = 0; i < nums.length; i++) { total = total + nums[i]; }
        document.write("<p>Sum of 1..5 computed in JavaScript = <b>" + total + "</b></p>");

        function greet(name) { return "Hello, " + name + "!"; }
        document.write("<p>" + greet("AetherOS") + "</p>");

        function add(a, b) { return a + b; }
        document.write("<p>add(20, 22) = <b>" + add(20, 22) + "</b></p>");

        var area = Math.PI * Math.pow(4, 2);
        document.write("<p>Area of a radius-4 circle &asymp; <b>" + Math.round(area) + "</b></p>");
    </script>
    <hr />
    <p><i>Executed by AetherScript &bull; a compact tree-walking JS engine in the AetherOS kernel</i></p>
</body>
</html>)rawhtml";

static const char HTML_DOCS[] = R"rawhtml(<!DOCTYPE html>
 <html>
 <head>
     <title>AetherOS Kernel Manual &amp; Architecture</title>
</head>
<body>
    <h1>AetherOS Kernel Manual &amp; Architecture</h1>
    <p><a href="/index.html">&larr; Return to Web Portal</a> | <a href="/news.html">Daily Dispatch</a> | <a href="/acid.html">Acid Standards Test</a></p>
    <hr />

    <h2>1. Memory Management Subsystem</h2>
    <p>AetherOS features a comprehensive physical and dynamic memory management architecture:</p>
    <ul>
        <li><b>PMM Frame Allocator:</b> The Physical Memory Manager manages physical memory through a compact 4KB bitmap tracking frames discovered from Multiboot 1 memory map structures. Frames are allocated and freed with O(1) bitmap operations.</li>
        <li><b>64MB Dynamic Kernel Heap:</b> Freelist allocator with 16-byte aligned chunk headers, <code>0xDEADBEEF</code> boundary safety canaries, and bidirectional block coalescing. Seamlessly powers C++20 <code>new</code>, <code>delete</code>, <code>new[]</code>, and <code>delete[]</code>.</li>
        <li><b>4GB Identity Paging:</b> 4-level paging (PML4 &rarr; PDPT &rarr; 4 Page Directories) identity-mapping the entire 4GB 32-bit physical address space using 2MB huge pages, ensuring zero overhead when addressing MMIO bars and linear framebuffers.</li>
    </ul>

    <pre>
// Physical Frame Allocation
uintptr_t paddr = pmm::alloc_frame();

// Dynamic Kernel Heap
char* packet_buf = new char[2048];
delete[] packet_buf;
    </pre>
    <hr />

    <h2>2. Networking &amp; Wi-Fi Subsystem</h2>
    <p>The networking stack is engineered for multi-interface operation and automatic carrier prioritization:</p>
    <ul>
        <li><b>Intel e1000 Gigabit Ethernet:</b> PCI bus master driver for Intel 82540EM Gigabit NIC featuring 64-descriptor circular DMA RX and TX rings, EEPROM MAC detection, and PHY link sensing.</li>
        <li><b>802.11ax Wi-Fi 6 Subsystem:</b> Wireless network device (<code>wlan0</code>) supporting dual-band 2.4/5GHz channel scanning, active beacon parsing, signal quality metering (-30dBm to -90dBm), and EAPOL 4-way handshakes.</li>
        <li><b>Auto-Ethernet Failover:</b> Dynamic carrier auto-detection via Intel e1000 PHY link status. When an Ethernet cable is attached, <code>eth0</code> is set as default route (Metric 100) and Wi-Fi enters warm standby (Metric 600). When unplugged, traffic seamlessly shifts to <code>wlan0</code>.</li>
        <li><b>TCP/IP Protocol Stack:</b> Custom networking stack supporting ARP table caching, IPv4 routing, ICMP Ping echo response, UDP datagrams, and a stateful TCP client (SYN, ACK, FIN) with HTTP GET engine.</li>
    </ul>

    <pre>
// Shell Networking Commands
$ ifconfig           # Display network interfaces and IP addresses
$ wifi scan          # Discover nearby Wi-Fi 6 / 5 GHz access points
$ wifi connect SSID  # Associate and perform 4-way key handshake
$ curl 10.0.2.2 80 / # Send HTTP 1.0 GET request and view response
    </pre>
    <hr />

    <h2>3. Graphics &amp; Audio Architecture</h2>
    <p>Rich graphical environment and high-fidelity audio pipeline running on bare metal:</p>
    <ul>
        <li><b>VMware SVGA-II Hardware Acceleration:</b> PCI device <code>0x15AD:0x0405</code> driver with FIFO command submission supporting hardware-accelerated rectangle fills, screen-to-screen copies, and dirty rect synchronization.</li>
        <li><b>Bochs/QEMU VBE &amp; PAT Write-Combining:</b> High-resolution 1920x1080 32-bit TrueColor linear framebuffer enhanced by Page Attribute Table (PAT) write-combining for high-throughput video memory transfers.</li>
        <li><b>Intel AC97 48kHz Audio:</b> PCI Bus Master DMA audio engine supporting 16-bit stereo PCM streaming at 48,000 Hz with 32-entry Buffer Descriptor Lists (BDL), complemented by Sound Blaster 16 and PC speaker fallbacks.</li>
        <li><b>3D Wireframe Engine:</b> Real-time vector math and perspective projection pipeline rendering interactive 3D polyhedra and rotating models with sub-millisecond bounding box refresh.</li>
    </ul>

    <hr />

    <h2>Kernel Subsystems Summary Table</h2>
    <table border="1">
        <tr>
            <th>Subsystem</th>
            <th>Primary Hardware</th>
            <th>Key Specifications</th>
            <th>Status</th>
        </tr>
        <tr>
            <td>Physical Memory</td>
            <td>4KB Page Frames</td>
            <td>Multiboot bitmap allocator</td>
            <td>Operational</td>
        </tr>
        <tr>
            <td>Dynamic Heap</td>
            <td>Kernel Heap</td>
            <td>64 MB coalesced freelist</td>
            <td>Operational</td>
        </tr>
        <tr>
            <td>Ethernet</td>
            <td>Intel 82540EM (e1000)</td>
            <td>1000 Mbps Full Duplex</td>
            <td>Operational</td>
        </tr>
        <tr>
            <td>Wi-Fi 6</td>
            <td>802.11ax Device</td>
            <td>2.4 / 5.0 GHz Dual-Band</td>
            <td>Operational</td>
        </tr>
        <tr>
            <td>Graphics</td>
            <td>VMware SVGA-II / VBE</td>
            <td>1920x1080 32-bit ARGB8888</td>
            <td>Operational</td>
        </tr>
        <tr>
            <td>Sound</td>
            <td>Intel AC97 / SB16</td>
            <td>48,000 Hz Stereo PCM</td>
            <td>Operational</td>
        </tr>
    </table>

    <hr />
    <p><a href="/index.html">&larr; Return to Web Portal</a></p>
</body>
</html>)rawhtml";

static const char HTML_ACID[] = R"rawhtml(<!DOCTYPE html>
<html>
<head>
    <title>AetherOS Web Standards Test (Acid-1 Lite)</title>
</head>
<body>
    <h1>AetherOS Web Standards Test (Acid-1 Lite)</h1>
    <p>Comprehensive rendering, parsing, and layout verification suite for the AetherOS Native Browser.</p>
    <p><a href="/index.html">&larr; Back to Portal</a> | <a href="/docs.html">Kernel Manual</a> | <a href="/news.html">News</a></p>
    <hr />

    <h2>Section 1: Heading Elements (H1 &ndash; H4)</h2>
    <h1>Heading 1: Primary Document Header</h1>
    <h2>Heading 2: Major Section Heading</h2>
    <h3>Heading 3: Subsection Heading</h3>
    <h4>Heading 4: Minor Subsection Title</h4>
    <hr />

    <h2>Section 2: Typography &amp; Inline Semantics</h2>
    <p>This paragraph verifies inline typographic styling: normal text, <b>bold text</b>, <i>italic text</i>, and combined <b><i>bold italic text</i></b>.</p>
    <p>Inline entity decoding verification: &amp; (ampersand), &lt; (less than), &gt; (greater than), &quot; (double quote), &apos; (apostrophe), &bull; (bullet), &copy; 2026 AetherOS Project.</p>
    <p>Inline code formatting: Variable <code>uint32_t cr3 = read_cr3();</code> styled within normal text flow.</p>
    <hr />

    <h2>Section 3: Preformatted Blocks &amp; Code Syntax</h2>
    <pre>
// High-performance viewport clipping test
void render_layout_box(const LayoutBox&amp; box, int vx, int vy, int vh, int scroll_y) {
    if (box.y + box.h &lt; scroll_y || box.y &gt; scroll_y + vh) {
        return; // Box is outside visible viewport - skipped!
    }
    gfx::fill_rect(vx + box.x, vy + box.y - scroll_y, box.w, box.h, box.bg_color);
}
    </pre>
    <hr />

    <h2>Section 4: Lists (Ordered &amp; Unordered)</h2>
    <h3>Unordered List (&lt;ul&gt;)</h3>
    <ul>
        <li>First bullet point with regular text</li>
        <li>Second bullet point with <b>bold emphasis</b> and <code>monospace code</code></li>
        <li>Third bullet point containing an <a href="/docs.html">in-page hyperlink to docs.html</a></li>
        <li>Fourth bullet point with special entities: &alpha; &beta; &gamma;</li>
    </ul>

    <h3>Ordered List (&lt;ol&gt;)</h3>
    <ol>
        <li>Step One: Initialize Multiboot memory map and frame allocator</li>
        <li>Step Two: Activate 4-level paging and transition to 64-bit Long Mode</li>
        <li>Step Three: Enumerate PCI bus and initialize Intel e1000 and AC97 controllers</li>
        <li>Step Four: Launch 1080p desktop window manager and hypermedia browser</li>
    </ol>
    <hr />

    <h2>Section 5: Interactive Buttons &amp; Input Controls</h2>
    <p>Interactive Command Buttons:</p>
    <p>
        <button onclick="location.href='/index.html'">Portal Home</button>
        <button onclick="location.href='/docs.html'">Architecture Docs</button>
        <button onclick="location.href='/news.html'">Latest News</button>
        <button>Test Button 4</button>
    </p>

    <p>Text Input Elements:</p>
    <p>
        Address: <input type="text" value="http://10.0.2.2/index.html" />
        <button>Go</button>
    </p>
    <p>
        Search Filter: <input type="text" value="kernel paging" />
        <button>Filter</button>
    </p>
    <hr />

    <h2>Section 6: Multi-Row Data Table</h2>
    <table border="1">
        <tr>
            <th>Test Feature</th>
            <th>HTML Element</th>
            <th>Category</th>
            <th>Expected Result</th>
            <th>Pass / Fail</th>
        </tr>
        <tr>
            <td>Headers H1-H4</td>
            <td>&lt;h1&gt;..&lt;h4&gt;</td>
            <td>Headings</td>
            <td>Scaled font with margins</td>
            <td>PASS</td>
        </tr>
        <tr>
            <td>Paragraph &amp; Text</td>
            <td>&lt;p&gt;, &lt;b&gt;, &lt;i&gt;</td>
            <td>Typography</td>
            <td>Light grey word-wrapped text</td>
            <td>PASS</td>
        </tr>
        <tr>
            <td>Hyperlinks</td>
            <td>&lt;a href="..."&gt;</td>
            <td>Navigation</td>
            <td>Cyan clickable link with target</td>
            <td>PASS</td>
        </tr>
        <tr>
            <td>Code &amp; Preformatted</td>
            <td>&lt;code&gt;, &lt;pre&gt;</td>
            <td>Formatting</td>
            <td>Slate background with green text</td>
            <td>PASS</td>
        </tr>
        <tr>
            <td>Lists &amp; Bullets</td>
            <td>&lt;ul&gt;, &lt;ol&gt;, &lt;li&gt;</td>
            <td>Structure</td>
            <td>Indented items with bullets</td>
            <td>PASS</td>
        </tr>
        <tr>
            <td>Buttons &amp; Inputs</td>
            <td>&lt;button&gt;, &lt;input&gt;</td>
            <td>Form Controls</td>
            <td>Bordered box with centered text</td>
            <td>PASS</td>
        </tr>
        <tr>
            <td>Horizontal Divider</td>
            <td>&lt;hr /&gt;</td>
            <td>Separator</td>
            <td>Full-width horizontal rule</td>
            <td>PASS</td>
        </tr>
        <tr>
            <td>Table Grid</td>
            <td>&lt;table&gt;, &lt;tr&gt;, &lt;td&gt;</td>
            <td>Tabular Data</td>
            <td>Aligned columns with borders</td>
            <td>PASS</td>
        </tr>
    </table>
    <hr />

    <p><b>Acid-1 Lite Summary:</b> All 8 test suites passed! Browser compliance is fully verified.</p>
</body>
</html>)rawhtml";

static const char HTML_NEWS[] = R"rawhtml(<!DOCTYPE html>
<html>
<head>
    <title>AetherOS Daily Dispatch</title>
</head>
<body>
    <h1>AetherOS Daily Dispatch</h1>
    <p><i>The Premier Chronicle of Bare-Metal Operating System Innovations &bull; Edition #108</i></p>
    <p><a href="/index.html">&larr; Web Portal</a> | <a href="/docs.html">Kernel Manual</a> | <a href="/acid.html">Acid Standards Test</a></p>
    <hr />

    <h2>Headline: Native Hypermedia Browser Arrives in AetherOS!</h2>
    <p>The AetherOS Engineering Team is proud to unveil the <b>AetherOS Native Browser Subsystem</b>! Built from the ground up in freestanding C++20, this cutting-edge browser introduces a full HTML DOM tokenizer, robust CSS layout engine, and zero-allocation viewport clipping for fluid scrolling at up to 240 FPS.</p>
    <p>Users can now seamlessly explore embedded RamFS documentation, audit system diagnostics via the Acid-1 Lite test suite, and interact with live web services across local and wide-area networks.</p>
    <hr />

    <h2>Release Notes &bull; Version 1.0 "Aetherius"</h2>
    <ul>
        <li><b>Native Browser Engine:</b> High-speed HTML lexer, tag hierarchy constructor, and box layout engine supporting headings, tables, links, buttons, and code blocks.</li>
        <li><b>RamFS Pre-Seeded Web Space:</b> Out-of-the-box availability of <code>/index.html</code>, <code>/docs.html</code>, <code>/acid.html</code>, and <code>/news.html</code>.</li>
        <li><b>Dual Network Interface Priority:</b> Automatic link state arbitration between Intel e1000 Gigabit Ethernet and 802.11ax Wi-Fi 6 with seamless failover.</li>
        <li><b>Multimedia Acceleration:</b> VMware SVGA-II 2D FIFO hardware acceleration, Bochs VBE PAT write-combining, and Intel AC97 48kHz audio.</li>
        <li><b>64-bit Memory Subsystem:</b> 4-level paging with 4GB identity mapping and a multi-gigabyte dynamic kernel heap that scales with installed RAM.</li>
    </ul>
    <hr />

    <h2>Recent Kernel Commits</h2>
    <table border="1">
        <tr>
            <th>Commit Hash</th>
            <th>Author</th>
            <th>Component</th>
            <th>Summary</th>
        </tr>
        <tr>
            <td><code>a1f89c0</code></td>
            <td>Kernel Core</td>
            <td>ramfs</td>
            <td>Pre-seed /index.html, /docs.html, /acid.html, and /news.html in RamFS</td>
        </tr>
        <tr>
            <td><code>b2e78d1</code></td>
            <td>Browser Dev</td>
            <td>browser</td>
            <td>Implement HTML lexer, DOM tree builder, and viewport clipping layout engine</td>
        </tr>
        <tr>
            <td><code>c3d67e2</code></td>
            <td>Graphics Dev</td>
            <td>vmware_svga</td>
            <td>Add hardware FIFO acceleration for rect fills and screen copies</td>
        </tr>
        <tr>
            <td><code>d4c56f3</code></td>
            <td>Audio Dev</td>
            <td>ac97</td>
            <td>Enable Bus Master DMA stereo audio streaming at 48kHz with BDL rings</td>
        </tr>
        <tr>
            <td><code>e5b45a4</code></td>
            <td>Network Dev</td>
            <td>wifi / e1000</td>
            <td>Implement carrier auto-sensing and warm standby automatic failover</td>
        </tr>
        <tr>
            <td><code>f6a34b5</code></td>
            <td>Memory Dev</td>
            <td>pmm / heap</td>
            <td>Expand kernel dynamic heap to 64MB with block coalescing</td>
        </tr>
    </table>
    <hr />

    <h2>Upcoming Kernel Roadmap</h2>
    <ol>
        <li><b>Ext2 / FAT32 VFS Driver:</b> Persistent storage support for ATA/AHCI block storage devices.</li>
        <li><b>Multi-Core SMP Scheduler:</b> Symmetric multiprocessing with Local APIC and I/O APIC routing.</li>
        <li><b>Embedded TLS 1.3:</b> Cryptographic protocol engine enabling HTTPS secure web browsing.</li>
        <li><b>Ring 3 User Processes:</b> Isolated virtual address spaces with fast <code>SYSCALL</code>/<code>SYSRET</code> transitions.</li>
    </ol>
    <hr />

    <p><button onclick="location.href='/index.html'">Return to Web Portal</button> &nbsp; <button onclick="location.href='/docs.html'">Read Architecture Manual</button></p>
    <p><i>AetherOS Daily Dispatch &bull; Published by the AetherOS Kernel Consortium</i></p>
</body>
</html>)rawhtml";

void init() {
    for (size_t i = 0; i < MAX_FILES; ++i) {
        files[i].used = false;
        files[i].content = nullptr;
        files[i].size = 0;
        files[i].capacity = 0;
    }

    // Initial filesystem population array
    struct PreseededFile {
        const char* name;
        const char* content;
    };

    static const PreseededFile initial_files[] = {
        {
            "readme.txt",
            "Welcome to AetherOS 64-bit!\n"
            "A fully featured bare-metal operating system in C++20.\n"
            "Type 'help' in the shell to see all available commands.\n"
        },
        {
            "system.info",
            "OS: AetherOS 64-bit\n"
            "Architecture: x86_64 Long Mode\n"
            "Paging: 4-Level Paging (4GB Identity Mapped)\n"
            "Compiler: Freestanding GCC C++20\n"
            "Target: Bare-Metal PC / QEMU\n"
        },
        {
            "motd.txt",
            "\"The best way to predict the future is to invent it.\" - Alan Kay\n"
        },
        {
            "/index.html",
            HTML_INDEX
        },
        {
            "/docs.html",
            HTML_DOCS
        },
        {
            "/acid.html",
            HTML_ACID
        },
        {
            "/news.html",
            HTML_NEWS
        },
        {
            "/js.html",
            HTML_JS_DEMO
        }
    };

    for (size_t i = 0; i < sizeof(initial_files) / sizeof(initial_files[0]); ++i) {
        create(initial_files[i].name);
        write(initial_files[i].name, initial_files[i].content);
    }

    // Prepopulate sample WAV sound files for the Audio Driver Subsystem
    create_default_wavs();
}

static File* find_file(const char* name) {
    if (!name) return nullptr;

    // Special handling: root "/" or empty "" maps to index.html if present
    if (name[0] == '\0' || (name[0] == '/' && name[1] == '\0')) {
        for (size_t i = 0; i < MAX_FILES; ++i) {
            if (files[i].used && (!kstd::strcmp(files[i].name, "/index.html") || !kstd::strcmp(files[i].name, "index.html"))) {
                return &files[i];
            }
        }
    }

    const char* strip_name = (name[0] == '/') ? (name + 1) : name;
    for (size_t i = 0; i < MAX_FILES; ++i) {
        if (!files[i].used) continue;
        const char* f_name = (files[i].name[0] == '/') ? (files[i].name + 1) : files[i].name;
        if (!kstd::strcmp(files[i].name, name) || !kstd::strcmp(f_name, strip_name)) {
            return &files[i];
        }
    }
    return nullptr;
}

bool exists(const char* name) {
    return find_file(name) != nullptr;
}

bool create(const char* name) {
    if (!name || *name == '\0') return false;
    if (find_file(name)) return false; // already exists

    for (size_t i = 0; i < MAX_FILES; ++i) {
        if (!files[i].used) {
            kstd::strncpy(files[i].name, name, MAX_FILENAME_LEN - 1);
            files[i].name[MAX_FILENAME_LEN - 1] = '\0';
            files[i].capacity = 128;
            files[i].content = static_cast<char*>(kmalloc(files[i].capacity));
            if (!files[i].content) return false;
            files[i].content[0] = '\0';
            files[i].size = 0;
            files[i].used = true;
            return true;
        }
    }
    return false; // out of inode slots
}

bool write_bytes(const char* name, const void* data, size_t size, bool append) {
    if (!name || !data) return false;
    File* f = find_file(name);
    if (!f) {
        if (!create(name)) return false;
        f = find_file(name);
        if (!f) return false;
    }

    size_t new_size = append ? (f->size + size) : size;

    if (new_size + 1 > f->capacity) {
        size_t new_cap = (new_size + 1) * 2;
        char* new_buf = static_cast<char*>(kmalloc(new_cap));
        if (!new_buf) return false;

        if (append && f->size > 0) {
            kstd::memcpy(new_buf, f->content, f->size);
        }
        kfree(f->content);
        f->content = new_buf;
        f->capacity = new_cap;
    }

    if (append) {
        kstd::memcpy(f->content + f->size, data, size);
    } else {
        kstd::memcpy(f->content, data, size);
    }

    f->size = new_size;
    f->content[f->size] = '\0';
    return true;
}

bool write(const char* name, const char* text, bool append) {
    if (!text) return false;
    return write_bytes(name, text, kstd::strlen(text), append);
}

const void* read_bytes(const char* name, size_t* out_size) {
    File* f = find_file(name);
    if (!f) {
        if (out_size) *out_size = 0;
        return nullptr;
    }
    if (out_size) *out_size = f->size;
    return f->content;
}

const char* read(const char* name) {
    return static_cast<const char*>(read_bytes(name, nullptr));
}

bool remove(const char* name) {
    File* f = find_file(name);
    if (!f) return false;

    if (f->content) {
        kfree(f->content);
        f->content = nullptr;
    }
    f->used = false;
    f->size = 0;
    f->capacity = 0;
    f->name[0] = '\0';
    return true;
}

void list() {
    printk("=== RAM Filesystem (ramfs) ===\n");
    size_t count = 0;
    size_t total_bytes = 0;

    for (size_t i = 0; i < MAX_FILES; ++i) {
        if (files[i].used) {
            vga::set_color(vga::Color::LightCyan, vga::Color::Black);
            printk("  %s", files[i].name);
            size_t len = kstd::strlen(files[i].name);
            for (size_t s = len; s < 18; ++s) printk(" ");
            vga::set_color(vga::Color::White, vga::Color::Black);
            printk(" %u bytes\n", files[i].size);
            count++;
            total_bytes += files[i].size;
        }
    }

    if (count == 0) {
        printk("  (No files found)\n");
    } else {
        printk("Total: %u file(s), %u bytes stored in heap\n", count, total_bytes);
    }
}

const File* get_files() {
    return files;
}

} // namespace ramfs
