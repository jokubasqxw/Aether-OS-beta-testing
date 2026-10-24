#include "js.hpp"
#include "heap.hpp"
#include "kstring.hpp"
#include "printk.hpp"
#include "pit.hpp"

// ============================================================================
// AetherScript - a compact tree-walking JavaScript interpreter.
//
// Memory model: every runtime value (string/object/function/environment) and
// every AST node is bump-allocated from an arena that is released when the
// script finishes, so the interpreter never leaks across navigations.
//
// Semantics are deliberately lenient (no exceptions): reading properties of
// undefined/null or calling a non-function yields undefined. This keeps
// real-world scripts from aborting the render.
// ============================================================================

namespace js {

namespace {

// ----------------------------- Arena ---------------------------------------
struct Chunk {
    Chunk* next;
    size_t used;
    size_t cap;
};

struct Arena {
    Chunk* head = nullptr;

    void* alloc(size_t n) {
        n = (n + 7u) & ~static_cast<size_t>(7u);
        if (n > 64u * 1024u) n = (n + 4095u) & ~static_cast<size_t>(4095u);
        if (!head || head->used + n > head->cap) {
            size_t cap = n > 65536u ? n : 65536u;
            Chunk* c = static_cast<Chunk*>(kmalloc(sizeof(Chunk) + cap));
            if (!c) return nullptr;
            c->next = head;
            c->used = 0;
            c->cap = cap;
            head = c;
        }
        void* p = reinterpret_cast<uint8_t*>(head + 1) + head->used;
        head->used += n;
        return p;
    }

    template <typename T>
    T* make() {
        void* p = alloc(sizeof(T));
        if (!p) return nullptr;
        memset(p, 0, sizeof(T));
        return static_cast<T*>(p);
    }

    void reset() {
        while (head) {
            Chunk* n = head->next;
            kfree(head);
            head = n;
        }
    }
};

static Arena* g_A = nullptr;

// ----------------------------- Values --------------------------------------
struct Str {
    int len;
    char* data;
};

struct Obj;
struct Func;
struct Env;

enum class VT { Undef, Null, Bool, Num, Str, Obj, Func };

struct Value {
    VT t = VT::Undef;
    bool b = false;
    double n = 0;
    Str* s = nullptr;
    Obj* o = nullptr;
    Func* f = nullptr;
};

struct Prop {
    Str* key;
    Value val;
};

struct Obj {
    bool is_array = false;
    Prop* props = nullptr;
    int nprops = 0;
    int pcap = 0;
    Value* elems = nullptr;
    int nelems = 0;
    int ecap = 0;
};

struct Binding {
    Str* name;
    Value val;
};

struct Env {
    Env* parent = nullptr;
    Binding* b = nullptr;
    int n = 0;
    int cap = 0;
    bool has_this = false;
    Value this_val;
};

typedef Value (*NativeFn)(Value thisv, Value* args, int argc);

struct Func {
    bool native = false;
    bool arrow = false;
    NativeFn nf = nullptr;
    Str** params = nullptr;
    int nparams = 0;
    void* body = nullptr;   // Node*
    Env* closure = nullptr;
    Str* name = nullptr;
};

// ----------------------------- AST -----------------------------------------
enum class NK {
    Num, Str, Bool, Null, Undef, Ident, This,
    Array, Object, Property, Func,
    Call, New, Member, Index,
    Unary, Update, Binary, Logical, Assign,
    Cond, Seq,
    VarDecl, ExprStmt, Block, If, While, DoWhile, For, ForIn, Switch, Case,
    FuncDecl, Return, Break, Continue, Empty
};

struct Node {
    NK k;
    double num = 0;
    bool bval = false;          // boolean literal value
    bool flag = false;          // prefix / const
    Str* s = nullptr;
    char op[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    Node* a = nullptr;
    Node* b = nullptr;
    Node* c = nullptr;
    Node* d = nullptr;
    Node** list = nullptr;
    int n = 0;
    Str** names = nullptr;
    int nn = 0;
};

// ----------------------------- Helpers -------------------------------------
static Str* mkstr(const char* s, int len) {
    Str* r = g_A->make<Str>();
    if (!r) return nullptr;
    char* d = static_cast<char*>(g_A->alloc(static_cast<size_t>(len) + 1));
    if (!d) { r->len = 0; r->data = nullptr; return r; }
    for (int i = 0; i < len; ++i) d[i] = s[i];
    d[len] = '\0';
    r->len = len;
    r->data = d;
    return r;
}
static Str* mkstr(const char* s) { return mkstr(s, static_cast<int>(kstd::strlen(s))); }
static Str* mkstr_range(Str* a, int start, int count) {
    if (!a) return mkstr("", 0);
    if (start < 0) start = 0;
    if (start > a->len) start = a->len;
    if (count < 0) count = 0;
    if (start + count > a->len) count = a->len - start;
    return mkstr(a->data + start, count);
}

static bool str_eq(Str* a, Str* b) {
    if (a == b) return true;
    if (!a || !b) return !a && !b;
    if (a->len != b->len) return false;
    return kstd::memcmp(a->data, b->data, static_cast<size_t>(a->len)) == 0;
}
static bool str_eq_c(Str* a, const char* b) {
    int bl = static_cast<int>(kstd::strlen(b));
    if (!a || a->len != bl) return false;
    return kstd::memcmp(a->data, b, static_cast<size_t>(bl)) == 0;
}

static Value vundef() { return Value{}; }
static Value vnull() { Value v; v.t = VT::Null; return v; }
static Value vbool(bool b) { Value v; v.t = VT::Bool; v.b = b; return v; }
static Value vnum(double n) { Value v; v.t = VT::Num; v.n = n; return v; }
static Value vstr(Str* s) { Value v; v.t = VT::Str; v.s = s; return v; }
static Value vobj(Obj* o) { Value v; v.t = VT::Obj; v.o = o; return v; }
static Value vfunc(Func* f) { Value v; v.t = VT::Func; v.f = f; return v; }

static Obj* mkobj(bool is_array = false) {
    Obj* o = g_A->make<Obj>();
    if (o) o->is_array = is_array;
    return o;
}

static Node* mknod(NK k) { Node* n = g_A->make<Node>(); if (n) n->k = k; return n; }

static double parse_float_c(const char* s);

static Str* num_to_str(double d) {
    char buf[64];
    if (d != d) return mkstr("NaN", 3);
    if (d == 0) return mkstr("0", 1);
    if (d < 0) {
        buf[0] = '-';
        int n = 1;
        double a = -d;
        unsigned long long ip = static_cast<unsigned long long>(a);
        char tmp[32];
        int ti = 0;
        if (ip == 0) tmp[ti++] = '0';
        while (ip > 0 && ti < 31) { tmp[ti++] = static_cast<char>('0' + (ip % 10)); ip /= 10; }
        for (int i = 0; i < ti; ++i) buf[n++] = tmp[ti - 1 - i];
        double frac = a - static_cast<double>(static_cast<unsigned long long>(a));
        if (frac > 1e-9) {
            buf[n++] = '.';
            for (int i = 0; i < 6; ++i) { frac *= 10; int dig = static_cast<int>(frac); buf[n++] = static_cast<char>('0' + dig); frac -= dig; }
            while (n > 1 && buf[n - 1] == '0') --n;
        }
        buf[n] = '\0';
        return mkstr(buf, n);
    }
    unsigned long long ip = static_cast<unsigned long long>(d);
    int n = 0;
    char tmp[32];
    int ti = 0;
    if (ip == 0) tmp[ti++] = '0';
    while (ip > 0 && ti < 31) { tmp[ti++] = static_cast<char>('0' + (ip % 10)); ip /= 10; }
    for (int i = 0; i < ti; ++i) buf[n++] = tmp[ti - 1 - i];
    double frac = d - static_cast<double>(static_cast<unsigned long long>(d));
    if (frac > 1e-9) {
        buf[n++] = '.';
        for (int i = 0; i < 6; ++i) { frac *= 10; int dig = static_cast<int>(frac); buf[n++] = static_cast<char>('0' + dig); frac -= dig; }
        while (n > 1 && buf[n - 1] == '0') --n;
    }
    buf[n] = '\0';
    return mkstr(buf, n);
}

// Forward decls
struct Interp;
static Interp* g_it = nullptr;
static Value eval_expr(struct Interp& it, Node* n);
static Value call_func(struct Interp& it, Func* fn, Value thisv, Value* args, int argc);
static Value get_prop(struct Interp& it, Value obj, Str* key);
static void set_prop(struct Interp& it, Value obj, Str* key, Value v);
static bool to_bool(const Value& v);
static double to_num(struct Interp& it, const Value& v);
static Str* to_str(struct Interp& it, const Value& v);
static bool value_identity(const Value& a, const Value& b);
static int strcmp_cmp(Str* a, Str* b);

// ----------------------------- Lexer ---------------------------------------
enum class TK { End, Num, Str, Ident, Punct };

struct Token {
    TK k = TK::End;
    double num = 0;
    Str* sval = nullptr;
    char text[16] = {0};
};

struct Lexer {
    const char* p;
    const char* end;
    Token cur;

    explicit Lexer(const char* src, size_t len) : p(src), end(src + len) { advance(); }

    void advance() {
        cur = Token{};
        while (p < end) {
            char c = *p;
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++p; continue; }
            if (c == '/' && p + 1 < end && p[1] == '/') { p += 2; while (p < end && *p != '\n') ++p; continue; }
            if (c == '/' && p + 1 < end && p[1] == '*') {
                p += 2;
                while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) ++p;
                if (p + 1 < end) p += 2; else p = end;
                continue;
            }
            break;
        }
        if (p >= end) { cur.k = TK::End; return; }

        char c = *p;
        // Numbers
        if ((c >= '0' && c <= '9') || (c == '.' && p + 1 < end && p[1] >= '0' && p[1] <= '9')) {
            const char* start = p;
            if (c == '0' && p + 1 < end && (p[1] == 'x' || p[1] == 'X')) {
                p += 2;
                uint64_t v = 0;
                while (p < end && is_hex(*p)) { v = v * 16 + hexval(*p); ++p; }
                cur.k = TK::Num; cur.num = static_cast<double>(v); return;
            }
            while (p < end && *p >= '0' && *p <= '9') ++p;
            if (p < end && *p == '.') { ++p; while (p < end && *p >= '0' && *p <= '9') ++p; }
            if (p < end && (*p == 'e' || *p == 'E')) {
                ++p;
                if (p < end && (*p == '+' || *p == '-')) ++p;
                while (p < end && *p >= '0' && *p <= '9') ++p;
            }
            cur.k = TK::Num; cur.num = parse_float_c(start);
            return;
        }
        // Strings
        if (c == '"' || c == '\'') {
            char q = c; ++p;
            static char buf[4096];
            int n = 0;
            while (p < end && *p != q) {
                char ch = *p++;
                if (ch == '\\' && p < end) {
                    char e = *p++;
                    switch (e) {
                        case 'n': ch = '\n'; break;
                        case 't': ch = '\t'; break;
                        case 'r': ch = '\r'; break;
                        case 'b': ch = '\b'; break;
                        case 'f': ch = '\f'; break;
                        case 'v': ch = '\v'; break;
                        case '0': ch = '\0'; break;
                        case '\\': ch = '\\'; break;
                        case '\'': ch = '\''; break;
                        case '"': ch = '"'; break;
                        case 'u': {
                            int u = 0, cnt = 0;
                            while (cnt < 4 && p < end && is_hex(*p)) { u = u * 16 + hexval(*p); ++p; ++cnt; }
                            if (u < 128) ch = static_cast<char>(u); else ch = '?';
                            break;
                        }
                        default: ch = e; break;
                    }
                }
                if (n < 4095) buf[n++] = ch;
            }
            if (p < end && *p == q) ++p;
            cur.k = TK::Str; cur.sval = mkstr(buf, n);
            return;
        }
        // Identifiers / keywords
        if (is_ident_start(c)) {
            const char* start = p;
            while (p < end && is_ident(*p)) ++p;
            int len = static_cast<int>(p - start);
            if (len > 15) len = 15;
            for (int i = 0; i < len; ++i) cur.text[i] = start[i];
            cur.text[len] = '\0';
            cur.k = TK::Ident;
            return;
        }
        // Punctuation / operators
        static const char* const ops3[] = { "===", "!==", ">>>", "<<=", ">>=", "**=", "...", "||=", "&&=" };
        static const char* const ops2[] = { "==", "!=", "<=", ">=", "&&", "||", "++", "--", "+=", "-=", "*=", "/=", "%=", "<<", ">>", "**", "=>", "??" };
        for (const char* o : ops3) {
            if (p + 3 <= end && o[0] == p[0] && o[1] == p[1] && o[2] == p[2]) {
                cur.k = TK::Punct; kstd::strncpy(cur.text, o, 3); p += 3; return;
            }
        }
        for (const char* o : ops2) {
            if (p + 2 <= end && o[0] == p[0] && o[1] == p[1]) {
                cur.k = TK::Punct; kstd::strncpy(cur.text, o, 2); p += 2; return;
            }
        }
        cur.k = TK::Punct; cur.text[0] = c; cur.text[1] = '\0'; ++p;
    }

    Token peek() const {
        Lexer tmp = *this;
        tmp.advance();
        return tmp.cur;
    }
    bool peek_p(const char* o) const {
        Token t = peek();
        return t.k == TK::Punct && kstd::strcmp(t.text, o) == 0;
    }

    static bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$'; }
    static bool is_ident(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }
    static bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
    static int hexval(char c) { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; return c - 'A' + 10; }
};

static double parse_float_c(const char* s) {
    double sign = 1;
    if (*s == '-') { sign = -1; ++s; } else if (*s == '+') { ++s; }
    double v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); ++s; }
    if (*s == '.') {
        ++s;
        double f = 0.1;
        while (*s >= '0' && *s <= '9') { v += (*s - '0') * f; f *= 0.1; ++s; }
    }
    if (*s == 'e' || *s == 'E') {
        ++s;
        int es = 1;
        if (*s == '-') { es = -1; ++s; } else if (*s == '+') ++s;
        int e = 0;
        while (*s >= '0' && *s <= '9') { e = e * 10 + (*s - '0'); ++s; }
        while (e-- > 0) v = (es > 0) ? v * 10 : v / 10;
    }
    return sign * v;
}

// ----------------------------- Parser --------------------------------------
struct Parser {
    Lexer lx;
    Node* program = nullptr;
    bool failed = false;

    explicit Parser(const char* src, size_t len) : lx(src, len) {}

    bool is_kw(const char* w) { return lx.cur.k == TK::Ident && kstd::strcmp(lx.cur.text, w) == 0; }
    bool is_p(const char* o) { return lx.cur.k == TK::Punct && kstd::strcmp(lx.cur.text, o) == 0; }
    void next() { lx.advance(); }
    bool eat_p(const char* o) { if (is_p(o)) { next(); return true; } return false; }
    bool eat_kw(const char* w) { if (is_kw(w)) { next(); return true; } return false; }
    void semi() { if (!eat_p(";")) { /* ASI: ignore */ } }

    Node** grow(Node** arr, int& cap, int need) {
        if (need <= cap) return arr;
        int nc = cap ? cap * 2 : 8;
        while (nc < need) nc *= 2;
        void* np = g_A->alloc(sizeof(Node*) * static_cast<size_t>(nc));
        Node** na = static_cast<Node**>(np);
        for (int i = 0; i < cap; ++i) na[i] = arr[i];
        cap = nc;
        return na;
    }

    Node* parse_program() {
        Node* blk = mknod(NK::Block);
        int cap = 0;
        while (lx.cur.k != TK::End && !failed) {
            Node* s = parse_statement();
            if (!s) break;
            if (s->k == NK::Empty) continue;
            blk->list = grow(blk->list, cap, blk->n + 1);
            blk->list[blk->n++] = s;
        }
        program = blk;
        return blk;
    }

    Node* parse_statement() {
        if (is_p("{")) return parse_block();
        if (is_p(";")) { next(); return mknod(NK::Empty); }
        if (is_kw("var") || is_kw("let") || is_kw("const")) return parse_vardecl(true);
        if (is_kw("function")) return parse_funcdecl();
        if (is_kw("if")) return parse_if();
        if (is_kw("while")) return parse_while();
        if (is_kw("do")) return parse_do();
        if (is_kw("for")) return parse_for();
        if (is_kw("switch")) return parse_switch();
        if (is_kw("return")) return parse_return();
        if (is_kw("break")) { next(); semi(); return mknod(NK::Break); }
        if (is_kw("continue")) { next(); semi(); return mknod(NK::Continue); }
        Node* e = parse_expression();
        semi();
        Node* st = mknod(NK::ExprStmt);
        st->a = e;
        return st;
    }

    Node* parse_block() {
        Node* blk = mknod(NK::Block);
        eat_p("{");
        int cap = 0;
        while (!is_p("}") && lx.cur.k != TK::End && !failed) {
            Node* s = parse_statement();
            if (!s) break;
            if (s->k == NK::Empty) continue;
            blk->list = grow(blk->list, cap, blk->n + 1);
            blk->list[blk->n++] = s;
        }
        eat_p("}");
        return blk;
    }

    Node* parse_vardecl(bool consume_semi) {
        bool is_const = is_kw("const");
        next(); // var/let/const
        Node* blk = mknod(NK::Block);
        int cap = 0;
        while (true) {
            if (lx.cur.k != TK::Ident) { failed = true; break; }
            Node* vd = mknod(NK::VarDecl);
            vd->s = mkstr(lx.cur.text);
            vd->flag = is_const;
            next();
            if (eat_p("=")) vd->a = parse_assign();
            blk->list = grow(blk->list, cap, blk->n + 1);
            blk->list[blk->n++] = vd;
            if (!eat_p(",")) break;
        }
        if (consume_semi) semi();
        if (blk->n == 1) return blk->list[0];
        return blk;
    }

    Node* parse_funcdecl() {
        next(); // function
        Node* fn = mknod(NK::FuncDecl);
        if (lx.cur.k == TK::Ident) { fn->s = mkstr(lx.cur.text); next(); }
        parse_params(fn);
        fn->a = parse_block();
        return fn;
    }

    void parse_params(Node* fn) {
        eat_p("(");
        int cap = 0;
        while (!is_p(")") && lx.cur.k != TK::End) {
            if (lx.cur.k != TK::Ident) { next(); continue; }
            if (fn->nn >= cap) {
                int nc = cap ? cap * 2 : 8;
                Str** na = static_cast<Str**>(g_A->alloc(sizeof(Str*) * static_cast<size_t>(nc)));
                for (int i = 0; i < fn->nn; ++i) na[i] = fn->names[i];
                fn->names = na;
                cap = nc;
            }
            fn->names[fn->nn++] = mkstr(lx.cur.text);
            next();
            if (!eat_p(",")) break;
        }
        eat_p(")");
    }

    Node* parse_if() {
        next();
        Node* n = mknod(NK::If);
        eat_p("(");
        n->a = parse_expression();
        eat_p(")");
        n->b = parse_statement();
        if (eat_kw("else")) n->c = parse_statement();
        return n;
    }

    Node* parse_while() {
        next();
        Node* n = mknod(NK::While);
        eat_p("(");
        n->a = parse_expression();
        eat_p(")");
        n->b = parse_statement();
        return n;
    }

    Node* parse_do() {
        next();
        Node* n = mknod(NK::DoWhile);
        n->b = parse_statement();
        eat_kw("while");
        eat_p("(");
        n->a = parse_expression();
        eat_p(")");
        semi();
        return n;
    }

    Node* parse_for() {
        next();
        eat_p("(");
        Node* n = mknod(NK::For);
        if (is_kw("var") || is_kw("let") || is_kw("const")) {
            n->a = parse_vardecl(false);
        } else if (!is_p(";")) {
            n->a = parse_expression();
        }
        eat_p(";");
        if (!is_p(";")) n->b = parse_expression();
        eat_p(";");
        if (!is_p(")")) n->c = parse_expression();
        eat_p(")");
        n->d = parse_statement();
        return n;
    }

    Node* parse_switch() {
        next();
        Node* n = mknod(NK::Switch);
        eat_p("(");
        n->a = parse_expression();
        eat_p(")");
        eat_p("{");
        int ccap = 0;
        while (!is_p("}") && lx.cur.k != TK::End && !failed) {
            Node* cs = mknod(NK::Case);
            if (eat_kw("case")) {
                cs->a = parse_expression();
                eat_p(":");
            } else if (eat_kw("default")) {
                eat_p(":");
            } else {
                next();
                continue;
            }
            int cap = 0;
            while (!is_p("}") && !is_kw("case") && !is_kw("default") && lx.cur.k != TK::End && !failed) {
                Node* s = parse_statement();
                if (!s) break;
                if (s->k == NK::Empty) continue;
                cs->list = grow(cs->list, cap, cs->n + 1);
                cs->list[cs->n++] = s;
            }
            n->list = grow(n->list, ccap, n->n + 1);
            n->list[n->n++] = cs;
        }
        eat_p("}");
        return n;
    }

    Node* parse_return() {
        next();
        Node* n = mknod(NK::Return);
        if (!is_p(";") && !is_p("}") && lx.cur.k != TK::End) n->a = parse_expression();
        semi();
        return n;
    }

    Node* parse_expression() {
        Node* e = parse_assign();
        if (is_p(",")) {
            Node* seq = mknod(NK::Seq);
            int cap = 2;
            seq->list = static_cast<Node**>(g_A->alloc(sizeof(Node*) * 16));
            seq->list[seq->n++] = e;
            while (eat_p(",")) {
                if (seq->n >= cap) { cap *= 2; Node** na = static_cast<Node**>(g_A->alloc(sizeof(Node*) * static_cast<size_t>(cap))); for (int i = 0; i < seq->n; ++i) na[i] = seq->list[i]; seq->list = na; }
                seq->list[seq->n++] = parse_assign();
            }
            return seq;
        }
        return e;
    }

    static bool is_assign_op(const char* t) {
        return !kstd::strcmp(t, "=") || !kstd::strcmp(t, "+=") || !kstd::strcmp(t, "-=") ||
               !kstd::strcmp(t, "*=") || !kstd::strcmp(t, "/=") || !kstd::strcmp(t, "%=");
    }

    Node* parse_arrow(Node* lhs) {
        next(); // =>
        Node* fn = mknod(NK::Func);
        fn->flag = true; // arrow (lexical this)
        int count = 0;
        if (lhs && lhs->k == NK::Ident) count = 1;
        else if (lhs && lhs->k == NK::Seq) count = lhs->n;
        if (count > 0) {
            fn->names = static_cast<Str**>(g_A->alloc(sizeof(Str*) * static_cast<size_t>(count)));
            if (lhs->k == NK::Ident) fn->names[0] = lhs->s;
            else for (int i = 0; i < count; ++i) {
                Node* p = lhs->list[i];
                fn->names[i] = (p && p->k == NK::Ident) ? p->s : mkstr("_", 1);
            }
            fn->nn = count;
        }
        if (is_p("{")) {
            fn->a = parse_block();
        } else {
            Node* ret = mknod(NK::Return);
            ret->a = parse_assign();
            Node* blk = mknod(NK::Block);
            blk->list = static_cast<Node**>(g_A->alloc(sizeof(Node*)));
            blk->list[0] = ret;
            blk->n = 1;
            fn->a = blk;
        }
        return fn;
    }

    Node* parse_assign() {
        Node* lhs = parse_conditional();
        if (is_p("=>")) return parse_arrow(lhs);
        if (lx.cur.k == TK::Punct && is_assign_op(lx.cur.text)) {
            Node* n = mknod(NK::Assign);
            kstd::strncpy(n->op, lx.cur.text, 3);
            next();
            n->a = lhs;
            n->b = parse_assign();
            return n;
        }
        return lhs;
    }

    Node* parse_conditional() {
        Node* test = parse_binary(1);
        if (eat_p("?")) {
            Node* n = mknod(NK::Cond);
            n->a = test;
            n->b = parse_assign();
            eat_p(":");
            n->c = parse_assign();
            return n;
        }
        return test;
    }

    static int prec(const char* op) {
        if (!kstd::strcmp(op, "||") || !kstd::strcmp(op, "??")) return 1;
        if (!kstd::strcmp(op, "&&")) return 2;
        if (!kstd::strcmp(op, "|")) return 3;
        if (!kstd::strcmp(op, "^")) return 4;
        if (!kstd::strcmp(op, "&")) return 5;
        if (!kstd::strcmp(op, "==") || !kstd::strcmp(op, "!=") || !kstd::strcmp(op, "===") || !kstd::strcmp(op, "!==")) return 6;
        if (!kstd::strcmp(op, "<") || !kstd::strcmp(op, ">") || !kstd::strcmp(op, "<=") || !kstd::strcmp(op, ">=")) return 7;
        if (!kstd::strcmp(op, "<<") || !kstd::strcmp(op, ">>") || !kstd::strcmp(op, ">>>")) return 8;
        if (!kstd::strcmp(op, "+") || !kstd::strcmp(op, "-")) return 9;
        if (!kstd::strcmp(op, "*") || !kstd::strcmp(op, "/") || !kstd::strcmp(op, "%")) return 10;
        if (!kstd::strcmp(op, "**")) return 11;
        return 0;
    }

    Node* parse_binary(int minp) {
        Node* left = parse_unary();
        while (lx.cur.k == TK::Punct) {
            int p = prec(lx.cur.text);
            if (p == 0 || p < minp) break;
            char op[4] = {0};
            kstd::strncpy(op, lx.cur.text, 3);
            // do not confuse "=" with "=="
            if (!kstd::strcmp(op, "=")) break;
            bool logical = (!kstd::strcmp(op, "&&") || !kstd::strcmp(op, "||") || !kstd::strcmp(op, "??"));
            next();
            Node* right = parse_binary(logical ? p + 1 : p + 1);
            Node* n = mknod(logical ? NK::Logical : NK::Binary);
            kstd::strncpy(n->op, op, 3);
            n->a = left;
            n->b = right;
            left = n;
        }
        return left;
    }

    Node* parse_unary() {
        if (is_p("!") || is_p("-") || is_p("+") || is_p("~") || is_kw("typeof") || is_kw("void") || is_kw("delete")) {
            Node* n = mknod(NK::Unary);
            kstd::strncpy(n->op, lx.cur.text, sizeof(n->op) - 1);
            n->op[sizeof(n->op) - 1] = '\0';
            next();
            n->a = parse_unary();
            return n;
        }
        if (is_p("++") || is_p("--")) {
            Node* n = mknod(NK::Update);
            kstd::strncpy(n->op, lx.cur.text, 3);
            n->flag = true; // prefix
            next();
            n->a = parse_unary();
            return n;
        }
        return parse_postfix();
    }

    Node* parse_postfix() {
        Node* e = parse_call_member();
        while (is_p("++") || is_p("--")) {
            Node* n = mknod(NK::Update);
            kstd::strncpy(n->op, lx.cur.text, 3);
            n->flag = false;
            n->a = e;
            next();
            e = n;
        }
        return e;
    }

    Node* parse_call_member() {
        Node* e = parse_primary();
        while (true) {
            if (eat_p(".")) {
                Node* n = mknod(NK::Member);
                n->a = e;
                if (lx.cur.k == TK::Ident) { n->s = mkstr(lx.cur.text); next(); }
                e = n;
            } else if (eat_p("[")) {
                Node* n = mknod(NK::Index);
                n->a = e;
                n->b = parse_expression();
                eat_p("]");
                e = n;
            } else if (is_p("(")) {
                Node* n = mknod(NK::Call);
                n->a = e;
                parse_args(n);
                e = n;
            } else {
                break;
            }
        }
        return e;
    }

    void parse_args(Node* call) {
        eat_p("(");
        int cap = 0;
        while (!is_p(")") && lx.cur.k != TK::End) {
            call->list = grow(call->list, cap, call->n + 1);
            call->list[call->n++] = parse_assign();
            if (!eat_p(",")) break;
        }
        eat_p(")");
    }

    Node* parse_primary() {
        Token t = lx.cur;
        if (t.k == TK::Num) { next(); Node* n = mknod(NK::Num); n->num = t.num; return n; }
        if (t.k == TK::Str) { next(); Node* n = mknod(NK::Str); n->s = t.sval; return n; }
        if (is_p("(")) {
            if (lx.peek_p(")")) {
                next(); next();
                return mknod(NK::Seq);
            }
            next(); Node* e = parse_expression(); eat_p(")"); return e;
        }
        if (is_p("[")) return parse_array();
        if (is_p("{")) return parse_object();
        if (is_kw("function")) return parse_func_expr();
        if (is_kw("new")) return parse_new();
        if (is_kw("true")) { next(); Node* n = mknod(NK::Bool); n->bval = true; return n; }
        if (is_kw("false")) { next(); Node* n = mknod(NK::Bool); n->bval = false; return n; }
        if (is_kw("null")) { next(); return mknod(NK::Null); }
        if (is_kw("undefined")) { next(); return mknod(NK::Undef); }
        if (is_kw("this")) { next(); return mknod(NK::This); }
        if (t.k == TK::Ident) { next(); Node* n = mknod(NK::Ident); n->s = mkstr(t.text); return n; }
        // Unknown token: skip to avoid infinite loop
        next();
        return mknod(NK::Undef);
    }

    Node* parse_array() {
        next(); // [
        Node* n = mknod(NK::Array);
        int cap = 0;
        while (!is_p("]") && lx.cur.k != TK::End) {
            if (is_p(",")) { n->list = grow(n->list, cap, n->n + 1); n->list[n->n++] = mknod(NK::Undef); next(); continue; }
            n->list = grow(n->list, cap, n->n + 1);
            n->list[n->n++] = parse_assign();
            if (!is_p("]")) eat_p(",");
        }
        eat_p("]");
        return n;
    }

    Node* parse_object() {
        next(); // {
        Node* n = mknod(NK::Object);
        int cap = 0;
        while (!is_p("}") && lx.cur.k != TK::End) {
            Node* prop = mknod(NK::Property);
            if (lx.cur.k == TK::Str) { prop->s = lx.cur.sval; next(); }
            else if (lx.cur.k == TK::Ident) { prop->s = mkstr(lx.cur.text); next(); }
            else if (lx.cur.k == TK::Num) { prop->s = num_to_str(lx.cur.num); next(); }
            else { next(); continue; }
            if (eat_p(":")) prop->a = parse_assign();
            n->list = grow(n->list, cap, n->n + 1);
            n->list[n->n++] = prop;
            if (!eat_p(",")) break;
        }
        eat_p("}");
        return n;
    }

    Node* parse_func_expr() {
        next(); // function
        Node* fn = mknod(NK::Func);
        if (lx.cur.k == TK::Ident) { fn->s = mkstr(lx.cur.text); next(); }
        parse_params(fn);
        fn->a = parse_block();
        return fn;
    }

    Node* parse_new() {
        next(); // new
        Node* callee = parse_call_member();
        Node* n = mknod(NK::New);
        if (callee->k == NK::Call) { n->a = callee->a; n->list = callee->list; n->n = callee->n; }
        else n->a = callee;
        return n;
    }
};

// ----------------------------- Interp --------------------------------------
struct Interp {
    Env* global = nullptr;
    Host* host = nullptr;
    Env* env = nullptr;
    Value last_exception; // unused, present for symmetry

    Env* new_env(Env* parent) {
        Env* e = g_A->make<Env>();
        e->parent = parent;
        return e;
    }
    Binding* find(Env* e, Str* name) {
        for (Env* s = e; s; s = s->parent) {
            for (int i = 0; i < s->n; ++i) if (str_eq(s->b[i].name, name)) return &s->b[i];
        }
        return nullptr;
    }
    Binding* declare(Env* e, Str* name) {
        for (int i = 0; i < e->n; ++i) if (str_eq(e->b[i].name, name)) return &e->b[i];
        if (e->n + 1 > e->cap) {
            int nc = e->cap ? e->cap * 2 : 8;
            Binding* nb = static_cast<Binding*>(g_A->alloc(sizeof(Binding) * static_cast<size_t>(nc)));
            for (int i = 0; i < e->n; ++i) nb[i] = e->b[i];
            e->b = nb; e->cap = nc;
        }
        e->b[e->n].name = name;
        e->b[e->n].val = vundef();
        return &e->b[e->n++];
    }
    void set(Str* name, Value v) {
        Binding* b = find(env, name);
        if (b) { b->val = v; return; }
        Binding* g = declare(global, name);
        g->val = v;
    }
    void set_this(Env* e, Value v) { e->has_this = true; e->this_val = v; }
};

// string / array properties grow helpers
static void obj_set(Obj* o, Str* key, Value v) {
    for (int i = 0; i < o->nprops; ++i) if (str_eq(o->props[i].key, key)) { o->props[i].val = v; return; }
    if (o->nprops + 1 > o->pcap) {
        int nc = o->pcap ? o->pcap * 2 : 8;
        Prop* np = static_cast<Prop*>(g_A->alloc(sizeof(Prop) * static_cast<size_t>(nc)));
        for (int i = 0; i < o->nprops; ++i) np[i] = o->props[i];
        o->props = np; o->pcap = nc;
    }
    o->props[o->nprops].key = key;
    o->props[o->nprops].val = v;
    o->nprops++;
}
static bool obj_get(Obj* o, Str* key, Value* out) {
    for (int i = 0; i < o->nprops; ++i) if (str_eq(o->props[i].key, key)) { *out = o->props[i].val; return true; }
    return false;
}
static void arr_ensure(Obj* o, int idx) {
    if (idx < 0) return;
    if (idx + 1 > o->ecap) {
        int nc = o->ecap ? o->ecap * 2 : 8;
        while (nc < idx + 1) nc *= 2;
        Value* ne = static_cast<Value*>(g_A->alloc(sizeof(Value) * static_cast<size_t>(nc)));
        for (int i = 0; i < o->nelems; ++i) ne[i] = o->elems[i];
        for (int i = o->nelems; i < nc; ++i) ne[i] = vundef();
        o->elems = ne; o->ecap = nc;
    }
    while (o->nelems <= idx) { o->elems[o->nelems] = vundef(); o->nelems++; }
}

// ----------------------------- Conversions ---------------------------------
static bool to_bool(const Value& v) {
    switch (v.t) {
        case VT::Undef: case VT::Null: return false;
        case VT::Bool: return v.b;
        case VT::Num: return v.n != 0 && v.n == v.n;
        case VT::Str: return v.s && v.s->len > 0;
        default: return true;
    }
}
static double to_num(struct Interp& it, const Value& v) {
    (void)it;
    switch (v.t) {
        case VT::Num: return v.n;
        case VT::Bool: return v.b ? 1 : 0;
        case VT::Null: return 0;
        case VT::Undef: return 0.0 / 0.0;
        case VT::Str: {
            if (!v.s || v.s->len == 0) return 0;
            const char* s = v.s->data;
            while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') ++s;
            if (!((*s >= '0' && *s <= '9') || *s == '-' || *s == '+' || *s == '.')) return 0.0 / 0.0;
            return parse_float_c(s);
        }
        default: return 0.0 / 0.0;
    }
}
static Str* arr_join(struct Interp& it, Obj* o, const char* sep);
static Str* to_str(struct Interp& it, const Value& v) {
    switch (v.t) {
        case VT::Undef: return mkstr("undefined", 9);
        case VT::Null: return mkstr("null", 4);
        case VT::Bool: return mkstr(v.b ? "true" : "false", v.b ? 4 : 5);
        case VT::Num: return num_to_str(v.n);
        case VT::Str: return v.s ? v.s : mkstr("", 0);
        case VT::Func: return mkstr("function () { [native code] }");
        case VT::Obj:
            if (v.o && v.o->is_array) return arr_join(it, v.o, ",");
            return mkstr("[object Object]");
    }
    return mkstr("", 0);
}

// ----------------------------- Property access -----------------------------
struct Interp;

static int str_index_of(Str* h, Str* n, int from) {
    if (!h || !n || n->len == 0) return 0;
    for (int i = from; i + n->len <= h->len; ++i) {
        if (kstd::memcmp(h->data + i, n->data, static_cast<size_t>(n->len)) == 0) return i;
    }
    return -1;
}

// (string methods are implemented inside get_prop with Interp access)

// ----------------------------- Builtins ------------------------------------
static Func* mknative(NativeFn fn) {
    Func* f = g_A->make<Func>();
    f->native = true;
    f->nf = fn;
    return f;
}

// global functions
static Value g_parseInt(Value, Value* a, int ac) {
    if (ac < 1 || a[0].t != VT::Str || !a[0].s) return vnum(0.0 / 0.0);
    const char* s = a[0].s->data;
    while (*s == ' ' || *s == '\t') ++s;
    int sign = 1;
    if (*s == '-') { sign = -1; ++s; } else if (*s == '+') ++s;
    int radix = 10;
    if (ac > 1) radix = static_cast<int>(to_num(*g_it, a[1]));
    if (radix == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    double v = 0; bool any = false;
    while (*s) {
        int d;
        char c = *s;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else break;
        if (d >= radix) break;
        v = v * radix + d; any = true; ++s;
    }
    return vnum(any ? sign * v : 0.0 / 0.0);
}
static Value g_parseFloat(Value, Value* a, int ac) {
    if (ac < 1 || a[0].t != VT::Str || !a[0].s) return vnum(0.0 / 0.0);
    const char* s = a[0].s->data;
    while (*s == ' ' || *s == '\t') ++s;
    return vnum(parse_float_c(s));
}
static Value g_isNaN(Value, Value* a, int ac) { double d = ac > 0 ? to_num(*g_it, a[0]) : 0.0 / 0.0; return vbool(d != d); }
static Value g_Number(Value, Value* a, int ac) { return vnum(ac > 0 ? to_num(*g_it, a[0]) : 0); }
static Value g_String(Value, Value* a, int ac) { return vstr(ac > 0 ? to_str(*g_it, a[0]) : mkstr("", 0)); }
static Value g_Boolean(Value, Value* a, int ac) { return vbool(ac > 0 ? to_bool(a[0]) : false); }

static Value str_pad(Value tv, Value* a, int ac, bool start) {
    Str* s = tv.t == VT::Str ? tv.s : mkstr("", 0);
    int target = ac > 0 ? static_cast<int>(to_num(*g_it, a[0])) : 0;
    Str* pad = (ac > 1 && a[1].t == VT::Str) ? a[1].s : mkstr(" ", 1);
    if (target <= s->len || pad->len == 0) return vstr(s);
    int total = target;
    char* d = static_cast<char*>(g_A->alloc(static_cast<size_t>(total) + 1));
    int fill = target - s->len;
    int n = 0;
    if (start) {
        for (int i = 0; i < fill; ++i) d[n++] = pad->data[i % pad->len];
        for (int i = 0; i < s->len; ++i) d[n++] = s->data[i];
    } else {
        for (int i = 0; i < s->len; ++i) d[n++] = s->data[i];
        for (int i = 0; i < fill; ++i) d[n++] = pad->data[i % pad->len];
    }
    d[n] = '\0';
    Str* r = mkstr("", 0); r->data = d; r->len = n;
    return vstr(r);
}

// ----------------------------- Property get/set ----------------------------
// (declared earlier)

// We need Interp for to_num/to_str; define methods after Interp known. Rather
// than fragment, the interpreter core below uses these free functions.

// ----------------------------- The evaluator -------------------------------
struct Completion { int kind; Value val; }; // 0 normal,1 return,2 break,3 continue

static int prop_to_index(Str* key) {
    if (!key || key->len == 0) return -1;
    int v = 0;
    for (int i = 0; i < key->len; ++i) {
        if (key->data[i] < '0' || key->data[i] > '9') return -1;
        v = v * 10 + (key->data[i] - '0');
    }
    return v;
}

static Value get_global(Interp& it, Str* key);

// property get for arrays and objects and strings and math etc.
static Value get_prop(struct Interp& it, Value obj, Str* key) {
    (void)it;
    if (!key) return vundef();
    if (obj.t == VT::Obj && obj.o) {
        Obj* o = obj.o;
        if (o->is_array) {
            if (str_eq_c(key, "length")) return vnum(o->nelems);
            int idx = prop_to_index(key);
            if (idx >= 0) return idx < o->nelems ? o->elems[idx] : vundef();
            // array methods
            if (str_eq_c(key, "push")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
                if (tv.t == VT::Obj && tv.o && tv.o->is_array) { for (int i = 0; i < ac; ++i) { arr_ensure(tv.o, tv.o->nelems); tv.o->elems[tv.o->nelems - 1] = a[i]; } return vnum(tv.o->nelems); }
                return vundef();
            }));
            if (str_eq_c(key, "pop")) return vfunc(mknative([](Value tv, Value*, int) -> Value {
                if (tv.t == VT::Obj && tv.o && tv.o->is_array && tv.o->nelems > 0) { Value v = tv.o->elems[tv.o->nelems - 1]; tv.o->nelems--; return v; }
                return vundef();
            }));
            if (str_eq_c(key, "shift")) return vfunc(mknative([](Value tv, Value*, int) -> Value {
                if (tv.t == VT::Obj && tv.o && tv.o->is_array && tv.o->nelems > 0) {
                    Value v = tv.o->elems[0];
                    for (int i = 1; i < tv.o->nelems; ++i) tv.o->elems[i - 1] = tv.o->elems[i];
                    tv.o->nelems--; return v;
                }
                return vundef();
            }));
            if (str_eq_c(key, "unshift")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
                if (tv.t == VT::Obj && tv.o && tv.o->is_array) {
                    for (int i = 0; i < ac; ++i) arr_ensure(tv.o, tv.o->nelems);
                    for (int i = tv.o->nelems - 1; i >= ac; --i) tv.o->elems[i] = tv.o->elems[i - ac];
                    for (int i = 0; i < ac; ++i) tv.o->elems[i] = a[i];
                    tv.o->nelems += 0;
                    return vnum(tv.o->nelems);
                }
                return vundef();
            }));
            if (str_eq_c(key, "join")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
                if (tv.t == VT::Obj && tv.o && tv.o->is_array) {
                    Str* sep = (ac > 0 && a[0].t == VT::Str) ? a[0].s : mkstr(",", 1);
                    Str* r = arr_join(*g_it, tv.o, sep->data);
                    return vstr(r);
                }
                return vstr(mkstr("", 0));
            }));
            if (str_eq_c(key, "indexOf")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
                if (tv.t == VT::Obj && tv.o && tv.o->is_array && ac > 0) {
                    int from = ac > 1 ? static_cast<int>(to_num(*g_it, a[1])) : 0;
                    if (from < 0) from += tv.o->nelems;
                    if (from < 0) from = 0;
                    for (int i = from; i < tv.o->nelems; ++i) {
                        if (value_identity(tv.o->elems[i], a[0])) return vnum(i);
                    }
                }
                return vnum(-1);
            }));
            if (str_eq_c(key, "slice")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
                if (!(tv.t == VT::Obj && tv.o && tv.o->is_array)) return vundef();
                int len = tv.o->nelems;
                int start = ac > 0 ? static_cast<int>(to_num(*g_it, a[0])) : 0;
                int end = ac > 1 ? static_cast<int>(to_num(*g_it, a[1])) : len;
                if (start < 0) start += len;
                if (end < 0) end += len;
                if (start < 0) start = 0;
                if (start > len) start = len;
                if (end < start) end = start;
                if (end > len) end = len;
                Obj* r = mkobj(true);
                for (int i = start; i < end; ++i) { arr_ensure(r, r->nelems); r->elems[r->nelems - 1] = tv.o->elems[i]; }
                return vobj(r);
            }));
            if (str_eq_c(key, "concat")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
                Obj* r = mkobj(true);
                if (tv.t == VT::Obj && tv.o) for (int i = 0; i < tv.o->nelems; ++i) { arr_ensure(r, r->nelems); r->elems[r->nelems - 1] = tv.o->elems[i]; }
                for (int j = 0; j < ac; ++j) {
                    if (a[j].t == VT::Obj && a[j].o && a[j].o->is_array) for (int i = 0; i < a[j].o->nelems; ++i) { arr_ensure(r, r->nelems); r->elems[r->nelems - 1] = a[j].o->elems[i]; }
                    else { arr_ensure(r, r->nelems); r->elems[r->nelems - 1] = a[j]; }
                }
                return vobj(r);
            }));
            if (str_eq_c(key, "reverse")) return vfunc(mknative([](Value tv, Value*, int) -> Value {
                if (tv.t == VT::Obj && tv.o && tv.o->is_array) { for (int i = 0, j = tv.o->nelems - 1; i < j; ++i, --j) { Value t = tv.o->elems[i]; tv.o->elems[i] = tv.o->elems[j]; tv.o->elems[j] = t; } }
                return tv;
            }));
            if (str_eq_c(key, "forEach")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
                if (tv.t == VT::Obj && tv.o && tv.o->is_array && ac > 0 && a[0].t == VT::Func) {
                    for (int i = 0; i < tv.o->nelems; ++i) { Value args[3] = { tv.o->elems[i], vnum(i), tv }; call_func(*g_it, a[0].f, vundef(), args, 3); }
                }
                return vundef();
            }));
            if (str_eq_c(key, "map")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
                Obj* r = mkobj(true);
                if (tv.t == VT::Obj && tv.o && tv.o->is_array && ac > 0 && a[0].t == VT::Func) {
                    for (int i = 0; i < tv.o->nelems; ++i) { Value args[3] = { tv.o->elems[i], vnum(i), tv }; Value m = call_func(*g_it, a[0].f, vundef(), args, 3); arr_ensure(r, r->nelems); r->elems[r->nelems - 1] = m; }
                }
                return vobj(r);
            }));
            if (str_eq_c(key, "filter")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
                Obj* r = mkobj(true);
                if (tv.t == VT::Obj && tv.o && tv.o->is_array && ac > 0 && a[0].t == VT::Func) {
                    for (int i = 0; i < tv.o->nelems; ++i) { Value args[3] = { tv.o->elems[i], vnum(i), tv }; Value k = call_func(*g_it, a[0].f, vundef(), args, 3); if (to_bool(k)) { arr_ensure(r, r->nelems); r->elems[r->nelems - 1] = tv.o->elems[i]; } }
                }
                return vobj(r);
            }));
            if (str_eq_c(key, "some")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
                if (tv.t == VT::Obj && tv.o && tv.o->is_array && ac > 0 && a[0].t == VT::Func) {
                    for (int i = 0; i < tv.o->nelems; ++i) { Value args[3] = { tv.o->elems[i], vnum(i), tv }; if (to_bool(call_func(*g_it, a[0].f, vundef(), args, 3))) return vbool(true); }
                }
                return vbool(false);
            }));
            if (str_eq_c(key, "every")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
                if (tv.t == VT::Obj && tv.o && tv.o->is_array && ac > 0 && a[0].t == VT::Func) {
                    for (int i = 0; i < tv.o->nelems; ++i) { Value args[3] = { tv.o->elems[i], vnum(i), tv }; if (!to_bool(call_func(*g_it, a[0].f, vundef(), args, 3))) return vbool(false); }
                }
                return vbool(true);
            }));
            if (str_eq_c(key, "find")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
                if (tv.t == VT::Obj && tv.o && tv.o->is_array && ac > 0 && a[0].t == VT::Func) {
                    for (int i = 0; i < tv.o->nelems; ++i) { Value args[3] = { tv.o->elems[i], vnum(i), tv }; if (to_bool(call_func(*g_it, a[0].f, vundef(), args, 3))) return tv.o->elems[i]; }
                }
                return vundef();
            }));
            if (str_eq_c(key, "findIndex")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
                if (tv.t == VT::Obj && tv.o && tv.o->is_array && ac > 0 && a[0].t == VT::Func) {
                    for (int i = 0; i < tv.o->nelems; ++i) { Value args[3] = { tv.o->elems[i], vnum(i), tv }; if (to_bool(call_func(*g_it, a[0].f, vundef(), args, 3))) return vnum(i); }
                }
                return vnum(-1);
            }));
            if (str_eq_c(key, "includes")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
                if (tv.t == VT::Obj && tv.o && tv.o->is_array && ac > 0) {
                    for (int i = 0; i < tv.o->nelems; ++i) if (value_identity(tv.o->elems[i], a[0])) return vbool(true);
                }
                return vbool(false);
            }));
            if (str_eq_c(key, "at")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
                if (!(tv.t == VT::Obj && tv.o && tv.o->is_array) || ac < 1) return vundef();
                int i = static_cast<int>(to_num(*g_it, a[0]));
                if (i < 0) i += tv.o->nelems;
                return (i >= 0 && i < tv.o->nelems) ? tv.o->elems[i] : vundef();
            }));
            if (str_eq_c(key, "reduce")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
                if (!(tv.t == VT::Obj && tv.o && tv.o->is_array) || ac < 1 || a[0].t != VT::Func) return vundef();
                int len = tv.o->nelems;
                if (len == 0 && ac < 2) return vundef();
                Value acc; int start = 0;
                if (ac > 1) acc = a[1];
                else { acc = tv.o->elems[0]; start = 1; }
                for (int i = start; i < len; ++i) { Value args[4] = { acc, tv.o->elems[i], vnum(i), tv }; acc = call_func(*g_it, a[0].f, vundef(), args, 4); }
                return acc;
            }));
            if (str_eq_c(key, "sort")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
                if (!(tv.t == VT::Obj && tv.o && tv.o->is_array)) return tv;
                Obj* o = tv.o;
                for (int i = 1; i < o->nelems; ++i) {
                    Value key = o->elems[i];
                    int j = i - 1;
                    while (j >= 0) {
                        int cmp = 0;
                        if (ac > 0 && a[0].t == VT::Func) {
                            Value args[2] = { o->elems[j], key };
                            double d = to_num(*g_it, call_func(*g_it, a[0].f, vundef(), args, 2));
                            cmp = d < 0 ? -1 : (d > 0 ? 1 : 0);
                        } else {
                            cmp = strcmp_cmp(to_str(*g_it, o->elems[j]), to_str(*g_it, key));
                        }
                        if (cmp <= 0) break;
                        o->elems[j + 1] = o->elems[j];
                        --j;
                    }
                    o->elems[j + 1] = key;
                }
                return tv;
            }));
            if (str_eq_c(key, "splice")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
                Obj* rem = mkobj(true);
                if (!(tv.t == VT::Obj && tv.o && tv.o->is_array)) return vobj(rem);
                Obj* o = tv.o;
                int len = o->nelems;
                int start = ac > 0 ? static_cast<int>(to_num(*g_it, a[0])) : 0;
                if (start < 0) start += len;
                if (start < 0) start = 0;
                if (start > len) start = len;
                int del = ac > 1 ? static_cast<int>(to_num(*g_it, a[1])) : len - start;
                if (del < 0) del = 0;
                if (start + del > len) del = len - start;
                int ins = ac > 2 ? ac - 2 : 0;
                for (int i = 0; i < del; ++i) { arr_ensure(rem, rem->nelems); rem->elems[rem->nelems - 1] = o->elems[start + i]; }
                Obj* tmp = mkobj(true);
                for (int i = 0; i < start; ++i) { arr_ensure(tmp, tmp->nelems); tmp->elems[tmp->nelems - 1] = o->elems[i]; }
                for (int i = 0; i < ins; ++i) { arr_ensure(tmp, tmp->nelems); tmp->elems[tmp->nelems - 1] = a[2 + i]; }
                for (int i = start + del; i < len; ++i) { arr_ensure(tmp, tmp->nelems); tmp->elems[tmp->nelems - 1] = o->elems[i]; }
                if (tmp->nelems > 0) arr_ensure(o, tmp->nelems - 1);
                for (int i = 0; i < tmp->nelems; ++i) o->elems[i] = tmp->elems[i];
                o->nelems = tmp->nelems;
                return vobj(rem);
            }));
            Value p; if (obj_get(o, key, &p)) return p;
            return vundef();
        }
        // plain object
        if (str_eq_c(key, "hasOwnProperty")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
            if (tv.t == VT::Obj && tv.o && ac > 0) { Value tmp; return vbool(obj_get(tv.o, to_str(*g_it, a[0]), &tmp)); }
            return vbool(false);
        }));
        Value p;
        if (obj_get(o, key, &p)) return p;
        return vundef();
    }
    if (obj.t == VT::Str) {
        Str* s = obj.s ? obj.s : mkstr("", 0);
        if (str_eq_c(key, "length")) return vnum(s->len);
        int idx = prop_to_index(key);
        if (idx >= 0) return idx < s->len ? vstr(mkstr(s->data + idx, 1)) : vundef();
        // string methods via host-independent lambdas
        if (str_eq_c(key, "charAt")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
            Str* s = tv.t == VT::Str ? tv.s : mkstr("", 0);
            int i = ac > 0 ? static_cast<int>(to_num(*g_it, a[0])) : 0;
            if (i < 0 || i >= s->len) return vstr(mkstr("", 0));
            return vstr(mkstr(s->data + i, 1));
        }));
        if (str_eq_c(key, "charCodeAt")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
            Str* s = tv.t == VT::Str ? tv.s : mkstr("", 0);
            int i = ac > 0 ? static_cast<int>(to_num(*g_it, a[0])) : 0;
            if (i < 0 || i >= s->len) return vnum(0.0 / 0.0);
            return vnum(static_cast<unsigned char>(s->data[i]));
        }));
        if (str_eq_c(key, "indexOf")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
            Str* s = tv.t == VT::Str ? tv.s : mkstr("", 0);
            if (ac < 1) return vnum(-1);
            Str* n = to_str(*g_it, a[0]);
            int from = ac > 1 ? static_cast<int>(to_num(*g_it, a[1])) : 0;
            return vnum(str_index_of(s, n, from));
        }));
        if (str_eq_c(key, "lastIndexOf")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
            Str* s = tv.t == VT::Str ? tv.s : mkstr("", 0);
            if (ac < 1) return vnum(-1);
            Str* n = to_str(*g_it, a[0]);
            int best = -1;
            for (int i = 0; i + n->len <= s->len; ++i) if (kstd::memcmp(s->data + i, n->data, static_cast<size_t>(n->len)) == 0) best = i;
            return vnum(best);
        }));
        if (str_eq_c(key, "substring") || str_eq_c(key, "slice") || str_eq_c(key, "substr")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
            Str* s = tv.t == VT::Str ? tv.s : mkstr("", 0);
            int len = s->len;
            int start = ac > 0 ? static_cast<int>(to_num(*g_it, a[0])) : 0;
            int end = ac > 1 ? static_cast<int>(to_num(*g_it, a[1])) : len;
            if (start < 0) start = 0;
            if (start > len) start = len;
            if (end < 0) end = 0;
            if (end > len) end = len;
            if (end < start) { int t = start; start = end; end = t; }
            return vstr(mkstr_range(s, start, end - start));
        }));
        if (str_eq_c(key, "toUpperCase")) return vfunc(mknative([](Value tv, Value*, int) -> Value {
            Str* s = tv.t == VT::Str ? tv.s : mkstr("", 0);
            Str* r = mkstr(s->data, s->len);
            for (int i = 0; i < r->len; ++i) r->data[i] = kstd::toupper(r->data[i]);
            return vstr(r);
        }));
        if (str_eq_c(key, "toLowerCase")) return vfunc(mknative([](Value tv, Value*, int) -> Value {
            Str* s = tv.t == VT::Str ? tv.s : mkstr("", 0);
            Str* r = mkstr(s->data, s->len);
            for (int i = 0; i < r->len; ++i) r->data[i] = kstd::tolower(r->data[i]);
            return vstr(r);
        }));
        if (str_eq_c(key, "trim")) return vfunc(mknative([](Value tv, Value*, int) -> Value {
            Str* s = tv.t == VT::Str ? tv.s : mkstr("", 0);
            int a0 = 0, a1 = s->len;
            while (a0 < a1 && (s->data[a0] == ' ' || s->data[a0] == '\t' || s->data[a0] == '\n' || s->data[a0] == '\r')) ++a0;
            while (a1 > a0 && (s->data[a1 - 1] == ' ' || s->data[a1 - 1] == '\t' || s->data[a1 - 1] == '\n' || s->data[a1 - 1] == '\r')) --a1;
            return vstr(mkstr_range(s, a0, a1 - a0));
        }));
        if (str_eq_c(key, "split")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
            Str* s = tv.t == VT::Str ? tv.s : mkstr("", 0);
            Obj* r = mkobj(true);
            if (ac == 0) { arr_ensure(r, 0); r->elems[0] = vstr(s); return vobj(r); }
            Str* sep = to_str(*g_it, a[0]);
            if (sep->len == 0) { for (int i = 0; i < s->len; ++i) { arr_ensure(r, r->nelems); r->elems[r->nelems - 1] = vstr(mkstr(s->data + i, 1)); } return vobj(r); }
            int start = 0;
            for (int i = 0; i + sep->len <= s->len; ) {
                if (kstd::memcmp(s->data + i, sep->data, static_cast<size_t>(sep->len)) == 0) {
                    arr_ensure(r, r->nelems); r->elems[r->nelems - 1] = vstr(mkstr_range(s, start, i - start));
                    i += sep->len; start = i;
                } else ++i;
            }
            arr_ensure(r, r->nelems); r->elems[r->nelems - 1] = vstr(mkstr_range(s, start, s->len - start));
            return vobj(r);
        }));
        if (str_eq_c(key, "replace")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
            Str* s = tv.t == VT::Str ? tv.s : mkstr("", 0);
            if (ac < 2) return vstr(s);
            Str* find = to_str(*g_it, a[0]);
            Str* rep = to_str(*g_it, a[1]);
            int at = str_index_of(s, find, 0);
            if (at < 0) return vstr(s);
            int newlen = s->len - find->len + rep->len;
            Str* r = mkstr("", 0);
            char* d = static_cast<char*>(g_A->alloc(static_cast<size_t>(newlen) + 1));
            int n = 0;
            for (int i = 0; i < at; ++i) d[n++] = s->data[i];
            for (int i = 0; i < rep->len; ++i) d[n++] = rep->data[i];
            for (int i = at + find->len; i < s->len; ++i) d[n++] = s->data[i];
            d[n] = '\0'; r->len = n; r->data = d;
            return vstr(r);
        }));
        if (str_eq_c(key, "concat")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
            Str* s = tv.t == VT::Str ? tv.s : mkstr("", 0);
            int total = s->len; for (int i = 0; i < ac; ++i) total += to_str(*g_it, a[i])->len;
            char* d = static_cast<char*>(g_A->alloc(static_cast<size_t>(total) + 1));
            int n = 0; for (int i = 0; i < s->len; ++i) d[n++] = s->data[i];
            for (int i = 0; i < ac; ++i) { Str* t = to_str(*g_it, a[i]); for (int j = 0; j < t->len; ++j) d[n++] = t->data[j]; }
            d[n] = '\0'; Str* r = mkstr("", 0); r->data = d; r->len = n; return vstr(r);
        }));
        if (str_eq_c(key, "includes")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
            Str* s = tv.t == VT::Str ? tv.s : mkstr("", 0);
            if (ac < 1) return vbool(false);
            return vbool(str_index_of(s, to_str(*g_it, a[0]), 0) >= 0);
        }));
        if (str_eq_c(key, "startsWith")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
            Str* s = tv.t == VT::Str ? tv.s : mkstr("", 0);
            if (ac < 1) return vbool(false);
            Str* n = to_str(*g_it, a[0]);
            return vbool(str_index_of(s, n, 0) == 0);
        }));
        if (str_eq_c(key, "endsWith")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
            Str* s = tv.t == VT::Str ? tv.s : mkstr("", 0);
            if (ac < 1) return vbool(false);
            Str* n = to_str(*g_it, a[0]);
            int end = ac > 1 ? static_cast<int>(to_num(*g_it, a[1])) : s->len;
            if (end > s->len) end = s->len;
            if (end < 0) end = 0;
            return vbool(n->len <= end && kstd::memcmp(s->data + end - n->len, n->data, static_cast<size_t>(n->len)) == 0);
        }));
        if (str_eq_c(key, "repeat")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
            Str* s = tv.t == VT::Str ? tv.s : mkstr("", 0);
            int count = ac > 0 ? static_cast<int>(to_num(*g_it, a[0])) : 0;
            if (count < 0) count = 0;
            if (count > 0 && s->len > 0 && count > 1048576 / s->len) count = 1048576 / s->len;
            int total = s->len * count;
            char* d = static_cast<char*>(g_A->alloc(static_cast<size_t>(total) + 1));
            int n = 0;
            for (int i = 0; i < count; ++i) for (int j = 0; j < s->len; ++j) d[n++] = s->data[j];
            d[n] = '\0';
            Str* r = mkstr("", 0); r->data = d; r->len = n;
            return vstr(r);
        }));
        if (str_eq_c(key, "padStart")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value { return str_pad(tv, a, ac, true); }));
        if (str_eq_c(key, "padEnd")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value { return str_pad(tv, a, ac, false); }));
        if (str_eq_c(key, "toString")) return vfunc(mknative([](Value tv, Value*, int) -> Value { return vstr(tv.t == VT::Str ? tv.s : mkstr("", 0)); }));
        return vundef();
    }
    if (obj.t == VT::Num) {
        if (str_eq_c(key, "toString")) return vfunc(mknative([](Value tv, Value*, int) -> Value { return vstr(num_to_str(tv.n)); }));
        if (str_eq_c(key, "toFixed")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
            int digits = ac > 0 ? static_cast<int>(to_num(*g_it, a[0])) : 0;
            char buf[64]; int n = 0;
            double d = tv.n < 0 ? -tv.n : tv.n;
            if (tv.n < 0) buf[n++] = '-';
            unsigned long long ip = static_cast<unsigned long long>(d);
            char tmp[32]; int ti = 0; if (ip == 0) tmp[ti++] = '0';
            while (ip > 0) { tmp[ti++] = static_cast<char>('0' + (ip % 10)); ip /= 10; }
            for (int i = 0; i < ti; ++i) buf[n++] = tmp[ti - 1 - i];
            if (digits > 0) {
                buf[n++] = '.';
                double frac = d - static_cast<double>(static_cast<unsigned long long>(d));
                for (int i = 0; i < digits; ++i) { frac *= 10; int dig = static_cast<int>(frac); buf[n++] = static_cast<char>('0' + dig); frac -= dig; }
            }
            buf[n] = '\0';
            return vstr(mkstr(buf, n));
        }));
        return vundef();
    }
    if (obj.t == VT::Func) {
        if (str_eq_c(key, "call")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
            if (tv.t != VT::Func) return vundef();
            Value thisArg = ac > 0 ? a[0] : vundef();
            return call_func(*g_it, tv.f, thisArg, ac > 1 ? a + 1 : nullptr, ac > 1 ? ac - 1 : 0);
        }));
        if (str_eq_c(key, "apply")) return vfunc(mknative([](Value tv, Value* a, int ac) -> Value {
            if (tv.t != VT::Func) return vundef();
            Value thisArg = ac > 0 ? a[0] : vundef();
            if (ac > 1 && a[1].t == VT::Obj && a[1].o && a[1].o->is_array) return call_func(*g_it, tv.f, thisArg, a[1].o->elems, a[1].o->nelems);
            return call_func(*g_it, tv.f, thisArg, nullptr, 0);
        }));
        if (str_eq_c(key, "fromCharCode")) return vfunc(mknative([](Value, Value* a, int ac) -> Value {
            char buf[512];
            int n = 0;
            for (int i = 0; i < ac && n < 511; ++i) {
                int code = static_cast<int>(to_num(*g_it, a[i]));
                buf[n++] = (code >= 0 && code < 256) ? static_cast<char>(code) : '?';
            }
            return vstr(mkstr(buf, n));
        }));
        return vundef();
    }
    return vundef();
}

static void set_prop(struct Interp& it, Value obj, Str* key, Value v) {
    (void)it;
    if (obj.t == VT::Obj && obj.o) {
        Obj* o = obj.o;
        if (o->is_array) {
            if (str_eq_c(key, "length")) {
                int nl = static_cast<int>(to_num(*g_it, v));
                if (nl < 0) nl = 0;
                if (nl > o->nelems) arr_ensure(o, nl - 1);
                else o->nelems = nl;
                return;
            }
            int idx = prop_to_index(key);
            if (idx >= 0) { arr_ensure(o, idx); o->elems[idx] = v; return; }
        }
        obj_set(o, key, v);
    }
}

static Str* arr_join(struct Interp& it, Obj* o, const char* sep) {
    int total = 0;
    Str** parts = static_cast<Str**>(g_A->alloc(sizeof(Str*) * static_cast<size_t>(o->nelems > 0 ? o->nelems : 1)));
    for (int i = 0; i < o->nelems; ++i) {
        if (o->elems[i].t == VT::Undef || o->elems[i].t == VT::Null) parts[i] = mkstr("", 0);
        else parts[i] = to_str(it, o->elems[i]);
        total += parts[i]->len;
    }
    int seplen = static_cast<int>(kstd::strlen(sep));
    total += seplen * (o->nelems > 0 ? o->nelems - 1 : 0);
    char* d = static_cast<char*>(g_A->alloc(static_cast<size_t>(total) + 1));
    int n = 0;
    for (int i = 0; i < o->nelems; ++i) {
        if (i) for (int j = 0; j < seplen; ++j) d[n++] = sep[j];
        for (int j = 0; j < parts[i]->len; ++j) d[n++] = parts[i]->data[j];
    }
    d[n] = '\0';
    Str* r = mkstr("", 0); r->data = d; r->len = n;
    return r;
}
// ----------------------------- Functions -----------------------------------
static Completion exec_block(Interp& it, Node* blk, Env* env);

static Value call_func(struct Interp& it, Func* fn, Value thisv, Value* args, int argc) {
    if (!fn) return vundef();
    if (fn->native) return fn->nf(thisv, args, argc);
    Env* e = it.new_env(fn->closure);
    if (!fn->arrow) it.set_this(e, thisv);
    for (int i = 0; i < fn->nparams; ++i) {
        Binding* b = it.declare(e, fn->params[i]);
        b->val = i < argc ? args[i] : vundef();
    }
    Env* saved = it.env;
    it.env = e;
    Completion c = exec_block(it, static_cast<Node*>(fn->body), e);
    it.env = saved;
    if (c.kind == 1) return c.val;
    return vundef();
}

// hoisting: pre-declare vars and functions in a block
static void hoist(Interp& it, Node* blk) {
    if (!blk || blk->k != NK::Block) return;
    for (int i = 0; i < blk->n; ++i) {
        Node* s = blk->list[i];
        if (!s) continue;
        if (s->k == NK::VarDecl && s->s) it.declare(it.env, s->s);
        else if (s->k == NK::FuncDecl && s->s) it.declare(it.env, s->s);
    }
}

// ----------------------------- Expression eval -----------------------------
static Value eval_binary(Interp& it, Node* n);
static Value eval_assign(Interp& it, Node* n);
static int strcmp_cmp(Str* a, Str* b);
static bool value_identity(const Value& a, const Value& b);

static Value eval_expr(struct Interp& it, Node* n) {
    if (!n) return vundef();
    switch (n->k) {
        case NK::Num: return vnum(n->num);
        case NK::Str: return vstr(n->s);
        case NK::Bool: return vbool(n->bval);
        case NK::Null: return vnull();
        case NK::Undef: return vundef();
        case NK::Ident: {
            Binding* b = it.find(it.env, n->s);
            if (b) return b->val;
            // global objects
            Value gv = get_global(it, n->s);
            return gv;
        }
        case NK::This: {
            for (Env* e = it.env; e; e = e->parent) if (e->has_this) return e->this_val;
            return vundef();
        }
        case NK::Array: {
            Obj* o = mkobj(true);
            for (int i = 0; i < n->n; ++i) { Value v = eval_expr(it, n->list[i]); arr_ensure(o, i); o->elems[i] = v; }
            return vobj(o);
        }
        case NK::Object: {
            Obj* o = mkobj(false);
            for (int i = 0; i < n->n; ++i) {
                Node* p = n->list[i];
                Value v = p->a ? eval_expr(it, p->a) : vundef();
                obj_set(o, p->s, v);
            }
            return vobj(o);
        }
        case NK::Func: {
            Func* f = g_A->make<Func>();
            f->params = n->names;
            f->nparams = n->nn;
            f->body = n->a;
            f->closure = it.env;
            f->name = n->s;
            f->arrow = n->flag;
            return vfunc(f);
        }
        case NK::Member: {
            Value base = eval_expr(it, n->a);
            return get_prop(it, base, n->s);
        }
        case NK::Index: {
            Value base = eval_expr(it, n->a);
            Value idx = eval_expr(it, n->b);
            return get_prop(it, base, to_str(it, idx));
        }
        case NK::Call: {
            Value thisv = vundef();
            Value callee = vundef();
            if (n->a->k == NK::Member) {
                thisv = eval_expr(it, n->a->a);
                callee = get_prop(it, thisv, n->a->s);
            } else if (n->a->k == NK::Index) {
                thisv = eval_expr(it, n->a->a);
                Value idx = eval_expr(it, n->a->b);
                callee = get_prop(it, thisv, to_str(it, idx));
            } else {
                callee = eval_expr(it, n->a);
            }
            Value args[16];
            int ac = n->n > 16 ? 16 : n->n;
            for (int i = 0; i < ac; ++i) args[i] = eval_expr(it, n->list[i]);
            if (callee.t == VT::Func) return call_func(it, callee.f, thisv, args, ac);
            return vundef();
        }
        case NK::New: {
            Obj* o = mkobj(false);
            Value args[16];
            int ac = n->n > 16 ? 16 : n->n;
            for (int i = 0; i < ac; ++i) args[i] = eval_expr(it, n->list[i]);
            if (n->a && (n->a->k == NK::Ident)) {
                // support new Array(n) and new Array(a, b, ...)
                if (n->a->s && str_eq_c(n->a->s, "Array")) {
                    Obj* arr = mkobj(true);
                    if (ac == 1 && args[0].t == VT::Num) {
                        int len = static_cast<int>(args[0].n);
                        if (len > 0) arr_ensure(arr, len - 1);
                        if (len < 0) arr->nelems = 0;
                    } else {
                        for (int i = 0; i < ac; ++i) { arr_ensure(arr, arr->nelems); arr->elems[arr->nelems - 1] = args[i]; }
                    }
                    return vobj(arr);
                }
            }
            Value callee = eval_expr(it, n->a);
            if (callee.t == VT::Func) call_func(it, callee.f, vobj(o), args, ac);
            return vobj(o);
        }
        case NK::Unary: {
            if (!kstd::strcmp(n->op, "typeof")) {
                Value v = eval_expr(it, n->a);
                switch (v.t) {
                    case VT::Undef: return vstr(mkstr("undefined", 9));
                    case VT::Null: return vstr(mkstr("object", 6));
                    case VT::Bool: return vstr(mkstr("boolean", 7));
                    case VT::Num: return vstr(mkstr("number", 6));
                    case VT::Str: return vstr(mkstr("string", 6));
                    case VT::Func: return vstr(mkstr("function", 8));
                    default: return vstr(mkstr("object", 6));
                }
            }
            if (!kstd::strcmp(n->op, "void")) { eval_expr(it, n->a); return vundef(); }
            if (!kstd::strcmp(n->op, "delete")) { eval_expr(it, n->a); return vbool(true); }
            Value v = eval_expr(it, n->a);
            if (!kstd::strcmp(n->op, "!")) return vbool(!to_bool(v));
            if (!kstd::strcmp(n->op, "-")) return vnum(-to_num(it, v));
            if (!kstd::strcmp(n->op, "+")) return vnum(to_num(it, v));
            if (!kstd::strcmp(n->op, "~")) return vnum(~static_cast<int>(to_num(it, v)));
            return vundef();
        }
        case NK::Update: {
            Value old = eval_expr(it, n->a);
            double d = to_num(it, old);
            d += (n->op[0] == '+') ? 1 : -1;
            // assign back
            if (n->a->k == NK::Ident) it.set(n->a->s, vnum(d));
            else if (n->a->k == NK::Member) { Value base = eval_expr(it, n->a->a); set_prop(it, base, n->a->s, vnum(d)); }
            else if (n->a->k == NK::Index) { Value base = eval_expr(it, n->a->a); Value idx = eval_expr(it, n->a->b); set_prop(it, base, to_str(it, idx), vnum(d)); }
            return n->flag ? vnum(d) : old;
        }
        case NK::Binary: return eval_binary(it, n);
        case NK::Logical: {
            Value a = eval_expr(it, n->a);
            bool ab = to_bool(a);
            if (!kstd::strcmp(n->op, "&&")) return ab ? eval_expr(it, n->b) : a;
            if (!kstd::strcmp(n->op, "||")) return ab ? a : eval_expr(it, n->b);
            // ??
            return (a.t == VT::Undef || a.t == VT::Null) ? eval_expr(it, n->b) : a;
        }
        case NK::Assign: return eval_assign(it, n);
        case NK::Cond: return to_bool(eval_expr(it, n->a)) ? eval_expr(it, n->b) : eval_expr(it, n->c);
        case NK::Seq: {
            Value v = vundef();
            for (int i = 0; i < n->n; ++i) v = eval_expr(it, n->list[i]);
            return v;
        }
        default:
            return vundef();
    }
}

static Value eval_binary(Interp& it, Node* n) {
    Value a = eval_expr(it, n->a);
    Value b = eval_expr(it, n->b);
    const char* op = n->op;
    if (!kstd::strcmp(op, "+")) {
        if (a.t == VT::Str || b.t == VT::Str || (a.t == VT::Obj) || (b.t == VT::Obj)) {
            Str* sa = to_str(it, a);
            Str* sb = to_str(it, b);
            int total = sa->len + sb->len;
            char* d = static_cast<char*>(g_A->alloc(static_cast<size_t>(total) + 1));
            int k = 0;
            for (int i = 0; i < sa->len; ++i) d[k++] = sa->data[i];
            for (int i = 0; i < sb->len; ++i) d[k++] = sb->data[i];
            d[k] = '\0';
            Str* r = mkstr("", 0); r->data = d; r->len = k;
            return vstr(r);
        }
        return vnum(to_num(it, a) + to_num(it, b));
    }
    if (!kstd::strcmp(op, "-")) return vnum(to_num(it, a) - to_num(it, b));
    if (!kstd::strcmp(op, "*")) return vnum(to_num(it, a) * to_num(it, b));
    if (!kstd::strcmp(op, "/")) return vnum(to_num(it, a) / to_num(it, b));
    if (!kstd::strcmp(op, "%")) { double x = to_num(it, a), y = to_num(it, b); return vnum(x - y * (double)(long long)(x / y)); }
    if (!kstd::strcmp(op, "**")) { double x = to_num(it, a), y = to_num(it, b); double r = 1; if (y >= 0) { for (int i = 0; i < (int)y; ++i) r *= x; } else r = 0; return vnum(r); }
    if (!kstd::strcmp(op, "<")) { if (a.t == VT::Str && b.t == VT::Str) { int c = str_eq(a.s, b.s) ? 0 : 0; (void)c; return vbool(strcmp_cmp(a.s, b.s) < 0); } return vbool(to_num(it, a) < to_num(it, b)); }
    if (!kstd::strcmp(op, ">")) { if (a.t == VT::Str && b.t == VT::Str) return vbool(strcmp_cmp(a.s, b.s) > 0); return vbool(to_num(it, a) > to_num(it, b)); }
    if (!kstd::strcmp(op, "<=")) { if (a.t == VT::Str && b.t == VT::Str) return vbool(strcmp_cmp(a.s, b.s) <= 0); return vbool(to_num(it, a) <= to_num(it, b)); }
    if (!kstd::strcmp(op, ">=")) { if (a.t == VT::Str && b.t == VT::Str) return vbool(strcmp_cmp(a.s, b.s) >= 0); return vbool(to_num(it, a) >= to_num(it, b)); }
    if (!kstd::strcmp(op, "==")) { if ((a.t == VT::Str || a.t == VT::Num || a.t == VT::Bool || a.t == VT::Null || a.t == VT::Undef) && (b.t == VT::Str || b.t == VT::Num || b.t == VT::Bool || b.t == VT::Null || b.t == VT::Undef)) return vbool(to_num(it, a) == to_num(it, b)); return vbool(value_identity(a, b)); }
    if (!kstd::strcmp(op, "!=")) { if ((a.t == VT::Str || a.t == VT::Num || a.t == VT::Bool || a.t == VT::Null || a.t == VT::Undef) && (b.t == VT::Str || b.t == VT::Num || b.t == VT::Bool || b.t == VT::Null || b.t == VT::Undef)) return vbool(to_num(it, a) != to_num(it, b)); return vbool(!value_identity(a, b)); }
    if (!kstd::strcmp(op, "===")) return vbool(value_identity(a, b));
    if (!kstd::strcmp(op, "!==")) return vbool(!value_identity(a, b));
    if (!kstd::strcmp(op, "&")) return vnum(static_cast<int>(to_num(it, a)) & static_cast<int>(to_num(it, b)));
    if (!kstd::strcmp(op, "|")) return vnum(static_cast<int>(to_num(it, a)) | static_cast<int>(to_num(it, b)));
    if (!kstd::strcmp(op, "^")) return vnum(static_cast<int>(to_num(it, a)) ^ static_cast<int>(to_num(it, b)));
    if (!kstd::strcmp(op, "<<")) return vnum(static_cast<int>(to_num(it, a)) << (static_cast<int>(to_num(it, b)) & 31));
    if (!kstd::strcmp(op, ">>")) return vnum(static_cast<int>(to_num(it, a)) >> (static_cast<int>(to_num(it, b)) & 31));
    if (!kstd::strcmp(op, ">>>")) return vnum(static_cast<unsigned>(static_cast<int>(to_num(it, a))) >> (static_cast<int>(to_num(it, b)) & 31));
    return vundef();
}

static Value eval_assign(Interp& it, Node* n) {
    Value rhs = eval_expr(it, n->b);
    if (!kstd::strcmp(n->op, "+=")) {
        Value cur = eval_expr(it, n->a);
        if (cur.t == VT::Str || rhs.t == VT::Str) {
            Str* sa = to_str(it, cur); Str* sb = to_str(it, rhs);
            char* d = static_cast<char*>(g_A->alloc(static_cast<size_t>(sa->len + sb->len) + 1));
            int k = 0; for (int i = 0; i < sa->len; ++i) d[k++] = sa->data[i]; for (int i = 0; i < sb->len; ++i) d[k++] = sb->data[i]; d[k] = 0;
            Str* r = mkstr("", 0); r->data = d; r->len = k; rhs = vstr(r);
        } else rhs = vnum(to_num(it, cur) + to_num(it, rhs));
    } else if (!kstd::strcmp(n->op, "-=")) rhs = vnum(to_num(it, eval_expr(it, n->a)) - to_num(it, rhs));
    else if (!kstd::strcmp(n->op, "*=")) rhs = vnum(to_num(it, eval_expr(it, n->a)) * to_num(it, rhs));
    else if (!kstd::strcmp(n->op, "/=")) rhs = vnum(to_num(it, eval_expr(it, n->a)) / to_num(it, rhs));
    else if (!kstd::strcmp(n->op, "%=")) { double x = to_num(it, eval_expr(it, n->a)), y = to_num(it, rhs); rhs = vnum(x - y * (double)(long long)(x / y)); }

    if (n->a->k == NK::Ident) it.set(n->a->s, rhs);
    else if (n->a->k == NK::Member) { Value base = eval_expr(it, n->a->a); set_prop(it, base, n->a->s, rhs); }
    else if (n->a->k == NK::Index) { Value base = eval_expr(it, n->a->a); Value idx = eval_expr(it, n->a->b); set_prop(it, base, to_str(it, idx), rhs); }
    return rhs;
}

static int strcmp_cmp(Str* a, Str* b);

static Completion exec_stmt_impl(Interp& it, Node* n);
static Completion exec_block(Interp& it, Node* blk, Env* env) {
    Env* saved = it.env;
    it.env = env;
    Completion c{0, vundef()};
    for (int i = 0; i < blk->n; ++i) {
        Node* s = blk->list[i];
        if (s && s->k == NK::FuncDecl && s->s) {
            Func* f = g_A->make<Func>();
            f->params = s->names; f->nparams = s->nn; f->body = s->a; f->closure = env; f->name = s->s;
            Binding* b = it.declare(env, s->s);
            b->val = vfunc(f);
            continue;
        }
        Completion r = exec_stmt_impl(it, s);
        if (r.kind != 0) { c = r; break; }
    }
    it.env = saved;
    return c;
}

static Completion exec_stmt_impl(Interp& it, Node* n) {
    Completion c{0, vundef()};
    if (!n) return c;
    switch (n->k) {
        case NK::Empty: return c;
        case NK::ExprStmt: eval_expr(it, n->a); return c;
        case NK::VarDecl: {
            Binding* b = it.declare(it.env, n->s);
            if (n->a) b->val = eval_expr(it, n->a);
            return c;
        }
        case NK::Block: {
            // reuse current env to keep var semantics simple
            hoist(it, n);
            return exec_block(it, n, it.env);
        }
        case NK::If: {
            if (to_bool(eval_expr(it, n->a))) return exec_stmt_impl(it, n->b);
            if (n->c) return exec_stmt_impl(it, n->c);
            return c;
        }
        case NK::While: {
            while (to_bool(eval_expr(it, n->a))) {
                Completion r = exec_stmt_impl(it, n->b);
                if (r.kind == 1) return r;
                if (r.kind == 2) break;
            }
            return c;
        }
        case NK::DoWhile: {
            do {
                Completion r = exec_stmt_impl(it, n->b);
                if (r.kind == 1) return r;
                if (r.kind == 2) break;
            } while (to_bool(eval_expr(it, n->a)));
            return c;
        }
        case NK::For: {
            Env* loop = it.new_env(it.env);
            Env* saved = it.env; it.env = loop;
            if (n->a) {
                if (n->a->k == NK::VarDecl) { Binding* b = it.declare(loop, n->a->s); if (n->a->a) b->val = eval_expr(it, n->a->a); }
                else if (n->a->k == NK::Block) exec_stmt_impl(it, n->a);
                else eval_expr(it, n->a);
            }
            while (true) {
                if (n->b && !to_bool(eval_expr(it, n->b))) break;
                Completion r = exec_stmt_impl(it, n->d);
                if (r.kind == 1) { it.env = saved; return r; }
                if (r.kind == 2) break;
                if (n->c) eval_expr(it, n->c);
            }
            it.env = saved;
            return c;
        }
        case NK::Switch: {
            Value disc = eval_expr(it, n->a);
            int start = -1, def = -1;
            for (int i = 0; i < n->n; ++i) {
                Node* cs = n->list[i];
                if (!cs) continue;
                if (!cs->a) { def = i; continue; }
                if (start < 0 && value_identity(disc, eval_expr(it, cs->a))) start = i;
            }
            if (start < 0) start = def;
            if (start >= 0) {
                for (int i = start; i < n->n; ++i) {
                    Node* cs = n->list[i];
                    if (!cs) continue;
                    for (int j = 0; j < cs->n; ++j) {
                        Completion r = exec_stmt_impl(it, cs->list[j]);
                        if (r.kind == 2) return c;
                        if (r.kind != 0) return r;
                    }
                }
            }
            return c;
        }
        case NK::Return: {
            c.kind = 1;
            c.val = n->a ? eval_expr(it, n->a) : vundef();
            return c;
        }
        case NK::Break: c.kind = 2; return c;
        case NK::Continue: c.kind = 3; return c;
        default:
            // expression-like statement
            eval_expr(it, n);
            return c;
    }
}

// ----------------------------- Global objects ------------------------------
static int strcmp_cmp(Str* a, Str* b) {
    if (!a) a = mkstr("", 0);
    if (!b) b = mkstr("", 0);
    int n = a->len < b->len ? a->len : b->len;
    for (int i = 0; i < n; ++i) { if (a->data[i] != b->data[i]) return (unsigned char)a->data[i] < (unsigned char)b->data[i] ? -1 : 1; }
    if (a->len == b->len) return 0;
    return a->len < b->len ? -1 : 1;
}

static bool value_identity(const Value& a, const Value& b) {
    if (a.t != b.t) return false;
    switch (a.t) {
        case VT::Undef: case VT::Null: return true;
        case VT::Bool: return a.b == b.b;
        case VT::Num: return a.n == b.n;
        case VT::Str: return str_eq(a.s, b.s);
        default: return a.o == b.o && a.f == b.f;
    }
}

// ----------------------------- JSON ----------------------------------------
static int json_esc_len(Str* s) {
    if (!s) return 0;
    int n = 0;
    for (int i = 0; i < s->len; ++i) {
        unsigned char c = static_cast<unsigned char>(s->data[i]);
        if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t' || c == '\b' || c == '\f') n += 2;
        else if (c < 0x20) n += 6;
        else n += 1;
    }
    return n;
}
static void json_esc_write(Str* s, char*& d) {
    if (!s) return;
    for (int i = 0; i < s->len; ++i) {
        unsigned char c = static_cast<unsigned char>(s->data[i]);
        if (c == '"' || c == '\\') { *d++ = '\\'; *d++ = static_cast<char>(c); }
        else if (c == '\n') { *d++ = '\\'; *d++ = 'n'; }
        else if (c == '\r') { *d++ = '\\'; *d++ = 'r'; }
        else if (c == '\t') { *d++ = '\\'; *d++ = 't'; }
        else if (c == '\b') { *d++ = '\\'; *d++ = 'b'; }
        else if (c == '\f') { *d++ = '\\'; *d++ = 'f'; }
        else if (c < 0x20) {
            static const char* hx = "0123456789abcdef";
            *d++ = '\\'; *d++ = 'u'; *d++ = '0'; *d++ = '0';
            *d++ = hx[(c >> 4) & 15]; *d++ = hx[c & 15];
        } else *d++ = static_cast<char>(c);
    }
}
static bool json_is_nullish(const Value& v) {
    if (v.t == VT::Undef || v.t == VT::Null || v.t == VT::Func) return true;
    if (v.t == VT::Num && (v.n != v.n || v.n > 1e308 || v.n < -1e308)) return true;
    return false;
}
static int json_len(Interp& it, const Value& v, int depth) {
    if (depth > 8 || json_is_nullish(v)) return 4;
    if (v.t == VT::Bool) return v.b ? 4 : 5;
    if (v.t == VT::Num) return to_str(it, v)->len;
    if (v.t == VT::Str) return 2 + json_esc_len(v.s);
    if (v.t == VT::Obj && v.o) {
        Obj* o = v.o;
        if (o->is_array) {
            int n = 2;
            for (int i = 0; i < o->nelems; ++i) { if (i) n += 1; n += json_len(it, o->elems[i], depth + 1); }
            return n;
        }
        int n = 2; bool first = true;
        for (int i = 0; i < o->nprops; ++i) {
            Str* key = o->props[i].key;
            if (!key || json_is_nullish(o->props[i].val)) continue;
            if (!first) n += 1;
            first = false;
            n += 2 + json_esc_len(key) + 1 + json_len(it, o->props[i].val, depth + 1);
        }
        return n;
    }
    return 4;
}
static void json_write(Interp& it, const Value& v, char*& d, int depth) {
    if (depth > 8 || json_is_nullish(v)) { for (int i = 0; i < 4; ++i) *d++ = "null"[i]; return; }
    if (v.t == VT::Bool) { if (v.b) { *d++='t';*d++='r';*d++='u';*d++='e'; } else { *d++='f';*d++='a';*d++='l';*d++='s';*d++='e'; } return; }
    if (v.t == VT::Num) { Str* s = to_str(it, v); for (int i = 0; i < s->len; ++i) *d++ = s->data[i]; return; }
    if (v.t == VT::Str) { *d++ = '"'; json_esc_write(v.s, d); *d++ = '"'; return; }
    if (v.t == VT::Obj && v.o) {
        Obj* o = v.o;
        if (o->is_array) {
            *d++ = '[';
            for (int i = 0; i < o->nelems; ++i) { if (i) *d++ = ','; json_write(it, o->elems[i], d, depth + 1); }
            *d++ = ']';
            return;
        }
        *d++ = '{'; bool first = true;
        for (int i = 0; i < o->nprops; ++i) {
            Str* key = o->props[i].key;
            if (!key || json_is_nullish(o->props[i].val)) continue;
            if (!first) *d++ = ',';
            first = false;
            *d++ = '"'; json_esc_write(key, d); *d++ = '"'; *d++ = ':';
            json_write(it, o->props[i].val, d, depth + 1);
        }
        *d++ = '}';
        return;
    }
    for (int i = 0; i < 4; ++i) *d++ = "null"[i];
}
static Value json_stringify(Interp& it, const Value& v) {
    int len = json_len(it, v, 0);
    char* d = static_cast<char*>(g_A->alloc(static_cast<size_t>(len) + 1));
    if (!d) return vundef();
    char* p = d;
    json_write(it, v, p, 0);
    *p = '\0';
    Str* r = mkstr("", 0); r->data = d; r->len = static_cast<int>(p - d);
    return vstr(r);
}

// ----------------------------- Interp & host globals -----------------------

// document / console / Math / JSON objects are built into the global env at
// the start of each execute() call.

static Value get_global(Interp& it, Str* key) {
    (void)it; (void)key;
    return vundef();
}

} // anonymous namespace

// ============================================================================
// Public entry point
// ============================================================================
bool execute(const char* source, Host* host) {
    if (!source) return false;
    // Recursion guard to avoid runaway nested writes
    static int depth = 0;
    if (depth > 3) return false;
    depth++;

    Arena arena;
    g_A = &arena;

    size_t len = kstd::strlen(source);
    if (len == 0) { arena.reset(); g_A = nullptr; depth--; return false; }

    Parser parser(source, len);
    Node* program = parser.parse_program();
    if (!program) { arena.reset(); g_A = nullptr; depth--; return false; }

    Interp it;
    it.host = host;
    it.global = it.new_env(nullptr);
    it.env = it.global;
    g_it = &it;

    // ---- install builtins ----
    // Number/String/Boolean/parseInt/parseFloat/isNaN
    it.declare(it.global, mkstr("parseInt"))->val = vfunc(mknative(g_parseInt));
    it.declare(it.global, mkstr("parseFloat"))->val = vfunc(mknative(g_parseFloat));
    it.declare(it.global, mkstr("isNaN"))->val = vfunc(mknative(g_isNaN));
    it.declare(it.global, mkstr("Number"))->val = vfunc(mknative(g_Number));
    it.declare(it.global, mkstr("String"))->val = vfunc(mknative(g_String));
    it.declare(it.global, mkstr("Boolean"))->val = vfunc(mknative(g_Boolean));
    it.declare(it.global, mkstr("isFinite"))->val = vfunc(mknative([](Value, Value* a, int ac) -> Value { double d = ac > 0 ? to_num(*g_it, a[0]) : 0.0 / 0.0; return vbool(d == d && d != 1.0 / 0.0 && d != -1.0 / 0.0); }));
    it.declare(it.global, mkstr("encodeURIComponent"))->val = vfunc(mknative([](Value, Value* a, int ac) -> Value { return vstr(ac > 0 ? to_str(*g_it, a[0]) : mkstr("", 0)); }));
    it.declare(it.global, mkstr("decodeURIComponent"))->val = vfunc(mknative([](Value, Value* a, int ac) -> Value { return vstr(ac > 0 ? to_str(*g_it, a[0]) : mkstr("", 0)); }));

    // Math object
    Obj* math = mkobj(false);
    obj_set(math, mkstr("floor"), vfunc(mknative([](Value, Value* a, int ac) -> Value { return vnum((double)(long long)(ac > 0 ? to_num(*g_it, a[0]) : 0)); })));
    obj_set(math, mkstr("ceil"), vfunc(mknative([](Value, Value* a, int ac) -> Value { double d = ac > 0 ? to_num(*g_it, a[0]) : 0; long long i = (long long)d; if (d > (double)i) i++; return vnum((double)i); })));
    obj_set(math, mkstr("round"), vfunc(mknative([](Value, Value* a, int ac) -> Value { double d = ac > 0 ? to_num(*g_it, a[0]) : 0; return vnum((long long)(d + (d >= 0 ? 0.5 : -0.5))); })));
    obj_set(math, mkstr("abs"), vfunc(mknative([](Value, Value* a, int ac) -> Value { double d = ac > 0 ? to_num(*g_it, a[0]) : 0; return vnum(d < 0 ? -d : d); })));
    obj_set(math, mkstr("sqrt"), vfunc(mknative([](Value, Value* a, int ac) -> Value { double d = ac > 0 ? to_num(*g_it, a[0]) : 0; if (d < 0) return vnum(0.0 / 0.0); double x = d, y; if (x == 0) return vnum(0); for (int i = 0; i < 20; ++i) { y = (x + d / x) / 2; if (y == x) break; x = y; } return vnum(x); })));
    obj_set(math, mkstr("pow"), vfunc(mknative([](Value, Value* a, int ac) -> Value { double x = ac > 0 ? to_num(*g_it, a[0]) : 0; double y = ac > 1 ? to_num(*g_it, a[1]) : 0; double r = 1; if (y >= 0) { for (int i = 0; i < (int)y; ++i) r *= x; } return vnum(r); })));
    obj_set(math, mkstr("max"), vfunc(mknative([](Value, Value* a, int ac) -> Value { double m = -1.0 / 0.0; for (int i = 0; i < ac; ++i) { double d = to_num(*g_it, a[i]); if (d > m) m = d; } return vnum(m); })));
    obj_set(math, mkstr("min"), vfunc(mknative([](Value, Value* a, int ac) -> Value { double m = 1.0 / 0.0; for (int i = 0; i < ac; ++i) { double d = to_num(*g_it, a[i]); if (d < m) m = d; } return vnum(m); })));
    obj_set(math, mkstr("random"), vfunc(mknative([](Value, Value*, int) -> Value { return vnum(((pit::get_ticks() * 1103515245ull + 12345ull) & 0x7fffffffull) / 2147483648.0); })));
    obj_set(math, mkstr("PI"), vnum(3.14159265358979323846));
    obj_set(math, mkstr("E"), vnum(2.71828182845904523536));
    it.declare(it.global, mkstr("Math"))->val = vobj(math);

    // Object
    Obj* object = mkobj(false);
    obj_set(object, mkstr("keys"), vfunc(mknative([](Value, Value* a, int ac) -> Value {
        Obj* r = mkobj(true);
        if (ac > 0 && a[0].t == VT::Obj && a[0].o) {
            Obj* o = a[0].o;
            if (o->is_array) {
                for (int i = 0; i < o->nelems; ++i) { arr_ensure(r, r->nelems); r->elems[r->nelems - 1] = vstr(num_to_str(i)); }
            } else {
                for (int i = 0; i < o->nprops; ++i) { arr_ensure(r, r->nelems); r->elems[r->nelems - 1] = vstr(o->props[i].key); }
            }
        }
        return vobj(r);
    })));
    obj_set(object, mkstr("values"), vfunc(mknative([](Value, Value* a, int ac) -> Value {
        Obj* r = mkobj(true);
        if (ac > 0 && a[0].t == VT::Obj && a[0].o) {
            Obj* o = a[0].o;
            if (o->is_array) for (int i = 0; i < o->nelems; ++i) { arr_ensure(r, r->nelems); r->elems[r->nelems - 1] = o->elems[i]; }
            else for (int i = 0; i < o->nprops; ++i) { arr_ensure(r, r->nelems); r->elems[r->nelems - 1] = o->props[i].val; }
        }
        return vobj(r);
    })));
    obj_set(object, mkstr("entries"), vfunc(mknative([](Value, Value* a, int ac) -> Value {
        Obj* r = mkobj(true);
        if (ac > 0 && a[0].t == VT::Obj && a[0].o) {
            Obj* o = a[0].o;
            for (int i = 0; i < (o->is_array ? o->nelems : o->nprops); ++i) {
                Obj* pair = mkobj(true);
                arr_ensure(pair, 0); pair->elems[0] = vstr(o->is_array ? num_to_str(i) : o->props[i].key);
                arr_ensure(pair, 1); pair->elems[1] = o->is_array ? o->elems[i] : o->props[i].val;
                arr_ensure(r, r->nelems); r->elems[r->nelems - 1] = vobj(pair);
            }
        }
        return vobj(r);
    })));
    obj_set(object, mkstr("assign"), vfunc(mknative([](Value, Value* a, int ac) -> Value {
        if (ac < 1 || a[0].t != VT::Obj || !a[0].o) return vundef();
        for (int i = 1; i < ac; ++i) {
            if (a[i].t != VT::Obj || !a[i].o) continue;
            Obj* src = a[i].o;
            if (src->is_array) for (int j = 0; j < src->nelems; ++j) set_prop(*g_it, a[0], num_to_str(j), src->elems[j]);
            else for (int j = 0; j < src->nprops; ++j) set_prop(*g_it, a[0], src->props[j].key, src->props[j].val);
        }
        return a[0];
    })));
    obj_set(object, mkstr("freeze"), vfunc(mknative([](Value, Value* a, int ac) -> Value { return ac > 0 ? a[0] : vundef(); })));
    it.declare(it.global, mkstr("Object"))->val = vobj(object);

    // Array
    Obj* array = mkobj(false);
    obj_set(array, mkstr("isArray"), vfunc(mknative([](Value, Value* a, int ac) -> Value { return vbool(ac > 0 && a[0].t == VT::Obj && a[0].o && a[0].o->is_array); })));
    obj_set(array, mkstr("of"), vfunc(mknative([](Value, Value* a, int ac) -> Value {
        Obj* r = mkobj(true);
        for (int i = 0; i < ac; ++i) { arr_ensure(r, r->nelems); r->elems[r->nelems - 1] = a[i]; }
        return vobj(r);
    })));
    obj_set(array, mkstr("from"), vfunc(mknative([](Value, Value* a, int ac) -> Value {
        Obj* r = mkobj(true);
        if (ac < 1) return vobj(r);
        if (a[0].t == VT::Str && a[0].s) {
            for (int i = 0; i < a[0].s->len; ++i) { arr_ensure(r, r->nelems); r->elems[r->nelems - 1] = vstr(mkstr(a[0].s->data + i, 1)); }
        } else if (a[0].t == VT::Obj && a[0].o && a[0].o->is_array) {
            for (int i = 0; i < a[0].o->nelems; ++i) { Value v = ac > 1 && a[1].t == VT::Func ? call_func(*g_it, a[1].f, vundef(), &a[0].o->elems[i], 1) : a[0].o->elems[i]; arr_ensure(r, r->nelems); r->elems[r->nelems - 1] = v; }
        } else if (a[0].t == VT::Obj && a[0].o) {
            Value lenv = get_prop(*g_it, a[0], mkstr("length"));
            int len = static_cast<int>(to_num(*g_it, lenv));
            for (int i = 0; i < len; ++i) { Value v = get_prop(*g_it, a[0], num_to_str(i)); arr_ensure(r, r->nelems); r->elems[r->nelems - 1] = v; }
        }
        return vobj(r);
    })));
    it.declare(it.global, mkstr("Array"))->val = vobj(array);

    // JSON
    Obj* json = mkobj(false);
    obj_set(json, mkstr("stringify"), vfunc(mknative([](Value, Value* a, int ac) -> Value { return ac > 0 ? json_stringify(*g_it, a[0]) : vundef(); })));
    it.declare(it.global, mkstr("JSON"))->val = vobj(json);

    // console
    Obj* console = mkobj(false);
    obj_set(console, mkstr("log"), vfunc(mknative([](Value, Value* a, int ac) -> Value {
        if (g_it && g_it->host && g_it->host->log) {
            // join args with spaces
            int total = 0; Str* parts[16];
            for (int i = 0; i < ac && i < 16; ++i) { parts[i] = to_str(*g_it, a[i]); total += parts[i]->len + 1; }
            char* d = static_cast<char*>(g_A->alloc(static_cast<size_t>(total) + 1));
            int n = 0;
            for (int i = 0; i < ac && i < 16; ++i) { if (i) d[n++] = ' '; for (int j = 0; j < parts[i]->len; ++j) d[n++] = parts[i]->data[j]; }
            d[n] = 0;
            g_it->host->log(g_it->host->user, d);
        }
        return vundef();
    })));
    obj_set(console, mkstr("warn"), get_prop(it, vobj(console), mkstr("log")));
    obj_set(console, mkstr("error"), get_prop(it, vobj(console), mkstr("log")));
    obj_set(console, mkstr("info"), get_prop(it, vobj(console), mkstr("log")));
    it.declare(it.global, mkstr("console"))->val = vobj(console);

    // document
    Obj* document = mkobj(false);
    obj_set(document, mkstr("write"), vfunc(mknative([](Value, Value* a, int ac) -> Value {
        if (g_it && g_it->host && g_it->host->write) {
            for (int i = 0; i < ac; ++i) { Str* s = to_str(*g_it, a[i]); if (s && s->data) g_it->host->write(g_it->host->user, s->data); }
        }
        return vundef();
    })));
    obj_set(document, mkstr("writeln"), get_prop(it, vobj(document), mkstr("write")));
    obj_set(document, mkstr("getElementById"), vfunc(mknative([](Value, Value*, int) -> Value { return vobj(mkobj(false)); })));
    obj_set(document, mkstr("getElementsByTagName"), vfunc(mknative([](Value, Value*, int) -> Value { return vobj(mkobj(true)); })));
    obj_set(document, mkstr("querySelector"), vfunc(mknative([](Value, Value*, int) -> Value { return vobj(mkobj(false)); })));
    obj_set(document, mkstr("querySelectorAll"), vfunc(mknative([](Value, Value*, int) -> Value { return vobj(mkobj(true)); })));
    obj_set(document, mkstr("createElement"), vfunc(mknative([](Value, Value*, int) -> Value { return vobj(mkobj(false)); })));
    obj_set(document, mkstr("addEventListener"), vfunc(mknative([](Value, Value*, int) -> Value { return vundef(); })));
    obj_set(document, mkstr("title"), vstr(mkstr("", 0)));
    it.declare(it.global, mkstr("document"))->val = vobj(document);
    it.declare(it.global, mkstr("window"))->val = vobj(mkobj(false));
    it.declare(it.global, mkstr("alert"))->val = get_prop(it, vobj(console), mkstr("log"));
    it.declare(it.global, mkstr("setTimeout"))->val = vfunc(mknative([](Value, Value* a, int ac) -> Value { if (ac > 0 && a[0].t == VT::Func) return call_func(*g_it, a[0].f, vundef(), nullptr, 0); return vundef(); }));
    it.declare(it.global, mkstr("NaN"))->val = vnum(0.0 / 0.0);
    it.declare(it.global, mkstr("Infinity"))->val = vnum(1.0 / 0.0);

    // ---- run ----
    hoist(it, program);
    exec_block(it, program, it.global);

    g_it = nullptr;
    arena.reset();
    g_A = nullptr;
    depth--;
    return true;
}

} // namespace js
