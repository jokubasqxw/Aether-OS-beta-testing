#ifndef KERNEL_BROWSER_ENGINE_HPP
#define KERNEL_BROWSER_ENGINE_HPP

#include "types.hpp"
#include "gfx.hpp"

namespace browser {

enum class NodeType {
    DOCUMENT,
    ELEMENT,
    TEXT
};

enum class TagType {
    UNKNOWN,
    HTML,
    HEAD,
    TITLE,
    BODY,
    H1,
    H2,
    H3,
    H4,
    P,
    A,
    BUTTON,
    UL,
    OL,
    LI,
    HR,
    DIV,
    SPAN,
    B,
    STRONG,
    I,
    EM,
    CODE,
    PRE,
    INPUT,
    TABLE,
    TR,
    TD,
    TH,
    BR,
    IMG,
    CENTER,
    FONT,
    BLOCKQUOTE
};

enum class BoxType {
    TEXT,
    HEADING,
    BLOCK,
    HR,
    BUTTON,
    CODE_BOX,
    INPUT_BOX,
    TABLE_CELL,
    BULLET
};

struct Attribute {
    char name[24];
    char value[128];
};

constexpr size_t MAX_ATTRS_PER_NODE = 8;
constexpr size_t MAX_LAYOUT_BOXES = 1024;
constexpr size_t MAX_DOM_NODES = 512;

struct DomNode {
    NodeType type;
    TagType tag;
    char text[256];
    Attribute attributes[MAX_ATTRS_PER_NODE];
    size_t attr_count;

    DomNode* parent;
    DomNode* first_child;
    DomNode* next_sibling;

    const char* get_attribute(const char* name) const;
};

struct LayoutBox {
    int x;
    int y;
    int w;
    int h;
    BoxType type;
    char text[128];
    char link_url[256];
    char img_src[128];
    uint32_t* img_pixels;
    uint32_t img_w;
    uint32_t img_h;
    uint32_t fg_color;
    uint32_t bg_color;
    uint32_t bg_color2;
    uint32_t border_color;
    uint8_t scale;
    bool has_bg;
    bool has_gradient;
    bool has_border;
    bool is_bold;
    bool is_underline;
    bool is_clickable;
    bool is_button;
    bool is_input;
    bool is_hovered;
};

struct Document {
    char title[64];
    char current_url[128];
    DomNode* root;
    LayoutBox* boxes;
    size_t box_count;
    size_t box_capacity;
    int page_width;
    int page_height;
    int scroll_y;
    uint32_t bg_color;
    uint32_t bg_color2;
    bool has_bg_gradient;
    bool is_loading;
    uint32_t load_time_ms;
    size_t source_bytes;
    bool dirty;  // needs redraw

    void clear();
};

// Document Lifecycle & Parser
Document* create_document();
void destroy_document(Document* doc);

bool parse_html(Document* doc, const char* html, size_t len = 0);
void layout(Document* doc, int viewport_width);

// High-Performance Viewport Rendering with Strict Viewport Clipping
void render(Document& doc, int vx, int vy, int vw, int vh, int scroll_y);

// Interactive Hit Testing (returns hovered/clicked link or button box)
const LayoutBox* hit_test(const Document& doc, int x, int y, int scroll_y);

// Update is_hovered flags on interactive boxes from viewport-relative mouse
// coordinates. Returns the hovered box index, or -1 if nothing is hovered.
int update_hover(Document& doc, int x, int y, int scroll_y);

// Built-in Internal Pages (about:home, about:docs, about:acid, about:about)
const char* get_builtin_page(const char* url);

// Navigation & URL resolution (handles about:, file:///, /path.html, and http://)
bool load_url(Document* doc, const char* url, int viewport_width);

// Resolve a link target against a base URL (relative, "../x", "/x", "//host").
void resolve_url(const char* base, const char* href, char* out, size_t cap);

} // namespace browser

#endif // KERNEL_BROWSER_ENGINE_HPP
