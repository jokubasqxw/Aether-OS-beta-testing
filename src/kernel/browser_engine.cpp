#include "browser_engine.hpp"
#include "ramfs.hpp"
#include "apphub.hpp"
#include "net.hpp"
#include "kstring.hpp"
#include "heap.hpp"
#include "pit.hpp"
#include "js.hpp"
#include "inflate.hpp"
#include "tls.hpp"

// Flex layout state (global for access from helper functions)
static bool g_in_flex = false;
static int g_flex_start_y = 0;
static int g_flex_cur_x = 0;
static int g_flex_max_h = 0;

namespace browser {

const char* DomNode::get_attribute(const char* name) const {
    if (!name) return nullptr;
    for (size_t i = 0; i < attr_count; ++i) {
        if (!kstd::strcmp(attributes[i].name, name)) {
            return attributes[i].value;
        }
    }
    return nullptr;
}

void Document::clear() {
    title[0] = '\0';
    current_url[0] = '\0';
    box_count = 0;
    page_width = 800;
    page_height = 600;
    scroll_y = 0;
    bg_color = 0xFF0B1420; // Default modern dark theme canvas
    bg_color2 = bg_color;
    has_bg_gradient = false;
    is_loading = false;
    load_time_ms = 0;
    source_bytes = 0;
    dirty = true;
}

Document* create_document() {
    Document* doc = static_cast<Document*>(kmalloc(sizeof(Document)));
    if (!doc) return nullptr;
    doc->boxes = static_cast<LayoutBox*>(kmalloc(sizeof(LayoutBox) * MAX_LAYOUT_BOXES));
    doc->box_capacity = MAX_LAYOUT_BOXES;
    doc->box_count = 0;
    doc->root = nullptr;
    doc->clear();
    return doc;
}

void destroy_document(Document* doc) {
    if (!doc) return;
    if (doc->boxes) {
        kfree(doc->boxes);
        doc->boxes = nullptr;
    }
    kfree(doc);
}

// Elements whose contents are opaque text (never rendered as markup/text)
static bool is_raw_text_tag(const char* name, size_t len) {
    static const char* const raw[] = { "script", "style", "noscript", "textarea", "template", "svg", "canvas" };
    for (const char* r : raw) {
        size_t rl = kstd::strlen(r);
        if (rl != len) continue;
        bool match = true;
        for (size_t i = 0; i < len; ++i) {
            char c = name[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != r[i]) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

// Void (self-closing) elements have no end tag and must not push layout state.
static bool is_void_name(const char* name) {
    static const char* const voids[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr"
    };
    for (const char* v : voids) if (!kstd::strcmp(name, v)) return true;
    return false;
}

// Case-insensitive Tag Matcher
static TagType resolve_tag(const char* name, size_t len) {
    if (len == 1) {
        if (name[0] == 'p' || name[0] == 'P') return TagType::P;
        if (name[0] == 'a' || name[0] == 'A') return TagType::A;
        if (name[0] == 'b' || name[0] == 'B') return TagType::B;
        if (name[0] == 'i' || name[0] == 'I') return TagType::I;
    } else if (len == 2) {
        if ((name[0] == 'h' || name[0] == 'H') && name[1] == '1') return TagType::H1;
        if ((name[0] == 'h' || name[0] == 'H') && name[1] == '2') return TagType::H2;
        if ((name[0] == 'h' || name[0] == 'H') && name[1] == '3') return TagType::H3;
        if ((name[0] == 'h' || name[0] == 'H') && name[1] == '4') return TagType::H4;
        if ((name[0] == 'u' || name[0] == 'U') && (name[1] == 'l' || name[1] == 'L')) return TagType::UL;
        if ((name[0] == 'o' || name[0] == 'O') && (name[1] == 'l' || name[1] == 'L')) return TagType::OL;
        if ((name[0] == 'l' || name[0] == 'L') && (name[1] == 'i' || name[1] == 'I')) return TagType::LI;
        if ((name[0] == 'h' || name[0] == 'H') && (name[1] == 'r' || name[1] == 'R')) return TagType::HR;
        if ((name[0] == 'b' || name[0] == 'B') && (name[1] == 'r' || name[1] == 'R')) return TagType::BR;
        if ((name[0] == 'e' || name[0] == 'E') && (name[1] == 'm' || name[1] == 'M')) return TagType::EM;
        if ((name[0] == 't' || name[0] == 'T') && (name[1] == 'r' || name[1] == 'R')) return TagType::TR;
        if ((name[0] == 't' || name[0] == 'T') && (name[1] == 'd' || name[1] == 'D')) return TagType::TD;
        if ((name[0] == 't' || name[0] == 'T') && (name[1] == 'h' || name[1] == 'H')) return TagType::TH;
    } else if (len == 3) {
        if (!kstd::strncmp(name, "div", 3) || !kstd::strncmp(name, "DIV", 3)) return TagType::DIV;
        if (!kstd::strncmp(name, "pre", 3) || !kstd::strncmp(name, "PRE", 3)) return TagType::PRE;
        if (!kstd::strncmp(name, "img", 3) || !kstd::strncmp(name, "IMG", 3)) return TagType::IMG;
    } else if (len == 4) {
        if (!kstd::strncmp(name, "html", 4) || !kstd::strncmp(name, "HTML", 4)) return TagType::HTML;
        if (!kstd::strncmp(name, "head", 4) || !kstd::strncmp(name, "HEAD", 4)) return TagType::HEAD;
        if (!kstd::strncmp(name, "body", 4) || !kstd::strncmp(name, "BODY", 4)) return TagType::BODY;
        if (!kstd::strncmp(name, "span", 4) || !kstd::strncmp(name, "SPAN", 4)) return TagType::SPAN;
        if (!kstd::strncmp(name, "code", 4) || !kstd::strncmp(name, "CODE", 4)) return TagType::CODE;
        if (!kstd::strncmp(name, "font", 4) || !kstd::strncmp(name, "FONT", 4)) return TagType::FONT;
    } else if (len == 5) {
        if (!kstd::strncmp(name, "title", 5) || !kstd::strncmp(name, "TITLE", 5)) return TagType::TITLE;
        if (!kstd::strncmp(name, "input", 5) || !kstd::strncmp(name, "INPUT", 5)) return TagType::INPUT;
        if (!kstd::strncmp(name, "table", 5) || !kstd::strncmp(name, "TABLE", 5)) return TagType::TABLE;
    } else if (len == 6) {
        if (!kstd::strncmp(name, "button", 6) || !kstd::strncmp(name, "BUTTON", 6)) return TagType::BUTTON;
        if (!kstd::strncmp(name, "strong", 6) || !kstd::strncmp(name, "STRONG", 6)) return TagType::STRONG;
        if (!kstd::strncmp(name, "center", 6) || !kstd::strncmp(name, "CENTER", 6)) return TagType::CENTER;
    } else if (len == 10) {
        if (!kstd::strncmp(name, "blockquote", 10) || !kstd::strncmp(name, "BLOCKQUOTE", 10)) return TagType::BLOCKQUOTE;
    }
    return TagType::UNKNOWN;
}

// Maps a Unicode code point to a representative ASCII character. The 8x16
// bitmap font only carries ASCII, so accented Latin letters fold to their base
// letter ("Ė" -> "E"), which keeps real pages readable.
static char latin_fold(uint32_t cp) {
    if (cp >= 0xC0 && cp <= 0xC5) return 'A';
    if (cp == 0xC6) return 'A';
    if (cp == 0xC7) return 'C';
    if (cp >= 0xC8 && cp <= 0xCB) return 'E';
    if (cp >= 0xCC && cp <= 0xCF) return 'I';
    if (cp == 0xD0) return 'D';
    if (cp == 0xD1) return 'N';
    if (cp >= 0xD2 && cp <= 0xD6) return 'O';
    if (cp == 0xD7) return 'x';
    if (cp == 0xD8) return 'O';
    if (cp >= 0xD9 && cp <= 0xDC) return 'U';
    if (cp == 0xDD) return 'Y';
    if (cp == 0xDE) return 'P';
    if (cp == 0xDF) return 's';
    if (cp >= 0xE0 && cp <= 0xE5) return 'a';
    if (cp == 0xE6) return 'a';
    if (cp == 0xE7) return 'c';
    if (cp >= 0xE8 && cp <= 0xEB) return 'e';
    if (cp >= 0xEC && cp <= 0xEF) return 'i';
    if (cp == 0xF0) return 'd';
    if (cp == 0xF1) return 'n';
    if (cp >= 0xF2 && cp <= 0xF6) return 'o';
    if (cp == 0xF7) return '/';
    if (cp == 0xF8) return 'o';
    if (cp >= 0xF9 && cp <= 0xFC) return 'u';
    if (cp == 0xFD || cp == 0xFF) return 'y';
    switch (cp) {
        case 0x104: case 0x105: return (cp & 1) ? 'a' : 'A';
        case 0x106: case 0x107: return (cp & 1) ? 'c' : 'C';
        case 0x10C: case 0x10D: return (cp & 1) ? 'c' : 'C';
        case 0x10E: case 0x10F: return (cp & 1) ? 'd' : 'D';
        case 0x110: case 0x111: return (cp & 1) ? 'd' : 'D';
        case 0x112: case 0x113: return (cp & 1) ? 'e' : 'E';
        case 0x116: case 0x117: return (cp & 1) ? 'e' : 'E';
        case 0x118: case 0x119: return (cp & 1) ? 'e' : 'E';
        case 0x11A: case 0x11B: return (cp & 1) ? 'e' : 'E';
        case 0x120: case 0x121: return (cp & 1) ? 'g' : 'G';
        case 0x124: case 0x125: return (cp & 1) ? 'h' : 'H';
        case 0x128: case 0x129: return (cp & 1) ? 'i' : 'I';
        case 0x12A: case 0x12B: return (cp & 1) ? 'i' : 'I';
        case 0x12C: case 0x12D: return (cp & 1) ? 'i' : 'I';
        case 0x12E: case 0x12F: return (cp & 1) ? 'i' : 'I';
        case 0x130: return 'I';
        case 0x131: return 'i';
        case 0x134: case 0x135: return (cp & 1) ? 'j' : 'J';
        case 0x136: case 0x137: return (cp & 1) ? 'k' : 'K';
        case 0x13B: case 0x13C: return (cp & 1) ? 'l' : 'L';
        case 0x13D: case 0x13E: return (cp & 1) ? 'l' : 'L';
        case 0x141: case 0x142: return (cp & 1) ? 'l' : 'L';
        case 0x143: case 0x144: return (cp & 1) ? 'n' : 'N';
        case 0x145: case 0x146: return (cp & 1) ? 'n' : 'N';
        case 0x147: case 0x148: return (cp & 1) ? 'n' : 'N';
        case 0x14C: case 0x14D: return (cp & 1) ? 'o' : 'O';
        case 0x150: case 0x151: return (cp & 1) ? 'o' : 'O';
        case 0x152: case 0x153: return (cp & 1) ? 'o' : 'O';
        case 0x154: case 0x155: return (cp & 1) ? 'r' : 'R';
        case 0x158: case 0x159: return (cp & 1) ? 'r' : 'R';
        case 0x15A: case 0x15B: return (cp & 1) ? 's' : 'S';
        case 0x15C: case 0x15D: return (cp & 1) ? 's' : 'S';
        case 0x15E: case 0x15F: return (cp & 1) ? 's' : 'S';
        case 0x160: case 0x161: return (cp & 1) ? 's' : 'S';
        case 0x162: case 0x163: return (cp & 1) ? 't' : 'T';
        case 0x164: case 0x165: return (cp & 1) ? 't' : 'T';
        case 0x166: case 0x167: return (cp & 1) ? 't' : 'T';
        case 0x168: case 0x169: return (cp & 1) ? 'u' : 'U';
        case 0x16A: case 0x16B: return (cp & 1) ? 'u' : 'U';
        case 0x16C: case 0x16D: return (cp & 1) ? 'u' : 'U';
        case 0x16E: case 0x16F: return (cp & 1) ? 'u' : 'U';
        case 0x170: case 0x171: return (cp & 1) ? 'u' : 'U';
        case 0x172: case 0x173: return (cp & 1) ? 'u' : 'U';
        case 0x174: case 0x175: return (cp & 1) ? 'w' : 'W';
        case 0x176: case 0x177: return (cp & 1) ? 'y' : 'Y';
        case 0x178: return 'Y';
        case 0x179: case 0x17A: return (cp & 1) ? 'Z' : 'z';
        case 0x17B: case 0x17C: return (cp & 1) ? 'Z' : 'z';
        case 0x17D: case 0x17E: return (cp & 1) ? 'Z' : 'z';
        default: return 0;
    }
}

static void emit_codepoint(char* dst, size_t& d, size_t max_len, uint32_t cp) {
    auto put = [&](char ch) { if (d + 1 < max_len) dst[d++] = ch; };
    if (cp == 0x2026) { put('.'); put('.'); put('.'); return; }
    if (cp == 0x00A9) { put('('); put('c'); put(')'); return; }
    if (cp == 0x00AE) { put('('); put('R'); put(')'); return; }
    if (cp == 0x2122) { put('('); put('t'); put('m'); put(')'); return; }
    if (cp == 0x20AC) { put('E'); put('U'); put('R'); return; }

    char c = 0;
    if (cp < 0x80) c = (cp == 9 || cp == 10 || cp == 13) ? ' ' : static_cast<char>(cp);
    else if (cp == 0xA0) c = ' ';
    else if (cp == 0x2018 || cp == 0x2019 || cp == 0x201A || cp == 0x2032) c = '\'';
    else if (cp == 0x201C || cp == 0x201D || cp == 0x201E || cp == 0x2033) c = '"';
    else if (cp == 0x00AB || cp == 0x00BB) c = '"';
    else if (cp >= 0x2010 && cp <= 0x2015) c = '-';
    else if (cp == 0x2022 || cp == 0x00B7 || cp == 0x25CF) c = '*';
    else { c = latin_fold(cp); if (!c) c = '?'; }
    put(c);
}

// Entity Decoder
static void decode_entities(const char* src, char* dst, size_t max_len) {
    size_t d = 0;
    while (*src && d < max_len - 1) {
        if (*src == '&') {
            if (src[1] == '#') {
                const char* q = src + 2;
                bool hex = false;
                if (*q == 'x' || *q == 'X') { hex = true; ++q; }
                uint32_t cp = 0;
                bool any = false;
                for (;;) {
                    char c = *q;
                    int dv = -1;
                    if (c >= '0' && c <= '9') dv = c - '0';
                    else if (hex && c >= 'a' && c <= 'f') dv = c - 'a' + 10;
                    else if (hex && c >= 'A' && c <= 'F') dv = c - 'A' + 10;
                    else break;
                    cp = cp * (hex ? 16u : 10u) + static_cast<uint32_t>(dv);
                    any = true;
                    ++q;
                    if (cp > 0x10FFFF) break;
                }
                if (any && *q == ';') {
                    emit_codepoint(dst, d, max_len, cp);
                    src = q + 1;
                    continue;
                }
            }
            if (!kstd::strncmp(src, "&nbsp;", 6)) {
                dst[d++] = ' ';
                src += 6;
                continue;
            } else if (!kstd::strncmp(src, "&lt;", 4)) {
                dst[d++] = '<';
                src += 4;
                continue;
            } else if (!kstd::strncmp(src, "&gt;", 4)) {
                dst[d++] = '>';
                src += 4;
                continue;
            } else if (!kstd::strncmp(src, "&amp;", 5)) {
                dst[d++] = '&';
                src += 5;
                continue;
            } else if (!kstd::strncmp(src, "&quot;", 6)) {
                dst[d++] = '"';
                src += 6;
                continue;
            } else if (!kstd::strncmp(src, "&apos;", 6)) {
                dst[d++] = '\'';
                src += 6;
                continue;
            } else if (!kstd::strncmp(src, "&copy;", 6)) {
                if (d + 3 < max_len - 1) {
                    dst[d++] = '(';
                    dst[d++] = 'c';
                    dst[d++] = ')';
                }
                src += 6;
                continue;
            } else if (!kstd::strncmp(src, "&bull;", 6)) {
                dst[d++] = '*';
                src += 6;
                continue;
            } else if (!kstd::strncmp(src, "&mdash;", 7)) {
                if (d + 2 < max_len - 1) {
                    dst[d++] = '-';
                    dst[d++] = '-';
                }
                src += 7;
                continue;
            } else if (!kstd::strncmp(src, "&rarr;", 6)) {
                if (d + 2 < max_len - 1) {
                    dst[d++] = '-';
                    dst[d++] = '>';
                }
                src += 6;
                continue;
            } else if (!kstd::strncmp(src, "&larr;", 6)) {
                if (d + 2 < max_len - 1) {
                    dst[d++] = '<';
                    dst[d++] = '-';
                }
                src += 6;
                continue;
            } else if (!kstd::strncmp(src, "&lsquo;", 7) || !kstd::strncmp(src, "&rsquo;", 7)) {
                dst[d++] = '\'';
                src += 7;
                continue;
            } else if (!kstd::strncmp(src, "&ldquo;", 7) || !kstd::strncmp(src, "&rdquo;", 7)) {
                dst[d++] = '"';
                src += 7;
                continue;
            } else if (!kstd::strncmp(src, "&hellip;", 8)) {
                if (d + 3 < max_len - 1) {
                    dst[d++] = '.'; dst[d++] = '.'; dst[d++] = '.';
                }
                src += 8;
                continue;
            } else if (!kstd::strncmp(src, "&ndash;", 7)) {
                dst[d++] = '-';
                src += 7;
                continue;
            } else if (!kstd::strncmp(src, "&middot;", 8)) {
                dst[d++] = '*';
                src += 8;
                continue;
            } else if (!kstd::strncmp(src, "&times;", 7)) {
                dst[d++] = 'x';
                src += 7;
                continue;
            } else if (!kstd::strncmp(src, "&deg;", 6)) {
                dst[d++] = 'o';
                src += 6;
                continue;
            } else if (!kstd::strncmp(src, "&sect;", 7)) {
                dst[d++] = 'S';
                src += 7;
                continue;
            }
        }

        // UTF-8 source text: fold each code point to a representative ASCII
        // character so smart quotes, dashes and accents stay readable.
        unsigned char uc = static_cast<unsigned char>(*src);
        if (uc < 0x80) {
            dst[d++] = static_cast<char>(uc);
            ++src;
            continue;
        }
        size_t need = 0;
        uint32_t cp = 0;
        if ((uc & 0xE0) == 0xC0) { need = 1; cp = uc & 0x1F; }
        else if ((uc & 0xF0) == 0xE0) { need = 2; cp = uc & 0x0F; }
        else if ((uc & 0xF8) == 0xF0) { need = 3; cp = uc & 0x07; }
        bool utf8_ok = (need > 0);
        for (size_t k = 1; utf8_ok && k <= need; ++k) {
            char sc = src[k];
            if (sc == '\0') { utf8_ok = false; break; }
            unsigned char cc = static_cast<unsigned char>(sc);
            if ((cc & 0xC0) != 0x80) { utf8_ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (utf8_ok && cp <= 0x10FFFF) {
            emit_codepoint(dst, d, max_len, cp);
            src += need + 1;
        } else {
            dst[d++] = '?';
            ++src;
        }
    }
    dst[d] = '\0';
}

// ======================= Minimal CSS engine =======================
// Supports a useful subset of CSS 2.1 so that real pages keep their visual
// identity: background-color, color, font-weight/style, text-decoration,
// border (color), padding, width, font-size and text-align, matched through
// tag / .class / #id simple selectors plus a trailing compound.

struct Style {
    bool has_bg = false;      uint32_t bg = 0;
    bool has_grad = false;    uint32_t grad_top = 0;  uint32_t grad_bottom = 0;
    bool has_fg = false;      uint32_t fg = 0;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool no_underline = false;
    bool display_none = false;
    bool display_flex = false;
    bool display_grid = false;
    bool has_border = false;  uint32_t border = 0;
    bool has_pad = false;     int pad = 0;
    bool has_width = false;   int width = 0;  bool width_pct = false;
    bool has_max_width = false; int max_width = 0; bool max_width_pct = false;
    bool has_mt = false;      int mt = 0;
    bool has_mb = false;      int mb = 0;
    bool has_ml = false;      int ml = 0;     bool ml_auto = false;
    bool has_mr = false;      int mr = 0;     bool mr_auto = false;
    int font_scale = 0;       // 0 normal, 1 large, 2 huge
    bool has_align = false;   int align = 0;  // 0 left, 1 center, 2 right
};

static void style_merge(Style& dst, const Style& s) {
    if (s.has_bg) {
        dst.has_bg = true; dst.bg = s.bg;
        dst.has_grad = s.has_grad; dst.grad_top = s.grad_top; dst.grad_bottom = s.grad_bottom;
    }
    if (s.has_fg) { dst.has_fg = true; dst.fg = s.fg; }
    if (s.bold) dst.bold = true;
    if (s.italic) dst.italic = true;
    if (s.underline) { dst.underline = true; dst.no_underline = false; }
    if (s.no_underline) { dst.no_underline = true; dst.underline = false; }
    if (s.display_none) dst.display_none = true;
    if (s.display_flex) dst.display_flex = true;
    if (s.display_grid) dst.display_grid = true;
    if (s.has_border) { dst.has_border = true; dst.border = s.border; }
    if (s.has_pad) { dst.has_pad = true; dst.pad = s.pad; }
    if (s.has_width) { dst.has_width = true; dst.width = s.width; dst.width_pct = s.width_pct; }
    if (s.has_max_width) { dst.has_max_width = true; dst.max_width = s.max_width; dst.max_width_pct = s.max_width_pct; }
    if (s.has_mt) { dst.has_mt = true; dst.mt = s.mt; }
    if (s.has_mb) { dst.has_mb = true; dst.mb = s.mb; }
    if (s.has_ml) { dst.has_ml = true; dst.ml = s.ml; dst.ml_auto = s.ml_auto; }
    if (s.has_mr) { dst.has_mr = true; dst.mr = s.mr; dst.mr_auto = s.mr_auto; }
    if (s.font_scale > dst.font_scale) dst.font_scale = s.font_scale;
    if (s.has_align) { dst.has_align = true; dst.align = s.align; }
}

static char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

static bool ci_eq_len(const char* a, size_t alen, const char* b) {
    size_t i = 0;
    for (; i < alen && b[i]; ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    }
    return i == alen && b[i] == '\0';
}

static bool ci_starts(const char* a, size_t alen, const char* prefix) {
    size_t i = 0;
    for (; prefix[i]; ++i) {
        if (i >= alen) return false;
        if (ascii_lower(a[i]) != ascii_lower(prefix[i])) return false;
    }
    return true;
}

static bool ci_contains(const char* a, size_t alen, const char* needle) {
    size_t nl = kstd::strlen(needle);
    if (nl == 0 || alen < nl) return false;
    for (size_t i = 0; i + nl <= alen; ++i) {
        size_t j = 0;
        while (j < nl && ascii_lower(a[i + j]) == ascii_lower(needle[j])) ++j;
        if (j == nl) return true;
    }
    return false;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const char* const NAMED_CSS[] = {
    "white", "black", "red", "green", "blue", "yellow", "orange", "purple",
    "gray", "grey", "silver", "teal", "navy", "maroon", "olive", "lime",
    "aqua", "cyan", "magenta", "fuchsia", "pink", "brown", "gold", "tan",
    "lightgray", "lightgrey", "darkgray", "darkgrey", "ivory", "beige",
    "transparent"
};
static const uint32_t NAMED_RGB[] = {
    0xFFFFFFFF, 0xFF000000, 0xFFFF0000, 0xFF008000, 0xFF0000FF, 0xFFFFFF00, 0xFFFFA500, 0xFF800080,
    0xFF808080, 0xFF808080, 0xFFC0C0C0, 0xFF008080, 0xFF000080, 0xFF800000, 0xFF808000, 0xFF00FF00,
    0xFF00FFFF, 0xFF00FFFF, 0xFFFF00FF, 0xFFFF00FF, 0xFFFFC0CB, 0xFFA52A2A, 0xFFFFD700, 0xFFD2B48C,
    0xFFD3D3D3, 0xFFD3D3D3, 0xFFA9A9A9, 0xFFA9A9A9, 0xFFFFFFF0, 0xFFF5F5DC,
    0x00000000
};

static bool parse_color(const char* v, size_t len, uint32_t* out) {
    size_t i = 0;
    while (i < len && (v[i] == ' ' || v[i] == '\t')) ++i;
    if (i >= len) return false;

    if (v[i] == '#') {
        size_t j = i + 1;
        while (j < len && hex_digit(v[j]) >= 0) ++j;
        size_t n = j - (i + 1);
        if (n != 3 && n != 6 && n != 8) return false;
        uint32_t val = 0;
        for (size_t k = i + 1; k < j; ++k) val = (val << 4) | static_cast<uint32_t>(hex_digit(v[k]));
        uint32_t r, g, b;
        if (n == 3) { r = ((val >> 8) & 0xF) * 17; g = ((val >> 4) & 0xF) * 17; b = (val & 0xF) * 17; }
        else if (n == 6) { r = (val >> 16) & 0xFF; g = (val >> 8) & 0xFF; b = val & 0xFF; }
        else { r = (val >> 24) & 0xFF; g = (val >> 16) & 0xFF; b = (val >> 8) & 0xFF; }
        *out = 0xFF000000u | (r << 16) | (g << 8) | b;
        return true;
    }

    if (i + 3 <= len && ascii_lower(v[i]) == 'r' && ascii_lower(v[i + 1]) == 'g' && ascii_lower(v[i + 2]) == 'b') {
        const char* p = v + i;
        const char* e = v + len;
        while (p < e && *p != '(') ++p;
        if (p >= e) return false;
        ++p;
        int comp[3] = {0, 0, 0};
        for (int c = 0; c < 3; ++c) {
            while (p < e && (*p == ' ' || *p == ',')) ++p;
            int val = 0;
            bool any = false;
            while (p < e && *p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); ++p; any = true; }
            if (!any) return false;
            comp[c] = val > 255 ? 255 : val;
        }
        *out = 0xFF000000u | (static_cast<uint32_t>(comp[0]) << 16) |
               (static_cast<uint32_t>(comp[1]) << 8) | static_cast<uint32_t>(comp[2]);
        return true;
    }

    size_t j = i;
    while (j < len && v[j] != ' ' && v[j] != '\t' && v[j] != ';') ++j;
    size_t n = j - i;
    for (size_t k = 0; k < sizeof(NAMED_CSS) / sizeof(NAMED_CSS[0]); ++k) {
        if (ci_eq_len(v + i, n, NAMED_CSS[k])) {
            if (NAMED_RGB[k] == 0) return false; // transparent
            *out = NAMED_RGB[k];
            return true;
        }
    }
    return false;
}

// Presentational HTML attributes (bgcolor="1e293b") omit the leading '#'.
static bool parse_legacy_color(const char* v, uint32_t* out) {
    if (!v || !*v) return false;
    size_t len = kstd::strlen(v);
    if (parse_color(v, len, out)) return true;
    if (v[0] == '#') return false;
    char tmp[16];
    tmp[0] = '#';
    size_t n = len < 14 ? len : 14;
    kstd::strncpy(tmp + 1, v, n);
    tmp[n + 1] = '\0';
    return parse_color(tmp, n + 1, out);
}

static int parse_px(const char* v, size_t len) {
    size_t i = 0;
    while (i < len && (v[i] == ' ' || v[i] == '\t')) ++i;
    bool neg = false;
    if (i < len && v[i] == '-') { neg = true; ++i; }
    int val = 0;
    while (i < len && v[i] >= '0' && v[i] <= '9') { val = val * 10 + (v[i] - '0'); ++i; }
    return neg ? -val : val;
}

static bool val_is_auto(const char* v, size_t len) {
    size_t i = 0;
    while (i < len && (v[i] == ' ' || v[i] == '\t')) ++i;
    size_t j = len;
    while (j > i && (v[j - 1] == ' ' || v[j - 1] == '\t')) --j;
    if (j - i != 4) return false;
    return ascii_lower(v[i]) == 'a' && ascii_lower(v[i + 1]) == 'u' &&
           ascii_lower(v[i + 2]) == 't' && ascii_lower(v[i + 3]) == 'o';
}

static bool val_is_bold(const char* v, size_t len) {
    if (ci_starts(v, len, "bold")) return true;
    if (ci_starts(v, len, "bolder")) return true;
    int n = parse_px(v, len);
    return n >= 600;
}

static void apply_decl(Style& st, const char* prop, size_t plen, const char* val, size_t vlen) {
    while (plen > 0 && (prop[0] == ' ' || prop[0] == '\t')) { ++prop; --plen; }
    while (plen > 0 && (prop[plen - 1] == ' ' || prop[plen - 1] == '\t')) --plen;
    while (vlen > 0 && (val[0] == ' ' || val[0] == '\t')) { ++val; --vlen; }
    while (vlen > 0 && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t' || val[vlen - 1] == ';')) --vlen;

    if (plen == 0 || vlen == 0) return;
    auto ci = [&](const char* n) { return ci_eq_len(prop, plen, n); };

    if (ci("background") || ci("background-color")) {
        if (ci_starts(val, vlen, "linear-gradient")) {
            // Accept the two-colour form and ignore direction keywords; the
            // renderer paints top-to-bottom. Stops like "#123 40%" are fine.
            size_t open = 0;
            while (open < vlen && val[open] != '(') ++open;
            uint32_t cols[2] = {0, 0};
            int nc = 0;
            size_t k = open < vlen ? open + 1 : vlen;
            while (k < vlen && nc < 2) {
                size_t s = k;
                while (k < vlen && val[k] != ',' && val[k] != ')') ++k;
                size_t e = k;
                while (s < e && (val[s] == ' ' || val[s] == '\t')) ++s;
                while (e > s && (val[e - 1] == ' ' || val[e - 1] == '\t')) --e;
                uint32_t c;
                if (e > s && parse_color(val + s, e - s, &c)) {
                    cols[nc++] = c;
                } else if (e > s) {
                    size_t cut = s;
                    while (cut < e && val[cut] != ' ' && val[cut] != '\t') ++cut;
                    if (cut > s && parse_color(val + s, cut - s, &c)) cols[nc++] = c;
                }
                if (k < vlen && val[k] == ',') ++k;
                else break;
            }
            if (nc > 0) {
                st.has_bg = true;
                st.has_grad = true;
                st.grad_top = cols[0];
                st.grad_bottom = nc > 1 ? cols[1] : cols[0];
                st.bg = cols[0];
            }
        } else {
            uint32_t c;
            if (parse_color(val, vlen, &c)) { st.has_bg = true; st.bg = c; st.has_grad = false; }
        }
    } else if (ci("color")) {
        uint32_t c;
        if (parse_color(val, vlen, &c)) { st.has_fg = true; st.fg = c; }
    } else if (ci("font-weight")) {
        if (val_is_bold(val, vlen)) st.bold = true;
    } else if (ci("font-style")) {
        if (ci_starts(val, vlen, "italic") || ci_starts(val, vlen, "oblique")) st.italic = true;
    } else if (ci("text-decoration")) {
        if (ci_contains(val, vlen, "underline")) { st.underline = true; st.no_underline = false; }
        if (ci_contains(val, vlen, "none")) { st.no_underline = true; st.underline = false; }
    } else if (ci("display")) {
        if (ci_starts(val, vlen, "none")) st.display_none = true;
        if (ci_starts(val, vlen, "flex")) st.display_flex = true;
        if (ci_starts(val, vlen, "grid")) st.display_grid = true;
    } else if (ci("border") || ci("border-color") || ci_starts(prop, plen, "border-")) {
        if (ci_contains(val, vlen, "none")) {
            st.has_border = true; st.border = 0; // zero-width: keep 0 meaning "no visible border"
        } else {
            uint32_t c;
            st.has_border = true;
            st.border = parse_color(val, vlen, &c) ? c : 0;
        }
    } else if (ci("padding") || ci_starts(prop, plen, "padding-")) {
        st.has_pad = true;
        st.pad = parse_px(val, vlen);
    } else if (ci("margin") || ci_starts(prop, plen, "margin-")) {
        int vals[4] = {0, 0, 0, 0};
        bool autos[4] = {false, false, false, false};
        int n = 0;
        size_t k = 0;
        while (k < vlen && n < 4) {
            while (k < vlen && (val[k] == ' ' || val[k] == '\t')) ++k;
            if (k >= vlen) break;
            size_t s = k;
            while (k < vlen && val[k] != ' ' && val[k] != '\t') ++k;
            vals[n] = parse_px(val + s, k - s);
            autos[n] = val_is_auto(val + s, k - s);
            ++n;
        }
        if (n > 0) {
            if (ci("margin-top")) {
                st.has_mt = true; st.mt = vals[0];
            } else if (ci("margin-bottom")) {
                st.has_mb = true; st.mb = vals[0];
            } else if (ci("margin-left")) {
                st.has_ml = true; st.ml = vals[0]; st.ml_auto = autos[0];
            } else if (ci("margin-right")) {
                st.has_mr = true; st.mr = vals[0]; st.mr_auto = autos[0];
            } else {
                int t = vals[0], r = vals[0], b = vals[0], l = vals[0];
                bool at = autos[0], ar = autos[0], ab = autos[0], al = autos[0];
                if (n == 2) {
                    r = l = vals[1];
                    ar = al = autos[1];
                } else if (n == 3) {
                    r = l = vals[1]; b = vals[2];
                    ar = al = autos[1];
                    ab = autos[2];
                } else if (n >= 4) {
                    r = vals[1]; b = vals[2]; l = vals[3];
                    ar = autos[1]; ab = autos[2]; al = autos[3];
                }
                st.has_mt = st.has_mb = st.has_ml = st.has_mr = true;
                st.mt = t; st.mr = r; st.mb = b; st.ml = l;
                st.ml_auto = al; st.mr_auto = ar;
                (void)at; (void)ab;
            }
        }
    } else if (ci("font-size")) {
        // Check for percentage values (e.g. "83%") - ignore them, use base size
        bool is_pct = false;
        for (size_t k = 0; k < vlen; ++k) {
            if (val[k] == '%') { is_pct = true; break; }
        }
        // Handle CSS keywords
        int px = 0;
        if (ci_starts(val, vlen, "small")) px = 12;
        else if (ci_starts(val, vlen, "medium")) px = 16;
        else if (ci_starts(val, vlen, "large")) px = 18;
        else if (ci_starts(val, vlen, "x-large")) px = 24;
        else if (ci_starts(val, vlen, "xx-large")) px = 32;
        else if (!is_pct) px = parse_px(val, vlen);
        // Clamp font sizes to reasonable range (base 8x16 font)
        // Map CSS px to scale: 1x=16px, 2x=32px, 3x=48px, etc.
        if (px <= 0 || px <= 16) st.font_scale = 0;
        else if (px <= 24) st.font_scale = 1;
        else if (px <= 40) st.font_scale = 2;
        else if (px <= 56) st.font_scale = 3;
        else if (px <= 72) st.font_scale = 4;
        else st.font_scale = 5;
    } else if (ci("text-align")) {
        st.has_align = true;
        if (ci_starts(val, vlen, "center")) st.align = 1;
        else if (ci_starts(val, vlen, "right")) st.align = 2;
        else st.align = 0;
    } else if (ci("width") || ci("min-width")) {
        int px = parse_px(val, vlen);
        bool pct = false;
        for (size_t k = 0; k < vlen; ++k) {
            if (val[k] == '%') { pct = true; break; }
        }
        if (px > 0) { st.has_width = true; st.width = px; st.width_pct = pct; }
    } else if (ci("max-width")) {
        int px = parse_px(val, vlen);
        bool pct = false;
        for (size_t k = 0; k < vlen; ++k) {
            if (val[k] == '%') { pct = true; break; }
        }
        if (px > 0) { st.has_max_width = true; st.max_width = px; st.max_width_pct = pct; }
    }
}

static void parse_declarations(Style& st, const char* d, size_t len) {
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && d[i] != ';') ++i;
        size_t seg = i - start;
        if (i < len) ++i;
        // split prop:val
        size_t c = start;
        while (c < start + seg && d[c] != ':') ++c;
        if (c < start + seg) {
            apply_decl(st, d + start, c - start, d + c + 1, start + seg - (c + 1));
        }
    }
}

struct CssRule {
    char tag[16];
    char cls[32];
    char id[32];
    Style st;
};

static constexpr int MAX_CSS_RULES = 320;
static CssRule g_rules[MAX_CSS_RULES];
static int g_rule_count = 0;

static void parse_selector_compound(CssRule& r, const char* sel, size_t len) {
    r.tag[0] = 0;
    r.cls[0] = 0;
    r.id[0] = 0;
    size_t start = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = sel[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '>' || c == '+' || c == '~') start = i + 1;
    }
    size_t i = start;
    size_t ts = i;
    while (i < len && sel[i] != '.' && sel[i] != '#' && sel[i] != ':' && sel[i] != '[') ++i;
    if (i > ts && !(i - ts == 1 && sel[ts] == '*')) {
        size_t n = i - ts;
        if (n > 15) n = 15;
        for (size_t k = 0; k < n; ++k) r.tag[k] = ascii_lower(sel[ts + k]);
        r.tag[n] = 0;
    }
    while (i < len) {
        if (sel[i] == '.') {
            ++i;
            size_t s = i;
            while (i < len && sel[i] != '.' && sel[i] != '#' && sel[i] != ':' && sel[i] != '[') ++i;
            if (r.cls[0] == 0) {
                size_t n = i - s;
                if (n > 31) n = 31;
                for (size_t k = 0; k < n; ++k) r.cls[k] = sel[s + k];
                r.cls[n] = 0;
            }
        } else if (sel[i] == '#') {
            ++i;
            size_t s = i;
            while (i < len && sel[i] != '.' && sel[i] != '#' && sel[i] != ':' && sel[i] != '[') ++i;
            size_t n = i - s;
            if (n > 31) n = 31;
            for (size_t k = 0; k < n; ++k) r.id[k] = sel[s + k];
            r.id[n] = 0;
        } else if (sel[i] == ':' || sel[i] == '[') {
            break;
        } else {
            ++i;
        }
    }
}

static void parse_stylesheet(const char* css, size_t len) {
    size_t i = 0;
    while (i < len) {
        while (i < len && (css[i] == ' ' || css[i] == '\n' || css[i] == '\t' || css[i] == '\r')) ++i;
        if (i + 1 < len && css[i] == '/' && css[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(css[i] == '*' && css[i + 1] == '/')) ++i;
            i += 2;
            continue;
        }
        if (i >= len) break;
        size_t s_start = i;
        while (i < len && css[i] != '{') ++i;
        if (i >= len) break;
        size_t s_len = i - s_start;
        ++i; // '{'
        size_t d_start = i;
        while (i < len && css[i] != '}') ++i;
        size_t d_len = i - d_start;
        if (i < len && css[i] == '}') ++i;

        size_t a = 0;
        for (size_t b = 0; b <= s_len; ++b) {
            if (b == s_len || css[s_start + b] == ',') {
                size_t x = a, y = b;
                while (x < y && (css[s_start + x] == ' ' || css[s_start + x] == '\n' || css[s_start + x] == '\t' || css[s_start + x] == '\r')) ++x;
                while (y > x && (css[s_start + y - 1] == ' ' || css[s_start + y - 1] == '\n' || css[s_start + y - 1] == '\t' || css[s_start + y - 1] == '\r')) --y;
                if (y > x && g_rule_count < MAX_CSS_RULES) {
                    CssRule& r = g_rules[g_rule_count];
                    r.st = Style{};
                    parse_selector_compound(r, css + s_start + x, y - x);
                    parse_declarations(r.st, css + d_start, d_len);
                    if (r.tag[0] || r.cls[0] || r.id[0]) ++g_rule_count;
                }
                a = b + 1;
            }
        }
    }
}

static bool class_contains(const char* cls_list, const char* want) {
    if (!cls_list || !*cls_list) return false;
    size_t wl = kstd::strlen(want);
    const char* p = cls_list;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
        const char* s = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') ++p;
        if (static_cast<size_t>(p - s) == wl && !kstd::strncmp(s, want, wl)) return true;
    }
    return false;
}

static bool rule_matches(const CssRule& r, const char* tag, const char* cls, const char* id) {
    if (r.tag[0] && kstd::strcmp(r.tag, tag) != 0) return false;
    if (r.id[0] && (!id || kstd::strcmp(r.id, id) != 0)) return false;
    if (r.cls[0] && !class_contains(cls, r.cls)) return false;
    return true;
}

static void compute_style(const char* tag, const char* cls, const char* id,
                          const char* inline_style, Style& out) {
    out = Style{};
    for (int i = 0; i < g_rule_count; ++i) {
        if (rule_matches(g_rules[i], tag, cls, id)) style_merge(out, g_rules[i].st);
    }
    if (inline_style && *inline_style) {
        Style s2;
        parse_declarations(s2, inline_style, kstd::strlen(inline_style));
        style_merge(out, s2);
    }
}

// Fast Box Allocator
static LayoutBox* add_box(Document* doc) {
    if (!doc || doc->box_count >= doc->box_capacity) return nullptr;
    LayoutBox* box = &doc->boxes[doc->box_count++];
    box->x = 0; box->y = 0; box->w = 0; box->h = 16;
    box->type = BoxType::TEXT;
    box->text[0] = '\0';
    box->link_url[0] = '\0';
    box->img_src[0] = '\0';
    box->img_pixels = nullptr;
    box->img_w = 0;
    box->img_h = 0;
    box->fg_color = gfx::Colors::White;
    box->bg_color = 0;
    box->bg_color2 = 0;
    box->border_color = 0;
    box->scale = 1;
    box->has_bg = false;
    box->has_gradient = false;
    box->has_border = false;
    box->is_bold = false;
    box->is_underline = false;
    box->is_clickable = false;
    box->is_button = false;
    box->is_input = false;
    box->is_hovered = false;
    return box;
}

// Word-wrapped text box generator. `left`/`right` are the content bounds of
// the current block (the right edge is exclusive); `align` shifts finished
// lines horizontally (0 left, 1 centre, 2 right).
static void add_wrapped_text(Document* doc, const char* text, int& cur_x, int& cur_y,
                             int left, int right, int align,
                             uint32_t fg, bool bold, bool underline,
                             const char* link_url = nullptr, bool is_btn = false,
                             bool has_bg = false, uint32_t bg = 0, int scale = 1) {
    if (!text || *text == '\0') return;
    if (scale < 1) scale = 1;
    if (scale > 6) scale = 6;

    char decoded[256];
    decode_entities(text, decoded, sizeof(decoded));

    const int char_w = 8 * scale;
    const int line_h = 18 * scale;
    size_t line_first = doc->box_count;
    int line_right = left;

    auto finish_line = [&]() {
        if (align != 0 && doc->box_count > line_first) {
            int delta = (align == 2) ? (right - line_right) : ((right - line_right) / 2);
            if (delta > 0) {
                for (size_t k = line_first; k < doc->box_count; ++k) {
                    doc->boxes[k].x += delta;
                }
            }
        }
        line_first = doc->box_count;
        line_right = left;
    };

    const char* p = decoded;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            if (*p == '\n') {
                finish_line();
                cur_x = left;
                cur_y += line_h;
            }
            p++;
        }
        if (!*p) break;

        // Extract single word
        const char* word_start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
        size_t word_len = static_cast<size_t>(p - word_start);
        if (word_len == 0) continue;

        int word_px = static_cast<int>(word_len) * char_w;

        // Wrap to next line if exceeding line width
        if (cur_x + word_px > right && cur_x > left + 2) {
            finish_line();
            cur_x = left;
            cur_y += line_h;
        }

        LayoutBox* box = add_box(doc);
        if (!box) return;

        box->x = g_in_flex ? g_flex_cur_x : cur_x;
        box->y = g_in_flex ? g_flex_start_y : cur_y;
        box->w = word_px;
        box->h = is_btn ? 22 : 18 * scale;
        box->scale = static_cast<uint8_t>(scale);
        
        if (g_in_flex) {
            g_flex_cur_x += box->w + 8;
            if (box->h > g_flex_max_h) g_flex_max_h = box->h;
        }
        box->fg_color = fg;
        box->is_bold = bold;
        box->is_underline = underline;
        box->is_button = is_btn;
        if (has_bg) { box->has_bg = true; box->bg_color = bg; }

        if (link_url && *link_url) {
            kstd::strncpy(box->link_url, link_url, 127);
            box->link_url[127] = '\0';
            box->is_clickable = true;
        }

        size_t copy_len = word_len < 127 ? word_len : 127;
        kstd::strncpy(box->text, word_start, copy_len);
        box->text[copy_len] = '\0';

        cur_x += word_px + char_w; // space between words
        line_right = cur_x - char_w;
    }
    finish_line();
}

// Elements that establish a new block formatting context in layout.
static bool is_block_tag(TagType t) {
    switch (t) {
        case TagType::DIV: case TagType::P:
        case TagType::H1: case TagType::H2: case TagType::H3: case TagType::H4:
        case TagType::UL: case TagType::OL: case TagType::LI:
        case TagType::TABLE: case TagType::TR: case TagType::TD: case TagType::TH:
        case TagType::CENTER: case TagType::BLOCKQUOTE:
            return true;
        default:
            return false;
    }
}

// HTML Stream Lexer and Layout Builder
bool parse_html(Document* doc, const char* html, size_t len) {
    if (!doc || !html) return false;
    doc->box_count = 0;
    doc->scroll_y = 0;
    doc->source_bytes = len ? len : kstd::strlen(html);
    doc->title[0] = '\0';
    doc->bg_color = 0xFF0B1420;
    doc->bg_color2 = doc->bg_color;
    doc->has_bg_gradient = false;

    uint64_t t_start = pit::uptime_ms();

    int cur_x = 12;
    int cur_y = 12;
    int max_w = doc->page_width > 200 ? doc->page_width - 24 : 760;
    int base_left = 12;
    int base_right = max_w;

    // Per-element layout snapshots (parallel to style_stack) so CSS padding,
    // width and margins can nest without leaking into sibling content.
    int layout_left[24] = {0};
    int layout_right[24] = {0};
    int layout_mb[24] = {0};
    int layout_pad[24] = {0};

    const char* p = html;
    const char* end = html + doc->source_bytes;

    TagType current_tag = TagType::UNKNOWN;
    char current_link[256] = {0};
    bool in_pre = false;
    bool in_title = false;
    bool in_list = false;
    bool in_ol = false;
    int ol_counter = 1;
    
    // Flex layout tracking
    bool& in_flex = g_in_flex;
    int& flex_start_y = g_flex_start_y;
    int& flex_max_h = g_flex_max_h;
    int& flex_cur_x = g_flex_cur_x;

    // ---- Simple table layout: buffer one row, then emit equal-width cells ----
    constexpr int TABLE_COLS = 8;
    char row_cells[TABLE_COLS][192];
    char row_links[TABLE_COLS][96];
    uint32_t row_fg[TABLE_COLS] = {0};
    uint32_t row_bg[TABLE_COLS] = {0};
    bool row_header[TABLE_COLS] = {false};
    int row_count = 0;
    int row_y = 0;
    char cell_buf[192];
    size_t cell_len = 0;
    char cell_link[96] = {0};
    uint32_t cell_fg = 0;
    uint32_t cell_bg = 0;
    bool cell_bold = false;
    bool in_cell = false;
    bool in_table = false;

    // ---- CSS: collect <style> blocks into the rule table ----
    g_rule_count = 0;
    for (const char* sp = html; sp + 6 <= end; ++sp) {
        if (sp[0] == '<' && ascii_lower(sp[1]) == 's' && ascii_lower(sp[2]) == 't' &&
            ascii_lower(sp[3]) == 'y' && ascii_lower(sp[4]) == 'l' && ascii_lower(sp[5]) == 'e') {
            const char* q = sp + 6;
            while (q < end && *q != '>') ++q;
            if (q >= end) break;
            ++q;
            const char* ce = q;
            while (ce + 8 <= end) {
                if (ce[0] == '<' && ce[1] == '/' && ascii_lower(ce[2]) == 's' &&
                    ascii_lower(ce[3]) == 't' && ascii_lower(ce[4]) == 'y' &&
                    ascii_lower(ce[5]) == 'l' && ascii_lower(ce[6]) == 'e') break;
                ++ce;
            }
            parse_stylesheet(q, static_cast<size_t>(ce - q));
            sp = ce;
        }
    }

    Style cur_style;
    Style style_stack[24];
    int style_top = 0;
    uint32_t default_fg = gfx::Colors::LightGrey;

    bool in_button = false;
    char button_buf[160];
    size_t button_len = 0;
    Style button_style;

    struct BgRegion { int box; int start_y; TagType tag; };
    BgRegion bg_stack[16];
    int bg_top = 0;

    int hidden_depth = 0;

    while (p < end) {
        if (*p == '<') {
            p++;
            if (p >= end) break;

            bool is_closing = false;
            if (*p == '/') {
                is_closing = true;
                p++;
            }

            // Extract tag name
            const char* tag_start = p;
            while (p < end && *p != '>' && *p != ' ' && *p != '/' && *p != '\t' && *p != '\n') p++;
            size_t tag_len = static_cast<size_t>(p - tag_start);
            TagType tag = resolve_tag(tag_start, tag_len);

            // Skip raw-text elements (script/style/etc.) entirely so their
            // contents are never tokenized or rendered as visible text.
            if (!is_closing && is_raw_text_tag(tag_start, tag_len)) {
                while (p < end && *p != '>') p++;   // finish opening tag
                if (p < end) p++;
                while (p < end) {
                    if (*p == '<' && p + 1 < end && p[1] == '/') {
                        const char* q = p + 2;
                        size_t k = 0;
                        while (k < tag_len && q + k < end) {
                            char c = q[k];
                            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                            char r = tag_start[k];
                            if (r >= 'A' && r <= 'Z') r = static_cast<char>(r - 'A' + 'a');
                            if (c != r) break;
                            ++k;
                        }
                        if (k == tag_len) {
                            while (p < end && *p != '>') p++;  // skip closing tag
                            if (p < end) p++;
                            break;
                        }
                    }
                    ++p;
                }
                current_tag = TagType::UNKNOWN;
                continue;
            }

            // Extract attributes if opening tag
            char href_val[128] = {0};
            char class_val[64] = {0};
            char id_val[48] = {0};
            char style_val[256] = {0};
            char type_val[24] = {0};
            char value_val[128] = {0};
            char ph_val[128] = {0};
            char alt_val[128] = {0};
char src_val[128] = {0};
            char align_val[16] = {0};
            char bgcolor_val[24] = {0};
            char color_val[24] = {0};
            int size_val = 0;
            int height_val = 0;
            int width_attr = 0;
            bool width_attr_pct = false;
            if (!is_closing) {
                while (p < end && *p != '>') {
                    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
                    if (*p == '>') break;

                    const char* attr_start = p;
                    while (p < end && *p != '=' && *p != ' ' && *p != '>') p++;
                    size_t attr_name_len = static_cast<size_t>(p - attr_start);
                    auto is_attr = [&](const char* n) {
                        size_t nl = kstd::strlen(n);
                        return attr_name_len == nl && !kstd::strncmp(attr_start, n, nl);
                    };

                    if (*p == '=') {
                        p++;
                        char quote = 0;
                        if (*p == '"' || *p == '\'') {
                            quote = *p++;
                        }
                        const char* val_start = p;
                        while (p < end && ((quote && *p != quote) || (!quote && *p != ' ' && *p != '>'))) p++;
                        size_t val_len = static_cast<size_t>(p - val_start);
                        if (quote && *p == quote) p++;

                        auto copy_attr = [&](char* dst, size_t cap) {
                            size_t n = val_len < cap - 1 ? val_len : cap - 1;
                            kstd::strncpy(dst, val_start, n);
                            dst[n] = '\0';
                        };
                        if (is_attr("href")) {
                            copy_attr(href_val, sizeof(href_val));
                        } else if (is_attr("class")) {
                            copy_attr(class_val, sizeof(class_val));
                        } else if (is_attr("id")) {
                            copy_attr(id_val, sizeof(id_val));
                        } else if (is_attr("style")) {
                            copy_attr(style_val, sizeof(style_val));
                        } else if (is_attr("type")) {
                            copy_attr(type_val, sizeof(type_val));
                        } else if (is_attr("value")) {
                            copy_attr(value_val, sizeof(value_val));
                        } else if (is_attr("placeholder")) {
                            copy_attr(ph_val, sizeof(ph_val));
                        } else if (is_attr("alt")) {
                            copy_attr(alt_val, sizeof(alt_val));
                        } else if (is_attr("src")) {
                            copy_attr(src_val, sizeof(src_val));
                        } else if (is_attr("align")) {
                            copy_attr(align_val, sizeof(align_val));
                        } else if (is_attr("bgcolor")) {
                            copy_attr(bgcolor_val, sizeof(bgcolor_val));
                        } else if (is_attr("color")) {
                            copy_attr(color_val, sizeof(color_val));
                        } else if (is_attr("width")) {
                            size_val = static_cast<int>(parse_px(val_start, val_len));
                            width_attr = size_val;
                            for (size_t k = 0; k < val_len; ++k) {
                                if (val_start[k] == '%') { width_attr_pct = true; break; }
                            }
                        } else if (is_attr("height")) {
                            height_val = static_cast<int>(parse_px(val_start, val_len));
                        } else if (is_attr("size")) {
                            size_val = static_cast<int>(parse_px(val_start, val_len));
                        } else if (is_attr("onclick")) {
                            const char* loc = kstd::strstr(val_start, "location.href='");
                            if (loc && loc < val_start + val_len) {
                                loc += 15;
                                const char* q_end = kstd::strchr(loc, '\'');
                                if (q_end && q_end <= val_start + val_len) {
                                    size_t l_len = static_cast<size_t>(q_end - loc);
                                    if (l_len > 127) l_len = 127;
                                    kstd::strncpy(href_val, loc, l_len);
                                    href_val[l_len] = '\0';
                                }
                            }
                        }
                    }
                }
            }

            while (p < end && *p != '>') p++;
            if (p < end && *p == '>') p++;

            // Lower-cased tag name for CSS matching.
            char tag_name[16];
            size_t tn = tag_len < 15 ? tag_len : 15;
            for (size_t k = 0; k < tn; ++k) tag_name[k] = ascii_lower(tag_start[k]);
            tag_name[tn] = '\0';

            // While inside a display:none subtree, only track nesting depth.
            if (hidden_depth > 0) {
                if (is_closing) { --hidden_depth; if (hidden_depth == 0) current_tag = TagType::UNKNOWN; }
                else ++hidden_depth;
                continue;
            }

            // Compute the element's computed style (opening tags only).
            bool voidtag = is_void_name(tag_name);
            if (!is_closing) {
                Style st;
                compute_style(tag_name, class_val, id_val, style_val, st);

                // Legacy presentational attributes, still common on older pages.
                if (align_val[0] && !st.has_align) {
                    size_t al = kstd::strlen(align_val);
                    if (ci_starts(align_val, al, "center")) { st.has_align = true; st.align = 1; }
                    else if (ci_starts(align_val, al, "right")) { st.has_align = true; st.align = 2; }
                    else if (ci_starts(align_val, al, "left")) { st.has_align = true; st.align = 0; }
                }
                if (tag == TagType::CENTER) { st.has_align = true; st.align = 1; }
                if (bgcolor_val[0] && !st.has_bg) {
                    uint32_t c;
                    if (parse_legacy_color(bgcolor_val, &c)) {
                        st.has_bg = true; st.bg = c; st.has_grad = false;
                    }
                }
                if (tag == TagType::FONT && color_val[0] && !st.has_fg) {
                    uint32_t c;
                    if (parse_legacy_color(color_val, &c)) { st.has_fg = true; st.fg = c; }
                }
                if (tag == TagType::TABLE && width_attr > 0 && !st.has_width) {
                    st.has_width = true; st.width = width_attr; st.width_pct = width_attr_pct;
                }
                if (tag == TagType::BLOCKQUOTE) {
                    if (!st.has_ml) { st.has_ml = true; st.ml = 24; }
                    if (!st.has_mb) { st.has_mb = true; st.mb = 10; }
                }
                // Default type scale for headings when CSS didn't set one.
                if (st.font_scale == 0) {
                    if (tag == TagType::H1) st.font_scale = 2;
                    else if (tag == TagType::H2 || tag == TagType::H3) st.font_scale = 1;
                }

                if (style_top > 0) {
                    Style& par = style_stack[style_top - 1];
                    if (!st.has_fg && par.has_fg) { st.has_fg = true; st.fg = par.fg; }
                    if (!st.bold) st.bold = par.bold;
                    if (!st.italic) st.italic = par.italic;
                    if (st.font_scale == 0) st.font_scale = par.font_scale;
                    if (!st.has_align && par.has_align) { st.has_align = true; st.align = par.align; }
                }
                cur_style = st;
                if (st.display_none) {
                    hidden_depth = 1;
                    continue;
                }

                int frame_left = base_left;
                int frame_right = base_right;
                int frame_mb = 0;
                int frame_pad = 0;
                int panel_left = base_left;
                int panel_right = base_right;
                int panel_top = cur_y;

                if (is_block_tag(tag)) {
                    int mt = st.has_mt ? st.mt : 0;
                    int mr = st.has_mr ? st.mr : 0;
                    int ml = st.has_ml ? st.ml : 0;
                    frame_mb = st.has_mb ? st.mb : 0;

                    if (cur_x > base_left + 2) {
                        cur_x = base_left;
                        int lh = 18;
                        if (doc->box_count > 0) {
                            const LayoutBox& lb = doc->boxes[doc->box_count - 1];
                            if (lb.type == BoxType::TEXT && lb.h > 16) lh = lb.h + 2;
                        }
                        cur_y += lh;
                    }
                    if (mt > 0) cur_y += mt;

                    int outer_l = base_left + ml;
                    int outer_r = base_right - mr;
                    if (outer_r < outer_l + 32) outer_r = outer_l + 32;

                    int avail = outer_r - outer_l;
                    int w = avail;
                    if (st.has_width) {
                        w = st.width_pct ? avail * st.width / 100 : st.width;
                        if (w > avail) w = avail;
                    }
                    if (st.has_max_width) {
                        int mw = st.max_width_pct ? avail * st.max_width / 100 : st.max_width;
                        if (mw > 0 && w > mw) w = mw;
                    }
                    if (w < 16) w = 16;
                    if (w > avail) w = avail;

                    // CSS "margin: 0 auto" and friends: centre (or right-align)
                    // the box inside the space left over by its width.
                    bool auto_l = st.has_ml && st.ml_auto;
                    bool auto_r = st.has_mr && st.mr_auto;
                    if (auto_l || auto_r) {
                        int free = avail - w;
                        if (free < 0) free = 0;
                        if (auto_l && auto_r) outer_l += free / 2;
                        else if (auto_l) outer_l += free;
                        outer_r = outer_l + w;
                    } else if (w < avail) {
                        outer_r = outer_l + w;
                    }
                    panel_left = outer_l;
                    panel_right = outer_r;
                    int pad = st.has_pad ? st.pad : 0;
                    if (pad < 0) pad = 0;
                    if (pad > 64) pad = 64;
                    frame_pad = pad;
                    panel_top = cur_y;
                    if (pad > 0) cur_y += pad;
                    base_left = outer_l + pad;
                    base_right = outer_r - pad;
                    if (base_right < base_left + 16) base_right = base_left + 16;
                    cur_x = base_left;
                }

                if (!voidtag && style_top < 24) {
                    layout_left[style_top] = frame_left;
                    layout_right[style_top] = frame_right;
                    layout_mb[style_top] = frame_mb;
                    layout_pad[style_top] = frame_pad;
                    style_stack[style_top++] = st;
                }
                
                // Track flex containers
                if (st.display_flex && !is_closing) {
                    g_in_flex = true;
                    g_flex_start_y = cur_y;
                    g_flex_cur_x = cur_x;
                    g_flex_max_h = 0;
                }

                if (tag == TagType::BODY || tag == TagType::HTML) {
                    if (st.has_bg) {
                        doc->bg_color = st.has_grad ? st.grad_top : st.bg;
                        doc->bg_color2 = st.has_grad ? st.grad_bottom : st.bg;
                        doc->has_bg_gradient = st.has_grad;
                        int lum = ((st.bg >> 16 & 0xFF) * 299 + (st.bg >> 8 & 0xFF) * 587 + (st.bg & 0xFF) * 114) / 1000;
                        if (lum > 140) default_fg = 0xFF1F2937;
                        else default_fg = gfx::Colors::LightGrey;
                    }
                } else {
                    bool is_inline = (tag == TagType::A || tag == TagType::SPAN || tag == TagType::B ||
                                      tag == TagType::STRONG || tag == TagType::I || tag == TagType::EM ||
                                      tag == TagType::CODE || tag == TagType::FONT);
                    bool want_panel = !is_inline && !voidtag && tag != TagType::BUTTON &&
                                      tag != TagType::TD && tag != TagType::TH && tag != TagType::TR &&
                                      (st.has_bg || (st.has_border && st.border != 0));
                    if (want_panel && bg_top < 16 && !in_pre) {
                        LayoutBox* pb = add_box(doc);
                        if (pb) {
                            pb->type = BoxType::BLOCK;
                            pb->x = panel_left;
                            pb->y = panel_top;
                            pb->w = panel_right - panel_left;
                            pb->h = 0;
                            pb->has_bg = st.has_bg;
                            pb->has_gradient = st.has_bg && st.has_grad;
                            pb->bg_color = st.has_bg ? (st.has_grad ? st.grad_top : st.bg) : 0;
                            pb->bg_color2 = pb->has_gradient ? st.grad_bottom : pb->bg_color;
                            pb->has_border = st.has_border && st.border != 0;
                            pb->border_color = pb->has_border ? st.border : 0;
                            bg_stack[bg_top].box = static_cast<int>(doc->box_count) - 1;
                            bg_stack[bg_top].start_y = panel_top;
                            bg_stack[bg_top].tag = tag;
                            ++bg_top;
                        }
                    }
                }
            }

            // Handle Tags
            if (is_closing) {
                // Close flex container
                if (g_in_flex && tag == TagType::DIV) {
                    g_in_flex = false;
                    cur_x = base_left;
                    cur_y = g_flex_start_y + g_flex_max_h + 8;
                }
                if (bg_top > 0 && bg_stack[bg_top - 1].tag == tag) {
                    int idx = bg_stack[bg_top - 1].box;
                    int pad = (style_top > 0) ? layout_pad[style_top - 1] : 0;
                    int tail_h = 16;
                    if (doc->box_count > 0) {
                        const LayoutBox& lb = doc->boxes[doc->box_count - 1];
                        if (lb.type == BoxType::TEXT && lb.h > 16) tail_h = lb.h;
                    }
                    int h = cur_y + tail_h + pad - bg_stack[bg_top - 1].start_y;
                    if (h < 24) h = 24;
                    if (idx >= 0 && static_cast<size_t>(idx) < doc->box_count) {
                        doc->boxes[idx].h = h;
                        if (doc->boxes[idx].w <= 0) doc->boxes[idx].w = base_right - base_left;
                    }
                    --bg_top;
                }

                // Emit a styled button box when a <button> closes.
                if (tag == TagType::BUTTON && in_button) {
                    LayoutBox* bb = add_box(doc);
                    if (bb) {
                        int tw = static_cast<int>(button_len) * 8;
                        int w = tw + 32;
                        if (w < 72) w = 72;
                        bb->type = BoxType::BUTTON;
                        bb->is_button = true;
                        bb->is_clickable = true;
                        bb->x = cur_x;
                        bb->y = cur_y;
                        bb->w = w;
                        bb->h = 28;
                        bb->has_bg = true;
                        bb->bg_color = button_style.has_bg ? button_style.bg : 0xFF2563EB;
                        bb->has_border = true;
                        bb->border_color = button_style.has_border ? button_style.border : 0xFF60A5FA;
                        bb->fg_color = button_style.has_fg ? button_style.fg : gfx::Colors::White;
                        button_buf[button_len] = '\0';
                        kstd::strncpy(bb->text, button_buf, 127);
                        bb->text[127] = '\0';
                        cur_x += w + 8;
                    }
                    in_button = false;
                }

                // Pop the element's inherited style and layout frame.
                int close_scale = 1;
                if (style_top > 0) {
                    int fs = style_stack[style_top - 1].font_scale;
                    if (fs > 0) close_scale = fs + 1;
                    if (close_scale > 3) close_scale = 3;
                }
                bool closed_block = is_block_tag(tag);
                if (style_top > 0) {
                    --style_top;
                    base_left = layout_left[style_top];
                    base_right = layout_right[style_top];
                    cur_y += layout_pad[style_top];
                    cur_y += layout_mb[style_top];
                }
                cur_style = style_top > 0 ? style_stack[style_top - 1] : Style{};

                if (tag == TagType::A || tag == TagType::BUTTON) {
                    current_link[0] = '\0';
                } else if (tag == TagType::PRE) {
                    in_pre = false;
                    cur_x = base_left;
                    cur_y += 20;
                } else if (tag == TagType::TITLE) {
                    in_title = false;
                } else if (tag == TagType::H1 || tag == TagType::H2 || tag == TagType::H3 || tag == TagType::H4) {
                    cur_x = base_left;
                    cur_y += 18 * close_scale;
                } else if (tag == TagType::P) {
                    cur_x = base_left;
                    cur_y += 18 * close_scale;
                } else if (tag == TagType::UL || tag == TagType::OL) {
                    in_list = false;
                    in_ol = false;
                    cur_x = base_left;
                    cur_y += 14;
                } else if (tag == TagType::TD || tag == TagType::TH) {
                    if (in_cell && row_count < TABLE_COLS) {
                        cell_buf[cell_len] = '\0';
                        kstd::strncpy(row_cells[row_count], cell_buf, sizeof(row_cells[0]) - 1);
                        row_cells[row_count][sizeof(row_cells[0]) - 1] = '\0';
                        kstd::strncpy(row_links[row_count], cell_link, sizeof(row_links[0]) - 1);
                        row_links[row_count][sizeof(row_links[0]) - 1] = '\0';
                        row_fg[row_count] = cell_fg;
                        row_bg[row_count] = cell_bg;
                        row_header[row_count] = cell_bold;
                        ++row_count;
                    }
                    in_cell = false;
                    cell_len = 0;
                    cell_link[0] = '\0';
                    cell_fg = 0;
                    cell_bg = 0;
                    cell_bold = false;
                    cur_x = base_left;
                } else if (tag == TagType::TR) {
                    if (row_count > 0 && base_right > base_left + 8) {
                        int cols = row_count;
                        int avail = base_right - base_left;
                        int cw = avail / cols;
                        if (cw < 24) cw = 24;
                        int bx = base_left;
                        for (int c = 0; c < cols; ++c) {
                            int w = (c == cols - 1) ? (base_right - bx) : cw;
                            if (w > base_right - bx) w = base_right - bx;
                            if (w < 16) break;
                            LayoutBox* cb = add_box(doc);
                            if (!cb) break;
                            cb->type = BoxType::TEXT;
                            cb->x = bx + 5;
                            cb->y = row_y + 5;
                            cb->w = w - 10;
                            cb->h = 20;
                            cb->has_bg = true;
                            cb->bg_color = row_bg[c] ? row_bg[c]
                                            : (row_header[c] ? 0xFF1E293B : 0xFF0F172A);
                            cb->has_border = true;
                            cb->border_color = 0xFF334155;
                            cb->fg_color = row_fg[c] ? row_fg[c]
                                            : (row_header[c] ? gfx::Colors::White : 0xFFCBD5E1);
                            cb->is_bold = row_header[c];
                            if (row_links[c][0]) {
                                kstd::strncpy(cb->link_url, row_links[c], 127);
                                cb->link_url[127] = '\0';
                                cb->is_clickable = true;
                            }
                            size_t maxc = static_cast<size_t>((cb->w > 8 ? cb->w - 8 : 0) / 8);
                            if (maxc > 126) maxc = 126;
                            size_t n = kstd::strlen(row_cells[c]);
                            if (n > maxc) n = maxc;
                            for (size_t k = 0; k < n; ++k) cb->text[k] = row_cells[c][k];
                            cb->text[n] = '\0';
                            bx += w;
                        }
                        cur_y = row_y + 28;
                    }
                    row_count = 0;
                    cur_x = base_left;
                } else if (tag == TagType::TABLE) {
                    in_table = false;
                    row_count = 0;
                    cur_x = base_left;
                    cur_y += 6;
                } else if (closed_block) {
                    cur_x = base_left;
                }
                current_tag = TagType::UNKNOWN;
            } else {
                current_tag = tag;
                if (tag == TagType::BUTTON) {
                    in_button = true;
                    button_len = 0;
                    button_style = cur_style;
                } else if (tag == TagType::INPUT) {
                    bool submit = (!kstd::strcmp(type_val, "submit") ||
                                   !kstd::strcmp(type_val, "button") ||
                                   !kstd::strcmp(type_val, "reset"));
                    LayoutBox* ib = add_box(doc);
                    if (ib) {
                        char disp_buf[128];
                        decode_entities(value_val[0] ? value_val : ph_val, disp_buf, sizeof(disp_buf));
                        const char* disp = disp_buf[0] ? disp_buf : (submit ? "Submit" : "");
                        kstd::strncpy(ib->text, disp, 127);
                        ib->text[127] = '\0';
                        size_t dl = kstd::strlen(ib->text);
                        int w = size_val > 0 ? size_val * 8 + 16
                                             : (submit ? static_cast<int>(dl) * 8 + 28 : 180);
                        if (w < 56) w = 56;
                        ib->type = BoxType::INPUT_BOX;
                        ib->is_input = true;
                        ib->is_button = submit;
                        ib->is_clickable = submit;
                        ib->x = g_in_flex ? g_flex_cur_x : cur_x;
                        ib->y = g_in_flex ? g_flex_start_y : cur_y;
                        ib->w = w;
                        ib->h = 26;
                        ib->has_bg = true;
                        ib->bg_color = cur_style.has_bg ? cur_style.bg
                                                              : (submit ? 0xFF2563EB : 0xFFFFFFFF);
                        ib->has_border = true;
                        ib->border_color = cur_style.has_border ? cur_style.border
                                                             : (submit ? 0xFF60A5FA : 0xFFD2D2D2);
                        ib->fg_color = cur_style.has_fg ? cur_style.fg
                                                             : (submit ? gfx::Colors::White : 0xFF1F1F1F);
                        cur_x += w + 6;
                    }
                    current_tag = TagType::UNKNOWN;
                } else if (tag == TagType::IMG) {
                    // No image decoder yet: reserve the image's box and show
                    // its alt text so page structure stays intact.
                    int iw = size_val > 0 ? size_val : 48;
                    int ih = height_val > 0 ? height_val : 36;
                    if (iw < 16) iw = 16;
                    if (ih < 16) ih = 16;
                    if (iw > 640) iw = 640;
                    if (ih > 480) ih = 480;
                    if (cur_x + iw > base_right && cur_x > base_left + 2) {
                        cur_x = base_left;
                        cur_y += 18;
                    }
                    LayoutBox* ib = add_box(doc);
                    if (ib) {
                        ib->type = BoxType::TEXT;
                        ib->x = g_in_flex ? g_flex_cur_x : cur_x;
                        ib->y = g_in_flex ? g_flex_start_y : cur_y;
                        ib->w = iw;
                        ib->h = ih;
                        ib->has_bg = true;
                        ib->bg_color = 0xFF13202E;
                        ib->has_border = true;
                        ib->border_color = 0xFF2C4A63;
                        ib->fg_color = 0xFF94A3B8;
                        kstd::strcpy(ib->img_src, src_val);
                        if (alt_val[0]) {
                            decode_entities(alt_val, ib->text, sizeof(ib->text));
                        } else {
                            kstd::strcpy(ib->text, "[img]");
                        }
                        size_t max_chars = static_cast<size_t>((iw - 8) / 8);
                        if (max_chars < 127) ib->text[max_chars] = '\0';
                    }
                    cur_x += iw + 8;
                    current_tag = TagType::UNKNOWN;
                } else if (tag == TagType::TITLE) {
                    in_title = true;
                } else if (tag == TagType::A || tag == TagType::BUTTON) {
                    if (href_val[0]) {
                        kstd::strncpy(current_link, href_val, 127);
                        current_link[127] = '\0';
                    }
                } else if (tag == TagType::HR) {
                    cur_x = base_left;
                    cur_y += 10;
                    LayoutBox* hr_box = add_box(doc);
                    if (hr_box) {
                        int hr_w = base_right - base_left - 8;
                        if (hr_w < 4) hr_w = 4;
                        hr_box->type = BoxType::HR;
                        hr_box->x = base_left;
                        hr_box->y = cur_y;
                        hr_box->w = hr_w;
                        hr_box->h = 2;
                        hr_box->fg_color = 0xFF284868;
                    }
                    cur_y += 14;
                } else if (tag == TagType::BR) {
                    cur_x = base_left + (in_list ? 16 : 0);
                    cur_y += 18;
                } else if (tag == TagType::PRE) {
                    in_pre = true;
                    cur_x = base_left + 4;
                    cur_y += 10;
                } else if (tag == TagType::H1) {
                    cur_x = base_left;
                    cur_y += 16;
                } else if (tag == TagType::H2) {
                    cur_x = base_left;
                    cur_y += 12;
                } else if (tag == TagType::H3) {
                    cur_x = base_left;
                    cur_y += 10;
                } else if (tag == TagType::H4) {
                    cur_x = base_left;
                    cur_y += 8;
                } else if (tag == TagType::P) {
                    cur_x = base_left;
                    cur_y += 10;
                } else if (tag == TagType::OL) {
                    in_list = true;
                    in_ol = true;
                    ol_counter = 1;
                    cur_x = base_left;
                    cur_y += 10;
                } else if (tag == TagType::UL) {
                    in_list = true;
                    in_ol = false;
                    cur_x = base_left;
                    cur_y += 10;
                } else if (tag == TagType::TABLE) {
                    in_table = true;
                    row_count = 0;
                    cur_x = base_left;
                    cur_y += 4;
                } else if (tag == TagType::TR) {
                    row_y = cur_y;
                    row_count = 0;
                    cur_x = base_left;
                } else if (tag == TagType::TD || tag == TagType::TH) {
                    in_cell = true;
                    cell_len = 0;
                    cell_link[0] = '\0';
                    cell_fg = cur_style.has_fg ? cur_style.fg : 0;
                    cell_bg = cur_style.has_bg ? cur_style.bg : 0;
                    cell_bold = (tag == TagType::TH) || cur_style.bold;
                    cur_x = base_left;
                } else if (tag == TagType::LI) {
                    cur_x = base_left + 2;
                    cur_y += 18;
                    LayoutBox* bullet = add_box(doc);
                    if (bullet) {
                        bullet->type = BoxType::BULLET;
                        bullet->x = cur_x;
                        bullet->y = cur_y;
                        bullet->h = 16;
                        if (in_ol) {
                            bullet->fg_color = gfx::Colors::Yellow;
                            char num_buf[16];
                            kstd::utoa(ol_counter++, num_buf, 10);
                            size_t nlen = kstd::strlen(num_buf);
                            num_buf[nlen] = '.';
                            num_buf[nlen + 1] = '\0';
                            kstd::strcpy(bullet->text, num_buf);
                            bullet->w = static_cast<int>((nlen + 1) * 8);
                            cur_x = base_left + 14 + bullet->w;
                        } else {
                            bullet->fg_color = gfx::Colors::Teal;
                            bullet->w = 8;
                            kstd::strcpy(bullet->text, "*");
                            cur_x = base_left + 16;
                        }
                    } else {
                        cur_x = base_left + 16;
                    }
                }
            }
            continue;
        }

        // Text accumulation
        const char* text_start = p;
        while (p < end && *p != '<') p++;
        size_t text_len = static_cast<size_t>(p - text_start);
        if (text_len == 0) continue;
        if (hidden_depth > 0) continue;
        
        // Check if text is only whitespace
        bool only_ws = true;
        for (size_t i = 0; i < text_len; ++i) {
            if (text_start[i] != ' ' && text_start[i] != '\\t' && text_start[i] != '\\r' && text_start[i] != '\\n') {
                only_ws = false;
                break;
            }
        }
        
        if (only_ws) {
            // Add a space between inline elements
            if (cur_x > base_left) cur_x += 8;
            continue;
        }

        char raw_text[256];
        size_t copy_len = text_len < 255 ? text_len : 255;
        kstd::strncpy(raw_text, text_start, copy_len);
        raw_text[copy_len] = '\0';

        if (in_title) {
            kstd::strncpy(doc->title, raw_text, 63);
            doc->title[63] = '\0';
            continue;
        }

        // Whitespace between table rows/cells must not create blank lines.
        if (in_table && !in_cell) {
            bool only_ws = true;
            for (const char* q = raw_text; *q; ++q) {
                if (*q != ' ' && *q != '\t' && *q != '\r' && *q != '\n') { only_ws = false; break; }
            }
            if (only_ws) continue;
        }

        // Inside a table cell: collect the text and lay it out at row end.
        if (in_cell) {
            char dec[256];
            decode_entities(raw_text, dec, sizeof(dec));
            for (const char* q = dec; *q && cell_len + 1 < sizeof(cell_buf); ++q) {
                char c = *q;
                if (c == '\n' || c == '\r' || c == '\t') c = ' ';
                if (c == ' ' && (cell_len == 0 || cell_buf[cell_len - 1] == ' ')) continue;
                cell_buf[cell_len++] = c;
            }
            if (!cell_link[0] && current_link[0]) {
                kstd::strncpy(cell_link, current_link, sizeof(cell_link) - 1);
                cell_link[sizeof(cell_link) - 1] = '\0';
            }
            if (cur_style.has_fg) cell_fg = cur_style.fg;
            if (cur_style.has_bg) cell_bg = cur_style.bg;
            if (cur_style.bold || current_tag == TagType::B ||
                current_tag == TagType::STRONG || current_tag == TagType::TH) {
                cell_bold = true;
            }
            continue;
        }

        if (in_pre) {
            // Preformatted text keeps exact spacing line-by-line
            char decoded[256];
            decode_entities(raw_text, decoded, sizeof(decoded));
            const char* line_ptr = decoded;
            while (*line_ptr) {
                const char* next_nl = kstd::strchr(line_ptr, '\n');
                size_t line_len = next_nl ? static_cast<size_t>(next_nl - line_ptr) : kstd::strlen(line_ptr);
                if (line_len > 0 && line_ptr[line_len - 1] == '\r') {
                    line_len--;
                }
                LayoutBox* box = add_box(doc);
                if (box) {
                    box->type = BoxType::CODE_BOX;
                    box->x = g_in_flex ? g_flex_cur_x : cur_x;
                    box->y = g_in_flex ? g_flex_start_y : cur_y;
                    box->w = static_cast<int>(line_len * 8 + 12);
                    box->h = 18;
                    box->bg_color = 0xFF08121C;
                    box->fg_color = gfx::Colors::Green;
                    box->has_bg = (line_len > 0);
                    size_t c_len = line_len < 127 ? line_len : 127;
                    kstd::strncpy(box->text, line_ptr, c_len);
                    box->text[c_len] = '\0';
                }
                cur_y += 18;
                if (!next_nl) break;
                line_ptr = next_nl + 1;
            }
            continue;
        }

        // Accumulate label text for a <button> into its buffer.
        if (in_button) {
            char dec[256];
            decode_entities(raw_text, dec, sizeof(dec));
            for (const char* q = dec; *q && button_len + 1 < sizeof(button_buf); ++q) {
                char c = *q;
                if (c == '\n' || c == '\r' || c == '\t') c = ' ';
                if (c == ' ' && (button_len == 0 || button_buf[button_len - 1] == ' ')) continue;
                button_buf[button_len++] = c;
            }
            continue;
        }

        // Typography styling: CSS computed style overrides tag defaults.
        uint32_t fg = cur_style.has_fg ? cur_style.fg : default_fg;
        bool bold = cur_style.bold;
        bool underline = false;
        bool is_btn = false;

        if (!cur_style.has_fg) {
            if (current_tag == TagType::H1) fg = 0xFF00E5FF;
            else if (current_tag == TagType::H2) fg = gfx::Colors::White;
            else if (current_tag == TagType::H3) fg = gfx::Colors::Teal;
            else if (current_tag == TagType::H4) fg = 0xFF94A3B8;
            else if (current_tag == TagType::B || current_tag == TagType::STRONG) fg = gfx::Colors::White;
            else if (current_tag == TagType::CODE) fg = gfx::Colors::Green;
        }
        if (current_tag == TagType::H1 || current_tag == TagType::H2 ||
            current_tag == TagType::H3 || current_tag == TagType::H4 ||
            current_tag == TagType::B || current_tag == TagType::STRONG) {
            bold = true;
        }

        if (current_tag == TagType::A || current_link[0] != '\0') {
            bool looks_btn = cur_style.has_bg || (cur_style.has_border && cur_style.border != 0);
            if (looks_btn) {
                if (!cur_style.has_fg) fg = gfx::Colors::White;
            } else {
                if (!cur_style.has_fg) fg = 0xFF38BDF8;
                underline = !cur_style.no_underline;
            }
        }
        if (cur_style.no_underline) underline = false;
        if (cur_style.underline) underline = true;

        bool inline_ctx = (current_tag == TagType::A || current_tag == TagType::SPAN ||
                           current_tag == TagType::B || current_tag == TagType::STRONG ||
                           current_tag == TagType::I || current_tag == TagType::EM ||
                           current_tag == TagType::CODE || current_tag == TagType::FONT);
        bool text_bg = inline_ctx && cur_style.has_bg;

        int align = cur_style.has_align ? cur_style.align : 0;
        int text_scale = 1 + cur_style.font_scale;
        if (text_scale > 6) text_scale = 6;
        add_wrapped_text(doc, raw_text, cur_x, cur_y, base_left, base_right, align,
                         fg, bold, underline,
                         current_link[0] ? current_link : nullptr, is_btn,
                         text_bg, cur_style.bg, text_scale);
    }

    int tail_h = 16;
    if (doc->box_count > 0) {
        const LayoutBox& lb = doc->boxes[doc->box_count - 1];
        if (lb.type == BoxType::TEXT && lb.h > 16) tail_h = lb.h;
    }
    doc->page_height = cur_y + tail_h + 40;
    doc->load_time_ms = static_cast<uint32_t>(pit::uptime_ms() - t_start);
    return true;
}

void layout(Document* doc, int viewport_width) {
    if (!doc) return;
    doc->page_width = viewport_width;
}

// Strict Viewport Clipping Renderer (60Hz..240Hz Performance Optimized)
void render(Document& doc, int vx, int vy, int vw, int vh, int scroll_y) {
    if (!doc.dirty && doc.scroll_y == scroll_y) return;
    doc.dirty = false;
    // 1. Fill document background
    if (doc.has_bg_gradient) {
        gfx::draw_gradient_v(vx, vy, vw, vh, doc.bg_color, doc.bg_color2);
    } else {
        gfx::fill_rect(vx, vy, vw, vh, doc.bg_color);
    }

    // 2. Render each visible box within viewport [vy, vy + vh]
    for (size_t i = 0; i < doc.box_count; ++i) {
        const LayoutBox& box = doc.boxes[i];

        int screen_y = vy + box.y - scroll_y;

        // Viewport Clipping Optimization: Skip off-screen elements entirely!
        if (screen_y + box.h < vy || screen_y > vy + vh) {
            continue;
        }

        int screen_x = vx + box.x;

        // Ensure box does not bleed outside right viewport boundary
        if (screen_x >= vx + vw) continue;

        // CSS block panels (element backgrounds / borders) render first and
        // contain no text of their own.
        if (box.type == BoxType::BLOCK) {
            if (box.has_bg) {
                if (box.has_gradient) {
                    gfx::draw_gradient_v(screen_x, screen_y, box.w, box.h,
                                         box.bg_color, box.bg_color2);
                } else {
                    gfx::fill_rect(screen_x, screen_y, box.w, box.h, box.bg_color);
                }
            }
            if (box.has_border) gfx::draw_rect(screen_x, screen_y, box.w, box.h, box.border_color);
            continue;
        }

        // Render image if box has an img_src
        if (box.img_src[0] != '\\0') {
            // Draw cached image if available, otherwise placeholder
            if (box.img_pixels) {
                gfx::Image img = {};
                img.pixels = box.img_pixels;
                img.width = box.img_w;
                img.height = box.img_h;
                gfx::draw_image_scaled(img, screen_x, screen_y, box.w, box.h);
            } else {
                gfx::fill_rect(screen_x, screen_y, box.w, box.h, 0xFF13202E);
                gfx::draw_rect(screen_x, screen_y, box.w, box.h, 0xFF2C4A63);
                if (box.text[0]) {
                    gfx::draw_string(box.text, screen_x + 4, screen_y + (box.h - 10) / 2, box.fg_color);
                }
            }
            continue;
        }
        if (box.is_button || box.is_input) {
            uint32_t bg = box.has_bg ? box.bg_color
                                     : (box.is_hovered ? 0xFF2A5C8A : 0xFF183B5E);
            uint32_t border = box.is_hovered ? gfx::Colors::Cyan
                                             : (box.has_border ? box.border_color : gfx::Colors::Teal);
            gfx::fill_rect(screen_x, screen_y, box.w, box.h, bg);
            gfx::draw_rect(screen_x, screen_y, box.w, box.h, border);
            int ty = screen_y + (box.h - 10) / 2;
            gfx::draw_string_ttf(box.text, screen_x + 6, ty, box.fg_color, 14);
            continue;
        }

        if (box.has_bg) {
            gfx::fill_rect(screen_x - 4, screen_y - 2, box.w + 8, box.h + 4, box.bg_color);
            gfx::draw_rect(screen_x - 4, screen_y - 2, box.w + 8, box.h + 4,
                           box.has_border ? box.border_color : 0xFF1C3A5A);
        }

        // Render Horizontal Rule
        if (box.type == BoxType::HR) {
            int line_w = (screen_x + box.w < vx + vw - 12) ? box.w : (vx + vw - 12 - screen_x);
            if (line_w > 0) {
                gfx::draw_line(screen_x, screen_y, screen_x + line_w, screen_y, box.fg_color);
            }
            continue;
        }

        // Render Text using TrueType font
        uint32_t fg = box.is_hovered ? gfx::Colors::Yellow : box.fg_color;
        int font_size = 14 + (box.scale > 1 ? (box.scale - 1) * 4 : 0);
        if (font_size < 12) font_size = 12;
        if (font_size > 48) font_size = 48;
        gfx::draw_string_ttf(box.text, screen_x, screen_y, fg, font_size);

        // Hyperlink Underline
        if (box.is_underline && box.w > 0) {
            int line_w = (screen_x + box.w < vx + vw - 12) ? box.w : (vx + vw - 12 - screen_x);
            if (line_w > 0) {
                int uy = screen_y + box.h - 2;
                gfx::draw_line(screen_x, uy, screen_x + line_w, uy, fg);
            }
        }
    }
}

// Hit Testing for Clickable Links & Buttons
const LayoutBox* hit_test(const Document& doc, int x, int y, int scroll_y) {
    int doc_y = y + scroll_y;
    for (size_t i = 0; i < doc.box_count; ++i) {
        const LayoutBox& box = doc.boxes[i];
        if (!box.is_clickable && !box.is_button) continue;

        if (x >= box.x && x <= box.x + box.w &&
            doc_y >= box.y && doc_y <= box.y + box.h) {
            return &box;
        }
    }
    return nullptr;
}

// Update Hover Highlight State for Interactive Boxes
int update_hover(Document& doc, int x, int y, int scroll_y) {
    int doc_y = y + scroll_y;
    int hovered = -1;
    for (size_t i = 0; i < doc.box_count; ++i) {
        LayoutBox& box = doc.boxes[i];
        bool interactive = box.is_clickable || box.is_button;
        bool hit = interactive &&
                   x >= box.x && x <= box.x + box.w &&
                   doc_y >= box.y && doc_y <= box.y + box.h;
        box.is_hovered = hit;
        if (hit && hovered < 0) {
            hovered = static_cast<int>(i);
        }
    }
    return hovered;
}

// Built-in Internal Pages (about:home, about:docs, about:acid, about:about)
const char* get_builtin_page(const char* url) {
    if (!url || !kstd::strcmp(url, "about:home") || !kstd::strcmp(url, "about:blank") || url[0] == '\0') {
        return "<html><head><title>AetherOS Web Browser</title></head><body>"
               "<h1>AetherOS Web Portal</h1>"
               "<p>Welcome to the <b>AetherOS High-Performance Native Browser</b> running on bare-metal x86_64 Long Mode with 4-level paging.</p>"
               "<hr>"
               "<h2>Featured Destinations</h2>"
               "<ul>"
               "  <li><a href=\"/index.html\">AetherOS Home Portal (RamFS)</a> - System overview & specs</li>"
               "  <li><a href=\"/docs.html\">Kernel Documentation</a> - Hardware drivers & memory management</li>"
               "  <li><a href=\"/acid.html\">Web Standards Acid Test</a> - Typography, buttons, boxes & lists</li>"
               "  <li><a href=\"/news.html\">Daily Dispatch</a> - Release notes & changelog</li>"
               "  <li><a href=\"about:acid\">Built-in Acid Benchmark</a> - Fast in-memory layout stress test</li>"
               "  <li><a href=\"http://10.0.2.2/\">Host Network Gateway</a> - Live HTTP server connection</li>"
               "</ul>"
               "<hr>"
               "<h2>System Status</h2>"
               "<p>Network: <b>Gigabit Ethernet (eth0) & Wi-Fi 802.11ax (wlan0)</b> with automatic carrier failover.</p>"
               "<p><button>Reload Page</button> <button>System Specs</button></p>"
               "</body></html>";
    }

    if (!kstd::strcmp(url, "about:about") || !kstd::strcmp(url, "about:help")) {
        return "<html><head><title>About Aether Browser</title></head><body>"
               "<h1>About Aether Web Browser</h1>"
               "<p>Version: <b>1.0 Modern Web Edition (C++20)</b></p>"
               "<hr>"
               "<h2>Supported Features</h2>"
               "<ul>"
               "  <li><b>HTML Tokenizer:</b> Headings (h1..h4), paragraphs, lists (ul/ol/li), rules (hr)</li>"
               "  <li><b>Hyperlinks:</b> Relative URL resolution, history navigation & app:// bundles</li>"
               "  <li><b>CSS 2.1:</b> Selectors, colours, borders, padding, %/max-width, margin:auto & text-align</li>"
               "  <li><b>Typography:</b> Scaled headings (1x-3x), CSS font-size, faux bold & underlines</li>"
               "  <li><b>Gradients:</b> Vertical linear-gradient page and panel backgrounds</li>"
               "  <li><b>Legacy HTML:</b> center, blockquote, font color, align & bgcolor attributes</li>"
               "  <li><b>Tables:</b> Aligned equal-width columns with header styling and linked cells</li>"
               "  <li><b>Box Model:</b> Dynamic word-wrapping & code boxes with dark slate backgrounds</li>"
               "  <li><b>Viewport Clipping:</b> Sub-millisecond redraws with zero off-screen rendering</li>"
               "  <li><b>HTTP Client:</b> Redirects, chunked bodies, gzip/deflate over Intel e1000 & Wi-Fi</li>"
               "</ul>"
               "<p><a href=\"about:home\">&lt;&lt; Return to Home</a></p>"
               "</body></html>";
    }

    if (!kstd::strcmp(url, "about:acid")) {
        return "<html><head><title>Acid Standards Test</title></head><body>"
               "<h1>Acid-1 Layout Compliance Test</h1>"
               "<p><a href=\"about:home\">&lt;&lt; Home</a></p>"
               "<hr>"
               "<h2>Section 1: Typography</h2>"
               "<p>Testing <b>bold styling</b>, <i>italic emphasis</i>, and <code>inline code snippets</code>.</p>"
               "<h2>Section 2: Interactive Controls</h2>"
               "<p><button>Interactive Button 1</button> <button>Action 2</button></p>"
               "<h2>Section 3: Preformatted Blocks</h2>"
               "<pre>"
               "// AetherOS Fast Render Loop\n"
               "void render_frame() {\n"
               "    gfx::swap_buffers();\n"
               "}\n"
               "</pre>"
               "</body></html>";
    }

    return nullptr;
}

// Renders the secure-connection failure page with the actual TLS error.
static void render_tls_note(Document* doc, const char* host) {
    char tls_page[1024];
    kstd::strcpy(tls_page, "<html><head><title>Secure Connection Failed</title></head><body>"
                           "<h1>Secure Connection Failed</h1>"
                           "<p>Could not establish a trusted TLS session with <b>");
    size_t toff = kstd::strlen(tls_page);
    kstd::strncpy(tls_page + toff, host, 63);
    tls_page[toff + 63] = '\0';
    kstd::strcpy(tls_page + kstd::strlen(tls_page),
                 "</b>.</p><p><b>Reason:</b> <code>");
    const char* err = tls::last_error();
    size_t eoff = kstd::strlen(tls_page);
    kstd::strncpy(tls_page + eoff, (err && *err) ? err : "unknown TLS error", 95);
    tls_page[eoff + 95] = '\0';
    kstd::strcpy(tls_page + kstd::strlen(tls_page),
                 "</code></p><p>Check the hostname, the server certificate, and the system clock.</p>"
                 "<hr><p><a href=\"about:home\">&lt;&lt; Return to Home Portal</a></p></body></html>");
    parse_html(doc, tls_page);
}

// ---------------------------------------------------------------------------
// Inline JavaScript execution
//
// Before layout we scan the document for inline <script> blocks, run each one
// through the AetherScript engine, and splice the document.write() output back
// into the HTML in place of the script. Scripts loaded via src= are ignored
// (no network fetcher for them yet). This is a single pass: output emitted by
// a script is not itself re-scanned for scripts.
// ---------------------------------------------------------------------------
namespace {

struct ScriptSink {
    char* buf;
    size_t len;
    size_t cap;
};

void script_sink_write(void* user, const char* html) {
    ScriptSink* s = static_cast<ScriptSink*>(user);
    if (!s || !html || !s->buf) return;
    size_t n = kstd::strlen(html);
    if (s->len + n + 1 > s->cap) {
        if (s->len + 1 >= s->cap) return;
        n = s->cap - s->len - 1;
    }
    for (size_t i = 0; i < n; ++i) s->buf[s->len++] = html[i];
    s->buf[s->len] = '\0';
}

bool ci_prefix(const char* a, const char* b) {
    while (*b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

} // namespace

// Returns a heap buffer holding the HTML with scripts executed, or nullptr if
// the input has no scripts (caller should parse the original in that case).
static char* fetch_url_content(const char* url) {
    if (!url || !*url) return nullptr;
    
    const char* u = url;
    bool is_https = false;
    if (!kstd::strncmp(u, "http://", 7)) {
        u += 7;
    } else if (!kstd::strncmp(u, "https://", 8)) {
        u += 8;
        is_https = true;
    }
    
    const char* slash = kstd::strchr(u, '/');
    size_t authority_len = slash ? static_cast<size_t>(slash - u) : kstd::strlen(u);
    const char* colon = nullptr;
    for (size_t i = 0; i < authority_len; ++i) {
        if (u[i] == ':') { colon = u + i; break; }
    }
    size_t host_len = colon ? static_cast<size_t>(colon - u) : authority_len;
    if (host_len >= 128) host_len = 127;
    
    char host[128];
    kstd::strncpy(host, u, host_len);
    host[host_len] = '\0';
    
    uint16_t port = is_https ? 443 : 80;
    if (colon) {
        uint32_t parsed = 0;
        const char* d = colon + 1;
        const char* aend = u + authority_len;
        while (d < aend && *d >= '0' && *d <= '9') {
            parsed = parsed * 10 + static_cast<uint32_t>(*d - '0');
            ++d;
        }
        if (parsed > 0 && parsed < 65536) port = static_cast<uint16_t>(parsed);
    }
    
    char path[256];
    if (slash) {
        kstd::strncpy(path, slash, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    } else {
        path[0] = '/';
        path[1] = '\0';
    }
    
    uint8_t target_ip[4];
    bool resolved = net::parse_ip(host, target_ip);
    if (!resolved && net::is_available()) {
        resolved = net::dns_resolve(host, target_ip, 3000);
    }
    if (!resolved) return nullptr;
    
    constexpr size_t cap = 65536;
    char* buf = static_cast<char*>(kmalloc(cap));
    if (!buf) return nullptr;
    
    int n = 0;
    if (is_https) {
        n = tls::https_get(target_ip, port, host, path, buf, cap - 1, 10000);
    } else {
        n = net::http_get(target_ip, port, path, buf, cap - 1, 4000, host);
    }
    
    if (n <= 0) {
        kfree(buf);
        return nullptr;
    }
    
    // Find the body (after \r\n\r\n)
    char* hdr_end = kstd::strstr(buf, "\r\n\r\n");
    if (hdr_end) {
        char* body = hdr_end + 4;
        size_t body_len = n - static_cast<int>(body - buf);
        // Move body to start of buffer
        for (size_t i = 0; i < body_len; ++i) {
            buf[i] = body[i];
        }
        buf[body_len] = '\0';
    } else {
        buf[n] = '\0';
    }
    
    return buf;
}


static char* expand_scripts(const char* html, size_t len, size_t* out_len, const char* base_url = nullptr) {
    bool has_script = false;
    for (size_t i = 0; i + 7 <= len; ++i) {
        if (html[i] == '<' && ci_prefix(html + i + 1, "script")) { has_script = true; break; }
    }
    if (!has_script) return nullptr;

    size_t cap = len * 2 + 65536;
    char* out = static_cast<char*>(kmalloc(cap));
    if (!out) return nullptr;
    size_t o = 0;

    size_t i = 0;
    while (i < len) {
        if (html[i] == '<' && i + 7 <= len && ci_prefix(html + i + 1, "script")) {
            char after = (i + 7 < len) ? html[i + 7] : '>';
            if (after != '>' && after != ' ' && after != '\t' && after != '\n' && after != '\r') {
                out[o++] = html[i++];
                continue;
            }
            // End of the opening tag
            size_t j = i + 7;
            while (j < len && html[j] != '>') ++j;
            if (j >= len) break;

            // Detect a src= attribute on the opening tag (skip such scripts)
            bool has_src = false;
            char src_url[256] = {0};
            for (size_t k = i + 7; k + 3 < j; ++k) {
                if ((html[k] == 's' || html[k] == 'S') &&
                    (html[k + 1] == 'r' || html[k + 1] == 'R') &&
                    (html[k + 2] == 'c' || html[k + 2] == 'C')) {
                    size_t m = k + 3;
                    while (m < j && (html[m] == ' ' || html[m] == '\t')) ++m;
                    if (m < j && html[m] == '=') {
                        has_src = true;
                        // Extract the URL value
                        m++; // skip '='
                        while (m < j && (html[m] == ' ' || html[m] == '\t' || html[m] == '"' || html[m] == '\'')) ++m;
                        size_t url_start = m;
                        while (m < j && html[m] != ' ' && html[m] != '\t' && html[m] != '"' && html[m] != '\'') ++m;
                        size_t url_len = m - url_start;
                        if (url_len > 0 && url_len < sizeof(src_url) - 1) {
                            for (size_t u = 0; u < url_len; ++u) src_url[u] = html[url_start + u];
                            src_url[url_len] = '\0';
                        }
                        break;
                    }
                }
            }

            size_t content_start = j + 1;
            size_t close = len;
            for (size_t k = content_start; k + 8 <= len; ++k) {
                if (html[k] == '<' && html[k + 1] == '/' && ci_prefix(html + k + 2, "script")) {
                    close = k;
                    break;
                }
            }

            if (has_src && src_url[0] && base_url) {
                // Fetch and execute external script
                char resolved_url[512];
                resolve_url(base_url, src_url, resolved_url, sizeof(resolved_url));
                
                // Parse URL
                const char* u = resolved_url;
                bool is_https = false;
                if (!kstd::strncmp(u, "http://", 7)) {
                    u += 7;
                } else if (!kstd::strncmp(u, "https://", 8)) {
                    u += 8;
                    is_https = true;
                }
                
                const char* slash = kstd::strchr(u, '/');
                size_t authority_len = slash ? static_cast<size_t>(slash - u) : kstd::strlen(u);
                const char* colon = nullptr;
                for (size_t i = 0; i < authority_len; ++i) {
                    if (u[i] == ':') { colon = u + i; break; }
                }
                size_t host_len = colon ? static_cast<size_t>(colon - u) : authority_len;
                if (host_len >= 128) host_len = 127;
                
                char host[128];
                kstd::strncpy(host, u, host_len);
                host[host_len] = '\0';
                
                uint16_t port = is_https ? 443 : 80;
                if (colon) {
                    uint32_t parsed = 0;
                    const char* d = colon + 1;
                    const char* aend = u + authority_len;
                    while (d < aend && *d >= '0' && *d <= '9') {
                        parsed = parsed * 10 + static_cast<uint32_t>(*d - '0');
                        ++d;
                    }
                    if (parsed > 0 && parsed < 65536) port = static_cast<uint16_t>(parsed);
                }
                
                char path[256];
                if (slash) {
                    kstd::strncpy(path, slash, sizeof(path) - 1);
                    path[sizeof(path) - 1] = '\0';
                } else {
                    path[0] = '/';
                    path[1] = '\0';
                }
                
                // Resolve hostname
                uint8_t target_ip[4];
                bool resolved = net::parse_ip(host, target_ip);
                if (!resolved && net::is_available()) {
                    resolved = net::dns_resolve(host, target_ip, 3000);
                }
                
                if (resolved) {
                    constexpr size_t script_cap = 65536;
                    char* script_buf = static_cast<char*>(kmalloc(script_cap));
                    if (script_buf) {
                        int n = 0;
                        if (is_https) {
                            n = tls::https_get(target_ip, port, host, path, script_buf, script_cap - 1, 10000);
                        } else {
                            n = net::http_get(target_ip, port, path, script_buf, script_cap - 1, 4000, host);
                        }
                        if (n > 0) {
                            script_buf[n] = '\0';
                            ScriptSink sink{out + o, 0, cap > o + 1 ? cap - o - 1 : 0};
                            js::Host host_ctx;
                            host_ctx.user = &sink;
                            host_ctx.log = nullptr;
                            host_ctx.write = script_sink_write;
                            host_ctx.fetch = [](void*, const char* url) -> char* { return fetch_url_content(url); };
                            js::execute(script_buf, &host_ctx);
                            o += sink.len;
                        }
                        kfree(script_buf);
                    }
                }
            } else if (!has_src && close > content_start) {
                size_t slen = close - content_start;
                char* src = static_cast<char*>(kmalloc(slen + 1));
                if (src) {
                    for (size_t k = 0; k < slen; ++k) src[k] = html[content_start + k];
                    src[slen] = '\0';
                    ScriptSink sink{out + o, 0, cap > o + 1 ? cap - o - 1 : 0};
                    js::Host host;
                    host.user = &sink;
                    host.log = nullptr;
                    host.write = script_sink_write;
                    host.fetch = [](void*, const char* url) -> char* { return fetch_url_content(url); };
                    js::execute(src, &host);
                    kfree(src);
                    o += sink.len;
                }
            }

            // Advance past the closing tag
            if (close < len) {
                size_t c = close + 8;
                while (c < len && html[c] != '>') ++c;
                i = (c < len) ? c + 1 : len;
            } else {
                i = len;
            }
            continue;
        }
        out[o++] = html[i++];
    }

    out[o] = '\0';
    if (out_len) *out_len = o;
    return out;
}

// Execute inline scripts, then lay out the (possibly rewritten) document.
static void parse_html_with_scripts(Document* doc, const char* html, size_t len) {
    size_t expanded_len = 0;
    char* expanded = expand_scripts(html, len, &expanded_len, doc ? doc->current_url : nullptr);
    if (expanded) {
        parse_html(doc, expanded, expanded_len);
        kfree(expanded);
    } else {
        parse_html(doc, html, len);
    }
}

// Case-insensitive substring search bounded to [hay, hay+hay_len).
static const char* find_ci(const char* hay, size_t hay_len, const char* needle) {
    size_t nl = kstd::strlen(needle);
    if (nl == 0 || hay_len < nl) return nullptr;
    for (size_t i = 0; i + nl <= hay_len; ++i) {
        size_t j = 0;
        while (j < nl) {
            char ca = hay[i + j];
            char cb = needle[j];
            if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
            if (ca != cb) break;
            ++j;
        }
        if (j == nl) return hay + i;
    }
    return nullptr;
}

static bool starts_with_ci(const char* s, const char* prefix) {
    while (*prefix) {
        char ca = *s++;
        char cb = *prefix++;
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

// Maximum size of a decompressed HTTP body we will buffer (256 KiB).
static constexpr size_t DECOMPRESS_CAP = 256 * 1024;

// Returns a pointer to the value of header `name` (which must include the
// trailing colon), or nullptr. Only the header region is searched.
static const char* header_value(const char* buf, size_t hdr_len, const char* name) {
    const char* h = find_ci(buf, hdr_len, name);
    if (!h) return nullptr;
    const char* v = h + kstd::strlen(name);
    while (*v == ' ' || *v == '\t') ++v;
    return v;
}

// Does a header value line contain `needle` (case-insensitive) before CR/LF?
static bool ci_line_contains(const char* s, const char* needle) {
    if (!s) return false;
    size_t line = 0;
    while (s[line] && s[line] != '\r' && s[line] != '\n') ++line;
    return find_ci(s, line, needle) != nullptr;
}

// Decodes an HTTP/1.1 chunked body into a freshly allocated buffer. Returns
// nullptr if the framing is malformed or memory is exhausted.
static char* dechunk_body(const char* body, size_t len, size_t* out_len) {
    char* out = static_cast<char*>(kmalloc(len + 1));
    if (!out) return nullptr;
    size_t i = 0, o = 0;
    for (;;) {
        size_t sz = 0;
        bool any = false;
        while (i < len) {
            char c = body[i];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            sz = sz * 16 + static_cast<size_t>(d);
            any = true;
            ++i;
            if (sz > (1u << 24)) break;
        }
        if (!any) { kfree(out); return nullptr; }
        while (i < len && body[i] != '\n') ++i;   // skip chunk extensions + CR
        if (i < len) ++i;                          // consume LF
        if (sz == 0) break;                        // last chunk
        if (i + sz > len) { kfree(out); return nullptr; }
        for (size_t k = 0; k < sz; ++k) out[o++] = body[i + k];
        i += sz;
        if (i < len && body[i] == '\r') ++i;
        if (i < len && body[i] == '\n') ++i;
    }
    out[o] = '\0';
    *out_len = o;
    return out;
}

// Parses an HTTP response held in `buf` (byte count `bytes`), transparently
// de-chunking and decompressing gzip/deflate payloads, then feeds the HTML to
// the parser. Returns true if any renderable content was produced.
static bool install_http_body(Document* doc, const char* buf, int bytes) {
    if (!doc || !buf || bytes <= 0) return false;
    size_t total = static_cast<size_t>(bytes);

    const char* hdr_end = kstd::strstr(buf, "\r\n\r\n");
    size_t hdr_len = hdr_end ? static_cast<size_t>(hdr_end - buf) : total;
    const char* body = hdr_end ? hdr_end + 4 : buf;
    size_t body_len = total - static_cast<size_t>(body - buf);
    if (body_len > total) body_len = 0;

    // Prefer an explicit Content-Length (binary-safe; body may hold NULs).
    if (const char* cl = header_value(buf, hdr_len, "content-length:")) {
        long long n = 0;
        while (*cl >= '0' && *cl <= '9' && n < (1 << 24)) n = n * 10 + (*cl++ - '0');
        if (n >= 0 && static_cast<size_t>(n) < body_len) body_len = static_cast<size_t>(n);
    }

    // Content-Encoding: decide whether we can decode it.
    int enc = 0; // 0 none, 1 gzip, 2 deflate
    bool unsupported = false;
    const char* ce = header_value(buf, hdr_len, "content-encoding:");
    if (ce) {
        if (starts_with_ci(ce, "gzip") || starts_with_ci(ce, "x-gzip")) enc = 1;
        else if (starts_with_ci(ce, "deflate")) enc = 2;
        else if (starts_with_ci(ce, "identity")) enc = 0;
        else if (starts_with_ci(ce, "br") || starts_with_ci(ce, "zstd") ||
                 starts_with_ci(ce, "compress") || starts_with_ci(ce, "lzma")) {
            unsupported = true; // we did not advertise these; caller will retry
        }
    }

    // Transfer-Encoding: chunked (possibly "gzip, chunked").
    bool chunked = false;
    if (const char* te = header_value(buf, hdr_len, "transfer-encoding:")) {
        if (ci_line_contains(te, "chunked")) chunked = true;
    }

    char* dechunked = nullptr;
    if (chunked) {
        size_t dl = 0;
        dechunked = dechunk_body(body, body_len, &dl);
        if (dechunked) { body = dechunked; body_len = dl; }
    }

    if (unsupported) {
        if (dechunked) kfree(dechunked);
        return false; // signal the caller to retry with Accept-Encoding: identity
    }

    // Sniff the gzip magic as a fallback (some servers mislabel the header).
    if (enc == 0 && body_len >= 2 &&
        static_cast<uint8_t>(body[0]) == 0x1f && static_cast<uint8_t>(body[1]) == 0x8b) {
        enc = 1;
    }

    if (enc != 0) {
        uint8_t* out = static_cast<uint8_t*>(kmalloc(DECOMPRESS_CAP));
        if (out) {
            size_t n = inflate::decompress(reinterpret_cast<const uint8_t*>(body), body_len, out, DECOMPRESS_CAP);
            if (n > 0) {
                parse_html_with_scripts(doc, reinterpret_cast<const char*>(out), n);
                kfree(out);
                if (dechunked) kfree(dechunked);
                return doc->box_count > 0;
            }
            kfree(out);
        }
        // Decompression failed: don't try to parse binary garbage.
        if (dechunked) kfree(dechunked);
        return false;
    }

    parse_html_with_scripts(doc, body, body_len);
    if (dechunked) kfree(dechunked);
    return doc->box_count > 0;
}

// ======================= URL resolution =======================
// Turns a link target (relative or absolute) into an absolute URL. http(s)
// and scheme-relative links are resolved against `base`; root-relative,
// query-only, fragment-only, "." and ".." forms are all handled. Non-web
// schemes (about:, app:, file:) and relative paths with no http base are
// passed through untouched.
void resolve_url(const char* base, const char* href, char* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!href) href = "";
    if (!base) base = "";

    auto copy_n = [&](const char* s, size_t n) {
        if (n >= cap) n = cap - 1;
        for (size_t i = 0; i < n; ++i) out[i] = s[i];
        out[n] = '\0';
    };

    // Empty target (or plain fragment) keeps the current document.
    if (href[0] == '\0') { copy_n(base, kstd::strlen(base)); return; }

    // Absolute URL with a scheme (http:, https:, about:, app:, file: ...).
    if (href[0]) {
        bool scheme = (href[0] >= 'a' && href[0] <= 'z') ||
                      (href[0] >= 'A' && href[0] <= 'Z');
        size_t i = 1;
        for (; scheme && href[i]; ++i) {
            char c = href[i];
            if (c == ':') break;
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.')) {
                scheme = false;
            }
        }
        if (scheme && href[i] == ':') { copy_n(href, kstd::strlen(href)); return; }
    }

    // Fragment-only and query-only references keep the current document.
    if (href[0] == '#' || href[0] == '?') {
        size_t n = 0;
        while (base[n]) {
            if (href[0] == '#' && base[n] == '#') break;
            if (href[0] == '?' && base[n] == '?') break;
            ++n;
        }
        if (n >= cap) n = cap - 1;
        for (size_t i = 0; i < n; ++i) out[i] = base[i];
        size_t o = n;
        for (size_t i = 0; href[i] && o + 1 < cap; ++i) out[o++] = href[i];
        out[o] = '\0';
        return;
    }

    // Scheme-relative "//host/path".
    if (href[0] == '/' && href[1] == '/') {
        const char* prefix = nullptr;
        if (!kstd::strncmp(base, "http://", 7)) prefix = "http:";
        else if (!kstd::strncmp(base, "https://", 8)) prefix = "https:";
        if (!prefix) { copy_n(href, kstd::strlen(href)); return; }
        size_t o = 0;
        for (size_t i = 0; prefix[i] && o + 1 < cap; ++i) out[o++] = prefix[i];
        for (size_t i = 0; href[i] && o + 1 < cap; ++i) out[o++] = href[i];
        out[o] = '\0';
        return;
    }

    // Only http(s) documents have an authority to resolve against.
    size_t scheme_len = 0;
    if (!kstd::strncmp(base, "http://", 7)) scheme_len = 7;
    else if (!kstd::strncmp(base, "https://", 8)) scheme_len = 8;
    else { copy_n(href, kstd::strlen(href)); return; }

    // Origin = scheme://authority (ends at the first / ? or # after it).
    size_t origin_len = kstd::strlen(base);
    for (size_t i = scheme_len; base[i]; ++i) {
        if (base[i] == '/' || base[i] == '?' || base[i] == '#') { origin_len = i; break; }
    }

    const char* bpath = (base[origin_len] == '/') ? base + origin_len : "/";
    size_t plen = 0;
    while (bpath[plen] && bpath[plen] != '?' && bpath[plen] != '#') ++plen;
    size_t dir_len = 0;
    for (size_t i = 0; i < plen; ++i) if (bpath[i] == '/') dir_len = i + 1;

    // Build origin + directory + href, then normalise "." / ".." segments.
    char raw[512];
    size_t r = 0;
    for (size_t i = 0; i < origin_len && r + 1 < sizeof(raw); ++i) raw[r++] = base[i];
    if (href[0] != '/') {
        if (dir_len == 0) { if (r + 1 < sizeof(raw)) raw[r++] = '/'; }
        else for (size_t i = 0; i < dir_len && r + 1 < sizeof(raw); ++i) raw[r++] = bpath[i];
    }
    for (size_t i = 0; href[i] && r + 1 < sizeof(raw); ++i) raw[r++] = href[i];
    raw[r] = '\0';

    size_t pp_start = origin_len;
    size_t pp_end = pp_start;
    while (raw[pp_end] && raw[pp_end] != '?' && raw[pp_end] != '#') ++pp_end;

    char norm[512];
    size_t nl = 1;
    norm[0] = '/';
    size_t i = pp_start;
    if (i < pp_end && raw[i] == '/') ++i;
    while (i < pp_end) {
        size_t s = i;
        while (i < pp_end && raw[i] != '/') ++i;
        size_t seg = i - s;
        if (seg == 0 || (seg == 1 && raw[s] == '.')) {
            // skip
        } else if (seg == 2 && raw[s] == '.' && raw[s + 1] == '.') {
            while (nl > 1 && norm[nl - 1] != '/') --nl;
            if (nl > 1) --nl;
        } else {
            if (nl > 1 && norm[nl - 1] != '/') norm[nl++] = '/';
            for (size_t k = 0; k < seg && nl + 1 < sizeof(norm); ++k) norm[nl++] = raw[s + k];
        }
        while (i < pp_end && raw[i] == '/') ++i;
    }
    if (pp_end > pp_start && raw[pp_end - 1] == '/' && nl > 1 && norm[nl - 1] != '/') {
        if (nl + 1 < sizeof(norm)) norm[nl++] = '/';
    }
    norm[nl] = '\0';

    size_t o = 0;
    for (size_t k = 0; k < origin_len && o + 1 < cap; ++k) out[o++] = raw[k];
    for (size_t k = 0; k < nl && o + 1 < cap; ++k) out[o++] = norm[k];
    for (size_t k = pp_end; raw[k] && o + 1 < cap; ++k) out[o++] = raw[k];
    out[o] = '\0';
}

// Google still serves a static HTML fallback (gbv=1). Normalise the homepage
// to that endpoint so the lightweight engine can render real content.
static bool is_google_host(const char* host) {
    if (!host) return false;
    const char* h = host;
    if (!kstd::strncmp(h, "www.", 4)) h += 4;
    if (!kstd::strcmp(h, "google.com")) return true;
    if (kstd::strncmp(h, "google.", 7) != 0) return false;
    const char* tld = h + 7;
    auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    return tld[0] && tld[1] && !tld[2] && alpha(tld[0]) && alpha(tld[1]);
}

static bool is_google_root(const char* path) {
    return !path || path[0] == '\0' || (path[0] == '/' && path[1] == '\0');
}

// URL Resolver & Loader
bool load_url(Document* doc, const char* url, int viewport_width) {
    if (!doc || !url || *url == '\0') return false;

    // Resolve relative links ("other.html", "../up.html", "/root.html")
    // against the page that is currently loaded. Absolute URLs pass through.
    char resolved[512];
    if (kstd::strcmp(url, "home") != 0) {
        resolve_url(doc->current_url, url, resolved, sizeof(resolved));
        if (resolved[0]) url = resolved;
    } else {
        url = "about:home";
    }

    doc->page_width = viewport_width;
    kstd::strncpy(doc->current_url, url, 127);
    doc->current_url[127] = '\0';

    // 1. Built-in 'about:' URLs
    if (!kstd::strncmp(url, "about:", 6) || !kstd::strcmp(url, "home")) {
        const char* page = get_builtin_page(url);
        if (page) {
            parse_html_with_scripts(doc, page, kstd::strlen(page));
            return true;
        }
    }

    // 2. Installed apps ('app://notes'), read from AetherFS and run in-engine.
    if (!kstd::strncmp(url, "app://", 6)) {
        const char* name = url + 6;
        int size = apphub::app_size(name);
        if (size > 0) {
            char* html = new char[static_cast<size_t>(size) + 1];
            if (html) {
                int got = apphub::read_app(name, html, static_cast<size_t>(size));
                if (got > 0) {
                    html[got] = '\0';
                    parse_html_with_scripts(doc, html, static_cast<size_t>(got));
                    delete[] html;
                    return true;
                }
                delete[] html;
            }
        }
        const char* missing =
            "<html><head><title>App Not Installed</title></head><body>"
            "<h1>App Not Installed</h1>"
            "<p>Install this app from the App Hub, then open it again.</p>"
            "</body></html>";
        parse_html_with_scripts(doc, missing, kstd::strlen(missing));
        return false;
    }

    // 3. Local RamFS file paths ('/index.html', 'file:///index.html', 'index.html')
    const char* filename = url;
    if (!kstd::strncmp(filename, "file:///", 8)) {
        filename += 8;
    } else if (filename[0] == '/') {
        filename += 1;
    }

    if (ramfs::exists(filename)) {
        size_t fsize = 0;
        const char* content = static_cast<const char*>(ramfs::read_bytes(filename, &fsize));
        if (content && fsize > 0) {
            parse_html_with_scripts(doc, content, fsize);
            return true;
        }
    }

    // 3. Network URLs: 'http://host[:port]/path', 'https://...', or bare 'host/path'
    //    (hostname resolution via DNS; HTTPS uses BearSSL with CA validation)
    bool is_network = !kstd::strncmp(url, "http://", 7) ||
                      !kstd::strncmp(url, "https://", 8) ||
                      (kstd::strchr(url, '.') != nullptr && kstd::strchr(url, ' ') == nullptr);

    if (is_network) {
        const char* u = url;
        bool is_https = false;
        if (!kstd::strncmp(u, "http://", 7)) {
            u += 7;
        } else if (!kstd::strncmp(u, "https://", 8)) {
            u += 8;
            is_https = true;
        }

        // Split authority from path
        const char* slash = kstd::strchr(u, '/');
        size_t authority_len = slash ? static_cast<size_t>(slash - u) : kstd::strlen(u);

        // Extract optional :port from the authority
        const char* colon = nullptr;
        for (size_t i = 0; i < authority_len; ++i) {
            if (u[i] == ':') { colon = u + i; break; }
        }
        size_t host_len = colon ? static_cast<size_t>(colon - u) : authority_len;
        if (host_len >= 128) host_len = 127;

        char host[128];
        kstd::strncpy(host, u, host_len);
        host[host_len] = '\0';

        uint16_t port = is_https ? 443 : 80;
        if (colon) {
            uint32_t parsed = 0;
            const char* d = colon + 1;
            const char* aend = u + authority_len;
            while (d < aend && *d >= '0' && *d <= '9') {
                parsed = parsed * 10 + static_cast<uint32_t>(*d - '0');
                ++d;
            }
            if (parsed > 0 && parsed < 65536) port = static_cast<uint16_t>(parsed);
        }

        char path[256];
        if (slash) {
            kstd::strncpy(path, slash, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
        } else {
            path[0] = '/';
            path[1] = '\0';
        }

        // Google's live homepage is a JavaScript app. Use its static HTML
        // fallback (gbv=1) and pin the interface language to English.
        bool google = is_google_host(host);
        if (google && is_google_root(path)) {
            kstd::strcpy(path, "/?gbv=1&hl=en");
        }

        // Upgrade Google URLs to HTTPS; their HTTP origin now serves the live
        // JavaScript homepage, which this engine cannot lay out faithfully.
        if (!is_https && google) {
            is_https = true;
            port = 443;
            char secure_url[512];
            size_t so = 0;
            const char* pre = "https://";
            for (size_t i = 0; pre[i] && so + 1 < sizeof(secure_url); ++i) secure_url[so++] = pre[i];
            for (size_t i = 0; host[i] && so + 1 < sizeof(secure_url); ++i) secure_url[so++] = host[i];
            for (size_t i = 0; path[i] && so + 1 < sizeof(secure_url); ++i) secure_url[so++] = path[i];
            secure_url[so] = '\0';
            kstd::strncpy(doc->current_url, secure_url, 127);
            doc->current_url[127] = '\0';
        } else if (google && is_https) {
            char secure_url[512];
            size_t so = 0;
            const char* pre = "https://";
            for (size_t i = 0; pre[i] && so + 1 < sizeof(secure_url); ++i) secure_url[so++] = pre[i];
            for (size_t i = 0; host[i] && so + 1 < sizeof(secure_url); ++i) secure_url[so++] = host[i];
            if (port != 443) {
                char pt[8];
                kstd::utoa(port, pt, 10);
                if (so + 1 < sizeof(secure_url)) secure_url[so++] = ':';
                for (size_t i = 0; pt[i] && so + 1 < sizeof(secure_url); ++i) secure_url[so++] = pt[i];
            }
            for (size_t i = 0; path[i] && so + 1 < sizeof(secure_url); ++i) secure_url[so++] = path[i];
            secure_url[so] = '\0';
            kstd::strncpy(doc->current_url, secure_url, 127);
            doc->current_url[127] = '\0';
        }

        // Resolve hostname (accepts quad-dotted IP directly)
        uint8_t target_ip[4];
        bool resolved = net::parse_ip(host, target_ip);
        if (!resolved && net::is_available()) {
            resolved = net::dns_resolve(host, target_ip, 3000);
        }

        if (!resolved) {
            char dns_page[512];
            kstd::strcpy(dns_page, "<html><head><title>Host Not Found</title></head><body>"
                                   "<h1>Host Not Found</h1><p>Could not resolve host '<b>");
            size_t off = kstd::strlen(dns_page);
            kstd::strncpy(dns_page + off, host, 95);
            dns_page[off + 95] = '\0';
            kstd::strcpy(dns_page + kstd::strlen(dns_page),
                         "</b>'. Check your network connection and DNS settings.</p>"
                         "<hr><p><a href=\"about:home\">&lt;&lt; Return to Home Portal</a></p></body></html>");
            parse_html(doc, dns_page);
            return false;
        }

        // HTTPS: BearSSL TLS 1.2 with full X.509 chain, hostname, and time validation.
        if (is_https) {
            constexpr size_t tls_cap = 524288;
            char* https_buf = static_cast<char*>(kmalloc(tls_cap));
            bool got = false;
            if (https_buf) {
                char cur_host[128];
                char cur_path[256];
                uint8_t cur_ip[4];
                kstd::strcpy(cur_host, host);
                kstd::strcpy(cur_path, path);
                memcpy(cur_ip, target_ip, 4);
                uint16_t cur_port = port;

                for (int hop = 0; hop < 4 && !got; ++hop) {
                    int n = tls::https_get(cur_ip, cur_port, cur_host, cur_path,
                                           https_buf, tls_cap - 1, 15000);
                    if (n <= 0) break;
                    https_buf[n] = '\0';

                    char* hdr_end = kstd::strstr(https_buf, "\r\n\r\n");
                    size_t hdr_len = hdr_end ? static_cast<size_t>(hdr_end - https_buf) : static_cast<size_t>(n);
                    const char* loc = header_value(https_buf, hdr_len, "location:");
                    if (loc && (!hdr_end || loc < hdr_end)) {
                        char loc_val[256];
                        size_t li = 0;
                        while (loc[li] && loc[li] != '\r' && loc[li] != '\n' && li < sizeof(loc_val) - 1) {
                            loc_val[li] = loc[li];
                            ++li;
                        }
                        loc_val[li] = '\0';
                        if (li == 0) break;

                        char cur_abs[512];
                        size_t co = 0;
                        const char* pre = "https://";
                        for (size_t i = 0; pre[i] && co + 1 < sizeof(cur_abs); ++i) cur_abs[co++] = pre[i];
                        for (size_t i = 0; cur_host[i] && co + 1 < sizeof(cur_abs); ++i) cur_abs[co++] = cur_host[i];
                        if (cur_port != 443) {
                            char pt[8];
                            kstd::utoa(cur_port, pt, 10);
                            if (co + 1 < sizeof(cur_abs)) cur_abs[co++] = ':';
                            for (size_t i = 0; pt[i] && co + 1 < sizeof(cur_abs); ++i) cur_abs[co++] = pt[i];
                        }
                        for (size_t i = 0; cur_path[i] && co + 1 < sizeof(cur_abs); ++i) cur_abs[co++] = cur_path[i];
                        cur_abs[co] = '\0';

                        char next_url[512];
                        resolve_url(cur_abs, loc_val, next_url, sizeof(next_url));
                        if (kstd::strncmp(next_url, "https://", 8) != 0) break;

                        const char* na = next_url + 8;
                        const char* nslash = kstd::strchr(na, '/');
                        size_t nalen = nslash ? static_cast<size_t>(nslash - na) : kstd::strlen(na);
                        const char* ncolon = nullptr;
                        for (size_t i = 0; i < nalen; ++i) {
                            if (na[i] == ':') { ncolon = na + i; break; }
                        }
                        size_t nhost_len = ncolon ? static_cast<size_t>(ncolon - na) : nalen;
                        if (nhost_len >= sizeof(cur_host)) break;

                        char new_host[128];
                        kstd::strncpy(new_host, na, nhost_len);
                        new_host[nhost_len] = '\0';

                        uint16_t new_port = 443;
                        if (ncolon) {
                            uint32_t pv = 0;
                            const char* d = ncolon + 1;
                            const char* ae = na + nalen;
                            while (d < ae && *d >= '0' && *d <= '9') {
                                pv = pv * 10 + static_cast<uint32_t>(*d - '0');
                                ++d;
                            }
                            if (pv > 0 && pv < 65536) new_port = static_cast<uint16_t>(pv);
                        }

                        char new_path[256];
                        if (nslash) {
                            kstd::strncpy(new_path, nslash, sizeof(new_path) - 1);
                            new_path[sizeof(new_path) - 1] = '\0';
                        } else {
                            new_path[0] = '/';
                            new_path[1] = '\0';
                        }

                        uint8_t nip[4];
                        if (net::parse_ip(new_host, nip) || net::dns_resolve(new_host, nip, 3000)) {
                            kstd::strcpy(cur_host, new_host);
                            memcpy(cur_ip, nip, 4);
                            cur_port = new_port;
                            kstd::strcpy(cur_path, new_path);
                            kstd::strncpy(doc->current_url, next_url, 127);
                            doc->current_url[127] = '\0';
                            continue;
                        }
                        break;
                    }

                    got = install_http_body(doc, https_buf, n);
                    if (!got) {
                        parse_html(doc,
                            "<html><head><title>No Displayable Content</title></head><body>"
                            "<h1>Page Returned No Content</h1>"
                            "<p>The HTTPS response could not be decoded by the browser.</p>"
                            "<hr><p><a href=\"about:home\">&lt;&lt; Return to Home Portal</a></p></body></html>");
                    }
                }
                kfree(https_buf);
            }
            if (got) {
                // Pre-fetch up to 3 images with short timeout
                int img_count = 0;
                for (size_t bi = 0; bi < doc->box_count && img_count < 3; ++bi) {
                    LayoutBox& b = doc->boxes[bi];
                    if (b.img_src[0] == '\\0' || b.img_pixels) continue;
                    img_count++;
                    
                    char full_url[256];
                    if (b.img_src[0] == '/') {
                        const char* u = doc->current_url;
                        bool is_https = (!kstd::strncmp(u, "https://", 8));
                        const char* after_proto = is_https ? (u + 8) : (u + 7);
                        const char* slash = kstd::strchr(after_proto, '/');
                        size_t host_len = slash ? static_cast<size_t>(slash - after_proto) : kstd::strlen(after_proto);
                        if (host_len > 200) host_len = 200;
                        if (is_https) kstd::strcpy(full_url, "https://");
                        else kstd::strcpy(full_url, "http://");
                        size_t fl = kstd::strlen(full_url);
                        for (size_t i = 0; i < host_len && fl + i < sizeof(full_url) - 1; ++i) full_url[fl + i] = after_proto[i];
                        full_url[fl + host_len] = '\\0';
                        size_t fl2 = kstd::strlen(full_url);
                        size_t sl = kstd::strlen(b.img_src);
                        if (fl2 + sl < sizeof(full_url)) {
                            for (size_t i = 0; i < sl; ++i) full_url[fl2 + i] = b.img_src[i];
                            full_url[fl2 + sl] = '\\0';
                        }
                    } else {
                        kstd::strcpy(full_url, b.img_src);
                    }
                    
                    const char* u = full_url;
                    bool is_https2 = false;
                    if (!kstd::strncmp(u, "http://", 7)) u += 7;
                    else if (!kstd::strncmp(u, "https://", 8)) { u += 8; is_https2 = true; }
                    
                    const char* slash = kstd::strchr(u, '/');
                    size_t authority_len = slash ? static_cast<size_t>(slash - u) : kstd::strlen(u);
                    char host[128];
                    size_t hl = authority_len;
                    if (hl >= 128) hl = 127;
                    kstd::strncpy(host, u, hl);
                    host[hl] = '\\0';
                    
                    uint16_t port = is_https2 ? 443 : 80;
                    char path[256];
                    if (slash) { kstd::strncpy(path, slash, sizeof(path) - 1); path[sizeof(path)-1] = '\\0'; }
                    else { path[0] = '/'; path[1] = '\\0'; }
                    
                    uint8_t target_ip[4];
                    bool resolved = net::parse_ip(host, target_ip);
                    if (!resolved && net::is_available()) resolved = net::dns_resolve(host, target_ip, 2000);
                    
                    if (resolved) {
                        constexpr size_t cap = 65536;
                        char* buf = static_cast<char*>(kmalloc(cap));
                        if (buf) {
                            int n = 0;
                            if (is_https2) n = tls::https_get(target_ip, port, host, path, buf, cap - 1, 2000);
                            else n = net::http_get(target_ip, port, path, buf, cap - 1, 2000, host);
                            
                            if (n > 0) {
                                char* body = nullptr;
                                for (int i = 0; i < n - 3; ++i) {
                                    if (buf[i] == '\\r' && buf[i+1] == '\\n' && buf[i+2] == '\\r' && buf[i+3] == '\\n') {
                                        body = buf + i + 4;
                                        break;
                                    }
                                }
                                if (!body) {
                                    for (int i = 0; i < n - 1; ++i) {
                                        if (buf[i] == '\\n' && buf[i+1] == '\\n') {
                                            body = buf + i + 2;
                                            break;
                                        }
                                    }
                                }
                                if (body) {
                                    size_t body_len = n - (body - buf);
                                    gfx::Image img = {};
                                    if (gfx::load_image_from_memory(reinterpret_cast<const uint8_t*>(body), body_len, &img)) {
                                        b.img_pixels = img.pixels;
                                        b.img_w = img.width;
                                        b.img_h = img.height;
                                    }
                                }
                            }
                            kfree(buf);
                        }
                    }
                }
                return true;
            }

            render_tls_note(doc, host);
            return false;
        }

        char* http_buf = static_cast<char*>(kmalloc(131072));
        if (http_buf) {
            bool have_body = false;
            bool hit_https = false;

            // Follow up to 4 HTTP redirects. Relative Location targets are
            // resolved against the current request URL so directory-relative
            // redirects keep working.
            for (int hop = 0; hop < 4 && !have_body; ++hop) {
                char cur_abs[512];
                {
                    size_t o = 0;
                    const char* pre = "http://";
                    for (size_t i = 0; pre[i] && o + 1 < sizeof(cur_abs); ++i) cur_abs[o++] = pre[i];
                    for (size_t i = 0; host[i] && o + 1 < sizeof(cur_abs); ++i) cur_abs[o++] = host[i];
                    if (port != 80) {
                        char pt[8];
                        kstd::utoa(port, pt, 10);
                        if (o + 1 < sizeof(cur_abs)) cur_abs[o++] = ':';
                        for (size_t i = 0; pt[i] && o + 1 < sizeof(cur_abs); ++i) cur_abs[o++] = pt[i];
                    }
                    for (size_t i = 0; path[i] && o + 1 < sizeof(cur_abs); ++i) cur_abs[o++] = path[i];
                    cur_abs[o] = '\0';
                }

                int bytes = net::http_get(target_ip, port, path, http_buf, 131072, 4000, host);
                if (bytes <= 0) break;
                http_buf[bytes] = '\0';

                char* hdr_end = kstd::strstr(http_buf, "\r\n\r\n");
                char* loc = kstd::strstr(http_buf, "Location:");
                if (!loc) loc = kstd::strstr(http_buf, "location:");

                if (loc && (!hdr_end || loc < hdr_end)) {
                    const char* lv = loc + 9;
                    while (*lv == ' ') ++lv;
                    char loc_val[256];
                    size_t li = 0;
                    while (lv[li] && lv[li] != '\r' && lv[li] != '\n' && li < sizeof(loc_val) - 1) {
                        loc_val[li] = lv[li];
                        ++li;
                    }
                    loc_val[li] = '\0';
                    if (li == 0) break;

                    char next_url[512];
                    resolve_url(cur_abs, loc_val, next_url, sizeof(next_url));
                    if (!kstd::strncmp(next_url, "https://", 8)) {
                        hit_https = true;
                        break;
                    }
                    if (kstd::strncmp(next_url, "http://", 7) != 0) break;

                    const char* na = next_url + 7;
                    const char* nslash = kstd::strchr(na, '/');
                    size_t nalen = nslash ? static_cast<size_t>(nslash - na) : kstd::strlen(na);
                    const char* ncolon = nullptr;
                    for (size_t i = 0; i < nalen; ++i) {
                        if (na[i] == ':') { ncolon = na + i; break; }
                    }
                    size_t nhost_len = ncolon ? static_cast<size_t>(ncolon - na) : nalen;
                    if (nhost_len >= sizeof(host)) break;

                    char new_host[128];
                    kstd::strncpy(new_host, na, nhost_len);
                    new_host[nhost_len] = '\0';

                    uint16_t new_port = 80;
                    if (ncolon) {
                        uint32_t pv = 0;
                        const char* d = ncolon + 1;
                        const char* ae = na + nalen;
                        while (d < ae && *d >= '0' && *d <= '9') {
                            pv = pv * 10 + static_cast<uint32_t>(*d - '0');
                            ++d;
                        }
                        if (pv > 0 && pv < 65536) new_port = static_cast<uint16_t>(pv);
                    }

                    char new_path[256];
                    if (nslash) {
                        kstd::strncpy(new_path, nslash, sizeof(new_path) - 1);
                        new_path[sizeof(new_path) - 1] = '\0';
                    } else {
                        new_path[0] = '/';
                        new_path[1] = '\0';
                    }

                    uint8_t nip[4];
                    if (net::parse_ip(new_host, nip) || net::dns_resolve(new_host, nip, 3000)) {
                        kstd::strcpy(host, new_host);
                        memcpy(target_ip, nip, 4);
                        port = new_port;
                        kstd::strcpy(path, new_path);
                        kstd::strncpy(doc->current_url, next_url, 127);
                        doc->current_url[127] = '\0';
                        continue;
                    }
                    break;
                }

                bool rendered = install_http_body(doc, http_buf, bytes);
                // Some servers answer with an encoding we cannot decode (e.g.
                // Brotli) or lie about gzip. Retry once asking for identity.
                if (!rendered) {
                    int b2 = net::http_get(target_ip, port, path, http_buf, 131072, 4000, host, "identity");
                    if (b2 > 0) {
                        http_buf[b2] = '\0';
                        rendered = install_http_body(doc, http_buf, b2);
                    }
                }
                if (!rendered) {
                    parse_html(doc,
                        "<html><head><title>No Displayable Content</title></head><body>"
                        "<h1>Page Returned No Content</h1>"
                        "<p>This page produced no renderable text. Pages that build their entire "
                        "layout with complex scripting frameworks may not render fully in Aether Browser.</p>"
                        "<hr><p><a href=\"about:home\">&lt;&lt; Return to Home Portal</a> | "
                        "<a href=\"http://example.com/\">Example</a> | "
                        "<a href=\"/index.html\">RamFS Index</a></p></body></html>");
                }
                have_body = true;
            }

            kfree(http_buf);
            if (have_body) return true;
            if (hit_https) {
                render_tls_note(doc, host);
                return false;
            }
        }
    }

    // Fallback: 404 Not Found Page
    char err_page[384];
    kstd::strcpy(err_page, "<html><head><title>404 Not Found</title></head><body>"
                           "<h1>404 - Page Not Found</h1>"
                           "<p>The requested URL '<b>");
    kstd::strncpy(err_page + kstd::strlen(err_page), url, 60);
    kstd::strcpy(err_page + kstd::strlen(err_page), "</b>' could not be resolved.</p>"
                           "<hr>"
                           "<p><a href=\"about:home\">&lt;&lt; Return to Home Portal</a> | <a href=\"/index.html\">RamFS Index</a></p>"
                           "</body></html>");
    parse_html(doc, err_page);
    return false;
}

} // namespace browser
