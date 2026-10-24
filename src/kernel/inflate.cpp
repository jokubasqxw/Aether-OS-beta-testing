#include "inflate.hpp"

namespace inflate {
namespace {

struct BitReader {
    const uint8_t* p;
    size_t len;
    size_t pos;
    uint32_t buf;
    int cnt;
    bool err;
};

static int br_get(BitReader& b, int n) {
    if (n <= 0) return 0;
    while (b.cnt < n) {
        if (b.pos >= b.len) { b.err = true; return 0; }
        b.buf |= static_cast<uint32_t>(b.p[b.pos++]) << b.cnt;
        b.cnt += 8;
    }
    int v = static_cast<int>(b.buf & ((1u << n) - 1u));
    b.buf >>= n;
    b.cnt -= n;
    return v;
}

static void br_align_byte(BitReader& b) {
    int drop = b.cnt & 7;
    b.buf >>= drop;
    b.cnt -= drop;
}

static int br_get_byte(BitReader& b) {
    if (b.cnt >= 8) {
        int v = static_cast<int>(b.buf & 0xffu);
        b.buf >>= 8;
        b.cnt -= 8;
        return v;
    }
    if (b.pos >= b.len) { b.err = true; return 0; }
    return b.p[b.pos++];
}

// Canonical Huffman decoding table.
struct Huff {
    uint16_t count[16];
    uint16_t symbol[288];
};

static void build_huff(Huff* h, const uint8_t* lengths, int n) {
    for (int i = 0; i < 16; ++i) h->count[i] = 0;
    for (int i = 0; i < n; ++i) h->count[lengths[i]]++;
    h->count[0] = 0;
    uint16_t offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; ++l) offs[l + 1] = static_cast<uint16_t>(offs[l] + h->count[l]);
    for (int i = 0; i < n; ++i) {
        if (lengths[i]) h->symbol[offs[lengths[i]]++] = static_cast<uint16_t>(i);
    }
}

static int decode_sym(BitReader& b, const Huff* h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len) {
        code |= br_get(b, 1);
        int count = h->count[len];
        if (code - first < count) return h->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t LEN_BASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t LEN_EXTRA[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t DIST_BASE[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const uint8_t DIST_EXTRA[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static void build_fixed(Huff* lit, Huff* dist) {
    uint8_t ll[288];
    for (int i = 0; i < 144; ++i) ll[i] = 8;
    for (int i = 144; i < 256; ++i) ll[i] = 9;
    for (int i = 256; i < 280; ++i) ll[i] = 7;
    for (int i = 280; i < 288; ++i) ll[i] = 8;
    build_huff(lit, ll, 288);
    uint8_t dl[30];
    for (int i = 0; i < 30; ++i) dl[i] = 5;
    build_huff(dist, dl, 30);
}

// Decodes one continuous DEFLATE stream (no container header).
static size_t inflate_raw(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_cap) {
    BitReader b{in, in_len, 0, 0, 0, false};
    size_t op = 0;

    Huff fixed_lit, fixed_dist;
    bool have_fixed = false;

    for (;;) {
        int bfinal = br_get(b, 1);
        int btype = br_get(b, 2);
        if (b.err) break;

        if (btype == 0) {
            br_align_byte(b);
            int len = br_get(b, 16);
            int nlen = br_get(b, 16);
            if (b.err) break;
            if (((len ^ 0xffff) & 0xffff) != nlen) break;
            for (int i = 0; i < len; ++i) {
                int c = br_get_byte(b);
                if (b.err) break;
                if (op >= out_cap) return out_cap;
                out[op++] = static_cast<uint8_t>(c);
            }
        } else if (btype == 1 || btype == 2) {
            Huff dyn_lit, dyn_dist;
            Huff* L;
            Huff* D;
            if (btype == 1) {
                if (!have_fixed) { build_fixed(&fixed_lit, &fixed_dist); have_fixed = true; }
                L = &fixed_lit;
                D = &fixed_dist;
            } else {
                int hlit = br_get(b, 5) + 257;
                int hdist = br_get(b, 5) + 1;
                int hclen = br_get(b, 4) + 4;
                if (b.err || hlit > 286 + 0 || hdist > 30) break;
                static const uint8_t ORD[19] = {
                    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
                };
                uint8_t cl_len[19] = {0};
                for (int i = 0; i < hclen; ++i) cl_len[ORD[i]] = static_cast<uint8_t>(br_get(b, 3));
                Huff cl;
                build_huff(&cl, cl_len, 19);

                uint8_t lengths[288 + 32];
                int total = hlit + hdist;
                int i = 0;
                while (i < total) {
                    int sym = decode_sym(b, &cl);
                    if (sym < 0) { b.err = true; break; }
                    if (sym < 16) {
                        lengths[i++] = static_cast<uint8_t>(sym);
                    } else if (sym == 16) {
                        if (i == 0) { b.err = true; break; }
                        int rep = br_get(b, 2) + 3;
                        uint8_t prev = lengths[i - 1];
                        while (rep-- && i < total) lengths[i++] = prev;
                    } else if (sym == 17) {
                        int rep = br_get(b, 3) + 3;
                        while (rep-- && i < total) lengths[i++] = 0;
                    } else {
                        int rep = br_get(b, 7) + 11;
                        while (rep-- && i < total) lengths[i++] = 0;
                    }
                }
                if (b.err) break;
                build_huff(&dyn_lit, lengths, hlit);
                build_huff(&dyn_dist, lengths + hlit, hdist);
                L = &dyn_lit;
                D = &dyn_dist;
            }

            for (;;) {
                int sym = decode_sym(b, L);
                if (sym < 0) { b.err = true; break; }
                if (sym < 256) {
                    if (op >= out_cap) return out_cap;
                    out[op++] = static_cast<uint8_t>(sym);
                } else if (sym == 256) {
                    break;
                } else {
                    int li = sym - 257;
                    if (li >= 29) { b.err = true; break; }
                    int length = LEN_BASE[li] + br_get(b, LEN_EXTRA[li]);
                    int dsym = decode_sym(b, D);
                    if (dsym < 0 || dsym >= 30) { b.err = true; break; }
                    int distance = DIST_BASE[dsym] + br_get(b, DIST_EXTRA[dsym]);
                    if (b.err) break;
                    if (distance <= 0 || static_cast<size_t>(distance) > op) { b.err = true; break; }
                    for (int k = 0; k < length; ++k) {
                        if (op >= out_cap) return out_cap;
                        out[op] = out[op - distance];
                        ++op;
                    }
                }
                if (b.err) break;
            }
        } else {
            b.err = true;
            break;
        }

        if (b.err) break;
        if (bfinal) break;
    }
    return op;
}

} // namespace

bool looks_compressed(const uint8_t* in, size_t in_len) {
    if (!in || in_len < 2) return false;
    if (in[0] == 0x1f && in[1] == 0x8b) return true;
    if ((in[0] & 0x0f) == 8 && ((static_cast<unsigned>(in[0]) << 8 | in[1]) % 31) == 0) return true;
    return false;
}

size_t decompress(const uint8_t* in, size_t in_len, uint8_t* out, size_t out_cap) {
    if (!in || in_len < 2 || !out || out_cap == 0) return 0;

    // gzip container (RFC 1952)
    if (in[0] == 0x1f && in[1] == 0x8b) {
        if (in_len < 18) return 0;
        int flg = in[3];
        size_t p = 10;
        if (flg & 4) {                       // FEXTRA
            if (p + 2 > in_len) return 0;
            uint16_t xlen = static_cast<uint16_t>(in[p] | (in[p + 1] << 8));
            p += 2 + xlen;
        }
        if (flg & 8) { while (p < in_len && in[p]) ++p; ++p; }    // FNAME
        if (flg & 16) { while (p < in_len && in[p]) ++p; ++p; }   // FCOMMENT
        if (flg & 2) p += 2;                 // FHCRC
        if (p >= in_len) return 0;
        return inflate_raw(in + p, in_len - p, out, out_cap);
    }

    // zlib container (RFC 1950): CMF/FLG, low nibble of CMF must be 8 (deflate)
    if ((in[0] & 0x0f) == 8 && ((static_cast<unsigned>(in[0]) << 8 | in[1]) % 31) == 0) {
        return inflate_raw(in + 2, in_len - 2, out, out_cap);
    }

    // Raw DEFLATE fallback.
    return inflate_raw(in, in_len, out, out_cap);
}

} // namespace inflate
